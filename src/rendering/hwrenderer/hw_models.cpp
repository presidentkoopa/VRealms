/*
** hw_models.cpp
**
** hardware renderer model handling code
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
#include "g_game.h"
#include "doomstat.h"
#include "g_level.h"
#include "r_state.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "i_time.h"
#include "cmdlib.h"
#include "hw_material.h"
#include "hwrenderer/data/buffers.h"
#include "flatvertices.h"
#include "hwrenderer/scene/hw_drawinfo.h"
#include "hw_renderstate.h"
#include "hwrenderer/scene/hw_portal.h"
#include "hw_bonebuffer.h"
#include "hw_models.h"

CVAR(Bool, gl_light_models, true, CVAR_ARCHIVE)

float gldepthmin, gldepthmax;

VSMatrix FHWModelRenderer::GetViewToWorldMatrix()
{
	VSMatrix objectToWorldMatrix;
	di->VPUniforms.mViewMatrix.inverseMatrix(objectToWorldMatrix);
	return objectToWorldMatrix;
}

void FHWModelRenderer::BeginDrawModel(FRenderStyle style, int smf_flags, const VSMatrix &objectToWorldMatrix, bool mirrored)
{
	state.SetDepthFunc(DF_LEqual);
	state.EnableTexture(true);
	// [BB] In case the model should be rendered translucent, do back face culling.
	// This solves a few of the problems caused by the lack of depth sorting.
	// [Nash] Don't do back face culling if explicitly specified in MODELDEF
	// TO-DO: Implement proper depth sorting.
	if ((smf_flags & MDL_FORCECULLBACKFACES) || (!(style == DefaultRenderStyle()) && !(smf_flags & MDL_DONTCULLBACKFACES)))
	{
		state.SetCulling((mirrored ^ portalState.isMirrored()) ? Cull_CCW : Cull_CW);
	}

	state.mModelMatrix = objectToWorldMatrix;
	state.EnableModelMatrix(true);

	// RS fork -- kept so SetSurfaceTransform can compose in front of it and
	// then put it back for the next surface.
	baseModelMatrix = objectToWorldMatrix;
	baseModelMatrixValid = true;
}

void FHWModelRenderer::EndDrawModel(FRenderStyle style, int smf_flags)
{
	if (eyeFadeOn) SetEyeFade(0.f, 0.f);	// RS fork -- never leave the fade on past its model
	state.SetBoneIndexBase(-1);
	state.EnableModelMatrix(false);
	state.SetDepthFunc(DF_Less);
	if ((smf_flags & MDL_FORCECULLBACKFACES) || (!(style == DefaultRenderStyle()) && !(smf_flags & MDL_DONTCULLBACKFACES)))
		state.SetCulling(Cull_None);
}

// RS fork -- NEAR-EYE FADE (modelrenderer.h). EFF_EYEFADE reads its two distances from the
// per-draw stream data, which only the Vulkan prolog maps, so it is selected on Vulkan only;
// GL and GLES draw the model whole. Never over an effect a caller already set.
void FHWModelRenderer::SetEyeFade(float nearDist, float farDist)
{
	const bool want = farDist > nearDist && nearDist >= 0.f && screen != nullptr && screen->IsVulkan();
	if (want)
	{
		if (!eyeFadeOn && state.GetSpecialEffect() != EFF_NONE) return;
		state.SetEffect(EFF_EYEFADE);
		state.SetEyeFade(nearDist, farDist);
		eyeFadeOn = true;
	}
	else if (eyeFadeOn)
	{
		state.SetEffect(EFF_NONE);
		state.SetEyeFade(0.f, 0.f);
		eyeFadeOn = false;
	}
}

void FHWModelRenderer::BeginDrawHUDModel(FRenderStyle style, const VSMatrix &objectToWorldMatrix, bool mirrored, int smf_flags)
{
	state.SetDepthFunc(DF_LEqual);
	state.SetDepthClamp(true);
	state.SetTextureModeFlagsExtra(TEXF_FlipNormal);
	
	/* hack the depth range to prevent view model from poking into walls */
    gldepthmin = 0;
    gldepthmax = 1;
    state.SetDepthRange(gldepthmin, gldepthmin + 0.3 * (gldepthmax - gldepthmin));

	// [BB] In case the model should be rendered translucent, do back face culling.
	// This solves a few of the problems caused by the lack of depth sorting.
	// TO-DO: Implement proper depth sorting.
	if (!(style == DefaultRenderStyle()) || (smf_flags & MDL_FORCECULLBACKFACES))
	{
		state.SetCulling((mirrored ^ portalState.isMirrored()) ? Cull_CW : Cull_CCW);
	}

	state.mModelMatrix = objectToWorldMatrix;
	state.EnableModelMatrix(true);

	// RS fork -- see the world-model path above.
	baseModelMatrix = objectToWorldMatrix;
	baseModelMatrixValid = true;
}

