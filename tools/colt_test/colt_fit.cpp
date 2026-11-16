// tools/colt_test/colt_fit.cpp -- THE COLT FITTED TO THE AUTHORED HAND (owner's plan, 2026-11-16).
//
// The hand pose stays authored: each hand (hand_final.fbx, Hand_Low / Hand_Low1) is posed through the engine's own
// path (vr_handpose_math.h: SelectHold + StepPoseHeld converged, PosePalette) as a weapon of grip class "pistol" --
// grip_pistol, the index on the trigger (wrap_half at trigger 0) -- and the GUN is fitted to it with three points
// measured on the meshes:
//   (a) the grip's centreline (the magazine's long axis, PCA of the Magaz part) through the wrapped fingers' channel
//       (the centres of the middle, ring and little fingers' loops: the centroid of each finger's four joints);
//   (b) the thumb-index web (the web skin vertex nearest the channel axis) snug under the beavertail (the backstrap
//       point just under the grip-safety tang);
//   (c) the index pad (the distal segment's most palmward skin vertex) on the trigger face.
// One rigid transform (gun -> hand frame, metres) by least squares; then the checks the owner asked for: no finger
// vertex inside the gun, the pad on the trigger face within a few mm, the web at the beavertail; the straight index
// (trigger untouched) along the frame. Writes the numbers, the seat for the engine (gun render coords -> the hand
// frame in gun file units, as vr_handpose_math.h ModelSeat is for a hand), and mesh dumps for py/rend.py.
//
//   colt_fit <hand_final.fbx> <colt1911.fbx> <outdir>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fbx_skinned.h"
#include "vr_handpose_math.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace FBXSkinned;
using namespace VRHandPose;

static const double kHandScale = 0.11094212;   // vrealms_iwad/MODELDEF.txt, the hands' Scale (metres per hand file unit)
static const double kGunMetres = 0.216;        // the task: a 1911 is ~21.6 cm long, muzzle to the back of the tang

static int gFails = 0;
static void Check(bool ok, const char *what) { printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) gFails++; }

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

struct V3 { double x = 0, y = 0, z = 0; V3() = default; V3(double a, double b, double c) : x(a), y(b), z(c) {} };
static V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
static V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static V3 operator*(V3 a, double s) { return { a.x * s, a.y * s, a.z * s }; }
static double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static double Len(V3 a) { return sqrt(Dot(a, a)); }
static V3 Unit(V3 a) { const double l = Len(a); return l > 0 ? a * (1.0 / l) : a; }
static V3 Pos(const double m[16]) { return { m[12], m[13], m[14] }; }
static V3 MatPoint(const double m[16], V3 p)
{
	return { m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13], m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14] };
}
static V3 MatDir(const double m[16], V3 p)
{
	return { m[0] * p.x + m[4] * p.y + m[8] * p.z, m[1] * p.x + m[5] * p.y + m[9] * p.z, m[2] * p.x + m[6] * p.y + m[10] * p.z };
}
static double Deg(double r) { return r * 180.0 / M_PI; }

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

// ---- the hand, posed through the engine path (tools/fbxload_test/test_fingers.cpp PoseHand, the same calls) ----
struct Posed { Mats G; Mats P; };
static const float kS[16] = { 1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 1 };

static Posed PoseHand(const Result &r, const Mats &Wrest, const HandRig &rig, const HandPoses &hp, const Inputs &in,
	const HoldPoses *hold, const HeldState *held)
{
	const int nj = (int)r.joints.size();
	SideState rot;
	PalmState palm;
	for (int i = 0; i < 400; i++)
	{
		HoldChoice ch;
		SelectHold(hold, *held, HeldState(), in, ch);
		StepPoseHeld(rot, palm, in, Retain(1.0 / 90.0), &hp, hold, &ch);
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
		Detail::Mul4(t, kS, pal[j].data());
	}
	std::vector<float> scratch;
	std::vector<char> moved;
	PosePalette(reinterpret_cast<const float (*)[16]>(pal.data()), reinterpret_cast<const float (*)[16]>(base.data()),
		reinterpret_cast<const float (*)[16]>(inv.data()), nj, rig, rot, reinterpret_cast<float (*)[16]>(outPal.data()), scratch, moved, palm.Q);
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

struct MeshV { V3 p, n; float u = 0, v = 0; int bone = 0; };
struct Mesh { std::vector<MeshV> v; std::vector<uint32_t> t; };

static Mesh Skin(const Result &r, const Mats &P)
{
	Mesh m;
	m.t = r.tris;
	for (const Vertex &v : r.verts)
	{
		MeshV o;
		for (int k = 0; k < 4; k++)
		{
			if (!v.weight[k]) continue;
			const double *M = P[v.bone[k]].data(), w = v.weight[k] / 255.0;
			o.p = o.p + MatPoint(M, { v.pos[0], v.pos[1], v.pos[2] }) * w;
			o.n = o.n + MatDir(M, { v.nrm[0], v.nrm[1], v.nrm[2] }) * w;
		}
		o.n = Unit(o.n);
		o.u = v.uv[0]; o.v = v.uv[1]; o.bone = v.bone[0];
		m.v.push_back(o);
	}
	return m;
}

static void Dump(const char *path, const Mesh &m)
{
	FILE *o = fopen(path, "w");
	if (!o) return;
	fprintf(o, "%zu %zu\n", m.v.size(), m.t.size() / 3);
	for (const MeshV &v : m.v) fprintf(o, "%.6f %.6f %.6f %.4f %.4f %.4f %.5f %.5f %d\n", v.p.x, v.p.y, v.p.z, v.n.x, v.n.y, v.n.z, v.u, v.v, v.bone);
	for (size_t t = 0; t < m.t.size(); t += 3) fprintf(o, "%u %u %u\n", m.t[t], m.t[t + 1], m.t[t + 2]);
	fclose(o);
}

// ---- rotation helpers (rotation vector -> matrix, row major 3x3) ----
struct M3 { double m[3][3]; V3 operator*(V3 p) const { return { m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z, m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z, m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z }; } };
static M3 FromCols(V3 a, V3 b, V3 c) { M3 r; r.m[0][0] = a.x; r.m[1][0] = a.y; r.m[2][0] = a.z; r.m[0][1] = b.x; r.m[1][1] = b.y; r.m[2][1] = b.z; r.m[0][2] = c.x; r.m[1][2] = c.y; r.m[2][2] = c.z; return r; }
static M3 Mul(const M3 &a, const M3 &b) { M3 r; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { r.m[i][j] = 0; for (int k = 0; k < 3; k++) r.m[i][j] += a.m[i][k] * b.m[k][j]; } return r; }
static M3 Rodrigues(V3 w)
{
	const double th = Len(w);
	M3 r = FromCols({ 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 });
	if (th < 1e-12) return r;
	const V3 k = w * (1.0 / th);
	const double c = cos(th), s = sin(th), C = 1 - c;
	r.m[0][0] = c + k.x * k.x * C;       r.m[0][1] = k.x * k.y * C - k.z * s; r.m[0][2] = k.x * k.z * C + k.y * s;
	r.m[1][0] = k.y * k.x * C + k.z * s; r.m[1][1] = c + k.y * k.y * C;       r.m[1][2] = k.y * k.z * C - k.x * s;
	r.m[2][0] = k.z * k.x * C - k.y * s; r.m[2][1] = k.z * k.y * C + k.x * s; r.m[2][2] = c + k.z * k.z * C;
	return r;
}

// closest point on triangle (Ericson, Real-Time Collision Detection 5.1.5)
static V3 ClosestOnTri(V3 p, V3 a, V3 b, V3 c, double &u, double &v, double &w)
{
	V3 ab = b - a, ac = c - a, ap = p - a;
	double d1 = Dot(ab, ap), d2 = Dot(ac, ap);
	if (d1 <= 0 && d2 <= 0) { u = 1; v = w = 0; return a; }
	V3 bp = p - b; double d3 = Dot(ab, bp), d4 = Dot(ac, bp);
	if (d3 >= 0 && d4 <= d3) { v = 1; u = w = 0; return b; }
	double vc = d1 * d4 - d3 * d2;
	if (vc <= 0 && d1 >= 0 && d3 <= 0) { double t = d1 / (d1 - d3); u = 1 - t; v = t; w = 0; return a + ab * t; }
	V3 cp = p - c; double d5 = Dot(ab, cp), d6 = Dot(ac, cp);
	if (d6 >= 0 && d5 <= d6) { w = 1; u = v = 0; return c; }
	double vb = d5 * d2 - d1 * d6;
	if (vb <= 0 && d2 >= 0 && d6 <= 0) { double t = d2 / (d2 - d6); u = 1 - t; w = t; v = 0; return a + ac * t; }
	double va = d3 * d6 - d5 * d4;
	if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) { double t = (d4 - d3) / ((d4 - d3) + (d5 - d6)); v = 1 - t; w = t; u = 0; return b + (c - b) * t; }
	double den = 1.0 / (va + vb + vc);
	v = vb * den; w = vc * den; u = 1 - v - w;
	return a + ab * v + ac * w;
}

