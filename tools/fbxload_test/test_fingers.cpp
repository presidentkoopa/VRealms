// tools/fbxload_test/test_fingers.cpp -- the VR hands' fingers on the REAL hand_final.fbx, both hands, outside the
// engine: the loader core (fbx_skinned.cpp) + ufbx + the renderer's own vr_handpose_math.h, nothing copied.
//
//   test_fingers <hand_final.fbx> <UZDXREMA vrhandpose.txt> <outdir>
//
//   W   THE TAKE WALKED: every frame of each hand's own take; per finger the most open frame (straightest) and the
//       most closed (fingertip nearest the palm), measured (the full walk and the pose choice: walk_poses.py).
//   F1  the canonical bone frames (measure_avatar.py's algorithm, vr_handpose_math.h MeasureFingerFrames) on each
//       hand's rest pose; rest.json is written for measure_ref.py, which runs measure_avatar.py's OWN lines on it.
//   F2  the pose rotations (mkfbxposes.py's formula, PoseRotation) from this loader's frames 0 and 318 against the
//       ones mkfbxposes.py wrote from Blender frames 1 and 319 (UZDXREMA wadsrc/static/vrhandpose.txt), and the
//       neighbouring frames, to show which numbering is right.
//   F3  fold direction: each joint of the fist's fingers turned a little toward its authored fist moves its tip
//       toward the palm (along the palm normal), both hands.
//   S1  EVERY STATE AT FULL INPUT through the engine path (StepPose converged at 90 Hz, PosePalette on the engine's
//       swapped palette): open, fist, point, thumbup, fingergun, trigger. Every finger joint, every tip and the palm
//       joint has the local rotation of the authored frame kPoseSources names for it (or the open frame), < 0.01 deg.
//   S2  the extended finger of point, thumbup and fingergun is the authored one: its joints' local rotations as S1,
//       its joints and tip in the hand where the composition puts them, and the palm under it unturned.
//   S3  open = the rest: every joint of the hand, the palm included, at rest.
//   S4  in between: at half grip each joint's local rotation is the slerp half-way between its two authored ones.
//   H0  the owner's raw frames as poses: for every raw pose of his fbx_poses.json and each hand that AUTHORED it, the
//       15 rotations from take frame (Blender - 1) against the ones mkfbxposes.py wrote for that hand ("own", not
//       "MIRRORED") in UZDXREMA's vrhandpose.txt.
//   H1  the held-object and weapon-grip poses built from each hand's own take (BuildHoldPoses): which poses a hand has
//       and which its take lacks, and that a raw pose is on a hand exactly when that hand authored it.
//   H2  EVERY HOLD POSE AT FULL INPUT through the engine path (SelectHold + StepPoseHeld converged at 90 Hz, PosePalette
//       on the engine's palette), both hands: every pose of the table by name, every hold subject, every weapon grip
//       class at trigger 0 and 1. (a) each joint's (and the palm's) drawn rotation equals the rotation made in double from its authored
//       frame, within 1e-5 deg; (b) every finger joint, tip and the palm has its authored frame's local rotation
//       through the whole palette path (float), printed. A pose the hand lacks leaves the controller's pose, checked.
//   --  dumps every state's mesh (the engine's 4 byte weights through the posed palette) for render.py.

#include "fbx_skinned.h"
#include "vr_handpose_math.h"

#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace FBXSkinned;
using namespace VRHandPose;

static int gFails = 0;
static void Check(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void Check(bool ok, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	printf("  %s  ", ok ? "PASS" : "FAIL");
	vprintf(fmt, ap);
	printf("\n");
	va_end(ap);
	if (!ok) gFails++;
}

static std::vector<char> ReadFile(const char *path)
{
	std::vector<char> d;
	FILE *f = fopen(path, "rb");
	if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
	fseek(f, 0, SEEK_END);
	d.resize(ftell(f));
	fseek(f, 0, SEEK_SET);
	if (fread(d.data(), 1, d.size(), f) != d.size()) { fprintf(stderr, "short read\n"); exit(2); }
	fclose(f);
	return d;
}

struct V3 { double x, y, z; };
static V3 Sub(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static double Len(V3 a) { return sqrt(Dot(a, a)); }
static V3 Pos(const double m[16]) { return { m[12], m[13], m[14] }; }
static V3 MatPoint(const double m[16], V3 p)
{
	return { m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13], m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14] };
}

typedef std::vector<std::array<double, 16>> Mats;

static void ComposeChain(const Result &r, const TRSd *locals, Mats &world)
{
	world.resize(r.joints.size());
	for (size_t j = 0; j < r.joints.size(); j++)
	{
		double m[16];
		ComposeTRS(locals[j], m);
		if (r.joints[j].parent < 0) memcpy(world[j].data(), m, sizeof(m));
		else MatMul(world[r.joints[j].parent].data(), m, world[j].data());
	}
}

// rot lines of mkfbxposes.py's output: pose -> side -> [finger][joint] quaternion
typedef std::map<std::string, std::map<char, std::array<std::array<std::array<double, 4>, 3>, 5>>> RotFile;
static bool ReadRot(const char *path, RotFile &out)
{
	FILE *f = fopen(path, "r");
	if (!f) return false;
	static const char *names[5] = { "thumb", "index", "middle", "ring", "pinky" };
	char line[512];
	while (fgets(line, sizeof(line), f))
	{
		char pose[64], side[8], fn[32]; int j; double q[4];
		if (sscanf(line, " rot %63s %7s %31s %d %lf %lf %lf %lf", pose, side, fn, &j, &q[0], &q[1], &q[2], &q[3]) != 8) continue;
		for (int k = 0; k < 5; k++)
			if (!strcmp(fn, names[k]) && j >= 1 && j <= 3)
				for (int c = 0; c < 4; c++) out[pose][side[0]][k][j - 1][c] = q[c];
	}
	fclose(f);
	return !out.empty();
}

static double QAngleDeg(const double a[4], const double b[4])
{
	double d = fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
	if (d > 1) d = 1;
	return 2 * acos(d) * 180.0 / M_PI;
}

// The engine path for one set of inputs, converged: StepPose at 90 Hz until settled, then PosePalette on the
// engine's swapped palette of the drawn frame. Returns the posed joint globals (file space) and the file-space
// palette for skinning.
struct Posed { Mats G; Mats P; SideState rot; PalmState palm; };
static const float kS[16] = { 1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 1 };

