#!/bin/sh
# THE GODOT XR TOOLS HANDS WITH THE OWNER'S POSES (vrealms_iwad/models/hands/godot_xr/godot_xr_hands.fbx), engine-free.
#   tools/hand_retarget/run.sh [GODOT_HANDS_DIR]
#     GODOT_HANDS_DIR: godot-xr-tools' addons/godot-xr-tools/hands (default /home/claude/godotvr/godot-xr-tools/...;
#     a public repo: git clone --depth 1 https://github.com/GodotVR/godot-xr-tools)
#   1. hand_final.fbx's take through the engine's loader (tools/fbxload_test/dump_take.cpp);
#   2. retarget_godot.py writes the FBX again and it must equal the committed one byte for byte;
#   3. check_retarget.cpp: the new file through the engine's loader and hand math against hand_final.fbx (L, C, P);
#   4. with RETARGET_RENDER=1, render_compare.py: every pose on both files, side by side, into $OUT/compare.
set -e
cd "$(dirname "$0")/../.."
GD=${1:-/home/claude/godotvr/godot-xr-tools/addons/godot-xr-tools/hands}
OUT=${RETARGET_OUT:-/tmp/hand_retarget_out}
NEW=vrealms_iwad/models/hands/godot_xr/godot_xr_hands.fbx
OLD=vrealms_iwad/models/hands/hand_final.fbx
SKIN=vrealms_iwad/models/hands/godot_xr/caucasian_realistic_baseColor.png
mkdir -p "$OUT/check"
gcc -std=c99 -O2 -c libraries/ufbx/ufbx.c -o "$OUT/ufbx.o"
g++ -std=c++17 -O2 -Wall -Wextra -Isrc/common/models -Ilibraries/ufbx \
	tools/fbxload_test/dump_take.cpp src/common/models/fbx_skinned.cpp "$OUT/ufbx.o" -o "$OUT/dump_take" -lm
"$OUT/dump_take" "$OLD" "$OUT/take.json"
echo "== retarget_godot.py: the FBX written again from hand_final's take and the Godot glTFs"
python3 -I tools/hand_retarget/retarget_godot.py "$OUT/take.json" "$GD" "$SKIN" "$OUT/godot_xr_hands.fbx" "$OUT/report.json"
if cmp -s "$OUT/godot_xr_hands.fbx" "$NEW"; then echo "PASS  the committed $NEW is what the converter writes (byte for byte)"
else echo "FAIL  $NEW differs from the converter's output ($OUT/godot_xr_hands.fbx)"; exit 1; fi
echo
echo "== check_retarget: the new file through the engine's loader and hand math, against hand_final.fbx"
g++ -std=c++17 -O2 -Wall -Wextra -Isrc/common/models -Isrc/r_data -Ilibraries/ufbx \
	tools/hand_retarget/check_retarget.cpp src/common/models/fbx_skinned.cpp "$OUT/ufbx.o" -o "$OUT/check_retarget" -lm
"$OUT/check_retarget" "$OLD" "$NEW" "$OUT/check"
if [ -n "$RETARGET_RENDER" ]; then
	python3 -I tools/hand_retarget/render_compare.py "$OUT/check" vrealms_iwad/models/hands/lambert1_Base_Color.png "$SKIN" "$OUT/compare"
	echo "renders in $OUT/compare"
fi
