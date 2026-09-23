# Silent Hill - PS Vita Port

A PlayStation Vita port of the [Silent Hill native PC port](https://github.com/SlickAmogus/silent-hill-decomp), which is built on the [Silent Hill PSX decompilation](https://github.com/shdecompilations/silent-hill-decomp). The game runs as a native Vita executable compiled from decompiled C source. It is **not an emulator** and **not a static recompilation**.

Rendering goes through [vitaGL](https://github.com/Rinnegatamante/vitaGL) (OpenGL ES 2.0 over sceGxm), with GLSL shaders compiled on the console by vitaShaRK. SDL2 handles input, audio and timers. The hardware abstraction layer is PsyCross, a PsyQ SDK compatibility layer originally written for REDRIVER2. This project uses its own copy with Vita support: [khajit2412/PsyCross](https://github.com/khajit2412/PsyCross) (`vita-port` branch).

## Status

**Work in progress. Not yet tested on a console.**

The Vita build compiles, links and packages into a `.vpk`. It has not been run on real hardware yet, so expect it to fail somewhere on first boot.

Done:
- Vita CMake target, SELF and VPK packaging
- SDL_GL calls routed to vitaGL
- PsyCross ES2 renderer target for vitaGL
- Blend modes that need a constant blend colour (which GXM lacks) done in the fragment shader instead
- Flashlight shadow maps: depth is packed into an RGBA8 texture, 512x512, on by default (`flashlight_mode = 1`)
- All 43 map overlays linked in statically
- Data folder `ux0:data/silenthill/`: config, saves and logs live there, and the bundled fonts and language packs are copied there on first run
- Memory: static data cut from 187 MB to 45 MB, extended memory mode enabled

Next, in order:
1. On-device testing: memory split, shader compilation, performance, audio latency
2. Vita controls: pad mapping, and the rear touchpad for L2/R2

Full technical notes: [pc_port/vita_port/HANDOFF.md](pc_port/vita_port/HANDOFF.md).

Not available on Vita: MSAA, post-processing, RetroAchievements, Discord Rich Presence, DLL mods/plugins, the ffmpeg FMV fallback and the Windows launcher.

## Requirements

- A PS Vita or PS TV running custom firmware (HENkaku/h-encore/Enso) with VitaShell
- `libshacccg.suprx` in `ur0:data/` or `ur0:data/external/`, needed to compile shaders at runtime. Extract it with [ShaRKBR33D](https://github.com/Rinnegatamante/ShaRKBR33D)
- Your own legally obtained Silent Hill PlayStation disc image (`.bin`), copied to `ux0:data/silenthill/gamedata/`. **No game data is included.**

Logs are written to `ux0:data/silenthill/` as `SilentHill_<date>_<time>.log`.

## Building

You need VitaSDK and a Linux host (WSL works) with CMake, Ninja, Python 3, `llvm-nm` and `llvm-objcopy`.

```sh
git clone --recursive -b v0.01 https://github.com/khajit2412/SilentVitaHill.git
cd SilentVitaHill

# VitaSDK
export VITASDK=/usr/local/vitasdk
export PATH=$VITASDK/bin:$PATH
git clone https://github.com/vitasdk/vdpm && (cd vdpm && ./bootstrap-vitasdk.sh)
yes | vdpm install vitaGL vitaShaRK sdl2 zlib libjpeg-turbo

cd pc_port
cmake -S . -B build-vita -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake -DCMAKE_BUILD_TYPE=Release
ninja -C build-vita        # -> build-vita/SilentHillPC.vpk
```

If you cloned without `--recursive`, run `git submodule update --init pc_port/PsyCross`. Both repositories are private for now, so you need access to both.

The build tested here used VitaSDK release 2026.08 (packages snapshot 2026-09-16), GCC 15.2.

## Licensing

**The port code is GPL-3.0**, as in the upstream PC port. That covers `pc_port/` and every `SH_PC_PORT` addition to the decompiled sources.

**The decompilation is not licensed by this project.** `src/`, `include/`, `configs/`, `asm/`, `lib/` and `rom/` are a decompilation of Konami's Silent Hill by the [silent-hill-decomp](https://github.com/shdecompilations/silent-hill-decomp) contributors. Silent Hill is © Konami.

**PsyCross is MIT-licensed** (© 2020 REDRIVER2 Project). See its `LICENSE`.

Full breakdown: [COPYRIGHT.md](COPYRIGHT.md).

## Credits

- The [Silent Hill PC port](https://github.com/SlickAmogus/silent-hill-decomp) by Chris Hardin (SlickAmogus) and contributors. Almost everything here is their work.
- The [silent-hill-decomp](https://github.com/shdecompilations/silent-hill-decomp) project.
- PsyCross by the REDRIVER2 Project (SoapyMan, InspirationByte), and the PC port's own fork of it.
- vitaGL, vitaShaRK and VitaSDK by Rinnegatamante and the VitaSDK team.
