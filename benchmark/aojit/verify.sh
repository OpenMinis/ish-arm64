#!/bin/bash
# Three-way check of an AOT build: guest/verify/check.sh must print the same with AOT on, AOT off
# (echo on|off > /proc/ish/jit, same ish) and ISH_JIT=0 (no images at all). Prints the images'
# state and per-module hits of the AOT-on run; exits 1 on any difference.
#
# usage: verify.sh <ish> -r|-f <rootfs>     (the suite installed there: install.sh)
#   AOJIT_SSH=host:port   also check ssh/scp against the sshd of guest/ssh/server.sh (another ish)
set -uo pipefail
[ $# -eq 3 ] || { sed -n '2,8p' "$0"; exit 2; }
ish=$1 fs=$2 rootfs=$3
run() { "$ish" "$fs" "$rootfs" "$@" < /dev/null 2> /dev/null; }
guest() { run /usr/bin/env AOJIT_SSH="${AOJIT_SSH:-}" /bin/sh -c "$1"; }
out=$(mktemp -d -t aojit_verify)
trap 'rm -rf "$out"' EXIT

echo "🔬 AOT on and off (one ish) …"
guest 'sh /tmp/aojit/verify/threeway.sh'
echo "🔬 ISH_JIT=0 …"
ISH_JIT=0 guest 'sh /tmp/aojit/verify/check.sh > /tmp/aojit/verify/res/out_nojit.txt 2>&1'
for f in out_on out_off out_nojit jit_on; do run /bin/cat /tmp/aojit/verify/res/$f.txt > "$out/$f.txt"; done

rc=0
[ -s "$out/out_on.txt" ] || { echo "❌ no output from check.sh (suite installed? install.sh)"; exit 1; }
for m in off nojit; do
    if diff -q "$out/out_on.txt" "$out/out_$m.txt" > /dev/null; then echo "✅ output identical: on vs $m ($(wc -l < "$out/out_on.txt" | tr -d ' ') lines)"
    else echo "❌ output differs: on vs $m"; diff "$out/out_on.txt" "$out/out_$m.txt" | head -20; rc=1; fi
done
grep -E "^images:" "$out/jit_on.txt"
grep -q " 0 rejected" "$out/jit_on.txt" || { echo "❌ images rejected (abi mismatch?)"; rc=1; }
sed 's#/usr/lib/python3.12/lib-dynload/##; s#.cpython-312-aarch64-linux-musl.so##' "$out/jit_on.txt" | awk '/modules with an image/ {on = 1; next} on && $2 ~ /^[0-9]+$/ && $2 > 0 {printf "   %-36s %7d / %-7d %5.1f%%  moved %d\n", $1, $4, $2, 100 * $4 / $2, $6}'
grep -E "^(== ssh|skipped)" -A0 "$out/out_on.txt" | grep -q skipped && echo "ℹ️  ssh part skipped (set AOJIT_SSH)"
exit $rc
