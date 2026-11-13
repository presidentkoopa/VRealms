/*
** fbx_skinned.cpp
**
** RS FORK -- one skinned mesh of an FBX file, read by ufbx, as plain arrays (see fbx_skinned.h).
** Engine-free: compiled into the engine (models_fbx.cpp) and into tools/fbxload_test as is.
** The checklist this answers to, item by item, is docs/FBX_LOADER.md ("C<n>" below).
**
**---------------------------------------------------------------------------
*/

#include "fbx_skinned.h"
#include "ufbx.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace FBXSkinned
{

// C14: how well the file's normals must face the surface to be used (mean cosine).
static const double NORMAL_TRUST = 0.95;

//===========================================================================
//
// Matrices: column-major 4x4 doubles, the layout VSMatrix and IQM use. ufbx_matrix is 3x4 with
// cols[0..3] (the last one the translation).
//
//===========================================================================

static void ToMat(const ufbx_matrix &m, double o[16])
{
	for (int c = 0; c < 4; c++)
	{
		o[c * 4 + 0] = m.cols[c].x;
		o[c * 4 + 1] = m.cols[c].y;
		o[c * 4 + 2] = m.cols[c].z;
		o[c * 4 + 3] = (c == 3) ? 1.0 : 0.0;
	}
}

static TRSd FromTransform(const ufbx_transform &t)
{
	TRSd r;
	r.t[0] = t.translation.x; r.t[1] = t.translation.y; r.t[2] = t.translation.z;
	double n = sqrt(t.rotation.x * t.rotation.x + t.rotation.y * t.rotation.y + t.rotation.z * t.rotation.z + t.rotation.w * t.rotation.w);
	if (!(n > 0)) n = 1;
	r.q[0] = t.rotation.x / n; r.q[1] = t.rotation.y / n; r.q[2] = t.rotation.z / n; r.q[3] = t.rotation.w / n;
	r.s[0] = t.scale.x; r.s[1] = t.scale.y; r.s[2] = t.scale.z;
	return r;
}

void ComposeTRS(const TRSd &t, double o[16])
{
	const double x = t.q[0], y = t.q[1], z = t.q[2], w = t.q[3];
	const double R[3][3] =
	{
		{ 1 - 2 * (y * y + z * z), 2 * (x * y - w * z),     2 * (x * z + w * y) },
		{ 2 * (x * y + w * z),     1 - 2 * (x * x + z * z), 2 * (y * z - w * x) },
		{ 2 * (x * z - w * y),     2 * (y * z + w * x),     1 - 2 * (x * x + y * y) },
	};
	for (int c = 0; c < 3; c++)
	{
		for (int r = 0; r < 3; r++) o[c * 4 + r] = R[r][c] * t.s[c];
		o[c * 4 + 3] = 0;
	}
	o[12] = t.t[0]; o[13] = t.t[1]; o[14] = t.t[2]; o[15] = 1;
}

void MatMul(const double a[16], const double b[16], double o[16])
{
	double r[16];
	for (int c = 0; c < 4; c++)
		for (int i = 0; i < 4; i++)
		{
			double s = 0;
			for (int k = 0; k < 4; k++) s += a[k * 4 + i] * b[c * 4 + k];
			r[c * 4 + i] = s;
		}
	memcpy(o, r, sizeof(r));
}

bool MatInvert(const double m[16], double out[16])
{
	// Gauss-Jordan with partial pivoting, on the full 4x4.
	double a[4][8];
	for (int r = 0; r < 4; r++)
	{
		for (int c = 0; c < 4; c++) a[r][c] = m[c * 4 + r];
		for (int c = 0; c < 4; c++) a[r][4 + c] = (r == c) ? 1.0 : 0.0;
	}
	for (int col = 0; col < 4; col++)
	{
		int piv = col;
		for (int r = col + 1; r < 4; r++) if (fabs(a[r][col]) > fabs(a[piv][col])) piv = r;
		if (fabs(a[piv][col]) < 1e-300) return false;
		if (piv != col) for (int c = 0; c < 8; c++) std::swap(a[piv][c], a[col][c]);
		const double d = a[col][col];
		for (int c = 0; c < 8; c++) a[col][c] /= d;
		for (int r = 0; r < 4; r++)
		{
			if (r == col) continue;
			const double f = a[r][col];
			if (f != 0) for (int c = 0; c < 8; c++) a[r][c] -= f * a[col][c];
		}
	}
	for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) out[c * 4 + r] = a[r][4 + c];
	return true;
}

