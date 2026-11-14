//
// kvx_roundtrip -- write voxel models with roth::VoxBuildKvx and read them back
// with the ENGINE'S OWN KVX parser, src/common/models/voxels.cpp (R_LoadKVX),
// compiled unchanged against the stubs in engine_stub/ (they supply only the
// engine types it names: TArray, DVector3, the file system that hands it the
// bytes). Then walk the parsed slabs exactly as FVoxelModel::Initialize does
// (models_voxel.cpp) and check, for every model:
//   - R_LoadKVX accepts it: one mip, the sizes, the pivot;
//   - every surface voxel is in exactly one slab, with its colour, and no
//     other voxel is;
//   - the faces FVoxelModel::MakeSlabPolys would emit are exactly the exposed
//     faces of the model (side bits per slab, top on a slab's first voxel,
//     bottom on its last);
//   - the palette is the model's, 6-bit.
//
//   kvx_roundtrip                       synthetic models (no retail data)
//   kvx_roundtrip <DATA dir> 15,16,20   also the voxelizer's models of those items
//
#include "../../src/roth/roth_voxelize.h"
#include "../../src/roth/roth_dbase100.h"

#include "v_video.h"
#include "filesystem.h"
#include "voxels.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using roth::VoxModel;

static int g_fail = 0;

static bool Check(bool ok, const char *what, const std::string &name)
{
	if (!ok) { printf("  FAIL %s: %s\n", name.c_str(), what); g_fail++; }
	return ok;
}

// faces: (kx, ky, kz, face bit) in KVX coordinates.
typedef std::tuple<int, int, int, int> Face;

