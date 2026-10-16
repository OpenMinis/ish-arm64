#!/bin/bash
# Build and run the fs/wait syscall regression tests inside iSH.
#
# These pin the behaviour of five fixes so a regression fails here rather than
# in a user's shell:
#   #30  O_DIRECTORY flag value + O_NOFOLLOW honored at path resolution
#   #31  fakefs stat/fstat st_dev (nlink, blksize) agreement
#   #29  waitpid not reporting its internal 1s timeout as EINTR
#        waitid si_status/si_code decoding
#        waitid si_uid
#
# Usage:
#   tests/regress/run.sh [-i <ish binary>] [-r <rootfs>] [-f <fakefs path>]
#
#   -i  iSH binary to test. Default: the first of build-arm64-release/ish,
#       build/ish that exists.
#   -r  rootfs to run against, passed to iSH as -r. Default alpine-arm64-321.
#   -f  a file under a fakefs mount point (e.g. /var/minis/shared/doc.txt) to
#       additionally check stat/fstat agreement on. Only meaningful when the
#       rootfs is mounted with -f and a path-translate hook is installed, so it
#       is optional; without it the #31 assertions still run against the rootfs.
#
# The guest test binary is built with a cross-compiler (aarch64-linux-musl-gcc
# by default, override with $CC_GUEST). If none is available the script skips
# rather than failing, since the toolchain is not part of the repo.

set -u
cd "$(dirname "$0")/../.."

ISH=""
ROOTFS="alpine-arm64-321"
FAKEFS_PATH=""
MOUNT_FLAG="-r"

while getopts "i:r:f:m:h" opt; do
    case $opt in
        i) ISH="$OPTARG" ;;
        r) ROOTFS="$OPTARG" ;;
        f) FAKEFS_PATH="$OPTARG" ;;
        m) MOUNT_FLAG="$OPTARG" ;;
        h) sed -n '2,26p' "$0"; exit 0 ;;
        *) exit 2 ;;
    esac
done

if [ -z "$ISH" ]; then
    for cand in build-arm64-release/ish build/ish; do
        if [ -x "$cand" ]; then ISH="$cand"; break; fi
    done
fi
if [ -z "$ISH" ] || [ ! -x "$ISH" ]; then
    echo "no iSH binary found; build one or pass -i <path>" >&2
    exit 2
fi
if [ ! -d "$ROOTFS" ]; then
    echo "rootfs $ROOTFS not found; pass -r <path>" >&2
    exit 2
fi

CC_GUEST="${CC_GUEST:-aarch64-linux-musl-gcc}"
if ! command -v "$CC_GUEST" >/dev/null 2>&1; then
    echo "SKIP: no guest cross-compiler ($CC_GUEST) available." >&2
    echo "      Install one or set CC_GUEST to build tests/regress/regress_syscall.c." >&2
    exit 0
fi

SRC=tests/regress/regress_syscall.c
# With -f the guest's / lives under <rootfs>/data; with -r it is <rootfs> itself.
GUEST_ROOT="$ROOTFS"
if [ "$MOUNT_FLAG" = "-f" ]; then
    GUEST_ROOT="$ROOTFS/data"
fi
BIN="$GUEST_ROOT/tmp/regress_syscall"
mkdir -p "$GUEST_ROOT/tmp"
echo "building $SRC with $CC_GUEST"
if ! "$CC_GUEST" -static -O0 -Wall -o "$BIN" "$SRC"; then
    echo "failed to build $SRC" >&2
    exit 1
fi
cp tests/regress/regress_cp.sh "$GUEST_ROOT/tmp/regress_cp.sh"

# A fakefs keeps its own metadata in meta.db, so files dropped straight into
# data/ are invisible to the guest. Register them by copying through the guest
# itself; the shell script is small enough for this to be cheap, but the test
# binary is not, so -f mode runs only the cp cases and expects the syscall
# binary to be run separately under -r (where every one of these assertions is
# equally valid -- none of them depend on the mount type).
if [ "$MOUNT_FLAG" = "-f" ]; then
    rm -f "$BIN"
    b64_sh=$(base64 < tests/regress/regress_cp.sh | tr -d '\n')
    rm -f "$GUEST_ROOT/tmp/regress_cp.sh"
    if ! "$ISH" "$MOUNT_FLAG" "$ROOTFS" /bin/sh -c \
        "printf %s '$b64_sh' | base64 -d > /tmp/regress_cp.sh"; then
        echo "failed to stage regress_cp.sh into the fakefs" >&2
        exit 1
    fi
fi

status=0

if [ "$MOUNT_FLAG" = "-f" ]; then
    echo
    echo "########## syscall regressions ##########"
    echo "  SKIP  staging a static binary into a fakefs is impractical;"
    echo "        run these with -r <rootfs> (assertions are mount-independent)."
