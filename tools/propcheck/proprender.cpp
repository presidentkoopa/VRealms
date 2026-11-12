// proprender.cpp -- draw a map's objects from a camera pose, on the host, with
// the rules roth_objects.cpp uses, so the result can be laid over a frame of
// the ORIGINAL engine (tools/oracle) taken from the same pose.
//
// It draws ONLY objects -- no walls, no floors, no lighting -- on a magenta
// background, with a depth buffer among themselves. What it proves is shape:
// where each prop stands, which way it is turned, which side of each face is
// drawn and how its picture lies on it. Where the overlay and the original
// agree, the engine's spawner (which calls the same roth::MeshFaceTexCoords and
// uses the same yaw rule, restated here) is building the right thing.
//
//   proprender <ROTH DATA folder> <MAP> <x> <y> <angle512> <floorZ> <out.ppm>
//
// The camera is tools/oracle's, solved there from geometry: eye = floorZ + 144,
// fx 309.8, fy 355.1, centre (320.5, 240.5) on 640x480, facing 90 + a*360/512
// degrees, counter-clockwise.
//
// Build: as tools/propcheck/propcheck.cpp, with this file in place of it.

#include "roth_raw.h"
#include "roth_install.h"
#include "roth_das.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
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

static const int W = 640, H = 480;
static const double FX = 309.8, FY = 355.1, CX = 320.5, CY = 240.5, NEARZ = 16.0;

struct Tex { int w = 0, h = 0; std::vector<uint8_t> px; bool ok = false; };   // px[x*h + y]
struct V { double x, y, z, u, v; };        // camera space: x right, y up, z forward

static std::vector<uint8_t> gRGB(W * H * 3);
static std::vector<float> gDepth(W * H, 1e30f);
static const std::vector<Colour> *gPal = nullptr;

static Tex LoadTex(const Pack &pack, int index, int sub)
{
	Tex t;
	Image img = pack.ReadSubImage(index, sub);
	if (!img.ok()) return t;
	t.w = img.height; t.h = img.width; t.px = img.frames[0]; t.ok = true;
	return t;
}

