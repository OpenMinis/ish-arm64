// Sockets. Callers speak Linux constants and sockaddr layouts; Winsock gets
// its own. Every socket is non-blocking underneath; a blocking caller waits in
// px_wsapoll, which a guest signal can interrupt.
#include "px_impl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mstcpip.h>
#include <afunix.h>
#include <iphlpapi.h>
#include <icmpapi.h>

int px_errno_wsa(int err) {
    switch (err) {
        case WSAEWOULDBLOCK: return EAGAIN;
        case WSAEINPROGRESS: return EINPROGRESS;
        case WSAEALREADY: return EALREADY;
        case WSAENOTSOCK: return ENOTSOCK;
        case WSAEDESTADDRREQ: return EDESTADDRREQ;
        case WSAEMSGSIZE: return EMSGSIZE;
        case WSAEPROTOTYPE: return EPROTOTYPE;
        case WSAENOPROTOOPT: return ENOPROTOOPT;
        case WSAEPROTONOSUPPORT: return EPROTONOSUPPORT;
        case WSAESOCKTNOSUPPORT: return EPROTONOSUPPORT;
        case WSAEOPNOTSUPP: return EOPNOTSUPP;
        case WSAEPFNOSUPPORT:
        case WSAEAFNOSUPPORT: return EAFNOSUPPORT;
        case WSAEADDRINUSE: return EADDRINUSE;
        case WSAEADDRNOTAVAIL: return EADDRNOTAVAIL;
        case WSAENETDOWN: return ENETDOWN;
        case WSAENETUNREACH: return ENETUNREACH;
        case WSAENETRESET: return ENETRESET;
        case WSAECONNABORTED: return ECONNABORTED;
        case WSAECONNRESET: return ECONNRESET;
        case WSAENOBUFS: return ENOBUFS;
        case WSAEISCONN: return EISCONN;
        case WSAENOTCONN: return ENOTCONN;
        case WSAESHUTDOWN: return EPIPE;
        case WSAETIMEDOUT: return ETIMEDOUT;
        case WSAECONNREFUSED: return ECONNREFUSED;
        case WSAEHOSTDOWN: return PX_EHOSTDOWN;
        case WSAEHOSTUNREACH: return EHOSTUNREACH;
        case WSAEACCES: return EACCES;
        case WSAEFAULT: return EFAULT;
        case WSAEINVAL: return EINVAL;
        case WSAEMFILE: return EMFILE;
        case WSAEINTR: return EINTR;
        case WSAEBADF: return EBADF;
        case WSA_NOT_ENOUGH_MEMORY: return ENOMEM;
        case WSANOTINITIALISED: return EIO;
        default: return EIO;
    }
}

// ---- translation

static int af_to_win(int af) {
    switch (af) {
        case 0: return AF_UNSPEC;
        case 1: return AF_UNIX;
        case 2: return AF_INET;
        case 10: return AF_INET6;
        default: return -1;
    }
}

static int af_from_win(int af) {
    switch (af) {
        case AF_UNIX: return 1;
        case AF_INET: return 2;
        case AF_INET6: return 10;
        default: return af;
    }
}

// Linux sockaddr -> Winsock sockaddr. They differ in the family number (and
// for AF_UNIX, the path separator).
static int addr_to_win(const void *addr, int len, struct sockaddr_storage *out, int *outlen) {
    if (len < 2 || len > (int) sizeof(*out))
        return px_fail(EINVAL);
    memcpy(out, addr, len);
    int af = af_to_win(((const unsigned short *) addr)[0]);
    if (af < 0)
        return px_fail(EAFNOSUPPORT);
    out->ss_family = af;
    *outlen = len;
    if (af == AF_UNIX) {
        struct sockaddr_un *un = (struct sockaddr_un *) out;
        for (char *p = un->sun_path; p < (char *) out + len && *p; p++)
            if (*p == '/')
                *p = '\\';
    }
    return 0;
}

static void addr_from_win(struct sockaddr_storage *in, int inlen, void *addr, px_socklen_t *len) {
    if (addr == NULL || len == NULL)
        return;
    if (inlen > 0 && in->ss_family == AF_UNIX) {
        struct sockaddr_un *un = (struct sockaddr_un *) in;
        for (char *p = un->sun_path; p < (char *) in + inlen && *p; p++)
            if (*p == '\\')
                *p = '/';
    }
    in->ss_family = af_from_win(in->ss_family);
    int n = inlen < *len ? inlen : *len;
    memcpy(addr, in, n);
    *len = inlen;
}

static int msg_flags_to_win(int flags, bool *dontwait) {
    int out = 0;
    if (flags & 0x1) out |= MSG_OOB;
    if (flags & 0x2) out |= MSG_PEEK;
    if (flags & 0x4) out |= MSG_DONTROUTE;
    if (flags & 0x100) out |= MSG_WAITALL;
    *dontwait = flags & 0x40;
    return out;
}

// ---- sockets in the fd table

static struct px_desc *sock_get(int fd) {
    struct px_desc *d = px_get(fd);
    if (d != NULL && d->kind != PX_SOCK) {
        px_put(d);
        errno = ENOTSOCK;
        return NULL;
    }
    return d;
}

static SOCKET sock_new(int af, int type, int protocol) {
    SOCKET s = WSASocketW(af, type, protocol, NULL, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET)
        return s;
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
    if (type == SOCK_STREAM && af != AF_UNIX) {
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *) &one, sizeof(one));
    }
    return s;
}

