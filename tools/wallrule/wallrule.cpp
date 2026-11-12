// wallrule.cpp -- where every wall texel comes from: ROTH.C's rule against
// what the REMAROTH loader makes the engine draw, over the retail maps.
//
// For every wall piece (upper / mid / lower of every face) this builds a
// PieceMap -- the piece's drawn height range and the function from (distance
// along the face, height) to a texel -- three times:
//
//   ROTH   the original's wall driver, from ROTH.C renderer.c (cited inline)
//   HEAD   src/maploader/rothmap.cpp as it stood before the wallrule patch,
//          pushed through the engine's wall code (hw_walls.cpp, gametexture.cpp)
//   FIXED  rothmap.cpp with the wallrule patch
//
// and reports (1) every piece where a loader model samples a different texel
// from ROTH, by cause; (2) every pair of adjacent coplanar pieces, whether the
// PICTURE runs on across the join (compared in pixels, so two different
// textures cut to continue each other count as continuous), under each rule,
// and the joins that ROTH keeps and a loader model breaks.
//
//   sh tools/wallrule/build.sh
//   /tmp/wallrule "<...>/Realms of the Haunting/DATA"              all maps, totals
//   /tmp/wallrule "<...>/DATA" STUDY1                              one map, with lists
//   /tmp/wallrule "<...>/DATA" STUDY1 -export /tmp/study1.pieces   for wallview.py
//
// Not covered: lighting, door leaves (polyobjects), mid-platform edges (3D
// floors), the parallax sky above FF_EDGE_MAP faces, floors and ceilings.

#include "roth_raw.h"
#include "roth_install.h"
#include "roth_das.h"
#include "roth_surface.h"

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <string>
#include <tuple>
#include <vector>

static std::vector<uint8_t> ReadFile(const std::string &p)
{
	std::vector<uint8_t> o; FILE *f = fopen(p.c_str(), "rb"); if (!f) return o;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); o.resize((size_t)n);
	if (n > 0 && fread(o.data(), 1, (size_t)n, f) != (size_t)n) o.clear();
	fclose(f); return o;
}

enum { T_TOP = 0, T_MID = 1, T_BOT = 2 };            // side_t::top / mid / bottom
static const char *TierName(int t) { return t == T_TOP ? "upper" : t == T_MID ? "mid" : "lower"; }

enum UMode { U_PLAIN, U_MOD, U_MASK, U_SMEAR };
enum VMode { V_PLAIN, V_MOD, V_MASK, V_ONECOPY };

// texel column = wrapU( mirror( floor(u0 + su * d) ) ),  d = distance from the face's first vertex
// texel row    = wrapV( floor(v0 + sv * (zAnchor - z)) )
struct PieceMap
{
	bool on = false;
	int tex = -1;
	int dimU = 0, dimV = 0;          // texels along the wall / down it
	double zTop = 0, zBot = 0;       // what is drawn
	double u0 = 0, su = 0; bool mirror = false; int uMode = U_PLAIN;
	double zAnc = 0, v0 = 0, sv = 0; int vMode = V_PLAIN;
	bool fit = false;
	bool vStuck = false;             // ROTH: the wrapping row mapper on a non power of two
	// a baked registration: the image the engine holds is these source columns / rows
	std::vector<uint16_t> colMap, rowMap;
};

static bool IsPow2(int n) { return n > 0 && (n & (n - 1)) == 0; }
static int Mod(int a, int n) { int r = a % n; return r < 0 ? r + n : r; }

// -1: nothing drawn here. -3: the original reads outside the image. Else column,row packed.
static int SampleCR(const PieceMap &p, double d, double z, int &col, int &row)
{
	if (!p.on || z > p.zTop || z < p.zBot) return -1;
	int c = (int)floor(p.u0 + p.su * d + 1e-7);
	if (p.mirror) c = p.dimU - 1 - c;
	switch (p.uMode)
	{
	case U_MOD: c = Mod(c, p.dimU); break;
	case U_MASK: c &= (p.dimU - 1); break;
	case U_SMEAR: c = 0; break;
	default: break;
	}
	int r = (int)floor(p.v0 + p.sv * (p.zAnc - z) + 1e-7);
	switch (p.vMode)
	{
	case V_MOD: r = Mod(r, p.dimV); break;
	case V_MASK: r &= (p.dimV - 1); break;
	case V_ONECOPY: if (r < 0 || r >= p.dimV) return -1; break;
	default: break;
	}
	if (c < 0 || c >= p.dimU || r < 0 || r >= p.dimV) return -3;
	if (!p.colMap.empty())
	{
		c = p.colMap[c]; r = p.rowMap[r];
		if (c == 0xFFFF || r == 0xFFFF) return -1;
	}
	col = c; row = r;
	return 0;
}

struct FaceGeo
{
	bool ok = false, twoSided = false;
	int x1, y1, x2, y2; double len = 0;
	int sector = -1, nsector = -1;
	int ownF, ownC, nF = 0, nC = 0;
	bool leafMid = false;            // the loader hands this doorway's mid piece to a door leaf
};

struct MapData
{
	roth::Map m;
	std::vector<uint8_t> packBytes;   // the pack reads from these for as long as it lives
	roth::Pack pack;
	uint16_t key = 0;
	std::map<int, roth::Image> images;
	const roth::Image &Img(int idx)
	{
		auto it = images.find(idx);
		if (it != images.end()) return it->second;
		return images[idx] = pack.ReadImage(idx, false);
	}
	bool Drawable(int idx)           // a picture: not the colour key, not a solid colour, decodes
	{
		if (idx < 0 || idx >= 0x1200 || (uint16_t)idx == key) return false;
		return Img(idx).ok();
	}
	std::vector<FaceGeo> geo;
	// Every image the patched loader registers for a wall: stored index, mirrored,
	// baked size (0 = the plain picture). Checked by CheckRegistrations.
	struct Reg { int idx; bool mirrored; int cols, rows; bool mapsInRange; };
	std::vector<Reg> regs;
};

