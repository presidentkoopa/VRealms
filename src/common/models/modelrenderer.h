/*
** modelrenderer.h
**
**
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

#pragma once
#include "renderstyle.h"
#include "matrix.h"
#include "model.h"

class FModelRenderer
{
public:
	virtual ~FModelRenderer() = default;

	virtual ModelRendererType GetType() const = 0;

	virtual void BeginDrawModel(FRenderStyle style, int smf_flags, const VSMatrix &objectToWorldMatrix, bool mirrored) = 0;
	virtual void EndDrawModel(FRenderStyle style, int smf_flags) = 0;

	virtual IModelVertexBuffer *CreateVertexBuffer(bool needindex, bool singleframe) = 0;

	virtual VSMatrix GetViewToWorldMatrix() = 0;

	virtual void BeginDrawHUDModel(FRenderStyle style, const VSMatrix &objectToWorldMatrix, bool mirrored, int smf_flags) = 0;
	virtual void EndDrawHUDModel(FRenderStyle style, int smf_flags) = 0;

	virtual void SetInterpolation(double interpolation) = 0;
	virtual void SetMaterial(FGameTexture *skin, bool clampNoFilter, FTranslationID translation) = 0;
	virtual void DrawArrays(int start, int count) = 0;
	virtual void DrawElements(int numIndices, size_t offset) = 0;
	virtual void SetupFrame(FModel* model, unsigned int frame1, unsigned int frame2, unsigned int size, int boneStartIndex) {};

	// RS FORK -- A MODEL-SPACE TRANSFORM FOR ONE SURFACE, mid-model.
	//
	// The object-to-world matrix is handed over once per MODEL, at
	// BeginDrawModel, which is right for a model that moves as one piece and
	// has nothing to say about a model whose parts move independently of each
	// other. This lets one surface be drawn with an extra transform in front
	// of that matrix and the next surface be drawn without it.
	//
	// Passing nullptr restores the model's own matrix. A renderer that does
	// not implement this simply draws every surface at the model's transform,
	// which is exactly the behaviour it had before this existed.
	virtual void SetSurfaceTransform(const VSMatrix* localTransform) {};

	// RS FORK -- the model's own object-to-world, for the draw-rate hand drive.
	//
	// A driven surface has to turn a live controller position into a position
	// along its own travel axis, and that means getting a world point into
	// MODEL space, which needs this matrix. The renderer was handed it at
	// BeginDrawModel and is the only thing that still has it at draw time --
	// the fill loop that needs it is several calls below the function that
	// computed it, and threading it down would touch every model format's
	// signature for the benefit of one.
	//
	// Returns false when there is nothing sensible to give (a renderer that
	// does not implement it, or a model not currently being drawn), and the
	// caller then leaves the surface where script last put it.
	virtual bool GetModelToWorldMatrix(VSMatrix* out) const { return false; }

	// RS FORK -- NEAR-EYE FADE for the world model being drawn, between BeginDrawModel and
	// EndDrawModel: its pixels closer to the eye than farDist dissolve in a dither, all of
	// them by nearDist (map units). farDist <= nearDist turns it off again. A renderer that
	// does not implement it draws the model whole, which is what it did before this existed.
	virtual void SetEyeFade(float nearDist, float farDist) {}
};
