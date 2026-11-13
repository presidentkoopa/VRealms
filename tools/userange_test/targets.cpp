// targets.cpp -- the use/examine targets of one map, read from the retail .RAW and DBASE100.DAT through the
// engine's own readers, with the geometry the use-range probe needs to stand in front of them.
// Engine-free; writes nothing.
//
//   g++ -std=c++17 -O1 -I src/roth -o targets tools/userange_test/targets.cpp src/roth/roth_raw.cpp \
//       src/roth/roth_install.cpp src/roth/roth_dbase100.cpp src/roth/roth_dbase100_install.cpp src/roth/roth_log.cpp
//   ./targets "<ROTH DATA folder>" STUDY1
//
// Prints:
//   START x y angle
//   SEC   idx off floor ceil cmdID cx cy                         (every sector: centroid of its face vertices)
//   FACE  trigOp key faceIdx sec v1x v1y v2x v2y nx ny len sister  (faces named by 0x18/0x32 triggers; n = unit normal into the sector)
//   FLOOR trigOp key sec off floor ceil cx cy                    (sectors named by 0x19/0x31 triggers)
//   DOOR  sec off floor ceil cx cy face v1x v1y v2x v2y         (door sectors and their faces)
//   ITEM  sec idx x y z rid itemId pickable flags9               (objects whose art is an inventory record's)
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "roth_raw.h"
#include "roth_install.h"
#include "roth_dbase100.h"
#include "roth_log.h"

using namespace roth;

static std::vector<uint8_t> ReadAll(const std::string &p)
{
	std::vector<uint8_t> b;
	FILE *f = fopen(p.c_str(), "rb");
	if (!f) return b;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	b.resize((size_t)n);
	if (n > 0 && fread(b.data(), 1, (size_t)n, f) != (size_t)n) b.clear();
	fclose(f);
	return b;
}

int main(int argc, char **argv)
{
	if (argc < 3) { printf("usage: targets <ROTH DATA folder> MAP\n"); return 2; }
	if (!TheInstall().Open(argv[1])) { printf("install: %s\n", TheInstall().Error().c_str()); return 2; }
	Dbase100 db;
	Log log;
	if (!LoadDbase100FromInstall(TheInstall(), db, &log)) { printf("DBASE100 not loaded\n"); return 2; }
	std::vector<uint8_t> raw = ReadAll(TheInstall().MapFile(argv[2]));
	if (raw.empty()) { printf("no map %s\n", argv[2]); return 2; }
	Map m = ParseRaw(raw.data(), raw.size());
	if (!m.ok()) { printf("parse: %s\n", m.error.c_str()); return 2; }
	printf("START %d %d %d\n", m.metadata.startX, m.metadata.startY, m.metadata.rotation);

	const int ns = (int)m.sectors.size();
	std::vector<double> cx(ns, 0), cy(ns, 0); std::vector<int> cn(ns, 0);
	for (const Face &f : m.faces)
	{
		if (f.sector < 0 || f.vertex1 < 0) continue;
		cx[f.sector] += m.vertices[f.vertex1].x; cy[f.sector] += m.vertices[f.vertex1].y; cn[f.sector]++;
	}
	for (int s = 0; s < ns; s++)
	{
		if (cn[s]) { cx[s] /= cn[s]; cy[s] /= cn[s]; }
		const Sector &S = m.sectors[s];
		printf("SEC %d %u %d %d %u %.0f %.0f\n", s, 30 + s * 26, S.floorHeight, S.ceilingHeight, S.commandID, cx[s], cy[s]);
	}
	auto face = [&](const char *tag, int fi) {
		const Face &f = m.faces[fi];
		const Vertex a = m.vertices[f.vertex1], b = m.vertices[f.vertex2];
		double dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy);
		double nx = -dy / len, ny = dx / len;
		// into the sector: towards its centroid
		double mx = (a.x + b.x) / 2., my = (a.y + b.y) / 2.;
		if ((cx[f.sector] - mx) * nx + (cy[f.sector] - my) * ny < 0) { nx = -nx; ny = -ny; }
		printf("%s %d sec=%d v1=%d,%d v2=%d,%d n=%.3f,%.3f len=%.0f sister=%d\n", tag, fi, f.sector, a.x, a.y, b.x, b.y, nx, ny, len, f.sister);
	};
	for (size_t ci = 0; ci < m.commands.size(); ci++)
	{
		const Command &c = m.commands[ci];
		if (!c.isTrigger) continue;
		if (c.opcode == 0x18 || c.opcode == 0x32)
			for (int fi : c.faces) { char t[64]; snprintf(t, sizeof t, "FACE op=%02x rec=%zu key=%u mod=%02x", c.opcode, ci + 1, c.key, c.modifier); face(t, fi); }
		if (c.opcode == 0x19 || c.opcode == 0x31)
			for (int s = 0; s < ns; s++)
				if (m.sectors[s].commandID == c.key && c.key)
					printf("FLOOR op=%02x rec=%zu key=%u mod=%02x f6=%02x sec=%d off=%u floor=%d ceil=%d c=%.0f,%.0f\n", c.opcode, ci + 1, c.key, c.modifier, c.fireFlags, s,
						30 + s * 26, m.sectors[s].floorHeight, m.sectors[s].ceilingHeight, cx[s], cy[s]);
	}
	for (int s = 0; s < ns; s++)
	{
		if (!m.sectors[s].IsDoorCapable()) continue;
		printf("DOOR sec=%d id=%04x floor=%d ceil=%d c=%.0f,%.0f\n", s, m.sectors[s].commandID, m.sectors[s].floorHeight, m.sectors[s].ceilingHeight, cx[s], cy[s]);
		for (size_t fi = 0; fi < m.faces.size(); fi++) if (m.faces[fi].sector == s) face("  DFACE", (int)fi);
	}
	for (int s = 0; s < (int)m.objects.size(); s++)
		for (size_t i = 0; i < m.objects[s].size(); i++)
		{
			const Object &o = m.objects[s][i];
			const uint16_t rid = (uint16_t)(o.textureIndex | (o.textureSource << 8));
			const int id = db.FindItemByRenderId(rid);
			if (id <= 0) continue;
			const DbItem *it = db.Item(id);
			const bool pick = it && !(it->flags & DBI_NOT_PICKABLE) && it->Category() != DB_CATEGORY_INFO;
			printf("ITEM %d.%zu x=%d y=%d z=%d rid=%04x item=%d pick=%d f9=%02x floor=%d\n", s, i, o.x, o.y, o.z, rid, id, pick, o.flags,
				s < ns ? m.sectors[s].floorHeight : 0);
		}
	return 0;
}
