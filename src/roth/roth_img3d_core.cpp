//
// roth_img3d_core.cpp -- see roth_img3d_core.h. Engine-free.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

// The engine's own rapidjson settings (common/engine/serializer_rapidjson.h), so every translation unit sees one
// layout of its templates.
#define RAPIDJSON_48BITPOINTER_OPTIMIZATION 0
#define RAPIDJSON_HAS_CXX11_RVALUE_REFS 1
#define RAPIDJSON_HAS_CXX11_RANGE_FOR 1
#include "rapidjson/document.h"

#include "roth_img3d_core.h"

#include <algorithm>
#include <math.h>
#include <string.h>

namespace roth
{
namespace img3d
{

namespace
{

constexpr double kPi = 3.14159265358979323846;

bool Num(const rapidjson::Value &v, double &out)
{
	if (!v.IsNumber()) return false;
	out = v.GetDouble();
	return isfinite(out);
}

bool Vec3(const rapidjson::Value &obj, const char *key, double out[3], bool &bad)
{
	auto it = obj.FindMember(key);
	if (it == obj.MemberEnd()) return false;
	const rapidjson::Value &a = it->value;
	if (!a.IsArray() || a.Size() != 3) { bad = true; return false; }
	for (unsigned i = 0; i < 3; i++) if (!Num(a[i], out[i])) { bad = true; return false; }
	return true;
}

} // namespace

const char *HoldName(Hold h)
{
	switch (h)
	{
	case HOLD_PALM: return "palm";
	case HOLD_FIST: return "fist";
	case HOLD_HANG: return "hang";
	}
	return "?";
}

bool ParseManifest(const char *json, size_t length, std::vector<Entry> &out, std::string &err, std::vector<std::string> *skipped)
{
	out.clear();
	rapidjson::Document doc;
	doc.Parse(json, length);
	if (doc.HasParseError()) { err = "not JSON (rapidjson error " + std::to_string((int)doc.GetParseError()) + " at byte " + std::to_string(doc.GetErrorOffset()) + ")"; return false; }
	if (!doc.IsObject()) { err = "the top level is not an object"; return false; }
	auto ver = doc.FindMember("version");
	if (ver == doc.MemberEnd() || !ver->value.IsInt() || ver->value.GetInt() != 1) { err = "no \"version\": 1"; return false; }
	auto items = doc.FindMember("items");
	if (items == doc.MemberEnd() || !items->value.IsArray()) { err = "no \"items\" array"; return false; }
	auto skip = [&](unsigned i, const std::string &why) { if (skipped) skipped->push_back("entry " + std::to_string(i) + ": " + why); };
	for (unsigned i = 0; i < items->value.Size(); i++)
	{
		const rapidjson::Value &e = items->value[i];
		if (!e.IsObject()) { skip(i, "not an object"); continue; }
		Entry r;
		auto id = e.FindMember("item");
		if (id == e.MemberEnd() || !id->value.IsInt() || id->value.GetInt() <= 0) { skip(i, "no positive \"item\""); continue; }
		r.item = id->value.GetInt();
		auto file = e.FindMember("file");
		if (file == e.MemberEnd() || !file->value.IsString() || file->value.GetStringLength() == 0) { skip(i, "item " + std::to_string(r.item) + ": no \"file\""); continue; }
		r.file = file->value.GetString();
		if (r.file.find("..") != std::string::npos) { skip(i, "item " + std::to_string(r.item) + ": \"file\" leaves the manifest's folder"); continue; }
		auto name = e.FindMember("name");
		if (name != e.MemberEnd() && name->value.IsString()) r.name = name->value.GetString();
		auto metres = e.FindMember("metres");
		if (metres != e.MemberEnd() && (!Num(metres->value, r.metres) || r.metres <= 0.0 || r.metres > 10.0))
		{ skip(i, "item " + std::to_string(r.item) + ": \"metres\" is not between 0 and 10"); continue; }
		auto size = e.FindMember("size");
		r.sizeFromTable = size != e.MemberEnd() && size->value.IsString() && strcmp(size->value.GetString(), "table") == 0;
		bool bad = false;
		Vec3(e, "grip", r.grip, bad);
		Vec3(e, "grip_rot", r.gripRot, bad);
		if (bad) { skip(i, "item " + std::to_string(r.item) + ": \"grip\" / \"grip_rot\" must be three numbers"); continue; }
		auto hold = e.FindMember("hold");
		if (hold != e.MemberEnd())
		{
			const char *h = hold->value.IsString() ? hold->value.GetString() : "";
			if (!strcmp(h, "palm")) r.hold = HOLD_PALM;
			else if (!strcmp(h, "fist")) r.hold = HOLD_FIST;
			else if (!strcmp(h, "hang")) r.hold = HOLD_HANG;
			else { skip(i, "item " + std::to_string(r.item) + ": \"hold\" is not palm, fist or hang"); continue; }
		}
		auto wy = e.FindMember("world_yaw");
		if (wy != e.MemberEnd() && !Num(wy->value, r.worldYaw)) { skip(i, "item " + std::to_string(r.item) + ": \"world_yaw\" is not a number"); continue; }
		bool dup = false;
		for (const Entry &o : out) if (o.item == r.item) dup = true;
		if (dup) { skip(i, "item " + std::to_string(r.item) + ": listed twice (the first is kept)"); continue; }
		out.push_back(r);
	}
	return true;
}

float Mesh::Longest() const
{
	float l = 0.f;
	for (int k = 0; k < 3; k++) if (hi[k] - lo[k] > l) l = hi[k] - lo[k];
	return l;
}

//--------------------------------------------------------------------------
// GLB
//--------------------------------------------------------------------------

namespace
{

struct M4 { double m[16]; };   // column-major, as glTF

M4 Identity() { M4 r; for (int i = 0; i < 16; i++) r.m[i] = (i % 5 == 0) ? 1.0 : 0.0; return r; }

M4 Mul(const M4 &a, const M4 &b)
{
	M4 r;
	for (int c = 0; c < 4; c++)
		for (int row = 0; row < 4; row++)
		{
			double s = 0;
			for (int k = 0; k < 4; k++) s += a.m[k * 4 + row] * b.m[c * 4 + k];
			r.m[c * 4 + row] = s;
		}
	return r;
}

struct Glb
{
	const rapidjson::Document *doc = nullptr;
	const uint8_t *bin = nullptr;
	size_t binSize = 0;
	std::string err;

