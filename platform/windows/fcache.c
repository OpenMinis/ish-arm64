// The contents of files read whole through a read-only descriptor, kept for
// the next read-only open of the same path. Such an open is a stat by name
// instead of CreateFile, and its reads, fstat and close make no system call:
// on the Windows test host the handle alone (CreateFile and CloseHandle,
// through the filter drivers) costs ~45 us a file, and a .pyc read ~110 us
// in all, where a stat by name is ~6 us. Python imports open and read the
// same few hundred .pyc files in every process.
//
// An entry is good while the file has the id, size and write and change
// times it had when it was opened to be read: a file replaced (a new id), or
// written in place (new times, maybe a new size), is read again. A path
// opened to be written loses its entry at once, as the file's times may lag
// behind writes still going on through that handle.
//
// ISH_FILE_CACHE=0 turns it off; ISH_FILE_CACHE_MB sets the size (32 MB).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "px_impl.h"

#define FC_FILE_MAX ((size_t) 1 << 20)
#define FC_BUCKETS 4096

struct px_fc {
    atomic_int refs;          // the table's, and one per descriptor
    struct px_fc *next;       // in its bucket
    struct px_fc *older, *newer;
    bool used;                // hit since the clock hand last passed it
    uint32_t hash;
    char *path;
    // what a stat by name says while the file is the same
    BYTE id[16];
    int64_t size, write, change;
    char data[];
};

static SRWLOCK fc_lock = SRWLOCK_INIT;
static struct px_fc *buckets[FC_BUCKETS];
static struct px_fc *oldest, *newest;
static size_t fc_bytes, fc_max;
static _Atomic uint64_t fc_hits, fc_stale, fc_fills;

static bool fc_enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("ISH_FILE_CACHE");
        const char *mb = getenv("ISH_FILE_CACHE_MB");
        fc_max = (mb != NULL ? (size_t) atoi(mb) : 32) << 20;
        on = !(e != NULL && e[0] == '0') && fc_max > 0;
    }
    return on;
}

static uint32_t fc_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s)
        h = (h ^ (uint8_t) *s++) * 16777619u;
    return h;
}

void px_fc_put(struct px_fc *fc) {
    if (fc != NULL && atomic_fetch_sub(&fc->refs, 1) == 1) {
        free(fc->path);
        free(fc);
    }
}

// fc_lock held exclusive; the table's reference is the caller's to put
static void fc_remove(struct px_fc *e) {
    for (struct px_fc **p = &buckets[e->hash % FC_BUCKETS]; *p != NULL; p = &(*p)->next) {
        if (*p == e) {
            *p = e->next;
            break;
        }
    }
    if (e->older != NULL)
        e->older->newer = e->newer;
    else
        oldest = e->newer;
    if (e->newer != NULL)
        e->newer->older = e->older;
    else
        newest = e->older;
    fc_bytes -= (size_t) e->size;
}

// fc_lock held
static struct px_fc *fc_find(const char *abs, uint32_t h) {
    for (struct px_fc *e = buckets[h % FC_BUCKETS]; e != NULL; e = e->next)
        if (e->hash == h && strcmp(e->path, abs) == 0)
            return e;
    return NULL;
}