// rothmap.cpp leafBuildable
static bool LeafBuildable(const roth::Map &m, const roth::Sector &rs)
{
	if (!rs.IsDoorCapable() || rs.faceCount != 4 || rs.hingeFace < 0) return false;
	if (rs.firstFaceIndex < 0 || rs.firstFaceIndex + 3 >= (int)m.faces.size()) return false;
	const int hr = rs.hingeFace - rs.firstFaceIndex;
	if (hr < 0 || hr > 3) return false;
	for (int j = 0; j < 4; j++)
	{
		const roth::Face &a = m.faces[rs.firstFaceIndex + j];
		const roth::Face &b = m.faces[rs.firstFaceIndex + ((j + 1) & 3)];
		if (a.vertex1 < 0 || a.vertex2 < 0 || a.vertex2 != b.vertex1) return false;
	}
	return true;
}

// The band of a see-through mid piece. compute_face_span_extents, renderer.c:8171-8215:
// top = min(ceilings), bottom = max(floors); with map flag 0x08 and the face's own
// sector's override byte (+0x0c) non-zero, cut to 4*|override| units from the top
// (override > 0) or from the bottom (override < 0).
static void MidBand(const roth::Map &m, const FaceGeo &g, uint8_t tf, double &top, double &bot, bool &banded)
{
	top = std::min(g.ownC, g.nC); bot = std::max(g.ownF, g.nF); banded = false;
	if ((tf & roth::FF_TRANS_UPLO))
	{
		const int tmo = m.sectors[g.sector].textureMapOverride;
		if (tmo > 0)      { bot = top - 4. * tmo; banded = true; }
		else if (tmo < 0) { top = bot - 4. * tmo; banded = true; }
	}
}

// The neighbour has a mid-platform this face draws the edge of, which sends the face
// down the masked branch of draw_world_face_clipped_spans (renderer.c:8781-8790):
// neighbour sector +0x18 non-zero, mid texture not the colour key, and
// project_wall_face_span_extents (8585-8621) accepting the heights.
static bool PlatformBranch(MapData &M, const FaceGeo &g, const roth::TextureMap &tm)
{
	if (g.nsector < 0) return false;
	const roth::Sector &ns = M.m.sectors[g.nsector];
	if (ns.platformIndex < 0 || ns.platformIndex >= (int)M.m.platforms.size()) return false;
	if (tm.midTexture == M.key) return false;
	const roth::MidPlatform &mp = M.m.platforms[ns.platformIndex];
	int ax = mp.undersideZ, dx = mp.topZ;
	if (ax >= mp.topZ) return false;
	if (ax >= g.ownC) return false;
	if (ax < g.ownF) ax = g.ownF;
	if (dx <= g.ownF) return false;
	if (dx > g.ownC) dx = g.ownC;
	const roth::Sector &os = M.m.sectors[g.sector];
	if (os.platformIndex >= 0 && os.platformIndex < (int)M.m.platforms.size())
	{
		const roth::MidPlatform &op = M.m.platforms[os.platformIndex];
		if (ax < op.topZ && dx > op.undersideZ)
		{
			if (ax >= op.undersideZ) ax = op.topZ;
			if (dx <= op.topZ) dx = op.undersideZ;
		}
	}
	return ax < dx;
}

