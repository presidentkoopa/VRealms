// tools/colt_test/fit_common.h -- what colt_fit and support_fit share: the hand posed through the engine path
// (vr_handpose_math.h SelectHold + StepPoseHeld converged, PosePalette), skinned; the small vector maths; the gun's signed
// distance (bucketed, then a dense grid). Moved out of colt_fit.cpp unchanged (2026-11-23).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "fbx_skinned.h"
#include "vr_handpose_math.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace FBXSkinned;
using namespace VRHandPose;

static inline std::vector<char> ReadFile(const char *path)
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
static inline V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
static inline V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static inline V3 operator*(V3 a, double s) { return { a.x * s, a.y * s, a.z * s }; }
static inline double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static inline double Len(V3 a) { return sqrt(Dot(a, a)); }
static inline V3 Unit(V3 a) { const double l = Len(a); return l > 0 ? a * (1.0 / l) : a; }
static inline V3 Pos(const double m[16]) { return { m[12], m[13], m[14] }; }
static inline V3 MatPoint(const double m[16], V3 p)
{
	return { m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13], m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14] };
}
static inline V3 MatDir(const double m[16], V3 p)
{
	return { m[0] * p.x + m[4] * p.y + m[8] * p.z, m[1] * p.x + m[5] * p.y + m[9] * p.z, m[2] * p.x + m[6] * p.y + m[10] * p.z };
}
static inline double Deg(double r) { return r * 180.0 / M_PI; }

typedef std::vector<std::array<double, 16>> Mats;

static inline void ComposeChain(const Result &r, const TRSd *locals, Mats &world)
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

static inline Posed PoseHand(const Result &r, const Mats &Wrest, const HandRig &rig, const HandPoses &hp, const Inputs &in,
	const HoldPoses *hold, const HeldState *held, const HeldState *other = nullptr)
{
	const int nj = (int)r.joints.size();
	SideState rot;
	PalmState palm;
	for (int i = 0; i < 400; i++)
	{
		HoldChoice ch;
		SelectHold(hold, *held, other != nullptr ? *other : HeldState(), in, ch);
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

static inline Mesh Skin(const Result &r, const Mats &P)
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

static inline void Dump(const char *path, const Mesh &m)
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
static inline M3 FromCols(V3 a, V3 b, V3 c) { M3 r; r.m[0][0] = a.x; r.m[1][0] = a.y; r.m[2][0] = a.z; r.m[0][1] = b.x; r.m[1][1] = b.y; r.m[2][1] = b.z; r.m[0][2] = c.x; r.m[1][2] = c.y; r.m[2][2] = c.z; return r; }
static inline M3 Mul(const M3 &a, const M3 &b) { M3 r; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { r.m[i][j] = 0; for (int k = 0; k < 3; k++) r.m[i][j] += a.m[i][k] * b.m[k][j]; } return r; }
static inline M3 Rodrigues(V3 w)
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

// the rotation vector of a rotation (log map)
static inline V3 RotVec(const M3 &R)
{
	double c = (R.m[0][0] + R.m[1][1] + R.m[2][2] - 1) * 0.5;
	c = c > 1 ? 1 : (c < -1 ? -1 : c);
	const double th = acos(c);
	if (th < 1e-9) return V3();
	const V3 a(R.m[2][1] - R.m[1][2], R.m[0][2] - R.m[2][0], R.m[1][0] - R.m[0][1]);
	if (M_PI - th < 1e-6)
	{
		// near 180 degrees: the axis from the symmetric part
		V3 k(sqrt(fmax(0.0, (R.m[0][0] + 1) * 0.5)), sqrt(fmax(0.0, (R.m[1][1] + 1) * 0.5)), sqrt(fmax(0.0, (R.m[2][2] + 1) * 0.5)));
		if (R.m[0][1] < 0) k.y = -k.y;
		if (R.m[0][2] < 0) k.z = -k.z;
		return Unit(k) * th;
	}
	return a * (th / (2 * sin(th)));
}

// closest point on triangle (Ericson, Real-Time Collision Detection 5.1.5)
static inline V3 ClosestOnTri(V3 p, V3 a, V3 b, V3 c, double &u, double &v, double &w)
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
static inline double SignedDistance(const Mesh &m, V3 p, double cut)
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
	double kCell = 16.0, kReach = 64.0;   // per instance (support_fit sizes the shooting hand's own); the gun's as before
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
	double kGrid = 5.0;
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

