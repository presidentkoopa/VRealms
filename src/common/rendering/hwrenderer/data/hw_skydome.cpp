/*
** hw_skydome.cpp
**
**
** Draws the sky.  Loosely based on the JDoom sky and the ZDoomGL 0.66.2 sky.
**
**---------------------------------------------------------------------------
**
** Copyright 2003-2018 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Copyright 2003 Tim Stump
**
** SPDX-License-Identifier: BSD-3-Clause
**
**---------------------------------------------------------------------------
**
*/

#include "filesystem.h"
#include "cmdlib.h"
#include "bitmap.h"
#include "skyboxtexture.h"
#include "hw_material.h"
#include "hw_skydome.h"
#include "hw_renderstate.h"
#include "v_video.h"
#include "hwrenderer/data/buffers.h"
#include "version.h"

//-----------------------------------------------------------------------------
//
// Shamelessly lifted from Doomsday (written by Jaakko Keränen)
// also shamelessly lifted from ZDoomGL! ;)
//
//-----------------------------------------------------------------------------
CVAR(Float, skyoffset, 0.f, 0)	// for testing


struct SkyColor
{
	FTextureID Texture;
	std::pair<PalEntry, PalEntry> Colors;
};

static TArray<SkyColor> SkyColors;

std::pair<PalEntry, PalEntry>& R_GetSkyCapColor(FGameTexture* tex)
{
	for (auto& sky : SkyColors)
	{
		if (sky.Texture == tex->GetID()) return sky.Colors;
	}

	auto itex = tex->GetTexture();
	SkyColor sky;

	FBitmap bitmap = itex->GetBgraBitmap(nullptr);
	int w = bitmap.GetWidth();
	int h = bitmap.GetHeight();

	const uint32_t* buffer = (const uint32_t*)bitmap.GetPixels();
	if (buffer)
	{
		sky.Colors.first = averageColor((uint32_t*)buffer, w * min(30, h), 0);
		if (h > 30)
		{
			sky.Colors.second = averageColor(((uint32_t*)buffer) + (h - 30) * w, w * 30, 0);
		}
		else sky.Colors.second = sky.Colors.first;
	}
	sky.Texture = tex->GetID();
	SkyColors.Push(sky);

	return SkyColors.Last().Colors;
}



//-----------------------------------------------------------------------------
//
//
//
//-----------------------------------------------------------------------------

FSkyVertexBuffer::FSkyVertexBuffer()
{
	CreateDome();
	mVertexBuffer = screen->CreateVertexBuffer();

	static const FVertexBufferAttribute format[] = {
		{ 0, VATTR_VERTEX, VFmt_Float3, (int)myoffsetof(FSkyVertex, x) },
		{ 0, VATTR_TEXCOORD, VFmt_Float2, (int)myoffsetof(FSkyVertex, u) },
		{ 0, VATTR_COLOR, VFmt_Byte4, (int)myoffsetof(FSkyVertex, color) },
		{ 0, VATTR_LIGHTMAP, VFmt_Float3, (int)myoffsetof(FSkyVertex, lu) },
	};
	mVertexBuffer->SetFormat(1, 4, sizeof(FSkyVertex), format);
	mVertexBuffer->SetData(mVertices.Size() * sizeof(FSkyVertex), &mVertices[0], BufferUsageType::Static);
}

FSkyVertexBuffer::~FSkyVertexBuffer()
{
	delete mVertexBuffer;
}

//-----------------------------------------------------------------------------
//
//
//
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//
// The world-locked sky cylinder -- see CreateRothSky in the header for why this
// is a cylinder and not a dome.
//
// The three constants are MEASURED in the running original at 640x480, not
// derived: the sky picture's row 0 sits at tanElev = 100.27/177.53 (elevation
// +29.5 degrees) and its last row, 145, at tanElev = (100.27-145)/177.53
// (elevation -14.1). 177.53 is half the vertical focal length, so the picture
// is drawn two screen rows per source row.
//
//-----------------------------------------------------------------------------

// row = ROTH_SKY_ROW0 - ROTH_SKY_ROWS_PER_TAN * tanElev, clamped to the picture
static const float ROTH_SKY_ROW0 = 100.27f;
static const float ROTH_SKY_ROWS_PER_TAN = 177.53f;
static const float ROTH_SKY_ROWS = 146.f;     // rows 0..145
// One source column is 1/1024 of a turn, so 256 columns span 90 degrees and the
// picture tiles four times per revolution.
static const float ROTH_SKY_DEGREES_PER_WRAP = 90.f;

