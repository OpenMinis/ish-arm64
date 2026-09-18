#!/bin/sh
# [T-ish-inode-orphan-pending] Metadata rows (meta.db `stats`) of files that
# lose their last path while an fd is open must still be cleaned at last
# close, and rename-over-existing must not leak the replaced inode's row.
# The guest half only performs the operations; the host runner then counts
# `stats` rows with no `paths` reference, which must be 0 (as it was before).
# Run inside the guest: ish -f <rootfs> /bin/sh < this_file
set -u
D=/tmp/io_$$; mkdir -p $D
# unlink while open, then close -> row cleaned at last close
echo 1 > $D/u; exec 3<$D/u; rm $D/u; cat <&3 >/dev/null; exec 3<&-
# rename over an OPEN file -> replaced inode cleaned at its last close
echo 2 > $D/r1; echo 3 > $D/r2; exec 4<$D/r2; mv $D/r1 $D/r2; cat <&4 >/dev/null; exec 4<&-
# rename over a CLOSED file -> replaced inode cleaned immediately
echo 4 > $D/rc; echo 5 > $D/rd; mv $D/rc $D/rd
# hard link: removing one name must NOT clean the shared inode
echo 6 > $D/h; ln $D/h $D/h2; rm $D/h
[ "$(cat $D/h2)" = 6 ] || echo "FAIL: hardlink target lost after unlinking sibling"
rm $D/h2
# rmdir while a dir fd is open
mkdir $D/dd; exec 5<$D/dd; rmdir $D/dd; exec 5<&-
# unlink while open in ANOTHER process
echo 7 > $D/x; ( exec 6<$D/x; sleep 1; exec 6<&- ) & sleep 0.2; rm $D/x; wait
rm -rf $D
echo "inode_orphan: guest steps done"
