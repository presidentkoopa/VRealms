// wallseams.cpp -- wall texture U/V: what ROTH.C samples against what the
// REMAROTH loader makes GZDoom sample, per wall piece and per join, offline.
//
// For every wall piece (lower / mid / upper tier of a face) this computes the
// continuous texture coordinate along the face at its first and last vertex,
// and the vertical coordinate at the piece's top, under two rules:
//
//   ROTH    the original's wall driver:
//             renderer.c:9272-9279  shiftX*2, shiftY, hfit (12 bits if extended)
//             renderer.c:8270-8277  draw flag 0x100 = NOT IMAGE_FIT
//             renderer.c:13334-13340 stored-extent path, HALF_PIXEL doubles
//             renderer.c:13342-13361 IMAGE_FIT path: 2*dim, shifts zeroed
//             renderer.c:4733-4737  texel = ((a + start) >> 1), FLIP_X, mask
//             renderer.c:5057-5063  DRAW_FROM_BOTTOM vertical start
//   REMA    the loader's arithmetic, src/maploader/rothmap.cpp:1164-1253,
//           1350-1378, 1401-1448, pushed through the engine's own texcoord
//           code: FTexCoordInfo::GetFromTexture / TextureOffset / RowOffset
//           (src/common/textures/gametexture.cpp:499-570),
//           HWWall::SetWallCoordinates (hw_walls.cpp:1378-1381, 1490-1491) and
//           FinishLoadingLineDef's TexelLength (maploader.cpp:1563), in float
//           where the engine is in float.
//
// A JOIN is a pair of faces A, B with A.vertex2 == B.vertex1, in the same
// sector or in two sectors that meet at that vertex, wearing the same texture
// on the same tier. The coordinate jump across it is B(start) - A(end), modulo
// the texture's horizontal texel count.
//
// Build (from the repo root):
//   sh tools/wallseams/build.sh            (plain g++, no engine headers)
// Run:
//   /tmp/wallseams "<...>/Realms of the Haunting/DATA"            all maps
//   /tmp/wallseams "<...>/Realms of the Haunting/DATA" STUDY1     one map + lists
//   -fixed   model the loader with the TexelLength / OneCopyScale change
//            (rothmap.cpp, "THE EXTENT GOES INTO TexelLength") instead of HEAD's
//   -joins   also list every join the ORIGINAL itself breaks
//
// What it does NOT cover: lighting, which tier is drawn at all, door leaves
// (rothmap.cpp's polyobject path), the runtime shift re-apply
// (roth_runtime.cpp ApplyTexmapShift), floors and ceilings, and near-plane
// clipping in the original. The two-sided mid piece is modelled as the loader
// builds it; pieces handed to a door leaf are left out, as the loader does.

#include "roth_raw.h"
#include "roth_install.h"
#include "roth_das.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

static bool DUMP_JOINS = false;    // -joins: every join the ORIGINAL breaks, with its record fields
static bool MODEL_FIXED = false;   // -fixed: the loader after tools/wallseams' patch

static std::vector<uint8_t> ReadFile(const std::string &p)
{
	std::vector<uint8_t> o; FILE *f = fopen(p.c_str(), "rb"); if (!f) return o;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); o.resize((size_t)n);
	if (n > 0 && fread(o.data(), 1, (size_t)n, f) != (size_t)n) o.clear();
	fclose(f); return o;
}

enum Tier { T_TOP = 0, T_MID = 1, T_BOT = 2 };   // side_t::top / mid / bottom
static const char *TierName(int t) { return t == T_TOP ? "upper" : t == T_MID ? "mid" : "lower"; }

struct Piece
{
	bool present = false;
	int tex = 0;
	int texH = 0;      // horizontal texel count  = stored image height  (renderer.c:13289, block +0xe)
	int texW = 0;      // vertical texel count    = stored row length    (renderer.c:13288, block +0xc)
	double height = 0; // world units, top to bottom of the piece
	// ROTH: continuous coordinate, texels. u at v1, u at v2, texels per world unit.
	double ru0 = 0, ru1 = 0, rsu = 0;
	double rv0 = 0, rsv = 0;          // v at the piece top, texels per world unit down
	bool   rWrapsNonPow2 = false;     // the original masks with (dim-1) on a non power of two
	// REMA: the same, as the engine will sample it.
	double mu0 = 0, mu1 = 0, msu = 0;
	double mv0 = 0, msv = 0;
	bool   midNoTile = false;         // see-through mid taller than one copy: engine draws one
};

