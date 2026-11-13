/*
** fbx_skinned.h
**
** RS FORK -- ONE SKINNED MESH OF AN FBX FILE, READ BY ufbx, AS PLAIN ARRAYS.
**
** The engine-free half of the FBX model (models_fbx.cpp is the engine half). Nothing of the
** engine in here -- std containers, doubles and ufbx only -- so tools/fbxload_test compiles and
** runs exactly this code against the file outside the engine.
**
** What it produces is what an IQM carries, so the engine draws it with the IQM bone path
** (models_iqm.cpp CalculateBones) unchanged:
**   - the skeleton: every joint the mesh's skin binds to (the FBX's own node names), parents
**     first; each with its REST transform as a local TRS (a root joint: its whole model-space
**     transform, so the controls above it -- a mirror, an animated arm -- are part of it);
**   - every frame of every take (anim stack) of the file, each joint's local TRS per frame,
**     evaluated by ufbx at the file's own frame rate over the span its curves are keyed;
**   - the mesh in the rest pose, one vertex per polygon corner, triangulated, with UVs, normals
**     and the 4 largest skin weights per vertex (the engine's FModelVertex has 4 bone slots).
**
** SPACE. ufbx converts the scene to right-handed Z-up (ufbx_axes_right_handed_z_up), the IQM
** file convention the engine's IQM path expects (models_iqm.cpp LoadPosition swaps Y and Z on
** load). Units are left as the file's: the size is MODELDEF Scale's job.
**
** THE REST POSE is the scene as stored (the nodes' own Lcl values). The skin is bound elsewhere
** (each cluster's bind matrix); the mesh is moved into the rest pose with the file's own skin
** (sum of w * node_to_world * geometry_to_bone) and the bind is checked to be ONE transform for
** every joint there (Diag::rebaseMaxDev), which is what makes the rest-based palette
** world(frame) * world(rest)^-1 skin exactly as the FBX does.
**
** The full checklist (ufbx and engine file:line) is docs/FBX_LOADER.md.
**
**---------------------------------------------------------------------------
*/

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace FBXSkinned
{

// A transform as translation, rotation (quaternion x y z w) and scale, composed T * R * S --
// the IQM joint convention (models_iqm.cpp: translate, multQuaternion, scale).
struct TRSd
{
	double t[3] = { 0, 0, 0 };
	double q[4] = { 0, 0, 0, 1 };
	double s[3] = { 1, 1, 1 };
};

struct Joint
{
	std::string name;		// the FBX node's own name
	int parent = -1;		// index into joints, always < this joint's index; -1 = root
	TRSd rest;				// rest local TRS (a root: model space)
	double restWorld[16];	// rest model-space matrix, column-major (ufbx node_to_world)
	TRSd restWorldTRS;		// the same as a TRS (a mirrored joint: one negative scale axis)
};

struct Take
{
	std::string name;		// the anim stack's name, e.g. "Take 001"
	int firstFrame = 0;		// index of its first frame in Result::frames (in frames, not TRS entries)
	int numFrames = 0;
	double fps = 0;
	double startTime = 0;	// seconds; frame k is at startTime + k / fps
	double keyFirst = 0, keyLast = 0;	// the key span the frame count came from (seconds)
};

struct Vertex
{
	float pos[3];			// rest pose, model space (Z up)
	float nrm[3];
	float uv[2];			// v already flipped to texture rows (1 - v), as IQM stores it
	uint16_t bone[4];
	uint8_t weight[4];		// sums to 255
	uint32_t controlPoint;	// the FBX vertex (control point) this corner uses
	uint32_t fbxIndex;		// the FBX polygon-vertex index
};

// One image a material property links to, as the FBX stores it. The bytes are the file's own embedded copy,
// unchanged (an encoded PNG/JPG/...); empty when the FBX only names a file on the author's disk.
struct EmbeddedImage
{
	std::string file;				// file name only ("" = the property links no texture)
	std::string property;			// the material property it is linked to, e.g. "Maya|normalCamera"
	std::vector<uint8_t> content;	// the embedded bytes, byte for byte (ufbx_texture.content)
};

// The material the mesh is drawn with, as ufbx maps it to PBR (ufbx_material.pbr): every map it links and the
// scalar values beside them. With `mirrorskin` (below) it is the TWIN's material, not the mesh's own.
struct Material
{
	std::string name;				// the material's name
	std::string mesh;				// the mesh it belongs to (this mesh, or the mirrorskin twin)
	std::string ownName;			// this mesh's own material (differs from name under mirrorskin)
	EmbeddedImage baseColor;		// pbr.base_color (else fbx.diffuse_color)
	EmbeddedImage normal;			// pbr.normal_map (Maya aiStandardSurface: normalCamera)
	EmbeddedImage roughness;		// pbr.roughness (aiStandardSurface: specularRoughness)
	EmbeddedImage metalness;		// pbr.metalness
	EmbeddedImage occlusion;		// pbr.ambient_occlusion
	bool metalnessHasValue = false;	double metalnessValue = 0;		// the scalar the map would replace
	bool roughnessHasValue = false;	double roughnessValue = 0;
};

// MIRRORSKIN: "this mesh's UVs are the twin mesh's mirrored in u; draw it with the twin's material, the mirror
// undone". Asked for by name in the selector (see ParseSelector) -- never guessed -- and checked at load on every
// polygon corner; a mesh that is not such a twin is refused, not drawn with a wrong skin.
struct MirrorSkin
{
	std::string twin;				// "" = off
	double axis = 0;				// measured: u_this + u_twin on every corner (1 would be a mirror about 0.5)
	double uSpread = 0;				// max |u_this + u_twin - axis| over the corners
	double vSpread = 0;				// max |v_this - v_twin|
	double positionDiff = 0;		// max |control point this - twin| (geometry space; information)
	int faces = 0, corners = 0;
	int reversedFaces = 0;			// faces whose corners run the other way round than the twin's
};

// The part of a MODELDEF name after "<file>.fbx:", parsed: `<mesh>[?<option>[,<option>...]]`.
//   mirrorskin=<twin>   see MirrorSkin
struct Selector
{
	std::string mesh;
	std::string mirrorSkin;
};
bool ParseSelector(const char *text, Selector &out, std::string &err);

struct Diag
{
	std::vector<std::string> skinnedMeshes;	// every skinned mesh in the file, by node name
	double restDeterminant = 0;	// det of the mesh's rest transform (negative: mirrored)
	double rebaseMaxDev = 0;	// max |world_j(rest) * geometry_to_bone_j - same for joint 0|
	double signedVolume = 0;	// of the rest mesh in the file's polygon order
	bool reversedForOutward = false;	// the file's order was inside out at rest and was reversed
	double normalAgreeFile = 0;	// mean dot(the file's normal through the rest skin, the surface's own normal)
	bool normalsFromSurface = false;	// the file's normals did not face the surface; the surface's own are used
	int controlPoints = 0;
	int maxInfluences = 0;
	int verticesOver4 = 0;		// control points with more than 4 weights
	double maxDroppedWeight = 0;	// largest weight sum cut off one control point (before renormalising)
	double maxQuantError = 0;	// largest |byte/255 - exact| over every kept weight
	int unweightedControlPoints = 0;
	int blendShapeDeformers = 0;	// present in the file but not loaded (the engine has no morphs on this path)
	std::string textureFile;	// the drawn material's base colour texture, file name only ("" if none) = material.baseColor.file
	std::string materialName;	// = material.name
};

struct Result
{
	std::string meshName;		// node name of the chosen mesh
	std::vector<Joint> joints;
	std::vector<Take> takes;
	std::vector<TRSd> frames;	// [frame * joints.size() + joint], local (roots: model space)
	int numFrames = 0;			// frames.size() / joints.size(); at least 1 (the rest pose)
	std::vector<Vertex> verts;
	std::vector<uint32_t> tris;	// 3 per triangle, wound as the engine's IQMs are (see fbx_skinned.cpp)
	Material material;
	MirrorSkin mirror;
	Diag diag;
};

// meshSelector: the node name of the skinned mesh to load (case-insensitive), optionally followed by options
// (ParseSelector). Empty: the file must have exactly one skinned mesh. False with err set on any failure.
bool Load(const void *data, size_t size, const char *meshSelector, Result &out, std::string &err);

// Column-major 4x4 helpers shared with the test.
void ComposeTRS(const TRSd &t, double out[16]);
void MatMul(const double a[16], const double b[16], double out[16]);
bool MatInvert(const double m[16], double out[16]);

} // namespace FBXSkinned