// Where the cylinder landed in the shared vertex array. File-static rather
// than members: hw_skydome.h is included very widely, and this is nobody
// else's business.
static int gRothStart = 0, gRothCount = 0;

static void CreateRothSky(TArray<FSkyVertex> &verts)
{
	const int cols = 256;              // 64 segments per texture wrap
	const float scale = 10000.f;
	// Far enough above and below that the clamp, not the geometry, decides what
	// is drawn at steep angles: the picture itself only covers tanElev within
	// about [-0.25, +0.56].
	const float tanTop = 4.f, tanBottom = -4.f;

	auto vOf = [](float tanElev) {
		return (ROTH_SKY_ROW0 - ROTH_SKY_ROWS_PER_TAN * tanElev) / ROTH_SKY_ROWS;
	};
	const float vTop = vOf(tanTop), vBottom = vOf(tanBottom);

	gRothStart = verts.Size();
	for (int c = 0; c <= cols; c++)
	{
		const float deg = c * 360.f / (float)cols;
		FAngle a = FAngle::fromDeg(deg);
		FVector2 pos = a.ToVector(scale);

		// u is NEGATED and x MIRRORED for the same reason the dome does both
		// (SkyVertexDoom: "Doom mirrors the sky vertically"): the view matrix
		// carries a scale(-1, ...) in x, so geometry and texture are both built
		// pre-mirrored to come out the right way round.
		const float u = -(deg - 90.f) / ROTH_SKY_DEGREES_PER_WRAP;

		// REMAROTH stores every Realms picture QUARTER-TURNED: the engine
		// texture's s axis is the picture's stored ROW and t its stored COLUMN
		// (roth_surface.h; the walls and flats rely on it too). So the column
		// law above lands on t and the row law on s. Checked against the
		// original's own texel-per-pixel capture (REMAROTH_SKY_MEASURED.md).
		FSkyVertex top, bot;
		top.Set(-pos.X, pos.Y, scale * tanTop, vTop, u);
		bot.Set(-pos.X, pos.Y, scale * tanBottom, vBottom, u);
		verts.Push(top);
		verts.Push(bot);
	}
	gRothCount = verts.Size() - gRothStart;
}

void FSkyVertexBuffer::SkyVertexDoom(int r, int c, bool zflip)
{
	static const FAngle maxSideAngle = FAngle::fromDeg(60.f);
	static const float scale = 10000.;

	FAngle topAngle = FAngle::fromDeg((c / (float)mColumns * 360.f));
	FAngle sideAngle = maxSideAngle * float(mRows - r) / float(mRows);
	float height = sideAngle.Sin();
	float realRadius = scale * sideAngle.Cos();
	FVector2 pos = topAngle.ToVector(realRadius);
	float z = (!zflip) ? scale * height : -scale * height;

	FSkyVertex vert;

	vert.color = r == 0 ? 0xffffff : 0xffffffff;

	// And the texture coordinates.
	if (!zflip)	// Flipped Y is for the lower hemisphere.
	{
		vert.u = (-c / (float)mColumns);
		vert.v = (r / (float)mRows);
	}
	else
	{
		vert.u = (-c / (float)mColumns);
		vert.v = 1.0f + ((mRows - r) / (float)mRows);
	}

	if (r != 4) z += 300;
	// And finally the vertex.
	vert.x = -pos.X;	// Doom mirrors the sky vertically!
	vert.y = z - 1.f;
	vert.z = pos.Y;

	mVertices.Push(vert);
}

//-----------------------------------------------------------------------------
//
//
//
//-----------------------------------------------------------------------------

void FSkyVertexBuffer::SkyVertexBuild(int r, int c, bool zflip)
{
	static const FAngle maxSideAngle = FAngle::fromDeg(60.f);
	static const float scale = 10000.;

	FAngle topAngle = FAngle::fromDeg((c / (float)mColumns * 360.f));
	FVector2 pos = topAngle.ToVector(scale);
	float z = (!zflip) ? (mRows - r) * 4000.f : -(mRows - r) * 4000.f;

	FSkyVertex vert;

	vert.color = r == 0 ? 0xffffff : 0xffffffff;

	// And the texture coordinates.
	if (zflip) r = mRows * 2 - r;
	vert.u = 0.5f + (-c / (float)mColumns);
	vert.v = (r / (float)(2*mRows));

	// And finally the vertex.
	vert.x = pos.X;
	vert.y = z - 1.f;
	vert.z = pos.Y;

	mVertices.Push(vert);
}

