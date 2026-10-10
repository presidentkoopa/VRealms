# Level rendering is not correct, and it blocks everything else

**For the co-designer. Written 2026-10-10.**

---

## 1. What this project is — and what it is not

**We are not rebuilding Realms of the Haunting.** We are making a GZDoom/QuestZDoom
VR fork read the player's own retail install *natively*: the original `.RAW` maps,
the original `.DAS` artwork packs, the original palettes, the original level-logic
command records. Nothing is converted, re-authored, re-textured or re-modelled.
The player must own the game; the engine reads their copy at load time and writes
nothing back.

That distinction matters for everything below, because it means **there is no
authoring work that can paper over a rendering bug.** If a wall is in the wrong
place, the map is not wrong — our reader is.

**This is also not reverse engineering.** We have three independent descriptions
of these file formats:

| | what it is | how we use it |
|---|---|---|
| the retail install | the actual data | the input, and the thing being read |
| **ROTH.C** | a C decompilation of the original DOS engine, function by function, with original addresses | the authority on *what the original does*. Read-only, never edited |
| **roth-editor** | an independent open-source editor (Godot) with its own reader for the same formats | a second opinion on *what the data means* |

So when something renders wrongly, the correct answer is always available. It is
a matter of going and reading it.

---

## 2. The state of it

In the **first room of the first map**, in a few minutes, the owner found:

- **holes in the floor and the ceiling** you can see straight through, into other
  parts of the level
- **doors with no texture at all**
- **misaligned wall textures**
- **tree sprites stretched** out of shape
- **door leaves (polyobjects) facing the wrong way**
- **the chandelier misplaced**, and its texture not right either

This is one room. There are 44 maps.

---

## 3. What was actually measured today, and how

The project has a comparison rig (`tools/rothdiff`) that can run **the original
engine** and **our engine** from the *same camera position* and record, for every
pixel, which surface each engine drew and out of which texture. It writes an
identity buffer, not a screenshot, so the comparison is machine-checkable rather
than a matter of opinion.

It was pointed at one position in STUDY1 — the upper landing, where the owner was
standing — and the result is this:

```
pose 570,556 angle 128        640x480
scored pixels        107,864
match                    0.0%
causes:
    texture                      58,110 px
    kind "ceiling" vs "wall"     19,901 px
    kind "floor"   vs "wall"     16,013 px
```

Two things to read carefully here:

1. **The "texture" mismatches are not trustworthy as a number.** The two engines
   identify textures in different ID spaces, so a texture-vs-texture comparison
   at this pose is close to meaningless. That part of the rig needs a mapping
   before its number means anything.

2. **The "kind" mismatches are absolutely trustworthy**, because "is this pixel a
   floor, a ceiling or a wall" is the same question in both engines.
   **35,914 pixels where the original draws floor or ceiling, we draw wall.**

### CORRECTION — that second number was the rig, not the renderer

**This section originally concluded that two linedefs, 87 and 88, were painting
untextured wall over the floor and ceiling. That conclusion was wrong, and it was
disproved by rendering an actual frame.** It is left here, corrected rather than
deleted, because the way it went wrong is the point of §5.

Lines 87 and 88 are an **open doorway** from the landing into the corridor beyond,
and **our renderer draws straight through them** — in a rendered frame the corridor
carries on past x = 256 with its walls, carpet and paintings, as it should.

The 35,914 "we draw wall" pixels were an artefact of how our half of the
comparison is built. `rothdiff_dump` casts one `Trace()` ray per pixel; that is a
raycast, not a picture, and it **stops on a two-sided line that the renderer
passes through**. So the rig was reporting a wall that is not drawn.

**Consequence: the rig's "kind" numbers cannot be trusted either**, until that
`Trace()` behaviour is understood. Both halves of §3 are now suspect — the
texture numbers for the ID-space reason above, and the kind numbers for this one.
That makes fixing the rig the first task, not an aside.

Six other explanations were tested and ruled out by measurement before that:
intermediate floors (all 15 resolve valid art), Realms' own lighting model (the
black regions are pixel-identical with it switched off), sky surfaces, degenerate
sectors (only 4 in the map, none of them these), mismatched sector heights, and
broken face pairings (zero non-mutual links in the map). Those remain valid.

### Where the holes actually are

