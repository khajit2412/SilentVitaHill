# Touch work to carry over from android-port

Everything below is in shared `pc_port/src` or `src/`, so iOS gets it by merging
`android-port`. Nothing here is Android-only; the guards are `SH_IOS ||
__ANDROID__` or no guard at all.

```
git merge fork/android-port
```

## The blocker: the results screen and the end credits

`5c1279bf3`

Two screens end a run, and neither could be left without a pad.

The results/ranking screen is `GameState_Unk15`, a gameState of its own. Every
test in `Tc_Mode` fell past it to "not InGame" and returned `TC_MODE_OFF`, so the
overlay was off entirely. It answers exactly one bind,
`controllerConfig.skip`, in `func_801E342C`. `TC_MODE_SKIP` gives it the same
lone corner button the map and save screens get, with `TB_SKIP` pressing Skip.

The end credits are worse: the scroll runs inside `map6_s02`'s own update during
`InGame`, so no state test can tell it from ordinary gameplay. Touch offered a
movement stick over a rolling credits list and nothing that sends Skip.
`func_801E3970` now stamps `g_PcCreditsFrame = g_TickCount` on every frame it
runs, and `Tc_Mode` treats a stamp within two ticks as the credits being up.
A stamp rather than a flag because there is no single exit point to clear one at.

iOS has both of these today.

## The Gamepad style

`6ff410a5f`

`touch_style` (`e_TouchStyle`): 0 = Context, the existing scheme; 1 = Gamepad, a
fixed PSX pad. Options row is on the Controls page, guarded
`SH_IOS || __ANDROID__`, so it appears on iOS as soon as you merge — **recount
that page first**, it is at 10 rows on Android against a 12 ceiling, and iOS
carries rows Android compiles out.

Adapted from [WhoisMiau0x1's Android fork](https://github.com/WhoisMiau0x1/Silent-Hill-1999-Android-port),
which is a fork of this port's PC branch that took the opposite approach on
purpose. Three of its decisions are worth knowing because they are not obvious:

- **Layout in units of screen HEIGHT, never width.** Height is the stable
  dimension in landscape. An 18:9 phone and a 4:3 tablet differ enormously in
  width but put the thumbs at the same place relative to height, so this keeps
  every control the same physical size and leaves the wide middle — where the
  game actually is — clear. On iOS this matters more than on Android: the aspect
  range across iPhone and iPad is wider.
- **The stick emits the D-PAD bits as well as the analog position.** Gameplay
  reads the stick, but the inventory, the map screen and every "press up or down
  to pick an entry" prompt read the d-pad only. A stick-only pad leaves those
  unnavigable. Our Context style does not need this because its menus are
  pointer-driven through `pc_mouse_cursor`.
- **Raw PSX pad bits, not `controllerConfig` binds.** This is a pad, so Circle
  has to stay Circle. The `TG_*` constants are spelled out rather than reusing
  libetc's `PAD*` names because the raw report is a little-endian `u_short`
  whose two bytes are **swapped** relative to what `PadRead()` returns —
  `PADRup` is `1<<4` while triangle in the raw buffer is `1<<12`. The libetc
  names compile cleanly here and silently turn triangle into d-pad up.

Only the `TC_MODE_GAMEPLAY` layout changes. Every other mode keeps its context
behaviour in both styles, which is what keeps the results screen, the map, the
save screen and the puzzles leavable whichever style is selected.

## Two crash fixes in shared code

Both are `src/`, both affect iOS, and both are invisible on desktop because
neither check is enabled there. Apple clang enables the stack protector, and
FORTIFY is on for release builds, so on iOS they abort exactly as they did on
Android rather than corrupting quietly.

- `5114098c3` — **final boss.** `func_800D952C` does
  `memset(&D_800F2448, 0xA5, 0x1900)`, 6400 bytes, into the 80-entry pool the
  comment in `data_stubs.c` describes. The header declared it as a *single*
  `s_800F3D48`. The storage was never short (the stub is `0x3000`); the
  declaration told the compiler the object was one 88-byte entry, so
  `__builtin_object_size` returned 88 and `__memset_chk` aborted. Declared as
  the array it always was.
- `e01660e15` — **sewer.** `map5_s00 func_800CB25C` projects a 5x5 grid, stepping
  `i` by 3 and storing THREE entries per iteration (`gte_stsxy3c` / `gte_stsz3c`
  are the contiguous forms), covering 0..26. The last iteration starts at
  `[4][4]`, element 24 of 25, and writes 24, 25 and 26 — eight bytes past each
  array. PSX let it go because the two arrays sat `0x68` apart and the spill
  landed in the gap; a modern layout puts the canary there. Both arrays now have
  27 entries of backing store behind a `[5]` pointer, so every `sp10[i][j]`
  reads unchanged.

## Why these were findable at all

`64206b223` gave Android a POSIX crash handler; `Sh_InstallCrashFilter` had been
a no-op outside Windows. That mattered twice over: there was no backtrace, and
the log is fully buffered at 64 KB with only a once-per-second periodic flush, so
a crash discarded up to a second of log — the second that explained it. Reports
arrived as a log whose last line was an ordinary room transition plus an abort
message from stderr, which survived only because stderr is unbuffered.

The handler is in the shared `#else /* !_WIN32 */` branch, so iOS picks it up by
merging. It uses `_Unwind_Backtrace` plus `dladdr` rather than
`backtrace()`/`backtrace_symbols()` — glibc extensions Bionic never shipped.
Apple *does* have those, but the unwinder path works on both and needs no
`#if`.

One caveat when reading its output: `dladdr` names the nearest **exported**
symbol, so a static function is reported as whatever exported symbol precedes it.
The sewer crash was logged as `map5_s01_AirScreamer_Update` and was actually in
`map5_s00 func_800CB25C`. To resolve one properly, rebuild the exact commit the
user ran, take the delta between a known symbol's logged offset and its address
in that build (`MainLoop` works well), and apply it to the frame you care about.
