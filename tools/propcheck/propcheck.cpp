// propcheck.cpp -- what every placed object in Realms of the Haunting IS in the
// original engine, and where this port's spawner built it differently.
//
// WHY THIS EXISTS
//
// "Trees are bad" (owner, in the headset, 2026-11-12). The trees, the chairs
// and the chandelier are 3D meshes, and the mesh path had been carrying four
// guesses since it was written: the unit square for every face's texture, flag
// bit 1 read as a vertical flip, every face drawn from both sides, and a yaw
// taken from a reading of the rotation byte that turned the wrong way. None of
// them can be seen to be wrong on one screenshot of a six-bladed fan. All of
// them can be checked against the retail data, which is what this does.
//
// It reads the player's own .RAW and .DAS through the same reader the loader
// uses -- including roth::MeshFaceTexCoords, the function the engine calls --
// and writes nothing.
//
//   propcheck <ROTH DATA folder>            every map: totals and rule checks
//   propcheck <ROTH DATA folder> STUDY1     plus every object of that map
//   propcheck <ROTH DATA folder> -mesh DEMO 4123    one mesh, face by face
//
// Build (from the repo root):
//   g++ -std=c++17 -O1 -I src/roth -o propcheck tools/propcheck/propcheck.cpp
//       src/roth/roth_raw.cpp src/roth/roth_install.cpp src/roth/roth_das.cpp
//       src/roth/roth_palette.cpp
//
// THE RULES IT CHECKS, each with where ROTH.C states it:
//
//  R1  A wall-type face's ring is top-left, top-right, bottom-right,
//      bottom-left (renderer.c:4283-4294). Checked as geometry: corners 0 and 1
//      share a height above corners 3 and 2, and each side is vertical.
//  R2  Its +0x24 word is its height and its +0x26 word its width
//      (renderer.c:13135-13136 load them, :4979-4981 divides by the first).
//  R3  A face is visible when its ring is CLOCKWISE on screen
//      (renderer.c:12307-12314). The original has a second, independent rule
//      for horizontal faces -- flags 0x10|0x20 test only the SIGN of a corner's
//      height against the eye, negated by 0x80 (renderer.c:12363-12369) -- so
//      the two must agree: a face seen only from below must have a ring that is
//      clockwise from below. Only eight retail faces use that rule, so the
//      check that carries weight is the broad one: a solid prop's faces look
//      OUT of it, so the side a clockwise ring shows should point away from
//      the middle of the mesh. If it does, the handedness this port gives the
//      mesh (x east, y north, up up, not mirrored) is the original's.
//  R4  Complementary faces. A card is stored as two faces over the same
//      corners, wound opposite ways. The original draws exactly one of the two
//      from any viewpoint; a port that draws faces double-sided draws four
//      coplanar copies. Counted, with how many pairs wear DIFFERENT texture
//      coordinates on their two sides.
//  R5  A face-indexed image pack's +0x1c byte is inside the pack's table
//      (renderer.c:13262-13266).
//  R6  The yaw. A mesh corner (x, y) lands in the world at
//          ( x cos R + y sin R,  -x sin R + y cos R ),   R = rot * 360/256
//      -- CLOCKWISE with the rotation byte (render_world.c:637-655 against the
//      view basis of renderer.c:12204-12217). A fixed-angle picture lies along
//      the same local x axis (renderer.c:6096-6110). Printed per object as the
//      bearing the port must use, beside the one it used before.

#include "roth_raw.h"
#include "roth_install.h"
#include "roth_das.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace roth;

static std::vector<uint8_t> ReadFile(const std::string &p)
{
	std::vector<uint8_t> o; FILE *f = fopen(p.c_str(), "rb"); if (!f) return o;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); o.resize((size_t)n);
	if (n > 0 && fread(o.data(), 1, (size_t)n, f) != (size_t)n) o.clear();
	fclose(f); return o;
}
static uint16_t RdU16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

struct LoadedPack
{
	std::string name;
	std::vector<uint8_t> bytes;
	Pack pack;
	bool ok = false;
};

static std::map<std::string, LoadedPack *> gPacks;

