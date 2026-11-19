#!/bin/bash
# [T-ish-bind-path-overflow] A guest path under a bind mount whose host form
# does not fit in PATH_MAX (1024 on Darwin, so 1023 characters).
#
# The host path was built with a bare snprintf and used cut off: a write to
# <dir>/victimEXTRA, five characters too long, went to <dir>/victim and was
# reported as a success. Every case here names a path that is too long and
# checks that the call fails with ENAMETOOLONG, that no name appears on the
# host, and that the file carrying the cut-off name is untouched.
#
#   write / append / truncate / read / stat / chmod / mkdir / symlink / link
#   unlink, rmdir, rename from and onto an over-long path
#   the boundary: 1023 characters work, 1024 do not
#   a hook-routed path (ISH_TEST_PATH_HOOK), the delete guard is not asked
#   a path the host already has beyond the limit
#   a mount point that does not fit is refused
#
# Usage: tests/regress/regress_bind_long_path.sh [-i <ish>] -t <rootfs tarball>
#   The tarball (e.g. an alpine minirootfs) is fakefsified into a temp dir.
#   Needs sqlite3 and python3 on the host.
set -u
cd "$(dirname "$0")/../.."
ISH=build-arm64-release/ish
TARBALL=""
while getopts "i:t:" opt; do
    case $opt in
        i) ISH="$OPTARG" ;;
        t) TARBALL="$OPTARG" ;;
        *) exit 2 ;;
    esac
done
FAKEFSIFY="$(dirname "$ISH")/tools/fakefsify"
if [ -z "$TARBALL" ] || [ ! -f "$TARBALL" ] || [ ! -x "$FAKEFSIFY" ]; then
    echo "SKIP: need -t <rootfs tarball> and $FAKEFSIFY" >&2
    exit 0
fi
W=$(mktemp -d -t regress_bind_long_path)
trap 'python3 -c "import shutil,sys; shutil.rmtree(sys.argv[1], ignore_errors=True)" "$W"' EXIT
BASE=$W/base
"$FAKEFSIFY" "$TARBALL" "$BASE" || exit 1
"$ISH" -f "$BASE" /bin/mkdir -p /mnt/b /mnt/r 2>/dev/null

pass=0; fail=0
check() { if [ "$1" = "$2" ]; then pass=$((pass+1)); echo "PASS $3"; else fail=$((fail+1)); echo "FAIL $3 (got '$1', want '$2')"; fi; }

LIMIT=$(printf '#include <limits.h>\nPATH_MAX\n' | cc -E -x c - 2>/dev/null | tail -1)
case "$LIMIT" in ''|*[!0-9]*) LIMIT=1024 ;; esac
MAXLEN=$((LIMIT - 1))

# A fresh world. $1 is the host folder's name under the world. Prints nothing;
# sets R (rootfs), H (resolved host folder), REL (the deep folder below it):
# H/REL/victim is exactly MAXLEN characters long.
n=0
fresh() {
    n=$((n+1)); D=$W/w$n; mkdir -p "$D"
    cp -R "$BASE" "$D/rootfs"; R=$D/rootfs
    L=$D/guard.log; : > "$L"
    REL=$(python3 - "$D/host" "$MAXLEN" <<'PY'
import os, sys
os.makedirs(sys.argv[1]); H = os.path.realpath(sys.argv[1]); maxlen = int(sys.argv[2])
need = maxlen - len("/victim") - len(H)
parts = []
while need > 0:
    k = min(200, need - 1)
    if need - 1 - k == 1: k -= 1
    parts.append("d" * k); need -= k + 1
rel = "/".join(parts); P = H + "/" + rel
os.makedirs(P)
assert len(P + "/victim") == maxlen
open(P + "/victim", "w").write("precious\n")
open(P + "/other", "w").write("other\n")
os.mkdir(P + "/vd")
open(P + "/vd/in", "w").write("in\n")
print(rel)
PY
)
    H=$(cd "$D/host" && pwd -P)
    P=$H/$REL
}
filt() { grep -v -E '^(fakefs_bind_mount|create_relative_symlink):'; }
g() { ISH_TEST_BIND="/mnt/b=$H" "$ISH" -f "$R" /bin/sh -c "cd '/mnt/b/$REL' || exit 99; $1" 2>&1 | filt | sed 's/d\{40,\}/<deep>/g'; }
gr() { ISH_TEST_PATH_HOOK="/mnt/r=$H" "$ISH" -f "$R" /bin/sh -c "cd '/mnt/r/$REL' || exit 99; $1" 2>&1 | filt | sed 's/d\{40,\}/<deep>/g'; }
names() { ls -A "$P" | LC_ALL=C sort | tr '\n' ' '; }
state() { echo "$(names)|$(cat "$P/victim")|$(cat "$P/other")|$(ls -A "$P/vd" | tr '\n' ' ')|$(stat -f %Lp "$P/victim")"; }
CLEAN="other vd victim |precious|other|in |644"
rows() { sqlite3 "$R/meta.db" "select count(*) from paths where cast(path as text) like '%EXTRA%';"; }
toolong() { case "$1" in *"ilename too long"*|*"ile name too long"*) echo ENAMETOOLONG ;; *) echo "$1" ;; esac; }

