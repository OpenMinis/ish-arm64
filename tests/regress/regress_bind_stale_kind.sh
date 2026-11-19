#!/bin/sh
# [FIX-261119-LP-W] meta.db rows a cutting build left behind: a long name under
# a bind recorded as a folder although the host has a file (or the other way
# round). The host's kind wins and the row is repaired, so the name works.
# Needs the state such a build leaves; on its own it checks the repaired case
# by making the row wrong itself: the folder is replaced from the HOST side.
#   $1 = bind guest root, $2 = the same folder's host path as the guest sees
#        it through a second bind (e.g. -b /mnt/h=<host folder>) - any second
#        route that changes the host behind meta.db's back
set -u
M=${1:-/mnt/longpath}; H=${2:-}
pass=0; fail=0
check() { if [ "$1" = "$2" ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL: $3 (got '$1', want '$2')"; fi; }
[ -d "$M" ] && [ -n "$H" ] && [ -d "$H" ] || { echo "SKIP: needs two binds of one folder"; exit 0; }
D=$M/sk_$$; mkdir -p "$D/thing"; check "$( [ -d "$D/thing" ] && echo dir )" dir "made as a folder"
# Behind meta.db: the folder becomes a file on the host.
rmdir "$H/sk_$$/thing" && echo content > "$H/sk_$$/thing"
check "$( [ -f "$D/thing" ] && echo file )" file "now seen as the file the host has"
check "$(cat "$D/thing" 2>&1)" content "and it reads"
echo more >> "$D/thing"; check "$?" 0 "and it can be written"
rm -rf "$D"; check "$( [ -e "$D" ] && echo left || echo gone )" gone "removed"
echo "bind_stale_kind: $pass passed, $fail failed"
[ $fail -eq 0 ]
