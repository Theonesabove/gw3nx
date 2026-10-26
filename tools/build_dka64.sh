#!/usr/bin/env bash
# Builds devkitA64 (aarch64-none-elf GCC + newlib) + libnx + switch-tools from GitHub/Ubuntu sources,
# for machines where the devkitPro package servers are unreachable.
set -euo pipefail
TC=${TC:-$HOME/gw3nx-toolchain}   # where the toolchain sources are cloned
export DEVKITPRO=/opt/devkitpro
P=$DEVKITPRO/devkitA64
T=aarch64-none-elf
J=2
export PATH=$P/bin:$PATH
cd $TC
log() { echo "=== $(date +%H:%M:%S) $*"; }

if [ ! -d binutils-src ]; then
  log "fetch binutils (ubuntu binutils-source)"
  apt-get download binutils-source >/dev/null && dpkg -x binutils-source_*.deb bsrc
  mkdir binutils-src && tar xf bsrc/usr/src/binutils/binutils-*.tar.* -C binutils-src --strip-components=1
fi
if [ ! -d gcc-src ]; then
  log "fetch gcc 16.1.0"
  GIT_LFS_SKIP_SMUDGE=1 git clone -q --depth 1 -b releases/gcc-16.1.0 https://github.com/gcc-mirror/gcc gcc-src
  (cd gcc-src && patch -p1 -s < $TC/buildscripts/patches/gcc-16.1.0-1.patch) || log "gcc patch had issues"
fi
if [ ! -d newlib-src ]; then
  log "fetch newlib (devkitPro branch)"
  git clone -q --depth 1 -b devkitPro https://github.com/devkitPro/newlib newlib-src
fi

mkdir -p build/binutils build/gcc build/newlib
if [ ! -f build/binutils/done ]; then
  log "binutils"
  (cd build/binutils && ../../binutils-src/configure --target=$T --prefix=$P --disable-nls --disable-werror \
      --enable-lto --enable-plugins --enable-poison-system-directories --disable-gdb --disable-sim --disable-gprofng >/dev/null \
   && make -j$J >/dev/null && make install >/dev/null && touch done)
fi
if [ ! -f build/gcc/stage1 ]; then
  log "gcc stage1"
  (cd build/gcc && ../../gcc-src/configure --target=$T --prefix=$P --enable-languages=c,c++,lto \
      --with-gnu-as --with-gnu-ld --with-gcc --enable-cxx-flags='-ffunction-sections' --disable-libstdcxx-verbose \
      --enable-poison-system-directories --enable-threads=posix --disable-win32-registry --disable-nls --disable-debug \
      --disable-libmudflap --disable-libssp --disable-libgomp --disable-libstdcxx-pch --enable-libstdcxx-time=yes \
      --enable-libstdcxx-filesystem-ts --with-newlib=yes --with-native-system-header-dir=/include --with-sysroot=$P/$T \
      --enable-lto --disable-tm-clone-registry --disable-__cxa_atexit \
      --with-march=armv8 --enable-multilib --with-pkgversion="devkitA64" \
      CFLAGS_FOR_TARGET="-O2 -ffunction-sections -fdata-sections" CXXFLAGS_FOR_TARGET="-O2 -ffunction-sections -fdata-sections" >/dev/null \
   && make -j$J all-gcc >/dev/null && make install-gcc >/dev/null && touch stage1)
fi
if [ ! -f build/newlib/done ]; then
  log "newlib"
  (cd build/newlib && CFLAGS_FOR_TARGET="-O2 -ffunction-sections -fdata-sections" \
      ../../newlib-src/configure --target=$T --prefix=$P --disable-newlib-supplied-syscalls --enable-newlib-mb \
      --disable-newlib-wide-orient --enable-newlib-register-fini >/dev/null \
   && make -j$J >/dev/null && make install >/dev/null && touch done)
fi
if [ ! -f build/gcc/stage2 ]; then
  log "gcc stage2 (libgcc, libstdc++)"
  (cd build/gcc && make -j$J all >/dev/null && make install-strip >/dev/null && touch stage2)
fi
# gcc's limits.h was generated before newlib existed: rebuild it so it chains to newlib's (PATH_MAX etc.)
G=$(echo $P/lib/gcc/$T/*/include)
cat gcc-src/gcc/limitx.h gcc-src/gcc/glimits.h gcc-src/gcc/limity.h > $G/limits.h
log "toolchain done: $($P/bin/$T-gcc --version | head -1)"
