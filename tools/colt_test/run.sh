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
g++ -std=c++17 -O3 -Wall -Wextra -Isrc/common/models -Isrc/r_data -Ilibraries/ufbx tools/colt_test/colt_fit.cpp src/common/models/fbx_skinned.cpp "$OUT/ufbx.o" -o "$OUT/colt_fit" -lm
"$OUT/colt_fit" vrealms_iwad/models/hands/hand_final.fbx vrealms_iwad/models/weapons/colt1911/colt1911.fbx "$OUT" | grep -v knuckle || true
if ! diff -q "$OUT/colt_fit.txt" tools/colt_test/colt_fit.txt >/dev/null; then echo "NOTE: colt_fit.txt differs from the committed one (the baked seat in roth_coltvr.cpp came from that)"; diff "$OUT/colt_fit.txt" tools/colt_test/colt_fit.txt || true; fi
if [ -n "$COLT_FIT_RENDER" ]; then
	for v in "side:0,-1,0:1,0,0" "top:0,0,1:0,1,0" "eye:-1,0,0:0,1,0" "under:0,0,-1:0,1,0"; do
		n=${v%%:*}; r=${v#*:}; d=${r%%:*}; u=${r#*:}
		python3 -I tools/colt_test/rend.py "$OUT/fit_$n.png" 800 600 view=$d up=$u "mesh=$OUT/Hand_Low_gun.txt:vrealms_iwad/models/weapons/colt1911/colt1911_diffuse.jpg" \
			"mesh=$OUT/Hand_Low_hand_ready.txt:0.85,0.7,0.6" "pts=$OUT/Hand_Low_pts.txt"
	done
	echo "renders in $OUT/fit_*.png"
fi
