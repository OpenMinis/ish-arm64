// DebugServer.c — JSON-RPC over HTTP debug server for iSH
// Allows inspecting the guest filesystem and task list via curl.
//
// Usage:
//   curl -s localhost:1234 -d '{"jsonrpc":"2.0","id":1,"method":"ping"}'
//   curl -s localhost:1234 -d '{"jsonrpc":"2.0","id":1,"method":"fs.stat","params":{"path":"/usr/lib/libpython3.11.so.1.0"}}'

// Debug-only: in Release builds the entire translation unit compiles to nothing.
#if defined(DEBUG) && defined(GUEST_ARM64)

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <poll.h>

#include "kernel/task.h"
#include "kernel/fs.h"
#include "kernel/init.h"
#include "kernel/calls.h"
#include "fs/fd.h"
#include "fs/stat.h"
#include "fs/path.h"
#include "fs/real.h"
#include "DebugServer.h"

// ---------------------------------------------------------------------------
// JSON buffer builder
// ---------------------------------------------------------------------------

#define JB_CAP 65536

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool need_comma; // track if we need comma before next value
} jbuf;

static void jb_init(jbuf *j) {
    j->buf = malloc(JB_CAP);
    j->len = 0;
    j->cap = JB_CAP;
    j->need_comma = false;
}

static void jb_ensure(jbuf *j, size_t extra) {
    while (j->len + extra >= j->cap) {
        j->cap *= 2;
        j->buf = realloc(j->buf, j->cap);
    }
}

static void jb_putc(jbuf *j, char c) {
    jb_ensure(j, 1);
    j->buf[j->len++] = c;
}

static void jb_puts(jbuf *j, const char *s) {
    size_t n = strlen(s);
    jb_ensure(j, n);
    memcpy(j->buf + j->len, s, n);
    j->len += n;
}

static void jb_printf(jbuf *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void jb_printf(jbuf *j, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    jb_ensure(j, 256);
    int n = vsnprintf(j->buf + j->len, j->cap - j->len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        if ((size_t)n >= j->cap - j->len) {
            jb_ensure(j, n + 1);
            va_start(ap, fmt);
            n = vsnprintf(j->buf + j->len, j->cap - j->len, fmt, ap);
            va_end(ap);
        }
        j->len += n;
    }
}

static void jb_comma(jbuf *j) {
    if (j->need_comma)
        jb_putc(j, ',');
    j->need_comma = false;
}

// Emit a JSON-escaped string (with quotes)
static void jb_string(jbuf *j, const char *s) {
    jb_comma(j);
    jb_putc(j, '"');
    for (; *s; s++) {
        unsigned char c = *s;
        if (c == '"')      jb_puts(j, "\\\"");
        else if (c == '\\') jb_puts(j, "\\\\");
        else if (c == '\n') jb_puts(j, "\\n");
        else if (c == '\r') jb_puts(j, "\\r");
        else if (c == '\t') jb_puts(j, "\\t");
        else if (c < 0x20)  jb_printf(j, "\\u%04x", c);
        else                 jb_putc(j, c);
    }
    jb_putc(j, '"');
    j->need_comma = true;
}

static void jb_int(jbuf *j, int64_t v) {
    jb_comma(j);
    jb_printf(j, "%lld", (long long)v);
    j->need_comma = true;
}

static void jb_uint(jbuf *j, uint64_t v) {
    jb_comma(j);
    jb_printf(j, "%llu", (unsigned long long)v);
    j->need_comma = true;
}

static void jb_bool(jbuf *j, bool v) {
    jb_comma(j);
    jb_puts(j, v ? "true" : "false");
    j->need_comma = true;
}

static void jb_null(jbuf *j) {
    jb_comma(j);
    jb_puts(j, "null");
    j->need_comma = true;
}

static void jb_key(jbuf *j, const char *k) {
    jb_comma(j);
    jb_putc(j, '"');
    jb_puts(j, k);
    jb_puts(j, "\":");
    j->need_comma = false;
}

static void jb_obj_start(jbuf *j) {
    jb_comma(j);
    jb_putc(j, '{');
    j->need_comma = false;
}

static void jb_obj_end(jbuf *j) {
    jb_putc(j, '}');
    j->need_comma = true;
}

static void jb_arr_start(jbuf *j) {
    jb_comma(j);
    jb_putc(j, '[');
    j->need_comma = false;
}

static void jb_arr_end(jbuf *j) {
    jb_putc(j, ']');
    j->need_comma = true;
}

static void jb_free(jbuf *j) {
    free(j->buf);
    j->buf = NULL;
    j->len = 0;
}

// ---------------------------------------------------------------------------
// Base64 encoder
// ---------------------------------------------------------------------------

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode(jbuf *j, const uint8_t *data, size_t len) {
    size_t i;
    for (i = 0; i + 2 < len; i += 3) {
        uint32_t v = (data[i] << 16) | (data[i+1] << 8) | data[i+2];
        jb_putc(j, b64_table[(v >> 18) & 0x3f]);
        jb_putc(j, b64_table[(v >> 12) & 0x3f]);
        jb_putc(j, b64_table[(v >>  6) & 0x3f]);
        jb_putc(j, b64_table[ v        & 0x3f]);
    }
    if (i < len) {
        uint32_t v = data[i] << 16;
        if (i + 1 < len) v |= data[i+1] << 8;
        jb_putc(j, b64_table[(v >> 18) & 0x3f]);
        jb_putc(j, b64_table[(v >> 12) & 0x3f]);
        jb_putc(j, (i + 1 < len) ? b64_table[(v >> 6) & 0x3f] : '=');
        jb_putc(j, '=');
    }
}

// ---------------------------------------------------------------------------
// Minimal JSON parser — extracts method, id, and flat params
// ---------------------------------------------------------------------------

#define MAX_PARAMS 8
#define MAX_KEY_LEN 64
#define MAX_VAL_LEN 4096

typedef struct {
    char method[MAX_KEY_LEN];
    int64_t id;
    bool has_id;
    struct {
        char key[MAX_KEY_LEN];
        char str_val[MAX_VAL_LEN];
        int64_t int_val;
        bool is_string;
    } params[MAX_PARAMS];
    int nparam;
} rpc_request;

// Skip whitespace
static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

// Parse a JSON string into buf (no unescaping of \uXXXX, just basics)
static const char *parse_string(const char *p, char *buf, size_t bufsz) {
    if (*p != '"') return NULL;
    p++;
    size_t i = 0;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            char c = *p;
            if (c == '"' || c == '\\' || c == '/') { if (i < bufsz - 1) buf[i++] = c; }
            else if (c == 'n') { if (i < bufsz - 1) buf[i++] = '\n'; }
            else if (c == 'r') { if (i < bufsz - 1) buf[i++] = '\r'; }
            else if (c == 't') { if (i < bufsz - 1) buf[i++] = '\t'; }
            else if (c == 'u') { p += 4; if (i < bufsz - 1) buf[i++] = '?'; } // lossy
            else { if (i < bufsz - 1) buf[i++] = c; }
        } else {
            if (i < bufsz - 1) buf[i++] = *p;
        }
        p++;
    }
    buf[i] = '\0';
    if (*p == '"') p++;
    return p;
}

