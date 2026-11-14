#!/bin/bash
# run.sh MAP script out.log [frames]  -- runs the ORIGINAL (ROTH.C headless) with moveprobe at the map's own start.
# MOVP_DIR: the probe dir (plugin.so, games/), default /home/claude/movp. Paths are the oracle workspace's.
set -u
OB=/home/claude/oracle_build; P=${MOVP_DIR:-/home/claude/movp}
MAP=$1; SCRIPT=$(realpath $2); OUT=$(realpath -m $3); FR=${4:-2000}
mkdir -p $P
[ -d $OB/games/$MAP ] || $OB/oracle_shot.sh $MAP 0 0 0 $P/warm.png >/dev/null 2>&1
G=$P/games/$MAP
if [ ! -d "$G" ]; then mkdir -p $P/games; cp -a $OB/games/$MAP "$G"; fi
rm -rf "$G/mods/zactprobe"; mkdir -p "$G/mods/zmoveprobe"; cp -f $P/plugin.so "$G/mods/zmoveprobe/plugin.so"
SYMS=$P/syms.txt; [ -s $SYMS ] || nm $OB/rothc/roth_c/roth | awk '$2=="T"||$2=="t"{printf "%s=%s\n", $3, $1}' > $SYMS
exec 9>/tmp/oracle_shot.lock; flock 9
( cd "$G" && unset ORACLE_POSE; MOVP_SCRIPT=$SCRIPT MOVP_OUT=$OUT MOVP_SYMS="$SYMS" ORACLE_PAINT=0 ORACLE_OUT="$P/last.oracle.bin" ORACLE_FRAMES=$FR \
    timeout 300 $OB/rothc/roth_c/roth --game-dir . --headless > "$OUT.enginelog" 2>&1 )
echo "rc=$?"
flock -u 9
