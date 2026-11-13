#!/bin/bash
# run.sh MAP x y ang script out.log [frames]   -- runs the original with actprobe
set -u
OB=/home/claude/oracle_build; P=${ACTP_DIR:-/home/claude/actp}   # ACTP_DIR: a private probe dir (plugin.so, games/)
MAP=$1; X=$2; Y=$3; A=$4; SCRIPT=$(realpath $5); OUT=$(realpath -m $6); FR=${7:-400}
DATA="/mnt/user-data/uploads/Realms of the Haunting/DATA"
RAW=$(find "$DATA/M" -maxdepth 1 -iname "$MAP.RAW" | head -1)
[ -d $OB/games/$MAP ] || $OB/oracle_shot.sh $MAP $X $Y 0 $P/warm.png >/dev/null 2>&1
G=$P/games/$MAP
if [ ! -d "$G" ]; then mkdir -p $P/games; cp -a $OB/games/$MAP "$G"; fi
mkdir -p "$G/mods/zactprobe"; cp -f $P/plugin.so "$G/mods/zactprobe/plugin.so"
if [ "$A" = "-" ]; then POSE=""; else POSE=$(python3 $OB/oracle_pose.py "$RAW" $X $Y $A "${ORACLE_SECTOR:-}" 2>/dev/null) || { echo "pose failed"; exit 3; }; fi
SYMS=$P/syms.txt; [ -s $SYMS ] || nm $OB/rothc/roth_c/roth | awk '$2=="T"||$2=="t"{printf "%s=%s\n", $3, $1}' > $SYMS
exec 9>/tmp/oracle_shot.lock; flock 9
( cd "$G" && if [ -z "$POSE" ]; then unset ORACLE_POSE; fi; ACTP_SCRIPT=$SCRIPT ACTP_OUT=$OUT ACTP_SYMS="$SYMS" ORACLE_POSE="$POSE" ORACLE_PAINT=0 ORACLE_OUT="$P/last.oracle.bin" ORACLE_FRAMES=$FR \
    timeout 300 $OB/rothc/roth_c/roth --game-dir . --headless > "$OUT.enginelog" 2>&1 )
echo "rc=$?"
flock -u 9
