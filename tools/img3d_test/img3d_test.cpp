// img3d_test.cpp -- the item models' reader (src/roth/roth_img3d_core.cpp) and the engine's picture decoder for them
// (src/common/models/fbx_image.h, the engine's own stb_image), run engine-free on procedural files only:
// tools/img3d/make_placeholder.py's boxes and GLBs built here. No retail art.
//
//   tools/img3d_test/build.sh            builds, writes the boxes into /tmp/img3d_test, runs this
//   img3d_test <plain boxes folder> <boxes-with-a-node folder>

#define STB_IMAGE_IMPLEMENTATION
#include "fbx_image.h"
#include "roth_img3d_core.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>

using namespace roth::img3d;

static int g_fail = 0, g_pass = 0;
static void Check(bool ok, const std::string &what)
{
	if (ok) g_pass++;
	else { g_fail++; printf("FAIL %s\n", what.c_str()); }
}

static std::vector<uint8_t> ReadAll(const std::string &p)
{
	std::vector<uint8_t> b;
	FILE *f = fopen(p.c_str(), "rb");
	if (!f) return b;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	b.resize(n > 0 ? (size_t)n : 0);
	if (n > 0 && fread(b.data(), 1, (size_t)n, f) != (size_t)n) b.clear();
	fclose(f);
	return b;
}

static bool Near(double a, double b, double e = 1e-5) { return fabs(a - b) <= e; }

// A GLB from a JSON text and a BIN blob.
static std::vector<uint8_t> MakeGlb(const std::string &json, const std::vector<uint8_t> &bin)
{
	std::string js = json;
	while (js.size() % 4) js += ' ';
	std::vector<uint8_t> b = bin;
	while (b.size() % 4) b.push_back(0);
	std::vector<uint8_t> out;
	auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) out.push_back((uint8_t)(v >> (8 * i))); };
	u32(0x46546C67u); u32(2); u32((uint32_t)(12 + 8 + js.size() + (b.empty() ? 0 : 8 + b.size())));
	u32((uint32_t)js.size()); u32(0x4E4F534Au);
	out.insert(out.end(), js.begin(), js.end());
	if (!b.empty()) { u32((uint32_t)b.size()); u32(0x004E4942u); out.insert(out.end(), b.begin(), b.end()); }
	return out;
}

