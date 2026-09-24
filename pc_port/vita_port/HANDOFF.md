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
1. DONE — links.
2. DONE — file paths: main_pc.c creates and chdirs into `ux0:data/silenthill/` (disc image goes in
   `gamedata/` there), creates the mod folders, and stages the VPK's `app0:assets/` (fonts, lang,
   decal, licenses; not the RA symbol maps / unlock sounds) on first run without overwriting.
   Memory cards are created by PsyCross in `gamedata/save/` on first use.
3. DONE — constant-colour blend modes: GXM has no glBlendColor, so BM_ADD_QUATER_SOURCE and
   BM_CONSTANT_ALPHA scale the source in the fragment shader (u_blendSrc). Quarter-add uses 0.5,
   matching the desktop constant alpha. Feedback damp keeps dst alpha instead of lerping it.
4. MEMORY — .bss 187 MB -> 44.6 MB. The static maps were NOT the cause (all 43 together ~1.5 MB);
   it was PsyX_GPU.cpp's PGXP / flashlight view-space hash tables (2^21 entries, 48 + 88 MB). On Vita
   they are 2^19 and calloc'd on first use: s_shadow 12 MB only with use_pgxp=1, s_vshadow 22 MB once
   the per-pixel flashlight runs (on by default). Probe overflow degrades a vertex to untracked.
   param.sfo ATTRIBUTE2=12 (extended memory). `_newlib_heap_size_user` = 160 MB in
   vita_sdlgl_shim.c; vitaGL's RAM pool is the remainder minus ram_threshold (16 MB). Both are
   guesses: tune from the "Vita free memory after GL init" log line on device.
   `-Wl,--defsym,__sce_headroom=0x2000` keeps vita-elf-create's "segment 1 overlaps" away.
5. Controls — partial. L2/R2 = front-touch top corners (vita_port/vita_touch_buttons.c, one
   table; SDL touch-to-mouse is off on Vita). A proper remappable layout is still to do.
6. On-device — boots to gameplay (2026-09-23, ~15-17 fps, CPU 444 MHz, culling on). Findings:
   - vitaGL compiles GLSL inside glLinkProgram (VGL_MODE_POSTPONED) and crashes on a failed
     compile; the reason only appears as [SHARK] lines (shark_install_log_cb in the shim).
   - Its GLSL translator gives each varying a TEXCOORDn and fails past 10: four gte scalars share
     `v_scalars` on Vita. `major` is a Cg keyword. No textureSize/textureLod (u_hiresSize +
     texture2DLod), no gl_VertexID (fb-pack tri from a VBO), modern overlay/FMV shaders mapped to
     ES 1.00 by PsyX_Shader_Preamble (PSYX_FRAGCOLOR_BUILTIN).
   - Log is line-buffered on Vita (no crash handler to flush it); build stamp refreshes every build.
   - vitaGL allocates per glBufferData: the 17 MB orphan per upload cost ~290 ms/frame. Vita now
     uploads exactly the used vertices.
   - SDL_GetWindowSize is the requested size, so Vita config defaults to 960x544.
   - Frame time (2026-09-24, ~15 fps): game logic ~12 ms, OT parse+submit ~9 ms, and a near-constant
     ~36 ms in vglSwapBuffers even at 2-3 draws/frame. [ENDSCENE] / [GLCOUNT] log lines (every 300
     frames) showed no FBO switches, readbacks or uploads, and render_scale made no difference.
     Ruled out: vsync (config vsync now reaches vitaGL, which starts with vblank wait off) and fill.
     Suspect under test: psyX_intr and the main-thread vblank wait both busy-spun, starving the GXM
     display-queue thread on the Vita's three cores; both now yield (200 / 100 us).
   - render_scale had never applied on Vita: GR_InitialiseRender asked GR_SceneAlwaysOffscreen
     before the context existed ("desktop GL"), made a target before vglInit, and that target blocked
     render_scale. Fixed; render_scale is re-applied after the context like Android/iOS.
   - Open: stale splash in the title screen's left bar (not the pillarbox clear; `dream_blur = 0`
     untested).

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
