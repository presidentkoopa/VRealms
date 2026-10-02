# Session handoff — 2026-10-02

Read `HANDOFF_REMAROTH.md` first; it is the guiding document and §10 is the
order of work. This file is only what changed today and what is in flight.

Tree is clean apart from an untracked `doomxr-log.txt` (a run artefact, safe to
delete or ignore). **22 commits are unpushed** — the owner must push; the
sandbox blocks writes to the public repo.

```
ed163a61ea  The two engines DO agree on a pose; the earlier claim was a fade
da92a4de5b  The oracle can save the frame it drew, and that frame can be looked at
41dd772d2d  Doors: the pale face is no longer outstanding, + the mesh that draws through one
09c664cfe4  The door's thickness edges were blank, and the last open door question is closed
```

---

## 1. Doors — the last open question is closed

`HANDOFF_REMAROTH.md` §12 now carries the settled rule. Summary:

A swinging leaf is a **four-sided prism** hosted as a GZDoom polyobject in a
void cell off the map. Only TWO of its four sides were ever skinned; the two
thin edges were blank while carrying artwork in the player's files (166/167 and
162/167), which is **10.4% of every door's surface drawing nothing**. The latch
edge is what faces you at 90° open, which is where the owner spotted it.

**Which surface lands on which quad was the last open point** and three readings
had disagreed. It is settled by reading what each ARGUMENT is used for, not by
matching two four-element lists:

```
_a(ebx, edx):  [out+0x26] = fs:[ fs:[edx+4] ] & 0xfff   <- EXTENT, from edx
               [out+0x0c] = fs:[ fs:[ebx+4] + 2 ]       <- PICTURE, from ebx
_b(ebx):       bx = fs:[ebx+8] FIRST (the SISTER), then both from there
call order:    _a(c3,c2), _a(c1,c0), _b(c3), _b(c1)      doors.c:553-559
```

The extent is the quad's width, so the extent argument names the quad:

| quad | width from | picture from |
|---|---|---|
| c0 hinge thickness | c0 | c1's OWN |
| c2 latch thickness | c2 | c3's OWN |
| c1 broad | c1's sister | c1's SISTER |
| c3 broad | c3's sister | c3's SISTER |

Verified on screen (closed / mid-swing / open) and by the load report:
`doors: leaf side had no artwork 0` across 30 leaves.

**Still open on doors:** swing DIRECTION (rule derived below, not wired);
closing/blocking/re-open never tested.

**The direction rule, derived but NOT wired.** `tick_swinging_doors` branches on
the door record's `+2`: bit 2 = closing, **bit 0 = opening NEGATIVE**, else
positive, clamped to ±0x40 (doors.c:1073-1130). Bit 0 is set by the `swap` test
in `setup_door_swing_geometry` (doors.c:529):

```
swap = (marker == 0xFFFE) ? 0 : (marker == 0xFFFD) ? 1 : (dxB == record[0x12])
```
`marker` = `fs:[hingeEdge+0xc]`; `dxB` = the sector owning c3's sister;
`record[0x12]` = `fs:[face+6]`, the sector the player activated the door FROM
(`spawn_door_instance` passes a FACE, not a sector — see doors.c:745-795).
Wiring it likely needs an engine change: GZDoom's `PODOOR_SWING` takes a fixed
`swingdir`.

## 2. A mesh draws through a closed door — NOT a door bug

`DEMO[4123]`, a mesh at `(-1780, 3720)` in STUDY1, is ~1700 units beyond door 9
and **renders in front of the closed leaf**, and over the swinging one. Found by
a mid-swing control, not by looking for it. It is the same object at the same
screen position in closed/mid/open frames, which first made it look like a door
texture failing to move — a numeric diff of that region (75.8% changed) refuted
that reading.

**The one test that classifies it, not yet run:** does a mesh also draw through
an ORDINARY wall? Pick a camera with solid map geometry between it and a mesh.
If yes, it is a general depth fault in the mesh path; if no, it is specific to
the polyobject leaf.

## 3. The oracle frame comparison — this is what is in flight

The owner chose this over starting the game layer. The handoff called the
missing piece "decoding the oracle framebuffer dump to an image on Windows".
**That piece is built.**

- `tools/rothdiff/rothdiff_plugin.c` now writes `<pose>.frame.pgm` in the PLAIN
  capture path. It only ever wrote a .pgm in the UV path, and every frame that
  path produces is PAINTED, so the real art was never kept. Raw palette indices
  — the palette belongs to the map pack and TICK_ISR context bars engine calls.
