# Hands and VR scale: where it stopped (2026-11-13, paused by the owner)

## On the branch, built on the owner's PC
- VR scale: Realms' headset metre is anchored on the study's door: 218 units = 2.03 m, so 107.4 units per metre (375bcc07, rothmap.cpp). The owner said "rooms too wide, ceilings too high" at the old 93.5.
  - `roth_vr_units_per_metre <n>` overrides it live. It is not saved; 0 means the door anchor.
  - Verified: the owner's PC log line "VR world scale 107.4 units/metre". Not yet judged in the headset.

## Not merged: `patches/hands_UNMERGED.patch`
The patch is `git diff --binary` against 375bcc07 and applies cleanly there. It is the two agents' work merged together.

**Converter (`tools/fbx_hands/fbx2iqm.py`):**
- Takes only `/home/claude/dxr/models/hands/hand_final.fbx`; no IQM hands were opened.
- Writes `vrealms_iwad/models/hands/hand_r.iqm`, `hand_l.iqm` and the skin texture.
- Hand length 0.19 m. The left hand uses the FBX's own left-hand poses.
- Frames follow RS_Hands' map: 0-10 baked poses, 11-1288 the take, 1289-1299 the holds. The wrist is zeroed in every frame.
- Verified by `iqmverify.py`: the take matches the FBX within 0.0011 deg; the recipe poses within 0.001 deg; 0.19 m on the mesh; handedness and texture correct.
- **Unfinished:** it skipped `tools_export_poses.py:801-1075`, the authored-frame picker. So ready_td/ready_tu (index up rather than on the trigger), pinch (loose) and hold_slide differ from the owner's `wired_poses_sheet.png` (see `tools/fbx_hands/sheets/compare_*`).
  - The agent was re-porting the picker and was STOPPED mid-way at the owner's pause. Its partial edits are only in `/home/claude/wt_fbx2iqm`, not in this patch.
  - Resume by porting TEP:801-1075 with a checklist, rechecking the ceilings (index 65 / middle 60 / ring 65 / pinky 110 against the table's 85/85/90/90; mesh fingertips vs bone tails), regenerating, and comparing the sheets.

**Engine:**
- `src/roth/roth_hands.cpp` spawns `RealmsHandRight` / `RealmsHandLeft` (`realmshands.zs`) in VR only. New FollowHandMode 3/4 ride the controller GRIP pose (`models.cpp`).
- `src/r_data/vr_handpose.cpp` poses the 15 finger joints per hand from:
  - the analog trigger and grip, plus the capacitive touches (owner: "react to capacitive touch so I can point and thumb and grab");
  - index off the trigger is straight; touching it gives ready_td's 30 deg; pulling blends to the fist;
  - thumb off every surface gives grip_tu's thumb up; touching gives the ready_td or point thumb.
- Ported from UZDXREMA vr_avatar_pose.cpp, with a 33-item checklist at the top of the file, plus touch bindings for Index, Vive and WMR in `vk_openxrdevice.cpp`.
- The hand is 0.19 m at any units per metre: MODELDEF Scale 1/93.5 on the metre-based grip path.
- Tests: `tools/hands_test/run.sh` gives 85/85 and 44/44 (each named input converges to its baked frame, 0.0000 mm).
- **Not built yet; nothing seen in the engine.**
- Flat captures: run `packiwad`, then `run -Exec tools/claude_bridge/cfg/hands_flat.cfg`; the test command is `roth_handtest <pose>`.
- Noted, not fixed: the Index profile binds left x/click and y/click, which the Index does not have.

**To land it:**
1. Finish the converter's picker.
2. Apply the patch.
3. Build on the PC, then `packiwad`.
4. Run the flat captures and compare them with the sheets.
5. The owner checks it in the headset.

## Open questions for the owner
- May the wrist ever be animated? Currently the controller owns it in every frame.
- The weapon in the posed hand: RealmsHeld still rides the aim pose. That is a later step; see the engine report in the patch's `vr_handpose.cpp` header.
