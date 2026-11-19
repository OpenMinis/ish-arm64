#!/bin/sh
# [FIX-261119-W] Paths longer than 300 characters under a bind mount: a whole
# Windows user folder mounted into Minis holds them (AppData\Local\Packages\...).
# Listing and walking such a tree must work and must never overrun fakefs's
# path buffers (fakefs_readdir appended entry names with unchecked strcat).
#
# Failed on Windows until FIX-261119-LP-W: PATH_MAX was 260 in ish.exe and
# bind host paths were cut at 259 characters ("File exists", "Is a
# directory"); see regress_bind_wrong_place.sh for what that did to files.
#
# Run inside the guest, with a host folder bound at $1 (default /mnt/longpath):
#   ish.exe -f <rootfs> -b /mnt/longpath=<empty host folder> /bin/sh /tmp/regress_bind_long_path.sh
set -u
M=${1:-/mnt/longpath}
pass=0; fail=0
check() { if [ "$1" = "$2" ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL: $3 (got '$1', want '$2')"; fi; }

if [ ! -d "$M" ]; then
    echo "SKIP: $M is not mounted"
    exit 0
fi
D="$M/lp_$$"
seg=abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ   # 62 characters
deep="$D"
i=0
while [ $i -lt 6 ]; do deep="$deep/$seg$i"; i=$((i+1)); done
check "$( [ ${#deep} -gt 300 ] && echo long )" long "the test path is over 300 characters (${#deep})"

mkdir -p "$deep"; check "$?" 0 "mkdir -p a ${#deep}-character path"
echo deep > "$deep/f.txt"; check "$?" 0 "write a file at the bottom"
check "$(cat "$deep/f.txt" 2>&1)" deep "read it back"
check "$(ls "$deep" 2>&1)" f.txt "list the bottom folder"
check "$(find "$D" -type f 2>/dev/null | wc -l | tr -d ' ')" 1 "find walks the whole tree"
check "$(find "$D" -type d 2>/dev/null | wc -l | tr -d ' ')" 7 "every folder is listed"
ls -R "$D" > /dev/null 2>&1; check "$?" 0 "ls -R does not fail"
# A long file name at the bottom too (255 is the guest's NAME_MAX).
name=$(printf '%0200d' 0)
echo x > "$deep/$name"; check "$(ls "$deep" | wc -l | tr -d ' ')" 2 "a 200-character name beside it"

rm -rf "$D"; check "$( [ -e "$D" ] && echo left || echo gone )" gone "rm -rf removes it"
echo "bind_long_path: $pass passed, $fail failed"
[ $fail -eq 0 ]
