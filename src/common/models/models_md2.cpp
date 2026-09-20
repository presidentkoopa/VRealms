/*
** models_md2.cpp
**
** MD2/DMD model format code
**
**---------------------------------------------------------------------------
**
** Copyright 2005-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/


#include "filesystem.h"
#include "model_md2.h"
#include "texturemanager.h"
#include "modelrenderer.h"
#include "printf.h"
#include "m_swap.h"

#ifdef _MSC_VER
#pragma warning(disable:4244) // warning C4244: conversion from 'double' to 'float', possible loss of data
#endif


// [XR] NOT static any more: Quake 1 MDL indexes this same 162-entry table with
// the same meaning, so models_mdl.cpp shares it rather than carrying a second
// copy that could drift. Declared in model_md2.h.
float   avertexnormals[NUMVERTEXNORMALS][3] = {
#include "tab_anorms.h"
};

//===========================================================================
//
// UnpackVector
//  Packed: pppppppy yyyyyyyy. Yaw is on the XY plane.
//
//===========================================================================

static void UnpackVector(unsigned short packed, float vec[3])
{
	float   yaw = (packed & 511) / 512.0f * 2 * M_PI;
	float   pitch = ((packed >> 9) / 127.0f - 0.5f) * M_PI;
	float   cosp = (float) cos(pitch);

	vec[VX] = (float) cos(yaw) * cosp;
	vec[VY] = (float) sin(yaw) * cosp;
	vec[VZ] = (float) sin(pitch);
}


//===========================================================================
//
// DMD file structure
//
//===========================================================================

struct dmd_chunk_t
{
	int             type;
	int             length;		   // Next chunk follows...
};

#pragma pack(1)
struct dmd_packedVertex_t
{
	uint8_t         vertex[3];
	unsigned short  normal;		   // Yaw and pitch.
};

struct dmd_packedFrame_t
{
	float           scale[3];
	float           translate[3];
	char            name[16];
	dmd_packedVertex_t vertices[1];
};
#pragma pack()

// Chunk types.
enum
{
	DMC_END,					   // Must be the last chunk.
	DMC_INFO					   // Required; will be expected to exist.
};

//===========================================================================
//
// FDMDModel::Load
//
//===========================================================================

bool FDMDModel::Load(const char * path, int lumpnum, const char * buffer, int length)
{
	dmd_chunk_t * chunk = (dmd_chunk_t*)(buffer + 12);
	char   *temp;
	ModelFrame *frame;
	int     i;

	int fileoffset = 12 + sizeof(dmd_chunk_t);

	chunk->type = LittleLong(chunk->type);
	while (chunk->type != DMC_END)
	{
		switch (chunk->type)
		{
		case DMC_INFO:			// Standard DMD information chunk.
			memcpy(&info, buffer + fileoffset, LittleLong(chunk->length));
			info.skinWidth = LittleLong(info.skinWidth);
			info.skinHeight = LittleLong(info.skinHeight);
			info.frameSize = LittleLong(info.frameSize);
			info.numSkins = LittleLong(info.numSkins);
			info.numVertices = LittleLong(info.numVertices);
			info.numTexCoords = LittleLong(info.numTexCoords);
			info.numFrames = LittleLong(info.numFrames);
			info.numLODs = LittleLong(info.numLODs);
			info.offsetSkins = LittleLong(info.offsetSkins);
			info.offsetTexCoords = LittleLong(info.offsetTexCoords);
			info.offsetFrames = LittleLong(info.offsetFrames);
			info.offsetLODs = LittleLong(info.offsetLODs);
			info.offsetEnd = LittleLong(info.offsetEnd);
			fileoffset += chunk->length;
			break;

		default:
			// Just skip all unknown chunks.
			fileoffset += chunk->length;
			break;
		}
		// Read the next chunk header.
		chunk = (dmd_chunk_t*)(buffer + fileoffset);
		chunk->type = LittleLong(chunk->type);
		fileoffset += sizeof(dmd_chunk_t);
	}

	// Allocate and load in the data.
	skins = new FTextureID[info.numSkins];

	for (i = 0; i < info.numSkins; i++)
	{
		skins[i] = LoadSkin(path, buffer + info.offsetSkins + i * 64);
	}
	temp = (char*)buffer + info.offsetFrames;
	frames = new ModelFrame[info.numFrames];

	for (i = 0, frame = frames; i < info.numFrames; i++, frame++)
	{
		dmd_packedFrame_t *pfr = (dmd_packedFrame_t *)(temp + info.frameSize * i);

		memcpy(frame->name, pfr->name, sizeof(pfr->name));
		frame->vindex = UINT_MAX;
	}
	mLumpNum = lumpnum;
	return true;
}

//===========================================================================
//
// FDMDModel::LoadGeometry
//
//===========================================================================

void FDMDModel::LoadGeometry()
{
	if (framevtx != NULL)
	{
		return;
	}

	auto lumpdata = fileSystem.ReadFile(mLumpNum);
	LoadGeometry(&lumpdata);
}

void FDMDModel::LoadGeometry(FileSys::FileData* lumpData)
{
	static int axis[3] = { VX, VY, VZ };
	auto buffer = lumpData->string();
	texCoords = new FTexCoord[info.numTexCoords];
	memcpy(texCoords, buffer + info.offsetTexCoords, info.numTexCoords * sizeof(FTexCoord));

	const char *temp = buffer + info.offsetFrames;
	framevtx= new ModelFrameVertexData[info.numFrames];

	ModelFrameVertexData *framev;
	int i, k, c;
	for(i = 0, framev = framevtx; i < info.numFrames; i++, framev++)
	{
		dmd_packedFrame_t *pfr = (dmd_packedFrame_t *) (temp + info.frameSize * i);
		dmd_packedVertex_t *pVtx;

		framev->vertices = new DMDModelVertex[info.numVertices];
		framev->normals = new DMDModelVertex[info.numVertices];

		// Translate each vertex.
		for(k = 0, pVtx = pfr->vertices; k < info.numVertices; k++, pVtx++)
		{
			UnpackVector((unsigned short)(pVtx->normal), framev->normals[k].xyz);
			for(c = 0; c < 3; c++)
			{
				framev->vertices[k].xyz[axis[c]] =
					(pVtx->vertex[c] * float(pfr->scale[c]) + float(pfr->translate[c]));
			}

			// See cachedMaxAbsX/Y/Z's declaration in model_md2.h: framevtx
			// is deleted by UnloadGeometry() once BuildVertexBuffer has
			// uploaded it, so this summary has to be taken now. Largest
			// per-axis magnitude across every frame -- same conservative
			// tradeoff FMD3Model/FOBJModel's GetLocalExtent already make.
			for (c = 0; c < 3; c++)
			{
				float a = framev->vertices[k].xyz[c];
				if (a < 0.f) a = -a;
				if (c == 0 && a > cachedMaxAbsX) cachedMaxAbsX = a;
				else if (c == 1 && a > cachedMaxAbsY) cachedMaxAbsY = a;
				else if (c == 2 && a > cachedMaxAbsZ) cachedMaxAbsZ = a;
			}
			hasCachedExtent = true;
		}
	}

	memcpy(lodInfo, buffer+info.offsetLODs, info.numLODs * sizeof(DMDLoDInfo));
	for(i = 0; i < info.numLODs; i++)
	{
		lodInfo[i].numTriangles = LittleLong(lodInfo[i].numTriangles);
		lodInfo[i].offsetTriangles = LittleLong(lodInfo[i].offsetTriangles);
		if (lodInfo[i].numTriangles > 0)
		{
			lods[i].triangles = new FTriangle[lodInfo[i].numTriangles];
			memcpy(lods[i].triangles, buffer + lodInfo[i].offsetTriangles, lodInfo[i].numTriangles * sizeof(FTriangle));
			for (int j = 0; j < lodInfo[i].numTriangles; j++)
			{
				for (int kk = 0; kk < 3; kk++)
				{
					lods[i].triangles[j].textureIndices[kk] = LittleShort(lods[i].triangles[j].textureIndices[kk]);
					lods[i].triangles[j].vertexIndices[kk] = LittleShort(lods[i].triangles[j].vertexIndices[kk]);
				}
			}
		}
	}

}

//===========================================================================
//
// Deletes everything that's no longer needed after building the vertex buffer
//
//===========================================================================

void FDMDModel::UnloadGeometry()
{
	int i;

	if (framevtx != NULL)
	{
		for (i=0;i<info.numFrames;i++)
		{
			if (framevtx[i].vertices != NULL) delete [] framevtx[i].vertices;
			if (framevtx[i].normals != NULL) delete [] framevtx[i].normals;

			framevtx[i].vertices = NULL;
			framevtx[i].normals = NULL;
		}
		delete[] framevtx;
		framevtx = NULL;
	}

	for(i = 0; i < info.numLODs; i++)
	{
		if (lods[i].triangles != NULL) delete[] lods[i].triangles;
		lods[i].triangles = NULL;
	}

	if (texCoords != NULL) delete[] texCoords;
	texCoords = NULL;
}

/**
 * Largest |X|/|Y|/|Z| across every vertex this model ever had, cached in
 * LoadGeometry() before UnloadGeometry() frees framevtx. See the field
 * declarations in model_md2.h.
 */
