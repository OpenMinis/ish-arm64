#!/bin/bash
# The fakefs delete guard (fs/fake.h): a host hook asked before unlink, rmdir
# and a rename onto an existing destination, on static bind mounts and on
# hook-routed paths. Driven through the CLI's ISH_TEST_BIND / ISH_TEST_GUARD.
#
#   off         no hook, or scope 0: every result is what it is without the
#               guard, and the hook is not called
#   allow       the operation happens; meta.db and the host agree afterwards
#   deny        EPERM; the host files and the meta.db rows are untouched
#   wait        the hook blocks first, then allows or denies: same results
#   moved       the mount is re-pointed or removed while the hook waits: EIO,
#               and nothing is deleted at the old place or the new one
#   kill        a task blocked in the hook dies on SIGKILL / SIGTERM at once
#   scope       bind-only / routed-only send only their own kind of path
#   never       rootfs paths, and a rename onto a free name, are never asked
#
# Usage: tests/regress/regress_delete_guard.sh [-i <ish>] -t <rootfs tarball>
#   The tarball (e.g. an alpine minirootfs) is fakefsified into a temp dir.
#   Needs sqlite3 on the host.
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
W=$(mktemp -d -t regress_delete_guard)
trap 'rm -rf "$W"' EXIT
BASE=$W/base
"$FAKEFSIFY" "$TARBALL" "$BASE" || exit 1
"$ISH" -f "$BASE" /bin/mkdir -p /mnt/b /mnt/r 2>/dev/null

pass=0; fail=0
check() { if [ "$1" = "$2" ]; then pass=$((pass+1)); echo "PASS $3"; else fail=$((fail+1)); echo "FAIL $3 (got '$1', want '$2')"; fi; }

# A fresh world: rootfs copy R, bind target H (and H2), routed target A, log L.
n=0
fresh() {
    n=$((n+1)); D=$W/w$n; mkdir -p "$D"
    cp -R "$BASE" "$D/rootfs"; R=$D/rootfs
    H=$D/host; H2=$D/host2; A=$D/sess; L=$D/guard.log
    mkdir -p "$H/dir/sub" "$H2/dir/sub" "$A/dir"
    for d in "$H" "$H2" "$A"; do
        echo one > "$d/f1"; echo two > "$d/f2"; echo three > "$d/dir/f3"
    done
    echo four > "$H/dir/sub/f4"; echo four > "$H2/dir/sub/f4"; mkdir -p "$H/empty" "$A/empty"
    : > "$L"
}
# g <guard mode or -> <scope or -> <extra env...> -- <guest argv...>
g() {
    local mode=$1 scope=$2; shift 2
    local envs=()
    while [ "$1" != "--" ]; do envs+=("$1"); shift; done; shift
    [ "$mode" != "-" ] && envs+=("ISH_TEST_GUARD=$mode" "ISH_TEST_GUARD_LOG=$L")
    [ "$scope" != "-" ] && envs+=("ISH_TEST_GUARD_SCOPE=$scope")
    env ISH_TEST_BIND="/mnt/b=$H" ISH_TEST_PATH_HOOK="/mnt/r=$A" ${envs[@]+"${envs[@]}"} "$ISH" -f "$R" "$@" 2>&1 |
        grep -v -E '^(fakefs_bind_mount|create_relative_symlink):'
}
tree() { (cd "$1" && find . | LC_ALL=C sort | while read -r p; do if [ -f "$p" ]; then echo "$p $(cat "$p")"; else echo "$p/"; fi; done); }
rows() { sqlite3 "$R/meta.db" "select cast(path as text) from paths where cast(path as text) like '/mnt/%' order by 1;"; }
orphans() { sqlite3 "$R/meta.db" "select (select count(*) from stats where inode not in (select inode from paths)) + (select count(*) from orphans);"; }
calls() { wc -l < "$L" | tr -d ' '; }

