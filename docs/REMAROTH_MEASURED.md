# REMAROTH — what has been measured against the running original

**Consolidated 2026-10-02.** Everything here came from running ROTH.C and reading
its output, not from reading its code. It replaces five dated notes that lived
only in the owner's Claude project (`REMAROTH_ORACLE_RESULTS`,
`REMAROTH_LIGHTING_ORACLE`, `REMAROTH_SKY_MEASURED`,
`REMAROTH_SPRITES_AND_STORM_MEASURED`, `REMAROTH_STATE_2026-10-01`). Where those
differ from this file, this file is right. Two of their lines are now wrong and
must not be followed:

- "leave the sprite sink off" — the sink is ON in the tree (`8bc39fa`); see
  `ROTH_BETTER.md` §10.
- "set `pixelratio` to 1.146" — never; see `HANDOFF_REMAROTH.md` §5. The
  projection is `r_view_vstretch`.

The rig that produced these numbers is `tools/oracle` and ran in a Linux
workspace, not on the Windows machine. The Windows-side equivalent is
`tools/rothdiff` plus the `oraclelog` mod (`HANDOFF_SESSION_2026-10-02.md` §3).

---

## 1. Camera

Fitted from the original's own 640×480 output.

```
FX = 309.77   X0 = 320.48     91.9° across
FY = 355.06   Y0 = 240.54     68.1° down       FY/FX = 1.1462
eye = playerZ + 144
```

**Player angle:** 512 per turn, **counter-clockwise from +Y**.
Facing θ = 90° + angle × 360/512, measured counter-clockwise from +X. ROTH 128
faces −X. `rothmap.cpp` converts the player start this way and is right.

### The two capture tools disagreed on the angle — found and FIXED 2026-10-02

**Fixed** in `RothAngleToDoom` (`src/roth/roth_diff.cpp:86`), which now adds the
angle rather than subtracting it, matching the player start. The compensation
described below is no longer needed; pose lines are passed as written.

**PROVEN on screen, 11:47.** `REMA2_poseC.png` against `ORACLE_poseC.png`: ours
now shows the long hall — the chandelier, the pillars down the left, the blue
carpet, and the recessed door at the far end between its two red sconces. Before
the fix the same pose line drew a panelled wall. The pose A control
(`REMA2_poseA.png`) is unchanged and still matches, so the fix did not disturb
the pose that already worked.

On `ed163a61` ("The two engines DO agree on a pose"): its settle finding is real
and stands — `ROTHDIFF_SETTLE` 60 was capturing mid-fade. But it retracted the
convention claim on the strength of **pose A alone**, which is angle 0, one of
the two angles where the two conventions coincide and the test cannot fail.
Poses B and C were captured in both engines (10:20 and 10:33) and never paired.
Pairing them afterwards from the files already on disk shows the original in the
long hall facing the pentagram door and ours facing a blank panelled wall, which
is what the paragraph below says. The retraction was of the right size for the
evidence it had; it was simply applied to a claim it had not tested.

What was wrong, for the record:

`rothdiff_shot`, `rothdiff_sprites` and `rothdiff_dump` convert with
`RothAngleToDoom` (`src/roth/roth_diff.cpp:86`), which is `90 − a × 360/512`:
the opposite sense. Its comment cites the *object* facing rule, which is a
different convention from the player's. The ROTH.C plugin writes the angle
straight into the original's own player angle.

So the same pose line gives the same view in both engines **only at angle 0 and
256**. For any other angle, `rothdiff_shot` needs `(512 − ROTH angle) % 512`.

Evidence:

- the two conversions, in code (`roth_diff.cpp:88` against `rothmap.cpp`, the
  player start);
- the existing pair captures in `tools/rothdiff/captures/`: `ORACLE_poseA` and
  `REMA_poseA` (angle 0) show the same view; `poseB` (384) and `poseC` (128) show
  opposite directions. The original at (864, 3840, 128) looks down the long hall
  at the pentagram door; ours at the same line looks at a wall;
- every Linux comparison in this file was taken with the conversion applied.

The older pose sets never caught it because they only used angles 0 and 256.

---

## 2. Flats

Rule, in the stored image layout, confirmed on STUDY1 and LRINTH1:

```
col = mx * ( -x / upt + k*shiftX )   mod W
row = my * ( +y / upt - k*shiftY )   mod H
upt = 2^s;  256x256 opaque: 2^(s-1) and k = 1 (otherwise k = 1/2)
mirror bits (sector +0x17: floor 0/1, ceiling 2/3) negate the axis INCLUDING its shift
```

The two signs that `HANDOFF_REMAROTH.md` §5 carries as `[derived]` are confirmed
by this.

Seen: s = 1 and 2; the 256×256-opaque exception on 9 textures; half-texel and
whole-texel shifts; mirrorX with a shift; mirrorY; non-square textures;
mid-platform tops (no mirrors), including shiftY = 254. **Not seen:** s = 0,
s = 3, a translucent 256×256 image.

The rule was right; the hand-off to the engine was not. World textures are
registered a quarter turn round, and flats used them as if they were not. Fixed
by setting the plane angle to 270 and solving against the turned axes
(`FlatToEngine`, `EngineTexel`). Scores, the original's pixels through our own
conversion:

| view | before | after |
|---|---|---|
| STUDY1 start, 141k flat px | 5.7% | 97.8% |
| LRINTH1 mirrorY floor, 69k px | 1.4% | 99.6% |
| LRINTH1 mirrorX floor, 104k px | 1.7% | 99.6% |

---

## 3. Walls

- Horizontal (stored row): `d/2 + shiftX`, `d` the distance from v1, shift in
  whole texels. FLIP_X: `-(d/2 + shiftX)`.
- Vertical (stored column): `(top − z)/2 + shiftY` from the piece's top anchor —
  one-sided: own ceiling; upper: own ceiling; lower: the neighbour's floor;
  see-through mid: the lower of the two ceilings.
- IMAGE_FIT: one copy across the face and one down the piece. HALF_PIXEL is
  ignored under IMAGE_FIT.
- DRAW_FROM_BOTTOM: `W − (z − bottom)/2 − shiftY`. 100% of 248k pixels on SALVAT
  face 14208.

The stored extent equals the face length on 27,696 of 27,704 visible textured
walls across the 44 maps. The 8 exceptions are data damage, not a second rule:
2 faces in RAQUIA4 whose 12-bit extent truncated (restored at load), 6 in
RAQUIA2 from the coordinate wrap (below).

Flag counts, all maps: IMAGE_FIT 13,745 · TRANSPARENT 9,853 · top-anchored
shiftY 5,361 · FLIP_X 5,340 · DRAW_FROM_BOTTOM 483 · HALF_PIXEL 40.

---

## 4. Lighting

32 shade rows from the DAS, row 0 brightest. Per surface, `light` = sector byte
`+0x0b`; 0 means unshaded.

```
bias = 8 + (light − 128)
cap  = 32 − (max(light − 124, 0) >> 2)
```

**Normal sectors:** `row = (Z >> (5 + Ln)) − bias`; ≤ 0 is row 0; capped at
`cap`; above 31 is the fog colour. `Ln` is metadata **`+0x10`**. STUDY1's is 2,
so the shift is 7 — the load report prints `depth >> 7 (lantern >> 5)`.
`ROTH_LIGHTING.md` says 5 for STUDY1; that was the other field.

**Candle ("lantern") sectors** — sector byte `+0x0a` bit 1, the lights-out bit:

```
row = ((Z + 1024 * max(|tx − jx|, |ty − jy|)) >> (5 + Ll)) − bias − E     clamped 1..31
```

`tx`, `ty` are the screen offsets from the view centre over the focal length.
`Ll` is metadata `+0x14`. `E` is metadata `+0x12` (8 in STUDY1), minus 13 when
the viewer stands outside a lantern sector. The cone centre jitters by
`(rng − 16)/256` on each axis, `rng = rng × 0x5e5 + 0x29`, stepped every 7 ticks
of the 70 Hz clock. `ROTH_LIGHTING.md` has none of this mode.

The original evaluates it at polygon vertices after clipping and interpolates
linearly on screen; walls use their four corners with Y clamped to the view.
We do the same (`main.vp`, a `noperspective` varying, with a per-pixel fallback).

**Verified exact on 2,943 of 2,943 logged vertices.**

**Lights out at load.** Every type-`0x02` record runs once from the `0x30998`
init table; an armed one floods its connected group, applies its delta and sets
the lights-out bit. STUDY1 runs three (−15, −15, −10) and darkens 139 sectors.

### How close the picture is

Median shade-row difference, ours minus the original, STUDY1:

| pose | rows |
|---|---|
| N2 | 0 |
| N3 — the study, (−370, −426) angle 81 | −3 |
| L2 | 0 |
| L3 | 0 |
| L1 — a corridor | −9, open; our depth there is ~8% short |

Coordinates for N2, L1, L2 and L3 were not kept. Re-pick them.

### An unexplained difference at the start pose — observed 2026-10-02

STUDY1 (864, 3840) angle 0. The original's frame is
`captures/ORACLE_poseA.png`; ours is a Linux build at 640×480 with neutral
post-processing. Mean grey level of matching regions (x0–x1, y0–y1):

| region | original | ours | ours, Windows 11:47 |
|---|---|---|---|
| centre pillar (240–335, 60–400) | 44.2 | 44.1 | 42.9 |
| far wall left of the pillar (120–225, 60–300) | 13.9 | 13.7 | 14.0 |
| **right-hand wall with the painting (440–620, 60–400)** | **1.7** | **19.1** | **19.2** |

The fourth column is `REMA2_poseA.png`, the Windows build at 640×480 with the
neutral post-processing of §8, read back through the same regions on 2026-10-02.
The original's own three numbers reproduced to the decimal, so the instrument
reads what it did on Linux. **The right-hand wall reproduces at 19.2 against
1.7** — two builds, two platforms, the correct frame size and neutral post. It
is not a capture artefact and it is not the projection. The pillar is 1.2 grey
levels darker than the Linux build measured; unexplained, and small beside the
17.5 it is sitting next to.

The middle of the picture agrees to a fraction of a grey level. The right-hand
wall is near black in the original and lit in ours. It shows in the Windows
build (`captures/PAIR_poseA.png`) and in a separate Linux build, so it is not a
capture artefact. **This is an observation, not a diagnosis.**

In the same Windows frame the door leaf on the left draws pale and bright where
the original shows a dark doorway. Also unexplained.

### Still open in lighting

