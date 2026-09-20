/*
** models_mdl.cpp
**
** Quake 1 alias model (.mdl) support.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** See model_mdl.h for why this derives from FDMDModel and what MDL does that
** MD2 does not. The short version: same packed-vertex animation and the same
** avertexnormals table, but the skin is embedded in Quake's own palette and
** seam vertices need two texcoords where DMD allows one.
**
*/

#include "filesystem.h"
#include "model_mdl.h"
#include "texturemanager.h"
#include "modelrenderer.h"
#include "printf.h"
#include "m_swap.h"
#include "image.h"
#include "imagehelpers.h"
#include "bitmap.h"

//===========================================================================
//
// QUAKE'S PALETTE, EMBEDDED.
//
// An MDL skin is 8-bit indexed and the indices mean nothing without this. It is
// not Doom's palette and it is not derivable from anything already in the tree,
// so it is written out: 256 RGB triples, the standard Quake 1 palette from
// gfx/palette.lmp.
//
// The last 32 entries are the fullbright range. Quake treats 224..255 as
// self-lit, which is why lava and screens glow in that engine. Nothing here
// acts on that -- the skin is converted to plain truecolour and lit like any
// other model skin -- but the range is noted because a model whose skin uses it
// will look flat compared to how it looks in Quake, and that is the reason.
//
//===========================================================================