bool FDMDModel::GetLocalExtent(float* outMaxAbsX, float* outMaxAbsY, float* outMaxAbsZ)
{
	*outMaxAbsX = cachedMaxAbsX;
	*outMaxAbsY = cachedMaxAbsY;
	*outMaxAbsZ = cachedMaxAbsZ;
	return hasCachedExtent;
}

//===========================================================================
//
//
//
//===========================================================================

FDMDModel::~FDMDModel()
{
	UnloadGeometry();

	// skins are managed by the texture manager so they must not be deleted here.
	if (skins != NULL) delete [] skins;
	if (frames != NULL) delete [] frames;
}

//===========================================================================
//
//
//
//===========================================================================

void FDMDModel::BuildVertexBuffer(FModelRenderer *renderer)
{
	if (!GetVertexBuffer(renderer->GetType()))
	{
		LoadGeometry();

		// ====================================================================
		// THE GEOMETRY MUST ACTUALLY BE HERE.
		//
		// This loop walks four separate heap arrays by index and dereferences
		// every one of them without ever asking whether they exist. When one of
		// them is null the result is an access violation deep inside the render
		// pass -- which is exactly how the respawn crash presented: the engine
		// died at `texCoords[ti]` reading address 6, the crash dialog opened
		// behind the VR view, and from the outside the game had simply frozen.
		//
		// LoadGeometry() returns EARLY when framevtx is already set, on the
		// assumption that everything else it allocates is set too. UnloadGeometry()
		// frees all of them together, so that assumption holds for one caller and
		// says nothing at all about two. Rather than guess at which caller, this
		// refuses to draw and SAYS WHICH POINTER AND WHICH MODEL -- a named model
		// in a report beats a faulting address in a dialog nobody can read.
		//
		// Refusing costs one model one frame. The crash cost the session.
		// ====================================================================
		const char *missing = nullptr;
		if (framevtx == nullptr)            missing = "framevtx (the vertex frames)";
		else if (texCoords == nullptr)      missing = "texCoords (the UVs)";
		else if (lods[0].triangles == nullptr) missing = "lods[0].triangles";
		else
		{
			for (int i = 0; i < info.numFrames && missing == nullptr; i++)
			{
				if (framevtx[i].vertices == nullptr) missing = "framevtx[].vertices";
				else if (framevtx[i].normals == nullptr) missing = "framevtx[].normals";
			}
		}
		if (missing != nullptr)
		{
			// ONCE PER MODEL. A refused model is retried every single frame, and the
			// first run of this guard wrote ten megabytes of the same four lines --
			// which buries the one thing the report exists to say.
			if (reportedMissingGeometry)
			{
				return;
			}
			reportedMissingGeometry = true;

			// fopen, not Printf: this fires in the render pass, and if it turns out
			// to be fatal after all, buffered console output dies with the process.
			if (FILE *f = fopen("model_fault.txt", "at"))
			{
				fprintf(f, "FDMDModel::BuildVertexBuffer refused to draw -- %s is null\n"
				           "  model: %s  (lump %d, %d frames, %d LODs)\n"
				           "  This would have been an access violation inside the render pass.\n",
					missing, mFileName.GetChars(), mLumpNum, info.numFrames, info.numLODs);
				fclose(f);
			}
			Printf(TEXTCOLOR_RED "model %s: geometry missing (%s) -- not drawn this frame\n",
				mFileName.GetChars(), missing);
			return;
		}

		int VertexBufferSize = info.numFrames * lodInfo[0].numTriangles * 3;
		unsigned int vindex = 0;

		auto vbuf = renderer->CreateVertexBuffer(false, info.numFrames == 1);
		SetVertexBuffer(renderer->GetType(), vbuf);

		FModelVertex *vertptr = vbuf->LockVertexBuffer(VertexBufferSize);

		for (int i = 0; i < info.numFrames; i++)
		{
			DMDModelVertex *vert = framevtx[i].vertices;
			DMDModelVertex *norm = framevtx[i].normals;

			frames[i].vindex = vindex;

			FTriangle *tri = lods[0].triangles;

			for (int ii = 0; ii < lodInfo[0].numTriangles; ii++)
			{
				for (int j = 0; j < 3; j++)
				{

					int ti = tri->textureIndices[j];
					int vi = tri->vertexIndices[j];

					FModelVertex *bvert = &vertptr[vindex++];
					bvert->Set(vert[vi].xyz[0], vert[vi].xyz[1], vert[vi].xyz[2], (float)texCoords[ti].s / info.skinWidth, (float)texCoords[ti].t / info.skinHeight);
					bvert->SetNormal(norm[vi].xyz[0], norm[vi].xyz[1], norm[vi].xyz[2]);
				}
				tri++;
			}
		}
		vbuf->UnlockVertexBuffer();
		UnloadGeometry();
	}
}