static bool RoundTrip(const VoxModel &m, const std::string &name, bool expectFail = false)
{
	std::string err;
	const std::vector<uint8_t> kvx = roth::VoxBuildKvx(m, &err);
	if (expectFail)
	{
		Check(kvx.empty(), "a model KVX cannot hold was written", name);
		printf("  %-28s refused as expected (%s)\n", name.c_str(), err.c_str());
		return kvx.empty();
	}
	if (!Check(!kvx.empty(), ("VoxBuildKvx failed: " + err).c_str(), name)) return false;
	fileSystem.lump = kvx;
	FVoxel *v = R_LoadKVX(0);
	if (!Check(v != nullptr, "R_LoadKVX rejected the bytes", name)) return false;
	const int fails0 = g_fail;
	Check(v->NumMips == 1, "mip count", name);
	const FVoxelMipLevel &mip = v->Mips[0];
	Check(mip.SizeX == m.sx && mip.SizeY == m.sz && mip.SizeZ == m.sy, "sizes", name);
	Check(fabs(mip.Pivot.X - (-m.originX / m.voxelPx)) <= 1.0 / 256 + 1e-9 && fabs(mip.Pivot.Y - (-m.originZ / m.voxelPx)) <= 1.0 / 256 + 1e-9 &&
		mip.Pivot.Z == m.sy, "pivot", name);
	bool palOk = v->Palette.Size() == 768;
	for (int i = 0; palOk && i < 768; i++) palOk = v->Palette[i] == (m.palette[i] >> 2);
	Check(palOk, "palette", name);

	// Model -> KVX: kx = x, ky = z, kz = sy - 1 - y.
	const int zs = m.sy;
	auto solidK = [&](int kx, int ky, int kz) { return m.Solid(kx, zs - 1 - kz, ky); };
	std::set<Face> expected;
	std::set<std::tuple<int, int, int>> surface;
	for (int kx = 0; kx < m.sx; kx++)
		for (int ky = 0; ky < m.sz; ky++)
			for (int kz = 0; kz < zs; kz++)
			{
				if (!solidK(kx, ky, kz)) continue;
				const int open = (!solidK(kx - 1, ky, kz) ? 1 : 0) | (!solidK(kx + 1, ky, kz) ? 2 : 0) | (!solidK(kx, ky - 1, kz) ? 4 : 0) |
					(!solidK(kx, ky + 1, kz) ? 8 : 0) | (!solidK(kx, ky, kz - 1) ? 16 : 0) | (!solidK(kx, ky, kz + 1) ? 32 : 0);
				if (!open) continue;
				surface.insert({ kx, ky, kz });
				for (int b = 1; b <= 32; b <<= 1) if (open & b) expected.insert({ kx, ky, kz, b });
			}

	// The engine's walk (FVoxelModel::Initialize / MakeSlabPolys).
	std::set<Face> emitted;
	std::set<std::tuple<int, int, int>> seen;
	int dup = 0, notSurface = 0, badColour = 0;
	uint8_t *slabs = mip.GetSlabData(false);
	for (int x = 0; x < mip.SizeX; x++)
	{
		uint8_t *slabxoffs = &slabs[mip.OffsetX[x]];
		short *xyoffs = &mip.OffsetXY[x * (mip.SizeY + 1)];
		for (int y = 0; y < mip.SizeY; y++)
		{
			kvxslab_t *voxptr = (kvxslab_t *)(slabxoffs + xyoffs[y]);
			kvxslab_t *voxend = (kvxslab_t *)(slabxoffs + xyoffs[y + 1]);
			for (; voxptr < voxend; voxptr = (kvxslab_t *)((uint8_t *)voxptr + voxptr->zleng + 3))
			{
				const int cull = voxptr->backfacecull;
				for (int j = 0; j < voxptr->zleng; j++)
				{
					const int z = voxptr->ztop + j;
					if (!seen.insert({ x, y, z }).second) dup++;
					if (!surface.count({ x, y, z })) notSurface++;
					else if (voxptr->col[j] != m.colour[m.Index(x, zs - 1 - z, y)]) badColour++;
					for (int b = 1; b <= 8; b <<= 1) if (cull & b) emitted.insert({ x, y, z, b });
					if (j == 0 && (cull & 16)) emitted.insert({ x, y, z, 16 });
					if (j == voxptr->zleng - 1 && (cull & 32)) emitted.insert({ x, y, z, 32 });
				}
			}
		}
	}
	Check(dup == 0, "a voxel in two slabs", name);
	Check(notSurface == 0, "a slab voxel that is not a surface voxel", name);
	Check(seen.size() == surface.size(), "surface voxels missing from the slabs", name);
	Check(badColour == 0, "colours", name);
	Check(emitted == expected, "the faces drawn are not the exposed faces", name);
	printf("  %-28s %3dx%3dx%3d  %7zu surface voxels  %8zu faces  %8zu bytes  %s\n", name.c_str(), m.sx, m.sy, m.sz, surface.size(),
		expected.size(), kvx.size(), g_fail == fails0 ? "ok" : "FAILED");
	delete v;
	return g_fail == fails0;
}

static VoxModel Blank(int sx, int sy, int sz)
{
	VoxModel m;
	m.sx = sx; m.sy = sy; m.sz = sz; m.voxelPx = 1.0;
	m.originX = -sx / 2.0 + 0.3; m.originY = 0; m.originZ = -sz / 2.0 - 0.2;
	m.solid.assign((size_t)sx * sy * sz, 0);
	m.colour.assign(m.solid.size(), 0);
	for (int i = 0; i < 256; i++) { m.palette[i * 3] = (uint8_t)i; m.palette[i * 3 + 1] = (uint8_t)(255 - i); m.palette[i * 3 + 2] = (uint8_t)(i * 7); }
	return m;
}

static std::vector<uint8_t> ReadFile(const std::string &path)
{
	std::vector<uint8_t> b;
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return b;
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	b.resize((size_t)n);
	if (n > 0 && fread(b.data(), 1, (size_t)n, f) != (size_t)n) b.clear();
	fclose(f);
	return b;
}