static ufbx_vec3 Vec3(double x, double y, double z)
{
	ufbx_vec3 v;	// set by member: ufbx_vec3 is a union of an anonymous struct and an array
	v.x = x; v.y = y; v.z = z;
	return v;
}

static bool IEquals(const char *a, const char *b)
{
	for (; *a && *b; a++, b++)
	{
		char x = *a, y = *b;
		if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
		if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
		if (x != y) return false;
	}
	return *a == *b;
}

static std::string MeshName(const ufbx_mesh *m)
{
	if (m->instances.count > 0 && m->instances.data[0]->name.length > 0) return m->instances.data[0]->name.data;
	return m->name.data;
}

static std::string BaseName(const ufbx_string &s)
{
	std::string f(s.data, s.length);
	const size_t k = f.find_last_of("/\\");
	return k == std::string::npos ? f : f.substr(k + 1);
}

namespace
{
	struct SceneGuard
	{
		ufbx_scene *s;
		explicit SceneGuard(ufbx_scene *x) : s(x) {}
		~SceneGuard() { if (s) ufbx_free_scene(s); }
	};
}

//===========================================================================
//
// C11/C12: every joint's local TRS in a scene (the loaded one = rest, or one ufbx evaluated at a
// frame's time). A root joint takes its whole node_to_world: the controls above it (their
// animation, the left hand's -1 X scale, ufbx's Z-up conversion on the scene root) live there.
// A child takes parentWorld^-1 * world, so any FBX inherit mode ufbx resolved is kept exactly;
// ufbx_matrix_to_transform folds a negative determinant into one negative scale axis.
//
//===========================================================================

static void JointLocals(const ufbx_scene *sc, const std::vector<uint32_t> &nodeIds, const std::vector<int> &parents,
	TRSd *outLocal, std::array<double, 16> *outWorld)
{
	for (size_t j = 0; j < nodeIds.size(); j++)
	{
		const ufbx_node *n = sc->nodes.data[nodeIds[j]];
		ufbx_matrix local;
		if (parents[j] < 0) local = n->node_to_world;
		else
		{
			const ufbx_node *p = sc->nodes.data[nodeIds[parents[j]]];
			const ufbx_matrix pinv = ufbx_matrix_invert(&p->node_to_world);
			local = ufbx_matrix_mul(&pinv, &n->node_to_world);
		}
		outLocal[j] = FromTransform(ufbx_matrix_to_transform(&local));
		if (outWorld) ToMat(n->node_to_world, outWorld[j].data());
	}
}

