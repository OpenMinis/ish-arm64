#!/bin/sh
# [FIX-261119-W2] A bind whose host folder holds the rootfs (a whole Windows
# user folder: the rootfs is under %LOCALAPPDATA%). Before ish.exe registered
# the rootfs data path (fakefs_set_rootfs_data_path, the guard of ed3c8189),
# every rootfs file's host path mapped back into that bind, so execve failed
# with EACCES and no command ran - the guest shell running this file did not
# even start. With the guard, programs run and the bind is still there.
#
# Run with the rootfs's parent bound, and this file in a bind of its own:
#   ish.exe -f <dir>\rootfs -b /mnt/anc=<dir> -b /mnt/t=<folder with this file> \
#       /bin/sh /mnt/t/regress_bind_rootfs_ancestor.sh
set -u
pass=0; fail=0
check() { if [ "$1" = "$2" ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL: $3 (got '$1', want '$2')"; fi; }

/bin/true; check "$?" 0 "exec /bin/true"
check "$(/bin/sh -c 'echo nested')" nested "a nested shell runs"
check "$(echo hi | /usr/bin/env cat)" hi "a pipeline of two programs"
check "$( [ -s /etc/os-release ] && echo ok )" ok "a rootfs file reads"
check "$(cd /bin && pwd)" /bin "cd in the rootfs keeps its path"
check "$( [ -d /mnt/anc ] && echo ok )" ok "the ancestor bind is there"
echo "bind_rootfs_ancestor: $pass passed, $fail failed"
[ $fail -eq 0 ]