//==========================================================================
// ROTH.C
//==========================================================================
static void RothPieces(MapData &M, int fi, PieceMap out[3])
{
	const FaceGeo &g = M.geo[fi];
	const roth::Face &f = M.m.faces[fi];
	const roth::TextureMap &tm = M.m.textureMaps[f.textureMap];
	const uint8_t tf = tm.flags;
	// renderer.c:9271-9279: the fit word's top bit selects the extended record;
	// start = 2*shiftX, row shift = shiftY, extent = low 12 bits.
	const int hfit = tm.StoredExtent();
	const int shX = tm.extended ? tm.shiftX : 0, shY = tm.extended ? tm.shiftY : 0;

	auto fill = [&](int tier, int texIdx, double top, double bot, uint8_t pf)
	{
		if (top <= bot) return;
		if (!M.Drawable(texIdx)) return;                 // 9222-9225: key draws nothing; solid colours carry no texel
		const roth::Image &im = M.Img(texIdx);
		PieceMap &p = out[tier];
		p.on = true; p.tex = texIdx;
		p.dimV = im.width;     // block +0xc, g_span_src_row_width (13288)
		p.dimU = im.height;    // block +0xe (13289)
		p.zTop = top; p.zBot = bot; p.zAnc = top;       // wrap_reoffset+0xa is the piece's own top: 8664, 8685, 8828, 8730
		const int h = (int)(top - bot);
		const bool half = (pf & roth::FF_HALF_PIXEL) != 0;
		p.mirror = (pf & roth::FF_FLIP_X) != 0;          // 4735: t = ~t + dimU
		if (!(pf & roth::FF_IMAGE_FIT))
		{
			// 8272: draw flag 0x100 <=> IMAGE_FIT clear. 13334-13340: extents are the
			// span's own (+0x26 = hfit across, +0x24 = piece height down), doubled by
			// HALF_PIXEL; all in half texels (4733-4734 halves the sum).
			const int extU = hfit * (half ? 2 : 1);
			p.u0 = shX; p.su = g.len > 0 ? (extU / 2.) / g.len : 0;
			// 13337-13338: the mask stays dimU-1 unless the run fits in one copy.
			const int16_t room = (int16_t)(uint16_t)(2 * p.dimU - 2 * shX);
			if (room >= (int16_t)(uint16_t)extU) p.uMode = U_PLAIN;
			else p.uMode = IsPow2(p.dimU) ? U_MOD : U_MASK;
			const int extV = h * (half ? 2 : 1);
			int start = shY;                              // 5050
			if (pf & roth::FF_PIN_BOTTOM)                 // 5056-5063
			{
				start = (int16_t)(((int16_t)(uint16_t)(2 * p.dimV - extV)) >> 1) - shY;
				if (start < 0) start &= (p.dimV - 1);
			}
			p.v0 = start; p.sv = half ? 1. : 0.5;         // 4739-4740: extV half texels down the piece
			// 5065-5069: the wrapping column mapper. On a non power of two it is
			// stateful and what it draws depends on the viewing distance (measured,
			// see roth_surface.cpp WrapRow); it is compared as a repeat and counted.
			if (2 * (p.dimV - start) < extV) { p.vMode = V_MOD; p.vStuck = !IsPow2(p.dimV); }
		}
		else
		{
			// 13342-13349 (span +0xf is never written for a world wall): shifts zeroed,
			// extents 2*dim each way, mask 0xffff. HALF_PIXEL is not read.
			p.fit = true;
			p.u0 = 0; p.su = g.len > 0 ? p.dimU / g.len : 0; p.uMode = U_PLAIN;
			p.v0 = 0; p.sv = h > 0 ? (double)p.dimV / h : 0; p.vMode = V_PLAIN;
		}
	};

	if (!g.twoSided)
	{
		// draw_world_face_projected_spans, single emit (8866-8876): the sector's whole height.
		fill(T_MID, tm.midTexture, g.ownC, g.ownF, tf);
		return;
	}
	// Every face with a neighbour takes draw_world_face_clipped_spans (edge flag 0x20,
	// renderer.c:7383/7390/7484 -> 9280).
	const bool transparent = (tf & roth::FF_TRANSPARENT) != 0;
	const bool masked = transparent || PlatformBranch(M, g, tm);
	// 8773 / 8785: the upper and lower are built with flags & 0xfa -- TRANSPARENT and
	// IMAGE_FIT cleared -- whenever the mid overlay follows; otherwise (8791) as stored.
	const uint8_t ef = masked ? (uint8_t)(tf & 0xfa) : tf;
	// build_world_face_edge_spans, 8658-8697.
	if (g.nC < g.ownC) fill(T_TOP, tm.upperTexture, g.ownC, std::max(g.nC, g.ownF), ef);
	if (g.nF > g.ownF) fill(T_BOT, tm.lowerTexture, std::min(g.nF, g.ownC), g.ownF, ef);
	if (transparent && !g.leafMid)
	{
		double top, bot; bool banded;
		MidBand(M.m, g, tf, top, bot, banded);
		fill(T_MID, tm.midTexture, top, bot, tf);          // finalize_world_span_overlay, 8727-8742
		// a band reaching past the opening: kept to the opening (the overhang's
		// visibility is a draw-order question this tool does not model)
		PieceMap &p = out[T_MID];
		if (p.on)
		{
			p.zTop = std::min(p.zTop, (double)std::min(g.ownC, g.nC));
			p.zBot = std::max(p.zBot, (double)std::max(g.ownF, g.nF));
			if (p.zTop <= p.zBot) p.on = false;
		}
	}
}

//==========================================================================
// The engine half of both loader models.
// gametexture.cpp GetFromTexture / TextureOffset / RowOffset, hw_walls.cpp
// SetWallCoordinates, DoTexture, DoMidTexture, Process.
//==========================================================================
struct SidePart
{
	bool on = false; int tex = -1; bool flippedPixels = false; double sx = 1, sy = 1, offX = 0, offY = 0;
	std::vector<uint16_t> colMap, rowMap;      // baked registration
};
struct SideModel { SidePart part[3]; int texelLength = 0; bool wrapMid = false; };

static int RenderDim(int disp, double scale)
{
	const float s = (float)scale;
	if (s == 1.f) return (int)floor(disp + 0.5);
	return (int)ceil((double)((float)disp / s));
}