// signed distance of p to the mesh (negative inside: behind the nearest surface's interpolated normal)
static double SignedDistance(const Mesh &m, V3 p, double cut)
{
	double best = 1e30; double sgn = 1;
	for (size_t t = 0; t < m.t.size(); t += 3)
	{
		const MeshV &A = m.v[m.t[t]], &B = m.v[m.t[t + 1]], &C = m.v[m.t[t + 2]];
		// cheap reject
		const double r = cut;
		if (p.x < fmin(A.p.x, fmin(B.p.x, C.p.x)) - r || p.x > fmax(A.p.x, fmax(B.p.x, C.p.x)) + r) continue;
		if (p.y < fmin(A.p.y, fmin(B.p.y, C.p.y)) - r || p.y > fmax(A.p.y, fmax(B.p.y, C.p.y)) + r) continue;
		if (p.z < fmin(A.p.z, fmin(B.p.z, C.p.z)) - r || p.z > fmax(A.p.z, fmax(B.p.z, C.p.z)) + r) continue;
		double u, v, w;
		const V3 q = ClosestOnTri(p, A.p, B.p, C.p, u, v, w);
		const double d = Len(p - q);
		if (d < best)
		{
			best = d;
			const V3 n = A.n * u + B.n * v + C.n * w;
			sgn = Dot(p - q, n) < 0 ? -1 : 1;
		}
	}
	return best * sgn;
}

