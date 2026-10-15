#!/bin/sh
# Record one guest module's translations and turn them into an AOT image.
#
# usage: record.sh <ish built with -Djit=true> <rootfs> <workload> <out/aot_NAME.S> [module]
#   workload  shell script inside the rootfs (guest path) that exercises the module;
#             it runs in one ish invocation so every translation shares one registry
#   module    substring of the module's guest path (default: ld-musl)
#   RECORDING=<file>  keep the raw recording there (default: a temporary file), and
#                     the workload's output in <file>.log
#
# A workload whose output shows a step that failed (FAIL, Traceback, a usage
# message, "skipped", a missing command or file, a crash) makes no image: the
# code that step would have run is missing from the recording. So does a
# recording made after the JIT's code region filled up (gen.py checks that).
# The image depends on the ish that recorded it (gen.c, the pinned-register
# conventions and the struct layouts the code loads from, summed up as the
# "abi" of /proc/ish/jit; an ish with another abi rejects the image) and on the
# module's bytes: it is matched to the module by ELF build-id, wherever that
# file is installed (by path for files without one), then block by block by
# offset and instruction words. Record again after an ish or package upgrade;
# a changed module is not an error, its blocks just stop matching.
set -e
[ $# -ge 4 ] || { sed -n '2,8p' "$0"; exit 1; }
ish=$1 rootfs=$2 workload=$3 out=$4 module=${5:-ld-musl}
# the image's symbol is _ish_aot_module_<name>: versioned names (aot_musl_1.2.5-r8.S) need . and - mapped
name=$(basename "$out" .S | sed 's/^aot_//' | tr '.-' '__')
if [ -n "$RECORDING" ]; then
    rec=$RECORDING
    log=$RECORDING.log
else
    rec=$(mktemp -t jit_aot_rec)
    log=$rec.log
    trap 'rm -f "$rec" "$log"' EXIT
fi

echo "📼 recording $module translations: $workload"
ISH_JIT_PIC=1 ISH_JIT_RECORD="$rec" ISH_JIT_RECORD_MOD="$module" \
    "$ish" -r "$rootfs" /bin/sh "$workload" < /dev/null > "$log" 2>&1 || echo "⚠️  $workload exited $?"
if grep -n -E 'FAIL|Traceback|unrecognized option|^Usage:|skipped|: not found|No such file or directory|Segmentation fault|Killed|❌' "$log" > "$log.bad"; then
    echo "❌ $workload: a step failed, so its code is not recorded (fix the workload or its rootfs):"
    head -5 "$log.bad" | cut -c1-200 | sed 's/^/   /'
    rm -f "$log.bad"
    exit 1
fi
rm -f "$log.bad"
python3 "$(dirname "$0")/gen.py" "$rec" "$ish" "$rootfs" "$out" --name "$name"
