#!/bin/bash
# oracle_capture.sh MAP NAME X Y ANGLE512 OUTDIR
# The original engine's five painted passes (tools/oracle) plus a real-art frame at one
# pose: OUTDIR/NAME_{0..4}.bin, NAME_real.png, NAME_tex.txt. Needs the oracle build's
# oracle_shot.sh (ORACLE_SHOT, default /home/claude/oracle_build/oracle_shot.sh), which
# takes the machine-wide capture lock itself. A pass that fails is retried.
MAP=$1; N=$2; X=$3; Y=$4; A=$5; D=${6:-.}
SHOT=${ORACLE_SHOT:-/home/claude/oracle_build/oracle_shot.sh}
mkdir -p "$D"
for try in 1 2 3; do [ -s "$D/${N}_real.png" ] && break; "$SHOT" $MAP $X $Y $A "$D/${N}_real.png" >/dev/null 2>"$D/${N}_real.err"; done
for k in 0 1 2 3 4; do
  for try in 1 2 3 4; do
    [ -s "$D/${N}_$k.bin" ] && break
    ORACLE_PAINT=1 ORACLE_PASS=$k ORACLE_KEEP=1 ORACLE_TEXLOG="$D/${N}_tex$k.txt" "$SHOT" $MAP $X $Y $A "$D/${N}_p$k.png" >/dev/null 2>>"$D/${N}_real.err"
    mv "$D/${N}_p$k.oracle.bin" "$D/${N}_$k.bin" 2>/dev/null
  done
done
cat "$D/${N}"_tex[0-4].txt 2>/dev/null | sort -u > "$D/${N}_tex.txt"
echo "$N: $(ls "$D/${N}"_[0-4].bin 2>/dev/null | wc -l) of 5 passes"