// THE GUN'S SURFACE, BUCKETED: every triangle in each 16-unit cell its box (grown by kReach) touches, so a query reads
// only the triangles near it. Distances beyond kReach are clamped (sign kept).
struct GunSDF
{
	static constexpr double kCell = 16.0, kReach = 64.0;
	const Mesh *m = nullptr;
	double lo[3] = {}; int n[3] = {};
	std::vector<std::vector<uint32_t>> cells;
	void Build(const Mesh &mesh)
	{
		m = &mesh;
		double hi[3] = { -1e30, -1e30, -1e30 };
		lo[0] = lo[1] = lo[2] = 1e30;
		for (const MeshV &v : mesh.v) { const double p[3] = { v.p.x, v.p.y, v.p.z }; for (int k = 0; k < 3; k++) { lo[k] = fmin(lo[k], p[k]); hi[k] = fmax(hi[k], p[k]); } }
		for (int k = 0; k < 3; k++) { lo[k] -= kReach; n[k] = (int)((hi[k] + kReach - lo[k]) / kCell) + 1; }
		cells.assign((size_t)n[0] * n[1] * n[2], {});
		for (size_t t = 0; t < mesh.t.size(); t += 3)
		{
			double a[3] = { 1e30, 1e30, 1e30 }, b[3] = { -1e30, -1e30, -1e30 };
			for (int c = 0; c < 3; c++) { const V3 &p = mesh.v[mesh.t[t + c]].p; const double q[3] = { p.x, p.y, p.z }; for (int k = 0; k < 3; k++) { a[k] = fmin(a[k], q[k]); b[k] = fmax(b[k], q[k]); } }
			int i0[3], i1[3];
			for (int k = 0; k < 3; k++) { i0[k] = (int)((a[k] - kReach - lo[k]) / kCell); i1[k] = (int)((b[k] + kReach - lo[k]) / kCell); if (i0[k] < 0) i0[k] = 0; if (i1[k] >= n[k]) i1[k] = n[k] - 1; }
			for (int x = i0[0]; x <= i1[0]; x++) for (int y = i0[1]; y <= i1[1]; y++) for (int z = i0[2]; z <= i1[2]; z++) cells[((size_t)x * n[1] + y) * n[2] + z].push_back((uint32_t)t);
		}
	}
	// A dense grid of the signed distance (gun file units, kGrid apart) over the gun's box grown by kReach, filled once
	// by Exact and read trilinearly by Query: the fit asks millions of times.
	static constexpr double kGrid = 5.0;
	int g[3] = {};
	std::vector<float> grid;
	void Fill()
	{
		for (int k = 0; k < 3; k++) g[k] = (int)((n[k] * kCell) / kGrid) + 1;
		grid.assign((size_t)g[0] * g[1] * g[2], 0.f);
		for (int x = 0; x < g[0]; x++)
			for (int y = 0; y < g[1]; y++)
				for (int z = 0; z < g[2]; z++)
					grid[((size_t)x * g[1] + y) * g[2] + z] = (float)Exact(V3(lo[0] + x * kGrid, lo[1] + y * kGrid, lo[2] + z * kGrid));
	}
	double Query(V3 p) const
	{
		const double q[3] = { (p.x - lo[0]) / kGrid, (p.y - lo[1]) / kGrid, (p.z - lo[2]) / kGrid };
		int i[3]; double f[3];
		for (int k = 0; k < 3; k++) { i[k] = (int)floor(q[k]); f[k] = q[k] - i[k]; if (i[k] < 0 || i[k] + 1 >= g[k]) return kReach; }
		double v = 0;
		for (int c = 0; c < 8; c++)
		{
			const int dx = c & 1, dy = (c >> 1) & 1, dz = (c >> 2) & 1;
			const double w = (dx ? f[0] : 1 - f[0]) * (dy ? f[1] : 1 - f[1]) * (dz ? f[2] : 1 - f[2]);
			v += w * grid[((size_t)(i[0] + dx) * g[1] + (i[1] + dy)) * g[2] + (i[2] + dz)];
		}
		return v;
	}
	double Exact(V3 p) const
	{
		const double q[3] = { p.x, p.y, p.z }; int i[3];
		for (int k = 0; k < 3; k++) { i[k] = (int)((q[k] - lo[k]) / kCell); if (i[k] < 0 || i[k] >= n[k]) return kReach; }
		const auto &c = cells[((size_t)i[0] * n[1] + i[1]) * n[2] + i[2]];
		double best = kReach, sgn = 1;
		for (uint32_t t : c)
		{
			const MeshV &A = m->v[m->t[t]], &B = m->v[m->t[t + 1]], &C = m->v[m->t[t + 2]];
			double u, v, w;
			const V3 cp = ClosestOnTri(p, A.p, B.p, C.p, u, v, w);
			const double d = Len(p - cp);
			if (d < best) { best = d; sgn = Dot(p - cp, A.n * u + B.n * v + C.n * w) < 0 ? -1 : 1; }
		}
		return best * sgn;
	}
};