int main(int argc, char **argv)
{
	std::mt19937 rng(1234);
	printf("synthetic models through R_LoadKVX (src/common/models/voxels.cpp, unchanged):\n");
	{
		VoxModel m = Blank(41, 41, 41);
		for (int z = 0; z < 41; z++) for (int y = 0; y < 41; y++) for (int x = 0; x < 41; x++)
		{
			const int dx = x - 20, dy = y - 20, dz = z - 20;
			if (dx * dx + dy * dy + dz * dz <= 400) { m.solid[m.Index(x, y, z)] = 1; m.colour[m.Index(x, y, z)] = (uint8_t)rng(); }
		}
		RoundTrip(m, "sphere r20");
	}
	{
		// A box with a through hole and a closed pocket (inner surfaces).
		VoxModel m = Blank(30, 20, 24);
		for (int z = 0; z < 24; z++) for (int y = 0; y < 20; y++) for (int x = 0; x < 30; x++)
		{
			bool s = true;
			if (x >= 12 && x < 18 && y >= 6 && y < 14) s = false;                          // the hole, along z
			if (x >= 3 && x < 8 && y >= 4 && y < 9 && z >= 5 && z < 10) s = false;          // the pocket
			m.solid[m.Index(x, y, z)] = s;
			m.colour[m.Index(x, y, z)] = (uint8_t)(x * 8 + y);
		}
		RoundTrip(m, "box, hole and pocket");
	}
	{
		VoxModel m = Blank(50, 1, 37);
		for (size_t i = 0; i < m.solid.size(); i++) { m.solid[i] = (rng() % 4) != 0; m.colour[i] = (uint8_t)rng(); }
		RoundTrip(m, "one-voxel plate, holed");
	}
	{
		VoxModel m = Blank(40, 40, 40);
		for (size_t i = 0; i < m.solid.size(); i++) { m.solid[i] = (rng() % 10) < 3; m.colour[i] = (uint8_t)rng(); }
		RoundTrip(m, "30% noise");
	}
	{
		VoxModel m = Blank(3, 255, 3);
		for (size_t i = 0; i < m.solid.size(); i++) { m.solid[i] = 1; m.colour[i] = (uint8_t)(i & 0xff); }
		RoundTrip(m, "255 tall column");
	}
	{
		VoxModel m = Blank(200, 140, 200);
		for (int z = 0; z < 200; z++) for (int y = 0; y < 140; y++) for (int x = 0; x < 200; x++)
		{
			const int dx = x - 100, dz = z - 100;
			const bool s = dx * dx + dz * dz < 99 * 99 && ((x + y + z) % 7 != 0 || y % 2);
			m.solid[m.Index(x, y, z)] = s; m.colour[m.Index(x, y, z)] = (uint8_t)(x ^ y ^ z);
		}
		RoundTrip(m, "200 wide, porous");
	}
	{
		VoxModel m = Blank(2, 256, 2);
		for (size_t i = 0; i < m.solid.size(); i++) m.solid[i] = 1;
		RoundTrip(m, "256 tall", true);
	}

	if (argc >= 3)
	{
		const std::string dir = argv[1];
		std::vector<uint8_t> d100 = ReadFile(dir + "/DBASE100.DAT"), d300 = ReadFile(dir + "/DBASE300.DAT");
		roth::Dbase100 db;
		if (d100.empty() || d300.empty() || !db.Load(d100.data(), d100.size())) { printf("cannot read %s\n", dir.c_str()); return 1; }
		printf("the voxelizer's models of retail items:\n");
		std::string list = argv[2];
		size_t p = 0;
		while (p < list.size())
		{
			const int id = atoi(list.c_str() + p);
			const roth::DbItem *it = db.Item(id);
			const uint8_t *gdv = nullptr;
			size_t gsz = 0;
			if (it && roth::VoxCloseupBytes(d300.data(), d300.size(), it->closeup, gdv, gsz))
			{
				const roth::VoxResult r = roth::VoxelizeCloseup(gdv, gsz);
				if (r.ok) RoundTrip(r.model, "item " + std::to_string(id));
				else printf("  item %d: no model (%s)\n", id, r.error.c_str());
			}
			p = list.find(',', p);
			if (p == std::string::npos) break;
			p++;
		}
	}
	printf(g_fail ? "KVX ROUND TRIP: %d FAILURES\n" : "KVX ROUND TRIP: all ok\n", g_fail);
	return g_fail ? 1 : 0;
}