//-----------------------------------------------------------------------------
//
//
//
//-----------------------------------------------------------------------------

void FSkyVertexBuffer::CreateSkyHemisphereDoom(int hemi)
{
	int r, c;
	bool zflip = !!(hemi & SKYHEMI_LOWER);

	mPrimStartDoom.Push(mVertices.Size());

	for (c = 0; c < mColumns; c++)
	{
		SkyVertexDoom(1, c, zflip);
	}

	// The total number of triangles per hemisphere can be calculated
	// as follows: rows * columns * 2 + 2 (for the top cap).
	for (r = 0; r < mRows; r++)
	{
		mPrimStartDoom.Push(mVertices.Size());
		for (c = 0; c <= mColumns; c++)
		{
			SkyVertexDoom(r + zflip, c, zflip);
			SkyVertexDoom(r + 1 - zflip, c, zflip);
		}
	}
}

//-----------------------------------------------------------------------------
//
//
//
//-----------------------------------------------------------------------------

void FSkyVertexBuffer::CreateSkyHemisphereBuild(int hemi)
{
	int r, c;
	bool zflip = !!(hemi & SKYHEMI_LOWER);

	mPrimStartBuild.Push(mVertices.Size());

	for (c = 0; c < mColumns; c++)
	{
		SkyVertexBuild(1, c, zflip);
	}

	// The total number of triangles per hemisphere can be calculated
	// as follows: rows * columns * 2 + 2 (for the top cap).
	for (r = 0; r < mRows; r++)
	{
		mPrimStartBuild.Push(mVertices.Size());
		for (c = 0; c <= mColumns; c++)
		{
			SkyVertexBuild(r + zflip, c, zflip);
			SkyVertexBuild(r + 1 - zflip, c, zflip);
		}
	}
}

//-----------------------------------------------------------------------------
//
//
//
//-----------------------------------------------------------------------------

