// doorcheck.cpp -- every door in the retail maps, resolved the way ROTH.C
// resolves it, against what the REMAROTH loader and runtime did before the
// 2026-11-12 door fix. Offline: reads the player's own .RAW and .DAS files
// through the src/roth readers and writes nothing.
//
// WHAT A DOOR IS IN THE ORIGINAL (all line numbers are ROTH.C,
// roth_c/src/engine):
//
//   CLOSED  there is no door object at all. The door sector (commandID
//           0xFFFF / 0xFFFE / 0xFFFD) is an ordinary thin sector and what the
//           player sees is the ROOM face's transparent mid piece, drawn by the
//           wall path between max(floors) and min(ceilings) of the room and
//           the door sector (compute_face_span_extents, renderer.c:8171-8207).
//           The room face's UPPER piece is the lintel above it.
//   OPEN    spawn_door_instance (doors.c:685) builds a record: a box mesh of
//           eight vertices and four quads (setup_door_swing_geometry,
//           doors.c:474-577; template obj3_owned.c:866-872), drawn by the
//           mesh path from the door sector's FLOOR upward (rwss_type04,
//           renderer.c:6422-6453: cx = sector floor - view z) and rotated
//           about the hinge by the angle byte. While a record exists the
//           static mid piece on both the door's own faces and the room faces
//           is skipped (finalize_world_span_overlay, renderer.c:8717-8726).
//
// PER DOOR this resolves, citing the rule:
//
//   hinge     the first face, probed in slot order 0,3,2,1, whose mapping is
//             extended with faceID >= 0xFFFD            doors.c:491-503
//   corners   c_k = slot (hinge + k) & 3                doors.c:509-513
//   marker    the hinge mapping's faceID                doors.c:506
//   mirror    marker FFFE never; FFFD always; FFFF when the sector across c3
//             is the room the door was opened from      doors.c:523-531
//   pivot     vertex1 of c0, or vertex2 when mirrored   doors.c:516, 528, 546
//   sense     the angle byte runs 0 -> +0x40, or 0 -> -0x40 when mirrored
//             (doors.c:1083-1124); rotate_quad turns the leaf by MINUS the
//             angle (renderer.c:1557-1578), so unmirrored is CLOCKWISE in map
//             coordinates and mirrored is counter-clockwise. 0x40 = 90 degrees.
//   height    door ceiling - floor; or -4 * TEXTURE_MAP_OVERRIDE when c1's own
//             mapping has TRANSPARENT and TRANS_UPLO    doors.c:534-538
//   base      the door sector's floor                   renderer.c:6436-6438
//   surfaces  quad on c2: picture and flags from c3's OWN mapping, extent c2's
//             quad on c0: picture and flags from c1's OWN mapping, extent c0's
//             quad on c3: picture, flags, extent from c3's SISTER
//             quad on c1: picture, flags, extent from c1's SISTER
//             (doors.c:553-559 with 281-297, and the template's vertex slots)
//   flags     IMAGE_FIT clear -> stored extents (hfit across, height down);
//             set -> one copy of the picture across the quad both ways;
//             TRANS_UPLO without TRANSPARENT -> untextured  doors.c:263-276,
//             renderer.c:13334-13361
//
// WHAT THE LOADER/RUNTIME DID BEFORE, and what this flags:
//
//   TEXLEN   every leaf side: the leaf's lines never went through
//            FinishLoadingLineDef, so side_t::TexelLength stayed 0 and the
//            wall drew ONE texel column smeared across its whole width
//            (hw_walls.cpp:1378-1381). That is "doors have no texture".
//   HEIGHT   a one-sided polyobject wall is drawn floor to ceiling of whatever
//            sector it is standing IN (hw_bsp.cpp AddLine passes
//            currentsector), so the leaf was as tall as the room it swung
//            into. Flagged where the leaf's own height differs from either
//            adjoining room's.
//   OVR      the old void-sector span used the override unscaled and without
//            the c1 flag test (it had no effect on screen; listed for the
//            record).
//   FIT      a surface whose source mapping is IMAGE_FIT was scaled as tiled.
//   SWING    the swing was always +90 degrees about vertex1 of c0.
//   HINGE    the loader took the lowest hinge slot; ROTH.C probes 0,3,2,1.
//
// Build:  sh tools/doorcheck/build.sh
// Run:    /tmp/doorcheck "<ROTH DATA folder>"            summary, all maps
//         /tmp/doorcheck "<ROTH DATA folder>" MAP        every door of one map
//         /tmp/doorcheck "<ROTH DATA folder>" -shots MAP [dist]
//                                                        rothdiff_shot lines

