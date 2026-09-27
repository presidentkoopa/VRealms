# The comparison rig: ROTH.C as a measurable oracle

`ROTH_GAME_PORT.md` §4 Step 0 asks for this before anything else, and §5 makes it
the acceptance test for every later step: instrument both sides, same inputs, diff
the logs, and the first differing tick is the bug.

Until this existed, REMAROTH's stages were signed off on **parser counts** —
sector and face totals diffed against a Python pipeline. Those are structurally
blind to whether anything looks or behaves like Realms, which is how "stages 1–8
done" and "the ceilings are wrong" were true at the same time.

**ROTH.C is the reference. Nothing counts as done until it matches.**

---

## What is here

`oraclelog.c` — a ROTH.C mod that prints one line per engine tick: player
position (16.16), angle, sector, health, view pitch, a digest of the DBASE100
story-flag bitmap, and all seven RNG states. It only READS, so it cannot change
what it measures. Every address it reads carries a citation in the source,
including the two specs that agree the position globals are X=0x90a8c,
height=0x90a90, Y=0x90a94 and that the engine's own macro names for them are
misleading.

REMAROTH emits the same header and the same columns, so the two are diffable
directly.

---

## Building ROTH.C on Windows, without admin or Linux

ROTH.C's Makefile cross-compiles from Linux, and MSVC cannot build it at all —
variable-length arrays, AT&T inline asm and a naked ABI-glue function are all
hard blockers, and rewriting the oracle's source to get around them would defeat
the point of it being the reference. The answer is a portable MinGW toolchain.

**Toolchain** (one-time, no installer, no admin):

- winlibs i686 MinGW-w64 GCC, extracted to `E:\DOOMWork\_toolchain\mingw32`
- prebuilt SDL3 MinGW libraries, copied to `roth_c/third_party/sdl3-win32`
  (the layout the Makefile expects: `include/` and `lib/`)
- `E:\DOOMWork\_toolchain\shim` holding **real .exe copies** of the binutils
  under their `i686-w64-mingw32-` prefixed names, plus `python3.exe`

That shim directory is the non-obvious part, and it has to be on `PATH` for any
rebuild. Two reasons it is needed:

1. The winlibs toolchain is **native i686**, not a cross, so its binutils are
   unprefixed (`windres.exe`, `ld.exe`) while `make CROSS=mingw` asks for
   `i686-w64-mingw32-windres` and friends.
2. The build's Python validators invoke `objdump` through `subprocess`, which
   cannot execute a shell-script shim — hence real `.exe` copies rather than
   `#!/bin/sh` wrappers. A `python3` copy is needed for the same reason: only
   `python` is on PATH here.

Doing it this way means **ROTH.C's tree is not modified at all**, which keeps its
authority as the reference intact.

**Build:**

```
export PATH="/e/DOOMWork/_toolchain/shim:/e/DOOMWork/_toolchain/mingw32/bin:$PATH"
cd E:/VRealms/tools/ROTH.C/roth_c
mingw32-make CROSS=mingw -j4
```

Produces `rothc.exe`. Its own link-time self-checks all pass, and they are what
certify it as an oracle: no `call_orig`/trampoline/int3 bridge (it maps zero bytes
of the 1996 binary), the `.arena` geometry pins the fixed low addresses the engine
stores absolute pointers into, and 1,131 override pads validate — which is what
makes the mod API usable.

**Build the mod:**

```
cd E:/DOOMWork/REMAROTH/tools/oraclelog
i686-w64-mingw32-gcc -shared -std=c11 -Wall -Wextra \
    -I E:/VRealms/tools/ROTH.C/sdk/include -o plugin.dll oraclelog.c
```

---

## Running it, and the two path traps

The retail install is left **read-only**. Two mirror directories of junctions do
the work, because the game writes saves and `.TMP` files into its own directory
and mods must live there too:

- `E:\DOOMWork\_oracle` — the game dir. Real copies of `ROTH\`'s loose files,
  directory junctions for `DATA`, `M`, `DIGI`, `MIDI`, `INSTALL`, plus
  `rothc.exe`, `SDL3.dll` and `mods\oraclelog\plugin.dll`.
- `E:\DOOMWork\_croot` — a DOS `C:` root containing a junction `ROTH` →
  `_oracle`.

```
cd E:/DOOMWork/_oracle
ROTH_ORACLE_LOG=roth_study1.csv ./rothc.exe --headless \
    --game-dir E:/DOOMWork/_oracle --c-root E:/DOOMWork/_croot --skip-gdv
```

**Trap 1: the game dir is `ROTH\`, not the install root.** `CONFIG.INI`,
`DBASE100.DAT` and `M\` all live one level down. Pointing at the install root
fails with "CONFIG.INI missing or corrupt".

**Trap 2: `--c-root` is not optional.** `CONFIG.INI` carries
`DestinationPath=C:\ROTH`, so the engine builds absolute DOS paths. Without the
mapping, `--trace` shows the signature: relative opens succeed
(`dbase100.dat -> 4`) while every absolute one fails
(`C:\ROTH\m\ademo.DAS -> -1`), and the game exits with code -1 right after
"Initiating DAS file" with no error message.

`--headless` means no window, which is what a rig wants.

---

## Confirmed on the first run

8,346 ticks logged, and four values cross-check REMAROTH independently:

| value | oracle | REMAROTH |
|---|---|---|
| player start X | 864.0 | 864 (loader report) |
| player start Y | 3840.0 | 3840 (loader report) |
| start angle | 128 / 512 | 128 (facing 180) |
| command RNG seed | `cc61` | `0xcc61` in roth_runtime.cpp |

The RNG seeds also match `GAME_core.md` §7's table exactly (`03f3`, `7e15`,
`04fd`, `7e15`).

## Known gap

The map loads and the position is STUDY1's start, but health and sector stay 0
for every tick: the game sits at the intro/menu and never enters play, because
headless has no input. Static measurements (object angles, flat scales,
projection) do not need play mode. A moving log diff does, and the SDK exposes the
ISR-level input functions (`ROTH_FN_move_input_forward`, `turn_input_left`, …) so
a fixed input script can be driven from the mod and replayed identically on both
sides. That is the next piece.
