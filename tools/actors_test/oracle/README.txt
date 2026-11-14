Differential test of src/roth/roth_actors* against the ORIGINAL (ROTH.C, the
headless oracle build of 2026-11-12 at /home/claude/oracle_build). Paths in the
scripts are that workspace's; nothing here is part of the engine build, and
nothing the scripts write may go into the repo.

  actprobe.c   a ROTH.C mod. Drives the original from a script ("<frame> op"
               for gameplay frames, "<100000+tick> op" for 70 Hz ticks) and logs
               its state in the format ActorWorld::Dump/DumpTick print:
               objects, both pools, the generators, player health; and with
               "ticktrace 1", every tick: the state before it, and every
               collision question the rules asked (COL/SWP) with the answer,
               every animated draw of a pooled object (ANIM), every wake.
               Overrides update_dynamic_entities, move_entity_with_collision,
               find_sector_and_collide, sweep_move_with_collision,
               play_entity_sound, begin_object_command_chain and
               select_surface_anim_frame (all pass through to the original).
  run.sh       MAP x y angle512|- script out.log [frames]: run the original.
  diffcmds.sh  MAP: every 0x16/0x3c/0x3a/0x33 record of the map through the
               original and through actors_test, full dumps compared.
  cmpticks.py  orig.log port.log: the per-tick states compared.
  scenario_*   the tick scripts the report quotes.

Build the probe: $OB/bin/i386-musl-gcc -shared -fPIC -std=gnu11 -O1 \
  -I$OB/rothc/sdk/include -o plugin.so actprobe.c
Replay: actors_test DATA MAP <(echo "1 replay start") orig.log > port.log
        python3 cmpticks.py orig.log port.log
        ("1 replay start simple" also measures SimpleCollision's answers.)

Added for the view, the wake and the creature art (2026-11-12):

  The probe also overrides build_sector_render_tree_recursive (VIS: every
  render tree of a frame -- root, window, camera, the sectors visited; SECH:
  the live sector heights; DOORS: the door pool), resolve_face_surface_id
  (FACE: a slotted creature's state, flags, pain byte, facing, the view
  angle, its view x and depth, the view index, and the picture and pain byte
  it came back with) and advance_das_sprite_animation_frame (ADV: one drawn
  step of a standard animated picture, with the block's once-a-frame gate).
  ANIM lines now carry the decoded frame (shown, W, H) and an FNV-1a hash of
  its pixels.

  actors_test ops that read these (oracle log as the 4th argument):
    viscmp / rootcmp / wakecmp   the view tree, its roots, the wakes
    facecmp                      CreaturePicture + ViewIndex against FACE
    advcmp                       StepPictureClock against ADV
    creaturerec <n>              a creature record's picture words
  gen_face_scenario.py         pose script -> pose script that forces every
                               slot through all states and pain (FACE traces)
  shot.sh                      run.sh that also keeps the frame the oracle
                               plugin captured (MAP x y a script out frames);
                               oracle_png.py out.bin out.ppm out.png
  scenario_study1_ghoul_shots.txt   STUDY1 at -864,3412 facing 466: the ghoul
                               171.16 wakes, walks, attacks; four shots of item
                               61 at frames 250-310 kill it. Its frames 60, 140,
                               280, 420, 560 are the originals for the bridge
                               script tools/claude_bridge/cfg/actors_ghoul_shots.cfg.

Weapons (2026-11-12): probe ops "weapons" (apply_weapon_action_attributes for
every item index into a 0xaa-filled four-slot table: WEAP lines) and
"dbrec <index>" (an item record as the running game holds it). actors_test
op weaponcmp compares ParseWeaponActions with the WEAP lines. Two fields are
not the file's: ops 0x19 and 0x21 are sound keys the original rewrites in
place at start-up (resolve_dbase100_sound_ids, audio.c:1896-1919), so those
bytes are taken from the original and counted.

The fire state machine (2026-11-12): probe overrides update_player_tick (PT,
logged after it with the frame scale it set, then WPN and INVS),
trigger_weapon_fire (TRIG, WPN, INVS), tick_weapon_hud_ammo_anim (RC),
rebuild_weapon_inventory_list (RWL, INVS) and spawn_projectile_from_aim
(SHOT). Op "pull" pulls the trigger at the start of the next
update_player_tick: frame ops run from the timer, and a trigger fired from
there lands in the middle of other writes to the log.
The log has two writers on the game thread: the overrides, and on_frame_game,
which the SIGALRM tick also reaches (vsync_timer_tick -> the software
cursor's int 0x33 poll -> frame_pump). musl's stdio is not re-entrant
(fflush zeroes wpos/wbase/wend, src/musl/src/stdio/fflush.c), so a tick
landing inside an fprintf or fflush wrote chunks twice or dropped bytes.
Before the fix that showed up as W150's 165/2506 "differences" (a duplicated
PT/RC/TICK 364 chunk replayed as an extra tick) and W19/W38/W68's single
ones (a byte missing from the original's line); W221's one segfault at tick
67 is not reproduced (13 reruns with the old probe, 12 with the new) and is
probably the same race. Every write section of the probe now holds SIGALRM
blocked (log_on/log_off); the tick is delivered when it is released. The
replay still counts lines of the wrong shape as "garbled"; with the fix there
are none. The original is deterministic at the tick level (two runs:
identical TICK/GLB/PRJ/AOB/SHOT/TRIG lines and identical sequences of WPN and
INVS changes); only the number of drawn frames per tick (PT/RC lines with
fts=0) follows the wall clock, and the replay is driven by those lines.
run.sh and diffcmds.sh take ACTP_DIR (a private probe dir with its own
plugin.so, games/ and cmds/), diffcmds.sh takes ACTORS_TEST (the port binary),
so a worktree can test its own build without touching /home/claude/actp.
diffcmds.sh drops the WPN/INVS lines the dump op now writes: the command
replay does not run the weapon.
weapons/swpn_<index>.txt: STUDY1 at -864,3412 facing 466, give the weapon
(and three of its ammo item when it has one), dump, pull every 12 frames from
150 to 438. Replay: actors_test DATA STUDY1 <(echo "0 replay start weapon")
orig.log > port.log; python3 cmpticks.py orig.log port.log. The port runs its
own weapon from the PT/TRIG/RC/RWL lines, compares every WPN/INVS line, and
its shots are its own (SHOT lines are not injected).

The VR fist's attack (2026-11-13, punch.sh): probe op "face S I" turns the original's player to object S.I
(angle = 512 - its own atan2_bearing: the player's angle turns the other way from the bearing) and logs FACETO;
punch.sh stands Adam at (-760, 3640), off the ghoul 171.16's axes (facing angle 79), and faces + pulls every 12
frames (his fists, 14 shots, the ghoul killed). The first version stood him due south of the ghoul (bearing 0),
where the two conventions agree, and so missed a mirrored aim; actors_test "fistshot" found it. actors_test "replay start punch <delta>"
replays it with the port's player turned <delta> away and every pull aimed at the faced point through
PunchAimAt (roth_actors.h), the engine's fist attack. punch.sh runs six turns and compares everything.