// Skip a JSON value (string, number, object, array, true, false, null)
static const char *skip_value(const char *p) {
    p = skip_ws(p);
    if (*p == '"') {
        p++;
        while (*p && *p != '"') { if (*p == '\\') p++; p++; }
        if (*p == '"') p++;
        return p;
    }
    if (*p == '{') {
        int depth = 1; p++;
        while (*p && depth > 0) {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
            else if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\') p++; p++; } }
            p++;
        }
        return p;
    }
    if (*p == '[') {
        int depth = 1; p++;
        while (*p && depth > 0) {
            if (*p == '[') depth++;
            else if (*p == ']') depth--;
            else if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\') p++; p++; } }
            p++;
        }
        return p;
    }
    // number, true, false, null
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t')
        p++;
    return p;
}

// Parse params object (flat key-value only)
static const char *parse_params(const char *p, rpc_request *req) {
    p = skip_ws(p);
    if (*p != '{') return skip_value(p);
    p++; // skip {
    while (1) {
        p = skip_ws(p);
        if (*p == '}') { p++; break; }
        if (*p == ',') { p++; continue; }
        // key
        char key[MAX_KEY_LEN];
        p = parse_string(p, key, sizeof(key));
        if (!p) return NULL;
        p = skip_ws(p);
        if (*p == ':') p++;
        p = skip_ws(p);
        // value
        if (*p == '"') {
            if (req->nparam < MAX_PARAMS) {
                strlcpy(req->params[req->nparam].key, key, MAX_KEY_LEN);
                p = parse_string(p, req->params[req->nparam].str_val, MAX_VAL_LEN);
                req->params[req->nparam].is_string = true;
                req->nparam++;
            } else {
                p = skip_value(p);
            }
        } else if (*p == '-' || (*p >= '0' && *p <= '9')) {
            if (req->nparam < MAX_PARAMS) {
                strlcpy(req->params[req->nparam].key, key, MAX_KEY_LEN);
                req->params[req->nparam].int_val = strtoll(p, NULL, 10);
                req->params[req->nparam].is_string = false;
                req->nparam++;
            }
            p = skip_value(p);
        } else {
            p = skip_value(p);
        }
    }
    return p;
}