static int sock_install(SOCKET s, int domain, int type, int protocol, int flags) {
    struct px_desc *d = px_desc_new(PX_SOCK);
    if (d == NULL) {
        closesocket(s);
        return px_fail(ENOMEM);
    }
    d->s = s;
    d->flags = PX_O_RDWR | ((flags & 04000 /* SOCK_NONBLOCK */) ? PX_O_NONBLOCK : 0);
    d->domain = domain;
    d->type = type;
    d->protocol = protocol;
    return px_install(d, 0, flags & 02000000 /* SOCK_CLOEXEC */);
}

// ---- ICMP datagram sockets
//
// Linux (and Darwin) let unprivileged programs ping through SOCK_DGRAM
// IPPROTO_ICMP sockets: write an echo request, read back the reply without
// its IP header. Winsock has no such socket, so emulate one over
// IcmpSendEcho2: each echo request goes to a worker thread, and the reply it
// gets is rebuilt as an ICMP echo reply and queued on a loopback datagram
// pair, prefixed with the address it came from. The read end is what the fd
// polls and reads.

struct icmp_job {
    SOCKET tx;
    IPAddr dest;
    uint16_t id, seq;
    size_t len;
    unsigned char data[];
};

static uint16_t icmp_checksum(const unsigned char *p, size_t n) {
    uint32_t sum = 0;
    for (; n > 1; p += 2, n -= 2)
        sum += (p[0] << 8) | p[1];
    if (n)
        sum += p[0] << 8;
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t) ~sum;
}

static DWORD WINAPI icmp_worker(void *param) {
    struct icmp_job *job = param;
    HANDLE icmp = IcmpCreateFile();
    size_t reply_size = sizeof(ICMP_ECHO_REPLY) + job->len + 8 + 64;
    void *reply = malloc(reply_size);
    if (icmp != INVALID_HANDLE_VALUE &&
            IcmpSendEcho2(icmp, NULL, NULL, NULL, job->dest, job->data, (WORD) job->len, NULL,
                reply, (DWORD) reply_size, 4000) > 0) {
        ICMP_ECHO_REPLY *r = reply;
        if (r->Status == IP_SUCCESS) {
            size_t n = r->DataSize;
            unsigned char *pkt = malloc(8 + 8 + n);
            memcpy(pkt, &r->Address, 4);
            pkt[4] = r->Options.Ttl;
            pkt[5] = pkt[6] = pkt[7] = 0;
            unsigned char *icmp_hdr = pkt + 8;
            icmp_hdr[0] = 0; // echo reply
            icmp_hdr[1] = 0;
            icmp_hdr[2] = icmp_hdr[3] = 0;
            icmp_hdr[4] = job->id >> 8;
            icmp_hdr[5] = job->id & 0xff;
            icmp_hdr[6] = job->seq >> 8;
            icmp_hdr[7] = job->seq & 0xff;
            memcpy(icmp_hdr + 8, r->Data, n);
            uint16_t sum = icmp_checksum(icmp_hdr, 8 + n);
            icmp_hdr[2] = sum >> 8;
            icmp_hdr[3] = sum & 0xff;
            send(job->tx, (char *) pkt, (int) (8 + 8 + n), 0);
            free(pkt);
        }
    }
    if (icmp != INVALID_HANDLE_VALUE)
        IcmpCloseHandle(icmp);
    free(reply);
    closesocket(job->tx);
    free(job);
    return 0;
}

static int icmp_socket_new(int flags) {
    SOCKET pair[2];
    if (px_sock_pair(SOCK_DGRAM, pair) < 0)
        return -1;
    struct px_desc *d = px_desc_new(PX_SOCK);
    d->s = pair[0];
    d->icmp_tx = pair[1];
    d->is_icmp = true;
    d->flags = PX_O_RDWR | ((flags & 04000) ? PX_O_NONBLOCK : 0);
    d->domain = 2;
    d->type = 2;
    d->protocol = 1;
    return px_install(d, 0, flags & 02000000);
}

static ssize_t icmp_send(struct px_desc *d, const void *buf, size_t n, const void *addr, px_socklen_t len) {
    const struct sockaddr_in *to = addr;
    if (to == NULL)
        to = &d->icmp_peer;
    else if (len < (px_socklen_t) sizeof(struct sockaddr_in))
        return px_fail(EINVAL);
    if (to->sin_addr.s_addr == 0 && addr == NULL)
        return px_fail(EDESTADDRREQ);
    const unsigned char *p = buf;
    if (n < 8 || p[0] != 8 /* echo request */)
        return px_fail(EINVAL);
    struct icmp_job *job = malloc(sizeof(*job) + n - 8);
    // The worker owns a duplicate of the write end: the fd may close first.
    WSAPROTOCOL_INFOW info;
    if (WSADuplicateSocketW(d->icmp_tx, GetCurrentProcessId(), &info) != 0 ||
            (job->tx = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &info, 0,
                WSA_FLAG_OVERLAPPED)) == INVALID_SOCKET) {
        free(job);
        return px_fail_wsa();
    }
    job->dest = to->sin_addr.s_addr;
    job->id = (p[4] << 8) | p[5];
    job->seq = (p[6] << 8) | p[7];
    job->len = n - 8;
    memcpy(job->data, p + 8, n - 8);
    HANDLE t = CreateThread(NULL, 0, icmp_worker, job, 0, NULL);
    if (t == NULL) {
        closesocket(job->tx);
        free(job);
        return px_fail_win();
    }
    CloseHandle(t);
    return n;
}