void FSkyVertexBuffer::CreateDome()
{
	// the first thing we put into the buffer is the fog layer object which is just 4 triangles around the viewpoint.

	mVertices.Reserve(12);
	mVertices[0].Set(1.0f, 1.0f, -1.0f);
	mVertices[1].Set(1.0f, -1.0f, -1.0f);
	mVertices[2].Set(-1.0f, 0.0f, -1.0f);

	mVertices[3].Set(1.0f, 1.0f, -1.0f);
	mVertices[4].Set(1.0f, -1.0f, -1.0f);
	mVertices[5].Set(0.0f, 0.0f, 1.0f);

	mVertices[6].Set(-1.0f, 0.0f, -1.0f);
	mVertices[7].Set(1.0f, 1.0f, -1.0f);
	mVertices[8].Set(0.0f, 0.0f, 1.0f);

	mVertices[9].Set(1.0f, -1.0f, -1.0f);
	mVertices[10].Set(-1.0f, 0.0f, -1.0f);
	mVertices[11].Set(0.0f, 0.0f, 1.0f);

	mColumns = 128;
	mRows = 4;
	CreateSkyHemisphereDoom(SKYHEMI_UPPER);
	CreateSkyHemisphereDoom(SKYHEMI_LOWER);
	mPrimStartDoom.Push(mVertices.Size());

	CreateSkyHemisphereBuild(SKYHEMI_UPPER);
	CreateSkyHemisphereBuild(SKYHEMI_LOWER);
	mPrimStartBuild.Push(mVertices.Size());

	// The Realms sky cylinder, appended after the dome and the box so neither
	// of their index ranges moves. Inert unless something draws it.
	CreateRothSky(mVertices);

	mSideStart = mVertices.Size();
	mFaceStart[0] = mSideStart + 10;
	mFaceStart[1] = mFaceStart[0] + 4;
	mFaceStart[2] = mFaceStart[1] + 4;
	mFaceStart[3] = mFaceStart[2] + 4;
	mFaceStart[4] = mFaceStart[3] + 4;
	mFaceStart[5] = mFaceStart[4] + 4;
	mFaceStart[6] = mFaceStart[5] + 4;
	mVertices.Reserve(10 + 7*4);
	FSkyVertex *ptr = &mVertices[mSideStart];

	// all sides
	ptr[0].SetXYZ(128.f, 128.f, -128.f, 0, 0);
	ptr[1].SetXYZ(128.f, -128.f, -128.f, 0, 1);
	ptr[2].SetXYZ(-128.f, 128.f, -128.f, 0.25f, 0);
	ptr[3].SetXYZ(-128.f, -128.f, -128.f, 0.25f, 1);
	ptr[4].SetXYZ(-128.f, 128.f, 128.f, 0.5f, 0);
	ptr[5].SetXYZ(-128.f, -128.f, 128.f, 0.5f, 1);
	ptr[6].SetXYZ(128.f, 128.f, 128.f, 0.75f, 0);
	ptr[7].SetXYZ(128.f, -128.f, 128.f, 0.75f, 1);
	ptr[8].SetXYZ(128.f, 128.f, -128.f, 1, 0);
	ptr[9].SetXYZ(128.f, -128.f, -128.f, 1, 1);

	// north face
	ptr[10].SetXYZ(128.f, 128.f, -128.f, 0, 0);
	ptr[11].SetXYZ(-128.f, 128.f, -128.f, 1, 0);
	ptr[12].SetXYZ(128.f, -128.f, -128.f, 0, 1);
	ptr[13].SetXYZ(-128.f, -128.f, -128.f, 1, 1);

	// east face
	ptr[14].SetXYZ(-128.f, 128.f, -128.f, 0, 0);
	ptr[15].SetXYZ(-128.f, 128.f, 128.f, 1, 0);
	ptr[16].SetXYZ(-128.f, -128.f, -128.f, 0, 1);
	ptr[17].SetXYZ(-128.f, -128.f, 128.f, 1, 1);

	// south face
	ptr[18].SetXYZ(-128.f, 128.f, 128.f, 0, 0);
	ptr[19].SetXYZ(128.f, 128.f, 128.f, 1, 0);
	ptr[20].SetXYZ(-128.f, -128.f, 128.f, 0, 1);
	ptr[21].SetXYZ(128.f, -128.f, 128.f, 1, 1);

	// west face
	ptr[22].SetXYZ(128.f, 128.f, 128.f, 0, 0);
	ptr[23].SetXYZ(128.f, 128.f, -128.f, 1, 0);
	ptr[24].SetXYZ(128.f, -128.f, 128.f, 0, 1);
	ptr[25].SetXYZ(128.f, -128.f, -128.f, 1, 1);

	// bottom face
	ptr[26].SetXYZ(128.f, -128.f, -128.f, 0, 0);
	ptr[27].SetXYZ(-128.f, -128.f, -128.f, 1, 0);
	ptr[28].SetXYZ(128.f, -128.f, 128.f, 0, 1);
	ptr[29].SetXYZ(-128.f, -128.f, 128.f, 1, 1);

	// top face
	ptr[30].SetXYZ(128.f, 128.f, -128.f, 0, 0);
	ptr[31].SetXYZ(-128.f, 128.f, -128.f, 1, 0);
	ptr[32].SetXYZ(128.f, 128.f, 128.f, 0, 1);
	ptr[33].SetXYZ(-128.f, 128.f, 128.f, 1, 1);

	// top face flipped
	ptr[34].SetXYZ(128.f, 128.f, -128.f, 0, 1);
	ptr[35].SetXYZ(-128.f, 128.f, -128.f, 1, 1);
	ptr[36].SetXYZ(128.f, 128.f, 128.f, 0, 0);
	ptr[37].SetXYZ(-128.f, 128.f, 128.f, 1, 0);
}

//-----------------------------------------------------------------------------
//
//
//
//-----------------------------------------------------------------------------

