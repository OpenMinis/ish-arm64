#!/bin/bash
# Build official EEMBC CoreMark (static aarch64 musl) into <rootfs>/opt/macjit/bin,
# plus a native macOS build for reference.
#   build_coremark.sh <coremark source dir> <rootfs> <macos out dir>
set -euo pipefail
SRC=$1 ROOT=$2 MAC=$3
OUT="$ROOT/opt/macjit/bin"
mkdir -p "$OUT" "$MAC"
cd "$SRC"
FILES="core_list_join.c core_main.c core_matrix.c core_state.c core_util.c posix/core_portme.c"
CC=aarch64-linux-musl-gcc
$CC -O2 -static -Iposix -I. -DFLAGS_STR='"-O2 -static"' -DITERATIONS=0 $FILES -o "$OUT/coremark_st"
$CC -O2 -static -Iposix -I. -DFLAGS_STR='"-O2 -static -pthread"' -DITERATIONS=0 \
    -DMULTITHREAD=4 -DUSE_PTHREAD -pthread $FILES -o "$OUT/coremark_mt4"
clang -O2 -Iposix -I. -DFLAGS_STR='"-O2"' -DITERATIONS=0 $FILES -o "$MAC/coremark_st"
clang -O2 -Iposix -I. -DFLAGS_STR='"-O2 -pthread"' -DITERATIONS=0 -DMULTITHREAD=4 -DUSE_PTHREAD \
    -pthread $FILES -o "$MAC/coremark_mt4"
file "$OUT"/coremark_* "$MAC"/coremark_* | sed 's/,.*//'