int px_socket(int domain, int type, int protocol) {
    px_init();
    int flags = type & (04000 | 02000000);
    type &= 0xf;
    if (domain == 2 && type == SOCK_DGRAM && protocol == 1 /* IPPROTO_ICMP */)
        return icmp_socket_new(flags);
    int af = af_to_win(domain);
    if (af < 0 || af == AF_UNSPEC)
        return px_fail(EAFNOSUPPORT);
    SOCKET s = sock_new(af, type, protocol);
    if (s == INVALID_SOCKET)
        return px_fail_wsa();
    return sock_install(s, domain, type, protocol, flags);
}

static SOCKET loopback_listener(int af, struct sockaddr_storage *addr, int *len) {
    SOCKET l = sock_new(af, SOCK_STREAM, 0);
    if (l == INVALID_SOCKET)
        return l;
    memset(addr, 0, sizeof(*addr));
    if (af == AF_INET6) {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *) addr;
        a->sin6_family = AF_INET6;
        a->sin6_addr = in6addr_loopback;
        *len = sizeof(*a);
    } else {
        struct sockaddr_in *a = (struct sockaddr_in *) addr;
        a->sin_family = AF_INET;
        a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        *len = sizeof(*a);
    }
    if (bind(l, (struct sockaddr *) addr, *len) == SOCKET_ERROR ||
            listen(l, 1) == SOCKET_ERROR ||
            getsockname(l, (struct sockaddr *) addr, len) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        closesocket(l);
        WSASetLastError(err);
        return INVALID_SOCKET;
    }
    return l;
}

int px_sock_pair(int type, SOCKET out[2]) {
    struct sockaddr_storage addr;
    int len;
    if (type == SOCK_DGRAM) {
        SOCKET a = sock_new(AF_INET, SOCK_DGRAM, 0), b = sock_new(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in sa = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        struct sockaddr_in aa, ba;
        int alen = sizeof(aa), blen = sizeof(ba);
        if (a == INVALID_SOCKET || b == INVALID_SOCKET ||
                bind(a, (struct sockaddr *) &sa, sizeof(sa)) || bind(b, (struct sockaddr *) &sa, sizeof(sa)) ||
                getsockname(a, (struct sockaddr *) &aa, &alen) || getsockname(b, (struct sockaddr *) &ba, &blen) ||
                connect(a, (struct sockaddr *) &ba, blen) || connect(b, (struct sockaddr *) &aa, alen)) {
            int err = WSAGetLastError();
            if (a != INVALID_SOCKET) closesocket(a);
            if (b != INVALID_SOCKET) closesocket(b);
            return px_fail(px_errno_wsa(err));
        }
        out[0] = a;
        out[1] = b;
        return 0;
    }
    SOCKET l = loopback_listener(AF_INET, &addr, &len);
    if (l == INVALID_SOCKET)
        return px_fail_wsa();
    SOCKET c = sock_new(AF_INET, SOCK_STREAM, 0);
    if (c == INVALID_SOCKET)
        goto fail;
    if (connect(c, (struct sockaddr *) &addr, len) == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)
        goto fail;
    SOCKET a = INVALID_SOCKET;
    for (int i = 0; i < 1000 && a == INVALID_SOCKET; i++) {
        WSAPOLLFD p = {.fd = l, .events = POLLRDNORM};
        WSAPoll(&p, 1, 10);
        struct sockaddr_storage peer;
        int plen = sizeof(peer);
        a = accept(l, (struct sockaddr *) &peer, &plen);
        if (a != INVALID_SOCKET) {
            // only our own connect, not a stranger that raced to the port
            struct sockaddr_storage mine;
            int mlen = sizeof(mine);
            getsockname(c, (struct sockaddr *) &mine, &mlen);
            if (mlen != plen || memcmp(&mine, &peer, plen) != 0) {
                closesocket(a);
                a = INVALID_SOCKET;
            }
        } else if (WSAGetLastError() != WSAEWOULDBLOCK) {
            goto fail;
        }
    }
    if (a == INVALID_SOCKET)
        goto fail;
    closesocket(l);
    u_long on = 1;
    ioctlsocket(a, FIONBIO, &on);
    int one = 1;
    setsockopt(a, IPPROTO_TCP, TCP_NODELAY, (char *) &one, sizeof(one));
    // wait for the connect to finish on our side
    WSAPOLLFD p = {.fd = c, .events = POLLWRNORM};
    WSAPoll(&p, 1, 1000);
    out[0] = a;
    out[1] = c;
    return 0;
fail:;
    int err = WSAGetLastError();
    closesocket(l);
    if (c != INVALID_SOCKET)
        closesocket(c);
    return px_fail(px_errno_wsa(err));
}

int px_socketpair(int domain, int type, int protocol, int sv[2]) {
    (void) domain; (void) protocol;
    px_init();
    int flags = type & (04000 | 02000000);
    type &= 0xf;
    SOCKET s[2];
    if (px_sock_pair(type == 2 ? SOCK_DGRAM : SOCK_STREAM, s) < 0)
        return -1;
    sv[0] = sock_install(s[0], 1, type, 0, flags);
    sv[1] = sock_install(s[1], 1, type, 0, flags);
    return 0;
}

int px_sock_wait(SOCKET s, short events, int timeout_ms) {
    WSAPOLLFD p = {.fd = s, .events = events};
    return px_wsapoll(&p, 1, timeout_ms);
}

// Retries op until it stops saying WSAEWOULDBLOCK, waiting for `events` in
// between, unless the caller asked not to block.
#define BLOCKING(d, dontwait, events, timeout, op) ({ \
    __typeof__(op) _res; \
    for (;;) { \
        _res = (op); \
        if (_res != (__typeof__(op)) SOCKET_ERROR) break; \
        int _err = WSAGetLastError(); \
        if (_err != WSAEWOULDBLOCK || (dontwait) || ((d)->flags & PX_O_NONBLOCK)) { \
            errno = px_errno_wsa(_err); \
            if ((d)->is_pipe && (errno == ECONNRESET || errno == ECONNABORTED)) errno = EPIPE; \
            break; \
        } \
        int _w = px_sock_wait((d)->s, (events), (timeout)); \
        if (_w < 0) break; \
        if (_w == 0 && (timeout) >= 0) { errno = EAGAIN; break; } \
    } \
    _res; \
})