struct FaceInfo
{
	Piece p[3];
	bool twoSided = false;
	double len = 0;
};

static double WrapDiff(double d, double n)
{
	if (n <= 0) return d;
	d = fmod(d, n);
	if (d > n / 2) d -= n;
	if (d < -n / 2) d += n;
	return d;
}

static bool IsPow2(int n) { return n > 0 && (n & (n - 1)) == 0; }

// ---------------------------------------------------------------------------
// The engine half of REMA. Mirrors gametexture.cpp:543-570 (GetFromTexture),
// :512-517 (TextureOffset), :499-504 (RowOffset), gametexture.h:514-515
// (FloatToTexU/V) and hw_walls.cpp:1378-1381 + 1490-1491. Texture scale is 1
// and world panning is off for every Realms texture (nothing in src/roth sets
// either), so mScale == the sidedef scale.
// ---------------------------------------------------------------------------
struct EngineU { double u0, u1; };

static EngineU EngineAlong(double sideScaleX, double sideOffX, int texDisplayW, int texelLength)
{
	const float x = (float)sideScaleX;
	int renderW;
	if (x == 1.f) renderW = (int)floor((double)texDisplayW + 0.5);
	else          renderW = (int)ceil((double)((float)texDisplayW / x));      // RoundUp
	const float ax = fabsf(x);
	const float tofs = (float)sideOffX;
	const float off = (ax == 1.f) ? tofs : tofs / ax;                          // TextureOffset
	const float l_ul = off / renderW;                                          // FloatToTexU
	const float texlength = (float)texelLength / renderW;
	EngineU e;
	e.u0 = (double)(l_ul + texlength * 0.f) * texDisplayW;
	e.u1 = (double)(l_ul + texlength * 1.f) * texDisplayW;
	return e;
}

struct EngineV { double v0, sv; };

static EngineV EngineDown(double sideScaleY, double sideOffY, int texDisplayH)
{
	const float y = (float)sideScaleY;
	int renderH;
	if (y == 1.f) renderH = (int)floor((double)texDisplayH + 0.5);
	else          renderH = (int)ceil((double)((float)texDisplayH / y));
	const float ay = fabsf(y);
	const float rofs = (float)sideOffY;
	const float off = (ay == 1.f) ? rofs : rofs / ay;                          // RowOffset
	EngineV e;
	e.v0 = (double)(off / renderH) * texDisplayH;       // texel row at the anchor (the piece top)
	e.sv = (double)texDisplayH / renderH;               // texel rows per world unit
	return e;
}

struct MapResult
{
	std::string name;
	long faces = 0, pieces = 0;
	long windingBad = 0, sisterNotReversed = 0, sisterNotMutual = 0;
	long startDiffer = 0, endDiffer = 0;           // |REMA - ROTH| > 0.25 texel at v1 / at v2
	long scaleDiffer = 0;                          // texels per unit differ by > 0.1%
	long scaleDiffer1 = 0, scaleDiffer10 = 0;      // > 1%, > 10%
	long scaleBy12bit = 0, scaleByRounding = 0, scaleByZeroExtent = 0, scaleOther = 0;
	double worstScale = 1.0;
	long vAnchorDiffer = 0, vScaleDiffer = 0, midNoTile = 0;
	long nonPow2Wrap = 0;
	long joins = 0, joinsContOrig = 0, joinsBrokenOrig = 0;
	long seams05 = 0, seams25 = 0, seams100 = 0;   // continuous in ROTH, REMA jump > 0.05/0.25/1 texel
	long origSeamKept = 0, origSeamChanged = 0;    // discontinuous in ROTH: REMA reproduces it or not (0.25)
	long origSeamHealed = 0;
	long flipShiftFaces = 0;
	long colJoins = 0, colContOrig = 0, colSeams25 = 0;   // joins within 10 degrees of straight
	long colAltV2 = 0;                                    // ...continuous if U ran from v2 instead (control)
};

