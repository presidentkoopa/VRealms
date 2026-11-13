// tools/fbxload_test/test_fingers.cpp -- the VR hands' fingers on the REAL hand_final.fbx, both hands, outside the
// engine: the loader core (fbx_skinned.cpp) + ufbx + the renderer's own vr_handpose_math.h, nothing copied.
//
//   test_fingers <hand_final.fbx> <vrhandcurl.txt> <UZDXREMA vrhandpose.txt> <outdir>
//
//   W   THE TAKE WALKED: every frame of each hand's own take; per finger the most open frame (straightest) and the
//       most closed (fingertip nearest the palm), and the open (0) and fist (318) frames the engine uses, measured.
//   F1  the canonical bone frames (measure_avatar.py's algorithm, vr_handpose_math.h MeasureFingerFrames) on each
//       hand's rest pose; rest.json is written for measure_ref.py, which runs measure_avatar.py's OWN lines on it.
//   F2  the pose rotations (mkfbxposes.py's formula, PoseRotation) from this loader's frames 0 and 318 against the
//       ones mkfbxposes.py wrote from Blender frames 1 and 319 (UZDXREMA wadsrc/static/vrhandpose.txt), and the
//       neighbouring frames, to show which numbering is right.
//   F3  fold direction: each finger curled alone, by the FRIK table and by the FBX poses, moves its tip toward
//       the palm (along the palm normal), both hands.
//   F4  fist (grip 1, trigger 1, both touches) through the engine path (StepSide converged, PosePalette, the
//       engine's swapped palette): every fingertip within the palm region; with thumb curl 1 the hand equals the
//       FBX's own fist frame; the tip (UP_TOP) carried.
//   F5  point (grip 1, trigger 0, no trigger touch, thumb touch): the index exactly at rest.
//       thumb up (grip 1, trigger 1, no thumb touch): the thumb exactly at rest.
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

