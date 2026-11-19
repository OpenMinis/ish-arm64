#!/bin/bash
# [T-ish-bind-host-mode] The mode of files under a static bind mount (a host
# folder the user mounted). Driven through the CLI's ISH_TEST_BIND.
#
#   host bits   a path the guest never chmod'ed reports the host file's
#               permission bits (0600 key, 0755 script, 0700 folder), owner
#               root, rdev 0; a symbolic link reports its target's; a host
#               chmod shows at the next stat
#   exec        a 0755 host script runs, a 0644 one does not (fstat at open)
#   guest chmod wins from then on, also when it sets the old fixed default
#               (0644 / 0755); it is not written through to the host
#   old rows    a row an older build wrote (0644 / 0755, no mark) follows the
#               host; one it chmod'ed keeps its mode
#   no writes   a stat of a path already seen changes nothing in meta.db
#   read-only   same bits; chmod is EROFS
#   elsewhere   rootfs files are untouched by any of this
#
# Usage: tests/regress/regress_bind_mode.sh [-i <ish>] [-o <older ish>] -t <rootfs tarball>
#   The tarball (e.g. an alpine minirootfs) is fakefsified into a temp dir.
#   -o: a build from before this change, to write real old rows (optional;
#   the same rows are also made by hand). Needs sqlite3 on the host.
set -u
cd "$(dirname "$0")/../.."
ISH=build-arm64-release/ish
OLD=""
TARBALL=""
while getopts "i:o:t:" opt; do
    case $opt in
        i) ISH="$OPTARG" ;;
        o) OLD="$OPTARG" ;;
        t) TARBALL="$OPTARG" ;;
        *) exit 2 ;;
    esac
done
FAKEFSIFY="$(dirname "$ISH")/tools/fakefsify"
if [ -z "$TARBALL" ] || [ ! -f "$TARBALL" ] || [ ! -x "$FAKEFSIFY" ]; then
    echo "SKIP: need -t <rootfs tarball> and $FAKEFSIFY" >&2
    exit 0
fi
W=$(mktemp -d -t regress_bind_mode)
trap 'chmod -R u+rwx "$W" 2>/dev/null; rm -rf "$W"' EXIT
BASE=$W/base
"$FAKEFSIFY" "$TARBALL" "$BASE" || exit 1
"$ISH" -f "$BASE" /bin/mkdir -p /mnt/b 2>/dev/null

pass=0; fail=0
check() { if [ "$1" = "$2" ]; then pass=$((pass+1)); echo "PASS $3"; else fail=$((fail+1)); echo "FAIL $3 (got '$1', want '$2')"; fi; }

# A fresh world: rootfs copy R and a host folder H.
n=0
fresh() {
    n=$((n+1)); D=$W/w$n; mkdir -p "$D"
    cp -R "$BASE" "$D/rootfs"; R=$D/rootfs
    H=$D/host; mkdir -p "$H/priv" "$H/pub"
    echo key > "$H/key"; chmod 600 "$H/key"
    printf '#!/bin/sh\necho ran\n' > "$H/run.sh"; chmod 755 "$H/run.sh"
    printf '#!/bin/sh\necho ran\n' > "$H/norun.sh"; chmod 644 "$H/norun.sh"
    echo plain > "$H/plain"; chmod 644 "$H/plain"
    echo grp > "$H/grp"; chmod 640 "$H/grp"
    echo in > "$H/priv/inside"; chmod 700 "$H/priv"; chmod 755 "$H/pub"
    ln -s key "$H/link"
}
filt() { grep -v -E '^(fakefs_bind_mount|create_relative_symlink):'; }
g() { ISH_TEST_BIND="/mnt/b=$H" "$ISH" -f "$R" "$@" 2>&1 | filt; }
gro() { ISH_TEST_BIND_RO="/mnt/b=$H" "$ISH" -f "$R" "$@" 2>&1 | filt; }
gold() { ISH_TEST_BIND="/mnt/b=$H" "$OLD" -f "$R" "$@" 2>&1 | filt; }
hostmode() { stat -f '%Lp' "$1"; }
# "inode:hex" of every row under the mount, for "nothing was written".
rows() { sqlite3 "$R/meta.db" "select cast(p.path as text) || ':' || hex(s.stat) from paths p join stats s using(inode) where cast(p.path as text) like '/mnt/b%' order by 1;"; }
rowof() { sqlite3 "$R/meta.db" "select hex(s.stat) from paths p join stats s using(inode) where cast(p.path as text) = '$1';"; }
ST='stat -c "%a %u:%g %t,%T %n"'

