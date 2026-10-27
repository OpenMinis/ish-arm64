#!/bin/bash
# The CLI's initial exec with long command lines (ish-arm64#42).
#
#   up to ARGV_MAX   the guest gets the whole command line; a 16 KB one used to
#                    overrun a fixed 4 KB stack buffer in xX_main_Xx.h and
#                    SIGSEGV the host
#   past ARGV_MAX    fails with E2BIG, like the kernel's execve, and exits
#   no hang          every run must end on its own: the crash report used to
#                    spin in dladdr on the smashed frame and never exit
#
# ARGV_MAX is 32 pages = 131072 bytes, counting every string's NUL and the
# final one. `/bin/sh -c "echo <n x a> | wc -c"` takes n + 26 of them.
#
# Usage: tests/regress/regress_long_cmdline.sh [-i <ish>] -t <rootfs tarball>
#   The tarball (e.g. an alpine minirootfs) is fakefsified into a temp dir,
#   so no existing rootfs is touched.
set -u
cd "$(dirname "$0")/../.."
ISH=build-arm64-release/ish
TARBALL=""
while getopts "i:t:" opt; do
    case $opt in
        i) ISH="$OPTARG" ;;
        t) TARBALL="$OPTARG" ;;
        *) exit 2 ;;
    esac
done
FAKEFSIFY="$(dirname "$ISH")/tools/fakefsify"
if [ -z "$TARBALL" ] || [ ! -f "$TARBALL" ] || [ ! -x "$FAKEFSIFY" ]; then
    echo "SKIP: need -t <rootfs tarball> and $FAKEFSIFY" >&2
    exit 0
fi
W=$(mktemp -d -t regress_long_cmdline)
trap 'rm -rf "$W"' EXIT
R=$W/rootfs
"$FAKEFSIFY" "$TARBALL" "$R" || exit 1
pass=0; fail=0
check() { if [ "$1" = "$2" ]; then pass=$((pass+1)); echo "PASS $3"; else fail=$((fail+1)); echo "FAIL $3 (got '$1', want '$2')"; fi; }

# One run, killed after 30 s so a hang reads as a failure instead of stalling
# the suite. Prints "<exit status>:<stdout>".
run() {
    local a out rc
    a=$(head -c "$1" /dev/zero | tr '\0' a)
    out=$(perl -e 'alarm shift; exec @ARGV' 30 "$ISH" -f "$R" /bin/sh -c "echo $a | wc -c" </dev/null 2>/dev/null)
    rc=$?
    echo "$rc:$(echo $out)"
}

for n in 4000 8192 16384 65536 131046; do
    check "$(run $n)" "0:$((n + 1))" "a $((n + 26))-byte command line reaches the guest whole"
done
# 249 = (-E2BIG) & 0xff: main returns xX_main_Xx's error.
check "$(run 131047)" "249:" "one byte past ARGV_MAX is E2BIG"
check "$(run 200000)" "249:" "far past ARGV_MAX is E2BIG"

echo "long cmdline: $pass passed, $fail failed"
[ "$fail" = 0 ]
