// ish.exe --serve: one kernel for many commands (protocol 1). Minis writes
// requests on ish.exe's stdin and reads replies and events on its stdout, one
// JSON object a line. pid 1 is ish's own: the thread that booted, which runs
// no guest code. It starts each command as its child, in a session of its
// own, reaps every process that ends up its child, and reports.
//
// Threads: the reader (requests; answers ping and state itself, taking no
// guest lock), pid 1 (everything else; it never touches a guest pipe), and
// each command's own task thread, which sets up its stdio and cwd, runs the
// execve and reports started or error before any guest code runs.
#ifdef _WIN32

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "kernel/init.h"
#include "kernel/mm.h"
#include "kernel/serve.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "fs/path.h"
#include "ish_commit.h"

bool ish_serve_mode;
const char *ish_serve_rootfs;

int do_wait(int idtype, pid_t_ id, struct siginfo_ *info, struct rusage_ *rusage, int options);
char *state_dump_text(void (*tag)(struct task *t, char *buf, size_t size));
void state_dump_set_tag(void (*tag)(struct task *t, char *buf, size_t size));
int fakefs_bind_mount(const char *linux_path, const char *host_path, bool read_only);
int fakefs_bind_unmount(const char *linux_path);
// fs/fake.h (internal to fakefs)
void fakefs_set_path_translate_hook(bool (*hook)(const char *guest_path, uint64_t fs_context,
                                                 char *out_host_path, size_t out_size));
void fakefs_set_path_readonly_hook(bool (*hook)(const char *guest_path, uint64_t fs_context));
void fakefs_set_path_reverse_hook(bool (*hook)(const char *host_path, char *out_guest_path, size_t out_size));
#ifdef GUEST_X86_64
bool vm_wanted(void);
#endif
#ifdef ISH_XJIT
bool xjit_enabled(void);
#endif

// --- JSON: what the protocol needs ---

enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };
struct jv {
    int type;
    bool b;
    double num;
    char *str;
    int n;              // J_ARR, J_OBJ
    struct jv *items;
    char **keys;        // J_OBJ
};

static void jv_free(struct jv *v) {
    free(v->str);
    for (int i = 0; i < v->n; i++) {
        jv_free(&v->items[i]);
        if (v->keys)
            free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
}

struct jp {
    const char *s;
};

static void jp_space(struct jp *p) {
    while (*p->s == ' ' || *p->s == '\t' || *p->s == '\r' || *p->s == '\n')
        p->s++;
}

static int hex4(const char *s) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= c - '0';
        else if (c >= 'a' && c <= 'f')
            v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            v |= c - 'A' + 10;
        else
            return -1;
    }
    return v;
}

static char *jp_string(struct jp *p) {
    if (*p->s != '"')
        return NULL;
    p->s++;
    size_t cap = 64, len = 0;
    char *out = malloc(cap);
    if (out == NULL)
        return NULL;
    for (;;) {
        if (len + 8 > cap) {
            char *o = realloc(out, cap *= 2);
            if (o == NULL)
                goto bad;
            out = o;
        }
        unsigned char c = *p->s++;
        if (c == '"')
            break;
        if (c == 0 || c < 0x20)
            goto bad;
        if (c != '\\') {
            out[len++] = c;
            continue;
        }
        c = *p->s++;
        switch (c) {
            case '"': case '\\': case '/': out[len++] = c; break;
            case 'b': out[len++] = '\b'; break;
            case 'f': out[len++] = '\f'; break;
            case 'n': out[len++] = '\n'; break;
            case 'r': out[len++] = '\r'; break;
            case 't': out[len++] = '\t'; break;
            case 'u': {
                int u = hex4(p->s);
                if (u < 0)
                    goto bad;
                p->s += 4;
                if (u >= 0xd800 && u < 0xdc00 && p->s[0] == '\\' && p->s[1] == 'u') {
                    int lo = hex4(p->s + 2);
                    if (lo >= 0xdc00 && lo < 0xe000) {
                        u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00);
                        p->s += 6;
                    }
                }
                if (u == 0)
                    goto bad; // no NUL in a C string
                if (u < 0x80) {
                    out[len++] = u;
                } else if (u < 0x800) {
                    out[len++] = 0xc0 | (u >> 6);
                    out[len++] = 0x80 | (u & 0x3f);
                } else if (u < 0x10000) {
                    out[len++] = 0xe0 | (u >> 12);
                    out[len++] = 0x80 | ((u >> 6) & 0x3f);
                    out[len++] = 0x80 | (u & 0x3f);
                } else {
                    out[len++] = 0xf0 | (u >> 18);
                    out[len++] = 0x80 | ((u >> 12) & 0x3f);
                    out[len++] = 0x80 | ((u >> 6) & 0x3f);
                    out[len++] = 0x80 | (u & 0x3f);
                }
                break;
            }
            default:
                goto bad;
        }
    }
    out[len] = 0;
    return out;
bad:
    free(out);
    return NULL;
}