struct Offender
{
	double key; int faceA, faceB, tier, tex, sector; int ax1, ay1, ax2, ay2, bx2, by2;
	double rJump, mJump; double rsA, msA, rsB, msB;
};
struct ScaleOff
{
	double key; int face, tier, tex, sector; int x1, y1, x2, y2; double rs, ms; int hfit; double len; int flags;
};

static bool RunMap(const std::string &name, bool detail, MapResult &R)
{
	R.name = name;
	std::vector<uint8_t> d = ReadFile(roth::TheInstall().MapFile(name.c_str()));
	if (d.empty()) return false;
	roth::Map m = roth::ParseRaw(d.data(), d.size());
	if (!m.ok()) return false;
	std::vector<uint8_t> pd = ReadFile(roth::TheInstall().PackFile(
		roth::TheInstall().PackFor(name.c_str()).c_str()));
	if (pd.empty()) return false;
	roth::Pack pack;
	if (!pack.Load(pd.data(), pd.size())) return false;
	const uint16_t key = pack.SkyMarkerIndex();

	std::map<int, std::pair<int, int>> shape;
	auto shapeOf = [&](int idx) {
		auto it = shape.find(idx);
		if (it != shape.end()) return it->second;
		roth::Image img = pack.ReadImage(idx, false);
		std::pair<int, int> s = { img.ok() ? img.width : 0, img.ok() ? img.height : 0 };
		shape[idx] = s;
		return s;
	};

	// rothmap.cpp:512-530
	auto leafBuildable = [&](const roth::Sector &rs) -> bool {
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
	};

	// Winding: GZDoom's front side is the RIGHT of v1->v2. A sector loop that
	// runs clockwise in map coordinates has its interior on the right.
	for (const roth::Sector &s : m.sectors)
	{
		if (s.firstFaceIndex < 0) continue;
		double area2 = 0;
		for (int j = 0; j < s.faceCount; j++)
		{
			const roth::Face &f = m.faces[s.firstFaceIndex + j];
			if (f.vertex1 < 0 || f.vertex2 < 0) continue;
			const roth::Vertex &a = m.vertices[f.vertex1], &b = m.vertices[f.vertex2];
			area2 += (double)a.x * b.y - (double)b.x * a.y;
		}
		if (area2 > 0) R.windingBad++;     // counter-clockwise: interior on the LEFT
	}

	std::vector<FaceInfo> info(m.faces.size());
	std::vector<ScaleOff> scaleOff;

	for (size_t fi = 0; fi < m.faces.size(); fi++)
	{
		const roth::Face &f = m.faces[fi];
		if (f.sector < 0 || f.vertex1 < 0 || f.vertex2 < 0) continue;
		if (f.textureMap < 0 || f.textureMap >= (int)m.textureMaps.size()) continue;
		const roth::TextureMap &tm = m.textureMaps[f.textureMap];
		const roth::Sector &own = m.sectors[f.sector];
		const bool twoSided = f.sister >= 0 && f.sister < (int)m.faces.size()
			&& m.faces[f.sister].sector >= 0;
		const roth::Sector *nbr = twoSided ? &m.sectors[m.faces[f.sister].sector] : nullptr;
		R.faces++;
		if (twoSided)
		{
			const roth::Face &s = m.faces[f.sister];
			if (s.vertex1 != f.vertex2 || s.vertex2 != f.vertex1) R.sisterNotReversed++;
			if (s.sister != (int)fi) R.sisterNotMutual++;
		}

		const roth::Vertex &a = m.vertices[f.vertex1], &b = m.vertices[f.vertex2];
		const double len = hypot((double)b.x - a.x, (double)b.y - a.y);
		FaceInfo &F = info[fi];
		F.twoSided = twoSided;
		F.len = len;
		if (len <= 0) continue;

		const uint8_t tf = tm.flags;
		const bool fit  = (tf & roth::FF_IMAGE_FIT) != 0;
		const bool half = (tf & roth::FF_HALF_PIXEL) != 0;
		const bool flip = (tf & roth::FF_FLIP_X) != 0;
		const bool pinB = (tf & roth::FF_PIN_BOTTOM) != 0;
		const bool trans = (tf & roth::FF_TRANSPARENT) != 0;
		const int hfit = tm.StoredExtent();          // renderer.c:9273-9279

		const double ownF = own.floorHeight, ownC = own.ceilingHeight;
		const double nF = nbr ? nbr->floorHeight : 0, nC = nbr ? nbr->ceilingHeight : 0;
		const double openTop = nbr ? std::min(ownC, nC) : ownC;
		const double openBot = nbr ? std::max(ownF, nF) : ownF;
		double bandTop = openTop, bandBot = openBot;
		bool banded = false;
		if (twoSided && trans && (tf & roth::FF_TRANS_UPLO))       // renderer.c:8196-8215
		{
			const int tmo = own.textureMapOverride;
			if (tmo > 0)      { bandBot = openTop - 4. * tmo; banded = true; }
			else if (tmo < 0) { bandTop = openBot - 4. * tmo; banded = true; }
		}

		bool wantMid = !twoSided || trans;
		if (twoSided && trans && (leafBuildable(own) || (nbr && leafBuildable(*nbr)))) wantMid = false;

		// --- REMA: the loader's per-face numbers, rothmap.cpp:1164-1253 --------
		const double unitsPerTexel = (!fit && half) ? 1. : 2.;
		const double baseScale = 1. / unitsPerTexel;
		double scaleX = baseScale;
		int texelLength = (int)(len + 0.5f);          // maploader.cpp:1563
		double stored = (double)hfit;
		bool restored12 = false;
		if (tm.extended && stored > 0. && len > 4096.)
		{
			const double s2 = stored + 4096. * floor((len - stored) / 4096. + 0.5);
			restored12 = (s2 != stored);
			stored = s2;
		}
		if (!fit && stored > 0.)
		{
			scaleX = (stored * 0.5) / len;
			if (half) scaleX *= 2.;
			// the patch: the base scale exactly, and the stored extent becomes
			// the side's TexelLength (set after FinishLoadingLineDef).
			if (MODEL_FIXED) { scaleX = baseScale; texelLength = (int)stored; }
		}
		double offX = 0., offY = 0.;
		if (tm.extended && !fit)
		{
			offX = tm.shiftX; offY = tm.shiftY;
			if (pinB) offY = -offY;
		}
		if (flip && tm.extended && !fit && tm.shiftX) R.flipShiftFaces++;

		for (int tier = 0; tier < 3; tier++)
		{
			double height; int texIdx;
			if (tier == T_MID)
			{
				if (!wantMid) continue;
				texIdx = tm.midTexture;
				height = twoSided ? bandTop - bandBot : ownC - ownF;
			}
			else if (tier == T_TOP)
			{
				if (!twoSided) continue;
				texIdx = tm.upperTexture; height = ownC - nC;
			}
			else
			{
				if (!twoSided) continue;
				texIdx = tm.lowerTexture; height = nF - ownF;
			}
			if (height <= 0) continue;
			if ((int16_t)(uint16_t)texIdx < 0) continue;      // solid colour, no texture coordinate
			if ((uint16_t)texIdx == key) continue;            // colour key: nothing drawn
			const std::pair<int, int> sh = shapeOf(texIdx);
			if (sh.first <= 0 || sh.second <= 0) continue;

			Piece &P = F.p[tier];
			P.present = true;
			P.tex = texIdx;
			P.texW = sh.first;     // vertical texels
			P.texH = sh.second;    // horizontal texels
			P.height = height;

			// --- ROTH -------------------------------------------------------
			double base0, base1;   // before FLIP_X
			if (!fit)
			{
				// renderer.c:13335-13339: horizontal extent = hfit half-texels across
				// the face (doubled by HALF_PIXEL); 9274: start = 2*shiftX half-texels.
				const double ext = (double)hfit * (half ? 2. : 1.);
				const double sh2 = tm.extended ? 2. * tm.shiftX : 0.;
				base0 = sh2 / 2.;
				base1 = (ext + sh2) / 2.;                    // (a + start) >> 1, renderer.c:4733-4734
				P.rsu = (ext / 2.) / len;
				// 13337-13338: mask stays dim-1 unless the run fits inside one copy.
				const double room = 2. * P.texH - sh2;
				if (room < ext && !IsPow2(P.texH)) P.rWrapsNonPow2 = true;
				// vertical
				const double extV = height * (half ? 2. : 1.);     // half-texels down the piece
				const double shY = tm.extended ? tm.shiftY : 0.;
				P.rsv = (half ? 1. : 0.5);
				if (pinB) P.rv0 = P.texW - extV / 2. - shY;        // renderer.c:5057-5063
				else      P.rv0 = shY;                             // renderer.c:5050
			}
			else
			{
				// renderer.c:13342-13349: shifts zeroed, extents 2*dim -> one copy each way.
				base0 = 0.; base1 = P.texH;
				P.rsu = (double)P.texH / len;
				P.rsv = (double)P.texW / height;
				P.rv0 = 0.;                                        // 5057 with ext == 2*W, shift 0
			}
			if (flip) { P.ru0 = -base0; P.ru1 = -base1; P.rsu = -P.rsu; }   // renderer.c:4735
			else      { P.ru0 = base0;  P.ru1 = base1; }

			// --- REMA: setPart, rothmap.cpp:1350-1448 -------------------------
			double sx = scaleX, sy = baseScale;
			const double dispW = P.texH, dispH = P.texW;      // roth_texture.cpp:447-448
			if (fit && !MODEL_FIXED)
			{
				sx = dispW / len;
				sy = dispH / height;
			}
			else if (fit)
			{
				// the patch: aim the engine's ceil() at TexelLength / the piece height
				sx = dispW / ((double)texelLength + (flip ? 0.5 : -0.5));
				sy = dispH / (floor(height + 0.5) - 0.5);
			}
			if (flip) sx = -sx;
			double pieceOffY = offY;
			if (pinB) pieceOffY += MODEL_FIXED ? dispH - (fit ? dispH : height * baseScale)
			                                   : dispH - height * sy;
			if (tier == T_MID && banded) pieceOffY += (bandTop - openTop) * sy;

			const EngineU eu = EngineAlong(sx, offX, (int)dispW, texelLength);
			P.mu0 = eu.u0; P.mu1 = eu.u1;
			P.msu = (eu.u1 - eu.u0) / len;
			const EngineV ev = EngineDown(sy, pieceOffY, (int)dispH);
			// the banded mid is anchored at openTop by the engine; the row at the
			// band top is what ROTH's rv0 describes.
			P.mv0 = ev.v0 + ((tier == T_MID && banded) ? (openTop - bandTop) * ev.sv : 0.);
			P.msv = ev.sv;
			if (tier == T_MID && twoSided && !fit && height * P.rsv > P.texW + 0.5)
				P.midNoTile = true;

			// --- per-piece tallies -------------------------------------------
			R.pieces++;
			if (fabs(WrapDiff(P.mu0 - P.ru0, P.texH)) > 0.25) R.startDiffer++;
			if (fabs(WrapDiff(P.mu1 - P.ru1, P.texH)) > 0.25) R.endDiffer++;
			if (P.rWrapsNonPow2)
			{
				R.nonPow2Wrap++;
				if (detail)
					printf("  non-pow2 wrap: face %d sector %d %s tex %d (%d texels across) (%d,%d)->(%d,%d) hfit %d shiftX %d"
						" -- the original masks with %d, the engine wraps modulo %d\n",
						(int)fi, f.sector, TierName(tier), texIdx, P.texH, a.x, a.y, b.x, b.y, hfit,
						tm.extended ? tm.shiftX : 0, P.texH - 1, P.texH);
			}
			if (P.midNoTile) R.midNoTile++;
			if (fabs(WrapDiff(P.mv0 - P.rv0, P.texW)) > 0.25) R.vAnchorDiffer++;
			if (P.rsv > 0 && fabs(P.msv / P.rsv - 1.) > 0.001) R.vScaleDiffer++;

			double ratio;
			if (P.rsu == 0.) ratio = (P.msu == 0.) ? 1. : 1e9;
			else ratio = P.msu / P.rsu;
			const double dev = fabs(ratio - 1.);
			if (dev > 0.001)
			{
				R.scaleDiffer++;
				if (dev > 0.01) R.scaleDiffer1++;
				if (dev > 0.10) R.scaleDiffer10++;
				if (!fit && hfit == 0) R.scaleByZeroExtent++;
				else if (restored12) R.scaleBy12bit++;
				else if (dev < 0.02) R.scaleByRounding++;
				else R.scaleOther++;
				if (ratio < 1e8 && fabs(log(fabs(ratio))) > fabs(log(R.worstScale))) R.worstScale = fabs(ratio);
				if (detail)
					scaleOff.push_back({ dev, (int)fi, tier, texIdx, f.sector, a.x, a.y, b.x, b.y,
						P.rsu, P.msu, hfit, len, tf });
			}
		}
	}

	// --- joins ---------------------------------------------------------------
	// faces by start vertex; sectors meeting at a vertex through a two-sided face
	std::multimap<int, int> startsAt;
	std::map<int, std::set<std::pair<int, int>>> meetAt;
	for (size_t fi = 0; fi < m.faces.size(); fi++)
	{
		const roth::Face &f = m.faces[fi];
		if (f.sector < 0 || f.vertex1 < 0 || f.vertex2 < 0) continue;
		startsAt.insert({ f.vertex1, (int)fi });
		if (f.sister >= 0 && f.sister < (int)m.faces.size() && m.faces[f.sister].sector >= 0)
		{
			const int s2 = m.faces[f.sister].sector;
			meetAt[f.vertex1].insert({ f.sector, s2 });
			meetAt[f.vertex2].insert({ f.sector, s2 });
		}
	}
	std::vector<Offender> off;
	for (size_t ai = 0; ai < m.faces.size(); ai++)
	{
		const roth::Face &A = m.faces[ai];
		if (A.sector < 0 || A.vertex2 < 0) continue;
		auto range = startsAt.equal_range(A.vertex2);
		for (auto it = range.first; it != range.second; ++it)
		{
			const int bi = it->second;
			const roth::Face &B = m.faces[bi];
			if (bi == (int)ai) continue;
			if (B.vertex2 == A.vertex1) continue;              // the other side of the same wall
			if (B.sector != A.sector)
			{
				auto mt = meetAt.find(A.vertex2);
				if (mt == meetAt.end()) continue;
				if (!mt->second.count({ A.sector, B.sector }) && !mt->second.count({ B.sector, A.sector }))
					continue;
			}
			for (int tier = 0; tier < 3; tier++)
			{
				const Piece &PA = info[ai].p[tier], &PB = info[bi].p[tier];
				if (!PA.present || !PB.present || PA.tex != PB.tex) continue;
				const double n = PA.texH;
				const double rJ = WrapDiff(PB.ru0 - PA.ru1, n);
				const double mJ = WrapDiff(PB.mu0 - PA.mu1, n);
				R.joins++;
				const bool contOrig = fabs(rJ) < 0.01;
				{
					const roth::Vertex &p1 = m.vertices[A.vertex1], &p2 = m.vertices[A.vertex2],
						&p3 = m.vertices[B.vertex2];
					const double ax = p2.x - p1.x, ay = p2.y - p1.y, bx = p3.x - p2.x, by = p3.y - p2.y;
					const double c = (ax * bx + ay * by) / (hypot(ax, ay) * hypot(bx, by));
					if (c > 0.9848)
					{
						R.colJoins++;
						if (contOrig) { R.colContOrig++; if (fabs(mJ) > 0.25) R.colSeams25++; }
						// CONTROL: the wrong-origin hypothesis (U measured from v2).
						// A's coordinate at the shared vertex would be its start value
						// and B's would be its end value.
						if (fabs(WrapDiff(PB.ru1 - PA.ru0, n)) < 0.01) R.colAltV2++;
					}
				}
				if (contOrig)
				{
					R.joinsContOrig++;
					if (fabs(mJ) > 0.05) R.seams05++;
					if (fabs(mJ) > 0.25) R.seams25++;
					if (fabs(mJ) > 1.00) R.seams100++;
				}
				else
				{
					R.joinsBrokenOrig++;
					if (fabs(WrapDiff(mJ - rJ, n)) <= 0.25) R.origSeamKept++;
					else if (fabs(mJ) <= 0.25) R.origSeamHealed++;
					else R.origSeamChanged++;
				}
				if (detail && DUMP_JOINS && !contOrig)
				{
					const roth::TextureMap &ta = m.textureMaps[A.textureMap], &tb = m.textureMaps[B.textureMap];
					const roth::Vertex &q1 = m.vertices[A.vertex1], &q2 = m.vertices[A.vertex2], &q3 = m.vertices[B.vertex2];
					printf("  origbroken %5d->%-5d sec %d/%d %-5s tex %d texH %d | A (%d,%d)->(%d,%d) hfit %d sh %d fl 0x%02x"
						" | B ->(%d,%d) hfit %d sh %d fl 0x%02x | ROTH jump %.2f REMA %.2f\n",
						(int)ai, bi, A.sector, B.sector, TierName(tier), PA.tex, PA.texH, q1.x, q1.y, q2.x, q2.y,
						ta.StoredExtent(), ta.shiftX, ta.flags, q3.x, q3.y, tb.StoredExtent(), tb.shiftX, tb.flags, rJ, mJ);
				}
				if (detail)
				{
					const roth::Vertex &a1 = m.vertices[A.vertex1], &a2 = m.vertices[A.vertex2],
						&b2 = m.vertices[B.vertex2];
					off.push_back({ fabs(WrapDiff(mJ - rJ, n)), (int)ai, bi, tier, PA.tex, A.sector,
						a1.x, a1.y, a2.x, a2.y, b2.x, b2.y, rJ, mJ, PA.rsu, PA.msu, PB.rsu, PB.msu });
				}
			}
		}
	}

	if (detail)
	{
		printf("\n%s: worst 20 joins by |REMA jump - ROTH jump| (texels, modulo the texture)\n", name.c_str());
		printf("  faceA->faceB sector tier  tex   A.v1 -> shared -> B.v2            ROTH jump  REMA jump"
			"   scale A (ROTH/REMA)  scale B (ROTH/REMA)\n");
		std::sort(off.begin(), off.end(), [](const Offender &x, const Offender &y) { return x.key > y.key; });
		for (size_t i = 0; i < off.size() && i < 20; i++)
		{
			const Offender &o = off[i];
			printf("  %5d->%-5d %5d %-5s %5d (%d,%d)->(%d,%d)->(%d,%d)  %8.3f  %8.3f   %.5f/%.5f  %.5f/%.5f\n",
				o.faceA, o.faceB, o.sector, TierName(o.tier), o.tex, o.ax1, o.ay1, o.ax2, o.ay2, o.bx2, o.by2,
				o.rJump, o.mJump, o.rsA, o.msA, o.rsB, o.msB);
		}
		printf("\n%s: worst 20 pieces by horizontal scale (texels per world unit) REMA / ROTH\n", name.c_str());
		printf("  face  sector tier  tex   v1 -> v2                  len      hfit flags  ROTH      REMA      factor\n");
		std::sort(scaleOff.begin(), scaleOff.end(), [](const ScaleOff &x, const ScaleOff &y) { return x.key > y.key; });
		for (size_t i = 0; i < scaleOff.size() && i < 20; i++)
		{
			const ScaleOff &o = scaleOff[i];
			printf("  %5d %5d %-5s %5d (%d,%d)->(%d,%d)  %8.2f %5d 0x%02x  %.5f  %.5f  %.4f\n",
				o.face, o.sector, TierName(o.tier), o.tex, o.x1, o.y1, o.x2, o.y2, o.len, o.hfit, o.flags,
				o.rs, o.ms, o.rs != 0 ? o.ms / o.rs : 0.);
		}
	}
	return true;
}