static void EnginePieces(MapData &M, int fi, const SideModel &S, PieceMap out[3])
{
	const FaceGeo &g = M.geo[fi];
	auto fill = [&](int tier, double top, double bot, double refZ)
	{
		const SidePart &sp = S.part[tier];
		if (!sp.on || top <= bot) return;
		const roth::Image &im = M.Img(sp.tex);
		PieceMap &p = out[tier];
		p.on = true; p.tex = sp.tex; p.dimV = im.width; p.dimU = im.height;
		if (!sp.colMap.empty())
		{
			p.colMap = sp.colMap; p.rowMap = sp.rowMap;
			p.dimU = (int)sp.colMap.size(); p.dimV = (int)sp.rowMap.size();
		}
		p.zTop = top; p.zBot = bot;
		const int rw = RenderDim(p.dimU, sp.sx), rh = RenderDim(p.dimV, sp.sy);
		const float ax = fabsf((float)sp.sx), ay = fabsf((float)sp.sy);
		const double tofs = ax == 1.f ? sp.offX : sp.offX / ax;
		const double rofs = ay == 1.f ? sp.offY : sp.offY / ay;
		// u (in copies) = tofs/rw + (TexelLength/rw) * d/len
		const double ul = tofs / rw, ur = ul + (double)S.texelLength / rw;
		p.mirror = sp.flippedPixels;
		if (rw < 0)
		{
			// a negative scale: u runs 0 -> negative. Column = floor(u*dimU) mod dimU.
			p.mirror = !p.mirror;
			p.u0 = -ul * p.dimU; p.su = g.len > 0 ? (ul - ur) * p.dimU / g.len : 0;
		}
		else { p.u0 = ul * p.dimU; p.su = g.len > 0 ? (ur - ul) * p.dimU / g.len : 0; }
		p.uMode = U_MOD;
		// hw_walls.cpp:1585-1588 -> SetMaterial(..., flags & 3): clamp when the run
		// starts at 0 and ends at or below 1. A NEGATIVE run satisfies that.
		if (tier != T_MID || !g.twoSided)
		{
			if ((float)ul == 0.f && (float)ur <= 1.f) p.uMode = (ur < 0) ? U_SMEAR : U_PLAIN;
		}
		p.zAnc = refZ + rofs;
		p.v0 = 0; p.sv = (double)p.dimV / rh; p.vMode = V_MOD;
		if (tier == T_MID && g.twoSided && !S.wrapMid)
		{
			p.vMode = V_ONECOPY;                           // DoMidTexture: clipped to the texture, HWF_CLAMPY
			p.zTop = std::min(p.zTop, p.zAnc);
			p.zBot = std::max(p.zBot, p.zAnc - rh);
			if (p.zTop <= p.zBot) p.on = false;
		}
		// fold the anchor into v0 at the drawn top so maps compare directly
		p.v0 = p.sv * (p.zAnc - p.zTop); p.zAnc = p.zTop;
	};
	if (!g.twoSided) { fill(T_MID, g.ownC, g.ownF, g.ownC); return; }
	// upper: Process, "upper texture": top anchored at the front ceiling (ML_DONTPEGTOP)
	if (g.nC < g.ownC) fill(T_TOP, g.ownC, std::max(g.nC, g.ownF), g.ownC);
	// lower: anchored at the REAL back floor, drawn from min(back floor, front ceiling)
	if (g.nF > g.ownF) fill(T_BOT, std::min(g.nF, g.ownC), g.ownF, g.nF);
	fill(T_MID, std::min(g.ownC, g.nC), std::max(g.ownF, g.nF), std::min(g.ownC, g.nC));
}

static double OneCopyScale(double texels, double span, bool mirrored)
{
	const double whole = floor(span + 0.5);
	if (whole < 1.) return texels / span;
	return texels / (whole + (mirrored ? 0.5 : -0.5));
}

