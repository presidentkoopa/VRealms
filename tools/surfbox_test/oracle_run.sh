#!/bin/bash
# oracle_run.sh MAP script out.log -- builds boxprobe.c and runs one script through the ORIGINAL (ROTH.C headless).
# Work dir: $SURFBOX_WORK (default /tmp/surfbox_test); nothing is written into the repo.
# Holds /tmp/oracle_shot.lock around the run (the host's /roth_fb shm is machine-wide).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OB=${ORACLE_BUILD:-/home/claude/oracle_build}
W=${SURFBOX_WORK:-/tmp/surfbox_test}; mkdir -p "$W"
MAP=$(echo "$1" | tr a-z A-Z); S=$(realpath "$2"); OUT=$(realpath -m "$3")
[ -s "$W/plugin.so" ] && [ "$W/plugin.so" -nt "$HERE/boxprobe.c" ] || \
	$OB/bin/i386-musl-gcc -shared -fPIC -std=gnu11 -O1 -Wall -I$OB/rothc/sdk/include -o "$W/plugin.so" "$HERE/boxprobe.c" || exit 1
[ -s "$W/syms.txt" ] || nm $OB/rothc/roth_c/roth > "$W/syms.txt"
[ -d $OB/games/$MAP ] || $OB/oracle_shot.sh $MAP 0 0 0 "$W/warm.png" >/dev/null 2>&1
G="$W/games/$MAP"
[ -d "$G" ] || { mkdir -p "$W/games"; cp -a $OB/games/$MAP "$G"; }
mkdir -p "$G/mods/boxprobe"; cp -f "$W/plugin.so" "$G/mods/boxprobe/plugin.so"
exec 9>/tmp/oracle_shot.lock; flock 9
rm -f "$OUT"
( cd "$G" && unset ORACLE_POSE ORACLE_PITCH; SB_SCRIPT="$S" SB_OUT="$OUT" SB_SYMS="$W/syms.txt" SB_NOCHAINS=${SB_NOCHAINS:-0} \
	ORACLE_PAINT=0 ORACLE_OUT="$W/last.bin" ORACLE_FRAMES=1000000 \
	timeout ${SB_TIMEOUT:-900} $OB/rothc/roth_c/roth --game-dir . --headless > "$OUT.enginelog" 2>&1 )
rc=$?
flock -u 9; exec 9>&-
rm -f "$W/last.bin"
grep -q '^END' "$OUT" 2>/dev/null || { echo "oracle_run.sh: the original did not finish the script (rc=$rc, see $OUT.enginelog)" >&2; tail -3 "$OUT.enginelog" >&2; exit 1; }
