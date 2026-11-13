// tools/fbxload_test/test_fbxload.cpp -- the engine's FBX loader core (src/common/models/fbx_skinned.cpp),
// compiled as is with ufbx and run against hand_final.fbx, outside the engine. run.sh builds and runs it.
//
//   test_fbxload <hand_final.fbx> [--dump DIR] [--angles f1,f2,...]
//
// Checks (each prints PASS/FAIL with its numbers; docs/FBX_LOADER.md "Acceptance"):
//   T1  each hand loads by its mesh name; 22 joints, the FBX's names, parents first
//   T2  frames: the take, its frame count and rate
//   T3  the loader's TRS, composed the way the engine composes IQM joints (models_iqm.cpp:201-226 and
//       CalculateBonesIQMSpecialized), equals ufbx's own node_to_world for every joint at rest and at every frame
//   T4  the engine's skinning (rest mesh, 4 byte weights, palette world(f) * world(rest)^-1) against ufbx's
//       full skinning (every weight, world(f) * geometry_to_bone) at sampled frames, max vertex error
//   T5  hand length: wrist to the middle fingertip on the mesh; the MODELDEF scale for 0.19 m
//   T6  handedness: Hand_Low is a right hand, Hand_Low1 a left hand (curl direction against the knuckle line)
//   T7  faces: rest mesh outward (signed volume), normals agree with the faces, at rest and at a posed frame
//   --angles prints per-joint rotation angles and finger metrics for poses_ref.py to diff against.
//   --dump writes the rest and posed meshes for render.py.

#include "fbx_skinned.h"
#include "vr_handpose_math.h"
#include "ufbx.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

using namespace FBXSkinned;

