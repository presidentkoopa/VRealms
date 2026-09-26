# How Realms lights a surface
### Transcribed from ROTH.C, 2026-09-26. Not inferred, not approximated.

This is the single biggest difference between how our build looks and how the
original looks. It is worth reading in full before touching lighting again,
because **Realms' model is not Doom's model** and treating it as one produces the
flat, evenly-lit room we currently have.

---

## The one thing to understand first

**A sector's light byte does not set brightness. It sets how fast the darkness
closes in with distance.**

Doom says "this room is at light level 160" and then darkens things a bit with
range. Realms says "in this room, the dark starts *here* and reaches full black
*there*". That is why the original is mostly black with pools of light: every
surface is lit almost entirely by how far away it is, and the sector only moves
the curve.

---

## The formula

From `renderer.c:4328-4340` (the per-column shade), `renderer.c:4129-4137` (the
per-surface setup) and `renderer.c:9186-9188` (where the light byte comes from):

```
light  = sector byte at +0x0b                       # 0x80 (128) is neutral
if light != 0:  light += flashBonus                 # see "the flash" below

off    = light - 0x80                                # SIGNED offset from neutral
bias   = 8 + off                                     # g_floorceil_depth_clip_bias
cap    = 0x20 - (max(off + 4, 0) >> 2)               # g_floorceil_clip_scale

# per column / per span, from the depth accumulator:
s = (depth >> shr_n) - bias - projectBias
if s <= 0:      -> brightest row
if s >= cap:    s = cap
if s >= 0x1f:   -> fully dark
row = s                                              # 0..31
```

The drawn pixel is then `table[(row << 8) | texel]`.

**So a bright sector (light > 128) makes `bias` larger, which subtracts more and
keeps `s` small — it stays bright further out. A dark sector (light < 128) makes
`bias` smaller and `cap` tighter, so it reaches black sooner and cannot get as
bright in the first place.** One byte moves both ends of the curve.

---

## Where the tables live — all of them in the player's own files

Read straight out of the `.DAS`, immediately after the palette
(`map_load.c:373-381`), in this exact order:

| Size | Goes to | What it is |
|---|---|---|
| — | palette | `read_das_palette()` |
| 2 | remap prefix | |
| `0x4000` | `0x86d14` = `g_world_shading_table_ptr` | **two 32-level ramps**: normal, then the tint ramp at +0x2000 |
| `0x10000` | `0x85d08` | the 64K translucency/blend LUT |
| `0x100` | `0x85d10` | |
| `0x100` | `0x86d20` | |

So the shading ramp is **32 levels x 256 colours**, exactly the shape of a Doom
COLORMAP. We do not need to build one, invent one, or approximate one: it is in
the file, and we already open that file.

The tint ramp (the second 0x2000) is selected per surface when the face flags
carry bit 2 — that is the water/lava look. A third table, the glow ramp, is
selected on flag 0x40 with a gate. `renderer.c:9189-9204` does the picking.

---

## The flash

`renderer.c:9187`:

```
if (light != 0) light += byte[g_render_sector_walk_mode + 0x23]
```

That byte is a **global, decaying brightness bonus**. `entity_ai.c:1218` raises it
(taking the max, not adding), and `player.c:459-462` decays it every tick by the
frame time scale. It is why firing a weapon lights the room, and why a sector
whose light byte is exactly 0 stays pitch black no matter what — the `!= 0` test
means a zero-light sector is immune to the flash.

Two behaviours fall straight out of this and both are worth keeping: muzzle
flashes and lightning genuinely illuminate the manor, and a sector authored at 0
is *absolutely* dark, which is a tool the level designers clearly used.

---

## The two constants I have not resolved

`shr_n` and `projectBias` are **self-modifying code**: `patch_span_driver_shade`
(`renderer.c:9125-9139`) pokes them into the span drivers' immediate operands,
reading them from two tables in the original executable's data segment at
`0x71db4` and `0x71dba`, indexed by the map's `lightAmbience` (metadata `+0x10`).

They are per-map, not per-sector, so there is one pair per level. They are not in
the `.DAS` or the `.RAW` — they are in `ROTH.EXE`, which the player has. Reading
them from there is the faithful answer and is no different in kind from reading
the maps.

**Do not guess these.** Until they are read, any implementation is calibrated
against nothing.

---

## What this means for our renderer

GZDoom's colormap is also 32 levels, so the table drops in. What does not drop in
is the light calculation: GZDoom derives its colormap index from `lightlevel` plus
a distance attenuation chosen by the light mode, and none of its modes are this.

So the engine needs a light mode that computes the row the way the block above
does, from the sector's byte and the depth, with the per-map pair applied. That is
an engine addition, and it is the right kind: a renderer that can shade by a
supplied curve is useful to anything, not just Realms.

**Until that exists, our sector `lightlevel` mapping is a placeholder and should
be described as one.** It is currently one-to-one, which at least stops the level
being uniformly white, but it is not the model and no amount of tuning it will
make it the model.
