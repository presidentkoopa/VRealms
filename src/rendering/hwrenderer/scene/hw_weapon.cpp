/*
** hw_weapon.cpp
**
** Weapon sprite utilities
**
**---------------------------------------------------------------------------
**
** Copyright 2000-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include "sbar.h"
#include "r_utility.h"
#include "v_video.h"
#include "palutil.h"
#include "i_time.h"
#include "doomstat.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "models.h"
#include "hw_weapon.h"
#include "hw_fakeflat.h"
#include "texturemanager.h"
#include "d_net.h"

#include "hw_models.h"
#include "hw_dynlightdata.h"
#include "hw_material.h"
#include "hw_lighting.h"
#include "hw_cvars.h"
#include "hw_vrwheel.h"
#include "hw_vrmodes.h"
#include "hwrenderer/scene/hw_drawinfo.h"
#include "hwrenderer/scene/hw_drawstructs.h"
#include "flatvertices.h"
#include "hw_lightbuffer.h"
#include "hw_renderstate.h"
#include "hw_effectlightbuffer.h"	// [EFFECTLIGHTS] EffectLightBuffer::Instance()->GetLiveCount(): the per-draw effect-light gate
#include "textures.h"
#include "menu.h"
#include <algorithm>
#include <cmath>
#include "playsim/p_local.h"
#include "playsim/p_hitscantracer.h"
#include "playsim/p_linetracedata.h"
#include "playsim/p_trace.h"

#include "vm.h"
#include "types.h"		// VARF_Meta / PField, for the safe weapon-field read below

EXTERN_CVAR(Float, transsouls)
EXTERN_CVAR(Int, gl_fuzztype)
EXTERN_CVAR(Bool, gl_texture_thread)
EXTERN_CVAR(Bool, r_drawplayersprites)
EXTERN_CVAR(Bool, r_deathcamera)
EXTERN_CVAR(Bool, r_visualstate_log)	// RS fork: defined in vmthunks.cpp
EXTERN_CVAR(Bool, vr_laser_sight)
EXTERN_CVAR(Bool, vr_laser_show_melee)
EXTERN_CVAR(Bool, vr_laser_hide_on_wheel)
EXTERN_CVAR(Bool, vr_laser_beam)
EXTERN_CVAR(Bool, vr_laser_other_players_beam)
EXTERN_CVAR(Bool, vr_laser_other_players_pointer)
EXTERN_CVAR(Color, vr_laser_color)
EXTERN_CVAR(Float, vr_laser_beam_alpha)
EXTERN_CVAR(Float, vr_laser_beam_width)
EXTERN_CVAR(Float, vr_laser_pointer_scale)
EXTERN_CVAR(Float, vr_laser_pointer_alpha)
EXTERN_CVAR(Int, vr_laser_pointer_glow)
EXTERN_CVAR(Float, vr_laser_pointer_glow_scale)
EXTERN_CVAR(Float, vr_laser_pointer_glow_intensity)
EXTERN_CVAR(Int, vr_laser_beam_length)
EXTERN_CVAR(Int, vr_laser_fixed_length)
EXTERN_CVAR(Float, vr_laser_source_offset_x)
EXTERN_CVAR(Float, vr_laser_source_offset_y)
EXTERN_CVAR(Float, vr_laser_source_offset_z)
EXTERN_CVAR(Float, vr_laser_beam_glow)
EXTERN_CVAR(Float, vr_laser_beam_emissive)
EXTERN_CVAR(Float, vr_laser_beam_taper)
EXTERN_CVAR(Float, vr_laser_beam_fade)
EXTERN_CVAR(Bool, vr_laser_color_cycle)
EXTERN_CVAR(Float, vr_laser_color_cycle_speed)
EXTERN_CVAR(Bool, vr_laser_color_cycle_dot)
EXTERN_CVAR(Bool, vr_laser_headshot_react)
EXTERN_CVAR(Color, vr_laser_headshot_color)
EXTERN_CVAR(Bool, vr_laser_headshot_pulse)
EXTERN_CVAR(Float, vr_laser_headshot_pulse_speed)
EXTERN_CVAR(Bool, vr_laser_lock)
EXTERN_CVAR(Float, vr_laser_lock_tighten)
EXTERN_CVAR(Float, vr_laser_lock_rate)
EXTERN_CVAR(Float, vr_laser_dot_range)
EXTERN_CVAR(Int, vr_laser_color_mode)
EXTERN_CVAR(Color, vr_laser_dot_color)
EXTERN_CVAR(Color, vr_laser_dot_color_offhand)
EXTERN_CVAR(Color, vr_laser_color_offhand)
EXTERN_CVAR(Bool, vr_laser_color_per_slot)
EXTERN_CVAR(Color, vr_laser_color_slot1)
EXTERN_CVAR(Color, vr_laser_color_slot2)
EXTERN_CVAR(Color, vr_laser_color_slot3)
EXTERN_CVAR(Color, vr_laser_color_slot4)
EXTERN_CVAR(Color, vr_laser_color_slot5)
EXTERN_CVAR(Color, vr_laser_color_slot6)
EXTERN_CVAR(Color, vr_laser_color_slot7)
EXTERN_CVAR(Color, vr_laser_color_slot8)
EXTERN_CVAR(Color, vr_laser_color_slot9)
EXTERN_CVAR(Color, vr_laser_color_slot0)
EXTERN_CVAR(Color, vr_hitscan_tracer_color)
EXTERN_CVAR(Float, vr_hitscan_tracer_alpha)
EXTERN_CVAR(Float, vr_hitscan_tracer_length)
EXTERN_CVAR(Float, vr_hitscan_tracer_width)
EXTERN_CVAR(Float, vr_hitscan_tracer_speed)

extern float hmdorientation[3];
extern float weaponangles[3];
extern float offhandangles[3];
extern float doomYaw;
//To force translucency for weapon sprites, tex->GetTranslucency returns false result for 32 bit PNG
CVAR(Bool, r_transparentPlayerSprites, true, CVAR_ARCHIVE)

EXTERN_CVAR(Int, r_PlayerSprites3DMode)
EXTERN_CVAR(Float, r_hudflatoverlay)
EXTERN_CVAR(Float, gl_fatItemWidth)

enum PlayerSprites3DMode
{
	CROSSED,
	BACK_ONLY,
	ITEM_ONLY,
	FAT_ITEM,
};

static bool WeaponSpriteMatches(AActor* equippedWeapon, AActor* spriteCaller);


CVARD(Bool, gl_weapon_purelightlevel, false, CVAR_GLOBALCONFIG | CVAR_ARCHIVE, "[This feature is temporarily disabled] Makes the lighting on weapon sprites (or models) purely match the sector's light level you're standing in");

//==========================================================================
//
// R_DrawPSprite
//
//==========================================================================

void HWDrawInfo::DrawPSprite(HUDSprite *huds, FRenderState &state)
{
	if (huds->RenderStyle.BlendOp == STYLEOP_Shadow)
	{
		state.SetColor(0.2f, 0.2f, 0.2f, 0.33f, huds->cm.Desaturation);
	}
	else
	{
		SetColor(state, Level, lightmode, huds->lightlevel, 0, isFullbrightScene(), huds->cm, huds->alpha, true);
	}
	state.SetLightIndex(-1);
	state.SetRenderStyle(huds->RenderStyle);
	state.SetTextureMode(huds->RenderStyle);
	state.SetObjectColor(huds->ObjectColor);
	// RS fork: the psprite's own additive term rides on top of the
	// sector's. Set before RenderHUDModel below and never cleared by the
	// model renderer, so a 3D weapon model receives it.
	{
		PalEntry add = huds->owner->Sector
			? PalEntry(huds->owner->Sector->AdditiveColors[sector_t::sprites] | 0xff000000)
			: PalEntry(0);
		const PalEntry g = huds->AddColor;
		if (g.r | g.g | g.b)
		{
			add.r = min<int>(255, add.r + g.r);
			add.g = min<int>(255, add.g + g.g);
			add.b = min<int>(255, add.b + g.b);
			add.a = 255;
		}
		state.SetAddColor(add);
	}
	// [BB] THE PSPRITE PATH TAKES THE SAME EXEMPTION THE SPRITE PATH DOES.
	//
	// Set only in HWSprite::DrawSprite before this, so the Spare-actors setting
	// reached world actors and stopped at anything drawn as a player sprite --
	// while the render state's default left psprites taking the darkening in
	// full. That is invisible until you can switch between the two, and the VR
	// hands can be either: world actors or psprites, toggled at runtime. The
	// same hands would darken differently depending on which mode they were in,
	// with nothing to explain why.
	//
	// And, like that path, a FULLBRIGHT frame is exempt outright (hw_sprites.cpp
	// uses fullbright ? 1 : DarkActorExempt). Without it a muzzle flash was
	// darkened on screen while the same flash as a world sprite was not.
	state.SetDarknessExempt(huds->bright ? 1.f : (float)Level->DarkActorExempt);
	// Diagnostic (r_visualstate_log): print when the psprite exemption changes
	// while darkness is on, so a test shows a muzzle flash reaching 1.00.
	if (r_visualstate_log && Level->DarkMode > 0)
	{
		static float lastExempt = -1.f;
		const float ex = huds->bright ? 1.f : (float)Level->DarkActorExempt;
		if (ex != lastExempt)
		{
			lastExempt = ex;
			Printf("psprite darkness exempt %.2f (%s frame)\n", ex, huds->bright ? "fullbright" : "lit");
		}
	}

	state.SetDynLight(huds->dynrgb[0], huds->dynrgb[1], huds->dynrgb[2]);
	state.EnableBrightmap(!(huds->RenderStyle.Flags & STYLEF_ColorIsFixed));

	if (huds->mframe)
	{
		// RS FORK -- SKIN ALPHA THAT IS NOT OPACITY.
		//
		// Model skins are alpha-tested here, so any texel below the mask
		// threshold is discarded. That is correct for a skin whose alpha means
		// transparency, and catastrophic for one where it does not: PBR texture
		// sets routinely pack roughness or gloss into the alpha channel, and
		// such a channel is mostly dark, so most of the weapon fails the test
		// and is thrown away. It presents as a gun that is largely invisible
		// with a few solid patches -- which reads as a broken mesh or a broken
		// export, and is neither.
		//
		// IgnoreSkinAlpha drops the threshold to zero for that model, so
		// nothing is discarded and the alpha channel is simply unused. The
		// alternative is stripping the alpha channel out of every texture of
		// every ripped weapon by hand, forever.
		state.AlphaFunc(Alpha_GEqual, huds->mframe->ignoresSkinAlpha() ? 0.f : gl_mask_threshold);

		// [EFFECTLIGHTS] Effect lights on the weapon model ("Engine docs/EFFECT_LIGHTS_LB_IMPL_NOTES.md"): mode 1, lit with N.L,
		// when the model takes dynamic light today (DrawPlayerSprites: not a fuzz shadow, not a fullbright scene, gl_light_weapons,
		// its owner not asking for no dynamic light), whether or not the level has dynamic lights. Set only on frames with an
		// effect light on the GPU and put back to 0 after the model, so every draw on a frame with none uploads exactly what it did.
		const int effectLightMode = (EffectLightBuffer::Instance() != nullptr && EffectLightBuffer::Instance()->GetLiveCount() > 0 &&
			huds->RenderStyle.BlendOp != STYLEOP_Shadow && !isFullbrightScene() && gl_light_weapons &&
			huds->owner != nullptr && !(huds->owner->renderflags2 & RF2_NODYNAMICLIGHTING)) ? 1 : 0;
		if (effectLightMode != 0) state.SetEffectLightMode(effectLightMode);
		FHWModelRenderer renderer(this, state, huds->lightindex);
		RenderHUDModel(&renderer, huds->weapon, huds->translation, huds->rotation + FVector3(huds->mx / 4., (huds->my - WEAPONTOP) / -4., 0), huds->pivot, huds->mframe, Net_ModifyObjectFrac(huds->weapon, Viewpoint.TicFrac));
		state.SetVertexBuffer(screen->mVertexData);
		if (effectLightMode != 0) state.SetEffectLightMode(0);	// [EFFECTLIGHTS]
	}
	else
	{
		auto vrmode = VRMode::GetVRModeCached(true);
		float thresh = (huds->texture->GetTranslucency() || huds->OverrideShader != -1) && !vrmode->IsVR() ? 0.f : gl_mask_sprite_threshold;
		state.AlphaFunc(Alpha_GEqual, thresh);
		FTranslationID trans = huds->weapon->GetTranslation();
		if ((huds->weapon->Flags & PSPF_PLAYERTRANSLATED)) trans = huds->owner->Translation;
		
		if (r_PlayerSprites3DMode != ITEM_ONLY && r_PlayerSprites3DMode != FAT_ITEM)
		{
			state.SetMaterial(huds->texture, UF_Sprite, CTF_Expand, CLAMP_XY_NOMIP, trans, huds->OverrideShader);
			state.Draw(DT_TriangleStrip, huds->mx, 4);
		}
		
		DPSprite* psp = huds->weapon;
		FTextureID lump;
		bool mirror;
		if (psp->GetCaller() != nullptr)
		{
			FState* spawn = psp->GetCaller()->FindState(NAME_Spawn);
			lump = sprites[spawn->sprite].GetSpriteFrame(0, 0, nullAngle, &mirror);
		}
		else lump.SetNull();

		auto gtex = TexMan.GetGameTexture(lump, false);
		FMaterial* tex = FMaterial::ValidateTexture(gtex, true, false);

		//TODO Cleanup code for rendering weapon models from sprites in VR mode
		if ((psp->GetID() == PSP_WEAPON || psp->GetID() == PSP_OFFHANDWEAPON) 
		&& vrmode->IsVR()
		&& r_PlayerSprites3DMode != BACK_ONLY
		&& psp->GetCaller() != nullptr
		&& tex != nullptr
		&& lump.isValid())
		{
			float vw = (float)viewwidth;
			float vh = (float)viewheight;

			state.AlphaFunc(Alpha_GEqual, 1);
			state.SetMaterial(gtex, UF_Sprite, CTF_Expand, CLAMP_XY_NOMIP, trans, huds->OverrideShader);
			
			auto spi = gtex->GetSpritePositioning(0);

			float fU1, fV1;
			float fU2, fV2;
			float z1 = 0.0f;
			float z2 = (huds->y2 - huds->y1) * std::min(3, spi.spriteWidth / spi.spriteHeight);

			if (!(mirror) != !(psp->Flags & PSPF_FLIP))
			{
				fU2 = spi.GetSpriteUL();
				fV1 = spi.GetSpriteVT();
				fU1 = spi.GetSpriteUR();
				fV2 = spi.GetSpriteVB();
			}
			else
			{
				fU1 = spi.GetSpriteUL();
				fV1 = spi.GetSpriteVT();
				fU2 = spi.GetSpriteUR();
				fV2 = spi.GetSpriteVB();
			}

			if (r_PlayerSprites3DMode == FAT_ITEM)
			{
				float x1 = vw / 2 + (huds->x1 - vw / 2) * gl_fatItemWidth;
				float x2 = vw / 2 + (huds->x2 - vw / 2) * gl_fatItemWidth;

				float inc = (x2 - x1) / 12.0f;
				for (float x = x1; x < x2; x += inc)
				{
					screen->mVertexData->Map();
					auto vert = screen->mVertexData->AllocVertices(4);
					auto vp = vert.first;
					vp[0].Set(x, huds->y1, -z1, fU1, fV1);
					vp[1].Set(x, huds->y2, -z1, fU1, fV2);
					vp[2].Set(x, huds->y1, -z2, fU2, fV1);
					vp[3].Set(x, huds->y2, -z2, fU2, fV2);
					screen->mVertexData->Unmap();
					state.Draw(DT_TriangleStrip, vert.second, 4, x == x1);
				}
			}
			else
			{
				float sy;
				float crossAt;
				if (r_PlayerSprites3DMode == ITEM_ONLY)
				{
					crossAt = 0.0f;
					sy = 0.0f;
				}
				else
				{
					sy = huds->y2 - huds->y1;
					crossAt = sy * 0.25f;
				}

				float y1 = huds->y1 - crossAt;
				float y2 = huds->y2 - crossAt;

				screen->mVertexData->Map();
				auto vert = screen->mVertexData->AllocVertices(4);
				auto vp = vert.first;
				vp[0].Set(vw / 2 - crossAt, y1, -z1, fU1, fV1);
				vp[1].Set(vw / 2 + sy / 2, y2, -z1, fU1, fV2);
				vp[2].Set(vw / 2 - crossAt, y1, -z2, fU2, fV1);
				vp[3].Set(vw / 2 + sy / 2, y2, -z2, fU2, fV2);

				auto vert2 = screen->mVertexData->AllocVertices(4);
				auto vp2 = vert2.first;
				vp2[0].Set(vw / 2 + crossAt, y1, -z1, fU1, fV1);
				vp2[1].Set(vw / 2 - sy / 2, y2, -z1, fU1, fV2);
				vp2[2].Set(vw / 2 + crossAt, y1, -z2, fU2, fV1);
				vp2[3].Set(vw / 2 - sy / 2, y2, -z2, fU2, fV2);
				
				screen->mVertexData->Unmap();
				state.Draw(DT_TriangleStrip, vert.second, 4, true);
				state.Draw(DT_TriangleStrip, vert2.second, 4, false);
			}
		}
	}

	state.SetTextureMode(TM_NORMAL);
	state.AlphaFunc(Alpha_GEqual, gl_mask_sprite_threshold);
	state.SetObjectColor(0xffffffff);
	state.SetAddColor(0);
	state.SetDynLight(0, 0, 0);
	state.EnableBrightmap(false);
}

void HWDrawInfo::DrawHudQuad(FRenderState& state, FGameTexture* texture, float width, float height, float xoffset, float yoffset, bool flipX, bool depthMask)
{
	if (texture == nullptr || width <= 0.0f || height <= 0.0f)
	{
		return;
	}

	texture->SetTranslucent(true);

	FRenderStyle hudQuadStyle = LegacyRenderStyles[STYLE_Translucent];
	if (texture->isHardwareCanvas())
	{
		auto* canvasTex = static_cast<FCanvasTexture*>(texture->GetTexture());
		if (canvasTex != nullptr && canvasTex->bTranslucentCanvas)
		{
			// The portable HUD surface is rendered into transparent black first,
			// so its RGB is effectively premultiplied by alpha. Compose it with
			// premultiplied blending here to match the regular camera HUD.
			hudQuadStyle.SrcAlpha = STYLEALPHA_One;
			hudQuadStyle.DestAlpha = STYLEALPHA_InvSrc;
		}
	}

	state.SetLightIndex(-1);
	state.SetRenderStyle(hudQuadStyle);
	state.AlphaFunc(Alpha_Greater, 0.0f);
	state.SetTextureMode(TM_NORMAL);
	state.ResetColor();
	state.SetObjectColor(0xffffffff);
	state.SetAddColor(0);
	state.SetDynLight(0, 0, 0);
	state.SetNoSoftLightLevel();
	state.SetLightParms(1.f, 0.f);
	state.EnableFog(false);
	state.SetFog(0, 0);
	state.ResetFadeColor();
	state.EnableTextureMatrix(false);
	state.EnableBrightmap(false);
	// Mounted HUD is a world-space quad, and it is UI: it reads as broken when a wall or a monster
	// clips through your own wrist panel. It draws on top, like every other HUD element (Ermac's
	// DoomXR c48d266, "fixed render order of portable HUD and weapon/item wheel to be top most like
	// regular UI"; the draw order that makes this safe -- after the tracers and the laser -- is
	// already ours). `depthMask` is kept in the signature for callers that still pass it; nothing
	// writes depth here, because an overlay that wrote depth would punch a hole in the scene behind it.
	state.EnableDepthTest(false);
	state.SetDepthMask(false);
	state.SetMaterial(texture, UF_Sprite, CTF_Expand, CLAMP_XY_NOMIP, 0, -1);

	screen->mVertexData->Map();
	auto vert = screen->mVertexData->AllocVertices(4);
	auto vp = vert.first;
	const float halfWidth = width * 0.5f;
	const float halfHeight = height * 0.5f;
	float u0 = flipX ? 1.0f : 0.0f;
	float u1 = flipX ? 0.0f : 1.0f;
	vp[0].Set(xoffset - halfWidth, yoffset - halfHeight, 0.0f, u0, 0.0f);
	vp[1].Set(xoffset + halfWidth, yoffset - halfHeight, 0.0f, u1, 0.0f);
	vp[2].Set(xoffset - halfWidth, yoffset + halfHeight, 0.0f, u0, 1.0f);
	vp[3].Set(xoffset + halfWidth, yoffset + halfHeight, 0.0f, u1, 1.0f);
	screen->mVertexData->Unmap();

	state.Draw(DT_TriangleStrip, vert.second, 4);
}

void HWDrawInfo::DrawVRHudBorder(FRenderState& state, float width, float height, PalEntry color, float xoffset, float yoffset)
{
	if (width <= 0.0f || height <= 0.0f)
	{
		return;
	}

	state.SetLightIndex(-1);
	state.SetRenderStyle(STYLE_Source);
	state.SetTextureMode(TM_NORMAL);
	state.SetColor(color);
	state.SetObjectColor(0xffffffff);
	state.SetAddColor(0);
	state.EnableBrightmap(false);
	state.EnableDepthTest(false);
	state.SetDepthMask(false);
	state.EnableTexture(false);

	screen->mVertexData->Map();
	auto vert = screen->mVertexData->AllocVertices(4);
	auto vp = vert.first;
	const float halfWidth = width * 0.5f;
	const float halfHeight = height * 0.5f;
	vp[0].Set(xoffset - halfWidth, yoffset - halfHeight, 0.0f, 0.0f, 0.0f);
	vp[1].Set(xoffset + halfWidth, yoffset - halfHeight, 0.0f, 1.0f, 0.0f);
	vp[2].Set(xoffset - halfWidth, yoffset + halfHeight, 0.0f, 0.0f, 1.0f);
	vp[3].Set(xoffset + halfWidth, yoffset + halfHeight, 0.0f, 1.0f, 1.0f);
	screen->mVertexData->Unmap();

	state.Draw(DT_TriangleStrip, vert.second, 4);
	state.EnableTexture(true);
	state.SetDepthMask(true);
	state.EnableDepthTest(true);
}

struct FLaserBeamPoints
{
	DVector3 Start;
	DVector3 HitEnd;
	DVector3 BeamEnd;

	// TRUE WHEN THE SIGHT IS RESTING ON SOMETHING THAT CAN DIE.
	//
	// The trace already knew this and was throwing it away -- it runs with
	// MF_SHOOTABLE and FTraceResults carries both HitType and the Actor it
	// hit. All that was missing was somewhere to put the answer.
	//
	// It is what turns a laser sight from decoration into a threat: the dot
	// on a wall is a dot, and the dot on a monster tightens, brightens and
	// starts to breathe. That is the whole menace of a real laser sight, and
	// it is also honest information -- it tells you your shot connects
	// before you take it.
	bool OnTarget = false;

	// The actor OnTarget refers to, or null. Carried out of the trace so the
	// caller can publish it to script (AActor.LaserTraceTarget*) without a
	// second trace -- see the headshot line-up reaction below.
	AActor* HitActor = nullptr;
};

static DVector3 GetWeaponLaserBeamOffset(AActor* weapon)
{
	if (weapon == nullptr)
	{
		return DVector3(0.0, 0.0, 0.0);
	}

	auto* offset = (DVector3*)weapon->ScriptVar(NAME_LaserBeamOffset, nullptr);
	return offset != nullptr ? *offset : DVector3(0.0, 0.0, 0.0);
}

// ---------------------------------------------------------------------
// WHICH COLOUR THIS SIGHT IS. Four tiers, highest wins -- see the cvar
// block in hw_vrmodes.cpp for why the weapon outranks the player's
// preference.
//
// SLOTNUMBER IS A META FIELD (`meta int SlotNumber;`, weapons.zs:41), so
// it lives on the class rather than the instance. ScriptVar handles that
// -- it returns `cls->Meta + sym->Offset` for meta symbols -- so it is
// read exactly like an ordinary field and needs no special case here.
//
// DO NOT USE ScriptVar HERE. It looks like the obvious call and it is a
// trap for this particular job.
//
// ScriptVar (dobject.cpp:669) does not return null when a field is
// missing -- it calls I_Error, which is [[noreturn]]. So a weapon class
// that has no LaserBeamColor does not degrade, it takes the game down
// mid-frame.
//
// That is not hypothetical: it is exactly what happened the first time
// this shipped. Building the `zdoom` target alone relinks the exe but
// does NOT repack doomxr.pk3, so the engine went looking for a ZScript
// field that the stale pk3 had never heard of, and the sight crashed the
// instant it was switched on. Correct build order fixes that instance;
// reading the field safely fixes the whole class of it, including any
// mismatched exe/pk3 pair a user might ever end up with.
//
// A cosmetic sight colour is never worth a hard error. Missing field =
// fall through to the next tier.
static bool TryReadWeaponInt(AActor* weapon, FName field, int& out)
{
	if (weapon == nullptr)
		return false;

	auto cls = weapon->GetClass();
	auto sym = dyn_cast<PField>(cls->FindSymbol(field, true));
	if (sym == nullptr)
		return false;

	// Meta fields live on the class, instance fields on the object --
	// mirroring ScriptVar's own branch. SlotNumber is meta; LaserBeamColor
	// is not, and this handles either without the caller caring.
	out = (sym->Flags & VARF_Meta)
		? *(int*)(cls->Meta + sym->Offset)
		: *(int*)(((char*)weapon) + sym->Offset);
	return true;
}

// `isDot` picks the pointer's colour rather than the beam's. Only mode 2
// makes them differ; below that both elements of a hand resolve the same,
// which is what keeps the lower modes feeling like one setting rather than
// two that have to be kept in sync by hand.
// COLOUR CYCLING -- replaces a resolved colour's HUE with a slowly drifting
// one while keeping its saturation and brightness exactly as authored, so a
// dim pointer stays dim and a saturated one stays saturated as it cycles.
//
// PHASE IS PER TARGET (0=mainhand beam, 1=mainhand dot, 2=offhand beam,
// 3=offhand dot), spaced 90 degrees apart so all four start visibly
// different colours rather than four copies of the same drift.
//
// A colour with zero saturation -- a straight grey or white sight -- has no
// hue to speak of, so cycling one grows it out to a fully saturated colour
// rather than leaving it looking broken. That is a deliberate call: the
// whole point of turning this on is to see colour, and a cycling sight that
// stayed grey the entire time would look like the feature had failed.
static PalEntry CycleHue(PalEntry base, int target)
{
	float r = base.r / 255.0f, g = base.g / 255.0f, b = base.b / 255.0f;

	// RGBtoHSV is not declared in palutil.h -- only the reverse direction is
	// -- so value and a usable saturation are pulled by hand. Cheap: three
	// compares and a subtract, and it only runs when cycling is on.
	float mx = std::max(r, std::max(g, b));
	float mn = std::min(r, std::min(g, b));
	float v = mx;
	float s = (mx > 0.0001f) ? (mx - mn) / mx : 0.0f;
	if (s < 0.35f) s = 0.85f;   // grey/white input: give the cycle something to show

	const double speed = std::max(0.0, (double)vr_laser_color_cycle_speed);
	const double phase = target * 90.0;
	const double hue = std::fmod(I_msTimeF() * (speed / 1000.0) + phase, 360.0);

	float cr, cg, cb;
	HSVtoRGB(&cr, &cg, &cb, (float)hue, s, v);
	return PalEntry(base.a,
		(uint8_t)std::clamp(cr * 255.0f, 0.0f, 255.0f),
		(uint8_t)std::clamp(cg * 255.0f, 0.0f, 255.0f),
		(uint8_t)std::clamp(cb * 255.0f, 0.0f, 255.0f));
}

// Straight per-channel lerp toward a target colour, alpha untouched. Used by
// the headshot line-up reaction to blend toward vr_laser_headshot_color
// instead of hard-swapping it, so a pulsing reaction fades rather than pops.
static PalEntry LerpColor(PalEntry from, PalEntry to, float t)
{
	t = std::clamp(t, 0.0f, 1.0f);
	return PalEntry(from.a,
		(uint8_t)(from.r + (to.r - from.r) * t),
		(uint8_t)(from.g + (to.g - from.g) * t),
		(uint8_t)(from.b + (to.b - from.b) * t));
}

static int GetLaserBeamColorFor(AActor* weapon, bool offhand, bool isDot)
{
	// 1. THE WEAPON'S OWN. -1 means the author said nothing.
	//
	// An INT with a -1 sentinel rather than a Color field, because Color
	// has no spare value: the property parser routes colour strings through
	// V_GetColor (palette.cpp:757), which fills RGB and leaves alpha 0, so
	// PalEntry 0 means both "unset" and "black" and a weapon that genuinely
	// wanted a black sight would be indistinguishable from one that never
	// set the property. Weapon.SlotNumber -1 and HitscanTracerOffset -1.0
	// are the same trick, already in this file's neighbours.
	int weaponColor = -1;
	if (TryReadWeaponInt(weapon, NAME_LaserBeamColor, weaponColor) && weaponColor >= 0)
		return weaponColor;

	{
		// 2. PER SLOT.
		int slot = -1;
		if (vr_laser_color_per_slot && TryReadWeaponInt(weapon, NAME_SlotNumber, slot))
		{
			switch (slot)
			{
			case 1: return (int)vr_laser_color_slot1;
			case 2: return (int)vr_laser_color_slot2;
			case 3: return (int)vr_laser_color_slot3;
			case 4: return (int)vr_laser_color_slot4;
			case 5: return (int)vr_laser_color_slot5;
			case 6: return (int)vr_laser_color_slot6;
			case 7: return (int)vr_laser_color_slot7;
			case 8: return (int)vr_laser_color_slot8;
			case 9: return (int)vr_laser_color_slot9;
			case 0: return (int)vr_laser_color_slot0;
			default: break;   // -1, the "no slot" default: fall through
			}
		}
	}

	// 3. THE MODE LADDER. Each rung uses more of the same four cvars, so
	// moving up never invalidates a colour already chosen.
	switch (vr_laser_color_mode)
	{
	case 2:   // all four independent
		if (offhand)
			return isDot ? (int)vr_laser_dot_color_offhand : (int)vr_laser_color_offhand;
		return isDot ? (int)vr_laser_dot_color : (int)vr_laser_color;

	case 1:   // per hand; beam and dot match within a hand
		return offhand ? (int)vr_laser_color_offhand : (int)vr_laser_color;

	default:  // 0 -- one colour for everything
		break;
	}

	// 4. THE GLOBAL.
	return (int)vr_laser_color;
}

static DVector3 LaserAngleToVector(DAngle yaw, DAngle pitch)
{
	const double pc = pitch.Cos();
	return DVector3(pc * yaw.Cos(), pc * yaw.Sin(), -pitch.Sin());
}

static DVector3 GetLaserBeamControllerDirection(bool offhand)
{
	const float* controllerAngles = offhand ? offhandangles : weaponangles;
	const DAngle yaw = DAngle::fromDeg(doomYaw + controllerAngles[1] - hmdorientation[1]);
	return LaserAngleToVector(yaw, DAngle::fromDeg(controllerAngles[0]));
}

static DVector3 GetLaserBeamAttackDirection(player_t* player, bool offhand)
{
	if (player == nullptr || player->mo == nullptr)
	{
		return {};
	}

	const VRMode* vrmode = VRMode::GetVRModeCached(true);
	if (vrmode != nullptr && vrmode->IsVR() && player == &players[consoleplayer])
	{
		return GetLaserBeamControllerDirection(offhand);
	}

	auto* mo = player->mo;
	const DAngle aimYaw = (offhand ? mo->OffhandAngle : mo->AttackAngle) + DAngle::fromDeg(90.0);
	const DAngle aimPitch = -(offhand ? mo->OffhandPitch : mo->AttackPitch);
	return LaserAngleToVector(aimYaw, aimPitch);
}

static DVector3 GetLaserBeamAttackOrigin(player_t* player, bool offhand)
{
	if (player == nullptr || player->mo == nullptr)
	{
		return {};
	}

	auto* mo = player->mo;
	const DVector3 fallback = offhand ? mo->OffhandPos : mo->AttackPos;
	const VRMode* vrmode = VRMode::GetVRModeCached(true);
	if (vrmode == nullptr || !vrmode->IsVR() || player != &players[consoleplayer])
	{
		return fallback;
	}

	VSMatrix controllerTransform;
	if (!vrmode->GetWeaponTransform(&controllerTransform, offhand ? VR_OFFHAND : VR_MAINHAND))
	{
		return fallback;
	}

	const FLOATTYPE* controllerMatrix = controllerTransform.get();
	return DVector3(controllerMatrix[12], controllerMatrix[14], controllerMatrix[13]);
}

static bool GetLaserBeamEndpoints(player_t* player, AActor* weapon, bool offhand, FLaserBeamPoints& points)
{
	if (player == nullptr || player->mo == nullptr || !player->mo->OverrideAttackPosDir)
	{
		return false;
	}

	auto* mo = player->mo;
	const DVector3 direction = GetLaserBeamAttackDirection(player, offhand);
	const DVector3 base = GetLaserBeamAttackOrigin(player, offhand);
	const DVector3 weaponOffset = GetWeaponLaserBeamOffset(weapon);
	const bool isLocalPlayer = player == &players[consoleplayer];
	const DVector3 forward = direction;
	DVector3 side = DVector3(0.0, 0.0, 1.0) ^ forward;
	if (side.LengthSquared() < 1e-8)
	{
		side = DVector3(0.0, 1.0, 0.0);
	}
	side.MakeUnit();
	DVector3 up = forward ^ side;
	if (up.LengthSquared() < 1e-8)
	{
		up = DVector3(0.0, 0.0, 1.0);
	}
	up.MakeUnit();

	const DVector3 totalOffset = DVector3(
		(double)vr_laser_source_offset_y + weaponOffset.Y,
		(double)vr_laser_source_offset_x + weaponOffset.X,
		(double)vr_laser_source_offset_z + weaponOffset.Z);

	points.Start = base +
		forward * (isLocalPlayer ? 0.0 : 20.0) +
		forward * totalOffset.X +
		side * totalOffset.Y +
		up * totalOffset.Z;

	const double maxDistance = 8192.0;
	FTraceResults trace{};
	const bool hit = Trace(points.Start, mo->Sector, direction, maxDistance, MF_SHOOTABLE,
		ML_BLOCKEVERYTHING | ML_BLOCKHITSCAN | ML_BLOCKUSE, mo, trace, TRACE_NoSky);
	points.HitEnd = hit ? trace.HitPos : (points.Start + forward * maxDistance);

	// Only a live, shootable thing counts. A corpse is still an actor and
	// still gets traced against, so CountsAsKill/health are what separate
	// "aimed at a threat" from "aimed at the mess you already made".
	points.OnTarget = hit
		&& trace.HitType == TRACE_HitActor
		&& trace.Actor != nullptr
		&& trace.Actor != mo
		&& (trace.Actor->flags & MF_SHOOTABLE)
		&& !(trace.Actor->flags & MF_CORPSE)
		&& trace.Actor->health > 0;
	points.HitActor = points.OnTarget ? trace.Actor : nullptr;
	DVector3 beamVector = points.HitEnd - points.Start;
	double beamDistance = beamVector.Length();
	double visibleDistance = beamDistance;
	switch (vr_laser_beam_length)
	{
	case 1:
		visibleDistance *= 0.5;
		break;
	case 2:
		visibleDistance = std::min((double)vr_laser_fixed_length, beamDistance);
		break;
	default:
		break;
	}

	// [BB] A script-side in-world menu can terminate the beam at whatever it is
	// pointing at.
	//
	// The trace above only knows about level geometry and actors, so a laser aimed
	// at a billboard panel shoots straight through it and stops on the wall
	// behind -- which reads as the beam ignoring the very thing it is selecting.
	// Script already knows that distance, having just hit-tested for it, so it
	// hands it over rather than the engine learning about billboards.
	//
	// Shortening only. It can never make the beam longer than the world allows,
	// so this cannot be used to shoot a laser through a wall.
	//
	// Only the hand that asked for it. The other hand may have a perfectly
	// ordinary laser sight running off the player's own cvars, and cutting that
	// short at the menu's arm's-length distance would be baffling.
	const double scriptRange = VR_IsScriptLaserForcedFor(offhand) ? VR_GetScriptLaserRange() : 0.0;
	if (scriptRange > 0.0)
	{
		visibleDistance = std::min(visibleDistance, scriptRange);
	}

	if (beamDistance <= 0.01)
	{
		points.BeamEnd = points.Start;
		return true;
	}

	beamVector.MakeUnit();
	points.BeamEnd = points.Start + beamVector * visibleDistance;
	return true;
}

static void DrawLaserBeamGeometry(FRenderState& state, const DVector3& beamStart, const DVector3& beamEnd, const DVector3& hitEnd, bool drawBeam, bool drawPointer, bool onTarget, int beamColorIn, int dotColorIn)
{
	if (!drawBeam && !drawPointer)
	{
		return;
	}

	// ---- THE DOT GOES QUIET AT RANGE, UNLESS IT HAS FOUND SOMETHING ----
	//
	// Close up the dot is useful on anything -- it tells you where the shot
	// lands on a wall, a switch, a ledge. At distance that same dot is just
	// a bright speck riding over every far surface in the room, and it
	// clutters the one thing the sight exists to show you.
	//
	// So past vr_laser_dot_range the dot only draws when it is resting on
	// something shootable. Sweep across a far wall and there is nothing;
	// cross a monster or a barrel and a dot snaps onto it. The sight stops
	// being a cursor and becomes a detector.
	//
	// FADED, NOT CUT. A hard cutoff pops the dot in and out as you pan past
	// the threshold, which reads as a glitch. It fades across the last
	// quarter of the range instead, so the transition is something you
	// never notice happening.
	float dotVisibility = 1.0f;
	if (drawPointer && !onTarget)
	{
		const float dotRange = std::max(0.0f, (float)vr_laser_dot_range);
		if (dotRange > 0.0f)
		{
			const double dotDist = (hitEnd - beamStart).Length();
			const float fadeStart = dotRange * 0.75f;
			if (dotDist >= dotRange)
				dotVisibility = 0.0f;
			else if (dotDist > fadeStart)
				dotVisibility = 1.0f - (float)((dotDist - fadeStart) / (dotRange - fadeStart));
		}
	}
	if (dotVisibility <= 0.002f)
		drawPointer = false;

	DVector3 pointerCenter = hitEnd;
	if (drawPointer)
	{
		DVector3 pointerBackDir = hitEnd - beamStart;
		if (pointerBackDir.LengthSquared() > 1e-8)
		{
			pointerBackDir.MakeUnit();
			pointerCenter -= pointerBackDir * 4.0;
		}
	}

	const int beamColor = beamColorIn;
	// The pointer and its glow use their own colour; only mode 2 makes it
	// differ from the beam, but the split has to exist here regardless.
	const int dotColor  = dotColorIn;

	// ---- THE LOCK ------------------------------------------------------
	//
	// Everything that makes the sight threatening rides this one number.
	//
	// Off a target it is 0 and the sight behaves exactly as it always has,
	// so nobody's existing config changes meaning. On a living target it
	// runs 0..1 on a ~2.3Hz breath -- fast enough to read as agitation
	// rather than as a slow pulse, slow enough not to strobe.
	//
	// It drives three things at once, because one cue is a decoration and
	// three at once is a state change you feel before you consciously see:
	//   the dot TIGHTENS   (a spread dot is idle, a small one is aimed)
	//   the dot BRIGHTENS  (it burns rather than sits)
	//   the glow SWELLS    (something is about to happen here)
	//
	// r_viewpoint.TicFrac is added so the breath is smooth at any framerate
	// rather than stepping at 35Hz.
	const bool lockActive = onTarget && vr_laser_lock;
	const double lockTime = (double)(level.maptime) + r_viewpoint.TicFrac;
	const float lockPulse = lockActive
		? (float)(0.5 + 0.5 * std::sin(lockTime * std::max(0.01f, (float)vr_laser_lock_rate)))
		: 0.0f;
	const float lock = lockActive ? 1.0f : 0.0f;
	state.EnableModelMatrix(false);
	state.SetLightIndex(-1);
	state.AlphaFunc(Alpha_Greater, 0.0f);
	state.ResetColor();
	state.SetObjectColor(0xffffffff);
	state.SetAddColor(0);
	state.SetDynLight(0, 0, 0);
	state.SetNoSoftLightLevel();
	state.SetLightParms(1.f, 0.f);
	state.EnableFog(false);
	state.SetFog(0, 0);
	state.ResetFadeColor();
	state.EnableTextureMatrix(false);
	state.EnableBrightmap(false);
	state.EnableTexture(false);
	state.EnableDepthTest(true);
	state.SetDepthMask(false);
	const float beamAlpha = std::clamp<float>(vr_laser_beam_alpha, 0.0f, 1.0f);
	if (drawBeam && beamAlpha > 0.0f)
	{
		const bool beamOpaque = beamAlpha >= 0.999f;

		const DVector3 beamTarget = drawPointer ? pointerCenter : beamEnd;
		DVector3 beamVec = beamTarget - beamStart;
		const double beamLength = beamVec.Length();
		if (beamLength > 0.01)
		{
			beamVec.MakeUnit();

			DVector3 beamRight, beamUp;
			beamVec.GetRightUp(beamRight, beamUp);

			const float beamRadius = 0.25f * std::max(0.05f, (float)vr_laser_beam_width);

			// SIXTEEN SEGMENTS, NOT EIGHT. Eight is an octagon, and at the
			// distance a VR player holds a gun from their face the flats are
			// plainly visible -- the sight read as a faceted plastic rod
			// rather than as a beam. Sixteen is round at arm's length and is
			// still a rounding error next to the scene.
			constexpr int beamSegments = 16;
			const int vertexCount = (beamSegments + 1) * 2;

			// CORE AND HALO, which is the whole fix.
			//
			// The sight used to be ONE tube at one flat colour and one flat
			// alpha. That is a drawn line, and a drawn line looks like a
			// drawn line -- it was the single reason the sight looked cheap
			// next to everything else this fork renders.
			//
			// The POINTER at the end of it already knew better: it draws its
			// dot and then three or four concentric discs at falling alpha,
			// and it is the only part of the sight that reads as light. The
			// beam simply never got the same treatment. This gives it the
			// same one, with the same shape of numbers.
			//
			// It is also the exact principle main.fp states for the weapon
			// beams (section 13): "a hard narrow CORE a couple of units
			// across, and a wide soft HALO around it. One without the other
			// reads as either a drawn line or a smear; together they read as
			// something incandescent."
			//
			// Innermost pass carries the authored alpha so existing configs
			// still mean what they meant; the outer passes are additive glow
			// on top and cost two more triangle strips.
			// vr_laser_beam_glow scales the halo passes only. At 0 the core
			// is all that draws and the sight is exactly the flat tube it
			// used to be, which is the honest way to offer "turn it off".
			const float halo = std::max(0.0f, (float)vr_laser_beam_glow);

			struct BeamPass { float radius; float alpha; };
			const BeamPass passes[] = {
				{ 1.00f, 1.00f },                 // core: tight and bright
				{ 2.60f, 0.30f * halo },          // inner halo
				{ 5.20f, 0.11f * halo },          // outer bloom
			};

			// TAPERED. A parallel-sided tube reads as a rod; real glare is
			// tighter at the aperture and blooms toward what it lands on.
			// Only the halo passes taper -- the core stays straight so the
			// sight remains a precise thing to aim with, which is its job.
			const float kTaperStart = std::clamp<float>(vr_laser_beam_taper, 0.05f, 1.0f);

			// FADED ALONG ITS LENGTH, which is the part that was missing.
			//
			// Every pass used to be ONE triangle strip drawn at ONE colour, so
			// the beam was exactly as bright two thousand units out as it was
			// at the muzzle -- a bar bolted to the gun rather than a sight.
			// kTaperStart above only ever narrowed the halo, and the core, the
			// bright line you actually look at, never changed at all.
			//
			// The strip is now cut into steps along its length and each step
			// is drawn a little dimmer, so the beam runs out instead of either
			// continuing forever or stopping dead in mid-air the way
			// vr_laser_beam_length 2 does.
			//
			// ALPHA IS A FUNCTION OF DISTANCE, NOT OF THE FRACTION DRAWN. A
			// beam that ends early because it hit a near wall must arrive at
			// that wall at full strength; only a LONG beam should be faint at
			// its far end. Driving the falloff off t would fade every short
			// beam to nothing over a few feet.
			const double fadeLen = std::max(0.0f, (float)vr_laser_beam_fade);
			const bool   fading  = fadeLen > 1.0;
			const double drawLen = fading ? std::min(beamLength, fadeLen) : beamLength;

			// Eight is enough for additive blending to read as smooth; the
			// tail steps drop out under the alpha test below, so a beam that
			// fades early costs fewer draws, not more.
			constexpr int kLengthSteps = 8;

			for (const BeamPass& pass : passes)
			{
				const float passAlpha = std::clamp(beamAlpha * pass.alpha, 0.0f, 1.0f);
				if (passAlpha <= 0.002f)
					continue;

				const bool isCore = (pass.radius <= 1.0f);

				// Only a fully opaque CORE draws as solid; the halo is always
				// additive or it would punch a flat disc through the world.
				//
				// AND NOT EVEN THE CORE WHILE FADING -- STYLE_Source writes the
				// colour flat and ignores the alpha it is handed, so an opaque
				// core would be the one part of the beam that refused to fade,
				// leaving a hard line inside a fading glow.
				const bool solidCore = isCore && beamOpaque && !fading;
				state.SetRenderStyle(solidCore ? STYLE_Source : STYLE_Add);

				// OVERBRIGHT THE CORE PAST WHITE so the bloom pass picks it
				// up. The scene target is half-float HDR and the bloom
				// threshold is 1.0, so a channel above 1.0 bleeds on its own
				// -- emissive glow with no light in the light list. Core
				// only: an overbright halo would haze the whole view.
				const float em = isCore ? std::max(1.0f, (float)vr_laser_beam_emissive) : 1.0f;

				const float rEnd   = beamRadius * pass.radius;
				const float rStart = isCore ? rEnd : rEnd * kTaperStart;

				for (int s = 0; s < kLengthSteps; ++s)
				{
					const double t0 = (double)s / (double)kLengthSteps;
					const double t1 = (double)(s + 1) / (double)kLengthSteps;

					float stepAlpha = passAlpha;
					if (fading)
					{
						const double mid = drawLen * 0.5 * (t0 + t1);
						stepAlpha = passAlpha *
							(float)std::clamp(1.0 - mid / fadeLen, 0.0, 1.0);
					}
					if (stepAlpha <= 0.002f)
						continue;

					state.SetColor(RPART(beamColor) / 255.0f * em, GPART(beamColor) / 255.0f * em,
						BPART(beamColor) / 255.0f * em, stepAlpha);

					const double r0 = rStart + (rEnd - rStart) * t0;
					const double r1 = rStart + (rEnd - rStart) * t1;
					const DVector3 p0 = beamStart + beamVec * (drawLen * t0);
					const DVector3 p1 = beamStart + beamVec * (drawLen * t1);

					screen->mVertexData->Map();
					auto verts = screen->mVertexData->AllocVertices(vertexCount);
					auto vp = verts.first;
					for (int i = 0; i <= beamSegments; ++i)
					{
						const double t = (double)i / (double)beamSegments;
						const double ang = t * 6.28318530717958647692;
						const double cs = std::cos(ang);
						const double sn = std::sin(ang);
						const DVector3 dir = beamRight * cs + beamUp * sn;
						const DVector3 startPos = p0 + dir * r0;
						const DVector3 endPos = p1 + dir * r1;
						vp[i * 2 + 0].Set((float)startPos.X, (float)startPos.Z, (float)startPos.Y, 0.0f, 0.0f);
						vp[i * 2 + 1].Set((float)endPos.X, (float)endPos.Z, (float)endPos.Y, 0.0f, 1.0f);
					}
					screen->mVertexData->Unmap();

					state.Draw(DT_TriangleStrip, verts.second, vertexCount, true);
				}
			}
		}
	}

	if (drawPointer)
	{
		const DVector3 camForward = r_viewpoint.ViewVector3D;
		DVector3 camUp(0.0, 0.0, 1.0);
		DVector3 camRight = camUp ^ camForward;
		if (camRight.LengthSquared() < 1e-8)
		{
			camUp = DVector3(0.0, 1.0, 0.0);
			camRight = camUp ^ camForward;
		}
		camRight.MakeUnit();
		camUp = camForward ^ camRight;
		camUp.MakeUnit();

		const double pointerDistance = (hitEnd - r_viewpoint.Pos).Length();
		const double fovScale = std::tan(r_viewpoint.GetFieldOfView().Radians() * 0.5);
		const double pointerScale = std::max(0.25, (double)vr_laser_pointer_scale);
		// TIGHTENS ON A TARGET. A dot that stays the same size whatever it
		// is resting on is a cursor; one that draws in when it finds meat
		// is a threat. The breath rides on top so it never settles.
		const float lockTighten = std::clamp<float>(vr_laser_lock_tighten, 0.0f, 0.9f);
		const float lockScale = 1.0f - lock * lockTighten * (0.65f + 0.35f * lockPulse);
		const float pointerRadius = (float)std::max(0.006,
			pointerDistance * fovScale * 0.01 * pointerScale * lockScale);
		const int pointerSegments = 16;
		const int pointerVertexCount = pointerSegments + 2;
		screen->mVertexData->Map();
		auto pointerVerts = screen->mVertexData->AllocVertices(pointerVertexCount);
		auto pv = pointerVerts.first;
		pv[0].Set((float)pointerCenter.X, (float)pointerCenter.Z, (float)pointerCenter.Y, 0.5f, 0.5f);
		for (int i = 0; i <= pointerSegments; ++i)
		{
			const double t = (double)i / (double)pointerSegments;
			const double ang = t * 6.28318530717958647692;
			const double cs = std::cos(ang);
			const double sn = std::sin(ang);
			const DVector3 ringOffset = (camRight * cs + camUp * sn) * pointerRadius;
			const DVector3 pos = pointerCenter + ringOffset;
			pv[i + 1].Set((float)pos.X, (float)pos.Z, (float)pos.Y, 0.0f, 0.0f);
		}
		screen->mVertexData->Unmap();

		// BRIGHTENS WITH IT, so the smaller dot does not also become a
		// fainter one -- tightening alone would read as the sight losing
		// confidence rather than gaining it.
		const float pointerAlpha = std::clamp<float>(
			vr_laser_pointer_alpha * dotVisibility * (1.0f + lock * (0.35f + 0.30f * lockPulse)), 0.0f, 1.0f);
		const bool pointerOpaque = pointerAlpha >= 0.999f;
		// THE DOT BURNS TOO, and harder when locked. It is the part you
		// actually look at, so if anything on the sight should read as
		// incandescent rather than painted, it is this. Same mechanism as
		// the beam core: push past 1.0 and let bloom do it.
		const float dotEm = std::max(1.0f, (float)vr_laser_beam_emissive) * (1.0f + lock * 0.5f * lockPulse);
		state.SetColor(RPART(dotColor) / 255.0f * dotEm, GPART(dotColor) / 255.0f * dotEm,
			BPART(dotColor) / 255.0f * dotEm, pointerAlpha);
		state.SetRenderStyle(pointerOpaque ? STYLE_Source : STYLE_Add);
		state.Draw(DT_TriangleFan, pointerVerts.second, pointerVertexCount, true);

		if (vr_laser_pointer_glow != 0)
		{
			// AND THE GLOW SWELLS. The dot draws in while the halo around it
			// pushes out -- opposite directions, which is what makes the
			// lock read as pressure rather than as a simple size change.
			const float glowScale = std::max(1.1f, (float)vr_laser_pointer_glow_scale)
				* (1.0f + lock * (0.45f + 0.55f * lockPulse));
			const float glowIntensity = std::max(0.1f, (float)vr_laser_pointer_glow_intensity)
				* (1.0f + lock * 0.8f * lockPulse);
			const bool dynamicGlow = vr_laser_pointer_glow == 2;
			state.SetRenderStyle(STYLE_Add);
			const int glowPasses = dynamicGlow ? 4 : 3;
			for (int pass = 0; pass < glowPasses; ++pass)
			{
				const float passScale = dynamicGlow
					? glowScale * (1.8f + pass * 0.9f)
					: glowScale * (1.0f + pass * 0.5f);
				const float passAlpha = pointerAlpha * glowIntensity * (dynamicGlow
					? (0.20f / (pass + 1))
					: (pass == 0 ? 0.45f : pass == 1 ? 0.22f : 0.10f));
				screen->mVertexData->Map();
				auto glowVerts = screen->mVertexData->AllocVertices(pointerVertexCount);
				auto gv = glowVerts.first;
				gv[0].Set((float)pointerCenter.X, (float)pointerCenter.Z, (float)pointerCenter.Y, 0.5f, 0.5f);
				for (int i = 0; i <= pointerSegments; ++i)
				{
					const double t = (double)i / (double)pointerSegments;
					const double ang = t * 6.28318530717958647692;
					const double cs = std::cos(ang);
					const double sn = std::sin(ang);
					const DVector3 ringOffset = (camRight * cs + camUp * sn) * (pointerRadius * passScale);
					const DVector3 pos = pointerCenter + ringOffset;
					gv[i + 1].Set((float)pos.X, (float)pos.Z, (float)pos.Y, 0.0f, 0.0f);
				}
				screen->mVertexData->Unmap();
				state.SetColor(RPART(dotColor) / 255.0f, GPART(dotColor) / 255.0f, BPART(dotColor) / 255.0f, passAlpha);
				state.Draw(DT_TriangleFan, glowVerts.second, pointerVertexCount, true);
			}
		}
	}

	state.EnableTexture(true);
	state.SetDepthMask(true);
	state.SetRenderStyle(DefaultRenderStyle());
	state.SetTextureMode(TM_NORMAL);
	state.SetColor(1.f, 1.f, 1.f, 1.f);
	state.SetObjectColor(0xffffffff);
	state.SetAddColor(0);
	state.SetDynLight(0, 0, 0);
	state.EnableBrightmap(false);
	state.EnableModelMatrix(false);
	state.ResetColor();
}

static void DrawHitscanTracerGeometry(FRenderState& state, const DVector3& tracerStart, const DVector3& tracerEnd)
{
	DVector3 tracerVec = tracerEnd - tracerStart;
	const double tracerLength = tracerVec.Length();
	if (tracerLength <= 0.01)
	{
		return;
	}

	tracerVec.MakeUnit();

	DVector3 tracerRight, tracerUp;
	tracerVec.GetRightUp(tracerRight, tracerUp);

	const int tracerColor = (int)vr_hitscan_tracer_color;
	const float tracerAlpha = std::clamp<float>(vr_hitscan_tracer_alpha, 0.0f, 1.0f);
	if (tracerAlpha <= 0.0f)
	{
		return;
	}

	const float tracerRadius = 0.5f * std::max(0.01f, (float)vr_hitscan_tracer_width);
	constexpr int tracerSegments = 8;
	const int vertexCount = (tracerSegments + 1) * 2;

	state.EnableModelMatrix(false);
	state.SetLightIndex(-1);
	state.AlphaFunc(Alpha_Greater, 0.0f);
	state.ResetColor();
	state.SetObjectColor(0xffffffff);
	state.SetAddColor(0);
	state.SetDynLight(0, 0, 0);
	state.SetNoSoftLightLevel();
	state.SetLightParms(1.f, 0.f);
	state.EnableFog(false);
	state.SetFog(0, 0);
	state.ResetFadeColor();
	state.EnableTextureMatrix(false);
	state.EnableBrightmap(false);
	state.EnableTexture(false);
	state.EnableDepthTest(true);
	state.SetDepthMask(false);
	state.SetRenderStyle(tracerAlpha >= 0.999f ? STYLE_Source : STYLE_Add);
	state.SetColor(RPART(tracerColor) / 255.0f, GPART(tracerColor) / 255.0f, BPART(tracerColor) / 255.0f, tracerAlpha);

	screen->mVertexData->Map();
	auto verts = screen->mVertexData->AllocVertices(vertexCount);
	auto vp = verts.first;
	for (int i = 0; i <= tracerSegments; ++i)
	{
		const double t = (double)i / (double)tracerSegments;
		const double ang = t * 6.28318530717958647692;
		const double cs = std::cos(ang);
		const double sn = std::sin(ang);
		const DVector3 ringOffset = (tracerRight * cs + tracerUp * sn) * tracerRadius;
		const DVector3 startPos = tracerStart + ringOffset;
		const DVector3 endPos = tracerEnd + ringOffset;
		vp[i * 2 + 0].Set((float)startPos.X, (float)startPos.Z, (float)startPos.Y, 0.0f, 0.0f);
		vp[i * 2 + 1].Set((float)endPos.X, (float)endPos.Z, (float)endPos.Y, 0.0f, 1.0f);
	}
	screen->mVertexData->Unmap();

	state.Draw(DT_TriangleStrip, verts.second, vertexCount, true);

	state.EnableTexture(true);
	state.SetDepthMask(true);
	state.SetRenderStyle(DefaultRenderStyle());
	state.SetTextureMode(TM_NORMAL);
	state.SetColor(1.f, 1.f, 1.f, 1.f);
	state.SetObjectColor(0xffffffff);
	state.SetAddColor(0);
	state.SetDynLight(0, 0, 0);
	state.EnableBrightmap(false);
	state.EnableModelMatrix(false);
	state.ResetColor();
}

static bool IsPointInView(const DVector3& point)
{
	DVector3 toPoint = point - r_viewpoint.Pos;
	const double distance = toPoint.Length();
	if (distance <= 0.01)
	{
		return true;
	}

	toPoint /= distance;
	if (toPoint.dot(r_viewpoint.ViewVector3D) <= 0.0)
	{
		return false;
	}

	DVector3 right, up;
	r_viewpoint.ViewVector3D.GetRightUp(right, up);
	if (right.LengthSquared() < 1e-8)
	{
		right = DVector3(0.0, 1.0, 0.0);
	}
	if (up.LengthSquared() < 1e-8)
	{
		up = DVector3(0.0, 0.0, 1.0);
	}
	right.MakeUnit();
	up.MakeUnit();

	const double tanHalfFov = std::tan(r_viewpoint.GetFieldOfView().Radians() * 0.5);
	const double forward = toPoint.dot(r_viewpoint.ViewVector3D);
	const double rightOffset = std::abs(toPoint.dot(right));
	const double upOffset = std::abs(toPoint.dot(up));
	const double limit = forward * tanHalfFov * 1.05;
	return rightOffset <= limit && upOffset <= limit;
}

void DrawHitscanTracers(FRenderState& state)
{
	if (menuactive != MENU_Off || VRWheel_IsActive())
	{
		return;
	}

	auto& tracers = P_GetHitscanTracers();
	if (tracers.empty())
	{
		return;
	}

	if (primaryLevel == nullptr)
	{
		return;
	}

	const double now = (double)primaryLevel->maptime + r_viewpoint.TicFrac;
	const double speed = std::max(1.0, (double)vr_hitscan_tracer_speed * 100.0 / (double)TICRATE);
	const double tracerLength = std::max(0.0, (double)vr_hitscan_tracer_length);

	tracers.erase(std::remove_if(tracers.begin(), tracers.end(), [now, speed](const FHitscanTracer& tracer)
	{
		if (now < tracer.SpawnTime)
		{
			return true;
		}

		const double age = now - tracer.SpawnTime;
		if (tracer.Lifetime > 0.0 && age >= tracer.Lifetime)
		{
			return true;
		}

		return (age * speed * tracer.SpeedScale) >= tracer.Distance;
	}), tracers.end());

	for (const auto& tracer : tracers)
	{
		const double age = now - tracer.SpawnTime;
		if (age < 0.0)
		{
			continue;
		}

		if (tracer.bRicochet && !IsPointInView(tracer.Start))
		{
			continue;
		}

		const double frontDistance = std::min(tracer.Distance, age * speed * tracer.SpeedScale);
		if (frontDistance <= 0.01 || frontDistance >= tracer.Distance)
		{
			continue;
		}

		const double backDistance = std::max(0.0, frontDistance - tracerLength);
		const DVector3 tracerStart = tracer.Start + tracer.Direction * backDistance;
		const DVector3 tracerEnd = tracer.Start + tracer.Direction * frontDistance;
		DrawHitscanTracerGeometry(state, tracerStart, tracerEnd);
	}
}

void DrawLaserSightWorld(FRenderState& state)
{
	// [BB] VR_IsScriptLaserForced lets a script-side in-world menu switch the laser
	// on for its duration without writing to these archived cvars -- the VM refuses
	// those writes, and rewriting a player's saved settings to draw a line for four
	// seconds would be wrong even if it did not.
	const bool scriptLaser = VR_IsScriptLaserForced();
	if (!scriptLaser && !vr_laser_sight && !vr_laser_beam && !vr_laser_other_players_beam && !vr_laser_other_players_pointer)
	{
		return;
	}

	if (menuactive != MENU_Off)
	{
		return;
	}

	player_t* player = &players[consoleplayer];

	auto drawHand = [&state](player_t* player, bool offhand, bool allowPointer, bool allowBeamToggle)
	{
		if (player == nullptr || player->mo == nullptr || !player->mo->OverrideAttackPosDir)
		{
			return;
		}

		// [BB] Hide this hand's laser only when this hand is the one holding a
		// wheel. It used to hide both on any open wheel, so a ring on one hand
		// blinded the other -- which matters more now that the other hand can
		// still shoot.
		if (vr_laser_hide_on_wheel && VRWheel_ShouldSuppressHandInput(offhand ? VR_OFFHAND : VR_MAINHAND))
		{
			return;
		}

		// [BB] A script menu worn on this hand needs the pointer no matter what
		// the hand is holding. Both gates below ask "is this hand worth drawing a
		// laser for", and for a cursor the answer is always yes -- an EMPTY off
		// hand hits the first one, which is precisely the hand an off-hand menu
		// is most likely to be worn on.
		const bool forcedHere = VR_IsScriptLaserForcedFor(offhand);

		AActor* weapon = offhand ? player->OffhandWeapon : player->ReadyWeapon;
		if (weapon == nullptr)
		{
			if (!vr_laser_show_melee && !forcedHere)
			{
				return;
			}
		}
		else if (!vr_laser_show_melee && !forcedHere && (weapon->IntVar(NAME_WeaponFlags) & WIF_MELEEWEAPON))
		{
			return;
		}

		FLaserBeamPoints points;
		if (GetLaserBeamEndpoints(player, weapon, offhand, points))
		{
			const bool drawBeam = allowBeamToggle || (weapon != nullptr && (weapon->IntVar(NAME_WeaponFlags) & WIF_HASLASERBEAM));
			const bool drawPointer = allowPointer;

			// Publish what the trace just found so a gameplay mod can ask
			// "is this a headshot" against the SAME hit, not a second one --
			// see AActor.LaserTraceTarget*/LaserTraceHitPos* in actor.zs.
			if (offhand)
			{
				player->mo->LaserTraceTargetOff = points.HitActor;
				player->mo->LaserTraceHitPosOff = points.HitEnd;
			}
			else
			{
				player->mo->LaserTraceTargetMain = points.HitActor;
				player->mo->LaserTraceHitPosMain = points.HitEnd;
			}

			PalEntry beamCol = GetLaserBeamColorFor(weapon, offhand, false);
			PalEntry dotCol  = GetLaserBeamColorFor(weapon, offhand, true);

			// CYCLING LAYERS ON TOP OF THE RESOLVED COLOUR, and only replaces
			// what it is explicitly asked to. The beam and dot are cycled
			// independently -- vr_laser_color_cycle_dot gates the dot on its
			// own, because the dot is the reactive element (vr_laser_dot_range)
			// and often wants to hold still while the beam leading to it moves.
			if (vr_laser_color_cycle)
			{
				const int base = offhand ? 2 : 0;
				beamCol = CycleHue(beamCol, base + 0);
				if (vr_laser_color_cycle_dot)
					dotCol = CycleHue(dotCol, base + 1);
			}

			// HEADSHOT LINE-UP REACTS ON TOP OF EVERYTHING ABOVE, including
			// colour cycling -- a live headshot lineup is the most urgent
			// thing the sight can say, so it always wins the pixel. Reacts
			// to a flag script wrote from the trace just published above
			// (one tic of lag), never decides "is this a head" itself.
			const bool headshotHot = offhand ? player->mo->LaserHeadshotLinedUpOff : player->mo->LaserHeadshotLinedUpMain;
			if (vr_laser_headshot_react && headshotHot)
			{
				float t = 1.0f;
				if (vr_laser_headshot_pulse)
				{
					constexpr double TWO_PI = 6.283185307179586;
					const double speed = std::max(0.0, (double)vr_laser_headshot_pulse_speed);
					const double wave = 0.5 + 0.5 * std::sin(I_msTimeF() * 0.001 * speed * TWO_PI);
					// Floored at 0.4, not 0: a pulse that dips to zero reads as
					// the reaction switching off and on, not as one continuous
					// alert breathing.
					t = 0.4f + 0.6f * (float)wave;
				}
				beamCol = LerpColor(beamCol, (int)vr_laser_headshot_color, t);
				dotCol  = LerpColor(dotCol,  (int)vr_laser_headshot_color, t);
			}

			DrawLaserBeamGeometry(state, points.Start, points.BeamEnd, points.HitEnd, drawBeam, drawPointer, points.OnTarget,
				(int)beamCol, (int)dotCol);
		}
	};

	// Asked per hand, so the forced pointer lands on the hand wearing the menu
	// and the other hand keeps whatever the player's own cvars say it should have.
	drawHand(player, false, VR_IsScriptLaserForcedFor(false) || !!vr_laser_sight,
	                        VR_IsScriptLaserForcedFor(false) || !!vr_laser_beam);
	drawHand(player, true,  VR_IsScriptLaserForcedFor(true)  || !!vr_laser_sight,
	                        VR_IsScriptLaserForcedFor(true)  || !!vr_laser_beam);

	if (multiplayer && (vr_laser_other_players_beam || vr_laser_other_players_pointer))
	{
		for (int i = 0; i < MAXPLAYERS; ++i)
		{
			player_t* other = &players[i];
			if (other == player || !playeringame[i] || other->mo == nullptr || other->playerstate != PST_LIVE)
			{
				continue;
			}

			drawHand(other, false, !!vr_laser_other_players_pointer, !!vr_laser_other_players_beam);
		}
	}
}

