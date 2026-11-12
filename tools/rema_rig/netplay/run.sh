#!/bin/bash
# N-player netgame on one machine, headless (Xvfb + lavapipe Vulkan, flat mode: the
# MULTIPLAYER path is what is under test, and it does not read a headset).
#   run.sh <name> <players 2..4> <tics> [extra engine args...]
# Player 0 hosts on port 5029; the rest join. Each plays tools/netplay/replay_p<N>.txt into
# its hands and writes run/<name>/p<N>.sync. Then syncdiff.py compares them.
H=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$H/../.." && pwd)
# +snd_backend null: without it a LEVEL CHANGE kills the engine on the rig -- the intermission
# re-inits OpenAL, there is no audio device, al::backend_exception is uncaught, exit 0 with no
# message (found 2026-11-09 with `catch throw`). -nosound alone does not cover that path.
BIN=${BIN:-/home/claude/rig/build}; [ -x "$BIN/doomxr" ] || BIN=$REPO/tools/linux_rig/bin
N=${1:-t}; NP=${2:-2}; T=${3:-400}; shift 3
R=$H/run/$N; rm -rf "$R"; mkdir -p "$R"
export SDL_AUDIODRIVER=dummy
# One Xvfb per instance: a second window on a display freezes the engine already drawing
# there (lavapipe/X11 occlusion), which looked exactly like a netcode stall.
if [ -n "$NETEM" ]; then
  pkill -f netem_proxy.py 2>/dev/null; sleep 0.5
  JOINPORT=5030; (setsid python3 "$H/netem_proxy.py" --listen 5030 --host 127.0.0.1:5029 $NETEM > "$R/netem.log" 2>&1 < /dev/null &)
fi
for ((i=0; i<NP; i++)); do pgrep -f "Xvfb :$((80+i)) " >/dev/null || (setsid Xvfb :$((80+i)) -screen 0 800x600x24 >/dev/null 2>&1 < /dev/null &); done; sleep 1
IWAD=${IWAD:-/usr/share/games/doom/freedoom2.wad}
# MODS="a.pk3 b.pk3" loads a set, in that order, on every instance
# FLAT="1 3" makes those players flatscreen (no replay, so no hands): mixed VR/flat co-op
# STILL="2" makes those players stand still: flat, no replay, no walk script (the subject of a
#   netplay_roomtest: its pawn must move by the room steps alone, on every machine)
# P0CMD="wait 300; foo" P1CMD=... one console command string run by that player only
# LATE="2" LATEDELAY=40: those players start after LATEDELAY seconds and join the running game
# GHOST1=<file> (GHOST2, GHOST3) makes that player a ghost: it plays the recording instead
# STRESS=1: the owner's stress run (2026-10-31) -- monsters on, every player in god mode, and the
#   host summons a dozen mixed monsters at the start (sv_cheats on, so god and summon are allowed)
STRESSCMD=""; STRESSHOST=""
if [ -n "$STRESS" ]; then
  NOMONSTERS=""; STRESSCMD="wait 10; god; "
  STRESSHOST="wait 22; summon ZombieMan; summon ShotgunGuy; summon ChaingunGuy; summon DoomImp; summon DoomImp; summon Demon; summon Spectre; summon LostSoul; summon Cacodemon; summon HellKnight; summon Revenant; summon Arachnotron; "
  set -- "$@" +sv_cheats 1
fi
FILES=""; [ -n "$MODS" ] && FILES="-file $MODS"
QUIT=$((T + 40))
pids=()
for ((i=0; i<NP; i++)); do
  mkdir -p "$R/home$i"
  REPLAY="+vr_replay_play $H/replay_p$i.txt"; EXEC="$(cat "$H/exec_p$i.txt")"; case " $FLAT " in *" $i "*) REPLAY="";; esac
  case " $STILL " in *" $i "*) REPLAY=""; EXEC="echo still";; esac
  PCMD=$(eval echo "\$P${i}CMD"); [ -z "$PCMD" ] && PCMD="echo"
  # god FIRST (a player killed by the fresh monsters respawns without the test weapon)
  [ $i = 0 ] && PCMD="$STRESSCMD$STRESSHOST$PCMD" || PCMD="$STRESSCMD$PCMD"
  G=$(eval echo \$GHOST$i); [ -n "$G" ] && { REPLAY="+ghost_play $G"; EXEC="echo ghost"; }
  NLATE=$(echo $LATE | wc -w)
  # NETEM="--delay 80 --jitter 20 --loss 2": guests join through netem_proxy.py (a bad network in
  # user space; the rig's kernel has no tc netem). Delay is one way, in ms.
  if [ $i = 0 ]; then NET="-host $NP -port 5029 -netautostart $((NP - NLATE))"; else NET="-join 127.0.0.1:${JOINPORT:-5029}"; fi
  case " $LATE " in *" $i "*) sleep ${LATEDELAY:-40};; esac
  ( cd "$BIN" && DISPLAY=:$((80+i)) HOME=$R/home$i timeout -k 5 ${TIMEOUT:-$((T * 6 / 35 + 240))} ./doomxr -iwad "$IWAD" $FILES -nosound -nomusic +snd_backend null -nocrashdialog -nostartscreen \
      $NET +vid_preferbackend 1 +cl_capfps 1 +vid_maxfps 35 -width 400 -height 300 \
      $REPLAY +vr_synclog "$R/p$i.sync" \
      -warp 1 -skill 3 ${NOMONSTERS--nomonsters} +"$EXEC" +"$PCMD" +"wait $QUIT; quit" "$@" > "$R/p$i.stdout" 2>&1; echo "exit $?" > "$R/p$i.exit" ) &
  pids+=($!)
  sleep 2
done
wait "${pids[@]}"
for ((i=0; i<NP; i++)); do printf "p%d: %s, %s tics logged\n" $i "$(cat $R/p$i.exit)" "$(grep -c '^T ' $R/p$i.sync 2>/dev/null)"; done
grep -h "Consistency\|consistency\|desync\|Network game synchronization\|Disconnect" "$R"/p*.stdout | sort | uniq -c | head
python3 "$H/syncdiff.py" "$R"/p*.sync | tail -25
