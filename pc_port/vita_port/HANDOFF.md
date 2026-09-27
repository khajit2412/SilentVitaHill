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
- arm-vita-eabi defaults to SHORT ENUMS (sizeof(enum) == 1). That broke the decomp's
  `typedef enum { false, true } bool`: `b = flags & (1 << 25)` stored 0 (inventory ignored the
  d-pad, logged by [INVPAD]). The build now uses -fno-short-enums (4-byte enums as on PSX/PC) and
  -fsigned-char (plain char signed as on MIPS/x86). The two vitasdk structs with enum members the
  port reads (SDL bind type, SDL_Scancode) are masked at their read sites.
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
5. Controls — partial. L/R = L2/R2 (look/aim); L1/R1 = front-touch top corners (vita_port/vita_touch_buttons.c, one
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
   - Busy-spin fix confirmed: swap 36 ms -> ~0.2 ms indoors. Outdoors at 960x544 the GPU was the
     limit (swap 23-47 ms); render_scale is now a Vita DISPLAY MODE (960x544 / 720x408 / 640x368 /
     480x272, scaled by the display hardware) instead of an offscreen target, whose
     glBlitFramebuffer stretch vitaGL did not do. Vita default render_scale = 0.75 (720x408).
   - At 720x408, dense town (~340 draws): ~18.6 fps. GPU (swap) ~20 ms; CPU: OT0 16.8 ms (parse
     5.5 + submit 10.6), World-draw 9.7 ms, Harry/particles/NPC ~5 ms. [SPLITS]: texture+shader
     3.3 ms, draw 4.1 ms (~12 us/draw). Every split re-sent ~25 uniforms + texture binds, so the
     shim now filters redundant glUniform*/glUseProgram/glBindTexture/glActiveTexture/glViewport
     calls and PsyCross caches the projection matrices (UNTESTED on device at commit time).
   - Next: leaner shader variants (opaque, no discard) for the GPU side; World-draw (GTE) on CPU.
   - Open: stale splash in the title screen's left bar (not the pillarbox clear; `dream_blur = 0`
     untested).

## Performance roadmap (from Rinnegatamante's CTR: High Octane, a PsyCross-derived Vita port)
Source: github.com/Rinnegatamante/Crash-Team-Racing-High-Octane (GPL-3.0), platform/native_renderer.c,
native_gpu.c. Its Vita-specific techniques and our status:
- Vertex upload into vitaGL scratch memory (vglAllocFromScratch + vglBufferData): DONE.
- Hardware paletted textures (GL_PALETTE4/8_RGBA8_OES) cached per (tpage, format, clut), rebuilt on
  VRAM writes (per-64x64-tile generations): DONE, GR_VitaPaletteTexture; rear-touch toggles it
  (g_vitaPalCache) for A/B, [PALCACHE] in the log. Split key includes the palette, so draws rise.
- No-discard shader variant for palettes without a transparent entry (PowerVR hidden-surface
  removal is off for any shader containing discard): DONE for cached palette splits (BM_NONE).
- Dedicated GPU thread: the game thread parses the OT into a frame packet (vertices, splits, envs),
  a render thread does BeginScene/draws/EndScene/swap; synchronous GL work is queued as tasks.
  NOT DONE: every GL caller (VRAM upload, feedback capture, offscreen, shadows, modern meshes,
  hires/pool textures, minimap, decals, FMV) must move to that thread or sync with it.
- Draw reordering (CTR native_gpu.c NativeGpu_DrawReorderedSplits): opaque splits carry an order
  depth per vertex (a_orderDepth, gl_Position.z = 1 - order*2/65535) so the depth test reproduces
  painter order and same-state splits are grouped into depth batches; semi-transparent splits go
  through a bbox-overlap dependency scheduler (only overlapping splits keep their order, capped at
  256 nodes per domain) that picks the most state-compatible ready split. NOT DONE: our depth
  buffer carries GTE depth for items vs world, so order depth would change that; the bbox
  scheduler is safe for all splits. Interim: palette splits under g_vitaPalMinArea (PSX px bbox)
  fall back to VRAM sampling and merge. DONE since: windowed version (Vita_ReorderSplits, 32
  groups, bbox-disjoint moves only, dfe=0/modern/capture splits are barriers), reordered
  vertices drawn in place (s_drawVerts). Rear touch cycles: 0 palette off, 1 all+reorder
  (default), 2 area 1024+reorder, 3 area 1024 no reorder. [SUBMIT] splits the submit time.
