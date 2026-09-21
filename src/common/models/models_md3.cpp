/*
** models_md3.cpp
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 2006-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include "filesystem.h"
#include "cmdlib.h"
#include "model_md3.h"
#include "texturemanager.h"
#include "modelrenderer.h"
#include "m_swap.h"

#define MAX_QPATH 64

#ifdef _MSC_VER
#pragma warning(disable:4244) // warning C4244: conversion from 'double' to 'float', possible loss of data
#endif

//===========================================================================
//
// decode the lat/lng normal to a 3 float normal
//
//===========================================================================

static void UnpackVector(unsigned short packed, float & nx, float & ny, float & nz)
{
	double lat = ( packed >> 8 ) & 0xff;
	double lng = ( packed & 0xff );
	lat *= M_PI/128;
	lng *= M_PI/128;

	nx = cos(lat) * sin(lng);
	ny = sin(lat) * sin(lng);
	nz = cos(lng);
}

//===========================================================================
//
// MD3 File structure
//
//===========================================================================

#pragma pack(4)
struct md3_header_t
{
	uint32_t Magic;
	uint32_t Version;
	char Name[MAX_QPATH];
	uint32_t Flags;
	uint32_t Num_Frames;
	uint32_t Num_Tags;
	uint32_t Num_Surfaces;
	uint32_t Num_Skins;
	uint32_t Ofs_Frames;
	uint32_t Ofs_Tags;
	uint32_t Ofs_Surfaces;
	uint32_t Ofs_Eof;
};

struct md3_surface_t
{
	uint32_t Magic;
	char Name[MAX_QPATH];
	uint32_t Flags;
	uint32_t Num_Frames;
	uint32_t Num_Shaders;
	uint32_t Num_Verts;
	uint32_t Num_Triangles;
	uint32_t Ofs_Triangles;
	uint32_t Ofs_Shaders;
	uint32_t Ofs_Texcoord;
	uint32_t Ofs_XYZNormal;
	uint32_t Ofs_End;
};

struct md3_triangle_t
{
	uint32_t vt_index[3];
};

struct md3_shader_t
{
	char Name[MAX_QPATH];
	uint32_t index;
};

struct md3_texcoord_t
{
	float s, t;
};

struct md3_vertex_t
{
	short x, y, z, n;
};

struct md3_frame_t
{
	float min_Bounds[3];
	float max_Bounds[3];
	float localorigin[3];
	float radius;
	char Name[16];
};
#pragma pack()


//===========================================================================
//
//
//
//===========================================================================

bool FMD3Model::Load(const char * path, int lumpnum, const char * buffer, int length)
{
	md3_header_t * hdr = (md3_header_t *)buffer;

	auto numFrames = LittleLong(hdr->Num_Frames);
	auto numSurfaces = LittleLong(hdr->Num_Surfaces);
	hasSurfaces = numSurfaces > 1;

	numTags = LittleLong(hdr->Num_Tags);

	md3_frame_t * frm = (md3_frame_t*)(buffer + LittleLong(hdr->Ofs_Frames));

	Frames.Resize(numFrames);
	for (unsigned i = 0; i < numFrames; i++)
	{
		strncpy(Frames[i].Name, frm[i].Name, 15);
		for (int j = 0; j < 3; j++) Frames[i].origin[j] = frm[i].localorigin[j];
	}

	md3_surface_t * surf = (md3_surface_t*)(buffer + LittleLong(hdr->Ofs_Surfaces));

	Surfaces.Resize(numSurfaces);

	for (unsigned i = 0; i < numSurfaces; i++)
	{
		MD3Surface * s = &Surfaces[i];
		md3_surface_t * ss = surf;

		surf = (md3_surface_t *)(((char*)surf) + LittleLong(surf->Ofs_End));

		// RS fork -- keep the surface's own name. Every other field of this
		// header was already being read and this one was skipped, so the part
		// names the artist authored ("slide", "m37a2_pump", "Magazine") lived
		// in the file and nowhere in memory. Copied through a buffer because
		// the MD3 field is not required to be null-terminated when the name
		// fills it.
		{
			char nbuf[sizeof(ss->Name) + 1];
			memcpy(nbuf, ss->Name, sizeof(ss->Name));
			nbuf[sizeof(ss->Name)] = 0;
			s->Name = nbuf;
		}

		s->numSkins = LittleLong(ss->Num_Shaders);
		s->numTriangles = LittleLong(ss->Num_Triangles);
		s->numVertices = LittleLong(ss->Num_Verts);

		// copy shaders (skins)
		md3_shader_t * shader = (md3_shader_t*)(((char*)ss) + LittleLong(ss->Ofs_Shaders));
		s->Skins.Resize(s->numSkins);

		for (unsigned ii = 0; ii < s->numSkins; ii++)
		{
			// [BB] According to the MD3 spec, Name is supposed to include the full path.
			// ... and since some tools seem to output backslashes, these need to be replaced with forward slashes to work.
			FixPathSeperator(shader[ii].Name);
			s->Skins[ii] = LoadSkin("", shader[ii].Name);
			// [BB] Fall back and check if Name is relative.
			if (!s->Skins[ii].isValid())
				s->Skins[ii] = LoadSkin(path, shader[ii].Name);
		}
	}
	mLumpNum = lumpnum;
	return true;
}

//===========================================================================
//
//
//
//===========================================================================

void FMD3Model::LoadGeometry()
{
	if (Surfaces.Size() > 0 && Surfaces[0].Vertices.Size() > 0)
	{
		return;
	}

	auto lumpdata = fileSystem.ReadFile(mLumpNum);
	LoadGeometry(&lumpdata);
}

void FMD3Model::LoadGeometry(FileSys::FileData* lumpData)
{
	auto buffer = lumpData->string();
	md3_header_t * hdr = (md3_header_t *)buffer;
	md3_surface_t * surf = (md3_surface_t*)(buffer + LittleLong(hdr->Ofs_Surfaces));

	for (unsigned i = 0; i < Surfaces.Size(); i++)
	{
		MD3Surface * s = &Surfaces[i];
		md3_surface_t * ss = surf;

		surf = (md3_surface_t *)(((char*)surf) + LittleLong(surf->Ofs_End));

		// copy triangle indices
		md3_triangle_t * tris = (md3_triangle_t*)(((char*)ss) + LittleLong(ss->Ofs_Triangles));
		s->Tris.Resize(s->numTriangles);

		for (unsigned ii = 0; ii < s->numTriangles; ii++) for (int j = 0; j < 3; j++)
		{
			s->Tris[ii].VertIndex[j] = LittleLong(tris[ii].vt_index[j]);
		}

		// Load texture coordinates
		md3_texcoord_t * tc = (md3_texcoord_t*)(((char*)ss) + LittleLong(ss->Ofs_Texcoord));
		s->Texcoords.Resize(s->numVertices);

		for (unsigned ii = 0; ii < s->numVertices; ii++)
		{
			s->Texcoords[ii].s = tc[ii].s;
			s->Texcoords[ii].t = tc[ii].t;
		}

		// Load vertices and texture coordinates
		md3_vertex_t * vt = (md3_vertex_t*)(((char*)ss) + LittleLong(ss->Ofs_XYZNormal));
		s->Vertices.Resize(s->numVertices * Frames.Size());

		for (unsigned ii = 0; ii < s->numVertices * Frames.Size(); ii++)
		{
			s->Vertices[ii].x = LittleShort(vt[ii].x) / 64.f;
			s->Vertices[ii].y = LittleShort(vt[ii].y) / 64.f;
			s->Vertices[ii].z = LittleShort(vt[ii].z) / 64.f;
			UnpackVector(LittleShort(vt[ii].n), s->Vertices[ii].nx, s->Vertices[ii].ny, s->Vertices[ii].nz);

			// See cachedMaxAbsX/Y/Z's declaration: this data is gone once
			// BuildVertexBuffer's surf->UnloadGeometry() runs, so the summary
			// has to be taken now, while it still exists. Largest per-axis
			// magnitude across every surface AND every frame -- conservative
			// (may slightly overstate the specific frame a holster shows),
			// same tradeoff FOBJModel::GetLocalExtent already makes.
			float ax = (s->Vertices[ii].x < 0.f) ? -s->Vertices[ii].x : s->Vertices[ii].x;
			float ay = (s->Vertices[ii].y < 0.f) ? -s->Vertices[ii].y : s->Vertices[ii].y;
			float az = (s->Vertices[ii].z < 0.f) ? -s->Vertices[ii].z : s->Vertices[ii].z;
			if (ax > cachedMaxAbsX) cachedMaxAbsX = ax;
			if (ay > cachedMaxAbsY) cachedMaxAbsY = ay;
			if (az > cachedMaxAbsZ) cachedMaxAbsZ = az;
			hasCachedExtent = true;
		}
	}
}

/**
 * Largest |X|/|Y|/|Z| across every vertex this model ever had, cached in
 * LoadGeometry() before BuildVertexBuffer() frees the raw data. See the
 * field declarations in model_md3.h for why this can't read Vertices
 * directly the way FOBJModel::GetLocalExtent does.
 */
