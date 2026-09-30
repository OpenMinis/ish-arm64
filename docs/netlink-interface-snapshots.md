# Route-netlink interface snapshots

This change replaces the empty link/address replies in the opt-in
`ISH_NETLINK_STUB=1` experiment with read-only host interface snapshots. The
switch is still off by default. It does not install a VPN on the host or change
host routes.

## What this fixes

At upstream `e6521d9c`, an unmodified Go `net.Interfaces()` call fails on the
Linux CLI with `route ip+net: netlinkrib: invalid argument`: `recvfrom` exposes
the backing AF_UNIX socketpair address instead of a kernel `sockaddr_nl`.
Even paths that get past that error receive empty interface dumps. Tailscale's
network monitor then has no usable interface with which to start its control
client.

With this patch, the same guest sees interface indices, names, flags, MTUs,
hardware addresses where available, and IPv4/IPv6 addresses with prefix lengths.
Official Tailscale 1.102.4 reaches `NeedsLogin`, answers `tailscale status`, and
obtains an authentication URL without a Tailscale source change or environment
override of its interface detection.

This is **control-plane startup**, not a successful VPN connection. No account
was authorised and no tunnel traffic was tested.

## Supported behaviour and limits

- `AF_NETLINK`, `NETLINK_ROUTE`, `SOCK_RAW` and `SOCK_DGRAM` only.
- `RTM_GETLINK` and `RTM_GETADDR` multipart dump requests are answered from
  `getifaddrs()`, `if_nametoindex()` and `SIOCGIFMTU`. Address dumps accept
  `AF_UNSPEC`, `AF_INET` and `AF_INET6`.
- Host flags and address families are translated to the Linux guest ABI.
  Darwin's embedded link-local IPv6 scope bytes are cleared.
- Each socket gets a distinct port ID; explicit bind collisions are rejected.
  Reply headers use the destination port and request sequence. Sender addresses
  identify the kernel (port zero), including on `recvfrom` and `recvmsg`.
- Scatter/gather uses the guest's 32-bit or 64-bit iovec layout. `MSG_PEEK`,
  `MSG_TRUNC`, `MSG_DONTWAIT`, receive `msg_flags` and name lengths are handled.
  `read` and `write` use the same netlink path.
- The socketpair receive queue supplies poll/epoll readiness. Reply enqueue is
  always nonblocking, including for a blocking guest socket: a full queue returns
  `EAGAIN` rather than waiting for that same guest to receive its own reply.
- Requests and complete snapshots are capped at **4096 bytes**. Oversized
  requests return `EMSGSIZE`; snapshots that do not fit are replaced by a single
  `NLMSG_ERROR(-ENOBUFS)`, not a partial successful dump. Large host interface
  inventories therefore need a future incremental dump implementation.
- Unsupported message types and non-dump requests receive
  `NLMSG_ERROR(-EOPNOTSUPP)`. Malformed headers receive `EINVAL` or an error
  reply. Error replies include the original header and `NLM_F_CAPPED`.
- One request per datagram is supported; batched request messages are rejected.
- `RTM_GETROUTE` still returns an empty completed dump. Default-route reporting,
  individual-link queries, additional netlink sockopts and route/address/link
  mutation are not implemented.
- Bind group masks are recorded, but **no multicast notifications are emitted**.
  Network changes require a fresh query; Wi-Fi/cellular handover and resume
  behaviour remain unverified. This is not a complete Linux netlink stack.
- Host interfaces are reported, not an isolated guest network namespace. Existing
  `SO_BINDTODEVICE` behaviour is unchanged by this patch.

## Validation on 2026-09-30

Host: Orange Pi 6 Plus, CIX P1 ARM64 (12 CPU cores), approximately 14 GiB usable
RAM, NVMe storage, Debian Trixie, Linux host-native CLI, 4 KiB host pages. Guest:
frozen Alpine 3.24.2 ARM64. Acceleration/native AOT was not involved.

| Check | Result |
| --- | --- |
| Native Linux syscall oracle | 221 checks pass; 9 links, 9 addresses |
| Patched guest, release and debug | 211 checks each pass; same link/address counts |
| Encoder under GCC ASan + UBSan | 10,000 malformed-header/capacity cases pass; bounded overflow and family filters pass |
| Go `net.Interfaces()` / `Interface.Addrs()` | 9 interfaces; 5 up, non-loopback interfaces with addresses |
| Frozen upstream syscall test | Fails the assigned-port/address assertion |
| Frozen upstream Go probe | Fails with `netlinkrib: invalid argument` |
| `ISH_NETLINK_STUB=0` | Still returns address-family-not-supported |
| Existing syscall regressions, release and debug | 52 pass each |
| Existing FMOV immediate regression, release and debug | 256/256 double and 256/256 single; zero failures |
| Existing GNU cp cases | 3 skipped each: GNU coreutils absent from frozen guest |
| Official tailscaled 1.102.4, frozen upstream | Exits with `netmon.New: ... netlinkrib: invalid argument` |
| Official tailscaled 1.102.4, patched release | Nonempty link state, `NeedsLogin`, working local API, control-server registration returns `authURL=true` |