//==========================================================================
//
// R_DrawPlayerSprites
//
//==========================================================================

void HWDrawInfo::DrawPlayerSprites(bool hudModelStep, FRenderState &state)
{
	auto vrmode = VRMode::GetVRModeCached(true);
	
	auto oldlightmode = lightmode;
	for (auto &hudsprite : hudsprites)
	{
		if (!vrmode->IsVR() && (!!hudsprite.mframe) != hudModelStep) continue;
		if (!hudsprite.mframe && isSoftwareLighting(oldlightmode)) SetFallbackLightMode();	// Software lighting cannot handle 2D content.

		// Which hand this sprite belongs to, decided by the psprite LAYER rather
		// than by matching weapon classes.
		//
		// WeaponSpriteMatches returns true on GetClass() equality, so it cannot
		// tell one hand's pistol from the other's -- with the same weapon class
		// in both hands it matched BOTH, which meant one hand's wheel
		// suppression skipped both hands' sprites, and the mainhand sprite was
		// handed the offhand's controller transform and drawn off where you
		// could not see it. The muzzle flash kept working because it is a
		// different layer with a different caller, which is why a weapon that
		// had gone invisible reappeared the moment it fired.
		//
		// Layer ids cannot cross hands: PSP_WEAPON is 1, PSP_OFFHANDWEAPON is
		// 1000000, and the chain is kept sorted and unique. The one case the id
		// cannot answer is PSP_FLASH (1000), which both hands share -- for that
		// the caller is checked, by pointer identity rather than by class.
		//
		// RS fork -- an explicit per-layer hand wins over both channels below.
		// Neither of them can put a layer at the MAIN hand while its caller is
		// the off-hand weapon, which is what a magazine carried over to a gun
		// held in the off hand needs. See DPSprite::HandOverride (p_pspr.h);
		// <0 is "not set" and leaves the id/caller logic untouched.
		int spriteHand = VR_MAINHAND;
		if (hudsprite.weapon != nullptr && hudsprite.weapon->HandOverride >= 0)
		{
			spriteHand = (hudsprite.weapon->HandOverride == VR_OFFHAND) ? VR_OFFHAND : VR_MAINHAND;
		}
		else if (hudsprite.weapon != nullptr)
		{
			if (hudsprite.weapon->GetID() >= PSP_OFFHANDWEAPON)
			{
				spriteHand = VR_OFFHAND;
			}
			else if (hudsprite.owner != nullptr && hudsprite.owner->player != nullptr)
			{
				AActor* caller = hudsprite.weapon->GetCaller();
				AActor* offhand = hudsprite.owner->player->OffhandWeapon;
				if (caller != nullptr && caller == offhand)
				{
					spriteHand = VR_OFFHAND;
				}
			}
		}

		if (hudsprite.weapon != nullptr && VRWheel_ShouldSuppressWeaponHand(spriteHand))
		{
			continue;
		}
		if (!hudsprite.mframe)
		{
			vrmode->AdjustPlayerSprites(state, spriteHand == VR_OFFHAND);
		}
		DrawPSprite(&hudsprite, state);
		if (!hudsprite.mframe) vrmode->UnAdjustPlayerSprites(state);
		lightmode = oldlightmode;
	}
}


