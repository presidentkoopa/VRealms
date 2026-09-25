# Engine changes made for Realms of the Haunting

A running record of everything we add to or change in the engine specifically for this project.

**Why this file exists:** REMAROTH is a clone of REMA, and REMA is shared ground for a large family
of mods worked on by many parallel sessions. Anything we do here may eventually be cherry-picked
back. A future session — or a future you — needs to know what was changed, why, and whether it was
meant to be general or Realms-specific. Undocumented engine edits are how a fork becomes
unmaintainable.

---

## Current status: **no engine changes made yet**

Everything working so far is plain data conversion done outside the engine, in Python. The engine
was rebuilt from clean REMA source and loads our generated levels without modification.

That is deliberate and worth preserving as long as it holds: no engine change means no recompile
to test an idea, and no risk to the other mods sharing this lineage. We change the engine when the
data model genuinely cannot be expressed otherwise — not to avoid effort elsewhere.

---

## Rules for anything added here

Taken from the project's standing engine philosophy, and they apply to us too:

1. **Name it for what it does, never for who asked.** A field called `TextureAnchorWorldSpace` gets
   reused. The same field called `RothWallFix` does not, and the next person writes a second one.
2. **Assume a second caller.** Anything added for Realms should be usable by another mod without
   modification.
3. **No per-mod special-casing in engine code.** If the engine needs to know *which game* is asking,
   the abstraction is wrong.
4. **Additive and default-off.** New parameters get defaults so existing callers are untouched. New
   per-surface state stays inert until something sets it. A change that alters behaviour for
   everything is a change that breaks a mod nobody is testing today.
5. **Give it a real home.** Put it where its neighbours live, with a comment explaining why it
   exists — the engine gets read by future sessions with none of this conversation's context.
6. **Propose before building.** Scope the change, name the files it touches, agree it, then build.

---

## Proposed change #1 — world-anchored texture addressing

**Status: proposed, not built. Blocked on confirming the original rule.**

### The problem
Doom positions a wall texture by starting it at the wall's first corner, then sliding it with a
per-wall offset. Realms appears to do something different: it positions textures against
**world-space coordinates**, as though wallpaper were stretched across the whole level and each
wall shows whatever part of it sits behind it. That is why Realms' walls line up with each other
automatically at corners, with no per-wall adjustment.

We can *approximate* this by calculating, for every wall, the Doom offset that happens to land the
texture in the same place. That works for straightforward walls but is an approximation, and the
original's edge-case flags (`HALF_PIXEL`, `EDGE_MAP`, `IMAGE_FIT`) may not be expressible in Doom's
vocabulary at all.

### The proposal
Let the engine address textures the way Realms does, rather than translating Realms into Doom's
model. The converter passes the original's own parameters through into the map as custom fields
(the UDMF map format permits arbitrary `user_*` fields on sidedefs and sectors); the engine uses
world-anchored addressing when those fields are present and its normal behaviour when they are not.

This satisfies rule 4 — inert unless set — and rule 1: the capability is "anchor this surface's
texture to world space", which is generally useful (seamless tiling across adjacent walls is a
thing plenty of mods would want), not a Realms favour.

### Why it is not built yet
We do not yet know the original rule precisely enough to implement it. Two investigations are
running: one reading the original engine reconstruction (`ROTH.C`), one reading roth-editor's
working implementation. Building engine code before we can state the formula would mean debugging
"do I understand the rule" and "did I write the C++ correctly" at the same time.

**Sequencing intent:** prove the rule in Python first, where an experiment costs seconds instead of
a recompile. Move it into the engine once it is understood and stable. If it turns out Doom's model
genuinely cannot express the rule, that shortcut collapses and this change becomes required rather
than preferred.

### Files it would likely touch
To be filled in when scoped — expected to be the wall/flat texture coordinate generation in the
hardware renderer, plus UDMF field parsing to carry the new per-surface values.

---

## Likely future candidates

Recorded so they are not forgotten, none investigated yet:

- **Raised walkways.** Realms' "mid-platforms" (2,299 across the game) give a sector a second
  floor/ceiling pair inside the same footprint. Doom's nearest equivalent is 3D floors. Whether to
  translate or to support the original concept directly is an open question of the same shape as
  the texture one.
- **Doors, switches and triggers.** Realms' level logic is a chained command system with its own
  opcodes. This almost certainly becomes engine or script work rather than translation, since
  Doom's numbered line-specials were never designed for these semantics.
- **Reading the original files directly.** Long-term, the engine could load `.RAW`/`.DAS` natively
  instead of us pre-converting to a `.pk3`. Attractive because nothing would need pre-generating on
  a player's machine, but strictly worse to debug until the conversion rules are fully understood.
