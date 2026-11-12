#!/bin/bash
# oracle_door_shot.sh -- make the ORIGINAL engine (the ROTH.C oracle build of 2026-11-12, /home/claude/oracle_build)
# open the door in front of the camera with its own dev_open_nearest_door, then capture a frame.
# Build the mod first: $OB/bin/i386-musl-gcc -shared -fPIC -std=gnu11 -O1 -I$OB/rothc/sdk/include -o $P/plugin.so doorprobe.c
# The door opens at gameplay frame atFrame; +15 frames is mid-swing, +200 is fully open (dwell 600).
# The opener position must be within 60 units of the door, in the room sector whose wall the door is.
# usage: oracle_door_shot.sh MAP camx camy ang  openx openy opensectorIndex  atFrame captureFrame out.png
set -u
OB=/home/claude/oracle_build; P=/tmp/dc/probe
MAP=$1; X=$2; Y=$3; A=$4; OX=$5; OY=$6; OS=$7; AT=$8; FR=$9; OUT=$(realpath -m "${10}")
DATA="/mnt/user-data/uploads/Realms of the Haunting/DATA"
RAW=$(find "$DATA/M" -maxdepth 1 -iname "$MAP.RAW" | head -1)
[ -d $OB/games/$MAP ] || $OB/oracle_shot.sh $MAP $X $Y $A /tmp/dc/probe/warm.png >/dev/null 2>&1
G=$P/games/$MAP
if [ ! -d "$G" ]; then mkdir -p $P/games; cp -a $OB/games/$MAP "$G"; mkdir -p "$G/mods/doorprobe"; cp $P/plugin.so "$G/mods/doorprobe/plugin.so"; fi
cp -f $P/plugin.so "$G/mods/doorprobe/plugin.so"
POSE=$(python3 $OB/oracle_pose.py "$RAW" $X $Y $A "${ORACLE_SECTOR:-}" 2>/dev/null) || exit 3
OPOSE=$(python3 $OB/oracle_pose.py "$RAW" $OX $OY 0 $OS 2>/dev/null) || exit 3
set -- $OPOSE; OPENER="$1 $2 $4"
FN=$(nm $OB/rothc/roth_c/roth | awk '$3=="dev_open_nearest_door"{print $1}')
SEL=$(nm $OB/rothc/roth_c/roth | awk '$3=="g_os_sel_base"{print $1}')
T=${OUT%.png}
exec 9>/tmp/oracle_shot.lock; flock 9
rm -f /tmp/roth_screen.ppm "$T.oracle.bin"
( cd "$G" && DOORPROBE_AT=$AT DOORPROBE_OPENER="$OPENER" DOORPROBE_FN=$FN DOORPROBE_SEL=$SEL ORACLE_POSE="$POSE" ORACLE_PAINT=0 ORACLE_OUT="$T.oracle.bin" ORACLE_FRAMES=$FR \
    timeout 120 $OB/rothc/roth_c/roth --game-dir . --headless > "$T.oracle.log" 2>&1 )
[ -s /tmp/roth_screen.ppm ] && cp /tmp/roth_screen.ppm "$T.oracle.ppm"
flock -u 9
grep -h "doorprobe" "$T.oracle.log"
python3 $OB/oracle_png.py "$T.oracle.bin" "$T.oracle.ppm" "$OUT" && rm -f "$T.oracle.ppm" "$T.oracle.bin"
