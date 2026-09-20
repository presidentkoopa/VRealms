/*
** model_md2.h
**
** MD2/DMD model format code
**
**---------------------------------------------------------------------------
**
** Copyright 2013-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#pragma once
#include "model.h"

#define MD2_MAGIC			0x32504449
#define DMD_MAGIC			0x4D444D44
#define MAX_LODS 4

// [XR] The axis swizzle every Quake-family loader writes vertices through.
// NOTE THE ORDER: VX=0, VZ=1, VY=2. Quake is Z-up and Doom's renderer is not,
// so the second and third components trade places on the way in. Shared with
// the MDL loader, which needs the identical mapping for identical data.
enum { VX, VZ, VY };

// [XR] Quake's 162 lighting normals, defined in models_md2.cpp. Shared with the
// MDL loader, which indexes the same table by the same convention.
#define NUMVERTEXNORMALS 162
extern float avertexnormals[NUMVERTEXNORMALS][3];

class FDMDModel : public FModel
{
protected:

	struct FTriangle
	{
		unsigned short           vertexIndices[3];
		unsigned short           textureIndices[3];
	};


	struct DMDHeader
	{
		int             magic;
		int             version;
		int             flags;
	};

	struct DMDModelVertex
	{
		float           xyz[3];
	};

	struct FTexCoord
	{
		short           s, t;
	};

	struct FGLCommandVertex
	{
		float           s, t;
		int             index;
	};

	struct DMDInfo
	{
		int             skinWidth;
		int             skinHeight;
		int             frameSize;
		int             numSkins;
		int             numVertices;
		int             numTexCoords;
		int             numFrames;
		int             numLODs;
		int             offsetSkins;
		int             offsetTexCoords;
		int             offsetFrames;
		int             offsetLODs;
		int             offsetEnd;
	};

	struct ModelFrame
	{
		char            name[16];
		unsigned int vindex;
	};

	struct ModelFrameVertexData
	{
		DMDModelVertex *vertices;
		DMDModelVertex *normals;
	};

	struct DMDLoDInfo
	{
		int             numTriangles;
		int             numGlCommands;
		int             offsetTriangles;
		int             offsetGlCommands;
	};

	struct DMDLoD
	{
		FTriangle		* triangles;
	};

	DMDHeader	    header;
	DMDInfo			info;
	FTextureID *	skins;
	ModelFrame  *	frames;
	bool			allowTexComp;  // Allow texture compression with this.

	// Temp data only needed for buffer construction
	FTexCoord *		texCoords;
	ModelFrameVertexData *framevtx;
	DMDLoDInfo		lodInfo[MAX_LODS];
	DMDLoD			lods[MAX_LODS];

	// Largest |X|/|Y|/|Z| across every frame's vertices, computed once in
	// LoadGeometry() while framevtx still exists -- UnloadGeometry() (called
	// from BuildVertexBuffer once the GPU buffer is built) deletes it for
	// good, same reason FMD3Model caches this instead of reading its own
	// per-frame vertex arrays on demand.
	float cachedMaxAbsX = 0.f, cachedMaxAbsY = 0.f, cachedMaxAbsZ = 0.f;
	bool hasCachedExtent = false;

public:
	FDMDModel()
	{
		frames = NULL;
		skins = NULL;
		for (int i = 0; i < MAX_LODS; i++)
		{
			lods[i].triangles = NULL;
		}
		info.numLODs = 0;
		texCoords = NULL;
		framevtx = NULL;
	}
	virtual ~FDMDModel();

	virtual bool Load(const char * fn, int lumpnum, const char * buffer, int length) override;
	virtual int FindFrame(const char* name, bool nodefault) override;
	virtual void RenderFrame(FModelRenderer *renderer, FGameTexture * skin, int frame, int frame2, double inter, FTranslationID translation, const FTextureID* surfaceskinids, int boneStartPosition, const FModelSurfaceOverrideList* surfov) override;
	virtual void LoadGeometry();
	virtual void LoadGeometry(FileSys::FileData* lumpData) override;
	virtual void AddSkins(uint8_t *hitlist, const FTextureID* surfaceskinids) override;

	// VIRTUAL, because what "no longer needed after building the vertex buffer"
	// means is the SUBCLASS'S to say. This base frees texCoords and the triangle
	// lists along with the frame vertices, which is right for MD2/DMD, where
	// LoadGeometry rebuilds all three. A format whose loader builds any of them
	// ONCE (FMDLModel does) must be able to keep them, or the second build after
	// a level reload finds a null pointer and faults inside the render pass.
	// Said once, not once a frame: see BuildVertexBuffer's geometry guard.
	bool reportedMissingGeometry = false;

	virtual void UnloadGeometry();
	void BuildVertexBuffer(FModelRenderer *renderer);
	bool GetLocalExtent(float* outMaxAbsX, float* outMaxAbsY, float* outMaxAbsZ) override;

};

// This uses the same internal representation as DMD
class FMD2Model : public FDMDModel
{
public:
	FMD2Model() {}
	virtual ~FMD2Model();

	virtual bool Load(const char * fn, int lumpnum, const char * buffer, int length);
	virtual void LoadGeometry();
	virtual void LoadGeometry(FileSys::FileData* lumpData) override;

};
