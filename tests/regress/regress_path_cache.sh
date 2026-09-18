#!/bin/sh
# [T-ish-pathcache-global] The process-wide path_normalize cache must never
# hand out a resolution that a namespace mutation has since changed. Every
# case below would pass with NO cache and must still pass with one: each
# mutation is followed immediately by a lookup that the old (TTL-only)
# cache would have answered from the stale entry for up to 100ms.
# Run inside the guest: ish -f <rootfs> /bin/sh < this_file
set -u
pass=0; fail=0
check() { if [ "$1" = "$2" ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL: $3 (got '$1', want '$2')"; fi; }
D=/tmp/pc_$$; mkdir -p $D/a $D/b; echo A > $D/a/f; echo B > $D/b/f

ln -s $D/a $D/L
check "$(cat $D/L/f)" A "symlink resolves"
ln -sfn $D/b $D/L
check "$(cat $D/L/f)" B "symlink retarget visible immediately"
rm $D/L; mkdir $D/L; echo C > $D/L/f
check "$(cat $D/L/f)" C "symlink replaced by a real dir"

mv $D/a $D/c
check "$(cat $D/a/f 2>/dev/null || echo ENOENT)" ENOENT "renamed-away dir gone"
check "$(cat $D/c/f)" A "renamed dir reachable"
ln -s $D/c $D/a
check "$(cat $D/a/f)" A "symlink at the former dir path"

# A resolution cached by one process must not survive a mutation made by another.
( ln -sfn $D/b $D/a )
check "$(cat $D/a/f)" B "cross-process retarget visible"

# Successful resolutions of MISSING intermediates are cached too ("/x/f" -> "/x/f");
# a symlink appearing at /x afterwards must take effect.
cat $D/nx/f >/dev/null 2>&1
ln -s $D/b $D/nx
check "$(cat $D/nx/f)" B "symlink created under a previously-missing intermediate"

# Hard link to a symlink, then unlink the original symlink.
ln -s $D/b $D/s1; ln $D/s1 $D/s2; rm $D/s1
check "$(cat $D/s2/f)" B "hardlinked symlink still resolves after original removed"

rm -rf $D
echo "path_cache: $pass passed, $fail failed"
[ $fail -eq 0 ]
