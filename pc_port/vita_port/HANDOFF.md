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
- Game target links: eboot.bin + SilentHillPC.vpk (vitasdk 2026.08, packages snapshot 20260916).
  glad.h forwards to vitaGL.h; dll_loader / pc_crash / pc_discord_ipc have Vita stubs;
  pc_ra_http.c excluded (RA off); libmathneon added after vitaGL.
- arm-vita-eabi defaults to SHORT ENUMS (sizeof(enum) == 1). Kept for ABI parity with the SDK
  libs; the decomp's STATIC_ASSERT_SIZEOF checks all pass. Watch for enum-typed fields in any
  struct that mirrors file/PSX data.
- vitaGL.h lacks GL_VERTEX_ARRAY_BINDING; the overlays query it to restore the VAO. Defined in
  glad.h so it compiles -- confirm on device that vitaGL answers it (else VAO 0 gets restored).

## Next steps (in order)
1. DONE — links. .bss is 187 MB (arm-vita-eabi-size), see step 4.
2. File paths: point fs_pc / config / saves at `ux0:data/silenthill/` (disc image lives there).
3. DONE — constant-colour blend modes: GXM has no glBlendColor, so BM_ADD_QUATER_SOURCE and
   BM_CONSTANT_ALPHA scale the source in the fragment shader (u_blendSrc). Quarter-add uses 0.5,
   matching the desktop constant alpha. Feedback damp keeps dst alpha instead of lerping it.
4. MEMORY — biggest risk: static maps reserve ~198 MB .bss (demand-zero on iOS, NOT on Vita).
   Measure `arm-vita-eabi-size` on the ELF. If too big: union the per-map .bss into one shared
   region (only one map is live at a time), or load maps with so_util (Rinnegatamante).
   Also tune vglInitExtended ram_threshold and `_newlib_heap_size_user`.
5. Controls: Vita pad/touch mapping (SDL gamecontroller works on Vita; rear touch for L2/R2).
6. On-device: shader compile errors from vitaShaRK (needs libshacccg.suprx on the console),
   performance (try 444 MHz via PSVshell), audio latency.

## Flashlight shadows on Vita
vitaGL has no depth textures, CLAMP_TO_BORDER or glDrawBuffers, so the shadow pre-pass renders into
an RGBA8 colour texture (window depth packed in the fragment shader, unpacked by the receiver's
shFetch) with a D24S8 renderbuffer for the depth test, CLAMP_TO_EDGE, NEAREST. The desktop
glPolygonOffset(1.5, 1.0) is applied to the packed value in the shader. Vita defaults:
flashlight_mode = 1 (Classic+Shadows), shadow_resolution = 512. flashlight_mode 0/2 turns them off.

## Disabled on Vita (optional PC extras)
MSAA, grey-frame PBO debug probe, depth-bits query.

## Never commit
Disc images, extracted game assets, saves.