static Posed PoseHand(const Result &r, const Mats &Wrest, const HandRig &rig, const HandPoses &hp, const Inputs &in, int frames = 400,
	const HoldPoses *hold = nullptr, const HeldState *held = nullptr, HoldChoice *choiceOut = nullptr)
{
	const int nj = (int)r.joints.size();
	Posed out;
	for (int i = 0; i < frames; i++)
	{
		if (hold != nullptr && held != nullptr)
		{
			// the engine's per-frame order (vr_handpose.cpp UpdateCurls): SelectHold, then StepPoseHeld
			HoldChoice ch;
			SelectHold(hold, *held, HeldState(), in, ch);
			StepPoseHeld(out.rot, out.palm, in, Retain(1.0 / 90.0), &hp, hold, &ch);
			if (choiceOut != nullptr) *choiceOut = ch;
		}
		else StepPose(out.rot, out.palm, in, Retain(1.0 / 90.0), &hp);
	}

	Mats W0;
	ComposeChain(r, &r.frames[(size_t)kDrawnFrame * nj], W0);
	std::vector<std::array<float, 16>> pal(nj), base(nj), inv(nj), outPal(nj);
	for (int j = 0; j < nj; j++)
	{
		double ri[16], p[16];
		MatInvert(Wrest[j].data(), ri);
		MatMul(W0[j].data(), ri, p);
		float pf[16], t[16];
		for (int k = 0; k < 16; k++) { pf[k] = (float)p[k]; base[j][k] = (float)Wrest[j][k]; inv[j][k] = (float)ri[k]; }
		Detail::Mul4(kS, pf, t);
		Detail::Mul4(t, kS, pal[j].data());	// the engine's palette: S . W . B^-1 . S
	}
	std::vector<float> scratch;
	std::vector<char> moved;
	PosePalette(reinterpret_cast<const float (*)[16]>(pal.data()), reinterpret_cast<const float (*)[16]>(base.data()),
		reinterpret_cast<const float (*)[16]>(inv.data()), nj, rig, out.rot, reinterpret_cast<float (*)[16]>(outPal.data()), scratch, moved,
		out.palm.Q);
	out.G.resize(nj);
	out.P.resize(nj);
	for (int j = 0; j < nj; j++)
	{
		float G[16], t[16], pf[16];
		PaletteToGlobal(outPal[j].data(), base[j].data(), G);
		Detail::Mul4(kS, outPal[j].data(), t);
		Detail::Mul4(t, kS, pf);	// back to file space for skinning the file-space rest mesh
		for (int k = 0; k < 16; k++) { out.G[j][k] = G[k]; out.P[j][k] = pf[k]; }
	}
	return out;
}

static void DumpMesh(const char *path, const Result &r, const Mats &P)
{
	FILE *o = fopen(path, "w");
	if (!o) return;
	fprintf(o, "%zu %zu\n", r.verts.size(), r.tris.size() / 3);
	for (auto &v : r.verts)
	{
		V3 p = { 0, 0, 0 }, n = { 0, 0, 0 };
		for (int k = 0; k < 4; k++)
		{
			if (!v.weight[k]) continue;
			const double *m = P[v.bone[k]].data(), w = v.weight[k] / 255.0;
			const V3 q = MatPoint(m, { v.pos[0], v.pos[1], v.pos[2] });
			p.x += q.x * w; p.y += q.y * w; p.z += q.z * w;
			n.x += (m[0] * v.nrm[0] + m[4] * v.nrm[1] + m[8] * v.nrm[2]) * w;
			n.y += (m[1] * v.nrm[0] + m[5] * v.nrm[1] + m[9] * v.nrm[2]) * w;
			n.z += (m[2] * v.nrm[0] + m[6] * v.nrm[1] + m[10] * v.nrm[2]) * w;
		}
		fprintf(o, "%.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f\n", p.x, p.y, p.z, n.x, n.y, n.z, v.uv[0], v.uv[1]);
	}
	for (size_t t = 0; t < r.tris.size(); t += 3) fprintf(o, "%u %u %u\n", r.tris[t], r.tris[t + 1], r.tris[t + 2]);
	fclose(o);
}