// Parse the top-level JSON-RPC request
static bool parse_rpc(const char *json, rpc_request *req) {
    memset(req, 0, sizeof(*req));
    const char *p = skip_ws(json);
    if (*p != '{') return false;
    p++;
    while (1) {
        p = skip_ws(p);
        if (*p == '}') break;
        if (*p == ',') { p++; continue; }
        char key[MAX_KEY_LEN];
        p = parse_string(p, key, sizeof(key));
        if (!p) return false;
        p = skip_ws(p);
        if (*p == ':') p++;
        p = skip_ws(p);
        if (strcmp(key, "method") == 0) {
            p = parse_string(p, req->method, sizeof(req->method));
            if (!p) return false;
        } else if (strcmp(key, "id") == 0) {
            if (*p == '"') {
                char tmp[64];
                p = parse_string(p, tmp, sizeof(tmp));
                req->id = strtoll(tmp, NULL, 10);
            } else {
                req->id = strtoll(p, NULL, 10);
                p = skip_value(p);
            }
            req->has_id = true;
        } else if (strcmp(key, "params") == 0) {
            p = parse_params(p, req);
            if (!p) return false;
        } else {
            p = skip_value(p);
        }
        if (!p) return false;
    }
    return req->method[0] != '\0';
}

// Param lookup helpers
static const char *param_str(const rpc_request *req, const char *key) {
    for (int i = 0; i < req->nparam; i++)
        if (req->params[i].is_string && strcmp(req->params[i].key, key) == 0)
            return req->params[i].str_val;
    return NULL;
}

static bool param_int(const rpc_request *req, const char *key, int64_t *out) {
    for (int i = 0; i < req->nparam; i++)
        if (!req->params[i].is_string && strcmp(req->params[i].key, key) == 0) {
            *out = req->params[i].int_val;
            return true;
        }
    return false;
}

// ---------------------------------------------------------------------------
// HTTP parser — find Content-Length and extract POST body
// ---------------------------------------------------------------------------

static const char *http_find_body(const char *req, size_t len, size_t *body_len) {
    // Find the blank line separating headers from body
    const char *end = req + len;
    const char *p = req;
    *body_len = 0;

    // Find Content-Length
    size_t content_length = 0;
    const char *cl = strcasestr(req, "Content-Length:");
    if (cl) {
        cl += 15;
        while (*cl == ' ') cl++;
        content_length = strtoul(cl, NULL, 10);
    }

    // Find \r\n\r\n
    const char *body = strstr(req, "\r\n\r\n");
    if (body) {
        body += 4;
        *body_len = content_length ? content_length : (end - body);
        return body;
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Context helper — borrow PID 1's `current` for fs operations
// ---------------------------------------------------------------------------

static void ensure_current(void) {
    if (current != NULL)
        return;
    lock(&pids_lock);
    struct task *init = pid_get_task(1);
    unlock(&pids_lock);
    if (init)
        current = init;
}

// ---------------------------------------------------------------------------
// RPC handlers
// ---------------------------------------------------------------------------

static void rpc_error(jbuf *j, int64_t id, int code, const char *msg) {
    jb_obj_start(j);
    jb_key(j, "jsonrpc"); jb_string(j, "2.0");
    jb_key(j, "id"); jb_int(j, id);
    jb_key(j, "error");
    jb_obj_start(j);
    jb_key(j, "code"); jb_int(j, code);
    jb_key(j, "message"); jb_string(j, msg);
    jb_obj_end(j);
    jb_obj_end(j);
}

static void rpc_result_start(jbuf *j, int64_t id) {
    jb_obj_start(j);
    jb_key(j, "jsonrpc"); jb_string(j, "2.0");
    jb_key(j, "id"); jb_int(j, id);
    jb_key(j, "result");
}

static void rpc_result_end(jbuf *j) {
    jb_obj_end(j); // close outer envelope
}

// --- ping ---
static void handle_ping(jbuf *j, int64_t id, const rpc_request *req) {
    (void)req;
    rpc_result_start(j, id);
    jb_string(j, "pong");
    rpc_result_end(j);
}

// --- fs.stat ---
static void handle_fs_stat(jbuf *j, int64_t id, const rpc_request *req) {
    const char *path = param_str(req, "path");
    if (!path) {
        rpc_error(j, id, -32602, "missing 'path' param");
        return;
    }

    ensure_current();
    struct statbuf stat;
    int err = generic_statat(AT_PWD, path, &stat, true);
    if (err < 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "statat failed: %d", err);
        rpc_error(j, id, -1, msg);
        return;
    }

    const char *type = "unknown";
    switch (stat.mode & S_IFMT) {
        case S_IFREG: type = "file"; break;
        case S_IFDIR: type = "directory"; break;
        case S_IFLNK: type = "symlink"; break;
        case S_IFCHR: type = "chardev"; break;
        case S_IFBLK: type = "blockdev"; break;
        case S_IFIFO: type = "fifo"; break;
        case S_IFSOCK: type = "socket"; break;
    }

    rpc_result_start(j, id);
    jb_obj_start(j);
    jb_key(j, "size"); jb_uint(j, stat.size);
    jb_key(j, "mode"); jb_printf(j, "\"0%o\"", stat.mode);
    j->need_comma = true;
    jb_key(j, "uid"); jb_uint(j, stat.uid);
    jb_key(j, "gid"); jb_uint(j, stat.gid);
    jb_key(j, "type"); jb_string(j, type);
    jb_key(j, "nlink"); jb_uint(j, stat.nlink);
    jb_key(j, "mtime"); jb_uint(j, stat.mtime);
    jb_key(j, "inode"); jb_uint(j, stat.inode);
    jb_obj_end(j);
    rpc_result_end(j);
}

// --- fs.readdir ---
static void handle_fs_readdir(jbuf *j, int64_t id, const rpc_request *req) {
    const char *path = param_str(req, "path");
    if (!path) {
        rpc_error(j, id, -32602, "missing 'path' param");
        return;
    }

    ensure_current();
    struct fd *dir = generic_open(path, O_RDONLY_ | O_DIRECTORY_, 0);
    if (IS_ERR(dir)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "open failed: %ld", (long)PTR_ERR(dir));
        rpc_error(j, id, -1, msg);
        return;
    }

    rpc_result_start(j, id);
    jb_arr_start(j);

    struct dir_entry entry;
    int count = 0;
    while (dir->ops->readdir && dir->ops->readdir(dir, &entry) == 1) {
        jb_obj_start(j);
        jb_key(j, "name"); jb_string(j, entry.name);
        jb_key(j, "inode"); jb_uint(j, entry.inode);
        jb_obj_end(j);
        if (++count >= 10000) break; // safety limit
    }

    jb_arr_end(j);
    rpc_result_end(j);
    fd_close(dir);
}

