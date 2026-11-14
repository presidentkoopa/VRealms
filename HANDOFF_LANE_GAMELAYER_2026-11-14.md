# Handoff: lane/gamelayer, 2026-11-14 (after the chapter 1 second pass and the chapter 2 dossier)

Read CLAUDE.md first, then this, then `docs/VR_CHANGES_LEDGER.md` (every change, decision, commit, evidence,
chapters) and `docs/VR_CHAPTER02_SIGNS_AND_PORTENTS.md` section 6.

## Where things stand
- **Chapter 1 is built and seen in FLAT on the owner's PC** (bridge logs and captures; nothing in the headset yet):
  gestures (knock, attempts, ward tingle, physical punch with the original's damage, tall wicks), the struck match,
  the Lamp hip hook, the item in the item hand, the Book in both hands, the niche light, item-to-the-eyes examine,
  record surface boxes (1:1), the original's player movement (1:1, 161/161 lines vs the engine-free port; the port
  0 differ over 9,686 ticks vs the running original), guns along the gun, quicksave/quickload, the off-wrist mount and
  the low-health red edge. Built but VR-only, unseen: world-locked movies (`movie_world_vr.cfg`), snap 45, real
  crouch, teleport, roomscale through the original's collision. The ledger's "Flat PC evidence" table has the details.
- **1:1 bugs found and fixed on the way (flat too):** examine records with no chain were never bound (no examine line
  ever played); a use on a face fired every binding instead of the original's one record; the 0x1d light step is
  signed (23 down-ramps in 9 maps rose); the flat frame presented the image before the last post-process pass
  (vk_renderdevice.cpp); MSVC link trap: forward-declare `class player_t`, never `struct`, in headers p_*.cpp include.
- **The Book never gives the silver key on its first look in the original** (its OnInspect's if-flag 4 / !if-flag 4).
- **Chapter 2 dossier:** `docs/VR_CHAPTER02_SIGNS_AND_PORTENTS.md` (65 entries, 298 records, top 10, needs, 32
  questions). CHAPTERS.md chapter 2 corrected. Questions 1-12 answered (section 6); **13-32 NOT YET ASKED**.

## Next steps, in order
1. **Ask the owner chapter 2 questions 13-32** (AskUserQuestion, four at a time, all of them; he insists on being asked
   everything, not a few). Use the dossier's section 6 wording and recommendations.
2. **Check the statuette plates (Q2/M2):** the owner believes the flat game auto-assigns a statuette to a plate. Read
   the records/boxes and run it in the original: auto-assign, or a plate per statuette? If auto-assign, the VR release
   anywhere on the dais does the same. If specific, tell the owner.
3. Build chapter 2 by lanes as chapter 1 was (agents in worktrees, cherry-pick onto the lane, PC runs through the
   bridge, evidence in the ledger): the sarcophagus statuettes, masks, Sword (physical swing), Shield (always off
   forearm), weapon body slots + inventory, the **two-handed Shotgun now** (reload/pump later with meshes), the Staff
   (read its WeaponAction; VR: swing = melee, trigger = projectile), the choice menu world-locked where the movie stood,
   **the words of passing SPOKEN (speech recognition of the original's choice texts; the original's choice fires)**.
4. Headset pass by the owner: the mount's readability (the original's line is one ~21 cm row), candle light strength,
   movies, snap/crouch/teleport, gun aim pitch sign (three readers use the opposite sign: BillboardHandFrame,
   roth_vrpick's controller-ray fallback, roth_interface PointerRays / roth_runtime UseByHand).

## Test cfgs that run on the owner's PC (bridge, `run` with `-Exec tools/claude_bridge/cfg/X.cfg` on ONE line)
gestures_flat, held_flat, held_eyes_flat, surfbox_flat, move_flat (compare with tools/move_test/cmp_pc.sh LOG run),
gunaim_flat, mount_flat, mount_close_flat, quicksave_flat, candles_flat; movie_world_vr is for the headset.
The bridge `.req` format: one argument per line, a flag and its value on the SAME line (`-Exec path`), e.g.
`run\n-Exec tools/claude_bridge/cfg/gestures_flat.cfg\n-TimeoutSec 240\n`. A run ends "EXIT=5 no engine log" even
when it worked: read `build-dxr/RelWithDebInfo/doomxr-log.txt` after each run (each run overwrites it).

## Worktrees (all detached; their commits are on the lane)
wt_gest, wt_held, wt_surfbox, wt_move, wt_mount, wt_ch02 hold untracked PC logs (*_pc_run_*.log): do not commit them.
Disk is tight (~5 GB free): remove a worktree only after `git cherry` shows its commits are on the lane.