//==========================================================================
// rothmap.cpp makeSide. fixed = with the wallrule patch.
//==========================================================================
static void LoaderSide(MapData &M, int fi, bool fixed, SideModel &S)
{
	const FaceGeo &g = M.geo[fi];
	const roth::Face &f = M.m.faces[fi];
	const roth::TextureMap &tm = M.m.textureMaps[f.textureMap];
	const uint8_t tf = tm.flags;
	const bool twoSided = g.twoSided;
	const bool imageFit = (tf & roth::FF_IMAGE_FIT) != 0;
	const bool half = (tf & roth::FF_HALF_PIXEL) != 0;
	const bool flip = (tf & roth::FF_FLIP_X) != 0;
	const bool pin = (tf & roth::FF_PIN_BOTTOM) != 0;
	bool wantMid = !twoSided || (tf & roth::FF_TRANSPARENT) != 0;
	if (g.leafMid) wantMid = false;

	double stored = tm.StoredExtent();
	if (tm.extended && stored > 0. && g.len > 4096.)
		stored += 4096. * floor((g.len - stored) / 4096. + 0.5);
	S.texelLength = (int)(g.len + 0.5f);                  // maploader.cpp FinishLoadingLineDef

	double bandTop, bandBot; bool banded = false;
	const double openTop = twoSided ? std::min(g.ownC, g.nC) : g.ownC;
	const double openBot = twoSided ? std::max(g.ownF, g.nF) : g.ownF;
	bandTop = openTop; bandBot = openBot;
	if (twoSided && (tf & roth::FF_TRANSPARENT)) MidBand(M.m, g, tf, bandTop, bandBot, banded);

	if (!fixed)
	{
		const double base = (!imageFit && half) ? 1. : 0.5;
		if (!imageFit && stored > 0. && g.len > 0.) S.texelLength = (int)std::min(stored, 65535.);
		double offX = 0, offY = 0;
		if (tm.extended && !imageFit) { offX = tm.shiftX; offY = tm.shiftY; if (pin) offY = -offY; }
		auto heightOf = [&](int part) {
			if (!twoSided) return (double)(g.ownC - g.ownF);
			if (part == T_TOP) return (double)(g.ownC - g.nC);
			if (part == T_BOT) return (double)(g.nF - g.ownF);
			return bandTop - bandBot;
		};
		auto setPart = [&](int part, int idx) {
			if (!M.Drawable(idx)) return;
			const roth::Image &im = M.Img(idx);
			const double texW = im.height, texH = im.width;
			double sx = base, sy = base;
			const double ph = heightOf(part);
			if (imageFit)
			{
				if (g.len > 0) sx = OneCopyScale(texW, g.len, flip);
				if (ph > 0) sy = OneCopyScale(texH, ph, false);
			}
			if (flip) sx = -sx;
			double oy = offY;
			if (pin && ph > 0 && !imageFit) oy += texH - ph * sy;
			if (part == T_MID && banded) oy += (bandTop - openTop) * sy;
			SidePart &sp = S.part[part];
			sp.on = true; sp.tex = idx; sp.sx = sx; sp.sy = sy; sp.offX = offX; sp.offY = oy;
		};
		if (wantMid) setPart(T_MID, tm.midTexture);
		if (twoSided) { setPart(T_TOP, tm.upperTexture); setPart(T_BOT, tm.lowerTexture); }
		return;
	}

	// ---- FIXED: the patched makeSide. It asks roth_surface for everything. ----
	roth::WallFace wf;
	wf.flags = tf; wf.extended = tm.extended; wf.extent = tm.StoredExtent();
	wf.shiftX = tm.extended ? tm.shiftX : 0; wf.shiftY = tm.extended ? tm.shiftY : 0;
	wf.length = g.len; wf.twoSided = twoSided;
	wf.ownFloor = g.ownF; wf.ownCeil = g.ownC; wf.nbrFloor = g.nF; wf.nbrCeil = g.nC;
	wf.ownOverride = M.m.sectors[g.sector].textureMapOverride;
	if (twoSided)
	{
		const roth::Sector &os = M.m.sectors[g.sector], &ns = M.m.sectors[g.nsector];
		const bool oh = os.platformIndex >= 0 && os.platformIndex < (int)M.m.platforms.size();
		const bool nh = ns.platformIndex >= 0 && ns.platformIndex < (int)M.m.platforms.size();
		wf.overlayFollows = roth::WallOverlayFollows(tf, tm.midTexture == M.key, g.ownF, g.ownC,
			nh, nh ? M.m.platforms[ns.platformIndex].undersideZ : 0, nh ? M.m.platforms[ns.platformIndex].topZ : 0,
			oh, oh ? M.m.platforms[os.platformIndex].undersideZ : 0, oh ? M.m.platforms[os.platformIndex].topZ : 0);
	}
	const int tl = roth::WallTexelLength(wf);
	if (tl >= 0) S.texelLength = tl;
	auto setPart = [&](int part, int idx) {
		if (!M.Drawable(idx)) return;
		const roth::Image &im = M.Img(idx);
		const int dimU = im.height, dimV = im.width;
		const roth::WallPiece wp = roth::WallPieceRule(wf, (roth::WallTier)part, dimU, dimV);
		const roth::WallEngineSetup e = roth::WallToEngine(wf, (roth::WallTier)part, wp, dimU, dimV, S.texelLength);
		if (!e.drawn) return;
		SidePart &sp = S.part[part];
		sp.on = true; sp.tex = idx; sp.flippedPixels = e.mirrored;
		sp.sx = e.xScale; sp.sy = e.yScale; sp.offX = e.xOffset; sp.offY = e.yOffset;
		bool registeredBaked = false;
		if (e.baked)
		{
			std::vector<uint16_t> cm(4096), rm(4096); int nc = 0, nr = 0;
			roth::WallBakeMaps(wp, e, dimU, dimV, cm.data(), nc, rm.data(), nr, 4096);
			if (nc > 0 && nr > 0)
			{
				cm.resize(nc); rm.resize(nr); sp.colMap = cm; sp.rowMap = rm;
				bool inRange = true;
				for (uint16_t c : cm) if (c != 0xFFFF && c >= dimU) inRange = false;
				for (uint16_t r : rm) if (r != 0xFFFF && r >= dimV) inRange = false;
				M.regs.push_back({ idx, false, nc, nr, inRange });
				registeredBaked = true;
			}
			// (rothmap.cpp falls back to the plain image when the maps come back empty)
		}
		if (!registeredBaked) M.regs.push_back({ idx, e.mirrored, 0, 0, true });
	};
	if (wantMid) setPart(T_MID, tm.midTexture);
	if (twoSided) { setPart(T_TOP, tm.upperTexture); setPart(T_BOT, tm.lowerTexture); }
}

//==========================================================================
struct Tally
{
	long faces = 0, pieces = 0;
	long diff[2] = { 0, 0 };                      // pieces where the model samples other texels than ROTH
	std::map<std::string, long> why[2];
	long origMask = 0, origStuck = 0;             // ROTH: column mask on a non power of two / row mapper on one
	long joins = 0, joinCont[3] = { 0, 0, 0 };    // ROTH, HEAD, FIXED
	long joinBroken[2] = { 0, 0 };                // continuous in ROTH, broken in the model
	long regs = 0, badRegs = 0;                   // distinct wall images the patched loader registers
};

static bool LoadMap(const std::string &name, MapData &M)
{
	std::vector<uint8_t> d = ReadFile(roth::TheInstall().MapFile(name.c_str()));
	if (d.empty()) return false;
	M.m = roth::ParseRaw(d.data(), d.size());
	if (!M.m.ok()) return false;
	M.packBytes = ReadFile(roth::TheInstall().PackFile(roth::TheInstall().PackFor(name.c_str()).c_str()));
	if (M.packBytes.empty() || !M.pack.Load(M.packBytes.data(), M.packBytes.size())) return false;
	M.key = M.pack.SkyMarkerIndex();
	const roth::Map &m = M.m;
	M.geo.resize(m.faces.size());
	for (size_t fi = 0; fi < m.faces.size(); fi++)
	{
		const roth::Face &f = m.faces[fi];
		FaceGeo &g = M.geo[fi];
		if (f.sector < 0 || f.vertex1 < 0 || f.vertex2 < 0) continue;
		if (f.textureMap < 0 || f.textureMap >= (int)m.textureMaps.size()) continue;
		g.x1 = m.vertices[f.vertex1].x; g.y1 = m.vertices[f.vertex1].y;
		g.x2 = m.vertices[f.vertex2].x; g.y2 = m.vertices[f.vertex2].y;
		g.len = hypot((double)g.x2 - g.x1, (double)g.y2 - g.y1);
		if (g.len <= 0) continue;
		g.ok = true; g.sector = f.sector;
		g.ownF = m.sectors[f.sector].floorHeight; g.ownC = m.sectors[f.sector].ceilingHeight;
		g.twoSided = f.sister >= 0 && f.sister < (int)m.faces.size() && m.faces[f.sister].sector >= 0;
		if (g.twoSided)
		{
			g.nsector = m.faces[f.sister].sector;
			g.nF = m.sectors[g.nsector].floorHeight; g.nC = m.sectors[g.nsector].ceilingHeight;
			const uint8_t tf = m.textureMaps[f.textureMap].flags;
			if ((tf & roth::FF_TRANSPARENT) && (LeafBuildable(m, m.sectors[f.sector]) || LeafBuildable(m, m.sectors[g.nsector])))
				g.leafMid = true;
		}
	}
	return true;
}

