#!/bin/bash
# run_probe.sh <out.trace>  -- play movies through the ORIGINAL (the ROTH.C oracle build) with gdvprobe.c
# loaded, then the same movies through the port (gdvtool), and compare (compare.py).
# Env: OB (oracle build, default /home/claude/oracle_build), ROTH_DATA (retail DATA folder),
#      GDV_WORK (scratch, default /tmp/gdvprobe -- never inside the repo), GDV_MOVIES (names).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OB=${OB:-/home/claude/oracle_build}
DATA="${ROTH_DATA:-/mnt/user-data/uploads/Realms of the Haunting/DATA}"
WORK=${GDV_WORK:-/tmp/gdvprobe}
OUT=$(realpath -m "${1:?usage: run_probe.sh out.trace}")
ROTHC=$OB/rothc/roth_c/roth
[ -x "$ROTHC" ] || { echo "run_probe: $ROTHC missing (see $OB/build_all.sh)" >&2; exit 1; }
G=$WORK/game; mkdir -p "$G/mods/gdvprobe" "$G/mods/nodata_shim" "$G/DATA"
$OB/bin/i386-musl-gcc -shared -fPIC -std=gnu11 -O1 -Wall -Wextra -I$OB/rothc/sdk/include \
    -o "$G/mods/gdvprobe/plugin.so" "$HERE/gdvprobe.c" || exit 1
cp -f $OB/shim/plugin.so "$G/mods/nodata_shim/plugin.so"
for f in "$DATA"/*; do b=$(basename "$f"); case "$b" in DATA|data|mods) ;; *) ln -sfn "$f" "$G/$b";; esac; done
for d in "$DATA/DATA" "$DATA/data"; do [ -d "$d" ] && for f in "$d"/*; do [ -e "$f" ] && ln -sfn "$f" "$G/DATA/$(basename "$f")"; done; done
[ -e "$G/DATA/FILELIST.TXT" ] || : > "$G/DATA/FILELIST.TXT"
# CONFIG.INI (the sound setup): without it the original runs silent and sets up no movie audio.
CFG=$(dirname "$DATA")/ROTH/CONFIG.INI
rm -f "$G/CONFIG.INI"; [ -e "$CFG" ] && \
  sed -e 's/^SourcePath=.*/SourcePath=./I' -e 's/^DestinationPath=.*/DestinationPath=./I' "$CFG" > "$G/CONFIG.INI"
printf 'SpeechSub=ON\r\nSpeechAud=ON\r\nMovieSub=ON\r\nMovieAud=ON\r\nVideoMode=8\r\nViewSize=0\r\nSoundFXVol=0x80\r\nSpeechVol=0x80\r\nMovieVol=0x80\r\nMusicVol=0x80\r\nMouseSpeed=0x80\r\n' > "$G/ROTH.INI"
nm "$ROTHC" | awk '{print $1" "$3}' > "$WORK/syms.txt"
exec 9>/tmp/oracle_shot.lock; flock 9           # the host's shared framebuffer is machine-global
( cd "$G" && GDV_OUT="$OUT" GDV_SYMS="$WORK/syms.txt" timeout ${GDV_TIMEOUT:-900} "$ROTHC" --game-dir . --headless > "$OUT.log" 2>&1 )
rc=$?
flock -u 9
tail -1 "$OUT" | grep -q '^END$' || { echo "run_probe: the probe did not finish (rc=$rc); log: $OUT.log" >&2; tail -5 "$OUT.log" >&2; exit 1; }
echo "run_probe: $(grep -c '^F ' "$OUT") record lines -> $OUT"
