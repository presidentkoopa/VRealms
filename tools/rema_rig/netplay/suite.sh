#!/bin/bash
# THE NETPLAY REGRESSION SUITE (ROADMAP step 3, "freeze it"): one command, a fixed list of
# scenarios, GREEN or RED per scenario, exit 1 if any is red. Every engine change -- the
# renderer rebuild above all -- must leave this green. Plain freedoom2 scenarios only, so it
# runs on any rig that has the engine (`tools/linux_rig/setup.sh`); the BD22 net set is a
# separate, slower pass (NETSET=1, needs /home/claude/netmods and ~6 GB).
#
#   bash tools/netplay/suite.sh            # ~12 min on the rig
#   NETSET=1 bash tools/netplay/suite.sh   # adds the net-set scenarios
#   ONLY="plain room" bash tools/netplay/suite.sh
#
# A scenario is green when the machines' sync logs agree tic for tic (syncdiff IN SYNC) AND its
# own check holds. The checks are deliberately about the CONTRACT (the pawn moved by the room
# steps on every machine; the clamp lines are identical; the level changed; the save reloaded),
# not about reference logs: the replay drifts against gametic with the lobby length, so a
# byte-for-byte reference would go red on nothing. Results land in run/suite_<scenario>/.
H=$(cd "$(dirname "$0")" && pwd); cd "$H"
MODSET="$(ls /home/claude/netmods/*.pk3 2>/dev/null | tr '\n' ' ')"
red=0; results=()

verdict() { # name ok why
  if [ "$2" = 1 ]; then results+=("GREEN  $1"); else results+=("RED    $1: $3"); red=1; fi
}
# IN SYNC, or clean until the end-of-run quit: the host's last tics never reach the guests, so
# syncdiff ends the comparison there ("clean through tic N"); N must be at or past the quit tic
insync() { local f="$H/run/$1/suite.out" t=$2
  grep -q "^IN SYNC" "$f" && return 0
  local n=$(sed -n 's/.*clean through tic \([0-9]*\)).*/\1/p' "$f" | tail -1)
  [ -n "$n" ] && [ "$n" -ge "$t" ]; }