else
    echo
    echo "########## syscall regressions ($ISH $MOUNT_FLAG $ROOTFS) ##########"
    "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_syscall ${FAKEFS_PATH:+"$FAKEFS_PATH"} || status=1

    echo
    echo "########## FMOV immediate decode ##########"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_fmov_imm" \
            tests/regress/regress_fmov_imm.c tests/regress/regress_fmov_imm.S; then
        "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_fmov_imm || status=1
        rm -f "$GUEST_ROOT/tmp/regress_fmov_imm"
    else
        echo "failed to build regress_fmov_imm" >&2
        status=1
    fi

    # AF_NETLINK stub: no read blocks without bound; links and addresses are
    # the host's; everything else is refused at once. The second run pins
    # the ISH_NETLINK_ROUTE=empty switch (read from the host environment).
    echo
    echo "########## netlink stub ##########"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_netlink_stub" \
            tests/regress/regress_netlink_stub.c; then
        out=$("$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_netlink_stub 2>/dev/null)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$|^SKIP' || status=1
        out=$(ISH_NETLINK_ROUTE=empty "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_netlink_stub route-empty "(d)" 2>/dev/null)
        echo "$out"
        echo "$out" | grep -Eq '^[0-9]+ passed, 0 failed$|^SKIP' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_netlink_stub"
    else
        echo "failed to build regress_netlink_stub" >&2
        status=1
    fi

    # A process exiting while its other threads wait in epoll_wait must leave
    # the kernel able to fork. On a fast idle host the safety valve rarely
    # leaks anything, so this passing here is necessary, not sufficient; the
    # leak path itself is exercised on a loaded device (see README).
    # A SIGSEGV handled by the guest at the same pc must keep being delivered
    # (HotSpot null checks): 258+ used to kill the process.
    echo
    echo "########## SIGSEGV handled at the same site ##########"
    if "$CC_GUEST" -static -O1 -Wall -pthread -o "$GUEST_ROOT/tmp/regress_segv_handler_loop" \
            tests/regress/regress_segv_handler_loop.c; then
        for mode in "" "onstack" "x thread"; do
            out=$("$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_segv_handler_loop 10000 $mode 2>/dev/null | tail -1)
            echo "[$mode] $out"
            echo "$out" | grep -q ' OK$' || status=1
        done
        rm -f "$GUEST_ROOT/tmp/regress_segv_handler_loop"
    else
        echo "failed to build regress_segv_handler_loop" >&2
        status=1
    fi

    echo
    echo "########## leaked thread exit ##########"
    if "$CC_GUEST" -static -O0 -Wall -pthread -o "$GUEST_ROOT/tmp/regress_leaked_thread_exit" \
            tests/regress/regress_leaked_thread_exit.c; then
        out=$(timeout 300 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_leaked_thread_exit 10 8 16 2>/dev/null | tail -1)
        echo "$out"
        echo "$out" | grep -q '^PASS' || status=1
        rm -f "$GUEST_ROOT/tmp/regress_leaked_thread_exit"
    else
        echo "failed to build regress_leaked_thread_exit" >&2
        status=1
    fi

fi

# Signals against blocking calls: the output must equal the expected file,
# which is what a real arm64 Alpine prints. Staged through the guest so it
# works on a fakefs as well; the ^C-on-a-pty cases (5*) need a /dev/ptmx,
# which only a fakefs (-m -f) has, so -r leaves them out. 9f runs again
# in-process while the host bombards iSH with SIGUSR1 (iSH's guest-signal
# wake-up), so its blocking read takes host EINTRs with no guest signal
# behind them and must retry without the caller noticing.
#
# Both guest-signal tests run twice: as is, and with SIGUSR1 blocked in iSH's
# initial mask (host_block_sigusr1), which is how an app that starts guest
# processes from a GCD worker runs them. A guest thread must be woken either
# way, or kill -9 cannot end a thread blocked in a host read or write.
host_mask=$(mktemp -t host_block_sigusr1)
if ! cc -O1 -o "$host_mask" tests/regress/host_block_sigusr1.c; then
    echo "failed to build host_block_sigusr1" >&2
    status=1
fi
echo
echo "########## signals vs blocking calls ##########"
sig_bin=$(mktemp -t regress_signal_eintr)
if "$CC_GUEST" -static -O1 -Wall -pthread -o "$sig_bin" tests/regress/regress_signal_eintr.c &&
        "$ISH" "$MOUNT_FLAG" "$ROOTFS" /bin/dd of=/tmp/regress_signal_eintr status=none < "$sig_bin" &&
        "$ISH" "$MOUNT_FLAG" "$ROOTFS" /bin/chmod 755 /tmp/regress_signal_eintr; then
    expected=$(cat tests/regress/regress_signal_eintr.expected)
    if [ "$MOUNT_FLAG" != "-f" ]; then
        echo "  SKIP  5a-5d (^C on a pty): no /dev/ptmx without a fakefs (-m -f)"
        expected=$(echo "$expected" | grep -v '^5')
    fi
    for mask in "" "$host_mask"; do
        out=$(timeout 900 $mask "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_signal_eintr 2>/dev/null)
        [ "$MOUNT_FLAG" != "-f" ] && out=$(echo "$out" | grep -v '^5')
        label=${mask:+" (SIGUSR1 blocked in iSH)"}
        if [ "$out" = "$expected" ]; then
            echo "PASS: every case matches arm64 Linux$label"
        else
            echo "FAIL: differs from arm64 Linux$label:"
            diff <(echo "$expected") <(echo "$out")
            status=1
        fi
    done
    sig_out=$(mktemp -t regress_9f)
    "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_signal_eintr -n 9f > "$sig_out" 2>/dev/null &
    ish_pid=$!
    # only once the case runs: before iSH installs its handler SIGUSR1 kills it
    while kill -0 $ish_pid 2>/dev/null && ! grep -q 9f "$sig_out"; do sleep 0.05; done
    while kill -0 $ish_pid 2>/dev/null; do kill -USR1 $ish_pid 2>/dev/null; sleep 0.005; done
    wait $ish_pid
    if grep -q 'read=4 ok data, took >=1900ms yes' "$sig_out"; then
        echo "PASS: 9f under host SIGUSR1"
    else
        echo "FAIL: 9f under host SIGUSR1: $(cat "$sig_out")"
        status=1
    fi
    rm -f "$sig_out"
    "$ISH" "$MOUNT_FLAG" "$ROOTFS" /bin/rm -f /tmp/regress_signal_eintr
else
    echo "failed to build or stage regress_signal_eintr" >&2
    status=1
fi
rm -f "$sig_bin"

# Threads blocked in a host write, read, accept or flock must not survive
# kill -9 or a sibling's _exit, with or without SIGUSR1 in iSH's mask.
echo
echo "########## kill threads blocked in host calls ##########"
kill_bin=$(mktemp -t regress_kill_blocked)
if "$CC_GUEST" -static -O1 -Wall -pthread -o "$kill_bin" tests/regress/regress_kill_blocked.c &&
        "$ISH" "$MOUNT_FLAG" "$ROOTFS" /bin/dd of=/tmp/regress_kill_blocked status=none < "$kill_bin" &&
        "$ISH" "$MOUNT_FLAG" "$ROOTFS" /bin/chmod 755 /tmp/regress_kill_blocked; then
    expected=$(cat tests/regress/regress_kill_blocked.expected)
    for mask in "" "$host_mask"; do
        out=$(timeout 300 $mask "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_kill_blocked 2>/dev/null)
        label=${mask:+" (SIGUSR1 blocked in iSH)"}
        if [ "$out" = "$expected" ]; then
            echo "PASS: every case matches arm64 Linux$label"
        else
            echo "FAIL: differs from arm64 Linux$label:"
            diff <(echo "$expected") <(echo "$out")
            status=1
        fi
    done
    "$ISH" "$MOUNT_FLAG" "$ROOTFS" /bin/rm -f /tmp/regress_kill_blocked
else
    echo "failed to build or stage regress_kill_blocked" >&2
    status=1
fi
rm -f "$kill_bin" "$host_mask"

# mmap/mprotect/munmap must get the address-space lock while other threads
# spin in guest code.
echo
echo "########## address-space writer vs spinning threads ##########"
starve_bin=$(mktemp -t regress_writer_starve)
if "$CC_GUEST" -static -O1 -Wall -pthread -o "$starve_bin" tests/regress/regress_writer_starve.c &&
        "$ISH" "$MOUNT_FLAG" "$ROOTFS" /bin/dd of=/tmp/regress_writer_starve status=none < "$starve_bin" &&
        "$ISH" "$MOUNT_FLAG" "$ROOTFS" /bin/chmod 755 /tmp/regress_writer_starve; then
    timeout 300 "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_writer_starve || status=1
    "$ISH" "$MOUNT_FLAG" "$ROOTFS" /bin/rm -f /tmp/regress_writer_starve
else
    echo "failed to build or stage regress_writer_starve" >&2
    status=1
fi
rm -f "$starve_bin"

# When init exits with a child still running, iSH must stop the child and
# exit 0 at once (it used to wait 2 s and die of its own SIGTERM, status 143).
echo
echo "########## init exits with a child still running ##########"
halt_start=$(date +%s)
"$ISH" "$MOUNT_FLAG" "$ROOTFS" /bin/sh -c 'sleep 30 & sleep 0.1; exit 0' < /dev/null
halt_rc=$?
halt_took=$(( $(date +%s) - halt_start ))
if [ "$halt_rc" = 0 ] && [ "$halt_took" -lt 2 ]; then
    echo "PASS: exit status 0 in under 2 s"
else
    echo "FAIL: exit status $halt_rc after ${halt_took}s"
    status=1
fi

echo
echo "########## cp regressions ##########"
"$ISH" "$MOUNT_FLAG" "$ROOTFS" /bin/sh /tmp/regress_cp.sh ${FAKEFS_PATH:+"$FAKEFS_PATH"} || status=1

rm -f "$BIN" "$GUEST_ROOT/tmp/regress_cp.sh"

echo
if [ "$status" = "0" ]; then
    echo "regress: all cases passed"
else
    echo "regress: FAILURES (see above)"
fi
exit $status
