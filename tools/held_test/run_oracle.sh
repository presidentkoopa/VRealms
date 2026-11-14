#!/bin/bash
# run_oracle.sh MAP SCRIPT OUT -- run the ORIGINAL (ROTH.C, headless, /home/claude/oracle_build) on MAP with
# heldprobe.c driving it from SCRIPT; the probe's lines go to OUT. Game folders and the built probe go under
# $W (default /tmp/heldw), never into the repo (they are made from retail data). tools/candles_test/run_oracle.sh
# with the held objects' probe.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OB=${OB:-/home/claude/oracle_build}
W=${W:-/tmp/heldw}
D=${ROTH_DATA:-"/mnt/user-data/uploads/Realms of the Haunting/DATA"}
M=$(echo "$1" | tr a-z A-Z); SCRIPT=$(realpath "$2"); OUT=$(realpath -m "$3")
mkdir -p "$W/probe"
if [ ! -s "$W/probe/plugin.so" ] || [ "$HERE/heldprobe.c" -nt "$W/probe/plugin.so" ]; then
  $OB/bin/i386-musl-gcc -shared -fPIC -std=gnu11 -O1 -Wall -I$OB/rothc/sdk/include -o "$W/probe/plugin.so" "$HERE/heldprobe.c" || exit 2
fi
nm $OB/rothc/roth_c/roth | grep -E " [Tt] (give_item|run_leftclick_object_trigger|scan_tag4_chunk|eval_dialogue_record_condition_with_cleanup|voice_stream_pump|try_interrupt_dialogue_voice)$" > "$W/probe/syms.txt"
G=$W/games/$M
if [ ! -d "$G" ]; then
  mkdir -p "$G/mods/heldprobe" "$G/mods/nodata_shim" "$G/DATA"
  for f in "$D"/*; do b=$(basename "$f"); case "$b" in ROTH.RES|DATA|data|mods) ;; *) ln -sfn "$f" "$G/$b";; esac; done
  for d in "$D/DATA" "$D/data"; do [ -d "$d" ] && for f in "$d"/*; do [ -e "$f" ] && ln -sfn "$f" "$G/DATA/$(basename "$f")"; done; done
  [ -e "$G/DATA/FILELIST.TXT" ] || : > "$G/DATA/FILELIST.TXT"
  cp -f $OB/shim/plugin.so "$G/mods/nodata_shim/plugin.so"
  python3 - "$M" "$D/ROTH.RES" "$G/ROTH.RES" <<'PY' || exit 1
import sys, re
m, src, dst = sys.argv[1].lower(), sys.argv[2], sys.argv[3]
s = open(src, 'rb').read().decode('latin-1')
mm = re.search(r'(?im)^m\\%s[ \t]+(m\\\S+)' % re.escape(m), s)
if not mm: sys.exit("map %s is not in ROTH.RES" % m)
s = re.sub(r'maps \{\r?\n', lambda k: k.group(0) + 'm\\%s %s\r\n' % (m, mm.group(1)), s, count=1)
open(dst, 'w', newline='', encoding='latin-1').write(s)
PY
fi
cp -f "$W/probe/plugin.so" "$G/mods/heldprobe/plugin.so"
( exec 9>/tmp/oracle_shot.lock; flock 9
  cd "$G" && CANDLE_SCRIPT="$SCRIPT" CANDLE_OUT="$OUT" CANDLE_SYMS="$W/probe/syms.txt" \
    timeout 120 $OB/rothc/roth_c/roth --game-dir . --headless > "$OUT.enginelog" 2>&1 )
grep -q "^END" "$OUT" 2>/dev/null || { echo "$M: ORIGINAL DID NOT FINISH ($OUT.enginelog)"; exit 3; }