//==========================================================================
//
//
//
//==========================================================================

static bool isBright(DPSprite *psp)
{
	if (psp != nullptr && psp->GetState() != nullptr)
	{
		bool disablefullbright = false;
		FTextureID lump = sprites[psp->GetSprite()].GetSpriteFrame(psp->GetFrame(), 0, nullAngle, nullptr);
		if (lump.isValid())
		{
			auto tex = TexMan.GetGameTexture(lump, true);
			if (tex) disablefullbright = tex->isFullbrightDisabled();
		}
		return psp->GetState()->GetFullbright() && !disablefullbright;
	}
	return false;
}

static bool WeaponSpriteMatches(AActor* equippedWeapon, AActor* spriteCaller)
{
	if (equippedWeapon == nullptr || spriteCaller == nullptr)
	{
		return false;
	}

	if (equippedWeapon == spriteCaller || equippedWeapon->GetClass() == spriteCaller->GetClass())
	{
		return true;
	}

	// A psprite's caller is not necessarily a Weapon any more. Hand models ride
	// their own psprite layers with a plain Inventory caller, and SisterWeapon is
	// declared on Weapon -- reading it off anything else is not a null return,
	// it is a fatal "Variable SisterWeapon not found in <class>" from ScriptVar.
	// Anything that is not a Weapon simply has no sister.
	auto sisterOf = [](AActor* a) -> AActor*
	{
		return (a != nullptr && a->IsKindOf(NAME_Weapon)) ? a->PointerVar<AActor>(NAME_SisterWeapon) : nullptr;
	};
	auto equippedSister = sisterOf(equippedWeapon);
	auto callerSister = sisterOf(spriteCaller);
	if (equippedSister == spriteCaller || callerSister == equippedWeapon)
	{
		return true;
	}

	return (equippedSister != nullptr && equippedSister->GetClass() == spriteCaller->GetClass()) ||
		(callerSister != nullptr && callerSister->GetClass() == equippedWeapon->GetClass());
}