# The same guest script under every "off" spelling and under "allow".
SCRIPT='cd /mnt/b; ls f1 >/dev/null; ls dir/f3 >/dev/null; ls empty >/dev/null
rm f1; echo "rm=$?"; rmdir empty; echo "rmdir=$?"; mv f2 dir/f3; echo "mv=$?"; cat dir/f3
rm -r dir; echo "rmr=$?"; rm nothere 2>&1; echo "enoent=$?"
cd /mnt/r; rm f1; echo "r_rm=$?"; rmdir empty; echo "r_rmdir=$?"; mv f2 dir/f3; echo "r_mv=$?"; cat dir/f3'

fresh; ref_out=$(g - - -- /bin/sh -c "$SCRIPT"); ref_host=$(tree "$H"); ref_sess=$(tree "$A"); ref_rows=$(rows); ref_orph=$(orphans)
check "$(echo "$ref_out" | tr '\n' ' ')" "rm=0 rmdir=0 mv=0 two rmr=0 rm: can't remove 'nothere': No such file or directory enoent=1 r_rm=0 r_rmdir=0 r_mv=0 two " "reference run without a guard"

fresh; out=$(g allow 0 -- /bin/sh -c "$SCRIPT")
check "$out" "$ref_out" "scope 0: same output as without a guard"
check "$(tree "$H")|$(tree "$A")" "$ref_host|$ref_sess" "scope 0: same host files"
check "$(rows)|$(orphans)" "$ref_rows|$ref_orph" "scope 0: same meta.db rows"
check "$(calls)" 0 "scope 0: the hook is never called"

fresh; out=$(g allow - -- /bin/sh -c "$SCRIPT")
check "$out" "$ref_out" "allow: same output as without a guard"
check "$(tree "$H")|$(tree "$A")" "$ref_host|$ref_sess" "allow: same host files"
check "$(rows)|$(orphans)" "$ref_rows|$ref_orph" "allow: same meta.db rows, no orphan"
# bind: rm f1, rmdir empty, mv onto dir/f3, rm -r dir (f3, f4, sub, dir) = 7; routed: rm, rmdir, mv = 3
check "$(calls)" 10 "allow: one call per removal (the missing file is not asked about)"
check "$(awk -F'\t' '$1==3' "$L" | wc -l | tr -d ' ')" 2 "allow: each rename onto an existing file is one REPLACE call"
check "$(awk -F'\t' '$4==1' "$L" | wc -l | tr -d ' ')" 3 "allow: routed paths are flagged routed"
check "$(awk -F'\t' '$1==3 && $4==0 {print $2 "|" $3}' "$L")" "/mnt/b/dir/f3|$(cd "$H" && pwd -P)/dir/f3" "a REPLACE names the destination, guest and host"
check "$(awk -F'\t' '{n=split($6,a,","); if (n < 2 || a[1] != $5) bad++} END {print bad+0}' "$L")" 0 "every call carries its process and at least one ancestor"

fresh; out=$(g wait:300:allow - -- /bin/sh -c "$SCRIPT")
check "$out" "$ref_out" "allow after a wait: same output"
check "$(tree "$H")|$(tree "$A")|$(rows)|$(orphans)" "$ref_host|$ref_sess|$ref_rows|$ref_orph" "allow after a wait: same host files and meta.db"