void FSkyVertexBuffer::SetupMatrices(FGameTexture *tex, float x_offset, float y_offset, bool mirror, int mode, VSMatrix &modelMatrix, VSMatrix &textureMatrix, bool tiled, float xscale, float yscale)
{
	float texw = tex->GetDisplayWidth();
	float texh = tex->GetDisplayHeight();

	modelMatrix.loadIdentity();

	modelMatrix.rotate(-180.0f + x_offset, 0.f, 1.f, 0.f);

	if (xscale == 0) xscale = texw < 1024.f ? floorf(1024.f / float(texw)) : 1.f;
	auto texskyoffset = tex->GetSkyOffset() + skyoffset;
	if (yscale == 0)
	{
		if (texh <= 128 && tiled)
		{
			modelMatrix.translate(0.f, (-40 + texskyoffset) * skyoffsetfactor, 0.f);
			modelMatrix.scale(1.f, 1.2f * 1.17f, 1.f);
			yscale = 240.f / texh;
		}
		else if (texh < 128)
		{
			// smaller sky textures must be tiled. We restrict it to 128 sky pixels, though
			modelMatrix.translate(0.f, -1250.f, 0.f);
			modelMatrix.scale(1.f, 128 / 230.f, 1.f);
			yscale = float(128 / texh);	// intentionally left as integer.
		}
		else if (texh < 200)
		{
			modelMatrix.translate(0.f, -1250.f, 0.f);
			modelMatrix.scale(1.f, texh / 230.f, 1.f);
			yscale = 1.f;
		}
		else if (texh <= 240)
		{
			modelMatrix.translate(0.f, (200 - texh + texskyoffset) * skyoffsetfactor, 0.f);
			modelMatrix.scale(1.f, 1.f + ((texh - 200.f) / 200.f) * 1.17f, 1.f);
			yscale = 1.f;
		}
		else
		{
			modelMatrix.translate(0.f, (-40 + texskyoffset) * skyoffsetfactor, 0.f);
			modelMatrix.scale(1.f, 1.2f * 1.17f, 1.f);
			yscale = 240.f / texh;
		}
	}
	else
	{
		modelMatrix.translate(0.f, (-40 + texskyoffset) * skyoffsetfactor, 0.f);
		modelMatrix.scale(1.f, 1.2f * 1.17f, 1.f);
	}
	textureMatrix.loadIdentity();
	textureMatrix.scale(mirror ? -xscale : xscale, yscale, 1.f);
	textureMatrix.translate(1.f, y_offset / texh, 1.f);
}

//-----------------------------------------------------------------------------
//
//
//
//-----------------------------------------------------------------------------

void FSkyVertexBuffer::RenderRow(FRenderState& state, EDrawType prim, int row, TArray<unsigned int>& mPrimStart, bool apply)
{
	state.Draw(prim, mPrimStart[row], mPrimStart[row + 1] - mPrimStart[row]);
}

//-----------------------------------------------------------------------------
//
//
//
//-----------------------------------------------------------------------------

void FSkyVertexBuffer::DoRenderDome(FRenderState& state, FGameTexture* tex, int mode, bool which, PalEntry color)
{
	auto& primStart = which ? mPrimStartBuild : mPrimStartDoom;
	if (tex && tex->isValid())
	{
		state.SetMaterial(tex, UF_Texture, 0, (mode == FSkyVertexBuffer::SKYMODE_FOGLAYER ? CLAMP_XY : CLAMP_NONE), 0, -1);
		state.EnableModelMatrix(true);
		state.EnableTextureMatrix(true);
	}

	int rc = mRows + 1;

	// The caps only get drawn for the main layer but not for the overlay.
	if (mode == FSkyVertexBuffer::SKYMODE_MAINLAYER && tex != nullptr)
	{
		auto col = R_GetSkyCapColor(tex);

		col.first.r = col.first.r * color.r / 255;
		col.first.g = col.first.g * color.g / 255;
		col.first.b = col.first.b * color.b / 255;
		col.second.r = col.second.r * color.r / 255;
		col.second.g = col.second.g * color.g / 255;
		col.second.b = col.second.b * color.b / 255;

		state.SetObjectColor(col.first);
		state.EnableTexture(false);
		RenderRow(state, DT_TriangleFan, 0, primStart);

		state.SetObjectColor(col.second);
		RenderRow(state, DT_TriangleFan, rc, primStart);
		state.EnableTexture(true);
	}
	state.SetObjectColor(color);
	for (int i = 1; i <= mRows; i++)
	{
		RenderRow(state, DT_TriangleStrip, i, primStart, i == 1);
		RenderRow(state, DT_TriangleStrip, rc + i, primStart, false);
	}

	state.EnableTextureMatrix(false);
	state.EnableModelMatrix(false);
}