static bool jp_value(struct jp *p, struct jv *v, int depth) {
    *v = (struct jv) {0};
    if (depth > 32)
        return false;
    jp_space(p);
    char c = *p->s;
    if (c == '"') {
        v->type = J_STR;
        return (v->str = jp_string(p)) != NULL;
    }
    if (c == '[' || c == '{') {
        bool obj = c == '{';
        v->type = obj ? J_OBJ : J_ARR;
        p->s++;
        jp_space(p);
        if (*p->s == (obj ? '}' : ']')) {
            p->s++;
            return true;
        }
        int cap = 0;
        for (;;) {
            if (v->n == cap) {
                cap = cap ? cap * 2 : 8;
                struct jv *items = realloc(v->items, cap * sizeof(*items));
                if (items == NULL)
                    return false;
                v->items = items;
                if (obj) {
                    char **keys = realloc(v->keys, cap * sizeof(*keys));
                    if (keys == NULL)
                        return false;
                    v->keys = keys;
                }
            }
            if (obj) {
                jp_space(p);
                char *key = jp_string(p);
                if (key == NULL)
                    return false;
                jp_space(p);
                if (*p->s != ':') {
                    free(key);
                    return false;
                }
                p->s++;
                v->keys[v->n] = key;
            }
            if (!jp_value(p, &v->items[v->n], depth + 1)) {
                if (obj)
                    free(v->keys[v->n]);
                jv_free(&v->items[v->n]);
                return false;
            }
            v->n++;
            jp_space(p);
            if (*p->s == ',') {
                p->s++;
                continue;
            }
            if (*p->s == (obj ? '}' : ']')) {
                p->s++;
                return true;
            }
            return false;
        }
    }
    if (strncmp(p->s, "true", 4) == 0 || strncmp(p->s, "false", 5) == 0) {
        v->type = J_BOOL;
        v->b = c == 't';
        p->s += v->b ? 4 : 5;
        return true;
    }
    if (strncmp(p->s, "null", 4) == 0) {
        p->s += 4;
        return true;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char *end;
        v->type = J_NUM;
        v->num = strtod(p->s, &end);
        if (end == p->s)
            return false;
        p->s = end;
        return true;
    }
    return false;
}

static bool json_parse(const char *s, struct jv *v) {
    struct jp p = {s};
    if (!jp_value(&p, v, 0)) {
        jv_free(v);
        return false;
    }
    jp_space(&p);
    if (*p.s != 0) {
        jv_free(v);
        return false;
    }
    return true;
}

static struct jv *jget(struct jv *obj, const char *key) {
    if (obj == NULL || obj->type != J_OBJ)
        return NULL;
    for (int i = 0; i < obj->n; i++)
        if (strcmp(obj->keys[i], key) == 0)
            return &obj->items[i];
    return NULL;
}

static const char *jstr(struct jv *obj, const char *key) {
    struct jv *v = jget(obj, key);
    return v != NULL && v->type == J_STR ? v->str : NULL;
}

// --- output ---

struct jb {
    char *buf;
    size_t len, cap;
};

static void jb_add(struct jb *b, const char *s, size_t n) {
    if (b->len + n + 2 > b->cap) {
        size_t cap = (b->cap ? b->cap * 2 : 256) + n;
        char *nb = realloc(b->buf, cap);
        if (nb == NULL)
            return;
        b->buf = nb;
        b->cap = cap;
    }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
}

static void jb_printf(struct jb *b, const char *fmt, ...) {
    char small[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(small, sizeof(small), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t) n < sizeof(small)) {
        jb_add(b, small, n);
        return;
    }
    char *big = malloc(n + 1);
    if (big == NULL)
        return;
    va_start(ap, fmt);
    vsnprintf(big, n + 1, fmt, ap);
    va_end(ap);
    jb_add(b, big, n);
    free(big);
}

// A JSON string; bytes that are not UTF-8 become U+FFFD.
static void jb_str(struct jb *b, const char *s) {
    if (s == NULL) {
        jb_add(b, "null", 4);
        return;
    }
    jb_add(b, "\"", 1);
    const unsigned char *p = (const unsigned char *) s;
    while (*p) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            char esc[2] = {'\\', c};
            jb_add(b, esc, 2);
            p++;
        } else if (c < 0x20 || c == 0x7f) {
            char esc[8];
            switch (c) {
                case '\n': jb_add(b, "\\n", 2); break;
                case '\r': jb_add(b, "\\r", 2); break;
                case '\t': jb_add(b, "\\t", 2); break;
                default:
                    snprintf(esc, sizeof(esc), "\\u%04x", c);
                    jb_add(b, esc, 6);
            }
            p++;
        } else if (c < 0x80) {
            jb_add(b, (const char *) p, 1);
            p++;
        } else {
            int n = c >= 0xf0 && c < 0xf5 ? 4 : c >= 0xe0 ? 3 : c >= 0xc2 && c < 0xe0 ? 2 : 0;
            bool ok = n > 0;
            for (int i = 1; ok && i < n; i++)
                ok = (p[i] & 0xc0) == 0x80;
            if (ok) {
                jb_add(b, (const char *) p, n);
                p += n;
            } else {
                jb_add(b, "\xef\xbf\xbd", 3);
                p++;
            }
        }
    }
    jb_add(b, "\"", 1);
}

static void jb_send(struct jb *b) {
    if (b->buf == NULL)
        return;
    jb_add(b, "}\n", 2);
    px_serve_write(b->buf, b->len);
    free(b->buf);
    *b = (struct jb) {0};
}

// {"ev":"<ev>","id":<id>  (jb_send closes it)
static struct jb ev_begin(const char *ev, const char *id) {
    struct jb b = {0};
    jb_printf(&b, "{\"ev\":\"%s\",\"id\":", ev);
    jb_str(&b, id);
    return b;
}

static const char *err_text(int err) {
    switch (err) {
        case 1: return "Operation not permitted";
        case 2: return "No such file or directory";
        case 3: return "No such process";
        case 7: return "Argument list too long";
        case 8: return "Exec format error";
        case 9: return "Bad file descriptor";
        case 12: return "Out of memory";
        case 13: return "Permission denied";
        case 17: return "File exists";
        case 20: return "Not a directory";
        case 21: return "Is a directory";
        case 22: return "Invalid argument";
        case 36: return "File name too long";
        case 40: return "Too many levels of symbolic links";
        default: return "Error";
    }
}

// err: Linux errno, positive
static void ev_error(const char *id, int err, const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    struct jb b = ev_begin("error", id);
    jb_printf(&b, ",\"errno\":%d,\"message\":", err);
    jb_str(&b, msg);
    jb_send(&b);
}

static void ev_ok(const char *id) {
    struct jb b = ev_begin("ok", id);
    jb_send(&b);
}

// --- commands ---

enum { CMD_STARTING, CMD_RUNNING, CMD_EXITED, CMD_FAILED };

struct cmd {
    struct cmd *next;
    char *id;
    pid_t_ pid;             // its process, and its session
    int state;
    uint64_t done_cpu_ms;   // its session's processes that have ended
};