# ---- writes ----
fresh
check "$(printf %s "$P/victim" | wc -c | tr -d ' ')" "$MAXLEN" "setup: the victim's host path is exactly $MAXLEN characters"
check "$(g 'cat victim')" "precious" "a path of exactly $MAXLEN characters is read"
out=$(g 'echo OVERWRITTEN > victimEXTRA; echo "rc=$?"')
check "$(toolong "$out" | head -1)|$(echo "$out" | tail -1)" "ENAMETOOLONG|rc=1" "create and write: ENAMETOOLONG"
check "$(state)" "$CLEAN" "create and write: no new name on the host, the victim is untouched"
out=$(g 'echo more >> victimEXTRA; echo "rc=$?"')
check "$(toolong "$out" | head -1)|$(echo "$out" | tail -1)" "ENAMETOOLONG|rc=1" "append: ENAMETOOLONG"
out=$(g 'cat /dev/null > victimEXTRA; echo "rc=$?"')
check "$(toolong "$out" | head -1)|$(echo "$out" | tail -1)" "ENAMETOOLONG|rc=1" "truncate: ENAMETOOLONG"
check "$(state)" "$CLEAN" "append and truncate: the victim keeps its content"
check "$(g '{ echo x > victimX; } 2>/dev/null; echo "rc=$?"')" "rc=1" "one character over the limit fails as well"
check "$(state)" "$CLEAN" "one over: nothing changed"

# ---- reads and attributes ----
check "$(toolong "$(g 'cat victimEXTRA')")" "ENAMETOOLONG" "read: ENAMETOOLONG, not the victim's content"
check "$(toolong "$(g 'stat victimEXTRA')")" "ENAMETOOLONG" "stat: ENAMETOOLONG"
check "$(g '[ -e victimEXTRA ] && echo yes || echo no')" "no" "the over-long name does not exist"
g 'chmod 600 victimEXTRA' >/dev/null
check "$(g 'stat -c %a victim')|$(stat -f %Lp "$P/victim")" "644|644" "chmod of the over-long name leaves the victim's mode alone"
check "$(g 'ls' | tr '\n' ' ')" "other vd victim " "the folder lists what the host has"

# ---- creating other things ----
out=$(g 'mkdir victimEXTRA; echo "rc=$?"')
check "$(toolong "$out" | head -1)|$(echo "$out" | tail -1)" "ENAMETOOLONG|rc=1" "mkdir: ENAMETOOLONG"
check "$(g 'mkdir -p vdEXTRAEXTRA/sub 2>/dev/null; echo "rc=$?"')" "rc=1" "mkdir -p of an over-long folder fails"
check "$(toolong "$(g 'ln -s other victimEXTRA')")" "ENAMETOOLONG" "symlink at an over-long name: ENAMETOOLONG"
check "$(toolong "$(g 'ln other victimEXTRA')")" "ENAMETOOLONG" "hard link at an over-long name: ENAMETOOLONG"
check "$(toolong "$(g 'mknod victimEXTRA p')")" "ENAMETOOLONG" "mknod (a fifo) at an over-long name: ENAMETOOLONG"
check "$(state)" "$CLEAN" "mkdir, symlink, link: no new name, nothing changed"

