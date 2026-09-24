#!/bin/sh
# Record one guest module's translations and turn them into an AOT image.
#
# usage: record.sh <ish built with -Djit=true> <rootfs> <workload> <out/aot_NAME.S> [module]
#   workload  shell script inside the rootfs (guest path) that exercises the module;
#             it runs in one ish invocation so every translation shares one registry
#   module    substring of the module's guest path (default: ld-musl)
#
# The image depends on gen.c and the pinned-register conventions of the ish
# that recorded it, and on the exact module file: record again after changing
# either (a mismatch is not an error, the loader just finds no matching keys).
set -e
[ $# -ge 4 ] || { sed -n '2,11p' "$0"; exit 1; }
ish=$1 rootfs=$2 workload=$3 out=$4 module=${5:-ld-musl}
name=$(basename "$out" .S | sed 's/^aot_//')
rec=$(mktemp -t jit_aot_rec)
trap 'rm -f "$rec"' EXIT

echo "📼 recording $module translations: $workload"
ISH_JIT_PIC=1 ISH_JIT_RECORD="$rec" ISH_JIT_RECORD_MOD="$module" \
    "$ish" -r "$rootfs" /bin/sh "$workload" > /dev/null
python3 "$(dirname "$0")/gen.py" "$rec" "$ish" "$rootfs" "$out" --name "$name"
