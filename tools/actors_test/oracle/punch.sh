#!/bin/bash
# punch.sh -- THE VR FIST'S ATTACK against the ORIGINAL's attack on the same target (owner 2026-11-13, Q6: "a fist
# swung into a monster at speed fires the ORIGINAL's attack where the fist lands, with the original's damage").
#
# The original (ROTH.C headless oracle, actprobe.c): STUDY1, Adam (his fists, item 68) standing 50 units from the
# ghoul 171.16; every 12 frames from 150 to 570 he FACES it by the original's own atan2_bearing ("face", logged
# FACETO with the ghoul's x, y) and pulls the trigger (trigger_weapon_fire(0, 0), the PT/TRIG/WPN trace).
# The port (actors_test "replay start punch <delta>"): the same run, but the port's player stands TURNED <delta>
# (512 per turn) away from where the original faced, and each pull is the fist's: TriggerFire, then the shot aimed
# at the latched point the original faced through PunchAimAt (roth_actors.h; the engine's WeaponTick does the same
# with the fist's contact), until the shot leaves. Every tick's actor / projectile / global state (cmpticks.py:
# the ghoul's health to its death included), every weapon state, and every collision question are compared.
# Pass: for every delta, 0 differ, 0 question mismatches, and every one of the original's shots aimed.
# Outputs under $PUNCH_WORK (default /tmp/punch_test); nothing into the repo.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OB=${ORACLE_BUILD:-/home/claude/oracle_build}
W=${PUNCH_WORK:-/tmp/punch_test}; mkdir -p "$W"
D="/mnt/user-data/uploads/Realms of the Haunting/DATA"
$OB/bin/i386-musl-gcc -shared -fPIC -std=gnu11 -O1 -I$OB/rothc/sdk/include -o "$W/plugin.so" "$HERE/actprobe.c" || exit 1
sh "$HERE/../build.sh" "$W/port" > "$W/build.log" 2>&1 || { tail -5 "$W/build.log"; exit 1; }
if [ ! -s "$W/orig.log" ] || [ "${PUNCH_RERUN:-0}" = 1 ]; then
	ACTP_DIR="$W" bash "$HERE/run.sh" STUDY1 ${PUNCH_POSE:--760 3640 0} "$HERE/scenario_study1_punch.txt" "$W/orig.log" 640 > "$W/run.out" 2>&1
fi
[ -s "$W/orig.log" ] || { echo "the original wrote nothing (see $W/run.out, $W/orig.log.enginelog)"; exit 1; }
shots=$(grep -c '^SHOT' "$W/orig.log")
hp=$(grep '^ACT .* obj=171.16 ' "$W/orig.log" | sed 's/.* hp=\([0-9-]*\) .*/\1/' | uniq | tr '\n' ' ')
echo "the original: $(grep -c '^TRIG' "$W/orig.log") pulls facing the ghoul, $shots shot(s); the ghoul's health: $hp"
bad=0
for d in 0 37 -90 200 -255 255; do
	"$W/port/actors_test" "$D" STUDY1 <(echo "0 replay start punch $d") "$W/orig.log" > "$W/port_$d.log" 2>&1
	cmp=$(python3 "$HERE/cmpticks.py" "$W/orig.log" "$W/port_$d.log" | head -1)
	q=$(grep '^REPLAY' "$W/port_$d.log" | sed 's/.*question-mismatches=//')
	wpn=$(grep '^WEAPON' "$W/port_$d.log" | sed 's/.*compared \([0-9]*\) (differ \([0-9]*\).*/\1 compared, \2 differ/' | head -1)
	aimed=$(grep '^PUNCH ' "$W/port_$d.log" | sed 's/.*: \([0-9]*\) shot.*/\1/')
	echo "turned $d: $cmp; collision questions mismatched $q; weapon states $wpn; shots aimed at the point $aimed of $shots"
	case "$cmp" in *"differ 0"*) ;; *) bad=1;; esac
	[ "$q" = 0 ] && [ "$aimed" = "$shots" ] && case "$wpn" in *", 0 differ"*) true;; *) false;; esac || bad=1
done
[ $bad = 0 ] && echo "PUNCH: the fist's aimed attack is the original's attack on that target at every turn" || echo "PUNCH: DIFFERENCES (see $W)"
exit $bad
