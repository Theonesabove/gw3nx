#!/usr/bin/env bash
# After build_dka64.sh: devkitPro scripts/rules, general-tools, switch-tools, libnx,
# libdrm_nouveau, Mesa (EGL + GLESv2 on nouveau) and SDL2 for Switch.
set -euo pipefail
TC=${TC:-$HOME/gw3nx-toolchain}   # where the toolchain sources are cloned
export DEVKITPRO=/opt/devkitpro
export DEVKITA64=$DEVKITPRO/devkitA64
export PATH=$DEVKITA64/bin:$DEVKITPRO/tools/bin:$DEVKITPRO/portlibs/switch/bin:$PATH
J=2
log() { echo "=== $(date +%H:%M:%S) $*"; }
stamp() { [ -f $TC/.done-$1 ]; }
done_() { touch $TC/.done-$1; }

if ! stamp scripts; then
  log "devkitPro scripts + rules"
  cp $TC/pacman-packages/dkp-toolchain-vars/{devkita64.sh,switchvars.sh,portlibs_prefix.sh} $DEVKITPRO/
  cp $TC/pacman-packages/dkp-meson-scripts/meson-*.sh $DEVKITPRO/
  chmod +x $DEVKITPRO/*.sh
  mkdir -p $DEVKITPRO/portlibs/switch/{bin,lib/pkgconfig,include}
  sed "s|\${DEVKITPRO}|$DEVKITPRO|" $TC/pacman-packages/switch/pkg-config/pkg-config.in > $DEVKITPRO/portlibs/switch/bin/aarch64-none-elf-pkg-config
  chmod +x $DEVKITPRO/portlibs/switch/bin/aarch64-none-elf-pkg-config
  make -C $TC/devkita64-rules install >/dev/null
  done_ scripts
fi
if ! stamp tools; then
  log "general-tools + switch-tools"
  (cd $TC/general-tools && ./autogen.sh >/dev/null 2>&1 && ./configure --prefix=$DEVKITPRO/tools >/dev/null && make -j$J >/dev/null && make install >/dev/null)
  (cd $TC/switch-tools && ./autogen.sh >/dev/null 2>&1 && ./configure --prefix=$DEVKITPRO/tools >/dev/null && make -j$J >/dev/null && make install >/dev/null)
  done_ tools
fi
if ! stamp libnx; then
  log "libnx"
  make -C $TC/libnx/nx -j$J >/dev/null || exit 1; make -C $TC/libnx/nx install >/dev/null || exit 1
  done_ libnx
fi
if ! stamp drm; then
  log "libdrm_nouveau"
  make -C $TC/libdrm_nouveau -j$J >/dev/null || exit 1; make -C $TC/libdrm_nouveau install PORTLIBS=$DEVKITPRO/portlibs/switch >/dev/null || exit 1
  done_ drm
fi
if ! stamp mesa; then
  log "mesa"
  cd $TC/mesa
  patch -p1 -N -s < $TC/pacman-packages/switch/mesa/gl_XML.py.patch || true
  patch -p1 -N -s < $TC/pacman-packages/switch/mesa/glX_XML.py.patch || true
  # newer newlib already has timespec_get
  sed -i 's|^#ifndef HAVE_TIMESPEC_GET$|#if !defined(HAVE_TIMESPEC_GET) \&\& !defined(__SWITCH__)|' include/c11/threads.h
  rm -rf build
  $DEVKITPRO/meson-cross.sh switch ../crossfile.txt build -Db_ndebug=true
  ninja -C build -j$J >/dev/null
  ninja -C build install >/dev/null
  sed -i "s,-lEGL$,-lEGL -ldrm_nouveau," $DEVKITPRO/portlibs/switch/lib/pkgconfig/egl.pc
  done_ mesa
fi
if ! stamp sdl; then
  log "SDL2"
  cd $TC/SDL
  ( source $DEVKITPRO/switchvars.sh
    ./configure --prefix="$PORTLIBS_PREFIX" --host=aarch64-none-elf --disable-shared --enable-static --enable-audio \
      --enable-joystick --disable-power --disable-filesystem --enable-cpuinfo --enable-pthreads --enable-timers --enable-video >/dev/null
    make -j$J >/dev/null && make install >/dev/null ) || exit 1
  done_ sdl
fi
cp -r /usr/include/boost $DEVKITPRO/portlibs/switch/include/   # header-only, for dynarmic
log "portlibs done"