// Does `b` sample the same texels as `a` (ROTH)? Returns "" or a cause.
static std::string Compare(MapData &M, int fi, int tier, const PieceMap &a, const PieceMap &b)
{
	const FaceGeo &g = M.geo[fi];
	const uint8_t tf = M.m.textureMaps[M.m.faces[fi].textureMap].flags;
	if (a.on != b.on) return a.on ? "piece missing" : "piece the original does not draw";
	if (!a.on) return "";
	if (a.tex != b.tex) return "wrong texture";
	int bad = 0, n = 0, worstC = 0, worstR = 0; bool extent = false;
	if (fabs(a.zTop - b.zTop) > 0.01 || fabs(a.zBot - b.zBot) > 0.01) extent = true;
	for (int i = 0; i < 9; i++)
	{
		const double d = g.len * (i + 0.5) / 9.;
		for (int j = 0; j < 9; j++)
		{
			const double z = a.zTop - (a.zTop - a.zBot) * (j + 0.5) / 9.;
			int ca, ra, cb, rb;
			const int sa = SampleCR(a, d, z, ca, ra), sb = SampleCR(b, d, z, cb, rb);
			n++;
			if (sa == -3) continue;                       // the original reads outside the image: no reference
			if (sb != 0) { bad++; continue; }
			int dc = abs(ca - cb), dr = abs(ra - rb);
			if (a.uMode != U_PLAIN || b.uMode == U_MOD) dc = std::min(dc, a.dimU - dc);
			if (a.vMode != V_PLAIN || b.vMode == V_MOD) dr = std::min(dr, a.dimV - dr);
			worstC = std::max(worstC, dc); worstR = std::max(worstR, dr);
			if (dc > 1 || dr > 1) bad++;
		}
	}
	if (!bad && !extent) return "";
	if (a.uMode == U_MASK || a.vMode == V_MASK) return "original masks a non power of two";
	if (b.uMode == U_SMEAR) return "FLIP_X smeared (negative scale + clamp)";
	if (extent && tier == T_MID && g.twoSided) return "see-through mid: one copy, no tiling / band overrun";
	if (extent) return "drawn extent";
	if (tier != T_MID && (tf & roth::FF_IMAGE_FIT) && !a.fit) return "IMAGE_FIT applied to an upper/lower the original draws unfitted";
	if (tier == T_BOT && g.nF > g.ownC) return "lower piece anchored above its own top";
	if (worstC > 1 && worstR <= 1) return "columns";
	if (worstR > 1 && worstC <= 1) return "rows";
	return "columns and rows";
}

static int PixelAt(MapData &M, const PieceMap &p, double d, double z)
{
	int c, r;
	if (SampleCR(p, d, z, c, r) != 0) return -1;
	const roth::Image &im = M.Img(p.tex);
	const size_t i = (size_t)c * im.width + r;       // stored rows run along the wall
	return i < im.frames[0].size() ? im.frames[0][i] : -1;
}

static double ColDist(MapData &M, int a, int b)
{
	if (a < 0 || b < 0) return 255.;
	const auto &P = M.pack.Palette();
	if (a >= (int)P.size() || b >= (int)P.size()) return 255.;
	return (abs(P[a].r - P[b].r) + abs(P[a].g - P[b].g) + abs(P[a].b - P[b].b)) / 3.;
}

// Does the picture run on from A's last column into B's first, over the heights both draw?
// Compared against how much A's own last two columns and B's own first two differ.
static int JoinContinuous(MapData &M, const FaceGeo &ga, const PieceMap &A, const FaceGeo &gb, const PieceMap &B, double top, double bot)
{
	if (!A.on || !B.on) return -1;
	const double stepA = A.su != 0 ? 1. / fabs(A.su) : 1., stepB = B.su != 0 ? 1. / fabs(B.su) : 1.;
	double across = 0, within = 0; int n = 0;
	for (double z = top - 0.5; z > bot; z -= 1.)
	{
		const int a1 = PixelAt(M, A, ga.len - 0.5 * stepA, z), a0 = PixelAt(M, A, ga.len - 1.5 * stepA, z);
		const int b0 = PixelAt(M, B, 0.5 * stepB, z), b1 = PixelAt(M, B, 1.5 * stepB, z);
		if (a1 < 0 || b0 < 0) continue;
		across += ColDist(M, a1, b0);
		within += 0.5 * (ColDist(M, a0 < 0 ? a1 : a0, a1) + ColDist(M, b0, b1 < 0 ? b0 : b1));
		n++;
	}
	if (n < 8) return -1;
	across /= n; within /= n;
	return across <= within * 1.6 + 6. ? 1 : 0;
}

