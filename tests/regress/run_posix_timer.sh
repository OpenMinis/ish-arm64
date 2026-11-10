#!/bin/sh
# [FIX-261031-04] POSIX timer bounds and signal-number checks.
#
#   tests/regress/run_posix_timer.sh [-i <ish binary>] [-r <rootfs>]
#
# Also builds tests/regress/host_timer_diag.c against the libish next to the
# iSH binary (its meson build dir) and runs it on the host: send_signal given
# a signal number iSH does not have, and the timer probes' records as the
# crash writer prints them - neither reachable from a guest.
#
# Every case runs in its own iSH under a host timeout: before the checks, an
# out-of-range timer id crashes iSH and the SIGEV_THREAD_ID case leaves
# pids_lock held (the next fork hangs), and neither may take the other cases
# down with it. The test binary is static, so with no -r a throwaway rootfs
# holding only /tmp is enough.
set -u
cd "$(dirname "$0")/../.."

ISH=""
ROOTFS=""
while getopts "i:r:" opt; do
    case "$opt" in
        i) ISH="$OPTARG" ;;
        r) ROOTFS="$OPTARG" ;;
        *) echo "usage: $0 [-i ish] [-r rootfs]" >&2; exit 2 ;;
    esac
done
if [ -z "$ISH" ]; then
    for cand in build-native/ish build-arm64-release/ish build/ish; do
        if [ -x "$cand" ]; then ISH="$cand"; break; fi
    done
fi
if [ -z "$ISH" ] || [ ! -x "$ISH" ]; then
    echo "no iSH binary found; pass -i <path>" >&2
    exit 1
fi

CC_GUEST="${CC_GUEST:-aarch64-linux-musl-gcc}"
if ! command -v "$CC_GUEST" >/dev/null 2>&1; then
    echo "SKIP: no guest cross-compiler ($CC_GUEST) available." >&2
    exit 0
fi
TIMEOUT=""
for t in timeout gtimeout; do
    if command -v "$t" >/dev/null 2>&1; then TIMEOUT="$t"; break; fi
done
if [ -z "$TIMEOUT" ]; then
    echo "SKIP: no timeout(1) on the host (coreutils)." >&2
    exit 0
fi

TMPROOT=""
if [ -z "$ROOTFS" ]; then
    TMPROOT=$(mktemp -d)
    ROOTFS="$TMPROOT"
fi
mkdir -p "$ROOTFS/tmp"
BIN="$ROOTFS/tmp/regress_posix_timer_bounds"
if ! "$CC_GUEST" -static -O0 -Wall -o "$BIN" tests/regress/regress_posix_timer_bounds.c; then
    echo "failed to build regress_posix_timer_bounds.c" >&2
    exit 1
fi

status=0

BUILD_DIR=$(dirname "$ISH")
if [ -f "$BUILD_DIR/libish.a" ]; then
    echo
    echo "########## posix timer: host checks ##########"
    HOST_BIN=$(mktemp)
    if cc -std=gnu11 -DLOG_HANDLER_DPRINTF=1 -DENGINE_ASBESTOS=1 -DGUEST_ARM64=1 \
            -I"$BUILD_DIR" -I. -Ivdso/arm64 -o "$HOST_BIN" tests/regress/host_timer_diag.c \
            "$BUILD_DIR/libish.a" "$BUILD_DIR/libish_emu.a" "$BUILD_DIR/libfakefs.a" \
            -lm -ldl -lsqlite3 2>/dev/null; then
        "$TIMEOUT" 20 "$HOST_BIN" || status=1
    else
        echo "  FAIL  host_timer_diag.c did not build against $BUILD_DIR"
        status=1
    fi
    rm -f "$HOST_BIN"
else
    echo "  SKIP  host checks: no libish.a next to $ISH"
fi

for c in signo ids normal thread_id diag leak stale; do
    echo
    echo "########## posix timer: $c ##########"
    out=$("$TIMEOUT" 20 "$ISH" -r "$ROOTFS" /tmp/regress_posix_timer_bounds "$c")
    rc=$?
    printf '%s\n' "$out"
    # iSH does not pass the guest's exit status on, so a failed check would
    # otherwise read as a pass: go by what the case printed.
    case "$out" in *"  FAIL  "*) status=1 ;; esac
    case "$rc" in
        0) ;;
        124) echo "  FAIL  $c: hung (no exit within 20 s)"; status=1 ;;
        *) [ "$rc" -gt 128 ] && echo "  FAIL  $c: iSH died with signal $((rc - 128))"
           status=1 ;;
    esac
done

rm -f "$BIN"
[ -n "$TMPROOT" ] && rm -rf "$TMPROOT"
echo
[ "$status" -eq 0 ] && echo "posix timer: all cases passed" || echo "posix timer: FAILED"
exit "$status"
