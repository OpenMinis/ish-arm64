#!/bin/bash
# Native Windows ARM64 ish (arm64 guest, JIT), cross-built on macOS or Linux,
# and the folder Minis ships as ish\ (zipped).
#
#   tools/cross/windows-aarch64.sh WORKDIR [COMMIT]
#
# Fetches pinned, checksummed inputs into WORKDIR/dl: llvm-mingw (the
# toolchain), the sqlite amalgamation (built into ish.exe), MSYS2 CLANGARM64
# packages (libarchive and its DLLs, for fakefsify.exe) and the Alpine
# aarch64 minirootfs. Builds WORKDIR/build with meson + ninja and leaves
#   WORKDIR/ish-win-arm64-<commit>/   ish.exe, fakefsify.exe, DLLs,
#                                     BUILD_INFO.txt, alpine-minirootfs-*.tar.gz
#   WORKDIR/ish-win-arm64-<commit>.zip
# COMMIT (default: git rev-parse HEAD of this checkout) goes into BUILD_INFO
# and into ish.exe (-Dcommit=, what serve's ready reports).
# Needs: curl, shasum or sha256sum, tar with xz, zstd, unzip, zip, python3,
# meson, ninja, pkg-config.
set -euo pipefail
src=$(cd "$(dirname "$0")/../.." && pwd)
work=${1:?usage: $0 WORKDIR [COMMIT]}
mkdir -p "$work/dl"
work=$(cd "$work" && pwd)
commit=${2:-$(git -C "$src" rev-parse HEAD)}

LLVM_MINGW=llvm-mingw-20260922-ucrt-macos-universal
[ "$(uname -s)" = Linux ] && LLVM_MINGW=llvm-mingw-20260922-ucrt-ubuntu-22.04-$(uname -m)
SQLITE=sqlite-amalgamation-3540100
ALPINE=alpine-minirootfs-3.21.9-aarch64.tar.gz
MSYS2=https://repo.msys2.org/mingw/clangarm64
pkgs=(
    "mingw-w64-clang-aarch64-libarchive-3.8.10-1-any.pkg.tar.zst 28ec399d6c3aa113330df371c16bc8ba184c3b291118fad005ec67a0fee03bf6"
    "mingw-w64-clang-aarch64-zlib-1.3.2-3-any.pkg.tar.zst 20f3bac9ffae6cf4175fdf9c0cd50b2b812665197e7d1e378ace69e1b8734ffa"
    "mingw-w64-clang-aarch64-bzip2-1.0.8-4-any.pkg.tar.zst 0e684014d7ad49bffe845b33e5bd34944497bbfbed411fe485174b823379d664"
    "mingw-w64-clang-aarch64-xz-5.8.4-1-any.pkg.tar.zst 3a9cad5162492a7c2e3dd215cc20e3432f68ffa453e130954cd9fa9bbecf349a"
    "mingw-w64-clang-aarch64-zstd-1.5.7-2-any.pkg.tar.zst f1444d0e664c6e60b324453ca3158ea159b1e880ba57ea229fbdc58e7ba5d9d2"
    "mingw-w64-clang-aarch64-lz4-1.10.0-1-any.pkg.tar.zst a534bc4eb85eb64ecf2d180e5e8d037a13779fe4381d48fc3028b42f37381c0a"
    "mingw-w64-clang-aarch64-libiconv-1.19-1-any.pkg.tar.zst 955499bc5cb73d86ea2850ece9bbdbbfd66b213e272e032f28147ecd42897e21"
    "mingw-w64-clang-aarch64-libb2-0.98.1-3-any.pkg.tar.zst 8e2f1a6a3d8a53d1b6fdcb2d9df090c3c162aa1f36084b4f023d4315c52ea545"
    "mingw-w64-clang-aarch64-libwinpthread-14.0.0.r532.gdbbc8e99d-1-any.pkg.tar.zst c9fc6a4165cfd2563c58fa8bd233f49bba9afdf25060f910d82063da981e9089"
)

sha256() { if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }
fetch() { # url sha256 [name]
    local f="$work/dl/${3:-$(basename "$1")}"
    [ -f "$f" ] || { echo "📥 $1"; curl -fsSL --retry 3 -o "$f.part" "$1" && mv "$f.part" "$f"; }
    [ -z "$2" ] || [ "$(sha256 "$f")" = "$2" ] || { echo "❌ $f: sha256 mismatch"; exit 1; }
}

case $LLVM_MINGW in
    *macos*) llvm_sha=52e5f5a7b131021d0c39a37a38fa380a1da7885cd04bd61afd0cd4ecfb8bc1f3 ;;
    *) llvm_sha= ;;   # not pinned: check it yourself