want() { case " ${ONLY:-all} " in *" all "*|*" $1 "*) return 0;; esac; return 1; }
run() { # name players tics [VAR=value...]: run.sh with that env; $EXTRA = extra engine args
  local n=$1 p=$2 t=$3; shift 3
  env "$@" bash "$H/run.sh" "suite_$n" "$p" "$t" $EXTRA > "$H/run/suite_$n.tmp" 2>&1
  mkdir -p "$H/run/suite_$n"; mv "$H/run/suite_$n.tmp" "$H/run/suite_$n/suite.out"
}
# how far player P's pawn x moved over its stepped commands in log L: x before the first command
# with head.roomdx != 0 (P-line field 28) to x at the last such command
roomtravel() { awk -v p="$2" '$1=="P" && $2==p { if ($28 != 0) { if (!s) { s=1; x0=prev }; x1=$3 }; prev=$3 }
  END { if (s) printf "%.2f\n", x1 - x0; else print "none" }' "$1"; }

mkdir -p "$H/run"

if want plain; then
  run plain 2 600
  insync suite_plain 600 && verdict plain 1 || verdict plain 0 "not in sync"
fi

if want stress; then
  run stress 2 700 STRESS=1
  insync suite_stress 700 && verdict stress 1 || verdict stress 0 "not in sync"
fi

if want room; then
  # a standing player takes 60 fake room steps west: its pawn must move by them on every machine
  run room 3 700 STILL=2 P2CMD="wait 400; netplay_roomtest -60"
  ok=0; why=""
  if insync suite_room 700; then
    # x just before the first stepped command vs at the last one (field 28 = head.roomdx); the
    # start spot has a wall ~18 units west, so 10 units is the bar, not 60
    r=$(roomtravel run/suite_room/p0.sync 2); rb=$(roomtravel run/suite_room/p2.sync 2)
    moved=$(awk -v a="$r" -v b="$rb" 'BEGIN{print (a <= -10 && a == b) ? 1 : 0}')
    [ "$moved" = 1 ] && ok=1 || why="p2 moved $r on p0, $rb on p2 over its room steps (want <= -10, equal)"
  else why="not in sync"; fi
  verdict room $ok "$why"
fi

if want clamp; then
  # a standing player fires with a fake hand 1500 units through the level: every machine must
  # print the same clamp lines, and at least one
  run clamp 3 700 STILL=2 P0CMD="vr_shotclamp_debug 1" P1CMD="vr_shotclamp_debug 1" \
      P2CMD="vr_shotclamp_debug 1; wait 400; netplay_handtest 1500"
  ok=0; why=""
  if insync suite_clamp 700; then
    l0=$(grep "shotclamp: p2" run/suite_clamp/p0.stdout | md5sum); l1=$(grep "shotclamp: p2" run/suite_clamp/p1.stdout | md5sum)
    l2=$(grep "shotclamp: p2" run/suite_clamp/p2.stdout | md5sum); c=$(grep -c "shotclamp: p2" run/suite_clamp/p0.stdout)
    if [ "$c" -gt 0 ] && [ "$l0" = "$l1" ] && [ "$l1" = "$l2" ]; then ok=1; else why="clamp lines: $c on p0, lists differ or empty"; fi
  else why="not in sync"; fi
  verdict clamp $ok "$why"
fi

if want netem; then
  run netem 2 600 NETEM="--delay 80 --jitter 20 --loss 2"
  insync suite_netem 600 && verdict netem 1 || verdict netem 0 "not in sync"
fi

if want level; then
  # host changes the map, both vote ready; the game must reach MAP02 and stay in sync
  run level 2 700 P0CMD="wait 150; changemap map02; wait 120; netplay_ready" P1CMD="wait 280; netplay_ready"
  ok=0; why=""
  if insync suite_level 700; then
    # maptime (T line field 3) restarts from 1 when the next map runs
    lvl() { awk '$1=="T" {if (prev > $3) c++; prev=$3} END{print c+0}' "$1"; }
    m=$(lvl run/suite_level/p0.sync); m1=$(lvl run/suite_level/p1.sync)
    [ "$m" -gt 0 ] && [ "$m1" -gt 0 ] && ok=1 || why="MAP02 never reached (level changes seen: p0 $m, p1 $m1)"
  else why="not in sync"; fi
  verdict level $ok "$why"
fi

if want load; then
  # save at 305, netplay_load at 500: both machines must be in sync through the reload
  run load 2 800 P0CMD="wait 305; save netsave_suite; wait 195; netplay_load netsave_suite"
  ok=0; why=""
  if insync suite_load 800; then
    sv=$(find run/suite_load/home0 -name 'netsave_suite*' | head -1)
    [ -n "$sv" ] && ok=1 || why="the save was never written"
  else why="not in sync"; fi
  verdict load $ok "$why"
fi

if want leave; then
  # the guest quits mid-game; the host must not report a consistency failure for it
  run leave 2 500 P1CMD="wait 300; quit"
  ok=1; why=""
  grep -q "resync started" run/suite_leave/p0.stdout && { ok=0; why="the host had to resync after the guest left"; }
  verdict leave $ok "$why"
fi

if want latejoin; then
  # a third player joins the running game (snapshot + DEM_NETADDCLIENT): in sync after it arrives
  run latejoin 3 700 LATE=2 LATEDELAY=40
  ok=0; why=""
  if insync suite_latejoin 700; then
    n=$(grep -c "^T " run/suite_latejoin/p2.sync); [ "$n" -gt 100 ] && ok=1 || why="the late joiner logged only $n tics"
  else why="not in sync"; fi
  verdict latejoin $ok "$why"
fi

if want hostleave; then
  # the host quits at 300; the arbiter moves and the two survivors must still agree afterwards
  run hostleave 3 600 P0CMD="wait 300; quit"
  ok=0; why=""
  a=$(grep -c "^T " run/suite_hostleave/p1.sync); b=$(grep -c "^T " run/suite_hostleave/p2.sync)
  if [ "$a" -gt 500 ] && [ "$b" -gt 500 ]; then
    # the tic in flight when the host vanished runs with different command sets on the two
    # survivors (known: the consistency check catches it and the auto-resync heals it, which is
    # the contract); what must hold is agreement from tic 400 on
    python3 "$H/syncdiff.py" --from 400 run/suite_hostleave/p1.sync run/suite_hostleave/p2.sync > run/suite_hostleave/survivors.out 2>&1
    if grep -q "^IN SYNC" run/suite_hostleave/survivors.out; then ok=1
    else n=$(sed -n 's/.*clean through tic \([0-9]*\)).*/\1/p' run/suite_hostleave/survivors.out | tail -1); [ -n "$n" ] && [ "$n" -ge 600 ] && ok=1 || why="survivors diverged: $(grep -m1 '^tic' run/suite_hostleave/survivors.out)"; fi
  else why="survivors stopped early (p1 $a, p2 $b tics)"; fi
  verdict hostleave $ok "$why"
fi

if want four; then
  run four 4 600
  insync suite_four 600 && verdict four 1 || verdict four 0 "not in sync"
fi

if [ -n "$NETSET" ] && [ -n "$MODSET" ]; then
  if want netset; then
    EXTRA="+gl_precache 0" run netset 2 700 MODS="$MODSET" STRESS=1
    insync suite_netset 700 && verdict netset 1 || verdict netset 0 "not in sync"
  fi
  if want netset_room; then
    # no STRESS here (a summoned monster east of the start spot blocked the steps once), but god:
    # the host's replay fires a minigun around the start and a dead player's steps are dropped
    EXTRA="+gl_precache 0 +sv_cheats 1" run netset_room 2 700 MODS="$MODSET" STILL=1 P1CMD="wait 10; god; wait 390; netplay_roomtest 60"
    ok=0; why=""
    if insync suite_netset_room 700; then
      r=$(roomtravel run/suite_netset_room/p0.sync 1); rb=$(roomtravel run/suite_netset_room/p1.sync 1)
      moved=$(awk -v a="$r" -v b="$rb" 'BEGIN{print (a >= 10 && a == b) ? 1 : 0}')
      [ "$moved" = 1 ] && ok=1 || why="p1 moved $r on p0, $rb on p1 over its room steps (want >= 10, equal)"
    else why="not in sync"; fi
    verdict netset_room $ok "$why"
  fi
fi

echo "== netplay suite ($(cat "$H/../linux_rig/bin/SOURCE" 2>/dev/null || echo 'local build')) =="
printf '%s\n' "${results[@]}"
[ $red = 0 ] && echo "ALL GREEN" || echo "RED"
exit $red