# ---- removing and renaming ----
out=$(g 'rm victimEXTRA; echo "rc=$?"')
check "$(toolong "$out" | head -1)|$(echo "$out" | tail -1)" "ENAMETOOLONG|rc=1" "unlink: ENAMETOOLONG"
check "$(g 'rm -f victimEXTRA 2>/dev/null; echo "rc=$?"')|$(state)" "rc=1|$CLEAN" "rm -f fails too; the victim is still there"
check "$(g 'rmdir vdEXTRAEXTRA 2>/dev/null; echo "rc=$?"; rm -rf vdEXTRAEXTRA 2>/dev/null; echo "rc=$?"')" "rc=1
rc=1" "rmdir and rm -r of an over-long folder name fail"
check "$(state)" "$CLEAN" "the folder with the cut-off name and its file are still there"
out=$(g 'mv other victimEXTRA; echo "rc=$?"')
check "$(toolong "$out" | head -1)|$(echo "$out" | tail -1)" "ENAMETOOLONG|rc=1" "rename onto an over-long name: ENAMETOOLONG"
check "$(state)" "$CLEAN" "rename onto: the source stays, the victim is not replaced"
check "$(g 'mv victimEXTRA moved 2>/dev/null; echo "rc=$?"')" "rc=1" "rename from an over-long name fails"
check "$(state)" "$CLEAN" "rename from: the victim is not moved away"
check "$(rows)" "0" "meta.db has no row for any over-long path"

# ---- the boundary: a new name of the same length as the victim works ----
check "$(g 'echo fits > victi2; echo "rc=$?"; cat victi2; mv victi2 victi3; rm victi3; echo "rc=$?"')" "rc=0
fits
rc=0" "create, read, rename and remove at exactly $MAXLEN characters work"
check "$(state)" "$CLEAN" "and leave the folder as it was"

# ---- hook-routed paths, and the delete guard ----
fresh
out=$(gr 'echo OVERWRITTEN > victimEXTRA; echo "rc=$?"')
check "$(toolong "$out" | head -1)|$(echo "$out" | tail -1)" "ENAMETOOLONG|rc=1" "hook-routed create: ENAMETOOLONG"
check "$(gr 'rm victimEXTRA 2>/dev/null; echo "rc=$?"; mv other victimEXTRA 2>/dev/null; echo "rc=$?"; cat victimEXTRA 2>/dev/null; echo "rc=$?"')" "rc=1
rc=1
rc=1" "hook-routed unlink, rename and read fail"
check "$(state)" "$CLEAN" "hook-routed: nothing changed on the host"
check "$(gr 'cat victim; echo ok > victi2; cat victi2; rm victi2')" "precious
ok" "hook-routed paths that fit still work"
out=$(ISH_TEST_BIND="/mnt/b=$H" ISH_TEST_GUARD=allow ISH_TEST_GUARD_LOG="$L" "$ISH" -f "$R" /bin/sh -c "cd '/mnt/b/$REL'; rm victimEXTRA 2>/dev/null; echo \"rc=\$?\"; mv other victimEXTRA 2>/dev/null; echo \"rc=\$?\"" 2>&1 | filt)
check "$out|$(wc -l < "$L" | tr -d ' ')" "rc=1
rc=1|0" "the delete guard is not asked about a path that cannot exist"
check "$(state)" "$CLEAN" "with a guard that allows everything: still nothing changed"

# ---- a path the host already has beyond the limit ----
fresh
(cd "$P" && mkdir longer_than_the_limit && echo deep > longer_than_the_limit/file)
check "$(g 'cat longer_than_the_limit/file 2>/dev/null; echo "rc=$?"')" "rc=1" "a host file beyond the limit cannot be read through the mount"
check "$(g '{ echo x > longer_than_the_limit/file; } 2>/dev/null; echo "rc=$?"; rm -rf longer_than_the_limit 2>/dev/null; echo "rc=$?"')" "rc=1
rc=1" "nor written, nor removed"
check "$(cd "$P" && cat longer_than_the_limit/file)|$(cat "$P/victim")" "deep|precious" "it and its neighbours are unchanged"

# ---- a mount point that does not fit ----
fresh
LONGGUEST=/$(python3 -c "print('g' * ($LIMIT + 50))")
out=$(ISH_TEST_BIND="$LONGGUEST=$H,/mnt/b=$H" "$ISH" -f "$R" /bin/sh -c "cat '/mnt/b/$REL/victim'" 2>&1)
check "$(echo "$out" | grep -c 'path too long')|$(echo "$out" | tail -1)" "1|precious" "an over-long mount point is refused; the next mount works"

echo
echo "passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