static void TestManifest(const std::string &dir)
{
	const std::vector<uint8_t> m = ReadAll(dir + "/manifest.json");
	Check(!m.empty(), "manifest.json read");
	std::vector<Entry> e;
	std::string err;
	std::vector<std::string> skipped;
	Check(ParseManifest((const char *)m.data(), m.size(), e, err, &skipped), "manifest parses: " + err);
	Check(e.size() == 3 && skipped.empty(), "three entries, none skipped");
	if (e.size() != 3) return;
	Check(e[0].item == 16 && e[0].file == "16_placeholder.glb" && e[0].hold == HOLD_PALM && e[0].sizeFromTable, "entry 16");
	Check(Near(e[0].metres, 0.055) && Near(e[0].grip[1], -0.025), "entry 16 size and grip");
	Check(e[1].item == 21 && e[1].hold == HOLD_FIST && Near(e[1].grip[1], 0.003), "entry 21");
	Check(e[2].item == 17 && Near(e[2].metres, 0.2), "entry 17");

	// The bad ones.
	struct Bad { const char *json; bool parses; int entries; int skips; const char *what; };
	const Bad bad[] = {
		{ "not json", false, 0, 0, "not JSON" },
		{ "{\"items\": []}", false, 0, 0, "no version" },
		{ "{\"version\": 2, \"items\": []}", false, 0, 0, "version 2" },
		{ "{\"version\": 1}", false, 0, 0, "no items" },
		{ "{\"version\": 1, \"items\": [{\"item\": 5}]}", true, 0, 1, "no file" },
		{ "{\"version\": 1, \"items\": [{\"item\": 5, \"file\": \"../x.glb\"}]}", true, 0, 1, "file leaves the folder" },
		{ "{\"version\": 1, \"items\": [{\"item\": 5, \"file\": \"a.glb\", \"hold\": \"pocket\"}]}", true, 0, 1, "unknown hold" },
		{ "{\"version\": 1, \"items\": [{\"item\": 5, \"file\": \"a.glb\", \"grip\": [1, 2]}]}", true, 0, 1, "grip of two" },
		{ "{\"version\": 1, \"items\": [{\"item\": 5, \"file\": \"a.glb\", \"metres\": -1}]}", true, 0, 1, "negative metres" },
		{ "{\"version\": 1, \"items\": [{\"item\": 5, \"file\": \"a.glb\"}, {\"item\": 5, \"file\": \"b.glb\"}]}", true, 1, 1, "an item twice" },
		{ "{\"version\": 1, \"items\": [{\"item\": 0, \"file\": \"a.glb\"}, 7, {\"item\": 6, \"file\": \"b.glb\", \"extra\": true}]}", true, 1, 2, "mixed" },
	};
	for (const Bad &b : bad)
	{
		std::vector<Entry> o;
		std::vector<std::string> sk;
		std::string er;
		const bool p = ParseManifest(b.json, strlen(b.json), o, er, &sk);
		Check(p == b.parses && (int)o.size() == b.entries && (int)sk.size() == b.skips, std::string("manifest case: ") + b.what + " (" + er + ")");
	}
	// Defaults.
	{
		std::vector<Entry> o;
		const char *j = "{\"version\": 1, \"items\": [{\"item\": 9, \"file\": \"9.glb\"}]}";
		Check(ParseManifest(j, strlen(j), o, err) && o.size() == 1 && o[0].hold == HOLD_PALM && !o[0].sizeFromTable && o[0].metres == 0.0
			&& o[0].grip[0] == 0 && o[0].worldYaw == 0, "defaults: palm, sized to the picture, grip at the origin");
	}
}

static void TestBox(const std::string &path, double sx, double sy, double sz, const uint8_t colA[3])
{
	const std::vector<uint8_t> b = ReadAll(path);
	Mesh m;
	std::string err;
	Check(ReadGlb(b.data(), b.size(), m, err), path + " reads: " + err);
	Check(m.Vertices() == 24 && m.Triangles() == 12 && m.nrm.size() == 72 && m.uv.size() == 48 && m.primitives == 1, path + ": 24 vertices, 12 triangles");
	Check(Near(m.lo[0], -sx / 2) && Near(m.hi[0], sx / 2) && Near(m.lo[1], 0) && Near(m.hi[1], sy) && Near(m.lo[2], -sz / 2) && Near(m.hi[2], sz / 2), path + ": bounds");
	Check(Near(m.Longest(), std::max(sx, std::max(sy, sz))), path + ": longest side");
	Check(m.imageMime == "image/png" && !m.image.empty(), path + ": an embedded PNG");
	// The picture through the engine's own decoder.
	FBXSkinned::ImageInfo info;
	Check(FBXSkinned::ImageInfoOf(m.image.data(), m.image.size(), info) && info.width == 64 && info.height == 64 && info.bits == 8, path + ": 64 x 64, 8 bits");
	int w = 0, h = 0;
	std::vector<uint8_t> rgba;
	Check(FBXSkinned::DecodeRGBA8(m.image.data(), m.image.size(), w, h, rgba) && w == 64 && h == 64, path + ": decodes");
	if (rgba.size() == 64 * 64 * 4)
	{
		const uint8_t *g = &rgba[(10 * 64 + 40) * 4], *a = &rgba[(1 * 64 + 1) * 4];
		Check(g[0] == 40 && g[1] == 200 && g[2] == 60 && g[3] == 255, path + ": the front's green at (40, 10)");
		Check(a[0] == colA[0] && a[1] == colA[1] && a[2] == colA[2], path + ": the first check's colour at (1, 1)");
	}
	// Every front-face vertex (normal +Z) maps its uv into the right (green) half.
	bool front = true;
	int frontCount = 0;
	for (size_t v = 0; v < m.Vertices(); v++)
		if (m.nrm[v * 3 + 2] > 0.9f) { frontCount++; if (m.uv[v * 2] < 0.5f - 1e-6f) front = false; }
	Check(frontCount == 4 && front, path + ": the front face (+Z) is textured from the green half");
}

