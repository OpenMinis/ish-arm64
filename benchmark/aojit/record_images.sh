#!/bin/sh
# Record the AOT images of images.json with a JIT build of ish, on a rootfs that
# has the suite installed (install.sh), and optionally assemble them for iOS.
#
# usage: record_images.sh <ish built with -Djit=true> <rootfs> <out dir> [image...]
#   out dir   gets aot_<name>.S (and rec/<name>.jsonl); with IOS=1 also aot_<name>_ios.o
#   image     names from images.json (default: all). Images are recorded in parallel
#             (JOBS, default 3). Link them with -Dcli_aot=<.S list> (Mac CLI) or
#             ISH_AOT_OBJECTS=<.o list> (app); they are not part of the repo.
set -e
[ $# -ge 3 ] || { sed -n '2,11p' "$0"; exit 1; }
ish=$1 rootfs=$2 out=$3; shift 3
here=$(cd "$(dirname "$0")" && pwd)
gen=$here/../../tools/jit_aot/gen.py
mkdir -p "$out/rec"
names=${*:-$(python3 -c "import json; print(' '.join(json.load(open('$here/images.json'))['images']))")}
rec() {
    name=$1
    set -- $(python3 -c "import json; i = json.load(open('$here/images.json'))['images']['$name']; print(i['module'], i['workload'])")
    echo "📼 $name: $1 <- /tmp/aojit/$2"
    ISH_JIT_PIC=1 ISH_JIT_RECORD="$out/rec/$name.jsonl" ISH_JIT_RECORD_MOD="$1" \
        "$ish" -r "$rootfs" /bin/sh "/tmp/aojit/$2" > /dev/null 2>&1 || echo "⚠️  $name: workload exited $?"
    python3 "$gen" "$out/rec/$name.jsonl" "$ish" "$rootfs" "$out/aot_$name.S" --name "$name" 2>&1 | grep -E "✅|❌" | cut -c1-160
    [ "$IOS" = 1 ] || return 0
    xcrun -sdk iphoneos clang -target arm64-apple-ios15.0 -c -o "$out/aot_${name}_ios.o" "$out/aot_$name.S" && echo "📱 $out/aot_${name}_ios.o"
}
n=0
for name in $names; do
    rec "$name" &
    n=$((n + 1)); [ $((n % ${JOBS:-3})) -ne 0 ] || wait
done
wait
ls -la "$out"/aot_*