//===========================================================================
//
// for skin precaching
//
//===========================================================================

void FDMDModel::AddSkins(uint8_t *hitlist, const FTextureID*)
{
	for (int i = 0; i < info.numSkins; i++)
	{
		if (skins[i].isValid())
		{
			hitlist[skins[i].GetIndex()] |= FTextureManager::HIT_Flat;
		}
	}
}

//===========================================================================
//
// FDMDModel::FindFrame
//
//===========================================================================
int FDMDModel::FindFrame(const char* name, bool nodefault)
{
	for (int i=0;i<info.numFrames;i++)
	{
		if (!stricmp(name, frames[i].name)) return i;
	}
	return FErr_NotFound;
}

//===========================================================================
//
//
//
//===========================================================================

void FDMDModel::RenderFrame(FModelRenderer *renderer, FGameTexture * skin, int frameno, int frameno2, double inter, FTranslationID translation, const FTextureID*, int boneStartPosition, const FModelSurfaceOverrideList* surfov)
{
	if (frameno >= info.numFrames || frameno2 >= info.numFrames) return;

	if (!skin)
	{
		if (info.numSkins == 0 || !skins[0].isValid()) return;
		skin = TexMan.GetGameTexture(skins[0], true);
		if (!skin) return;
	}

	renderer->SetInterpolation(inter);
	renderer->SetMaterial(skin, false, translation);
	renderer->SetupFrame(this, frames[frameno].vindex, frames[frameno2].vindex, lodInfo[0].numTriangles * 3, -1);
	renderer->DrawArrays(0, lodInfo[0].numTriangles * 3);
	renderer->SetInterpolation(0.f);
}