# ---- host bits ----
fresh
out=$(g /bin/sh -c "cd /mnt/b; $ST key run.sh plain grp priv pub link")
check "$out" "600 0:0 0,0 key
755 0:0 0,0 run.sh
644 0:0 0,0 plain
640 0:0 0,0 grp
700 0:0 0,0 priv
755 0:0 0,0 pub
600 0:0 0,0 link" "first stat: the host file's bits, owner root, rdev 0, a link as its target"
out2=$(g /bin/sh -c "cd /mnt/b; $ST key run.sh plain grp priv pub link")
check "$out2" "$out" "second run (rows exist now): the same"
check "$(g /bin/sh -c 'ls -l /mnt/b/key | cut -c1-10')" "-rw-------" "ls -l shows the key as -rw-------"
check "$(g /bin/cat /mnt/b/key)" "key" "a 0600 file is still readable (the guest is root)"
check "$(g /bin/ls /mnt/b/priv)" "inside" "a 0700 folder is still listable"

chmod 644 "$H/key"; chmod 600 "$H/plain"; chmod 750 "$H/pub"
check "$(g /bin/sh -c "cd /mnt/b; stat -c '%a %n' key plain pub")" "644 key
600 plain
750 pub" "a host chmod shows at the next stat"

# ---- exec ----
fresh
check "$(g /bin/sh -c 'cd /mnt/b; ./run.sh; echo "rc=$?"')" "ran
rc=0" "a 0755 host script runs"
check "$(g /bin/sh -c 'cd /mnt/b; ./norun.sh 2>/dev/null; echo "rc=$?"')" "rc=126" "a 0644 host script does not"
chmod 755 "$H/norun.sh"
check "$(g /bin/sh -c 'cd /mnt/b; ./norun.sh; echo "rc=$?"')" "ran
rc=0" "it runs once the host makes it executable"

# ---- guest chmod ----
fresh
check "$(g /bin/sh -c 'cd /mnt/b; chmod 640 plain; echo "rc=$?"; stat -c "%a %t,%T" plain')" "rc=0
640 0,0" "chmod in the guest takes effect, rdev still reads 0"
check "$(hostmode "$H/plain")" "644" "it is not written through to the host file"
check "$(g /bin/stat -c %a /mnt/b/plain)" "640" "it persists across runs"
chmod 600 "$H/plain"
check "$(g /bin/stat -c %a /mnt/b/plain)" "640" "a later host chmod does not undo the guest's choice"
check "$(g /bin/sh -c 'cd /mnt/b; chmod 644 key; stat -c %a key')" "644" "chmod to the old fixed default (0644) on a 0600 host file is kept too"
check "$(g /bin/stat -c %a /mnt/b/key)" "644" "and persists"
check "$(g /bin/sh -c 'cd /mnt/b; chmod 755 priv; stat -c %a priv')" "755" "the same for a folder (0755 on a 0700 host folder)"
check "$(hostmode "$H/key")|$(hostmode "$H/priv")" "600|700" "the host key and folder are unchanged"
check "$(g /bin/sh -c 'cd /mnt/b; chmod +x norun.sh; ./norun.sh; echo "rc=$?"')" "ran
rc=0" "chmod +x in the guest makes a host 0644 script runnable"
check "$(g /bin/sh -c 'cd /mnt/b; chown 1000:1000 grp; stat -c "%a %u:%g" grp')" "640 1000:1000" "chown changes the owner and leaves the mode following the host"