void FHWModelRenderer::EndDrawHUDModel(FRenderStyle style, int smf_flags)
{
	state.SetBoneIndexBase(-1);
	state.EnableModelMatrix(false);
	state.SetTextureModeFlagsExtra(0);

	state.SetDepthFunc(DF_Less);
	if (!(style == DefaultRenderStyle()) || (smf_flags & MDL_FORCECULLBACKFACES))
		state.SetCulling(Cull_None);

	state.SetDepthRange(gldepthmin, gldepthmax);
}

IModelVertexBuffer *FHWModelRenderer::CreateVertexBuffer(bool needindex, bool singleframe)
{
	return new FModelVertexBuffer(needindex, singleframe);
}

void FHWModelRenderer::SetInterpolation(double inter)
{
	state.SetInterpolationFactor((float)inter);
}

void FHWModelRenderer::SetMaterial(FGameTexture *skin, bool clampNoFilter, FTranslationID translation)
{
	state.SetMaterial(skin, UF_Skin, 0, clampNoFilter ? CLAMP_NOFILTER : CLAMP_NONE, translation, -1);
	state.SetLightIndex(modellightindex);
}

void FHWModelRenderer::DrawArrays(int start, int count)
{
	state.Draw(DT_Triangles, start, count);
}

void FHWModelRenderer::DrawElements(int numIndices, size_t offset)
{
	state.DrawIndexed(DT_Triangles, int(offset / sizeof(unsigned int)), numIndices);
}

//===========================================================================
//
//
//
//===========================================================================

void FHWModelRenderer::SetupFrame(FModel *model, unsigned int frame1, unsigned int frame2, unsigned int size, int boneStartIndex)
{
	auto mdbuff = static_cast<FModelVertexBuffer*>(model->GetVertexBuffer(GetType()));
	//boneIndexBase = boneStartIndex;//boneStartIndex >= 0 ? boneStartIndex : screen->mBones->UploadBones(bones);
	state.SetBoneIndexBase(boneStartIndex);
	if (mdbuff)
	{
		state.SetVertexBuffer(mdbuff->vertexBuffer(), frame1, frame2);
		if (mdbuff->indexBuffer()) state.SetIndexBuffer(mdbuff->indexBuffer());
	}
}

// RS FORK -- ONE SURFACE, MOVED, WITHOUT MOVING THE MODEL.
//
// Composed in FRONT of the model's own object-to-world, so the transform is
// in the MODEL's local space: a translation of (0, 0, -4) moves the part four
// units down the model's own axes, wherever and however the model itself is
// oriented in the world. Applying it the other way round would move the part
// along world axes and every offset would be wrong the moment the weapon was
// canted -- the same trap the grab points in this project's scripts already
// document at length.
//
// nullptr restores the model's matrix for the surfaces that follow. Cheap
// enough to call per surface; the multiply only happens for surfaces that
// actually asked for a transform.
void FHWModelRenderer::SetSurfaceTransform(const VSMatrix* localTransform)
{
	if (!baseModelMatrixValid) return;

	if (localTransform)
	{
		VSMatrix m = baseModelMatrix;
		m.multMatrix(*localTransform);
		state.mModelMatrix = m;
	}
	else
	{
		state.mModelMatrix = baseModelMatrix;
	}
	state.EnableModelMatrix(true);
}

// RS FORK -- hand the model's own object-to-world back out, for the draw-rate
// hand drive. This is the matrix BeginDrawModel was given; the fill loop needs
// it to put a live controller position into model space, and by then it is the
// only place it still exists.
bool FHWModelRenderer::GetModelToWorldMatrix(VSMatrix* out) const
{
	if (!out || !baseModelMatrixValid) return false;
	*out = baseModelMatrix;
	return true;
}
