#pragma once
//
// roth_img3d_core.h -- THE ITEM MODELS MADE ON THE OWNER'S PC (tools/img3d, docs/IMG3D_PIPELINE.md), read back.
// Engine-free (rapidjson only), so tools/img3d_test runs the very code the engine runs.
//
// The owner's decision of 2026-11-16: the held and lying items are real 3D in VR. Their meshes are generated on his
// PC by a local image-to-3D model from the item's own pictures in his install, cleaned and scaled by
// tools/img3d/post.py, and written OUTSIDE the repository with a manifest. Nothing here was derived from retail art
// and nothing it reads is ever committed (CLAUDE.md).
//
// THE MANIFEST (manifest.json, written by post.py; tools/img3d/make_placeholder.py writes one by hand):
//   { "version": 1,
//     "items": [ { "item": 16, "name": "Matchbox", "file": "16_matchbox.glb",
//                  "metres": 0.055, "size": "table",           "table": the longest side was set from the per-item
//                                                               table; "picture": post.py did not know it (the
//                                                               engine sizes the lying item to its picture)
//                  "grip": [0, 0.008, 0],                       metres, in the model's own glTF axes
//                  "grip_rot": [0, 0, 0],                       degrees: yaw about +Y, pitch about +X, roll about +Z
//                  "hold": "palm",                              "palm" | "fist" | "hang" (below)
//                  "world_yaw": 0 } ] }                         degrees added to the object's facing when lying
//   `file` is relative to the manifest's folder. Unknown keys are ignored; a bad entry is skipped and named.
//
// THE MESH (one GLB, glTF 2.0 binary): metres, +Y up, +Z the item's front, the lying base at y = 0 (post.py puts it
// there). Every triangle primitive of every mesh the default scene reaches is read, node transforms applied;
// POSITION required, NORMAL and TEXCOORD_0 optional; indices u8/u16/u32 or none. ONE picture: the first primitive's
// material's baseColorTexture (post.py writes a single material), kept ENCODED (PNG/JPG) for the engine's own
// stb_image (common/models/fbx_image.h).
//
// THE PARTS (2026-11-23, tools/img3d/film_build.py: glass, a glowing liquid, a cork, a cut gem): every primitive is a
// part with its own material, primitives sharing a material joined into one. Of a material this reads: the base
// colour picture (embedded), baseColorFactor (used as the part's colour when it has no picture; a picture is drawn as
// it is), alphaMode BLEND (drawn see-through at the factor's alpha) and a non-zero emissiveFactor (it glows: drawn
// fullbright). A primitive with no material is one opaque white part. `image` above stays the FIRST part's picture.
//
// THE TWO PLACEMENTS (the engine's model space puts the vertical in the MIDDLE, model_memorymesh.h):
//   lying in the world   glTF (x, y, z) -> Doom (forward = +z, left = +x, up = +y) -> model (z, y, x). A proper
//                        turn, so the item is not mirrored; the actor's yaw is the object's facing.
//   in a hand            the grip frame (r_data/vr_handpose_math.h DefaultHandFit): X wrist -> fingers, -Y the palm
//                        normal (+Y the back of the hand), the thumb on -Z for the right hand and +Z for the left, in
//                        METRES, the grip socket at the origin. A model drawn there with no seat takes its vertex
//                        coordinates AS hand-frame coordinates (models.cpp: the seat is the only thing between them),
//                        so a vertex is written as its hand-frame point. Before that: minus the grip point, then the
//                        grip turn, then the hold:
//                          palm  up -> out of the palm (-Y), front -> the fingers (+X), left -> +Z (both hands)
//                          fist  up -> the thumb (-Z right, +Z left), front -> the fingers (+X)
//                          hang  not hand-locked: the engine stands it upright at the hand's palm each tic
//                        All three are proper turns (det +1).
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace roth
{
namespace img3d
{

enum Hold { HOLD_PALM = 0, HOLD_FIST = 1, HOLD_HANG = 2 };

struct Entry
{
	int item = 0;
	std::string name;
	std::string file;             // as written (relative to the manifest's folder)
	double metres = 0.0;          // the mesh's longest side as post.py made it
	bool sizeFromTable = false;   // true: `metres` is the item's real size; false: size it to its picture
	double grip[3] = { 0, 0, 0 };
	double gripRot[3] = { 0, 0, 0 };
	Hold hold = HOLD_PALM;
	double worldYaw = 0.0;
};

// Parse a manifest. False only when the text is not a manifest at all (`err` says why); bad entries are skipped and
// listed in `skipped`, one line each.
bool ParseManifest(const char *json, size_t length, std::vector<Entry> &out, std::string &err,
	std::vector<std::string> *skipped = nullptr);

struct Part
{
	size_t firstIndex = 0, indexCount = 0;   // its triangles: idx[firstIndex .. firstIndex + indexCount)
	int material = -1;                       // the glTF material (-1: none)
	std::string name;
	std::vector<uint8_t> image;              // its base colour picture, encoded; empty: drawn in `color`
	std::string imageMime;
	float color[4] = { 1.f, 1.f, 1.f, 1.f };   // baseColorFactor, 0..1
	bool blend = false;                      // alphaMode BLEND: see-through at color[3]
	bool glow = false;                       // emissiveFactor not zero: fullbright
	bool Plain() const { return !blend && !glow; }
};

struct Mesh
{
	std::vector<float> pos;        // x y z per vertex, metres, glTF axes, node transforms applied
	std::vector<float> nrm;        // x y z per vertex; empty when the file has none
	std::vector<float> uv;         // u v per vertex, glTF's (v down, top-left origin); empty when none
	std::vector<uint32_t> idx;     // three per triangle
	std::vector<uint8_t> image;    // the base colour picture, encoded; empty when none
	std::string imageMime;
	float lo[3] = { 0, 0, 0 }, hi[3] = { 0, 0, 0 };   // bounds of pos
	int primitives = 0;
	std::vector<Part> parts;       // at least one when ReadGlb succeeds (THE PARTS)
	// The part the item's own object wears: the first plain one (neither see-through nor glowing), else the first.
	int Primary() const { for (size_t k = 0; k < parts.size(); k++) if (parts[k].Plain()) return (int)k; return 0; }
	size_t Vertices() const { return pos.size() / 3; }
	size_t Triangles() const { return idx.size() / 3; }
	float Longest() const;
};

// Read a GLB. False with `err` when it is not one this reader takes.
bool ReadGlb(const uint8_t *data, size_t size, Mesh &out, std::string &err);

// A point or a direction from the mesh's glTF axes into the engine's model space, lying in the world. `scaleH` and
// `scaleV` are map units per metre across and up (the caller's pixel stretch is in scaleV).
void WorldPoint(const float g[3], double scaleH, double scaleV, float out[3]);
void WorldDirection(const float g[3], float out[3]);

// The same into the hand frame (metres, grip socket at the origin) for the hand of `rightHand`. HOLD_HANG uses the
// palm placement (the engine does not hand-lock it, but a caller may still want the numbers).
void HandPoint(const Entry &e, bool rightHand, const float g[3], float out[3]);
void HandDirection(const Entry &e, bool rightHand, const float g[3], float out[3]);

const char *HoldName(Hold h);

} // namespace img3d
} // namespace roth