static LoadedPack *GetPack(const std::string &name)
{
	auto it = gPacks.find(name);
	if (it != gPacks.end()) return it->second;
	auto *lp = new LoadedPack;
	lp->name = name;
	lp->bytes = ReadFile(TheInstall().PackFile(name.c_str()));
	lp->ok = !lp->bytes.empty() && lp->pack.Load(lp->bytes.data(), lp->bytes.size());
	gPacks[name] = lp;
	return lp;
}

static double Norm360(double d) { d = fmod(d, 360.0); if (d < 0) d += 360.0; return d; }

// The picture a mesh face wears, as the engine will register it: across = the
// stored row count, down = the stored row length.
static bool FaceTexSize(const Pack &pack, const MeshFace &f, int &w, int &h)
{
	w = h = 0;
	if (!f.Textured() || f.texture >= 0x1200) return false;
	Image img = pack.ReadSubImage(f.texture, f.subTexture);
	if (!img.ok()) return false;
	w = img.height; h = img.width;
	return true;
}

static const char *KindName(MeshFaceKind k)
{
	switch (k)
	{
	case MeshFaceKind::Wall: return "wall";
	case MeshFaceKind::Polygon: return "polygon";
	case MeshFaceKind::Plane: return "plane";
	default: return "billboard";
	}
}

struct MeshAudit
{
	int faces = 0, wall = 0, polygon = 0, plane = 0, billboard = 0, flat = 0;
	int r1Checked = 0, r1Bad = 0;
	int r2Checked = 0, r2BadV = 0, r2BadU = 0;
	int r3Checked = 0, r3Bad = 0;
	int outward = 0, inward = 0, edgeOn = 0;
	int r1Sloped = 0, r1Degenerate = 0;
	int pairs = 0, pairsDifferentUV = 0, unpaired = 0;
	int r5Checked = 0, r5Bad = 0;
	int texMissing = 0;
	int oldUvDiffers = 0;       // faces whose corners the old unit square mapped elsewhere
	int oldSubIgnored = 0;      // faces naming a picture other than the pack's first
	int tiled = 0, mirrored = 0, packedUV = 0, storedExtents = 0;
	int minX = 0, maxX = 0, minUp = 0, maxUp = 0, minY = 0, maxY = 0;
	void Add(const MeshAudit &o)
	{
		faces += o.faces; wall += o.wall; polygon += o.polygon; plane += o.plane;
		billboard += o.billboard; flat += o.flat;
		r1Checked += o.r1Checked; r1Bad += o.r1Bad;
		r2Checked += o.r2Checked; r2BadV += o.r2BadV; r2BadU += o.r2BadU;
		r3Checked += o.r3Checked; r3Bad += o.r3Bad;
		outward += o.outward; inward += o.inward; edgeOn += o.edgeOn;
		r1Sloped += o.r1Sloped; r1Degenerate += o.r1Degenerate;
		pairs += o.pairs; pairsDifferentUV += o.pairsDifferentUV; unpaired += o.unpaired;
		r5Checked += o.r5Checked; r5Bad += o.r5Bad; texMissing += o.texMissing;
		oldUvDiffers += o.oldUvDiffers; oldSubIgnored += o.oldSubIgnored;
		tiled += o.tiled; mirrored += o.mirrored; packedUV += o.packedUV;
		storedExtents += o.storedExtents;
	}
};

// What roth_objects.cpp gave every face before 2026-11-12: the unit square in
// corner order, flipped DOWN the picture by flag bit 1.
static void OldUV(const MeshFace &f, float u[4], float v[4])
{
	static const float U[4] = { 0.f, 0.f, 1.f, 1.f };
	static const float V[4] = { 0.f, 1.f, 1.f, 0.f };
	for (int i = 0; i < 4; i++) { u[i] = U[i]; v[i] = (f.flags & 2) ? 1.f - V[i] : V[i]; }
}