- Measured 2026-09-26 (town fight): scratch vertex upload took swap from 19 ms to 0.2 ms (the
  wait was vitaGL's GC); palette cache on every split raised draws 145 -> 450-600 and OT0 submit
  to 12-16 ms (CPU-bound again). GPU backlog at swap ~24 ms but swap-after-drain 0.14 ms.
- Dirty-rect VRAM uploads instead of the full 1 MB glTexSubImage2D: not done (uploads are rare).
- Stripped Vita shaders (nearest only, no dither, low precision): partly (no filter, no dither,
  arithmetic decode). vglUseLowPrecision would break the exact CLUT index math in highp.
- Timing thread sleeps to its next deadline (was 5000 wake-ups/s at high priority): DONE.
- Render thread (vita_glcmd.c, generated by gen_glcmd.py): every GL call recorded on the game
  thread and replayed into vitaGL on core 1; queries answered from a recorded-state copy; returns
  that must come from GL wait ([GLTHREAD] waits/frame). config `vita_render_thread = 0` = direct.
- Cores: game thread 0, render thread 1, helpers (timing, CD spooler, vitaGL GC) on core 3 when a
  CoreUnlocker80000H/CapUnlocker plugin allows it (probed at startup, [CORES] in the log), else 2.
  Core 3 is the system core: optional bonus only, never required.
- [GPUDRAIN] every 60th frame measures the GPU backlog at swap: tells GPU-bound from display/GC wait.

## Renderer roadmap: adopt what others do better (agreed 2026-09-27; PGXP is a must-have)
One big change per build, each behind a config switch, tested before the next.
1. Draw order as depth (CTR native_gpu.c/native_renderer.c): per-primitive order counter in the
   vertex, gl_Position.z = 1 - order*2/65535, depth test LEQUAL + write on. PowerVR HSR then shades
   opaque pixels once (today 3-4x overdraw) and opaque batches can be regrouped by state; draw
   opaque -> discard -> translucent (in order). Must coexist with PGXP depth modes.
2. Per-frame packet to the render thread (CTR NativeGpuFramePacket): hand over vertices + split
   list; the render thread runs the whole split loop. Replaces per-GL-call recording on core 0.
3. Slim GrVertex: 68 bytes today vs CTR 22. Pack PGXP/view-space/flags; keep what PGXP needs.
4. Flashlight cookie: projected beam texture in light space (as SH2/SH3 on PS2) instead of per-pixel
   cone/falloff maths; reuses the shadow light coordinates.
5. PGXP storage: direct-mapped arrays for the packet buffers and scratch memory (DuckStation keeps a
   RAM-mirroring shadow; original PsyCross stores an index in the prim) instead of hash tables;
   other addresses keep the hash.
Measure each with the [PHASE]/[SUBMIT]/[GPUDRAIN]/[PERF] lines, PGXP on and off.

## Next optimizations (research, 2026-09-27)
1. Build vitaGL ourselves with CTR's flags: HAVE_SHADER_CACHE=1 NO_DEBUG=1 READBACKS_SPEEDHACK=1
   CIRCULAR_POOL_SPEEDHACK=1, plus vglSetShaderCachePath (no compile hitches). Stock vdpm build today.
2. Pin the SDL audio (software SPU) and "PsyX SPU spatial" threads to the helper core (core 2).
3. Half precision for the flashlight/shadow maths: vitaGL strips mediump/lowp, but Cg half types
   written directly should pass through. vglUseLowPrecision (CTR uses it) is global -- too risky here.
4. Shadow map as a real depth texture via vglTexImageDepthBuffer (DF32M): depth-only pass, no RGBA
   pack/unpack.
5. Check sceGxm parameter-buffer peak (vitaGL debug build); vglSetParamBufferSize(32 MB) if near 16 MB.
6. RAM headroom (heap 160 MB vs CTR 256 MB): resident_textures back on, cache room files in RAM.
7. Optional: vglSwapResolution for dynamic resolution under GPU load.

## Later (after performance is fine-tuned)
- Sharpening upscale: render_scale picks a smaller display mode and the Vita scaler stretches it
  (bilinear, soft). If 640x368 looks too soft, render to a target and draw it to 960x544 with a
  sharpening (FSR1/CAS-style) quad -- glBlitFramebuffer stretch does not work on vitaGL. ~1-2 ms.
  Optionally keep HUD/text at native 960x544 while the 3D world renders small.
- Performance / Quality preset: one options row replacing the separate flashlight + fps settings.
  Quality = Modern + Shadows, locked 30 fps. Performance = Classic flashlight (vertex-lit, the only
  style cheap enough), no shadows, 60 fps. Blocked on 60 fps itself: needs the OT-parse worker thread,
  a NEON GTE, possibly a lower render_scale in Performance, and a check of how game logic ticks at 60.
  Until then players keep the individual options.
- Sharper text: either a hi-res replacement of the two font TIMs (FILE_1ST_FONT16_TIM /
  FILE_TIM_FONT24_TIM, same glyph layout; hires_override.c already special-cases them and point-
  samples) -- needs artwork and loose files/texture packs enabled on Vita -- or routing game text
  through the port's TrueType glyph-atlas renderer (layout/colour work to keep the original look).
- Ramp graphics back up once there is headroom. Turned down for speed on Vita so far:
  resident_textures 1 -> 0 (expanded chunk-texture pool: longer texture draw distance),
  render_scale 1.0 -> 0.65 (960x544 -> 640x368, hardware display scaler upscales for free), PSX dither (not in the ES2 shader path),
  texture filtering compiled out (PSYX_NO_TEXFILTER), flashlight shadow map 512 (PC 2048) and
  its 16-tap PCF, lean no-flashlight shaders. Re-test each against the fps budget.
- Optional CRT look, off by default: RF/composite filter + subtle scanlines as a toggle, tuned for
  the Vita screen. Needs a GLSL ES 1.00 port of the PC post-process pass (PSYX_HAS_POSTPROCESS is 0
  on ES2), the scene rendered to a target first, and scanlines matched to the PSX line count or
  drawn at 960x544 output (720x408 is not a multiple of 240). Pair it with restoring the PSX dither,
  which the ES2 shader path currently skips, so the composite blur has something to blend.

## Known issues
- Glass/blood/muzzle "triangles" (fixed): gte_stsxy3_g3 stored SXY0-2 at the 64-bit PC byte offsets
  16/24/32; the Vita prim header is 8 bytes (P_LEN 2), so the writes hit uv/clut/tpage and left x0-x2
  stale. The garbage tpage also pointed into the framebuffer band, arming the feedback capture (a
  glReadPixels stall every frame: fights at ~11 fps). Any PC-port code that hardcodes the 12-byte
  prim header, or other x64 layouts, is suspect on Vita.
- Framebuffer feedback is live on Vita (GR_StoreFrameBufferPsx, capture via glCopyTexSubImage2D +
  quad downscale). GXM does not clip to the viewport: full-screen passes into a VRAM sub-rect must draw
  an exact NDC quad, never the oversize triangle (its overhang packed smeared capture edges over the
  font rows 496-511 and the sign textures at x 320-400). Scissored glClear on vitaGL FBOs is flipped
  and drawn through a stencil mask, so VRAM rects are zeroed with a viewport quad (GR_VitaZeroRect).
- The capture's glReadPixels on the display forces sceGxmFinish; it only runs while a prim samples the
  display band (dream blur, loading trail). If that ever shows up in gameplay, render the scene
  offscreen and copy on the GPU instead.
- Draw batching (Vita_BatchSplits): untextured prims join the neighbouring VRAM split via a ulo>uhi
  vertex marker, then equal-state splits are joined.
- Air Screamer hover attack (fixed): s_func_800D2E04.field_D70 is [5][2] (and field_D5C [5][2],
  swallowing "unk_D6C"); only rows 0-1 were extracted, so sharedFunc_800D82B8's hitbox radius read
  past the struct -- 0 on x64 by luck, a pointer's high half (negative) on Vita, failing the
  Control_23 reach test forever. Rows 2-4 read from the USA disc (all six AS overlays agree).
  General lesson: any out-of-bounds read the x64 build survives by luck can misbehave on Vita.

## Flashlight shadows on Vita
vitaGL has no depth textures, CLAMP_TO_BORDER or glDrawBuffers, so the shadow pre-pass renders into
an RGBA8 colour texture (window depth packed in the fragment shader, unpacked by the receiver's
shFetch) with a D24S8 renderbuffer for the depth test, CLAMP_TO_EDGE, NEAREST. The desktop
glPolygonOffset(1.5, 1.0) is applied to the packed value in the shader. Vita defaults:
flashlight_mode = 3 (Modern+Shadows), flashlight intensity 1.00 / size 0.70 (tuned on device),
shadow_map_size = 256 (soft blob shadows). flashlight_mode 0/2 turns them off.
Cost control, all Vita-only:
- Beam-only lighting: triangles outside the cone/range (bounding sphere test, GR_VitaBeamClass) draw
  with the lean shader and a flat u_flDim; only beam triangles run the lit program. [BEAM] logs it.
- One lit program per style x shadows (GR_VitaLitSet, built on first use).
- PCF: bilinear 4x4, vectorised per row, skipped when the 4 corner taps agree (thin casters such as
  knobs drop out, which suits the blob look). u_shadowFadeDist is not supported.
- Shadow light is steadied (dead zone + easing, snap on jumps) so breathing sway does not crawl the
  map; the pass is skipped when the caster hash and light matrix match the map already drawn.
- Measured before the corner early-out: Modern without shadows ~27 fps in rooms, Modern+Shadows
  ~18-23 fps (GPU-bound).

## Disabled on Vita (optional PC extras)
MSAA, grey-frame PBO debug probe, depth-bits query.

## Never commit
Disc images, extracted game assets, saves.