// --- fs.read ---
static void handle_fs_read(jbuf *j, int64_t id, const rpc_request *req) {
    const char *path = param_str(req, "path");
    if (!path) {
        rpc_error(j, id, -32602, "missing 'path' param");
        return;
    }

    int64_t offset = 0, length = 4096;
    param_int(req, "offset", &offset);
    param_int(req, "length", &length);
    if (length > 1024 * 1024) length = 1024 * 1024; // 1MB cap
    if (length < 0) length = 4096;

    ensure_current();
    struct fd *fd = generic_open(path, O_RDONLY_, 0);
    if (IS_ERR(fd)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "open failed: %ld", (long)PTR_ERR(fd));
        rpc_error(j, id, -1, msg);
        return;
    }

    // Seek to offset
    if (offset > 0 && fd->ops->lseek)
        fd->ops->lseek(fd, offset, LSEEK_SET);

    uint8_t *buf = malloc(length);
    ssize_t nread = 0;
    if (fd->ops->read)
        nread = fd->ops->read(fd, buf, length);

    rpc_result_start(j, id);
    jb_obj_start(j);
    jb_key(j, "encoding"); jb_string(j, "base64");
    jb_key(j, "size"); jb_int(j, nread > 0 ? nread : 0);
    jb_key(j, "data");
    jb_putc(j, '"');
    if (nread > 0)
        b64_encode(j, buf, nread);
    jb_putc(j, '"');
    j->need_comma = true;
    jb_obj_end(j);
    rpc_result_end(j);

    free(buf);
    fd_close(fd);
}

// --- fs.write ---
// Simple base64 decoder for fs.write binary support
static int b64_decode(const char *src, size_t src_len, unsigned char *dst, size_t *out_len) {
    static const int8_t b64_table[256] = {
        ['A']=0,['B']=1,['C']=2,['D']=3,['E']=4,['F']=5,['G']=6,['H']=7,
        ['I']=8,['J']=9,['K']=10,['L']=11,['M']=12,['N']=13,['O']=14,['P']=15,
        ['Q']=16,['R']=17,['S']=18,['T']=19,['U']=20,['V']=21,['W']=22,['X']=23,
        ['Y']=24,['Z']=25,['a']=26,['b']=27,['c']=28,['d']=29,['e']=30,['f']=31,
        ['g']=32,['h']=33,['i']=34,['j']=35,['k']=36,['l']=37,['m']=38,['n']=39,
        ['o']=40,['p']=41,['q']=42,['r']=43,['s']=44,['t']=45,['u']=46,['v']=47,
        ['w']=48,['x']=49,['y']=50,['z']=51,['0']=52,['1']=53,['2']=54,['3']=55,
        ['4']=56,['5']=57,['6']=58,['7']=59,['8']=60,['9']=61,['+']=62,['/']=63,
    };
    size_t o = 0;
    uint32_t accum = 0;
    int bits = 0;
    for (size_t i = 0; i < src_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int8_t v = b64_table[c];
        if (c != 'A' && v == 0) continue; // skip invalid
        accum = (accum << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            dst[o++] = (unsigned char)(accum >> bits);
            accum &= (1u << bits) - 1;
        }
    }
    *out_len = o;
    return 0;
}