# ---- deny
DENY='cd /mnt/b; ls f1 f2 dir/f3 empty >/dev/null
rm f1 2>&1; echo "rm=$?"; rmdir empty 2>&1; echo "rmdir=$?"; mv f2 dir/f3 2>&1; echo "mv=$?"; cat f2 dir/f3
rm -r dir 2>/dev/null; echo "rmr=$?"; mv f1 fresh; echo "move=$?"; mv fresh f1
cd /mnt/r; rm f1 2>&1; echo "r_rm=$?"; rmdir empty 2>&1; echo "r_rmdir=$?"; mv f2 dir/f3 2>&1; echo "r_mv=$?"'
for mode in deny wait:300:deny badvalue; do
    fresh; before_host=$(tree "$H"); before_sess=$(tree "$A")
    g - - -- /bin/sh -c 'ls -R /mnt/b >/dev/null' >/dev/null; before_rows=$(rows)
    out=$(g $mode - -- /bin/sh -c "$DENY")
    check "$(echo "$out" | grep -c 'Operation not permitted')" 6 "$mode: every refused removal reports EPERM"
    check "$(echo "$out" | grep -E '^(rm|rmdir|mv|rmr|r_rm|r_rmdir|r_mv)=' | tr '\n' ' ')" "rm=1 rmdir=1 mv=1 rmr=1 r_rm=1 r_rmdir=1 r_mv=1 " "$mode: every refused removal fails"
    check "$(echo "$out" | grep -E '^(two|three)$' | tr '\n' ' ')" "two three " "$mode: a refused rename leaves source and destination as they were"
    check "$(echo "$out" | grep '^move=')" "move=0" "$mode: a rename onto a free name is not a removal"
    check "$(tree "$H")" "$before_host" "$mode: not one host file is gone (bind)"
    check "$(tree "$A")" "$before_sess" "$mode: not one host file is gone (routed)"
    check "$(rows)" "$before_rows" "$mode: the meta.db rows are the ones from before"
    check "$(orphans)" 0 "$mode: no orphan inode"
    check "$(sqlite3 "$R/meta.db" 'pragma integrity_check;')" ok "$mode: meta.db integrity"
    check "$(g - - -- /bin/sh -c 'rm /mnt/b/f1 && rm -r /mnt/b/dir && echo gone')" gone "$mode: with the guard off again the same files delete normally"
done

# ---- only some paths are refused: rm -rf stops at them and keeps going elsewhere
fresh
out=$(g deny - ISH_TEST_GUARD_ONLY=/dir/sub -- /bin/sh -c 'rm -rf /mnt/b/dir 2>/dev/null; echo "rc=$?"')
check "$out" "rc=1" "rm -rf over a refused subtree fails"
check "$(tree "$H/dir")" "./
./sub/
./sub/f4 four" "the refused subtree is whole, its siblings are gone"
check "$(rows | grep -c '/mnt/b/dir/f3')" 0 "the deleted sibling's row is gone"
check "$(orphans)" 0 "no orphan inode after a partly refused rm -rf"

# ---- the mount moves while the hook waits
fresh
out=$(g wait:400:allow - "ISH_TEST_GUARD_REBIND=/mnt/b=$H2" -- /bin/sh -c 'rm /mnt/b/f1 2>&1; echo "rc=$?"')
check "$(echo "$out" | tail -1)" "rc=1" "mount re-pointed during the wait: the delete fails"
check "$(echo "$out" | grep -ci 'i/o error')" 1 "...with EIO"
check "$(cat "$H/f1" 2>/dev/null)|$(cat "$H2/f1" 2>/dev/null)" "one|one" "...and the file is still there at the old place and at the new one"
fresh
out=$(g wait:400:allow - "ISH_TEST_GUARD_REBIND=/mnt/b=$H2" -- /bin/sh -c 'mv /mnt/b/f2 /mnt/b/f1 2>&1; echo "rc=$?"')
check "$(echo "$out" | tail -1)|$(cat "$H/f1")|$(cat "$H/f2")|$(cat "$H2/f1")|$(cat "$H2/f2")" "rc=1|one|two|one|two" "mount re-pointed during the wait: a rename touches neither place"
fresh
out=$(g wait:400:allow - ISH_TEST_GUARD_UNBIND=/mnt/b -- /bin/sh -c 'rm /mnt/b/f1 2>&1; echo "rc=$?"')
check "$(echo "$out" | tail -1)|$(cat "$H/f1" 2>/dev/null)" "rc=1|one" "mount removed during the wait: the delete fails and the file stays"
fresh
out=$(g wait:400:allow - ISH_TEST_GUARD_UNBIND=/mnt/b -- /bin/sh -c 'rmdir /mnt/b/empty 2>&1; echo "rc=$?"')
check "$(echo "$out" | tail -1)|$([ -d "$H/empty" ] && echo there)" "rc=1|there" "mount removed during the wait: rmdir fails and the directory stays"