//==========================================================================
//
// Weapon position
//
//==========================================================================

static WeaponPosition2D GetWeaponPosition2D(player_t *player, double ticFrac, DPSprite *psp)
{
	WeaponPosition2D w;
	BobType = PSPB_2D;
	FVector2 interp = PlayerBob[player - players].Interpolate2D(Net_ModifyFrac(ticFrac));
	w.bobx = interp.X;
	w.boby = interp.Y;

	DPSprite *readyWeaponPsp = player->FindPSprite(PSP_WEAPON);
	DPSprite *offhandWeaponPsp = player->FindPSprite(PSP_OFFHANDWEAPON);

	// Interpolate the main weapon layer once so as to be able to add it to other layers.
	w.weapon = WeaponSpriteMatches(player->ReadyWeapon, psp->GetCaller()) ? readyWeaponPsp : offhandWeaponPsp;
	if (w.weapon != nullptr)
	{
		if (w.weapon->firstTic)
		{
			w.wx = (float)w.weapon->x;
			w.wy = (float)w.weapon->y;
		}
		else
		{
			const double frac = Net_ModifyObjectFrac(w.weapon, ticFrac);
			w.wx = (float)(w.weapon->oldx + (w.weapon->x - w.weapon->oldx) * frac);
			w.wy = (float)(w.weapon->oldy + (w.weapon->y - w.weapon->oldy) * frac);
		}
	}
	else
	{
		w.wx = 0;
		w.wy = 0;
	}
	return w;
}