- the left bay window pane in the study (`HANDOFF_REMAROTH.md` §4);
- corridor L1;
- the two differences above.

---

## 5. Sky

STUDY2, standing outdoors at (−8565, 8394). 7 captures over yaw and pitch,
631,505 sky pixels, each reporting the texel the original drew.

- **Column** `= (2 × rayAngle) & 0xFF`, the ray angle an arctangent of screen x.
  In map terms, θ counter-clockwise from +X: `col = (θ − 90°) × 1024/360 mod 256`.
  Yaw +37 moves it exactly +74 columns. The 256-wide picture spans 90° and
  repeats four times a turn. 99.1–99.4% of pixels within one column.
- **Row** `= clamp(floor(100.27 − 177.53 × tanElev), 0, 145)`, `tanElev` the
  ray's height over its depth **along the view axis**, so the row depends on
  screen y only. 177.53 is FY/2. At pitch 0, row 0 is at screen y = 40.
- **Pitch shears it 1:1**: `pitch × 355/128` pixels. Pitch 60 measured 166,
  predicted 166; pitch 100 measured 276, predicted 277.
- **Both ends clamp and smear.** Row 0 repeats above the picture, row 145 below.
  It never wraps.

Our cylinder, after the fixes of 2026-10-01 (UVs swapped, `CLAMP_X`, the palette
shader taken off the sky texture), at pitch 0 in the centre band: rows 100%
within one; columns 82% within one, **with a constant one-column remainder**.
Off-centre rows drift up to about 5 because a world-locked cylinder uses
horizontal distance where the original uses view-axis depth; that is correct
for VR and a known difference on a flat screen.

Not tested: the sky under lights-out, or with the storm glow.

---

## 6. Props

Both sides are numbers. The original hands its wall driver a screen rectangle
for every billboard; `rothdiff_sprites` prints ours through the camera in §1.
Differences are ours minus the original, in 640×480 pixels; negative means ours
is higher on screen. Measured on the tree as of 2026-10-01 18:50.

| art | depth | top | bottom | verdict |
|---|---|---|---|---|
| DEMO 4208 | 934 | +0.2 | +1.2 | on |
| DEMO 4105, the armour | 900, 936 | −2.7, −2.3 | −1.7, −1.8 | on |
| DEMO 4189 | 735, 588 | −2.4, −2.1 | −0.9, −1.5 | on |
| ADEMO 141 | 428, 301, 212 | +1.1, +1.4, +0.6 | +0.9, +0.8, +1.3 | on |
| ADEMO 230 | 366, 458 | +0.9, +0.9 | +1.0, +1.5 | on |
| ADEMO 232 | 439, 319 | −0.5, −1.6 | +1.8, +3.7 | on |
| DEMO 4102 | 234, 193 | −24.5, −29.3 | −23.0, −28.6 | **was drifting; FIXED 2026-10-02** |
| ADEMO 45, the snake | 184, 358 | −6.9, −2.6 | −7.1, −3.0 | **same cause, FIXED** |
| DEMO 4163 | 108, 130 | — | — | directional; frame 0 is not the view shown |

Widths agree within a pixel on everything that matched.

### Reference rectangles, so the two misses can be re-checked without the oracle

The original's rectangle is `x left–right, y top–bottom`.

| prop | object at | camera | ROTH angle | original | ours then |
|---|---|---|---|---|---|
| DEMO 4102 | (904, 3606) | (864, 3840) | 256 | x 231–304, y **218–482**, depth 234 | y 193.5–459.0 |
| DEMO 4102 | (904, 3606) | (864, 3840) | 320 | x 495–583, y **213–533**, depth 193 | y 183.7–504.4 |
| ADEMO 45 | (−560, −380) | (−370, −426) | 81 | x 181–241, y **442–525**, depth 184 | y 435.1–517.9 |
| ADEMO 45 | (−672, −232) | (−370, −426) | 81 | x 303–333, y **343–386**, depth 358 | y 340.4–383.0 |

`rothdiff_sprites 864 3840 256 out.txt` is the clean check for 4102: its line
`ROTH_DEMO_O04102 904 3606` should read `ytop 218, ybot 482`. Pass the ROTH
angle as written — 320 and 81 for the other two rows. The `(512 − angle) % 512`
compensation these rows once needed is gone with the §1 fix; angle 256 is no
longer a special case, it was only ever the one that needed no compensation.

`tools/rothdiff/sprcheck.py` reads the three dumps and prints the differences
against this table; `captures/sprites.cfg` takes them in one run.

### Both misses were one fault, and it was not placement — 2026-10-02

**Prop Z was not stable after load.** Realms routinely places a prop below its
own floor, through the modifier nibble, so `Z() != floorz`; that makes
`AActor::Tick` run `P_ZMovement` every tic (`p_mobj.cpp:5122`), and
`P_ZMovement` snaps `Z() <= floorz` back up to `floorz` (`:3199`) regardless of
`NOGRAVITY`. Props were placed correctly and then rose out of the floor over the
next few tics. Fixed by `+NOINTERACTION` on `LoaderProp`.

Found by capturing the same props twice in one run: **35 of 75 moved between the
two captures**, by exactly the shifts applied to them (+4, +8, +16). Afterwards,
0 of 75.

