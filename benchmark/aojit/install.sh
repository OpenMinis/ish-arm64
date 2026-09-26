#!/bin/sh
# Copy the AOJIT suite into a rootfs (Mac CLI) as /tmp/aojit, plus the modules of an
# older rootfs for the family cases, and generate the inputs.
#
# usage: install.sh <ish> <rootfs> [old rootfs]
#   old rootfs  same Alpine branch at older package versions (e.g. alpine-arm64-321-old):
#               its libz, libpython and busybox go to /tmp/aojit/oldlib
# then:   <ish> -r <rootfs> python3 /tmp/aojit/run_cases.py [--tier all] ...
set -e
[ $# -ge 2 ] || { sed -n '2,9p' "$0"; exit 1; }
ish=$1 rootfs=$2 old=$3
here=$(cd "$(dirname "$0")" && pwd)
dst=$rootfs/tmp/aojit
echo "📦 suite -> $dst"
mkdir -p "$dst"
cp -R "$here/guest/." "$dst/"
cp "$here/images.json" "$dst/"
if [ -n "$old" ]; then
    echo "🧬 older modules from $old -> /tmp/aojit/oldlib"
    mkdir -p "$dst/oldlib"
    cp -L "$old/usr/lib/libz.so.1" "$old/usr/lib/libpython3.12.so.1.0" "$old/bin/busybox" "$dst/oldlib/"
fi
"$ish" -r "$rootfs" /bin/sh /tmp/aojit/setup.sh
