/*
** model_memorymesh.cpp
**
** A model built from geometry the caller already holds in memory.
** See model_memorymesh.h for what it is for and why it exists.
**
**---------------------------------------------------------------------------
** SPDX-License-Identifier: GPL-3.0-or-later
**---------------------------------------------------------------------------
*/

#include "model_memorymesh.h"
#include "modelrenderer.h"
#include "texturemanager.h"
#include "gametexture.h"
#include "printf.h"

#include <utility>

//==========================================================================
//
//
//
//==========================================================================

FMemoryMeshModel::FMemoryMeshModel(FMemoryMeshData &&data)
	: mData(std::move(data))
{
	hasSurfaces = true;
}

bool FMemoryMeshModel::Load(const char *, int, const char *, int)
{
	// Geometry came from the constructor. Nothing should ever get here, and a
	// silent success would hide the mistake.
	return false;
}

int FMemoryMeshModel::FindFrame(const char *, bool)
{
	return FErr_Singleframe;
}

FName FMemoryMeshModel::GetSurfaceName(int surface)
{
	if (surface < 0 || surface >= (int)mSurfaces.Size()) return NAME_None;
	// A surface here is "everything drawn with one texture", so the texture is
	// the only name it has.
	auto tex = TexMan.GetGameTexture(mSurfaces[surface].skin, false);
	return tex != nullptr ? FName(tex->GetName().GetChars()) : NAME_None;
}

//==========================================================================
//
// Faces grouped by texture, in first-appearance order.
//
// Done once and cached, because BuildVertexBuffer runs per renderer type and
// the grouping is what fixes the surface indices a caller's SurfaceSkin
// overrides address.
//
//==========================================================================

void FMemoryMeshModel::GroupSurfaces()
{
	if (mGrouped) return;
	mGrouped = true;

	for (auto &f : mData.faces)
	{
		if (f.count != 3 && f.count != 4) continue;
		unsigned s = 0;
		for (; s < mSurfaces.Size(); s++)
			if (mSurfaces[s].skin == f.skin) break;
		if (s == mSurfaces.Size())
		{
			Surface add;
			add.skin = f.skin;
			mSurfaces.Push(add);
		}
		// A quad is two triangles; doubleSided emits each one twice.
		unsigned tris = (f.count == 4) ? 2 : 1;
		if (mData.doubleSided) tris *= 2;
		mSurfaces[s].numTris += tris;
	}
}

//==========================================================================
//
//
//
//==========================================================================

void FMemoryMeshModel::BuildVertexBuffer(FModelRenderer *renderer)
{
	if (GetVertexBuffer(renderer->GetType())) return;

	GroupSurfaces();

	unsigned vbufsize = 0;
	for (auto &s : mSurfaces)
	{
		s.vbStart = vbufsize;
		vbufsize += s.numTris * 3;
	}

	// No index buffer, single frame: the same shape FOBJModel uses, which is
	// what DrawArrays below expects.
	auto vbuf = renderer->CreateVertexBuffer(false, true);
	SetVertexBuffer(renderer->GetType(), vbuf);

	FModelVertex *vertptr = vbuf->LockVertexBuffer(vbufsize == 0 ? 1 : vbufsize);
	if (vertptr == nullptr) return;

	// Written per surface, so the walk over faces is repeated per surface. The
	// meshes this exists for are tens to hundreds of faces; a smarter pass is
	// not worth the extra state.
	for (unsigned si = 0; si < mSurfaces.Size(); si++)
	{
		FModelVertex *out = vertptr + mSurfaces[si].vbStart;

		auto emitTri = [&](int a, int b, int c, const FVector2 &ua, const FVector2 &ub,
			const FVector2 &uc)
		{
			const FVector3 &va = mData.vertices[a];
			const FVector3 &vb = mData.vertices[b];
			const FVector3 &vc = mData.vertices[c];
			FVector3 n = (vb - va) ^ (vc - va);
			float len = n.Length();
			if (len > 0.f) n /= len;

			out[0].Set(va.X, va.Y, va.Z, ua.X, ua.Y);
			out[1].Set(vb.X, vb.Y, vb.Z, ub.X, ub.Y);
			out[2].Set(vc.X, vc.Y, vc.Z, uc.X, uc.Y);
			for (int k = 0; k < 3; k++) out[k].SetNormal(n.X, n.Y, n.Z);
			out += 3;
		};

		for (auto &f : mData.faces)
		{
			if (f.skin != mSurfaces[si].skin) continue;
			if (f.count != 3 && f.count != 4) continue;
			if (f.count == 3)
			{
				emitTri(f.vertex[0], f.vertex[1], f.vertex[2], f.uv[0], f.uv[1], f.uv[2]);
				if (mData.doubleSided)
					emitTri(f.vertex[0], f.vertex[2], f.vertex[1], f.uv[0], f.uv[2], f.uv[1]);
			}
			else
			{
				emitTri(f.vertex[0], f.vertex[1], f.vertex[2], f.uv[0], f.uv[1], f.uv[2]);
				emitTri(f.vertex[0], f.vertex[2], f.vertex[3], f.uv[0], f.uv[2], f.uv[3]);
				if (mData.doubleSided)
				{
					emitTri(f.vertex[0], f.vertex[2], f.vertex[1], f.uv[0], f.uv[2], f.uv[1]);
					emitTri(f.vertex[0], f.vertex[3], f.vertex[2], f.uv[0], f.uv[3], f.uv[2]);
				}
			}
		}
	}

	vbuf->UnlockVertexBuffer();
}