static const uint8_t QuakePalette[768] = {
	  0,  0,  0,  15, 15, 15,  31, 31, 31,  47, 47, 47,  63, 63, 63,  75, 75, 75,
	 91, 91, 91, 107,107,107, 123,123,123, 139,139,139, 155,155,155, 171,171,171,
	187,187,187, 203,203,203, 219,219,219, 235,235,235,  15, 11,  7,  23, 15, 11,
	 31, 23, 11,  39, 27, 15,  47, 35, 19,  55, 43, 23,  63, 47, 23,  75, 55, 27,
	 83, 59, 27,  91, 67, 31,  99, 75, 31, 107, 83, 31, 115, 87, 31, 123, 95, 35,
	131,103, 35, 143,111, 35,  11, 11, 15,  19, 19, 27,  27, 27, 39,  39, 39, 51,
	 47, 47, 63,  55, 55, 75,  63, 63, 87,  71, 71,103,  79, 79,115,  91, 91,127,
	 99, 99,139, 107,107,151, 115,115,163, 123,123,175, 131,131,187, 139,139,203,
	  0,  0,  0,   7,  7,  0,  11, 11,  0,  19, 19,  0,  27, 27,  0,  35, 35,  0,
	 43, 43,  7,  47, 47,  7,  55, 55,  7,  63, 63,  7,  71, 71,  7,  75, 75, 11,
	 83, 83, 11,  91, 91, 11,  99, 99, 11, 107,107, 15,   7,  0,  0,  15,  0,  0,
	 23,  0,  0,  31,  0,  0,  39,  0,  0,  47,  0,  0,  55,  0,  0,  63,  0,  0,
	 71,  0,  0,  79,  0,  0,  87,  0,  0,  95,  0,  0, 103,  0,  0, 111,  0,  0,
	119,  0,  0, 127,  0,  0,  19, 19,  0,  27, 27,  0,  35, 35,  0,  47, 43,  0,
	 55, 47,  0,  67, 55,  0,  75, 59,  7,  87, 67,  7,  95, 71,  7, 107, 75, 11,
	119, 83, 15, 131, 87, 19, 139, 91, 19, 151, 95, 27, 163, 99, 31, 175,103, 35,
	 35, 19,  7,  47, 23, 11,  59, 31, 15,  75, 35, 19,  87, 43, 23,  99, 47, 31,
	115, 55, 35, 127, 59, 43, 143, 67, 51, 159, 79, 51, 175, 99, 47, 191,119, 47,
	207,143, 43, 223,171, 39, 239,203, 31, 255,243, 27,  11,  7,  0,  27, 19,  0,
	 43, 35, 15,  55, 43, 19,  71, 51, 27,  83, 55, 35,  99, 63, 43, 111, 71, 51,
	127, 83, 63, 139, 95, 71, 155,107, 83, 167,123, 95, 183,135,107, 195,147,123,
	211,163,139, 227,179,151, 171,139,163, 159,127,151, 147,115,135, 139,103,123,
	127, 91,111, 119, 83, 99, 107, 75, 87,  95, 63, 75,  87, 55, 67,  75, 47, 55,
	 67, 39, 47,  55, 31, 35,  43, 23, 27,  35, 19, 19,  23, 11, 11,  15,  7,  7,
	187,115,159, 175,107,143, 163, 95,131, 151, 87,119, 139, 79,107, 127, 75, 95,
	115, 67, 83, 107, 59, 75,  95, 51, 63,  83, 43, 55,  71, 35, 43,  59, 31, 35,
	 47, 23, 27,  35, 19, 19,  23, 11, 11,  15,  7,  7, 219,195,187, 203,179,167,
	191,163,155, 175,151,139, 163,135,123, 151,123,111, 135,111, 95, 123, 99, 83,
	107, 87, 71,  95, 75, 59,  83, 63, 51,  67, 51, 39,  55, 43, 31,  39, 31, 23,
	 27, 19, 15,  15, 11,  7, 111,131,123, 103,123,111,  95,115,103,  87,107, 95,
	 79, 99, 87,  71, 91, 79,  63, 83, 71,  55, 75, 63,  47, 67, 55,  43, 59, 47,
	 35, 51, 39,  31, 43, 31,  23, 35, 23,  15, 27, 19,  11, 19, 11,   7, 11,  7,
	255,243, 27, 239,223, 23, 219,203, 19, 203,183, 15, 187,167, 15, 171,151, 11,
	155,131,  7, 139,115,  7, 123, 99,  7, 107, 83,  0,  91, 71,  0,  75, 55,  0,
	 59, 43,  0,  43, 31,  0,  27, 15,  0,  11,  7,  0,   0,  0,255,  11, 11,239,
	 19, 19,223,  27, 27,207,  35, 35,191,  43, 43,175,  47, 47,159,  47, 47,143,
	 47, 47,127,  47, 47,111,  47, 47, 95,  43, 43, 79,  35, 35, 63,  27, 27, 47,
	 19, 19, 31,  11, 11, 15,  43,  0,  0,  59,  0,  0,  75,  7,  0,  95,  7,  0,
	111, 15,  0, 127, 23,  7, 147, 31,  7, 163, 39, 11, 183, 51, 15, 195, 75, 27,
	207, 99, 43, 219,127, 59, 227,151, 79, 231,171, 95, 239,191,119, 247,211,139,
	167,123, 59, 183,155, 55, 199,195, 55, 231,227, 87, 127,191,255, 171,231,255,
	215,255,255, 103,  0,  0, 139,  0,  0, 179,  0,  0, 215,  0,  0, 255,  0,  0,
	255,243,147, 255,247,199, 255,255,255, 159, 91, 83
};

//===========================================================================
//
// The embedded skin as an image source.
//
// Truecolour out, because the indices are Quake's and mean nothing to Doom's
// palette. CreatePalettedPixels maps through the game palette so the software
// and paletted paths still get something sensible rather than garbage.
//
//===========================================================================

class FMDLSkinTexture : public FImageSource
{
public:
	FMDLSkinTexture(const uint8_t *pixels, int w, int h)
	{
		Width = w;
		Height = h;
		bUseGamePalette = false;
		mPixels.Resize(w * h);
		memcpy(mPixels.Data(), pixels, w * h);
	}

