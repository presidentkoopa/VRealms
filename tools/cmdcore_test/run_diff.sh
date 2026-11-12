#!/bin/bash
# run_diff.sh MAP [MAP...] -- the differential test of src/roth/roth_cmdcore.cpp against the ORIGINAL.
#
#   1. cmdcore_test gen   writes a script for MAP: every 0x14/0x15/0x1c/0x1e/0x1f/0x20/0x21/0x22/0x2d/
#                         0x2e/0x33/0x35/0x3f/0x41 record, driven through its cases, and every trigger chain
#                         made only of records both sides can run, fired three times with the deferred queue.
#   2. the ORIGINAL       ROTH.C built headless (/home/claude/oracle_build) loads MAP with the cmdprobe mod
#                         (cmdprobe.c here), which calls the original's own handlers and prints every changed
#                         byte of the command table, the geometry and the objects, the chain globals and the
#                         particle pool.
#   3. the port           cmdcore_test run executes the same script and prints the same lines.
#   4. diff               IDENTICAL or DIFFER per map.
# Needs: tools/cmdcore_test/build.sh run first (OUT below), the oracle build, the retail DATA folder.
# Writes only under $W (default /tmp/cmdw). Never into the repo.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OB=${OB:-/home/claude/oracle_build}
W=${W:-/tmp/cmdw}; OUT=${OUT:-$HERE/out}
D=${ROTH_DATA:-"/mnt/user-data/uploads/Realms of the Haunting/DATA"}
mkdir -p "$W/probe" "$W/res"
if [ ! -s "$W/probe/plugin.so" ] || [ "$HERE/cmdprobe.c" -nt "$W/probe/plugin.so" ]; then
  $OB/bin/i386-musl-gcc -shared -fPIC -std=gnu11 -O1 -Wall -I$OB/rothc/sdk/include -o "$W/probe/plugin.so" "$HERE/cmdprobe.c" || exit 2
fi
# the handlers the scripts call by name (the probe's table holds 512)
nm $OB/rothc/roth_c/roth | grep -E " [Tt] (cmd_[a-z_0-9]+|reset_command_chain_state|process_deferred_command_queue|tick_particles|dbase100_bitmap_test_(set|clear)|eval_dialogue_record_by_id|finish_dialogue_record_eval|run_command_dbase100_record)$" > "$W/probe/syms.txt"
for M in "$@"; do
  M=$(echo "$M" | tr a-z A-Z)
  "$OUT/cmdcore_test" gen "$D" $M "$W/res/$M.script" || exit 2
  # --- the original: a game dir with the map first in ROTH.RES (the recipe of oracle_shot.sh) ---
  G=$W/games/$M
  if [ ! -d "$G" ]; then
    mkdir -p "$G/mods/cmdprobe" "$G/mods/nodata_shim" "$G/DATA"
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
  cp -f "$W/probe/plugin.so" "$G/mods/cmdprobe/plugin.so"
  ( exec 9>/tmp/oracle_shot.lock; flock 9
    cd "$G" && CMDPROBE_AT=60 CMDPROBE_SCRIPT="$W/res/$M.script" CMDPROBE_OUT="$W/res/$M.orig" CMDPROBE_SYMS="$W/probe/syms.txt" \
      timeout 60 $OB/rothc/roth_c/roth --game-dir . --headless > "$W/res/$M.orig.log" 2>&1 )
  grep -q "^END" "$W/res/$M.orig" 2>/dev/null || { echo "$M: ORIGINAL DID NOT RUN ($W/res/$M.orig.log)"; continue; }
  "$OUT/cmdcore_test_san" run "$D" $M "$W/res/$M.script" "$W/res/$M.port"
  grep -v "^INFO\|^N " "$W/res/$M.orig" > "$W/res/$M.orig.f"; grep -v "^INFO\|^N " "$W/res/$M.port" > "$W/res/$M.port.f"
  steps=$(grep -c "^D " "$W/res/$M.port.f")
  if cmp -s "$W/res/$M.orig.f" "$W/res/$M.port.f"; then echo "$M: IDENTICAL  ($steps steps, $(wc -l < "$W/res/$M.orig.f") lines)"
  else echo "$M: DIFFER  ($steps steps; $(diff "$W/res/$M.orig.f" "$W/res/$M.port.f" | grep -c '^[<>]') differing lines)"; fi
done