int main(int argc, char **argv)
{
	if (argc < 4) { fprintf(stderr, "usage: test_fingers <fbx> <vrhandpose.txt> <outdir>\n"); return 2; }
	std::vector<char> file = ReadFile(argv[1]);
	RotFile rotFile;
	const bool haveRot = ReadRot(argv[2], rotFile);
	const std::string outDir = argv[3];
	const int kFist = 318;	// the owner's "fist" 319 (Blender): F2 compares it with mkfbxposes.py's output
	static const char *fingerNames[5] = { "thumb", "index", "middle", "ring", "pinky" };
	static const char *tipNames[5] = { "THUMB_UP_TOP_joint", "INDEX_UP_TOP_joint", "MIDDLE_F_UP_TOP_joint", "RING_UP_TOP_joint", "PINK_UP_TOP_joint" };

	FILE *rj = fopen((outDir + "/rest.json").c_str(), "w");
	fprintf(rj, "{\n");

	const char *meshes[2] = { "Hand_Low", "Hand_Low1" };
	const char *selectors[2] = { "Hand_Low", "Hand_Low1?mirrorskin=Hand_Low" };	// as vrealms_iwad/MODELDEF.txt names them
	for (int h = 0; h < 2; h++)
	{
		const bool right = (h == 0);
		const char sideCh = right ? 'R' : 'L';
		Result r; std::string err;
		if (!Load(file.data(), file.size(), selectors[h], r, err)) { printf("%s: %s\n", selectors[h], err.c_str()); return 2; }
		const int nj = (int)r.joints.size();
		std::map<std::string, int> idx;
		for (int j = 0; j < nj; j++) idx[r.joints[j].name] = j;
		const int hand = idx.at(kHandJoint), palmJ = idx.at("HANDPALM_joint");
		int joints[5][3], tips[5];
		for (int f = 0; f < 5; f++)
		{
			for (int j = 0; j < 3; j++) joints[f][j] = idx.at(kFingerJointNames[f][j]);
			tips[f] = idx.at(tipNames[f]);
		}
		printf("\n==== %s (%s hand), %d joints, %d frames\n", meshes[h], right ? "RIGHT" : "LEFT", nj, r.numFrames);

		Mats Wrest;
		{
			std::vector<TRSd> rl(nj);
			for (int j = 0; j < nj; j++) rl[j] = r.joints[j].rest;
			ComposeChain(r, rl.data(), Wrest);
		}
		std::vector<RestJoint> J(nj);
		for (int j = 0; j < nj; j++)
		{
			J[j].parent = r.joints[j].parent;
			memcpy(J[j].G, Wrest[j].data(), sizeof(J[j].G));
			memcpy(J[j].t, r.joints[j].rest.t, sizeof(J[j].t));
		}

		// ---- F1: the canonical frames, and the hand's palm; the authored poses (BuildPoses)
		HandRig rig;
		FingerFrame frames[5][3];
		double palm[3];
		char why[256] = "";
		const bool built = BuildHand(J, hand, palmJ, joints, right, rig, frames, palm, why, sizeof(why));
		Check(built, "F1 %s: canonical frames measured on the %s side (%s)", meshes[h], right ? "right" : "left", built ? "every fold check passed" : why);
		if (!built) continue;
		const int side = right ? SIDE_RIGHT : SIDE_LEFT;
		auto getQ = [&r, nj](int frame, int joint, double q[4]) { for (int k = 0; k < 4; k++) q[k] = r.frames[(size_t)frame * nj + joint].q[k]; };
		HandPoses hp;
		const bool posesBuilt = BuildPoses(rig, frames, side, r.numFrames, getQ, hp, why, sizeof(why));
		Check(posesBuilt && rig.Palm == palmJ && rig.PalmBelow.size() == 20, "F1 %s: the authored poses built (%s); the palm joint and the 20 joints below it",
			meshes[h], posesBuilt ? "kPoseSources" : why);
		if (!posesBuilt) continue;
		printf("  palm normal (out of the palm, the side the fingers fold to) %.5f %.5f %.5f\n", palm[0], palm[1], palm[2]);
		double minFold = 9;
		for (int f = 0; f < 5; f++)
			for (int j = 0; j < 3; j++)
			{
				const FingerFrame &F = frames[f][j];
				minFold = fmin(minFold, F.fold);
				printf("  %-6s %d %-20s X %+.4f %+.4f %+.4f  Z %+.4f %+.4f %+.4f  fold %.3f%s\n", fingerNames[f], j + 1, kFingerJointNames[f][j],
					F.X[0], F.X[1], F.X[2], F.Z[0], F.Z[1], F.Z[2], F.fold, F.borrowed ? "  tipaxis" : "");
			}
		printf("  smallest Y.palm %.3f (measure_avatar refuses <= 0.05)\n", minFold);
		// rest.json for measure_ref.py
		fprintf(rj, "%s\"%s\": {\"side\": \"%c\", \"joints\": [\n", h ? ",\n" : "", meshes[h], sideCh);
		for (int j = 0; j < nj; j++)
		{
			const TRSd &t = r.joints[j].rest;
			fprintf(rj, "  {\"name\": \"%s\", \"parent\": %d, \"t\": [%.17g, %.17g, %.17g], \"r\": [%.17g, %.17g, %.17g, %.17g], \"s\": [%.17g, %.17g, %.17g]}%s\n",
				r.joints[j].name.c_str(), r.joints[j].parent, t.t[0], t.t[1], t.t[2], t.q[0], t.q[1], t.q[2], t.q[3], t.s[0], t.s[1], t.s[2], j + 1 < nj ? "," : "");
		}
		fprintf(rj, "], \"ours\": {\"palm\": [%.17g, %.17g, %.17g], \"frames\": [\n", palm[0], palm[1], palm[2]);
		for (int f = 0; f < 5; f++)
			for (int j = 0; j < 3; j++)
			{
				const FingerFrame &F = frames[f][j];
				fprintf(rj, "  {\"finger\": \"%s\", \"joint\": %d, \"X\": [%.17g, %.17g, %.17g], \"Y\": [%.17g, %.17g, %.17g], \"Z\": [%.17g, %.17g, %.17g], \"borrowed\": %s}%s\n",
					fingerNames[f], j + 1, F.X[0], F.X[1], F.X[2], F.Y[0], F.Y[1], F.Y[2], F.Z[0], F.Z[1], F.Z[2], F.borrowed ? "true" : "false", (f == 4 && j == 2) ? "" : ",");
			}
		fprintf(rj, "]}}");

		// ---- W: the take walked. Every position is taken back into the REST placement of the hand
		// (Wrest_root . W_root(f)^-1), so the hand's own motion through the take is removed.
		const V3 palmC = Pos(Wrest[palmJ].data());	// HANDPALM_joint at rest: the palm's centre
		const V3 pn = { palm[0], palm[1], palm[2] };
		const double L = Len(Sub(Pos(Wrest[tips[2]].data()), Pos(Wrest[hand].data())));	// wrist to middle tip joint
		struct FM { double straight, dpalm, side, ext; };
		// extension: (tip - knuckle) along the hand's own finger axis f (wrist -> middle knuckle, rest) / chain length:
		// 1 = straight AND in line with the palm. Straightness alone also scores a finger bent 90 degrees at the knuckle.
		V3 fAxis = Sub(Pos(Wrest[joints[2][0]].data()), Pos(Wrest[hand].data()));
		{ const double l = Len(fAxis); fAxis = { fAxis.x / l, fAxis.y / l, fAxis.z / l }; }
		auto measure = [&](int fr, FM out[5]) {
			Mats W;
			ComposeChain(r, &r.frames[(size_t)fr * nj], W);
			double inv[16], back[16];
			MatInvert(W[hand].data(), inv);
			MatMul(Wrest[hand].data(), inv, back);
			for (int f = 0; f < 5; f++)
			{
				const V3 p0 = MatPoint(back, Pos(W[joints[f][0]].data())), p1 = MatPoint(back, Pos(W[joints[f][1]].data()));
				const V3 p2 = MatPoint(back, Pos(W[joints[f][2]].data())), p3 = MatPoint(back, Pos(W[tips[f]].data()));
				const double chain = Len(Sub(p1, p0)) + Len(Sub(p2, p1)) + Len(Sub(p3, p2));
				out[f].straight = Len(Sub(p3, p0)) / chain;
				out[f].dpalm = Len(Sub(p3, palmC)) / L;
				out[f].side = Dot(Sub(p3, palmC), pn) / L;
				out[f].ext = Dot(Sub(p3, p0), fAxis) / chain;
			}
		};
		FM m0[5];
		measure(0, m0);
		int mostOpen[5], mostClosed[5];
		double vOpen[5], vClosed[5];
		for (int f = 0; f < 5; f++) { mostOpen[f] = mostClosed[f] = 0; vOpen[f] = -1; vClosed[f] = 1e9; }
		int bestFist = -1; double bestFistV = 1e9;
		int bestOpen = -1; double bestOpenV = -1;
		std::vector<std::array<FM, 5>> all(r.numFrames);
		for (int fr = 0; fr < r.numFrames; fr++)
		{
			measure(fr, all[fr].data());
			double sumD = 0, minS = 9;
			for (int f = 0; f < 5; f++)
			{
				const FM &m = all[fr][f];
				const double openness = (f == 0) ? m.straight : m.ext;	// the thumb does not lie along f
				if (openness > vOpen[f]) { vOpen[f] = openness; mostOpen[f] = fr; }
				if (m.dpalm < vClosed[f]) { vClosed[f] = m.dpalm; mostClosed[f] = fr; }
				if (f > 0) sumD += m.dpalm;
				minS = fmin(minS, f == 0 ? m.straight : m.ext);
			}
			if (sumD < bestFistV) { bestFistV = sumD; bestFist = fr; }
			if (minS > bestOpenV) { bestOpenV = minS; bestOpen = fr; }
		}
		printf("  W the take walked (%d frames), the hand's own motion removed. straight = tip-to-knuckle / chain length; ext = the same along the palm's finger axis (1 = straight and in line with the palm);\n"
			"    to-palm = tip (UP_TOP) to HANDPALM_joint / hand length (%.3f); side = along the palm normal / hand length\n", r.numFrames, L);
		printf("    %-6s | most open frame (ext; thumb: straight) | most closed frame (to-palm) | frame 0: straight ext to-palm | frame 318: straight ext to-palm side | 317 / 319 to-palm\n", "finger");
		for (int f = 0; f < 5; f++)
		{
			printf("    %-6s | %4d (%.4f)                          | %4d (%.4f)                 | %.4f %.4f %.4f               | %.4f %.4f %.4f %+.4f              | %.4f / %.4f\n",
				fingerNames[f], mostOpen[f], vOpen[f], mostClosed[f], vClosed[f], all[0][f].straight, all[0][f].ext, all[0][f].dpalm,
				all[318][f].straight, all[318][f].ext, all[318][f].dpalm, all[318][f].side, all[317][f].dpalm, all[319][f].dpalm);
		}
		double sum0 = 0, sum318 = 0, min0 = 9;
		for (int f = 1; f < 5; f++) { sum318 += all[318][f].dpalm; }
		for (int f = 0; f < 5; f++) { sum0 += all[0][f].straight; min0 = fmin(min0, f == 0 ? all[0][f].straight : all[0][f].ext); }
		printf("    whole hand: most closed (least sum of the four fingers' tip-to-palm) frame %d (%.4f; frame 318: %.4f); most open (the least open finger the most open) frame %d (%.4f; frame 0: %.4f)\n",
			bestFist, bestFistV, sum318, bestOpen, bestOpenV, min0);
		// list the frames tied within 0.5% of the best fist and of the best open, to show whether 0/318 are among them
		{
			int nf = 0, no = 0;
			printf("    frames within 0.5%% of the best fist:");
			for (int fr = 0; fr < r.numFrames; fr++)
			{
				double s = 0;
				for (int f = 1; f < 5; f++) s += all[fr][f].dpalm;
				if (s <= bestFistV * 1.005) { if (nf < 40) printf(" %d", fr); nf++; }
			}
			printf("%s (%d)\n    frames within 0.5%% of the best open:", nf > 40 ? " ..." : "", nf);
			for (int fr = 0; fr < r.numFrames; fr++)
			{
				double mn = 9;
				for (int f = 0; f < 5; f++) mn = fmin(mn, f == 0 ? all[fr][f].straight : all[fr][f].ext);
				if (mn >= bestOpenV * 0.995) { if (no < 40) printf(" %d", fr); no++; }
			}
			printf("%s (%d)\n", no > 40 ? " ..." : "", no);
		}
		bool openStraight = true;
		double minExt = 9;
		for (int f = 1; f < 5; f++) { openStraight = openStraight && all[kOpenFrame][f].straight > 0.98; minExt = fmin(minExt, all[kOpenFrame][f].ext); }
		Check(openStraight, "W %s: the open frame %d has index, middle, ring and pinky straight (straightness > 0.98 each)", meshes[h], kOpenFrame);
		printf("  W %s finding: in frame %d those four are bent a little at the knuckle (ext as low as %.3f); the take's most open hand is frame %d (%.3f). "
			"The thumb's straightness is %.4f in frame %d, %.4f at its best (frame %d).\n",
			meshes[h], kOpenFrame, minExt, bestOpen, bestOpenV, all[kOpenFrame][0].straight, kOpenFrame, vOpen[0], mostOpen[0]);
		bool fistIn = true;
		for (int f = 1; f < 5; f++) fistIn = fistIn && all[kFist][f].dpalm < 0.45 && all[kFist][f].side > 0.0;
		Check(fistIn, "W %s: the fist frame %d has every finger's tip within 0.45 hand lengths of the palm centre, on the palm side", meshes[h], kFist);

		// ---- F2: the pose rotations against mkfbxposes.py's own output
		if (haveRot && rotFile.count("fbx_fist") && rotFile["fbx_fist"].count(sideCh))
		{
			const int cand[] = { 317, 318, 319 };
			for (int fr : cand)
			{
				double worst = 0, worstOpen = 0, worstConv = 0;
				for (int f = 0; f < 5; f++)
					for (int j = 0; j < 3; j++)
					{
						const int jt = joints[f][j];
						double q[4], qo[4];
						const double *ref = r.frames[(size_t)0 * nj + jt].q;
						PoseRotation(frames[f][j], ref, r.frames[(size_t)fr * nj + jt].q, q);
						PoseRotation(frames[f][j], ref, r.frames[(size_t)kOpenFrame * nj + jt].q, qo);
						worst = fmax(worst, QAngleDeg(q, rotFile["fbx_fist"][sideCh][f][j].data()));
						// mkfbxposes.py reads hand B MIRRORED into a right-handed space and builds a right-handed frame
						// there; this hand's own frame is the mirror image of the right hand's (Z reflected). The same
						// rotation then reads (-x, -y, z, w).
						const double qc[4] = { -q[0], -q[1], q[2], q[3] };
						worstConv = fmax(worstConv, QAngleDeg(qc, rotFile["fbx_fist"][sideCh][f][j].data()));
						worstOpen = fmax(worstOpen, QAngleDeg(qo, rotFile["fbx_open"][sideCh][f][j].data()));
					}
				printf("  F2 frames 0 -> %d through PoseRotation vs vrhandpose.txt fbx_fist %c (mkfbxposes.py, Blender 1 -> 319): worst joint %.4f deg (as (-x,-y,z,w): %.4f deg); open vs fbx_open %.4f deg\n",
					fr, sideCh, worst, worstConv, worstOpen);
				if (fr == kFist)
				{
					const double w = right ? worst : worstConv;
					Check(w < 0.25 && worstOpen < 0.05, "F2 %s: the engine's fist and open poses equal the owner's pipeline's, every joint within %.3f deg%s",
						meshes[h], w, right ? "" : " (read in the mirrored frame mkfbxposes.py uses for the left hand)");
				}
			}
			// and per joint, at the frame used
			printf("  F2 per joint at frame %d: ours / vrhandpose.txt angle from open (deg):", kFist);
			for (int f = 0; f < 5; f++)
				for (int j = 0; j < 3; j++)
				{
					const double *q = rotFile["fbx_fist"][sideCh][f][j].data();
					printf("%s %s%d %.1f/%.1f", j == 0 ? "\n     " : "", fingerNames[f], j + 1,
						2 * acos(fmin(1.0, fabs((double)hp.Target[POSE_FIST][f][j][3]))) * 180 / M_PI, 2 * acos(fmin(1.0, fabs(q[3]))) * 180 / M_PI);
				}
			printf("\n");
		}
		else Check(false, "F2 %s: no fbx_fist %c lines in %s", meshes[h], sideCh, argv[3]);

		// ---- F3: fold direction. MA :545-552 refuses a frame whose fold carries the bone away from the palm; this
		// checks it as motion: each joint alone turned a tenth of the way to its authored fist moves its finger's tip
		// along the palm normal.
		{
			bool ok = true;
			printf("  F3 each joint alone 0.1 of the way to the authored fist, tip travel along the palm normal / hand length (x1000):\n   ");
			for (int f = 0; f < 5; f++)
			{
				printf(" %s", fingerNames[f]);
				for (int jj = 0; jj < 3; jj++)
				{
					SideState rot;
					for (int ff = 0; ff < 5; ff++)
						for (int j = 0; j < 3; j++)
							QSlerp(hp.Open[ff][j], hp.Target[POSE_FIST][ff][j], (ff == f && j == jj) ? 0.1f : 0.f, rot.Q[ff][j]);
					rot.Init = true;
					std::vector<std::array<float, 16>> G(nj);
					for (int j = 0; j < nj; j++) for (int k = 0; k < 16; k++) G[j][k] = (float)Wrest[j][k];
					std::vector<char> moved;
					ApplyFingerTurns(reinterpret_cast<float (*)[16]>(G.data()), nj, rig, rot, moved);
					const V3 t0 = Pos(Wrest[tips[f]].data());
					const V3 t1 = { G[tips[f]][12], G[tips[f]][13], G[tips[f]][14] };
					const double along = Dot(Sub(t1, t0), pn) / L;
					printf(" %+.1f", along * 1000);
					// the thumb: in the fist its MID and TOP turn it ACROSS the palm (toward the little finger), not into it;
					// printed, not judged (test_curl.py exempts the thumb from its distance check for the same reason)
					if (f > 0) ok = ok && along > 0.0;
					if (f == 0)
					{
						const V3 kAxis = Sub(Pos(Wrest[joints[1][0]].data()), Pos(Wrest[joints[4][0]].data()));
						printf("(across %+.1f)", -Dot(Sub(t1, t0), kAxis) / Len(kAxis) / L * 1000);
					}
				}
			}
			printf("\n");
			Check(ok, "F3 %s: every joint of index, middle, ring and pinky starts its fold to the authored fist toward the palm (the thumb: across it, printed)", meshes[h]);
		}

		// ---- S1-S4: the six states through the engine path
		{
			struct St { const char *name; float grip, trigger; bool tt, th; int pose; };	// pose: the PoseRule pose; -1 = open
			const St states[] =
			{
				{ "open",      0, 0, false, false, -1 },
				{ "fist",      1, 1, true,  true,  POSE_FIST },
				{ "point",     1, 0, false, true,  POSE_POINT },
				{ "thumbup",   1, 1, true,  false, POSE_THUMBUP },
				{ "fingergun", 1, 0, false, false, POSE_FINGERGUN },
				{ "trigger",   0, 1, true,  false, POSE_THUMBUP },	// at no grip only the index moves: the fist's index
			};
			// the composition a state is at full input: per finger the take frame its three joints come from, and the palm's
			auto composition = [&](const St &s, int src[5], int &palmSrc) {
				for (int f = 0; f < 5; f++) src[f] = kOpenFrame;
				palmSrc = kOpenFrame;
				if (s.pose < 0) return;
				const PoseSource &ps = kPoseSources[side][s.pose];
				if (s.grip > 0) { for (int f = 0; f < 5; f++) src[f] = ps.Finger[f]; if (ps.Palm >= 0) palmSrc = ps.Palm; }
				else src[FINGER_INDEX] = ps.Finger[FINGER_INDEX];
			};
			// the composition's locals: frame 0 (what the model draws), each finger's three joints from its frame, the palm's
			auto compose = [&](const int src[5], int palmSrc, std::vector<TRSd> &loc) {
				loc.assign(r.frames.begin() + (size_t)kDrawnFrame * nj, r.frames.begin() + (size_t)(kDrawnFrame + 1) * nj);
				for (int f = 0; f < 5; f++) for (int j = 0; j < 3; j++) loc[joints[f][j]] = r.frames[(size_t)src[f] * nj + joints[f][j]];
				loc[palmJ] = r.frames[(size_t)palmSrc * nj + palmJ];
			};
			auto rotAngle = [](const double *a, const double *b) {
				// the angle of a^T b on the 3x3s with their column scales divided out
				double A[9], B[9];
				for (int c = 0; c < 3; c++)
				{
					const double la = sqrt(a[c * 4] * a[c * 4] + a[c * 4 + 1] * a[c * 4 + 1] + a[c * 4 + 2] * a[c * 4 + 2]);
					const double lb = sqrt(b[c * 4] * b[c * 4] + b[c * 4 + 1] * b[c * 4 + 1] + b[c * 4 + 2] * b[c * 4 + 2]);
					for (int k = 0; k < 3; k++) { A[c * 3 + k] = a[c * 4 + k] / la; B[c * 3 + k] = b[c * 4 + k] / lb; }
				}
				double tr = 0;
				for (int c = 0; c < 3; c++) for (int k = 0; k < 3; k++) tr += A[c * 3 + k] * B[c * 3 + k];
				return acos(fmax(-1.0, fmin(1.0, (tr - 1) / 2))) * 180 / M_PI;
			};
			auto localOf = [](const double *gp, const double *gj, double out[16]) { double ip[16]; MatInvert(gp, ip); MatMul(ip, gj, out); };
			// the joints S1 compares: every finger joint, every tip, the palm
			std::vector<int> cmp;
			for (int f = 0; f < 5; f++) { for (int j = 0; j < 3; j++) cmp.push_back(joints[f][j]); cmp.push_back(tips[f]); }
			cmp.push_back(palmJ);
			const double mm = 0.11094212 * 1000;	// file units -> mm (MODELDEF: 0.19 m over the hand's 1.7126 units)

			for (const St &s : states)
			{
				Inputs in; in.grip = s.grip; in.trigger = s.trigger; in.triggerTouch = s.tt; in.thumbTouch = s.th; in.valid = true;
				{
					PoseInputs pi;
					PoseRule(in, pi);
					if (s.pose >= 0) Check(pi.Pose == s.pose, "S0 %s %s: PoseRule picks pose %d (expected %d)", meshes[h], s.name, pi.Pose, s.pose);
				}
				Posed p = PoseHand(r, Wrest, rig, hp, in);
				int src[5], palmSrc;
				composition(s, src, palmSrc);
				std::vector<TRSd> loc;
				compose(src, palmSrc, loc);
				Mats WC;
				ComposeChain(r, loc.data(), WC);
				double worstAng = 0, worstPos = 0;
				std::string worstName;
				for (int jt : cmp)
				{
					const int pj = r.joints[jt].parent;
					double lo[16], lc[16], gp[16], gj[16];
					for (int k = 0; k < 16; k++) { gp[k] = p.G[pj][k]; gj[k] = p.G[jt][k]; }
					localOf(gp, gj, lo);
					localOf(WC[pj].data(), WC[jt].data(), lc);
					const double a = rotAngle(lo, lc);
					if (a > worstAng) { worstAng = a; worstName = r.joints[jt].name; }
					worstPos = fmax(worstPos, Len(Sub(Pos(lo), Pos(lc))) * mm);
				}
				printf("  S1 %-9s (grip %.0f trigger %.0f touch %d thumb %d): thumb %d index %d middle %d ring %d pinky %d palm %d -- worst joint %.5f deg (%s); "
					"local offsets within %.3f mm (the take's own animated translations, which a turn does not move)\n",
					s.name, s.grip, s.trigger, (int)s.tt, (int)s.th, src[0], src[1], src[2], src[3], src[4], palmSrc, worstAng, worstName.c_str(), worstPos);
				Check(worstAng < 0.01, "S1 %s %s: every finger joint, tip and the palm has its authored frame's local rotation (worst %.5f deg)", meshes[h], s.name, worstAng);

				// S2: the extended finger(s) where the composition has them, in the hand (Root_joint's frame)
				std::vector<int> ext;
				if (!strcmp(s.name, "point")) ext = { FINGER_INDEX };
				if (!strcmp(s.name, "thumbup")) ext = { FINGER_THUMB };
				if (!strcmp(s.name, "fingergun")) ext = { FINGER_THUMB, FINGER_INDEX };
				for (int f : ext)
				{
					double ih[16], ihc[16], d = 0, fromOpen = 0, gh[16];
					for (int k = 0; k < 16; k++) gh[k] = p.G[hand][k];
					MatInvert(gh, ih);
					MatInvert(WC[hand].data(), ihc);
					double i0[16];
					MatInvert(Wrest[hand].data(), i0);
					for (int j = 0; j < 4; j++)
					{
						const int jt = j < 3 ? joints[f][j] : tips[f];
						double g[16];
						for (int k = 0; k < 16; k++) g[k] = p.G[jt][k];
						const V3 a = MatPoint(ih, Pos(g)), b = MatPoint(ihc, Pos(WC[jt].data())), o = MatPoint(i0, Pos(Wrest[jt].data()));
						d = fmax(d, Len(Sub(a, b)) * mm);
						fromOpen = fmax(fromOpen, Len(Sub(a, o)) * mm);
					}
					const bool palmStill = rotAngle(p.G[palmJ].data(), Wrest[palmJ].data()) < 1e-3;
					Check(d < 0.6 && palmStill, "S2 %s %s: the %s is the authored one from frame %d -- its joints and tip within %.3f mm of the composition in the hand "
						"(the take's own animated translations), the palm under it unturned (its own pose moves it %.1f mm from the open hand)",
						meshes[h], s.name, fingerNames[f], src[f], d, fromOpen);
				}
				if (!strcmp(s.name, "open"))
				{
					double d = 0;
					for (int j = 0; j < nj; j++) for (int k = 0; k < 16; k++) d = fmax(d, fabs(p.G[j][k] - Wrest[j][k]));
					Check(d < 1e-5, "S3 %s open: every joint of the hand, the palm included, at rest (max matrix difference %.2g)", meshes[h], d);
				}
				char fn[1024];
				snprintf(fn, sizeof(fn), "%s/%s_ctl_%s.txt", outDir.c_str(), meshes[h], s.name); DumpMesh(fn, r, p.P);
			}

			// S4: half-way. Every in-between is a mix of two authored poses: at grip 0.5 (point) each finger joint's local
			// rotation is the slerp half-way between its open-frame and its pose-frame local rotations.
			{
				Inputs in; in.grip = 0.5f; in.trigger = 0; in.triggerTouch = false; in.thumbTouch = true; in.valid = true;
				Posed p = PoseHand(r, Wrest, rig, hp, in);
				const PoseSource &ps = kPoseSources[side][POSE_POINT];
				double worst = 0, biggest = 0;
				for (int f = 0; f < 5; f++)
					for (int j = 0; j < 3; j++)
					{
						const int jt = joints[f][j], pj = r.joints[jt].parent;
						double lo[16], gp[16], gj[16], a0[16], a1[16];
						for (int k = 0; k < 16; k++) { gp[k] = p.G[pj][k]; gj[k] = p.G[jt][k]; }
						localOf(gp, gj, lo);
						ComposeTRS(r.frames[(size_t)kOpenFrame * nj + jt], a0);
						ComposeTRS(r.frames[(size_t)ps.Finger[f] * nj + jt], a1);
						const double total = rotAngle(a0, a1), x = rotAngle(a0, lo), y = rotAngle(lo, a1);
						biggest = fmax(biggest, total);
						worst = fmax(worst, fmax(fabs(x - total / 2), fabs(y - total / 2)));
					}
				Check(worst < 0.01, "S4 %s point at grip 0.5: every finger joint half-way along the arc between its two authored local rotations (within %.5f deg; arcs up to %.1f deg)",
					meshes[h], worst, biggest);
			}

			// ---- H0: the owner's raw frames against mkfbxposes.py's own output for the hand that authored them
			{
				double worstOwn = 0;
				std::string worstPose;
				int n = 0;
				for (int p = 0; p < kOwnerPoseCount; p++)
				{
					const OwnerPose &op = kOwnerPoses[p];
					if (op.blender <= 0 || OwnerRawFrame(p, side) < 0) continue;
					if (!rotFile.count(op.name) || !rotFile[op.name].count(sideCh)) { Check(false, "H0 %s: %s %c is not in vrhandpose.txt", meshes[h], op.name, sideCh); continue; }
					const int fr = OwnerRawFrame(p, side);
					double w = 0;
					for (int f = 0; f < 5; f++)
						for (int j = 0; j < 3; j++)
						{
							const int jt = joints[f][j];
							double q[4];
							PoseRotation(frames[f][j], r.frames[(size_t)kDrawnFrame * nj + jt].q, r.frames[(size_t)fr * nj + jt].q, q);
							const double qc[4] = { right ? q[0] : -q[0], right ? q[1] : -q[1], q[2], q[3] };	// the left read in MKP's mirrored frame (F2)
							w = fmax(w, QAngleDeg(qc, rotFile[op.name][sideCh][f][j].data()));
						}
					if (w > worstOwn) { worstOwn = w; worstPose = op.name; }
					n++;
				}
				Check(n > 0 && worstOwn < 0.25, "H0 %s: the %d raw poses this hand authored, from take frame = Blender - 1, equal mkfbxposes.py's own rotations for "
					"this hand within %.3f deg (worst %s)", meshes[h], n, worstOwn, worstPose.c_str());
			}

			// ---- H1: the hold poses of this hand's own take
			HoldPoses hold;
			{
				const bool ok = BuildHoldPoses(rig, frames, side, r.numFrames, getQ, hold, why, sizeof(why));
				int have = 0, rawOk = 0, raw = 0;
				std::string lacks;
				for (int p = 0; p < kOwnerPoseCount; p++)
				{
					const OwnerPose &op = kOwnerPoses[p];
					if (hold.Have[p]) have++;
					if (op.blender > 0)
					{
						raw++;
						const bool authored = op.hands == HANDS_BOTH || (op.hands == HANDS_A) == right;
						if (hold.Have[p] == authored) rawOk++;
						if (!hold.Have[p]) lacks += std::string(lacks.empty() ? "" : ", ") + op.name;
					}
				}
				printf("  H1 %s: %d of %d held-object / weapon-grip poses from its own take; it lacks (never mirrored from the other hand): %s\n",
					meshes[h], have, kOwnerPoseCount, lacks.c_str());
				Check(ok && rawOk == raw, "H1 %s: built (%s); each of the %d raw poses is on this hand exactly when this hand authored it", meshes[h], ok ? "ok" : why, raw);
			}

			// ---- H2: every hold pose at full input, through the engine path
			{
				struct HC { std::string label; HeldState held; Inputs in; };
				std::vector<HC> cases;
				Inputs in0; in0.valid = true;
				for (int p = 0; p < kOwnerPoseCount; p++) { HC c; c.label = std::string("pose ") + kOwnerPoses[p].name; c.held.pose = kOwnerPoses[p].name; c.in = in0; cases.push_back(c); }
				for (int sj = GRIPSUBJ_Round; sj < GRIPSUBJ_MAX; sj++) { HC c; c.label = std::string("subject ") + GripSubjectName(sj); c.held.subject = sj; c.in = in0; cases.push_back(c); }
				const char *classes[] = { "", "pistol", "shotgun", "ssg", "rifle", "rocket", "plasma", "bfg", "saw" };
				for (const char *cl : classes)
					for (int t = 0; t < 2; t++)
					{
						HC c; c.label = std::string("weapon ") + (*cl ? cl : "(no class)") + (t ? ", trigger pulled" : ", trigger off");
						c.held.weapon = true; c.held.gripClass = cl;
						c.in = in0; c.in.trigger = (float)t; c.in.triggerTouch = t != 0;
						cases.push_back(c);
					}
				double worstQ = 0, worstLocal = 0;
				std::string worstQName, worstLocalName;
				int posed = 0, absent = 0, absentOk = 0;
				for (const HC &c : cases)
				{
					HoldChoice ch;
					Posed p = PoseHand(r, Wrest, rig, hp, c.in, 400, &hold, &c.held, &ch);
					// the composition the choice names: per finger the authored frame, else the controller's (PoseRule at
					// these inputs: amount 0 -> the open frame, 1 -> kPoseSources)
					PoseInputs pi;
					PoseRule(c.in, pi);
					int src[5];
					bool all = true;
					for (int f = 0; f < 5; f++)
					{
						int fr;
						if (f == FINGER_INDEX && ch.IdxReady >= 0) fr = hold.Frame[c.in.trigger >= 1.f ? ch.IdxFire : ch.IdxReady][f];
						else if (ch.Src[f][0] >= 0) fr = hold.Frame[ch.Src[f][0]][f];
						else { all = false; fr = pi.Amount[f] >= 1.f ? kPoseSources[side][pi.Pose].Finger[f] : kOpenFrame; }
						src[f] = fr;
					}
					const int pm = ch.Src[FINGER_MIDDLE][0];
					const int palmSrc = (all && pm >= 0 && hold.PalmDriven[pm]) ? hold.PalmFrame[pm] : kDrawnFrame;
					if (!ch.Any())
					{
						// nothing for this hand: exactly the controller's pose
						absent++;
						Posed q = PoseHand(r, Wrest, rig, hp, c.in);
						double d = 0;
						for (int j = 0; j < nj; j++) for (int k = 0; k < 16; k++) d = fmax(d, fabs(p.G[j][k] - q.G[j][k]));
						if (d == 0.0) absentOk++;
						continue;
					}
					posed++;
					// (a) the drawn rotations against the authored frames' rotations made in double
					for (int f = 0; f < 5; f++)
						for (int j = 0; j < 3; j++)
						{
							const int jt = joints[f][j];
							double q[4];
							PoseRotation(frames[f][j], r.frames[(size_t)kDrawnFrame * nj + jt].q, r.frames[(size_t)src[f] * nj + jt].q, q);
							const double a[4] = { p.rot.Q[f][j][0], p.rot.Q[f][j][1], p.rot.Q[f][j][2], p.rot.Q[f][j][3] };
							// the angle of a^-1 q, by atan2 (exact for small angles)
							const double w = a[3] * q[3] + a[0] * q[0] + a[1] * q[1] + a[2] * q[2];
							const double x = a[3] * q[0] - a[0] * q[3] - a[1] * q[2] + a[2] * q[1];
							const double y = a[3] * q[1] + a[0] * q[2] - a[1] * q[3] - a[2] * q[0];
							const double z = a[3] * q[2] - a[0] * q[1] + a[1] * q[0] - a[2] * q[3];
							const double ang = 2 * atan2(sqrt(x * x + y * y + z * z), fabs(w)) * 180 / M_PI;
							if (ang > worstQ) { worstQ = ang; worstQName = c.label; }
						}
					// (a) the palm likewise (identity frame: its own axes)
					{
						FingerFrame I;
						I.X[0] = 1; I.Y[1] = 1; I.Z[2] = 1;
						double q[4];
						PoseRotation(I, r.frames[(size_t)kDrawnFrame * nj + palmJ].q, r.frames[(size_t)palmSrc * nj + palmJ].q, q);
						const double a2[4] = { p.palm.Q[0], p.palm.Q[1], p.palm.Q[2], p.palm.Q[3] };
						const double w = a2[3] * q[3] + a2[0] * q[0] + a2[1] * q[1] + a2[2] * q[2];
						const double x = a2[3] * q[0] - a2[0] * q[3] - a2[1] * q[2] + a2[2] * q[1];
						const double y = a2[3] * q[1] + a2[0] * q[2] - a2[1] * q[3] - a2[2] * q[0];
						const double z = a2[3] * q[2] - a2[0] * q[1] + a2[1] * q[0] - a2[2] * q[3];
						const double ang = 2 * atan2(sqrt(x * x + y * y + z * z), fabs(w)) * 180 / M_PI;
						if (ang > worstQ) { worstQ = ang; worstQName = c.label + " (palm)"; }
					}
					// (b) the whole palette path: local rotations against the composition's
					std::vector<TRSd> loc;
					compose(src, palmSrc, loc);
					Mats WC;
					ComposeChain(r, loc.data(), WC);
					for (int jt : cmp)
					{
						const int pj = r.joints[jt].parent;
						double lo[16], lc[16], gp[16], gj[16];
						for (int k = 0; k < 16; k++) { gp[k] = p.G[pj][k]; gj[k] = p.G[jt][k]; }
						localOf(gp, gj, lo);
						localOf(WC[pj].data(), WC[jt].data(), lc);
						const double a = rotAngle(lo, lc);
						if (a > worstLocal) { worstLocal = a; worstLocalName = c.label + " / " + r.joints[jt].name; }
					}
					if (c.label == "pose hold_magazine" || c.label == "pose hold_round" || c.label == "pose fbx_curl" || c.label == "pose hold_forend"
						|| c.label == "pose hold_foregrip" || c.label == "pose hold_inserting" || c.label == "weapon pistol, trigger off")
					{
						std::string lab = c.label.substr(c.label.find(' ') + 1);
						for (char &ch2 : lab) if (ch2 == ' ' || ch2 == ',') ch2 = '_';
						char fn[1024];
						snprintf(fn, sizeof(fn), "%s/%s_hold_%s.txt", outDir.c_str(), meshes[h], lab.c_str()); DumpMesh(fn, r, p.P);
					}
				}
				printf("  H2 %s: %zu held states (%d poses by name, %d subjects, %d weapon grips): %d posed by the owner's poses, %d absent on this hand "
					"(%d of them exactly the controller's pose); drawn rotations vs the authored frames (double): worst %.2g deg (%s); "
					"palette path, local rotations vs the composition: worst %.2g deg (%s)\n",
					meshes[h], cases.size(), kOwnerPoseCount, GRIPSUBJ_MAX - 1, (int)(cases.size() - kOwnerPoseCount - (GRIPSUBJ_MAX - 1)), posed, absent, absentOk,
					worstQ, worstQName.c_str(), worstLocal, worstLocalName.c_str());
				Check(posed > 0 && worstQ < 1e-5, "H2 %s: every hold pose at full input is its authored frame(s): drawn rotations within %.2g deg (< 1e-5)", meshes[h], worstQ);
				Check(worstLocal < 1e-4, "H2 %s: and through the float palette, every finger joint, tip and the palm within %.2g deg of the composition", meshes[h], worstLocal);
				Check(absentOk == absent, "H2 %s: a pose this hand lacks leaves exactly the controller's pose (%d of %d)", meshes[h], absentOk, absent);
			}
		}

		// the two frames used, as the FBX has them
		for (int fr : { kOpenFrame, kFist })
		{
			Mats W;
			ComposeChain(r, &r.frames[(size_t)fr * nj], W);
			Mats P(nj);
			for (int j = 0; j < nj; j++) { double ri[16]; MatInvert(Wrest[j].data(), ri); MatMul(W[j].data(), ri, P[j].data()); }
			// the hand's own motion removed, so the frame is drawn where the engine draws the hand
			double inv[16], back[16];
			MatInvert(W[hand].data(), inv);
			MatMul(Wrest[hand].data(), inv, back);
			for (int j = 0; j < nj; j++) { double t[16]; MatMul(back, P[j].data(), t); memcpy(P[j].data(), t, sizeof(t)); }
			char fn[1024];
			snprintf(fn, sizeof(fn), "%s/%s_fbx_%d.txt", outDir.c_str(), meshes[h], fr); DumpMesh(fn, r, P);
		}
	}
	fprintf(rj, "\n}\n");
	fclose(rj);
	printf("\n%s: %d failure(s)\n", gFails ? "FAIL" : "PASS", gFails);
	return gFails ? 1 : 0;
}