# ---- a task blocked in the hook can be killed
for sig in KILL TERM; do
    fresh
    start=$(date +%s)
    out=$(g hang - -- /bin/sh -c "rm /mnt/b/f1 & p=\$!; sleep 1; kill -$sig \$p; wait \$p; echo \"status=\$?\"")
    took=$(( $(date +%s) - start ))
    want=137; [ $sig = TERM ] && want=143
    check "$(echo "$out" | tail -1)" "status=$want" "SIG$sig ends a task blocked in the hook"
    check "$([ $took -lt 8 ] && echo quick || echo "slow ${took}s")" quick "...within seconds"
    check "$(cat "$H/f1" 2>/dev/null)|$(orphans)" "one|0" "...the file is still there, no orphan row"
done
fresh
start=$(date +%s)
out=$(g hang - -- /bin/sh -c 'for i in 1 2 3 4; do rm /mnt/b/dir/sub/f4 & done; rm -r /mnt/b/dir & sleep 1; kill -9 -1 2>/dev/null; kill -9 $(jobs -p) 2>/dev/null; wait; echo done')
took=$(( $(date +%s) - start ))
check "$out|$([ $took -lt 10 ] && echo quick || echo "slow ${took}s")" "done|quick" "five tasks blocked in the hook are all killed"
check "$(cat "$H/dir/sub/f4")|$(cat "$H/dir/f3")" "four|three" "...and deleted nothing"

# ---- several tasks at once
fresh
for i in 1 2 3 4 5 6 7 8; do echo x > "$H/p$i"; done
out=$(g wait:200:allow - -- /bin/sh -c 'cd /mnt/b; for i in 1 2 3 4 5 6 7 8; do rm p$i & done; wait; ls p* 2>/dev/null | wc -l')
check "$(echo $out)" 0 "eight parallel deletes, each allowed after a wait, all happen"
check "$(orphans)|$(rows | grep -c '/mnt/b/p')" "0|0" "...and leave no row behind"
fresh
for i in 1 2 3 4 5 6 7 8; do echo x > "$H/p$i"; done
out=$(g wait:200:deny - -- /bin/sh -c 'cd /mnt/b; for i in 1 2 3 4 5 6 7 8; do rm p$i 2>/dev/null & done; wait; ls p* | wc -l')
check "$(echo $out)|$(ls "$H"/p* | wc -l | tr -d ' ')|$(orphans)" "8|8|0" "eight parallel deletes, each refused after a wait, delete nothing"

# ---- scope
fresh; g deny 1 -- /bin/sh -c 'rm /mnt/r/f1; rm /mnt/b/f1 2>/dev/null' >/dev/null
check "$([ -e "$A/f1" ] && echo kept || echo gone)|$([ -e "$H/f1" ] && echo kept || echo gone)|$(calls)" "gone|kept|1" "scope bind-only: routed paths are not asked about"
fresh; g deny 2 -- /bin/sh -c 'rm /mnt/r/f1 2>/dev/null; rm /mnt/b/f1' >/dev/null
check "$([ -e "$A/f1" ] && echo kept || echo gone)|$([ -e "$H/f1" ] && echo kept || echo gone)|$(calls)" "kept|gone|1" "scope routed-only: bind mounts are not asked about"