static int timeout_of(int64_t ms) {
    return ms > 0 ? (int) ms : -1;
}

ssize_t px_sock_read(struct px_desc *d, void *buf, size_t n) {
    if (n > INT_MAX)
        n = INT_MAX;
    int res = BLOCKING(d, false, POLLRDNORM, timeout_of(d->rcvtimeo_ms), recv(d->s, buf, (int) n, 0));
    if (res == SOCKET_ERROR) {
        // a reset pipe or stdin reads as EOF
        if (d->is_pipe && errno == EPIPE)
            return 0;
        return -1;
    }
    return res;
}

ssize_t px_sock_write(struct px_desc *d, const void *buf, size_t n) {
    if (n > INT_MAX)
        n = INT_MAX;
    int res = BLOCKING(d, false, POLLWRNORM, timeout_of(d->sndtimeo_ms), send(d->s, buf, (int) n, 0));
    return res == SOCKET_ERROR ? -1 : res;
}

int px_sock_ioctl_fionread(struct px_desc *d, int *out) {
    u_long n = 0;
    if (ioctlsocket(d->s, FIONREAD, &n) == SOCKET_ERROR)
        return px_fail_wsa();
    *out = (int) n;
    return 0;
}

int px_bind(int fd, const void *addr, px_socklen_t len) {
    struct px_desc *d = sock_get(fd);
    if (d == NULL)
        return -1;
    if (d->is_icmp) {
        px_put(d);
        return 0;
    }
    struct sockaddr_storage wa;
    int wlen, res = addr_to_win(addr, len, &wa, &wlen);
    if (res == 0 && bind(d->s, (struct sockaddr *) &wa, wlen) == SOCKET_ERROR)
        res = px_fail_wsa();
    px_put(d);
    return res;
}

int px_connect(int fd, const void *addr, px_socklen_t len) {
    struct px_desc *d = sock_get(fd);
    if (d == NULL)
        return -1;
    if (d->is_icmp) {
        if (len >= (px_socklen_t) sizeof(struct sockaddr_in))
            memcpy(&d->icmp_peer, addr, sizeof(d->icmp_peer));
        px_put(d);
        return 0;
    }
    struct sockaddr_storage wa;
    int wlen, res = addr_to_win(addr, len, &wa, &wlen);
    if (res < 0)
        goto out;
    if (connect(d->s, (struct sockaddr *) &wa, wlen) == 0)
        goto out;
    int err = WSAGetLastError();
    if (err != WSAEWOULDBLOCK) {
        res = px_fail(px_errno_wsa(err));
        goto out;
    }
    if (d->type == SOCK_DGRAM)
        goto out;
    if (d->flags & PX_O_NONBLOCK) {
        res = px_fail(EINPROGRESS);
        goto out;
    }
    WSAPOLLFD p = {.fd = d->s, .events = POLLWRNORM};
    if (px_wsapoll(&p, 1, -1) < 0) {
        res = -1;
        goto out;
    }
    int soerr = 0, slen = sizeof(soerr);
    getsockopt(d->s, SOL_SOCKET, SO_ERROR, (char *) &soerr, &slen);
    if (soerr != 0)
        res = px_fail(px_errno_wsa(soerr));
out:
    px_put(d);
    return res;
}

int px_listen(int fd, int backlog) {
    struct px_desc *d = sock_get(fd);
    if (d == NULL)
        return -1;
    int res = listen(d->s, backlog > SOMAXCONN ? SOMAXCONN : backlog) == SOCKET_ERROR ? px_fail_wsa() : 0;
    px_put(d);
    return res;
}

int px_accept4(int fd, void *addr, px_socklen_t *len, int flags) {
    struct px_desc *d = sock_get(fd);
    if (d == NULL)
        return -1;
    struct sockaddr_storage wa;
    int wlen = sizeof(wa);
    SOCKET s = BLOCKING(d, false, POLLRDNORM, -1, accept(d->s, (struct sockaddr *) &wa, &wlen));
    int res;
    if (s == INVALID_SOCKET) {
        res = -1;
    } else {
        u_long on = 1;
        ioctlsocket(s, FIONBIO, &on);
        addr_from_win(&wa, wlen, addr, len);
        res = sock_install(s, d->domain, d->type, d->protocol, flags);
    }
    px_put(d);
    return res;
}

int px_accept(int fd, void *addr, px_socklen_t *len) {
    return px_accept4(fd, addr, len, 0);
}

ssize_t px_send(int fd, const void *buf, size_t n, int flags) {
    return px_sendto(fd, buf, n, flags, NULL, 0);
}

ssize_t px_recv(int fd, void *buf, size_t n, int flags) {
    return px_recvfrom(fd, buf, n, flags, NULL, NULL);
}