//-----------------------------------------------------------------------------
//
// This is only for Doom-style skies.
//
//-----------------------------------------------------------------------------

void FSkyVertexBuffer::RenderDome(FRenderState& state, FGameTexture* tex, float x_offset, float y_offset, bool mirror, int mode, bool tiled, float xscale, float yscale, PalEntry color)
{
	if (tex)
	{
		SetupMatrices(tex, x_offset, y_offset, mirror, mode, state.mModelMatrix, state.mTextureMatrix, tiled, xscale, yscale);
	}
	DoRenderDome(state, tex, mode, false, color);
}


//-----------------------------------------------------------------------------
//
//
//
//-----------------------------------------------------------------------------

void FSkyVertexBuffer::RenderRothSky(FRenderState& state, FGameTexture* tex)
{
	if (tex == nullptr || !tex->isValid() || gRothCount < 4) return;

	// CLAMP_Y is the whole point of the vertical half: beyond the picture the
	// original smears its first and last source row rather than wrapping, and
	// clamping the sampler is exactly that. X repeats, because the picture
	// tiles four times per revolution.
	// The picture's rows are on s (see CreateRothSky), so that is the axis to
	// clamp; its columns, on t, repeat.
	state.SetMaterial(tex, UF_Texture, 0, CLAMP_X, 0, -1);
	// Neither matrix carries anything here -- both laws are already in the
	// vertex UVs, which is what makes this exact.
	state.EnableModelMatrix(false);
	state.EnableTextureMatrix(false);
	state.SetCulling(Cull_None);
	state.Draw(DT_TriangleStrip, gRothStart, gRothCount);
	state.SetCulling(Cull_None);
}

void FSkyVertexBuffer::RenderBox(FRenderState& state, FSkyBox* tex, float x_offset, bool sky2, float stretch, const FVector3& skyrotatevector, const FVector3& skyrotatevector2, PalEntry color)
{
	int faces;

	state.SetObjectColor(color);
	state.EnableModelMatrix(true);
	state.mModelMatrix.loadIdentity();
	state.mModelMatrix.scale(1, 1 / stretch, 1); // Undo the map's vertical scaling as skyboxes are true cubes.

	if (!sky2)
		state.mModelMatrix.rotate(-180.0f + x_offset, skyrotatevector.X, skyrotatevector.Z, skyrotatevector.Y);
	else
		state.mModelMatrix.rotate(-180.0f + x_offset, skyrotatevector2.X, skyrotatevector2.Z, skyrotatevector2.Y);

	if (tex->GetSkyFace(5))
	{
		faces = 4;

		// north
		state.SetMaterial(tex->GetSkyFace(0), UF_Texture, 0, CLAMP_XY, 0, -1);
		state.Draw(DT_TriangleStrip, FaceStart(0), 4);

		// east
		state.SetMaterial(tex->GetSkyFace(1), UF_Texture, 0, CLAMP_XY, 0, -1);
		state.Draw(DT_TriangleStrip, FaceStart(1), 4);

		// south
		state.SetMaterial(tex->GetSkyFace(2), UF_Texture, 0, CLAMP_XY, 0, -1);
		state.Draw(DT_TriangleStrip, FaceStart(2), 4);

		// west
		state.SetMaterial(tex->GetSkyFace(3), UF_Texture, 0, CLAMP_XY, 0, -1);
		state.Draw(DT_TriangleStrip, FaceStart(3), 4);
	}
	else
	{
		faces = 1;
		state.SetMaterial(tex->GetSkyFace(0), UF_Texture, 0, CLAMP_XY, 0, -1);
		state.Draw(DT_TriangleStrip, FaceStart(-1), 10);
	}

	// top
	state.SetMaterial(tex->GetSkyFace(faces), UF_Texture, 0, CLAMP_XY, 0, -1);
	state.Draw(DT_TriangleStrip, FaceStart(tex->GetSkyFlip() ? 6 : 5), 4);

	// bottom
	state.SetMaterial(tex->GetSkyFace(faces + 1), UF_Texture, 0, CLAMP_XY, 0, -1);
	state.Draw(DT_TriangleStrip, FaceStart(4), 4);

	state.EnableModelMatrix(false);
	state.SetObjectColor(0xffffffff);
}
