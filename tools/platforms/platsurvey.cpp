// Survey of everything in the retail maps that MOVES SIDEWAYS or carries:
// RAW opcode 0x09 "move sector" records, moving mid-platforms, and the props
// that ride either. Engine-free: plain g++ and src/roth/roth_raw.
//
//   tools/platforms/build.sh && /tmp/platsurvey ".../DATA/M"/*.RAW
//
// The 0x09 fields are read the way ROTH.C reads them:
//   cmd_move_sector     raw_commands.c:4588   (the registrar)
//   tick_moving_sector  raw_commands.c:5869   (the per-frame tick)
//   apply_moving_sector_carry  renderer.c:985 (who rides)
#include "roth_raw.h"
#include "roth_platformplan.h"   // the SAME planner the loader runs
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <algorithm>
using namespace roth;

static std::vector<uint8_t> RF(const char *p)
{
	std::vector<uint8_t> o; FILE *f = fopen(p, "rb"); if (!f) return o;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); o.resize(n);
	if (fread(o.data(), 1, n, f) != (size_t)n) o.clear(); fclose(f); return o;
}

static bool PointInSector(const Map &m, int si, double px, double py)
{
	const Sector &s = m.sectors[si]; bool in = false;
	for (int j = 0; j < s.faceCount; j++)
	{
		const Face &f = m.faces[s.firstFaceIndex + j];
		if (f.vertex1 < 0 || f.vertex2 < 0) continue;
		double ax = m.vertices[f.vertex1].x, ay = m.vertices[f.vertex1].y;
		double bx = m.vertices[f.vertex2].x, by = m.vertices[f.vertex2].y;
		if ((ay > py) != (by > py) && px < (bx - ax) * (py - ay) / (by - ay) + ax) in = !in;
	}
	return in;
}

