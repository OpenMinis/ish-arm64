#!/bin/sh
# Read-only netlink regression. ROOT is a disposable realfs (-r) guest root.
set -eu
if [ "$#" -ne 2 ]; then
    echo "usage: $0 /absolute/path/to/ish /absolute/path/to/realfs-root" >&2
    exit 2
fi
repo=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
ish=$1
root=$2
cc=${CC_GUEST:-aarch64-linux-musl-gcc}
work=$(mktemp -d)
name=regress-netlink-$$
trap 'rm -f "$root/tmp/$name"; rm -rf "$work"' EXIT HUP INT TERM
# CPPFLAGS can supply Linux UAPI headers when using native musl-gcc on Linux.
# Intentional word splitting for conventional compiler flag variables.
$cc ${CPPFLAGS:-} -static -O2 -Wall -o "$work/test" "$repo/tests/regress/regress_netlink.c"
mkdir -p "$root/tmp"
cp "$work/test" "$root/tmp/$name"
# Some CLI builds return zero even when a guest test fails: require the marker.
ISH_NETLINK_STUB=1 "$ish" -r "$root" "/tmp/$name" ish >"$work/output" 2>&1
cat "$work/output"
grep -q '^NETLINK_PASS ' "$work/output"
! grep -q '^FAIL ' "$work/output"
