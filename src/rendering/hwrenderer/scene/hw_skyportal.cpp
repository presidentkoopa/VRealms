/*
** hw_skyportal.cpp
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 2003-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include "doomtype.h"
#include "g_level.h"
#include "filesystem.h"
#include "r_state.h"
#include "r_utility.h"
#include "g_levellocals.h"
#include "hw_cvars.h"
#include "hw_skydome.h"
#include "hwrenderer/scene/hw_portal.h"
#include "hw_renderstate.h"
#include "skyboxtexture.h"

std::pair<PalEntry, PalEntry>& R_GetSkyCapColor(FGameTexture* tex);

// RS FORK -- draw the sky on a world-locked CYLINDER instead of GZDoom's dome.
// FSkyVertexBuffer::CreateRothSky says what the projection is, why an engine
// would want it, and why a dome cannot express it.
//
// A cvar so the two can be compared without a reload, and off by default so no
// existing map changes. A map that wants it sets it at load; Realms does.
//
// This replaces an earlier attempt that tried to cancel the viewer's pitch by
// ROTATING the dome. It could never have worked: on a dome v is a function of
// the sampled direction's elevation, so rotating it relocates the pole and its
// grey cap disc without removing either, and leaves v non-linear in screen row.
CVAR(Bool, r_skyband, false, CVAR_ARCHIVE)

//-----------------------------------------------------------------------------
//
//
//
//-----------------------------------------------------------------------------
void HWSkyPortal::DrawContents(HWDrawInfo *di, FRenderState &state)
{
	// [BB] A sky IS the outdoor case, so it takes the outdoor fog scale rather
	// than whatever the last wall or flat happened to leave behind.
	if (di->Level != nullptr)
		state.SetFogDensityScale((float)di->Level->FogOutdoorScale);
	bool drawBoth = false;
	auto &vp = di->Viewpoint;

	// We have no use for Doom lighting special handling here, so disable it for this function.
	auto oldlightmode = di->lightmode;
	if (isSoftwareLighting(oldlightmode))
	{
		di->SetFallbackLightMode();
		state.SetNoSoftLightLevel();
	}

	if (!gl_skydome)
	{
		FGameTexture* skytex = origin->texture[0] ? origin->texture[0] : origin->texture[1];
		if (skytex != nullptr)
		{
			auto& col = R_GetSkyCapColor(skytex);
			state.SetSceneColor(col.first);
			state.InitSceneClearColor();
		}
		di->lightmode = oldlightmode;
		return;
	}

	state.ResetColor();
	state.EnableFog(false);
	state.AlphaFunc(Alpha_GEqual, 0.f);
	state.SetRenderStyle(STYLE_Translucent);
	bool oldClamp = state.SetDepthClamp(true);

	di->SetupView(state, 0, 0, 0, !!(mState->MirrorFlag & 1), !!(mState->PlaneMirrorFlag & 1));
	di->RemoveMultiviewPositionParallax();
	di->ApplyViewpoint(state);

	state.SetVertexBuffer(vertexBuffer);
	auto skybox = origin->texture[0] ? dynamic_cast<FSkyBox*>(origin->texture[0]->GetTexture()) : nullptr;
	if (skybox)
	{
		vertexBuffer->RenderBox(state, skybox, origin->x_offset[0], origin->sky2, di->Level->info->pixelstretch, di->Level->info->skyrotatevector, di->Level->info->skyrotatevector2);
	}
	else
	{
		if (origin->texture[0]==origin->texture[1] && origin->doublesky) origin->doublesky=false;

		// RS FORK -- the world-locked cylinder. It takes no viewpoint and no
		// matrices: both of its laws are already in the vertex UVs, which is
		// what makes it exact, and what makes it correct in VR for free, since
		// nothing in it depends on where the camera is.
		if (r_skyband && origin->texture[0])
		{
			state.SetTextureMode(TM_OPAQUE);
			vertexBuffer->RenderRothSky(state, origin->texture[0]);
			state.SetTextureMode(TM_NORMAL);
		}
		else if (origin->texture[0])
		{
			state.SetTextureMode(TM_OPAQUE);
			vertexBuffer->RenderDome(state, origin->texture[0], origin->x_offset[0], origin->y_offset, origin->mirrored, FSkyVertexBuffer::SKYMODE_MAINLAYER, !!(di->Level->flags & LEVEL_FORCETILEDSKY));
			state.SetTextureMode(TM_NORMAL);
		}

		state.AlphaFunc(Alpha_Greater, 0.f);

		if (origin->doublesky && origin->texture[1])
		{
			vertexBuffer->RenderDome(state, origin->texture[1], origin->x_offset[1], origin->y_offset, false, FSkyVertexBuffer::SKYMODE_SECONDLAYER, !!(di->Level->flags & LEVEL_FORCETILEDSKY));
		}
	}

	if (di->Level->skyfog>0 && (origin->fadecolor & 0xffffff) != 0)
	{
		PalEntry FadeColor = origin->fadecolor;
		FadeColor.a = clamp<int>(di->Level->skyfog, 0, 255);

		if (di->Level->flags3 & LEVEL3_SKYMIST && origin->texture[2])
		{
			float misth = origin->texture[2]->GetDisplayHeight();
			float myscale = di->Level->hw_skymistyscale;
			float myoffset = (myscale - 1.0)*0.857*misth; // [DVR] Why so many magic numbers when it comes to sky??
			vertexBuffer->RenderDome(state, origin->texture[2], origin->x_offset[2], myoffset, false, FSkyVertexBuffer::SKYMODE_FOGLAYER, !!(di->Level->flags & LEVEL_FORCETILEDSKY), 0, (myscale == 0.0 ? 0 : 240.0/misth/myscale), FadeColor);
		}
		else if (!di->isFullbrightScene())
		{
			state.EnableTexture(false);
			state.SetObjectColor(FadeColor);
			state.Draw(DT_Triangles, 0, 12);
			state.EnableTexture(true);
			state.SetObjectColor(0xffffffff);
		}
	}
	di->lightmode = oldlightmode;
	state.SetDepthClamp(oldClamp);
}

const char *HWSkyPortal::GetName() { return "Sky"; }