// Every wall image the patched loader would register, as roth_texture.cpp Build makes
// it: width = baked columns or the picture's stored height, height = baked rows or its
// stored row length (the quarter turn), and every frame's buffer must hold w*h bytes.
// GZDoom keeps texture sizes in uint16_t (FTexture::Width/Height, textures.h), and the
// Vulkan backend refuses a 0 (vk_hwtexture.cpp CreateTexture).
static long CheckRegistrations(MapData &M, const std::string &name, long &bad)
{
	std::map<std::tuple<int, bool, int, int>, bool> seen;
	for (const MapData::Reg &r : M.regs)
	{
		auto key = std::make_tuple(r.idx, r.mirrored, r.cols, r.rows);
		if (seen.count(key)) continue;
		seen[key] = true;
		const roth::Image all = M.pack.ReadImage(r.idx, true);
		const int w = r.cols ? r.cols : all.height, h = r.rows ? r.rows : all.width;
		std::string why;
		if (!all.ok()) why = "picture does not decode";
		else if (w <= 0 || h <= 0) why = "zero size";
		else if (w > 65535 || h > 65535) why = "size does not fit uint16";
		else if (!r.mapsInRange) why = "bake map points outside the picture";
		else
			for (size_t f = 0; f < all.frames.size(); f++)
				if (all.frames[f].size() < (size_t)all.width * all.height) { why = "frame " + std::to_string(f) + " smaller than the picture"; break; }
		if (getenv("WALLRULE_REGS"))
			printf("  REG %s index %d %s%s %dx%d frames %zu\n", name.c_str(), r.idx, r.mirrored ? "mirrored " : "",
				r.cols ? "baked" : "plain", w, h, all.frames.size());
		if (!why.empty())
		{
			bad++;
			printf("  BAD REGISTRATION %s index %d %s%s %dx%d: %s\n", name.c_str(), r.idx, r.mirrored ? "mirrored " : "",
				r.cols ? "baked" : "plain", w, h, why.c_str());
		}
	}
	return (long)seen.size();
}

static void RunMap(const std::string &name, bool detail, const char *exportPath, Tally &T)
{
	MapData M;
	if (!LoadMap(name, M)) { printf("%s: cannot load\n", name.c_str()); return; }
	const size_t nf = M.m.faces.size();
	std::vector<PieceMap> R(nf * 3), H(nf * 3), F(nf * 3);
	for (size_t fi = 0; fi < nf; fi++)
	{
		if (!M.geo[fi].ok) continue;
		T.faces++;
		RothPieces(M, (int)fi, &R[fi * 3]);
		SideModel sh, sf;
		LoaderSide(M, (int)fi, false, sh); EnginePieces(M, (int)fi, sh, &H[fi * 3]);
		LoaderSide(M, (int)fi, true, sf);  EnginePieces(M, (int)fi, sf, &F[fi * 3]);
		for (int t = 0; t < 3; t++)
		{
			const PieceMap &a = R[fi * 3 + t];
			if (a.on) T.pieces++;
			if (a.on && a.uMode == U_MASK) T.origMask++;
			if (a.on && a.vStuck) T.origStuck++;
			for (int k = 0; k < 2; k++)
			{
				const std::string w = Compare(M, (int)fi, t, a, k ? F[fi * 3 + t] : H[fi * 3 + t]);
				if (w.empty()) continue;
				T.diff[k]++; T.why[k][w]++;
				if (detail)
				{
					const FaceGeo &g = M.geo[fi];
					const roth::TextureMap &tm = M.m.textureMaps[M.m.faces[fi].textureMap];
					printf("  %s face %-5d sector %-4d %-5s tex %-4d (%d,%d)->(%d,%d) z %d..%d nbr %d..%d flags %02x hfit %d sh %d,%d : %s\n",
						k ? "FIXED" : "HEAD ", (int)fi, g.sector, TierName(t), a.on ? a.tex : -1, g.x1, g.y1, g.x2, g.y2,
						g.ownF, g.ownC, g.nF, g.nC, tm.flags, tm.StoredExtent(), tm.extended ? tm.shiftX : 0, tm.extended ? tm.shiftY : 0, w.c_str());
				}
			}
		}
	}

	T.regs += CheckRegistrations(M, name, T.badRegs);

	// Joins: B starts where A ends, same direction (coplanar, same facing), any sectors.
	std::map<std::pair<int, int>, std::vector<int>> startsAt;
	for (size_t fi = 0; fi < nf; fi++) if (M.geo[fi].ok) startsAt[{ M.geo[fi].x1, M.geo[fi].y1 }].push_back((int)fi);
	for (size_t ai = 0; ai < nf; ai++)
	{
		const FaceGeo &ga = M.geo[ai];
		if (!ga.ok) continue;
		auto it = startsAt.find({ ga.x2, ga.y2 });
		if (it == startsAt.end()) continue;
		for (int bi : it->second)
		{
			const FaceGeo &gb = M.geo[bi];
			const double cross = (double)(ga.x2 - ga.x1) * (gb.y2 - gb.y1) - (double)(ga.y2 - ga.y1) * (gb.x2 - gb.x1);
			const double dot = (double)(ga.x2 - ga.x1) * (gb.x2 - gb.x1) + (double)(ga.y2 - ga.y1) * (gb.y2 - gb.y1);
			if (dot <= 0 || fabs(cross) > 0.02 * ga.len * gb.len) continue;
			for (int ta = 0; ta < 3; ta++) for (int tb = 0; tb < 3; tb++)
			{
				const PieceMap &ra = R[ai * 3 + ta], &rb = R[bi * 3 + tb];
				if (!ra.on || !rb.on) continue;
				const double top = std::min(ra.zTop, rb.zTop), bot = std::max(ra.zBot, rb.zBot);
				if (top - bot < 8) continue;
				const int cr = JoinContinuous(M, ga, ra, gb, rb, top, bot);
				if (cr < 0) continue;
				T.joins++;
				if (cr) T.joinCont[0]++;
				const PieceMap *mod[2][2] = { { &H[ai * 3 + ta], &H[bi * 3 + tb] }, { &F[ai * 3 + ta], &F[bi * 3 + tb] } };
				for (int k = 0; k < 2; k++)
				{
					const int cm = JoinContinuous(M, ga, *mod[k][0], gb, *mod[k][1], top, bot);
					if (cm == 1) T.joinCont[k + 1]++;
					if (cr == 1 && cm != 1)
					{
						T.joinBroken[k]++;
						if (detail)
							printf("  %s JOIN broken at (%d,%d): face %d %s tex %d (sector %d) -> face %d %s tex %d (sector %d), z %.0f..%.0f\n",
								k ? "FIXED" : "HEAD ", ga.x2, ga.y2, (int)ai, TierName(ta), ra.tex, ga.sector, bi, TierName(tb), rb.tex, gb.sector, bot, top);
					}
				}
			}
		}
	}

	if (exportPath)
	{
		FILE *o = fopen(exportPath, "w");
		if (o)
		{
			fprintf(o, "key %d\n", M.key);
			for (size_t si = 0; si < M.m.sectors.size(); si++)
				fprintf(o, "S %zu %d %d %d %d\n", si, M.m.sectors[si].floorHeight, M.m.sectors[si].ceilingHeight,
					M.m.sectors[si].firstFaceIndex, M.m.sectors[si].faceCount);
			for (size_t fi = 0; fi < nf; fi++)
			{
				const FaceGeo &g = M.geo[fi];
				if (!g.ok) continue;
				fprintf(o, "F %zu %d %d %d %d %d %d %d\n", fi, g.x1, g.y1, g.x2, g.y2, g.sector, g.nsector,
					M.m.faces[fi].sister);
				const std::vector<PieceMap> *sets[3] = { &R, &H, &F };
				for (int k = 0; k < 3; k++) for (int t = 0; t < 3; t++)
				{
					const PieceMap &p = (*sets[k])[fi * 3 + t];
					if (!p.on) continue;
					fprintf(o, "P %d %d %d %d %d %g %g %.9g %.9g %d %d %g %.9g %.9g %d\n", k, t, p.tex, p.dimU, p.dimV,
						p.zTop, p.zBot, p.u0, p.su, p.mirror ? 1 : 0, p.uMode, p.zAnc, p.v0, p.sv, p.vMode);
				}
			}
			fclose(o);
		}
	}
}