ssize_t px_sendto(int fd, const void *buf, size_t n, int flags, const void *addr, px_socklen_t len) {
    struct px_desc *d = sock_get(fd);
    if (d == NULL)
        return -1;
    bool dontwait;
    int wflags = msg_flags_to_win(flags, &dontwait);
    struct sockaddr_storage wa;
    int wlen = 0;
    ssize_t res;
    if (d->is_icmp) {
        res = icmp_send(d, buf, n, addr, len);
        px_put(d);
        return res;
    }
    if (addr != NULL && addr_to_win(addr, len, &wa, &wlen) < 0) {
        res = -1;
    } else {
        if (n > INT_MAX)
            n = INT_MAX;
        int r = BLOCKING(d, dontwait, POLLWRNORM, timeout_of(d->sndtimeo_ms),
                addr ? sendto(d->s, buf, (int) n, wflags, (struct sockaddr *) &wa, wlen)
                     : send(d->s, buf, (int) n, wflags));
        res = r == SOCKET_ERROR ? -1 : r;
    }
    px_put(d);
    return res;
}

// One queued reply: strips the prefix (source address, TTL), fills in a Linux
// sockaddr_in and the TTL. Like Darwin's ICMP datagram sockets, the reply
// comes with an IPv4 header unless IP_STRIPHDR was set: iSH maps the guest's
// SOCK_RAW onto these, and raw readers (busybox ping as root) parse it.
static ssize_t icmp_recv(struct px_desc *d, void *buf, size_t n, bool dontwait, int *trunc,
        void *addr, px_socklen_t *len, int *ttl) {
    char *pkt = malloc(65536 + 64);
    int got = BLOCKING(d, dontwait, POLLRDNORM, timeout_of(d->rcvtimeo_ms), recv(d->s, pkt, 65000, 0));
    if (got == SOCKET_ERROR || got < 8) {
        free(pkt);
        return got == SOCKET_ERROR ? -1 : px_fail(EAGAIN);
    }
    size_t body = got - 8;
    char *src = pkt + 8;
    if (!d->icmp_striphdr) {
        // synthesize the IP header in front of the ICMP message
        // the prefix (address, TTL) sits in bytes 0-7, which the move keeps
        memmove(pkt + 20, pkt + 8, body);
        unsigned char *ip = (unsigned char *) pkt;
        unsigned char ttl8 = (unsigned char) ip[4];
        uint32_t from;
        memcpy(&from, ip, 4);
        memset(ip, 0, 20);
        ip[0] = 0x45;
        ip[2] = (20 + body) >> 8;
        ip[3] = (20 + body) & 0xff;
        ip[8] = ttl8;
        ip[9] = 1; // ICMP
        memcpy(ip + 12, &from, 4);
        ip[16] = 127; ip[19] = 1; // to: us
        uint16_t sum = icmp_checksum(ip, 20);
        ip[10] = sum >> 8;
        ip[11] = sum & 0xff;
        if (ttl != NULL)
            *ttl = ttl8;
        memcpy(pkt + 65536, &from, 4); // keep the source for the address below
        body += 20;
        src = pkt;
    } else if (ttl != NULL) {
        *ttl = (unsigned char) pkt[4];
    }
    *trunc = body > n;
    memcpy(buf, src, body < n ? body : n);
    if (addr != NULL && len != NULL) {
        struct { uint16_t family, port; uint32_t addr; char zero[8]; } in = {2, 0, 0, {0}};
        memcpy(&in.addr, d->icmp_striphdr ? pkt : pkt + 65536, 4);
        size_t c = (size_t) *len < sizeof(in) ? (size_t) *len : sizeof(in);
        memcpy(addr, &in, c);
        *len = sizeof(in);
    }
    free(pkt);
    return body < n ? body : n;
}

ssize_t px_recvfrom(int fd, void *buf, size_t n, int flags, void *addr, px_socklen_t *len) {
    struct px_desc *d = sock_get(fd);
    if (d == NULL)
        return -1;
    bool dontwait;
    int wflags = msg_flags_to_win(flags, &dontwait);
    if (d->is_icmp) {
        int trunc;
        ssize_t r = icmp_recv(d, buf, n, dontwait, &trunc, addr, len, NULL);
        px_put(d);
        return r;
    }
    struct sockaddr_storage wa;
    int wlen = sizeof(wa);
    if (n > INT_MAX)
        n = INT_MAX;
    int r = BLOCKING(d, dontwait, POLLRDNORM, timeout_of(d->rcvtimeo_ms),
            recvfrom(d->s, buf, (int) n, wflags, (struct sockaddr *) &wa, &wlen));
    ssize_t res;
    if (r == SOCKET_ERROR) {
        // Winsock reports a datagram larger than the buffer as an error;
        // Linux truncates it and says so in the flags.
        if (errno == EMSGSIZE) {
            r = (int) n;
        } else if (d->is_pipe && errno == EPIPE) {
            r = 0;
        } else {
            px_put(d);
            return -1;
        }
    }
    if (addr != NULL && d->type != SOCK_STREAM)
        addr_from_win(&wa, wlen, addr, len);
    else if (len != NULL && d->type == SOCK_STREAM && addr != NULL)
        *len = 0;
    res = r;
    px_put(d);
    return res;
}