// Taken with pids_lock held, never the other way round; nothing blocks under
// it (events are written after it is let go).
static pthread_mutex_t cmds_lock = PTHREAD_MUTEX_INITIALIZER;
static struct cmd *cmds;

static struct cmd *cmd_find(const char *id) {
    for (struct cmd *c = cmds; c != NULL; c = c->next)
        if (strcmp(c->id, id) == 0)
            return c;
    return NULL;
}

static struct cmd *cmd_by_sid(pid_t_ sid) {
    for (struct cmd *c = cmds; c != NULL; c = c->next)
        if (c->pid == sid && c->state != CMD_FAILED)
            return c;
    return NULL;
}

static void cmd_remove(struct cmd *cmd) {
    for (struct cmd **c = &cmds; *c != NULL; c = &(*c)->next) {
        if (*c == cmd) {
            *c = cmd->next;
            free(cmd->id);
            free(cmd);
            return;
        }
    }
}

// --- the queue pid 1 works from ---

struct req {
    struct req *next;
    struct jv v;
};

// A process that ended (exit_hook), for the command its session belongs to.
struct ended {
    struct ended *next;
    pid_t_ pid, sid;
    int status;
    uint64_t cpu_ms;
};

// innermost of all: exit_hook takes it with pids_lock held
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cond = PTHREAD_COND_INITIALIZER;
static struct req *reqs, **reqs_tail = &reqs;
static struct ended *endeds, **endeds_tail = &endeds;
static bool q_eof;
static _Atomic bool braked;
// px_uptime_ms() (the tick count) of the start, and of the last exec or exit
static uint64_t start_ms;
static _Atomic uint64_t last_activity_ms;

static void q_wake(void) {
    pthread_mutex_lock(&q_lock);
    pthread_cond_signal(&q_cond);
    pthread_mutex_unlock(&q_lock);
}

static uint64_t tv_ms(struct timeval_ tv) {
    return (uint64_t) tv.sec * 1000 + tv.usec / 1000;
}

// Called with pids_lock held, as the last thread of a process leaves it.
static void serve_exit_hook(struct task *task, int status) {
    struct tgroup *group = task->group;
    struct ended *e = malloc(sizeof(*e));
    if (e == NULL)
        return;
    e->next = NULL;
    e->pid = group->leader->pid;
    e->sid = group->sid;
    e->status = status;
    e->cpu_ms = tv_ms(group->rusage.utime) + tv_ms(group->rusage.stime);
    pthread_mutex_lock(&q_lock);
    *endeds_tail = e;
    endeds_tail = &e->next;
    pthread_cond_signal(&q_cond);
    pthread_mutex_unlock(&q_lock);
}

static void serve_braked_hook(void) {
    if (!atomic_exchange(&braked, true))
        q_wake();
}

// --- per-exec binds (fakefs' translate hooks, keyed by fs_context) ---

struct bmap {
    char *guest, *host;
    size_t glen, hlen;
    bool ro;
};
struct bctx {
    int n;
    struct bmap *m;
};

// Never freed, only added to (by pid 1): the hooks read them without a lock.
#define MAX_BCTX 4096
static struct bctx *bctxs[MAX_BCTX];
static _Atomic int nbctx;

static const struct bmap *bctx_lookup(const char *guest, uint64_t ctx) {
    if (ctx == 0 || ctx > (uint64_t) atomic_load_explicit(&nbctx, memory_order_acquire))
        return NULL;
    const struct bctx *c = bctxs[ctx - 1];
    const struct bmap *best = NULL;
    for (int i = 0; i < c->n; i++) {
        const struct bmap *m = &c->m[i];
        if (strncmp(guest, m->guest, m->glen) == 0 && (guest[m->glen] == '/' || guest[m->glen] == 0) &&
                (best == NULL || m->glen > best->glen))
            best = m;
    }
    return best;
}

static bool bind_translate(const char *guest, uint64_t ctx, char *out, size_t size) {
    const struct bmap *m = bctx_lookup(guest, ctx);
    if (m == NULL)
        return false;
    snprintf(out, size, "%s%s", m->host, guest + m->glen);
    return true;
}

static bool bind_readonly(const char *guest, uint64_t ctx) {
    const struct bmap *m = bctx_lookup(guest, ctx);
    return m != NULL && m->ro;
}

static bool bind_reverse(const char *host, char *out, size_t size) {
    int n = atomic_load_explicit(&nbctx, memory_order_acquire);
    for (int k = 0; k < n; k++) {
        const struct bctx *c = bctxs[k];
        for (int i = 0; i < c->n; i++) {
            const struct bmap *m = &c->m[i];
            if (strncmp(host, m->host, m->hlen) == 0 && (host[m->hlen] == '/' || host[m->hlen] == 0)) {
                snprintf(out, size, "%s%s", m->guest, host + m->hlen);
                return true;
            }
        }
    }
    return false;
}

static bool bctx_same(const struct bctx *a, const struct bctx *b) {
    if (a->n != b->n)
        return false;
    for (int i = 0; i < a->n; i++) {
        bool found = false;
        for (int j = 0; j < b->n && !found; j++)
            found = strcmp(a->m[i].guest, b->m[j].guest) == 0 && strcmp(a->m[i].host, b->m[j].host) == 0 &&
                    a->m[i].ro == b->m[j].ro;
        if (!found)
            return false;
    }
    return true;
}

static void bctx_free(struct bctx *c) {
    for (int i = 0; i < c->n; i++) {
        free(c->m[i].guest);
        free(c->m[i].host);
    }
    free(c->m);
    free(c);
}