static void handle_fs_write(jbuf *j, int64_t id, const rpc_request *req) {
    const char *path = param_str(req, "path");
    const char *data = param_str(req, "data");
    const char *data_b64 = param_str(req, "data_base64");
    if (!path) {
        rpc_error(j, id, -32602, "missing 'path' param");
        return;
    }
    if (!data && !data_b64) {
        rpc_error(j, id, -32602, "missing 'data' or 'data_base64' param");
        return;
    }

    ensure_current();
    struct fd *fd = generic_open(path, O_WRONLY_ | O_CREAT_ | O_TRUNC_, 0755);
    if (IS_ERR(fd)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "open failed: %ld", (long)PTR_ERR(fd));
        rpc_error(j, id, -1, msg);
        return;
    }

    const void *write_data;
    size_t len;
    unsigned char *decoded = NULL;

    if (data_b64) {
        size_t src_len = strlen(data_b64);
        decoded = malloc(src_len); // decoded is always <= src
        size_t decoded_len = 0;
        b64_decode(data_b64, src_len, decoded, &decoded_len);
        write_data = decoded;
        len = decoded_len;
    } else {
        write_data = data;
        len = strlen(data);
    }

    ssize_t nwritten = 0;
    if (fd->ops->write)
        nwritten = fd->ops->write(fd, write_data, len);
    fd_close(fd);
    free(decoded);

    rpc_result_start(j, id);
    jb_obj_start(j);
    jb_key(j, "written"); jb_int(j, nwritten > 0 ? nwritten : 0);
    jb_obj_end(j);
    rpc_result_end(j);
}

// --- fs.readlink ---
static void handle_fs_readlink(jbuf *j, int64_t id, const rpc_request *req) {
    const char *path = param_str(req, "path");
    if (!path) {
        rpc_error(j, id, -32602, "missing 'path' param");
        return;
    }

    ensure_current();
    char target[MAX_PATH];
    ssize_t n = generic_readlinkat(AT_PWD, path, target, sizeof(target) - 1);
    if (n < 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "readlink failed: %zd", n);
        rpc_error(j, id, -1, msg);
        return;
    }
    target[n] = '\0';

    rpc_result_start(j, id);
    jb_string(j, target);
    rpc_result_end(j);
}

// --- fs.exists ---
static void handle_fs_exists(jbuf *j, int64_t id, const rpc_request *req) {
    const char *path = param_str(req, "path");
    if (!path) {
        rpc_error(j, id, -32602, "missing 'path' param");
        return;
    }

    ensure_current();
    struct statbuf stat;
    int err = generic_statat(AT_PWD, path, &stat, true);

    rpc_result_start(j, id);
    jb_bool(j, err == 0);
    rpc_result_end(j);
}

// --- fs.hexdump ---
static void handle_fs_hexdump(jbuf *j, int64_t id, const rpc_request *req) {
    const char *path = param_str(req, "path");
    if (!path) {
        rpc_error(j, id, -32602, "missing 'path' param");
        return;
    }

    int64_t offset = 0, length = 256;
    param_int(req, "offset", &offset);
    param_int(req, "length", &length);
    if (length > 65536) length = 65536;
    if (length < 0) length = 256;

    ensure_current();
    struct fd *fd = generic_open(path, O_RDONLY_, 0);
    if (IS_ERR(fd)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "open failed: %ld", (long)PTR_ERR(fd));
        rpc_error(j, id, -1, msg);
        return;
    }

    if (offset > 0 && fd->ops->lseek)
        fd->ops->lseek(fd, offset, LSEEK_SET);

    uint8_t *buf = malloc(length);
    ssize_t nread = 0;
    if (fd->ops->read)
        nread = fd->ops->read(fd, buf, length);

    rpc_result_start(j, id);
    jb_arr_start(j);

    if (nread > 0) {
        for (ssize_t i = 0; i < nread; i += 16) {
            char line[128];
            int pos = snprintf(line, sizeof(line), "%08llx  ", (unsigned long long)(offset + i));

            // Hex bytes
            for (int k = 0; k < 16; k++) {
                if (i + k < nread)
                    pos += snprintf(line + pos, sizeof(line) - pos, "%02x ", buf[i + k]);
                else
                    pos += snprintf(line + pos, sizeof(line) - pos, "   ");
                if (k == 7)
                    pos += snprintf(line + pos, sizeof(line) - pos, " ");
            }

            // ASCII
            pos += snprintf(line + pos, sizeof(line) - pos, " |");
            for (int k = 0; k < 16 && i + k < nread; k++) {
                uint8_t c = buf[i + k];
                line[pos++] = (c >= 0x20 && c <= 0x7e) ? c : '.';
            }
            line[pos++] = '|';
            line[pos] = '\0';

            jb_string(j, line);
        }
    }

    jb_arr_end(j);
    rpc_result_end(j);

    free(buf);
    fd_close(fd);
}

// --- task.list ---
static void handle_task_list(jbuf *j, int64_t id, const rpc_request *req) {
    (void)req;
    rpc_result_start(j, id);
    jb_arr_start(j);

    lock(&pids_lock);
    for (int i = 1; i < MAX_PID; i++) {
        struct task *task = pid_get_task(i);
        if (!task) continue;
        jb_obj_start(j);
        jb_key(j, "pid"); jb_int(j, task->pid);
        jb_key(j, "comm"); jb_string(j, task->comm);
        jb_obj_end(j);
    }
    unlock(&pids_lock);

    jb_arr_end(j);
    rpc_result_end(j);
}