	int CopyPixels(FBitmap *bmp, int conversion, int frame = 0) override
	{
		TArray<uint32_t> rgba(Width * Height, true);
		for (int i = 0; i < Width * Height; i++)
		{
			const uint8_t *c = &QuakePalette[mPixels[i] * 3];
			rgba[i] = MAKEARGB(255, c[0], c[1], c[2]);
		}
		// CF_BGRA, NOT CF_RGBA. MAKEARGB builds a uint32_t, and on a little-endian
		// machine that lands in memory as B,G,R,A -- so the bytes handed over here
		// are BGRA even though the variable is called rgba. CF_RGBA reads byte 0
		// as red, which swaps red and blue: Quake's browns and oranges come out
		// as blues, and its dark tones go to black. It renders, it just renders
		// the wrong colour, so nothing anywhere reports a problem.
		//
		// This is the engine's own convention, not a guess -- FBitmap::Blit
		// (bitmap.h) and the font sheet path (font.cpp) both pass CF_BGRA for
		// four-byte-per-pixel data.
		bmp->CopyPixelDataRGB((int)0, (int)0, (const uint8_t*)rgba.Data(), Width, Height,
			4, Width * 4, 0, CF_BGRA);
		return 0;
	}

	PalettedPixels CreatePalettedPixels(int conversion, int frame = 0) override
	{
		PalettedPixels pixels(Width * Height);
		// Column-major, which is what the paletted path expects.
		for (int x = 0; x < Width; x++)
		{
			for (int y = 0; y < Height; y++)
			{
				const uint8_t *c = &QuakePalette[mPixels[y * Width + x] * 3];
				pixels[x * Height + y] = ImageHelpers::RGBToPalette(false, c[0], c[1], c[2]);
			}
		}
		return pixels;
	}

private:
	TArray<uint8_t> mPixels;
};

//===========================================================================
//
// The on-disk layout. Everything is little-endian and packed; there is no
// offset table, so Load() walks it.
//
//===========================================================================

#define MDL_MAGIC 0x4F504449	// "IDPO"

struct mdl_header_t
{
	int   magic;
	int   version;		// always 6
	float scale[3];		// packed byte -> world, per axis
	float origin[3];
	float radius;
	float eyePosition[3];
	int   numSkins;
	int   skinWidth;
	int   skinHeight;
	int   numVertices;
	int   numTriangles;
	int   numFrames;
	int   syncType;
	int   flags;
	float size;
};

struct mdl_texcoord_t { int onseam, s, t; };
struct mdl_triangle_t { int facesFront, vertex[3]; };
struct mdl_vertex_t   { uint8_t v[3], normalIndex; };


//===========================================================================
//
// FMDLModel::Load
//
//===========================================================================

