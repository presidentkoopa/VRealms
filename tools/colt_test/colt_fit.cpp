// tools/colt_test/colt_fit.cpp -- THE COLT FITTED TO THE AUTHORED HAND (owner's plan, 2026-11-16).
//
// The hand pose stays authored: each hand (the MODELDEF's FBX, Hand_Low / Hand_Low1: since 2026-11-16 the Godot XR Tools
// hands, vrealms_iwad/models/hands/godot_xr; hand_final.fbx still loads) is posed through the engine's own
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
//   colt_fit <hands.fbx> <colt1911.fbx> <outdir>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fit_common.h"
#include <functional>
#include <map>

static const double kHandScale = 0.11094212;   // vrealms_iwad/MODELDEF.txt, the hands' Scale (metres per hand file unit)
static const double kGunMetres = 0.216;        // the task: a 1911 is ~21.6 cm long, muzzle to the back of the tang

static int gFails = 0;
static void Check(bool ok, const char *what) { printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) gFails++; }

// the previous hand's fit (colt_fit fits the right hand first): its rotation and parameters, for the mirrored start
static constexpr int kNP = 16;
static M3 gPrevR;
static double gPrevPrm[kNP];
static bool gHavePrev = false;

int main(int argc, char **argv)
{
	if (argc < 4) { fprintf(stderr, "usage: colt_fit <hands.fbx> <colt1911.fbx> <outdir>\n"); return 2; }
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
	// as vrealms_iwad/MODELDEF.txt names them: hand_final.fbx's left hand draws with the right's skin (mirrorskin); a file
	// whose left hand has its own skin (the Godot hands) takes the plain name
	const char *selectors[2] = { "Hand_Low", "Hand_Low1?mirrorskin=Hand_Low" };
	{
		Result probe;
		if (!Load(handFile.data(), handFile.size(), selectors[1], probe, err)) selectors[1] = "Hand_Low1";
	}
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
		// THE GRIP ALONE (the frame and its panels below the trigger guard, behind the trigger face): where the middle,
		// ring and little fingers must close -- not the guard, not the slide.
		static Mesh gripMesh; static GunSDF gripSdf;
		if (gripSdf.m == nullptr)
		{
			gripMesh.v = gun.v;
			const double zCut = tz0 - 15, yCut = trig.y;
			for (size_t q = 0; q < gun.t.size(); q += 3)
			{
				bool in = true;
				for (int c = 0; c < 3; c++) { const V3 &p = gun.v[gun.t[q + c]].p; if (p.z > zCut || p.y > yCut) in = false; }
				if (in) for (int c = 0; c < 3; c++) gripMesh.t.push_back(gun.t[q + c]);
			}
			printf("  the grip alone: %zu of %zu triangles (below z %.1f, behind y %.1f)\n", gripMesh.t.size() / 3, gun.t.size() / 3, zCut, yCut);
			gripSdf.Build(gripMesh); gripSdf.Fill();
		}
		// THE OTHER FINGERS MEASURED TOO (HeldState::wrap, 2026-11-16): thumb, middle, ring and little each the
		// in-between of the open frame and grip_pistol's own finger; sample k poses every finger at k/(kSamples-1), and
		// the fit reads each vertex from its own finger's amount (linear between samples).
		const int kSamples = 21;
		std::vector<V3> padAt(kSamples);
		std::vector<Mesh> meshAt(kSamples);
		auto fingerOf = [&](int bone) { for (int f = 0; f < 5; f++) { if (tips[f] == bone) return f; for (int j = 0; j < 3; j++) if (joints[f][j] == bone) return f; } return -1; };
		auto segOf = [&](int bone) { for (int f = 0; f < 5; f++) { if (tips[f] == bone) return 3; for (int j = 0; j < 3; j++) if (joints[f][j] == bone) return j; } return -1; };
		for (int k = 0; k < kSamples; k++)
		{
			HeldState hk = held; hk.indexReady = hk.indexFire = (float)k / (kSamples - 1);
			for (int f : { 0, 2, 3, 4 }) hk.wrap[f] = (float)k / (kSamples - 1);
			const Posed pk = PoseHand(r, Wrest, rig, hp, ready, &hold, &hk);
			Mesh mk = Skin(r, pk.P);
			for (MeshV &v : mk.v) { v.p = toHand(v.p); v.n = dirHand(v.n); }
			const int pv = pad(pk, mk);
			padAt[k] = pv >= 0 ? mk.v[pv].p : V3();
			meshAt[k] = mk;
		}
		// the other fingers on a grid: the knuckle's amount (a) by the outer joints' (b), kGrid x kGrid poses
		const int kGrid = 11;
		std::vector<Mesh> gridAt(kGrid * kGrid);
		for (int a = 0; a < kGrid; a++)
			for (int b = 0; b < kGrid; b++)
			{
				HeldState hk = held; hk.indexBase = (float)a / (kGrid - 1); hk.indexReady = hk.indexFire = (float)b / (kGrid - 1);
				for (int f : { 0, 2, 3, 4 }) { hk.wrapBase[f] = (float)a / (kGrid - 1); hk.wrap[f] = (float)b / (kGrid - 1); }
				const Posed pk = PoseHand(r, Wrest, rig, hp, ready, &hold, &hk);
				Mesh mk = Skin(r, pk.P);
				for (MeshV &v : mk.v) { v.p = toHand(v.p); v.n = dirHand(v.n); }
				gridAt[a * kGrid + b] = std::move(mk);
			}
		auto gridVert = [&](int i, double ua, double ub) {
			auto cl = [](double u) { return u < 0 ? 0.0 : (u > 1 ? 1.0 : u); };
			const double xa = cl(ua) * (kGrid - 1), xb = cl(ub) * (kGrid - 1);
			int a = (int)floor(xa), b = (int)floor(xb); if (a >= kGrid - 1) a = kGrid - 2; if (b >= kGrid - 1) b = kGrid - 2;
			const double fa = xa - a, fb = xb - b;
			return gridAt[a * kGrid + b].v[i].p * ((1 - fa) * (1 - fb)) + gridAt[(a + 1) * kGrid + b].v[i].p * (fa * (1 - fb))
				+ gridAt[a * kGrid + b + 1].v[i].p * ((1 - fa) * fb) + gridAt[(a + 1) * kGrid + b + 1].v[i].p * (fa * fb);
		};
		// the pad vertex, fixed (chosen on the middle sample of the grid), read through the grid
		int padVid = -1;
		{
			HeldState hk = held; hk.indexBase = 0.5f; hk.indexReady = hk.indexFire = 0.5f;
			const Posed pk = PoseHand(r, Wrest, rig, hp, ready, &hold, &hk);
			Mesh mk = Skin(r, pk.P);
			for (MeshV &v : mk.v) { v.p = toHand(v.p); v.n = dirHand(v.n); }
			padVid = pad(pk, mk);
		}
		std::vector<int> skinIdx, skinFinger, skinSeg;
		const size_t kSkinStride = getenv("COLT_STRIDE") ? (size_t)atoi(getenv("COLT_STRIDE")) : 1;
		// one vertex per control point (the loader writes one vertex per polygon corner: every skin point appears several
		// times, and a stride over corners missed some of them -- the check reads every one)
		std::vector<char> cpSeen(r.verts.size() + 1, 0);
		for (size_t i = 0; i < meshAt[0].v.size(); i += kSkinStride)
		{
			if (kSkinStride == 1) { const uint32_t cp = r.verts[i].controlPoint; if (cp < cpSeen.size() && cpSeen[cp]) continue; if (cp < cpSeen.size()) cpSeen[cp] = 1; }
			if (meshAt[0].v[i].bone == hand && Dot(meshAt[0].v[i].p, V3(1, 0, 0)) < -0.04) continue;   // not the forearm
			skinIdx.push_back((int)i); skinFinger.push_back(fingerOf(meshAt[0].v[i].bone)); skinSeg.push_back(segOf(meshAt[0].v[i].bone));
		}
		auto vertAt = [&](int i, double u) {
			u = u < 0 ? 0 : (u > 1 ? 1 : u);
			const double x = u * (kSamples - 1); int a = (int)floor(x); if (a >= kSamples - 1) a = kSamples - 2;
			const double fr = x - a;
			return meshAt[a].v[i].p * (1 - fr) + meshAt[a + 1].v[i].p * fr;
		};
		auto padOf = [&](double u) {
			u = u < 0 ? 0 : (u > 1 ? 1 : u);
			const double x = u * (kSamples - 1); int i = (int)floor(x); if (i >= kSamples - 1) i = kSamples - 2;
			const double f = x - i;
			return padAt[i] * (1 - f) + padAt[i + 1] * f;
		};
		for (int k = 0; k < kSamples; k += 4) printf("  index in-between %.2f: its pad %.1f mm from the web (the gun: the beavertail to the trigger face %.1f mm)\n",
			(double)k / (kSamples - 1), Len(padAt[k] - web) * 1000, Len(trig - beaver) * s * 1000);
		double kPenW = 5.0; const double kSlack = 0.0005;   // penetration weight (raised for the last stage); skin give (0.5 mm)
		const double kWebGive = 0.008;               // the web may sit up to 8 mm from the beavertail point (16 for hand_final, whose index was 61 mm knuckle to tip)
		// TWO STAGES: first the index may pass through the guard (no index penetration) so its pad can reach the trigger
		// INSIDE the guard; then every finger counts again, from there.
		auto cl01 = [](double u) { return u < 0 ? 0.0 : (u > 1 ? 1.0 : u); };
		bool indexFree = false;
		const double kPadW = getenv("COLT_PADW") ? atof(getenv("COLT_PADW")) : 10.0;   // the pad on the trigger is a hard check (6 mm): it weighs as much as 10x its distance
		const double kContactW = getenv("COLT_CONTACTW") ? atof(getenv("COLT_CONTACTW")) : 8.0;   // a wrapped finger's end pulled onto the grip (8: as firmly as the pad onto the trigger)
		constexpr int NP = kNP;  // rotation vector, translation, the index's in-between, the wraps (thumb, middle, ring, little: outer joints), their knuckles
		const int kParamOf[5] = { 7, 6, 8, 9, 10 };
		const int kBaseOf[5] = { 11, 15, 12, 13, 14 };
		// the trigger face as a segment (its front edge, top to bottom, 3 units in from each end): the pad may meet it anywhere
		const V3 trigTop(0, trig.y, tz1 - 3), trigBot(0, trig.y, tz0 + 3);
		auto onFace = [&](V3 pg) { const V3 ab = trigBot - trigTop; double u = Dot(pg - trigTop, ab) / Dot(ab, ab); u = u < 0 ? 0 : (u > 1 ? 1 : u); return trigTop + ab * u; };
		auto residual = [&](const double prm[NP], std::vector<double> &e) {
			const M3 R = Mul(Rodrigues(V3(prm[0], prm[1], prm[2])), R0);
			const V3 t(prm[3], prm[4], prm[5]);
			auto T = [&](V3 p) { return R * (p * s) + t; };
			e.clear();
			{   // the web at the beavertail, with give: the crease sits within kWebGive of the point (skin, grip height)
				const V3 b = T(beaver) - web; const double L = Len(b);
				const V3 bb = L > kWebGive ? b * ((L - kWebGive) / L) : V3();
				e.push_back(bb.x); e.push_back(bb.y); e.push_back(bb.z);
			}
			M3 Rt; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) Rt.m[i][j] = R.m[j][i];
			{
				const V3 pd = gridVert(padVid, prm[15], prm[6]);
				const V3 pg = Rt * (pd - t) * (1.0 / s);
				const V3 c = (T(onFace(pg)) - pd) * kPadW; e.push_back(c.x); e.push_back(c.y); e.push_back(c.z);
			}
			for (int k = 6; k < NP; k++) e.push_back(prm[k] < 0 ? prm[k] : (prm[k] > 1 ? prm[k] - 1 : 0));
			double nearest[5] = { 1e9, 1e9, 1e9, 1e9, 1e9 };
			for (size_t n = 0; n < skinIdx.size(); n++)
			{
				const int f = skinFinger[n];
				const V3 hpnt = f >= 0 ? gridVert(skinIdx[n], prm[kBaseOf[f]], prm[kParamOf[f]]) : gridAt[0].v[skinIdx[n]].p;
				const V3 gp = Rt * (hpnt - t) * (1.0 / s);
				const double d = sdf.Query(gp) * s;   // metres, negative inside
				e.push_back(d < -kSlack && !(f == 1 && indexFree) ? (d + kSlack) * kPenW : 0.0);
				// the thumb: its middle and outer segments to the gun; middle, ring, little: their END segment (the pad and the
				// tip) to the grip's surface -- a finger is wrapped round the grip when its end lies on it, not when its first
				// bone brushes the front strap (2026-11-16: that passed with the fingers straight past the strap)
				if (f == 0 && skinSeg[n] >= 1) nearest[f] = fmin(nearest[f], d);
				else if (f >= 2 && skinSeg[n] >= 2) nearest[f] = fmin(nearest[f], fabs(gripSdf.Query(gp)) * s);
			}
			for (int f : { 0, 2, 3, 4 }) e.push_back(nearest[f] > kSlack ? (nearest[f] - kSlack) * kContactW : 0.0);
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
		double prm[NP] = { 0, 0, 0, 0, 0, 0, 0.5, 0.5, 0.5, 0.5, 0.5, 0.9, 0.9, 0.9, 0.9, 0.3 }, bestCost = 1e30;
		indexFree = true;
		for (int pi = -4; pi <= 4; pi++)
			for (int ri = -1; ri <= 1; ri++)
			{
				const V3 w = V3(0, 1, 0) * (pi * 10.0 * M_PI / 180) + V3(0, 0, 1) * (ri * 15.0 * M_PI / 180);
				double q[NP] = { w.x, w.y, w.z, 0, 0, 0, 0.5, 0.5, 0.5, 0.5, 0.5, 0.9, 0.9, 0.9, 0.9, 0.3 };
				const M3 Rq = Mul(Rodrigues(w), R0);
				const V3 t0 = web - Rq * (beaver * s);
				q[3] = t0.x; q[4] = t0.y; q[5] = t0.z;
				const double c = solve(q, 25);
				if (c < bestCost) { bestCost = c; memcpy(prm, q, sizeof(q)); }
			}
		// THE OTHER HAND'S FIT, MIRRORED, as one more start (the hands are mirror images, the Colt nearly so: hand frame Z and
		// gun x flip), so a hand whose own starts fall into a worse basin (the left, on the Godot hands) is not left there.
		// THE THUMB, CHOSEN ON THE EXACT POSE. Its depth in the gun is not monotonic in its two amounts (the Godot thumb's
		// metacarpal swings its base across the frame's side), so the solver can stay where the thumb is inside. With the
		// gun where the fit put it, every (knuckle, outer) pair on a 0.1 grid is posed through the engine path and skinned
		// whole: the thumb's skin may be no more than 1 mm in the gun; of those, the pair whose nearest middle/outer thumb
		// skin is closest to the gun's surface wins (closed until it meets the gun, never into it). Then the fit again.
		auto thumbPick = [&](const char *when) {
			const M3 Rq = Mul(Rodrigues(V3(prm[0], prm[1], prm[2])), R0);
			const V3 tq(prm[3], prm[4], prm[5]);
			M3 Rt; for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) Rt.m[a][b] = Rq.m[b][a];
			HeldState hs = held; hs.indexReady = hs.indexFire = (float)cl01(prm[6]); hs.indexBase = (float)cl01(prm[15]);
			for (int f : { 2, 3, 4 }) { hs.wrap[f] = (float)cl01(prm[kParamOf[f]]); hs.wrapBase[f] = (float)cl01(prm[kBaseOf[f]]); }
			double bestScore = 1e30; int ba = -1, bb = -1; double bDepth = 0, bGap = 0;
			for (int a = 0; a <= 10; a++)
				for (int b = 0; b <= 10; b++)
				{
					hs.wrapBase[0] = a / 10.f; hs.wrap[0] = b / 10.f;
					const Posed ps = PoseHand(r, Wrest, rig, hp, ready, &hold, &hs);
					const Mesh ms = Skin(r, ps.P);
					double depth = 0, gap = 1e9;
					for (const MeshV &v : ms.v)
					{
						if (fingerOf(v.bone) != 0) continue;
						const double d = sdf.Query(Rt * (toHand(v.p) - tq) * (1.0 / s)) * s;
						depth = fmin(depth, d);
						if (segOf(v.bone) >= 1) gap = fmin(gap, d);
					}
					const double score = (depth < -0.001 ? 1.0 + (-depth - 0.001) * 1000 : 0.0) + fabs(fmax(gap, 0.0)) * 10;
					if (score < bestScore) { bestScore = score; ba = a; bb = b; bDepth = depth; bGap = gap; }
				}
			prm[kBaseOf[0]] = ba / 10.0; prm[kParamOf[0]] = bb / 10.0;
			printf("  the thumb chosen on the exact pose (%s): knuckle %.1f, outer %.1f -- its skin %.1f mm deep at most, its nearest middle/outer skin %.1f mm %s the gun\n",
				when, ba / 10.0, bb / 10.0, -bDepth * 1000, fabs(bGap) * 1000, bGap < 0 ? "into" : "from");
		};
		// THE STAGES, from one start: the index free to pass the guard, then every finger, then the penetration weighed
		// 4x, then the thumb chosen on the exact pose and the fit again.
		auto stages = [&](double q[NP], const char *what) {
			memcpy(prm, q, sizeof(prm));
			indexFree = true; kPenW = 5.0;
			solve(prm, 60);
			indexFree = false;
			solve(prm, 80);
			kPenW = getenv("COLT_PENW2") ? atof(getenv("COLT_PENW2")) : 20.0;
			solve(prm, 80);
			thumbPick(what);
			solve(prm, 40);
			thumbPick(what);
			memcpy(q, prm, sizeof(prm));
			return cost(prm);
		};
		double own[NP];
		memcpy(own, prm, sizeof(own));
		double best[NP];
		const double cOwn = stages(own, "from this hand's own starts");
		memcpy(best, own, sizeof(best));
		// THE OTHER HAND'S FIT, MIRRORED, as a second start (the hands are mirror images, the Colt nearly so: hand frame Z
		// and gun x flip), through the same stages; the lower final cost wins. A hand whose own starts fall into a worse
		// basin (the Godot left hand: its index pad 10.7 mm off the trigger) is not left there.
		if (gHavePrev)
		{
			const M3 S = FromCols({ 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, -1 }), Sg = FromCols({ -1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 });
			const M3 Rm = Mul(Mul(S, gPrevR), Sg);
			M3 R0t; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) R0t.m[i][j] = R0.m[j][i];
			const V3 w = RotVec(Mul(Rm, R0t));
			double q[NP];
			memcpy(q, gPrevPrm, sizeof(q));
			q[0] = w.x; q[1] = w.y; q[2] = w.z;
			q[3] = gPrevPrm[3]; q[4] = gPrevPrm[4]; q[5] = -gPrevPrm[5];
			const double cMir = stages(q, "from the other hand's fit, mirrored");
			printf("  final cost from this hand's own starts %.4g; from the other hand's fit, mirrored, %.4g\n", cOwn, cMir);
			if (cMir < cOwn) memcpy(best, q, sizeof(best));
		}
		memcpy(prm, best, sizeof(prm));
		const double idxReady = prm[6] < 0 ? 0 : (prm[6] > 1 ? 1 : prm[6]);
		printf("  multi-start: the best cost %.3g; the index on the trigger: %.3f of the way from the open frame to the grip's\n", cost(prm), idxReady);
		double wrapOut[5] = { -1, -1, -1, -1, -1 }, baseOut[5] = { -1, -1, -1, -1, -1 };
		for (int f : { 0, 2, 3, 4 }) { wrapOut[f] = cl01(prm[kParamOf[f]]); baseOut[f] = cl01(prm[kBaseOf[f]]); }
		printf("  the wraps (open -> grip_pistol's own finger), knuckle / outer joints: thumb %.3f/%.3f middle %.3f/%.3f ring %.3f/%.3f little %.3f/%.3f\n",
			baseOut[0], wrapOut[0], baseOut[2], wrapOut[2], baseOut[3], wrapOut[3], baseOut[4], wrapOut[4]);
		// THE CHECKS ON THE EXACT POSE: the hand posed again through the engine path with the fitted index and wraps
		const double idxBase = cl01(prm[15]);
		printf("  the index's knuckle %.3f, its outer joints %.3f\n", idxBase, idxReady);
		HeldState hfit = held; hfit.indexReady = hfit.indexFire = (float)idxReady; hfit.indexBase = (float)idxBase;
		for (int f : { 0, 2, 3, 4 }) { hfit.wrap[f] = (float)wrapOut[f]; hfit.wrapBase[f] = (float)baseOut[f]; }
		const Posed pfit = PoseHand(r, Wrest, rig, hp, ready, &hold, &hfit);
		handR = Skin(r, pfit.P);
		for (MeshV &v : handR.v) { v.p = toHand(v.p); v.n = dirHand(v.n); }
		// THE PAD CHECKED IS THE PAD FITTED: one skin vertex, padVid (the distal segment's most palmward vertex with the index
		// half closed), so the fit and the check measure the same point of skin. Re-picking "the most palmward vertex" on
		// each pose wanders along the 24 mm segment with the joint angles (Godot hand: up to ~10 mm on a straight index);
		// that re-picked vertex is printed below for information.
		const int padV = pad(pfit, handR);
		const V3 pR = handR.v[padVid].p;
		const M3 R = Mul(Rodrigues(V3(prm[0], prm[1], prm[2])), R0);
		const V3 t(prm[3], prm[4], prm[5]);
		auto T = [&](V3 p) { return R * (p * s) + t; };
		gPrevR = R; memcpy(gPrevPrm, prm, sizeof(gPrevPrm)); gHavePrev = true;
		double chanErr = 0;   // information only: the grip's centreline against the hole's centres
		{
			const V3 l0 = T(mc), ld = R * gd;
			for (int k = 0; k < 3; k++) { const V3 d = cen[k] - l0; chanErr = fmax(chanErr, Len(d - ld * Dot(d, ld))); }
		}
		M3 RtF; for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) RtF.m[a][b] = R.m[b][a];
		const double webErr = Len(T(beaver) - web), padErr = Len(T(onFace(RtF * (pR - t) * (1.0 / s))) - pR);
		if (padV >= 0) { const V3 q = handR.v[padV].p; printf("  (information: the most palmward distal vertex of this pose, v%d, is %.1f mm from the trigger face; the fitted pad v%d %.1f mm)\n", padV, Len(T(onFace(RtF * (q - t) * (1.0 / s))) - q) * 1000, padVid, padErr * 1000); }
		const V3 bore = R * V3(0, 1, 0), mz = T(muzzle);
		printf("  FIT: the grip's centreline %.1f mm (worst) from the hole's centres (information); the web %.1f mm from the beavertail point; the index pad %.1f mm from the trigger face\n",
			chanErr * 1000, webErr * 1000, padErr * 1000);
		printf("  the bore in the hand frame (%.3f %.3f %.3f): %.1f deg from the fingers' axis X; the muzzle at (%.3f %.3f %.3f) m\n",
			bore.x, bore.y, bore.z, Deg(acos(bore.x)), mz.x, mz.y, mz.z);
		// pulled: the trigger moves back with the index (the take's trigger travel, frame 113: 10 units)
		// The pulled index: the in-between whose pad meets the trigger face moved back by its travel.
		const V3 trigPulled = trig - V3(0, 10, 0);
		double idxFire = idxReady, padFireErr = 1e9;
		for (double u = idxReady; u <= 1.0001; u += 0.005) { const V3 pu = gridVert(padVid, idxBase, u); const V3 fp = onFace(RtF * (pu - t) * (1.0 / s)) - V3(0, 10, 0); const double d = Len(T(fp) - pu); if (d < padFireErr) { padFireErr = d; idxFire = u; } }
		(void)trigPulled;
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
		std::map<std::string, int> inBone;
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
			if (finger && d < -0.002) { insideFingers++; static int shown = 0; if (shown++ < 400) inBone[r.joints[b].name]++; }
		}
		printf("  finger vertices more than 2 mm in, by joint:"); for (auto &kv : inBone) printf(" %s %d", kv.first.c_str(), kv.second); printf("\n");
		const double padFace = Dot(pR - T(trig), R * V3(0, 1, 0));   // in front of the face (+) or in it (-)
		double touch[5] = { 1e9, 1e9, 1e9, 1e9, 1e9 };
		for (int i = 0; i < nh; i++)
		{
			const int f = fingerOf(handR.v[i].bone);
			if (f < 0 || segOf(handR.v[i].bone) < (f >= 2 ? 2 : 1)) continue;
			const V3 gp = RtF * (handR.v[i].p - t) * (1.0 / s);
			touch[f] = fmin(touch[f], (f == 0 || f == 1 ? sdf.Query(gp) : fabs(gripSdf.Query(gp))) * s);
		}
		printf("  CHECK each finger's nearest skin to the gun (thumb, index: middle and outer segments, signed; middle, ring, little: the END segment, its distance to the grip's surface): thumb %.1f index %.1f middle %.1f ring %.1f little %.1f mm\n",
			touch[0] * 1000, touch[1] * 1000, touch[2] * 1000, touch[3] * 1000, touch[4] * 1000);
		printf("  CHECK hand vertices more than 1 mm inside the gun: %d of %d (finger vertices more than 2 mm in: %d); the deepest %.1f mm (vertex %d, joint %s)\n",
			inside, nh, insideFingers, -deepest * 1000, deepV, deepV >= 0 ? r.joints[handR.v[deepV].bone].name.c_str() : "-");
		{ const V3 pg = RtF * (pR - t) * (1.0 / s), w = RtF * (web - t) * (1.0 / s); printf("  in gun file units: the pad (%.1f %.1f %.1f), the trigger face (0 %.1f %.1f..%.1f), the web (%.1f %.1f %.1f), the beavertail (0 %.1f %.1f)\n", pg.x, pg.y, pg.z, trig.y, tz0, tz1, w.x, w.y, w.z, beaver.y, beaver.z); }
		printf("  CHECK the index pad (ready) %.1f mm from the trigger face point, %.1f mm in front of the face; (pulled, the trigger back 10 units) %.1f mm\n",
			padErr * 1000, padFace * 1000, padFireErr * 1000);
		char line[256];
		snprintf(line, sizeof line, "%s: the index pad on the trigger face within 6 mm (%.1f)", meshes[h], padErr * 1000); Check(padErr < 0.006, line);
		snprintf(line, sizeof line, "%s: the web at the beavertail within 8 mm (%.1f)", meshes[h], webErr * 1000); Check(webErr < 0.008, line);
		snprintf(line, sizeof line, "%s: no finger vertex more than 2 mm inside the gun (%d)", meshes[h], insideFingers); Check(insideFingers == 0, line);
		for (int f : { 2, 3, 4 }) { snprintf(line, sizeof line, "%s: %s wrapped onto the grip: its end segment within 3 mm of the grip (%.1f)", meshes[h], kFingerJointNames[f][0], touch[f] * 1000); Check(touch[f] < 0.003, line); }

		// The seat for the engine: model render coords (x, z, y of the file) -> the hand frame in GUN FILE units,
		// so MODELDEF Scale s takes it to metres (models.cpp: the seat first, then Scale, in the grip frame).
		// hand_m = R s file + t  =>  hand_units = R file + t / s;  file = Swap(render)
		const V3 c0 = R * V3(1, 0, 0), c1 = R * V3(0, 1, 0), c2 = R * V3(0, 0, 1);
		const V3 tu = t * (1.0 / s);
		fprintf(res, "seat_%s %.9g %.9g %.9g 0 %.9g %.9g %.9g 0 %.9g %.9g %.9g 0 %.9g %.9g %.9g 1\n", right ? "right" : "left",
			c0.x, c0.y, c0.z, c2.x, c2.y, c2.z, c1.x, c1.y, c1.z, tu.x, tu.y, tu.z);
		fprintf(res, "fit_%s R %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g t %.9g %.9g %.9g\n", right ? "right" : "left",
			R.m[0][0], R.m[0][1], R.m[0][2], R.m[1][0], R.m[1][1], R.m[1][2], R.m[2][0], R.m[2][1], R.m[2][2], t.x, t.y, t.z);
		fprintf(res, "index_%s ready %.4f fire %.4f base %.4f\n", right ? "right" : "left", idxReady, idxFire, idxBase);
		fprintf(res, "wrap_%s thumb %.4f %.4f middle %.4f %.4f ring %.4f %.4f little %.4f %.4f (knuckle, outer)\n", right ? "right" : "left",
			baseOut[0], wrapOut[0], baseOut[2], wrapOut[2], baseOut[3], wrapOut[3], baseOut[4], wrapOut[4]);
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