// The context of exec's binds (0 for none), or a Linux errno (negative).
static int64_t bctx_of(struct jv *binds, char *msg, size_t msg_size) {
    if (binds == NULL || binds->type == J_NULL || (binds->type == J_ARR && binds->n == 0))
        return 0;
    if (binds->type != J_ARR) {
        snprintf(msg, msg_size, "binds: expected an array");
        return -22;
    }
    struct bctx *c = calloc(1, sizeof(*c));
    if (c == NULL || (c->m = calloc(binds->n, sizeof(*c->m))) == NULL) {
        free(c);
        return -12;
    }
    for (int i = 0; i < binds->n; i++) {
        const char *guest = jstr(&binds->items[i], "guest"), *host = jstr(&binds->items[i], "host");
        struct jv *ro = jget(&binds->items[i], "ro");
        if (guest == NULL || host == NULL || guest[0] != '/') {
            snprintf(msg, msg_size, "binds[%d]: expected {\"guest\":\"/...\",\"host\":\"...\"}", i);
            bctx_free(c);
            return -22;
        }
        struct stat st;
        char resolved[MAX_PATH];
        if (stat(host, &st) < 0 || realpath(host, resolved) == NULL) {
            snprintf(msg, msg_size, "binds[%d]: host %s: %s", i, host, err_text(2));
            bctx_free(c);
            return -2;
        }
        struct bmap *m = &c->m[c->n++];
        m->guest = strdup(guest);
        m->glen = strlen(guest);
        while (m->glen > 1 && m->guest[m->glen - 1] == '/')
            m->guest[--m->glen] = 0;
        m->host = strdup(resolved);
        m->hlen = strlen(resolved);
        while (m->hlen > 1 && (m->host[m->hlen - 1] == '/' || m->host[m->hlen - 1] == '\\'))
            m->host[--m->hlen] = 0;
        m->ro = ro != NULL && ro->type == J_BOOL && ro->b;
    }
    int n = atomic_load(&nbctx);
    for (int k = 0; k < n; k++) {
        if (bctx_same(bctxs[k], c)) {
            bctx_free(c);
            return k + 1;
        }
    }
    if (n == MAX_BCTX) {
        snprintf(msg, msg_size, "binds: too many different sets");
        bctx_free(c);
        return -12;
    }
    bctxs[n] = c;
    atomic_store_explicit(&nbctx, n + 1, memory_order_release);
    return n + 1;
}

// --- exec ---

struct exec_args {
    struct cmd *cmd;
    char *id;
    char *file;             // argv[0]
    char *argv, *envp;      // NUL-separated, an empty string at the end
    size_t argc;
    char *cwd;
    void *h[3];             // stdin, stdout, stderr; NULL: /dev/null
    bool err_is_out;
};

static void exec_args_free(struct exec_args *a) {
    for (int i = 0; i < 3; i++)
        if (a->h[i] != NULL)
            px_handle_close(a->h[i]);
    free(a->id);
    free(a->file);
    free(a->argv);
    free(a->envp);
    free(a->cwd);
    free(a);
}

// A guest fd for stdio slot i (takes its handle).
static struct fd *stdio_fd(struct exec_args *a, int i, int *err) {
    void *h = a->h[i];
    a->h[i] = NULL;
    if (h == NULL) {
        struct fd *fd = generic_open("/dev/null", i == 0 ? O_RDONLY_ : O_WRONLY_, 0);
        if (IS_ERR(fd)) {
            *err = PTR_ERR(fd);
            return NULL;
        }
        return fd;
    }
    int real = px_fd_from_handle(h, i == 0);
    if (real < 0) {
        *err = errno_map();
        return NULL;
    }
    struct fd *fd = open_fd_from_actual_fd(real);
    if (fd == NULL) {
        close(real);
        *err = _ENOMEM;
    }
    return fd;
}

// argv[0] by PATH (from the command's own env), as execvp does.
static char *find_program(const char *name, const char *envp, int *err) {
    if (strchr(name, '/') != NULL)
        return strdup(name);
    const char *path = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    for (const char *e = envp; *e; e += strlen(e) + 1)
        if (strncmp(e, "PATH=", 5) == 0)
            path = e + 5;
    *err = _ENOENT;
    bool eacces = false;
    while (*path) {
        const char *colon = strchr(path, ':');
        size_t dlen = colon ? (size_t) (colon - path) : strlen(path);
        char cand[MAX_PATH];
        if (dlen + strlen(name) + 2 < sizeof(cand)) {
            if (dlen == 0)
                snprintf(cand, sizeof(cand), "%s", name);
            else
                snprintf(cand, sizeof(cand), "%.*s/%s", (int) dlen, path, name);
            struct statbuf st;
            if (generic_statat(AT_PWD, cand, &st, true) == 0 && (st.mode & S_IFMT) == S_IFREG) {
                if (generic_accessat(AT_PWD, cand, 1 /* X_OK */) == 0) {
                    *err = 0;
                    return strdup(cand);
                }
                eacces = true;
            }
        }
        if (colon == NULL)
            break;
        path = colon + 1;
    }
    if (eacces)
        *err = _EACCES;
    return NULL;
}

// On the command's own task thread, before any guest code: a failure is the
// exec's error, and the task goes.
static void exec_first(void *arg) {
    struct exec_args *a = arg;
    int err = 0;
    char what[MAX_PATH + 64] = "";

    struct fd *out = NULL;
    for (int i = 0; i < 3 && err == 0; i++) {
        struct fd *fd;
        if (i == 2 && a->err_is_out)
            fd = fd_retain(out);
        else if ((fd = stdio_fd(a, i, &err)) == NULL)
            snprintf(what, sizeof(what), "%s", i == 0 ? "stdin" : i == 1 ? "stdout" : "stderr");
        if (fd != NULL)
            current->files->files[i] = fd;
        if (i == 1)
            out = fd;
    }

    if (err == 0) {
        struct statbuf st;
        err = generic_statat(AT_PWD, a->cwd, &st, true);
        if (err == 0 && (st.mode & S_IFMT) != S_IFDIR)
            err = _ENOTDIR;
        struct fd *dir = err == 0 ? generic_open(a->cwd, O_RDONLY_, 0) : NULL;
        if (err == 0 && IS_ERR(dir))
            err = PTR_ERR(dir);
        if (err == 0)
            fs_chdir(current->fs, dir);
        else
            snprintf(what, sizeof(what), "cwd %s", a->cwd);
    }

    char *file = NULL;
    if (err == 0) {
        file = find_program(a->file, a->envp, &err);
        if (file == NULL)
            snprintf(what, sizeof(what), "%s", a->file);
    }
    if (err == 0) {
        err = do_execve(file, a->argc, a->argv, a->envp);
        if (err < 0)
            snprintf(what, sizeof(what), "%s", file);
    }
    free(file);

    pid_t_ pid = current->pid;
    pthread_mutex_lock(&cmds_lock);
    a->cmd->state = err == 0 ? CMD_RUNNING : CMD_FAILED;
    pthread_mutex_unlock(&cmds_lock);
    if (err == 0) {
        struct jb b = ev_begin("started", a->id);
        jb_printf(&b, ",\"pid\":%d", pid);
        jb_send(&b);
    } else {
        ev_error(a->id, -err, "%s: %s", what, err_text(-err));
    }
    exec_args_free(a);
    if (err != 0)
        do_exit(127 << 8);
}