# ---- never asked
fresh
out=$(g deny - -- /bin/sh -c 'echo a > /tmp/a; echo b > /tmp/b; mkdir /tmp/d; rm /tmp/a; mv /tmp/b /tmp/c; mv /tmp/c /tmp/b; rmdir /tmp/d; ls /tmp | wc -l')
check "$(echo $out)|$(calls)" "1|0" "rootfs paths are never sent to the hook"
fresh
out=$(g deny - -- /bin/sh -c 'mv /mnt/b/f1 /mnt/b/new && mv /mnt/r/f1 /mnt/r/new && mv /mnt/b/dir /mnt/b/dir2 && echo ok')
check "$out|$(calls)" "ok|0" "renames onto free names are never sent to the hook"
fresh
out=$(g deny - -- /bin/sh -c 'mv /mnt/b/f1 /mnt/r/f1 2>/dev/null; echo "rc=$?"; cat /mnt/b/f1')
check "$(echo $out)" "rc=1 one" "a move across kinds (EXDEV, then copy + unlink of the source) is asked about the source and refused"
fresh
out=$(g deny - -- /bin/sh -c 'ln /mnt/r/f1 /mnt/r/link 2>/dev/null && mv /mnt/r/f1 /mnt/r/link; echo "rc=$?"')
check "$out|$(calls)" "rc=0|0" "a rename onto a hard link of itself is not a removal"

