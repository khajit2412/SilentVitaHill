**Title:** [Release] Silent Hill (PS1) native PS Vita port v0.7: decomp-based, widescreen, PGXP, real-time flashlight shadows

---

Silent Hill now runs natively on the PS Vita. This is **not an emulator** and **not a recompilation**: it's built from the Silent Hill PSX decompilation and the native PC port, compiled for the Vita and rendered with vitaGL.

**You need your own copy of Silent Hill (USA) for PS1 as a `.bin` image. No game data is included.**

**Features**

* Two graphics modes (Options > Vita Preferences > Graphics Mode):
  * **Quality**: 480x272, 2x MSAA, PGXP (no texture warping or polygon wobble), per-pixel flashlight with real-time soft shadows, 30 fps
  * **Classic**: 720x408, 2x MSAA, original PS1 flashlight, 60 fps cap
* Widescreen (Hor+)
* Camera styles: Original, Third Person, Over the Shoulder, First Person, with free aim
* Optional 2D (screen-relative) controls, minimap, crosshair, aim assist
* Exit to Menu from the in-game Options

**Installation**

1. You need `libshacccg.suprx` in `ur0:data/` (extract it with ShaRKBR33D). The game compiles its shaders on the console.
2. Install `SilentHillVita.vpk` with VitaShell.
3. Copy your Silent Hill (USA) `.bin` to `ux0:data/silenthill/gamedata/`. Any file name works.
4. Launch. The first time a new effect appears there can be a short pause while its shader compiles; after that it's cached.

**Controls**

* Left stick / D-pad: move. Right stick: look (Third Person, Over the Shoulder and First Person cameras)
* Cross: action / attack / confirm. Circle: flashlight. Square: run. Triangle: map
* Select: inventory (Options is in there too). Start: pause / skip
* R: aim. L: look (original camera)
* **Top-left / top-right corner of the touch screen: sidestep left / right** (the PS1's L1/R1, because the Vita only has two shoulder buttons)
* In Vita Preferences, **tap the top-left / top-right touch corner to switch pages**
* Change camera: Options > Vita Preferences > next page > Camera
* With a melee weapon, holding R stops the sprint and goes straight into the aim stance

**Known limitations**

* Quality mode mostly holds 30 fps; heavy outdoor areas can dip into the low 20s
* No post-processing filters (CRT etc.) on Vita yet
* Bug reports: set `enable_debug_log = 1` in `ux0:data/silenthill/config.cfg`, reproduce, and attach the log from `ux0:data/silenthill/`

**Credits**

* **Konami / Team Silent**: Silent Hill. All game content and artwork belong to Konami.
* **silent-hill-decomp contributors**: the PS1 decompilation everything is built on
* **Chris Hardin (SlickAmogus) and contributors**: the Silent Hill native PC port. Almost all of the game work in this port is theirs.
* **REDRIVER2 Project (SoapyMan, InspirationByte)**: PsyCross
* **Rinnegatamante**: vitaGL, vitaShaRK, ShaRKBR33D, and the Crash Team Racing: High Octane Vita port, which was the reference for many of the renderer optimizations
* **VitaSDK team** and the **SDL2 Vita port** contributors
* **iCatButler** (PGXP) and **DuckStation** (precise-vertex approach)
* **HENkaku / Enso / VitaShell developers**
* Vita port: **khajit2412**

**Download / source:** https://github.com/khajit2412/SilentVitaHill (GPL-3.0)

This project is not affiliated with or endorsed by Konami.