# ---- files the guest creates ----
fresh
check "$(g /bin/sh -c 'cd /mnt/b; touch n; mkdir d; (umask 077; touch s; mkdir sd); stat -c "%a %n" n d s sd')" "644 n
755 d
600 s
700 sd" "new files and folders report the mode the guest asked for"
check "$(g /bin/sh -c 'cd /mnt/b; cp -p /etc/passwd c1; chmod 751 /tmp; cp -pr /tmp c2; stat -c "%a %n" c1 c2')" "644 c1
751 c2" "cp -p into the mount keeps the source's mode"
check "$(g /bin/sh -c 'cp -p /mnt/b/key /root/k; cp -p /mnt/b/run.sh /root/r; stat -c "%a %n" /root/k /root/r')" "600 /root/k
755 /root/r" "cp -p out of the mount carries the host bits"

# ---- rows written before this change ----
fresh
g /bin/sh -c 'cd /mnt/b; stat key run.sh plain priv >/dev/null; chmod 600 plain; chmod 711 pub' >/dev/null
# What an older build left: no mark in rdev (bytes 13-16 of the row).
sqlite3 "$R/meta.db" "update stats set stat = substr(stat, 1, 12) || x'00000000' where inode in (select inode from paths where cast(path as text) like '/mnt/b/%');"
check "$(rowof /mnt/b/key)|$(rowof /mnt/b/plain)" "A4810000000000000000000000000000|80810000000000000000000000000000" "hand-made old rows: a default one and a chmod'ed one, neither marked"
check "$(g /bin/sh -c "cd /mnt/b; stat -c '%a %n' key run.sh priv plain pub")" "600 key
755 run.sh
700 priv
600 plain
711 pub" "old default rows follow the host; old chmod'ed rows keep their mode"
if [ -n "$OLD" ] && [ -x "$OLD" ]; then
    fresh
    gold /bin/sh -c 'cd /mnt/b; ls -l >/dev/null; stat key run.sh priv >/dev/null; chmod 400 grp' >/dev/null
    check "$(gold /bin/sh -c "cd /mnt/b; stat -c '%a %n' key run.sh priv grp")" "644 key
644 run.sh
755 priv
400 grp" "(older build) reports the fixed 0644 / 0755"
    check "$(g /bin/sh -c "cd /mnt/b; stat -c '%a %n' key run.sh priv grp")" "600 key
755 run.sh
700 priv
400 grp" "rows the older build wrote: host bits now, its chmod kept"
else
    echo "SKIP rows written by an older build (no -o <older ish>)"
fi

# ---- no meta.db writes on stat ----
fresh
g /bin/sh -c 'cd /mnt/b; ls -l . priv pub >/dev/null; stat key run.sh plain grp priv pub link >/dev/null' >/dev/null
before=$(rows)
g /bin/sh -c 'cd /mnt/b; ls -l . priv pub >/dev/null; stat key run.sh plain grp priv pub link >/dev/null; cat key >/dev/null; ./run.sh >/dev/null' >/dev/null
check "$(rows)" "$before" "stat, ls -l, read and exec of paths already seen leave every row as it was"
check "$(echo "$before" | grep -c ':.\{24\}00000000$')" "$(echo "$before" | wc -l | tr -d ' ')" "no row is marked unless the guest chmod'ed it"

# ---- read-only mount ----
fresh
check "$(gro /bin/sh -c "cd /mnt/b; stat -c '%a %n' key run.sh priv")" "600 key
755 run.sh
700 priv" "read-only mount: the same host bits"
out=$(gro /bin/sh -c 'cd /mnt/b; chmod 644 key 2>&1; echo "rc=$?"; stat -c %a key')
check "$(echo "$out" | sed 's/.*: //' | tr '\n' ' ')" "Read-only file system rc=1 600 " "read-only mount: chmod is refused (EROFS), nothing changes"
check "$(gro /bin/sh -c 'cd /mnt/b; ./run.sh')" "ran" "read-only mount: a 0755 script still runs"

# ---- rootfs files ----
fresh
check "$(g /bin/sh -c 'stat -c "%a %u:%g" /etc/passwd /bin/busybox /root')" "644 0:0
755 0:0
700 0:0" "rootfs files report their meta.db mode as before"
g /bin/chmod 600 /etc/hostname >/dev/null
check "$(rowof /etc/hostname | cut -c25-32)" "00000000" "a chmod outside a mount writes no mark"

echo
echo "passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