	const rapidjson::Value *Arr(const char *key) const
	{
		auto it = doc->FindMember(key);
		return (it != doc->MemberEnd() && it->value.IsArray()) ? &it->value : nullptr;
	}
	const rapidjson::Value *At(const char *key, int i) const
	{
		const rapidjson::Value *a = Arr(key);
		if (a == nullptr || i < 0 || (unsigned)i >= a->Size() || !(*a)[i].IsObject()) return nullptr;
		return &(*a)[i];
	}
	static int Int(const rapidjson::Value &o, const char *key, int def)
	{
		auto it = o.FindMember(key);
		return (it != o.MemberEnd() && it->value.IsInt()) ? it->value.GetInt() : def;
	}

	// A bufferView's bytes inside the BIN chunk.
	bool View(int index, const uint8_t *&p, size_t &len, int &stride)
	{
		const rapidjson::Value *v = At("bufferViews", index);
		if (v == nullptr) { err = "bufferView " + std::to_string(index) + " missing"; return false; }
		if (Int(*v, "buffer", 0) != 0) { err = "a bufferView names a buffer other than the GLB's own"; return false; }
		const int off = Int(*v, "byteOffset", 0), n = Int(*v, "byteLength", -1);
		stride = Int(*v, "byteStride", 0);
		if (off < 0 || n < 0 || (size_t)off + (size_t)n > binSize) { err = "bufferView " + std::to_string(index) + " outside the BIN chunk"; return false; }
		p = bin + off;
		len = (size_t)n;
		return true;
	}