ssize_t px_sendmsg(int fd, const struct px_msghdr *msg, int flags) {
    struct px_desc *d = sock_get(fd);
    if (d == NULL)
        return -1;
    if (msg->msg_controllen != 0) {
        // no fd passing (SCM_RIGHTS) between Winsock sockets
        px_put(d);
        return px_fail(EOPNOTSUPP);
    }
    bool dontwait;
    int wflags = msg_flags_to_win(flags, &dontwait);
    if (d->is_icmp) {
        size_t total = 0;
        for (size_t i = 0; i < msg->msg_iovlen; i++)
            total += msg->msg_iov[i].iov_len;
        char *flat = malloc(total ? total : 1);
        for (size_t i = 0, off = 0; i < msg->msg_iovlen; off += msg->msg_iov[i].iov_len, i++)
            memcpy(flat + off, msg->msg_iov[i].iov_base, msg->msg_iov[i].iov_len);
        ssize_t r = icmp_send(d, flat, total, msg->msg_name, msg->msg_namelen);
        free(flat);
        px_put(d);
        return r;
    }
    WSABUF bufs[64];
    DWORD nbufs = msg->msg_iovlen < 64 ? (DWORD) msg->msg_iovlen : 64;
    for (DWORD i = 0; i < nbufs; i++) {
        bufs[i].buf = msg->msg_iov[i].iov_base;
        bufs[i].len = (ULONG) msg->msg_iov[i].iov_len;
    }
    struct sockaddr_storage wa;
    int wlen = 0;
    if (msg->msg_name != NULL && addr_to_win(msg->msg_name, msg->msg_namelen, &wa, &wlen) < 0) {
        px_put(d);
        return -1;
    }
    DWORD sent = 0;
    int r = BLOCKING(d, dontwait, POLLWRNORM, timeout_of(d->sndtimeo_ms),
            WSASendTo(d->s, bufs, nbufs, &sent, wflags, msg->msg_name ? (struct sockaddr *) &wa : NULL, wlen, NULL, NULL));
    px_put(d);
    return r == SOCKET_ERROR ? -1 : (ssize_t) sent;
}

ssize_t px_recvmsg(int fd, struct px_msghdr *msg, int flags) {
    struct px_desc *d = sock_get(fd);
    if (d == NULL)
        return -1;
    bool dontwait;
    DWORD wflags = msg_flags_to_win(flags, &dontwait);
    if (d->is_icmp) {
        char *tmp = malloc(65536);
        int trunc;
        px_socklen_t nl = msg->msg_namelen;
        int ttl = 64;
        ssize_t r = icmp_recv(d, tmp, 65536, dontwait, &trunc, msg->msg_name, &nl, &ttl);
        // IP_TTL control message, Linux layout {size_t len; int level, type;} + int
        struct { size_t len; int level, type; int ttl; } cm = {sizeof(size_t) + 3 * sizeof(int), 0, 2, ttl};
        if (r >= 0 && msg->msg_control != NULL && msg->msg_controllen >= sizeof(cm)) {
            memcpy(msg->msg_control, &cm, sizeof(cm));
            msg->msg_controllen = (sizeof(cm) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1);
        } else {
            msg->msg_controllen = 0;
        }
        msg->msg_flags = 0;
        if (r >= 0) {
            msg->msg_namelen = nl;
            size_t off = 0;
            for (size_t i = 0; i < msg->msg_iovlen && off < (size_t) r; i++) {
                size_t c = msg->msg_iov[i].iov_len < (size_t) r - off ? msg->msg_iov[i].iov_len : (size_t) r - off;
                memcpy(msg->msg_iov[i].iov_base, tmp + off, c);
                off += c;
            }
            if (off < (size_t) r)
                msg->msg_flags |= 0x20; // MSG_TRUNC
            r = off;
        }
        free(tmp);
        px_put(d);
        return r;
    }
    WSABUF bufs[64];
    DWORD nbufs = msg->msg_iovlen < 64 ? (DWORD) msg->msg_iovlen : 64;
    for (DWORD i = 0; i < nbufs; i++) {
        bufs[i].buf = msg->msg_iov[i].iov_base;
        bufs[i].len = (ULONG) msg->msg_iov[i].iov_len;
    }
    struct sockaddr_storage wa;
    int wlen = sizeof(wa);
    DWORD got = 0;
    int r = BLOCKING(d, dontwait, POLLRDNORM, timeout_of(d->rcvtimeo_ms),
            (wflags = msg_flags_to_win(flags, &dontwait),
             WSARecvFrom(d->s, bufs, nbufs, &got, &wflags, (struct sockaddr *) &wa, &wlen, NULL, NULL)));
    msg->msg_flags = 0;
    msg->msg_controllen = 0;
    if (r == SOCKET_ERROR) {
        if (errno == EMSGSIZE) {
            msg->msg_flags |= 0x20; // MSG_TRUNC
            got = 0;
            for (DWORD i = 0; i < nbufs; i++)
                got += bufs[i].len;
        } else if (d->is_pipe && errno == EPIPE) {
            got = 0;
        } else {
            px_put(d);
            return -1;
        }
    }
    if (msg->msg_name != NULL) {
        if (d->type != SOCK_STREAM) {
            px_socklen_t l = msg->msg_namelen;
            addr_from_win(&wa, wlen, msg->msg_name, &l);
            msg->msg_namelen = l;
        } else {
            msg->msg_namelen = 0;
        }
    }
    px_put(d);
    return got;
}

int px_shutdown(int fd, int how) {
    struct px_desc *d = sock_get(fd);
    if (d == NULL)
        return -1;
    int res = shutdown(d->s, how) == SOCKET_ERROR ? px_fail_wsa() : 0;
    px_put(d);
    return res;
}

