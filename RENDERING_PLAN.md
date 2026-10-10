# Rendering plan — one room right, then one map, then the game

**2026-10-10. This comes before ROADMAP.md M2.** The game layer waits until
STUDY1 renders right (`RENDERING_STATUS_2026-10-10.md` §4).

**The room:** STUDY1's start area and the upper landing, as the owner saw them.
**The gate:** a rendered frame of ours against the original's real-art frame,
same pose. Not the identity buffer, not judging by eye alone.

---

## R0 — The gate, before any fix *(first, short)*

1. **Pose set.** Put one pose on each defect below, plus pose A
   (864, 3840, 0) as the control, in a file in `tools/rothdiff/`. Add a few
   outdoor poses later.
2. **For each pose:** the original's frame (`run_oracle.ps1`), ours
   (`rothdiff_shot`, `pair2.cfg`, 640×480), and `sidebyside.ps1`. Neutral
   post-processing, including `vid_fixgamma 0` (`docs/REMAROTH_MEASURED.md` §12).
3. **Stop trusting the identity buffer** until two things are fixed:
   - its texture ids are in different spaces in the two engines;
   - our half is `Trace()`, which stops on open doorways the renderer draws
     through (lines 87/88, `RENDERING_NEXT_STEP.md` §1).
4. **Control first.** Pose A's centre must still match (measured doc §4). If it
   doesn't, the rig is wrong, not the port.

**Done when** every pose has a pair image, and the owner has looked at the set.

## The six defects, in order

For each one, the order is: name the pose, find the rule in the documents
below, change one thing, render the pair again.

| # | Defect the owner saw | What is already known | Where to look | First thing to do |
|---|---|---|---|---|
| **R1** | **Holes in floor and ceiling** | Not lines 87/88. The end of the landing corridor is a stack of **2-unit-thick sectors** (56, 57), and we draw it black, with black triangles in the flats nearby. Same on 2026-10-01, so it is not new. | `RENDERING_NEXT_STEP.md` | Leave out sectors 56 and 57 in one test build. If the holes go, it's the thin-sector handling: the node builder on 2-unit lines, and mid `0x8000`. Then find the other holes the owner saw and check each against this. |
| **R2** | **Doors with no texture** and **leaves facing the wrong way** | The leaf skin rule is settled (`HANDOFF_REMAROTH.md` §12). The thickness edges were fixed in `09c664cf`. **Unverified:** front/back art on the 29.3% of leaves whose two faces differ (c1/c3, `ror` against `rol`); the hinge always taken as vertex1 (comment in `rothmap.cpp` at the hinge); swing direction not wired (session 10-02 §1). **The door-height change** (`TEXTURE_MAP_OVERRIDE`, 43 leaves, 10-03 §2.1) was committed and has never been seen. | `HANDOFF_REMAROTH.md` §12; `HANDOFF_SESSION_2026-10-03.md` §2.1; `tools/rothdiff/doorgeom.cpp` | Pair images at two doors: one of the 43 with an override, one without, closed. Check the load report's `doors:` counters. If the untextured ones are the 43, the height change is the cause: revert it and look again. |
| **R3** | **The chandelier misplaced**, its texture wrong, and **trees stretched** | Both are **3D meshes**, the same code path. Mesh scale is exactly 1.0 (§13), so stretching is **not** scale. **Mesh face texture mapping is the largest open item** in §13 ("do not implement from `face+0x24`/`+0x26`"). The absolute direction of mesh yaw is unsettled. | `HANDOFF_REMAROTH.md` §13; `roth_das.cpp` `ReadMesh`; `roth_objects.cpp`; `tools/rothdiff/meshcheck.cpp` | Pair images at the chandelier and at a courtyard tree (STUDY1, camera 1128 1560). Then find the mesh face UV rule in ROTH.C, tracing it from the visible pass (the §2 trap), and measure it with the oracle before writing it. Placement: compare the mesh's origin and yaw against the original's frame. |
| **R4** | **Misaligned wall textures** | The wall rule is measured (`docs/REMAROTH_MEASURED.md` §3). Still open: **which end of a face has u = 0** (`ROTH_SURFACES_FIX.md` §3.3). The load report counts **572 IMAGE_FIT pieces with no vertical fit**, and TRANS_UPLO bands that are inexact. | measured doc §3; `roth_surface.*`; load report "Texturing" | Pair images of the misaligned walls. Sort each into: horizontal start, vertical fit, or band. Fix one class, render again. |
| R5 | Everything parked | Right-hand wall lighting at pose A, the sky's one column, the bay pane, the storm glow | `LANE_START.md` §5 Step 2 | Only if one of them makes the room unreadable. |

## Done

- **One room:** every R0 pose matches the original, and the owner walks STUDY1's
  start and landing in the headset and sees nothing wrong.
- **One map:** the same across STUDY1. Add poses wherever he finds something.
- **Then** go back to `ROADMAP.md`: M1 (point and press), then M2.

## Rules for this work

- **One defect, one change, one pair image.** No batching several fixes into
  one build.
- **Read the rule before guessing.** ROTH.C, then roth-editor as a second
  opinion (`docs/NATIVE_rotheditor_rules.md`). Every rule is traced from
  `render_world_face_list`.
- **Write each finding into the document it belongs in, the same day.**
