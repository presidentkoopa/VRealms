// tools/colt_test/support_fit.cpp -- THE COLT HELD IN TWO HANDS: the support hand fitted to the gun AND to the posed
// shooting hand (owner 2026-11-23: "does our hands allow for 'two handing' the pistol", with the picture "How to Grip a
// Pistol", panels 3, 4 and 6: the thumbs-forward two-hand grip).
//
// The shooting hand is posed exactly as the engine poses it (colt_fit.txt: its seat, its measured index and wraps;
// grip_pistol through SelectHold + StepPoseHeld), and the gun placed in it by colt_fit's fit. The OTHER hand (its own
// mesh, as the engine draws it) holds the GRIPSUBJ_Support subject (hold_support = the owner's authored
// fbx_twohand_support), each finger the in-between of the open frame and that frame's finger by a knuckle amount and an
// outer-joints amount (vr_handpose_math.h HeldState::wrap / wrapBase, the held-subject path). Fitted: the hand's rigid
// place on the gun (6) and the ten amounts, by least squares, toward:
//   (a) the support palm on the grip's OFF-SIDE panel (the shooting hand's thumb side), its palm facing the panel;
//   (b) its index, middle, ring and little end segments on the shooting hand's finger backs, round the front strap under
//       the trigger guard (the end segments within 3 mm of the shooting hand's skin, in front of the strap);
//   (c) its thumb along the frame on the off side below the slide, pointing forward (within 25 degrees of the bore);
//   (d) no skin of the support hand more than 2 mm into the gun or into the shooting hand (and none of the shooting
//       hand's into it: the same contact read from the other side).
// Writes per pairing (the right hand shooting, the left supporting; and the mirror) the seat for the engine: the support
// hand's frame in the SHOOTING hand's frame (metres; vr_handpose_math.h hand frame: X fingers, Y back of the hand, Z the
// thumb axis), and its ten amounts; mesh dumps for rend.py.
//
//   support_fit <hands.fbx> <colt1911.fbx> <colt_fit.txt> <outdir>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fit_common.h"
#include <map>

static const double kHandScale = 0.11094212;   // vrealms_iwad/MODELDEF.txt, the hands' Scale (metres per hand file unit)

// HARD checks (the brief's limit: no skin of either hand more than 2 mm into the gun or the other hand) fail the run; the
// SHAPE checks (the picture's contacts) are reported PASS / NOT MET and written to support_fit.txt, the run going on.
static int gFails = 0, gNotMet = 0;
static void Check(bool ok, const char *what, bool hard = false)
{
	printf("%s %s\n", ok ? "PASS" : (hard ? "FAIL" : "NOT MET"), what);
	if (!ok) { if (hard) gFails++; else gNotMet++; }
}

// ---- colt_fit.txt ----
struct MainFit { M3 R; V3 t; double idxReady = 0, idxBase = 0, wrap[5] = { -1, -1, -1, -1, -1 }, wrapBase[5] = { -1, -1, -1, -1, -1 }; bool ok = false; };
struct FitFile { double s = 0; MainFit side[2]; };
static FitFile ReadFit(const char *path)
{
	FitFile f;
	FILE *in = fopen(path, "r");
	if (!in) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
	char line[1024];
	while (fgets(line, sizeof line, in))
	{
		char key[64];
		if (sscanf(line, "%63s", key) != 1) continue;
		const std::string k = key;
		if (k == "gun_metres_per_unit") sscanf(line, "%*s %lf", &f.s);
		for (int h = 0; h < 2; h++)
		{
			const std::string sn = h == 0 ? "right" : "left";
			MainFit &m = f.side[h];
			if (k == "fit_" + sn)
			{
				double r[9], t[3];
				sscanf(line, "%*s R %lf %lf %lf %lf %lf %lf %lf %lf %lf t %lf %lf %lf", &r[0], &r[1], &r[2], &r[3], &r[4], &r[5], &r[6], &r[7], &r[8], &t[0], &t[1], &t[2]);
				for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) m.R.m[i][j] = r[i * 3 + j];
				m.t = V3(t[0], t[1], t[2]);
				m.ok = true;
			}
			if (k == "index_" + sn) { double fire; sscanf(line, "%*s ready %lf fire %lf base %lf", &m.idxReady, &fire, &m.idxBase); }
			if (k == "wrap_" + sn)
				sscanf(line, "%*s thumb %lf %lf middle %lf %lf ring %lf %lf little %lf %lf", &m.wrapBase[0], &m.wrap[0], &m.wrapBase[2], &m.wrap[2],
					&m.wrapBase[3], &m.wrap[3], &m.wrapBase[4], &m.wrap[4]);
		}
	}
	fclose(in);
	return f;
}

// ---- one hand model, as colt_fit builds it (the engine's path) ----
struct HandModel
{
	Result r, ro;
	bool haveOther = false;
	int nj = 0, hand = -1, palmJ = -1, joints[5][3] = {}, tips[5] = {};
	Mats Wrest;
	HandRig rig;
	FingerFrame frames[5][3];
	HandPoses hp;
	HoldPoses hold;
	Vec3d X, Y, Z, sock;
	bool right = true;