| prop | pose | dy top before | after |
|---|---|---|---|
| DEMO 4102 | 256 | −0.2 | −0.2 |
| DEMO 4102 | 320 | −29.3 | +0.1 |
| ADEMO 45 | 81, depth 184 | −6.9 | +0.8 |
| ADEMO 45 | 81, depth 358 | −2.6 | +1.4 |

Worst vertical error over the four rectangles is now 1.4 px.

**The snake was never a separate fault.** Its "about 3.5 units too high" was the
same drift: its true Z is 396 and it had been rising to 400, the +4 of nibble 2.
The "ours then" column above is therefore not a property of a prop at all — the
same prop measured −0.2 px in a run's first capture and −29.3 px in its second.
**A placement error and a drift are indistinguishable in a single frame.** Any
future prop number should say which capture of the run it came from.

---

## 7. The storm phase counter

Logged every tick over 2,876 ticks of STUDY2. The rule is read from the code
(`tick_ambient_render_and_map`, `game_core.c:348`); the log confirms burst
timing and values, not the rule frame by frame.

```
w  = u16[0x85328]            the same generator as the candle flicker
al = (w & 0xff) >> 1 ;  ah = w >> 8
if phase != 0:  sum = phase + al;  phase = sum & 0xff
                if sum <= 0xff: done
                phase = 0
if ah <= 2: phase = 1
```

Once per **rendered frame** in the original. The glow draws when the sector has
flag bit 6 and `(phase & 0x49) != 0`.

Observed: 6 bursts in 41 seconds, each 7 or 14 ticks; lit on 50 of 2,876 ticks,
about 1.7% of the time.

---

## 8. Other findings

- **RAQUIA2 straddles the 16-bit coordinate wrap.** Re-centred at load
  (`RecentreWrappedMap`, by −30720 on y, a multiple of 2048 so flats stay
  aligned). Its longest face drops from 64,320 units to 3,456. No other map
  moves.
- **Some walls are drawn from run-time copies of their texture** (STUDY1: DAS
  763 → 2758, 765 → 2760). The picture checked is identical, so using the file
  entry is fine.
- **STUDY1's three 382-high ceilings** (sectors 482, 486, 488) are drawn at 384
  by the original. Probably a start-up script. Two units.
- **Sprite art is stored rotated, like walls.**
- **Any picture comparison needs neutral post-processing**: `vid_gamma 1`,
  `vid_contrast 1`, `vid_brightness 0`, `vid_saturation 1`, `gl_bloom 0`,
  `gl_tonemap 0`, `gl_ssao 0`, `gl_lens 0`, `gl_fxaa 0`. The fork's defaults
  change every pixel.

  **THAT LIST WAS INCOMPLETE, and the missing three are archived and global.**
  Add `vid_fixgamma 0`, `vid_blackpoint 0`, `vid_whitepoint 0`. `vid_fixgamma`
  is `CVAR_ARCHIVE | CVAR_GLOBALCONFIG` with a default of 0, so a human turning
  the gamma down in the headset writes it into the config and silently darkens
  **every capture afterwards**, in this game and every other the engine runs.
  On 2026-10-02 it sat at −1 and took the whole frame down by a gamma of about
  1.47 — mean 25.3 to 8.9 — which read for an hour as a shader regression,
  survived a full revert of the tree, and was still there with the source back
  at HEAD. A capture is only comparable if these are neutral too.

  **It also corrects the control.** Fully neutralised, the centre pillar at
  pose A reads **44.2 against the original's 44.2**, where the figure recorded
  here as 44.1 and re-measured all day as 42.9 was taken with `vid_fixgamma`
  already off-default. The far wall reads 14.3 against 13.9. The right-hand
  wall is unmoved at 19.6 against 1.7, so that discrepancy is real and is not
  an artefact of this.

---

## 9. The use key and the door leaf — measured 2026-10-02 evening

Six door poses in STUDY1, from `doorgeom -cam STUDY1 48`, replayed by
`captures/usedoor.cfg`. Read `doomxr-log.txt`, not a `logfile` (see §10).

| | before | after |
|---|---|---|
| Lines carrying `ROTH_LINE_SPECIAL` (9000) | 74 | **194** |
| Marked lines within `USERANGE` (64) at a door pose | 2 | **4** |
| Nearest marked line at a door pose | 40 units | **48 units** |
| What the use ray met at the door | line 1669, `special 0` | line 1669, **`special 9000`** |
| Activation event delivered to the hook | discarded | **`0x2` = `SPAC_Use`** |
| Doors that swung | 0 of 6 | **6 of 6** |

194 is 74 trigger walls plus 30 door leaves of four lines each — the leaves were
previously invisible to the trigger layer entirely. `doors reachable 30` and
`door leaf lines 120 registered` in the load report are the loader's own count of
the same thing.

**Line 1669 is a door leaf, poly tag 1.** It was recorded as a line that
"carries no Realms chain" and read as a puzzle; it carries no chain because it
is not a trigger line. Being one-sided and `ML_BLOCKING` with `special 0`, it
stopped the use ray dead (`P_UseTraverse` → `blocked` → `P_LineOpening` range 0
→ "can't use through a wall", `p_map.cpp:6608`), which is why the two marked
lines at 40 units were never reached. Full trace in
`docs/TRIGGERS_the_use_key_problem.md` §2.4.

