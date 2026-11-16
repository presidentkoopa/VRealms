#!/bin/sh
# RSQ Distance Grab's takeable set per map (takeable.cpp says what it is). Default map: STUDY1.
set -e
cd "$(dirname "$0")"
DATA="${ROTH_DATA:-/mnt/user-data/uploads/Realms of the Haunting/DATA}"
OUT=${DGRAB_TEST_OUT:-/tmp/dgrab_test_out}
mkdir -p "$OUT"
S=../../src/roth
g++ -std=c++17 -O1 -I $S -o "$OUT/takeable" takeable.cpp $S/roth_raw.cpp $S/roth_install.cpp $S/roth_dbase100.cpp \
	$S/roth_dbase100_install.cpp $S/roth_log.cpp $S/roth_inventory.cpp
[ $# -eq 0 ] && set -- STUDY1
for m in "$@"; do "$OUT/takeable" "$DATA" "$m"; done
