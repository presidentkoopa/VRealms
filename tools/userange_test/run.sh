#!/bin/bash
# run.sh MAP script out.log -- builds rangeprobe.c and runs one script through the ORIGINAL (ROTH.C headless).
# Work dir: $USERANGE_WORK (default /tmp/userange_test); nothing is written into the repo.
# Holds /tmp/oracle_shot.lock around the run (the host's /roth_fb shm is machine-wide).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OB=${ORACLE_BUILD:-/home/claude/oracle_build}
W=${USERANGE_WORK:-/tmp/userange_test}; mkdir -p "$W"
MAP=$(echo "$1" | tr a-z A-Z); S=$(realpath "$2"); OUT=$(realpath -m "$3")
$OB/bin/i386-musl-gcc -shared -fPIC -std=gnu11 -O1 -Wall -I$OB/rothc/sdk/include -o "$W/plugin.so" "$HERE/rangeprobe.c" || exit 1
[ -s "$W/syms.txt" ] || nm $OB/rothc/roth_c/roth > "$W/syms.txt"
[ -d $OB/games/$MAP ] || $OB/oracle_shot.sh $MAP 0 0 0 "$W/warm.png" >/dev/null 2>&1
G="$W/games/$MAP"
[ -d "$G" ] || { mkdir -p "$W/games"; cp -a $OB/games/$MAP "$G"; }
mkdir -p "$G/mods/rangeprobe"; cp -f "$W/plugin.so" "$G/mods/rangeprobe/plugin.so"
exec 9>/tmp/oracle_shot.lock; flock 9
rm -f "$OUT"
( cd "$G" && unset ORACLE_POSE ORACLE_PITCH; RP_SCRIPT="$S" RP_OUT="$OUT" RP_SYMS="$W/syms.txt" \
	ORACLE_PAINT=0 ORACLE_OUT="$W/last.bin" ORACLE_FRAMES=1000000 \
	timeout ${RP_TIMEOUT:-600} $OB/rothc/roth_c/roth --game-dir . --headless > "$OUT.enginelog" 2>&1 )
rc=$?
flock -u 9; exec 9>&-
grep -q '^END' "$OUT" 2>/dev/null || { echo "run.sh: the original did not finish the script (rc=$rc, see $OUT.enginelog)" >&2; exit 1; }
echo "run.sh: $(grep -c '^PK' "$OUT") picks -> $OUT"