//===========================================================================
//
// Internal data structures of MD2 files - only used during loading
//
//===========================================================================

struct md2_header_t
{
	int             magic;
	int             version;
	int             skinWidth;
	int             skinHeight;
	int             frameSize;
	int             numSkins;
	int             numVertices;
	int             numTexCoords;
	int             numTriangles;
	int             numGlCommands;
	int             numFrames;
	int             offsetSkins;
	int             offsetTexCoords;
	int             offsetTriangles;
	int             offsetFrames;
	int             offsetGlCommands;
	int             offsetEnd;
};

struct md2_triangleVertex_t
{
	uint8_t            vertex[3];
	uint8_t            lightNormalIndex;
};

struct md2_packedFrame_t
{
	float           scale[3];
	float           translate[3];
	char            name[16];
	md2_triangleVertex_t vertices[1];
};

//===========================================================================
//
// FMD2Model::Load
//
//===========================================================================

bool FMD2Model::Load(const char * path, int lumpnum, const char * buffer, int length)
{
	md2_header_t * md2header = (md2_header_t *)buffer;
	ModelFrame *frame;
	uint8_t   *md2_frames;
	int     i;

	// Convert it to DMD.
	header.magic = MD2_MAGIC;
	header.version = 8;
	header.flags = 0;
	info.skinWidth = LittleLong(md2header->skinWidth);
	info.skinHeight = LittleLong(md2header->skinHeight);
	info.frameSize = LittleLong(md2header->frameSize);
	info.numLODs = 1;
	info.numSkins = LittleLong(md2header->numSkins);
	info.numTexCoords = LittleLong(md2header->numTexCoords);
	info.numVertices = LittleLong(md2header->numVertices);
	info.numFrames = LittleLong(md2header->numFrames);
	info.offsetSkins = LittleLong(md2header->offsetSkins);
	info.offsetTexCoords = LittleLong(md2header->offsetTexCoords);
	info.offsetFrames = LittleLong(md2header->offsetFrames);
	info.offsetLODs = LittleLong(md2header->offsetEnd);	// Doesn't exist.
	lodInfo[0].numTriangles = LittleLong(md2header->numTriangles);
	lodInfo[0].numGlCommands = LittleLong(md2header->numGlCommands);
	lodInfo[0].offsetTriangles = LittleLong(md2header->offsetTriangles);
	lodInfo[0].offsetGlCommands = LittleLong(md2header->offsetGlCommands);
	info.offsetEnd = LittleLong(md2header->offsetEnd);

	if (info.offsetFrames + info.frameSize * info.numFrames > length)
	{
		Printf("LoadModel: Model '%s' file too short\n", path);
		return false;
	}
	if (lodInfo[0].numGlCommands <= 0)
	{
		Printf("LoadModel: Model '%s' invalid NumGLCommands\n", path);
		return false;
	}

	skins = new FTextureID[info.numSkins];

	for (i = 0; i < info.numSkins; i++)
	{
		skins[i] = LoadSkin(path, buffer + info.offsetSkins + i * 64);
	}

	// The frames need to be unpacked.
	md2_frames = (uint8_t*)buffer + info.offsetFrames;
	frames = new ModelFrame[info.numFrames];

	for (i = 0, frame = frames; i < info.numFrames; i++, frame++)
	{
		md2_packedFrame_t *pfr = (md2_packedFrame_t *)(md2_frames + info.frameSize * i);

		memcpy(frame->name, pfr->name, sizeof(pfr->name));
		frame->vindex = UINT_MAX;
	}
	mLumpNum = lumpnum;
	return true;
}