# ---- a plain-rootfs prefix: a directory reached through a link in the rootfs
# (how a task with no fs_context reaches the app's /var/minis/workspace)
plain_world() {
    fresh
    "$ISH" -f "$R" /bin/mkdir -p /mnt/w 2>/dev/null
    # Its own directory: $A is also the target of the path hook, and a cwd
    # inside it would read back as /mnt/r.
    # The files are made from the guest, through the link: a plain path is
    # only visible to the guest once meta.db has a row for it.
    PW=$D/plain; mkdir -p "$PW"
    rm -rf "$R/data/mnt/w"; ln -s "$PW" "$R/data/mnt/w"
    # Absolute paths throughout: the CLI has no reverse mapping for a cwd
    # that lies outside the rootfs.
    "$ISH" -f "$R" /bin/sh -c 'mkdir /mnt/w/dir /mnt/w/empty && echo one > /mnt/w/f1 && echo two > /mnt/w/f2 && echo three > /mnt/w/dir/f3' 2>/dev/null
    mkdir -p "$D/other/dir"; echo one > "$D/other/f1"; echo two > "$D/other/f2"; echo three > "$D/other/dir/f3"
}
PL='rm /mnt/w/f1 2>&1; echo "rm=$?"; rmdir /mnt/w/empty 2>&1; echo "rmdir=$?"; mv /mnt/w/f2 /mnt/w/dir/f3 2>&1; echo "mv=$?"; mv /mnt/w/dir/f3 /mnt/w/dir/new; echo "move=$?"; cat /mnt/w/dir/new'
plain_world; ref=$(g - - -- /bin/sh -c "$PL"); ref_tree=$(tree "$PW")
check "$(echo $ref)" "rm=0 rmdir=0 mv=0 move=0 two" "plain prefix: reference run without a guard"
plain_world; out=$(g deny - -- /bin/sh -c "$PL")
check "$out|$(tree "$PW")|$(calls)" "$ref|$ref_tree|0" "plain prefix not registered: nothing is asked, same result"
plain_world; out=$(g deny 1 ISH_TEST_GUARD_PLAIN=/mnt/w -- /bin/sh -c "$PL")
check "$out|$(tree "$PW")|$(calls)" "$ref|$ref_tree|0" "plain prefix registered but the routed scope bit clear: nothing is asked"
plain_world; out=$(g allow - ISH_TEST_GUARD_PLAIN=/mnt/w -- /bin/sh -c "$PL")
check "$out|$(tree "$PW")" "$ref|$ref_tree" "plain prefix, allow: same result as without a guard"
check "$(calls)|$(awk -F'\t' '$1==3' "$L" | wc -l | tr -d ' ')" "3|1" "plain prefix, allow: unlink, rmdir and the one rename onto an existing file are asked about; the move onto a free name is not"
check "$(awk -F'\t' '$1==1 {print $2 "|" $3}' "$L")" "/mnt/w/f1|$(cd "$R/data" && pwd -P)/mnt/w/f1" "plain prefix: the host path is the one under the rootfs data directory, link not resolved"
plain_world; before=$(tree "$PW"); out=$(g deny - ISH_TEST_GUARD_PLAIN=/mnt/w -- /bin/sh -c "$PL")
check "$(echo "$out" | grep -c 'Operation not permitted')|$(tree "$PW")" "3|$(echo "$before" | sed 's|dir/f3 three|dir/new three|')" "plain prefix, deny: three refusals, not one file gone (the move onto a free name still happens)"
check "$(orphans)|$(sqlite3 "$R/meta.db" 'pragma integrity_check;')" "0|ok" "plain prefix, deny: no orphan inode, meta.db integrity"
check "$(g - - -- /bin/sh -c 'rm /mnt/w/f1 && echo gone')" gone "plain prefix: with the guard off again the refused file deletes normally"
plain_world; out=$(g deny - ISH_TEST_GUARD_PLAIN=/mnt/w -- /bin/sh -c 'echo x > /mnt/wx; rm /mnt/wx; echo "sibling=$?"; rm -rf /mnt/w/dir; echo "sub=$?"; echo a > /tmp/a; rm /tmp/a; echo "tmp=$?"')
check "$(echo $out | sed 's/rm: [^=]*permitted //g')" "sibling=0 sub=1 tmp=0" "plain prefix: only paths below it are asked about, not a sibling with the same beginning nor the rest of the rootfs"
plain_world
out=$(g wait:400:allow - ISH_TEST_GUARD_PLAIN=/mnt/w "ISH_TEST_GUARD_RELINK=$R/data/mnt/w=$D/other" -- /bin/sh -c 'rm /mnt/w/f1 2>&1; echo "rc=$?"')
check "$(echo "$out" | tail -1)|$(echo "$out" | grep -ci 'i/o error')|$(cat "$PW/f1")|$(cat "$D/other/f1")" "rc=1|1|one|one" "plain prefix, link re-pointed during the wait: EIO, and the file is still there in the old directory and in the new one"
plain_world
out=$(g wait:400:allow - ISH_TEST_GUARD_PLAIN=/mnt/w "ISH_TEST_GUARD_RELINK=$R/data/mnt/w=$D/other" -- /bin/sh -c 'mv /mnt/w/f2 /mnt/w/dir/f3 2>&1; echo "rc=$?"')
check "$(echo "$out" | tail -1)|$(cat "$PW/f2")|$(cat "$PW/dir/f3")|$(cat "$D/other/f2")|$(cat "$D/other/dir/f3")" "rc=1|two|three|two|three" "plain prefix, link re-pointed during the wait: a rename touches neither directory"
plain_world
out=$(g wait:300:allow - ISH_TEST_GUARD_PLAIN=/mnt/w -- /bin/sh -c 'rm /mnt/w/f1; echo "rc=$?"')
check "$out|$([ -e "$PW/f1" ] && echo kept || echo gone)" "rc=0|gone" "plain prefix, allowed after a wait with the link unchanged: the delete happens"
plain_world
start=$(date +%s)
out=$(g hang - ISH_TEST_GUARD_PLAIN=/mnt/w -- /bin/sh -c 'rm /mnt/w/f1 & p=$!; sleep 1; kill -9 $p; wait $p; echo "status=$?"')
check "$(echo "$out" | tail -1)|$(cat "$PW/f1")|$([ $(( $(date +%s) - start )) -lt 8 ] && echo quick)" "status=137|one|quick" "plain prefix: a task blocked in the hook is killed at once and deletes nothing"

echo "$pass passed, $fail failed"
[ $fail -eq 0 ]
