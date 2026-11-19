#!/bin/sh
# [FIX-261119-LP-W] Inside the rootfs (no bind), a folder whose host path is
# past 260 characters lists its entries and rm -rf removes it. Before the fix
# F_GETPATH was capped at 260 bytes on Windows (PATH_MAX), so the folder's own
# path could not be read back: readdir skipped every entry (ls empty) and
# rm -rf stopped with "Directory not empty".
#   $1 = a folder in the rootfs to work in (default /tmp)
set -u
B=${1:-/tmp}/rd_$$
pass=0; fail=0
check() { if [ "$1" = "$2" ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL: $3 (got '$1', want '$2')"; fi; }
mkdir -p "$B"; c=$B; j=0
while [ ${#c} -lt 420 ]; do c="$c/level-$(printf '%04d' $j)"; mkdir "$c"; echo "f$j" > "$c/file.txt"; j=$((j+1)); done
check "$(ls "$c")" file.txt "the deepest folder lists its file"
check "$(find "$B" -name file.txt | wc -l | tr -d ' ')" "$j" "find sees every file"
check "$(cat "$c/file.txt")" "f$((j-1))" "the deepest file reads back"
rm -rf "$B"; check "$( [ -e "$B" ] && echo left || echo gone )" gone "rm -rf removes the whole tree"
echo "rootfs_deep_listing: $pass passed, $fail failed"
[ $fail -eq 0 ]