//===========================================================================
//
// FMD2Model::LoadGeometry
//
//===========================================================================

void FMD2Model::LoadGeometry()
{
	if (framevtx != NULL)
	{
		return;
	}

	auto lumpdata = fileSystem.ReadFile(mLumpNum);
	LoadGeometry(&lumpdata);
}

void FMD2Model::LoadGeometry(FileSys::FileData* lumpData)
{
	static int axis[3] = { VX, VY, VZ };
	uint8_t   *md2_frames;
	auto buffer = lumpData->string();

	texCoords = new FTexCoord[info.numTexCoords];
	memcpy(texCoords, (uint8_t*)buffer + info.offsetTexCoords, info.numTexCoords * sizeof(FTexCoord));

	md2_frames = (uint8_t*)buffer + info.offsetFrames;
	framevtx = new ModelFrameVertexData[info.numFrames];
	ModelFrameVertexData *framev;
	int i, k, c;

	for(i = 0, framev = framevtx; i < info.numFrames; i++, framev++)
	{
		md2_packedFrame_t *pfr = (md2_packedFrame_t *) (md2_frames + info.frameSize * i);
		md2_triangleVertex_t *pVtx;

		framev->vertices = new DMDModelVertex[info.numVertices];
		framev->normals = new DMDModelVertex[info.numVertices];

		// Translate each vertex.
		for(k = 0, pVtx = pfr->vertices; k < info.numVertices; k++, pVtx++)
		{
			memcpy(framev->normals[k].xyz,
				avertexnormals[pVtx->lightNormalIndex], sizeof(float) * 3);

			for(c = 0; c < 3; c++)
			{
				framev->vertices[k].xyz[axis[c]] =
					(pVtx->vertex[c] * pfr->scale[c] + pfr->translate[c]);
			}

			// FMD2Model has its own LoadGeometry, separate from
			// FDMDModel::LoadGeometry (the DMD-format one) -- this cache
			// has to be filled here too, not just there, or an actual
			// .md2 file (as opposed to a .dmd one) never gets one. See
			// cachedMaxAbsX/Y/Z's declaration in model_md2.h.
			for (c = 0; c < 3; c++)
			{
				float a = framev->vertices[k].xyz[c];
				if (a < 0.f) a = -a;
				if (c == 0 && a > cachedMaxAbsX) cachedMaxAbsX = a;
				else if (c == 1 && a > cachedMaxAbsY) cachedMaxAbsY = a;
				else if (c == 2 && a > cachedMaxAbsZ) cachedMaxAbsZ = a;
			}
			hasCachedExtent = true;
		}
	}

	lods[0].triangles = new FTriangle[lodInfo[0].numTriangles];

	int cnt = lodInfo[0].numTriangles;
	memcpy(lods[0].triangles, buffer + lodInfo[0].offsetTriangles, sizeof(FTriangle) * cnt);
	for (int j = 0; j < cnt; j++)
	{
		for (int kk = 0; kk < 3; kk++)
		{
			lods[0].triangles[j].textureIndices[kk] = LittleShort(lods[0].triangles[j].textureIndices[kk]);
			lods[0].triangles[j].vertexIndices[kk] = LittleShort(lods[0].triangles[j].vertexIndices[kk]);
		}
	}
}

FMD2Model::~FMD2Model()
{
}