static MeshAudit AuditMesh(const Pack &pack, const Mesh &m, bool verbose)
{
	MeshAudit a;
	if (!m.vertices.empty())
	{
		a.minX = a.maxX = m.vertices[0].x; a.minUp = a.maxUp = m.vertices[0].up;
		a.minY = a.maxY = m.vertices[0].y;
		for (const auto &v : m.vertices)
		{
			a.minX = std::min(a.minX, (int)v.x); a.maxX = std::max(a.maxX, (int)v.x);
			a.minUp = std::min(a.minUp, (int)v.up); a.maxUp = std::max(a.maxUp, (int)v.up);
			a.minY = std::min(a.minY, (int)v.y); a.maxY = std::max(a.maxY, (int)v.y);
		}
	}

	std::map<std::vector<int>, std::vector<int>> byCorners;     // sorted corners -> face indices

	for (size_t fi = 0; fi < m.faces.size(); fi++)
	{
		const MeshFace &f = m.faces[fi];
		a.faces++;
		const MeshFaceKind kind = f.Kind();
		if (kind == MeshFaceKind::Wall) a.wall++;
		else if (kind == MeshFaceKind::Polygon) a.polygon++;
		else if (kind == MeshFaceKind::Plane) a.plane++;
		else a.billboard++;
		if (!f.Textured()) a.flat++;
		if (f.tile != 0 && kind != MeshFaceKind::Polygon) a.tiled++;
		if (f.flags & 2) a.mirrored++;
		if (kind == MeshFaceKind::Polygon && (f.flags & 0x1000)) a.packedUV++;
		if (kind == MeshFaceKind::Wall && (f.flags & 0x100)) a.storedExtents++;

		int tw = 0, th = 0;
		const bool haveTex = FaceTexSize(pack, f, tw, th);
		if (f.Textured() && !haveTex) a.texMissing++;

		if (f.Textured() && f.texture < 0x1200)
		{
			const FatEntry *te = pack.Entry(f.texture);
			if (te && te->kind == EntryKind::ImagePack)
			{
				const int n = pack.SubImageCount(f.texture);
				a.r5Checked++;
				if (n == 0 || f.subTexture >= n) a.r5Bad++;
				if (f.subTexture != 0) a.oldSubIgnored++;
			}
		}

		float u[4], v[4];
		const bool haveUV = haveTex && MeshFaceTexCoords(m, f, tw, th, u, v);
		if (haveUV)
		{
			float ou[4], ov[4];
			OldUV(f, ou, ov);
			bool differs = false;
			for (int i = 0; i < f.count; i++)
				if (fabsf(ou[i] - u[i]) > 0.01f || fabsf(ov[i] - v[i]) > 0.01f) differs = true;
			if (differs) a.oldUvDiffers++;
		}

		if (kind == MeshFaceKind::Wall && f.count == 4)
		{
			const auto &v0 = m.vertices[f.vertex[0]], &v1 = m.vertices[f.vertex[1]];
			const auto &v2 = m.vertices[f.vertex[2]], &v3 = m.vertices[f.vertex[3]];
			// A face whose left and right edges coincide has no width and the
			// column driver draws nothing for it; DEMO1's 4124 carries two.
			const bool degenerate = (v0.x == v1.x && v0.y == v1.y);
			// The driver takes four separate corner heights (renderer.c:4286-
			// 4293), so a top or bottom edge may slope; the SIDES may not lean.
			const bool sides = v0.x == v3.x && v0.y == v3.y && v1.x == v2.x && v1.y == v2.y
				&& v0.up >= v3.up && v1.up >= v2.up && (v0.up > v3.up || v1.up > v2.up);
			const bool ok = sides && v0.up == v1.up && v3.up == v2.up;
			if (degenerate) a.r1Degenerate++;
			else
			{
				a.r1Checked++;
				if (!sides) a.r1Bad++;
				else if (!ok) a.r1Sloped++;
			}
			if (!degenerate && ok && !(f.flags & 0x100))
			{
				a.r2Checked++;
				const double h = v0.up - v3.up;
				const double w = hypot(double(v1.x - v0.x), double(v1.y - v0.y));
				if (fabs(h - f.extentV) > 1.5) a.r2BadV++;
				if (fabs(w - f.extentU) > 1.5) a.r2BadU++;
			}
		}

		// R3. World is right-handed (x, y, up). For a ring that is clockwise
		// seen from the side it faces, (v1-v0) x (v2-v0) points AWAY from the
		// viewer, so the visible side is the one the cross product's negative
		// points to.
		if ((f.flags & 0x30) == 0x30 && !(f.flags & 0x200) && f.count >= 3)
		{
			const auto &v0 = m.vertices[f.vertex[0]], &v1 = m.vertices[f.vertex[1]];
			const auto &v2 = m.vertices[f.vertex[2]];
			const double ax = v1.x - v0.x, ay = v1.y - v0.y;
			const double bx = v2.x - v0.x, by = v2.y - v0.y;
			const double crossUp = ax * by - ay * bx;       // up component of (v1-v0)x(v2-v0)
			const double visibleUp = -crossUp;              // > 0: seen from above
			// renderer.c:12365-12368: visible when the corner's height above the
			// eye is positive, i.e. the eye is BELOW; 0x80 negates.
			const bool originalFromBelow = (f.flags & 0x80) == 0;
			a.r3Checked++;
			if (visibleUp == 0.0 || (visibleUp < 0.0) != originalFromBelow) a.r3Bad++;
		}

		// R3, the broad form. A solid prop's faces look OUTWARD. Take the side
		// a clockwise ring shows and ask whether it points away from the
		// middle of the mesh. Cards (complementary pairs) cancel exactly and
		// are counted on both sides, so they do not bias the ratio.
		if (f.count >= 3)
		{
			const auto &v0 = m.vertices[f.vertex[0]], &v1 = m.vertices[f.vertex[1]];
			const auto &v2 = m.vertices[f.vertex[2]];
			const double ax = v1.x - v0.x, ay = v1.y - v0.y, az = v1.up - v0.up;
			const double bx = v2.x - v0.x, by = v2.y - v0.y, bz = v2.up - v0.up;
			const double nx = -(ay * bz - az * by), ny = -(az * bx - ax * bz), nz = -(ax * by - ay * bx);
			double cx = 0, cy = 0, cz = 0;
			for (int i = 0; i < f.count; i++)
			{
				cx += m.vertices[f.vertex[i]].x; cy += m.vertices[f.vertex[i]].y;
				cz += m.vertices[f.vertex[i]].up;
			}
			cx = cx / f.count - (a.minX + a.maxX) * 0.5;
			cy = cy / f.count - (a.minY + a.maxY) * 0.5;
			cz = cz / f.count - (a.minUp + a.maxUp) * 0.5;
			const double d = nx * cx + ny * cy + nz * cz;
			const double scale = sqrt((nx * nx + ny * ny + nz * nz) * (cx * cx + cy * cy + cz * cz));
			if (scale <= 0 || fabs(d) < 0.15 * scale) a.edgeOn++;
			else if (d > 0) a.outward++;
			else a.inward++;
		}

		if (f.count >= 3)
		{
			std::vector<int> key(f.vertex, f.vertex + f.count);
			std::sort(key.begin(), key.end());
			byCorners[key].push_back((int)fi);
		}

		if (verbose)
		{
			printf("    f%-3zu %-9s flags %04x tex %04x sub %2u tile %02x ext %3ux%-3u [",
				fi, KindName(kind), f.flags, f.texture, f.subTexture, f.tile,
				f.extentU, f.extentV);
			for (int i = 0; i < std::max(f.count, 1); i++) printf("%s%d", i ? "," : "", f.vertex[i]);
			printf("]");
			if (haveTex) printf("  pic %dx%d", tw, th);
			if (haveUV)
			{
				printf("  uv");
				for (int i = 0; i < f.count; i++) printf(" (%.2f,%.2f)", u[i], v[i]);
			}
			printf("\n");
		}
	}

	for (const auto &kv : byCorners)
	{
		const std::vector<int> &list = kv.second;
		if (list.size() < 2) { a.unpaired++; continue; }
		a.pairs++;
		// Do the two sides show the same thing? Compare texture, picture and
		// the coordinate each shared corner gets.
		const MeshFace &fa = m.faces[list[0]], &fb = m.faces[list[1]];
		int tw = 0, th = 0, tw2 = 0, th2 = 0;
		float ua[4], va[4], ub[4], vb[4];
		bool same = fa.texture == fb.texture && fa.subTexture == fb.subTexture
			&& fa.count == fb.count;
		if (same && FaceTexSize(pack, fa, tw, th) && FaceTexSize(pack, fb, tw2, th2)
			&& MeshFaceTexCoords(m, fa, tw, th, ua, va)
			&& MeshFaceTexCoords(m, fb, tw2, th2, ub, vb))
		{
			for (int i = 0; i < fa.count && same; i++)
				for (int j = 0; j < fb.count; j++)
					if (fa.vertex[i] == fb.vertex[j]
						&& (fabsf(ua[i] - ub[j]) > 0.01f || fabsf(va[i] - vb[j]) > 0.01f))
					{ same = false; break; }
		}
		if (!same) a.pairsDifferentUV++;
	}
	return a;
}