#include "roth_raw.h"
#include "roth_install.h"
#include "roth_das.h"

#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <map>
#include <cmath>
#include <algorithm>

using namespace roth;

static std::vector<uint8_t> ReadFile(const std::string &p)
{
	std::vector<uint8_t> o; FILE *f = fopen(p.c_str(), "rb"); if (!f) return o;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); o.resize((size_t)n);
	if (n > 0 && fread(o.data(), 1, (size_t)n, f) != (size_t)n) o.clear();
	fclose(f); return o;
}

struct Surface
{
	int quadFace = -1;      // the face whose edge the quad stands on
	int texFace = -1;       // whose mapping gives the picture and the flags
	int extFace = -1;       // whose mapping gives the stored extent
	bool viaSister = false;
	bool missing = false;   // ROTH.C would read through a 0xFFFF sister link
	uint16_t texture = 0;
	uint8_t flags = 0;
	int extent = 0;
	uint16_t draw = 0;      // the surface's draw-flag word, doors.c:263-276
	double len = 0.;
	int texW = 0, texH = 0; // 0 when not a picture in the map's pack
	long holes = 0;         // texels of palette index 0: see-through under TRANSPARENT
	const char *kind = "";
};

struct Door
{
	int sector = -1;
	bool ok = false; const char *why = "";
	int hingeSlot = -1, loaderHingeSlot = -1, hingeCount = 0;
	uint16_t marker = 0;
	int c[4] = { -1, -1, -1, -1 };
	int secA = -1, secB = -1;         // across c1, across c3
	int floorZ = 0, ceilZ = 0, height = 0;
	bool heightFromOverride = false;
	int oldLo = 0, oldHi = 0;         // the old void-sector span
	Surface s[4];                     // indexed by corner: quad on c0..c3
	// pivot[0], sense[0]: opened from the room across c1 (or a fixed marker);
	// [1]: opened from the room across c3.
	int pivotX[2], pivotY[2]; int sense[2]; int into[2]; bool mirror[2];
};

static const TextureMap *TmOf(const Map &m, int face)
{
	if (face < 0 || face >= (int)m.faces.size()) return nullptr;
	const int t = m.faces[face].textureMap;
	return (t >= 0 && t < (int)m.textureMaps.size()) ? &m.textureMaps[t] : nullptr;
}

static const char *KindName(const Pack *pk, int index, int *w, int *h, long *holes)
{
	*w = *h = 0; *holes = 0;
	if (index >= 0xFF00) return "colour";
	if (index >= 0x8000) return "colour";
	if (pk == nullptr) return "?";
	const FatEntry *e = pk->Entry(index);
	if (e == nullptr) return "OUT-OF-PACK";
	switch (e->kind)
	{
	case EntryKind::Empty: return "EMPTY";
	case EntryKind::Object3D: return "MESH";
	case EntryKind::Indirection: return "indirect";
	default: break;
	}
	Image im = pk->ReadImage(index, false);
	if (!im.ok()) return "UNREADABLE";
	*w = im.height; *h = im.width;   // wall art is stored a quarter turn round (roth_texture.cpp:13)
	if (im.ZeroIsTransparent()) for (uint8_t px : im.frames[0]) if (px == 0) (*holes)++;
	return e->kind == EntryKind::Animated ? "animated" : "picture";
}

