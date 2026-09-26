#pragma once
//
// A model whose geometry is handed to the engine in memory.
//
// Every other FModel subclass exists to parse a file: it is given a lump and
// decodes a published format out of it. This one is given the finished mesh.
// It is for code that HAS triangles already -- a loader that reads a foreign
// game's own data off the player's disk, a procedural generator, a mesh built
// from level geometry -- and therefore has nothing on disk for FindModel to
// open and no MODELDEF to name.
//
// FVoxelModel is the precedent: it is constructed from an in-memory FVoxel and
// its Load() is never called. This is the same lifecycle, generalised away from
// voxels: give it vertices, faces and a texture per face.
//
// Deliberately format-agnostic. It knows nothing about who built the mesh, and
// a second caller with a completely different source should need no change
// here. Register the finished object with RegisterModel() (model.h) to get an
// index, and bind that index to an actor class with AddSpriteModelFrame()
// (r_data/models.h).
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include "model.h"
#include "vectors.h"
#include "textureid.h"
#include "tarray.h"

// Give an already-built model an index, for geometry that never came from a
// file. Defined in model.cpp beside FindModel, which is the only other way in;
// Models takes ownership. Declared here rather than in model.h so that the
// general model header does not have to change for it.
unsigned RegisterModel(FModel *model, const char *pseudoName);

//==========================================================================
//
// One face. Three or four corners, its own texture, its own UVs.
//
// UVs are per CORNER rather than per vertex because a source format may well
// reuse one vertex in faces that map it differently -- Realms of the Haunting's
// prop meshes carry no UVs at all and imply the unit square from corner order,
// which only works per face.
//
//==========================================================================

struct FMemoryMeshFace
{
	int        vertex[4] = { 0, 0, 0, 0 };  // indices into FMemoryMeshData::vertices
	FVector2   uv[4];                       // one per corner, same order
	int        count = 0;                   // 3 or 4; anything else is dropped
	FTextureID skin = FNullTextureID();     // the texture this face is drawn with
};

//==========================================================================
//
// The mesh, in the engine's own model space -- the space the vertex buffer
// takes, which is NOT the world's. FSpriteModelFrame::ObjectToWorldMatrix
// (models.cpp) maps it with `translate(X, Z, Y)` and yaws about the vertical,
// so:
//
//     model x  ->  world X        model y  ->  world Z (UP)
//     model z  ->  world Y        and (x, z) turns counter-clockwise with Yaw
//
// A caller holding Doom-handed geometry (x east, y north, z up) therefore
// writes (x, z, y) -- the vertical goes in the MIDDLE.
//
//==========================================================================

struct FMemoryMeshData
{
	TArray<FVector3>        vertices;
	TArray<FMemoryMeshFace> faces;

	// Emit every face a second time wound the other way. Models are backface
	// culled (hw_models.cpp:57), so a source whose winding is not known, or
	// which genuinely contains open single-sided panels, would otherwise have
	// faces that vanish from one side. Off by default: a closed mesh with known
	// winding should not pay for it.
	bool doubleSided = false;
};

//==========================================================================
//
//
//
//==========================================================================

class FMemoryMeshModel : public FModel
{
public:
	explicit FMemoryMeshModel(FMemoryMeshData &&data);

	// Never called: this model has no file. Returns false so that anything
	// which does reach it fails loudly rather than drawing nothing.
	bool Load(const char *fn, int lumpnum, const char *buffer, int length) override;

	int FindFrame(const char *name, bool nodefault) override;
	int NumFrames() override { return 1; }

	// Frame numbers are ignored, as FVoxelModel ignores them. That is what lets
	// a caller bind this with modelframes[0] = -1, which is what the decoupled
	// animation path hands down.
	void RenderFrame(FModelRenderer *renderer, FGameTexture *skin, int frame, int frame2,
		double inter, FTranslationID translation, const FTextureID *surfaceskinids,
		int boneStartPosition, const FModelSurfaceOverrideList *surfov) override;

	void BuildVertexBuffer(FModelRenderer *renderer) override;
	void AddSkins(uint8_t *hitlist, const FTextureID *surfaceskinids) override;

	int   GetSurfaceCount() override { return (int)mSurfaces.Size(); }
	FName GetSurfaceName(int surface) override;
	bool  GetLocalExtent(float *outMaxAbsX, float *outMaxAbsY, float *outMaxAbsZ) override;

private:
	// One draw call per distinct texture, in first-appearance order so a caller
	// can predict which surface index a texture ended up as.
	struct Surface
	{
		FTextureID skin;
		unsigned   vbStart = 0;
		unsigned   numTris = 0;
	};

	FMemoryMeshData  mData;
	TArray<Surface>  mSurfaces;
	bool             mGrouped = false;

	void GroupSurfaces();
};