static bool ReadTable(const char *path, CurlJoint table[5][3])
{
	FILE *f = fopen(path, "r");
	if (!f) return false;
	static const char *names[5] = { "thumb", "index", "middle", "ring", "pinky" };
	char line[512];
	int seen = 0;
	while (fgets(line, sizeof(line), f))
	{
		char fn[32]; int j; double deg, ax, ay, az;
		if (sscanf(line, " curl %31s %d %lf %lf %lf %lf", fn, &j, &deg, &ax, &ay, &az) != 6) continue;
		for (int k = 0; k < 5; k++)
			if (!strcmp(fn, names[k]) && j >= 1 && j <= 3)
			{
				const float a[3] = { (float)ax, (float)ay, (float)az };
				const float l = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
				for (int c = 0; c < 3; c++) table[k][j - 1].Axis[c] = a[c] / l;
				table[k][j - 1].Degrees = (float)deg;
				seen++;
			}
	}
	fclose(f);
	return seen == 15;
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

// The engine path for one set of inputs, converged: StepSide at 90 Hz until settled, then PosePalette on the
// engine's swapped palette of the drawn frame. Returns the posed joint globals (file space) and the file-space
// palette for skinning.
struct Posed { Mats G; Mats P; SideState rot; };
static const float kS[16] = { 1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 1 };

static Posed PoseHand(const Result &r, const Mats &Wrest, const HandRig &rig, const JointPose (*poses)[3], const CurlJoint (*table)[3],
	const Inputs &in, int frames = 400)
{
	const int nj = (int)r.joints.size();
	Posed out;
	CurlState cs;
	for (int i = 0; i < frames; i++) StepSide(cs, out.rot, in, Retain(1.0 / 90.0), poses, table);

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
		reinterpret_cast<const float (*)[16]>(inv.data()), nj, rig, out.rot, reinterpret_cast<float (*)[16]>(outPal.data()), scratch, moved);
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
	if (argc < 5) { fprintf(stderr, "usage: test_fingers <fbx> <vrhandcurl.txt> <vrhandpose.txt> <outdir>\n"); return 2; }
	std::vector<char> file = ReadFile(argv[1]);
	CurlJoint table[5][3];
	if (!ReadTable(argv[2], table)) { fprintf(stderr, "cannot read the curl table %s\n", argv[2]); return 2; }
	RotFile rotFile;
	const bool haveRot = ReadRot(argv[3], rotFile);
	const std::string outDir = argv[4];
	static const char *fingerNames[5] = { "thumb", "index", "middle", "ring", "pinky" };
	static const char *tipNames[5] = { "THUMB_UP_TOP_joint", "INDEX_UP_TOP_joint", "MIDDLE_F_UP_TOP_joint", "RING_UP_TOP_joint", "PINK_UP_TOP_joint" };

	FILE *rj = fopen((outDir + "/rest.json").c_str(), "w");
	fprintf(rj, "{\n");

	const char *meshes[2] = { "Hand_Low", "Hand_Low1" };
	for (int h = 0; h < 2; h++)
	{
		const bool right = (h == 0);
		const char sideCh = right ? 'R' : 'L';
		Result r; std::string err;
		if (!Load(file.data(), file.size(), meshes[h], r, err)) { printf("%s: %s\n", meshes[h], err.c_str()); return 2; }
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

		// ---- F1: the canonical frames, and the hand's palm
		HandRig rig;
		JointPose poses[5][3];
		FingerFrame frames[5][3];
		double palm[3];
		char why[256] = "";
		std::vector<std::array<double, 4>> qD(nj), qO(nj), qF(nj);
		for (int j = 0; j < nj; j++)
			for (int k = 0; k < 4; k++)
			{
				qD[j][k] = r.frames[(size_t)kDrawnFrame * nj + j].q[k];
				qO[j][k] = r.frames[(size_t)kOpenFrame * nj + j].q[k];
				qF[j][k] = r.frames[(size_t)kFistFrame * nj + j].q[k];
			}
		const bool built = BuildHand(J, hand, joints, right, reinterpret_cast<const double (*)[4]>(qD.data()),
			reinterpret_cast<const double (*)[4]>(qO.data()), reinterpret_cast<const double (*)[4]>(qF.data()), rig, poses, frames, palm, why, sizeof(why));
		Check(built, "F1 %s: canonical frames measured on the %s side (%s)", meshes[h], right ? "right" : "left", built ? "every fold check passed" : why);
		if (!built) continue;
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
		for (int f = 1; f < 5; f++) fistIn = fistIn && all[kFistFrame][f].dpalm < 0.45 && all[kFistFrame][f].side > 0.0;
		Check(fistIn, "W %s: the fist frame %d has every finger's tip within 0.45 hand lengths of the palm centre, on the palm side", meshes[h], kFistFrame);

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
				if (fr == kFistFrame)
				{
					const double w = right ? worst : worstConv;
					Check(w < 0.25 && worstOpen < 0.05, "F2 %s: the engine's fist and open poses equal the owner's pipeline's, every joint within %.3f deg%s",
						meshes[h], w, right ? "" : " (read in the mirrored frame mkfbxposes.py uses for the left hand)");
				}
			}
			// and per joint, at the frame used
			printf("  F2 per joint at frame %d: ours / vrhandpose.txt angle from open (deg):", kFistFrame);
			for (int f = 0; f < 5; f++)
				for (int j = 0; j < 3; j++)
				{
					const double *q = rotFile["fbx_fist"][sideCh][f][j].data();
					printf("%s %s%d %.1f/%.1f", j == 0 ? "\n     " : "", fingerNames[f], j + 1,
						2 * acos(fmin(1.0, fabs((double)poses[f][j].Fist[3]))) * 180 / M_PI, 2 * acos(fmin(1.0, fabs(q[3]))) * 180 / M_PI);
				}
			printf("\n");
		}
		else Check(false, "F2 %s: no fbx_fist %c lines in %s", meshes[h], sideCh, argv[3]);

		// ---- F3: fold direction. MA :545-552 refuses a frame whose fold carries the bone away from the palm; this
		// checks it as motion: each joint alone turned a little (curl 0.1) moves its finger's tip along the palm normal.
		// Both ways of making the target: the FRIK table's axis, and the FBX's own fist.
		for (int mode = 0; mode < 2; mode++)
		{
			bool ok = true;
			printf("  F3 %s: each joint alone at curl 0.1, tip travel along the palm normal / hand length (x1000):\n   ", mode ? "FBX poses " : "FRIK table");
			for (int f = 0; f < 5; f++)
			{
				printf(" %s", fingerNames[f]);
				for (int jj = 0; jj < 3; jj++)
				{
					SideState rot;
					for (int ff = 0; ff < 5; ff++)
						for (int j = 0; j < 3; j++)
							JointTarget(mode ? &poses[ff][j] : nullptr, mode ? nullptr : &table[ff][j], (ff == f && j == jj) ? 0.1f : 0.f, rot.Q[ff][j]);
					rot.Init = true;
					std::vector<std::array<float, 16>> G(nj);
					for (int j = 0; j < nj; j++) for (int k = 0; k < 16; k++) G[j][k] = (float)Wrest[j][k];
					std::vector<char> moved;
					ApplyFingerTurns(reinterpret_cast<float (*)[16]>(G.data()), nj, rig, rot, moved);
					const V3 t0 = Pos(Wrest[tips[f]].data());
					const V3 t1 = { G[tips[f]][12], G[tips[f]][13], G[tips[f]][14] };
					const double along = Dot(Sub(t1, t0), pn) / L;
					printf(" %+.1f", along * 1000);
					// the four fingers fold toward the palm normal. The THUMB is held to this only on the table's axes:
					// in the FBX's fist its MID and TOP turn it ACROSS the palm (toward the little finger), not into it
					// (test_curl.py exempts the thumb from its distance check for the same reason); printed, not judged.
					if (f > 0 || mode == 0) ok = ok && along > 0.0;
					if (f == 0 && mode == 1)
					{
						const V3 kAxis = Sub(Pos(Wrest[joints[1][0]].data()), Pos(Wrest[joints[4][0]].data()));
						printf("(across %+.1f)", -Dot(Sub(t1, t0), kAxis) / Len(kAxis) / L * 1000);
					}
				}
			}
			printf("\n");
			Check(ok, mode ? "F3 %s %s: every joint of index, middle, ring and pinky starts its fold toward the palm (the thumb: across it, printed)" : "F3 %s %s: every joint of every finger starts its fold toward the palm",
				meshes[h], mode ? "FBX poses" : "FRIK table");
		}

		// ---- F4: the fist through the engine path
		{
			Inputs in; in.grip = 1; in.trigger = 1; in.triggerTouch = true; in.thumbTouch = true; in.valid = true;
			Posed p = PoseHand(r, Wrest, rig, poses, table, in);
			bool inPalm = true;
			printf("  F4 fist (grip 1, trigger 1, touches; thumb curl 0.6): tip to palm centre / hand length, side:");
			for (int f = 0; f < 5; f++)
			{
				const V3 t = Pos(p.G[tips[f]].data());
				const double d = Len(Sub(t, palmC)) / L, s = Dot(Sub(t, palmC), pn) / L;
				printf(" %s %.3f %+.3f", fingerNames[f], d, s);
				if (f > 0) inPalm = inPalm && d < 0.45 && s > 0.0;
			}
			printf("\n");
			Check(inPalm, "F4 %s fist: index, middle, ring and pinky tips within 0.45 hand lengths of the palm centre, on the palm side", meshes[h]);
			const double dT = Len(Sub(Pos(p.G[tips[0]].data()), Pos(Wrest[tips[0]].data()))) / L;
			printf("  F4 thumb at curl 0.6 (the rule's value for a touch): tip moved %.3f hand lengths from rest, %+.3f along the palm normal\n",
				dT, Dot(Sub(Pos(p.G[tips[0]].data()), Pos(Wrest[tips[0]].data())), pn) / L);

			// With every curl 1 each finger joint's LOCAL transform is the FBX fist frame's. Compared as locals because
			// the take also turns HANDPALM_joint (the palm cups: the knuckles move), which no curl drives.
			SideState full;
			for (int f = 0; f < 5; f++) for (int j = 0; j < 3; j++) JointTarget(&poses[f][j], nullptr, 1.f, full.Q[f][j]);
			full.Init = true;
			std::vector<std::array<float, 16>> G(nj);
			for (int j = 0; j < nj; j++) for (int k = 0; k < 16; k++) G[j][k] = (float)Wrest[j][k];
			std::vector<char> moved;
			ApplyFingerTurns(reinterpret_cast<float (*)[16]>(G.data()), nj, rig, full, moved);
			Mats WF;
			ComposeChain(r, &r.frames[(size_t)kFistFrame * nj], WF);
			double worstPos = 0, worstAng = 0;
			for (int f = 0; f < 5; f++)
			{
				for (int j = 0; j < 4; j++)
				{
					const int jt = j < 3 ? joints[f][j] : tips[f], p = r.joints[jt].parent;
					double gp[16], gj[16], ip[16], lo[16], lf[16], ipf[16];
					for (int k = 0; k < 16; k++) { gp[k] = G[p][k]; gj[k] = G[jt][k]; }
					MatInvert(gp, ip); MatMul(ip, gj, lo);
					MatInvert(WF[p].data(), ipf); MatMul(ipf, WF[jt].data(), lf);
					worstPos = fmax(worstPos, Len(Sub(Pos(lo), Pos(lf))));
					// rotation difference of the two 3x3s: angle of lo^T lf
					double tr = 0;
					for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) tr += lo[a * 4 + b] * lf[a * 4 + b];
					worstAng = fmax(worstAng, acos(fmax(-1.0, fmin(1.0, (tr - 1) / 2))) * 180 / M_PI);
				}
			}
			double palmTurn;
			{
				const double *a = r.frames[(size_t)kDrawnFrame * nj + palmJ].q, *b = r.frames[(size_t)kFistFrame * nj + palmJ].q;
				palmTurn = QAngleDeg(a, b);
			}
			printf("  F4 every curl 1 vs the FBX's frame %d, each finger joint and tip in its parent: worst rotation %.4f deg, worst offset %.6f file units; "
				"HANDPALM_joint turns %.1f deg in that frame and is not driven (the knuckles stay at rest)\n", kFistFrame, worstAng, worstPos, palmTurn);
			// The rotations are the frame's (float slerp at t = 1). The offset is THUMB_TOP_joint's translation, which the take
			// animates by 0.004 units (0.4 mm) and the turn, as VAP's, never moves: rotations only.
			Check(worstAng < 0.05 && worstPos * 0.11094212 * 1000 < 1.0, "F4 %s: every curl 1 gives each finger joint the FBX fist frame's local rotation (within %.4f deg; offsets within %.2f mm, the take's own THUMB_TOP translation)",
				meshes[h], worstAng, worstPos * 0.11094212 * 1000);
			char fn[1024];
			snprintf(fn, sizeof(fn), "%s/%s_ctl_fist.txt", outDir.c_str(), meshes[h]); DumpMesh(fn, r, p.P);
		}

		// ---- F5: point and thumb up; the other states for the sheet
		struct St { const char *name; float grip, trigger; bool tt, th; };
		const St states[] = { { "open", 0, 0, false, false }, { "point", 1, 0, false, true }, { "thumbup", 1, 1, true, false }, { "trigger", 0, 1, true, false } };
		for (const St &s : states)
		{
			Inputs in; in.grip = s.grip; in.trigger = s.trigger; in.triggerTouch = s.tt; in.thumbTouch = s.th; in.valid = true;
			Posed p = PoseHand(r, Wrest, rig, poses, table, in);
			char fn[1024];
			snprintf(fn, sizeof(fn), "%s/%s_ctl_%s.txt", outDir.c_str(), meshes[h], s.name); DumpMesh(fn, r, p.P);
			auto fingerMoved = [&](int f) {
				double d = 0;
				for (int j = 0; j < 3; j++) d = fmax(d, Len(Sub(Pos(p.G[joints[f][j]].data()), Pos(Wrest[joints[f][j]].data()))));
				return fmax(d, Len(Sub(Pos(p.G[tips[f]].data()), Pos(Wrest[tips[f]].data())))) * 0.11094212 * 1000;
			};
			if (!strcmp(s.name, "point"))
			{
				const double d = fingerMoved(1);
				Check(d < 0.01, "F5 %s point: the index stays at rest (joints and tip within %.4f mm)", meshes[h], d);
			}
			if (!strcmp(s.name, "thumbup"))
			{
				const double d = fingerMoved(0);
				Check(d < 0.01, "F5 %s thumb up: the thumb stays at rest (within %.4f mm)", meshes[h], d);
			}
			if (!strcmp(s.name, "open"))
			{
				double d = 0;
				for (int f = 0; f < 5; f++) d = fmax(d, fingerMoved(f));
				Check(d < 0.01, "F5 %s open: every finger at rest (within %.4f mm)", meshes[h], d);
			}
		}
		// the two frames used, as the FBX has them
		for (int fr : { kOpenFrame, kFistFrame })
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