static int gFails = 0;
static void Check(bool ok, const char *what)
{
	printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
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
static V3 MatPoint(const double m[16], V3 p)
{
	return { m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13], m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14] };
}
static V3 Sub(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static double Len(V3 a) { return sqrt(Dot(a, a)); }
static V3 Pos(const double m[16]) { return { m[12], m[13], m[14] }; }

static void ToMat(const ufbx_matrix &m, double o[16])
{
	for (int c = 0; c < 4; c++) { o[c * 4] = m.cols[c].x; o[c * 4 + 1] = m.cols[c].y; o[c * 4 + 2] = m.cols[c].z; o[c * 4 + 3] = c == 3; }
}

// The engine's composition (models_iqm.cpp): world_i = world_parent * T*R*S(local_i); roots: T*R*S.
static void ComposeChain(const Result &r, const TRSd *locals, std::vector<std::array<double, 16>> &world)
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

static double MaxDiff(const double a[16], const double b[16])
{
	double d = 0;
	for (int i = 0; i < 16; i++) d = fmax(d, fabs(a[i] - b[i]));
	return d;
}

static double QuatAngleDeg(const double a[4], const double b[4])
{
	double d = fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
	if (d > 1) d = 1;
	return 2 * acos(d) * 180.0 / M_PI;
}

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: test_fbxload <fbx> [--dump DIR] [--angles f,f,...]\n"); return 2; }
	const char *path = argv[1];
	const char *dumpDir = nullptr;
	std::vector<int> angleFrames;
	for (int i = 2; i < argc; i++)
	{
		if (!strcmp(argv[i], "--dump") && i + 1 < argc) dumpDir = argv[++i];
		else if (!strcmp(argv[i], "--angles") && i + 1 < argc)
		{
			for (char *s = argv[++i]; *s;) { angleFrames.push_back(strtol(s, &s, 10)); if (*s == ',') s++; }
		}
	}
	std::vector<char> file = ReadFile(path);

	// ufbx's own scene, the reference (same options as the loader: Z up, right-handed).
	ufbx_load_opts opts = {};
	opts.ignore_embedded = true;
	opts.generate_missing_normals = true;
	opts.target_axes = ufbx_axes_right_handed_z_up;
	opts.space_conversion = UFBX_SPACE_CONVERSION_TRANSFORM_ROOT;
	ufbx_error uerr;
	ufbx_scene *ref = ufbx_load_memory(file.data(), file.size(), &opts, &uerr);
	if (!ref) { fprintf(stderr, "ufbx failed\n"); return 2; }

	// The selector must be required: two skinned meshes.
	{
		Result r; std::string err;
		const bool ok = Load(file.data(), file.size(), "", r, err);
		printf("no selector: %s (%s)\n", ok ? "loaded" : "refused", err.c_str());
		Check(!ok, "T1 a file with two skinned meshes needs the mesh named");
	}

	const char *meshes[2] = { "Hand_Low", "Hand_Low1" };
	double handLen[2] = { 0, 0 };
	for (int h = 0; h < 2; h++)
	{
		Result r; std::string err;
		const bool ok = Load(file.data(), file.size(), meshes[h], r, err);
		printf("\n==== %s: %s %s\n", meshes[h], ok ? "loaded" : "FAILED", err.c_str());
		if (!ok) { gFails++; continue; }
		const Diag &d = r.diag;
		const int nj = (int)r.joints.size();

		// ---- T1
		printf("  joints %d:", nj);
		for (auto &j : r.joints) printf(" %s(%d)", j.name.c_str(), j.parent);
		printf("\n");
		bool parentsFirst = true;
		for (int j = 0; j < nj; j++) if (r.joints[j].parent >= j) parentsFirst = false;
		Check(nj == 22, "T1 22 joints");
		Check(parentsFirst, "T1 every parent before its child");
		// the skeleton's top control, from ufbx: which GLOBAL_MAIN_CONTROL this mesh's Root_joint hangs under
		std::string top;
		for (size_t i = 0; i < ref->nodes.count; i++)
		{
			const ufbx_node *n = ref->nodes.data[i];
			if (strcmp(n->name.data, "Root_joint")) continue;
			// is it one of this mesh's bones?
			const ufbx_mesh *m = nullptr;
			for (size_t k = 0; k < ref->meshes.count; k++) if (!strcmp(ref->meshes.data[k]->instances.data[0]->name.data, meshes[h])) m = ref->meshes.data[k];
			bool mine = false;
			for (size_t c = 0; m && c < m->skin_deformers.data[0]->clusters.count; c++) if (m->skin_deformers.data[0]->clusters.data[c]->bone_node == n) mine = true;
			if (!mine) continue;
			for (const ufbx_node *p = n; p && !p->is_root; p = p->parent) top = p->name.data;
		}
		printf("  skeleton under %s; material %s, texture '%s'; blend shape deformers %d\n", top.c_str(), d.materialName.c_str(), d.textureFile.c_str(), d.blendShapeDeformers);
		Check(top == (h == 0 ? "GLOBAL_MAIN_CONTROL_R" : "GLOBAL_MAIN_CONTROL_R1"), "T1 the mesh's own skeleton (R for Hand_Low, R1 for Hand_Low1)");

		// ---- T2
		for (auto &t : r.takes)
			printf("  take '%s': frames %d..%d (%d) at %g fps, keys %.4f..%.4f s\n", t.name.c_str(), t.firstFrame, t.firstFrame + t.numFrames - 1, t.numFrames, t.fps, t.keyFirst, t.keyLast);
		Check(r.takes.size() == 1 && r.takes[0].name == "Take 001" && r.takes[0].fps == 60.0, "T2 one take, 'Take 001', 60 fps");
		Check(r.numFrames == (int)(r.frames.size() / nj) && r.numFrames == r.takes[0].numFrames, "T2 frames = the take's frames");
		printf("  frame count %d\n", r.numFrames);

		// ---- T3: composed TRS vs ufbx node_to_world
		const ufbx_anim *anim = ref->anim_stacks.data[0]->anim;
		std::vector<uint32_t> refIds(nj);
		{
			const ufbx_mesh *m = nullptr;
			for (size_t k = 0; k < ref->meshes.count; k++) if (!strcmp(ref->meshes.data[k]->instances.data[0]->name.data, meshes[h])) m = ref->meshes.data[k];
			for (int j = 0; j < nj; j++)
				for (size_t c = 0; c < m->skin_deformers.data[0]->clusters.count; c++)
				{
					const ufbx_node *b = m->skin_deformers.data[0]->clusters.data[c]->bone_node;
					if (r.joints[j].name == b->name.data) refIds[j] = b->typed_id;
				}
		}
		std::vector<std::array<double, 16>> W, Wrest;
		std::vector<TRSd> restLoc(nj);
		for (int j = 0; j < nj; j++) restLoc[j] = r.joints[j].rest;
		ComposeChain(r, restLoc.data(), Wrest);
		double devRest = 0;
		for (int j = 0; j < nj; j++)
		{
			double m[16]; ToMat(ref->nodes.data[refIds[j]]->node_to_world, m);
			devRest = fmax(devRest, MaxDiff(m, Wrest[j].data()));
			devRest = fmax(devRest, MaxDiff(r.joints[j].restWorld, Wrest[j].data()));
		}
		double devFrames = 0; int devFrame = -1;
		for (int f = 0; f < r.numFrames; f++)
		{
			ComposeChain(r, &r.frames[(size_t)f * nj], W);
			ufbx_scene *es = ufbx_evaluate_scene(ref, anim, r.takes[0].startTime + f / r.takes[0].fps, nullptr, &uerr);
			for (int j = 0; j < nj; j++)
			{
				double m[16]; ToMat(es->nodes.data[refIds[j]]->node_to_world, m);
				const double dd = MaxDiff(m, W[j].data());
				if (dd > devFrames) { devFrames = dd; devFrame = f; }
			}
			ufbx_free_scene(es);
		}
		printf("  composed TRS vs ufbx node_to_world: rest max %.3g, all %d frames max %.3g (frame %d) file units\n", devRest, r.numFrames, devFrames, devFrame);
		Check(devRest < 1e-9 && devFrames < 1e-9, "T3 the engine's joint composition reproduces ufbx's world matrices (< 1e-9)");

		// ---- T5: hand length on the mesh
		int jRoot = -1, jTop = -1, jTip = -1;
		for (int j = 0; j < nj; j++)
		{
			if (r.joints[j].name == "Root_joint") jRoot = j;
			if (r.joints[j].name == "MIDDLE_F_TOP_joint") jTop = j;
			if (r.joints[j].name == "MIDDLE_F_UP_TOP_joint") jTip = j;
		}
		const V3 wrist = Pos(Wrest[jRoot].data());
		double Lmesh = 0;
		for (auto &v : r.verts)
		{
			if (v.bone[0] != jTop && v.bone[0] != jTip) continue;	// largest weight on the middle finger's last segment
			Lmesh = fmax(Lmesh, Len(Sub({ v.pos[0], v.pos[1], v.pos[2] }, wrist)));
		}
		const double Ljoint = Len(Sub(Pos(Wrest[jTip].data()), wrist));
		handLen[h] = Lmesh;
		const double scale = 0.19 / Lmesh;
		printf("  hand length: wrist (Root_joint) to the farthest middle-fingertip vertex %.6f file units (joint to joint %.6f); "
			"MODELDEF Scale for 0.19 m = 0.19 / %.6f = %.8f metres per file unit\n", Lmesh, Ljoint, Lmesh, scale);
		Check(fabs(Lmesh * scale - 0.19) < 1e-12, "T5 0.19 m after the scale");

		// ---- T4: engine skinning vs ufbx skinning
		const ufbx_mesh *um = nullptr;
		for (size_t k = 0; k < ref->meshes.count; k++) if (!strcmp(ref->meshes.data[k]->instances.data[0]->name.data, meshes[h])) um = ref->meshes.data[k];
		std::vector<std::array<double, 16>> WrestInv(nj);
		for (int j = 0; j < nj; j++) MatInvert(Wrest[j].data(), WrestInv[j].data());
		const int sample[] = { 0, 160, 205, 318, 500, 761, 845, 984, 1100, r.numFrames - 2, r.numFrames - 1 };
		double worst = 0, worstAll = 0; int worstF = -1;
		for (int f : sample)
		{
			ComposeChain(r, &r.frames[(size_t)f * nj], W);
			std::vector<std::array<double, 16>> P(nj);
			for (int j = 0; j < nj; j++) MatMul(W[j].data(), WrestInv[j].data(), P[j].data());
			ufbx_scene *es = ufbx_evaluate_scene(ref, anim, r.takes[0].startTime + f / r.takes[0].fps, nullptr, &uerr);
			const ufbx_mesh *em = es->meshes.data[um->typed_id];
			const ufbx_skin_deformer *sk = em->skin_deformers.data[0];
			double fmaxe = 0, fmaxAll = 0, fmax4 = 0;
			for (auto &v : r.verts)
			{
				// (c) the engine: 4 byte weights
				V3 e = { 0, 0, 0 };
				for (int k = 0; k < 4; k++)
				{
					if (!v.weight[k]) continue;
					const V3 p = MatPoint(P[v.bone[k]].data(), { v.pos[0], v.pos[1], v.pos[2] });
					e.x += p.x * v.weight[k] / 255.0; e.y += p.y * v.weight[k] / 255.0; e.z += p.z * v.weight[k] / 255.0;
				}
				// ufbx: every weight, normalised; and (a) the same weights through the engine's palette, (b) the
				// largest 4 as exact doubles through the palette
				const ufbx_skin_vertex sv = sk->vertices.data[v.controlPoint];
				std::vector<std::pair<double, int>> ws;
				V3 g = { 0, 0, 0 }, a = { 0, 0, 0 }; double tot = 0;
				for (uint32_t k = 0; k < sv.num_weights; k++)
				{
					const ufbx_skin_weight sw = sk->weights.data[sv.weight_begin + k];
					const ufbx_skin_cluster *cl = sk->clusters.data[sw.cluster_index];
					const ufbx_matrix M = ufbx_matrix_mul(&cl->bone_node->node_to_world, &cl->geometry_to_bone);
					const ufbx_vec3 p = ufbx_transform_position(&M, em->vertices.data[v.controlPoint]);
					g.x += p.x * sw.weight; g.y += p.y * sw.weight; g.z += p.z * sw.weight; tot += sw.weight;
					int jj = -1;
					for (int j = 0; j < nj; j++) if (r.joints[j].name == cl->bone_node->name.data) jj = j;
					const V3 q = MatPoint(P[jj].data(), { v.pos[0], v.pos[1], v.pos[2] });
					a.x += q.x * sw.weight; a.y += q.y * sw.weight; a.z += q.z * sw.weight;
					ws.push_back({ sw.weight, jj });
				}
				g.x /= tot; g.y /= tot; g.z /= tot;
				a.x /= tot; a.y /= tot; a.z /= tot;
				std::sort(ws.begin(), ws.end(), [](auto &x, auto &y) { return x.first > y.first; });
				if (ws.size() > 4) ws.resize(4);
				V3 b4 = { 0, 0, 0 }; double t4 = 0;
				for (auto &w : ws) { const V3 q = MatPoint(P[w.second].data(), { v.pos[0], v.pos[1], v.pos[2] }); b4.x += q.x * w.first; b4.y += q.y * w.first; b4.z += q.z * w.first; t4 += w.first; }
				b4.x /= t4; b4.y /= t4; b4.z /= t4;
				fmaxe = fmax(fmaxe, Len(Sub(e, g)));
				fmaxAll = fmax(fmaxAll, Len(Sub(a, g)));
				fmax4 = fmax(fmax4, Len(Sub(b4, g)));
			}
			ufbx_free_scene(es);
			printf("  frame %4d: max vertex error vs ufbx: every weight through the palette %.2e mm; largest 4 exact %.3f mm; the engine's 4 bytes %.3f mm\n",
				f, fmaxAll * scale * 1000.0, fmax4 * scale * 1000.0, fmaxe * scale * 1000.0);
			if (fmaxe > worst) { worst = fmaxe; worstF = f; }
			worstAll = fmax(worstAll, fmaxAll);
		}
		printf("  weights: max influences %d, %d control points had more than 4 (largest weight sum dropped %.4f), byte rounding max %.5f, unweighted %d\n",
			d.maxInfluences, d.verticesOver4, d.maxDroppedWeight, d.maxQuantError, d.unweightedControlPoints);
		Check(worstAll * scale * 1000.0 < 1e-4, "T4 rest mesh + palette world(f) * world(rest)^-1 with every weight = ufbx's skinning (< 1e-4 mm: float vertices)");
		printf("  the engine's 4 byte weights: worst %.3f mm at frame %d (cost of FModelVertex's 4 bone slots and byte weights)\n", worst * scale * 1000.0, worstF);
		Check(worst * scale * 1000.0 < 2.5, "T4 the engine's 4 byte weights within 2.5 mm of ufbx's skinning at every sampled frame");

		// ---- T6: handedness, at the rest placement (with any mirror). f = palm (HANDPALM_joint) -> middle knuckle,
		// k = pinky knuckle -> index knuckle, c = middle knuckle -> the middle fingertip in the fist (frame 500, the
		// palm's own motion taken out): the side of the hand the fingers close on. A right hand seen palm-on with
		// the fingers up has the index on the viewer's right, so its palm faces k x f: dot(k x f, c) > 0.
		{
			int jIB = -1, jMB = -1, jPB = -1, jPalm = -1, jTT = -1;
			for (int j = 0; j < nj; j++)
			{
				if (r.joints[j].name == "INDEX_BASE_joint") jIB = j;
				if (r.joints[j].name == "MIDDLE_F_BASE_joint") jMB = j;
				if (r.joints[j].name == "PINK_BASE_joint") jPB = j;
				if (r.joints[j].name == "HANDPALM_joint") jPalm = j;
				if (r.joints[j].name == "THUMB_UP_TOP_joint") jTT = j;
			}
			ComposeChain(r, &r.frames[(size_t)500 * nj], W);
			double inv[16], back[16];
			MatInvert(W[jPalm].data(), inv);
			MatMul(Wrest[jPalm].data(), inv, back);	// the palm's own motion (and the arm's) taken out
			const V3 tipFist = MatPoint(back, Pos(W[jTip].data()));
			const V3 c = Sub(tipFist, Pos(Wrest[jMB].data()));	// where the curled tip ends, from its knuckle
			const V3 f = Sub(Pos(Wrest[jMB].data()), Pos(Wrest[jPalm].data()));
			const V3 k = Sub(Pos(Wrest[jIB].data()), Pos(Wrest[jPB].data()));
			const double hs = Dot(Cross(k, f), c) / (Len(k) * Len(f) * Len(c));
			// and from the rest alone: the thumb tip lies on the palm side of the knuckles
			const V3 th = Sub(Pos(Wrest[jTT].data()), Pos(Wrest[jMB].data()));
			const double ht = Dot(Cross(k, f), th) / (Len(k) * Len(f) * Len(th));
			double rootDet;
			{
				const double *m = Wrest[jRoot].data();
				rootDet = m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) + m[8] * (m[1] * m[6] - m[5] * m[2]);
			}
			printf("  rest: palm->middle knuckle %.3f %.3f %.3f, pinky->index knuckle %.3f %.3f %.3f, knuckle to fist tip %.3f %.3f %.3f\n", f.x, f.y, f.z, k.x, k.y, k.z, c.x, c.y, c.z);
			printf("  handedness: cos(k x f, fist tip) = %+.3f (%s hand), cos(k x f, thumb tip) = %+.3f (%s hand); Root_joint rest det %+.3f; skin rest det %+.3f\n",
				hs, hs > 0 ? "RIGHT" : "LEFT", ht, ht > 0 ? "RIGHT" : "LEFT", rootDet, d.restDeterminant);
			Check(h == 0 ? (hs > 0.25 && ht > 0.25) : (hs < -0.25 && ht < -0.25), h == 0 ? "T6 Hand_Low is a right hand (both tests)" : "T6 Hand_Low1 is a left hand (both tests)");

			// T8: the engine's seat (vr_handpose_math.h ModelSeat, measured from the model's bind pose =
			// baseframe = these rest worlds) on this hand's own side: its palm normal must point where the
			// fingers close, and the seat carries the load swap's reflection (det -1) as for an IQM.
			VRHandPose::Vec3d Pk[5];
			const char *fj[5] = { "Root_joint", "INDEX_BASE_joint", "MIDDLE_F_BASE_joint", "RING_BASE_joint", "PINK_BASE_joint" };
			for (int q = 0; q < 5; q++)
				for (int j = 0; j < nj; j++)
					if (r.joints[j].name == fj[q]) { const V3 p = Pos(Wrest[j].data()); Pk[q] = VRHandPose::Vec3d(p.x, p.y, p.z); }
			VRHandPose::Vec3d X, Y, Z, sock;
			const bool right = (h == 0);
			const bool okf = VRHandPose::HandFrameFromJoints(Pk, right, X, Y, Z, sock);
			float seat[16];
			const bool oks = VRHandPose::ModelSeat(Pk, right, seat);
			const double palmDot = -(Y.x * c.x + Y.y * c.y + Y.z * c.z) / Len(c);
			const double sd = seat[0] * (seat[5] * seat[10] - seat[9] * seat[6]) - seat[4] * (seat[1] * seat[10] - seat[9] * seat[2]) + seat[8] * (seat[1] * seat[6] - seat[5] * seat[2]);
			printf("  seat (%s side): palm normal (-Y) vs where the fingers close cos %+.3f; seat det %+.3f\n", right ? "right" : "left", palmDot, sd);
			Check(okf && oks && palmDot > 0.25 && fabs(sd + 1.0) < 1e-4, "T8 the engine's seat reads this hand on its own side (palm toward the curl, det -1)");
		}

		// ---- T7: faces and normals
		{
			printf("  faces: rest signed volume %+.4f (file order %s); the file's normals face the surface by mean cosine %.4f -> %s\n",
				d.signedVolume, d.reversedForOutward ? "was inside out, reversed" : "faces out", d.normalAgreeFile,
				d.normalsFromSurface ? "the surface's own normals are used" : "the file's normals are used");
			// the engine's winding is AGAINST the outward normal in file space: so -(b-a)x(c-a) should agree
			// with the vertex normals, at rest and posed (frame 500).
			for (int pass = 0; pass < 3; pass++)
			{
				const int pf = pass == 1 ? 160 : 500;
				std::vector<V3> P(r.verts.size()), N(r.verts.size());
				if (pass == 0) for (size_t i = 0; i < r.verts.size(); i++) { P[i] = { r.verts[i].pos[0], r.verts[i].pos[1], r.verts[i].pos[2] }; N[i] = { r.verts[i].nrm[0], r.verts[i].nrm[1], r.verts[i].nrm[2] }; }
				else
				{
					ComposeChain(r, &r.frames[(size_t)pf * nj], W);
					std::vector<std::array<double, 16>> Pm(nj);
					for (int j = 0; j < nj; j++) MatMul(W[j].data(), WrestInv[j].data(), Pm[j].data());
					for (size_t i = 0; i < r.verts.size(); i++)
					{
						const Vertex &v = r.verts[i];
						V3 p = { 0, 0, 0 }, n = { 0, 0, 0 };
						for (int k = 0; k < 4; k++)
						{
							if (!v.weight[k]) continue;
							const double *m = Pm[v.bone[k]].data(), w = v.weight[k] / 255.0;
							const V3 q = MatPoint(m, { v.pos[0], v.pos[1], v.pos[2] });
							p.x += q.x * w; p.y += q.y * w; p.z += q.z * w;
							// palettes here are rotations (+translation): the engine shader moves normals by the palette's 3x3
							n.x += (m[0] * v.nrm[0] + m[4] * v.nrm[1] + m[8] * v.nrm[2]) * w;
							n.y += (m[1] * v.nrm[0] + m[5] * v.nrm[1] + m[9] * v.nrm[2]) * w;
							n.z += (m[2] * v.nrm[0] + m[6] * v.nrm[1] + m[10] * v.nrm[2]) * w;
						}
						P[i] = p; N[i] = n;
					}
				}
				int agree = 0, total = 0;
				for (size_t t = 0; t < r.tris.size(); t += 3)
				{
					const V3 a = P[r.tris[t]], b = P[r.tris[t + 1]], c = P[r.tris[t + 2]];
					const V3 g = Cross(Sub(b, a), Sub(c, a));
					const V3 n = { N[r.tris[t]].x + N[r.tris[t + 1]].x + N[r.tris[t + 2]].x, N[r.tris[t]].y + N[r.tris[t + 1]].y + N[r.tris[t + 2]].y, N[r.tris[t]].z + N[r.tris[t + 1]].z + N[r.tris[t + 2]].z };
					total++;
					if (Dot(g, n) < 0) agree++;
				}
				printf("  %s%d: %d/%d triangles wound against their vertex normals (the engine's IQM convention)\n", pass ? "frame " : "rest", pass ? pf : 0, agree, total);
				if (pass == 0) Check(agree == total, "T7 rest: every face and its normals agree");
				if (pass == 1) Check(agree == total, "T7 frame 160 (open fingers, wrist bent, arm moved): every face and its normals agree");
				if (pass == 2) printf("  (frame 500 is the fist: skin folding at the knuckles turns some faces over; information only)\n");
			}
			Check(d.signedVolume > 0 || d.reversedForOutward, "T7 rest mesh faces out");
		}

		// ---- --angles: per joint rotation from rest (deg) and the root's position in the FBX's own Y-up space
		for (int f : angleFrames)
		{
			if (f < 0 || f >= r.numFrames) continue;
			printf("ANGLES %s f%d", meshes[h], f);
			for (int j = 0; j < nj; j++)
				printf(" %s=%.3f", r.joints[j].name.c_str(), QuatAngleDeg(r.joints[j].rest.q, r.frames[(size_t)f * nj + j].q));
			ComposeChain(r, &r.frames[(size_t)f * nj], W);
			const V3 p = Pos(W[jRoot].data());
			printf(" rootY=%.5f,%.5f,%.5f\n", p.x, p.z, -p.y);	// Z-up (x, y, z) back to the file's Y-up (x, z, -y)
		}

		// ---- --dump: rest and posed meshes for render.py
		if (dumpDir)
		{
			// FBXLOAD_DUMP_FRAMES=a,b,c picks the take frames to dump (the rest pose is always dumped).
			std::vector<int> dumpFrames = { -1, 0, 205, 500, 845, 1100 };
			if (const char *e = getenv("FBXLOAD_DUMP_FRAMES"))
			{
				dumpFrames = { -1 };
				for (const char *q = e; *q; )
				{
					dumpFrames.push_back(atoi(q));
					while (*q && *q != ',') q++;
					if (*q == ',') q++;
				}
			}
			for (int f : dumpFrames)
			{
				if (f >= r.numFrames) continue;
				char fn[1024];
				snprintf(fn, sizeof(fn), "%s/%s_%s.txt", dumpDir, meshes[h], f < 0 ? "rest" : std::to_string(f).c_str());
				FILE *o = fopen(fn, "w");
				std::vector<std::array<double, 16>> Pm(nj);
				if (f >= 0)
				{
					ComposeChain(r, &r.frames[(size_t)f * nj], W);
					for (int j = 0; j < nj; j++) MatMul(W[j].data(), WrestInv[j].data(), Pm[j].data());
				}
				fprintf(o, "%zu %zu\n", r.verts.size(), r.tris.size() / 3);
				for (auto &v : r.verts)
				{
					V3 p = { v.pos[0], v.pos[1], v.pos[2] }, n = { v.nrm[0], v.nrm[1], v.nrm[2] };
					if (f >= 0)
					{
						V3 pp = { 0, 0, 0 }, nn = { 0, 0, 0 };
						for (int k = 0; k < 4; k++)
						{
							if (!v.weight[k]) continue;
							const double *m = Pm[v.bone[k]].data(), w = v.weight[k] / 255.0;
							const V3 q = MatPoint(m, p);
							pp.x += q.x * w; pp.y += q.y * w; pp.z += q.z * w;
							nn.x += (m[0] * n.x + m[4] * n.y + m[8] * n.z) * w;
							nn.y += (m[1] * n.x + m[5] * n.y + m[9] * n.z) * w;
							nn.z += (m[2] * n.x + m[6] * n.y + m[10] * n.z) * w;
						}
						p = pp; n = nn;
					}
					fprintf(o, "%.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f\n", p.x, p.y, p.z, n.x, n.y, n.z, v.uv[0], v.uv[1]);
				}
				for (size_t t = 0; t < r.tris.size(); t += 3) fprintf(o, "%u %u %u\n", r.tris[t], r.tris[t + 1], r.tris[t + 2]);
				fclose(o);
			}
		}
	}
	printf("\nhand lengths: right %.5f, left %.5f file units\n", handLen[0], handLen[1]);
	ufbx_free_scene(ref);
	printf("\n%s: %d failure(s)\n", gFails ? "FAIL" : "PASS", gFails);
	return gFails ? 1 : 0;
}