// NUL-separated strings of a JSON array of strings, an empty one at the end.
static char *nul_list(struct jv *arr, size_t *count) {
    size_t size = 1;
    for (int i = 0; i < arr->n; i++) {
        if (arr->items[i].type != J_STR)
            return NULL;
        size += strlen(arr->items[i].str) + 1;
    }
    char *list = malloc(size), *p = list;
    if (list == NULL)
        return NULL;
    for (int i = 0; i < arr->n; i++) {
        size_t n = strlen(arr->items[i].str) + 1;
        memcpy(p, arr->items[i].str, n);
        p += n;
    }
    *p = 0;
    if (count != NULL)
        *count = arr->n;
    return list;
}

// A handle field: null (or missing) is NULL, a number is that handle.
static bool handle_field(struct jv *req, const char *key, void **h) {
    struct jv *v = jget(req, key);
    *h = NULL;
    if (v == NULL || v->type == J_NULL)
        return true;
    if (v->type != J_NUM || v->num < 0 || v->num != floor(v->num))
        return false;
    *h = (void *) (uintptr_t) v->num;
    return true;
}

static void op_exec(struct jv *req, const char *id) {
    struct exec_args *a = calloc(1, sizeof(*a));
    if (a == NULL) {
        ev_error(id, 12, "out of memory");
        return;
    }
    struct jv *se = jget(req, "stderr");
    a->err_is_out = se != NULL && se->type == J_STR && strcmp(se->str, "stdout") == 0;
    // every field read, so that all the handles given are closed on an error
    bool in_ok = handle_field(req, "stdin", &a->h[0]);
    bool out_ok = handle_field(req, "stdout", &a->h[1]);
    bool err_ok = a->err_is_out || handle_field(req, "stderr", &a->h[2]);
    bool handles_ok = in_ok && out_ok && err_ok;
    a->id = strdup(id);

    struct jv *argv = jget(req, "argv"), *env = jget(req, "env");
    const char *cwd = jstr(req, "cwd");
    char msg[MAX_PATH + 128] = "";
    int err = 0;
    if (!handles_ok) {
        err = 22;
        snprintf(msg, sizeof(msg), "stdin/stdout/stderr: expected a handle number or null");
    } else if (argv == NULL || argv->type != J_ARR || argv->n == 0 ||
               (a->argv = nul_list(argv, &a->argc)) == NULL) {
        err = 22;
        snprintf(msg, sizeof(msg), "argv: expected a non-empty array of strings");
    } else if (env != NULL && env->type != J_NULL &&
               (env->type != J_ARR || (a->envp = nul_list(env, NULL)) == NULL)) {
        err = 22;
        snprintf(msg, sizeof(msg), "env: expected an array of strings");
    } else {
        if (a->envp == NULL)
            a->envp = calloc(1, 1);
        a->file = strdup(argv->items[0].str);
        a->cwd = strdup(cwd != NULL ? cwd : "/");
    }
    int64_t ctx = 0;
    if (err == 0) {
        ctx = bctx_of(jget(req, "binds"), msg, sizeof(msg));
        if (ctx < 0)
            err = (int) -ctx;
    }
    pthread_mutex_lock(&cmds_lock);
    bool dup = cmd_find(id) != NULL;
    pthread_mutex_unlock(&cmds_lock);
    if (err == 0 && dup) {
        err = 17;
        snprintf(msg, sizeof(msg), "id %s is in use", id);
    }
    if (err != 0) {
        exec_args_free(a);
        ev_error(id, err, "%s", msg);
        return;
    }

    struct task *init = current;
    err = become_new_init_child();
    struct task *task = current;
    current = init;
    if (err < 0) {
        exec_args_free(a);
        ev_error(id, -err, "can't make a process: %s", err_text(-err));
        return;
    }
    task->group->fs_context = (uint64_t) ctx;

    struct cmd *cmd = calloc(1, sizeof(*cmd));
    cmd->id = strdup(id);
    cmd->pid = task->pid;
    cmd->state = CMD_STARTING;
    pthread_mutex_lock(&cmds_lock);
    cmd->next = cmds;
    cmds = cmd;
    pthread_mutex_unlock(&cmds_lock);
    a->cmd = cmd;
    atomic_store(&last_activity_ms, px_uptime_ms());

    err = task_start_first(task, exec_first, a);
    if (err < 0) {
        pthread_mutex_lock(&cmds_lock);
        cmd_remove(cmd);
        pthread_mutex_unlock(&cmds_lock);
        lock(&pids_lock);
        task_destroy(task);
        unlock(&pids_lock);
        exec_args_free(a);
        ev_error(id, -err, "can't start a thread: %s", err_text(-err));
    }
}

// --- what is running ---

// With pids_lock: guest kB mapped by a process.
static uint64_t mapped_kb_of(struct task *leader) {
    uint64_t pages = 0;
    lock(&leader->general_lock);
    struct mem *mem = leader->mem;
    if (mem != NULL) {
        read_wrlock(&mem->lock);
        page_t page = 0;
        while (page < MEM_PAGES) {
            struct pt_entry *pt = mem_pt(mem, page);
            if (pt != NULL && pt->data != NULL)
                pages++;
            mem_next_page(mem, &page);
        }
        read_wrunlock(&mem->lock);
    }
    unlock(&leader->general_lock);
    return pages * (PAGE_SIZE / 1024);
}