static WeaponPosition3D GetWeaponPosition3D(player_t *player, double ticFrac, DPSprite *psp)
{
	WeaponPosition3D w;
	BobType = PSPB_3D;
	PlayerBob[player - players].Interpolate3D(w.translation, w.rotation, Net_ModifyFrac(ticFrac));

	DPSprite *readyWeaponPsp = player->FindPSprite(PSP_WEAPON);
	DPSprite *offhandWeaponPsp = player->FindPSprite(PSP_OFFHANDWEAPON);

	// Interpolate the main weapon layer once so as to be able to add it to other layers.
	w.weapon = WeaponSpriteMatches(player->ReadyWeapon, psp->GetCaller()) ? readyWeaponPsp : offhandWeaponPsp;
	if (w.weapon != nullptr)
	{
		if (w.weapon->firstTic)
		{
			w.wx = (float)w.weapon->x;
			w.wy = (float)w.weapon->y;
		}
		else
		{
			const double frac = Net_ModifyObjectFrac(w.weapon, ticFrac);
			w.wx = (float)(w.weapon->oldx + (w.weapon->x - w.weapon->oldx) * frac);
			w.wy = (float)(w.weapon->oldy + (w.weapon->y - w.weapon->oldy) * frac);
		}

		auto weaponActor = w.weapon->GetCaller();

		if (weaponActor && weaponActor->IsKindOf(NAME_Weapon))
		{
			DVector3 *dPivot = (DVector3*) weaponActor->ScriptVar(NAME_BobPivot3D, nullptr);
			w.pivot.X = (float) dPivot->X;
			w.pivot.Y = (float) dPivot->Y;
			w.pivot.Z = (float) dPivot->Z;
		}
		else
		{
			w.pivot = FVector3(0,0,0);
		}
	}
	else
	{
		w.wx = 0;
		w.wy = 0;
		w.pivot = FVector3(0,0,0);
	}
	return w;
}

