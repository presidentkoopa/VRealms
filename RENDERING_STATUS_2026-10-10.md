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

That second number is the holes. We are painting wall over floor and ceiling that
should be visible, and the wall we paint has no texture on it, so you see through
it into nothing.

It was then narrowed to its cause:

- **two linedefs**, numbered 87 and 88, account for **all 36,096** of those pixels
- they are the **west wall of the room**, from (256,396) to (256,716), split at
  y=556, with sectors 404 and 405 behind them
- they carry **texture index 0 with no mid, upper or lower texture**
- for this map's artwork pack, **index 0 is the pack's "draw nothing" key** — the
  index that means *paint no pixels at all*

Six other explanations were tested and **ruled out by measurement, not argument**:
intermediate floors (all 15 resolve valid art), Realms' own lighting model (the
black regions are pixel-identical with it switched off), sky surfaces, degenerate
sectors (only 4 in the map, none of them these), mismatched sector heights (both
sides are 400..720), and broken face pairings (zero non-mutual links in the map).

A third wall in the same room — line 90 — has **identical data in every field**
and renders correctly. So the defect is not in the wall record. It is in how we
turn that record into geometry.

**The most likely cause, and it is already written down in our own docs:** we
build every wall from floor to ceiling, because our loader does not carry the
per-face wall height the original stores. `ROTH_STATE.md` §7 says this in as many
words and calls it "geometry work, not texturing". A wall that should stop partway
up, stretched to the ceiling and carrying a "draw nothing" texture, is exactly a
see-through hole across the ceiling.

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

1. **Make the comparison the gate.** Pick a pose set — say 8–12 camera positions
   across STUDY1, plus a handful in an outdoor map and a map with water — and
   require a match percentage, per pose, before any loader change lands. The rig
   already supports a saved baseline and exits non-zero on a regression.

2. **Fix the texture-ID mapping in the rig first** (§3 item 1), or the headline
   number stays uninformative and only the "kind" mismatches can be trusted.

3. **Then work the defect classes in this order**, because they are roughly in
   descending order of "makes the room unreadable":
   - wall heights (the holes — §3)
   - door leaf orientation and door textures
   - wall texture alignment
   - sprite scaling (the trees) and prop placement (the chandelier)

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
