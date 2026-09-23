# PS Vita port — handoff notes

Base: `SlickAmogus/silent-hill-decomp`, branch `android-port` (Sep 22, 2026).
Work branch: `vita-port`. PsyCross (submodule) changes are in `0001-psycross-vita.patch`.

## Build
```sh
export VITASDK=/usr/local/vitasdk   # vitasdk snapshot 2026-09-14 used
# packages: vitaGL vitashark SDL2 zlib libjpeg-turbo (vdpm or vitasdk/packages releases)
cd pc_port
cmake -S . -B build-vita -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake -DCMAKE_BUILD_TYPE=Release
ninja -C build-vita            # -> eboot.bin, SilentHillPC.vpk
```
Host needs llvm-nm + llvm-objcopy (static map symbol prefixing), python3.

## Done
- CMake: VITA detected from VITASDK; treated like ANDROID/IOS (software SPU via SDL audio,
  SH_NO_OPENAL, no RA/ffmpeg/libjpeg desktop features); SH_STATIC_MAPS forced (43 overlays);
  no PIC; vitaGL/vitashark/Sce*_stub link list; SELF + VPK packaging. Title ID SHPC00001 (placeholder).
- `vita_port/vita_sdlgl_shim.{h,c}`: force-included; routes all SDL_GL_* to vitaGL
  (vitasdk SDL2 has no GL context support). SDL keeps input/audio/timers.
- PsyCross: `__vita__` -> RENDERER_OGLES, OGLES_VERSION 2 (GLSL ES 1.00 via vitaShaRK);
  VRAM texture uses GL_RG8 (renderable U8U8 on GXM) instead of ES2 LUMINANCE_ALPHA;
  compat defines for desktop-only paths; EGL typedef clash with vitaGL.h guarded.
- psycross_static: builds clean for arm-vita-eabi.
- Game target: 216/679 objects compiled with 0 errors before sandbox time limit. NOT yet linked.

## Next steps (in order)
1. Finish `ninja -C build-vita`, fix compile/link errors (expect missing stubs, dlopen refs in
   dll_loader.c / pc_plugins.c / map_overlay_loader.c -> compile out for VITA).
2. File paths: point fs_pc / config / saves at `ux0:data/silenthill/` (disc image lives there).
3. KNOWN GAP — blend mode 3 (B + F/4): GXM has no constant blend colour. Currently plain additive
   (too bright). Fix: uniform scaling src colour by 0.25 in the fragment shader for
   BM_ADD_QUATER_SOURCE, blend GL_ONE,GL_ONE. Same idea for BM_CONSTANT_ALPHA (feedback damp).
4. MEMORY — biggest risk: static maps reserve ~198 MB .bss (demand-zero on iOS, NOT on Vita).
   Measure `arm-vita-eabi-size` on the ELF. If too big: union the per-map .bss into one shared
   region (only one map is live at a time), or load maps with so_util (Rinnegatamante).
   Also tune vglInitExtended ram_threshold and `_newlib_heap_size_user`.
5. Controls: Vita pad/touch mapping (SDL gamecontroller works on Vita; rear touch for L2/R2).
6. On-device: shader compile errors from vitaShaRK (needs libshacccg.suprx on the console),
   performance (try 444 MHz via PSVshell), audio latency.

## Disabled on Vita (optional PC extras)
MSAA, flashlight shadow maps, grey-frame PBO debug probe, depth-bits query.

## Never commit
Disc images, extracted game assets, saves.