//==========================================================================
//
// Bobbing
//
//==========================================================================

static FVector2 BobWeapon2D(WeaponPosition2D &weap, DPSprite *psp, double ticFrac)
{
	if (psp->firstTic)
	{ // Can't interpolate the first tic.
		psp->firstTic = false;
		psp->ResetInterpolation();
	}

	float sx = float(psp->oldx + (psp->x - psp->oldx) * ticFrac);
	float sy = float(psp->oldy + (psp->y - psp->oldy) * ticFrac);

	if (psp->Flags & PSPF_ADDBOB)
	{
		sx += (psp->Flags & PSPF_MIRROR) ? -weap.bobx : weap.bobx;
		sy += weap.boby;
	}

	if (psp->Flags & PSPF_ADDWEAPON && !(psp->GetID() == PSP_WEAPON || psp->GetID() == PSP_OFFHANDWEAPON))
	{
		sx += weap.wx;
		sy += weap.wy;
	}
	return { sx, sy };
}

static FVector2 BobWeapon3D(WeaponPosition3D &weap, DPSprite *psp, FVector3 &translation, FVector3 &rotation, FVector3 &pivot, double ticFrac)
{
	if (psp->firstTic)
	{ // Can't interpolate the first tic.
		psp->firstTic = false;
		psp->ResetInterpolation();
	}

	float sx = float(psp->oldx + (psp->x - psp->oldx) * ticFrac);
	float sy = float(psp->oldy + (psp->y - psp->oldy) * ticFrac);
	float sz = 0;

	if (psp->Flags & PSPF_ADDBOB)
	{
		if (psp->Flags & PSPF_MIRROR)
		{
			translation = FVector3(-weap.translation.X, weap.translation.Y, weap.translation.Z);
			rotation = FVector3(-weap.rotation.X, weap.rotation.Y, weap.rotation.Z);
			pivot = FVector3(-weap.pivot.X, weap.pivot.Y, weap.pivot.Z);
		}
		else
		{
			translation = weap.translation ;
			rotation = weap.rotation ;
			pivot = weap.pivot ;
		}
	}
	else
	{
		translation = rotation = pivot = FVector3(0,0,0);
	}

	if (psp->Flags & PSPF_ADDWEAPON && !(psp->GetID() == PSP_WEAPON || psp->GetID() == PSP_OFFHANDWEAPON))
	{
		sx += weap.wx;
		sy += weap.wy;
	}
	return { sx, sy };
}

//==========================================================================
//
// Lighting
//
//==========================================================================

WeaponLighting HWDrawInfo::GetWeaponLighting(sector_t *viewsector, const DVector3 &pos, int cm, area_t in_area, const DVector3 &playerpos, bool weaponPureLightLevel = false)
{
	WeaponLighting l;

	if (cm)
	{
		l.lightlevel = 255;
		l.cm.Clear();
		l.isbelow = false;
	}
	else
	{
		auto fakesec = hw_FakeFlat(viewsector, in_area, false);

		// calculate light level for weapon sprites
		l.lightlevel = RescaleLightLevel(fakesec->lightlevel);

		// calculate colormap for weapon sprites
		if (viewsector->e->XFloor.ffloors.Size() && !(Level->flags3 & LEVEL3_NOCOLOREDSPRITELIGHTING))
		{
			TArray<lightlist_t> & lightlist = viewsector->e->XFloor.lightlist;
			for (unsigned i = 0; i<lightlist.Size(); i++)
			{
				double lightbottom;

				if (i<lightlist.Size() - 1)
				{
					lightbottom = lightlist[i + 1].plane.ZatPoint(pos);
				}
				else
				{
					lightbottom = viewsector->floorplane.ZatPoint(pos);
				}

				if (lightbottom < pos.Z)
				{
					l.cm = lightlist[i].extra_colormap;
					l.lightlevel = RescaleLightLevel(*lightlist[i].p_lightlevel);
					break;
				}
			}
		}
		else
		{
			l.cm = fakesec->Colormap;
			if (Level->flags3 & LEVEL3_NOCOLOREDSPRITELIGHTING) l.cm.ClearColor();
		}

		l.lightlevel = CalcLightLevel(lightmode, l.lightlevel, getExtraLight(), true, 0, weaponPureLightLevel);

		if (isSoftwareLighting(lightmode) || l.lightlevel < 92)
		{
			// Korshun: the way based on max possible light level for sector like in software renderer.
			double min_L = 36.0 / 31.0 - ((l.lightlevel / 255.0) * (63.0 / 31.0)); // Lightlevel in range 0-63
			if (min_L < 0)
				min_L = 0;
			else if (min_L > 1.0)
				min_L = 1.0;

			l.lightlevel = int((1.0 - min_L) * 255);
		}
		else
		{
			l.lightlevel = (2 * l.lightlevel + 255) / 3;
		}
		l.lightlevel = viewsector->CheckSpriteGlow(l.lightlevel, playerpos);
		l.isbelow = fakesec != viewsector && in_area == area_below;
	}

	// Korshun: fullbright fog in opengl, render weapon sprites fullbright (but don't cancel out the light color!)
	if (Level->brightfog && ((Level->flags&LEVEL_HASFADETABLE) || l.cm.FadeColor != 0))
	{
		l.lightlevel = 255;
	}
	return l;
}

//==========================================================================
//
//
//
//==========================================================================

void HUDSprite::SetBright(bool isbelow)
{
	if (!isbelow)
	{
		cm.MakeWhite();
	}
	else
	{
		// under water areas keep most of their color for fullbright objects
		cm.LightColor.r = (3 * cm.LightColor.r + 0xff) / 4;
		cm.LightColor.g = (3 * cm.LightColor.g + 0xff) / 4;
		cm.LightColor.b = (3 * cm.LightColor.b + 0xff) / 4;
	}
	lightlevel = 255;
}

//==========================================================================
//
// Render Style
//
//==========================================================================

bool HUDSprite::GetWeaponRenderStyle(DPSprite *psp, AActor *playermo, sector_t *viewsector, WeaponLighting &lighting, double ticFrac)
{
	auto rs = psp->GetRenderStyle(playermo->RenderStyle, playermo->InterpolatedAlpha(ticFrac));

	visstyle_t vis;

	vis.RenderStyle = STYLE_Count;
	vis.Alpha = rs.second;
	vis.Invert = false;
	playermo->AlterWeaponSprite(&vis);

	alpha = (psp->Flags & PSPF_FORCEALPHA) ? 0.f : vis.Alpha;

	if (vis.RenderStyle != STYLE_Count && !(psp->Flags & PSPF_FORCESTYLE))
	{
		RenderStyle = vis.RenderStyle;
	}
	else
	{
		RenderStyle = rs.first;
	}
	if (RenderStyle.BlendOp == STYLEOP_None) return false;

	if (vis.Invert)
	{
		// this only happens for Strife's inverted weapon sprite
		RenderStyle.Flags |= STYLEF_InvertSource;
	}

	// Set the render parameters

	OverrideShader = -1;
	if (RenderStyle.BlendOp == STYLEOP_Fuzz)
	{
		if (gl_fuzztype != 0)
		{
			// Todo: implement shader selection here
			RenderStyle = LegacyRenderStyles[STYLE_Translucent];
			OverrideShader = SHADER_NoTexture + gl_fuzztype;
			alpha = 0.99f;	// trans may not be 1 here
		}
		else
		{
			RenderStyle.BlendOp = STYLEOP_Shadow;
		}
	}

	if (RenderStyle.Flags & STYLEF_TransSoulsAlpha)
	{
		alpha	= transsouls;
	}
	else if (RenderStyle.Flags & STYLEF_Alpha1)
	{
		alpha = 1.f;
	}
	else if (alpha == 0.f)
	{
		alpha = vis.Alpha;
	}
	if (!RenderStyle.IsVisible(alpha)) return false;	// if it isn't visible skip the rest.

	PalEntry ThingColor = (playermo->RenderStyle.Flags & STYLEF_ColorIsFixed) ? playermo->fillcolor : 0xffffff;
	ThingColor.a = 255;

	// RS FORK -- PER-PSPRITE TINT ON THE HELD 3D MODEL. Added 2026-08-08.
	//
	// Stock reads only playermo->fillcolor, which is (a) the PLAYER's
	// colour, so every psprite shares one value and no weapon can differ
	// from another, and (b) gated behind STYLEF_ColorIsFixed, a flag
	// whose styles render a flat silhouette -- so the only way to get a
	// colour was to throw the texture away.
	//
	// psp->Tint is neither: it is per-layer (mainhand and offhand are
	// separate psprites) and it MULTIPLIES the model's own skin, so
	// detail survives. Default 0xffffffff is the identity, which is why
	// this is invisible to every mod that does not set it.
	ThingColor = ThingColor.Modulate(psp->Tint);
	ThingColor.a = 255;

	const bool bright = isBright(psp);
	ObjectColor = bright ? ThingColor : ThingColor.Modulate(viewsector->SpecialColors[sector_t::sprites]);
	AddColor = psp->Glow;

	lightlevel = lighting.lightlevel;
	cm = lighting.cm;
	// RS fork: kept so the draw path can exempt a fullbright frame (muzzle
	// flash) from world darkness, as the world-sprite path already does.
	this->bright = bright;
	if (bright) SetBright(lighting.isbelow);

	return true;
}

