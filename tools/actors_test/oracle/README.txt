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