- `tools/rothdiff/framepng.cpp` converts that to a PNG using the map pack's own
  palette via the loader's reader. Self-contained PNG writer (stored-deflate).
  It warns when >90% of the frame is index 0, which caught a fade on the first
  run.
- `tools/rothdiff/sidebyside.ps1` builds the pair image.
- `tools/rothdiff/doorgeom.cpp` — door leaf geometry census (`-cam MAP` emits
  ready-to-paste `rothdiff_shot` lines aimed at each door).

**Verified: the two engines AGREE on a pose.** An earlier claim in commit
`da92a4de5b` that they disagreed was wrong — that oracle frame was captured
before the pose pin took hold. Corrected in `ed163a61ea`.

### THE NEXT STEP, which is where I stopped

**Our capture must render at ROTH's frame size and projection.** ROTH draws
640×480 with `FX = 309.77`, `FY = 355.06` — a NON-SQUARE-PIXEL projection,
91.9° across and 68.1° down (`HANDOFF_REMAROTH.md` §5). Our shots came out
3840×2137 at the window's own FOV, so the pair is currently only good for "is it
the same place", not for a pixel comparison.

`rothdiff_shot` ends in `M_ScreenShot`, which grabs the window, so this may need
NO engine change — try `vid_setsize 640 480` in the capture cfg first. Note §5
says the vertical stretch is already applied per map via `r_view_vstretch` in
`hw_entrypoint.cpp`, gated on `!IsVR()`, so check whether it is already correct
at 640×480 before changing anything. **Do NOT use MAPINFO `pixelratio`** — the
VR code reads it to convert headset metres to world units.

### Capture rules learned the hard way today — all three cost a run

1. **Suppress the notify area.** The capture's own `Printf` is drawn across the
   picture. `con_notifytime 0`, `con_midtime 0`, `showmessages 0` — now in
   `tools/rothdiff/captures/pair.cfg`.
2. **Never capture while the owner is in the headset.** The HMD pose is added to
   a camera that is supposed to be pinned; the frame comes back rolled and
   pitched. ALWAYS ask before launching.
3. **`ROTHDIFF_SETTLE` under ~150 catches a fade.** 60 gave a frame that was
   98.8% index 0 and a second frame taken before the pose pin applied — which is
   what produced the false "the engines disagree" conclusion. Use 200.

Plus: the GDV skip and the menu auto-answer live in the **oraclelog** mod, not
rothdiff. `rothc.exe` with rothdiff alone never reaches a map.

### Build notes

- The oracle plugin is built with **32-bit MSVC** (`vcvars32.bat` + `cl /LD`);
  there is no mingw on this machine. The previous mingw build is kept as
  `mods/rothdiff/plugin.dll.mingw`.
- MSVC lives at `C:\Program Files\Microsoft Visual Studio\18\Community` (18, not
  2022 — several recorded recipes name the wrong path).
- PowerShell 5.1 traps that bit twice today: variables are **case-insensitive**,
  so a local `$out` silently clobbers a `[string]$Out` parameter and a loop
  variable `$n` clobbers `$N`; and `$a[$y, $x]` does not parse for a 2-D array.
  `Diff` is an alias for `Compare-Object`.
- `Set-Location` does NOT change .NET's working directory — use absolute paths
  with `[System.IO.File]`.

## 4. Where the plan stands

`HANDOFF_REMAROTH.md` §10, items 1-4:

1. **Sprites** — effectively closed. One real gap: the per-view LATERAL anchor
   (differs across views in 8 of 9 directional entries). The "pillars are too
   tall" premise is dead; scale is exactly 1.0.
2. **Lighting** — done, except the storm glow has NEVER BEEN SEEN on screen, and
   muzzle flash is BLOCKED until the game layer exists.
3. **Doors** — see §1 above.
4. **Level logic / game layer** — 62.7% of command records dispatch. Inventory
   (458 records) and actor spawning (481) are 70% of what is missing. The owner
   explicitly deferred this pending the comparison instrument, and said they
   "will need an unusual amount of convincing" to start it. **Do not re-open
   that argument unprompted.**

## 5. How to work with this owner

See `HANDOFF_REMAROTH.md` §11, and add:

- **Ask decisions as a direct question.** A six-paragraph status that ends in a
  question is not acceptable; use a short question with short options.
- **They are not technical by their own account** and depend on what they see
  and what the agent says. A number that measures the wrong thing corrupts both
  — run the CONTROL before the experiment. Three of this project's steering
  numbers measured the wrong thing.
- **Do not overclaim.** The original engine has been running here for months;
  claiming a "first" for anything about it is wrong and will be called out.
- **Launches disrupt VR.** One approval = one run, unless they grant a window.