At the **far end of the corridor, x = −256** — in the original, a painting between
two candle stands; in ours, a black rectangle with black triangles in the floor
and ceiling in front of it.

That end of the corridor is built from a stack of **2-unit-thick sectors** (56 and
57, each two units deep, with mid texture `0x8000` on their sides and the painting
carried on a 2-unit-tall upper). Thin-sliver geometry of that kind is a plausible
source of both the black face and the missing triangles in nearby flats — a
mis-built BSP looks exactly like triangles missing from a floor.

Two things are already established about it:

- **It is not new.** A build from 2026-10-01 shows the same black end and the same
  flat triangles, so nothing changed recently caused it.
- **The door polyobjects are involved but are not the cause.** With their spots
  turned off the triangles change shape; the black end stays.

**Explicitly not the cause: wall heights.** An earlier draft of this document
proposed that we build every wall floor-to-ceiling and that this was the bug. The
note in `ROTH_STATE.md` §7 that says so is about the parallax sky above EDGE_MAP
walls, and has nothing to do with this geometry. Do not change wall heights for
this.

---

## 4. Why this blocks everything else

The project plan (`ROADMAP.md`) has nine milestones. One is complete. The large
remaining items are the game layer: dialogue (371 command records, present in all
44 maps), inventory, actors and weapons, saves. Measured over all 44 maps, **1,879
of 3,588 live command records currently run — 52.4%.**

None of that matters yet. A faithful dialogue system delivered into a room with
holes in the floor is not progress anyone can evaluate, and it cannot be tested in
a headset by the one person who has to sign it off.

**The order has to be: make one room correct, then make one map correct, then
build the game layer on top of something trustworthy.**

---

## 5. Why it has been hard, honestly

Not because the formats are unknown. They are not — see §1.

The loader was built and checked **by eye, against screenshots.** The comparison
rig described in §3 exists, works, and is documented inside its own source as
having been written precisely because *"every texture argument on this project so
far has been settled by looking at two screenshots, and every one of those
settlements has been wrong at least once."*

It was then not used as the gate. Today's session spent roughly an hour
diagnosing our engine in isolation — forming and discarding one hypothesis after
another — before running the original at the same coordinates, which settled the
question in a single capture and immediately proved the geometry was being built
wrongly. That is a process failure, and it is the one worth fixing first, because
it is the reason defects of this size reached the headset at all.

---

## 6. What should happen next

There is now a worked plan for this — `RENDERING_PLAN.md`, with the detail for the
holes in `RENDERING_NEXT_STEP.md`. In summary:

1. **Make the gate RENDERED FRAMES, not the identity buffer.** A pose set, our
   frame against the original's real-art frame at the same pose, side by side,
   with neutral post-processing. Start with a control pose whose centre is known
   to match; if the control fails, the rig is wrong rather than the port.

2. **Do not trust the identity rig until both of its faults are fixed** — the
   texture IDs being in different spaces, and our half stopping on open doorways
   the renderer draws through (§3). Until then it can mislead a whole session, as
   it did this one.

3. **Then work the defects in order**, each as *one* change with a rendered pair
   image before and after:
   - the holes — test by leaving sectors 56 and 57 out of one build; if they come
     back, the thin-sliver geometry is the bug (node builder, and mid `0x8000`)
   - doors: untextured leaves and leaves facing the wrong way
   - the chandelier and the stretched trees — both are 3D meshes on the same code
     path, and mesh face texture mapping is the largest open item in that area
   - wall texture alignment

4. **Do not start the dialogue or inventory milestones until a room renders
   correctly.** They are large, they are invisible, and they will be built on
   assumptions that a broken renderer cannot validate.

---

## 7. What is genuinely in good shape

So it is clear this is not a write-off:

- the maps, sectors, artwork packs, palettes and level-logic records all **parse
  correctly** off the retail files, unmodified
- the level-logic interpreter runs **52.4%** of the shipping game's command
  records, measured across all 44 maps
- doors, triggers, lighting and the VR hand interaction all function
- it runs at 60 fps in the headset
- the comparison rig, the oracle runner and the per-map load reports **all work**,
  and are the reason §3 could be written at all

The reading of the install is substantially right. It is the turning of that data
into geometry that is not, and that is a bounded problem with three authoritative
references available to solve it against.