bool FMDLModel::Load(const char * path, int lumpnum, const char * buffer, int length)
{
	if (length < (int)sizeof(mdl_header_t)) return false;

	const mdl_header_t *hdr = (const mdl_header_t *)buffer;
	if (LittleLong(hdr->magic) != MDL_MAGIC || LittleLong(hdr->version) != 6)
		return false;

	const int numskins  = LittleLong(hdr->numSkins);
	const int skinw     = LittleLong(hdr->skinWidth);
	const int skinh     = LittleLong(hdr->skinHeight);
	const int numverts  = LittleLong(hdr->numVertices);
	const int numtris   = LittleLong(hdr->numTriangles);
	const int numframes = LittleLong(hdr->numFrames);

	if (numverts <= 0 || numtris <= 0 || numframes <= 0 || skinw <= 0 || skinh <= 0)
	{
		Printf("LoadModel: '%s' has no usable geometry\n", path);
		return false;
	}

	for (int i = 0; i < 3; i++)
	{
		mScale[i]  = hdr->scale[i];
		mOrigin[i] = hdr->origin[i];
	}
	mVertexCount = numverts;

	const uint8_t *p   = (const uint8_t*)buffer + sizeof(mdl_header_t);
	const uint8_t *end = (const uint8_t*)buffer + length;

	// ---- skins -----------------------------------------------------------
	// Each is a group flag then the pixels. A group (flag 1) is an animated
	// skin: a count, that many float intervals, then that many images. The
	// first image is taken and the rest skipped -- see the note in the header.
	skins = new FTextureID[max(numskins, 1)];
	for (int i = 0; i < numskins; i++)
	{
		if (p + 4 > end) { Printf("LoadModel: '%s' truncated in skins\n", path); return false; }
		const int group = LittleLong(*(const int*)p); p += 4;

		int count = 1;
		if (group != 0)
		{
			if (p + 4 > end) return false;
			count = LittleLong(*(const int*)p); p += 4;
			if (count < 1) return false;
			p += count * 4;		// the interval table
		}
		if (p + (ptrdiff_t)skinw * skinh * count > end)
		{
			Printf("LoadModel: '%s' truncated in skin %d\n", path, i);
			return false;
		}

		FStringf texname("%s_skin%d", path, i);
		auto tex = MakeGameTexture(new FImageTexture(new FMDLSkinTexture(p, skinw, skinh)),
			texname.GetChars(), ETextureType::Override);
		skins[i] = TexMan.AddGameTexture(tex);

		p += (ptrdiff_t)skinw * skinh * count;
	}
	if (numskins == 0) skins[0].SetInvalid();

	// ---- texcoords -------------------------------------------------------
	if (p + (ptrdiff_t)numverts * sizeof(mdl_texcoord_t) > end)
	{
		Printf("LoadModel: '%s' truncated in texcoords\n", path);
		return false;
	}
	const mdl_texcoord_t *stverts = (const mdl_texcoord_t *)p;
	p += (ptrdiff_t)numverts * sizeof(mdl_texcoord_t);

	// ---- triangles -------------------------------------------------------
	if (p + (ptrdiff_t)numtris * sizeof(mdl_triangle_t) > end)
	{
		Printf("LoadModel: '%s' truncated in triangles\n", path);
		return false;
	}
	const mdl_triangle_t *tris = (const mdl_triangle_t *)p;
	p += (ptrdiff_t)numtris * sizeof(mdl_triangle_t);

	mFrameDataOfs = (int)(p - (const uint8_t*)buffer);

	// ---- SEAM EXPANSION --------------------------------------------------
	//
	// A vertex flagged onseam sits on the texture's wrap line and needs a
	// different s depending on whether the triangle using it faces front or
	// back. DMD has one texcoord per vertex, so the back copy becomes a second
	// vertex: same packed position, s shifted by half the skin width.
	//
	// Built lazily -- a model whose triangles all face front (or which has no
	// seam vertices at all) allocates no duplicates and ends up with exactly
	// the file's vertex count.
	TArray<int> backCopy(numverts, true);
	for (int i = 0; i < numverts; i++) backCopy[i] = -1;

	mSourceVert.Clear();
	mSourceVert.Reserve(numverts);
	for (int i = 0; i < numverts; i++) mSourceVert[i] = i;

	TArray<FTexCoord> tc(numverts, true);
	for (int i = 0; i < numverts; i++)
	{
		tc[i].s = (short)LittleLong(stverts[i].s);
		tc[i].t = (short)LittleLong(stverts[i].t);
	}

	TArray<unsigned int> indices;
	indices.Reserve(numtris * 3);
	for (int i = 0; i < numtris; i++)
	{
		const bool front = LittleLong(tris[i].facesFront) != 0;
		for (int j = 0; j < 3; j++)
		{
			int v = LittleLong(tris[i].vertex[j]);
			if (v < 0 || v >= numverts) { Printf("LoadModel: '%s' bad vertex index\n", path); return false; }

			if (!front && LittleLong(stverts[v].onseam) != 0)
			{
				if (backCopy[v] < 0)
				{
					FTexCoord shifted;
					shifted.s = (short)(LittleLong(stverts[v].s) + skinw / 2);
					shifted.t = (short)LittleLong(stverts[v].t);
					backCopy[v] = (int)tc.Push(shifted);
					mSourceVert.Push(v);
				}
				v = backCopy[v];
			}
			indices[i * 3 + j] = (unsigned)v;
		}
	}

	// ---- fill the DMD structures FDMDModel renders from ------------------
	header.magic   = MDL_MAGIC;
	header.version = 6;
	header.flags   = 0;

	info.skinWidth    = skinw;
	info.skinHeight   = skinh;
	info.numLODs      = 1;
	info.numSkins     = max(numskins, 1);
	info.numVertices  = (int)tc.Size();		// AFTER seam expansion
	info.numTexCoords = (int)tc.Size();
	info.numFrames    = numframes;
	info.frameSize    = 0;					// MDL frames are not fixed-size; unused here
	lodInfo[0].numTriangles = numtris;

	texCoords = new FTexCoord[tc.Size()];
	memcpy(texCoords, tc.Data(), tc.Size() * sizeof(FTexCoord));

	// BOTH INDEX ARRAYS, AND THEY ARE THE SAME NUMBER.
	//
	// DMD and MD2 address a position and a texcoord SEPARATELY -- FTriangle
	// carries vertexIndices[] and textureIndices[] and BuildVertexBuffer reads
	// texCoords[tri->textureIndices[j]] (models_md2.cpp). MDL has no such split:
	// one index per corner addresses both, which is exactly why a vertex sitting
	// on the skin seam has to be DUPLICATED at load rather than given a second
	// texcoord. That duplication is already done above, so by here the expanded
	// arrays are parallel and one index is correct for both.
	//
	// Setting only vertexIndices leaves textureIndices at whatever new[] left
	// there and every texcoord lookup reads an unrelated entry. The geometry and
	// the colours come out perfect and the skin is shattered across the mesh --
	// which reads as a broken model or a bad export, not as a missing assignment.
	lods[0].triangles = new FTriangle[numtris];
	for (int i = 0; i < numtris; i++)
	{
		for (int j = 0; j < 3; j++)
		{
			const int idx = indices[i * 3 + j];
			lods[0].triangles[i].vertexIndices[j]  = idx;
			lods[0].triangles[i].textureIndices[j] = idx;
		}
	}

	// ---- frame names, so FindFrame and MODELDEF FrameIndex work ----------
	//
	// Walked rather than indexed: a group frame carries a count, an interval
	// table and that many sub-frames, so the stride is not constant and the
	// only way to the next frame is through this one.
	frames = new ModelFrame[numframes];
	const uint8_t *fp = (const uint8_t*)buffer + mFrameDataOfs;
	for (int i = 0; i < numframes; i++)
	{
		if (fp + 4 > end) { Printf("LoadModel: '%s' truncated in frames\n", path); return false; }
		const int ftype = LittleLong(*(const int*)fp); fp += 4;

		int sub = 1;
		if (ftype != 0)
		{
			if (fp + 12 > end) return false;
			sub = LittleLong(*(const int*)fp); fp += 4;
			if (sub < 1) return false;
			fp += 8;				// group bbox
			fp += sub * 4;			// intervals
		}

		// Take the first sub-frame's name; skip the rest.
		if (fp + 8 + 16 > end) return false;
		fp += 8;					// this frame's bbox
		memcpy(frames[i].name, fp, 16);
		frames[i].name[15] = 0;
		fp += 16;
		fp += (ptrdiff_t)numverts * sizeof(mdl_vertex_t);

		for (int s = 1; s < sub; s++)
		{
			if (fp + 24 > end) return false;
			fp += 8 + 16 + (ptrdiff_t)numverts * sizeof(mdl_vertex_t);
		}

		frames[i].vindex = UINT_MAX;
	}

	mLumpNum = lumpnum;
	return true;
}

