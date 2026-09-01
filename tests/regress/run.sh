#!/bin/bash
# Build and run the fs/wait syscall regression tests inside iSH.
#
# These pin the behaviour of five fixes so a regression fails here rather than
# in a user's shell:
#   #30  O_DIRECTORY flag value + O_NOFOLLOW honored at path resolution
#   #31  fakefs stat/fstat st_dev (nlink, blksize) agreement
#   #29  waitpid not reporting its internal 1s timeout as EINTR
#        waitid si_status/si_code decoding
#        waitid si_uid
#
# Usage:
#   tests/regress/run.sh [-i <ish binary>] [-r <rootfs>] [-f <fakefs path>]
#
#   -i  iSH binary to test. Default: the first of build-arm64-release/ish,
#       build/ish that exists.
#   -r  rootfs to run against, passed to iSH as -r. Default alpine-arm64-321.
#   -f  a file under a fakefs mount point (e.g. /var/minis/shared/doc.txt) to
#       additionally check stat/fstat agreement on. Only meaningful when the
#       rootfs is mounted with -f and a path-translate hook is installed, so it
#       is optional; without it the #31 assertions still run against the rootfs.
#
# The guest test binary is built with a cross-compiler (aarch64-linux-musl-gcc
# by default, override with $CC_GUEST). If none is available the script skips
# rather than failing, since the toolchain is not part of the repo.

set -u
cd "$(dirname "$0")/../.."

ISH=""
ROOTFS="alpine-arm64-321"
FAKEFS_PATH=""
MOUNT_FLAG="-r"

while getopts "i:r:f:m:h" opt; do
    case $opt in
        i) ISH="$OPTARG" ;;
        r) ROOTFS="$OPTARG" ;;
        f) FAKEFS_PATH="$OPTARG" ;;
        m) MOUNT_FLAG="$OPTARG" ;;
        h) sed -n '2,26p' "$0"; exit 0 ;;
        *) exit 2 ;;
    esac
done

if [ -z "$ISH" ]; then
    for cand in build-arm64-release/ish build/ish; do
        if [ -x "$cand" ]; then ISH="$cand"; break; fi
    done
fi
if [ -z "$ISH" ] || [ ! -x "$ISH" ]; then
    echo "no iSH binary found; build one or pass -i <path>" >&2
    exit 2
fi
if [ ! -d "$ROOTFS" ]; then
    echo "rootfs $ROOTFS not found; pass -r <path>" >&2
    exit 2
fi

CC_GUEST="${CC_GUEST:-aarch64-linux-musl-gcc}"
if ! command -v "$CC_GUEST" >/dev/null 2>&1; then
    echo "SKIP: no guest cross-compiler ($CC_GUEST) available." >&2
    echo "      Install one or set CC_GUEST to build tests/regress/regress_syscall.c." >&2
    exit 0
fi

SRC=tests/regress/regress_syscall.c
# With -f the guest's / lives under <rootfs>/data; with -r it is <rootfs> itself.
GUEST_ROOT="$ROOTFS"
if [ "$MOUNT_FLAG" = "-f" ]; then
    GUEST_ROOT="$ROOTFS/data"
fi
BIN="$GUEST_ROOT/tmp/regress_syscall"
mkdir -p "$GUEST_ROOT/tmp"
echo "building $SRC with $CC_GUEST"
if ! "$CC_GUEST" -static -O0 -Wall -o "$BIN" "$SRC"; then
    echo "failed to build $SRC" >&2
    exit 1
fi
cp tests/regress/regress_cp.sh "$GUEST_ROOT/tmp/regress_cp.sh"

# A fakefs keeps its own metadata in meta.db, so files dropped straight into
# data/ are invisible to the guest. Register them by copying through the guest
# itself; the shell script is small enough for this to be cheap, but the test
# binary is not, so -f mode runs only the cp cases and expects the syscall
# binary to be run separately under -r (where every one of these assertions is
# equally valid -- none of them depend on the mount type).
if [ "$MOUNT_FLAG" = "-f" ]; then
    rm -f "$BIN"
    b64_sh=$(base64 < tests/regress/regress_cp.sh | tr -d '\n')
    rm -f "$GUEST_ROOT/tmp/regress_cp.sh"
    if ! "$ISH" "$MOUNT_FLAG" "$ROOTFS" /bin/sh -c \
        "printf %s '$b64_sh' | base64 -d > /tmp/regress_cp.sh"; then
        echo "failed to stage regress_cp.sh into the fakefs" >&2
        exit 1
    fi
fi

status=0

if [ "$MOUNT_FLAG" = "-f" ]; then
    echo
    echo "########## syscall regressions ##########"
    echo "  SKIP  staging a static binary into a fakefs is impractical;"
    echo "        run these with -r <rootfs> (assertions are mount-independent)."
else
    echo
    echo "########## syscall regressions ($ISH $MOUNT_FLAG $ROOTFS) ##########"
    "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_syscall ${FAKEFS_PATH:+"$FAKEFS_PATH"} || status=1

    echo
    echo "########## FMOV immediate decode ##########"
    if "$CC_GUEST" -static -O0 -Wall -o "$GUEST_ROOT/tmp/regress_fmov_imm" \
            tests/regress/regress_fmov_imm.c tests/regress/regress_fmov_imm.S; then
        "$ISH" "$MOUNT_FLAG" "$ROOTFS" /tmp/regress_fmov_imm || status=1
        rm -f "$GUEST_ROOT/tmp/regress_fmov_imm"
    else
        echo "failed to build regress_fmov_imm" >&2
        status=1
    fi
fi

echo
echo "########## cp regressions ##########"
"$ISH" "$MOUNT_FLAG" "$ROOTFS" /bin/sh /tmp/regress_cp.sh ${FAKEFS_PATH:+"$FAKEFS_PATH"} || status=1

rm -f "$BIN" "$GUEST_ROOT/tmp/regress_cp.sh"

echo
if [ "$status" = "0" ]; then
    echo "regress: all cases passed"
else
    echo "regress: FAILURES (see above)"
fi
exit $status