// doors.c:263-276 -- the surface's draw word from the mapping's flag byte.
static uint16_t DrawWord(uint8_t ch)
{
	uint16_t dx = ch;
	if (!(ch & 4)) dx |= 0x100;
	dx &= 0x103;
	if (!(ch & 0x10)) dx |= 0x40;
	dx |= 0x18;
	if ((ch & 9) && !(ch & 1)) dx &= 0xfff7;
	return dx;
}

static Door Resolve(const Map &m, int si, const Pack *pk)
{
	Door d; d.sector = si;
	const Sector &rs = m.sectors[si];
	if (rs.faceCount != 4) { d.why = "not four faces (doors.c:488)"; return d; }
	const int f0 = rs.firstFaceIndex;
	if (f0 < 0 || f0 + 3 >= (int)m.faces.size()) { d.why = "faces out of range"; return d; }
	for (int j = 0; j < 4; j++)
		if (m.faces[f0 + j].vertex2 != m.faces[f0 + ((j + 1) & 3)].vertex1
			|| m.faces[f0 + j].vertex1 < 0) { d.why = "face ring does not close"; return d; }

	auto isHinge = [&](int slot)
	{
		const TextureMap *tm = TmOf(m, f0 + slot);
		return tm && tm->extended && tm->faceID >= TRIGGER_DOOR_A;
	};
	static const int probe[4] = { 0, 3, 2, 1 };          // doors.c:491-503 (rol)
	for (int k = 0; k < 4; k++) if (isHinge(probe[k])) { d.hingeSlot = probe[k]; break; }
	for (int k = 0; k < 4; k++) if (isHinge(k)) { if (d.loaderHingeSlot < 0) d.loaderHingeSlot = k; d.hingeCount++; }
	if (d.hingeSlot < 0) { d.why = "no hinge face (doors.c:504)"; return d; }

	for (int k = 0; k < 4; k++) d.c[k] = f0 + ((d.hingeSlot + k) & 3);
	d.marker = TmOf(m, d.c[0])->faceID;
	const Face &c0 = m.faces[d.c[0]], &c1 = m.faces[d.c[1]], &c3 = m.faces[d.c[3]];
	d.secA = c1.sister >= 0 ? m.faces[c1.sister].sector : -1;
	d.secB = c3.sister >= 0 ? m.faces[c3.sister].sector : -1;

	d.floorZ = rs.floorHeight; d.ceilZ = rs.ceilingHeight;
	d.height = (int16_t)(uint16_t)(rs.ceilingHeight - rs.floorHeight);
	if (const TextureMap *t1 = TmOf(m, d.c[1]))
		if ((t1->flags & 1) && (t1->flags & 8))            // doors.c:534-538
		{
			d.height = (int16_t)(uint16_t)(-(int(rs.textureMapOverride) << 2));
			d.heightFromOverride = true;
		}
	// What rothmap.cpp gave the void sector before the fix.
	d.oldLo = d.floorZ; d.oldHi = d.ceilZ;
	if (rs.textureMapOverride < 0) d.oldHi = std::min(d.ceilZ, d.floorZ - rs.textureMapOverride);
	else if (rs.textureMapOverride > 0) d.oldLo = std::max(d.floorZ, d.ceilZ - rs.textureMapOverride);

	// The four surfaces, by the quad's corner.
	for (int k = 0; k < 4; k++)
	{
		Surface &s = d.s[k];
		s.quadFace = d.c[k];
		const Face &qf = m.faces[d.c[k]];
		s.len = std::hypot(double(m.vertices[qf.vertex2].x) - m.vertices[qf.vertex1].x,
			double(m.vertices[qf.vertex2].y) - m.vertices[qf.vertex1].y);
		if (k == 1 || k == 3)
		{
			s.viaSister = true;
			if (qf.sister < 0) { s.missing = true; continue; }
			s.texFace = s.extFace = qf.sister;
		}
		else
		{
			s.texFace = d.c[k + 1];
			s.extFace = d.c[k];
		}
		const TextureMap *tt = TmOf(m, s.texFace), *et = TmOf(m, s.extFace);
		if (!tt || !et) { s.missing = true; continue; }
		s.texture = tt->midTexture;
		s.flags = tt->flags;
		s.extent = et->fitWord & 0xfff;
		s.draw = DrawWord(tt->flags);
		s.kind = KindName(pk, s.texture, &s.texW, &s.texH, &s.holes);
	}

	// The swing, for each side it can be opened from.
	for (int side = 0; side < 2; side++)
	{
		bool mir;
		if (d.marker == 0xfffe) mir = false;
		else if (d.marker == 0xfffd) mir = true;
		else mir = (side == 1);                            // opened from across c3
		d.mirror[side] = mir;
		const Vertex &pv = m.vertices[mir ? c0.vertex2 : c0.vertex1];
		d.pivotX[side] = pv.x; d.pivotY[side] = pv.y;
		d.sense[side] = mir ? +1 : -1;                     // +1 = counter-clockwise
		d.into[side] = mir ? d.secA : d.secB;              // record[0x10], doors.c:541
	}
	d.ok = true;
	return d;
}