// One convex polygon, already in camera space. `flat` < 0 for a textured face.
static void DrawPoly(std::vector<V> poly, const Tex *tex, int flat, bool masked, bool cull)
{
	// Clip to the near plane.
	std::vector<V> in;
	for (size_t i = 0; i < poly.size(); i++)
	{
		const V &a = poly[i], &b = poly[(i + 1) % poly.size()];
		const bool ia = a.z >= NEARZ, ib = b.z >= NEARZ;
		if (ia) in.push_back(a);
		if (ia != ib)
		{
			const double t = (NEARZ - a.z) / (b.z - a.z);
			V c = { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, NEARZ,
				a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t };
			in.push_back(c);
		}
	}
	if (in.size() < 3) return;
	std::vector<double> sx(in.size()), sy(in.size());
	for (size_t i = 0; i < in.size(); i++)
	{
		sx[i] = CX + FX * in[i].x / in[i].z;
		sy[i] = CY - FY * in[i].y / in[i].z;
	}
	// The original's test, renderer.c:12307-12314: with y DOWN, the ring is
	// drawn when (P2-P0)x(P1-P0) = dx2*dy1 - dx1*dy2 is negative.
	double area = 0;
	for (size_t i = 1; i + 1 < in.size(); i++)
		area += (sx[i + 1] - sx[0]) * (sy[i] - sy[0]) - (sx[i] - sx[0]) * (sy[i + 1] - sy[0]);
	if (cull && area >= 0) return;

	for (size_t k = 1; k + 1 < in.size(); k++)
	{
		const size_t id[3] = { 0, k, k + 1 };
		double x0 = sx[id[0]], y0 = sy[id[0]], x1 = sx[id[1]], y1 = sy[id[1]], x2 = sx[id[2]], y2 = sy[id[2]];
		const double den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
		if (fabs(den) < 1e-9) continue;
		int minx = (int)floor(fmin(x0, fmin(x1, x2))), maxx = (int)ceil(fmax(x0, fmax(x1, x2)));
		int miny = (int)floor(fmin(y0, fmin(y1, y2))), maxy = (int)ceil(fmax(y0, fmax(y1, y2)));
		if (minx < 0) minx = 0; if (miny < 0) miny = 0;
		if (maxx > W - 1) maxx = W - 1; if (maxy > H - 1) maxy = H - 1;
		const double iz[3] = { 1 / in[id[0]].z, 1 / in[id[1]].z, 1 / in[id[2]].z };
		for (int y = miny; y <= maxy; y++) for (int x = minx; x <= maxx; x++)
		{
			const double px = x + 0.5, py = y + 0.5;
			const double l0 = ((y1 - y2) * (px - x2) + (x2 - x1) * (py - y2)) / den;
			const double l1 = ((y2 - y0) * (px - x2) + (x0 - x2) * (py - y2)) / den;
			const double l2 = 1 - l0 - l1;
			if (l0 < 0 || l1 < 0 || l2 < 0) continue;
			const double w = l0 * iz[0] + l1 * iz[1] + l2 * iz[2];
			const double z = 1 / w;
			if (z >= gDepth[y * W + x]) continue;
			int ci = flat;
			if (flat < 0)
			{
				if (!tex || !tex->ok) continue;
				double u = (l0 * in[id[0]].u * iz[0] + l1 * in[id[1]].u * iz[1] + l2 * in[id[2]].u * iz[2]) * z;
				double v = (l0 * in[id[0]].v * iz[0] + l1 * in[id[1]].v * iz[1] + l2 * in[id[2]].v * iz[2]) * z;
				u -= floor(u); v -= floor(v);
				int tx = (int)(u * tex->w), ty = (int)(v * tex->h);
				if (tx >= tex->w) tx = tex->w - 1; if (ty >= tex->h) ty = tex->h - 1;
				ci = tex->px[(size_t)tx * tex->h + ty];
				if (masked && ci == 0) continue;
			}
			gDepth[y * W + x] = (float)z;
			const Colour c = (size_t)ci < gPal->size() ? (*gPal)[ci] : Colour{ 0, 0, 0 };
			gRGB[(y * W + x) * 3] = c.r; gRGB[(y * W + x) * 3 + 1] = c.g; gRGB[(y * W + x) * 3 + 2] = c.b;
		}
	}
}