//===========================================================================
//
// FMDLModel::LoadGeometry
//
// Expands every frame's packed bytes with the model's GLOBAL scale and origin.
// A duplicated seam vertex reads its original's packed position, which is what
// mSourceVert is for.
//
//===========================================================================

void FMDLModel::LoadGeometry()
{
	if (framevtx != NULL) return;
	auto lumpdata = fileSystem.ReadFile(mLumpNum);
	LoadGeometry(&lumpdata);
}

void FMDLModel::LoadGeometry(FileSys::FileData* lumpData)
{
	static const int axis[3] = { VX, VY, VZ };
	auto buffer = lumpData->string();
	const uint8_t *fp = (const uint8_t*)buffer + mFrameDataOfs;

	framevtx = new ModelFrameVertexData[info.numFrames];

	for (int i = 0; i < info.numFrames; i++)
	{
		const int ftype = LittleLong(*(const int*)fp); fp += 4;
		int sub = 1;
		if (ftype != 0)
		{
			sub = LittleLong(*(const int*)fp); fp += 4;
			fp += 8;
			fp += sub * 4;
		}
		fp += 8 + 16;		// bbox + name

		const mdl_vertex_t *packed = (const mdl_vertex_t *)fp;

		ModelFrameVertexData *framev = &framevtx[i];
		framev->vertices = new DMDModelVertex[info.numVertices];
		framev->normals  = new DMDModelVertex[info.numVertices];

		for (int k = 0; k < info.numVertices; k++)
		{
			const int src = mSourceVert[k];
			const mdl_vertex_t &pv = packed[src];

			memcpy(framev->normals[k].xyz, avertexnormals[pv.normalIndex], sizeof(float) * 3);

			for (int c = 0; c < 3; c++)
				framev->vertices[k].xyz[axis[c]] = pv.v[c] * mScale[c] + mOrigin[c];
		}

		fp += (ptrdiff_t)mVertexCount * sizeof(mdl_vertex_t);
		for (int s = 1; s < sub; s++)
			fp += 8 + 16 + (ptrdiff_t)mVertexCount * sizeof(mdl_vertex_t);
	}
}

