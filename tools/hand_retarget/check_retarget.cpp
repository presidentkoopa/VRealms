// tools/hand_retarget/check_retarget.cpp -- the retargeted hands (retarget_godot.py) against hand_final.fbx, through the
// engine's own loader (fbx_skinned.cpp + ufbx) and the renderer's own hand math (vr_handpose_math.h), nothing copied.
//
//   check_retarget <hand_final.fbx> <new.fbx> <outdir>
//
//   L   the new file loads as the engine loads it (MODELDEF selectors: hand_final "Hand_Low" / "Hand_Low1?mirrorskin=
//       Hand_Low"; the new file "Hand_Low" / "Hand_Low1"): the same 22 joint names, parents first, the same take, the same
//       frame count; the skin binds at the rest (every cluster rebased alike), faces outward.
//   C   the canonical frames (BuildHand) measured on the new rest: every finger joint's frame is its own local axes
//       (identity), as the converter built them; the palm normal and the folds pass the engine's own checks.
//   P   THE CARRY: every pose rotation the engine makes (BuildPoses: open and kPoseSources; BuildHoldPoses: every pose of
//       kOwnerPoses, own or copied from the other hand) on the new hand equals hand_final's, finger joint by finger joint
//       (the angle between the two quaternions), and the palm's equals hand_final's in the hand's own frame.
//   D   dumps every posed state of both files for render_compare.py: each pose of kOwnerPoses by name (HeldState::pose,
//       the engine's SelectHold + StepPoseHeld converged at 90 Hz, PosePalette), and the six controller states.
//   M   measures: the hand's size and finger lengths at 0.11094212 m a unit (MODELDEF), and per pose the fingertip
//       directions (knuckle -> tip, in the hand frame) of the new hand against hand_final's.

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

static const double kScale = 0.11094212;	// vrealms_iwad/MODELDEF.txt: metres per hand file unit

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

static double QAngleDeg(const double a[4], const double b[4])
{
	double d = fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
	if (d > 1) d = 1;
	return 2 * acos(d) * 180.0 / M_PI;
}
// the angle between two unit quaternions from the chord, not the dot product: acos near 1 cannot resolve the floats'
// last bit (a float dot product of equal rotations reads up to ~0.05 deg)
static double QAngleF(const float a[4], const float b[4])
{
	double dp = 0, dm = 0;
	for (int k = 0; k < 4; k++) { dp += (double(a[k]) - b[k]) * (double(a[k]) - b[k]); dm += (double(a[k]) + b[k]) * (double(a[k]) + b[k]); }
	double c = sqrt(dp < dm ? dp : dm) * 0.5;
	c = c > 1 ? 1 : c;
	return 4 * asin(c) * 180.0 / M_PI;
}

