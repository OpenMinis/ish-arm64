#!/bin/bash
# fakefs paths routed by the path-translate hook (how the app maps
# /var/minis/{workspace,attachments,offloads,browser} to a per-session host
# directory), driven through the CLI's ISH_TEST_PATH_HOOK.
#
#   chmod +x       the host file's bits are the mode; exec must then work
#   utime          lands on the hooked host file, not on whatever the rootfs's
#                  own data/<path> (the app's UI-session symlink) points at
#   fd vs meta.db  a hooked fd's fake inode is a host inode number; a meta.db
#                  row that happens to carry the same number must not leak its
#                  type/mode into fstat (it made a file read as /dev/null)
#
# Usage: tests/regress/regress_hook_paths.sh [-i <ish>] -t <rootfs tarball>
#   The tarball (e.g. an alpine minirootfs) is fakefsified into a temp dir,
#   so no existing rootfs is touched. Needs sqlite3 on the host.
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
W=$(mktemp -d -t regress_hook_paths)
trap 'rm -rf "$W"' EXIT
R=$W/rootfs; A=$W/sessA; B=$W/sessB
mkdir -p "$A" "$B"
"$FAKEFSIFY" "$TARBALL" "$R" || exit 1
g() { "$ISH" -f "$R" "$@" 2>/dev/null; }
gh() { ISH_TEST_PATH_HOOK=/var/minis/workspace=$A "$ISH" -f "$R" "$@" 2>/dev/null; }
g /bin/mkdir -p /var/minis/workspace
# The rootfs's own copy of the path is a symlink to ANOTHER session, as the
# app's file-browser link is whenever the UI shows a different session.
rm -rf "$R/data/var/minis/workspace"; ln -s "$B" "$R/data/var/minis/workspace"
echo hello > "$A/f"; echo decoy > "$B/f"
pass=0; fail=0
check() { if [ "$1" = "$2" ]; then pass=$((pass+1)); echo "PASS $3"; else fail=$((fail+1)); echo "FAIL $3 (got '$1', want '$2')"; fi; }

out=$(gh /bin/sh -c 'printf "#!/bin/sh\necho ran\n" > /var/minis/workspace/x.sh; chmod +x /var/minis/workspace/x.sh; /var/minis/workspace/x.sh')
check "$out" ran "chmod +x on a hooked file, then exec"
check "$(gh /bin/sh -c 'stat -c %a /var/minis/workspace/x.sh')" 755 "stat reports the host mode"

gh /bin/touch -d '2001-02-03 04:05:06' /var/minis/workspace/f
check "$(date -r "$A/f" +%Y)" 2001 "utime reaches the hooked file"
check "$(date -r "$B/f" +%Y)" "$(date +%Y)" "utime leaves the rootfs-side file alone"

# Plant a meta.db row whose inode equals the hooked file's host inode, typed
# as a character device (1:3, /dev/null).
ino=$(stat -f %i "$A/f")
sqlite3 "$R/meta.db" "insert or replace into stats(inode, stat) values ($ino, x'b6210000000000000000000003010000');"
check "$(gh /bin/cat /var/minis/workspace/f)" hello "read a hooked file whose host inode collides with a meta.db row"
check "$(gh /bin/sh -c 'wc -c < /var/minis/workspace/f')" 6 "its size through an fd"

echo "hook paths: $pass passed, $fail failed"
[ "$fail" = 0 ]