static std::vector<std::vector<float>> SortedVerts(const Mesh &m)
{
	std::vector<std::vector<float>> v;
	for (size_t i = 0; i < m.Vertices(); i++)
	{
		// Rounded to 0.1 mm (and normals to 1e-4) so the sort does not split on float noise or a signed zero.
		std::vector<float> r;
		for (int k = 0; k < 3; k++) r.push_back(roundf(m.pos[i * 3 + k] * 1e4f) / 1e4f + 0.f);
		for (int k = 0; k < 3; k++) r.push_back(roundf(m.nrm[i * 3 + k] * 1e4f) / 1e4f + 0.f);
		v.push_back(r);
	}
	std::sort(v.begin(), v.end());
	return v;
}

static void TestNode(const std::string &plain, const std::string &moved)
{
	const std::vector<uint8_t> a = ReadAll(plain), b = ReadAll(moved);
	Mesh ma, mb;
	std::string err;
	Check(ReadGlb(a.data(), a.size(), ma, err) && ReadGlb(b.data(), b.size(), mb, err), "the node test's two files read: " + err);
	const auto va = SortedVerts(ma), vb = SortedVerts(mb);
	bool same = va.size() == vb.size();
	for (size_t i = 0; same && i < va.size(); i++)
		for (int k = 0; k < 6; k++) if (fabs(va[i][k] - vb[i][k]) > 1e-5f) same = false;
	if (!same && !va.empty())
		for (size_t i = 0; i < 4 && i < va.size(); i++)
			printf("  plain %.4f %.4f %.4f n %.2f %.2f %.2f | node %.4f %.4f %.4f n %.2f %.2f %.2f\n", va[i][0], va[i][1], va[i][2], va[i][3], va[i][4], va[i][5],
				vb[i][0], vb[i][1], vb[i][2], vb[i][3], vb[i][4], vb[i][5]);
	Check(same, "a node's translation and rotation land every vertex and normal where the plain box has them");
}