// 3x3 helpers (row-major m[r][c])
struct M3 { double m[3][3]; };
static M3 Mul(const M3 &a, const M3 &b) { M3 o; for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) { o.m[r][c] = 0; for (int k = 0; k < 3; k++) o.m[r][c] += a.m[r][k] * b.m[k][c]; } return o; }
static M3 T(const M3 &a) { M3 o; for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) o.m[r][c] = a.m[c][r]; return o; }
static M3 FromQ(const float q[4]) { M3 o; const double qd[4] = { q[0], q[1], q[2], q[3] }; Detail::QuatToRows(qd, o.m); return o; }
static M3 Of4(const double g[16]) { M3 o; for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) o.m[r][c] = g[c * 4 + r]; return o; }
static double Det(const M3 &a) { const double (*m)[3] = a.m; return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]); }
static M3 Inv(const M3 &a)
{
	const double (*m)[3] = a.m; const double d = Det(a); M3 o;
	o.m[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / d; o.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / d; o.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / d;
	o.m[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / d; o.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / d; o.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / d;
	o.m[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / d; o.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / d; o.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / d;
	return o;
}
static double RotAngleDeg(const M3 &a, const M3 &b)	// angle of a^T b
{
	double f = 0;	// |a - b|_F = 2 sqrt(2) sin(angle / 2): resolves small angles, as the trace does not
	for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) f += (a.m[r][c] - b.m[r][c]) * (a.m[r][c] - b.m[r][c]);
	double s = sqrt(f) / (2 * sqrt(2.0));
	s = s > 1 ? 1 : s;
	return 2 * asin(s) * 180.0 / M_PI;
}

static const float kS[16] = { 1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 1 };

struct Hand
{
	std::string label, mesh;
	Result r;
	Mats Wrest;
	HandRig rig;
	FingerFrame frames[5][3];
	double palm[3];
	HandPoses hp;
	HoldPoses hold;
	M3 HF;			// the hand frame (HandFrameFromJoints) at rest, file space, columns X Y Z
	double wrist[3];
	bool ok = false;
};

static bool Build(Hand &h, const Result &other, int side, std::string &why)
{
	const Result &r = h.r;
	const int nj = (int)r.joints.size();
	std::map<std::string, int> idx;
	for (int j = 0; j < nj; j++) idx[r.joints[j].name] = j;
	int joints[5][3];
	for (int f = 0; f < 5; f++) for (int j = 0; j < 3; j++) joints[f][j] = idx.count(kFingerJointNames[f][j]) ? idx[kFingerJointNames[f][j]] : -1;
	{
		std::vector<TRSd> rl(nj);
		for (int j = 0; j < nj; j++) rl[j] = r.joints[j].rest;
		ComposeChain(r, rl.data(), h.Wrest);
	}
	std::vector<RestJoint> J(nj);
	for (int j = 0; j < nj; j++)
	{
		J[j].parent = r.joints[j].parent;
		memcpy(J[j].G, h.Wrest[j].data(), sizeof(J[j].G));
		memcpy(J[j].t, r.joints[j].rest.t, sizeof(J[j].t));
	}
	char w[256] = "";
	if (!BuildHand(J, idx.at(kHandJoint), idx.at(kPalmJoint), joints, side == SIDE_RIGHT, h.rig, h.frames, h.palm, w, sizeof(w))) { why = w; return false; }
	auto getQ = [&r, nj](int frame, int joint, double q[4]) { for (int k = 0; k < 4; k++) q[k] = r.frames[(size_t)frame * nj + joint].q[k]; };
	if (!BuildPoses(h.rig, h.frames, side, r.numFrames, getQ, h.hp, w, sizeof(w))) { why = w; return false; }
	std::map<std::string, int> oidx;
	for (size_t j = 0; j < other.joints.size(); j++) oidx[other.joints[j].name] = (int)j;
	const int onj = (int)other.joints.size();
	auto getQOther = [&other, &oidx, onj](int frame, const char *name, double q[4]) {
		auto it = oidx.find(name);
		if (it == oidx.end() || frame < 0 || frame >= other.numFrames) return false;
		for (int k = 0; k < 4; k++) q[k] = other.frames[(size_t)frame * onj + it->second].q[k];
		return true;
	};
	if (!BuildHoldPoses(h.rig, h.frames, side, r.numFrames, getQ, getQOther, other.numFrames, h.hold, w, sizeof(w))) { why = w; return false; }
	Vec3d P[5];
	const char *pj[5] = { "Root_joint", "INDEX_BASE_joint", "MIDDLE_F_BASE_joint", "RING_BASE_joint", "PINK_BASE_joint" };
	for (int k = 0; k < 5; k++) { const double *m = h.Wrest[idx.at(pj[k])].data(); P[k] = Vec3d(m[12], m[13], m[14]); }
	Vec3d X, Y, Z, s;
	HandFrameFromJoints(P, side == SIDE_RIGHT, X, Y, Z, s);
	const double c[3][3] = { { X.x, Y.x, Z.x }, { X.y, Y.y, Z.y }, { X.z, Y.z, Z.z } };
	memcpy(h.HF.m, c, sizeof(c));
	h.wrist[0] = P[0].x; h.wrist[1] = P[0].y; h.wrist[2] = P[0].z;
	h.ok = true;
	return true;
}

// the palm's turn q (in the palm joint's own axes at rest) as a rotation in the hand frame: HF^T Gp R(q) Gp^-1 HF
static M3 PalmInHand(const Hand &h, const float q[4])
{
	const M3 Gp = Of4(h.Wrest[h.rig.Palm].data());
	return Mul(Mul(T(h.HF), Mul(Mul(Gp, FromQ(q)), Inv(Gp))), h.HF);
}

struct Posed { Mats G; Mats P; };
static Posed PoseHand(const Hand &h, const Inputs &in, const HeldState *held)
{
	const Result &r = h.r;
	const int nj = (int)r.joints.size();
	SideState rot;
	PalmState palm;
	for (int i = 0; i < 400; i++)
	{
		if (held != nullptr)
		{
			HoldChoice ch;
			SelectHold(&h.hold, *held, HeldState(), in, ch);
			StepPoseHeld(rot, palm, in, Retain(1.0 / 90.0), &h.hp, &h.hold, &ch);
		}
		else StepPose(rot, palm, in, Retain(1.0 / 90.0), &h.hp);
	}
	Mats W0;
	ComposeChain(r, &r.frames[(size_t)kDrawnFrame * nj], W0);
	std::vector<std::array<float, 16>> pal(nj), base(nj), inv(nj), outPal(nj);
	for (int j = 0; j < nj; j++)
	{
		double ri[16], p[16];
		MatInvert(h.Wrest[j].data(), ri);
		MatMul(W0[j].data(), ri, p);
		float pf[16], t[16];
		for (int k = 0; k < 16; k++) { pf[k] = (float)p[k]; base[j][k] = (float)h.Wrest[j][k]; inv[j][k] = (float)ri[k]; }
		Detail::Mul4(kS, pf, t);
		Detail::Mul4(t, kS, pal[j].data());
	}
	std::vector<float> scratch;
	std::vector<char> moved;
	PosePalette(reinterpret_cast<const float (*)[16]>(pal.data()), reinterpret_cast<const float (*)[16]>(base.data()),
		reinterpret_cast<const float (*)[16]>(inv.data()), nj, h.rig, rot, reinterpret_cast<float (*)[16]>(outPal.data()), scratch, moved, palm.Q);
	Posed out;
	out.G.resize(nj);
	out.P.resize(nj);
	for (int j = 0; j < nj; j++)
	{
		float G[16], t[16], pf[16];
		PaletteToGlobal(outPal[j].data(), base[j].data(), G);
		Detail::Mul4(kS, outPal[j].data(), t);
		Detail::Mul4(t, kS, pf);
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
		double p[3] = { 0, 0, 0 }, n[3] = { 0, 0, 0 };
		for (int k = 0; k < 4; k++)
		{
			if (!v.weight[k]) continue;
			const double *m = P[v.bone[k]].data(), w = v.weight[k] / 255.0;
			for (int c = 0; c < 3; c++)
			{
				p[c] += (m[c] * v.pos[0] + m[4 + c] * v.pos[1] + m[8 + c] * v.pos[2] + m[12 + c]) * w;
				n[c] += (m[c] * v.nrm[0] + m[4 + c] * v.nrm[1] + m[8 + c] * v.nrm[2]) * w;
			}
		}
		fprintf(o, "%.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f\n", p[0], p[1], p[2], n[0], n[1], n[2], v.uv[0], v.uv[1]);
	}
	for (size_t t = 0; t < r.tris.size(); t += 3) fprintf(o, "%u %u %u\n", r.tris[t], r.tris[t + 1], r.tris[t + 2]);
	fclose(o);
}

// knuckle -> tip unit direction of each finger in the hand frame (posed joints)
static void TipDirs(const Hand &h, const Mats &G, double out[5][3])
{
	static const char *tips[5] = { "THUMB_UP_TOP_joint", "INDEX_UP_TOP_joint", "MIDDLE_F_UP_TOP_joint", "RING_UP_TOP_joint", "PINK_UP_TOP_joint" };
	for (int f = 0; f < 5; f++)
	{
		int base = h.rig.Joint[f][0], tip = -1;
		for (size_t j = 0; j < h.r.joints.size(); j++) if (h.r.joints[j].name == tips[f]) tip = (int)j;
		double d[3];
		for (int c = 0; c < 3; c++) d[c] = G[tip][12 + c] - G[base][12 + c];
		double v[3];
		for (int c = 0; c < 3; c++) v[c] = h.HF.m[0][c] * d[0] + h.HF.m[1][c] * d[1] + h.HF.m[2][c] * d[2];
		const double l = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		for (int c = 0; c < 3; c++) out[f][c] = v[c] / l;
	}
}

int main(int argc, char **argv)
{
	if (argc < 4) { fprintf(stderr, "usage: check_retarget <hand_final.fbx> <new.fbx> <outdir>\n"); return 2; }
	const std::vector<char> fo = ReadFile(argv[1]), fn = ReadFile(argv[2]);
	const std::string outDir = argv[3];
	const char *meshes[2] = { "Hand_Low", "Hand_Low1" };
	const char *selOld[2] = { "Hand_Low", "Hand_Low1?mirrorskin=Hand_Low" };
	const char *selNew[2] = { "Hand_Low", "Hand_Low1" };
	Hand H[2][2];	// [file 0 old / 1 new][side]
	for (int file = 0; file < 2; file++)
		for (int s = 0; s < 2; s++)
		{
			std::string err;
			const std::vector<char> &d = file ? fn : fo;
			if (!Load(d.data(), d.size(), (file ? selNew : selOld)[s], H[file][s].r, err)) { printf("FAIL load %s %s: %s\n", file ? "new" : "old", meshes[s], err.c_str()); return 2; }
			H[file][s].label = file ? "new" : "old";
			H[file][s].mesh = meshes[s];
		}
	printf("== L: the new file as the engine loads it\n");
	for (int s = 0; s < 2; s++)
	{
		const Result &o = H[0][s].r, &n = H[1][s].r;
		bool same = o.joints.size() == n.joints.size();
		for (size_t j = 0; same && j < o.joints.size(); j++) same = o.joints[j].name == n.joints[j].name && o.joints[j].parent == n.joints[j].parent;
		Check(same, "L %s: the same %zu joints, names and parents, in the same order", meshes[s], n.joints.size());
		Check(n.numFrames == o.numFrames && n.takes.size() == 1 && n.takes[0].name == o.takes[0].name && fabs(n.takes[0].fps - o.takes[0].fps) < 1e-9,
			"L %s: take '%s', %d frames at %g fps (hand_final: '%s', %d at %g)", meshes[s], n.takes.empty() ? "?" : n.takes[0].name.c_str(), n.numFrames,
			n.takes.empty() ? 0.0 : n.takes[0].fps, o.takes[0].name.c_str(), o.numFrames, o.takes[0].fps);
		Check(n.diag.rebaseMaxDev < 1e-5, "L %s: every cluster binds at the rest (rebase spread %.2g)", meshes[s], n.diag.rebaseMaxDev);
		printf("  %s: %zu vertices, %zu triangles, %d control points, max influences %d (over 4: %d, largest dropped %.3f), unweighted %d, "
			"normals %s (file vs surface %.3f), %s, signed volume %.4f, texture '%s', material '%s', determinant %.3f\n",
			meshes[s], n.verts.size(), n.tris.size() / 3, n.diag.controlPoints, n.diag.maxInfluences, n.diag.verticesOver4, n.diag.maxDroppedWeight,
			n.diag.unweightedControlPoints, n.diag.normalsFromSurface ? "FROM THE SURFACE" : "from the file", n.diag.normalAgreeFile,
			n.diag.reversedForOutward ? "faces reversed for outward" : "faces outward as stored", n.diag.signedVolume, n.diag.textureFile.c_str(),
			n.diag.materialName.c_str(), n.diag.restDeterminant);
		Check(!n.diag.normalsFromSurface && n.diag.unweightedControlPoints == 0, "L %s: the file's normals face the surface (%.3f) and every point is skinned", meshes[s], n.diag.normalAgreeFile);
		// the root's rest transform equals hand_final's (the seat's frame: where each hand sits)
		double d = 0;
		for (int k = 0; k < 16; k++) d = fmax(d, fabs(o.joints[0].restWorld[k] - n.joints[0].restWorld[k]));
		Check(d < 1e-6, "L %s: Root_joint's rest world is hand_final's (difference %.2g)", meshes[s], d);
	}
	printf("\n== C: the canonical frames on the new rest (BuildHand), and the poses (BuildPoses, BuildHoldPoses)\n");
	for (int file = 0; file < 2; file++)
		for (int s = 0; s < 2; s++)
		{
			std::string why;
			const bool ok = Build(H[file][s], H[file][1 - s].r, s, why);
			Check(ok, "C %s %s: built (%s)", H[file][s].label.c_str(), meshes[s], ok ? "BuildHand, BuildPoses, BuildHoldPoses" : why.c_str());
			if (!ok) return 1;
		}
	for (int s = 0; s < 2; s++)
	{
		Hand &n = H[1][s];
		double worst = 0, minFold = 9;
		for (int f = 0; f < 5; f++)
			for (int j = 0; j < 3; j++)
			{
				const FingerFrame &F = n.frames[f][j];
				const double e[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
				for (int c = 0; c < 3; c++) { worst = fmax(worst, fabs(F.X[c] - e[0][c])); worst = fmax(worst, fabs(F.Y[c] - e[1][c])); worst = fmax(worst, fabs(F.Z[c] - e[2][c])); }
				minFold = fmin(minFold, F.fold);
			}
		Check(worst < 1e-5, "C %s: every finger joint's canonical frame is its own local axes (largest difference %.2g); smallest fold %.3f", meshes[s], worst, minFold);
		double pa = 0;
		for (int c = 0; c < 3; c++) pa = fmax(pa, fabs(n.palm[c] - (-n.HF.m[c][1])));
		const Hand &o = H[0][s];
		double hd = 0;
		for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) hd = fmax(hd, fabs(o.HF.m[r][c] - n.HF.m[r][c]));
		Check(hd < 1e-5, "C %s: the new hand frame at rest is hand_final's (largest axis difference %.2g)", meshes[s], hd);
	}

	printf("\n== P: THE CARRY -- the engine's pose rotations, new hand against hand_final\n");
	for (int s = 0; s < 2; s++)
	{
		const Hand &o = H[0][s], &n = H[1][s];
		double wOpen = 0, wTarget = 0, wHold = 0, wPalmT = 0, wPalmH = 0;
		int nHold = 0, nPalm = 0, haveDiff = 0;
		char wHoldAt[160] = "";
		double thumbMax = 0; std::string thumbAt;
		for (int f = 0; f < 5; f++)
			for (int j = 0; j < 3; j++)
			{
				wOpen = fmax(wOpen, QAngleF(o.hp.Open[f][j], n.hp.Open[f][j]));
				for (int p = 0; p < POSE_COUNT; p++)
				{
					const double a = QAngleF(o.hp.Target[p][f][j], n.hp.Target[p][f][j]);
					if (f != FINGER_THUMB) wTarget = fmax(wTarget, a);
					else if (a > thumbMax) { thumbMax = a; thumbAt = std::string("controller pose ") + std::to_string(p); }
				}
			}
		for (int p = 0; p < POSE_COUNT; p++)
			if (o.hp.PalmDriven[p] && n.hp.PalmDriven[p])
				wPalmT = fmax(wPalmT, RotAngleDeg(PalmInHand(o, o.hp.PalmTarget[p]), PalmInHand(n, n.hp.PalmTarget[p])));
		for (int p = 0; p < kOwnerPoseCount; p++)
		{
			if (o.hold.Have[p] != n.hold.Have[p] || o.hold.PalmDriven[p] != n.hold.PalmDriven[p] || o.hold.Copied[p] != n.hold.Copied[p]) haveDiff++;
			if (!o.hold.Have[p] || !n.hold.Have[p]) continue;
			nHold++;
			double wp = 0;
			int wf = 0, wj = 0;
			for (int f = 0; f < 5; f++) for (int j = 0; j < 3; j++)
			{
				const double a = QAngleF(o.hold.Q[p][f][j], n.hold.Q[p][f][j]);
				if (f == FINGER_THUMB) { if (a > thumbMax) { thumbMax = a; thumbAt = kOwnerPoses[p].name; } continue; }
				if (a > wp) { wp = a; wf = f; wj = j; }
			}
			if (wp > wHold) { wHold = wp; snprintf(wHoldAt, sizeof(wHoldAt), "%s, %s joint %d, frame %d", kOwnerPoses[p].name, kFingerJointNames[wf][wj], wj + 1, o.hold.Frame[p][wf]); }
			if (o.hold.PalmDriven[p])
			{
				nPalm++;
				wPalmH = fmax(wPalmH, RotAngleDeg(PalmInHand(o, o.hold.PalmQ[p]), PalmInHand(n, n.hold.PalmQ[p])));
			}
		}
		Check(wOpen < 1e-3 && wTarget < 1e-3, "P %s: open (15 joints) and the four controller poses (kPoseSources; index, middle, ring, little: 12 joints each): largest difference %.2g / %.2g deg",
			meshes[s], wOpen, wTarget);
		Check(wPalmT < 1e-3, "P %s: the palm in the controller poses that drive it, in the hand frame: largest difference %.2g deg", meshes[s], wPalmT);
		Check(haveDiff == 0 && nHold > 0, "P %s: kOwnerPoses: the same %d poses present, copied and palm-driven on both files (%d differ)", meshes[s], nHold, haveDiff);
		Check(wHold < 1e-3, "P %s: every pose of kOwnerPoses, index, middle, ring and little (12 joints each): largest difference %.2g deg (%s)", meshes[s], wHold, wHoldAt);
		printf("  NOTE  P %s: the THUMB departs where retarget_godot.py's contact rule moves it (its tip on a finger in hand_final): largest joint difference %.1f deg (%s)\n",
			meshes[s], thumbMax, thumbAt.c_str());
		Check(wPalmH < 1e-3, "P %s: the palm in the %d poses that drive it, in the hand frame: largest difference %.2g deg", meshes[s], nPalm, wPalmH);
	}

	if (getenv("CR_DEBUG")) for (int s = 0; s < 2; s++) for (int p = 0; p < kOwnerPoseCount; p++) { if (!H[0][s].hold.Have[p] || kOwnerPoses[p].blender <= 0) continue; printf("DBG %s %-22s", meshes[s], kOwnerPoses[p].name); for (int f = 0; f < 5; f++) for (int j = 0; j < 3; j++) printf(" %.3f", QAngleF(H[0][s].hold.Q[p][f][j], H[1][s].hold.Q[p][f][j])); printf("\n"); }
	printf("\n== M: size (at %.8f m a unit)\n", kScale);
	for (int file = 0; file < 2; file++)
		for (int s = 0; s < 2; s++)
		{
			const Hand &h = H[file][s];
			auto P = [&](const char *nm) { for (size_t j = 0; j < h.r.joints.size(); j++) if (h.r.joints[j].name == nm) return std::array<double, 3>{ h.Wrest[j][12], h.Wrest[j][13], h.Wrest[j][14] }; return std::array<double, 3>{ 0, 0, 0 }; };
			auto D = [&](const char *a, const char *b) { auto x = P(a), y = P(b); return sqrt((x[0] - y[0]) * (x[0] - y[0]) + (x[1] - y[1]) * (x[1] - y[1]) + (x[2] - y[2]) * (x[2] - y[2])) * kScale * 1000; };
			const double ip = D("INDEX_BASE_joint", "INDEX_MID_joint"), ii = D("INDEX_MID_joint", "INDEX_TOP_joint"), id = D("INDEX_TOP_joint", "INDEX_UP_TOP_joint");
			const double mp = D("MIDDLE_F_BASE_joint", "MIDDLE_F_MID_joint") + D("MIDDLE_F_MID_joint", "MIDDLE_F_TOP_joint") + D("MIDDLE_F_TOP_joint", "MIDDLE_F_UP_TOP_joint");
			printf("  %s %-9s wrist->middle tip joint %.1f mm; index proximal %.1f intermediate %.1f distal %.1f (MCP->tip along the bones %.1f); middle %.1f; "
				"index->pinky knuckles %.1f; wrist->middle knuckle %.1f mm\n", h.label.c_str(), meshes[s], D("Root_joint", "MIDDLE_F_UP_TOP_joint"), ip, ii, id, ip + ii + id, mp,
				D("INDEX_BASE_joint", "PINK_BASE_joint"), D("Root_joint", "MIDDLE_F_BASE_joint"));
		}

	printf("\n== D: every pose posed on both files (dumps for render_compare.py), and the fingertip directions\n");
	{
		FILE *hf = fopen((outDir + "/handframes.txt").c_str(), "w");	// for render_compare.py: wrist, then the hand frame's X Y Z
		for (int s = 0; s < 2; s++)
		{
			const Hand &h = H[1][s];
			fprintf(hf, "%s %.9f %.9f %.9f", meshes[s], h.wrist[0], h.wrist[1], h.wrist[2]);
			for (int c = 0; c < 3; c++) fprintf(hf, " %.9f %.9f %.9f", h.HF.m[0][c], h.HF.m[1][c], h.HF.m[2][c]);
			fprintf(hf, "\n");
		}
		fclose(hf);
	}
	FILE *list = fopen((outDir + "/poses.txt").c_str(), "w");
	for (int s = 0; s < 2; s++)
	{
		struct St { std::string name; Inputs in; bool held; HeldState hs; };
		std::vector<St> states;
		auto ctl = [&](const char *nm, float tr, float gr, bool tt, bool th) { St x; x.name = std::string("ctl_") + nm; x.in.trigger = tr; x.in.grip = gr; x.in.triggerTouch = tt; x.in.thumbTouch = th; x.in.valid = true; x.held = false; states.push_back(x); };
		ctl("open", 0, 0, false, false);
		ctl("fist", 1, 1, true, true);
		ctl("point", 0, 1, false, true);
		ctl("thumbup", 1, 1, true, false);
		ctl("fingergun", 0, 1, false, false);
		ctl("trigger", 1, 0, true, true);
		for (int p = 0; p < kOwnerPoseCount; p++)
		{
			if (!H[1][s].hold.Have[p]) continue;
			St x; x.name = std::string("pose_") + kOwnerPoses[p].name;
			x.in.valid = true; x.held = true; x.hs.pose = kOwnerPoses[p].name;
			states.push_back(x);
		}
		double worstDir = 0; std::string worstName;
		for (const St &st : states)
		{
			Posed po = PoseHand(H[0][s], st.in, st.held ? &st.hs : nullptr);
			Posed pn = PoseHand(H[1][s], st.in, st.held ? &st.hs : nullptr);
			char fnm[512];
			snprintf(fnm, sizeof(fnm), "%s/old_%s_%s.txt", outDir.c_str(), meshes[s], st.name.c_str()); DumpMesh(fnm, H[0][s].r, po.P);
			snprintf(fnm, sizeof(fnm), "%s/new_%s_%s.txt", outDir.c_str(), meshes[s], st.name.c_str()); DumpMesh(fnm, H[1][s].r, pn.P);
			for (int file = 0; file < 2; file++)	// the posed joints, for rend.py pts=
			{
				snprintf(fnm, sizeof(fnm), "%s/%s_%s_%s.pts", outDir.c_str(), file ? "new" : "old", meshes[s], st.name.c_str());
				FILE *pf = fopen(fnm, "w");
				const Mats &G = file ? pn.G : po.G;
				for (size_t j = 0; pf && j < G.size(); j++)
				{
					std::string nm = H[file][s].r.joints[j].name;
					nm = nm.substr(0, nm.size() > 6 ? nm.size() - 6 : nm.size());
					fprintf(pf, "%s %.6f %.6f %.6f %s\n", nm.c_str(), G[j][12], G[j][13], G[j][14], file ? "cyan" : "yellow");
				}
				if (pf) fclose(pf);
			}
			double dO[5][3], dN[5][3], w = 0;
			TipDirs(H[0][s], po.G, dO);
			TipDirs(H[1][s], pn.G, dN);
			std::string per;
			for (int f = 0; f < 5; f++)
			{
				double c = dO[f][0] * dN[f][0] + dO[f][1] * dN[f][1] + dO[f][2] * dN[f][2];
				c = c > 1 ? 1 : c;
				const double a = acos(c) * 180 / M_PI;
				char b[32]; snprintf(b, sizeof(b), " %.1f", a); per += b;
				w = fmax(w, a);
			}
			if (w > worstDir) { worstDir = w; worstName = st.name; }
			fprintf(list, "%s %s %.2f%s\n", meshes[s], st.name.c_str(), w, per.c_str());
		}
		printf("  %s: %zu states dumped; knuckle->tip directions (thumb..little) differ by at most %.1f deg (%s): the bones' angles are equal, the "
			"rigs' bone proportions differ (poses.txt lists every pose)\n", meshes[s], states.size(), worstDir, worstName.c_str());
	}
	fclose(list);
	printf("\n%s: %d failure(s)\n", gFails ? "FAIL" : "PASS", gFails);
	return gFails ? 1 : 0;
}
