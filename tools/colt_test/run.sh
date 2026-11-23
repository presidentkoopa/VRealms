#!/bin/sh
# The Colt's real parts, engine-free (roth_colt.h): the reload's rules against the original's (test_colt), the
# Colt's reload through the port beside the running original (the oracle build, when there is one), the fit of the mesh
# to the authored hand (colt_fit), the loader's part= option (fbx_part).
#   tools/colt_test/run.sh [DATA]       DATA: the retail DATA folder (default: the uploads copy)
set -e
cd "$(dirname "$0")/../.."
DATA=${1:-${ROTH_DATA:-"/mnt/user-data/uploads/Realms of the Haunting/DATA"}}
OUT=${COLT_TEST_OUT:-/tmp/colt_test_out}
mkdir -p "$OUT"
SRC=src/roth
echo "== test_colt: the reload's rules on the port's own weapon logic"
g++ -std=c++17 -Wall -Wextra -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I$SRC \
	-o "$OUT/test_colt" tools/colt_test/test_colt.cpp $SRC/roth_actors.cpp $SRC/roth_actor_*.cpp \
	$SRC/roth_raw.cpp $SRC/roth_dbase100.cpp $SRC/roth_das.cpp $SRC/roth_palette.cpp
"$OUT/test_colt" "$DATA"
echo
echo "== the Colt's reload, the port beside the running original (the headless oracle build; tools/actors_test replay mode weapon)"
OB=/home/claude/oracle_build
if [ -x "$OB/bin/i386-musl-gcc" ]; then
	P="$OUT/probe"; mkdir -p "$P"
	"$OB/bin/i386-musl-gcc" -shared -fPIC -std=gnu11 -O1 -I"$OB/rothc/sdk/include" -o "$P/plugin.so" tools/actors_test/oracle/actprobe.c
	ACTP_DIR="$P" bash tools/actors_test/oracle/run.sh STUDY1 -864 3412 466 tools/colt_test/oracle_reload_script.txt "$OUT/oracle_reload.log" 1300 >/dev/null
	tools/actors_test/build.sh "$OUT/at" >/dev/null
	"$OUT/at/actors_test" "$DATA" STUDY1 tools/colt_test/replay_weapon.txt "$OUT/oracle_reload.log" > "$OUT/reload.port" 2>&1 || true
	grep '^WEAPON' "$OUT/reload.port"
	echo "the original's magazine word and i51 count, each time they change:"; grep '^INVS' "$OUT/oracle_reload.log" | uniq | sed 's/.*4:9:/  Colt word /; s/ 5:51:/, i51 /; s/ |//' | uniq | tr '\n' ';'; echo
	grep -q 'weapon states compared [0-9]* (differ 0' "$OUT/reload.port" && grep -q 'inventory states compared [0-9]* (differ 0' "$OUT/reload.port" \
		|| { echo "FAIL: the port's Colt reload differs from the original's"; exit 1; }
	echo "PASS: the Colt's 36 pulls with its reload: every weapon and inventory state of the port equal to the original's"
else
	echo "SKIP: no oracle build at $OB"
fi
echo
echo "== fbx_part: the loader's part=<joint>"
gcc -std=c99 -O2 -c libraries/ufbx/ufbx.c -o "$OUT/ufbx.o"
g++ -std=c++17 -O2 -Wall -Wextra -Isrc/common/models -Ilibraries/ufbx tools/colt_test/fbx_part.cpp src/common/models/fbx_skinned.cpp "$OUT/ufbx.o" -o "$OUT/fbx_part" -lm
"$OUT/fbx_part" vrealms_iwad/models/weapons/colt1911/colt1911.fbx
echo
echo "== colt_fit: the gun fitted to the authored grip_pistol hand (numbers; COLT_FIT_RENDER=1 renders the views)"
HANDS=vrealms_iwad/models/hands/godot_xr/godot_xr_hands.fbx   # vrealms_iwad/MODELDEF.txt's hands
g++ -std=c++17 -O3 -Wall -Wextra -Isrc/common/models -Isrc/r_data -Ilibraries/ufbx tools/colt_test/colt_fit.cpp src/common/models/fbx_skinned.cpp "$OUT/ufbx.o" -o "$OUT/colt_fit" -lm
FIT=0
"$OUT/colt_fit" "$HANDS" vrealms_iwad/models/weapons/colt1911/colt1911.fbx "$OUT" > "$OUT/colt_fit.log" || FIT=1
grep -v knuckle "$OUT/colt_fit.log"
if ! diff -q "$OUT/colt_fit.txt" tools/colt_test/colt_fit.txt >/dev/null; then echo "NOTE: colt_fit.txt differs from the committed one (the baked seat in roth_coltvr.cpp came from that)"; diff "$OUT/colt_fit.txt" tools/colt_test/colt_fit.txt || true; fi
if [ -n "$COLT_FIT_RENDER" ]; then
	python3 -I tools/colt_test/fitviews.py "$OUT" fit vrealms_iwad/models/hands/godot_xr/caucasian_realistic_baseColor.png
	echo "renders in $OUT/fit_*.png (the gun's left side, its right side, oblique; both hands: fit_all.png)"
fi
[ $FIT -eq 0 ] || { echo "FAIL: colt_fit"; exit 1; }
echo
echo "== support_fit: the other hand fitted to the gun and the posed shooting hand (two hands; SUPPORT_RENDER=1 renders the views)"
g++ -std=c++17 -O3 -Wall -Wextra -Itools/colt_test -Isrc/common/models -Isrc/r_data -Ilibraries/ufbx tools/colt_test/support_fit.cpp src/common/models/fbx_skinned.cpp "$OUT/ufbx.o" -o "$OUT/support_fit" -lm
SFIT=0
"$OUT/support_fit" "$HANDS" vrealms_iwad/models/weapons/colt1911/colt1911.fbx tools/colt_test/colt_fit.txt "$OUT" > "$OUT/support_fit.log" || SFIT=1
grep -E "^(PASS|FAIL|NOT MET)|hard check|  (fingers|the palm|the thumb|support skin)" "$OUT/support_fit.log"
if ! diff -q "$OUT/support_fit.txt" tools/colt_test/support_fit.txt >/dev/null; then echo "NOTE: support_fit.txt differs from the committed one (the baked seat in roth_coltvr.cpp came from that)"; diff "$OUT/support_fit.txt" tools/colt_test/support_fit.txt || true; fi
if [ -n "$SUPPORT_RENDER" ]; then
	python3 -I tools/colt_test/supportviews.py "$OUT" support vrealms_iwad/models/hands/godot_xr/caucasian_realistic_baseColor.png
	SUPPORT_SKIN=1 python3 -I tools/colt_test/supportviews.py "$OUT" support_skin vrealms_iwad/models/hands/godot_xr/caucasian_realistic_baseColor.png
	echo "renders in $OUT/support*_<pair>_*.png (pair 0: the right hand shoots; 1: the left)"
fi
[ $SFIT -eq 0 ] || { echo "FAIL: support_fit (a hand in the gun or in the other hand)"; exit 1; }