	bool Load(const std::vector<char> &file, const char *sel, const char *selOther, bool isRight)
	{
		right = isRight;
		std::string err;
		if (!FBXSkinned::Load(file.data(), file.size(), sel, r, err)) { printf("%s: %s\n", sel, err.c_str()); return false; }
		haveOther = FBXSkinned::Load(file.data(), file.size(), selOther, ro, err);
		nj = (int)r.joints.size();
		std::map<std::string, int> idx, idxO;
		for (int j = 0; j < nj; j++) idx[r.joints[j].name] = j;
		if (haveOther) for (int j = 0; j < (int)ro.joints.size(); j++) idxO[ro.joints[j].name] = j;
		const int njo = haveOther ? (int)ro.joints.size() : 0;
		hand = idx.at(kHandJoint); palmJ = idx.at(kPalmJoint);
		for (int f = 0; f < 5; f++) for (int j = 0; j < 3; j++) joints[f][j] = idx.at(kFingerJointNames[f][j]);
		const char *tipNames[5] = { "THUMB_UP_TOP_joint", "INDEX_UP_TOP_joint", "MIDDLE_F_UP_TOP_joint", "RING_UP_TOP_joint", "PINK_UP_TOP_joint" };
		for (int f = 0; f < 5; f++) tips[f] = idx.at(tipNames[f]);
		{ std::vector<TRSd> rl(nj); for (int j = 0; j < nj; j++) rl[j] = r.joints[j].rest; ComposeChain(r, rl.data(), Wrest); }
		std::vector<RestJoint> J(nj);
		for (int j = 0; j < nj; j++) { J[j].parent = r.joints[j].parent; memcpy(J[j].G, Wrest[j].data(), sizeof(J[j].G)); memcpy(J[j].t, r.joints[j].rest.t, sizeof(J[j].t)); }
		double palmN[3]; char why[256] = "";
		if (!BuildHand(J, hand, palmJ, joints, right, rig, frames, palmN, why, sizeof(why))) { printf("%s: %s\n", sel, why); return false; }
		const int side = right ? SIDE_RIGHT : SIDE_LEFT;
		const Result *rp = &r, *rop = &ro;
		const int njl = nj;
		auto getQ = [rp, njl](int frame, int joint, double q[4]) { for (int k = 0; k < 4; k++) q[k] = rp->frames[(size_t)frame * njl + joint].q[k]; };
		auto getQOther = [rop, idxO, njo](int frame, const char *name, double q[4]) {
			auto it = idxO.find(name);
			if (it == idxO.end() || frame < 0 || frame >= rop->numFrames) return false;
			for (int k = 0; k < 4; k++) q[k] = rop->frames[(size_t)frame * njo + it->second].q[k];
			return true;
		};
		if (!BuildPoses(rig, frames, side, r.numFrames, getQ, hp, why, sizeof(why))) { printf("%s poses: %s\n", sel, why); return false; }
		if (!BuildHoldPoses(rig, frames, side, r.numFrames, getQ, getQOther, haveOther ? ro.numFrames : 0, hold, why, sizeof(why))) { printf("%s holds: %s\n", sel, why); return false; }
		Vec3d P5[5];
		const int frameJ[5] = { hand, joints[1][0], joints[2][0], joints[3][0], joints[4][0] };
		for (int k = 0; k < 5; k++) { const V3 p = Pos(Wrest[frameJ[k]].data()); P5[k] = Vec3d(p.x, p.y, p.z); }
		HandFrameFromJoints(P5, right, X, Y, Z, sock);
		return true;
	}
	V3 ToHand(V3 p) const { const V3 d = p - V3(sock.x, sock.y, sock.z); return V3(Dot(d, V3(X.x, X.y, X.z)), Dot(d, V3(Y.x, Y.y, Y.z)), Dot(d, V3(Z.x, Z.y, Z.z))) * kHandScale; }
	V3 DirHand(V3 d) const { return Unit(V3(Dot(d, V3(X.x, X.y, X.z)), Dot(d, V3(Y.x, Y.y, Y.z)), Dot(d, V3(Z.x, Z.y, Z.z)))); }
	// posed, skinned, in the hand frame (metres); joints' globals too
	Mesh PoseMesh(const HeldState &held, const Inputs &in, Mats *G = nullptr, const HeldState *other = nullptr) const
	{
		const ::Posed p = PoseHand(r, Wrest, rig, hp, in, &hold, &held, other);
		Mesh m = Skin(r, p.P);
		for (MeshV &v : m.v) { v.p = ToHand(v.p); v.n = DirHand(v.n); }
		if (G) *G = p.G;
		return m;
	}
	int FingerOf(int bone) const { for (int f = 0; f < 5; f++) { if (tips[f] == bone) return f; for (int j = 0; j < 3; j++) if (joints[f][j] == bone) return f; } return -1; }
	int SegOf(int bone) const { for (int f = 0; f < 5; f++) { if (tips[f] == bone) return 3; for (int j = 0; j < 3; j++) if (joints[f][j] == bone) return j; } return -1; }
};

// the previous pairing's fit (the right hand shooting first): its support hand's rotation and parameters, mirrored as a
// start for the other pairing (the hands are mirror images, the Colt nearly so: gun x and the hand frame's Z flip)
static M3 gPrevR;
static double gPrevPrm[16];
static bool gHavePrev = false;

static M3 Transpose(const M3 &a) { M3 r; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r.m[i][j] = a.m[j][i]; return r; }