static bool fc_same(const struct px_fc *e, const struct px_stat_basic *b) {
    return !(b->FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
        memcmp(e->id, b->FileId128, sizeof(e->id)) == 0 && e->size == b->EndOfFile.QuadPart &&
        e->write == b->LastWriteTime.QuadPart && e->change == b->ChangeTime.QuadPart;
}

static void fc_forget(struct px_fc *e) {
    AcquireSRWLockExclusive(&fc_lock);
    bool kept = fc_find(e->path, e->hash) == e;
    if (kept)
        fc_remove(e);
    ReleaseSRWLockExclusive(&fc_lock);
    if (kept)
        px_fc_put(e);
}

struct px_fc *px_fc_get(const char *abs, BY_HANDLE_FILE_INFORMATION *info) {
    if (!fc_enabled())
        return NULL;
    uint32_t h = fc_hash(abs);
    AcquireSRWLockShared(&fc_lock);
    struct px_fc *e = fc_find(abs, h);
    if (e != NULL)
        atomic_fetch_add(&e->refs, 1);
    ReleaseSRWLockShared(&fc_lock);
    if (e == NULL)
        return NULL;
    struct px_stat_basic b;
    int saved = errno;
    if (px_stat_basic(abs, &b) == 1 && fc_same(e, &b)) {
        e->used = true;
        px_info_from_basic(info, &b);
        fc_hits++;
        return e;
    }
    errno = saved;
    fc_stale++;
    fc_forget(e);
    px_fc_put(e);
    return NULL;
}

void px_fc_offer(struct px_desc *d, const void *data, size_t len) {
    if (!fc_enabled() || len > FC_FILE_MAX || len > fc_max / 8)
        return;
    FILE_ID_INFO id;
    if (!GetFileInformationByHandleEx(d->h, FileIdInfo, &id, sizeof(id)))
        return;
    struct px_fc *e = malloc(sizeof(*e) + len);
    if (e == NULL)
        return;
    atomic_init(&e->refs, 1);
    e->used = false;
    e->path = _strdup(d->path);
    e->hash = fc_hash(d->path);
    memcpy(e->id, &id.FileId, sizeof(e->id));
    e->size = (int64_t) len;
    e->write = d->fc_write;
    e->change = d->fc_change;
    memcpy(e->data, data, len);
    fc_fills++;

    struct px_fc *gone = NULL;
    AcquireSRWLockExclusive(&fc_lock);
    struct px_fc *old = fc_find(e->path, e->hash);
    if (old != NULL) {
        fc_remove(old);
        old->next = gone;
        gone = old;
    }
    e->next = buckets[e->hash % FC_BUCKETS];
    buckets[e->hash % FC_BUCKETS] = e;
    e->older = newest;
    e->newer = NULL;
    if (newest != NULL)
        newest->newer = e;
    else
        oldest = e;
    newest = e;
    fc_bytes += len;
    // a clock: what was hit since goes round once more
    while (fc_bytes > fc_max && oldest != e) {
        struct px_fc *o = oldest;
        fc_remove(o);
        if (o->used) {
            o->used = false;
            o->next = buckets[o->hash % FC_BUCKETS];
            buckets[o->hash % FC_BUCKETS] = o;
            o->older = newest;
            o->newer = NULL;
            newest->newer = o;
            newest = o;
            fc_bytes += (size_t) o->size;
        } else {
            o->next = gone;
            gone = o;
        }
    }
    ReleaseSRWLockExclusive(&fc_lock);
    while (gone != NULL) {
        struct px_fc *next = gone->next;
        px_fc_put(gone);
        gone = next;
    }
}

void px_fc_drop(const char *abs) {
    if (!fc_enabled())
        return;
    uint32_t h = fc_hash(abs);
    AcquireSRWLockShared(&fc_lock);
    bool there = fc_find(abs, h) != NULL;
    ReleaseSRWLockShared(&fc_lock);
    if (!there)
        return;
    AcquireSRWLockExclusive(&fc_lock);
    struct px_fc *e = fc_find(abs, h);
    if (e != NULL)
        fc_remove(e);
    ReleaseSRWLockExclusive(&fc_lock);
    px_fc_put(e);
}

ssize_t px_fc_pread(struct px_fc *fc, void *buf, size_t n, int64_t off) {
    if (off >= fc->size)
        return 0;
    size_t avail = (size_t) (fc->size - off);
    if (n > avail)
        n = avail;
    memcpy(buf, fc->data + off, n);
    return (ssize_t) n;
}

int64_t px_fc_size(struct px_fc *fc) {
    return fc->size;
}

HANDLE px_desc_handle(struct px_desc *d) {
    HANDLE h = d->h;
    if (h != INVALID_HANDLE_VALUE || d->fc == NULL)
        return h;
    h = px_open_handle(d->path, GENERIC_READ, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL);
    if (h == INVALID_HANDLE_VALUE) {
        px_fail_win();
        return h;
    }
    // what is read must stay the file that was opened
    FILE_ID_INFO id;
    if (!GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof(id)) ||
            memcmp(&id.FileId, d->fc->id, sizeof(d->fc->id)) != 0) {
        CloseHandle(h);
        px_fail(EIO);
        return INVALID_HANDLE_VALUE;
    }
    HANDLE was = InterlockedCompareExchangePointer(&d->h, h, INVALID_HANDLE_VALUE);
    if (was != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        h = was;
    }
    return h;
}

void px_fc_stats(uint64_t out[4]) {
    out[0] = fc_hits;
    out[1] = fc_stale;
    out[2] = fc_fills;
    out[3] = fc_bytes;
}
