#!/bin/sh
# [FIX-261119-LP-W] A path under a bind that is long on the host is either used
# whole or refused with ENAMETOOLONG - never cut. Before the fix, ish.exe built
# bind host paths in char[260] and cut them at 259 characters: a write landed
# on (and overwrote) the file with the cut name, a rename or unlink could move
# or remove it, and a mkdir made a folder with a cut name (INV-261119-LP).
#
#   $1 = the bind's guest root (default /mnt/longpath)
#   $2 = the length of the bind's host folder (e.g. 64 for C:/Users/.../x), so
#        the test can place names exactly at the old 259-character edge
# Run: ish.exe -f <rootfs> -b /mnt/longpath=<empty host folder> \
#          -b /mnt/t=<folder with this file> /bin/sh /mnt/t/regress_bind_wrong_place.sh /mnt/longpath <len>
set -u
M=${1:-/mnt/longpath}; HB=${2:-0}
pass=0; fail=0
check() { if [ "$1" = "$2" ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL: $3 (got '$1', want '$2')"; fi; }
hl() { echo $(( HB + ${#1} - ${#M} )); }
[ -d "$M" ] || { echo "SKIP: $M is not mounted"; exit 0; }
[ "$HB" -gt 0 ] || { echo "SKIP: give the host folder's length as \$2"; exit 0; }
D="$M/wp_$$"; mkdir -p "$D"

# A folder whose host path is 252 characters: a 34-character name there was
# cut to 6 ("WRONGP") at the old edge.
c=$D; j=0
while [ $(( $(hl "$c") + 5 )) -le 252 ]; do c="$c/p$(printf '%03d' $j)"; j=$((j+1)); done
while [ $(hl "$c") -lt 252 ]; do c="${c}x"; done
mkdir -p "$c"
check "$(hl "$c")" 252 "the test folder is 252 characters on the host"
LONG=WRONGPLACE-0000000000000000000.txt
CUT=$(echo "$LONG" | cut -c1-$(( 259 - 252 - 1 )))
check "$CUT" WRONGP "the old cut name"
echo UNRELATED-ORIGINAL > "$c/$CUT"

# 1. A write to the long name writes the long name, and only it.
echo NEW-CONTENT > "$c/$LONG"; check "$?" 0 "write the long name"
check "$(cat "$c/$CUT")" UNRELATED-ORIGINAL "the file with the cut name is untouched"
check "$(cat "$c/$LONG" 2>&1)" NEW-CONTENT "the long name holds what was written"
check "$(ls "$c" | sort | tr '\n' ' ')" "$CUT $LONG " "the folder holds exactly the two files"

# 2. rename and unlink of long names leave the cut name alone.
mv "$c/$LONG" "$c/${LONG}.moved"; check "$?" 0 "rename a long name"
check "$(cat "$c/$CUT")" UNRELATED-ORIGINAL "rename did not touch the cut name"
check "$(cat "$c/${LONG}.moved" 2>&1)" NEW-CONTENT "the renamed file has its content"
rm "$c/${LONG}.moved"; check "$?" 0 "unlink a long name"
check "$(cat "$c/$CUT" 2>&1)" UNRELATED-ORIGINAL "unlink did not remove the cut name"

# 3. mkdir of a long name makes that name, not a cut one.
mkdir "$c/DIRECTORY-WITH-A-LONG-NAME-000000"; check "$?" 0 "mkdir a long name"
check "$(ls -d "$c"/DIRECT* 2>/dev/null | wc -l | tr -d ' ')" 1 "one folder, with its whole name"
check "$( [ -d "$c/DIRECTORY-WITH-A-LONG-NAME-000000" ] && echo yes )" yes "it is there under its whole name"

# 4. Past 4095 host characters: ENAMETOOLONG, and nothing is made anywhere.
#    The guest path stays under the guest's own 4096 limit.
d=$c; k=0
while [ $(( $(hl "$d") + 101 )) -le 4080 ]; do d="$d/$(printf "%0100d" $k)"; mkdir "$d" || break; k=$((k+1)); done
check "$( [ $(hl "$d") -gt 3970 ] && echo deep )" deep "a folder about 4000 characters deep on the host ($(hl "$d"))"
before=$(ls "$d" | wc -l | tr -d ' ')
name=$(printf "%0150d" 7)
touch "$d/$name" 2>/tmp/lp_err; rc=$?
check "$rc" 1 "a create past 4095 host characters fails"
too_long() { sed 's/.*: //' /tmp/lp_err | sed 's/^Filename too long$/File name too long/'; }   # busybox / libc wording
check "$(too_long)" "File name too long" "with ENAMETOOLONG"
mkdir "$d/$name" 2>/tmp/lp_err; check "$(too_long)" "File name too long" "mkdir past 4095: ENAMETOOLONG"
check "$(ls "$d" | wc -l | tr -d ' ')" "$before" "nothing new appeared in the folder"

rm -rf "$D"; check "$( [ -e "$D" ] && echo left || echo gone )" gone "rm -rf removes the whole tree"
echo "bind_wrong_place: $pass passed, $fail failed"
[ $fail -eq 0 ]