The authentication URL was not followed. `tailscale up --timeout=35s` therefore
ended with the expected timeout rather than reaching `Running`. One run printed
the URL in the CLI; a second exposed it in the daemon log but the CLI only printed
the timeout. Both reached the server's `authURL=true` response. The temporary
daemon shut down afterwards. No persistent state/auth key was used, and host
Tailscale settings were not changed.

The official ARM64 archive was downloaded from
`https://pkgs.tailscale.com/stable/tailscale_1.102.4_arm64.tgz`; SHA-256 matched the
publisher's `.sha256` file:

```text
9dd1e6a592a014bbaea0103167ffe299adeda4ba14e078ce9c2895364f6c4c3f
```

### Linux build boundary

Upstream's CLI at this revision includes Darwin-only Mach/ucontext memory and
crash diagnostics, and fakefs uses Darwin `stat` field names and `F_GETPATH`.
The Linux tests use generated build-directory adapters, **not changes to the
app or production CLI**. They disable Apple-only crash recovery and footprint
sampling, alias the timestamp fields and leave the fakefs `F_GETPATH` bind-mount
operation unsupported. The tested filesystem import, ordinary I/O and sockets
do not require that operation.

`tests/regress/netlink-linux-build.ts` reproduces these adapters. It also
pre-generates `cpu-offsets.h` to avoid an existing clean-build dependency race in
`main.c`. Clang is required for the upstream assembler register aliases.

```sh
bun tests/regress/netlink-linux-build.ts build-netlink-linux release
bun tests/regress/netlink-linux-build.ts build-netlink-linux-debug debug
```

These are Linux host-adapted results. **Darwin/Xcode/iOS builds, Apple network
permissions, device suspension, network handover and authenticated traffic remain
untested.** Xcode's source list includes the new encoder, but that is not proof of
an Apple build. The attempted independent automated review timed out; it is not
counted as review evidence.

## Reproduce the probes

Use a disposable guest root, not a running app filesystem. The C test requires
Linux UAPI headers and a static ARM64 Linux compiler. A native `musl-gcc` setup
may need `CPPFLAGS` pointing to a directory containing `linux`, `asm` and
`asm-generic` headers. `CC_GUEST` defaults to `aarch64-linux-musl-gcc`.

```sh
CC_GUEST=aarch64-linux-musl-gcc \
  tests/regress/run_netlink.sh "$PWD/build-netlink-linux/ish" /absolute/guest-root

# Native Linux oracle, without the iSH-specific unsupported-operation tests:
aarch64-linux-musl-gcc -static -O2 -Wall \
  tests/regress/regress_netlink.c -o /tmp/regress-netlink
/tmp/regress-netlink

# Standalone Linux encoder, no emulator runtime:
gcc -I. -std=gnu11 -O1 -g -fsanitize=address,undefined \
  tests/regress/netlink-snapshot.c fs/netlink.c -o /tmp/netlink-snapshot
/tmp/netlink-snapshot

# Build the real Go networking probe; stage it in a disposable realfs root:
CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build \
  -o /absolute/guest-root/tmp/interfaces tests/regress/netlink_interfaces.go
ISH_NETLINK_STUB=1 build-netlink-linux/ish -r /absolute/guest-root /tmp/interfaces
```

The native/guest tests expect at least one up non-loopback host interface. The
runner requires the `NETLINK_PASS` marker: this upstream CLI can return host exit
zero after a guest failure, so host exit status alone is not a pass.

### Official userspace daemon

Use a **fakefs** guest for the daemon test. The non-root realfs backend cannot
create the guest Unix-socket inode used by the local API (`bind: operation not
permitted`). Import a guest tar containing the official ARM64 `tailscale` and
`tailscaled` binaries; copying new files directly into fakefs `data/` does not
register them in its metadata database.

Start the app/CLI with `ISH_NETLINK_STUB=1`. Inside the disposable guest:

```sh
# No TUN, no fixed UDP/SOCKS port and no persistent node state.
tailscaled --tun=userspace-networking --state=mem: \
  --socket=/tmp/ish-netlink-test.sock --port=0 \
  --socks5-server=127.0.0.1:0 >/tmp/ish-netlink-test.log 2>&1 &
daemon=$!
trap 'kill "$daemon" 2>/dev/null; wait "$daemon" 2>/dev/null' EXIT INT TERM
sleep 8
tailscale --socket=/tmp/ish-netlink-test.sock status
# This requests an auth URL but does not authorise an account. A timeout is
# expected unless someone deliberately completes login in a browser.
tailscale --socket=/tmp/ish-netlink-test.sock up --accept-dns=false \
  --accept-routes=false --hostname=ish-netlink-smoke --timeout=35s
# Inspect locally; do not publish the actual authentication URL.
grep 'authURL=true' /tmp/ish-netlink-test.log
```

`status` should report `Logged out.` rather than failing to contact the daemon.
The daemon log should contain nonempty `link state` and a registration response
with `authURL=true`, not an empty-interface pause. Do not claim a successful
connection until account authorisation and traffic tests have also passed.