bool FMD3Model::GetLocalExtent(float* outMaxAbsX, float* outMaxAbsY, float* outMaxAbsZ)
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

void FMD3Model::BuildVertexBuffer(FModelRenderer *renderer)
{
	if (!GetVertexBuffer(renderer->GetType()))
	{
		LoadGeometry();

		unsigned int vbufsize = 0;
		unsigned int ibufsize = 0;

		for (unsigned i = 0; i < Surfaces.Size(); i++)
		{
			MD3Surface * surf = &Surfaces[i];
			vbufsize += Frames.Size() * surf->numVertices;
			ibufsize += 3 * surf->numTriangles;
		}

		auto vbuf = renderer->CreateVertexBuffer(true, Frames.Size() == 1);
		SetVertexBuffer(renderer->GetType(), vbuf);

		FModelVertex *vertptr = vbuf->LockVertexBuffer(vbufsize);
		unsigned int *indxptr = vbuf->LockIndexBuffer(ibufsize);

		assert(vertptr != nullptr && indxptr != nullptr);

		unsigned int vindex = 0, iindex = 0;

		for (unsigned i = 0; i < Surfaces.Size(); i++)
		{
			MD3Surface * surf = &Surfaces[i];

			surf->vindex = vindex;
			surf->iindex = iindex;
			for (unsigned j = 0; j < Frames.Size() * surf->numVertices; j++)
			{
				MD3Vertex* vert = &surf->Vertices[j];

				FModelVertex *bvert = &vertptr[vindex++];

				int tc = j % surf->numVertices;
				bvert->Set(vert->x, vert->z, vert->y, surf->Texcoords[tc].s, surf->Texcoords[tc].t);
				bvert->SetNormal(vert->nx, vert->nz, vert->ny);
			}

			for (unsigned k = 0; k < surf->numTriangles; k++)
			{
				for (int l = 0; l < 3; l++)
				{
					indxptr[iindex++] = surf->Tris[k].VertIndex[l];
				}
			}
			surf->UnloadGeometry();
		}
		vbuf->UnlockVertexBuffer();
		vbuf->UnlockIndexBuffer();
	}
}