// --- task.info ---
static void handle_task_info(jbuf *j, int64_t id, const rpc_request *req) {
    int64_t pid = 0;
    if (!param_int(req, "pid", &pid)) {
        rpc_error(j, id, -32602, "missing 'pid' param");
        return;
    }

    lock(&pids_lock);
    struct task *task = pid_get_task_zombie((dword_t)pid);
    if (!task) {
        unlock(&pids_lock);
        rpc_error(j, id, -1, "no such pid");
        return;
    }

    rpc_result_start(j, id);
    jb_obj_start(j);
    jb_key(j, "pid"); jb_int(j, task->pid);
    jb_key(j, "tgid"); jb_int(j, task->tgid);
    jb_key(j, "uid"); jb_uint(j, task->uid);
    jb_key(j, "comm"); jb_string(j, task->comm);
    jb_key(j, "zombie"); jb_bool(j, task->zombie);
    jb_key(j, "parent_pid"); jb_int(j, task->parent ? task->parent->pid : 0);
    jb_obj_end(j);
    rpc_result_end(j);

    unlock(&pids_lock);
}

// --- guest.exec ---
// Run a command in the guest and capture stdout+stderr.
// Creates a new guest process (child of init), redirects stdout/stderr to a
// host pipe, starts the task, and reads output until the process exits or
// a timeout (default 10s) is reached.

struct exec_ctx {
    jbuf *j;
    int64_t rpc_id;
    int pipe_rd;
    int pipe_wr;
    pid_t_ guest_pid;
    int64_t timeout_ms;
    int client_fd;
};

static void *exec_reader_thread(void *arg) {
    struct exec_ctx *ctx = (struct exec_ctx *)arg;

    char *output = malloc(256 * 1024);
    size_t output_len = 0;
    size_t output_cap = 256 * 1024;
    int64_t remaining = ctx->timeout_ms;

    while (remaining > 0) {
        struct pollfd pfd = { .fd = ctx->pipe_rd, .events = POLLIN };
        int poll_timeout = remaining > 500 ? 500 : (int)remaining;
        int ret = poll(&pfd, 1, poll_timeout);
        remaining -= poll_timeout;

        if (ret > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
            while (output_len < output_cap - 1) {
                ssize_t n = read(ctx->pipe_rd, output + output_len, output_cap - 1 - output_len);
                if (n <= 0) break;
                output_len += n;
            }
            if (pfd.revents & POLLHUP)
                break;
        }

        if (ret == 0) {
            lock(&pids_lock);
            struct task *t = pid_get_task_zombie((dword_t)ctx->guest_pid);
            bool done = (t == NULL || t->zombie);
            unlock(&pids_lock);
            if (done) {
                while (output_len < output_cap - 1) {
                    ssize_t n = read(ctx->pipe_rd, output + output_len, output_cap - 1 - output_len);
                    if (n <= 0) break;
                    output_len += n;
                }
                break;
            }
        }
    }
    output[output_len] = '\0';
    close(ctx->pipe_rd);

    lock(&pids_lock);
    struct task *t = pid_get_task_zombie((dword_t)ctx->guest_pid);
    int exit_code = -1;
    bool timed_out = (remaining <= 0);
    if (t && t->zombie)
        exit_code = t->exit_code;
    unlock(&pids_lock);

    jbuf j;
    jb_init(&j);
    rpc_result_start(&j, ctx->rpc_id);
    jb_obj_start(&j);
    jb_key(&j, "pid"); jb_int(&j, ctx->guest_pid);
    jb_key(&j, "exit_code"); jb_int(&j, exit_code);
    jb_key(&j, "timed_out"); jb_bool(&j, timed_out);
    jb_key(&j, "output"); jb_string(&j, output);
    jb_obj_end(&j);
    rpc_result_end(&j);

    // Send response - client may have already disconnected, so ignore errors
    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n", j.len);
    // Set SO_NOSIGPIPE to avoid SIGPIPE on broken connection
    int on = 1;
    setsockopt(ctx->client_fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
    // Use MSG_NOSIGNAL equivalent - on macOS, SO_NOSIGPIPE is per-socket
    send(ctx->client_fd, header, hlen, 0);
    send(ctx->client_fd, j.buf, j.len, 0);
    close(ctx->client_fd);

    jb_free(&j);
    free(output);
    free(ctx);
    return NULL;
}

static void handle_guest_exec(jbuf *j, int64_t id, const rpc_request *req) {
    const char *cmd = param_str(req, "cmd");
    if (!cmd) {
        rpc_error(j, id, -32602, "missing 'cmd' param");
        return;
    }

    int64_t timeout_ms = 10000;
    param_int(req, "timeout", &timeout_ms);
    if (timeout_ms < 1000) timeout_ms = 1000;
    if (timeout_ms > 300000) timeout_ms = 300000; // 5 min max

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        rpc_error(j, id, -1, "pipe() failed");
        return;
    }
    int flags = fcntl(pipefd[0], F_GETFL);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    struct task *saved_current = current;

    int err = become_new_init_child();
    if (err < 0) {
        current = saved_current;
        close(pipefd[0]);
        close(pipefd[1]);
        char msg[128];
        snprintf(msg, sizeof(msg), "become_new_init_child failed: %d", err);
        rpc_error(j, id, -1, msg);
        return;
    }

    struct task *task = current;

    // fd0: /dev/null, fd1/fd2: pipe write end
    struct fd *devnull = adhoc_fd_create(&realfs_fdops);
    if (devnull) {
        devnull->real_fd = open("/dev/null", O_RDONLY);
        task->files->files[0] = devnull;
    }
    struct fd *out_fd = adhoc_fd_create(&realfs_fdops);
    if (out_fd) {
        out_fd->real_fd = dup(pipefd[1]);
        task->files->files[1] = out_fd;
    }
    struct fd *err_fd = adhoc_fd_create(&realfs_fdops);
    if (err_fd) {
        err_fd->real_fd = dup(pipefd[1]);
        task->files->files[2] = err_fd;
    }
    close(pipefd[1]);

    const char *shell = "/bin/sh";
    const char *dash_c = "-c";
    char argv_buf[4096];
    size_t pos = 0;
    memcpy(argv_buf + pos, shell, strlen(shell) + 1); pos += strlen(shell) + 1;
    memcpy(argv_buf + pos, dash_c, strlen(dash_c) + 1); pos += strlen(dash_c) + 1;
    memcpy(argv_buf + pos, cmd, strlen(cmd) + 1); pos += strlen(cmd) + 1;

    const char *envp =
        "TERM=dumb\0"
        "HOME=/root\0"
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\0"
#if defined(GUEST_ARM64)
        "PYTHONMALLOC=malloc\0"
#endif
        ;

    err = do_execve(shell, 3, argv_buf, envp);
    if (err < 0) {
        current = saved_current;
        close(pipefd[0]);
        char msg[128];
        snprintf(msg, sizeof(msg), "do_execve failed: %d", err);
        rpc_error(j, id, -1, msg);
        return;
    }

    pid_t_ guest_pid = task->pid;
    if (task_start(task) < 0) {
        current = saved_current;
        close(pipefd[0]);
        rpc_error(j, id, -1, "could not start task thread (out of resources)");
        return;
    }
    current = saved_current;

    // The response will be sent by the reader thread, so set a sentinel
    // in the json buffer to tell the caller not to send a response.
    // We pass client_fd via a special mechanism.
    struct exec_ctx *ctx = malloc(sizeof(struct exec_ctx));
    ctx->rpc_id = id;
    ctx->pipe_rd = pipefd[0];
    ctx->guest_pid = guest_pid;
    ctx->timeout_ms = timeout_ms;
    ctx->client_fd = -1; // will be set by caller

    // Store ctx pointer in jbuf as a sentinel
    jb_init(j); // reset
    jb_puts(j, "__EXEC_ASYNC__");
    // Stash ctx pointer after the sentinel text
    jb_ensure(j, sizeof(void *));
    memcpy(j->buf + j->len, &ctx, sizeof(void *));
    j->len += sizeof(void *);
}