int main(int argc, char **argv)
{
	if (argc < 5) { fprintf(stderr, "usage: support_fit <hands.fbx> <colt1911.fbx> <colt_fit.txt> <outdir>\n"); return 2; }
	const std::vector<char> handFile = ReadFile(argv[1]), gunFile = ReadFile(argv[2]);
	const FitFile fit = ReadFit(argv[3]);
	const std::string outDir = argv[4];
	if (fit.s <= 0 || !fit.side[0].ok || !fit.side[1].ok) { printf("colt_fit.txt: incomplete\n"); return 2; }
	const double s = fit.s;

	// ---- the gun at rest, file units (Z up, +Y the muzzle, +X its right side) ----
	Result g; std::string err;
	if (!Load(gunFile.data(), gunFile.size(), "", g, err)) { printf("gun: %s\n", err.c_str()); return 2; }
	std::map<std::string, int> gj;
	for (size_t j = 0; j < g.joints.size(); j++) gj[g.joints[j].name] = (int)j;
	Mats gPal(g.joints.size());
	for (auto &m : gPal) { memset(m.data(), 0, sizeof(double) * 16); m[0] = m[5] = m[10] = m[15] = 1; }
	const Mesh gun = Skin(g, gPal);
	GunSDF sdf; sdf.Build(gun); sdf.Fill();
	// the grip's front strap: the frontmost frame point at each height of the grip (below the guard)
	const int jMain = gj.at("Main");
	double strapY = -1e30;
	for (const MeshV &v : gun.v) if (v.bone == jMain && fabs(v.p.x) < 20 && v.p.z < -60 && v.p.z > -200) strapY = fmax(strapY, v.p.y);
	// the trigger guard's underside: the lowest frame point in front of the strap
	double guardZ = 1e30;
	for (const MeshV &v : gun.v) if (v.bone == jMain && v.p.y > strapY + 10 && v.p.y < 40 && fabs(v.p.x) < 15) guardZ = fmin(guardZ, v.p.z);
	printf("gun: the front strap at y %.1f (file units); the trigger guard's underside at z %.1f; the slide's bottom z 103.6\n", strapY, guardZ);

	const char *selectors[2] = { "Hand_Low", "Hand_Low1?mirrorskin=Hand_Low" };
	{ Result probe; if (!Load(handFile.data(), handFile.size(), selectors[1], probe, err)) selectors[1] = "Hand_Low1"; }

	FILE *res = fopen((outDir + "/support_fit.txt").c_str(), "w");
	fprintf(res, "# tools/colt_test/support_fit.cpp output: the support hand's frame in the shooting hand's frame (metres, column-major 4x4),\n"
		"# and its fingers (thumb, index, middle, ring, little: knuckle, outer) from fbx_open toward fbx_twohand_support\n");

	for (int pair = 0; pair < 2; pair++)
	{
		const int mainH = pair, supH = 1 - pair;   // pair 0: the right hand shoots; pair 1: the left
		const bool mainRight = mainH == 0;
		const double off = mainRight ? -1.0 : 1.0;   // the off side of the gun: the shooting hand's THUMB side (gun -x for a right hand)
		HandModel M, S;
		if (!M.Load(handFile, selectors[mainH], selectors[supH], mainRight) || !S.Load(handFile, selectors[supH], selectors[mainH], !mainRight)) return 2;
		printf("\n==== %s shoots, %s supports\n", mainRight ? "the RIGHT hand" : "the LEFT hand", mainRight ? "the LEFT" : "the RIGHT");
		{
			const V3 x(S.X.x, S.X.y, S.X.z), y(S.Y.x, S.Y.y, S.Y.z), z(S.Z.x, S.Z.y, S.Z.z);
			printf("  the support hand's frame: (X x Y) . Z = %.3f (+1: right-handed)\n", Dot(Cross(x, y), z));
		}

		// ---- the shooting hand, posed as the engine poses it, in GUN FILE units ----
		const MainFit &mf = fit.side[mainH];
		HeldState mh; mh.weapon = true; mh.gripClass = "pistol";
		mh.indexReady = mh.indexFire = (float)mf.idxReady; mh.indexBase = (float)mf.idxBase;
		for (int f : { 0, 2, 3, 4 }) { mh.wrap[f] = (float)mf.wrap[f]; mh.wrapBase[f] = (float)mf.wrapBase[f]; }
		Inputs ready; ready.valid = true; ready.trigger = 0.f; ready.triggerTouch = true;
		Mats mG;
		Mesh mainM = M.PoseMesh(mh, ready, &mG);
		const M3 Rt = Transpose(mf.R);
		auto handToGun = [&](V3 h) { return Rt * (h - mf.t) * (1.0 / s); };    // shooting hand metres -> gun file units
		for (MeshV &v : mainM.v) { v.p = handToGun(v.p); v.n = Unit(Rt * v.n); }
		// the reach (64 units, 17 mm) covers a hand's half-thickness: a point deeper than the reach would read OUTSIDE (+reach)
		GunSDF mainSdf; mainSdf.kReach = 64.0; mainSdf.kGrid = 4.0; mainSdf.Build(mainM); mainSdf.Fill();
		// the shooting hand's landmarks: its thumb tip; its middle, ring, little finger backs
		const V3 mThumbTip = handToGun(M.ToHand(Pos(mG[M.tips[0]].data())));
		printf("  the shooting hand's thumb tip at (%.1f %.1f %.1f) gun units\n", mThumbTip.x, mThumbTip.y, mThumbTip.z);
		// the shooting hand's own depth in the gun (colt_fit's known palm: information)
		{
			double deep = 0; int deepFingers = 0;
			for (const MeshV &v : mainM.v) { const double d = sdf.Query(v.p) * s; deep = fmin(deep, d); if (d < -0.002 && M.FingerOf(v.bone) >= 0) deepFingers++; }
			printf("  the shooting hand in the gun (colt_fit's fit, unchanged): deepest %.1f mm, finger vertices more than 2 mm in: %d\n", -deep * 1000, deepFingers);
		}

		// ---- the support hand: its poses on a grid of (knuckle, outer) amounts, every finger at once ----
		HeldState sh; sh.subject = GRIPSUBJ_Support;
		Inputs supIn; supIn.valid = true; supIn.grip = 1.f; supIn.trigger = 0.f;
		const int kG = 11;
		std::vector<Mesh> gridAt(kG * kG);
		for (int a = 0; a < kG; a++)
			for (int b = 0; b < kG; b++)
			{
				HeldState hk = sh;
				for (int f = 0; f < 5; f++) { hk.wrapBase[f] = (float)a / (kG - 1); hk.wrap[f] = (float)b / (kG - 1); }
				gridAt[a * kG + b] = S.PoseMesh(hk, supIn, nullptr, &mh);
			}
		{
			HoldChoice ch; SelectHold(&S.hold, sh, mh, supIn, ch);
			printf("  the support hand's pose (SelectHold, the shooting hand's held state as the other): %s\n", DescribeHold(&S.hold, sh, ch).c_str());
		}
		{
			// the authored frame itself (no wraps): what the owner's fbx_twohand_support is, for the log
			Mats G0; const Mesh m0 = S.PoseMesh(sh, supIn, &G0, &mh);
			Dump((outDir + "/sup" + std::to_string(pair) + "_authored_hand.txt").c_str(), m0);
		}
		if (getenv("SUP_DUMP_POSES"))
			for (const char *pn : { "fbx_twohand_support", "fbx_twohand_gun", "hold_support_pistol" })
			{
				HeldState hp; hp.pose = pn;
				Dump((outDir + "/sup" + std::to_string(pair) + "_pose_" + pn + ".txt").c_str(), S.PoseMesh(hp, supIn, nullptr, &mh));
			}
		if (getenv("SUP_DUMP_GRID"))
			for (int a : { 0, kG - 1 }) for (int b : { 0, kG - 1 })
				Dump((outDir + "/sup" + std::to_string(pair) + "_grid_" + std::to_string(a) + "_" + std::to_string(b) + ".txt").c_str(), gridAt[a * kG + b]);
		auto gridVert = [&](int i, double ua, double ub) {
			auto cl = [](double u) { return u < 0 ? 0.0 : (u > 1 ? 1.0 : u); };
			const double xa = cl(ua) * (kG - 1), xb = cl(ub) * (kG - 1);
			int a = (int)floor(xa), b = (int)floor(xb); if (a >= kG - 1) a = kG - 2; if (b >= kG - 1) b = kG - 2;
			const double fa = xa - a, fb = xb - b;
			return gridAt[a * kG + b].v[i].p * ((1 - fa) * (1 - fb)) + gridAt[(a + 1) * kG + b].v[i].p * (fa * (1 - fb))
				+ gridAt[a * kG + b + 1].v[i].p * ((1 - fa) * fb) + gridAt[(a + 1) * kG + b + 1].v[i].p * (fa * fb);
		};
		const Mesh &rest = gridAt[0];
		// the skin points: one per control point, not the forearm
		std::vector<int> idx, fin, seg;
		{
			std::vector<char> seen(S.r.verts.size() + 1, 0);
			for (size_t i = 0; i < rest.v.size(); i++)
			{
				const uint32_t cp = S.r.verts[i].controlPoint;
				if (cp < seen.size()) { if (seen[cp]) continue; seen[cp] = 1; }
				if (rest.v[i].bone == S.hand && rest.v[i].p.x < -0.04) continue;
				idx.push_back((int)i); fin.push_back(S.FingerOf(rest.v[i].bone)); seg.push_back(S.SegOf(rest.v[i].bone));
			}
		}
		// the palm: the palm joint's skin facing the palm side (-Y), its middle; its centre and normal (fixed: the palm takes
		// fbx_twohand_support's frame whatever the fingers do)
		std::vector<int> palmV;
		V3 palmC, palmN;
		{
			for (size_t i = 0; i < rest.v.size(); i++)
				if (rest.v[i].bone == S.palmJ && rest.v[i].n.y < -0.6) palmV.push_back((int)i);
			for (int i : palmV) { palmC = palmC + rest.v[i].p; palmN = palmN + rest.v[i].n; }
			palmC = palmC * (1.0 / palmV.size()); palmN = Unit(palmN);
			// keep the central ones (within 20 mm of the centre)
			std::vector<int> keep;
			for (int i : palmV) if (Len(rest.v[i].p - palmC) < 0.020) keep.push_back(i);
			palmV = keep;
			palmC = V3(); for (int i : palmV) palmC = palmC + rest.v[i].p; palmC = palmC * (1.0 / palmV.size());
			printf("  the support palm: %zu skin points, centre (%.4f %.4f %.4f) m, normal (%.3f %.3f %.3f) in its hand frame\n", palmV.size(), palmC.x, palmC.y, palmC.z, palmN.x, palmN.y, palmN.z);
		}

		// the support index's knuckle: its first segment's skin points (their centre, read through the grid)
		std::vector<int> knuckleV;
		for (size_t n = 0; n < idx.size(); n++) if (fin[n] == 1 && seg[n] == 0) knuckleV.push_back((int)n);
		// the shooting hand's skin points (one per control point): read against the SUPPORT hand's own distance field, built in
		// the support hand's frame for the fingers of the round before (rebuilt each round: the converse of "support skin in the
		// shooting hand", which a vertex test from one side alone misses -- 147 shooting-hand points 12 mm deep went unseen)
		std::vector<int> mainIdx;
		{
			std::vector<char> seenM(M.r.verts.size() + 1, 0);
			for (size_t i = 0; i < mainM.v.size(); i++)
			{
				const uint32_t cp = M.r.verts[i].controlPoint;
				if (cp < seenM.size()) { if (seenM[cp]) continue; seenM[cp] = 1; }
				if (mainM.v[i].bone == M.hand && (mf.R * (mainM.v[i].p * s) + mf.t).x < -0.04) continue;   // not the forearm stub (its open rim)
				mainIdx.push_back((int)i);
			}
		}
		Mesh supLocal; GunSDF supLocalSdf; bool haveLocal = false;
		std::vector<char> supActive(idx.size(), 1), mainActive(mainIdx.size(), 1);   // refilter, below
		auto buildLocal = [&](const double *prm6) {
			HeldState hk = sh;
			for (int f = 0; f < 5; f++) { hk.wrap[f] = (float)fmin(1.0, fmax(0.0, prm6[f])); hk.wrapBase[f] = (float)fmin(1.0, fmax(0.0, prm6[5 + f])); }
			supLocal = S.PoseMesh(hk, supIn, nullptr, &mh);
			for (MeshV &v : supLocal.v) v.p = v.p * (1.0 / s);     // hand metres -> file-unit scale (the SDF's cell sizes)
			supLocalSdf = GunSDF(); supLocalSdf.kReach = 64.0; supLocalSdf.kGrid = 4.0; supLocalSdf.Build(supLocal); supLocalSdf.Fill();
			haveLocal = true;
		};
		// the support thumb's end segment's centre: under the shooting thumb -- two thumb radii (9 mm each) below the shooting
		// thumb's centre line, less 2 mm of give -- and above the trigger guard's opening (z -10: the frame over the guard)
		const double zHi = mThumbTip.z - 2 * 0.009 / s - 0.002 / s, zLo = -10.0;
		printf("  the support thumb's band: its end segment's centre z %.1f..%.1f (the shooting thumb's tip z %.1f)\n", zLo, zHi, mThumbTip.z);
		const V3 knuckleAt(off * 88.0, strapY + 10.0, guardZ - 25.0);   // gun units: under the guard, at the grip's front corner, off side

		// ---- the fit: gun metres = Rs h + ts ----
		constexpr int NP = 16;   // rotation vector, translation, five fingers' outer amounts, five knuckles'
		const int kOut[5] = { 6, 7, 8, 9, 10 }, kBase[5] = { 11, 12, 13, 14, 15 };
		const V3 anchor = V3(off * 56.0, -265.0, -120.0) * s;   // the off-side panel's middle, metres (a weak pull only)
		double kPenW = 5.0;
		const double kSlack = 0.0005;
		double kW = 1.0;   // the shape terms' weight (the continuation below raises it)
		auto residual = [&](const double prm[NP], std::vector<double> &e) {
			const M3 R = Rodrigues(V3(prm[0], prm[1], prm[2]));
			const V3 t(prm[3], prm[4], prm[5]);
			auto T = [&](V3 h) { return (R * h + t) * (1.0 / s); };   // -> gun file units
			e.clear();
			for (int k = 6; k < NP; k++) e.push_back((prm[k] < 0 ? prm[k] : (prm[k] > 1 ? prm[k] - 1 : 0)) * 10);
			// (a) the palm on the off-side panel: its centre in contact (1 mm), its normal into the panel (within 20 degrees),
			// near the panel's middle (15 mm give)
			{
				// on the gun, or on the shooting hand's fingers where they cover the panel (picture panel 3)
				// (the fields saturate at their reach, 17 mm: the true gap is reported against the panel's plane, x = +-55 units)
				const V3 pc = T(palmC);
				const double d = fmin(sdf.Query(pc), mainSdf.Query(pc)) * s;
				const double dPlane = (off * pc.x - 55.0) * s;
				e.push_back((d - 0.001) * 30 * kW);
				(void)dPlane;   // NOT a pull: pulled onto the panel the palm goes through the shooting fingers that wrap onto it (2026-11-23 run: 16 checks failed, 15 mm overlaps)
				const V3 n = R * palmN;
				const double c = -Dot(n, V3(off, 0, 0));    // 1 = the palm faces the panel (its normal toward the gun's centre)
				e.push_back(c < cos(20 * M_PI / 180) ? (cos(20 * M_PI / 180) - c) * 1.0 * kW : 0.0);
				const V3 a = pc * s - anchor; const double L = sqrt(a.y * a.y + a.z * a.z);
				e.push_back(L > 0.025 ? (L - 0.025) * 1.0 * kW : 0.0);
			}
			// the index's knuckle under the trigger guard at the grip's front corner (panel 4), 6 mm give
			{
				V3 c; for (int n : knuckleV) c = c + T(gridVert(idx[n], prm[kBase[1]], prm[kOut[1]])); c = c * (1.0 / knuckleV.size());
				const V3 d = (c - knuckleAt) * s; const double L = Len(d);
				const V3 dd = L > 0.006 ? d * ((L - 0.006) / L) : V3();
				e.push_back(dd.x * 10 * kW); e.push_back(dd.y * 10 * kW); e.push_back(dd.z * 10 * kW);
			}
			// the shooting hand's skin out of the support hand (the round's own fingers)
			if (haveLocal)
			{
				const M3 Rinv = Transpose(R);
				for (size_t k = 0; k < mainIdx.size(); k++)
				{
					if (!mainActive[k]) continue;
					const int i = mainIdx[k];
					const V3 hl = Rinv * (mainM.v[i].p * s - t) * (1.0 / s);
					const double d = supLocalSdf.Query(hl) * s;
					e.push_back(d < -kSlack ? (d + kSlack) * kPenW : 0.0);
				}
			}
			// (d) no skin in the gun or in the shooting hand; (b) the finger ends on the shooting hand; (c) the thumb
			double nearMain[5] = { 1e9, 1e9, 1e9, 1e9, 1e9 }, nearGunThumb = 1e9;
			V3 tipAt[5]; int tipN[5] = {};
			for (size_t n = 0; n < idx.size(); n++)
			{
				const int f = fin[n];
				const V3 hp = f >= 0 ? gridVert(idx[n], prm[kBase[f]], prm[kOut[f]]) : rest.v[idx[n]].p;
				const V3 gp = T(hp);
				const double dg = sdf.Query(gp) * s, dm = supActive[n] ? mainSdf.Query(gp) * s : 1.0;
				e.push_back(dg < -kSlack ? (dg + kSlack) * kPenW : 0.0);
				e.push_back(dm < -kSlack ? (dm + kSlack) * kPenW : 0.0);
				if (f >= 1 && seg[n] >= 2) { nearMain[f] = fmin(nearMain[f], f == 4 ? fmin(fabs(dm), fabs(dg)) : fabs(dm)); tipAt[f] = tipAt[f] + gp; tipN[f]++; }
				if (f == 0 && seg[n] >= 1) nearGunThumb = fmin(nearGunThumb, fmin(fabs(dg), fabs(dm)));   // on the frame, or on the shooting fingers there
			}
			for (int f = 1; f < 5; f++)
			{
				e.push_back(nearMain[f] > kSlack ? (nearMain[f] - kSlack) * 8 * kW : 0.0);
				// round the FRONT: the end segment in front of the strap (on the shooting fingers' backs, not beside the grip)
				const V3 c = tipN[f] ? tipAt[f] * (1.0 / tipN[f]) : V3();
				const double ahead = (c.y - strapY) * s;    // metres in front of the strap
				if (f < 4) e.push_back(ahead < 0.010 ? (0.010 - ahead) * 4 * kW : 0.0);   // the little finger may hang under the grip's foot
			}
			e.push_back(nearGunThumb > kSlack ? (nearGunThumb - kSlack) * 30 * kW : 0.0);
			{
				// (c) the thumb: on the OFF side (every middle / end point outside the frame's off-side face), its last segment
				// along the bore (within 20 degrees), forward, its end segment between the trigger guard's top and the shooting
				// hand's thumb (zLo..zHi: picture panel 6, the shooting thumb rests on the support thumb)
				V3 c1, c2; int n1 = 0, n2 = 0;
				double wrongSide = 0;
				for (size_t n = 0; n < idx.size(); n++)
				{
					if (fin[n] != 0 || seg[n] < 1) continue;
					const V3 gp = T(gridVert(idx[n], prm[kBase[0]], prm[kOut[0]]));
					if (seg[n] == 1) { c1 = c1 + gp; n1++; } else { c2 = c2 + gp; n2++; }
					const double xo = gp.x * off;   // outward on the off side
					if (xo < 45) wrongSide = fmax(wrongSide, 45 - xo);
				}
				e.push_back(wrongSide * s * 30 * kW);
				if (n1 && n2)
				{
					c1 = c1 * (1.0 / n1); c2 = c2 * (1.0 / n2);
					const double cy = Dot(Unit(c2 - c1), V3(0, 1, 0));
					e.push_back(cy < cos(20 * M_PI / 180) ? (cos(20 * M_PI / 180) - cy) * 1.0 * kW : 0.0);
					const double z = c2.z;
					e.push_back(z < zLo ? (zLo - z) * s * 60 * kW : (z > zHi ? (z - zHi) * s * 60 * kW : 0.0));
					e.push_back(c2.y < -120 ? (-120 - c2.y) * s * 60 * kW : (c2.y > 60 ? (c2.y - 60) * s * 60 * kW : 0.0));
				}
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
					const double hstep = k >= 6 ? 1e-3 : 1e-6;
					double q[NP]; memcpy(q, prm, sizeof(q)); q[k] += hstep;
					residual(q, e2);
					for (size_t i = 0; i < m; i++) Jm[i][k] = (e2[i] - e[i]) / hstep;
				}
				double A[NP][NP] = {}, bvec[NP] = {};
				for (size_t i = 0; i < m; i++) for (int a = 0; a < NP; a++) { if (Jm[i][a] == 0) continue; bvec[a] -= Jm[i][a] * e[i]; for (int b = 0; b < NP; b++) A[a][b] += Jm[i][a] * Jm[i][b]; }
				for (int a = 0; a < NP; a++) A[a][a] = A[a][a] * (1 + lambda) + 1e-12;
				double Mx[NP][NP + 1];
				for (int a = 0; a < NP; a++) { for (int b = 0; b < NP; b++) Mx[a][b] = A[a][b]; Mx[a][NP] = bvec[a]; }
				for (int c = 0; c < NP; c++)
				{
					int piv = c; for (int rr = c + 1; rr < NP; rr++) if (fabs(Mx[rr][c]) > fabs(Mx[piv][c])) piv = rr;
					for (int k = 0; k <= NP; k++) std::swap(Mx[c][k], Mx[piv][k]);
					for (int rr = 0; rr < NP; rr++) if (rr != c) { const double f = Mx[rr][c] / Mx[c][c]; for (int k = c; k <= NP; k++) Mx[rr][k] -= f * Mx[c][k]; }
				}
				double q[NP];
				for (int a = 0; a < NP; a++) q[a] = prm[a] + Mx[a][NP] / Mx[a][a];
				const double cq = cost(q), cp = cost(prm);
				if (cq < cp) { memcpy(prm, q, sizeof(q)); lambda *= 0.3; } else lambda *= 10;
				if (lambda > 1e8) break;
			}
			return cost(prm);
		};
		// the points that can matter this round: support points within 30 mm of the shooting hand, shooting-hand points
		// within 30 mm of the support hand (the rest are tested again at the next round's refilter and in the checks)
		auto refilter = [&](const double *prm) {
			const M3 R = Rodrigues(V3(prm[0], prm[1], prm[2]));
			const V3 t(prm[3], prm[4], prm[5]);
			const M3 Rinv = Transpose(R);
			int a = 0, b = 0;
			for (size_t n = 0; n < idx.size(); n++)
			{
				const int f = fin[n];
				const V3 hp = f >= 0 ? gridVert(idx[n], prm[kBase[f]], prm[kOut[f]]) : rest.v[idx[n]].p;
				const V3 gp = (R * hp + t) * (1.0 / s);
				supActive[n] = fabs(mainSdf.Query(gp)) * s < 0.030; a += supActive[n];
			}
			for (size_t k = 0; k < mainIdx.size(); k++)
			{
				const V3 hl = Rinv * (mainM.v[mainIdx[k]].p * s - t) * (1.0 / s);
				mainActive[k] = fabs(supLocalSdf.Query(hl)) * s < 0.030; b += mainActive[k];
			}
			return a + b;
		};
		// THE STARTS (picture panels 3 and 6; the authored frame's shape: the fingers hooked about 90 degrees at the knuckles,
		// the thumb straight along the hand): the hand's X (wrist -> knuckles) along the bore, tipped -10..+30 degrees
		// (down positive), its back outward, the knuckle line standing at the grip's off-side front corner, the hooked fingers
		// across the front strap; its palm's centre on the off-side panel; turned about the panel's normal -10, 0, +10; every
		// finger as authored (1, 1).
		double best[NP]; double bestCost = 1e30;
		const double startW = getenv("SUP_START_W") ? atof(getenv("SUP_START_W")) : 1.0;
		{ double w10[10]; for (double &x : w10) x = startW; buildLocal(w10); }   // every start's fingers: the converse counts from the first start
		for (int ai = -1; ai < 4; ai++)
			for (int ti = -1; ti <= 1; ti++)
				for (int zi = -2; zi <= 1; zi++)
				{
					const double a = (ai * 10) * M_PI / 180;
					const V3 Xg(0, cos(a), -sin(a)), Yg(off, 0, 0), Zg = Cross(Xg, Yg);
					M3 R0 = FromCols(Xg, Yg, Zg);
					R0 = Mul(Rodrigues(V3(off, 0, 0) * (ti * 10.0 * M_PI / 180)), R0);
					const V3 w = RotVec(R0);
					const V3 t0 = anchor + V3(off * 0.004, -0.010 + zi * 0.0, zi * 0.012) - R0 * palmC;
					double q[NP] = { w.x, w.y, w.z, t0.x, t0.y, t0.z, startW, startW, startW, startW, startW, startW, startW, startW, startW, startW };
					kPenW = 20.0; kW = 0.3;   // judged as the continuation begins: apart first
					refilter(q);
					const double c = solve(q, 25);
					if (c < bestCost) { bestCost = c; memcpy(best, q, sizeof(q)); }
				}
		printf("  multi-start: best cost %.4g\n", bestCost);
		auto runContinuation = [&](double prm[NP]) {
		// THE CONTINUATION: first apart (the shape terms weak, the penetration strong: the hands separate), then the shape
		// terms raised step by step with the penetration kept strong, the support hand's own field rebuilt each step -- so the
		// hand slides INTO contact rather than starting inside the other one (a start inside it stays there: 2026-11-23 runs)
		const double shapeW[8] = { 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0 };
		kPenW = 40.0;
		double cFin = 0;
		for (int round = 0; round < 8; round++)
		{
			buildLocal(prm + 6);
			refilter(prm);
			kW = shapeW[round];
			cFin = solve(prm, 40);
			printf("  step %d (shape weight %.1f, penetration weight %.0f): cost %.4g\n", round, kW, kPenW, cFin);
		}
		return cFin;
		};
		double prm[NP]; memcpy(prm, best, sizeof(prm));
		double cFin = runContinuation(prm);
		if (gHavePrev)
		{
			// THE OTHER PAIRING'S FIT, MIRRORED, as a second start; the lower final cost wins
			const M3 Sg = FromCols({ -1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }), Sh = FromCols({ 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, -1 });
			const V3 w = RotVec(Mul(Mul(Sg, gPrevR), Sh));
			double q[NP]; memcpy(q, gPrevPrm, sizeof(q));
			q[0] = w.x; q[1] = w.y; q[2] = w.z; q[3] = -gPrevPrm[3];
			const double cMir = runContinuation(q);
			printf("  final cost from this pairing's own starts %.4g; from the other pairing's fit, mirrored, %.4g\n", cFin, cMir);
			if (cMir < cFin) { memcpy(prm, q, sizeof(q)); cFin = cMir; }
		}
		printf("  final cost %.4g\n", cFin);
		auto cl01 = [](double u) { return u < 0 ? 0.0 : (u > 1 ? 1.0 : u); };
		double wOut[5], wBase[5];
		for (int f = 0; f < 5; f++) { wOut[f] = cl01(prm[kOut[f]]); wBase[f] = cl01(prm[kBase[f]]); }

		// ---- THE CHECKS ON THE EXACT POSE (the hand posed again through the engine path with the fitted amounts) ----
		HeldState hfit = sh;
		for (int f = 0; f < 5; f++) { hfit.wrap[f] = (float)wOut[f]; hfit.wrapBase[f] = (float)wBase[f]; }
		Mesh supM = S.PoseMesh(hfit, supIn, nullptr, &mh);
		const M3 R = Rodrigues(V3(prm[0], prm[1], prm[2]));
		const V3 t(prm[3], prm[4], prm[5]);
		gPrevR = R; memcpy(gPrevPrm, prm, sizeof(gPrevPrm)); gHavePrev = true;
		for (MeshV &v : supM.v) { v.p = (R * v.p + t) * (1.0 / s); v.n = Unit(R * v.n); }
		GunSDF supSdf; supSdf.kReach = 64.0; supSdf.kGrid = 4.0; supSdf.Build(supM); supSdf.Fill();
		double deepGun = 0, deepMain = 0, deepMainInSup = 0; int nGun = 0, nMain = 0, nMainInSup = 0;
		std::map<std::string, int> inGunBy, inMainBy, mainInBy;
		std::vector<char> seen(S.r.verts.size() + 1, 0);
		double nearMain[5] = { 1e9, 1e9, 1e9, 1e9, 1e9 }, thumbGun = 1e9, thumbAny = 1e9;
		V3 tipC[5]; int tipN[5] = {};
		V3 c1, c2; int n1 = 0, n2 = 0;
		for (size_t i = 0; i < supM.v.size(); i++)
		{
			const uint32_t cp = S.r.verts[i].controlPoint;
			if (cp < seen.size()) { if (seen[cp]) continue; seen[cp] = 1; }
			const MeshV &v = supM.v[i];
			if (v.bone == S.hand && rest.v[i].p.x < -0.04) continue;   // the forearm stub
			const double dg = sdf.Query(v.p) * s, dm = mainSdf.Query(v.p) * s;
			if (dg < -0.002) { nGun++; inGunBy[S.r.joints[v.bone].name]++; }
			if (dm < -0.002) { nMain++; inMainBy[S.r.joints[v.bone].name]++; }
			deepGun = fmin(deepGun, dg); deepMain = fmin(deepMain, dm);
			const int f = S.FingerOf(v.bone), sg = S.SegOf(v.bone);
			if (f >= 1 && sg >= 2) { nearMain[f] = fmin(nearMain[f], f == 4 ? fmin(fabs(dm), fabs(dg)) : fabs(dm)); tipC[f] = tipC[f] + v.p; tipN[f]++; }
			if (f == 0 && sg >= 1) { thumbGun = fmin(thumbGun, fabs(dg)); thumbAny = fmin(thumbAny, fmin(fabs(dg), fabs(dm))); }
			if (f == 0 && sg == 1) { c1 = c1 + v.p; n1++; }
			if (f == 0 && sg == 2) { c2 = c2 + v.p; n2++; }
		}
		{
			std::vector<char> seenM(M.r.verts.size() + 1, 0);
			for (size_t i = 0; i < mainM.v.size(); i++)
			{
				const uint32_t cp = M.r.verts[i].controlPoint;
				if (cp < seenM.size()) { if (seenM[cp]) continue; seenM[cp] = 1; }
				const double d = supSdf.Query(mainM.v[i].p) * s;
				if (d < -0.002) { nMainInSup++; mainInBy[M.r.joints[mainM.v[i].bone].name]++; if (getenv("SUP_DEBUG") && nMainInSup < 6) { const double ex = SignedDistance(supM, mainM.v[i].p, 60) * s; printf("    shooting-hand point %zu (%s) at (%.1f %.1f %.1f): grid %.1f mm, exact %.1f mm\n", i, M.r.joints[mainM.v[i].bone].name.c_str(), mainM.v[i].p.x, mainM.v[i].p.y, mainM.v[i].p.z, d * 1000, ex * 1000); } }
				deepMainInSup = fmin(deepMainInSup, d);
			}
		}
		c1 = c1 * (1.0 / (n1 ? n1 : 1)); c2 = c2 * (1.0 / (n2 ? n2 : 1));
		const V3 pc = (R * palmC + t) * (1.0 / s);
		const double palmGun = sdf.Query(pc) * s, palmHand = mainSdf.Query(pc) * s, palmD = fmin(palmGun, palmHand), palmFace = -Dot(R * palmN, V3(off, 0, 0));
		const double thumbAng = acos(fmax(-1.0, fmin(1.0, Dot(Unit(c2 - c1), V3(0, 1, 0))))) * 180 / M_PI;
		printf("  fingers (knuckle/outer, open -> fbx_twohand_support): thumb %.3f/%.3f index %.3f/%.3f middle %.3f/%.3f ring %.3f/%.3f little %.3f/%.3f\n",
			wBase[0], wOut[0], wBase[1], wOut[1], wBase[2], wOut[2], wBase[3], wOut[3], wBase[4], wOut[4]);
		printf("  the palm's centre %.1f mm out from the panel's plane\n", (off * pc.x - 55.0) * s * 1000);
		printf("  the palm's centre at (%.1f %.1f %.1f) gun units, %.1f mm from the gun, %.1f mm from the shooting hand (both read to 17.4 at most), facing the panel cos %.3f (%.1f deg)\n", pc.x, pc.y, pc.z, palmGun * 1000, palmHand * 1000, palmFace, acos(fmin(1.0, palmFace)) * 180 / M_PI);
		for (int f = 1; f < 5; f++)
		{
			const V3 c = tipC[f] * (1.0 / (tipN[f] ? tipN[f] : 1));
			printf("  %s end segment: %.1f mm from the shooting hand's skin; its centre (%.1f %.1f %.1f): %.1f mm in front of the front strap, x %.1f (the off side %s)\n",
				kFingerJointNames[f][0], nearMain[f] * 1000, c.x, c.y, c.z, (c.y - strapY) * s * 1000, c.x, off < 0 ? "-x" : "+x");
		}
		printf("  the thumb: its last segment %.1f deg from the bore, its middle and end %.1f mm from the gun (%.1f from the gun or the shooting hand); its end segment's centre (%.1f %.1f %.1f) (the guard's underside z %.1f, the slide's bottom 103.6)\n",
			thumbAng, thumbGun * 1000, thumbAny * 1000, c2.x, c2.y, c2.z, guardZ);
		printf("  support skin more than 2 mm in the gun: %d (deepest %.1f mm); in the shooting hand: %d (deepest %.1f mm); the shooting hand's skin more than 2 mm in it: %d (deepest %.1f mm)\n",
			nGun, -deepGun * 1000, nMain, -deepMain * 1000, nMainInSup, -deepMainInSup * 1000);
		printf("  in the gun by joint:"); for (auto &kv : inGunBy) printf(" %s %d", kv.first.c_str(), kv.second); printf("\n");
		printf("  in the shooting hand by joint:"); for (auto &kv : inMainBy) printf(" %s %d", kv.first.c_str(), kv.second); printf("\n");
		printf("  the shooting hand's skin in the support hand, by joint:"); for (auto &kv : mainInBy) printf(" %s %d", kv.first.c_str(), kv.second); printf("\n");
		char line[256];
		const char *tag = mainRight ? "right shoots" : "left shoots";
		snprintf(line, sizeof line, "%s: the support palm's centre on the off-side panel or the shooting fingers over it within 4 mm (%.1f; %.1f out from the panel's plane), facing it within 25 deg (%.1f)", tag, palmD * 1000, (off * pc.x - 55.0) * s * 1000, acos(fmin(1.0, palmFace)) * 180 / M_PI);
		Check(fabs(palmD) < 0.004 && palmFace > cos(25 * M_PI / 180), line);
		for (int f = 1; f < 4; f++)
		{
			const V3 c = tipC[f] * (1.0 / (tipN[f] ? tipN[f] : 1));
			snprintf(line, sizeof line, "%s: the support %s's end on the shooting hand within 3 mm (%.1f), in front of the strap (%.1f mm)", tag, kFingerJointNames[f][0], nearMain[f] * 1000, (c.y - strapY) * s * 1000);
			Check(nearMain[f] < 0.003 && c.y > strapY, line);
		}
		snprintf(line, sizeof line, "%s: the support little finger's end on the shooting hand or the grip within 3 mm (%.1f)", tag, nearMain[4] * 1000);
		Check(nearMain[4] < 0.003, line);
		snprintf(line, sizeof line, "%s: the support thumb along the bore within 25 deg (%.1f), on the frame or the shooting fingers there within 3 mm (%.1f), between the guard and the shooting thumb (z %.1f in %.1f..%.1f)", tag, thumbAng, thumbAny * 1000, c2.z, zLo, zHi);
		Check(thumbAng < 25 && thumbAny < 0.003 && c2.z > zLo - 3 && c2.z < zHi + 3, line);
		snprintf(line, sizeof line, "%s: no support skin more than 2 mm in the gun (%d)", tag, nGun); Check(nGun == 0, line, true);
		snprintf(line, sizeof line, "%s: no skin of either hand more than 2 mm in the other (%d support, %d shooting)", tag, nMain, nMainInSup); Check(nMain == 0 && nMainInSup == 0, line, true);

		// ---- THE SEAT FOR THE ENGINE: the support hand frame -> the shooting hand frame, metres ----
		// shooting hand metres = Rm (gun metres) + tm (colt_fit: h = R (s g) + t); gun metres = R h_s + t
		const M3 Rrel = Mul(mf.R, R);
		const V3 trel = mf.R * t + mf.t;
		fprintf(res, "support_%s %.9g %.9g %.9g 0 %.9g %.9g %.9g 0 %.9g %.9g %.9g 0 %.9g %.9g %.9g 1\n", mainRight ? "rightshoots" : "leftshoots",
			Rrel.m[0][0], Rrel.m[1][0], Rrel.m[2][0], Rrel.m[0][1], Rrel.m[1][1], Rrel.m[2][1], Rrel.m[0][2], Rrel.m[1][2], Rrel.m[2][2], trel.x, trel.y, trel.z);
		fprintf(res, "fingers_%s thumb %.4f %.4f index %.4f %.4f middle %.4f %.4f ring %.4f %.4f little %.4f %.4f (knuckle, outer)\n", mainRight ? "rightshoots" : "leftshoots",
			wBase[0], wOut[0], wBase[1], wOut[1], wBase[2], wOut[2], wBase[3], wOut[3], wBase[4], wOut[4]);
		fprintf(res, "check_%s palm_plane_mm %.2f palm_mm %.2f palm_deg %.1f ends_mm %.2f %.2f %.2f %.2f thumb_deg %.1f thumb_mm %.2f in_gun %d in_main %d main_in %d\n", mainRight ? "rightshoots" : "leftshoots",
			(off * pc.x - 55.0) * s * 1000, palmD * 1000, acos(fmin(1.0, palmFace)) * 180 / M_PI, nearMain[1] * 1000, nearMain[2] * 1000, nearMain[3] * 1000, nearMain[4] * 1000, thumbAng, thumbGun * 1000, nGun, nMain, nMainInSup);

		// dumps (gun file units -> metres for rend.py's scale): the gun, the shooting hand, the support hand
		auto toM = [&](Mesh m) { for (MeshV &v : m.v) v.p = v.p * s; return m; };
		const std::string pre = outDir + "/sup" + std::to_string(pair);
		Dump((pre + "_gun.txt").c_str(), toM(gun));
		Dump((pre + "_main.txt").c_str(), toM(mainM));
		Dump((pre + "_sup.txt").c_str(), toM(supM));
		FILE *pt = fopen((pre + "_pts.txt").c_str(), "w");
		fprintf(pt, "palm %.6f %.6f %.6f yellow\nthumbM %.6f %.6f %.6f cyan\n", pc.x * s, pc.y * s, pc.z * s, mThumbTip.x * s, mThumbTip.y * s, mThumbTip.z * s);
		fclose(pt);
	}
	fclose(res);
	printf("\n%s: %d hard check(s) failed; %d shape check(s) not met (written to support_fit.txt)\n", gFails ? "FAIL" : "PASS", gFails, gNotMet);
	return gFails ? 1 : 0;
}
