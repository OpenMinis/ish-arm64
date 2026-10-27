#!/bin/bash
# mkios.sh <srcdir> <builddir>: libish, libish_emu and libfakefs for iOS arm64,
# configured as app/xcode-meson.sh does for Release, without debug info and
# with __LINE__/__DATE__/__TIME__ neutral (neutral.h), so two trees can be
# compared object by object (cmp.sh). Build both trees from the SAME <srcdir>
# path one after the other: __FILE__ strings carry it.
set -e
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$1" && pwd); mkdir -p "$2"; bld=$(cd "$2" && pwd)
export SDKROOT=$(xcrun --sdk iphoneos --show-sdk-path)
export IPHONEOS_DEPLOYMENT_TARGET=15.0
cd "$bld"
if [ ! -f build.ninja ]; then
  export CC_FOR_BUILD="env -u SDKROOT -u IPHONEOS_DEPLOYMENT_TARGET xcrun clang"
  export CC="$CC_FOR_BUILD"
  cat > cross.txt <<X
[binaries]
c = 'clang'
ar = 'ar'
[host_machine]
system = 'darwin'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
[built-in options]
c_args = ['-arch', 'arm64', '-g0', '-include', '$here/neutral.h', '-ffile-prefix-map=$src=SRC', '-fdebug-prefix-map=$src=SRC']
[properties]
needs_exe_wrapper = true
X
  meson setup "$src" --cross-file cross.txt -Dguest_arch=arm64 -Djit=true -Djit_emit=false -Dbuildtype=debugoptimized -Db_ndebug=false >/dev/null
fi
ninja libish.a libish_emu.a libfakefs.a 2>&1 | tail -2