//===========================================================================
//
// for skin precaching
//
//===========================================================================

void FMD3Model::AddSkins(uint8_t *hitlist, const FTextureID* surfaceskinids)
{
	for (unsigned i = 0; i < Surfaces.Size(); i++)
	{
		if (surfaceskinids && surfaceskinids[i].isValid())
		{
			hitlist[surfaceskinids[i].GetIndex()] |= FTextureManager::HIT_Flat;
		}

		MD3Surface * surf = &Surfaces[i];
		for (unsigned j = 0; j < surf->numSkins; j++)
		{
			if (surf->Skins[j].isValid())
			{
				hitlist[surf->Skins[j].GetIndex()] |= FTextureManager::HIT_Flat;
			}
		}
	}
}

//===========================================================================
//
//
//
//===========================================================================

int FMD3Model::FindFrame(const char* name, bool nodefault)
{
	for (unsigned i = 0; i < Frames.Size(); i++)
	{
		if (!stricmp(name, Frames[i].Name)) return i;
	}
	return FErr_NotFound;
}

//===========================================================================
//
//
//
//===========================================================================

void FMD3Model::RenderFrame(FModelRenderer *renderer, FGameTexture * skin, int frameno, int frameno2, double inter, FTranslationID translation, const FTextureID* surfaceskinids, int boneStartPosition, const FModelSurfaceOverrideList* surfov)
{
	if ((unsigned)frameno >= Frames.Size() || (unsigned)frameno2 >= Frames.Size()) return;

	// RS FORK -- PER-SURFACE FRAME ADDRESSING (model.h, FModelSurfaceOverride).
	//
	// The loop below already worked out each surface's OWN place in the vertex
	// buffer -- `surf->vindex + frameno * surf->numVertices` -- because MD3
	// stores every surface's vertices separately for every frame. The only
	// thing shared was `frameno` itself. Letting it vary per surface is
	// therefore not new machinery: it is the same index arithmetic with a
	// different number in it, which is why a slide can now travel while the
	// frame it is mounted in stays still.
	//
	// The interpolation factor is renderer STATE rather than a draw argument,
	// so it is re-set only when a surface actually wants a different one and
	// restored for the next surface that does not. Without that bookkeeping
	// one part's blend would leak onto every part drawn after it.
	renderer->SetInterpolation(inter);
	float baseInter = (float)inter;
	float curInter  = baseInter;

	for (unsigned i = 0; i < Surfaces.Size(); i++)
	{
		MD3Surface * surf = &Surfaces[i];

		int sFrame = frameno, sFrameNext = frameno2;
		const FModelSurfaceOverride* ov = surfov ? surfov->Find((int)i) : nullptr;
		if (ov)
		{
			// Not drawn at all. This is what "the magazine is out of the gun"
			// is, and it costs one branch rather than a junk frame index
			// aimed at nothing.
			if (ov->hidden) continue;

			// Out of range is IGNORED rather than clamped: a silently clamped
			// frame is a wrong pose that looks deliberate, where falling back
			// to the caller's frame is at least the pose everything else is in.
			if (ov->frame >= 0 && (unsigned)ov->frame < Frames.Size())
			{
				sFrame     = ov->frame;
				sFrameNext = (ov->frameNext >= 0 && (unsigned)ov->frameNext < Frames.Size())
					? ov->frameNext : ov->frame;
			}

			float want = (ov->lerp >= 0.f) ? (ov->lerp > 1.f ? 1.f : ov->lerp) : baseInter;
			if (want != curInter) { renderer->SetInterpolation(want); curInter = want; }
		}
		else if (curInter != baseInter)
		{
			renderer->SetInterpolation(baseInter);
			curInter = baseInter;
		}

		// [BB] In case no skin is specified via MODELDEF, check if the MD3 has a skin for the current surface.
		// Note: Each surface may have a different skin.
		FGameTexture *surfaceSkin = skin;
		if (!surfaceSkin)
		{
			if (surfaceskinids && surfaceskinids[i].isValid())
			{
				surfaceSkin = TexMan.GetGameTexture(surfaceskinids[i], true);
			}
			else if (surf->numSkins > 0 && surf->Skins[0].isValid())
			{
				surfaceSkin = TexMan.GetGameTexture(surf->Skins[0], true);
			}

			if (!surfaceSkin)
			{
				continue;
			}
		}

		renderer->SetMaterial(surfaceSkin, false, translation);

		// RS FORK -- A LIVE TRANSFORM ON TOP OF THE FRAME.
		//
		// The frame above says which baked POSE this surface wears; this says
		// where that pose is. Every part driven by this system is rigid, so a
		// position is all that was ever missing -- see FModelSurfaceOverride
		// in model.h for why frame selection alone cannot put a part exactly
		// where a hand is, and why that is a class of bug rather than one.
		//
		// Only surfaces that asked pay for it, and the transform is undone
		// immediately after so it cannot leak onto the next surface -- the
		// same discipline the interpolation bookkeeping above follows, and
		// for the same reason.
		const bool transformed = (ov && ov->hasTransform);
		if (transformed)
		{
			VSMatrix local;
			local.loadIdentity();
			local.translate(ov->offset.X, ov->offset.Y, ov->offset.Z);

			// Identity quaternion is the common case (a part that slides and
			// does not turn), and skipping the rotate keeps it exact rather
			// than passing it through a conversion that need not happen.
			if (ov->rotation.X != 0.f || ov->rotation.Y != 0.f || ov->rotation.Z != 0.f || ov->rotation.W != 1.f)
			{
				local.multQuaternion(ov->rotation);
			}
			renderer->SetSurfaceTransform(&local);
		}

		// sFrame / sFrameNext, not frameno / frameno2 -- this is the whole of
		// per-surface addressing. Everything else above is bookkeeping.
		renderer->SetupFrame(this, surf->vindex + sFrame * surf->numVertices, surf->vindex + sFrameNext * surf->numVertices, surf->numVertices, -1);
		renderer->DrawElements(surf->numTriangles * 3, surf->iindex * sizeof(unsigned int));

		// RS FORK -- A BLENDED SURFACE: the same frame again toward its blend texture
		// (model.h FModelSurfaceOverride::blendSkin), before the transform is undone
		// so the second pass lands exactly on the first.
		if (ov && ov->blendAmount > 0.f && ov->blendSkin.isValid())
		{
			FGameTexture* blendTex = TexMan.GetGameTexture(ov->blendSkin, true);
			if (blendTex && blendTex->isValid())
			{
				renderer->BeginSurfaceBlend(blendTex, ov->blendAmount, translation);
				renderer->DrawElements(surf->numTriangles * 3, surf->iindex * sizeof(unsigned int));
				renderer->EndSurfaceBlend();
			}
		}

		if (transformed) renderer->SetSurfaceTransform(nullptr);
	}
	renderer->SetInterpolation(0.f);
}
