#!/bin/bash
# run_oracle.sh <out.trace> [mode]  -- run the ORIGINAL (the ROTH.C oracle build) with gameprobe.c loaded.
# Env: OB (oracle build, default /home/claude/oracle_build), ROTH_DATA (retail DATA folder),
#      PROOF_WORK (scratch, default /tmp/gameproof -- never inside the repo),
#      PROOF_SECTIONS (default AIR), PROOF_ACTIONS="lo hi", PROOF_RAWRECS (see rawrecs.py).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OB=${OB:-/home/claude/oracle_build}
DATA="${ROTH_DATA:-/mnt/user-data/uploads/Realms of the Haunting/DATA}"
WORK=${PROOF_WORK:-/tmp/gameproof}
OUT=$(realpath -m "${1:?usage: run_oracle.sh out.trace [mode]}"); MODE=${2:-0}
ROTHC=$OB/rothc/roth_c/roth
[ -x "$ROTHC" ] || { echo "run_oracle: $ROTHC missing (see $OB/build_all.sh)" >&2; exit 1; }
G=$WORK/game; mkdir -p "$G/mods/gameprobe" "$G/mods/nodata_shim" "$G/DATA"
$OB/bin/i386-musl-gcc -shared -fPIC -std=gnu11 -O1 -Wall -Wextra -I$OB/rothc/sdk/include \
    -o "$G/mods/gameprobe/plugin.so" "$HERE/gameprobe.c" || exit 1
cp -f $OB/shim/plugin.so "$G/mods/nodata_shim/plugin.so"
for f in "$DATA"/*; do b=$(basename "$f"); case "$b" in DATA|data|mods) ;; *) ln -sfn "$f" "$G/$b";; esac; done
for d in "$DATA/DATA" "$DATA/data"; do [ -d "$d" ] && for f in "$d"/*; do [ -e "$f" ] && ln -sfn "$f" "$G/DATA/$(basename "$f")"; done; done
[ -e "$G/DATA/FILELIST.TXT" ] || : > "$G/DATA/FILELIST.TXT"
# CONFIG.INI (the sound setup) lives in the install folder beside DATA. Without it the original
# runs with sound disabled, and its loader then rewrites every DBASE100 sound id to 0
# (find_sound_sample_index, renderer.c:11410-11411). PROOF_NOCONFIG=1 runs without it.
CFG=$(dirname "$DATA")/ROTH/CONFIG.INI
# Its two install paths are pointed at the game folder; the sound settings are kept.
rm -f "$G/CONFIG.INI"; [ -z "${PROOF_NOCONFIG:-}" ] && [ -e "$CFG" ] && \
  sed -e 's/^SourcePath=.*/SourcePath=./I' -e 's/^DestinationPath=.*/DestinationPath=./I' "$CFG" > "$G/CONFIG.INI"
printf 'SpeechSub=ON\r\nSpeechAud=ON\r\nMovieSub=ON\r\nMovieAud=ON\r\nVideoMode=8\r\nViewSize=0\r\nSoundFXVol=0x80\r\nSpeechVol=0x80\r\nMovieVol=0x80\r\nMusicVol=0x80\r\nMouseSpeed=0x80\r\n' > "$G/ROTH.INI"
nm "$ROTHC" | awk '$2=="T"||$2=="t"{print $1" "$3}' > "$WORK/syms.txt"
exec 9>/tmp/oracle_shot.lock; flock 9           # the host's shared framebuffer is machine-global
( cd "$G" && PROOF_OUT="$OUT" PROOF_MODE=$MODE PROOF_SYMS="$WORK/syms.txt" \
    timeout ${PROOF_TIMEOUT:-600} "$ROTHC" --game-dir . --headless > "$OUT.log" 2>&1 )
rc=$?
flock -u 9
tail -1 "$OUT" | grep -q '^END$' || { echo "run_oracle: the probe did not finish (rc=$rc); log: $OUT.log" >&2; tail -5 "$OUT.log" >&2; exit 1; }
echo "run_oracle: $(wc -l < "$OUT") lines -> $OUT"