static std::string FlagStr(uint8_t f)
{
	std::string s;
	if (f & 1) s += "TRANSP ";
	if (f & 2) s += "FLIPX ";
	if (f & 4) s += "FIT ";
	if (f & 8) s += "UPLO ";
	if (f & 0x20) s += "HALFPX ";
	if (f & 0x80) s += "PINBOT ";
	if (s.empty()) s = "- ";
	s.pop_back();
	return s;
}

struct Totals
{
	long doors = 0, unbuildable = 0, id[3] = { 0, 0, 0 };
	long marker[3] = { 0, 0, 0 };
	long hingeDiffers = 0, multiHinge = 0;
	long heightOvr = 0, heightBad = 0, oldSpanWrong = 0;
	long tallerRoom = 0;             // an adjoining room is taller than the leaf
	long sides = 0, sidesUntextured = 0, sidesFit = 0, sidesMissing = 0;
	long sidesNotPicture = 0, sidesExtentNotLen = 0, sidesKey = 0, sidesHoles = 0;
	long swingWrong = 0, pivotWrong = 0, bothSenses = 0;
	long frontBackDiffer = 0;
	long closedDiffers = 0;          // closed wall law != leaf law on a broad face
	long cdShift = 0, cdHalf = 0, cdPin = 0, cdBand = 0, cdSpan = 0, cdNoMid = 0;
	long cmds = 0, cmdDwell0 = 0, cmdKey0 = 0, cmdUnresolved = 0;
	std::map<int, long> speedHist, dwellHist, heightHist;
};

static void PrintDoor(const Map &m, const Door &d, const char *mapName, int tag)
{
	const Sector &rs = m.sectors[d.sector];
	printf("%s sector %d  id %04x  tag %d  floor %d ceil %d  override %d  light %d\n",
		mapName, d.sector, rs.commandID, tag, d.floorZ, d.ceilZ, rs.textureMapOverride, rs.light);
	if (!d.ok) { printf("    NOT A DOOR ROTH.C CAN BUILD: %s\n", d.why); return; }
	printf("    hinge slot %d (marker %04x)%s   leaf z %d .. %d (height %d%s)\n",
		d.hingeSlot, d.marker,
		d.hingeSlot != d.loaderHingeSlot ? "  [HINGE: loader took another slot]" : "",
		d.floorZ, d.floorZ + d.height, d.height,
		d.heightFromOverride ? ", -4*override" : ", ceiling-floor");
	for (int side = 0; side < 2; side++)
	{
		if (side == 1 && d.marker != 0xffff) break;
		printf("    swing%s: pivot (%d,%d) %s 90 deg into sector %d\n",
			d.marker == 0xffff ? (side ? " (opened from across c3)" : " (opened from across c1)") : "",
			d.pivotX[side], d.pivotY[side],
			d.sense[side] > 0 ? "counter-clockwise" : "clockwise", d.into[side]);
	}
	for (int a = 0; a < 2; a++)
	{
		const int sec = a ? d.secB : d.secA;
		if (sec < 0) continue;
		const Sector &ns = m.sectors[sec];
		printf("    room across c%d: sector %d  floor %d ceil %d  override %d\n",
			a ? 3 : 1, sec, ns.floorHeight, ns.ceilingHeight, ns.textureMapOverride);
	}
	static const char *what[4] = { "c0 hinge edge", "c1 broad", "c2 latch edge", "c3 broad" };
	for (int k = 0; k < 4; k++)
	{
		const Surface &s = d.s[k];
		const Face &qf = m.faces[s.quadFace];
		if (s.missing) { printf("    %-13s  NO SOURCE (no sister face)\n", what[k]); continue; }
		printf("    %-13s (%d,%d)-(%d,%d) len %.1f  tex %u %s %dx%d  [%s]  extent %d  draw %03x%s%s%s\n",
			what[k], m.vertices[qf.vertex1].x, m.vertices[qf.vertex1].y,
			m.vertices[qf.vertex2].x, m.vertices[qf.vertex2].y, s.len,
			s.texture, s.kind, s.texW, s.texH, FlagStr(s.flags).c_str(), s.extent, s.draw,
			(s.draw & 8) ? "" : "  UNTEXTURED",
			((s.flags & 1) && s.holes > 0) ? "  HAS HOLES" : "",
			(s.flags & 4) ? "  one copy across the quad" : "");
	}
}