static int name_of(int fd, void *addr, px_socklen_t *len, bool peer) {
    struct px_desc *d = sock_get(fd);
    if (d == NULL)
        return -1;
    struct sockaddr_storage wa;
    int wlen = sizeof(wa);
    int r = peer ? getpeername(d->s, (struct sockaddr *) &wa, &wlen) : getsockname(d->s, (struct sockaddr *) &wa, &wlen);
    int res = 0;
    if (r == SOCKET_ERROR) {
        int err = WSAGetLastError();
        // an unbound socket: Linux returns the family and zeros
        if (!peer && err == WSAEINVAL) {
            memset(&wa, 0, sizeof(wa));
            wa.ss_family = af_to_win(d->domain);
            wlen = d->domain == 10 ? sizeof(struct sockaddr_in6) : d->domain == 1 ? 2 : sizeof(struct sockaddr_in);
        } else {
            res = px_fail(px_errno_wsa(err));
        }
    }
    if (res == 0)
        addr_from_win(&wa, wlen, addr, len);
    px_put(d);
    return res;
}

int px_getpeername(int fd, void *addr, px_socklen_t *len) {
    return name_of(fd, addr, len, true);
}

int px_getsockname(int fd, void *addr, px_socklen_t *len) {
    return name_of(fd, addr, len, false);
}

// Linux (level, name) -> Winsock; 0 when there is no equivalent.
static bool opt_to_win(int level, int name, int *wlevel, int *wname) {
    switch (level) {
        case 1: /* SOL_SOCKET */
            *wlevel = SOL_SOCKET;
            switch (name) {
                case 1: *wname = SO_DEBUG; return true;
                case 4: *wname = SO_ERROR; return true;
                case 5: *wname = SO_DONTROUTE; return true;
                case 6: *wname = SO_BROADCAST; return true;
                case 7: *wname = SO_SNDBUF; return true;
                case 8: *wname = SO_RCVBUF; return true;
                case 9: *wname = SO_KEEPALIVE; return true;
                case 10: *wname = SO_OOBINLINE; return true;
                case 13: *wname = SO_LINGER; return true;
                case 30: *wname = SO_ACCEPTCONN; return true;
            }
            return false;
        case 6: /* IPPROTO_TCP */
            *wlevel = IPPROTO_TCP;
            switch (name) {
                case 1: *wname = TCP_NODELAY; return true;
                case 4: *wname = TCP_KEEPIDLE; return true;
                case 5: *wname = TCP_KEEPINTVL; return true;
                case 6: *wname = TCP_KEEPCNT; return true;
            }
            return false;
        case 0: /* IPPROTO_IP */
            *wlevel = IPPROTO_IP;
            switch (name) {
                case 1: *wname = IP_TOS; return true;
                case 2: *wname = IP_TTL; return true;
                case 3: *wname = IP_HDRINCL; return true;
                case 32: *wname = IP_MULTICAST_IF; return true;
                case 33: *wname = IP_MULTICAST_TTL; return true;
                case 34: *wname = IP_MULTICAST_LOOP; return true;
                case 35: *wname = IP_ADD_MEMBERSHIP; return true;
                case 36: *wname = IP_DROP_MEMBERSHIP; return true;
            }
            return false;
        case 41: /* IPPROTO_IPV6 */
            *wlevel = IPPROTO_IPV6;
            switch (name) {
                case 16: *wname = IPV6_UNICAST_HOPS; return true;
                case 17: *wname = IPV6_MULTICAST_IF; return true;
                case 18: *wname = IPV6_MULTICAST_HOPS; return true;
                case 19: *wname = IPV6_MULTICAST_LOOP; return true;
                case 20: *wname = IPV6_ADD_MEMBERSHIP; return true;
                case 21: *wname = IPV6_DROP_MEMBERSHIP; return true;
                case 26: *wname = IPV6_V6ONLY; return true;
            }
            return false;
    }
    return false;
}

static int put_int(void *val, px_socklen_t *len, int v) {
    if (*len < (px_socklen_t) sizeof(int))
        return px_fail(EINVAL);
    *(int *) val = v;
    *len = sizeof(int);
    return 0;
}

int px_getsockopt(int fd, int level, int name, void *val, px_socklen_t *len) {
    struct px_desc *d = sock_get(fd);
    if (d == NULL)
        return -1;
    int res = 0;
    if (level == 1) {
        switch (name) {
            case 3: /* SO_TYPE */ res = put_int(val, len, d->type); goto out;
            case 38: /* SO_PROTOCOL */ res = put_int(val, len, d->protocol); goto out;
            case 39: /* SO_DOMAIN */ res = put_int(val, len, d->domain); goto out;
            case 2: /* SO_REUSEADDR */ res = put_int(val, len, 1); goto out;
            case 15: /* SO_REUSEPORT */ res = put_int(val, len, 0); goto out;
            case 20: case 21: { /* SO_RCVTIMEO, SO_SNDTIMEO */
                int64_t ms = name == 20 ? d->rcvtimeo_ms : d->sndtimeo_ms;
                struct px_timeval tv = {ms / 1000, (ms % 1000) * 1000};
                if (*len >= (px_socklen_t) sizeof(tv)) {
                    memcpy(val, &tv, sizeof(tv));
                    *len = sizeof(tv);
                } else {
                    res = px_fail(EINVAL);
                }
                goto out;
            }
            case 17: { /* SO_PEERCRED */
                struct { int pid; unsigned uid, gid; } cred = {(int) GetCurrentProcessId(), 0, 0};
                if (*len >= (px_socklen_t) sizeof(cred)) {
                    memcpy(val, &cred, sizeof(cred));
                    *len = sizeof(cred);
                } else {
                    res = px_fail(EINVAL);
                }
                goto out;
            }
        }
    }
    int wlevel, wname;
    if (!opt_to_win(level, name, &wlevel, &wname)) {
        res = px_fail(ENOPROTOOPT);
        goto out;
    }
    int wlen = *len;
    if (getsockopt(d->s, wlevel, wname, val, &wlen) == SOCKET_ERROR) {
        res = px_fail_wsa();
        goto out;
    }
    *len = wlen;
    if (level == 1 && name == 4 /* SO_ERROR */ && *(int *) val != 0)
        *(int *) val = px_errno_wsa(*(int *) val);
    // Winsock BOOL options come back as one byte
    if (wlen == 1 && *len >= (px_socklen_t) sizeof(int)) {
        *(int *) val = *(unsigned char *) val;
        *len = sizeof(int);
    }
out:
    px_put(d);
    return res;
}