//==========================================================================
//
//
//
//==========================================================================

void FMemoryMeshModel::RenderFrame(FModelRenderer *renderer, FGameTexture *skin, int, int,
	double, FTranslationID translation, const FTextureID *surfaceskinids, int,
	const FModelSurfaceOverrideList *)
{
	for (unsigned i = 0; i < mSurfaces.Size(); i++)
	{
		FGameTexture *use = skin;
		if (use == nullptr)
		{
			if (surfaceskinids != nullptr && i < MD3_MAX_SURFACES && surfaceskinids[i].isValid())
				use = TexMan.GetGameTexture(surfaceskinids[i], true);
			else if (mSurfaces[i].skin.isValid())
				use = TexMan.GetGameTexture(mSurfaces[i].skin, true);
		}
		if (use == nullptr || mSurfaces[i].numTris == 0) continue;

		renderer->SetMaterial(use, false, translation);
		renderer->SetupFrame(this, mSurfaces[i].vbStart, mSurfaces[i].vbStart,
			mSurfaces[i].numTris * 3, -1);
		renderer->DrawArrays(0, mSurfaces[i].numTris * 3);
	}
}

//==========================================================================
//
//
//
//==========================================================================

void FMemoryMeshModel::AddSkins(uint8_t *hitlist, const FTextureID *surfaceskinids)
{
	GroupSurfaces();
	for (unsigned i = 0; i < mSurfaces.Size(); i++)
	{
		if (surfaceskinids != nullptr && i < MD3_MAX_SURFACES && surfaceskinids[i].isValid())
			hitlist[surfaceskinids[i].GetIndex()] |= FTextureManager::HIT_Flat;
		else if (mSurfaces[i].skin.isValid())
			hitlist[mSurfaces[i].skin.GetIndex()] |= FTextureManager::HIT_Flat;
	}
}

//==========================================================================
//
//
//
//==========================================================================

bool FMemoryMeshModel::GetLocalExtent(float *outMaxAbsX, float *outMaxAbsY, float *outMaxAbsZ)
{
	float mx = 0.f, my = 0.f, mz = 0.f;
	for (auto &v : mData.vertices)
	{
		float ax = v.X < 0.f ? -v.X : v.X;
		float ay = v.Y < 0.f ? -v.Y : v.Y;
		float az = v.Z < 0.f ? -v.Z : v.Z;
		if (ax > mx) mx = ax;
		if (ay > my) my = ay;
		if (az > mz) mz = az;
	}
	*outMaxAbsX = mx;
	*outMaxAbsY = my;
	*outMaxAbsZ = mz;
	return mData.vertices.Size() > 0;
}
