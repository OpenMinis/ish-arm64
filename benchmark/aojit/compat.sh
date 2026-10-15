#!/bin/bash
# The ARM64 half of benchmark/run.sh's compatibility suite against a given ish (an AOT build, say).
# A failing test runs again with ISH_JIT=0 and once more as it was, to tell AOT from other causes.
#
# usage: compat.sh <ish> [fakefs rootfs (default: alpine-arm64-fakefs next to the repo)]
set -uo pipefail
[ $# -ge 1 ] || { sed -n '2,6p' "$0"; exit 2; }
here=$(cd "$(dirname "$0")" && pwd); repo=$(cd "$here/../.." && pwd)
lib=$(mktemp -t aojit_compat); sed '$d' "$repo/benchmark/run.sh" > "$lib"   # everything but its main call
source "$lib"; rm -f "$lib"
ISH_ARM64=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
FAKEFS_ARM64=${2:-$repo/alpine-arm64-fakefs}; ASSETS_DIR=$repo/benchmark/assets
_purge_fakefs_special_files "$FAKEFS_ARM64" "ARM64"
for asset in bun_lang_test.js; do
    [ -f "$ASSETS_DIR/$asset" ] && timeout 10 "$ISH_ARM64" -f "$FAKEFS_ARM64" /bin/sh -c "cat > /tmp/$asset" < "$ASSETS_DIR/$asset" 2> /dev/null
done
total=${#COMPAT_TESTS[@]} n=0 pass=0 fails=()
for line in "${COMPAT_TESTS[@]}"; do
    n=$((n + 1)); IFS='|' read -r cat name cmd <<< "$line"
    t=15; case "$cmd" in *"npx -y"*) t=60 ;; esac
    case "$name" in "bun lang+stdlib"|"codex --version") t=20 ;; "claude -p (no-auth)") t=30 ;; esac
    if timeout "$t" "$ISH_ARM64" -f "$FAKEFS_ARM64" /bin/sh -c "$cmd" < /dev/null > /dev/null 2>&1; then r=PASS; pass=$((pass + 1))
    else r="FAIL(rc=$?)"; fails+=("$line|$t"); fi
    printf "[%3d/%d] %-9s %-24s %s\n" $n $total "$cat" "$name" "$r"
done
echo "== $pass / $total pass"
for f in "${fails[@]+"${fails[@]}"}"; do
    IFS='|' read -r cat name cmd t <<< "$f"
    if ISH_JIT=0 timeout "$t" "$ISH_ARM64" -f "$FAKEFS_ARM64" /bin/sh -c "$cmd" < /dev/null > /dev/null 2>&1; then b=PASS; else b="FAIL(rc=$?)"; fi
    if timeout "$t" "$ISH_ARM64" -f "$FAKEFS_ARM64" /bin/sh -c "$cmd" < /dev/null > /dev/null 2>&1; then a=PASS; else a="FAIL(rc=$?)"; fi
    echo "   ↻ $cat/$name: without AOT (ISH_JIT=0) $b, again with AOT $a"
done
"$ISH_ARM64" -f "$FAKEFS_ARM64" /bin/cat /proc/ish/jit < /dev/null 2> /dev/null | grep -E "^images:"
[ $pass -eq $total ]