**`SPAC_*` bits now asked for on a trigger line:** `Use | UseThrough | UseBack |
Impact`, reading `0x442` on the line. Cross, AnyCross and Push were dropped —
`GAME_core.md` §5.2 makes every face-keyed opcode a click or a hit, so face
triggers had been firing on brushing past their wall. Leaves take the use bits
only.

---

## 10. Two instrument facts that cost a run each

- **`logfile <path>` in a capture cfg does not work.** It closes
  `doomxr-log.txt` and sends everything after it nowhere. No cfg that declares
  one — `pair2.cfg`, `lantern.cfg`, `doorskin.cfg`, the `cone_*` set — has ever
  produced the file it names. One run's entire output was lost to this before it
  was noticed. The engine already logs to `doomxr-log.txt` beside the exe, live
  and line by line; read that.

- **A load report left unflushed is not a short load.** `roth_<MAP>.log` is
  buffered, so a run that is killed rather than quitting leaves a ~350-byte stub
  that looks exactly like a map that failed to load one stage in. Check the
  console for `Realms: N objects spawned` before believing the file.

- **The engine exits `0xC0000409` during shutdown**, after
  `rothdiff: all captures done; quitting` and with no fatal error logged.
  `captures/quit_control.cfg` is the control: the same rig and quit path at a
  pose with no door in range, where `ActivateLine` never runs and the use line
  reports `0 within 64`, exits the same way. It happens on the plain screenshot
  path too, with the PNG already written — which is why no earlier lane noticed
  it. Not diagnosed. It does not affect a capture or a measurement, but it means
  **an exit code cannot be used to tell whether a run succeeded.**

---

## 11. The trigger layer, per opcode — measured 2026-10-02 night

`GAME_core.md` §5.2 gives one event per trigger opcode and states that it
"corrects R/ROTH_COMMANDS.md and R/src/roth/roth_runtime.cpp:54-55". That
correction is applied. The counts below are STUDY1, from the load report's new
per-opcode table.

| opcode | records | event (§5.2) | dispatch | fires? |
|---|---|---|---|---|
| `0x18` | 26 | left-click wall face | the use key, via `P_ActivateLine` | **yes** |
| `0x13` | **23** | enter/leave sector | `NotifyPlayerSector`, per tic from `P_PlayerThink` | **yes** |
| `0x19` | 12 | left-click floor / platform top | `roth::UseFlat`, a ray that can hit a flat | **yes, 12 of 12** |
| `0x1a` | 4 | attack or projectile hits a face | `SPAC_Impact`, from a hitscan | **yes, 16 of 16 landed shots** |
| `0x32` | 1 | right-click (examine) a face | none — no examine input exists | no |
| `0x31` | **0** | right-click floor/ceiling | `UseFlat`, but no examine input | n/a in this map |

**"12 sector-keyed" was really 35.** `0x13` matched neither classifier, so all 23
fell through the binding loop in silence — no counter, no warning, nothing in
the report. It is the second-largest trigger category in the map and none of
them had ever fired in any build.

**Confirmed fired:** `0x13` in sectors 26, 42, 51, 108, 174, 193, 409. `0x19` in
all 12 of its records — 221 (six), 320, 322, 409, 413, 414, 415.

**Every `0x19` in STUDY1 carries flags `0x10`**, so bit 0 is clear on all of
them and there is **no platform-top record in this map**. That path is
implemented (§below) and untested.

**Entering sector 174 warps the map to STUDY3** (349 sectors, 1135 lines, 60
objects). Its `0x13` chain carries an `0x3b`, which was implemented and had
simply never been reachable. First Realms scripted map transition this port has
executed from play. Whether the original warps from that spot on that approach
is NOT established — see the facing mask below.

### The facing mask at a record's +0x06 — read out of ROTH.C

`fire_sector_trigger`, `E/raw_commands.c:2876-2878`:

```c
t   = (g_player_angle - 0x40) & 0x1ff
bit = 1 << ((t >> 7) & 7)
if (!(f6 & bit)) { ...fire... }
```

The player's **facing**, not the direction of travel. 512 units to the turn; the
`- 0x40` rotates the boundaries 45° so the four 128-unit quadrants are centred
on the cardinals; and **a SET bit FORBIDS that quadrant**. Most records carry an
all-clear mask, which is why ignoring it looked harmless.

Implemented for the sector-enter path **and nowhere else**. **Measured only in
the negative:** records with masks `0x00` and `0x10` all still fire, so the gate
is not inverted and is not silently blocking the 23. Nothing has yet been
*refused* by it — no record in the sample has any of bits 0-3 set — so the
blocking half is unproven.

### `+0x06` is read three different ways, with two different polarities

This is the trap. The three channels do not share a convention, and assuming
they did put the sector-enter gate into the floor path, where it silently
suppressed a real trigger.

| channel | index from | sense | state |
|---|---|---|---|
| `0x13` enter sector | the **player angle**, `(angle − 0x40) >> 7`, four quadrants | a **set bit BLOCKS** | implemented |
| `0x18`/`0x32` wall face | the **pick**, `dir_mask1[byte[pick+0x1a] & 3]` where `dir_mask1 = {1,4,2,1}` (`:3012`) | a **set bit ALLOWS** | implemented — and it is **not a direction**, see below |
| `0x19`/`0x31` floor | **no direction mask at all** | bit 0 selects the **surface** | implemented |

