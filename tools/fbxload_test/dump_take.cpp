// tools/fbxload_test/dump_take.cpp -- every frame of both hands' takes as the engine's loader reads them (fbx_skinned.cpp
// + ufbx), written out for walk_poses.py: per hand the joint names, parents, the rest local TRS and every frame's local
// TRS (t xyz, q xyzw, s xyz), and the skinned mesh as the engine gets it (rest position, normal, uv, 4 bone/weight
// pairs, triangles), so walk_poses.py can draw any authored frame or composition. Nothing is computed here; the walk is
// in walk_poses.py.
//
//   dump_take <hand_final.fbx> <out.json>

#include "fbx_skinned.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace FBXSkinned;

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: dump_take <fbx> <out.json>\n"); return 2; }
	FILE *f = fopen(argv[1], "rb");
	if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
	fseek(f, 0, SEEK_END);
	std::vector<char> d(ftell(f));
	fseek(f, 0, SEEK_SET);
	if (fread(d.data(), 1, d.size(), f) != d.size()) { fprintf(stderr, "short read\n"); return 2; }
	fclose(f);

	FILE *o = fopen(argv[2], "w");
	if (!o) { fprintf(stderr, "cannot write %s\n", argv[2]); return 2; }
	// the selectors vrealms_iwad/MODELDEF.txt uses
	const char *meshes[2] = { "Hand_Low", "Hand_Low1" };
	const char *selectors[2] = { "Hand_Low", "Hand_Low1?mirrorskin=Hand_Low" };
	fprintf(o, "{\n");
	for (int h = 0; h < 2; h++)
	{
		Result r; std::string err;
		if (!Load(d.data(), d.size(), selectors[h], r, err)) { fprintf(stderr, "%s: %s\n", selectors[h], err.c_str()); return 2; }
		const size_t nj = r.joints.size();
		auto trs = [&](const TRSd &t) {
			fprintf(o, "[%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g]", t.t[0], t.t[1], t.t[2], t.q[0], t.q[1], t.q[2], t.q[3], t.s[0], t.s[1], t.s[2]);
		};
		fprintf(o, "%s\"%s\": {\"fps\": %.17g, \"names\": [", h ? ",\n" : "", meshes[h], r.takes.empty() ? 0.0 : r.takes[0].fps);
		for (size_t j = 0; j < nj; j++) fprintf(o, "%s\"%s\"", j ? "," : "", r.joints[j].name.c_str());
		fprintf(o, "],\n \"parents\": [");
		for (size_t j = 0; j < nj; j++) fprintf(o, "%s%d", j ? "," : "", r.joints[j].parent);
		fprintf(o, "],\n \"rest\": [");
		for (size_t j = 0; j < nj; j++) { if (j) fprintf(o, ","); trs(r.joints[j].rest); }
		fprintf(o, "],\n \"frames\": [\n");
		for (int fr = 0; fr < r.numFrames; fr++)
		{
			fprintf(o, "%s  [", fr ? ",\n" : "");
			for (size_t j = 0; j < nj; j++) { if (j) fprintf(o, ","); trs(r.frames[(size_t)fr * nj + j]); }
			fprintf(o, "]");
		}
		fprintf(o, "\n ],\n \"verts\": [");
		for (size_t v = 0; v < r.verts.size(); v++)
		{
			const Vertex &x = r.verts[v];
			fprintf(o, "%s[%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u,%u,%u,%u,%u,%u,%u,%u]", v ? "," : "", x.pos[0], x.pos[1], x.pos[2], x.nrm[0], x.nrm[1], x.nrm[2],
				x.uv[0], x.uv[1], x.bone[0], x.bone[1], x.bone[2], x.bone[3], x.weight[0], x.weight[1], x.weight[2], x.weight[3]);
		}
		fprintf(o, "],\n \"tris\": [");
		for (size_t t = 0; t < r.tris.size(); t++) fprintf(o, "%s%u", t ? "," : "", r.tris[t]);
		fprintf(o, "]}");
	}
	fprintf(o, "\n}\n");
	fclose(o);
	return 0;
}
