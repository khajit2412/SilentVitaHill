#!/bin/sh
# Builds vitaGL with the shader cache and installs it over the vitasdk package
# (the stock package is already NO_DEBUG=1; HAVE_SHADER_CACHE is the addition).
# READBACKS_SPEEDHACK and CIRCULAR_POOL_SPEEDHACK (used by CTR) are left out:
# the framebuffer capture relies on glReadPixels finishing the scene, and the
# scratch vertex uploads can stay in flight for more than one frame here.
set -e
: "${VITASDK:=/usr/local/vitasdk}"
export PATH="$VITASDK/bin:$PATH"
WORK="${1:-/tmp/vitagl-build}"
REV=464876a
[ -d "$WORK/.git" ] || git clone https://github.com/Rinnegatamante/vitaGL "$WORK"
cd "$WORK"
git fetch -q origin && git checkout -q "$REV"
make clean >/dev/null 2>&1 || true
make -j"$(nproc)" NO_DEBUG=1 HAVE_SHADER_CACHE=1
LIB="$VITASDK/arm-vita-eabi/lib/libvitaGL.a"
[ -f "$LIB.stock" ] || cp "$LIB" "$LIB.stock"
cp libvitaGL.a "$LIB"
cp source/vitaGL.h "$VITASDK/arm-vita-eabi/include/"
echo "installed $(git rev-parse --short HEAD) with HAVE_SHADER_CACHE"
