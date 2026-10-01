/*
** hw_skydome.h
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
*/

#pragma once

#include "matrix.h"
#include "hwrenderer/data/buffers.h"
#include "hw_renderstate.h"
#include "skyboxtexture.h"

class FGameTexture;
class FRenderState;
class IVertexBuffer;
struct HWSkyPortal;
struct HWDrawInfo;

// 57 world units roughly represent one sky texel for the glTranslate call.
const int skyoffsetfactor = 57;

struct FSkyVertex
{
	float x, y, z, u, v, lu, lv, lindex;
	PalEntry color;

	void Set(float xx, float zz, float yy, float uu=0, float vv=0, PalEntry col=0xffffffff)
	{
		x = xx;
		z = zz;
		y = yy;
		u = uu;
		v = vv;
		lu = 0.0f;
		lv = 0.0f;
		lindex = -1.0f;
		color = col;
	}

	void SetXYZ(float xx, float yy, float zz, float uu = 0, float vv = 0, PalEntry col = 0xffffffff)
	{
		x = xx;
		y = yy;
		z = zz;
		u = uu;
		v = vv;
		lu = 0.0f;
		lv = 0.0f;
		lindex = -1.0f;
		color = col;
	}

};

class FSkyVertexBuffer
{
	friend struct HWSkyPortal;
public:
	static const int SKYHEMI_UPPER = 1;
	static const int SKYHEMI_LOWER = 2;

	enum
	{
		SKYMODE_MAINLAYER = 0,
		SKYMODE_SECONDLAYER = 1,
		SKYMODE_FOGLAYER = 2
	};

	IVertexBuffer *mVertexBuffer;

	TArray<FSkyVertex> mVertices;
	TArray<unsigned int> mPrimStartDoom;
	TArray<unsigned int> mPrimStartBuild;

	int mRows, mColumns;

	// indices for sky cubemap faces
	int mFaceStart[7];
	int mSideStart;

	void SkyVertexDoom(int r, int c, bool yflip);
	void SkyVertexBuild(int r, int c, bool yflip);
	void CreateSkyHemisphereDoom(int hemi);
	void CreateSkyHemisphereBuild(int hemi);
	void CreateDome();

public:

	// A BAND SKY instead of a dome.
	//
	// A dome maps the viewer's pitch onto curvature, so the sky wraps overhead.
	// Some engines never do that: they paint the sky as a strip anchored to the
	// viewport that only drifts sideways as you turn, because their look-up is a
	// vertical shear of the finished picture rather than a camera rotation.
	// Realms of the Haunting is one (render_parallax_sky_columns,
	// renderer.c:5383), but the projection is not specific to it and is named
	// for what it does, not for who asked.
	//
	// It is the ordinary dome geometry with the viewer's PITCH CANCELLED, which
	// pins the texture's vertical position to the screen while yaw still moves
	// it sideways. The cancelling rotation is about the viewer's own right axis,
	// which in the dome's own space means conjugating it by the view yaw -- see
	// SetupMatrices.
	//
	// Inert by default: `active` false is exactly what every existing caller
	// already gets.
	struct BandSky
	{
		bool active;
		float pitch;   // the viewer pitch to cancel, degrees
		float yaw;     // the viewer yaw, naming the axis, degrees
		// A constructor rather than member initializers: GCC rejects a default
		// argument (`= BandSky()` below) that needs a nested class's member
		// initializers before the enclosing class is complete. MSVC allows it.
		BandSky() : active(false), pitch(0.f), yaw(0.f) {}
	};

	FSkyVertexBuffer();
	~FSkyVertexBuffer();
	void SetupMatrices(FGameTexture *tex, float x_offset, float y_offset, bool mirror, int mode, VSMatrix &modelmatrix, VSMatrix &textureMatrix, bool tiled, float xscale = 0, float vertscale = 0, const BandSky &band = BandSky());
	std::pair<IVertexBuffer *, IIndexBuffer *> GetBufferObjects() const
	{
		return std::make_pair(mVertexBuffer, nullptr);
	}

	int FaceStart(int i)
	{
		if (i >= 0 && i < 7) return mFaceStart[i];
		else return mSideStart;
	}

	void RenderRow(FRenderState& state, EDrawType prim, int row, TArray<unsigned int>& mPrimStart, bool apply = true);
	void DoRenderDome(FRenderState& state, FGameTexture* tex, int mode, bool which, PalEntry color = 0xffffffff);
	void RenderDome(FRenderState& state, FGameTexture* tex, float x_offset, float y_offset, bool mirror, int mode, bool tiled, float xscale = 0, float yscale = 0, PalEntry color = 0xffffffff, const BandSky &band = BandSky());
	void RenderBox(FRenderState& state, FSkyBox* tex, float x_offset, bool sky2, float stretch, const FVector3& skyrotatevector, const FVector3& skyrotatevector2, PalEntry color = 0xffffffff);

};
