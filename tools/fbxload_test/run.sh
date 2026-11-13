#!/bin/sh
# The engine's FBX loader core (src/common/models/fbx_skinned.cpp) with ufbx, outside the engine, against
# the owner's hand_final.fbx. docs/FBX_LOADER.md "Acceptance" lists what each check is.
#   tools/fbxload_test/run.sh [path/to/hand_final.fbx]     (default: vrealms_iwad/models/hands/hand_final.fbx)
#   FBXLOAD_RENDER=1 also writes software renders of the loader's meshes (render.py) into the output dir, and the
#   fingers' contact sheets (fingers/sheet_Hand_Low.png, fingers/sheet_Hand_Low1.png): each hand with the skin the
#   MODELDEF gives it (the left: mirrorskin=Hand_Low), lit by its vertex normals and, beside that, by the FBX's
#   normal map (render.py --normal).
#   The maps (maps_ref.py) are checked against tools/fbx_hands' own FBX reader and libpng (OpenCV).
#   The fingers (test_fingers.cpp, measure_ref.py) compare against UZDXREMA's own files: set UZDXREMA if its
#   checkout is not at /home/claude/uzdxrema.
#   The poses (dump_take.cpp + walk_poses.py): both takes walked, the engine's kPoseSources checked against the walk;
#   with FBXLOAD_RENDER also the contact sheets walk/poses_Hand_Low.png and walk/poses_Hand_Low1.png (the six states as
#   composed, the owner's frames and the candidates, wrist at rest).
set -e
cd "$(dirname "$0")/../.."
UZ=${UZDXREMA:-/home/claude/uzdxrema}
FBX=${1:-vrealms_iwad/models/hands/hand_final.fbx}
OUT=${FBXLOAD_TEST_OUT:-/tmp/fbxload_test_out}
mkdir -p "$OUT/dump"
mkdir -p "$OUT/maps"
gcc -std=c99 -O2 -c libraries/ufbx/ufbx.c -o "$OUT/ufbx.o"
g++ -std=c++17 -O2 -w -c -Isrc/common/thirdparty/stb tools/fbxload_test/stb_image_impl.cpp -o "$OUT/stb_image.o"
g++ -std=c++17 -O2 -Wall -Wextra -Isrc/common/models -Isrc/r_data -Ilibraries/ufbx -Isrc/common/thirdparty/stb \
	tools/fbxload_test/test_fbxload.cpp src/common/models/fbx_skinned.cpp "$OUT/ufbx.o" "$OUT/stb_image.o" -o "$OUT/test_fbxload" -lm
FRAMES=0,160,205,318,500,761,845,984,1100,1277,1278
"$OUT/test_fbxload" "$FBX" --dump "$OUT/dump" --maps "$OUT/maps" --angles $FRAMES > "$OUT/test.txt" || { cat "$OUT/test.txt"; exit 1; }
grep -v '^ANGLES' "$OUT/test.txt"
grep '^ANGLES' "$OUT/test.txt" > "$OUT/ours.txt"
echo
echo "== the loader's joint rotations against tools/fbx_hands/poses.py (its own FBX reader, no ufbx)"
python3 -I tools/fbxload_test/poses_ref.py "$FBX" $FRAMES > "$OUT/ref.txt"
python3 -I tools/fbxload_test/compare.py "$OUT/ours.txt" "$OUT/ref.txt"
echo
TEX=vrealms_iwad/models/hands/lambert1_Base_Color.png
python3 -I tools/fbxload_test/maps_ref.py "$FBX" "$OUT/maps" "$TEX"
if [ -n "$FBXLOAD_RENDER" ]; then
	NRM="--normal $OUT/maps/normal_decoded.png"
	python3 -I tools/fbxload_test/render.py $NRM "$OUT/dump" "$TEX" "$OUT/rest.png" Hand_Low_rest Hand_Low1_rest
	python3 -I tools/fbxload_test/render.py $NRM "$OUT/dump" "$TEX" "$OUT/posed.png" Hand_Low_500 Hand_Low1_500 Hand_Low_1100 Hand_Low1_845
fi

echo
echo "== the fingers on the FBX (test_fingers.cpp: the take walked, frames, poses vs mkfbxposes.py, fold, every state at full input)"
mkdir -p "$OUT/fingers"
if [ ! -f "$UZ/wadsrc/static/vrhandpose.txt" ] || [ ! -f "$UZ/tools/avatar/measure_avatar.py" ]; then
	echo "FAIL: UZDXREMA's checkout is not at $UZ (set UZDXREMA): the fingers cannot be compared with its files"
	exit 1
fi
g++ -std=c++17 -O2 -Wall -Wextra -Isrc/common/models -Isrc/r_data -Ilibraries/ufbx \
	tools/fbxload_test/test_fingers.cpp src/common/models/fbx_skinned.cpp "$OUT/ufbx.o" -o "$OUT/test_fingers" -lm
"$OUT/test_fingers" "$FBX" "$UZ/wadsrc/static/vrhandpose.txt" "$OUT/fingers" > "$OUT/fingers.txt" || { cat "$OUT/fingers.txt"; exit 1; }
cat "$OUT/fingers.txt"
echo
echo "== the canonical frames against measure_avatar.py's own lines"
python3 -I tools/fbxload_test/measure_ref.py "$OUT/fingers/rest.json" "$UZ"
echo
echo "== THE POSES: both takes walked frame by frame (walk_poses.py on dump_take's frames), and the engine's kPoseSources checked"
mkdir -p "$OUT/walk"
g++ -std=c++17 -O2 -Wall -Wextra -Isrc/common/models -Ilibraries/ufbx \
	tools/fbxload_test/dump_take.cpp src/common/models/fbx_skinned.cpp "$OUT/ufbx.o" -o "$OUT/dump_take" -lm
"$OUT/dump_take" "$FBX" "$OUT/walk/take.json"
SHEET=""
[ -n "$FBXLOAD_RENDER" ] && SHEET="--sheet $TEX"
python3 -I tools/fbxload_test/walk_poses.py $SHEET "$OUT/walk/take.json" "$OUT/walk" src/r_data/vr_handpose_math.h
if [ -n "$FBXLOAD_RENDER" ]; then
	for h in Hand_Low Hand_Low1; do
		python3 -I tools/fbxload_test/render.py $NRM "$OUT/fingers" "$TEX" "$OUT/fingers/sheet_$h.png" \
			${h}_ctl_open ${h}_ctl_fist ${h}_ctl_point ${h}_ctl_thumbup ${h}_ctl_fingergun ${h}_ctl_trigger
	done
fi