// --- guest.kill ---
// Kill a guest process by PID.

#include "kernel/signal.h"

static void handle_guest_kill(jbuf *j, int64_t id, const rpc_request *req) {
    int64_t pid = -1;
    param_int(req, "pid", &pid);
    if (pid <= 0) {
        rpc_error(j, id, -32602, "missing or invalid 'pid' param");
        return;
    }

    int64_t sig = SIGKILL_;
    param_int(req, "signal", &sig);

    struct siginfo_ info = SIGINFO_NIL;
    lock(&pids_lock);
    struct task *task = pid_get_task((dword_t)pid);
    if (task) {
        send_signal(task, (int)sig, info);
    }
    unlock(&pids_lock);

    rpc_result_start(j, id);
    jb_obj_start(j);
    jb_key(j, "pid"); jb_int(j, pid);
    jb_key(j, "found"); jb_bool(j, task != NULL);
    jb_key(j, "signal"); jb_int(j, sig);
    jb_obj_end(j);
    rpc_result_end(j);
}

// ---------------------------------------------------------------------------
// Dispatcher
// ---------------------------------------------------------------------------

typedef void (*rpc_handler_fn)(jbuf *j, int64_t id, const rpc_request *req);

static struct {
    const char *method;
    rpc_handler_fn handler;
} rpc_methods[] = {
    { "ping",        handle_ping },
    { "fs.stat",     handle_fs_stat },
    { "fs.readdir",  handle_fs_readdir },
    { "fs.read",     handle_fs_read },
    { "fs.write",    handle_fs_write },
    { "fs.readlink", handle_fs_readlink },
    { "fs.exists",   handle_fs_exists },
    { "fs.hexdump",  handle_fs_hexdump },
    { "task.list",   handle_task_list },
    { "task.info",   handle_task_info },
    { "guest.exec",  handle_guest_exec },
    { "guest.kill",  handle_guest_kill },
    { NULL, NULL },
};