int main(int argc, char **argv)
{
	bool detail = false;
	int total09 = 0, maps09 = 0, totalMidMove = 0, totalRiders = 0, planYes = 0, planNo = 0, planActors = 0;
	std::map<std::string, int> shapeCount;
	for (int a = 1; a < argc; a++)
	{
		std::string path = argv[a];
		if (path == "-d") { detail = true; continue; }
		auto d = RF(argv[a]); if (d.empty()) continue;
		Map m = ParseRaw(d.data(), d.size());
		std::string name = path.substr(path.rfind('/') + 1);
		if (!m.ok()) { printf("%s ERR %s\n", name.c_str(), m.error.c_str()); continue; }

		// ---- which sectors can a script make a door of / which are doors
		int n09 = 0;
		for (size_t ci = 0; ci < m.commands.size(); ci++)
		{
			const Command &c = m.commands[ci];
			if ((c.opcode & 0x7f) != 0x09) continue;
			if (n09++ == 0) { maps09++; printf("\n== %s  (%zu sectors, %zu commands)\n", name.c_str(), m.sectors.size(), m.commands.size()); }
			total09++;

			// every sector carrying this commandID; the original takes the FIRST
			// (find_geometry_record, raw_commands.c:4601)
			std::vector<int> hits;
			for (size_t s = 0; s < m.sectors.size(); s++) if (m.sectors[s].commandID == c.key && c.key != 0) hits.push_back((int)s);
			const uint8_t f6 = c.Byte(6), spd = c.Byte(7);
			const int16_t wA = (int16_t)c.Word(0x0A), wC = (int16_t)c.Word(0x0C);
			const uint16_t dwell = c.Word(0x0E);
			const bool axisX = (f6 & 0x40) != 0;                 // raw_commands.c:4606
			const int tgtPos = (int16_t)(-2 * (int)wA);          // :5884  the MAX vertex stops here going +
			const int tgtNeg = (int16_t)(-2 * (int)wC);          // :5888  the MIN vertex stops here going -
			printf("cmd %4zu key %5u mod %02x f6 %02x(axis %c%s%s carry:%s%s shift:%s%s%s%s) speed %3u A %6d C %6d dwell %u size %zu\n",
				ci + 1, c.key, c.modifier, f6, axisX ? 'X' : 'Y', (f6 & 0x20) ? " repeat" : "", (c.modifier & 8) ? " DISABLED" : "",
				(f6 & 1) ? "floor" : "", (f6 & 4) ? "+mid" : "",
				(f6 & 1) ? "F" : "", (f6 & 2) ? "C" : "", (f6 & 4) ? "T" : "", (f6 & 8) ? "U" : "",
				spd, wA, wC, dwell, 6 + c.args.size() * 2);
			if (hits.empty()) { printf("     NO SECTOR carries this id\n"); shapeCount["no sector"]++; continue; }
			const int si = hits[0];
			const Sector &s = m.sectors[si];

			// the four vertices the registrar takes: vertex1 of the first four faces
			int vx[4], minx = 1 << 30, maxx = -(1 << 30), miny = 1 << 30, maxy = -(1 << 30);
			std::set<int> moved;
			for (int k = 0; k < 4; k++)
			{
				const Face &f = m.faces[s.firstFaceIndex + k];
				vx[k] = f.vertex1; moved.insert(f.vertex1);
			}
			for (int j = 0; j < s.faceCount; j++)
			{
				const Vertex &v = m.vertices[m.faces[s.firstFaceIndex + j].vertex1];
				minx = std::min(minx, (int)v.x); maxx = std::max(maxx, (int)v.x);
				miny = std::min(miny, (int)v.y); maxy = std::max(maxy, (int)v.y);
			}
			int lo = 1 << 30, hi = -(1 << 30);
			for (int k = 0; k < 4; k++) { const Vertex &v = m.vertices[vx[k]]; int cc = (axisX ? v.x : v.y) - (axisX ? m.wrapShiftX : m.wrapShiftY); lo = std::min(lo, cc); hi = std::max(hi, cc); }
			// travel: positive direction until max == tgtPos, negative until min == tgtNeg
			const int travPos = tgtPos - hi, travNeg = tgtNeg - lo;
			const bool startNeg = !(c.modifier & 0x02);          // :4623  chunk[5] |= 0x80 when bit 2 clear
			bool rect = s.faceCount == 4;
			if (rect) for (int k = 0; k < 4; k++) { const Face &f = m.faces[s.firstFaceIndex + k]; const Vertex &p = m.vertices[f.vertex1], &q = m.vertices[f.vertex2]; if (p.x != q.x && p.y != q.y) rect = false; }

			// neighbours across each face, and other sectors that share a moved vertex
			std::set<int> nb, sharers; int oneSided = 0, doorNb = 0;
			for (int j = 0; j < s.faceCount; j++)
			{
				const Face &f = m.faces[s.firstFaceIndex + j];
				if (f.sister < 0) { oneSided++; continue; }
				int o = m.faces[f.sister].sector; nb.insert(o);
				if (o >= 0 && m.sectors[o].IsDoorCapable()) doorNb++;
			}
			for (size_t fi = 0; fi < m.faces.size(); fi++)
			{
				const Face &f = m.faces[fi]; if (f.sector == si || f.sector < 0) continue;
				if (moved.count(f.vertex1) || moved.count(f.vertex2)) sharers.insert(f.sector);
			}
			// riders: objects in the sector, and those the original would carry
			// (sector +0x16 bit 2, z == floor: renderer.c:1005, :958)
			int objs = (int)m.objects[si].size(), onFloor = 0, onTop = 0;
			for (const Object &o : m.objects[si]) { if (o.z == s.floorHeight) onFloor++; if (s.platformIndex >= 0 && o.z == m.platforms[s.platformIndex].topZ) onTop++; }
			totalRiders += objs;
			// plan overlap with any other sector's bounding box (cheap flag only)
			printf("     sector %4d%s faces %d %s bbox x[%d..%d] y[%d..%d] (%dx%d) floor %d ceil %d (h %d) ftex %u ctex %u light %u flags %02x flags2 %04x%s\n",
				si, hits.size() > 1 ? "(+dups)" : "", s.faceCount, rect ? "RECT" : (s.faceCount == 4 ? "quad" : "POLY"),
				minx, maxx, miny, maxy, maxx - minx, maxy - miny, s.floorHeight, s.ceilingHeight, s.ceilingHeight - s.floorHeight,
				s.floorTexture, s.ceilingTexture, s.light, s.flags, s.flags2, (s.flags2 & 2) ? " OBJCARRY" : "");
			if (s.platformIndex >= 0) { const MidPlatform &mp = m.platforms[s.platformIndex]; printf("     mid-platform top %d underside %d ttex %u utex %u\n", mp.topZ, mp.undersideZ, mp.topTexture, mp.undersideTexture); }
			printf("     axis coord now [%d..%d]; +dir stops max at %d (travel %+d), -dir stops min at %d (travel %+d); starts %s; %d u/tic\n",
				lo, hi, tgtPos, travPos, tgtNeg, travNeg, startNeg ? "NEG" : "POS", 2 * spd);
			printf("     one-sided faces %d, neighbours %zu (door-capable %d), other sectors sharing moved vertices %zu, objects %d (z==floor %d, z==midtop %d), wrapShift %d,%d\n",
				oneSided, nb.size(), doorNb, sharers.size(), objs, onFloor, onTop, m.wrapShiftX, m.wrapShiftY);
			if (detail)
			{
				for (int k = 0; k < s.faceCount; k++) { const Face &f = m.faces[s.firstFaceIndex + k]; const Vertex &p = m.vertices[f.vertex1], &q = m.vertices[f.vertex2];
					int o = f.sister >= 0 ? m.faces[f.sister].sector : -1;
					printf("       face %d v%d(%d,%d)->v%d(%d,%d) ", k, f.vertex1, p.x, p.y, f.vertex2, q.x, q.y);
					if (o >= 0) { const Sector &t = m.sectors[o]; printf("-> sector %d floor %d ceil %d id %u faces %d%s\n", o, t.floorHeight, t.ceilingHeight, t.commandID, t.faceCount, t.IsDoorCapable() ? " DOOR" : ""); }
					else printf("one-sided (tm %d)\n", f.textureMap); }
				for (int o : sharers) if (!nb.count(o)) printf("       vertex-sharer (not a neighbour) sector %d\n", o);
				for (const Object &o : m.objects[si]) printf("       object at %d,%d z %d tex %u/%u rtype %02x id %u\n", o.x, o.y, o.z, o.textureSource, o.textureIndex, o.renderType, o.commandID);
				// who fires it
				for (size_t ti = 0; ti < m.commands.size(); ti++) { const Command &t = m.commands[ti]; if (!t.isTrigger) continue;
					int idx = t.chainStart, guard = 0; while (idx > 0 && idx <= (int)m.commands.size() && guard++ < 64) { if (idx == (int)ci + 1) { printf("       fired by trigger cmd %zu op %02x key %u\n", ti + 1, t.opcode, t.key); break; } idx = m.commands[idx - 1].nextIndex; } }
			}
			// ---- what the platform design needs to know (see the report):
			// are the four neighbours level with each other, and how does the
			// moving sector differ from them?
			bool uni = true; int nbF = 0, nbC = 0, nbL = 0; bool first = true; int nbFt = 0, nbCt = 0;
			for (int o : nb) { if (o < 0) continue; const Sector &t = m.sectors[o];
				if (first) { nbF = t.floorHeight; nbC = t.ceilingHeight; nbL = t.light; nbFt = t.floorTexture; nbCt = t.ceilingTexture; first = false; }
				else if (t.floorHeight != nbF || t.ceilingHeight != nbC) uni = false; }
			const bool slab = s.platformIndex >= 0 && m.platforms[s.platformIndex].topZ > m.platforms[s.platformIndex].undersideZ;
			const char *cls = "?";
			if (oneSided) cls = "E pocket (one-sided walls move)";
			else if (!uni) cls = "X neighbours not level";
			else if (s.floorHeight < nbF || s.ceilingHeight > nbC) cls = "X moving pit/alcove";
			else if (slab && s.floorHeight == nbF && s.ceilingHeight == nbC) cls = "A slab over a level channel";
			else if (s.floorHeight > nbF && s.ceilingHeight < nbC && s.ceilingHeight - s.floorHeight <= 8) cls = "C barrier (floor block to the ceiling)";
			else if (s.floorHeight > nbF && s.ceilingHeight < nbC) cls = "D floor block + ceiling block";
			else if (s.floorHeight > nbF) cls = "B raised floor plate/block";
			else if (s.ceilingHeight < nbC) cls = "B' hanging block";
			else cls = "N nothing differs (invisible)";
			// the sweep: sample the footprint at both ends and count the sectors under it
			std::set<int> under; int stacked = 0, voidPts = 0, doorUnder = 0;
			for (int end = 0; end < 3; end++)
			{
				const int off = end == 0 ? 0 : end == 1 ? travPos : travNeg;
				for (int ix = 0; ix <= 4; ix++) for (int iy = 0; iy <= 4; iy++)
				{
					double px = minx + (maxx - minx) * (ix * 0.24 + 0.02) + (axisX ? off : 0);
					double py = miny + (maxy - miny) * (iy * 0.24 + 0.02) + (axisX ? 0 : off);
					int n = 0;
					for (size_t t = 0; t < m.sectors.size(); t++) if (PointInSector(m, (int)t, px, py)) { n++; under.insert((int)t); }
					if (n > 1) stacked++; if (n == 0) voidPts++;
				}
			}
			for (int o : under) if (m.sectors[o].IsDoorCapable()) doorUnder++;
			int others = 0; std::string othersS;
			for (size_t ti = 0; ti < m.commands.size(); ti++) { const Command &t = m.commands[ti]; uint8_t op = t.opcode & 0x7f;
				if (ti == ci || t.isTrigger || t.key != c.key) continue;
				if (op == 0x03 || op == 0x07 || op == 0x0a || op == 0x0b || op == 0x0e || op == 0x1d || op == 0x02 || op == 0x11 || op == 0x09) { others++; char b[40]; snprintf(b, sizeof b, " %02x@%d", (unsigned)op, (int)ti + 1); othersS += b; } }
			printf("     CLASS %s | neighbours floor %d ceil %d light %d ftex %d ctex %d%s | sweep covers %zu sectors, stacked samples %d, void samples %d, door sectors %d | other sector cmds on key:%s\n",
				cls, nbF, nbC, nbL, nbFt, nbCt, uni ? "" : " (NOT uniform)", under.size(), stacked, voidPts, doorUnder, others ? othersS.c_str() : " none");
			if (detail) { printf("       sweep sectors:"); for (int o : under) printf(" %d", o); printf("\n"); }
			shapeCount[cls]++;
			char buf[64]; snprintf(buf, sizeof buf, "%s, %d faces", rect ? "rect" : "non-rect", s.faceCount); shapeCount[buf]++;
		}

		// ---- what the loader will build (roth_platforms 1): the planner's own answer
		if (n09)
		{
			const std::vector<rothplat::Plan> plans = rothplat::PlanMovingSectors(m);
			for (const rothplat::Plan &p : plans)
			{
				printf("  PLAN sector %4d records", p.sector);
				for (int r : p.records) printf(" %d", r);
				if (!p.supported) { printf("  -> NOT BUILT: %s\n", p.reason.c_str()); planNo++; continue; }
				planYes++;
				printf("  -> %s; footprint %dx%d, %zu box(es) of radius %g per piece;", p.reason.c_str(), p.x1 - p.x0, p.y1 - p.y0, p.centres.size(), p.radius);
				for (const rothplat::Piece &b : p.pieces) { printf(" %s z %d..%d", b.kind == rothplat::PIECE_SLAB ? "slab" : b.kind == rothplat::PIECE_FLOORBLOCK ? "floor-block" : "ceil-block", b.z0, b.z1); planActors += (int)p.centres.size(); }
				printf("; static sector: floor %s, ceiling %s\n", p.lowerFloor ? "lowered to neighbours'" : "kept", p.raiseCeil ? "raised to neighbours'" : "kept");
			}
		}

		// ---- moving mid-platforms: 0x03 with +0x06 bit 0 reaches the slab
		// (raw_commands.c:2590); 0x0a/0x0b bit 0x04 is the ALT path through +0x18.
		int midMove = 0, midSecs = 0, midObjs = 0, platSectors = 0, platObjs = 0;
		for (size_t s = 0; s < m.sectors.size(); s++) if (m.sectors[s].platformIndex >= 0) { platSectors++; platObjs += (int)m.objects[s].size(); }
		for (size_t ci = 0; ci < m.commands.size(); ci++)
		{
			const Command &c = m.commands[ci]; const uint8_t op = c.opcode & 0x7f;
			if (op != 0x03 || !(c.Byte(6) & 1)) continue;
			int withPlat = 0, ob = 0;
			for (size_t s = 0; s < m.sectors.size(); s++) if (m.sectors[s].commandID == c.key && c.key && m.sectors[s].platformIndex >= 0) { withPlat++; ob += (int)m.objects[s].size(); }
			if (!withPlat) continue;
			midMove++; midSecs += withPlat; midObjs += ob;
			if (detail) printf("   %s cmd %zu op 03 key %u f6 %02x speed %u limit %d: %d sector(s) with a slab, %d object(s) in them\n", name.c_str(), ci + 1, c.key, c.Byte(6), c.Byte(7), 2 * (int16_t)c.Word(0x0A), withPlat, ob);
		}
		totalMidMove += midMove;
		int allObjs = 0; for (auto &v : m.objects) allObjs += (int)v.size();
		printf("-- %-13s 0x09 %2d | mid-platform sectors %3d (objects in them %3d) | moving-slab cmds %2d (sectors %2d, objects %2d) | objects total %4d\n",
			name.c_str(), n09, platSectors, platObjs, midMove, midSecs, midObjs, allObjs);
	}
	printf("\nTOTAL 0x09 records %d in %d maps; moving-slab commands %d; objects inside 0x09 sectors %d\n", total09, maps09, totalMidMove, totalRiders);
	printf("PLANNER: %d moving sectors become platforms (%d actors), %d are not built\n", planYes, planActors, planNo);
	for (auto &p : shapeCount) printf("  shape %-22s %d\n", p.first.c_str(), p.second);
	return 0;
}
