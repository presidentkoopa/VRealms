// tools/colt_test/fbx_part.cpp -- the FBX loader's part=<joint> option (fbx_skinned.h) on the Colt: each part keeps the
// triangles bound to that joint and those under it, centred on their own box; a joint that is not there is refused.
//   fbx_part <colt1911.fbx>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "fbx_skinned.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace FBXSkinned;
static int gFails = 0;
static void Check(bool ok, const std::string &what) { printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str()); if (!ok) gFails++; }
int main(int argc, char **argv)
{
	if (argc < 2) return 2;
	FILE *f = fopen(argv[1], "rb");
	if (!f) return 2;
	fseek(f, 0, SEEK_END); std::vector<char> d(ftell(f)); fseek(f, 0, SEEK_SET);
	if (fread(d.data(), 1, d.size(), f) != d.size()) return 2;
	fclose(f);
	Result whole; std::string err;
	Check(Load(d.data(), d.size(), "Default_1", whole, err), "the whole mesh loads (" + err + ")");
	// every vertex rigid on one joint (the parts are cut by joint)
	int rigid = 0; for (const Vertex &v : whole.verts) if (v.weight[0] == 255) rigid++;
	Check(rigid == (int)whole.verts.size(), "every vertex bound to one joint (" + std::to_string(rigid) + " of " + std::to_string(whole.verts.size()) + ")");
	struct P { const char *sel; int tris; };
	const P parts[] = { { "Default_1?part=Magaz", 2938 }, { "Default_1?part=Gils", 264 }, { "Default_1?part=Bullet_2", 334 } };
	for (const P &p : parts)
	{
		Result r;
		const bool ok = Load(d.data(), d.size(), p.sel, r, err);
		double lo[3] = { 1e30, 1e30, 1e30 }, hi[3] = { -1e30, -1e30, -1e30 };
		for (const Vertex &v : r.verts) for (int k = 0; k < 3; k++) { lo[k] = fmin(lo[k], v.pos[k]); hi[k] = fmax(hi[k], v.pos[k]); }
		double off = 0; for (int k = 0; k < 3; k++) off = fmax(off, fabs(lo[k] + hi[k]));
		char b[300];
		snprintf(b, sizeof b, "%s: %zu triangles (expected %d), centred (box centre within %.4f), centre taken off (%.2f %.2f %.2f), joints %zu = the whole mesh's %zu",
			p.sel, r.tris.size() / 3, p.tris, off / 2, r.partCentre[0], r.partCentre[1], r.partCentre[2], r.joints.size(), whole.joints.size());
		Check(ok && (int)(r.tris.size() / 3) == p.tris && off < 1e-3 && r.joints.size() == whole.joints.size(), b);
	}
	Result bad;
	Check(!Load(d.data(), d.size(), "Default_1?part=Nope", bad, err) && err.find("no joint named") != std::string::npos, "an unknown joint refused: " + err);
	printf("%s: %d failed\n", gFails ? "FAIL" : "PASS", gFails);
	return gFails ? 1 : 0;
}