static void dispatch_rpc(const char *json_body, jbuf *j) {
    rpc_request req;
    if (!parse_rpc(json_body, &req)) {
        rpc_error(j, 0, -32700, "parse error");
        return;
    }

    for (int i = 0; rpc_methods[i].method; i++) {
        if (strcmp(rpc_methods[i].method, req.method) == 0) {
            rpc_methods[i].handler(j, req.id, &req);
            return;
        }
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "unknown method: %s", req.method);
    rpc_error(j, req.id, -32601, msg);
}

// ---------------------------------------------------------------------------
// HTTP handler
// ---------------------------------------------------------------------------

static void handle_connection(int client_fd) {
    // Read the full HTTP request (up to 64KB)
    char *reqbuf = malloc(65536);
    size_t total = 0;
    while (total < 65535) {
        ssize_t n = read(client_fd, reqbuf + total, 65535 - total);
        if (n <= 0) break;
        total += n;
        reqbuf[total] = '\0';
        // Check if we have the full body
        size_t body_len;
        const char *body = http_find_body(reqbuf, total, &body_len);
        if (body && (size_t)(total - (body - reqbuf)) >= body_len)
            break;
    }
    reqbuf[total] = '\0';

    // Extract the JSON body
    size_t body_len;
    const char *body = http_find_body(reqbuf, total, &body_len);

    jbuf j;
    jb_init(&j);

    if (!body || body_len == 0) {
        // Might be a bare JSON POST without HTTP headers
        // Try parsing reqbuf directly as JSON
        const char *p = skip_ws(reqbuf);
        if (*p == '{') {
            dispatch_rpc(p, &j);
        } else {
            rpc_error(&j, 0, -32700, "no JSON body found");
        }
    } else {
        // Make a null-terminated copy of the body
        char *json = malloc(body_len + 1);
        memcpy(json, body, body_len);
        json[body_len] = '\0';
        dispatch_rpc(json, &j);
        free(json);
    }

    // Check for async exec sentinel
    if (j.len > 14 && memcmp(j.buf, "__EXEC_ASYNC__", 14) == 0) {
        // Extract the exec_ctx pointer from after the sentinel
        struct exec_ctx *ctx;
        memcpy(&ctx, j.buf + 14, sizeof(void *));
        ctx->client_fd = client_fd;
        jb_free(&j);
        free(reqbuf);
        // Spawn reader thread — it will send the response and close client_fd
        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&tid, &attr, exec_reader_thread, ctx);
        pthread_attr_destroy(&attr);
        return; // don't close client_fd — reader thread owns it
    }

    // Send HTTP response
    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n", j.len);

    write(client_fd, header, hlen);
    write(client_fd, j.buf, j.len);

    jb_free(&j);
    free(reqbuf);
    close(client_fd);
}

// ---------------------------------------------------------------------------
// Server thread
// ---------------------------------------------------------------------------

static void *connection_thread(void *arg) {
    int client_fd = *(int *)arg;
    free(arg);
    handle_connection(client_fd);
    return NULL;
}

static void *server_thread(void *arg) {
    int port = (int)(intptr_t)arg;

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        fprintf(stderr, "[DebugServer] socket failed: %s\n", strerror(errno));
        return NULL;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[DebugServer] bind port %d failed: %s\n", port, strerror(errno));
        close(server_fd);
        return NULL;
    }

    if (listen(server_fd, 5) < 0) {
        fprintf(stderr, "[DebugServer] listen failed: %s\n", strerror(errno));
        close(server_fd);
        return NULL;
    }

    fprintf(stderr, "[DebugServer] listening on 127.0.0.1:%d\n", port);

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[DebugServer] accept failed: %s\n", strerror(errno));
            continue;
        }
        // Set a read timeout so slow/stuck clients don't block the handler thread
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // Handle each connection in its own detached thread
        pthread_t conn_tid;
        pthread_attr_t conn_attr;
        pthread_attr_init(&conn_attr);
        pthread_attr_setdetachstate(&conn_attr, PTHREAD_CREATE_DETACHED);
        int *fd_arg = malloc(sizeof(int));
        *fd_arg = client_fd;
        if (pthread_create(&conn_tid, &conn_attr, connection_thread, fd_arg) != 0) {
            // Fallback: handle synchronously
            free(fd_arg);
            handle_connection(client_fd);
        }
        pthread_attr_destroy(&conn_attr);
    }

    return NULL;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void debug_server_start(int port) {
    // Ignore SIGPIPE globally so send() to disconnected clients returns EPIPE
    // instead of killing the process (especially exec_reader_thread)
    signal(SIGPIPE, SIG_IGN);

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int err = pthread_create(&tid, &attr, server_thread, (void *)(intptr_t)port);
    pthread_attr_destroy(&attr);
    if (err != 0)
        fprintf(stderr, "[DebugServer] pthread_create failed: %s\n", strerror(err));
}

#endif // defined(DEBUG) && defined(GUEST_ARM64)