esac
fetch "https://github.com/mstorsjo/llvm-mingw/releases/download/20260922/$LLVM_MINGW.tar.xz" "$llvm_sha"
fetch "https://www.sqlite.org/2026/$SQLITE.zip" 0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
fetch "https://dl-cdn.alpinelinux.org/alpine/v3.21/releases/aarch64/$ALPINE" a389940ee42579ead202fb91607fb25698a58a3a5b55c15b4603e5d39cfa4492
for p in "${pkgs[@]}"; do fetch "$MSYS2/${p% *}" "${p#* }"; done

# Unpacked trees. pkg-config output is split on spaces, so the MSYS2 tree is
# reached through a link without any.
[ -d "$work/$LLVM_MINGW" ] || tar xf "$work/dl/$LLVM_MINGW.tar.xz" -C "$work"
T=$work/$LLVM_MINGW/bin
sys=$work/sysroot
if [ ! -f "$sys/lib/libsqlite3.a" ]; then
    mkdir -p "$sys/include" "$sys/lib"
    rm -rf "$work/$SQLITE" && unzip -q -o "$work/dl/$SQLITE.zip" -d "$work"
    "$T/aarch64-w64-mingw32-clang" -O2 -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION \
        -c "$work/$SQLITE/sqlite3.c" -o "$work/$SQLITE/sqlite3.o"
    "$T/aarch64-w64-mingw32-ar" rcs "$sys/lib/libsqlite3.a" "$work/$SQLITE/sqlite3.o"
    cp "$work/$SQLITE/sqlite3.h" "$work/$SQLITE/sqlite3ext.h" "$sys/include/"
fi
msys=$work/msys2
if [ ! -d "$msys/clangarm64" ]; then
    mkdir -p "$msys"
    for p in "${pkgs[@]}"; do zstd -q -d -c "$work/dl/${p% *}" | tar x -C "$msys"; done
fi
link=${TMPDIR:-/tmp}/ish-winarm64-msys2-$(printf %s "$msys" | cksum | cut -d" " -f1)
ln -sfn "$msys" "$link"

cross=$work/cross-aarch64.ini
cat > "$cross" <<EOF
[binaries]
c = ['$T/aarch64-w64-mingw32-clang', '-I$sys/include', '-L$sys/lib']
ar = '$T/aarch64-w64-mingw32-ar'
strip = '$T/aarch64-w64-mingw32-strip'
windres = '$T/aarch64-w64-mingw32-windres'
pkg-config = 'pkg-config'

[host_machine]
system = 'windows'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[properties]
sys_root = '$link'
pkg_config_libdir = '$link/clangarm64/lib/pkgconfig'

[project options]
cli = 'enabled'
EOF

opts=(-Dbuildtype=release -Dguest_arch=arm64 -Djit=true -Ddefault_library=static -Dcommit="$commit")
if [ -d "$work/build" ]; then
    meson configure "$work/build" "${opts[@]}" >/dev/null
else
    meson setup "$work/build" "$src" --cross-file "$cross" "${opts[@]}" >/dev/null
fi
ninja -C "$work/build" ish.exe tools/fakefsify.exe

out=$work/ish-win-arm64-$commit
rm -rf "$out" && mkdir -p "$out"
cp "$work/build/ish.exe" "$work/build/tools/fakefsify.exe" "$work/dl/$ALPINE" "$out/"
# fakefsify's DLLs, transitively (ish.exe is static)
need=$("$T/llvm-objdump" -p "$out/fakefsify.exe" | awk '/DLL Name/{print $3}')
while [ -n "$need" ]; do
    next=
    for d in $need; do
        case $d in lib*|zlib*) ;; *) continue ;; esac
        [ -f "$out/$d" ] && continue
        cp "$msys/clangarm64/bin/$d" "$out/" || { echo "❌ $d not in the MSYS2 packages"; exit 1; }
        next="$next $("$T/llvm-objdump" -p "$out/$d" | awk '/DLL Name/{print $3}')"
    done
    need=$next
done
cat > "$out/BUILD_INFO.txt" <<EOF
commit=$commit
guest_arch=arm64
host_arch=aarch64
engine=jit
aot=0
toolchain=$LLVM_MINGW
meson=${opts[*]}
rootfs=$ALPINE
built=$(date -u +%Y-%m-%dT%H:%M:%SZ)
EOF
(cd "$out" && rm -f "../$(basename "$out").zip" && zip -q -r "../$(basename "$out").zip" .)
echo "✅ $out.zip"