static void TestBuilt()
{
	// One triangle, uint16 indices, then the same without indices; a POSITION-only file has no normals and no UVs.
	std::vector<uint8_t> bin;
	auto f32 = [&](float v) { uint8_t t[4]; memcpy(t, &v, 4); bin.insert(bin.end(), t, t + 4); };
	f32(0); f32(0); f32(0); f32(1); f32(0); f32(0); f32(0); f32(1); f32(0);
	bin.push_back(0); bin.push_back(0); bin.push_back(1); bin.push_back(0); bin.push_back(2); bin.push_back(0); bin.push_back(0); bin.push_back(0);
	const std::string head = "{\"asset\":{\"version\":\"2.0\"},\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6}],"
		"\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],";
	Mesh m;
	std::string err;
	std::vector<uint8_t> g = MakeGlb(head + "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}]}", bin);
	Check(ReadGlb(g.data(), g.size(), m, err) && m.Triangles() == 1 && m.nrm.empty() && m.uv.empty() && m.image.empty(), "uint16 indices, no scene, no normals, no picture: " + err);
	g = MakeGlb(head + "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}]}", bin);
	Check(ReadGlb(g.data(), g.size(), m, err) && m.Triangles() == 1 && m.idx[2] == 2, "no indices: the vertices in order: " + err);
	g = MakeGlb(head + "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1,\"mode\":1}]}]}", bin);
	Check(!ReadGlb(g.data(), g.size(), m, err), "a lines-only file: no triangles");
	g = MakeGlb(head + "\"extensionsRequired\":[\"KHR_draco_mesh_compression\"],\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}]}", bin);
	Check(!ReadGlb(g.data(), g.size(), m, err) && err.find("KHR_draco") != std::string::npos, "a required extension is refused and named: " + err);
	g = MakeGlb(head + "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}]}", std::vector<uint8_t>(bin.begin(), bin.begin() + 20));
	Check(!ReadGlb(g.data(), g.size(), m, err), "a BIN chunk too short for its accessors is refused: " + err);
	// THE PARTS (film_build.py's glass, glowing liquid and cork): one part a material, a primitive sharing the last
	// part's material joins it, a primitive with none is a plain white part, and the object wears the first plain one.
	g = MakeGlb(head + "\"materials\":[{\"name\":\"glass\",\"alphaMode\":\"BLEND\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.5,0.6,0.7,0.3]}},"
		"{\"name\":\"liquid\",\"emissiveFactor\":[0.2,0.9,0.3],\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.2,0.9,0.3,1]}}],"
		"\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1,\"material\":0},{\"attributes\":{\"POSITION\":0},\"indices\":1,\"material\":1},"
		"{\"attributes\":{\"POSITION\":0},\"indices\":1,\"material\":1},{\"attributes\":{\"POSITION\":0},\"indices\":1}]}]}", bin);
	{
		const bool read = ReadGlb(g.data(), g.size(), m, err);
		Check(read && m.parts.size() == 3 && m.Triangles() == 4, "four primitives, three materials: three parts: " + err);
		if (read && m.parts.size() == 3)
		{
			const Part &a = m.parts[0], &b = m.parts[1], &c = m.parts[2];
			Check(a.name == "glass" && a.blend && !a.glow && fabs(a.color[3] - 0.3f) < 1e-6f && a.firstIndex == 0 && a.indexCount == 3 && a.image.empty(),
				"the glass: see-through at 0.3, three indices, no picture");
			Check(b.name == "liquid" && b.glow && !b.blend && b.firstIndex == 3 && b.indexCount == 6 && fabs(b.color[1] - 0.9f) < 1e-6f,
				"the liquid: glowing, its two primitives joined (six indices from 3)");
			Check(c.material == -1 && c.Plain() && c.firstIndex == 9 && c.indexCount == 3 && c.color[0] == 1.f, "no material: a plain white part");
			Check(m.Primary() == 2, "the object wears the first plain part");
		}
	}
	bin[36] = 9;   // an index past the vertices
	g = MakeGlb(head + "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}]}", bin);
	Check(!ReadGlb(g.data(), g.size(), m, err), "an index past the vertices is refused: " + err);
	// Truncated and wrong magic.
	std::vector<uint8_t> t = g;
	t.resize(30);
	Check(!ReadGlb(t.data(), t.size(), m, err), "a truncated file is refused: " + err);
	t = g;
	t[0] = 'x';
	Check(!ReadGlb(t.data(), t.size(), m, err), "a wrong magic is refused: " + err);
}

static double Det(const float a[3], const float b[3], const float c[3])
{
	return a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0]);
}

