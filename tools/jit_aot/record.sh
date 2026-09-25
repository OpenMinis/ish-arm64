#!/bin/sh
# Record one guest module's translations and turn them into an AOT image.
#
# usage: record.sh <ish built with -Djit=true> <rootfs> <workload> <out/aot_NAME.S> [module]
#   workload  shell script inside the rootfs (guest path) that exercises the module;
#             it runs in one ish invocation so every translation shares one registry
#   module    substring of the module's guest path (default: ld-musl)
#
# The image depends on the ish that recorded it (gen.c, the pinned-register
# conventions and the struct layouts the code loads from, summed up as the
# "abi" of /proc/ish/jit; an ish with another abi rejects the image) and on the
# module's bytes: it is matched to the module by ELF build-id, wherever that
# file is installed (by path for files without one), then block by block by
# offset and instruction words. Record again after an ish or package upgrade;
# a changed module is not an error, its blocks just stop matching.
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