	// An accessor, as doubles, `comps` per element.
	bool Read(int index, int wantComps, std::vector<double> &out)
	{
		out.clear();
		const rapidjson::Value *a = At("accessors", index);
		if (a == nullptr) { err = "accessor " + std::to_string(index) + " missing"; return false; }
		if (a->HasMember("sparse")) { err = "sparse accessors are not read"; return false; }
		const int ct = Int(*a, "componentType", 0), count = Int(*a, "count", -1);
		auto ty = a->FindMember("type");
		const char *type = (ty != a->MemberEnd() && ty->value.IsString()) ? ty->value.GetString() : "";
		const int comps = !strcmp(type, "SCALAR") ? 1 : !strcmp(type, "VEC2") ? 2 : !strcmp(type, "VEC3") ? 3 : !strcmp(type, "VEC4") ? 4 : 0;
		if (comps == 0 || comps < wantComps || count < 0) { err = "accessor " + std::to_string(index) + " has type " + type; return false; }
		int size = 0;
		switch (ct)
		{
		case 5120: case 5121: size = 1; break;
		case 5122: case 5123: size = 2; break;
		case 5125: case 5126: size = 4; break;
		default: err = "accessor " + std::to_string(index) + ": component type " + std::to_string(ct); return false;
		}
		auto nm = a->FindMember("normalized");
		const bool normalized = nm != a->MemberEnd() && nm->value.IsBool() && nm->value.GetBool();
		auto bvi = a->FindMember("bufferView");
		if (bvi == a->MemberEnd()) { out.assign((size_t)count * wantComps, 0.0); return true; }   // all zeros (glTF 3.6.2.3)
		const uint8_t *p; size_t len; int stride;
		if (!bvi->value.IsInt() || !View(bvi->value.GetInt(), p, len, stride)) { if (err.empty()) err = "bad bufferView"; return false; }
		const int off = Int(*a, "byteOffset", 0);
		const int elem = size * comps;
		if (stride == 0) stride = elem;
		if (off < 0 || stride < elem) { err = "accessor " + std::to_string(index) + ": bad offset or stride"; return false; }
		if (count > 0 && (size_t)off + (size_t)stride * (count - 1) + elem > len) { err = "accessor " + std::to_string(index) + " runs past its bufferView"; return false; }
		out.resize((size_t)count * wantComps);
		for (int i = 0; i < count; i++)
		{
			const uint8_t *e = p + off + (size_t)stride * i;
			for (int c = 0; c < wantComps; c++)
			{
				const uint8_t *q = e + c * size;
				double v = 0;
				switch (ct)
				{
				case 5120: { int8_t x; memcpy(&x, q, 1); v = normalized ? std::max(x / 127.0, -1.0) : x; break; }
				case 5121: { v = normalized ? q[0] / 255.0 : q[0]; break; }
				case 5122: { int16_t x; memcpy(&x, q, 2); v = normalized ? std::max(x / 32767.0, -1.0) : x; break; }
				case 5123: { uint16_t x; memcpy(&x, q, 2); v = normalized ? x / 65535.0 : x; break; }
				case 5125: { uint32_t x; memcpy(&x, q, 4); v = x; break; }
				case 5126: { float x; memcpy(&x, q, 4); v = x; break; }
				}
				out[(size_t)i * wantComps + c] = v;
			}
		}
		return true;
	}

	M4 NodeMatrix(const rapidjson::Value &n)
	{
		M4 r = Identity();
		auto mx = n.FindMember("matrix");
		if (mx != n.MemberEnd() && mx->value.IsArray() && mx->value.Size() == 16)
		{
			for (int i = 0; i < 16; i++) r.m[i] = mx->value[i].IsNumber() ? mx->value[i].GetDouble() : r.m[i];
			return r;
		}
		double t[3] = { 0, 0, 0 }, q[4] = { 0, 0, 0, 1 }, s[3] = { 1, 1, 1 };
		auto get = [&](const char *k, double *d, unsigned n4) {
			auto it = n.FindMember(k);
			if (it != n.MemberEnd() && it->value.IsArray() && it->value.Size() == n4)
				for (unsigned i = 0; i < n4; i++) if (it->value[i].IsNumber()) d[i] = it->value[i].GetDouble();
		};
		get("translation", t, 3);
		get("rotation", q, 4);
		get("scale", s, 3);
		const double x = q[0], y = q[1], z = q[2], w = q[3];
		const double R[9] = {   // column-major
			1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
			2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
			2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y) };
		for (int c = 0; c < 3; c++)
			for (int row = 0; row < 3; row++) r.m[c * 4 + row] = R[c * 3 + row] * s[c];
		r.m[12] = t[0]; r.m[13] = t[1]; r.m[14] = t[2];
		return r;
	}