static void TestPlacement()
{
	const float X[3] = { 1, 0, 0 }, Y[3] = { 0, 1, 0 }, Z[3] = { 0, 0, 1 };
	float o[3], ox[3], oy[3], oz[3];
	// Lying: glTF front (+z) -> Doom forward = model x; up -> model y; glTF left (+x) -> Doom +Y = model z.
	WorldDirection(Z, o); Check(o[0] == 1 && o[1] == 0 && o[2] == 0, "world: the front is the actor's forward (model x)");
	WorldDirection(Y, o); Check(o[1] == 1, "world: up is the model's vertical (y)");
	WorldDirection(X, o); Check(o[2] == 1, "world: the item's left is Doom +Y (model z)");
	const float p[3] = { 0.1f, 0.2f, 0.3f };
	WorldPoint(p, 10.0, 5.0, o);
	Check(Near(o[0], 3.0) && Near(o[1], 1.0) && Near(o[2], 1.0), "world: points scale across and up separately");
	// In Doom terms the world turn is proper: forward x left = up in both frames.
	// glTF z x glTF x = glTF y (front x left = up), and Doom X x Doom Y = Doom Z.

	for (int hold = 0; hold < 2; hold++)
		for (int right = 0; right < 2; right++)
		{
			Entry e;
			e.hold = hold ? HOLD_FIST : HOLD_PALM;
			HandDirection(e, right != 0, X, ox);
			HandDirection(e, right != 0, Y, oy);
			HandDirection(e, right != 0, Z, oz);
			const std::string tag = std::string(hold ? "fist" : "palm") + (right ? ", right hand" : ", left hand");
			Check(Near(Det(ox, oy, oz), 1.0), tag + ": a proper turn (det +1, never mirrored)");
			Check(oz[0] == 1 && oz[1] == 0 && oz[2] == 0, tag + ": the front points along the fingers (+X)");
			if (!hold) Check(oy[1] == -1, tag + ": up comes out of the palm (-Y)");
			else Check(oy[2] == (right ? -1 : 1), tag + ": up runs along the thumb (-Z right, +Z left)");
		}
	// The grip point lands on the socket; the grip turn turns about it.
	Entry e;
	e.grip[0] = 0.01; e.grip[1] = 0.02; e.grip[2] = 0.03;
	const float g[3] = { 0.01f, 0.02f, 0.03f };
	HandPoint(e, true, g, o);
	Check(Near(o[0], 0) && Near(o[1], 0) && Near(o[2], 0), "the grip point is the hand's grip socket");
	e.gripRot[0] = 90;   // yaw: the front (+z) turns to +x
	const float fz[3] = { 0.01f, 0.02f, 0.03f + 1.0f };
	HandPoint(e, true, fz, o);
	float lx[3];
	Entry plain;
	HandDirection(plain, true, X, lx);
	Check(Near(o[0], lx[0]) && Near(o[1], lx[1]) && Near(o[2], lx[2]), "a 90-degree grip yaw turns the front onto the item's left (+x)");
	Check(strcmp(HoldName(HOLD_HANG), "hang") == 0, "hold names");
}

// post.py's output (tools/img3d_test/post_test.py made it from the boxes): every manifest entry reads, decodes, and
// stands on y = 0 at its stated size.
static void TestPostFolder(const std::string &dir)
{
	const std::vector<uint8_t> m = ReadAll(dir + "/manifest.json");
	std::vector<Entry> e;
	std::string err;
	Check(!m.empty() && ParseManifest((const char *)m.data(), m.size(), e, err) && e.size() == 2, "post.py's manifest parses with two entries: " + err);
	for (const Entry &x : e)
	{
		const std::vector<uint8_t> b = ReadAll(dir + "/" + x.file);
		Mesh me;
		Check(ReadGlb(b.data(), b.size(), me, err), "post.py's " + x.file + " reads: " + err);
		Check(Near(me.Longest(), x.metres, 1e-4) && Near(me.lo[1], 0, 1e-6) && x.sizeFromTable, x.file + ": the manifest's size, base on y = 0");
		int w = 0, h = 0;
		std::vector<uint8_t> rgba;
		Check(FBXSkinned::DecodeRGBA8(me.image.data(), me.image.size(), w, h, rgba) && w == 64 && h == 64, x.file + ": its picture decodes");
	}
}

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: img3d_test <boxes folder> <boxes-with-a-node folder> [post.py output folder]\n"); return 2; }
	const std::string dir = argv[1], trs = argv[2];
	TestManifest(dir);
	const uint8_t orange[3] = { 230, 120, 30 }, yellow[3] = { 230, 210, 40 }, blue[3] = { 40, 80, 230 };
	TestBox(dir + "/16_placeholder.glb", 0.055, 0.016, 0.036, orange);
	TestBox(dir + "/21_placeholder.glb", 0.060, 0.006, 0.020, yellow);
	TestBox(dir + "/17_placeholder.glb", 0.200, 0.040, 0.130, blue);
	TestBox(trs + "/17_placeholder.glb", 0.200, 0.040, 0.130, blue);
	TestNode(dir + "/17_placeholder.glb", trs + "/17_placeholder.glb");
	TestBuilt();
	TestPlacement();
	if (argc >= 4) TestPostFolder(argv[3]);
	printf("img3d_test: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