static double DoomDegToRoth(double deg) { return (deg - 90.) * 512. / 360.; }

// Even-odd test against one sector's own faces.
static bool InSector(const Map &m, int sec, double px, double py)
{
	const Sector &rs = m.sectors[sec];
	bool in = false;
	for (int j = 0; j < rs.faceCount; j++)
	{
		const Face &f = m.faces[rs.firstFaceIndex + j];
		const double x1 = m.vertices[f.vertex1].x, y1 = m.vertices[f.vertex1].y;
		const double x2 = m.vertices[f.vertex2].x, y2 = m.vertices[f.vertex2].y;
		if ((y1 > py) != (y2 > py) && px < x1 + (x2 - x1) * (py - y1) / (y2 - y1)) in = !in;
	}
	return in;
}

static int SectorAt(const Map &m, double px, double py)
{
	for (size_t i = 0; i < m.sectors.size(); i++)
		if (m.sectors[i].firstFaceIndex >= 0 && InSector(m, (int)i, px, py)) return (int)i;
	return -1;
}

static int EmitShots(const char *mapName, int dist)
{
	std::vector<uint8_t> raw = ReadFile(TheInstall().MapFile(mapName));
	Map m = ParseRaw(raw.data(), raw.size());
	if (!m.ok()) { printf("// %s: %s\n", mapName, m.error.c_str()); return 2; }
	int tag = 0;
	for (size_t si = 0; si < m.sectors.size(); si++)
	{
		if (!m.sectors[si].IsDoorCapable()) continue;
		Door d = Resolve(m, (int)si, nullptr);
		if (!d.ok) continue;
		tag++;
		for (int k = 1; k <= 3; k += 2)
		{
			const Face &f = m.faces[d.c[k]];
			if (f.sister < 0) continue;
			const double x1 = m.vertices[f.vertex1].x, y1 = m.vertices[f.vertex1].y;
			const double x2 = m.vertices[f.vertex2].x, y2 = m.vertices[f.vertex2].y;
			const double dx = x2 - x1, dy = y2 - y1, len = std::hypot(dx, dy);
			// The face's own sector (the door) is on its right, so the room is on
			// the left: (-dy, dx). Stand there and look back.
			// Back off until the camera stands in a real sector at the door's
			// own floor height (the shot command stands it on that floor).
			double cx = 0., cy = 0.; int at = -1, used = 0;
			for (int dd = dist; dd >= 40; dd -= 10)
			{
				cx = (x1 + x2) * .5 - dy / len * dd; cy = (y1 + y2) * .5 + dx / len * dd;
				at = SectorAt(m, cx, cy);
				used = dd;
				if (at >= 0 && m.sectors[at].floorHeight == m.sectors[si].floorHeight
					&& !m.sectors[at].IsDoorCapable()) break;
				at = -1;
			}
			if (at < 0) { printf("// %s tag %d c%d: no standing room in front of it\n", mapName, tag, k); continue; }
			const double deg = std::atan2(-dx, dy) * 180. / 3.14159265358979323846;
			int a = (int)std::lround(DoomDegToRoth(deg)); a = ((a % 512) + 512) % 512;
			printf("rothdiff_shot %d %d %d <OUT>/%s_tag%02d_c%d.png   // door sector %zu seen from sector %d, %d units back (standing in sector %d)\n",
				(int)std::lround(cx), (int)std::lround(cy), a, mapName, tag, k, si,
				m.faces[f.sister].sector, used, at);
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) { printf("usage: doorcheck <ROTH DATA folder> [MAP | -shots MAP [dist]]\n"); return 2; }
	if (!TheInstall().Open(argv[1])) { printf("install: %s\n", TheInstall().Error().c_str()); return 2; }
	if (argc > 3 && std::string(argv[2]) == "-shots")
		return EmitShots(argv[3], argc > 4 ? atoi(argv[4]) : 200);
	const std::string only = argc > 2 ? argv[2] : "";

	Totals T;
	int maps = 0;
	printf("%-10s %5s %5s %5s %6s %6s %6s %6s %6s %6s\n", "map", "doors", "nobld", "sides",
		"TEXLEN", "HEIGHT", "OVR", "FIT", "SWING", "HINGE");
	for (const std::string &name : TheInstall().MapNames())
	{
		std::vector<uint8_t> raw = ReadFile(TheInstall().MapFile(name.c_str()));
		if (raw.empty()) continue;
		Map m = ParseRaw(raw.data(), raw.size());
		if (!m.ok()) { printf("%-10s parse error: %s\n", name.c_str(), m.error.c_str()); continue; }
		std::vector<uint8_t> pd = ReadFile(TheInstall().PackFile(TheInstall().PackFor(name.c_str()).c_str()));
		Pack pack; const bool havePack = !pd.empty() && pack.Load(pd.data(), pd.size());
		maps++;

		long doors = 0, nobld = 0, sides = 0, fTex = 0, fHeight = 0, fOvr = 0, fFit = 0, fSwing = 0, fHinge = 0;
		std::map<int, Door> bySector;
		int tag = 0;
		for (size_t si = 0; si < m.sectors.size(); si++)
		{
			const Sector &rs = m.sectors[si];
			if (!rs.IsDoorCapable()) continue;
			Door d = Resolve(m, (int)si, havePack ? &pack : nullptr);
			T.id[rs.commandID - 0xfffd]++;
			if (!d.ok) { nobld++; T.unbuildable++; if (name == only) PrintDoor(m, d, name.c_str(), 0); continue; }
			tag++; doors++; T.doors++;
			bySector[(int)si] = d;
			T.marker[d.marker - 0xfffd]++;
			T.heightHist[d.height]++;
			if (d.hingeCount > 1) T.multiHinge++;
			if (d.hingeSlot != d.loaderHingeSlot) { fHinge++; T.hingeDiffers++; }
			if (d.heightFromOverride) T.heightOvr++;
			if (d.height <= 0) T.heightBad++;
			if (d.oldLo != d.floorZ || d.oldHi != d.floorZ + d.height) { fOvr++; T.oldSpanWrong++; }
			bool taller = false;
			for (int a = 0; a < 2; a++)
			{
				const int sec = a ? d.secB : d.secA;
				if (sec < 0) continue;
				const Sector &ns = m.sectors[sec];
				if (ns.ceilingHeight != d.floorZ + d.height || ns.floorHeight != d.floorZ) taller = true;
			}
			if (taller) { fHeight++; T.tallerRoom++; }
			for (int k = 0; k < 4; k++)
			{
				const Surface &s = d.s[k];
				if (s.missing) { T.sidesMissing++; continue; }
				sides++; T.sides++;
				fTex++;                                   // TexelLength was 0 on every one
				if (!(s.draw & 8)) T.sidesUntextured++;
				if (s.flags & 4) { fFit++; T.sidesFit++; }
				else if (std::fabs(s.extent - s.len) > 1.) T.sidesExtentNotLen++;
				if (s.texW == 0 && std::string(s.kind) != "colour") T.sidesNotPicture++;
				if (havePack && s.texture == pack.SkyMarkerIndex()) T.sidesKey++;
				if ((s.flags & 1) && s.holes > 0) T.sidesHoles++;
			}
			if (!d.s[1].missing && !d.s[3].missing && d.s[1].texture != d.s[3].texture) T.frontBackDiffer++;
			// The old runtime: always counter-clockwise about vertex1 of c0.
			const Face &c0 = m.faces[d.c[0]];
			bool swingBad = false, pivotBad = false;
			for (int side = 0; side < (d.marker == 0xffff ? 2 : 1); side++)
			{
				if (d.sense[side] != +1) swingBad = true;
				if (d.pivotX[side] != m.vertices[c0.vertex1].x || d.pivotY[side] != m.vertices[c0.vertex1].y) pivotBad = true;
			}
			if (d.marker == 0xffff) T.bothSenses++;
			if (swingBad) { fSwing++; T.swingWrong++; }
			else if (pivotBad) { fSwing++; T.pivotWrong++; }
			// Closed: the room face is drawn by the WALL law, which also honours
			// shifts, HALF_PIXEL and PIN_BOTTOM and bands by the ROOM's override;
			// the leaf law has none of those. Where they are all absent and the
			// opening is exactly the leaf's span the two pictures are the same.
			for (int k = 1; k <= 3; k += 2)
			{
				const Surface &s = d.s[k];
				if (s.missing) continue;
				const TextureMap *tm = TmOf(m, s.texFace);
				const Sector &ns = m.sectors[m.faces[s.texFace].sector];
				const int top = std::min<int>(ns.ceilingHeight, rs.ceilingHeight);
				const int bot = std::max<int>(ns.floorHeight, rs.floorHeight);
				bool differs = false;
				if (!(tm->flags & 4) && tm->extended && (tm->shiftX || tm->shiftY)) { differs = true; T.cdShift++; }
				if (!(tm->flags & 4) && (tm->flags & 0x20)) { differs = true; T.cdHalf++; }
				if (tm->flags & 0x80) { differs = true; T.cdPin++; }
				// The closed piece's own span: the opening, cut to a band of
				// 4*|override| by the ROOM's override under TRANSPARENT+TRANS_UPLO
				// (compute_face_span_extents, renderer.c:8196-8207).
				int ctop = top, cbot = bot;
				if ((tm->flags & 1) && (tm->flags & 8) && ns.textureMapOverride != 0)
				{
					T.cdBand++;
					if (ns.textureMapOverride > 0) cbot = ctop - 4 * ns.textureMapOverride;
					else ctop = cbot - 4 * ns.textureMapOverride;
				}
				if (ctop != d.floorZ + d.height || cbot != d.floorZ) { differs = true; T.cdSpan++; }
				if (!(tm->flags & 1)) { differs = true; T.cdNoMid++; }   // closed draws no mid piece at all
				if (differs) { T.closedDiffers++; if (name == only) printf("    ^ sector %d c%d: closed wall picture differs from the leaf\n", d.sector, k); }
			}
			if (name == only) PrintDoor(m, d, name.c_str(), tag);
		}

		// The commands that open doors: cmd_open_door, opcode 0x2f
		// (raw_commands.c:4053-4087). M is byte +7, the dwell word +0x0a.
		for (const Command &c : m.commands)
		{
			if (c.isTrigger || c.opcode != 0x2f) continue;
			T.cmds++;
			T.speedHist[c.subFlags]++;
			T.dwellHist[c.aux]++;
			if (c.aux == 0) T.cmdDwell0++;
			if (c.key == 0) { T.cmdKey0++; continue; }
			bool hit = false;
			for (int fi : c.faces)
			{
				if (fi < 0 || fi >= (int)m.faces.size()) continue;
				const int sis = m.faces[fi].sister;
				// spawn_door_instance: the door is the sector BEHIND the face
				// (doors.c:697-698), the room is the face's own (doors.c:707).
				if (sis >= 0 && bySector.count(m.faces[sis].sector)) hit = true;
				if (name == only)
					printf("    0x2f key %04x face %d: door sector %d opened from sector %d  M %d dwell %d%s\n",
						c.key, fi, sis >= 0 ? m.faces[sis].sector : -1, m.faces[fi].sector,
						c.subFlags, c.aux, (sis >= 0 && bySector.count(m.faces[sis].sector)) ? "" : "  (not a buildable door)");
			}
			if (!hit) T.cmdUnresolved++;
		}

		printf("%-10s %5ld %5ld %5ld %6ld %6ld %6ld %6ld %6ld %6ld\n", name.c_str(), doors, nobld,
			sides, fTex, fHeight, fOvr, fFit, fSwing, fHinge);
	}

	printf("\n%d maps, %ld doors ROTH.C can build (%ld door-capable sectors it cannot)\n", maps, T.doors, T.unbuildable);
	printf("  sector id       FFFD %ld   FFFE %ld   FFFF %ld\n", T.id[0], T.id[1], T.id[2]);
	printf("  hinge marker    FFFD %ld (always mirrored)   FFFE %ld (never)   FFFF %ld (away from the opener)\n",
		T.marker[0], T.marker[1], T.marker[2]);
	printf("  more than one hinge-marked face %ld; loader's slot differs from ROTH.C's %ld\n", T.multiHinge, T.hingeDiffers);
	printf("  height from -4*override %ld; height <= 0 %ld\n", T.heightOvr, T.heightBad);
	printf("  leaf heights:");
	for (auto &kv : T.heightHist) printf(" %d x%ld", kv.first, kv.second);
	printf("\n  old void-sector span differs from ROTH.C's leaf span %ld\n", T.oldSpanWrong);
	printf("  a room beside the door is not exactly the leaf's span (leaf was drawn at ROOM height) %ld\n", T.tallerRoom);
	printf("  leaf sides %ld: untextured by flag %ld, IMAGE_FIT %ld, tiled with extent != length %ld,\n"
		"      not a picture in the pack %ld, on the pack's colour key %ld, no source face %ld,\n"
		"      TRANSPARENT with see-through texels in the picture %ld\n",
		T.sides, T.sidesUntextured, T.sidesFit, T.sidesExtentNotLen, T.sidesNotPicture, T.sidesKey, T.sidesMissing, T.sidesHoles);
	printf("  the two broad faces wear different pictures %ld\n", T.frontBackDiffer);
	printf("  broad faces whose CLOSED wall picture is not the leaf's %ld  (may overlap: shift %ld, half-pixel %ld,\n"
		"      pin-bottom %ld, (banded by the room's override %ld,) closed span != leaf span %ld, room face not TRANSPARENT %ld)\n",
		T.closedDiffers, T.cdShift, T.cdHalf, T.cdPin, T.cdBand, T.cdSpan, T.cdNoMid);
	printf("  old swing (+90 about c0.vertex1): wrong sense %ld, right sense wrong pivot %ld\n", T.swingWrong, T.pivotWrong);
	printf("  open-door commands (0x2f) %ld: key 0 %ld, dwell 0 (stays open) %ld, keyed but no buildable door %ld\n",
		T.cmds, T.cmdKey0, T.cmdDwell0, T.cmdUnresolved);
	printf("    speed M:"); for (auto &kv : T.speedHist) printf(" %d x%ld", kv.first, kv.second);
	printf("\n    dwell A:"); for (auto &kv : T.dwellHist) printf(" %d x%ld", kv.first, kv.second);
	printf("\n");
	return 0;
}
