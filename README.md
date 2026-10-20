# Silent Hill - PS Vita Port (v0.7)

A native PlayStation Vita port of Silent Hill (1999). It is built from the [Silent Hill native PC port](https://github.com/SlickAmogus/silent-hill-decomp), which in turn is built on the [Silent Hill PSX decompilation](https://github.com/shdecompilations/silent-hill-decomp). The game runs as a native Vita executable compiled from decompiled C source. It is **not an emulator** and **not a static recompilation**.

**No game data is included.** You need your own copy of Silent Hill for PlayStation.

## Features

- Native code with OpenGL ES rendering through vitaGL.
- Two graphics modes (Vita Preferences > Graphics Mode):
  - **Quality**: 480x272, 2x MSAA, PGXP (no texture warping or polygon wobble), per-pixel flashlight with real-time soft shadows, 30 fps cap.
  - **Classic**: 720x408, 2x MSAA, original PS1 flashlight, PGXP off, 60 fps cap.
- Widescreen (Hor+), from the PC port.
- Camera styles: Original, Third Person, Over the Shoulder and First Person, with free aim.
- Optional 2D (screen-relative) controls, minimap, crosshair and aim assist.
- Exit to Menu from the in-game Options screen (inventory > Option).
- Saves, config and mods live in `ux0:data/silenthill/`.

## Requirements

- A PS Vita or PS TV on custom firmware (HENkaku / h-encore / Enso) with VitaShell.
- `libshacccg.suprx` in `ur0:data/` or `ur0:data/external/`. The game compiles its shaders on the console the first time they are used. Extract it with [ShaRKBR33D](https://github.com/Rinnegatamante/ShaRKBR33D).
- Your own legally obtained **Silent Hill (USA)** PlayStation disc image as a single `.bin` file. PAL and Japanese images are detected too, but USA is the tested version.

## Installation

1. Install `SilentHillVita.vpk` with VitaShell.
2. Copy your disc image (`.bin`) to `ux0:data/silenthill/gamedata/`. Any file name works; the game finds it by its disc serial.
3. Start the game from the LiveArea. The first boot creates its folders and copies its fonts.

The first time a new effect or area appears, the game compiles a shader, which can cause a short pause. Compiled shaders are cached in `ux0:data/silenthill/shader_cache/`, so this only happens once.

## Controls

| Vita | Action |
|---|---|
| Left stick / D-pad | Move |
| Right stick | Look (Third Person, Over the Shoulder and First Person cameras) |
| Cross | Action / attack / confirm |
| Circle | Flashlight on / off |
| Square | Run |
| Triangle | Map |
| Select | Inventory |
| Start | Pause / skip cutscenes |
| R | Aim |
| L | Look (original camera) |
| **Top-left corner** of the touch screen | Sidestep left (PS1 L1) |
| **Top-right corner** of the touch screen | Sidestep right (PS1 R1) |

The Vita has two shoulder buttons where the PS1 had four, so L and R take the PS1's L2 and R2 (look and aim), and the PS1's L1 and R1 are the two top corners of the front touch screen. Each corner is a quarter of the screen wide and a third of it tall.

### Menus

- Cross confirms; Circle, Triangle or Square goes back.
- **Options** is on the title screen, and in-game in the inventory (Select > Option).
- **Vita Preferences** (Options > Vita Preferences) has three pages: Graphics, Camera and HUD. Tap the **top-left** or **top-right corner** of the touch screen to go to the previous or next page, or use the Next Page / Prev Page rows.
- To change the camera style: Options > Vita Preferences > next page > Camera.
- To return to the title screen during play: Select (inventory) > Option > Exit to Menu. Unsaved progress is lost, just like quitting on the PS1.

### Melee weapons in the free-aim cameras

Holding R (aim) with a melee weapon stops the sprint and puts Harry straight into the aim stance, including with 2D controls and a fully pushed stick.

## Reporting bugs

Logging is off by default. To get a log for a bug report, set `enable_debug_log = 1` in `ux0:data/silenthill/config.cfg` and restart. The log is written to `ux0:data/silenthill/` as `SilentHill_<date>_<time>.log`.

## Known limitations

- Performance: Quality mode mostly holds 30 fps, but heavy outdoor areas can dip into the low 20s.
- Post-processing filters (CRT, scanlines and so on) from the PC port are not available on Vita yet.
- Not available on Vita: RetroAchievements, Discord Rich Presence, DLL mods, the ffmpeg FMV fallback and the Windows launcher.
- Loose-file mods and DuckStation-format texture packs load from `ux0:data/silenthill/gamedata/`, but large HD texture packs are likely to stutter or run out of memory.

## Credits

This port exists because of the work of many people.

- **Konami and Team Silent**: Silent Hill (1999). All game content, characters and artwork, including the LiveArea and bubble artwork, belong to Konami.
- **The [silent-hill-decomp](https://github.com/shdecompilations/silent-hill-decomp) contributors**: the decompilation of the PlayStation game that everything here is built on.
- **Chris Hardin (SlickAmogus) and the contributors of the [Silent Hill PC port](https://github.com/SlickAmogus/silent-hill-decomp)**: the native port, widescreen, the camera styles, PGXP, the per-pixel flashlight and most of the gameplay additions. Almost everything here is their work.
- **[PsyCross](https://github.com/OpenDriver2/PsyCross)** by the REDRIVER2 Project (SoapyMan, InspirationByte): the PsyQ SDK compatibility layer. This port uses its own fork with Vita support: [ShellingBest/PsyCross](https://github.com/ShellingBest/PsyCross).
- **Rinnegatamante**: [vitaGL](https://github.com/Rinnegatamante/vitaGL), vitaShaRK and [ShaRKBR33D](https://github.com/Rinnegatamante/ShaRKBR33D). His [Crash Team Racing: High Octane](https://github.com/Rinnegatamante/Crash-Team-Racing-High-Octane) Vita port was the reference for many of this port's renderer optimizations.
- **The [VitaSDK](https://vitasdk.org) team** and the SDL2 Vita port contributors.
- **PGXP**: the original PGXP by iCatButler; the precise-vertex lookup follows [DuckStation](https://github.com/stenzek/duckstation)'s approach.
- **Fonts**: Oswald and Barlow Semi Condensed, under the SIL Open Font License 1.1.
- **The HENkaku / Enso and VitaShell developers**, for making homebrew on the Vita possible.
- **Vita port**: [ShellingBest](https://github.com/ShellingBest).

## Building

You need VitaSDK and a Linux host (WSL works) with CMake, Ninja, Python 3, `llvm-nm` and `llvm-objcopy`.

```sh
git clone --recursive https://github.com/ShellingBest/SilentHillVita.git
cd SilentHillVita

export VITASDK=/usr/local/vitasdk
export PATH=$VITASDK/bin:$PATH
git clone https://github.com/vitasdk/vdpm && (cd vdpm && ./bootstrap-vitasdk.sh)
yes | vdpm install vitaGL vitaShaRK sdl2 zlib libjpeg-turbo

# vitaGL with the shader cache, as this port expects
sh pc_port/vita_port/build_vitagl.sh

cd pc_port
cmake -S . -B build-vita -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake -DCMAKE_BUILD_TYPE=Release
ninja -C build-vita        # -> build-vita/SilentHillVita.vpk
```

If you cloned without `--recursive`, run `git submodule update --init pc_port/PsyCross`.

Technical notes: [pc_port/vita_port/HANDOFF.md](pc_port/vita_port/HANDOFF.md).

## Licensing

**The port code is GPL-3.0**, as in the upstream PC port. That covers `pc_port/` and every `SH_PC_PORT` addition to the decompiled sources.

**The decompilation is not licensed by this project.** `src/`, `include/`, `configs/`, `asm/`, `lib/` and `rom/` are a decompilation of Konami's Silent Hill by the [silent-hill-decomp](https://github.com/shdecompilations/silent-hill-decomp) contributors. Silent Hill is © Konami.

**PsyCross is MIT-licensed** (© 2020 REDRIVER2 Project). See its `LICENSE`.

Full breakdown: [COPYRIGHT.md](COPYRIGHT.md). This project is not affiliated with or endorsed by Konami.
