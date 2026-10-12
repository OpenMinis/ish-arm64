#!/bin/bash
# Make the rootfs the daily cases run in (Windows test machine, MSYS2 bash):
# Alpine minirootfs -> fakefs, the packages the cases use, guest/ copied to
# /tmp/daily and its inputs generated there. Needs the network.
#
#   benchmark/daily/mkrootfs.sh [DEST] [MINIROOTFS.tar.gz]
#
# A separate rootfs, so the cases neither see nor change anyone's working one.
set -e
here=$(cd "$(dirname "$0")" && pwd)
dest=${1:-C:/src/rootfs/daily-x86_64}
tarball=${2:-C:/src/rootfs/alpine-minirootfs-3.21.8-x86_64.tar.gz}
build=${BUILD:-$here/../../build-x86_64-jit}
export MSYS2_ARG_CONV_EXCL='*' MSYS2_ENV_CONV_EXCL='*'
export PATH=/clang64/bin:$PATH   # fakefsify links libarchive dynamically
ish() { "$build/ish.exe" -f "$dest" /usr/bin/env -i HOME=/root PATH=/usr/sbin:/usr/bin:/sbin:/bin LANG=C.UTF-8 "$@"; }

if [ ! -e "$dest/meta.db" ]; then
    echo "📦 fakefsify $tarball -> $dest"
    "$build/tools/fakefsify.exe" "$tarball" "$dest"
fi
echo nameserver 1.1.1.1 | ish /bin/sh -c 'cat > /etc/resolv.conf'
echo "📥 apk add"
ish /bin/sh -c 'apk update -q && apk add -q python3 py3-pip nodejs npm git make gcc musl-dev go curl unzip zip xz bzip2 openssl'
echo "📁 guest files -> /tmp/daily"
tar cf - -C "$here/guest" . | ish /bin/sh -c 'rm -rf /tmp/daily && mkdir -p /tmp/daily && tar xf - -C /tmp/daily'
ish /bin/sh -c 'cd /tmp/daily && sh setup.sh'
echo "✅ $dest"