int main(int argc, char **argv)
{
	if (argc < 8)
	{
		printf("usage: proprender <ROTH DATA folder> <MAP> <x> <y> <angle512> <floorZ> <out.ppm>\n");
		return 2;
	}
	if (!TheInstall().Open(argv[1])) { printf("install: bad\n"); return 2; }
	const std::string name = argv[2];
	const double camX = atof(argv[3]), camY = atof(argv[4]);
	const double theta = (90.0 + atof(argv[5]) * 360.0 / 512.0) * M_PI / 180.0;
	const double eye = atof(argv[6]) + 144.0;
	const double fwx = cos(theta), fwy = sin(theta), rtx = sin(theta), rty = -cos(theta);

	std::vector<uint8_t> raw = ReadFile(TheInstall().MapFile(name.c_str()));
	Map rm = ParseRaw(raw.data(), raw.size());
	if (!rm.ok()) { printf("map: %s\n", rm.error.c_str()); return 2; }
	std::vector<uint8_t> pb = ReadFile(TheInstall().PackFile(TheInstall().PackFor(name.c_str()).c_str()));
	std::vector<uint8_t> sb = ReadFile(TheInstall().PackFile(TheInstall().SharedPack().c_str()));
	Pack level, shared;
	if (!level.Load(pb.data(), pb.size())) { printf("pack: bad\n"); return 2; }
	const bool haveShared = !sb.empty() && shared.Load(sb.data(), sb.size());
	gPal = &level.Palette();
	for (int i = 0; i < W * H; i++) { gRGB[i * 3] = 255; gRGB[i * 3 + 1] = 0; gRGB[i * 3 + 2] = 255; }

	auto toCam = [&](double wx, double wy, double wz, double u, double v) -> V
	{
		const double dx = wx - camX, dy = wy - camY;
		V o = { dx * rtx + dy * rty, wz - eye, dx * fwx + dy * fwy, u, v };
		return o;
	};

	std::map<int, Tex> texCache[2];
	auto getTex = [&](bool sh, int index, int sub) -> const Tex *
	{
		const int key = (index << 8) | (sub & 0xff);
		auto it = texCache[sh].find(key);
		if (it == texCache[sh].end())
			it = texCache[sh].emplace(key, LoadTex(sh ? shared : level, index, sub)).first;
		return &it->second;
	};

	// A camera-facing picture, as the engine's face sprite: bottom centre on
	// the anchor (top centre for a hanging one).
	auto drawBillboard = [&](bool sh, int index, double x, double y, double z, bool xflip)
	{
		const Pack &pk = sh ? shared : level;
		Image img = pk.ReadImage(index, false);
		if (!img.ok()) return;
		const Tex *t = getTex(sh, index, 0);
		double upp = 2.0;
		if (img.modifier & IM_HALF_SIZE) { const int s = (img.imageType >> 5) & 3; upp = s == 0 ? 1.0 : 2.0 * (1 << s); }
		const bool hang = (img.modifier & IM_HANG) != 0;
		const double shift = double((int16_t)(uint16_t)((img.modifier & 0x0f) * 2 + (uint16_t)pk.ReadAnchor(index).y));
		const double hw = t->w * upp * 0.5, hh = t->h * upp;
		const double bottom = hang ? z + shift - hh : z - shift;
		const double u0 = xflip ? 1 : 0, u1 = xflip ? 0 : 1;
		std::vector<V> q = {
			toCam(x - rtx * hw, y - rty * hw, bottom + hh, u0, 0), toCam(x + rtx * hw, y + rty * hw, bottom + hh, u1, 0),
			toCam(x + rtx * hw, y + rty * hw, bottom, u1, 1), toCam(x - rtx * hw, y - rty * hw, bottom, u0, 1) };
		DrawPoly(q, t, -1, true, false);
	};

	int meshes = 0, quads = 0, bills = 0;
	// PROPRENDER_ONLY="sector.index" draws that one object, which is how an
	// overlay is kept readable in a crowded room. PROPRENDER_OLDYAW=1 turns
	// everything the way the spawner did before 2026-11-12 (counter-clockwise),
	// to show the two conventions side by side against the original.
	int onlySec = -1, onlyIdx = -1;
	if (const char *only = getenv("PROPRENDER_ONLY")) sscanf(only, "%d.%d", &onlySec, &onlyIdx);
	const bool oldYaw = getenv("PROPRENDER_OLDYAW") != nullptr;

	for (size_t si = 0; si < rm.objects.size(); si++)
	for (size_t oi = 0; oi < rm.objects[si].size(); oi++)
	{
		const Object &o = rm.objects[si][oi];
		if (onlySec >= 0 && ((int)si != onlySec || (int)oi != onlyIdx)) continue;
		if (o.flags & 0x80) continue;
		int artIndex = 0;
		const bool sh = Pack::ResolveObjectArt(o.textureIndex, o.textureSource, artIndex);
		if (sh && !haveShared) continue;
		const Pack &pk = sh ? shared : level;
		const FatEntry *e = pk.Entry(artIndex);
		if (e == nullptr) continue;
		// R, clockwise: local x -> (cos R, -sin R), local y -> (sin R, cos R).
		const double R = o.rotation * M_PI / 128.0;
		const double cR = cos(R), sR = oldYaw ? -sin(R) : sin(R);

		if (e->kind == EntryKind::Object3D)
		{
			Mesh m = pk.ReadMesh(artIndex);
			if (!m.ok()) continue;
			meshes++;
			for (const MeshFace &f : m.faces)
			{
				if (f.Kind() == MeshFaceKind::Billboard)
				{
					const Mesh::Vertex &v = m.vertices[f.vertex[0]];
					// The face's texture word is an OBJECT art word here, not a
					// DAS id: low byte index, high byte source (renderer.c:6603-6605).
					int hi = 0;
					const bool hs = Pack::ResolveObjectArt((uint8_t)(f.texture & 0xff),
						(uint8_t)(f.texture >> 8), hi);
					if (hs && !haveShared) continue;
					drawBillboard(hs, hi, o.x + v.x * cR + v.y * sR, o.y - v.x * sR + v.y * cR,
						o.z + v.up, false);
					continue;
				}
				const Tex *t = nullptr;
				int tw = 0, th = 0, flat = -1;
				if (f.texture & 0x8000) flat = f.texture & 0xff;
				else if (f.texture < 0x1200) { t = getTex(sh, f.texture, f.subTexture); tw = t->w; th = t->h; }
				float u[4] = { 0, 0, 0, 0 }, v[4] = { 0, 0, 0, 0 };
				if (flat < 0 && !MeshFaceTexCoords(m, f, tw, th, u, v)) continue;
				std::vector<V> poly;
				for (int i = 0; i < f.count; i++)
				{
					const Mesh::Vertex &mv = m.vertices[f.vertex[i]];
					poly.push_back(toCam(o.x + mv.x * cR + mv.y * sR, o.y - mv.x * sR + mv.y * cR,
						o.z + mv.up, u[i], v[i]));
				}
				DrawPoly(poly, t, flat, f.Masked(), true);
			}
		}
		else if (e->kind == EntryKind::Plain || e->kind == EntryKind::Animated || e->kind == EntryKind::ImagePack)
		{
			if (!o.FixedAngle()) { bills++; drawBillboard(sh, artIndex, o.x, o.y, o.z, o.HorizontalFlip()); continue; }
			quads++;
			Image img = pk.ReadImage(artIndex, false);
			if (!img.ok()) continue;
			const Tex *t = getTex(sh, artIndex, 0);
			double upp = 2.0;
			if (img.modifier & IM_HALF_SIZE) { const int s = (img.imageType >> 5) & 3; upp = s == 0 ? 1.0 : 2.0 * (1 << s); }
			const bool hang = (img.modifier & IM_HANG) != 0;
			// The rotated path leaves the art's own anchor out and keeps only
			// the nibble (renderer.c:6123-6128).
			const double shift = (img.modifier & 0x0f) * 2;
			const double hw = t->w * upp * 0.5, hh = t->h * upp;
			const double bottom = hang ? o.z + shift - hh : o.z - shift;
			// Edge B (u = 0) at -ext along local x, edge A at +ext.
			const double u0 = o.HorizontalFlip() ? 1 : 0, u1 = 1 - u0;
			std::vector<V> q = {
				toCam(o.x - cR * hw, o.y + sR * hw, bottom + hh, u0, 0), toCam(o.x + cR * hw, o.y - sR * hw, bottom + hh, u1, 0),
				toCam(o.x + cR * hw, o.y - sR * hw, bottom, u1, 1), toCam(o.x - cR * hw, o.y + sR * hw, bottom, u0, 1) };
			DrawPoly(q, t, -1, true, false);
		}
	}

	FILE *f = fopen(argv[7], "wb");
	if (!f) return 1;
	fprintf(f, "P6 %d %d 255\n", W, H);
	fwrite(gRGB.data(), 1, gRGB.size(), f);
	fclose(f);
	printf("%s: drew %d meshes, %d fixed-angle pictures, %d billboards\n", name.c_str(), meshes, quads, bills);
	return 0;
}
