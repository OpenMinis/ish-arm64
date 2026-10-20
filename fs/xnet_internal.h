#ifndef FS_XNET_INTERNAL_H
#define FS_XNET_INTERNAL_H
// The half of fs/xnet.c that fs/sock.c calls. See fs/xnet.h for what it does.
#include <stdatomic.h>
#include <stdbool.h>
#include <sys/socket.h>

struct fd;

// The master switch, read once per connect(). Nothing else of xnet runs for
// a socket while it is false and the socket was never forwarded.
extern atomic_bool xnet_on;

// Returned by xnet_connect when connect() should go on as if xnet did not
// exist. Any other value is connect()'s result.
#define XNET_PASS 1

// connect() of an AF_INET/AF_INET6 stream socket with no xnet state yet.
// addr is the host form sockaddr_read() produced.
int xnet_connect(struct fd *sock, const struct sockaddr *addr, socklen_t len);
// connect() again on a socket xnet took (sock->socket.xnet != NULL):
// EALREADY, EISCONN, the error not reported yet, or XNET_PASS after putting
// a fresh socket back for a new attempt, as Linux allows after a failure.
int xnet_reconnect(struct fd *sock);
// Before reading or writing a socket xnet took: 0 to go on, or EAGAIN (not
// connected yet and the call would not block), EINTR, or the connection's
// error, reported once. A blocking call waits for the entry's answer.
int xnet_io_gate(struct fd *sock, bool dontwait);
// SO_ERROR: true and *err set (0 or a negative errno) if xnet answers it.
bool xnet_so_error(struct fd *sock, int *err);
// getpeername/getsockname of a socket xnet took. Return 0 with the address
// in *out (host form), a negative errno, or XNET_PASS to ask the host.
int xnet_getpeername(struct fd *sock, struct sockaddr_storage *out, socklen_t *len);
int xnet_getsockname(struct fd *sock, struct sockaddr_storage *out, socklen_t *len);
// The socket's last reference is going away.
void xnet_release(struct fd *sock);

#endif