//==========================================================================
//
// Coordinates
//
//==========================================================================

bool HUDSprite::GetWeaponRect(HWDrawInfo *di, DPSprite *psp, float sx, float sy, player_t *player, double ticfrac)
{
	float			tx;
	float			scale;
	float			scalex;
	float			ftextureadj;
	float			ftexturemid;

	// decide which patch to use
	bool mirror;
	FTextureID lump = sprites[psp->GetSprite()].GetSpriteFrame(psp->GetFrame(), 0, nullAngle, &mirror);
	if (!lump.isValid()) return false;

	auto tex = TexMan.GetGameTexture(lump, false);
	if (!tex || !tex->isValid()) return false;

	FTextureID lastPatch = psp->LastPatch;
	if (gametic - primaryLevel->starttime > 2 &&
		lump != lastPatch &&
		gl_texture_thread &&
		screen->SupportsBackgroundCache())
	{
		int scaleflags = CTF_Expand;
		if (shouldUpscale(tex, UF_Sprite)) scaleflags |= CTF_Upscale;

		FState* nextState = psp->GetState();
		for (int i = 0; i < 5; i++)
		{
			if (nextState == nullptr) break;

			FState* renderState = nextState->GetNextState();
			for (int skip = 0; skip < 8 && renderState != nullptr && renderState->GetTics() <= 0; skip++)
			{
				renderState = renderState->GetNextState();
			}
			nextState = renderState;

			if (renderState != nullptr && renderState->GetTics() > 0)
			{
				FTextureID lump2 = sprites[psp->GetSprite()].GetSpriteFrame(renderState->GetFrame(), 0, nullAngle, nullptr);
				if (lump2.isValid())
				{
					auto tex2 = TexMan.GetGameTexture(lump2, false);
					if (tex2 && tex2->isValid())
					{
						int scaleflags2 = CTF_Expand;
						if (shouldUpscale(tex2, UF_Sprite)) scaleflags2 |= CTF_Upscale;
						screen->BackgroundCacheTextureMaterial(tex2, psp->Translation, scaleflags2, true);
					}
				}
			}
		}

		FMaterial* gltex = FMaterial::ValidateTexture(tex, scaleflags, false);
		MaterialLayerInfo* layer = nullptr;
		IHardwareTexture* hwtex = gltex != nullptr ? gltex->GetLayer(0, psp->Translation.index(), &layer) : nullptr;
		if (gltex == nullptr || hwtex == nullptr || !hwtex->IsValid())
		{
			if (gltex)
			{
				screen->BackgroundCacheMaterial(gltex, psp->Translation, true);
			}
			else
			{
				screen->BackgroundCacheTextureMaterial(tex, psp->Translation, scaleflags, true);
			}

			bool foundNewer = false;
			for (int i = 0; i < min((long)psp->LastPatches.length, psp->LastPatches.pos); i++)
			{
				if (psp->LastPatches[i] == 0) continue;

				FTextureID lump2;
				lump2.SetIndex(psp->LastPatches[i]);
				if (!lump2.isValid()) continue;

				auto tex2 = TexMan.GetGameTexture(lump2, false);
				FMaterial* gltex2 = FMaterial::ValidateTexture(tex2, scaleflags, false);
				MaterialLayerInfo* layer2 = nullptr;
				IHardwareTexture* hwtex2 = gltex2 != nullptr ? gltex2->GetLayer(0, psp->Translation.index(), &layer2) : nullptr;
				if (gltex2 != nullptr && hwtex2 != nullptr && hwtex2->IsValid())
				{
					lump = lump2;
					tex = tex2;
					foundNewer = true;
					break;
				}
			}

			if (!foundNewer && lastPatch.isValid())
			{
				lump = lastPatch;
				tex = TexMan.GetGameTexture(lump, false);
				if (!tex || !tex->isValid()) return false;
			}
			else if (!foundNewer)
			{
				return false;
			}
		}
	}

	psp->LastPatch = lump;
	auto& spi = tex->GetSpritePositioning(1);

	float vw = (float)viewwidth;
	float vh = (float)viewheight;

	FloatRect r = spi.GetSpriteRect();

	// calculate edges of the shape
	scalex = psp->baseScale.X * (320.0f / (240.0f * r_viewwindow.WidescreenRatio)) * (vw / 320);

	tx = (psp->Flags & PSPF_MIRROR) ? ((160 - r.width) - (sx + r.left)) : (sx - (160 - r.left));
	x1 = tx * scalex + vw / 2;
	// [MC] Disabled these because vertices can be manipulated now.
	//if (x1 > vw)	return false; // off the right side
	x1 += viewwindowx;


	tx += r.width;
	x2 = tx * scalex + vw / 2;
	//if (x2 < 0) return false; // off the left side
	x2 += viewwindowx;

	// killough 12/98: fix psprite positioning problem
	ftextureadj = (120.0f / psp->baseScale.Y) - 100.0f; // [XA] scale relative to weapon baseline
	ftexturemid = 100.f - sy - r.top - psp->GetYAdjust(screenblocks >= 11) - ftextureadj;

	// [XA] note: Doom's native 1.2x aspect ratio was originally
	// handled here by multiplying SCREENWIDTH by 200 instead of
	// 240, but now the baseScale var defines this from now on.
	scale = psp->baseScale.Y * (SCREENHEIGHT*vw) / (SCREENWIDTH * 240.0f);

	// Canvas textures are stored upside down
	if (tex && tex->isHardwareCanvas()) scale *= -1;

	y1 = viewwindowy + vh / 2 - (ftexturemid * scale);
	y2 = y1 + (r.height * scale) + 1;

	const bool flip = (psp->Flags & PSPF_FLIP);
	if (!(mirror) != !(flip))
	{
		u2 = spi.GetSpriteUL();
		v1 = spi.GetSpriteVT();
		u1 = spi.GetSpriteUR();
		v2 = spi.GetSpriteVB();
	}
	else
	{
		u1 = spi.GetSpriteUL();
		v1 = spi.GetSpriteVT();
		u2 = spi.GetSpriteUR();
		v2 = spi.GetSpriteVB();
	}

	// [MC] Code copied from DTA_Rotate.
	// Big thanks to IvanDobrovski who helped me modify this.

	WeaponInterp Vert;
	Vert.v[0] = FVector2(x1, y1);
	Vert.v[1] = FVector2(x1, y2);
	Vert.v[2] = FVector2(x2, y1);
	Vert.v[3] = FVector2(x2, y2);

	for (int i = 0; i < 4; i++)
	{
		const float cx = (flip) ? -psp->Coord[i].X : psp->Coord[i].X;
		Vert.v[i] += FVector2(cx * scalex, psp->Coord[i].Y * scale);
	}
	if (psp->rotation != nullAngle || !psp->scale.isZero())
	{
		// [MC] Sets up the alignment for starting the pivot at, in a corner.
		float anchorx, anchory;
		switch (psp->VAlign)
		{
			default:
			case PSPA_TOP:		anchory = 0.0;	break;
			case PSPA_CENTER:	anchory = 0.5;	break;
			case PSPA_BOTTOM:	anchory = 1.0;	break;
		}

		switch (psp->HAlign)
		{
			default:
			case PSPA_LEFT:		anchorx = 0.0;	break;
			case PSPA_CENTER:	anchorx = 0.5;	break;
			case PSPA_RIGHT:	anchorx = 1.0;	break;
		}
		// Handle PSPF_FLIP.
		if (flip) anchorx = 1.0 - anchorx;

		FAngle rot = FAngle::fromDeg(float((flip) ? -psp->rotation.Degrees() : psp->rotation.Degrees()));
		const float cosang = rot.Cos();
		const float sinang = rot.Sin();

		float xcenter, ycenter;
		const float width = x2 - x1;
		const float height = y2 - y1;
		const float px = float((flip) ? -psp->pivot.X : psp->pivot.X);
		const float py = float(psp->pivot.Y);

		// Set up the center and offset accordingly. PivotPercent changes it to be a range [0.0, 1.0]
		// instead of pixels and is enabled by default.
		if (psp->Flags & PSPF_PIVOTPERCENT)
		{
			xcenter = x1 + (width * anchorx + width * px);
			ycenter = y1 + (height * anchory + height * py);
		}
		else
		{
			xcenter = x1 + (width * anchorx + scalex * px);
			ycenter = y1 + (height * anchory + scale * py);
		}

		// Now adjust the position, rotation and scale of the image based on the latter two.
		for (int i = 0; i < 4; i++)
		{
			Vert.v[i] -= {xcenter, ycenter};
			const float xx = xcenter + psp->scale.X * (Vert.v[i].X * cosang + Vert.v[i].Y * sinang);
			const float yy = ycenter - psp->scale.Y * (Vert.v[i].X * sinang - Vert.v[i].Y * cosang);
			Vert.v[i] = {xx, yy};
		}
	}
	psp->Vert = Vert;

	if (psp->scale.X == 0.0 || psp->scale.Y == 0.0)
		return false;

	const bool interp = (psp->InterpolateTic || psp->Flags & PSPF_INTERPOLATE);

	for (int i = 0; i < 4; i++)
	{
		FVector2 t = Vert.v[i];
		if (interp)
			t = psp->Prev.v[i] + (psp->Vert.v[i] - psp->Prev.v[i]) * ticfrac;

		Vert.v[i] = t;
	}

	// [MC] If this is absolutely necessary, uncomment it. It just checks if all the vertices
	// are all off screen either to the right or left, but is it honestly needed?
	/*
	if ((
		Vert.v[0].X > 0.0 &&
		Vert.v[1].X > 0.0 &&
		Vert.v[2].X > 0.0 &&
		Vert.v[3].X > 0.0) || (
		Vert.v[0].X < vw &&
		Vert.v[1].X < vw &&
		Vert.v[2].X < vw &&
		Vert.v[3].X < vw))
		return false;
	*/
	auto verts = screen->mVertexData->AllocVertices(4);
	mx = verts.second;

	verts.first[0].Set(Vert.v[0].X, Vert.v[0].Y, 0, u1, v1);
	verts.first[1].Set(Vert.v[1].X, Vert.v[1].Y, 0, u1, v2);
	verts.first[2].Set(Vert.v[2].X, Vert.v[2].Y, 0, u2, v1);
	verts.first[3].Set(Vert.v[3].X, Vert.v[3].Y, 0, u2, v2);

	texture = tex;
	return true;
}

