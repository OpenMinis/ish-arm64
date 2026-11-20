#!/bin/sh
# [FIX-261120-04] Interval timer firing while its process or thread exits.
#
#   tests/regress/run_itimer_exit.sh [-i <ish binary>] [-r <rootfs>]
#
# Runs tests/regress/regress_itimer_exit.c in iSH under a host timeout.
# The test binary is static, so with no -r a throwaway rootfs holding only
# /tmp is enough.
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
BIN="$ROOTFS/tmp/regress_itimer_exit"
if ! "$CC_GUEST" -static -O1 -Wall -pthread -o "$BIN" tests/regress/regress_itimer_exit.c; then
    echo "failed to build regress_itimer_exit.c" >&2
    exit 1
fi

status=0
echo "########## itimer vs exit ($ISH) ##########"
out=$("$TIMEOUT" 120 "$ISH" -r "$ROOTFS" /tmp/regress_itimer_exit 30 6 2>&1)
rc=$?
printf '%s\n' "$out" | tail -12
# iSH does not pass the guest's exit status on: go by what the test printed.
case "$out" in *"OK children="*) ;; *) echo "  FAIL  no OK line (crash, hang or a child without SIGALRM)"; status=1 ;; esac
case "$out" in *"HOST CRASH"*) echo "  FAIL  iSH crashed"; status=1 ;; esac
[ "$rc" -eq 124 ] && { echo "  FAIL  hung (no exit within 120 s)"; status=1; }

rm -f "$BIN"
[ -n "$TMPROOT" ] && rm -rf "$TMPROOT"
echo
[ "$status" -eq 0 ] && echo "itimer exit: passed" || echo "itimer exit: FAILED"
exit "$status"