// With pids_lock: CPU of a running process so far.
static uint64_t cpu_ms_of(struct tgroup *group) {
    lock(&group->lock);
    uint64_t ms = tv_ms(group->rusage.utime) + tv_ms(group->rusage.stime);
    struct task *t;
    list_for_each_entry(&group->threads, t, group_links)
        ms += px_pthread_cpu_ms(t->thread);
    unlock(&group->lock);
    return ms;
}

struct usage {
    int procs;
    uint64_t cpu_ms, mapped_kb;
};

// With pids_lock: the running processes of session sid.
static struct usage session_usage(pid_t_ sid, bool mapped) {
    struct usage u = {0};
    struct pid *pid = pid_get(sid);
    if (pid == NULL)
        return u;
    struct tgroup *group;
    list_for_each_entry(&pid->session, group, session) {
        if (group->leader == NULL || group->leader->zombie || group->leader->exiting)
            continue;
        u.procs++;
        u.cpu_ms += cpu_ms_of(group);
        if (mapped)
            u.mapped_kb += mapped_kb_of(group->leader);
    }
    return u;
}

// For the snapshot: which command a task's session is.
static void state_tag(struct task *t, char *buf, size_t size) {
    if (t->group == NULL)
        return;
    pthread_mutex_lock(&cmds_lock);
    struct cmd *c = cmd_by_sid(t->group->sid);
    if (c != NULL)
        snprintf(buf, size, "cmd=%s", c->id);
    pthread_mutex_unlock(&cmds_lock);
}

// --- reaping and reporting ---

#define P_ALL_ 0
#define WNOHANG_ 1
#define WEXITED_ 4

static void reap(void) {
    for (;;) {
        struct siginfo_ info = {0};
        struct rusage_ rusage = {0};
        if (do_wait(P_ALL_, 0, &info, &rusage, WEXITED_ | WNOHANG_) < 0 || info.child.pid == 0)
            return;
    }
}

static void report_ended(struct ended *e) {
    struct jb b = {0};
    lock(&pids_lock);
    pthread_mutex_lock(&cmds_lock);
    struct cmd *cmd = NULL;
    for (struct cmd *c = cmds; c != NULL && cmd == NULL; c = c->next)
        if (c->pid == e->sid || c->pid == e->pid)
            cmd = c;
    if (cmd != NULL && cmd->state == CMD_FAILED && cmd->pid == e->pid) {
        cmd_remove(cmd);
    } else if (cmd != NULL && cmd->pid == e->sid) {
        cmd->done_cpu_ms += e->cpu_ms;
        if (cmd->pid == e->pid && cmd->state == CMD_RUNNING) {
            struct usage u = session_usage(cmd->pid, false);
            int sig = e->status & 0x7f, code = (e->status >> 8) & 0xff;
            b = ev_begin("exit", cmd->id);
            jb_printf(&b, ",\"pid\":%d", cmd->pid);
            if (sig != 0)
                jb_printf(&b, ",\"code\":null,\"signal\":%d,\"core\":%s,\"rc\":%d", sig,
                          e->status & 0x80 ? "true" : "false", 128 + sig);
            else
                jb_printf(&b, ",\"code\":%d,\"signal\":null,\"core\":false,\"rc\":%d", code, code);
            jb_printf(&b, ",\"left\":%d,\"cpu_ms\":%llu", u.procs,
                      (unsigned long long) (cmd->done_cpu_ms + u.cpu_ms));
            if (u.procs == 0)
                cmd_remove(cmd);
            else
                cmd->state = CMD_EXITED;
        }
    }
    pthread_mutex_unlock(&cmds_lock);
    unlock(&pids_lock);
    jb_send(&b);
}

// Commands whose last background process has now gone.
static void report_gone(void) {
    for (;;) {
        char *id = NULL;
        lock(&pids_lock);
        pthread_mutex_lock(&cmds_lock);
        for (struct cmd *c = cmds; c != NULL && id == NULL; c = c->next) {
            if (c->state == CMD_EXITED && session_usage(c->pid, false).procs == 0) {
                id = c->id;
                c->id = NULL;
                cmd_remove(c);
            }
        }
        pthread_mutex_unlock(&cmds_lock);
        unlock(&pids_lock);
        if (id == NULL)
            return;
        struct jb b = ev_begin("gone", id);
        jb_send(&b);
        free(id);
    }
}

// The brake refused memory: the command with the most guest memory mapped
// goes, so that ish itself does not.
static void out_of_memory(void) {
    static uint64_t last_kill_ms;
    uint64_t now = px_uptime_ms();
    if (last_kill_ms != 0 && now - last_kill_ms < 3000)
        return;
    char *id = NULL;
    pid_t_ sid = 0;
    uint64_t most = 0;
    lock(&pids_lock);
    pthread_mutex_lock(&cmds_lock);
    for (struct cmd *c = cmds; c != NULL; c = c->next) {
        if (c->state != CMD_RUNNING && c->state != CMD_EXITED)
            continue;
        uint64_t kb = session_usage(c->pid, true).mapped_kb;
        if (kb > most) {
            most = kb;
            sid = c->pid;
            free(id);
            id = strdup(c->id);
        }
    }
    pthread_mutex_unlock(&cmds_lock);
    unlock(&pids_lock);
    if (id == NULL)
        return;
    last_kill_ms = now;
    struct jb b = ev_begin("oom", id);
    jb_printf(&b, ",\"mapped_kb\":%llu", (unsigned long long) most);
    jb_send(&b);
    send_session_signal(sid, SIGKILL_, (struct siginfo_) {.code = SI_KERNEL_});
    free(id);
}

// --- other requests ---

