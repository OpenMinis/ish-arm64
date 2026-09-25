#!/bin/sh
# [T-ish-offload-path-scope] OpenMinis#288 — host unit test for the offload path
# policy. Run from anywhere; resolves the ish root from this script's location.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${TMPDIR:-/tmp}/ish_path_scope_test.$$"
trap 'rm -f "$out"' EXIT
cc -std=c11 -Wall -Wextra -Werror -I "$root" \
   "$root/kernel/native_offload_policy.c" "$here/test_path_scope.c" -o "$out"
"$out"