### `dir_mask1` is NOT a direction mask. It selects a BAND OF THE WALL.

This is the single most misleading name in the trigger layer, and it is ROTH.C's
own: the author called the array `dir_mask` and the index "the approach". The
code says otherwise, and **code beats comments**.

Tracing the index to where it is written:

- the pick record is the deferred-surface descriptor at `0x90a48`;
- its `+0x1a` is address `0x90a62`, which is
  `g_subpass_reflect_param_b + 0x6` (`g_names.h:623`, extent `0xc`);
- `exit_f_writeout` fills that from `byte[g_turn_view_scale_state + 2]`
  (`renderer.c:4500`);
- and that byte is set by the **span builders**, one value per part of the wall:

| value | written by | `dir_mask1` bit | means |
|---|---|---|---|
| 0 | main face span, `renderer.c:8716` | `0x01` | the **mid** texture |
| 1 | **top** edge span, `:8673` | `0x04` | the **upper** section |
| 2 | **bottom** edge span, `:8694` | `0x02` | the **lower** section |
| 3 | mid base+overlay multipass, `:8772` | `0x01` | the mid again |

So `+0x06` on a face record says **which band of the wall the trigger answers
on** — a switch on the mid texture, a panel in the lower section — and has
nothing to do with where the player stands. Values 0 and 3 mapping to the same
bit is the clearest tell: both are the mid texture, drawn in one pass or two.

**The authored data agrees plainly.** STUDY1's 26 `0x18` records carry `0x01`
twenty-four times (mid only), `0x02` once (lower only) and `0x06` once (lower
and upper but *not* the mid). The single `0x32` carries `0x06`. Those are
choices about wall sections.

**Implementing it as a direction gate would have been wrong and silent:** 24 of
26 records would have been restricted to one quadrant of player facing, and
those triggers would simply have stopped working from most directions.

GZDoom classifies the same three bands by the same rule
(`p_trace.cpp:526-529`), so this needs only the hit height — `roth::WallPartBit`.

**It is live but currently has nothing to bite on**, and that is worth stating
rather than discovering later:

- On the **use** path there is no hit height to classify with. `P_UseTraverse`
  builds `optpos` as `{ start.X, start.Y, usething->Z() }` (`p_map.cpp:6525`) —
  the player's **feet** — because the whole path is 2D. Feeding that in would
  classify by where the player stands: in a doorway the feet sit at the back
  floor, so a mid-texture switch reads as a LOWER hit and is refused. So
  `p_spec.cpp` passes `nullptr` and the gate skips itself. Driving it properly
  needs one aimed pick, the same thing the floor probe wanted.
- On the **impact** path the tracer has a real hit point and one site now passes
  it (`p_trace.cpp:532`), but every `0x1a` in STUDY1 carries mask `0x00`, so
  nothing is restricted.

- The face index comes from the pick, not the player's angle, and index 3
  repeats index 0.
- On a floor record, `(record[+6] & 1) ? (type == 8) : (type == 2)` chooses the
  **mid-platform top** over the **sector floor** (`:3068`) — §4.5's "record flag
  bit0 selects the platform-top". The port tells them apart by the trace's
  `ffloor` being non-null.
- The category-B (examine) twin uses a **third** table, `dir_mask2 = {1,2,4,8}`
  (`:3100`). Not implemented.

**Also unimplemented on both click channels: the authored bounding box.** A
record whose `+0x0c` is non-zero additionally requires the player's x/z inside
`[+0x0c..+0x0e] × [+0x10..+0x12]` (`:3034-3038`, `:3075-3079`). Most records
leave it unset and fire from anywhere.

### The probe runs BEFORE the line path, and must

`P_UseTraverse`'s blocked path ends in `return true` — "can't use through a
wall", with the `*usefail` sound — so an **obstructed ray reports the use as
consumed**. Guarding the flat probe on `!used` skipped it in exactly the case it
was needed: standing in sector 409 looking at its floor, the 2D ray met a wall,
the use was eaten, and the floor trigger under the crosshair was never offered.
Nothing in the log said so, because the guard was outside the function.

Running first is also *closer* to the original: `activate_targeted_object` picks
whatever is under the cursor and a floor there wins outright (§4.5). It does not
steal uses from doors — at a shallow pitch the trace reaches a door's wall long
before the floor.

### A centre-screen ray is the narrowest case, not the real one

Sector 409 looked like a dispatch failure and was a **probe aiming** problem.
The rig looked down at the original's full-down view pitch, about 44°, and in a
cramped room that is not steep enough: the ray met a **wall** at z −0.6 some 27
units out before reaching the floor. At 80° all 12 fire.

Worth keeping, because it bears on the eventual design: **the original does not
aim with the view centre at all.** It picks under a free cursor over the
rendered frame, so its reachable aim is far wider than a centre ray — and in VR
it will be a hand pointer, wider still. Any judgement about whether a floor
trigger is "reachable" that is made with a centre-screen ray is pessimistic.

### Which record flags actually occur in STUDY1 — measured 2026-10-03

