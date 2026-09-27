#!/bin/bash
# Is a module worth an image? For one candidate workload (guest/measure/<name>.sh): wall time with
# the images on, off and with everything native (the runtime JIT build: the upper bound of what
# images could give), the translations per module (ISH_JIT_MAP), and where the CPU goes, per guest
# module (macOS sample + tools/jit_aot/attrib.py).
#
# usage: profile.sh <workload> <AOT ish> <JIT ish> -r|-f <rootfs>
#   AOT ish   pure-AOT build with the current images (-Djit=true -Djit_emit=false -Dcli_aot=...)
#   JIT ish   runtime JIT build (-Djit=true)
#   DUR=<s>   seconds to sample (default 12, a bit longer than the workload); OUT=<dir> keeps the files
#   AOJIT_SSH=host:port for the ssh workload
set -uo pipefail
[ $# -eq 5 ] || { sed -n '2,12p' "$0"; exit 2; }
w=$1 aot=$2 jit=$3 fs=$4 rootfs=$5
here=$(cd "$(dirname "$0")" && pwd)
out=${OUT:-$(mktemp -d -t aojit_profile)}; mkdir -p "$out"
cmd="sh /tmp/aojit/measure/$w.sh > /dev/null 2>&1"
now() { python3 -c 'import time; print(time.time())'; }
wall() {  # wall <label> <env>... <ish>: best of 2
    local label=$1 best= s t; shift
    for k in 1 2; do
        s=$(now); env "$@" "$fs" "$rootfs" /usr/bin/env AOJIT_SSH="${AOJIT_SSH:-}" /bin/sh -c "$cmd" < /dev/null > /dev/null 2>&1
        t=$(python3 -c "print(round($(now) - $s, 2))"); best=$(python3 -c "print(min(${best:-$t}, $t))")
    done
    printf '  %-28s %6.2fs\n' "$label" "$best"
}
echo "⏱  $w: wall time, best of 2"
wall "AOT on" "$aot"
wall "AOT off (ISH_JIT=0)" ISH_JIT=0 "$aot"
wall "all native (JIT, PIC)" ISH_JIT_PIC=1 "$jit"
echo "🧮 translations per module and CPU per module (JIT build, sampled for ${DUR:-12}s)"
rm -f "$out/map.txt"
ISH_JIT_PIC=1 ISH_JIT_MAP="$out/map.txt" "$jit" "$fs" "$rootfs" /usr/bin/env AOJIT_SSH="${AOJIT_SSH:-}" \
    /bin/sh -c "$cmd; sleep $(( ${DUR:-12} + 3 ))" < /dev/null > /dev/null 2>&1 & pid=$!
sleep 0.2; sample $pid "${DUR:-12}" 1 -file "$out/sample.txt" > /dev/null 2>&1
wait $pid
awk '$1 == "M" {printf "  %8d blocks %8d units %10d B  %s\n", $3, $4, $6, $7}' "$out/map.txt" | sort -rn | head -20
python3 "$here/../../tools/jit_aot/attrib.py" "$out/sample.txt" "$out/map.txt" --top-ish 10
[ -n "${OUT:-}" ] && echo "📄 $out/{map,sample}.txt" || rm -rf "$out"