struct ObjClass
{
	const char *name = "?";
	bool drawn = false;
	bool mesh = false, fixedAngle = false, directional = false;
	double w = 0, h = 0;        // drawn size, world units
	double upp = 0;
	bool hang = false;
	double shift = 0;
	int views = 0;
};

// The size rule of TextureSet::Sprite, restated so this tool needs no engine
// header: renderer.c:6512-6525 (size), :6549-6561 (anchor).
static void SpriteSize(const Pack &pack, int index, ObjClass &c)
{
	Image img = pack.ReadImage(index, false);
	if (!img.ok()) return;
	c.upp = 2.0;
	if (img.modifier & IM_HALF_SIZE)
	{
		const int s = (img.imageType >> 5) & 3;
		c.upp = (s == 0) ? 1.0 : 2.0 * double(1 << s);
	}
	c.hang = (img.modifier & IM_HANG) != 0;
	const AnchorOffset an = pack.ReadAnchor(index);
	c.shift = double((int16_t)(uint16_t)((img.modifier & 0x0f) * 2 + (uint16_t)an.y));
	c.w = img.height * c.upp;       // stored rows are the picture's columns
	c.h = img.width * c.upp;
}

static int MeshMain(const char *packName, int index)
{
	LoadedPack *lp = GetPack(packName);
	if (!lp->ok) { printf("pack %s: cannot open\n", packName); return 2; }
	Mesh m = lp->pack.ReadMesh(index);
	if (!m.ok()) { printf("%s[%d] is not a readable mesh\n", packName, index); return 2; }
	printf("%s[%d]  %zu vertices, %zu faces%s\n", packName, index, m.vertices.size(),
		m.faces.size(), m.expl ? "  (EXPL)" : "");
	for (size_t i = 0; i < m.vertices.size(); i++)
		printf("    v%-3zu x %5d  y %5d  up %5d\n", i, m.vertices[i].x, m.vertices[i].y,
			m.vertices[i].up);
	MeshAudit a = AuditMesh(lp->pack, m, true);
	printf("  extent x %d..%d  y %d..%d  up %d..%d\n", a.minX, a.maxX, a.minY, a.maxY,
		a.minUp, a.maxUp);
	printf("  %d wall, %d polygon, %d plane, %d billboard; %d complementary pairs\n",
		a.wall, a.polygon, a.plane, a.billboard, a.pairs);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		printf("usage: propcheck <ROTH DATA folder> [MAP | -mesh PACK INDEX]\n");
		return 2;
	}
	if (!TheInstall().Open(argv[1])) { printf("install: %s\n", TheInstall().Error().c_str()); return 2; }
	if (argc >= 5 && !strcmp(argv[2], "-mesh")) return MeshMain(argv[3], atoi(argv[4]));
	const std::string wantMap = argc > 2 ? argv[2] : "";

	LoadedPack *shared = GetPack(TheInstall().SharedPack());

	//------------------------------------------------------------------
	// Part 1. Every mesh entry in every map pack, against the rules.
	//------------------------------------------------------------------
	std::map<std::string, MeshAudit> meshAudit;       // "PACK/index"
	std::map<std::string, int> placedCount;
	MeshAudit all;
	int meshEntries = 0, signBits = 0, faceIndexedPacks = 0;
	std::set<std::string> packNames;
	for (const std::string &name : TheInstall().MapNames())
		packNames.insert(TheInstall().PackFor(name.c_str()));
	for (const std::string &pn : packNames)
	{
		LoadedPack *lp = GetPack(pn);
		if (!lp->ok) { printf("pack %s: cannot open\n", pn.c_str()); continue; }
		for (int i = 0; i < lp->pack.Count(); i++)
		{
			const FatEntry *e = lp->pack.Entry(i);
			if (e == nullptr) continue;
			if (e->kind == EntryKind::ImagePack && lp->pack.SubImageCount(i) > 0)
			{
				faceIndexedPacks++;
				for (int k = 0; k < lp->pack.SubImageCount(i); k++)
					if (RdU16(lp->bytes.data() + e->offset + 8 + k * 2) & 0x8000) signBits++;
			}
			if (e->kind != EntryKind::Object3D || e->size == 0) continue;
			Mesh m = lp->pack.ReadMesh(i);
			if (!m.ok()) continue;
			meshEntries++;
			MeshAudit a = AuditMesh(lp->pack, m, false);
			meshAudit[pn + "/" + std::to_string(i)] = a;
			all.Add(a);
		}
	}

	//------------------------------------------------------------------
	// Part 2. Every object of every map.
	//------------------------------------------------------------------
	struct MapTotals
	{
		int total = 0, hidden = 0, billboard = 0, fixed = 0, directional = 0, mesh = 0;
		int creature = 0, empty = 0, other = 0;
		int yawMesh = 0, yawFixedPlane = 0, yawFixedMirror = 0, yawDir = 0;
		int meshUv = 0, meshSub = 0, meshPairs = 0;
		int sky = -1, skyKind = -1, skyW = 0, skyH = 0;
		int skyFlats = 0;
	};
	std::map<std::string, MapTotals> totals;
	MapTotals grand;

	for (const std::string &name : TheInstall().MapNames())
	{
		std::vector<uint8_t> raw = ReadFile(TheInstall().MapFile(name.c_str()));
		if (raw.empty()) continue;
		Map rm = ParseRaw(raw.data(), raw.size());
		if (!rm.ok())
		{
			printf("%s: cannot parse (%s)\n", name.c_str(), rm.error.c_str());
			continue;
		}
		const std::string pn = TheInstall().PackFor(name.c_str());
		LoadedPack *lp = GetPack(pn);
		if (!lp->ok) continue;
		MapTotals &t = totals[name];
		const bool list = (wantMap == name);

		// The sky. The MAP names the picture (metadata +0x18, map_load.c:214);
		// render_parallax_sky_columns resolves it as a normal DAS block
		// (renderer.c:5414).
		t.sky = rm.metadata.skyTexture;
		if (const FatEntry *se = lp->pack.Entry(t.sky))
		{
			t.skyKind = (int)se->kind;
			Image si = lp->pack.ReadImage(t.sky, false);
			if (si.ok()) { t.skyW = si.height; t.skyH = si.width; }
		}

		if (list)
			printf("\n%s  (pack %s)  -- every object\n"
				"  sec.idx      x      y      z  rot  class        art            size WxH  anchor"
				"   bearing now (was)\n", name.c_str(), pn.c_str());

		for (size_t si = 0; si < rm.objects.size(); si++)
		for (size_t oi = 0; oi < rm.objects[si].size(); oi++)
		{
			const Object &o = rm.objects[si][oi];
			t.total++;
			ObjClass c;
			int artIndex = 0;
			const bool useShared = Pack::ResolveObjectArt(o.textureIndex, o.textureSource, artIndex);
			LoadedPack *ap = useShared ? shared : lp;
			const FatEntry *e = ap->ok ? ap->pack.Entry(artIndex) : nullptr;
			std::string meshKey;

			if (o.flags & 0x80) { c.name = "not-drawn"; t.hidden++; }       // renderer.c:8035
			else if (e == nullptr) { c.name = "no-art"; t.other++; }
			else switch (e->kind)
			{
			case EntryKind::Empty: c.name = "empty"; t.empty++; break;
			case EntryKind::Indirection:
				if (ap->pack.Indirect(artIndex) == IndirectKind::Directional)
				{
					Directional d = ap->pack.ReadDirectional(artIndex);
					c.name = "directional"; c.directional = true; c.drawn = d.ok();
					c.views = d.count;
					if (d.ok())
					{
						int fi = 0;
						const bool fs = Pack::ResolveDasId(d.frames[0].entry, fi);
						LoadedPack *fp = fs ? shared : lp;
						if (fp->ok) SpriteSize(fp->pack, fi, c);
					}
					t.directional++;
				}
				else { c.name = "creature"; t.creature++; }
				break;
			case EntryKind::Object3D:
			{
				c.name = "MESH"; c.mesh = true; c.drawn = true; t.mesh++;
				meshKey = ap->name + "/" + std::to_string(artIndex);
				placedCount[meshKey]++;
				auto it = meshAudit.find(meshKey);
				if (it != meshAudit.end())
				{
					c.w = std::max(it->second.maxX - it->second.minX, it->second.maxY - it->second.minY);
					c.h = it->second.maxUp - it->second.minUp;
					if (it->second.oldUvDiffers) t.meshUv++;
					if (it->second.oldSubIgnored) t.meshSub++;
					if (it->second.pairs) t.meshPairs++;
				}
				break;
			}
			default:
				c.drawn = true;
				SpriteSize(ap->pack, artIndex, c);
				if (o.FixedAngle()) { c.name = "fixed-quad"; c.fixedAngle = true; t.fixed++; }
				else { c.name = "billboard"; t.billboard++; }
				break;
			}

			// R6. The bearings, in the port's degrees (counter-clockwise from
			// +X). R is clockwise, so the object's own +y axis points at 90 - R.
			const double R = o.rotation * 360.0 / 256.0;
			const double was = Norm360(90.0 + R);
			double now = -1;
			const char *what = "";
			if (c.mesh) { now = Norm360(-R); what = "model yaw"; }
			else if (c.fixedAngle) { now = Norm360(270.0 - R); what = "face normal"; }
			else if (c.directional) { now = Norm360(90.0 - R); what = "facing"; }
			if (now >= 0 && c.drawn)
			{
				const double d = Norm360(now - was);
				if (c.mesh && d > 0.5 && d < 359.5) t.yawMesh++;
				if (c.directional && d > 0.5 && d < 359.5) t.yawDir++;
				if (c.fixedAngle)
				{
					const double dp = fmod(d, 180.0);
					if (dp > 0.5 && dp < 179.5) t.yawFixedPlane++;
					else if (d > 0.5 && d < 359.5) t.yawFixedMirror++;
				}
			}

			if (list)
			{
				char art[32], size[32] = "", anch[24] = "", bear[48] = "";
				snprintf(art, sizeof art, "%s[%d]", ap->name.c_str(), artIndex);
				if (c.w > 0) snprintf(size, sizeof size, "%4.0fx%-4.0f", c.w, c.h);
				if (c.mesh) snprintf(anch, sizeof anch, "origin");
				else if (c.upp > 0) snprintf(anch, sizeof anch, "%s%+.0f", c.hang ? "top" : "bot",
					c.hang ? c.shift : -c.shift);
				if (now >= 0) snprintf(bear, sizeof bear, "%s %5.1f (%5.1f)%s", what, now, was,
					fabs(Norm360(now - was)) > 0.5 && Norm360(now - was) < 359.5 ? "  <- DIFFERS" : "");
				printf("  %3zu.%-3zu %6d %6d %6d  %3u  %-11s  %-13s %10s  %-7s  %s%s\n",
					si, oi, o.x, o.y, o.z, o.rotation, c.name, art, size, anch, bear,
					(o.flags & 0x10) ? "  x-flip" : "");
			}
		}

		grand.total += t.total; grand.hidden += t.hidden; grand.billboard += t.billboard;
		grand.fixed += t.fixed; grand.directional += t.directional; grand.mesh += t.mesh;
		grand.creature += t.creature; grand.empty += t.empty; grand.other += t.other;
		grand.yawMesh += t.yawMesh; grand.yawFixedPlane += t.yawFixedPlane;
		grand.yawFixedMirror += t.yawFixedMirror; grand.yawDir += t.yawDir;
		grand.meshUv += t.meshUv; grand.meshSub += t.meshSub; grand.meshPairs += t.meshPairs;
	}

	printf("\nPER MAP -- what the original makes of each object, and what the port built differently\n");
	printf("  map        objs  hide  bill fixed  dir  mesh crea |  MESH: yaw  uv  sub pairs |"
		" FIXED: plane mirror | DIR yaw | sky pic\n");
	for (const auto &kv : totals)
	{
		const MapTotals &t = kv.second;
		printf("  %-9s %5d %5d %5d %5d %4d %5d %4d | %9d %4d %4d %5d | %12d %6d | %7d | %d (%dx%d)\n",
			kv.first.c_str(), t.total, t.hidden, t.billboard, t.fixed, t.directional, t.mesh,
			t.creature, t.yawMesh, t.meshUv, t.meshSub, t.meshPairs, t.yawFixedPlane,
			t.yawFixedMirror, t.yawDir, t.sky, t.skyW, t.skyH);
	}
	printf("  %-9s %5d %5d %5d %5d %4d %5d %4d | %9d %4d %4d %5d | %12d %6d | %7d |\n", "ALL",
		grand.total, grand.hidden, grand.billboard, grand.fixed, grand.directional, grand.mesh,
		grand.creature, grand.yawMesh, grand.meshUv, grand.meshSub, grand.meshPairs,
		grand.yawFixedPlane, grand.yawFixedMirror, grand.yawDir);
	printf("  (MESH yaw/uv/sub/pairs: placed meshes whose yaw, face coordinates or face picture the\n"
		"   old spawner got wrong, and those holding complementary face pairs it drew four deep.\n"
		"   FIXED plane: fixed-angle pictures it stood in the wrong plane; mirror: right plane,\n"
		"   picture reversed.)\n");

	printf("\nMESH ENTRIES  (%d readable, %d face-indexed image packs)\n", meshEntries, faceIndexedPacks);
	printf("  entry          placed  faces wall poly plane bill flat | pairs diffUV | oldUV sub |"
		" size x*y*up, lowest up\n");
	for (const auto &kv : meshAudit)
	{
		const MeshAudit &a = kv.second;
		printf("  %-14s %6d  %5d %4d %4d %5d %4d %4d | %5d %6d | %5d %3d | %dx%dx%d, %d\n",
			kv.first.c_str(), placedCount.count(kv.first) ? placedCount[kv.first] : 0,
			a.faces, a.wall, a.polygon, a.plane, a.billboard, a.flat, a.pairs,
			a.pairsDifferentUV, a.oldUvDiffers, a.oldSubIgnored,
			a.maxX - a.minX, a.maxY - a.minY, a.maxUp - a.minUp, a.minUp);
	}

	printf("\nRULE CHECKS over all %d faces of all %d mesh entries\n", all.faces, meshEntries);
	printf("  R1 wall ring is TL,TR,BR,BL           %4d checked, %d fail  (%d of them slope along"
		" the top or bottom; %d more have no width)\n", all.r1Checked, all.r1Bad, all.r1Sloped,
		all.r1Degenerate);
	printf("  R2 +0x24 is the height, +0x26 width   %4d checked, %d height off, %d width off\n",
		all.r2Checked, all.r2BadV, all.r2BadU);
	printf("  R3 clockwise ring = the side the\n"
	       "     height-sign rule shows              %4d checked, %d disagree  (too few to decide"
		" anything: only flat slivers and one unplaced mesh use that rule)\n",
		all.r3Checked, all.r3Bad);
	printf("     the same, broadly: a clockwise ring\n"
	       "     shows the side facing OUT of its mesh %4d out, %d in, %d edge-on to the centre\n",
		all.outward, all.inward, all.edgeOn);
	printf("  R4 complementary pairs                %4d pairs, %d wear different coordinates"
		" on their two sides; %d faces unpaired\n", all.pairs, all.pairsDifferentUV, all.unpaired);
	printf("  R5 face picture index inside its pack %4d checked, %d fail; mirror bits in the"
		" tables: %d\n", all.r5Checked, all.r5Bad, signBits);
	printf("  faces by driver: %d wall, %d polygon, %d plane, %d billboard; %d flat colour\n",
		all.wall, all.polygon, all.plane, all.billboard, all.flat);
	printf("  modifiers: %d mirrored (bit 1), %d tiled (+0x0f), %d with per-corner bytes (0x1000),"
		" %d stored-extent walls (0x100)\n", all.mirrored, all.tiled, all.packedUV, all.storedExtents);
	printf("  the old unit square put a corner elsewhere on %d faces; %d faces name a picture"
		" other than their pack's first; %d textured faces have no decodable picture\n",
		all.oldUvDiffers, all.oldSubIgnored, all.texMissing);

	const bool fail = all.r1Bad || all.r5Bad || all.inward * 2 > all.outward;
	printf("\n%s\n", fail ? "SOME RULE FAILED -- read the lines above" : "all structural rules hold");
	return fail ? 1 : 0;
}
