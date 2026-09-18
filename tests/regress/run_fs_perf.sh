#!/bin/bash
# Regression + benchmark runner for the fs hot path work (path cache,
# inode orphan cleanup, openat inode reference), against a COPY of a fakefs
# rootfs so meta.db checks are reproducible.
#
# Usage: tests/regress/run_fs_perf.sh [-i <ish>] [-r <fakefs rootfs>] [-b]
#   -b  also run the fork+exec storm benchmarks (serial 1500, parallel 20x100)
set -u
cd "$(dirname "$0")/../.."
ISH=build-native/ish; ROOTFS=alpine-arm64-fakefs; BENCH=0
while getopts "i:r:bh" o; do case $o in i) ISH=$OPTARG;; r) ROOTFS=$OPTARG;; b) BENCH=1;; *) sed -n '2,8p' "$0"; exit 0;; esac; done
[ -x "$ISH" ] || { echo "no ish binary at $ISH" >&2; exit 2; }
[ -d "$ROOTFS" ] || { echo "no rootfs at $ROOTFS" >&2; exit 2; }
T=$(mktemp -d); cp -R "$ROOTFS" "$T/rootfs"; R="$T/rootfs"; trap 'rm -rf "$T"' EXIT
orph() { sqlite3 "$R/meta.db" "select count(*) from stats where inode not in (select inode from paths)"; }
fail=0
b=$(orph); echo "orphan rows before: $b"; [ "$b" = 0 ] || { echo "rootfs copy already has orphans; pick a clean one" >&2; exit 2; }

echo "== path cache =="; "$ISH" -f "$R" /bin/sh < tests/regress/regress_path_cache.sh || fail=1
echo "== inode orphan cleanup =="; "$ISH" -f "$R" /bin/sh < tests/regress/regress_inode_orphan.sh || fail=1
o=$(orph); echo "orphan rows after: $o"; [ "$o" = 0 ] || { echo "FAIL: $o orphan stats rows leaked"; fail=1; }

CC_GUEST="${CC_GUEST:-aarch64-linux-musl-gcc}"
if command -v "$CC_GUEST" >/dev/null 2>&1; then
    echo "== open/unlink race (8 procs x 3000) =="
    "$CC_GUEST" -static -O0 -o "$T/race" tests/regress/regress_open_unlink_race.c || fail=1
    timeout 300 "$ISH" -f "$R" /bin/sh -c 'cat > /tmp/race && chmod +x /tmp/race && /tmp/race; rc=$?; rm -f /tmp/race; exit $rc' < "$T/race" || fail=1
    o=$(orph); [ "$o" = 0 ] || { echo "FAIL: $o orphan rows after the race"; fail=1; }
    echo "== sub-ms futex / nanosleep waits (Go idle pattern) =="
    "$CC_GUEST" -static -O0 -o "$T/subms" tests/regress/regress_subms_wait.c || fail=1
    timeout 120 "$ISH" -f "$R" /bin/sh -c 'cat > /tmp/subms && chmod +x /tmp/subms && /tmp/subms; rc=$?; rm -f /tmp/subms; exit $rc' < "$T/subms" 2>&1 | grep -v '^\[iSH\]\[' || fail=1
else
    echo "skip: no $CC_GUEST for the race test"
fi

echo "== /proc readers vs exiting processes (25s; crashed iSH in <1s before the fix) =="
out=$(timeout 180 "$ISH" -f "$R" /bin/sh < tests/regress/regress_proc_exit_race.sh 2>&1 | grep -v '^\[iSH\]\[')
echo "$out" | tail -1
echo "$out" | grep -q PROC_RACE_OK || { echo "FAIL: iSH died during the proc/exit race"; fail=1; }

if [ $BENCH = 1 ]; then
    t() { python3 -c 'import time;print(time.time())'; }
    s=$(t); timeout 300 "$ISH" -f "$R" /bin/sh -c 'i=0; while [ $i -lt 1500 ]; do /bin/true; i=$((i+1)); done' >/dev/null 2>&1; e=$(t)
    echo "bench serial   1500 x /bin/true: $(python3 -c "print(f'{$e-$s:.2f}s')")"
    s=$(t); timeout 300 "$ISH" -f "$R" /bin/sh -c 'j=0; while [ $j -lt 20 ]; do ( i=0; while [ $i -lt 100 ]; do /bin/true; i=$((i+1)); done ) & j=$((j+1)); done; wait' >/dev/null 2>&1; e=$(t)
    echo "bench parallel 20 x 100 /bin/true: $(python3 -c "print(f'{$e-$s:.2f}s')")"
fi
[ $fail = 0 ] && echo "ALL PASSED" || echo "FAILURES"
exit $fail
