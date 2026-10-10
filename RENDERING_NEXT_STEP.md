# Rendering: the holes are not lines 87/88 — 2026-10-10

**For the lane working on `RENDERING_STATUS_2026-10-10.md`.** Read this before
changing any wall heights.

Picture: `tools/rothdiff/captures/landing_570_556.png`. It shows the original
and REMAROTH, both **rendered** at STUDY1 (570, 556), ROTH angle 128.

---

## 1. Lines 87 and 88 are an open doorway, and we already draw through them

The data, read from the retail `STUDY1.RAW`:

| face | from | to | sector | other side | textures |
|---|---|---|---|---|---|
| 14486 | (256,396) | (256,556) | 25, floor 400 / ceiling 720 | 404, 400 / 720 | all 0 |
| 14498 | (256,556) | (256,716) | 25, 400 / 720 | 405, 400 / 720 | all 0 |

Equal heights on both sides, no texture on any part. This is plain open floor
from the landing (sector 25) into the corridor beyond it (404 and 405, then
368). Line 90, which renders correctly, is the same thing on the east side.

**Our renderer draws straight through them.** In the picture, the corridor
carries on past x = 256 with its walls, carpet and paintings.

So the "35,914 px where we draw wall" in §3 of the status doc is the
**identity rig**, not the renderer. `rothdiff_dump` builds our half with one
`Trace()` per pixel. That is a raycast, not a picture, and here it stops on a
two-sided line that the renderer passes through. **Find out why `Trace()` stops
there before you trust that rig's "kind" numbers again.**

**Do not change wall heights for this.** The "we build every wall floor to
ceiling" note in `ROTH_STATE.md` §7 is about the sky above EDGE_MAP walls. It
has nothing to do with this doorway.

---

## 2. Where the holes actually are

At the **far end of the corridor, x = −256**. In the original that is a painting
between two candle stands. In ours it is a black rectangle, plus black
triangles in the floor and ceiling between the camera and the far wall.

The far end is built from **two 2-unit-thick sectors**, then a box:

| sector | x | floor / ceiling | faces |
|---|---|---|---|
| 368, the corridor | −256 … 0 | 400 / 720 | west face (−256,506)→(−256,606) opens into 56. Upper 153, lower 43. |
| **56** | **−258 … −256**, 2 units | 500 / 600 | two side faces 2 units long, mid `0x8000`. Back face into 57: **upper 512, flags `0xa0`, extended record (`u0c` = 32)**. |
| **57** | **−260 … −258**, 2 units | 500 / **598** | the same `0x8000` sides. Back face into 58: upper 3. |
| 58 | −324 … −260 | 500 / 600 | a 64-deep box, trigger 0x2e |

So the painting comes from a stack of 2-unit slivers: art on a **2-unit-tall
upper** (600 against 598), with mid `0x8000` on the sliver sides.

### Already established

- **It is not new.** A build from 2026-10-01 shows the same black end and the
  same flat triangles, so nothing changed today caused it.
- **The door polyobjects are involved, but they are not the cause.** With door
  polyobject spots turned off, the triangles change shape but the black end
  stays.

---

## 2b. CONFIRMED by rendering, 2026-10-10 — it is sectors 56 and 57

Step 1 below was run. A new cvar `roth_skip_sectors 56,57` leaves those sectors'
faces out of the build entirely (7 faces), and the same pose was rendered with
and without, real art, neutral post-processing:

| | ceiling | floor |
|---|---|---|
| with 56/57 | black wedge across the coffering | black wedge across the carpet |
| **without** | **coffering unbroken** | **carpet clean to the far wall** |

**Both flat holes are the thin-sliver geometry.** The narrow black strip still at
the far end of the test frame is only the painting, which the test deleted.

Frames: `scratchpad/r1/base.png`, `scratchpad/r1/skip5657.png`.

`roth_skip_sectors` takes a comma list of Realms sector numbers. Not archived — a
build with sectors missing is a test, never a load. It exists so that "is this
the geometry that breaks the picture" is settled by rendering, not by argument.

### Mid `0x8000` is NOT the cause — checked against ROTH.C

`renderer.c:9222-9225` is a SIGN test, not an `0xFFxx` test:

```c
if ((int16_t)a6 < 0)        /* any top-bit index -> solid palette colour */
else if (a6 != colorkey)    /* -> textured */
else                        /* -> draw nothing */
```

`0x8000` is negative, so the original **does** make it a solid colour, and
`0x8000 - 0x8000` is palette entry 0 — exactly what `TEX_COLOUR_BASE_LO`
(`roth_texture.h:274`) already does. Our two bases, `0xFF00` and `0x8000`, both
reduce to the index's low byte, as the original does. **Rule this out; do not
"fix" it.**

That leaves **the node builder on 2-unit lines and sectors** as the live
hypothesis. Roth maps set `ForceNodeBuild` (`rothmap.cpp:356`), so every sliver
goes through `FNodeBuilder`, and triangles missing from the flats of the room in
FRONT of the slivers is precisely what a bad BSP looks like.

---

## 3. Do these in order, with a rendered frame each time

Each step: `rothdiff_shot` at this pose against the oracle's real-art frame.
Not the identity buffer.

1. **Is the black the slivers?** Skip sectors 56 and 57 for one test build
   (no lines and no sector) and render again. If the far end and the triangles
   come back, this geometry is the bug. If not, say so and look elsewhere.
2. **If it is the slivers, the two likely causes** — check both, they are only
   hypotheses:
   - **The node builder on 2-unit lines and sectors.** Look at the build
     messages and the GL subsectors it makes for 56 and 57. Triangles missing
     from the flats of rooms nearby is what a bad BSP looks like.
   - **Mid `0x8000`.** The three-way test (`HANDOFF_REMAROTH.md` §5) turns
     `0xFFxx` into a solid colour. `0x8000` is negative as a signed number but
     is not `0xFFxx`. Check what `roth_surface`'s classification does with it,
     and what ROTH.C does at `renderer.c:9222-9225`.
3. **The painting itself:** art on a 2-unit upper with flags `0xa0` and an
   extended record. Find what the original does with that piece before
   building it. `HANDOFF_REMAROTH.md` §5 and `docs/REMAROTH_MEASURED.md` §3
   cover the wall rules.
4. **Then the gate the status doc asks for.** Rendered frames at a fixed pose
   set, each against the oracle, before any loader change lands. Start with
   this pose.

---

*Measured in Claude's Linux workspace: the original (ROTH.C, headless, real
art) and a REMAROTH build from 2026-10-01, at the same pose. The sector table
is read straight from the retail map file.*