int px_setsockopt(int fd, int level, int name, const void *val, px_socklen_t len) {
    struct px_desc *d = sock_get(fd);
    if (d == NULL)
        return -1;
    int res = 0;
    if (d->is_icmp && level == 0 && name == 23 /* IP_STRIPHDR */) {
        d->icmp_striphdr = len >= (px_socklen_t) sizeof(int) && *(const int *) val;
        goto out;
    }
    if (level == 1 && (name == 20 || name == 21)) { /* SO_RCVTIMEO, SO_SNDTIMEO */
        if (len < (px_socklen_t) sizeof(struct px_timeval)) {
            res = px_fail(EINVAL);
        } else {
            const struct px_timeval *tv = val;
            int64_t ms = tv->tv_sec * 1000 + tv->tv_usec / 1000;
            if (ms == 0 && tv->tv_usec != 0)
                ms = 1;
            if (name == 20)
                d->rcvtimeo_ms = ms;
            else
                d->sndtimeo_ms = ms;
        }
        goto out;
    }
    int wlevel, wname;
    // Options with no Winsock meaning (SO_REUSEADDR's Linux meaning is
    // Winsock's default; SO_REUSEPORT, IP_MTU_DISCOVER, TCP_CORK...)
    // succeed without effect, like many Linux options do on other stacks.
    if (!opt_to_win(level, name, &wlevel, &wname))
        goto out;
    if (setsockopt(d->s, wlevel, wname, val, len) == SOCKET_ERROR)
        res = px_fail_wsa();
out:
    px_put(d);
    return res;
}

const char *px_inet_ntop(int af, const void *src, char *dst, px_socklen_t size) {
    px_init();
    return inet_ntop(af_to_win(af), src, dst, size);
}

int px_inet_pton(int af, const char *src, void *dst) {
    px_init();
    return inet_pton(af_to_win(af), src, dst);
}

// ---- poll

int px_poll(struct px_pollfd *fds, unsigned long nfds, int timeout) {
    px_init();
    WSAPOLLFD *wfds = calloc(nfds + 1, sizeof(*wfds));
    int *map = calloc(nfds + 1, sizeof(int));
    struct px_desc **descs = calloc(nfds + 1, sizeof(*descs));
    ULONG n = 0;
    int ready = 0;
    for (unsigned long i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        if (fds[i].fd < 0)
            continue;
        struct px_desc *d = px_get(fds[i].fd);
        if (d == NULL) {
            fds[i].revents = PX_POLLNVAL;
            ready++;
            continue;
        }
        descs[i] = d;
        short ev = fds[i].events;
        switch (d->kind) {
            case PX_SOCK: {
                short we = 0;
                if (ev & PX_POLLIN) we |= POLLRDNORM;
                if (ev & PX_POLLPRI) we |= POLLRDBAND;
                if (ev & PX_POLLOUT) we |= POLLWRNORM;
                wfds[n].fd = d->s;
                wfds[n].events = we;
                map[n] = (int) i;
                n++;
                break;
            }
            case PX_CONSOLE_OUT:
                fds[i].revents = ev & PX_POLLOUT;
                break;
            default:
                fds[i].revents = ev & (PX_POLLIN | PX_POLLOUT);
                break;
        }
        if (fds[i].revents)
            ready++;
    }
    int res;
    if (n == 0 && ready == 0) {
        res = px_wsapoll(NULL, 0, timeout);
    } else {
        res = n == 0 ? 0 : px_wsapoll(wfds, n, ready ? 0 : timeout);
    }
    if (res >= 0) {
        for (ULONG j = 0; j < n; j++) {
            short re = wfds[j].revents, out = 0;
            if (re & (POLLRDNORM | POLLRDBAND)) out |= PX_POLLIN;
            if (re & POLLRDBAND) out |= PX_POLLPRI;
            if (re & POLLWRNORM) out |= PX_POLLOUT;
            if (re & POLLERR) out |= PX_POLLERR;
            if (re & POLLHUP) out |= PX_POLLHUP | (fds[map[j]].events & PX_POLLIN);
            if (re & POLLNVAL) out |= PX_POLLNVAL;
            fds[map[j]].revents = out;
            if (out)
                ready++;
        }
        res = ready;
    }
    for (unsigned long i = 0; i < nfds; i++)
        px_put(descs[i]);
    free(wfds);
    free(map);
    free(descs);
    return res;
}
