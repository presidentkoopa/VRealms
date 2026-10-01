#!/bin/bash
# capture.sh GAMEDIR PREFIX "POSE" -> PREFIX_{real,0..4}.bin, PREFIX_tex.txt
G=$1; P=$2; POSE=$3
cd $G
run(){ timeout 90 ${ROTHC:-roth} --game-dir . --headless > $P_$1.log 2>&1; }
ORACLE_POSE="$POSE" ORACLE_PAINT=0 ORACLE_OUT=${P}_real.bin timeout 90 ${ROTHC:-roth} --game-dir . --headless > ${P}_real.log 2>&1
cp /tmp/roth_screen.ppm ${P}_real.ppm 2>/dev/null
for k in 0 1 2 3 4; do
  ORACLE_POSE="$POSE" ORACLE_PASS=$k ORACLE_OUT=${P}_$k.bin ORACLE_TEXLOG=${P}_tex$k.txt timeout 90 ${ROTHC:-roth} --game-dir . --headless > ${P}_$k.log 2>&1
done
wait
cat ${P}_tex*.txt | sort -u > ${P}_tex.txt
grep -h "captured" ${P}_*.log