From the load report's `record flags present, by opcode` line, **one count per
record** (counting bindings instead inflates every face-keyed opcode, since one
record binds to each face its key resolves to — `0x18`'s 26 reads as 68).

| opcode | `+0x06` | `+0x07` | records | meaning |
|---|---|---|---|---|
| `0x13` | `0x00` | `0x00` | 1 | water variant |
| `0x13` | `0x00` | `0x01` | 12 | plain |
| `0x13` | `0x10` | `0x00` | 2 | water variant |
| `0x13` | `0x10` | `0x01` | 7 | plain |
| `0x13` | `0x20` | `0x04` | 1 | water variant |
| `0x18` | `0x01` | `0x00` | 24 | |
| `0x18` | `0x02` | `0x00` | 1 | |
| `0x18` | `0x06` | `0x00` | 1 | |
| `0x19` | `0x10` | `0x00` | 12 | the sector floor |
| `0x1a` | `0x00` | `0x00` | 4 | |
| `0x32` | `0x06` | `0x00` | 1 | |

Totals reconcile with the opcode table: 23, 26, 12, 4, 1.

**What this settles, and it changes what is worth building:**

- **The leave refire cannot be tested in this map.** No `0x13` record has
  `+0x06 & 0x40`, the bit that asks for it. Building it would be building blind.
- **The facing mask's blocking half cannot be tested either.** Every `0x13`
  mask is `0x00`, `0x10` or `0x20` — none has any of bits 0-3, so nothing can be
  refused by it here.
- **The linked-platform variant does not occur.** `+0x07` is `0x00` or `0x01`
  except for one `0x04`, and `VariantOf` makes that water too (bits 0 and 1
  clear). So 19 plain, 4 water, **0 linked** — that branch is implemented and
  unexercised.
- **No platform-top `0x19` exists.** All 12 have `+0x06 & 1` clear.
- **The FACE direction mask is present on every `0x18`, and it matters.** The
  values `0x01`, `0x02`, `0x06` are exactly `dir_mask1`-shaped (`{1,4,2,1}`;
  `0x06 = 0x02|0x04`). All 26 `0x18` records and the single `0x32` carry a
  non-zero mask, so the port currently fires **every one of them from any
  approach** where the original restricts them. This is the largest remaining
  fidelity gap in the trigger layer.

### What ROTH.C says and the port does not do

Recorded so none of it is re-derived.

- **`+0x06 & 0x40` is the "fire on leave too" flag.** `twe_link_state:3205-3213`
  refires the latched record when the player leaves its sector, and the latch is
  set only for a record with that bit (`fire_sector_trigger:2893`). So
  enter-vs-leave was never ambiguous, only unread. **Not built, and now known
  to be untestable in STUDY1** — no record there carries the bit.
- **The three link variants ARE implemented**, and they need no sector byte: the
  variant comes from the **record's `+0x07`**, which `mark_geometry_records_by_id`
  (`raw_commands.c:4994-5005`) turns into the sector marker —
  `mask = 0x40; if (b7 & 3) { mask = 0xc0; if (b7 & 1) mask = 0x80; }`. So bit 0
  set is **plain**, bit 1 alone is **linked platform**, neither is **water**.
  `twe_link_state:3220-3232` then branches on it. The linked variant's `cell+8`
  is the mid-platform's **topZ**, so its test is "standing on the platform top".
  Z needs no conversion — the loader takes `floorZ = double(rs.floorHeight)`
  (`rothmap.cpp:465`).

  **The "water triggers over-fire" claim this file previously made was wrong.**
  `g_player_z` is the player's **feet**, not the eye — `collision_physics.c:65`
  builds the extent as `[qZ, qZ + height + 0xa]` — so a player standing normally
  on the floor has feet *at* `floorHeight` and is **inside** the link. The test
  only excludes being *above* the floor: mid-jump, or stood on a platform within
  the sector. The overstatement came from reading the branch without checking
  which end of the player Z measures.

  Two **deliberate differences** remain. The original ORs the marker per
  *sector*, so two `0x13` records of different variants on one sector combine
  (`0x40|0x80` → `0xc0`, "linked") — STUDY1's sector 430 carries two. This
  evaluates per *record* instead. And the original fires exactly **one** record
  per entry, found by `find_object_record_by_id(word[sector+0x14])`
  (`:3264-3267`); this fires every `0x13` bound to the sector. Both unmeasured.
- **Two gates the original applies first** (`fire_sector_trigger:2869-2871`): the
  modifier byte `+0x02` blocks on mask `0x29` (armed `0x01`, spent `0x08`,
  registered `0x20`) where the port tests only the spent bit, at bind time; and
  `+0x07 & 4` requires the player to be **moving**, against
  `g_move_speed_accum`, which has no equivalent here.
- **Record flag bit0 selects the platform top** instead of the sector floor
  (`GAME_core.md` §4.5). Not implemented: a mid-platform top reports its own
  sector, so a probe landing on one reads as a sector with no binding.

### Why these had no caller, which is not "nobody wired it"

`P_UseLines` is entirely two-dimensional — `start` and `end` are `DVector2` and
`P_UseTraverse` walks lines out of the blockmap. There is no flat in that path to
hit and no pitch in it to aim with, so a floor click is not a question it can be
asked. That is the structural reason `0x19` had no dispatch, and why "wire a
caller for `FireSectorTriggers`" kept looking like the only option: it was the
only option *through that path*.

`UseFlat` runs as a **fallback**, after the line path finds nothing. The original
picks whatever is under the cursor, so a floor there wins outright even with a
wall further along the ray. The port's order costs an under-fire — a floor
trigger in front of a usable wall will not fire — and was chosen because the
line path is verified for doors and faces and a probe running first could steal
a use from a door the player aimed slightly below. Matching the original means
one pick classifying face / floor / object / door together.

### A line fires only the side that was touched — fixed 2026-10-03

`ActivateLine` used to fire the chains on **both** sidedefs and discard the
`side` the engine passes; the loop ended in `(void)side`. A Realms two-sided
wall is **two faces**, sisters, each paired to one sidedef, so one hitscan at
side 91 fired both side 91 (face 85) *and* side 92 (face 1366) — and where both
sisters resolve from the same record, **the chain ran twice for one shot**.

The original has no such ambiguity: `dispatch_entry_command_trigger` resolves
**one** texmap from the pick (`word[p1+8]`, `:3019`) and matches that face alone.

Now one firing per shot, measured: 16 firings across 22 shots, **no shot firing
twice**. The six that fired nothing are placement, not dispatch — see below.

The door path is the regression control for this, because the loader could have
paired a face to the opposite sidedef from the one Doom calls front. **6 of 6
doors still swing.** `captures/impact_and_doors.cfg` runs both together, doors
first, for exactly that reason.

### `SPAC_Push` reaches the hook and correctly fires nothing

Our lines no longer request `SPAC_Push`, but `CheckForPushSpecial`
(`p_map.cpp:2327`) calls `P_ActivateLine` with it anyway, and the Realms hook
sits at the **top** of `P_ActivateLine` — before `P_TestActivateLine` consults
the activation bits. So the hook sees events the line never asked for. That is
fine because it gates on the event itself; the log shows `event 0x8 -> NO chain
bound`. Worth knowing: **the activation bits do not filter what reaches the
hook**, only which dispatch sites bother to offer the line.

### Open

- The **shoot rig misses in tight spots.** A 40-unit stand-off from a wall's
  midpoint can land inside neighbouring geometry; the player is shoved before
  the shot and the hitscan goes elsewhere. It showed only as a stray
  `SPAC_Push` on an adjacent sidedef — 6 of 22 shots. The rig now warns when
  the stand-off lands in a different sector than the side belongs to, so such a
  shot says it proves nothing instead of reading as a dead trigger.
- The **platform-top** path (`+0x06` bit 0 set) is implemented and untested —
  STUDY1 has no such record.
- The **face direction mask** and the **bounding box** on both click channels
  are unimplemented, so a face trigger fires from any approach and from
  anywhere. Both are quoted above with line numbers.
- The facing mask's **blocking** half is unproven; nothing has been refused yet.

---

## 12. Running a test without touching the owner's config

**Always pass `-config tools/rothdiff/captures/capture.ini`.** Without it a run
reads *and writes* the owner's `doomxr.ini`, which is the whole of §8/§3.5's
archived-cvar leak — `vr_mode`, `roth_lighting`, `roth_pattern`, `vid_fixgamma`.

`capture.ini` is **a copy of the owner's ini with `vr_mode` forced to 0**, and it
is a copy on purpose. It is not committed (`tools/rothdiff/.gitignore` excludes
`captures/*` except `*.cfg`, correctly — it holds the owner's own paths).
Recreate it with:

```
cp "$USERPROFILE/Documents/My Games/DoomXR/doomxr.ini" \
   tools/rothdiff/captures/capture.ini
sed -i 's/^vr_mode=.*/vr_mode=0/' tools/rothdiff/captures/capture.ini
```

**Do not write a minimal one.** Two attempts cost the owner a disrupted session
each: the first inherited `vr_mode=15` from their config and **took over the
headset while they were wearing it**; the second defaulted to **fullscreen at
3840×2160 on the OpenGL backend** and never loaded the map. The display settings
in the owner's file are known to work. `vr_mode` is `CVAR_GLOBALCONFIG`, so
checking it once early says nothing about later runs — the owner can change it
between them, and did.

### Other rig facts

- **`+exec` runs before the deferred `map`.** A console command in a cfg that
  inspects the level finds none — `roth_trigger_list` printed "no Realms level
  loaded". Anything needing the built level belongs in the **load report**, which
  is written during the load.
- **A pose batch is authored for one map, and the level logic can change the
  map.** The rig now abandons its queue and quits on a map change. Before that,
  the sector-174 warp left a 22-sector probe running in STUDY3 against STUDY1's
  numbers; it crashed several probes later and looked like the dispatch failing.
- **`rothdiff_sector <n...>`** stands in each sector in turn; **`rothdiff_usefloor
  <n...>`** does the same but pitches down 126 ROTH units (≈44°) and presses use.
  Kept separate deliberately: the enter-sector test must not press use, or it
  fires whatever `0x18` is on a nearby wall and muddies the log. A sector was not
  addressable at all before these — the report says which sectors carry a
  trigger, but nothing offline turns a sector number into a point inside it, and
  `findspots` works by surface type.
- **LNK1103 follows the `.cpp` you edited**, not header changes. It hit on five
  of six builds in one session, each time naming the object just recompiled. The
  remedy is deleting that `.obj` plus `doomxr.iobj` and building again; budget
  the LTCG pass as a normal cost. `HANDOFF_REMAROTH.md` §9's "do not delete
  `doomxr.iobj` unless LNK1103 has actually appeared" still holds — it does
  appear, constantly.