static void op_signal(struct jv *req, const char *id) {
    struct jv *sigv = jget(req, "sig");
    const char *to = jstr(req, "to");
    if (to == NULL)
        to = "session";
    if (sigv == NULL || sigv->type != J_NUM || sigv->num < 1 || sigv->num >= NUM_SIGS) {
        ev_error(id, 22, "sig: expected a signal number");
        return;
    }
    int sig = (int) sigv->num;
    struct siginfo_ info = {.code = SI_USER_};
    if (strcmp(to, "all") == 0) {
        int err = send_all_signal(sig, info);
        if (err < 0)
            ev_error(id, -err, "no process to signal");
        else
            ev_ok(id);
        return;
    }
    pthread_mutex_lock(&cmds_lock);
    struct cmd *cmd = id != NULL ? cmd_find(id) : NULL;
    pid_t_ pid = cmd != NULL && cmd->state != CMD_FAILED ? cmd->pid : 0;
    pthread_mutex_unlock(&cmds_lock);
    int err = _ESRCH;
    if (pid != 0) {
        if (strcmp(to, "session") == 0) {
            err = send_session_signal(pid, sig, info);
        } else if (strcmp(to, "group") == 0) {
            err = send_group_signal(pid, sig, info);
        } else if (strcmp(to, "process") == 0) {
            lock(&pids_lock);
            struct task *task = pid_get_task(pid);
            err = task != NULL ? 0 : _ESRCH;
            if (task != NULL)
                send_signal(task, sig, info);
            unlock(&pids_lock);
        } else {
            ev_error(id, 22, "to: expected process, group, session or all");
            return;
        }
    }
    if (err < 0)
        ev_error(id, -err, "%s: nothing left to signal", id ? id : "");
    else
        ev_ok(id);
}

static void op_stats(struct jv *req, const char *id) {
    struct jv *ids = jget(req, "ids");
    struct jb b = ev_begin("stats", id);
    uint64_t private_kb, ws_kb, cpu_ms;
    px_serve_host_stats(&private_kb, &ws_kb, &cpu_ms);
    uint64_t now = px_uptime_ms(), last = atomic_load(&last_activity_ms), out = px_output_tick;
    if (out > last && out <= now)
        last = out;
    int procs = 0;
    uint64_t mapped_kb = 0;
    struct jb per = {0};
    lock(&pids_lock);
    for (dword_t p = 2; p <= MAX_PID; p++) {
        struct task *t = pid_get_task(p);
        if (t == NULL || t->exiting || t != t->group->leader)
            continue;
        procs++;
        mapped_kb += mapped_kb_of(t);
    }
    pthread_mutex_lock(&cmds_lock);
    bool first = true;
    for (struct cmd *c = cmds; c != NULL; c = c->next) {
        if (c->state == CMD_FAILED || c->state == CMD_STARTING)
            continue;
        bool wanted = ids == NULL || ids->type != J_ARR;
        for (int i = 0; !wanted && i < ids->n; i++)
            wanted = ids->items[i].type == J_STR && strcmp(ids->items[i].str, c->id) == 0;
        if (!wanted)
            continue;
        struct usage u = session_usage(c->pid, true);
        jb_printf(&per, first ? "" : ",");
        first = false;
        jb_str(&per, c->id);
        jb_printf(&per, ":{\"cpu_ms\":%llu,\"mapped_kb\":%llu,\"procs\":%d}",
                  (unsigned long long) (c->done_cpu_ms + u.cpu_ms), (unsigned long long) u.mapped_kb, u.procs);
    }
    pthread_mutex_unlock(&cmds_lock);
    unlock(&pids_lock);
    jb_printf(&b, ",\"host\":{\"private_kb\":%llu,\"ws_kb\":%llu,\"uptime_ms\":%llu,\"idle_ms\":%llu,"
                  "\"cpu_ms\":%llu,\"procs\":%d,\"mapped_kb\":%llu},\"cmds\":{",
              (unsigned long long) private_kb, (unsigned long long) ws_kb, (unsigned long long) (now - start_ms),
              (unsigned long long) (now - last), (unsigned long long) cpu_ms, procs,
              (unsigned long long) mapped_kb);
    if (per.buf != NULL)
        jb_add(&b, per.buf, per.len);
    free(per.buf);
    jb_add(&b, "}", 1);
    jb_send(&b);
}

static void op_bind(struct jv *req, const char *id, bool bind) {
    const char *guest = jstr(req, "guest"), *host = jstr(req, "host");
    struct jv *ro = jget(req, "ro");
    if (guest == NULL || guest[0] != '/' || (bind && host == NULL)) {
        ev_error(id, 22, bind ? "expected guest and host" : "expected guest");
        return;
    }
    int err = bind ? fakefs_bind_mount(guest, host, ro != NULL && ro->type == J_BOOL && ro->b)
                   : fakefs_bind_unmount(guest);
    if (err < 0)
        ev_error(id, -err, "%s: %s", guest, err_text(-err));
    else
        ev_ok(id);
}

static void op_log(struct jv *req, const char *id) {
    px_log_set_file(jstr(req, "file"));
    ev_ok(id);
}

static int live_processes(void) {
    int n = 0;
    lock(&pids_lock);
    for (dword_t p = 2; p <= MAX_PID; p++) {
        struct task *t = pid_get_task(p);
        if (t != NULL && !t->exiting && t == t->group->leader)
            n++;
    }
    unlock(&pids_lock);
    return n;
}

static void serve_bye(void) {
    struct jb b = {0};
    jb_printf(&b, "{\"ev\":\"bye\"");
    jb_send(&b);
}

static void handle_ended(void);

// Waits up to ms for every guest process to end, reporting as they do.
static void wait_all_gone(uint64_t ms) {
    uint64_t end = px_uptime_ms() + ms;
    while (live_processes() > 0 && px_uptime_ms() < end) {
        usleep(20 * 1000);
        handle_ended();
    }
    handle_ended();
}