int main(int argc, char **argv)
{
	std::vector<std::string> args; const char *exportPath = nullptr;
	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "-export") && i + 1 < argc) exportPath = argv[++i];
		else args.push_back(argv[i]);
	}
	if (args.empty()) { printf("usage: wallrule <DATA dir> [MAP] [-export file]\n"); return 2; }
	if (!roth::TheInstall().Open(args[0].c_str())) { printf("install: %s\n", roth::TheInstall().Error().c_str()); return 2; }
	Tally all;
	const bool one = args.size() > 1;
	printf("%-10s %6s %7s | %8s %8s | %6s %6s %6s %6s | %7s %7s\n", "map", "faces", "pieces", "HEAD!=", "FIXED!=",
		"joins", "ROTHok", "HEADok", "FIXok", "HEADbrk", "FIXbrk");
	for (const std::string &name : roth::TheInstall().MapNames())
	{
		if (one && name != args[1]) continue;
		Tally T;
		RunMap(name, one, one ? exportPath : nullptr, T);
		printf("%-10s %6ld %7ld | %8ld %8ld | %6ld %6ld %6ld %6ld | %7ld %7ld\n", name.c_str(), T.faces, T.pieces,
			T.diff[0], T.diff[1], T.joins, T.joinCont[0], T.joinCont[1], T.joinCont[2], T.joinBroken[0], T.joinBroken[1]);
		all.faces += T.faces; all.pieces += T.pieces; all.origMask += T.origMask; all.origStuck += T.origStuck; all.regs += T.regs; all.badRegs += T.badRegs; all.joins += T.joins;
		for (int k = 0; k < 2; k++) { all.diff[k] += T.diff[k]; all.joinBroken[k] += T.joinBroken[k]; for (auto &kv : T.why[k]) all.why[k][kv.first] += kv.second; }
		for (int k = 0; k < 3; k++) all.joinCont[k] += T.joinCont[k];
	}
	printf("%-10s %6ld %7ld | %8ld %8ld | %6ld %6ld %6ld %6ld | %7ld %7ld\n", "TOTAL", all.faces, all.pieces,
		all.diff[0], all.diff[1], all.joins, all.joinCont[0], all.joinCont[1], all.joinCont[2], all.joinBroken[0], all.joinBroken[1]);
	printf("\nwall images the patched loader registers: %ld distinct, %ld bad\n", all.regs, all.badRegs);
	printf("\npieces whose COLUMNS the original masks with (dim-1) on a non power of two (a fixed scramble; reproduced): %ld\n", all.origMask);
	printf("pieces whose ROWS run past a non power of two image (the original's result depends on distance; drawn as a repeat): %ld\n", all.origStuck);
	for (int k = 0; k < 2; k++)
	{
		printf("\n%s: pieces sampling other texels than ROTH.C, by cause\n", k ? "FIXED" : "HEAD");
		for (auto &kv : all.why[k]) printf("  %7ld  %s\n", kv.second, kv.first.c_str());
	}
	return 0;
}