	bool Primitive(const rapidjson::Value &prim, const M4 &xf, Mesh &out)
	{
		const int mode = Int(prim, "mode", 4);
		if (mode != 4) return true;   // points / lines / strips: not a surface this reader takes; skipped
		auto attrs = prim.FindMember("attributes");
		if (attrs == prim.MemberEnd() || !attrs->value.IsObject()) { err = "a primitive has no attributes"; return false; }
		const int ip = Int(attrs->value, "POSITION", -1), in = Int(attrs->value, "NORMAL", -1), it = Int(attrs->value, "TEXCOORD_0", -1);
		if (ip < 0) { err = "a primitive has no POSITION"; return false; }
		std::vector<double> P, N, T, I;
		if (!Read(ip, 3, P)) return false;
		const size_t n = P.size() / 3;
		if (in >= 0 && (!Read(in, 3, N) || N.size() != n * 3)) { if (err.empty()) err = "NORMAL count differs from POSITION"; return false; }
		if (it >= 0 && (!Read(it, 2, T) || T.size() != n * 2)) { if (err.empty()) err = "TEXCOORD_0 count differs from POSITION"; return false; }
		const int ii = Int(prim, "indices", -1);
		if (ii >= 0) { if (!Read(ii, 1, I)) return false; }
		else { I.resize(n); for (size_t k = 0; k < n; k++) I[k] = (double)k; }
		if (I.size() % 3 != 0) { err = "a triangle list whose index count is not a multiple of 3"; return false; }

		// A file mixing primitives with and without normals or UVs: keep the arrays parallel.
		const size_t base = out.pos.size() / 3;
		if (base > 0 && out.nrm.empty() != N.empty())
		{
			if (out.nrm.empty()) out.nrm.assign(base * 3, 0.f); else N.assign(n * 3, 0.0);
		}
		if (base > 0 && out.uv.empty() != T.empty())
		{
			if (out.uv.empty()) out.uv.assign(base * 2, 0.f); else T.assign(n * 2, 0.0);
		}
		const double *m = xf.m;
		for (size_t k = 0; k < n; k++)
		{
			const double x = P[k * 3], y = P[k * 3 + 1], z = P[k * 3 + 2];
			out.pos.push_back((float)(m[0] * x + m[4] * y + m[8] * z + m[12]));
			out.pos.push_back((float)(m[1] * x + m[5] * y + m[9] * z + m[13]));
			out.pos.push_back((float)(m[2] * x + m[6] * y + m[10] * z + m[14]));
			if (!N.empty())
			{
				// The node's turn only (a uniform scale renormalises away; a shear would need the inverse transpose,
				// which post.py never writes).
				const double a = N[k * 3], b = N[k * 3 + 1], c = N[k * 3 + 2];
				double nx = m[0] * a + m[4] * b + m[8] * c, ny = m[1] * a + m[5] * b + m[9] * c, nz = m[2] * a + m[6] * b + m[10] * c;
				const double l = sqrt(nx * nx + ny * ny + nz * nz);
				if (l > 0) { nx /= l; ny /= l; nz /= l; }
				out.nrm.push_back((float)nx); out.nrm.push_back((float)ny); out.nrm.push_back((float)nz);
			}
			if (!T.empty()) { out.uv.push_back((float)T[k * 2]); out.uv.push_back((float)T[k * 2 + 1]); }
		}
		for (double v : I)
		{
			if (v < 0 || v >= (double)n) { err = "an index past the primitive's vertices"; return false; }
			out.idx.push_back((uint32_t)(base + (size_t)v));
		}
		// The picture: the first primitive's material's base colour.
		if (out.primitives == 0) Picture(Int(prim, "material", -1), out);
		out.primitives++;
		return err.empty();
	}

	void Picture(int material, Mesh &out)
	{
		const rapidjson::Value *mat = At("materials", material);
		if (mat == nullptr) return;
		auto pbr = mat->FindMember("pbrMetallicRoughness");
		if (pbr == mat->MemberEnd() || !pbr->value.IsObject()) return;
		auto bct = pbr->value.FindMember("baseColorTexture");
		if (bct == pbr->value.MemberEnd() || !bct->value.IsObject()) return;
		const rapidjson::Value *tex = At("textures", Int(bct->value, "index", -1));
		if (tex == nullptr) return;
		const rapidjson::Value *img = At("images", Int(*tex, "source", -1));
		if (img == nullptr) return;
		const int bv = Int(*img, "bufferView", -1);
		const uint8_t *p; size_t len; int stride;
		if (bv < 0 || !View(bv, p, len, stride)) { err.clear(); return; }   // a URI picture: not embedded, none
		out.image.assign(p, p + len);
		auto mime = img->FindMember("mimeType");
		out.imageMime = (mime != img->MemberEnd() && mime->value.IsString()) ? mime->value.GetString() : "";
	}

