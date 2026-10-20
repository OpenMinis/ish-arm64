#ifndef FS_XNET_H
#define FS_XNET_H
// Transparent forwarding of guest TCP connections to the app's local SOCKS5
// entry (MinisApp's nethub), which sends them on through Tailscale, an
// upstream proxy or directly, by its own routing decision.
//
// A guest connect() whose destination matches the route snapshot below is
// not connected to the destination. iSH connects a hidden host socket to
// 127.0.0.1:<entry port>, authenticates, asks for CONNECT <destination>, and
// once the entry answers, puts that socket in place of the guest's (same
// host fd number, so every poll/epoll/select watching it keeps working).
// The guest sees an ordinary TCP connection to its destination:
// getpeername() is the destination, failures are the errno Linux would give
// (ECONNREFUSED, EHOSTUNREACH, ENETUNREACH, ETIMEDOUT, EACCES), blocking and
// non-blocking connect, EINTR, EALREADY and EISCONN behave as on Linux.
//
// Everything else is left alone, byte for byte as before: the switch is off
// by default, and while it is off, unconfigured, or the snapshot has no hook
// prefix, connect() costs one atomic load. The guest never sees the entry's
// port or password: they live only in kernel memory, are never logged, and
// nothing is put in the guest's environment or /proc.
//
// UDP is not forwarded yet (stage 2), nor is inbound traffic or ICMP.
//
// Threading: every function here may be called from any host thread, at any
// time, including while guest connections are being made. Strings are copied
// before the call returns; the caller keeps ownership of what it passes.
// Changes apply to connect() calls that start afterwards; connections
// already made or in progress are not touched.
//
// Logs: one line per decision and per outcome, prefixed "[Net][Xparent]",
// carrying a connection id "x<hex>" that is also sent to the entry (see
// ish_xnet_configure), never the password.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The SOCKS5 entry: 127.0.0.1:<entry_port>, RFC 1929 username/password.
//
// username is the routing tag the entry decides by (e.g. "shell"). If
// append_conn_id is true, each connection authenticates as
// "<username>~<id>" (e.g. "shell~x1f"), so the entry can print the same id
// as iSH's log; the entry must then strip "~..." before using the tag.
// password is the entry's per-process secret. Both are at most 255 bytes.
//
// entry_port 0 (or a NULL password) unconfigures: nothing is hooked until
// configured again. Returns 0, or -EINVAL for an over-long or NULL field.
int ish_xnet_configure(uint16_t entry_port, const char *username, const char *password,
        bool append_conn_id);

// The route snapshot: which destinations go through the entry.
//
// hook:   prefixes to forward, "a.b.c.d/n", "x::y/n" or a bare address (a
//         host route). For example the tailnet's prefixes and peer addresses,
//         subnet routes accepted from peers, or "0.0.0.0/0" and "::/0" when an
//         exit node is in use. Nothing is built in: an empty list hooks
//         nothing.
// exempt: prefixes never forwarded even when a hook prefix covers them; it
//         wins over hook. Must hold the local networks the device is on
//         right now (e.g. the Wi-Fi subnet), which are reachable directly,
//         and the bypass list of an upstream proxy route (addresses only:
//         host-name bypass rules cannot be applied here, the hook sees only
//         the address the guest connects to).
//         Loopback, unspecified, multicast and link-local destinations are
//         never forwarded whatever the lists say.
// self_addrs: this node's tailnet addresses (one IPv4, one IPv6, either may
//         be missing). getsockname() on a forwarded connection reports the
//         one of the destination's family, so the local end looks like the
//         tailnet interface; without one it reports the unspecified address.
// dns_intercept: reserved for stage 2 (UDP); ignored for now.
// fail_closed: what to do when the entry cannot be reached. false (the
//         default, and what the task asks for): the connection goes out
//         directly, as if not hooked, and the entry is retried after 10 s.
//         true: the connect() fails with ENETUNREACH instead. This is the
//         privacy setting for exit-node mode, where going out directly
//         exposes the device's own address to the destination; it also means
//         a dead entry cuts every hooked connection, so leave it off unless
//         the user asked for it.
//
// generation is any number the caller chooses (it is logged and reported in
// the stats, so a stale snapshot can be recognised). A NULL list with count
// 0 is an empty list. Returns 0, or -EINVAL if any entry does not parse (the
// previous snapshot is then kept, and the bad entry is logged).
int ish_xnet_set_routes(uint64_t generation,
        const char *const *hook, size_t hook_count,
        const char *const *exempt, size_t exempt_count,
        bool dns_intercept,
        const char *const *self_addrs, size_t self_count,
        bool fail_closed);

// The master switch, off at start. Off means connect() is not looked at at
// all. Turning it off does not affect connections already forwarded.
void ish_xnet_set_enabled(bool enabled);
bool ish_xnet_enabled(void);

// Counters since start (never reset), for debug.net.xparent stats and
// debug.net.diagnose. Plain numbers, no addresses, no secrets.
struct ish_xnet_stats {
    bool enabled;
    bool configured;
    bool supported;           // false on hosts where hooking is unavailable
    uint64_t generation;      // of the current snapshot
    uint32_t hook_prefixes;
    uint32_t exempt_prefixes;
    // connect() decisions
    uint64_t hooked;          // forwarded through the entry
    uint64_t pass_no_match;   // no hook prefix covers the destination
    uint64_t pass_exempt;     // an exempt prefix covers it
    uint64_t pass_local;      // loopback/unspecified/multicast/link-local
    uint64_t pass_bound;      // guest bound the socket to a device or an address
    uint64_t pass_entry_down; // entry unreachable, went out directly
    uint64_t pass_unconfigured; // switch on but no entry or no snapshot
    uint64_t refused_closed;  // entry unreachable and fail_closed: ENETUNREACH
    // outcomes of hooked connections
    uint64_t connected;
    uint64_t fail_refused;    // SOCKS 0x05 -> ECONNREFUSED
    uint64_t fail_host_unreach; // 0x04 -> EHOSTUNREACH
    uint64_t fail_net_unreach;  // 0x03 -> ENETUNREACH
    uint64_t fail_timeout;    // 0x06, or no answer in time -> ETIMEDOUT
    uint64_t fail_denied;     // 0x02 -> EACCES
    uint64_t fail_other;      // other reply codes -> ECONNREFUSED
    uint64_t fail_entry;      // entry broke off mid-handshake -> ECONNREFUSED
    uint64_t cancelled;       // guest closed the socket before the answer
    uint64_t entry_down_events; // times the entry was found unreachable
    uint32_t pending;         // waiting for the entry's answer right now
};
void ish_xnet_stats(struct ish_xnet_stats *out);

// The same, as "key value" lines (the format of /proc/ish/xnet). Returns
// the length written, truncated to size - 1.
size_t ish_xnet_describe(char *buf, size_t size);

// For the command-line build only (main.c, ISH_XNET_CONFIG=<file>): apply a
// configuration from a host file of "key=value" lines - port, user,
// password, conn_id (0/1), hook, exempt, self (comma-separated lists),
// fail_closed (0/1), generation, enabled (0/1). The app calls the functions
// above instead. Returns 0 or -errno.
int ish_xnet_load_file(const char *path);

#endif
