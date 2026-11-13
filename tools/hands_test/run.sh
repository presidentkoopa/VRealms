#!/bin/sh
# The VR hands' finger math, outside the engine.
#   tools/hands_test/run.sh
#
#   test_handpose.cpp  the rate, the hand frame, the fit and the seat
#   test_holds.cpp     DIFFERENTIAL: which pose shapes a hand holding a weapon or an object (VAP :476-656) and that
#                      pose as the joints' targets (VAP :672-684), against UZDXREMA's OWN CODE cut out below
#   test_curls.cpp     DIFFERENTIAL: the rate, the smoothing and the turn on the joints, against UZDXREMA's
#                      OWN CODE -- the lines below are cut out of its vr_avatar_pose.cpp and vr_armik.h at
#                      build time and compiled beside the port, then both are fed the same targets; and
#                      the pose rule's cases (the rule itself is not UZDXREMA's: the owner's authored
#                      poses). The source is read-only; set UZDXREMA if it is not at the default path.
set -e
cd "$(dirname "$0")"
OUT=${HANDS_TEST_OUT:-/tmp/hands_test_out}
UZ=${UZDXREMA:-/home/claude/uzdxrema}
mkdir -p "$OUT/uz"

g++ -std=c++17 -O1 -Wall -Wextra -I../../src/r_data test_handpose.cpp -o "$OUT/test_handpose"
"$OUT/test_handpose"

VAP="$UZ/src/r_data/vr_avatar_pose.cpp"
ARM="$UZ/src/r_data/vr_armik.h"
if [ ! -f "$VAP" ] || [ ! -f "$ARM" ]; then
	echo "FAIL: UZDXREMA's source is not at $UZ (set UZDXREMA=<its checkout>); the differential test cannot run"
	exit 1
fi
echo "UZDXREMA: $UZ, HEAD $(git -C "$UZ" rev-parse --short HEAD 2>/dev/null || echo '?')"

# Each cut is pinned by the text of its first and last line: if the source moves, this refuses.
pin() {
	got=$(sed -n "${2}p" "$1" | sed 's/^[[:space:]]*//')
	case "$got" in
		"$3"*) ;;
		*) echo "FAIL: $1:$2 reads '$got', expected '$3' -- the source moved; re-pin the lines"; exit 1 ;;
	esac
}
pin "$ARM" 35 "struct FVRMat3"
pin "$ARM" 54 "};"
pin "$VAP" 336 "static VSMatrix FromMat3Pos(const FVRMat3& r, const FVector3& p)"
pin "$VAP" 348 "}"
pin "$VAP" 392 "void QSlerp(const float a[4], const float b[4], float t, float out[4])"
pin "$VAP" 414 "}"
pin "$VAP" 429 "if (!(dt > 0.0) || dt > 0.25) dt = 1.0 / 90.0;"
pin "$VAP" 431 "const float retain = (float)pow(0.75, dt * 90.0);"
pin "$VAP" 691 "float* cur = gRot[sd].Q[f][j];"
pin "$VAP" 693 "else { float o[4]; QSlerp(cur, wq, 1.f - retain, o);"
pin "$VAP" 696 "gRot[sd].Init = true;"
pin "$VAP" 1785 "for (int f = 0; f < VRFINGER_COUNT; ++f)"
pin "$VAP" 1794 "const float* q = gRot[sd].Q[f][j];"
pin "$VAP" 1833 "gGlobals[cj] = moved;"
pin "$VAP" 1836 "}"
OXH="$UZ/src/common/rendering/vulkan/stereo3d/vk_openxrdevice.h"
[ -f "$OXH" ] || { echo "FAIL: $OXH is missing"; exit 1; }
pin "$VAP" 476 "if (VRRig_HandHoldsWeapon(hidx))"
pin "$VAP" 558 "else"
pin "$VAP" 587 "const int subj = VRRig_HandGripSubject(hidx);"
pin "$VAP" 622 "const int otherHand = (hidx == VRRIG_MAIN) ? VRRIG_OFF : VRRIG_MAIN;"
pin "$VAP" 656 "}"
pin "$VAP" 657 ""
pin "$VAP" 658 "for (int f = 0; f < VRFINGER_COUNT; ++f)"
pin "$VAP" 672 "float wq[4];"
pin "$VAP" 684 "have = VRAvatar_NamedPoseRot(srcPose[f][j].GetChars(), sd, f, j, wq);"
pin "$VAP" 685 "if (!have)"
pin "$OXH" 105 "enum EGripSubject"
pin "$OXH" 131 "};"

sed -n '35,54p'     "$ARM" > "$OUT/uz/fvrmat3.inc"			# FVRMat3
sed -n '336,348p'   "$VAP" > "$OUT/uz/frommat3pos.inc"		# FromMat3Pos
sed -n '392,414p'   "$VAP" > "$OUT/uz/qmath.inc"			# QSlerp, QAxisAngle
sed -n '429,431p'   "$VAP" > "$OUT/uz/retain.inc"			# the rate
sed -n '691,693p'   "$VAP" > "$OUT/uz/gslerp.inc"			# gRot's smoothing of one joint toward its target
sed -n '696p'       "$VAP" > "$OUT/uz/ginit.inc"			# gRot[sd].Init = true
sed -n '1785,1836p' "$VAP" > "$OUT/uz/turn.inc"			# the turn on the joint globals and the carry
sed -n '476,656p'   "$VAP" > "$OUT/uz/hold_select.inc"		# which pose shapes a holding hand (weapon / object)
sed -n '672,684p'   "$VAP" > "$OUT/uz/hold_rot.inc"		# that choice as the joint's rotation target
sed -n '105,131p'   "$OXH" > "$OUT/uz/gripsubject.inc"		# EGripSubject

g++ -std=c++17 -O1 -Wall -Wextra -Wno-unused-variable -Wno-unused-but-set-variable -I../../src/r_data -I"$OUT/uz" \
	test_curls.cpp -o "$OUT/test_curls"
"$OUT/test_curls"

g++ -std=c++17 -O1 -Wall -Wextra -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-parameter -I../../src/r_data -I../../src -I"$OUT/uz" \
	test_holds.cpp -o "$OUT/test_holds"
"$OUT/test_holds"