int main(int argc, char **argv)
{
	if (argc < 4) { fprintf(stderr, "usage: colt_fit <hand_final.fbx> <colt1911.fbx> <outdir>\n"); return 2; }
	const std::vector<char> handFile = ReadFile(argv[1]), gunFile = ReadFile(argv[2]);
	const std::string outDir = argv[3];

	// ---- the gun, at rest, in its file units (Z up, +Y the muzzle, +X its right side) ----
	Result g; std::string err;
	if (!Load(gunFile.data(), gunFile.size(), "", g, err)) { printf("gun: %s\n", err.c_str()); return 2; }
	std::map<std::string, int> gj;
	for (size_t j = 0; j < g.joints.size(); j++) gj[g.joints[j].name] = (int)j;
	Mats gRest; { std::vector<TRSd> rl; for (auto &J : g.joints) rl.push_back(J.rest); ComposeChain(g, rl.data(), gRest); }
	Mats gPal(g.joints.size());
	for (auto &m : gPal) { memset(m.data(), 0, sizeof(double) * 16); m[0] = m[5] = m[10] = m[15] = 1; }
	const Mesh gun = Skin(g, gPal);
	double ylo = 1e30, yhi = -1e30;
	for (const MeshV &v : gun.v) { ylo = fmin(ylo, v.p.y); yhi = fmax(yhi, v.p.y); }
	const double s = kGunMetres / (yhi - ylo);
	printf("gun: %zu vertices, %zu triangles, length %.2f file units (muzzle +Y %.2f, tang %.2f) -> %.6g m per unit (%.3f m)\n",
		gun.v.size(), gun.t.size() / 3, yhi - ylo, yhi, ylo, s, kGunMetres);

	// (a) the grip's centreline: the magazine body's long axis (PCA), through its centroid
	V3 mc; int mn = 0;
	const int jMag = gj.at("Magaz");
	for (const MeshV &v : gun.v) if (v.bone == jMag) { mc = mc + v.p; mn++; }
	mc = mc * (1.0 / mn);
	double C[3][3] = {};
	for (const MeshV &v : gun.v) if (v.bone == jMag)
	{
		const V3 d = v.p - mc; const double a[3] = { d.x, d.y, d.z };
		for (int i = 0; i < 3; i++) for (int k = 0; k < 3; k++) C[i][k] += a[i] * a[k];
	}
	V3 gd(0, -0.25, 1);   // power iteration from roughly "up the grip"
	for (int it = 0; it < 100; it++) gd = Unit(V3(C[0][0] * gd.x + C[0][1] * gd.y + C[0][2] * gd.z, C[1][0] * gd.x + C[1][1] * gd.y + C[1][2] * gd.z, C[2][0] * gd.x + C[2][1] * gd.y + C[2][2] * gd.z));
	if (gd.z < 0) gd = gd * -1.0;
	printf("grip centreline (magazine axis): through (%.1f %.1f %.1f), direction (%.4f %.4f %.4f) = %.1f deg back from the bore's normal (a 1911's grip angle is ~18 deg)\n",
		mc.x, mc.y, mc.z, gd.x, gd.y, gd.z, Deg(atan2(-gd.y, gd.z)));

	// (b) the beavertail: the rear profile of the frame (Main), the deepest point under the tang: the backstrap's
	// rearmost point at each height, from the tang's underside down; the web sits where the backstrap meets the tang.
	const int jMain = gj.at("Main");
	auto rearmost = [&](double z0, double z1, V3 &at) {
		double best = 1e30; bool any = false;
		for (const MeshV &v : gun.v) if (v.bone == jMain && fabs(v.p.x) < 25 && v.p.z >= z0 && v.p.z < z1 && v.p.y < -250) { if (v.p.y < best) { best = v.p.y; at = v.p; any = true; } }
		return any;
	};
	// the tang: the rearmost frame point above z 60; the backstrap below it: from the tang's bottom down 30 units
	V3 tang; rearmost(70, 130, tang);
	double tangBottom = 1e30;
	for (const MeshV &v : gun.v) if (v.bone == jMain && fabs(v.p.x) < 25 && v.p.y < tang.y + 30 && v.p.z > 40) tangBottom = fmin(tangBottom, v.p.z);
	V3 back; rearmost(tangBottom - 20, tangBottom, back);
	const V3 beaver(0, back.y, tangBottom);
	printf("beavertail: tang rearmost (%.1f %.1f %.1f), its underside at z %.1f; backstrap under it y %.1f -> the web point (0 %.1f %.1f)\n",
		tang.x, tang.y, tang.z, tangBottom, back.y, beaver.y, beaver.z);
	// (c) the trigger face: the Trig part's front face at the blade's middle height
	const int jTrig = gj.at("Trig");
	double tz0 = 1e30, tz1 = -1e30;
	for (const MeshV &v : gun.v) if (v.bone == jTrig) { tz0 = fmin(tz0, v.p.z); tz1 = fmax(tz1, v.p.z); }
	const double tzMid = 0.5 * (tz0 + tz1);
	double tyFace = -1e30;
	for (const MeshV &v : gun.v) if (v.bone == jTrig && fabs(v.p.x) < 6 && fabs(v.p.z - tzMid) < 8) tyFace = fmax(tyFace, v.p.y);
	const V3 trig(0, tyFace, tzMid);
	printf("trigger face: Trig spans z %.1f..%.1f; its front face at mid height (0 %.1f %.1f), facing +Y (the muzzle)\n", tz0, tz1, trig.y, trig.z);
	// the bore: the barrel's (Dyl) centre line, and the muzzle
	const int jDyl = gj.at("Dyl");
	double bx = 0, bz = 0; int bn = 0;
	for (const MeshV &v : gun.v) if (v.bone == jDyl) { bx += v.p.x; bz += v.p.z; bn++; }
	// the bore height: the Bullet (chambered round) part's centre, the round sits on the bore
	const int jBullet = gj.at("Bullet");
	double rz = 0; int rn = 0; double rx = 0;
	for (const MeshV &v : gun.v) if (v.bone == jBullet) { rz += v.p.z; rx += v.p.x; rn++; }
	rz /= rn; rx /= rn;
	const V3 muzzle(rx, yhi, rz), breech(rx, ylo + 0.5 * (yhi - ylo), rz);
	printf("bore: x %.2f z %.2f (the chambered round's centre); the muzzle at y %.1f\n", rx, rz, yhi);

	FILE *res = fopen((outDir + "/colt_fit.txt").c_str(), "w");
	fprintf(res, "# tools/colt_test/colt_fit.cpp output: gun file units, the scale, and per hand the seat\n");
	fprintf(res, "gun_metres_per_unit %.9g\n", s);
	fprintf(res, "bore %.4f %.4f %.4f muzzle_y %.4f\n", rx, 0.0, rz, yhi);
	fprintf(res, "grip_point %.4f %.4f %.4f grip_dir %.6f %.6f %.6f\n", mc.x, mc.y, mc.z, gd.x, gd.y, gd.z);
	fprintf(res, "beavertail %.4f %.4f %.4f trigger %.4f %.4f %.4f\n", beaver.x, beaver.y, beaver.z, trig.x, trig.y, trig.z);

	// ---- each hand ----
	const char *meshes[2] = { "Hand_Low", "Hand_Low1" };
	const char *selectors[2] = { "Hand_Low", "Hand_Low1?mirrorskin=Hand_Low" };
	for (int h = 0; h < 2; h++)
	{
		const bool right = h == 0;
		Result r;
		if (!Load(handFile.data(), handFile.size(), selectors[h], r, err)) { printf("%s: %s\n", selectors[h], err.c_str()); return 2; }
		Result ro;
		const bool haveOther = Load(handFile.data(), handFile.size(), selectors[1 - h], ro, err);
		const int nj = (int)r.joints.size();
		std::map<std::string, int> idx;
		for (int j = 0; j < nj; j++) idx[r.joints[j].name] = j;
		std::map<std::string, int> idxO;
		if (haveOther) for (int j = 0; j < (int)ro.joints.size(); j++) idxO[ro.joints[j].name] = j;
		const int njo = haveOther ? (int)ro.joints.size() : 0;
		const int hand = idx.at(kHandJoint), palmJ = idx.at(kPalmJoint);
		int joints[5][3];
		for (int f = 0; f < 5; f++) for (int j = 0; j < 3; j++) joints[f][j] = idx.at(kFingerJointNames[f][j]);
		const char *tipNames[5] = { "THUMB_UP_TOP_joint", "INDEX_UP_TOP_joint", "MIDDLE_F_UP_TOP_joint", "RING_UP_TOP_joint", "PINK_UP_TOP_joint" };
		int tips[5];
		for (int f = 0; f < 5; f++) tips[f] = idx.at(tipNames[f]);
		Mats Wrest;
		{ std::vector<TRSd> rl(nj); for (int j = 0; j < nj; j++) rl[j] = r.joints[j].rest; ComposeChain(r, rl.data(), Wrest); }
		std::vector<RestJoint> J(nj);
		for (int j = 0; j < nj; j++) { J[j].parent = r.joints[j].parent; memcpy(J[j].G, Wrest[j].data(), sizeof(J[j].G)); memcpy(J[j].t, r.joints[j].rest.t, sizeof(J[j].t)); }
		HandRig rig; FingerFrame frames[5][3]; double palmN[3]; char why[256] = "";
		if (!BuildHand(J, hand, palmJ, joints, right, rig, frames, palmN, why, sizeof(why))) { printf("%s: %s\n", meshes[h], why); return 2; }
		const int side = right ? SIDE_RIGHT : SIDE_LEFT;
		auto getQ = [&r, nj](int frame, int joint, double q[4]) { for (int k = 0; k < 4; k++) q[k] = r.frames[(size_t)frame * nj + joint].q[k]; };
		auto getQOther = [&](int frame, const char *name, double q[4]) {
			auto it = idxO.find(name);
			if (it == idxO.end() || frame < 0 || frame >= ro.numFrames) return false;
			for (int k = 0; k < 4; k++) q[k] = ro.frames[(size_t)frame * njo + it->second].q[k];
			return true;
		};
		HandPoses hp;
		if (!BuildPoses(rig, frames, side, r.numFrames, getQ, hp, why, sizeof(why))) { printf("%s poses: %s\n", meshes[h], why); return 2; }
		HoldPoses hold;
		if (!BuildHoldPoses(rig, frames, side, r.numFrames, getQ, getQOther, haveOther ? ro.numFrames : 0, hold, why, sizeof(why))) { printf("%s holds: %s\n", meshes[h], why); return 2; }

		// The hand frame (vr_handpose_math.h HandFrameFromJoints on the REST joints, as the engine's seat), in metres.
		Vec3d P5[5];
		const int frameJ[5] = { hand, joints[1][0], joints[2][0], joints[3][0], joints[4][0] };
		for (int k = 0; k < 5; k++) { const V3 p = Pos(Wrest[frameJ[k]].data()); P5[k] = Vec3d(p.x, p.y, p.z); }
		Vec3d X, Y, Z, sock;
		HandFrameFromJoints(P5, right, X, Y, Z, sock);
		auto toHand = [&](V3 p) { const V3 d = p - V3(sock.x, sock.y, sock.z); return V3(Dot(d, V3(X.x, X.y, X.z)), Dot(d, V3(Y.x, Y.y, Y.z)), Dot(d, V3(Z.x, Z.y, Z.z))) * kHandScale; };
		auto dirHand = [&](V3 d) { return Unit(V3(Dot(d, V3(X.x, X.y, X.z)), Dot(d, V3(Y.x, Y.y, Y.z)), Dot(d, V3(Z.x, Z.y, Z.z)))); };

		HeldState held; held.weapon = true; held.gripClass = "pistol";
		Inputs ready; ready.valid = true; ready.trigger = 0.f; ready.triggerTouch = true;
		Inputs fire = ready; fire.trigger = 1.f;
		const Posed pr = PoseHand(r, Wrest, rig, hp, ready, &hold, &held);
		const Posed pf = PoseHand(r, Wrest, rig, hp, fire, &hold, &held);
		Mesh handR = Skin(r, pr.P), handF = Skin(r, pf.P);
		for (MeshV &v : handR.v) { v.p = toHand(v.p); v.n = dirHand(v.n); }
		for (MeshV &v : handF.v) { v.p = toHand(v.p); v.n = dirHand(v.n); }
		auto J3 = [&](const Posed &p, int j) { return toHand(Pos(p.G[j].data())); };

		// The channel: the hole the wrapped fingers and the palm close round. Its direction first from the knuckle line
		// (index -> little knuckle); then per finger (middle, ring, little) the cross-section through that finger's MID
		// joint: the hand's skin vertices within 4 mm of the plane, and the point of the plane farthest from every one of
		// them that is ENCLOSED (skin in at least 7 of 8 directions round it) -- the hole's centre and radius.
		V3 chanDir = Unit(J3(pr, joints[4][0]) - J3(pr, joints[1][0]));
		V3 cen[3]; double holeR[3];
		for (int pass = 0; pass < 2; pass++)
		{
			for (int f = 2; f <= 4; f++)
			{
				const V3 o = J3(pr, joints[f][1]);
				V3 e1 = Unit(Cross(chanDir, V3(0, 1, 0))); if (Len(e1) < 0.5) e1 = Unit(Cross(chanDir, V3(1, 0, 0)));
				const V3 e2 = Cross(chanDir, e1);
				std::vector<std::pair<double, double>> pts;
				for (const MeshV &v : handR.v) { const V3 d = v.p - o; if (fabs(Dot(d, chanDir)) < 0.004) pts.push_back({ Dot(d, e1), Dot(d, e2) }); }
				double best = -1, bu = 0, bv = 0;
				for (double u = -0.06; u <= 0.06; u += 0.001)
					for (double w = -0.06; w <= 0.06; w += 0.001)
					{
						double md = 1e9; int sect = 0;
						for (auto &q : pts)
						{
							const double du = q.first - u, dv = q.second - w, d = sqrt(du * du + dv * dv);
							md = fmin(md, d);
							int k = (int)floor((atan2(dv, du) + M_PI) / (2 * M_PI) * 8); if (k > 7) k = 7;
							sect |= 1 << k;
						}
						int n = 0; for (int k = 0; k < 8; k++) n += (sect >> k) & 1;
						if (n >= 7 && md > best) { best = md; bu = u; bv = w; }
					}
				cen[f - 2] = o + e1 * bu + e2 * bv;
				holeR[f - 2] = best;
			}
			chanDir = Unit(cen[2] - cen[0]);
		}
		const V3 palmAxis = Unit(J3(pr, joints[2][0]) - J3(pr, hand));
		// The web: the crease between thumb and index -- of the skin within 12 mm of the line from the thumb's middle joint
		// to the index knuckle (its middle 60 %), the vertex deepest toward the wrist (least X): where a beavertail sits.
		const V3 fa = J3(pr, joints[0][1]), fb = J3(pr, joints[1][0]);
		int webV = -1; double webBest = 1e30;
		for (size_t i = 0; i < handR.v.size(); i++)
		{
			const V3 q = handR.v[i].p, ab = fb - fa;
			const double u = Dot(q - fa, ab) / Dot(ab, ab);
			if (u < 0.2 || u > 0.8 || Len(q - (fa + ab * u)) > 0.012) continue;
			if (q.x < webBest) { webBest = q.x; webV = (int)i; }
		}
		// The index pad: the distal segment's most palmward skin vertex (the segment's palmward axis = the finger
		// frame's +Y carried by the posed global).
		auto pad = [&](const Posed &p, const Mesh &m) {
			const int jt = joints[1][2];
			const FingerFrame &F = frames[1][2];
			double Gr[16]; memcpy(Gr, p.G[jt].data(), sizeof(Gr));
			const V3 yLocal(F.Y[0], F.Y[1], F.Y[2]);
			const V3 palmward = dirHand(Unit(MatDir(Gr, yLocal)));
			const V3 a = J3(p, jt), b = J3(p, tips[1]);
			int best = -1; double bv = -1e30;
			for (size_t i = 0; i < m.v.size(); i++)
			{
				const V3 q = m.v[i].p;
				const V3 ab = b - a; const double t = Dot(q - a, ab) / Dot(ab, ab);
				if (t < 0.2 || t > 0.8) continue;
				if (Len(q - (a + ab * t)) > 0.012) continue;
				const double sc = Dot(q - a, palmward);
				if (sc > bv) { bv = sc; best = (int)i; }
			}
			return best;
		};
		const int padR = pad(pr, handR), padF = pad(pf, handF);
		if (webV < 0 || padR < 0 || padF < 0) { printf("%s: landmarks not found (web %d pad %d %d)\n", meshes[h], webV, padR, padF); return 2; }
		const V3 web = handR.v[webV].p, pR0 = handR.v[padR].p, pF = handF.v[padF].p;
		printf("\n==== %s (%s hand), posed grip_pistol (index on the trigger, wrap_half at trigger 0; wrap_tight at 1)\n", meshes[h], right ? "RIGHT" : "LEFT");
		printf("  hand frame: X fingers, Y back of the hand, Z thumb axis (%s); metres, the grip socket at 0\n", right ? "thumb -Z" : "thumb +Z");
		printf("  the hole (middle, ring, little): centres (%.4f %.4f %.4f) (%.4f %.4f %.4f) (%.4f %.4f %.4f), radius %.1f %.1f %.1f mm; axis (%.3f %.3f %.3f)\n",
			cen[0].x, cen[0].y, cen[0].z, cen[1].x, cen[1].y, cen[1].z, cen[2].x, cen[2].y, cen[2].z, holeR[0] * 1000, holeR[1] * 1000, holeR[2] * 1000, chanDir.x, chanDir.y, chanDir.z);
		for (int f = 0; f < 5; f++) { const V3 a = J3(pr, joints[f][0]), b = J3(pr, tips[f]); printf("  %s knuckle (%.3f %.3f %.3f) tip (%.3f %.3f %.3f)\n", kFingerJointNames[f][0], a.x, a.y, a.z, b.x, b.y, b.z); }
		printf("  the wrist->middle-knuckle axis (%.3f %.3f %.3f): %.1f deg from the channel axis\n", palmAxis.x, palmAxis.y, palmAxis.z, Deg(acos(fabs(Dot(palmAxis, chanDir)))));
		printf("  web vertex %d (%.4f %.4f %.4f), x %.4f (the deepest toward the wrist); index pad (ready) v%d (%.4f %.4f %.4f), (pulled) v%d (%.4f %.4f %.4f)\n",
			webV, web.x, web.y, web.z, webBest, padR, pR0.x, pR0.y, pR0.z, padF, pF.x, pF.y, pF.z);

		// ---- the fit: h = R (s g) + t ----
		const V3 fwdH(1, 0, 0), upH = right ? V3(0, 0, -1) : V3(0, 0, 1);
		const M3 R0 = FromCols(Cross(fwdH, upH), fwdH, upH);   // gun x (right) = fwd x up, gun y = fwd, gun z = up
		// THE TRIGGER FINGER, MEASURED (HeldState::indexReady, vr_handpose_math.h): the index as the in-between of the
		// open frame and the grip's own index, sampled every 0.05; at each, the pad and the skin the gun may meet.
		static GunSDF sdf; if (sdf.m == nullptr) { sdf.Build(gun); sdf.Fill(); }
		const int kSamples = 21;
		std::vector<V3> padAt(kSamples);
		std::vector<std::vector<V3>> skinAt(kSamples);
		std::vector<Mesh> meshAt(kSamples);
		for (int k = 0; k < kSamples; k++)
		{
			HeldState hk = held; hk.indexReady = hk.indexFire = (float)k / (kSamples - 1);
			const Posed pk = PoseHand(r, Wrest, rig, hp, ready, &hold, &hk);
			Mesh mk = Skin(r, pk.P);
			for (MeshV &v : mk.v) { v.p = toHand(v.p); v.n = dirHand(v.n); }
			const int pv = pad(pk, mk);
			padAt[k] = pv >= 0 ? mk.v[pv].p : V3();
			for (size_t i = 0; i < mk.v.size(); i += 3)
			{
				if (mk.v[i].bone == hand && Dot(mk.v[i].p, V3(1, 0, 0)) < -0.04) continue;   // not the forearm
				skinAt[k].push_back(mk.v[i].p);
			}
			meshAt[k] = mk;
		}
		auto padOf = [&](double u) {
			u = u < 0 ? 0 : (u > 1 ? 1 : u);
			const double x = u * (kSamples - 1); int i = (int)floor(x); if (i >= kSamples - 1) i = kSamples - 2;
			const double f = x - i;
			return padAt[i] * (1 - f) + padAt[i + 1] * f;
		};
		for (int k = 0; k < kSamples; k += 4) printf("  index in-between %.2f: its pad %.1f mm from the web (the gun: the beavertail to the trigger face %.1f mm)\n",
			(double)k / (kSamples - 1), Len(padAt[k] - web) * 1000, Len(trig - beaver) * s * 1000);
		const double kPenW = 2.0, kSlack = 0.0005;   // penetration weight; skin give (0.5 mm)
		constexpr int NP = 7;   // rotation vector, translation, the index's in-between
		auto residual = [&](const double prm[NP], std::vector<double> &e) {
			const M3 R = Mul(Rodrigues(V3(prm[0], prm[1], prm[2])), R0);
			const V3 t(prm[3], prm[4], prm[5]);
			auto T = [&](V3 p) { return R * (p * s) + t; };
			e.clear();
			const V3 b = T(beaver) - web; e.push_back(b.x); e.push_back(b.y); e.push_back(b.z);
			const V3 c = T(trig) - padOf(prm[6]); e.push_back(c.x); e.push_back(c.y); e.push_back(c.z);
			e.push_back(prm[6] < 0 ? prm[6] : (prm[6] > 1 ? prm[6] - 1 : 0));
			M3 Rt; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) Rt.m[i][j] = R.m[j][i];
			int k = (int)lround((prm[6] < 0 ? 0 : (prm[6] > 1 ? 1 : prm[6])) * (kSamples - 1));
			for (const V3 &hpnt : skinAt[k])
			{
				const double d = sdf.Query(Rt * (hpnt - t) * (1.0 / s)) * s;   // metres, negative inside
				e.push_back(d < -kSlack ? (d + kSlack) * kPenW : 0.0);
			}
		};
		std::vector<double> e, e2;
		auto cost = [&](const double p[NP]) { residual(p, e2); double c = 0; for (double x : e2) c += x * x; return c; };
		auto solve = [&](double prm[NP], int iters) {
			double lambda = 1e-3;
			for (int it = 0; it < iters; it++)
			{
				residual(prm, e);
				const size_t m = e.size();
				std::vector<std::array<double, NP>> Jm(m);
				for (int k = 0; k < NP; k++)
				{
					double q[NP]; memcpy(q, prm, sizeof(q)); q[k] += k == 6 ? 1e-3 : 1e-7;
					residual(q, e2);
					for (size_t i = 0; i < m; i++) Jm[i][k] = (e2[i] - e[i]) / (k == 6 ? 1e-3 : 1e-7);
				}
				double A[NP][NP] = {}, bvec[NP] = {};
				for (size_t i = 0; i < m; i++) for (int a = 0; a < NP; a++) { bvec[a] -= Jm[i][a] * e[i]; for (int b = 0; b < NP; b++) A[a][b] += Jm[i][a] * Jm[i][b]; }
				for (int a = 0; a < NP; a++) A[a][a] = A[a][a] * (1 + lambda) + 1e-12;
				double M[NP][NP + 1];
				for (int a = 0; a < NP; a++) { for (int b = 0; b < NP; b++) M[a][b] = A[a][b]; M[a][NP] = bvec[a]; }
				for (int c = 0; c < NP; c++)
				{
					int piv = c; for (int rr = c + 1; rr < NP; rr++) if (fabs(M[rr][c]) > fabs(M[piv][c])) piv = rr;
					for (int k = 0; k <= NP; k++) std::swap(M[c][k], M[piv][k]);
					for (int rr = 0; rr < NP; rr++) if (rr != c) { const double f = M[rr][c] / M[c][c]; for (int k = c; k <= NP; k++) M[rr][k] -= f * M[c][k]; }
				}
				double q[NP];
				for (int a = 0; a < NP; a++) q[a] = prm[a] + M[a][NP] / M[a][a];
				const double cq = cost(q), cp = cost(prm);
				if (cq < cp) { memcpy(prm, q, sizeof(q)); lambda *= 0.3; } else lambda *= 10;
				if (lambda > 1e8) break;
			}
			return cost(prm);
		};
		// MULTI-START: the gun pitched about the back-of-hand axis Y (the gun's own right) from -40 to +40 deg, turned
		// +-15 about the thumb axis Z, its beavertail put on the web, the index half way; the lowest cost wins.
		double prm[NP] = { 0, 0, 0, 0, 0, 0, 0.5 }, bestCost = 1e30;
		for (int pi = -4; pi <= 4; pi++)
			for (int ri = -1; ri <= 1; ri++)
			{
				const V3 w = V3(0, 1, 0) * (pi * 10.0 * M_PI / 180) + V3(0, 0, 1) * (ri * 15.0 * M_PI / 180);
				double q[NP] = { w.x, w.y, w.z, 0, 0, 0, 0.5 };
				const M3 Rq = Mul(Rodrigues(w), R0);
				const V3 t0 = web - Rq * (beaver * s);
				q[3] = t0.x; q[4] = t0.y; q[5] = t0.z;
				const double c = solve(q, 25);
				if (c < bestCost) { bestCost = c; memcpy(prm, q, sizeof(q)); }
			}
		solve(prm, 80);
		const double idxReady = prm[6] < 0 ? 0 : (prm[6] > 1 ? 1 : prm[6]);
		printf("  multi-start: the best cost %.3g; the index on the trigger: %.3f of the way from the open frame to the grip's\n", cost(prm), idxReady);
		const int kReady = (int)lround(idxReady * (kSamples - 1));
		handR = meshAt[kReady];
		const V3 pR = padOf(idxReady);
		const M3 R = Mul(Rodrigues(V3(prm[0], prm[1], prm[2])), R0);
		const V3 t(prm[3], prm[4], prm[5]);
		auto T = [&](V3 p) { return R * (p * s) + t; };
		double chanErr = 0;   // information only: the grip's centreline against the hole's centres
		{
			const V3 l0 = T(mc), ld = R * gd;
			for (int k = 0; k < 3; k++) { const V3 d = cen[k] - l0; chanErr = fmax(chanErr, Len(d - ld * Dot(d, ld))); }
		}
		const double webErr = Len(T(beaver) - web), padErr = Len(T(trig) - pR);
		const V3 bore = R * V3(0, 1, 0), mz = T(muzzle);
		printf("  FIT: the grip's centreline %.1f mm (worst) from the hole's centres (information); the web %.1f mm from the beavertail point; the index pad %.1f mm from the trigger face\n",
			chanErr * 1000, webErr * 1000, padErr * 1000);
		printf("  the bore in the hand frame (%.3f %.3f %.3f): %.1f deg from the fingers' axis X; the muzzle at (%.3f %.3f %.3f) m\n",
			bore.x, bore.y, bore.z, Deg(acos(bore.x)), mz.x, mz.y, mz.z);
		// pulled: the trigger moves back with the index (the take's trigger travel, frame 113: 10 units)
		// The pulled index: the in-between whose pad meets the trigger face moved back by its travel.
		const V3 trigPulled = trig - V3(0, 10, 0);
		double idxFire = idxReady, padFireErr = 1e9;
		for (double u = idxReady; u <= 1.0001; u += 0.005) { const double d = Len(T(trigPulled) - padOf(u)); if (d < padFireErr) { padFireErr = d; idxFire = u; } }
		(void)pF;
		// The thumb: its tip against the thumb safety (the frame's rear left above the grip for a right hand: the gun's
		// -x side at the safety's height), information only (the thumb is the authored frame).
		const V3 thumbTip = J3(pr, tips[0]);
		const V3 safety(right ? -40.0 : 40.0, -300.0, 120.0);
		printf("  the index pulled: %.3f (its pad %.1f mm from the face moved back 10 units); the thumb tip %.1f mm from the thumb safety (%s side)\n",
			idxFire, padFireErr * 1000, Len(thumbTip - T(safety)) * 1000, right ? "left" : "right");

		// ---- the checks: no finger inside the gun; the pad on the face; the web at the tang ----
		Mesh gunH = gun;
		for (MeshV &v : gunH.v) { v.p = T(v.p); v.n = R * v.n; }
		int inside = 0, insideFingers = 0; double deepest = 0; int deepV = -1;
		const int nh = (int)handR.v.size();
		for (int i = 0; i < nh; i++)
		{
			M3 Rt; for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) Rt.m[a][b] = R.m[b][a];
			const double d = sdf.Query(Rt * (handR.v[i].p - t) * (1.0 / s)) * s;
			if (d < -0.001) { inside++; if (d < deepest) { deepest = d; deepV = i; } }
			const int b = handR.v[i].bone;
			bool finger = false;
			for (int f = 0; f < 5; f++) for (int j = 0; j < 3; j++) if (joints[f][j] == b) finger = true;
			for (int f = 0; f < 5; f++) if (tips[f] == b) finger = true;
			if (finger && d < -0.002) insideFingers++;
		}
		const double padFace = Dot(pR - T(trig), R * V3(0, 1, 0));   // in front of the face (+) or in it (-)
		printf("  CHECK hand vertices more than 1 mm inside the gun: %d of %d (finger vertices more than 2 mm in: %d); the deepest %.1f mm (vertex %d, joint %s)\n",
			inside, nh, insideFingers, -deepest * 1000, deepV, deepV >= 0 ? r.joints[handR.v[deepV].bone].name.c_str() : "-");
		printf("  CHECK the index pad (ready) %.1f mm from the trigger face point, %.1f mm in front of the face; (pulled, the trigger back 10 units) %.1f mm\n",
			padErr * 1000, padFace * 1000, padFireErr * 1000);
		char line[256];
		snprintf(line, sizeof line, "%s: the index pad on the trigger face within 6 mm (%.1f)", meshes[h], padErr * 1000); Check(padErr < 0.006, line);
		snprintf(line, sizeof line, "%s: the web at the beavertail within 8 mm (%.1f)", meshes[h], webErr * 1000); Check(webErr < 0.008, line);
		snprintf(line, sizeof line, "%s: no finger vertex more than 2 mm inside the gun (%d)", meshes[h], insideFingers); Check(insideFingers == 0, line);

		// The seat for the engine: model render coords (x, z, y of the file) -> the hand frame in GUN FILE units,
		// so MODELDEF Scale s takes it to metres (models.cpp: the seat first, then Scale, in the grip frame).
		// hand_m = R s file + t  =>  hand_units = R file + t / s;  file = Swap(render)
		const V3 c0 = R * V3(1, 0, 0), c1 = R * V3(0, 1, 0), c2 = R * V3(0, 0, 1);
		const V3 tu = t * (1.0 / s);
		fprintf(res, "seat_%s %.9g %.9g %.9g 0 %.9g %.9g %.9g 0 %.9g %.9g %.9g 0 %.9g %.9g %.9g 1\n", right ? "right" : "left",
			c0.x, c0.y, c0.z, c2.x, c2.y, c2.z, c1.x, c1.y, c1.z, tu.x, tu.y, tu.z);
		fprintf(res, "fit_%s R %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g t %.9g %.9g %.9g\n", right ? "right" : "left",
			R.m[0][0], R.m[0][1], R.m[0][2], R.m[1][0], R.m[1][1], R.m[1][2], R.m[2][0], R.m[2][1], R.m[2][2], t.x, t.y, t.z);
		fprintf(res, "index_%s ready %.4f fire %.4f\n", right ? "right" : "left", idxReady, idxFire);
		fprintf(res, "check_%s chan_mm %.2f web_mm %.2f pad_mm %.2f pad_fire_mm %.2f inside %d inside_fingers %d deepest_mm %.2f\n", right ? "right" : "left",
			chanErr * 1000, webErr * 1000, padErr * 1000, padFireErr * 1000, inside, insideFingers, -deepest * 1000);

		// dumps for py/rend.py, and the landmarks
		Dump((outDir + "/" + meshes[h] + "_hand_ready.txt").c_str(), handR);
		Dump((outDir + "/" + meshes[h] + "_gun.txt").c_str(), gunH);
		FILE *pt = fopen((outDir + "/" + meshes[h] + "_pts.txt").c_str(), "w");
		fprintf(pt, "web %.6f %.6f %.6f yellow\npad %.6f %.6f %.6f cyan\nbeaver %.6f %.6f %.6f orange\ntrig %.6f %.6f %.6f magenta\n",
			web.x, web.y, web.z, pR.x, pR.y, pR.z, T(beaver).x, T(beaver).y, T(beaver).z, T(trig).x, T(trig).y, T(trig).z);
		for (int k = 0; k < 3; k++) fprintf(pt, "c%d %.6f %.6f %.6f lime\n", k, cen[k].x, cen[k].y, cen[k].z);
		fprintf(pt, "socket 0 0 0 white\nmuzzle %.6f %.6f %.6f red\n", mz.x, mz.y, mz.z);
		fclose(pt);
	}
	fclose(res);
	printf("\n%s: %d check(s) failed\n", gFails ? "FAIL" : "PASS", gFails);
	return gFails ? 1 : 0;
}