	bool Node(int index, const M4 &parent, Mesh &out, int depth)
	{
		if (depth > 64) { err = "the node tree is deeper than 64 (a cycle?)"; return false; }
		const rapidjson::Value *n = At("nodes", index);
		if (n == nullptr) { err = "node " + std::to_string(index) + " missing"; return false; }
		const M4 xf = Mul(parent, NodeMatrix(*n));
		const int mesh = Int(*n, "mesh", -1);
		if (mesh >= 0 && !MeshAt(mesh, xf, out)) return false;
		auto ch = n->FindMember("children");
		if (ch != n->MemberEnd() && ch->value.IsArray())
			for (auto &c : ch->value.GetArray()) if (c.IsInt() && !Node(c.GetInt(), xf, out, depth + 1)) return false;
		return true;
	}

	bool MeshAt(int index, const M4 &xf, Mesh &out)
	{
		const rapidjson::Value *m = At("meshes", index);
		if (m == nullptr) { err = "mesh " + std::to_string(index) + " missing"; return false; }
		auto prims = m->FindMember("primitives");
		if (prims == m->MemberEnd() || !prims->value.IsArray()) { err = "mesh " + std::to_string(index) + " has no primitives"; return false; }
		for (auto &p : prims->value.GetArray()) if (!p.IsObject() || !Primitive(p, xf, out)) { if (err.empty()) err = "a bad primitive"; return false; }
		return true;
	}
};

uint32_t U32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

} // namespace

bool ReadGlb(const uint8_t *data, size_t size, Mesh &out, std::string &err)
{
	out = Mesh();
	if (data == nullptr || size < 20) { err = "shorter than a GLB header"; return false; }
	if (U32(data) != 0x46546C67u) { err = "not a GLB (no 'glTF' magic)"; return false; }
	if (U32(data + 4) != 2) { err = "GLB version " + std::to_string(U32(data + 4)) + " (2 is read)"; return false; }
	const size_t total = U32(data + 8);
	if (total > size) { err = "the GLB says it is longer than the file"; return false; }
	const uint8_t *json = nullptr, *bin = nullptr;
	size_t jsonLen = 0, binLen = 0;
	for (size_t at = 12; at + 8 <= total;)
	{
		const size_t len = U32(data + at);
		const uint32_t type = U32(data + at + 4);
		if (at + 8 + len > total) { err = "a chunk runs past the file"; return false; }
		if (type == 0x4E4F534Au && json == nullptr) { json = data + at + 8; jsonLen = len; }
		else if (type == 0x004E4942u && bin == nullptr) { bin = data + at + 8; binLen = len; }
		at += 8 + len;
	}
	if (json == nullptr) { err = "no JSON chunk"; return false; }
	rapidjson::Document doc;
	doc.Parse((const char *)json, jsonLen);
	if (doc.HasParseError() || !doc.IsObject()) { err = "the JSON chunk does not parse"; return false; }
	auto asset = doc.FindMember("asset");
	if (asset == doc.MemberEnd() || !asset->value.IsObject()) { err = "no asset block"; return false; }
	auto ver = asset->value.FindMember("version");
	if (ver == asset->value.MemberEnd() || !ver->value.IsString() || ver->value.GetString()[0] != '2') { err = "not glTF 2.x"; return false; }
	auto ext = doc.FindMember("extensionsRequired");
	if (ext != doc.MemberEnd() && ext->value.IsArray() && ext->value.Size() > 0)
	{
		err = "the file requires extensions this reader does not take (";
		for (auto &e : ext->value.GetArray()) if (e.IsString()) { err += e.GetString(); err += " "; }
		err += ")";
		return false;
	}

	Glb g;
	g.doc = &doc;
	g.bin = bin;
	g.binSize = binLen;
	const M4 id = Identity();
	const rapidjson::Value *scenes = g.Arr("scenes");
	if (scenes != nullptr && scenes->Size() > 0)
	{
		int s = Glb::Int(doc, "scene", 0);
		const rapidjson::Value *scene = g.At("scenes", s);
		if (scene == nullptr) { err = "scene " + std::to_string(s) + " missing"; return false; }
		auto nodes = scene->FindMember("nodes");
		if (nodes != scene->MemberEnd() && nodes->value.IsArray())
			for (auto &n : nodes->value.GetArray()) if (n.IsInt() && !g.Node(n.GetInt(), id, out, 0)) { err = g.err; return false; }
	}
	else if (const rapidjson::Value *meshes = g.Arr("meshes"))
	{
		for (unsigned m = 0; m < meshes->Size(); m++) if (!g.MeshAt((int)m, id, out)) { err = g.err; return false; }
	}
	if (out.idx.empty()) { err = "no triangles"; return false; }
	for (int k = 0; k < 3; k++) { out.lo[k] = 1e30f; out.hi[k] = -1e30f; }
	for (size_t v = 0; v < out.Vertices(); v++)
		for (int k = 0; k < 3; k++)
		{
			const float c = out.pos[v * 3 + k];
			if (!isfinite(c)) { err = "a vertex that is not a number"; return false; }
			if (c < out.lo[k]) out.lo[k] = c;
			if (c > out.hi[k]) out.hi[k] = c;
		}
	return true;
}

//--------------------------------------------------------------------------
// Placement
//--------------------------------------------------------------------------

void WorldDirection(const float g[3], float out[3])
{
	// Doom: forward = glTF +z, left = glTF +x, up = glTF +y; the model space writes Doom (x, z, y).
	out[0] = g[2];
	out[1] = g[1];
	out[2] = g[0];
}

void WorldPoint(const float g[3], double scaleH, double scaleV, float out[3])
{
	out[0] = (float)(g[2] * scaleH);
	out[1] = (float)(g[1] * scaleV);
	out[2] = (float)(g[0] * scaleH);
}

namespace
{

// The grip turn: yaw about +Y, then pitch about +X, then roll about +Z, applied roll first (R = Ry * Rx * Rz).
void GripTurn(const Entry &e, const double v[3], double out[3])
{
	const double y = e.gripRot[0] * kPi / 180.0, p = e.gripRot[1] * kPi / 180.0, r = e.gripRot[2] * kPi / 180.0;
	double a[3] = { v[0] * cos(r) - v[1] * sin(r), v[0] * sin(r) + v[1] * cos(r), v[2] };   // about Z
	double b[3] = { a[0], a[1] * cos(p) - a[2] * sin(p), a[1] * sin(p) + a[2] * cos(p) };   // about X
	out[0] = b[0] * cos(y) + b[2] * sin(y);                                                // about Y
	out[1] = b[1];
	out[2] = -b[0] * sin(y) + b[2] * cos(y);
}

// glTF axes -> hand frame, by the hold (roth_img3d_core.h). Columns: where glTF x, y, z go.
void HoldTurn(Hold hold, bool right, const double v[3], float out[3])
{
	// x = left, y = up, z = front
	double hx = 0, hy = 0, hz = 0;
	if (hold == HOLD_FIST)
	{
		const double t = right ? -1.0 : 1.0;   // the thumb's side of Z
		// up -> thumb (t Z), front -> +X, left -> up x front = (tZ) x X = t (Z x X) = t Y
		hx = v[2];
		hy = t * v[0];
		hz = t * v[1];
	}
	else
	{
		// up -> -Y (out of the palm), front -> +X, left -> (-Y) x X = +Z
		hx = v[2];
		hy = -v[1];
		hz = v[0];
	}
	out[0] = (float)hx;
	out[1] = (float)hy;
	out[2] = (float)hz;
}

} // namespace

void HandPoint(const Entry &e, bool rightHand, const float g[3], float out[3])
{
	const double d[3] = { g[0] - e.grip[0], g[1] - e.grip[1], g[2] - e.grip[2] };
	double t[3];
	GripTurn(e, d, t);
	HoldTurn(e.hold, rightHand, t, out);
}

void HandDirection(const Entry &e, bool rightHand, const float g[3], float out[3])
{
	const double d[3] = { g[0], g[1], g[2] };
	double t[3];
	GripTurn(e, d, t);
	HoldTurn(e.hold, rightHand, t, out);
}

} // namespace img3d
} // namespace roth