//==========================================================================
//
// R_DrawPlayerSprites
//
//==========================================================================
void HWDrawInfo::PreparePlayerSprites2D(sector_t * viewsector, area_t in_area)
{
	static PClass * wpCls = PClass::FindClass("Weapon");
	static unsigned ModifyBobLayerVIndex = GetVirtualIndex(wpCls, "ModifyBobLayer");
	static VMFunction * ModifyBobLayerOrigFunc = wpCls->Virtuals.Size() > ModifyBobLayerVIndex ? wpCls->Virtuals[ModifyBobLayerVIndex] : nullptr;

	AActor * playermo = players[consoleplayer].camera;
	player_t * player = playermo->player;

	const auto &vp = Viewpoint;

	AActor *camera = vp.camera;

	// UZDXREMA: do NOT hoist the WeaponPosition2D out here. The fork's
	// GetWeaponPosition2D takes a third `DPSprite *psp` argument for per-hand
	// positioning, so it must be evaluated per psprite inside the loop below.
	WeaponLighting light = GetWeaponLighting(viewsector, vp.Pos, isFullbrightScene(), in_area, camera->Pos());

	// hack alert! Rather than changing everything in the underlying lighting code let's just temporarily change
	// light mode here to draw the weapon sprite.
	auto oldlightmode = lightmode;
	if (isSoftwareLighting(oldlightmode)) SetFallbackLightMode();

	const double bobFrac = Net_ModifyFrac(vp.TicFrac);
	for (DPSprite *psp = player->psprites; psp != nullptr && psp->GetID() < PSP_TARGETCENTER; psp = psp->GetNext())
	{
		if (weaponStabilised && psp->GetCaller() == player->OffhandWeapon)
		{
			continue;
		}
		if (psp->NoDraw) continue;   // RS FORK -- see the 3D pass
		if (!psp->GetState()) continue;

		FSpriteModelFrame *smf = FindModelFrame(psp->Caller, psp->GetSprite(), psp->GetFrame(), false);

		// This is an 'either-or' proposition. This maybe needs some work to allow overlays with weapon models but as originally implemented this just won't work.
		if (smf) continue;

		// RS FORK -- FLAT OVERLAYS ON A WEAPON THAT IS ITSELF A MODEL.
		//
		// VR runs two passes over this same psprite list: PreparePlayerSprites3D
		// keeps the layers that resolve a model, this one keeps the layers that
		// do not. A muzzle flash is its own layer, owned by the same weapon,
		// and it has no model of its own -- so when the gun is a mesh, the 3D
		// pass draws the gun and this pass draws the flash, and you get a flat
		// billboard hanging in front of a 3D weapon.
		//
		// This cannot be fixed from ZScript. psp->alpha is discarded by
		// DPSprite::GetRenderStyle unless the layer carries PSPF_ALPHA or
		// PSPF_FORCEALPHA, which a plain A_GunFlash overlay does not set, and
		// the decision itself is a `continue` in a render loop that no script
		// participates in.
		//
		// Deliberately scoped to "the weapon owning this layer is drawn as a
		// model" rather than "hide flashes". A sprite weapon keeps its flash:
		// the lookup below returns null for it and nothing is suppressed. With
		// the cvar at its 1.0 default nothing is suppressed either way, so
		// stock behaviour is untouched until someone opts in.
		float flatOverlayAlpha = 1.0f;
		if (r_hudflatoverlay < 1.0f && psp->Caller != nullptr
			&& psp->GetID() != PSP_WEAPON && psp->GetID() != PSP_OFFHANDWEAPON)
		{
			bool ownerDrawsAsModel = false;
			for (DPSprite *own = player->psprites;
				 own != nullptr && own->GetID() < PSP_TARGETCENTER;
				 own = own->GetNext())
			{
				if (own == psp || own->Caller != psp->Caller) continue;
				if (own->GetID() != PSP_WEAPON && own->GetID() != PSP_OFFHANDWEAPON) continue;
				if (!own->GetState()) continue;
				if (FindModelFrame(own->Caller, own->GetSprite(), own->GetFrame(), false))
				{
					ownerDrawsAsModel = true;
					break;
				}
			}
			if (ownerDrawsAsModel)
			{
				if (r_hudflatoverlay <= 0.0f) continue;   // fully hidden
				flatOverlayAlpha = r_hudflatoverlay;      // dimmed
			}
		}

		HUDSprite hudsprite;
		hudsprite.owner = playermo;
		hudsprite.mframe = smf;
		hudsprite.weapon = psp;

		if (!hudsprite.GetWeaponRenderStyle(psp, camera, viewsector, light, vp.TicFrac)) continue;

		// RS fork -- dim rather than hide, when the cvar sits between 0 and 1.
		// Applied after the render style, which is what establishes the layer's
		// own alpha in the first place.
		if (flatOverlayAlpha < 1.0f) hudsprite.alpha *= flatOverlayAlpha;

		WeaponPosition2D weap = GetWeaponPosition2D(camera->player, vp.TicFrac, psp);

		VMFunction * ModifyBobLayer = nullptr;
		DVector2 bobxy = DVector2(weap.bobx , weap.boby);

		if(weap.weapon && weap.weapon->GetCaller())
		{
			PClass * cls = weap.weapon->GetCaller()->GetClass();
			ModifyBobLayer = cls->Virtuals.Size() > ModifyBobLayerVIndex ? cls->Virtuals[ModifyBobLayerVIndex] : nullptr;

			if( ModifyBobLayer == ModifyBobLayerOrigFunc) ModifyBobLayer = nullptr;
		}

		if(ModifyBobLayer && (psp->Flags & PSPF_ADDBOB))
		{
			DVector2 out;
			VMValue param[] = { weap.weapon->GetCaller() , bobxy.X , bobxy.Y , psp->GetID() , bobFrac };
			VMReturn ret(&out);

			VMCall(ModifyBobLayer, param, 5, &ret, 1);

			weap.bobx = out.X;
			weap.boby = out.Y;
		}

		const double frac = Net_ModifyObjectFrac(psp, vp.TicFrac);
		FVector2 spos = BobWeapon2D(weap, psp, frac);

		hudsprite.dynrgb[0] = hudsprite.dynrgb[1] = hudsprite.dynrgb[2] = 0;
		hudsprite.lightindex = -1;
		// set the lighting parameters
		if (hudsprite.RenderStyle.BlendOp != STYLEOP_Shadow && Level->HasDynamicLights && !isFullbrightScene() && gl_light_sprites)
		{
			GetDynSpriteLight(playermo, nullptr, hudsprite.dynrgb);
		}

		if (!hudsprite.GetWeaponRect(this, psp, spos.X, spos.Y, player, min<double>(bobFrac, frac))) continue;
		hudsprites.Push(hudsprite);
	}
	lightmode = oldlightmode;
}

void HWDrawInfo::PreparePlayerSprites3D(sector_t * viewsector, area_t in_area)
{
	// RS FORK -- HUD bone anchoring: a new pass, so anchors stored last frame
	// are no longer current. Anything whose target stops being drawn falls back
	// to its own placement instead of freezing where the target last was.
	HudAnchor_BeginFrame();

	static PClass * wpCls = PClass::FindClass("Weapon");

	static unsigned ModifyBobLayer3DVIndex = GetVirtualIndex(wpCls, "ModifyBobLayer3D");
	static unsigned ModifyBobPivotLayer3DVIndex = GetVirtualIndex(wpCls, "ModifyBobPivotLayer3D");

	static VMFunction * ModifyBobLayer3DOrigFunc = wpCls->Virtuals.Size() > ModifyBobLayer3DVIndex ? wpCls->Virtuals[ModifyBobLayer3DVIndex] : nullptr;
	static VMFunction * ModifyBobPivotLayer3DOrigFunc = wpCls->Virtuals.Size() > ModifyBobPivotLayer3DVIndex ? wpCls->Virtuals[ModifyBobPivotLayer3DVIndex] : nullptr;

	AActor * playermo = players[consoleplayer].camera;
	player_t * player = playermo->player;

	const auto &vp = Viewpoint;

	AActor *camera = vp.camera;

	// UZDXREMA: do NOT hoist the WeaponPosition3D out here - see the 2D loop.
	// GetWeaponPosition3D takes a third `DPSprite *psp` for per-hand positioning.
	WeaponLighting light = GetWeaponLighting(viewsector, vp.Pos, isFullbrightScene(), in_area, camera->Pos());

	// hack alert! Rather than changing everything in the underlying lighting code let's just temporarily change
	// light mode here to draw the weapon sprite.
	auto oldlightmode = lightmode;
	if (isSoftwareLighting(oldlightmode)) SetFallbackLightMode();

	const double bobFrac = Net_ModifyFrac(vp.TicFrac);
	for (DPSprite *psp = player->psprites; psp != nullptr && psp->GetID() < PSP_TARGETCENTER; psp = psp->GetNext())
	{
		if (weaponStabilised && psp->GetCaller() == player->OffhandWeapon)
		{
			continue;
		}
		// RS FORK -- a layer the script has hidden. Checked in BOTH passes, so
		// a suppressed weapon disappears whether it draws as a model or a
		// sprite; hiding it in one pass only would make the result depend on
		// whether the mod happened to ship a mesh.
		if (psp->NoDraw) continue;
		if (!psp->GetState()) continue;
		FSpriteModelFrame *smf = FindModelFrame(psp->Caller, psp->GetSprite(), psp->GetFrame(), false);

		// This is an 'either-or' proposition. This maybe needs some work to allow overlays with weapon models but as originally implemented this just won't work.
		if (!smf) continue;

		HUDSprite hudsprite;
		hudsprite.owner = playermo;
		hudsprite.mframe = smf;
		hudsprite.weapon = psp;

		WeaponPosition3D weap = GetWeaponPosition3D(camera->player, vp.TicFrac, psp);

		VMFunction * ModifyBobLayer3D = nullptr;
		VMFunction * ModifyBobPivotLayer3D = nullptr;

		DVector3 translation = DVector3(weap.translation);
		DVector3 rotation = DVector3(weap.rotation);
		DVector3 pivot = DVector3(weap.pivot);

		if(weap.weapon && weap.weapon->GetCaller())
		{
			PClass * cls = weap.weapon->GetCaller()->GetClass();
			ModifyBobLayer3D = cls->Virtuals.Size() > ModifyBobLayer3DVIndex ? cls->Virtuals[ModifyBobLayer3DVIndex] : nullptr;
			ModifyBobPivotLayer3D = cls->Virtuals.Size() > ModifyBobPivotLayer3DVIndex ? cls->Virtuals[ModifyBobPivotLayer3DVIndex] : nullptr;

			if( ModifyBobLayer3D == ModifyBobLayer3DOrigFunc) ModifyBobLayer3D = nullptr;
			if( ModifyBobPivotLayer3D == ModifyBobPivotLayer3DOrigFunc) ModifyBobPivotLayer3D = nullptr;
		}

		if(ModifyBobLayer3D && (psp->Flags & PSPF_ADDBOB))
		{
			DVector3 t, r;

			VMReturn returns[2];

			returns[0].Vec3At(&t);
			returns[1].Vec3At(&r);

			VMValue param[] = { weap.weapon->GetCaller() , translation.X, translation.Y, translation.Z, rotation.X, rotation.Y, rotation.Z, psp->GetID() , bobFrac };
			VMCall(ModifyBobLayer3D, param, 9, returns, 2);

			weap.translation = FVector3(t);
			weap.rotation = FVector3(r);
		}

		if(ModifyBobPivotLayer3D && (psp->Flags & PSPF_ADDBOB))
		{
			DVector3 p;

			VMReturn ret(&p);

			VMValue param[] = { weap.weapon->GetCaller() , pivot.X, pivot.Y, pivot.Z, psp->GetID() , bobFrac };
			VMCall(ModifyBobPivotLayer3D, param, 6, &ret, 1);

			weap.pivot = FVector3(p);
		}

		if (!hudsprite.GetWeaponRenderStyle(psp, camera, viewsector, light, vp.TicFrac)) continue;

		FVector2 spos = BobWeapon3D(weap, psp, hudsprite.translation, hudsprite.rotation, hudsprite.pivot, Net_ModifyObjectFrac(psp, vp.TicFrac));

		hudsprite.dynrgb[0] = hudsprite.dynrgb[1] = hudsprite.dynrgb[2] = 0;
		hudsprite.lightindex = -1;
		// set the lighting parameters
		// UZDXREMA: gl_light_weapons (not gl_light_sprites) so VR can drop dynamic
		// lighting on the weapon model independently of world sprites; upstream's
		// RF2_NODYNAMICLIGHTING actor guard is kept alongside it.
		if (hudsprite.RenderStyle.BlendOp != STYLEOP_Shadow && Level->HasDynamicLights && !isFullbrightScene() && gl_light_weapons && !(playermo->renderflags2 & RF2_NODYNAMICLIGHTING))
		{
			hw_GetDynModelLight(playermo, lightdata);
			hudsprite.lightindex = screen->mLights->UploadLights(lightdata);
			LightProbe* probe = FindLightProbe(playermo->Level, playermo->X(), playermo->Y(), playermo->Center());
			if (probe)
			{
				hudsprite.dynrgb[0] = probe->Red;
				hudsprite.dynrgb[1] = probe->Green;
				hudsprite.dynrgb[2] = probe->Blue;
			}
		}

		// [BB] In the HUD model step we just render the model and break out.
		hudsprite.mx = spos.X;
		hudsprite.my = spos.Y;

		hudsprites.Push(hudsprite);
	}
	lightmode = oldlightmode;
}

void HWDrawInfo::PreparePlayerSprites(sector_t * viewsector, area_t in_area)
{

	AActor * playermo = players[consoleplayer].camera;
	player_t * player = playermo->player;

	const auto &vp = Viewpoint;

	AActor *camera = vp.camera;

	// this is the same as the software renderer
	if (!player ||
		!r_drawplayersprites ||
		!camera->player ||
		(player->cheats & CF_CHASECAM) ||
		(r_deathcamera && camera->health <= 0))
		return;

	// UZDXREMA: both passes run unconditionally. They are complementary filters,
	// not alternatives - PreparePlayerSprites2D skips any psprite that HAS a model
	// frame (`if (smf) continue;`) and PreparePlayerSprites3D skips any psprite
	// that does NOT (`if (!smf) continue;`). A VR player therefore gets the 3D
	// weapon model AND its 2D psprite overlay layers: muzzle flashes, the laser
	// sight pointer/dot and script HUD overlays.
	//
	// Do NOT reintroduce upstream's `IsHUDModelForPlayerAvailable()` either/or
	// branch here. It compiles and runs, and silently drops every 2D psprite layer
	// the moment the weapon has a model frame - which in this fork is essentially
	// always.
	PreparePlayerSprites3D(viewsector,in_area);
	PreparePlayerSprites2D(viewsector,in_area);

	PrepareTargeterSprites(vp.TicFrac);
}


//==========================================================================
//
// R_DrawPlayerSprites
//
//==========================================================================

void HWDrawInfo::PrepareTargeterSprites(double ticfrac)
{
	AActor * playermo = players[consoleplayer].camera;
	player_t * player = playermo->player;
	AActor *camera = Viewpoint.camera;

	// this is the same as above
	if (!player ||
		!r_drawplayersprites ||
		!camera->player ||
		(player->cheats & CF_CHASECAM) ||
		(r_deathcamera && camera->health <= 0))
		return;

	HUDSprite hudsprite;

	hudsprite.owner = playermo;
	hudsprite.mframe = nullptr;
	hudsprite.cm.Clear();
	hudsprite.lightlevel = 255;
	hudsprite.bright = true;  // RS fork: targeter is drawn fullbright, so darkness exempts it
	hudsprite.ObjectColor = 0xffffffff;
	hudsprite.AddColor = 0;   // RS fork
	hudsprite.alpha = 1;
	hudsprite.RenderStyle = DefaultRenderStyle();
	hudsprite.OverrideShader = -1;
	hudsprite.dynrgb[0] = hudsprite.dynrgb[1] = hudsprite.dynrgb[2] = 0;

	// The Targeter's sprites are always drawn normally.
	for (DPSprite *psp = player->FindPSprite(PSP_TARGETCENTER); psp != nullptr; psp = psp->GetNext())
	{
		if (psp->GetState() != nullptr && (psp->GetID() != PSP_TARGETCENTER || CrosshairImage == nullptr))
		{
			hudsprite.weapon = psp;

			if (hudsprite.GetWeaponRect(this, psp, psp->x, psp->y, player, Net_ModifyObjectFrac(psp, ticfrac)))
			{
				hudsprites.Push(hudsprite);
			}
		}
	}
}