static void PrintResult(const MapResult &r)
{
	printf("%-9s pieces %6ld | joins %6ld: orig-continuous %6ld -> REMA jump >0.05tx %5ld, >0.25tx %5ld, >1tx %4ld"
		" | orig-broken %5ld: kept %5ld changed %4ld healed %3ld"
		" | U scale differs >0.1%% %5ld (>1%% %4ld, >10%% %3ld; worst x%.3f)"
		" | u@v1 differs %4ld u@v2 differs %5ld | V anchor %4ld V scale %4ld\n",
		r.name.c_str(), r.pieces, r.joins, r.joinsContOrig, r.seams05, r.seams25, r.seams100,
		r.joinsBrokenOrig, r.origSeamKept, r.origSeamChanged, r.origSeamHealed,
		r.scaleDiffer, r.scaleDiffer1, r.scaleDiffer10, r.worstScale,
		r.startDiffer, r.endDiffer, r.vAnchorDiffer, r.vScaleDiffer);
}

int main(int argc, char **argv)
{
	std::vector<std::string> args;
	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "-fixed")) MODEL_FIXED = true;
		else if (!strcmp(argv[i], "-joins")) DUMP_JOINS = true;
		else args.push_back(argv[i]);
	}
	if (args.empty()) { printf("usage: wallseams <folder with ROTH.RES and M> [MAP] [-fixed]\n"); return 2; }
	if (!roth::TheInstall().Open(args[0].c_str())) { printf("install: %s\n", roth::TheInstall().Error().c_str()); return 2; }

	printf("model: %s\n", MODEL_FIXED ? "PATCHED loader" : "CURRENT loader (HEAD)");
	MapResult tot; tot.name = "ALL";
	int maps = 0;
	for (const std::string &name : roth::TheInstall().MapNames())
	{
		if (args.size() > 1 && name != args[1]) continue;
		MapResult r;
		if (!RunMap(name, args.size() > 1, r)) { printf("%s: could not load\n", name.c_str()); continue; }
		maps++;
		PrintResult(r);
		if (args.size() > 1)
		{
			printf("  sectors wound counter-clockwise (front would be on the left)  %ld\n", r.windingBad);
			printf("  two-sided faces whose sister is not the reversed edge         %ld\n", r.sisterNotReversed);
			printf("  two-sided faces whose sister link is not mutual               %ld\n", r.sisterNotMutual);
			printf("  scale differences by cause: 12-bit extent restored %ld, zero extent %ld,"
				" engine rounding (<2%%) %ld, other %ld\n",
				r.scaleBy12bit, r.scaleByZeroExtent, r.scaleByRounding, r.scaleOther);
			printf("  straight-on joins (within 10 deg): %ld, continuous in ROTH %ld, of those REMA >0.25tx %ld;"
				" continuous under the U-from-v2 control %ld\n", r.colJoins, r.colContOrig, r.colSeams25, r.colAltV2);
			printf("  FLIP_X faces that also carry a shiftX                         %ld\n", r.flipShiftFaces);
			printf("  pieces the ORIGINAL wraps with a non-power-of-two mask        %ld\n", r.nonPow2Wrap);
			printf("  see-through mids taller than one copy (engine draws one)      %ld\n", r.midNoTile);
		}
#define ADD(x) tot.x += r.x
		ADD(faces); ADD(pieces); ADD(windingBad); ADD(sisterNotReversed); ADD(sisterNotMutual);
		ADD(startDiffer); ADD(endDiffer); ADD(scaleDiffer); ADD(scaleDiffer1); ADD(scaleDiffer10);
		ADD(scaleBy12bit); ADD(scaleByRounding); ADD(scaleByZeroExtent); ADD(scaleOther);
		ADD(vAnchorDiffer); ADD(vScaleDiffer); ADD(midNoTile); ADD(nonPow2Wrap);
		ADD(joins); ADD(joinsContOrig); ADD(joinsBrokenOrig); ADD(seams05); ADD(seams25); ADD(seams100);
		ADD(origSeamKept); ADD(origSeamChanged); ADD(origSeamHealed); ADD(flipShiftFaces);
		ADD(colJoins); ADD(colContOrig); ADD(colSeams25); ADD(colAltV2);
#undef ADD
		if (fabs(log(r.worstScale)) > fabs(log(tot.worstScale))) tot.worstScale = r.worstScale;
	}
	if (args.size() == 1)
	{
		printf("\n%d maps\n", maps);
		PrintResult(tot);
		printf("  sectors wound counter-clockwise  %ld\n", tot.windingBad);
		printf("  sister not the reversed edge     %ld   sister link not mutual %ld\n",
			tot.sisterNotReversed, tot.sisterNotMutual);
		printf("  scale differences by cause: 12-bit extent restored %ld, zero extent %ld,"
			" engine rounding (<2%%) %ld, other %ld\n",
			tot.scaleBy12bit, tot.scaleByZeroExtent, tot.scaleByRounding, tot.scaleOther);
		printf("  straight-on joins (within 10 deg): %ld, continuous in ROTH %ld, of those REMA >0.25tx %ld;"
			" continuous under the U-from-v2 control %ld\n", tot.colJoins, tot.colContOrig, tot.colSeams25, tot.colAltV2);
		printf("  FLIP_X faces that also carry a shiftX  %ld\n", tot.flipShiftFaces);
		printf("  pieces the ORIGINAL wraps with a non-power-of-two mask  %ld\n", tot.nonPow2Wrap);
		printf("  see-through mids taller than one copy (engine draws one)  %ld\n", tot.midNoTile);
	}
	return 0;
}