//===========================================================================
//
// FMDLModel::UnloadGeometry
//
// FREE ONLY WHAT LoadGeometry MADE. This exists because the base class's
// version freed things this format never rebuilds, and that was a crash.
//
// WHAT WENT WRONG, because the shape of it will recur. FDMDModel's vertex
// buffer builder is LoadGeometry -> fill -> UnloadGeometry, and for MD2 and DMD
// that round trip is symmetric: LoadGeometry makes the frame vertices, the
// texcoords AND the triangle lists, so UnloadGeometry is right to free all
// three. MDL is not symmetric. Its texcoords and triangles are derived once, in
// Load(), out of the seam-expansion pass -- they cannot be rebuilt from the
// lump alone without redoing it, and LoadGeometry does not try. So the
// inherited UnloadGeometry was freeing permanent data as though it were
// scratch.
//
// The first draw was always fine. The SECOND build -- which is what a level
// reload, a respawn or a new game triggers -- called LoadGeometry, got its
// frame vertices back, and then walked a texCoords array that had been deleted
// and never remade. An access violation inside the render pass, a crash dialog
// opening behind the VR view, and from the player's side the game simply froze
// on respawn. It cost days.
//
// THE RULE THIS SETTLES: UnloadGeometry drops exactly what the matching
// LoadGeometry can put back, and nothing else. Any future format whose loader
// precomputes something at Load time overrides this the same way.
//
//===========================================================================

void FMDLModel::UnloadGeometry()
{
	// The frame vertices, and only those: FMDLModel::LoadGeometry rebuilds them
	// from the lump every time it is asked, so they are safe to drop.
	if (framevtx != NULL)
	{
		for (int i = 0; i < info.numFrames; i++)
		{
			if (framevtx[i].vertices != NULL) delete[] framevtx[i].vertices;
			if (framevtx[i].normals != NULL) delete[] framevtx[i].normals;
			framevtx[i].vertices = NULL;
			framevtx[i].normals = NULL;
		}
		delete[] framevtx;
		framevtx = NULL;
	}

	// texCoords and lods[].triangles are deliberately LEFT ALONE. They belong to
	// Load(), they last as long as the model does, and ~FDMDModel still frees
	// them: a destructor dispatches to its own class's version, so teardown goes
	// through the base and nothing leaks.
}

//===========================================================================
//
// FMDLModel::AddSkins
//
// The skin is ours -- built from the embedded pixels in Load -- so it is
// registered here rather than resolved by name. A MODELDEF Skin directive still
// overrides it, which is how a converted or hand-painted replacement is used.
//
//===========================================================================

void FMDLModel::AddSkins(uint8_t *hitlist, const FTextureID* surfaceskinids)
{
	if (surfaceskinids && surfaceskinids[0].isValid())
	{
		hitlist[surfaceskinids[0].GetIndex()] |= FTextureManager::HIT_Flat;
		return;
	}
	for (int i = 0; i < info.numSkins; i++)
	{
		if (skins[i].isValid())
			hitlist[skins[i].GetIndex()] |= FTextureManager::HIT_Flat;
	}
}

FMDLModel::~FMDLModel()
{
}