static noreturn void op_shutdown(struct jv *req) {
    struct jv *grace = req != NULL ? jget(req, "grace_ms") : NULL;
    uint64_t ms = grace != NULL && grace->type == J_NUM && grace->num >= 0 ? (uint64_t) grace->num : 2000;
    send_all_signal(SIGTERM_, (struct siginfo_) {.code = SI_USER_});
    wait_all_gone(ms);
    send_all_signal(SIGKILL_, (struct siginfo_) {.code = SI_USER_});
    wait_all_gone(2000);
    // init's own end stops whatever is left, unmounts (meta.db) and exits
    // the host process after halt_hook says bye
    halt_hook = serve_bye;
    do_exit(0);
}

// --- the loops ---

// The reader thread: requests from Minis. ping and state are answered here
// (no guest lock); the rest go to pid 1.
static void *reader(void *unused) {
    (void) unused;
    for (;;) {
        char *line = px_serve_read_line();
        if (line == NULL)
            break;
        if (line[0] == 0) {
            free(line);
            continue;
        }
        struct req *r = calloc(1, sizeof(*r));
        if (r == NULL || !json_parse(line, &r->v) || r->v.type != J_OBJ) {
            free(r);
            ev_error(NULL, 22, "bad request: not a JSON object");
            free(line);
            continue;
        }
        free(line);
        const char *op = jstr(&r->v, "op"), *id = jstr(&r->v, "id");
        if (op != NULL && strcmp(op, "ping") == 0) {
            struct jb b = ev_begin("pong", id);
            jb_printf(&b, ",\"uptime_ms\":%llu", (unsigned long long) (px_uptime_ms() - start_ms));
            jb_send(&b);
            jv_free(&r->v);
            free(r);
            continue;
        }
        if (op != NULL && strcmp(op, "state") == 0) {
            char *text = state_dump_text(state_tag);
            struct jb b = ev_begin("state", id);
            jb_add(&b, ",\"text\":", 8);
            jb_str(&b, text != NULL ? text : "");
            jb_send(&b);
            free(text);
            jv_free(&r->v);
            free(r);
            continue;
        }
        pthread_mutex_lock(&q_lock);
        *reqs_tail = r;
        reqs_tail = &r->next;
        pthread_cond_signal(&q_cond);
        pthread_mutex_unlock(&q_lock);
    }
    pthread_mutex_lock(&q_lock);
    q_eof = true;
    pthread_cond_signal(&q_cond);
    pthread_mutex_unlock(&q_lock);
    return NULL;
}

static void handle_ended(void) {
    reap();
    pthread_mutex_lock(&q_lock);
    struct ended *e = endeds;
    endeds = NULL;
    endeds_tail = &endeds;
    pthread_mutex_unlock(&q_lock);
    if (e == NULL)
        return;
    while (e != NULL) {
        struct ended *next = e->next;
        report_ended(e);
        free(e);
        e = next;
    }
    atomic_store(&last_activity_ms, px_uptime_ms());
    report_gone();
}

static void handle_request(struct jv *req) {
    const char *op = jstr(req, "op");
    struct jv *idv = jget(req, "id");
    const char *id = idv != NULL && idv->type == J_STR ? idv->str : NULL;
    if (op == NULL) {
        ev_error(id, 22, "missing op");
    } else if (strcmp(op, "exec") == 0) {
        if (id == NULL)
            ev_error(NULL, 22, "exec: missing id");
        else
            op_exec(req, id);
    } else if (strcmp(op, "signal") == 0) {
        op_signal(req, id);
    } else if (strcmp(op, "stats") == 0) {
        op_stats(req, id);
    } else if (strcmp(op, "bind") == 0 || strcmp(op, "unbind") == 0) {
        op_bind(req, id, op[0] == 'b');
    } else if (strcmp(op, "log") == 0) {
        op_log(req, id);
    } else if (strcmp(op, "shutdown") == 0) {
        op_shutdown(req);
    } else {
        ev_error(id, 22, "unknown op %s", op);
    }
}

static const char *engine(void) {
#ifdef GUEST_X86_64
    if (vm_wanted())
        return "vm";
#endif
#ifdef ISH_XJIT
    if (xjit_enabled())
        return "translated";
#endif
    return "gadgets";
}

noreturn void serve_main(void) {
    exit_hook = serve_exit_hook;
    ish_mem_braked_hook = serve_braked_hook;
    fakefs_set_path_translate_hook(bind_translate);
    fakefs_set_path_readonly_hook(bind_readonly);
    fakefs_set_path_reverse_hook(bind_reverse);
    state_dump_set_tag(state_tag);
    start_ms = px_uptime_ms();
    atomic_store(&last_activity_ms, start_ms);

    char rootfs[MAX_PATH + 1] = "";
    if (ish_serve_rootfs != NULL && realpath(ish_serve_rootfs, rootfs) == NULL)
        snprintf(rootfs, sizeof(rootfs), "%s", ish_serve_rootfs);
    struct jb b = {0};
    jb_printf(&b, "{\"ev\":\"ready\",\"proto\":1,\"pid\":%lu,\"version\":", px_host_pid());
    jb_str(&b, ISH_COMMIT);
    jb_printf(&b, ",\"engine\":\"%s\",\"exec_binds\":true,\"rootfs\":", engine());
    jb_str(&b, rootfs);
    jb_send(&b);

    pthread_t t;
    if (pthread_create(&t, NULL, reader, NULL) != 0)
        op_shutdown(NULL);
    pthread_detach(t);

    for (;;) {
        pthread_mutex_lock(&q_lock);
        while (reqs == NULL && endeds == NULL && !q_eof && !atomic_load(&braked)) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_sec += 1;
            if (pthread_cond_timedwait(&q_cond, &q_lock, &until) != 0)
                break;
        }
        struct req *r = reqs;
        reqs = NULL;
        reqs_tail = &reqs;
        bool eof = q_eof;
        pthread_mutex_unlock(&q_lock);

        handle_ended();
        if (atomic_exchange(&braked, false))
            out_of_memory();
        while (r != NULL) {
            struct req *next = r->next;
            handle_request(&r->v);
            jv_free(&r->v);
            free(r);
            r = next;
            handle_ended();
        }
        if (eof)
            op_shutdown(NULL);
    }
}

#endif