bool Load(const void *data, size_t size, const char *meshSelector, Result &out, std::string &err)
{
	out = Result();

	// C1-C3: load. Embedded blobs (the 19 MB of PNGs) are not kept: the engine takes textures
	// from its own files. Z up, right-handed; units untouched.
	ufbx_load_opts opts = {};
	opts.ignore_embedded = true;
	opts.generate_missing_normals = true;
	opts.target_axes = ufbx_axes_right_handed_z_up;
	opts.space_conversion = UFBX_SPACE_CONVERSION_TRANSFORM_ROOT;
	ufbx_error uerr;
	ufbx_scene *scene = ufbx_load_memory(data, size, &opts, &uerr);
	if (scene == nullptr)
	{
		char buf[1024];
		ufbx_format_error(buf, sizeof(buf), &uerr);
		err = std::string("ufbx: ") + buf;
		return false;
	}
	SceneGuard guard(scene);

	// C4: the mesh, by its node's name.
	const ufbx_mesh *mesh = nullptr;
	for (size_t i = 0; i < scene->meshes.count; i++)
	{
		const ufbx_mesh *m = scene->meshes.data[i];
		if (m->skin_deformers.count == 0) continue;
		const std::string nm = MeshName(m);
		out.diag.skinnedMeshes.push_back(nm);
		if (meshSelector && *meshSelector ? IEquals(nm.c_str(), meshSelector) : true)
		{
			if (mesh != nullptr && !(meshSelector && *meshSelector))
			{
				err = "more than one skinned mesh; name one after a colon (file.fbx:Mesh)";
				mesh = nullptr;
				break;
			}
			if (mesh == nullptr) mesh = m;
		}
	}
	if (mesh == nullptr)
	{
		if (err.empty()) err = std::string("no skinned mesh named '") + (meshSelector ? meshSelector : "") + "'";
		err += "; skinned meshes in the file:";
		for (auto &n : out.diag.skinnedMeshes) err += " " + n;
		return false;
	}
	if (mesh->instances.count != 1) { err = "the mesh is used by " + std::to_string(mesh->instances.count) + " nodes; one is expected"; return false; }
	if (mesh->skin_deformers.count != 1) { err = "the mesh has " + std::to_string(mesh->skin_deformers.count) + " skin deformers; one is expected"; return false; }
	const ufbx_node *meshNode = mesh->instances.data[0];
	const ufbx_skin_deformer *skin = mesh->skin_deformers.data[0];
	out.meshName = MeshName(mesh);
	out.diag.blendShapeDeformers = (int)mesh->blend_deformers.count;

	// C5: material and its base colour texture (file name only; the engine looks it up beside the model).
	const ufbx_material *mat = meshNode->materials.count ? meshNode->materials.data[0] : (mesh->materials.count ? mesh->materials.data[0] : nullptr);
	if (mat)
	{
		out.diag.materialName = mat->name.data;
		const ufbx_texture *tex = mat->pbr.base_color.texture ? mat->pbr.base_color.texture : mat->fbx.diffuse_color.texture;
		if (tex)
		{
			out.diag.textureFile = BaseName(tex->relative_filename.length ? tex->relative_filename : tex->filename);
			if (out.diag.textureFile.empty()) out.diag.textureFile = BaseName(tex->absolute_filename);
		}
	}

	// C6/C7: the skeleton = the nodes the skin's clusters bind to, ordered parents first by walking
	// the scene tree; each joint's parent is its nearest ancestor that is also a joint.
	std::unordered_map<uint32_t, int> isBone;	// node typed_id -> cluster index
	for (size_t c = 0; c < skin->clusters.count; c++)
	{
		const ufbx_skin_cluster *cl = skin->clusters.data[c];
		if (cl->bone_node == nullptr) { err = "a skin cluster has no bone node"; return false; }
		isBone[cl->bone_node->typed_id] = (int)c;
	}
	std::vector<uint32_t> nodeIds;
	std::vector<int> parents;
	std::unordered_map<uint32_t, int> jointOfNode;
	{
		struct Item { const ufbx_node *n; int parentJoint; };
		std::vector<Item> stack;
		stack.push_back({ scene->root_node, -1 });
		while (!stack.empty())
		{
			Item it = stack.back(); stack.pop_back();
			int pj = it.parentJoint;
			if (isBone.count(it.n->typed_id))
			{
				const int j = (int)nodeIds.size();
				nodeIds.push_back(it.n->typed_id);
				parents.push_back(pj);
				jointOfNode[it.n->typed_id] = j;
				pj = j;
			}
			for (size_t k = it.n->children.count; k-- > 0;) stack.push_back({ it.n->children.data[k], pj });
		}
	}
	if (nodeIds.size() != isBone.size()) { err = "a bone node is not under the scene root"; return false; }
	const int nj = (int)nodeIds.size();
	std::vector<int> clusterJoint(skin->clusters.count);
	for (size_t c = 0; c < skin->clusters.count; c++) clusterJoint[c] = jointOfNode[skin->clusters.data[c]->bone_node->typed_id];

	// C8: the rest pose -- the scene as stored.
	std::vector<TRSd> restLocal(nj);
	std::vector<std::array<double, 16>> restWorld(nj);
	JointLocals(scene, nodeIds, parents, restLocal.data(), restWorld.data());
	out.joints.resize(nj);
	for (int j = 0; j < nj; j++)
	{
		out.joints[j].name = scene->nodes.data[nodeIds[j]]->name.data;
		out.joints[j].parent = parents[j];
		out.joints[j].rest = restLocal[j];
		memcpy(out.joints[j].restWorld, restWorld[j].data(), sizeof(double) * 16);
		out.joints[j].restWorldTRS = FromTransform(ufbx_matrix_to_transform(&scene->nodes.data[nodeIds[j]]->node_to_world));
	}

	// C9: the skin at rest. Every cluster's world(rest) * geometry_to_bone should be one transform:
	// then the rest mesh skinned by world(frame) * world(rest)^-1 is the FBX's own skinning.
	std::vector<ufbx_matrix> skinAtRest(skin->clusters.count);
	for (size_t c = 0; c < skin->clusters.count; c++)
	{
		const ufbx_skin_cluster *cl = skin->clusters.data[c];
		skinAtRest[c] = ufbx_matrix_mul(&cl->bone_node->node_to_world, &cl->geometry_to_bone);
		if (c > 0)
			for (int k = 0; k < 4; k++)
			{
				out.diag.rebaseMaxDev = std::max(out.diag.rebaseMaxDev, fabs(skinAtRest[c].cols[k].x - skinAtRest[0].cols[k].x));
				out.diag.rebaseMaxDev = std::max(out.diag.rebaseMaxDev, fabs(skinAtRest[c].cols[k].y - skinAtRest[0].cols[k].y));
				out.diag.rebaseMaxDev = std::max(out.diag.rebaseMaxDev, fabs(skinAtRest[c].cols[k].z - skinAtRest[0].cols[k].z));
			}
	}
	out.diag.restDeterminant = skin->clusters.count ? ufbx_matrix_determinant(&skinAtRest[0]) : 1.0;

	// C10: per control point, the weights (all of them, for the rest mesh) and the rest transform.
	const size_t ncp = mesh->num_vertices;
	out.diag.controlPoints = (int)ncp;
	struct CPW { std::vector<std::pair<int, double>> w; };	// joint, weight
	std::vector<CPW> cpw(ncp);
	std::vector<ufbx_matrix> cpRest(ncp);
	for (size_t v = 0; v < ncp; v++)
	{
		ufbx_matrix acc = {};
		double tot = 0;
		if (v < skin->vertices.count)
		{
			const ufbx_skin_vertex sv = skin->vertices.data[v];
			for (uint32_t k = 0; k < sv.num_weights; k++)
			{
				const ufbx_skin_weight sw = skin->weights.data[sv.weight_begin + k];
				if (!(sw.weight > 0)) continue;
				cpw[v].w.push_back({ clusterJoint[sw.cluster_index], sw.weight });
				const ufbx_matrix &M = skinAtRest[sw.cluster_index];
				for (int c = 0; c < 4; c++)
				{
					acc.cols[c].x += M.cols[c].x * sw.weight;
					acc.cols[c].y += M.cols[c].y * sw.weight;
					acc.cols[c].z += M.cols[c].z * sw.weight;
				}
				tot += sw.weight;
			}
		}
		if (tot > 0)
		{
			for (int c = 0; c < 4; c++) { acc.cols[c].x /= tot; acc.cols[c].y /= tot; acc.cols[c].z /= tot; }
			cpRest[v] = acc;
		}
		else
		{
			out.diag.unweightedControlPoints++;
			cpRest[v] = meshNode->geometry_to_world;
		}
		out.diag.maxInfluences = std::max(out.diag.maxInfluences, (int)cpw[v].w.size());
	}
	std::vector<ufbx_vec3> cpPos(ncp);
	for (size_t v = 0; v < ncp; v++) cpPos[v] = ufbx_transform_position(&cpRest[v], mesh->vertices.data[v]);

	// C13: triangles in the file's polygon order, then made to face out at rest. A mesh skinned to a
	// mirrored skeleton turns inside out unless its file order was authored reversed for it
	// (Hand_Low1's is); the signed volume of the rest mesh says which happened, whatever the file did.
	std::vector<uint32_t> tri;	// corner (polygon-vertex) indices, outward CCW
	{
		std::vector<uint32_t> scratch(mesh->max_face_triangles * 3);
		for (size_t f = 0; f < mesh->num_faces; f++)
		{
			const uint32_t nt = ufbx_triangulate_face(scratch.data(), scratch.size(), mesh, mesh->faces.data[f]);
			tri.insert(tri.end(), scratch.begin(), scratch.begin() + nt * 3);
		}
	}
	auto cornerPos = [&](uint32_t idx) { return cpPos[mesh->vertex_indices.data[idx]]; };
	double vol = 0;
	for (size_t t = 0; t < tri.size(); t += 3)
	{
		const ufbx_vec3 a = cornerPos(tri[t]), b = cornerPos(tri[t + 1]), c = cornerPos(tri[t + 2]);
		vol += (a.x * (b.y * c.z - b.z * c.y) - a.y * (b.x * c.z - b.z * c.x) + a.z * (b.x * c.y - b.y * c.x)) / 6.0;
	}
	out.diag.signedVolume = vol;
	if (vol < 0)
	{
		out.diag.reversedForOutward = true;
		for (size_t t = 0; t < tri.size(); t += 3) std::swap(tri[t + 1], tri[t + 2]);
	}

	// C14: normals. The surface's own smooth normal per control point (area-weighted faces of the rest
	// mesh, outward) is the reference. The file's per-corner normals, carried through the rest skin
	// (inverse transpose, ufbx_matrix_for_normals), are kept if they face that surface (mean dot >=
	// NORMAL_TRUST); otherwise the surface's own are used, and both numbers are reported. Measured on
	// hand_final.fbx: each hand's stored normals are its surface normals under one rigid rotation (best
	// fit mean cosine 0.9988; Hand_Low's is 46.8 degrees about the file's up axis), i.e. stale, and they
	// face the surface by only 0.72 (Hand_Low) and 0.20 (Hand_Low1); docs/FBX_LOADER.md C14.
	std::vector<ufbx_vec3> geo(ncp, Vec3(0, 0, 0));
	for (size_t t = 0; t < tri.size(); t += 3)
	{
		const ufbx_vec3 a = cornerPos(tri[t]), b = cornerPos(tri[t + 1]), c = cornerPos(tri[t + 2]);
		const double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z, vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
		const ufbx_vec3 n = Vec3(uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx);	// |n| = 2 * area
		for (int k = 0; k < 3; k++)
		{
			ufbx_vec3 &g = geo[mesh->vertex_indices.data[tri[t + k]]];
			g.x += n.x; g.y += n.y; g.z += n.z;
		}
	}
	auto unit = [](ufbx_vec3 v) { double l = sqrt(v.x * v.x + v.y * v.y + v.z * v.z); if (l > 0) { v.x /= l; v.y /= l; v.z /= l; } return v; };
	for (auto &g : geo) g = unit(g);
	const size_t nidx = mesh->num_indices;
	std::vector<ufbx_vec3> nFile(nidx);
	double agree = 0;
	for (size_t i = 0; i < nidx; i++)
	{
		const uint32_t cp = mesh->vertex_indices.data[i];
		const ufbx_vec3 n = mesh->vertex_normal.exists ? ufbx_get_vertex_vec3(&mesh->vertex_normal, i) : geo[cp];
		const ufbx_matrix nm = ufbx_matrix_for_normals(&cpRest[cp]);
		nFile[i] = unit(ufbx_transform_direction(&nm, n));
		agree += nFile[i].x * geo[cp].x + nFile[i].y * geo[cp].y + nFile[i].z * geo[cp].z;
	}
	if (nidx) agree /= nidx;
	out.diag.normalAgreeFile = agree;
	out.diag.normalsFromSurface = !(agree >= NORMAL_TRUST);
	std::vector<ufbx_vec3> nUse(nidx);
	for (size_t i = 0; i < nidx; i++) nUse[i] = out.diag.normalsFromSurface ? geo[mesh->vertex_indices.data[i]] : nFile[i];

	// C15: 4 weights per control point -- the largest four, renormalised, as bytes summing to 255
	// (largest remainder). FModelVertex has 4 bone slots (i_modelvertexbuffer.h).
	struct Q4 { uint16_t b[4] = { 0, 0, 0, 0 }; uint8_t w[4] = { 0, 0, 0, 0 }; };
	std::vector<Q4> cpq(ncp);
	for (size_t v = 0; v < ncp; v++)
	{
		auto items = cpw[v].w;
		if (items.empty()) { cpq[v].b[0] = 0; cpq[v].w[0] = 255; continue; }	// unskinned: rides joint 0 (counted above)
		std::sort(items.begin(), items.end(), [](const std::pair<int, double> &a, const std::pair<int, double> &b)
			{ return a.second != b.second ? a.second > b.second : a.first < b.first; });
		double dropped = 0;
		for (size_t k = 4; k < items.size(); k++) dropped += items[k].second;
		if (items.size() > 4) { out.diag.verticesOver4++; items.resize(4); }
		out.diag.maxDroppedWeight = std::max(out.diag.maxDroppedWeight, dropped);
		double tot = 0;
		for (auto &it : items) tot += it.second;
		double exact[4]; int q[4]; int sum = 0;
		for (size_t k = 0; k < items.size(); k++) { exact[k] = items[k].second / tot * 255.0; q[k] = (int)floor(exact[k]); sum += q[k]; }
		int order[4] = { 0, 1, 2, 3 };
		std::sort(order, order + items.size(), [&](int a, int b) { return (exact[a] - q[a]) > (exact[b] - q[b]); });
		for (int r = 0; r < 255 - sum; r++) q[order[r % items.size()]]++;
		for (size_t k = 0; k < items.size(); k++)
		{
			cpq[v].b[k] = (uint16_t)items[k].first;
			cpq[v].w[k] = (uint8_t)q[k];
			out.diag.maxQuantError = std::max(out.diag.maxQuantError, fabs(q[k] - exact[k]) / 255.0);
		}
	}

	// C16: one vertex per polygon corner.
	out.verts.resize(nidx);
	for (size_t i = 0; i < nidx; i++)
	{
		Vertex &o = out.verts[i];
		const uint32_t cp = mesh->vertex_indices.data[i];
		o.pos[0] = (float)cpPos[cp].x; o.pos[1] = (float)cpPos[cp].y; o.pos[2] = (float)cpPos[cp].z;
		o.nrm[0] = (float)nUse[i].x; o.nrm[1] = (float)nUse[i].y; o.nrm[2] = (float)nUse[i].z;
		if (mesh->vertex_uv.exists)
		{
			const ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, i);
			o.uv[0] = (float)uv.x; o.uv[1] = (float)(1.0 - uv.y);	// FBX v runs up; texture rows run down (IQM)
		}
		else { o.uv[0] = o.uv[1] = 0.f; }
		for (int k = 0; k < 4; k++) { o.bone[k] = cpq[cp].b[k]; o.weight[k] = cpq[cp].w[k]; }
		o.controlPoint = cp;
		o.fbxIndex = (uint32_t)i;
	}

	// C17: wound as the engine's IQMs are: (b - a) x (c - a) AGAINST the outward normal in the
	// file's Z-up space (the load swap of Y and Z turns it around in render space).
	out.tris.resize(tri.size());
	for (size_t t = 0; t < tri.size(); t += 3)
	{
		out.tris[t] = tri[t];
		out.tris[t + 1] = tri[t + 2];
		out.tris[t + 2] = tri[t + 1];
	}

	// C18-C20: every take. Its frames are the file's frame rate over the span keyed on the
	// skeleton's own curves and on every node above it (the controls carry the arm and the mirror).
	std::vector<char> drives(scene->nodes.count, 0);
	for (int j = 0; j < nj; j++)
		for (const ufbx_node *n = scene->nodes.data[nodeIds[j]]; n != nullptr && !n->is_root; n = n->parent)
			drives[n->typed_id] = 1;
	double fps = scene->settings.frames_per_second;
	if (!(fps > 0)) fps = 30.0;
	std::vector<TRSd> scratch(nj);
	for (size_t s = 0; s < scene->anim_stacks.count; s++)
	{
		const ufbx_anim_stack *st = scene->anim_stacks.data[s];
		double k0 = std::numeric_limits<double>::infinity(), k1 = -k0;
		for (size_t l = 0; l < st->layers.count; l++)
		{
			const ufbx_anim_layer *L = st->layers.data[l];
			for (size_t p = 0; p < L->anim_props.count; p++)
			{
				const ufbx_anim_prop &ap = L->anim_props.data[p];
				if (ap.element == nullptr || ap.element->type != UFBX_ELEMENT_NODE) continue;
				if (!drives[((const ufbx_node *)ap.element)->typed_id]) continue;
				for (int c = 0; c < 3; c++)
				{
					const ufbx_anim_curve *cv = ap.anim_value ? ap.anim_value->curves[c] : nullptr;
					if (cv == nullptr || cv->keyframes.count == 0) continue;
					k0 = std::min(k0, cv->keyframes.data[0].time);
					k1 = std::max(k1, cv->keyframes.data[cv->keyframes.count - 1].time);
				}
			}
		}
		if (!(k1 >= k0)) continue;	// nothing in this take moves this mesh's skeleton
		Take tk;
		tk.name = st->name.data;
		tk.fps = fps;
		tk.startTime = k0;
		tk.keyFirst = k0;
		tk.keyLast = k1;
		tk.numFrames = (int)floor((k1 - k0) * fps + 0.5) + 1;
		tk.firstFrame = out.numFrames;
		for (int f = 0; f < tk.numFrames; f++)
		{
			const double t = k0 + f / fps;
			ufbx_scene *es = ufbx_evaluate_scene(scene, st->anim, t, nullptr, &uerr);
			if (es == nullptr)
			{
				char buf[1024];
				ufbx_format_error(buf, sizeof(buf), &uerr);
				err = "ufbx_evaluate_scene (" + tk.name + " frame " + std::to_string(f) + "): " + buf;
				return false;
			}
			JointLocals(es, nodeIds, parents, scratch.data(), nullptr);
			ufbx_free_scene(es);
			out.frames.insert(out.frames.end(), scratch.begin(), scratch.end());
		}
		out.numFrames += tk.numFrames;
		out.takes.push_back(tk);
	}
	if (out.numFrames == 0)
	{
		// No take moves it: one frame, the rest pose (the IQM loader does the same for an IQM
		// without animation, models_iqm.cpp:259-265).
		out.frames = restLocal;
		out.numFrames = 1;
	}
	return true;
}

} // namespace FBXSkinned
