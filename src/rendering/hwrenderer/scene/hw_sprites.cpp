/*
** hw_sprites.cpp
**
** Sprite/Particle rendering
**
**---------------------------------------------------------------------------
**
** Copyright 2002-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include "p_local.h"
#include "p_effect.h"
#include "g_level.h"
#include "doomstat.h"
#include "c_cvars.h"
#include "r_defs.h"
#include "r_sky.h"
#include "r_utility.h"
#include "a_pickups.h"
#include "a_corona.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "events.h"
#include "actorinlines.h"
#include "r_data/r_vanillatrans.h"
#include "matrix.h"
#include "i_time.h"
#include "models.h"
#include "vectors.h"
#include "texturemanager.h"
#include "hw_sdffont.h"
#include "v_font.h"
#include "basics.h"
#include "d_net.h"

#include "hw_models.h"
#include "hwrenderer/scene/hw_drawstructs.h"
#include "hwrenderer/scene/hw_drawinfo.h"
#include "hwrenderer/scene/hw_fakeflat.h"
#include "hwrenderer/scene/hw_portal.h"
#include "flatvertices.h"
#include "hw_cvars.h"
#include "hw_clock.h"
#include "hw_lighting.h"
#include "hw_material.h"
#include "hw_dynlightdata.h"
#include "hw_lightbuffer.h"
#include "hw_renderstate.h"
#include "hw_effectlightbuffer.h"	// [EFFECTLIGHTS] EffectLightBuffer::Instance()->GetLiveCount(): the per-draw effect-light gate
#include "quaternion.h"
#include "hw_vrmodes.h"

#include "p_visualthinker.h"

extern TArray<spritedef_t> sprites;
extern TArray<spriteframe_t> SpriteFrames;
extern uint32_t r_renderercaps;

const float LARGE_VALUE = 1e19f;
const float MY_SQRT2    = 1.41421356237309504880; // sqrt(2)

EXTERN_CVAR(Bool, r_debug_disable_vis_filter)
EXTERN_CVAR(Float, transsouls)
EXTERN_CVAR(Float, r_actorspriteshadowalpha)
EXTERN_CVAR(Float, r_actorspriteshadowfadeheight)
EXTERN_CVAR(Float, r_voxeldistance)
EXTERN_CVAR(Float, r_voxeldistance_band)	// RS fork -- hysteresis for the voxel distance cull
EXTERN_CVAR(Bool, gl_texture_thread)
EXTERN_CVAR(Bool, gl_texture_thread_models)

//==========================================================================
//
// Sprite CVARs
//
//==========================================================================

// [BB] Which way a billboard's texture runs across its face. The basis
// vectors follow the panel system's documented convention (right is
// (sin y, -cos y, 0)), but handedness bugs of this kind are invisible until
// something with text on it renders backwards -- and this has bitten the
// project before. A cvar makes it a five-second fix in the headset instead
// of a rebuild, the same way the panel code handles its pitch bias.
CVAR(Bool, bb_flipu, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// [BB] Global presentation adjustments, applied to every billboard as it is
// drawn. These exist because the right value for each is a matter of taste
// and of headset, and neither is knowable from here -- comfortable card size
// and the tilt that makes a panel look upright both differ per person.
CVAR(Float, bb_scale, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, bb_tiltbias, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// [BB] Which distance-field atlas BB_TEXT draws with. Names a texture and,
// beside it, "sdffonts/<name>.txt". Set it to something that does not exist
// and text falls back to the bitmap font rather than disappearing.
//
// KEEP THE NAME TO EIGHT CHARACTERS. The texture manager does not find a
// longer one -- "sdfpixmono" resolved to nothing and every string quietly drew
// through the bitmap path instead, which looks like the atlas simply being bad
// rather than never being consulted.
CVAR(String, bb_sdffont, "sdfmono", CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, gl_usecolorblending, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, gl_sprite_blend, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Int, gl_spriteclip, -1, CVAR_ARCHIVE)
CVAR(Bool, r_debug_nolimitanamorphoses, false, 0)
CVAR(Float, r_spriteclipanamorphicminbias, 0.6, CVAR_ARCHIVE)
CVAR(Float, gl_sclipthreshold, 10.0, CVAR_ARCHIVE)
CVAR(Float, gl_sclipfactor, 1.8f, CVAR_ARCHIVE)
CVAR(Int, gl_particles_style, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) // 0 = square, 1 = round, 2 = smooth
CVAR(Int, gl_billboard_mode, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, gl_billboard_faces_camera, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)  // for VR should be true
CVAR(Bool, hw_force_cambbpref, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, gl_billboard_particles, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CUSTOM_CVAR(Int, gl_fuzztype, 8, CVAR_ARCHIVE)
{
	if (self < 0 || self > 8) self = 0;
}

//==========================================================================
//
//
//
//==========================================================================

// [BB] Defined in hw_walls.cpp, which is where every other caller lives. It is
// not in a header because until now nothing outside that file wanted it.
// [G6] Takes the SECTOR, because it sets that sector's glow seed with its planes
// ("Engine docs/GLOW_SEAM_SEED_G6_IMPL_NOTES.md").
void SetGlowPlanes(FRenderState &state, sector_t *sec);

float FogScaleForSector(FLevelLocals *Level, sector_t *sec);

void HWSprite::DrawSprite(HWDrawInfo *di, FRenderState &state, bool translucent)
{
	state.SetFogDensityScale(FogScaleForSector(di->Level, actor ? actor->Sector : nullptr));
	bool additivefog = false;
	bool foglayer = false;
	int rel = fullbright ? 0 : getExtraLight();
	auto &vp = di->Viewpoint;

	// [BB] THE FOG SLAB NEEDS THIS SPRITE'S OWN FLOOR.
	//
	// SetFogFollow resolves the slab's top against uGlowBottomPlane, per
	// fragment -- that is the whole mechanism by which mist sits at a
	// different world height in every room without anyone scripting it.
	//
	// Walls set those planes (hw_walls.cpp) and flats set them
	// (hw_flats.cpp). Sprites never did. FRenderState::Reset zeroes them, and
	// FogSlabAt runs in main.fp BEFORE the vWorldNormal test that would have
	// told it this fragment is a sprite -- so an actor was fogged against
	// whichever wall or flat happened to be drawn before it, or against zero
	// after a reset. A monster standing in a pit was fogged for the walkway
	// above it, which is the one place the error is most visible.
	//
	// PLANES ONLY, deliberately. EnableGlow stays off and is not touched here;
	// the sector glow itself is gated on uGlowTopColor.a / uGlowBottomColor.a
	// rather than on these planes, so a sprite still does not pick up a
	// sector's glow colour. This is the fog's input and nothing else's.
	{
		sector_t *fogsec = nullptr;
		if (actor != nullptr) fogsec = actor->Sector;
		else if (particle != nullptr && particle->subsector != nullptr) fogsec = particle->subsector->sector;

		if (fogsec != nullptr)
			SetGlowPlanes(state, fogsec);
	}

	if (translucent)
	{
		bool translucentCanvas = false;
		if (texture && texture->isHardwareCanvas())
		{
			auto* canvasTex = static_cast<FCanvasTexture*>(texture->GetTexture());
			translucentCanvas = (canvasTex != nullptr && canvasTex->bTranslucentCanvas);
		}

		// The translucent pass requires special setup for the various modes.

		// for special render styles brightmaps would not look good - especially for subtractive.
		if (RenderStyle.BlendOp != STYLEOP_Add)
		{
			state.EnableBrightmap(false);
		}

		// Optionally use STYLE_ColorBlend in place of STYLE_Add for fullbright items.
		if (RenderStyle == LegacyRenderStyles[STYLE_Add] && trans > 1.f - FLT_EPSILON &&
			gl_usecolorblending && !di->isFullbrightScene() && actor &&
			fullbright && texture && !texture->GetTranslucency() && !translucentCanvas)
		{
			RenderStyle = LegacyRenderStyles[STYLE_ColorAdd];
		}

		state.SetRenderStyle(RenderStyle);
		state.SetTextureMode(RenderStyle);

		// [BB] A fullbright sprite is declaring that it is NOT lit by the room,
		// so world darkness -- which scales the light a room has -- has nothing
		// to say about it. This is the same rule the glow already gets: the
		// darkness pass runs before it because it is emissive, and this is that
		// rule applied to the other emissive case.
		//
		// The UI billboards depend on it. They are marked fullbright precisely
		// so a panel stays readable in a dark room (see HWSprite::ProcessBillboard
		// below), and fullbright is implemented as lightlevel = 255 -- which
		// arrives here indistinguishable from a brightly lit surface and gets
		// scaled to nothing along with everything else.
		// Fullbright things are exempt outright; everything else takes as much
		// of the darkening as the level says actors should. See
		// FLevelLocals::DarkActorExempt.
		state.SetDarknessExempt(fullbright ? 1.f : (float)di->Level->DarkActorExempt);
		if (translucentCanvas)
		{
			state.SetTextureMode(TM_NORMAL);
		}

		if (hw_styleflags == STYLEHW_NoAlphaTest)
		{
			state.AlphaFunc(Alpha_GEqual, 0.f);
		}
		else if (!texture || (!texture->GetTranslucency() && !translucentCanvas)) state.AlphaFunc(Alpha_GEqual, gl_mask_sprite_threshold);
		else state.AlphaFunc(Alpha_Greater, 0.f);

		if (RenderStyle.BlendOp == STYLEOP_Shadow)
		{
			float fuzzalpha = 0.44f;
			float minalpha = 0.1f;

			// fog + fuzz don't work well without some fiddling with the alpha value!
			if (!Colormap.FadeColor.isBlack())
			{
				float dist = Dist2(vp.Pos.X, vp.Pos.Y, x, y);
				int fogd = GetFogDensity(di->Level, di->lightmode, lightlevel, Colormap.FadeColor, Colormap.FogDensity, Colormap.BlendFactor);

				// this value was determined by trial and error and is scale dependent!
				float factor = 0.05f + exp(-fogd * dist / 62500.f);
				fuzzalpha *= factor;
				minalpha *= factor;
			}

			state.AlphaFunc(Alpha_GEqual, gl_mask_sprite_threshold);
			state.SetColor(0.2f, 0.2f, 0.2f, fuzzalpha, Colormap.Desaturation);
			additivefog = true;
			lightlist = nullptr;	// the fuzz effect does not use the sector's light di->Level-> so splitting is not needed.
		}
		else if (RenderStyle.BlendOp == STYLEOP_Add && RenderStyle.DestAlpha == STYLEALPHA_One)
		{
			additivefog = true;
		}
	}
	else if (modelframe == nullptr)
	{
		// This still needs to set the texture mode. As blend mode it will always use GL_ONE/GL_ZERO
		state.SetTextureMode(RenderStyle);
		state.SetDepthBias(-1, -128);
	}
	// [OUTLINE] Neon outline traced from this actor's own sprite. HERE rather
	// than earlier in the function because the alpha-func calls above are what
	// the wire mode has to override, and they run inside the translucency
	// branch.
	//
	// Per draw call, so one corpse can be traced without every monster in the
	// room being traced with it. FRenderState::Reset puts mode back to 0 for
	// everything else in the frame, and mode 0 is one float compare.
	if (actor != nullptr && actor->OutlineMode > 0 && actor->OutlineStrength > 0.0)
	{
		state.SetSpriteOutline(actor->OutlineColorA, actor->OutlineColorB,
			(float)actor->OutlineStrength, (float)actor->OutlineThickness,
			(float)actor->OutlineThreshold, (float)actor->OutlineGlow,
			(float)actor->OutlinePulse, actor->OutlineMode);

		// Wire mode replaces the sprite's alpha with the traced edge, and the
		// usual masked-sprite threshold would then cut off the soft half of
		// every line -- which is the half that reads as glow.
		if (actor->OutlineMode == 2) state.AlphaFunc(Alpha_GEqual, 0.f);
	}
	else
	{
		state.ClearSpriteOutline();
	}

	// [EFFECTLIGHTS] Effect lights on this sprite or model ("Engine docs/EFFECT_LIGHTS_LB_IMPL_NOTES.md"), on what takes dynamic
	// light today, whether or not the level has dynamic lights: never a fuzz shadow, a fullbright thing or a fullbright scene. A
	// model lit per pixel (PutSprite's test) takes mode 1, with N.L. A sprite, a particle, a voxel or a model without per-pixel
	// lighting takes mode 2, without N.L (a sprite's normal is zero), where GetDynSpriteLight would light it: an actor with
	// gl_light_sprites, a particle with gl_light_particles, neither asking for no dynamic light (so never a billboard). Set only on
	// frames with an effect light on the GPU and put back to 0 at the end, so every draw on a frame with none uploads exactly what
	// it did.
	int effectLightMode = 0;
	if (RenderStyle.BlendOp != STYLEOP_Shadow && !di->isFullbrightScene() && !fullbright &&
		EffectLightBuffer::Instance() != nullptr && EffectLightBuffer::Instance()->GetLiveCount() > 0)
	{
		const bool litActor = gl_light_sprites && actor != nullptr && !(actor->renderflags2 & RF2_NODYNAMICLIGHTING);
		if (modelframe != nullptr && !modelframe->isVoxel && !(modelframeflags & MDL_NOPERPIXELLIGHTING))
			effectLightMode = litActor ? 1 : 0;
		else if (litActor || (gl_light_particles && particle != nullptr && !(particle->flags & SPF_NODYNAMICLIGHTING)))
			effectLightMode = 2;
		if (effectLightMode != 0) state.SetEffectLightMode(effectLightMode);
	}

	if (RenderStyle.BlendOp != STYLEOP_Shadow)
	{
		if (di->Level->HasDynamicLights && !di->isFullbrightScene() && !fullbright)
		{
			if (dynlightindex == -1)	// only set if we got no light buffer index. This covers all cases where sprite lighting is used.
			{
				float out[3] = {};
				di->GetDynSpriteLight(gl_light_sprites ? actor : nullptr, gl_light_particles ? particle : nullptr, out);
				state.SetDynLight(out[0], out[1], out[2]);
			}
		}
		sector_t *cursec = actor ? actor->Sector : particle ? particle->subsector->sector : nullptr;
		if (cursec != nullptr)
		{
			const PalEntry finalcol = fullbright
				? ThingColor
				: ThingColor.Modulate(cursec->SpecialColors[sector_t::sprites]);

			state.SetObjectColor(finalcol);
			state.SetAddColor(cursec->AdditiveColors[sector_t::sprites] | 0xff000000);
		}
		else if (isBillboard)
		{
			// BILLBOARDS HAVE NO SECTOR, AND THAT WAS SILENTLY THROWING THEIR
			// COLOUR AWAY. Fixed 2026-08-08.
			//
			// ProcessBillboard sets neither `actor` nor `particle`
			// (:2010-2011) -- correctly, a billboard is neither -- so
			// `cursec` is always null and this whole block was skipped. The
			// draw state kept the white reset from :385, and EVERY billboard
			// rendered white no matter what colour was passed to
			// AddBillboard(). Tier colours, meters, label/value contrast: all
			// discarded, with nothing logged.
			//
			// A billboard is a UI primitive placed in world space, not a
			// thing standing in a room, so it deliberately takes its colour
			// straight through without the sector's sprite tint or additive
			// colour applied. That is also what makes it predictable: the
			// colour a caller asks for is the colour it gets, in any sector.
			state.SetObjectColor(ThingColor);

			// [BB] The halo rides here. uAddColor is otherwise untouched on
			// this path -- the sector branch above sets it, the billboard
			// branch never did -- so it is free carriage for two numbers the
			// SDF shader needs and nothing else reads. The shader ignores it
			// unless it is the one running, and every other payload leaves
			// this zero.
			state.SetAddColor(bbGlow);
			// uObjectColor2 is untouched on this path, so the gradient's far
			// end rides there. Its ALPHA is the switch -- the shader treats 0
			// as "no gradient" rather than as a transparent black to blend
			// toward, which would darken everything that never asked.
			state.SetObjectColor2(bbColor2);
		}
		SetColor(state, di->Level, di->lightmode, lightlevel, rel, di->isFullbrightScene(), Colormap, trans);
	}


	if (Colormap.FadeColor.isBlack()) foglevel = lightlevel;

	if (RenderStyle.Flags & STYLEF_FadeToBlack)
	{
		Colormap.FadeColor = 0;
		additivefog = true;
	}

	if (RenderStyle.BlendOp == STYLEOP_RevSub || RenderStyle.BlendOp == STYLEOP_Sub)
	{
		if (!modelframe)
		{
			// non-black fog with subtractive style needs special treatment
			if (!Colormap.FadeColor.isBlack())
			{
				foglayer = true;
				// Due to the two-layer approach we need to force an alpha test that lets everything pass
				state.AlphaFunc(Alpha_Greater, 0);
			}
		}
		else RenderStyle.BlendOp = STYLEOP_Fuzz;	// subtractive with models is not going to work.
	}

	if (!foglayer) SetFog(state, di->Level, di->lightmode, foglevel, rel, di->isFullbrightScene(), &Colormap, additivefog);
	else
	{
		state.EnableFog(false);
		state.SetFog(0, 0);
	}

	int clampmode = CLAMP_XY;

	if (texture && texture->isNoMipmap())
	{
		clampmode = CLAMP_XY_NOMIP;
	}

	uint32_t spritetype = actor? uint32_t(actor->renderflags & RF_SPRITETYPEMASK) : 0;
	if (texture) state.SetMaterial(texture, UF_Sprite, (spritetype == RF_FACESPRITE) ? CTF_Expand : 0, clampmode, translation, OverrideShader);
	else if (!modelframe) state.EnableTexture(false);

	//SetColor(lightlevel, rel, Colormap, trans);

	unsigned int iter = lightlist ? lightlist->Size() : 1;
	bool clipping = false;
	if (lightlist || topclip != LARGE_VALUE || bottomclip != -LARGE_VALUE)
	{
		clipping = true;
		state.EnableSplit(true);
	}

	secplane_t bottomp = { { 0, 0, -1. }, bottomclip, 1. };
	secplane_t topp = { { 0, 0, -1. }, topclip, 1. };
	for (unsigned i = 0; i < iter; i++)
	{
		if (lightlist)
		{
			// set up the light slice
			secplane_t *topplane = i == 0 ? &topp : &(*lightlist)[i].plane;
			secplane_t *lowplane = i == (*lightlist).Size() - 1 ? &bottomp : &(*lightlist)[i + 1].plane;
			int thislight = (*lightlist)[i].caster != nullptr ? RescaleLightLevel(*(*lightlist)[i].p_lightlevel) : lightlevel;
			int thisll = actor == nullptr ? thislight : (uint8_t)actor->Sector->CheckSpriteGlow(thislight, actor->InterpolatedPosition(vp.TicFrac));

			FColormap thiscm;
			thiscm.CopyFog(Colormap);
			CopyFrom3DLight(thiscm, &(*lightlist)[i]);
			if (di->Level->flags3 & LEVEL3_NOCOLOREDSPRITELIGHTING)
			{
				thiscm.Decolorize();
			}

			SetColor(state, di->Level, di->lightmode, thisll, rel, di->isFullbrightScene(), thiscm, trans);
			if (!foglayer)
			{
				SetFog(state, di->Level, di->lightmode, thislight, rel, di->isFullbrightScene(), &thiscm, additivefog);
			}
			SetSplitPlanes(state, *topplane, *lowplane);
		}
		else if (clipping)
		{
			SetSplitPlanes(state, topp, bottomp);
		}

		if (!modelframe)
		{
			state.SetNormal(0, 0, 0);


			if (screen->BuffersArePersistent())
			{
				CreateVertices(di);
			}
			if (polyoffset)
			{
				state.SetDepthBias(-1, -128);
			}
			// [BB] A no-depth billboard draws over the world rather than
			// being occluded by it. This is what a HUD-locked panel needs:
			// welded to the view, it would otherwise be sliced in half every
			// time the player backs into a wall.
			if (isBillboard && bbNoDepth)
			{
				state.SetDepthFunc(DF_Always);
			}
			state.SetLightIndex(-1);
			state.Draw(DT_TriangleStrip, vertexindex, 4);
			if (isBillboard && bbNoDepth)
			{
				state.SetDepthFunc(DF_LEqual);
			}

			if (foglayer)
			{
				// If we get here we know that we have colored fog and no fixed colormap.
				SetFog(state, di->Level, di->lightmode, foglevel, rel, false, &Colormap, additivefog);
				state.SetTextureMode(TM_FOGLAYER);
				state.SetRenderStyle(STYLE_Translucent);
				state.Draw(DT_TriangleStrip, vertexindex, 4);
				state.SetTextureMode(TM_NORMAL);
			}
		}
		else
		{
			if (actor && di->Level->LightProbes.Size() > 0)
			{
				LightProbe* probe = FindLightProbe(di->Level, actor->X(), actor->Y(), actor->Center());
				if (probe)
					state.SetDynLight(probe->Red, probe->Green, probe->Blue);
			}

			if(actor && (actor->flags9 & MF9_DECOUPLEDANIMATIONS))
			{
				IFVIRTUALPTR(actor, AActor, AnimateBones)
				{
					CallVM<void>(func, actor, di->Viewpoint.TicFrac);
				}
			}

			FHWModelRenderer renderer(di, state, dynlightindex);
			RenderModel(&renderer, x, y, z, modelframe, actor, Net_ModifyObjectFrac(actor, di->Viewpoint.TicFrac));
			state.SetVertexBuffer(screen->mVertexData);
		}
	}

	if (clipping)
	{
		state.EnableSplit(false);
	}

	if (translucent)
	{
		state.EnableBrightmap(true);
		state.SetRenderStyle(STYLE_Translucent);
		state.SetTextureMode(TM_NORMAL);
		if (actor != nullptr && (actor->renderflags & RF_SPRITETYPEMASK) == RF_FLATSPRITE)
		{
			state.ClearDepthBias();
		}
	}
	else if (modelframe == nullptr)
	{
		state.ClearDepthBias();
	}

	state.SetObjectColor(0xffffffff);
	state.SetAddColor(0);
	state.EnableTexture(true);
	state.SetDynLight(0, 0, 0);
	state.SetDarknessExempt(0.f);
	if (effectLightMode != 0) state.SetEffectLightMode(0);	// [EFFECTLIGHTS]
}

//==========================================================================
//
//
//
//==========================================================================

void HandleSpriteOffsets(Matrix3x4 *mat, const FRotator *HW, FVector2 *offset, bool XYBillboard)
{
	FAngle zero = FAngle::fromDeg(0);
	FAngle pitch = (XYBillboard) ? HW->Pitch : zero;
	FAngle yaw = FAngle::fromDeg(270.) - HW->Yaw;

	FQuaternion quat = FQuaternion::FromAngles(yaw, pitch, zero);
	FVector3 sideVec = quat * FVector3(0, 1, 0);
	FVector3 upVec = quat * FVector3(0, 0, 1);
	FVector3 res = sideVec * offset->X + upVec * offset->Y;
	mat->Translate(res.X, res.Z, res.Y);
}

bool HWSprite::CalculateVertices(HWDrawInfo* di, FVector3* v, DVector3* vp)
{
	float pixelstretch = di->Level->pixelstretch;

	// [BB] Billboards solved their own corners in ProcessBillboard, from an
	// explicit yaw/tilt rather than from actor renderflags. Nothing below
	// applies to them.
	if (isBillboard)
	{
		v[0] = bbVerts[0];
		v[1] = bbVerts[1];
		v[2] = bbVerts[2];
		v[3] = bbVerts[3];
		return false;
	}

	FVector3 center = FVector3((x1 + x2) * 0.5, (y1 + y2) * 0.5, (z1 + z2) * 0.5);
	const auto& HWAngles = di->Viewpoint.HWAngles;
	Matrix3x4 mat;
	if (actor != nullptr && (actor->renderflags & RF_SPRITETYPEMASK) == RF_FLATSPRITE)
	{
		// [MC] Rotate around the center or offsets given to the sprites.
		// Counteract any existing rotations, then rotate the angle.
		// Tilt the actor up or down based on pitch (increase 'somersaults' forward).
		// Then counteract the roll and DO A BARREL ROLL.

		mat.MakeIdentity();
		FAngle pitch = FAngle::fromDeg(-Angles.Pitch.Degrees());
		pitch.Normalized180();

		mat.Translate(x, z, y);
		mat.Rotate(0, 1, 0, 270. - Angles.Yaw.Degrees());
		mat.Rotate(1, 0, 0, pitch.Degrees());

		if (actor->renderflags & RF_ROLLCENTER)
		{
			mat.Translate(center.X - x, 0, center.Y - y);
			mat.Rotate(0, 1, 0, - Angles.Roll.Degrees());
			mat.Translate(-center.X, -z, -center.Y);
		}
		else
		{
			mat.Rotate(0, 1, 0, - Angles.Roll.Degrees());
			mat.Translate(-x, -z, -y);
		}
		v[0] = mat * FVector3(x2, z, y2);
		v[1] = mat * FVector3(x1, z, y2);
		v[2] = mat * FVector3(x2, z, y1);
		v[3] = mat * FVector3(x1, z, y1);

		return true;
	}

	// [DVR] +ANGLEDROLL
	const bool AngledRoll = (actor != nullptr && actor->renderflags2 & RF2_ANGLEDROLL);

	// [BB] Billboard stuff
	const bool drawWithXYBillboard = ((particle && gl_billboard_particles && !(particle->flags & SPF_NO_XY_BILLBOARD)) || (!(actor && actor->renderflags & RF_FORCEYBILLBOARD)
		//&& di->mViewActor != nullptr
		&& (gl_billboard_mode == 1 || (actor && actor->renderflags & RF_FORCEXYBILLBOARD)))) && !AngledRoll;

	bool drawBillboardFacingCamera = gl_billboard_faces_camera;
	if (!hw_force_cambbpref)
	{
		if (actor)
		{
			if (actor->renderflags2 & RF2_BILLBOARDNOFACECAMERA)
				drawBillboardFacingCamera = false;
			else if (actor->renderflags2 & RF2_BILLBOARDFACECAMERA)
				drawBillboardFacingCamera = true;
		}
		else if (particle && particle->texture.isValid())
		{
			if (particle->flags & SPF_NOFACECAMERA)
				drawBillboardFacingCamera = false;
			else if (particle->flags & SPF_FACECAMERA)
				drawBillboardFacingCamera = true;
		}
	}

	// [Nash] has +ROLLSPRITE
	const bool drawRollSpriteActor = (actor != nullptr && actor->renderflags & RF_ROLLSPRITE);
	const bool drawRollParticle = (particle != nullptr && particle->flags & SPF_ROLL);
	const bool doRoll = (drawRollSpriteActor || drawRollParticle);

	// [fgsfds] check sprite type mask
	uint32_t spritetype = (uint32_t)-1;
	if (actor != nullptr) spritetype = actor->renderflags & RF_SPRITETYPEMASK;

	// [Nash] is a flat sprite
	const bool isWallSprite = (actor != nullptr) && (spritetype == RF_WALLSPRITE);
	const bool useOffsets = ((actor != nullptr) && !(actor->renderflags & RF_ROLLCENTER)) || (particle && !(particle->flags & SPF_ROLLCENTER));

	FVector2 offset = FVector2( offx, offy );
	float xx = -center.X + x;
	float yy = -center.Y + y;
	float zz = -center.Z + z;
	// [Nash] check for special sprite drawing modes
	if (drawWithXYBillboard || drawBillboardFacingCamera || isWallSprite)
	{
		mat.MakeIdentity();
		mat.Translate(center.X, center.Z, center.Y); // move to sprite center
		mat.Scale(1.0, 1.0/pixelstretch, 1.0);	// unstretch sprite by level aspect ratio

		// [MC] Sprite offsets.
		if (!offset.isZero())
			HandleSpriteOffsets(&mat, &HWAngles, &offset, true);

		// Order of rotations matters. Perform yaw rotation (Y, face camera) before pitch (X, tilt up/down).
		if (drawBillboardFacingCamera && !isWallSprite)
		{
			// [CMB] Rotate relative to camera XY position, not just camera direction,
			// which is nicer in VR
			float xrel = center.X - vp->X;
			float yrel = center.Y - vp->Y;
			float absAngleDeg = atan2(-yrel, xrel) * (180 / M_PI);
			float counterRotationDeg = 270. - HWAngles.Yaw.Degrees(); // counteracts existing sprite rotation
			float relAngleDeg = counterRotationDeg + absAngleDeg;

			mat.Rotate(0, 1, 0, relAngleDeg);
		}

		// [fgsfds] calculate yaw vectors
		float rollDegrees = doRoll ? Angles.Roll.Degrees() : 0;
		float angleRad = (FAngle::fromDeg(270.) - HWAngles.Yaw).Radians();

		// [fgsfds] Rotate the sprite about the sight vector (roll)
		if (isWallSprite)
		{
			float yawvecX = Angles.Yaw.Cos();
			float yawvecY = Angles.Yaw.Sin();
			mat.Rotate(0, 1, 0, 0);
			if (drawRollSpriteActor)
			{

				if (useOffsets) mat.Translate(xx, zz, yy);
				mat.Rotate(yawvecX, 0, yawvecY, rollDegrees);
				if (useOffsets) mat.Translate(-xx, -zz, -yy);
			}
		}
		else if (doRoll)
		{
			if (useOffsets) mat.Translate(xx, zz, yy);
			if (drawWithXYBillboard)
			{
				mat.Rotate(-sin(angleRad), 0, cos(angleRad), -HWAngles.Pitch.Degrees());
			}
			mat.Rotate(cos(angleRad), 0, sin(angleRad), rollDegrees);
			if (useOffsets) mat.Translate(-xx, -zz, -yy);
		}
		else if (drawWithXYBillboard)
		{
			// Rotate the sprite about the vector starting at the center of the sprite
			// triangle strip and with direction orthogonal to where the player is looking
			// in the x/y plane.
			mat.Rotate(-sin(angleRad), 0, cos(angleRad), -HWAngles.Pitch.Degrees());
		}

		mat.Scale(1.0, pixelstretch, 1.0);	// stretch sprite by level aspect ratio
		mat.Translate(-center.X, -center.Z, -center.Y); // retreat from sprite center

		v[0] = mat * FVector3(x1, z1, y1);
		v[1] = mat * FVector3(x2, z1, y2);
		v[2] = mat * FVector3(x1, z2, y1);
		v[3] = mat * FVector3(x2, z2, y2);
	}
	else // traditional "Y" billboard mode
	{
		if (doRoll || !offset.isZero() || (actor && (actor->renderflags2 & RF2_ISOMETRICSPRITES)))
		{
			mat.MakeIdentity();

			if (!offset.isZero())
				HandleSpriteOffsets(&mat, &HWAngles, &offset, false);

			if (doRoll)
			{
				// Compute center of sprite
				float angleRad = (FAngle::fromDeg(270.) - HWAngles.Yaw).Radians();
				float rollDegrees = Angles.Roll.Degrees();
				float pitchDegrees = 0.0;

				if (AngledRoll)
				{
					rollDegrees = fmodf(rollDegrees, 360.0f);
					DAngle ang = di->Viewpoint.Angles.Yaw + actor->Angles.Yaw + actor->AngledRollOffset - DAngle::fromDeg(90.0);
					angleRad = ang.Radians();
					FVector3 relPos = center - FVector3(di->Viewpoint.Pos);
					if (useOffsets) relPos += FVector3(xx, yy, zz);

					Matrix3x4 rolltilt;
					rolltilt.MakeIdentity();
					ang = actor->Angles.Yaw + actor->AngledRollOffset;
					rolltilt.Rotate(ang.Cos(), ang.Sin(), 0.0, -rollDegrees);
					pitchDegrees = 270.0 - DVector3(rolltilt * relPos).Angle().Degrees();
				}

				mat.Translate(center.X, center.Z, center.Y);
				if (useOffsets) mat.Translate(xx, zz, yy);
				mat.Scale(1.0, 1.0/pixelstretch, 1.0);	// unstretch sprite by level aspect ratio
				if (AngledRoll) mat.Rotate(0.0, 1.0, 0.0, -HWAngles.Yaw.Degrees()); // Cancel regular Y-billboarding
				mat.Rotate(cos(angleRad), 0, sin(angleRad), rollDegrees);
				if (AngledRoll) mat.Rotate(0.0, 1.0, 0.0, pitchDegrees); // New Y-billboarding about rolled z-axis
				mat.Scale(1.0, pixelstretch, 1.0);	// stretch sprite by level aspect ratio
				if (useOffsets) mat.Translate(-xx, -zz, -yy);
				mat.Translate(-center.X, -center.Z, -center.Y);
			}

			if (actor && (actor->renderflags2 & RF2_ISOMETRICSPRITES) && di->Viewpoint.bDoOrtho)
			{
				float angleRad = (FAngle::fromDeg(270.) - HWAngles.Yaw).Radians();
				mat.Translate(center.X, center.Z, center.Y);
				mat.Translate(0.0, z2 - center.Z, 0.0);
				mat.Rotate(-sin(angleRad), 0, cos(angleRad), -actor->isotheta);
				mat.Translate(0.0, center.Z - z2, 0.0);
				mat.Translate(-center.X, -center.Z, -center.Y);
			}

			v[0] = mat * FVector3(x1, z1, y1);
			v[1] = mat * FVector3(x2, z1, y2);
			v[2] = mat * FVector3(x1, z2, y1);
			v[3] = mat * FVector3(x2, z2, y2);

		}
		else
		{
			v[0] = FVector3(x1, z1, y1);
			v[1] = FVector3(x2, z1, y2);
			v[2] = FVector3(x1, z2, y1);
			v[3] = FVector3(x2, z2, y2);
		}

	}
	return false;
}

//==========================================================================
//
//
//
//==========================================================================

inline void HWSprite::PutSprite(HWDrawInfo *di, bool translucent)
{
	// That's a lot of checks...
	if (modelframe && !modelframe->isVoxel && !(modelframeflags & MDL_NOPERPIXELLIGHTING) && RenderStyle.BlendOp != STYLEOP_Shadow
		&& gl_light_sprites && di->Level->HasDynamicLights && !di->isFullbrightScene() && !fullbright
		&& actor && !(actor->renderflags2 & RF2_NODYNAMICLIGHTING))
	{
		hw_GetDynModelLight(actor, lightdata);
		dynlightindex = screen->mLights->UploadLights(lightdata);
	}
	else
		dynlightindex = -1;

	vertexindex = -1;
	if (!screen->BuffersArePersistent())
	{
		CreateVertices(di);
	}
	di->AddSprite(this, translucent);
}

//==========================================================================
//
//
//
//==========================================================================

void HWSprite::CreateVertices(HWDrawInfo *di)
{
	if (modelframe == nullptr)
	{
		FVector3 v[4];
		polyoffset = CalculateVertices(di, v, &di->Viewpoint.CenterEyePos);
		auto vert = screen->mVertexData->AllocVertices(4);
		auto vp = vert.first;
		vertexindex = vert.second;

		vp[0].Set(v[0][0], v[0][1], v[0][2], ul, vt);
		vp[1].Set(v[1][0], v[1][1], v[1][2], ur, vt);
		vp[2].Set(v[2][0], v[2][1], v[2][2], ul, vb);
		vp[3].Set(v[3][0], v[3][1], v[3][2], ur, vb);
	}

}


//==========================================================================
//
//
//
//==========================================================================

void HWSprite::SplitSprite(HWDrawInfo *di, sector_t * frontsector, bool translucent)
{
	HWSprite copySprite;
	double lightbottom;
	unsigned int i;
	bool put=false;
	TArray<lightlist_t> & lightlist=frontsector->e->XFloor.lightlist;

	for(i=0;i<lightlist.Size();i++)
	{
		// Particles don't go through here so we can safely assume that actor is not nullptr
		if (i<lightlist.Size()-1) lightbottom=lightlist[i+1].plane.ZatPoint(actor);
		else lightbottom=frontsector->floorplane.ZatPoint(actor);

		if (lightbottom<z2) lightbottom=z2;

		if (lightbottom<z1)
		{
			copySprite=*this;
			copySprite.lightlevel = RescaleLightLevel(*lightlist[i].p_lightlevel);
			copySprite.Colormap.CopyLight(lightlist[i].extra_colormap);

			if (di->Level->flags3 & LEVEL3_NOCOLOREDSPRITELIGHTING)
			{
				copySprite.Colormap.Decolorize();
			}

			if (!ThingColor.isWhite())
			{
				copySprite.Colormap.LightColor.r = (copySprite.Colormap.LightColor.r*ThingColor.r) >> 8;
				copySprite.Colormap.LightColor.g = (copySprite.Colormap.LightColor.g*ThingColor.g) >> 8;
				copySprite.Colormap.LightColor.b = (copySprite.Colormap.LightColor.b*ThingColor.b) >> 8;
			}

			z1=copySprite.z2=lightbottom;
			vt=copySprite.vb=copySprite.vt+
				(lightbottom-copySprite.z1)*(copySprite.vb-copySprite.vt)/(z2-copySprite.z1);
			copySprite.PutSprite(di, translucent);
			put=true;
		}
	}
}

//==========================================================================
//
//
//
//==========================================================================

void HWSprite::PerformSpriteClipAdjustment(AActor *thing, const DVector2 &thingpos, float spriteheight)
{
	const float NO_VAL = 100000000.0f;
	bool clipthing = (thing->player || thing->flags3&MF3_ISMONSTER || thing->IsKindOf(NAME_Inventory)) && (thing->flags&MF_ICECORPSE || !(thing->flags&MF_CORPSE));
	bool smarterclip = !clipthing && gl_spriteclip == 3;
	if ((clipthing || gl_spriteclip > 1) && !(thing->flags2 & MF2_FLOATBOB))
	{

		float btm = NO_VAL;
		float top = -NO_VAL;
		extsector_t::xfloor &x = thing->Sector->e->XFloor;

		if (x.ffloors.Size())
		{
			for (unsigned int i = 0; i < x.ffloors.Size(); i++)
			{
				F3DFloor * ff = x.ffloors[i];
				if (ff->flags & FF_THISINSIDE) continue;	// only relevant for software rendering.
				float floorh = ff->top.plane->ZatPoint(thingpos);
				float ceilingh = ff->bottom.plane->ZatPoint(thingpos);
				if (floorh == thing->floorz)
				{
					btm = floorh;
				}
				if (ceilingh == thing->ceilingz)
				{
					top = ceilingh;
				}
				if (btm != NO_VAL && top != -NO_VAL)
				{
					break;
				}
			}
		}
		else if (thing->Sector->GetHeightSec())
		{
			if (thing->flags2&MF2_ONMOBJ && thing->floorz ==
				thing->Sector->heightsec->floorplane.ZatPoint(thingpos))
			{
				btm = thing->floorz;
				top = thing->ceilingz;
			}
		}
		if (btm == NO_VAL)
			btm = thing->Sector->floorplane.ZatPoint(thing) - thing->Floorclip;
		if (top == NO_VAL)
			top = thing->Sector->ceilingplane.ZatPoint(thingpos);

		// +/-1 to account for the one pixel empty frame around the sprite.
		float diffb = (z2+1) - btm;
		float difft = (z1-1) - top;
		if (diffb >= 0 /*|| !gl_sprite_clip_to_floor*/) diffb = 0;
		// Adjust sprites clipping into ceiling and adjust clipping adjustment for tall graphics
		if (smarterclip)
		{
			// Reduce slightly clipping adjustment of corpses
			if (thing->flags & MF_CORPSE || spriteheight > fabs(diffb))
			{
				float ratio = clamp<float>((fabs(diffb) * (float)gl_sclipfactor / (spriteheight + 1)), 0.5, 1.0);
				diffb *= ratio;
			}
			if (!diffb)
			{
				if (difft <= 0) difft = 0;
				if (difft >= (float)gl_sclipthreshold)
				{
					// dumb copy of the above.
					if (!(thing->flags3&MF3_ISMONSTER) || (thing->flags&MF_NOGRAVITY) || (thing->flags&MF_CORPSE) || difft > (float)gl_sclipthreshold)
					{
						difft = 0;
					}
				}
				if (spriteheight > fabs(difft))
				{
					float ratio = clamp<float>((fabs(difft) * (float)gl_sclipfactor / (spriteheight + 1)), 0.5, 1.0);
					difft *= ratio;
				}
				z2 -= difft;
				z1 -= difft;
			}
		}
		if (diffb <= (0 - (float)gl_sclipthreshold))	// such a large displacement can't be correct!
		{
			// for living monsters standing on the floor allow a little more.
			if (!(thing->flags3&MF3_ISMONSTER) || (thing->flags&MF_NOGRAVITY) || (thing->flags&MF_CORPSE) || diffb < (-1.8*(float)gl_sclipthreshold))
			{
				diffb = 0;
			}
		}
		z2 -= diffb;
		z1 -= diffb;
	}
}

//==========================================================================
//
//
//
//==========================================================================

CVAR(Float, gl_sprite_distance_cull, 2000.0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, gl_sprite_decor_distance_cull, 1200.0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

bool IsDistanceCulled(AActor* thing)
{
	double culldist = gl_sprite_distance_cull * gl_sprite_distance_cull;
	if (culldist <= 0.0)
		return false;

	double dist = (thing->Pos() - r_viewpoint.Pos).LengthSquared();

	if (dist > culldist)
		return true;
	return false;
}

void HWSprite::Process(HWDrawInfo *di, AActor* thing, sector_t * sector, area_t in_area, int thruportal, bool isSpriteShadow)
{
	sector_t rs;
	sector_t * rendersector;

	isBillboard = false;	// [BB] never inherit a previous use's billboard state

	if (thing == nullptr)
		return;

	if (IsDistanceCulled(thing)) 
		return;

	// [ZZ] allow CustomSprite-style direct picnum specification
	bool isPicnumOverride = thing->picnum.isValid();

	// Don't waste time projecting sprites that are definitely not visible.
	if ((thing->sprite == 0 && !isPicnumOverride) || !thing->IsVisibleToPlayer() || ((thing->renderflags & RF_MASKROTATION) && !thing->IsInsideVisibleAngles()))
	{
		return;
	}

#if 0
	if (thing->IsKindOf(NAME_Corona))
	{
		di->Coronas.Push(static_cast<ACorona*>(thing));
		return;
	}
#endif

	const auto &vp = di->Viewpoint;
	AActor *camera = vp.camera;

	// RS FORK -- AActor::VisibleCVar: drawn only while the named cvar is above
	// zero. Read here, every frame, so it answers behind a paused menu.
	// GetCVar(consoleplayer, ...) and not FindCVar: a user cvar's value does not
	// live in the raw object FindCVar returns (see GetPlacementCVar, models.cpp).
	if (thing->VisibleCVar != NAME_None)
	{
		FBaseCVar *gate = GetCVar(consoleplayer, thing->VisibleCVar.GetChars());
		if (gate == nullptr || gate->GetGenericRep(CVAR_Float).Float <= 0.f) return;
	}

	// RS FORK -- AActor::AlphaCVar: the fade a setting says, read here every
	// frame so a slider answers behind a paused menu. Ignored when the cvar does
	// not exist, so a typo cannot make a thing invisible.
	double alpha = thing->InterpolatedAlpha(vp.TicFrac);
	if (thing->AlphaCVar != NAME_None)
	{
		FBaseCVar *fade = GetCVar(consoleplayer, thing->AlphaCVar.GetChars());
		if (fade != nullptr) alpha = fade->GetGenericRep(CVAR_Float).Float;
	}

	// RS FORK -- AActor::PulseHz: breathing, on the WALL clock. A menu freezes
	// the playsim, and a mark that says "this is the one you are editing" has to
	// keep moving exactly then, so the phase comes from I_msTime and not from any
	// game tic. A sine between full alpha and PulseDepth of it -- a fade, never a
	// step, because a hard flash is a photosensitivity problem.
	if (thing->PulseHz > 0.0)
	{
		const double depth = clamp(thing->PulseDepth, 0.0, 1.0);
		const double phase = (double)I_msTime() * 0.001 * thing->PulseHz * 2.0 * M_PI;
		alpha *= depth + (1.0 - depth) * (0.5 + 0.5 * sin(phase));
	}
	if (thing->renderflags & RF_INVISIBLE || !thing->RenderStyle.IsVisible(alpha))
	{
		if (!(thing->flags & MF_STEALTH) || !di->isStealthVision() || thing == camera)
			return;
	}

	// check renderrequired vs ~r_rendercaps, if anything matches we don't support that feature,
	// check renderhidden vs r_rendercaps, if anything matches we do support that feature and should hide it.
	if ((!r_debug_disable_vis_filter && !!(thing->RenderRequired & ~r_renderercaps)) ||
		(!!(thing->RenderHidden & r_renderercaps)))
		return;

	int spritenum = thing->sprite;
	DVector2 sprscale = thing->InterpolatedScale(vp.TicFrac);
	if (thing->player != nullptr)
	{
		P_CheckPlayerSprite(thing, spritenum, sprscale);
	}

	// [RH] Interpolate the sprite's position to make it look smooth
	DVector3 thingpos = thing->InterpolatedPosition(vp.TicFrac);
	if (thruportal == 1) thingpos += di->Level->Displacements.getOffset(thing->Sector->PortalGroup, sector->PortalGroup);

	AActor *viewmaster = thing;
	if ((thing->flags8 & MF8_MASTERNOSEE) && thing->master != nullptr)
	{
		viewmaster = thing->master;
	}

	// [Nash] filter visibility in mirrors
	bool isInMirror = di->mCurrentPortal && (di->mCurrentPortal->mState->MirrorFlag > 0 || di->mCurrentPortal->mState->PlaneMirrorFlag > 0);
	if (thing->renderflags2 & RF2_INVISIBLEINMIRRORS && isInMirror)
	{
		return;
	}
	else if (thing->renderflags2 & RF2_ONLYVISIBLEINMIRRORS && !isInMirror)
	{
		return;
	}
	// Some added checks if the camera actor is not supposed to be seen. It can happen that some portal setup has this actor in view in which case it may not be skipped here
	if (viewmaster == camera && !vp.showviewer)
	{
		if (vp.bForceNoViewer || (viewmaster->player && viewmaster->player->crossingPortal)) return;
		DVector3 vieworigin = viewmaster->Pos();

		//If we get here, then we want to override the location of the camera actor
		auto vrmode = VRMode::GetVRMode(true);
		if (vrmode->GetTeleportLocation(thingpos))
		{
			vieworigin = thingpos;

			//Scale Doom Guy up a bit
			sprscale *= 1.2;
		}

		if (thruportal == 1) vieworigin += di->Level->Displacements.getOffset(viewmaster->Sector->PortalGroup, sector->PortalGroup);
		if (fabs(vieworigin.X - vp.ActorPos.X) < 2 && fabs(vieworigin.Y - vp.ActorPos.Y) < 2) return;

		// Necessary in order to prevent sprite pop-ins with viewpos and models.
		auto* sec = viewmaster->Sector;
		if (sec && !sec->PortalBlocksMovement(sector_t::ceiling))
		{
			double zh = sec->GetPortalPlaneZ(sector_t::ceiling);
			double top = (viewmaster->player ? max<double>(viewmaster->player->viewz, viewmaster->Top()) + 1 : viewmaster->Top());
			if (viewmaster->Z() < zh && top >= zh)
				return;
		}
	}
	// Thing is invisible if close to the camera.
	if (viewmaster->renderflags & RF_MAYBEINVISIBLE)
	{
		DVector3 viewpos = viewmaster->InterpolatedPosition(vp.TicFrac);
		if (thruportal == 1) viewpos += di->Level->Displacements.getOffset(viewmaster->Sector->PortalGroup, sector->PortalGroup);
		if (fabs(viewpos.X - vp.CenterEyePos.X) < 32 && fabs(viewpos.Y - vp.CenterEyePos.Y) < 32) return;
	}

	modelframe = isPicnumOverride ? nullptr : FindModelFrame(thing, spritenum, thing->frame, !!(thing->flags & MF_DROPPED));

	// [BB] VOXEL DISTANCE CULL.
	//
	// A decoration voxel pack tags every column, torch, barrel and pickup in
	// the game, so a room that used to cost a few hundred billboards costs a
	// few hundred model draws instead. Past r_voxeldistance the ordinary
	// sprite is drawn instead -- which is exactly what the player saw before
	// the pack was loaded, and is indistinguishable at range.
	//
	// Nulling modelframe IS the fallback: everything downstream already treats
	// "no model frame" as "draw the billboard", so this needs no second path.
	//
	// VOXELS ONLY. A model is authored deliberately for one actor and is
	// usually rare; a voxel arrives in bulk from a pack, which is what makes
	// this worth having and what makes culling it safe.
	//
	// NEVER culls a held object. VoxelOverride means something is deliberately
	// holding this as a solid thing -- it is by definition within arm's reach,
	// so the test could only ever fire as a bug, and skipping it costs one
	// compare.
	//
	// Squared throughout, so this adds no sqrt to a per-sprite path.
	//
	// RS FORK, 2026-09-13 -- three changes:
	//
	// THINGPOS, not a fresh InterpolatedPosition. thingpos carries the
	// line-portal displacement (above); the old measurement put a thing seen
	// through a line portal at the wrong distance.
	//
	// HYSTERESIS, r_voxeldistance_band. Voxel -> sprite past the limit; sprite
	// -> voxel only back inside (limit - band), so a thing on the line does not
	// flicker. The last side is AActor::VoxelFarLatch. Only the MAIN pass writes
	// it; mirrors, other portals and the sprite-shadow pass only read it, so a
	// second view of the same thing cannot flip it. The band is held to half
	// the limit, so a large band can never strand a thing as a sprite.
	//
	// NEVER INVISIBLE. A voxel whose frame has no sprite texture is kept
	// (KeepVoxelWithoutSprite, models.cpp): nulling it would draw nothing.
	const bool voxelLatchPass = di->mCurrentPortal == nullptr && !isSpriteShadow;
	if (modelframe != nullptr && modelframe->isVoxel && !thing->VoxelOverride && r_voxeldistance > 0)
	{
		const double lim = (double)r_voxeldistance;
		const double band = min<double>(max<double>((double)r_voxeldistance_band, 0.), lim * 0.5);
		const double nearLim = lim - band;
		const double dx = thingpos.X - vp.CenterEyePos.X;
		const double dy = thingpos.Y - vp.CenterEyePos.Y;
		const double dz = thingpos.Z - vp.CenterEyePos.Z;
		const double d2 = dx * dx + dy * dy + dz * dz;
		// isFar, not "far": minwindef.h defines far (and near) as empty macros.
		const bool isFar = thing->VoxelFarLatch ? (d2 >= nearLim * nearLim) : (d2 > lim * lim);
		if (voxelLatchPass) thing->VoxelFarLatch = isFar ? 1 : 0;
		if (isFar && !KeepVoxelWithoutSprite(spritenum, thing->frame)) modelframe = nullptr;
	}
	else if (voxelLatchPass && thing->VoxelFarLatch)
	{
		// Held, not a voxel this frame, or the cull is off: forget the old side,
		// so the next time the cull applies it starts from "voxel" and the plain
		// limit rather than from a stale "far".
		thing->VoxelFarLatch = 0;
	}

	modelframeflags = modelframe ? modelframe->getFlags(thing->modelData) : 0;

	if (modelframe != nullptr &&
		!modelframe->isVoxel &&
		gametic - primaryLevel->starttime > 2 &&
		gl_texture_thread &&
		gl_texture_thread_models &&
		(spritenum != thing->lastModelSprite || thing->frame != thing->lastModelFrame) &&
		screen->SupportsBackgroundCache())
	{
		bool success = true;

		for (int i = modelframe->skinIDs.Size() - 1; i >= 0; i--)
		{
			auto tex = TexMan.GetGameTexture(modelframe->skinIDs[i], false);
			if (tex == nullptr || !tex->isValid())
			{
				continue;
			}

			int scaleflags = 0;
			if (shouldUpscale(tex, UF_Sprite)) scaleflags |= CTF_Upscale;

			FMaterial* gltex = FMaterial::ValidateTexture(tex, scaleflags, false);
			MaterialLayerInfo* layer = nullptr;
			IHardwareTexture* hwtex = gltex != nullptr ? gltex->GetLayer(0, thing->Translation.index(), &layer) : nullptr;
			if (gltex == nullptr || hwtex == nullptr || !hwtex->IsValid())
			{
				if (gltex != nullptr)
				{
					screen->BackgroundCacheMaterial(gltex, thing->Translation, false);
				}
				else
				{
					screen->BackgroundCacheTextureMaterial(tex, thing->Translation, scaleflags, false);
				}
				success = false;
			}
		}

		for (int i = modelframe->surfaceskinIDs.Size() - 1; i >= 0; i--)
		{
			auto tex = TexMan.GetGameTexture(modelframe->surfaceskinIDs[i], false);
			if (tex == nullptr || !tex->isValid())
			{
				continue;
			}

			int scaleflags = 0;
			if (shouldUpscale(tex, UF_Sprite)) scaleflags |= CTF_Upscale;

			FMaterial* gltex = FMaterial::ValidateTexture(tex, scaleflags, false);
			MaterialLayerInfo* layer = nullptr;
			IHardwareTexture* hwtex = gltex != nullptr ? gltex->GetLayer(0, thing->Translation.index(), &layer) : nullptr;
			if (gltex == nullptr || hwtex == nullptr || !hwtex->IsValid())
			{
				if (gltex != nullptr)
				{
					screen->BackgroundCacheMaterial(gltex, thing->Translation, false);
				}
				else
				{
					screen->BackgroundCacheTextureMaterial(tex, thing->Translation, scaleflags, false);
				}
				success = false;
			}
		}

		for (int i = 0; i < modelframe->modelsAmount; i++)
		{
			int id = modelframe->modelIDs[i];
			if (id >= 0)
			{
				auto* model = Models[id];
				if (model != nullptr && model->GetVertexBuffer(GLModelRendererType) == nullptr)
				{
					if (screen->BackgroundLoadModel(model))
					{
						success = false;
					}
				}
			}
		}

		if (!success)
		{
			if (thing->lastModelSprite > -1)
			{
				modelframe = FindModelFrame(thing, thing->lastModelSprite, thing->lastModelFrame, !!(thing->flags & MF_DROPPED));
				modelframeflags = modelframe ? modelframe->getFlags(thing->modelData) : 0;
				if (modelframe == nullptr) return;
			}
			else
			{
				return;
			}
		}
		else
		{
			thing->lastModelSprite = spritenum;
			thing->lastModelFrame = thing->frame;
		}
	}

	// Too close to the camera. This doesn't look good if it is a sprite.
	if (thing != camera && fabs(thingpos.X - vp.CenterEyePos.X) < 2 && fabs(thingpos.Y - vp.CenterEyePos.Y) < 2
		&& vp.CenterEyePos.Z >= thingpos.Z - 2 && vp.CenterEyePos.Z <= thingpos.Z + thing->Height + 2
		&& !thing->Vel.isZero() && !modelframe) // exclude vertically moving objects from this check.
	{
		return;
	}

	// don't draw first frame of a player missile
	if (thing->flags&MF_MISSILE)
	{
		if (!(thing->flags7 & MF7_FLYCHEAT) && thing->target == vp.ViewActor && vp.ViewActor != nullptr)
		{
			double speed = thing->Vel.Length();
			if (speed >= thing->target->radius / 2)
			{
				double clipdist = clamp(thing->Speed, thing->target->radius, thing->target->radius * 2);
				if ((thingpos - vp.CenterEyePos).LengthSquared() < clipdist * clipdist) return;
			}
		}
		thing->flags7 |= MF7_FLYCHEAT;	// do this only once for the very first frame, but not if it gets into range again.
	}

	if (thruportal != 2 && di->mClipPortal != nullptr)
	{
		int clipres = di->mClipPortal->ClipPoint(thingpos.XY());
		if (clipres == PClip_InFront) return;
	}
	// disabled because almost none of the actual game code is even remotely prepared for this. If desired, use the INTERPOLATE flag.
	if (thing->renderflags & RF_INTERPOLATEANGLES)
		Angles = thing->InterpolatedAngles(vp.TicFrac);
	else
		Angles = thing->Angles;

	if (sector->sectornum != thing->Sector->sectornum && !thruportal)
	{
		// This cannot create a copy in the fake sector cache because it'd interfere with the main thread, so provide a local buffer for the copy.
		// Adding synchronization for this one case would cost more than it might save if the result here could be cached.
		rendersector = hw_FakeFlat(thing->Sector, in_area, false, &rs);
	}
	else
	{
		rendersector = sector;
	}
	topclip = rendersector->PortalBlocksMovement(sector_t::ceiling) ? LARGE_VALUE : rendersector->GetPortalPlaneZ(sector_t::ceiling);
	bottomclip = rendersector->PortalBlocksMovement(sector_t::floor) ? -LARGE_VALUE : rendersector->GetPortalPlaneZ(sector_t::floor);

	uint32_t spritetype = (thing->renderflags & RF_SPRITETYPEMASK);
	x = thingpos.X + thing->WorldOffset.X;
	z = thingpos.Z + thing->WorldOffset.Z;
	y = thingpos.Y + thing->WorldOffset.Y;
	if (spritetype == RF_FACESPRITE) z -= thing->Floorclip; // wall and flat sprites are to be considered di->Level-> geometry so this may not apply.

	// snap shadow Z to the floor
	if (isSpriteShadow)
	{
		z = thing->floorz;
	}
	// [RH] Make floatbobbing a renderer-only effect.
	else
	{
		float fz = thing->GetBobOffset(Net_ModifyObjectFrac(thing, vp.TicFrac));
		z += fz;
	}

	// don't bother drawing sprite shadows if this is a model (it will never look right)
	if (modelframe && isSpriteShadow)
	{
		return;
	}
	if (!modelframe)
	{
		bool mirror = false;
		bool backfaced = false;
		// [VR] CenterEyePos is the camera position without the per-eye view shift, so the
		// rotation-frame / backface decision is made once for both eyes instead of per eye.
		DAngle ang = (thingpos - vp.CenterEyePos).Angle();
		if (di->Viewpoint.bDoOrtho) ang = vp.Angles.Yaw;
		else if (actor && thing->renderflags2 & RF2_ANGLEDROLL)
		{
			Matrix3x4 rolltilt;
			rolltilt.MakeIdentity();
			DAngle tempang = thing->Angles.Yaw + thing->AngledRollOffset;
			rolltilt.Rotate(tempang.Cos(), tempang.Sin(), 0.0, -thing->Angles.Roll.Degrees());
			tempang = DVector3(rolltilt * FVector3(thingpos - vp.CenterEyePos)).Angle();
			backfaced = fabs((ang - tempang).Degrees()) > 90.0;
			if (backfaced) // (ang - tempang).Cos() < 0.0)
			{
				ang = DAngle::fromDeg(180.0) - ang;
			}
		}
		FTextureID patch;
		// [ZZ] add direct picnum override
		if (isPicnumOverride)
		{
			// Animate picnum overrides.
			auto tex = TexMan.GetGameTexture(thing->picnum, true);
			if (tex == nullptr) return;

			if (tex->GetRotations() != 0xFFFF)
			{
				// choose a different rotation based on player view
				spriteframe_t* sprframe = &SpriteFrames[tex->GetRotations()];
				DAngle sprang = thing->GetSpriteAngle(ang, vp.TicFrac);
				angle_t rot;
				if (sprframe->Texture[0] == sprframe->Texture[1])
				{
					if (thing->flags7 & MF7_SPRITEANGLE)
						rot = (thing->SpriteAngle + DAngle::fromDeg(45.0 / 2 * 9)).BAMs() >> 28;
					else
						rot = (sprang - (thing->Angles.Yaw + thing->SpriteRotation) + DAngle::fromDeg(45.0 / 2 * 9)).BAMs() >> 28;
				}
				else
				{
					if (thing->flags7 & MF7_SPRITEANGLE)
						rot = (thing->SpriteAngle + DAngle::fromDeg(45.0 / 2 * 9 - 180.0 / 16)).BAMs() >> 28;
					else
						rot = (sprang - (thing->Angles.Yaw + thing->SpriteRotation) + DAngle::fromDeg(45.0 / 2 * 9 - 180.0 / 16)).BAMs() >> 28;
				}
				auto picnum = sprframe->Texture[rot];
				if (sprframe->Flip & (1 << rot))
				{
					mirror = true;
				}
			}

			patch =  tex->GetID();
		}
		else
		{
			DAngle sprangle;
			int rot;
			if (!(thing->renderflags & RF_FLATSPRITE) || thing->flags7 & MF7_SPRITEANGLE)
			{
				sprangle = thing->GetSpriteAngle(ang, vp.TicFrac);
				rot = -1;
			}
			else
			{
				// Flat sprites cannot rotate in a predictable manner.
				sprangle = nullAngle;
				rot = 0;
			}
			// this fix should not be needed anymore
			// keep player sprite on mirror with same orientation
			// if (thing == camera && screen->stencilValue > 0)
			// {
			// 	rot = 0;
			// }
			patch = sprites[spritenum].GetSpriteFrame(thing->frame, rot, sprangle, &mirror, !!(thing->renderflags & RF_SPRITEFLIP));
		}

		if (!patch.isValid()) return;
		int type = thing->renderflags & RF_SPRITETYPEMASK;
		auto tex = TexMan.GetGameTexture(patch, false);
		if (!tex || !tex->isValid()) return;
		int scaleflags = tex->ShouldExpandSprite() ? CTF_Expand : 0;
		if (shouldUpscale(tex, UF_Sprite)) scaleflags |= CTF_Upscale;

		FTextureID lastPatch = thing->LastPatch;
		if (gametic - primaryLevel->starttime > 2 &&
			(patch != lastPatch || scaleflags != thing->lastScaleFlags) &&
			gl_texture_thread &&
			screen->SupportsBackgroundCache())
		{
			FMaterial* gltex = FMaterial::ValidateTexture(tex, scaleflags, false);
			MaterialLayerInfo* layer = nullptr;
			IHardwareTexture* hwtex = gltex != nullptr ? gltex->GetLayer(0, thing->Translation.index(), &layer) : nullptr;
			if (gltex == nullptr || hwtex == nullptr || !hwtex->IsValid())
			{
				if (gltex)
				{
					screen->BackgroundCacheMaterial(gltex, thing->Translation, true);
				}
				else
				{
					screen->BackgroundCacheTextureMaterial(tex, thing->Translation, scaleflags, true);
				}

				if (lastPatch.isValid() && scaleflags == thing->lastScaleFlags)
				{
					patch = lastPatch;
					tex = TexMan.GetGameTexture(patch, false);
					if (!tex || !tex->isValid()) return;
				}
				else
				{
					return;
				}
			}
		}

		thing->LastPatch = patch;
		thing->lastScaleFlags = scaleflags;
		auto& spi = tex->GetSpritePositioning(type == RF_FACESPRITE);

		offx = (float)thing->GetSpriteOffset(false);
		offy = (float)thing->GetSpriteOffset(true);

		vt = spi.GetSpriteVT();
		vb = spi.GetSpriteVB();
		if (thing->renderflags & RF_YFLIP) std::swap(vt, vb);

		auto r = spi.GetSpriteRect();

		// [SP] SpriteFlip
		if (thing->renderflags & RF_SPRITEFLIP)
			thing->renderflags ^= RF_XFLIP;

		if (mirror ^ !!(thing->renderflags & RF_XFLIP))
		{
			r.left = -r.width - r.left;	// mirror the sprite's x-offset
			ul = spi.GetSpriteUL();
			ur = spi.GetSpriteUR();
		}
		else
		{
			ul = spi.GetSpriteUR();
			ur = spi.GetSpriteUL();
		}

		texture = tex;
		if (!texture || !texture->isValid())
			return;

		// Canvas textures are stored upside down
		if (!isPicnumOverride && texture && texture->isHardwareCanvas()) std::swap(vt, vb);

		if (thing->renderflags & RF_SPRITEFLIP) // [SP] Flip back
			thing->renderflags ^= RF_XFLIP;

		// If sprite is isometric, do both vertical scaling and partial rotation to face the camera to compensate for Y-billboarding.
		// Using just rotation (about z=0) might cause tall+slender (high aspect ratio) sprites to clip out of collision box
		// at the top and clip into whatever is behind them from the viewpoint's perspective. - [DVR]
		thing->isoscaleY = 1.0;
		thing->isotheta = vp.HWAngles.Pitch.Degrees();
		if (thing->renderflags2 & RF2_ISOMETRICSPRITES)
		{
			float floordist = thing->radius * vp.floordistfact;
			floordist -= 0.5 * r.width * vp.cotfloor;
			float sineisotheta = floordist / r.height;
			double scl = g_sqrt( 1.0 + sineisotheta * sineisotheta - 2.0 * vp.PitchSin * sineisotheta );
			if ((thing->radius > 0.0) && (scl > fabs(vp.PitchCos)))
			{
				thing->isoscaleY = scl / ( fabs(vp.PitchCos) > 0.01 ? fabs(vp.PitchCos) : 0.01 );
				thing->isotheta = 180.0 * asin( sineisotheta / thing->isoscaleY ) / M_PI;
			}
		}

		r.Scale(sprscale.X, isSpriteShadow ? sprscale.Y * 0.15 * thing->isoscaleY : sprscale.Y * thing->isoscaleY);

		if (((thing->renderflags & RF_ROLLSPRITE) || (thing->renderflags2 & RF2_SQUAREPIXELS)) && !(thing->renderflags2 & RF2_STRETCHPIXELS))
		{
			double ps = di->Level->pixelstretch;
			double mult = 1.0 / sqrt(ps); // shrink slightly
			r.Scale(mult * ps, mult);
		}

		float rightfac = -r.left;
		float leftfac = rightfac - r.width;
		z1 = z - r.top;
		z2 = z1 - r.height;

		float spriteheight = sprscale.Y * r.height * thing->isoscaleY;

		// Tests show that this doesn't look good for many decorations and corpses
		if (spriteheight > 0 && gl_spriteclip > 0 && (thing->renderflags & RF_SPRITETYPEMASK) == RF_FACESPRITE)
		{
			PerformSpriteClipAdjustment(thing, thingpos.XY(), spriteheight);
		}

		switch (spritetype)
		{
		case RF_FACESPRITE:
		{
			float viewvecX = vp.ViewVector.X;
			float viewvecY = vp.ViewVector.Y;

			x1 = x - viewvecY*leftfac;
			x2 = x - viewvecY*rightfac;
			y1 = y + viewvecX*leftfac;
			y2 = y + viewvecX*rightfac;
			if (thing->renderflags2 & RF2_ISOMETRICSPRITES) // If sprites are drawn from an isometric perspective
			{
				x1 -= viewvecX * thing->radius * MY_SQRT2;
				x2 -= viewvecX * thing->radius * MY_SQRT2;
				y1 -= viewvecY * thing->radius * MY_SQRT2;
				y2 -= viewvecY * thing->radius * MY_SQRT2;
			}
			break;
		}
		case RF_FLATSPRITE:
		{
			float bottomfac = -r.top;
			float topfac = bottomfac - r.height;

			x1 = x + leftfac;
			x2 = x + rightfac;
			y1 = y - topfac;
			y2 = y - bottomfac;
			// [MC] Counteract in case of any potential problems. Tests so far haven't
			// shown any outstanding issues but that doesn't mean they won't appear later
			// when more features are added.
			z1 += offy;
			z2 += offy;
			break;
		}
		case RF_WALLSPRITE:
		{
			float viewvecX = Angles.Yaw.Cos();
			float viewvecY = Angles.Yaw.Sin();

			x1 = x + viewvecY*leftfac;
			x2 = x + viewvecY*rightfac;
			y1 = y - viewvecX*leftfac;
			y2 = y - viewvecX*rightfac;
			break;
		}
		}
	}
	else
	{
		x1 = x2 = x;
		y1 = y2 = y;
		z1 = z2 = z;
		texture = nullptr;
	}

	depth = (float)((x - vp.CenterEyePos.X) * vp.TanCos + (y - vp.CenterEyePos.Y) * vp.TanSin);
	if(thing->renderflags2 & RF2_ISOMETRICSPRITES) depth = depth * vp.PitchCos - vp.PitchSin * z2; // Helps with stacking actors with small xy offsets
	if(actor && (thing->renderflags2 & RF2_ANGLEDROLL) && !(thing->renderflags & RF_ROLLCENTER))
	{
		double rollsin = thing->Angles.Roll.Sin();
		DAngle tempang2 = thing->Angles.Yaw + thing->AngledRollOffset + DAngle::fromDeg(rollsin < 0.0
																						? 90.0 : -90.0);
		double r2 = 0.5 * fabs((z2 - z1) * rollsin);
		depth = (float)((x + r2 * tempang2.Cos() - vp.Pos.X) * vp.TanCos
						+ (y + r2 * tempang2.Sin() - vp.Pos.Y) * vp.TanSin);
	}
	if (isSpriteShadow) depth += EQUAL_EPSILON; // always sort shadows behind the sprite.

	if (gl_spriteclip == -1 && (thing->renderflags & RF_SPRITETYPEMASK) == RF_FACESPRITE) // perform anamorphosis
	{
		float minbias = r_spriteclipanamorphicminbias;
		minbias = clamp(minbias, 0.3f, 1.0f);

		float btm = thing->Sector->floorplane.ZatPoint(thing) - thing->Floorclip;
		float top = thing->Sector->ceilingplane.ZatPoint(thingpos);

		float vbtm = thing->Sector->floorplane.ZatPoint(vp.Pos);
		float vtop = thing->Sector->ceilingplane.ZatPoint(vp.Pos);

		float vpx = vp.Pos.X;
		float vpy = vp.Pos.Y;
		float vpz = vp.Pos.Z;

		float tpx = thingpos.X;
		float tpy = thingpos.Y;
		float tpz = thingpos.Z;

		if (!(r_debug_nolimitanamorphoses))
		{
			// this should help prevent clipping through walls ...
			float objradiusbias = 1.f - thing->radius / sqrt((vpx - tpx) * (vpx - tpx) + (vpy - tpy) * (vpy - tpy));
			minbias = max(minbias, objradiusbias);
		}

		float bintersect, tintersect;
		if (z2 < vpz && vbtm < vpz)
			bintersect = min((btm - vpz) / (z2 - vpz), (vbtm - vpz) / (z2 - vpz));
		else
			bintersect = 1.0;

		if (z1 > vpz && vtop > vpz)
			tintersect = min((top - vpz) / (z1 - vpz), (vtop - vpz) / (z1 - vpz));
		else
			tintersect = 1.0;

		if (thing->waterlevel >= 1 && thing->waterlevel <= 2)
			bintersect = tintersect = 1.0f;

		float spbias = clamp(min(bintersect, tintersect), minbias, 1.0f);
		float vpbias = 1.0 - spbias;
		x1 = x1 * spbias + vpx * vpbias;
		y1 = y1 * spbias + vpy * vpbias;
		z1 = z1 * spbias + vpz * vpbias;
		x2 = x2 * spbias + vpx * vpbias;
		y2 = y2 * spbias + vpy * vpbias;
		z2 = z2 * spbias + vpz * vpbias;
	}

	// light calculation

	bool enhancedvision = false;

	// allow disabling of the fullbright flag by a brightmap definition
	// (e.g. to do the gun flashes of Doom's zombies correctly.
	fullbright = (thing->flags5 & MF5_BRIGHT) ||
		((thing->renderflags & RF_FULLBRIGHT) && (!texture || !texture->isFullbrightDisabled()));

	if (fullbright)	lightlevel = 255;
	else lightlevel = RescaleLightLevel(thing->GetLightLevel(rendersector));

	foglevel = (uint8_t)clamp<short>(rendersector->lightlevel, 0, 255); // this *must* use the sector's light level or the fog will just look bad.

	lightlevel = rendersector->CheckSpriteGlow(lightlevel, thingpos);

	ThingColor = (thing->RenderStyle.Flags & STYLEF_ColorIsFixed) ? thing->fillcolor : 0xffffff;
	// RS FORK -- AActor::TintColor: tint whatever is drawn, texture kept.
	if (thing->TintColor & 0xffffff)
		ThingColor = ThingColor.Modulate(PalEntry(thing->TintColor));
	ThingColor.a = 255;
	RenderStyle = thing->RenderStyle;

	// colormap stuff is a little more complicated here...
	if (di->isFullbrightScene())
	{
		enhancedvision = di->isStealthVision();

		Colormap.Clear();

		if (di->isNightvision())
		{
			if ((thing->IsKindOf(NAME_Inventory) || thing->flags3&MF3_ISMONSTER || thing->flags&MF_MISSILE || thing->flags&MF_CORPSE))
			{
				RenderStyle.Flags |= STYLEF_InvertSource;
			}
		}
	}
	else
	{
		Colormap = rendersector->Colormap;
		if (fullbright)
		{
			if (rendersector == &di->Level->sectors[rendersector->sectornum] || in_area != area_below)
				// under water areas keep their color for fullbright objects
			{
				// Only make the light white but keep everything else (fog, desaturation and Boom colormap.)
				Colormap.MakeWhite();
			}
			else
			{
				// Keep the color, but brighten things a bit so that a difference can be seen.
				Colormap.LightColor.r = (3 * Colormap.LightColor.r + 0xff) / 4;
				Colormap.LightColor.g = (3 * Colormap.LightColor.g + 0xff) / 4;
				Colormap.LightColor.b = (3 * Colormap.LightColor.b + 0xff) / 4;
			}
		}
		else if (di->Level->flags3 & LEVEL3_NOCOLOREDSPRITELIGHTING)
		{
			Colormap.Decolorize();
		}
	}

	translation = thing->Translation;

	OverrideShader = -1;
	trans = alpha;
	hw_styleflags = STYLEHW_Normal;

	if (RenderStyle.BlendOp >= STYLEOP_Fuzz && RenderStyle.BlendOp <= STYLEOP_FuzzOrRevSub)
	{
		RenderStyle.CheckFuzz();
		if (RenderStyle.BlendOp == STYLEOP_Fuzz)
		{
			if (gl_fuzztype != 0 && !(RenderStyle.Flags & STYLEF_InvertSource))
			{
				RenderStyle = LegacyRenderStyles[STYLE_Translucent];
				OverrideShader = SHADER_NoTexture + gl_fuzztype;
				trans = 0.99f;	// trans may not be 1 here
				hw_styleflags = STYLEHW_NoAlphaTest;
			}
			else
			{
				// Without shaders only the standard effect is available.
				RenderStyle.BlendOp = STYLEOP_Shadow;
			}
		}
	}

	if (RenderStyle.Flags & STYLEF_TransSoulsAlpha)
	{
		trans = transsouls;
	}
	else if (RenderStyle.Flags & STYLEF_Alpha1)
	{
		trans = 1.f;
	}
	if (r_UseVanillaTransparency && (thing->renderflags & RF_ZDOOMTRANS))
	{
		// [SP] "canonical transparency" - with the flip of a CVar, disable transparency for Doom objects,
		//   and disable 'additive' translucency for certain objects from other games.
		if (r_UseVanillaTransparency == 1 || AutoTrans.CheckKey(thing->GetClass()->TypeName) != nullptr)
		{
			trans = 1.f;
			RenderStyle.BlendOp = STYLEOP_Add;
			RenderStyle.SrcAlpha = STYLEALPHA_One;
			RenderStyle.DestAlpha = STYLEALPHA_Zero;
		}
	}
	if (trans >= 1.f - FLT_EPSILON && RenderStyle.BlendOp != STYLEOP_Shadow && (
		(RenderStyle.SrcAlpha == STYLEALPHA_One && RenderStyle.DestAlpha == STYLEALPHA_Zero) ||
		(RenderStyle.SrcAlpha == STYLEALPHA_Src && RenderStyle.DestAlpha == STYLEALPHA_InvSrc)
		))
	{
		// This is a non-translucent sprite (i.e. STYLE_Normal or equivalent)
		trans = 1.f;

		if (!gl_sprite_blend || modelframe ||
			(thing->renderflags & (RF_FLATSPRITE | RF_WALLSPRITE)) ||
			(hw_force_cambbpref ? gl_billboard_faces_camera :
			(gl_billboard_faces_camera && !(thing->renderflags2 & RF2_BILLBOARDNOFACECAMERA)) ||
			thing->renderflags2 & RF2_BILLBOARDFACECAMERA))
		{
			RenderStyle.SrcAlpha = STYLEALPHA_One;
			RenderStyle.DestAlpha = STYLEALPHA_Zero;
			hw_styleflags = STYLEHW_Solid;
		}
		else
		{
			RenderStyle.SrcAlpha = STYLEALPHA_Src;
			RenderStyle.DestAlpha = STYLEALPHA_InvSrc;
		}
	}
	if ((texture && texture->GetTranslucency()) || (RenderStyle.Flags & STYLEF_RedIsAlpha) || (modelframe && thing->RenderStyle != DefaultRenderStyle()))
	{
		if (hw_styleflags == STYLEHW_Solid)
		{
			RenderStyle.SrcAlpha = STYLEALPHA_Src;
			RenderStyle.DestAlpha = STYLEALPHA_InvSrc;
		}
		hw_styleflags = STYLEHW_NoAlphaTest;
	}

	if (enhancedvision && gl_enhanced_nightvision)
	{
		if (RenderStyle.BlendOp == STYLEOP_Shadow)
		{
			// enhanced vision makes them more visible!
			trans = 0.5f;
			FRenderStyle rs = RenderStyle;
			RenderStyle = STYLE_Translucent;
			RenderStyle.Flags = rs.Flags;	// Flags must be preserved, at this point it can only be STYLEF_InvertSource
		}
		else if (thing->flags & MF_STEALTH)
		{
			// enhanced vision overcomes stealth!
			if (trans < 0.5f) trans = 0.5f;
		}
	}

	// for sprite shadow, use a translucent stencil renderstyle
	if (isSpriteShadow)
	{
		RenderStyle = STYLE_Stencil;
		ThingColor = MAKEARGB(255, 0, 0, 0);
		// fade shadow progressively as the thing moves higher away from the floor
		if (r_actorspriteshadowfadeheight > 0.0) {
			trans *= clamp(0.0f, float(r_actorspriteshadowalpha - (thingpos.Z - thing->floorz) * (1.0 / r_actorspriteshadowfadeheight)), float(r_actorspriteshadowalpha));
		} else {
			trans *= r_actorspriteshadowalpha;
		}
		hw_styleflags = STYLEHW_NoAlphaTest;
	}

	if (trans == 0.0f) return;

	// end of light calculation

	actor = thing;
	index = thing->SpawnOrder;

	// sprite shadows should have a fixed index of -1 (ensuring they're drawn behind particles which have index 0)
	// sorting should be irrelevant since they're always translucent
	if (isSpriteShadow)
	{
		index = -1;
	}

	particle = nullptr;

	const bool drawWithXYBillboard = (!(actor->renderflags & RF_FORCEYBILLBOARD)
		&& (actor->renderflags & RF_SPRITETYPEMASK) == RF_FACESPRITE
		&& (gl_billboard_mode == 1 || actor->renderflags & RF_FORCEXYBILLBOARD));


	// no light splitting when:
	// 1. no lightlist
	// 2. any fixed colormap
	// 3. any bright object
	// 4. any with render style shadow (which doesn't use the sector light)
	// 5. anything with render style reverse subtract (light effect is not what would be desired here)
	if (thing->Sector->e->XFloor.lightlist.Size() != 0 && !di->isFullbrightScene() && !fullbright &&
		RenderStyle.BlendOp != STYLEOP_Shadow && RenderStyle.BlendOp != STYLEOP_RevSub)
	{
		if (screen->hwcaps & RFL_NO_CLIP_PLANES)	// on old hardware we are rather limited...
		{
			lightlist = nullptr;
			if (!drawWithXYBillboard && !modelframe)
			{
				SplitSprite(di, thing->Sector, hw_styleflags != STYLEHW_Solid);
			}
		}
		else
		{
			lightlist = &thing->Sector->e->XFloor.lightlist;
		}
	}
	else
	{
		lightlist = nullptr;
	}

	PutSprite(di, hw_styleflags != STYLEHW_Solid);
	rendered_sprites++;
}


//==========================================================================
//
//
//
//==========================================================================

void HWSprite::ProcessParticle(HWDrawInfo *di, particle_t *particle, sector_t *sector, DVisualThinker *spr)//, int shade, int fakeside)
{
	if (!particle || particle->alpha <= 0)
		return;

	if (spr && !spr->ValidTexture())
		return;

	isBillboard = false;	// [BB] never inherit a previous use's billboard state

	lightlevel = RescaleLightLevel(spr ? spr->GetLightLevel(sector) : sector->GetSpriteLight());
	foglevel = (uint8_t)clamp<short>(sector->lightlevel, 0, 255);

	trans = particle->alpha;
	OverrideShader = (particle->flags & SPF_ALLOWSHADERS) ? -1 : 0;
	modelframe = nullptr;
	texture = nullptr;
	topclip = LARGE_VALUE;
	bottomclip = -LARGE_VALUE;
	index = 0;
	actor = nullptr;
	this->particle = particle;
	fullbright = particle->flags & SPF_FULLBRIGHT;

	if (di->isFullbrightScene())
	{
		Colormap.Clear();
	}
	else if (!(particle->flags & SPF_FULLBRIGHT))
	{
		TArray<lightlist_t> & lightlist=sector->e->XFloor.lightlist;
		double lightbottom;

		Colormap = sector->Colormap;
		for(unsigned int i=0;i<lightlist.Size();i++)
		{
			if (i<lightlist.Size()-1) lightbottom = lightlist[i+1].plane.ZatPoint(particle->Pos);
			else lightbottom = sector->floorplane.ZatPoint(particle->Pos);

			if (lightbottom < particle->Pos.Z)
			{
				lightlevel = RescaleLightLevel(*lightlist[i].p_lightlevel);
				Colormap.CopyLight(lightlist[i].extra_colormap);
				break;
			}
		}
		if (di->Level->flags3 & LEVEL3_NOCOLOREDSPRITELIGHTING)
		{
			Colormap.Decolorize();	// ZDoom never applies colored light to particles.
		}
	}
	else
	{
		lightlevel = 255;
		Colormap = sector->Colormap;
		Colormap.ClearColor();
	}

	if(particle->style != STYLE_None)
	{
		RenderStyle = particle->style;
	}
	else
	{
		RenderStyle = STYLE_Translucent;
	}

	ThingColor = particle->color;
	ThingColor.a = 255;
	const auto& vp = di->Viewpoint;

	double timefrac = Net_ModifyParticleFrac(particle, vp.TicFrac);
	if (paused || (di->Level->isFrozen() && !(particle->flags & SPF_NOTIMEFREEZE)))
		timefrac = 0.;


	if (spr && !(spr->flags & VTF_IsParticle))
	{
		AdjustVisualThinker(di, spr, sector);
	}
	else
	{
		bool has_texture = false;
		bool custom_animated_texture = false;
		int particle_style = 0;
		float size = particle->size;
		if (!spr)
		{
			has_texture = particle->texture.isValid();
			custom_animated_texture = (particle->flags & SPF_LOCAL_ANIM) && particle->animData.ok;
			particle_style = has_texture ? 2 : gl_particles_style; // Treat custom texture the same as smooth particles
		}
		else
		{
			size = float(spr->Scale.X);
			const int ptype = spr->GetParticleType();
			particle_style = (ptype != PT_DEFAULT) ? ptype : gl_particles_style;
		}
		// [BB] Load the texture for round or smooth particles
		if (particle_style)
		{
			FTextureID lump;
			if (particle_style == 1)
			{
				lump = TexMan.glPart2;
			}
			else if (particle_style == 2)
			{
				if(custom_animated_texture)
				{
					lump = TexAnim.UpdateStandaloneAnimation(particle->animData, di->Level->LocalWorldTimer + timefrac);
				}
				else if(has_texture)
				{
					lump = particle->texture;
				}
				else
				{
					lump = TexMan.glPart;
				}
			}
			else
			{
				lump.SetNull();
			}

			if (lump.isValid())
			{
				translation = NO_TRANSLATION;

				ul = vt = 0;
				ur = vb = 1;

				texture = TexMan.GetGameTexture(lump, !custom_animated_texture);
			}
		}


		float xvf = (particle->Vel.X) * timefrac;
		float yvf = (particle->Vel.Y) * timefrac;
		float zvf = (particle->Vel.Z) * timefrac;

		offx = 0.f;
		offy = 0.f;

		x = float(particle->Pos.X) + xvf;
		y = float(particle->Pos.Y) + yvf;
		z = float(particle->Pos.Z) + zvf;

		if(particle->flags & SPF_ROLL)
		{
			float rvf = (particle->RollVel) * timefrac;
			Angles.Roll = TAngle<double>::fromDeg(particle->Roll + rvf);
		}

		float factor;
		if (particle_style == 1) factor = 1.3f / 7.f;
		else if (particle_style == 2) factor = 2.5f / 7.f;
		else factor = 1 / 7.f;
		float scalefac= size * factor;

		float ps = di->Level->pixelstretch;

		scalefac /= sqrt(ps); // shrink it slightly to account for the stretch

		float viewvecX = vp.ViewVector.X * scalefac * ps;
		float viewvecY = vp.ViewVector.Y * scalefac;

		x1=x+viewvecY;
		x2=x-viewvecY;
		y1=y-viewvecX;
		y2=y+viewvecX;
		z1=z-scalefac;
		z2=z+scalefac;

		depth = (float)((x - vp.Pos.X) * vp.TanCos + (y - vp.Pos.Y) * vp.TanSin);

		// [BB] Translucent particles have to be rendered without the alpha test.
		if (particle_style != 2 && trans>=1.0f-FLT_EPSILON) hw_styleflags = STYLEHW_Solid;
		else hw_styleflags = STYLEHW_NoAlphaTest;
	}

	if (sector->e->XFloor.lightlist.Size() != 0 && !di->isFullbrightScene() && !fullbright)
		lightlist = &sector->e->XFloor.lightlist;
	else
		lightlist = nullptr;

	PutSprite(di, hw_styleflags != STYLEHW_Solid);
	rendered_sprites++;
}

//==========================================================================
//
//==========================================================================
//
// [BB] Billboard payloads
//
// What a billboard draws on itself. Only BB_TEXTURE used to draw; the rest
// were declared and left waiting on shaders that were never written, so a
// panel asking for one got an invisible quad and no error.
//
// They do not need shaders. A payload is allowed to emit more than one quad,
// so a bar is a track and a fill, and a number is a row of glyphs -- ordinary
// textured quads, sorted by the sprite lists like everything else. The two
// genuinely shape-shaped payloads come from three small generated graphics
// (bbwhite, bbpanel, bbring) tinted by the billboard's colour.
//
// The cost of a texture instead of a shader, stated plainly: a rounded plate
// stretched to a non-square billboard has its corner radius stretched with it,
// and a ring's thickness is fixed by the artwork rather than settable. Both are
// worth it to have all five payloads working today rather than none.
//
//==========================================================================

static FGameTexture* GetBillboardShape(const char* name)
{
	FTextureID id = TexMan.CheckForTexture(name, ETextureType::Any);
	if (!id.isValid()) return nullptr;
	return TexMan.GetGameTexture(id, true);
}

// The glyph row shared by BB_DIGITS and BB_GLYPH. Laid out across the
// billboard's width at its own aspect, centred, so a number fills the panel it
// was given rather than sitting at some arbitrary pixel size.
static void EmitBillboardGlyphs(const char* text, double halfw, double halfh, PalEntry tint,
	const std::function<void(double, double, double, double, FGameTexture*, PalEntry, const FBillboardUV&)>& emit)
{
	if (text == nullptr || *text == 0) return;
	FFont* font = SmallFont;
	if (font == nullptr) return;

	const int fontHeight = max(1, font->GetHeight());
	int totalPixels = 0;
	for (const uint8_t* c = (const uint8_t*)text; *c != 0; ++c)
	{
		totalPixels += font->GetCharWidth((int)*c);
	}
	if (totalPixels <= 0) return;

	// Fit to whichever axis runs out first, so a long number shrinks to fit
	// rather than running off the ends of its own panel.
	const double byWidth = (halfw * 2.0) / (double)totalPixels;
	const double byHeight = (halfh * 2.0) / (double)fontHeight;
	const double unitsPerPixel = min(byWidth, byHeight);

	double pen = -(totalPixels * unitsPerPixel) * 0.5;
	for (const uint8_t* c = (const uint8_t*)text; *c != 0; ++c)
	{
		int charWidth = 0;
		FGameTexture* glyph = font->GetChar((int)*c, CR_UNTRANSLATED, &charWidth);
		const double advance = charWidth * unitsPerPixel;
		if (glyph != nullptr && advance > 0.0)
		{
			const double gw = glyph->GetDisplayWidth() * unitsPerPixel;
			const double gh = glyph->GetDisplayHeight() * unitsPerPixel;
			emit((pen + advance * 0.5) / halfw, 0.0, gw * 0.5, gh * 0.5, glyph, tint, FBillboardUV());
		}
		pen += advance;
	}
}

//==========================================================================
//
// [BB] The sixteen-segment alphabet, as bit masks.
//
// This lives in C++ rather than in the shader on purpose. The shader needs to
// know WHICH BARS are lit, not which character it is, and answering "which
// bars does R use" with a 36-entry branch chain running once per pixel would
// be absurd when the answer never changes. The mask is computed once per
// glyph here and handed over as two bytes.
//
// Bit order matches the frame drawn in func_segment.fp:
//   0 a1  1 a2   2 b   3 c   4 d2  5 d1  6 e   7 f
//   8 g1  9 g2  10 h  11 i  12 j  13 k  14 l  15 m
//
// Zero is reserved: it means PLATE to the shader, so a character with no lit
// bars must never be emitted. Space is handled by advancing the pen without
// drawing, which is also one fewer quad.
//
//==========================================================================

static uint16_t SegmentMask(int ch)
{
	if (ch >= 'a' && ch <= 'z') ch -= 32;		// the display has one case

	switch (ch)
	{
	case '0': return 0x00FF;  case '1': return 0x000C;  case '2': return 0x0377;
	case '3': return 0x023F;  case '4': return 0x038C;  case '5': return 0x03BB;
	case '6': return 0x03FB;  case '7': return 0x000F;  case '8': return 0x03FF;
	case '9': return 0x03BF;

	// B is NOT the textbook 16-segment B (0x4A3F). That form has no left
	// verticals and leans on the centre uprights for its spine, which renders
	// as a 3 with a bar through it -- fine in the abstract, unreadable when
	// every boss in RS_Main is named B0001. This keeps the left stem and drops
	// only the LEFT half of the middle bar, so it reads as a B and still
	// cannot be confused with 8, which has both halves.
	case 'A': return 0x03CF;  case 'B': return 0x02FF;  case 'C': return 0x00F3;
	case 'D': return 0x483F;  case 'E': return 0x03F3;  case 'F': return 0x01C3;
	case 'G': return 0x02FB;  case 'H': return 0x03CC;  case 'I': return 0x4833;
	case 'J': return 0x007C;  case 'K': return 0x91C0;  case 'L': return 0x00F0;
	case 'M': return 0x14CC;  case 'N': return 0x84CC;  case 'O': return 0x00FF;
	case 'P': return 0x03C7;  case 'Q': return 0x80FF;  case 'R': return 0x83C7;
	case 'S': return 0x03BB;  case 'T': return 0x4803;  case 'U': return 0x00FC;
	case 'V': return 0x30C0;  case 'W': return 0xA0CC;  case 'X': return 0xB400;
	case 'Y': return 0x5400;  case 'Z': return 0x3033;

	case '-': return 0x0300;  case '_': return 0x0030;  case '=': return 0x0330;
	case '+': return 0x4B00;  case '*': return 0xFC00;  case '/': return 0x3000;
	case '\\': return 0x8400; case '|': return 0x4800;  case '\'': return 0x0800;
	case '"': return 0x0880;  case '(': return 0x3000;  case ')': return 0x8400;
	case '[': return 0x00F3;  case ']': return 0x003F;  case '?': return 0x0287;
	case '!': return 0x4008;  case '.': return 0x4000;  case ':': return 0x4800;
	}
	return 0;		// unknown -- draws nothing rather than inventing a glyph
}

// [BB] The same row, but read out of a distance-field atlas instead of the
// bitmap font -- see hw_sdffont.h for what that buys. Every glyph is still an
// ordinary quad off one texture, so this keeps the "a payload needs no shader"
// shape the others have; the only difference is that the caller turns
// OverrideShader on around it.
//
// A cell is square and carries the spread margin baked in, so the ink sits
// inset from the cell's own edge. The pen therefore tracks the INK origin and
// each quad is pushed back by that margin -- without it every letter would
// drift one spread further right than the last.
static void EmitBillboardSDFText(FSDFFont* font, const char* text, double halfw, double halfh,
	PalEntry tint,
	const std::function<void(double, double, double, double, FGameTexture*, PalEntry, const FBillboardUV&)>& emit)
{
	if (font == nullptr || text == nullptr || *text == 0) return;
	FGameTexture* atlas = font->Atlas();
	if (atlas == nullptr) return;

	const double cell = font->Cell();
	if (cell <= 0.0) return;

	// Metrics come from the shared header so that what is drawn here and what
	// MeasureBillboardText() promises script cannot drift apart.
	const FSDFTextMetrics metrics = SDFMeasureText(font, text);
	if (metrics.widest <= 0.0) return;

	// Fit to whichever axis runs out first, exactly as the bitmap row does, so
	// FIT TO THE EM BOX, NOT THE CELL. A cell is the em box plus a spread of
	// margin on every side, so fitting the cell to the panel spent a quarter
	// of the height on field that is by definition empty -- the text came out
	// noticeably smaller than the size it was asked for, with dead space above
	// and below it. The generator centres the em box in the cell, so scaling by
	// the em box and still drawing the whole cell leaves the text centred and
	// keeps the margin available for the halo to live in.
	//
	// The horizontal term is the WIDEST LINE and the vertical term is the WHOLE
	// BLOCK. Both matter: measuring width across the whole string would make a
	// two-line label think it was twice as wide as it is and shrink to nothing,
	// and fitting one line's height would let a three-line block overflow its
	// panel by two lines without any indication that it had.
	const double spread = font->Spread();
	const double emBox = max(cell - 2.0 * spread, 1.0);
	const double scale = min((halfw * 2.0) / metrics.widest,
	                         (halfh * 2.0) / (emBox * metrics.blockEm));

	const double halfH = cell * scale * 0.5;		// full cell tall: the halo needs its margin
	const double margin = spread * scale;
	const double lineStep = emBox * SDFTEXT_LINE_PITCH * scale;

	// The block is centred on the panel, so the first line's centre sits half a
	// block up and then half a line back down. Single-line text puts blockEm at
	// exactly 1.0, which cancels to the 0.0 this used to pass -- the one-line
	// case is unchanged, not merely similar.
	double penY = (emBox * metrics.blockEm * scale) * 0.5 - (emBox * scale) * 0.5;

	int drawn = 0, missing = 0;
	FBillboardUV firstUV;

	const uint8_t* c = (const uint8_t*)text;
	for (;;)
	{
		// Each line is centred on its own width rather than on the block's, so
		// a short line among long ones reads as centred instead of left-ragged.
		double pen = -(SDFMeasureLine(font, c) * scale) * 0.5;

		for (; *c != 0 && *c != '\n'; ++c)
		{
			if (*c == '\r') continue;

			const FSDFGlyph* g = font->Glyph((int)*c);
			if (g == nullptr) { missing++; continue; }

			const double advance = g->advance * scale;
			if (advance > 0.0)
			{
				// TRIM THE QUAD TO THE PART OF THE CELL THAT CAN CONTAIN ANYTHING.
				//
				// Cells are square, but an advance is narrower than a cell is wide
				// -- 32 against 64 on this font -- so a full-cell quad overhangs
				// its neighbour by half a cell of pure empty field. Empty field
				// still carries halo, and two overlapping halos ADD, which is what
				// was brightening the gaps between letters.
				//
				// Everything a glyph can draw lives within its advance plus one
				// spread of halo each side, so the quad and its UVs are clipped to
				// exactly that. Nothing real is lost and the overlap halves.
				const double usefulCells = min(g->advance + 2.0 * spread, cell);
				const double halfW = usefulCells * scale * 0.5;
				const double uSpan = (g->u1 - g->u0) * (usefulCells / cell);

				FBillboardUV uv;
				uv.u0 = g->u0; uv.v0 = g->v0;
				uv.u1 = g->u0 + (float)uSpan; uv.v1 = g->v1;
				if (drawn == 0) firstUV = uv;
				emit((pen - margin + halfW) / halfw, penY / halfh, halfW, halfH, atlas, tint, uv);
				drawn++;
			}
			pen += advance;
		}

		if (*c == 0) break;
		++c;					// step over the newline itself
		penY -= lineStep;
	}

	// One shot, first SDF string of the session. The loader reporting a good
	// atlas only proves the FILE was read -- it says nothing about whether the
	// draw path ever ran, which is the thing that is actually hard to see from
	// outside. Quads, UVs and scale here mean a wrong-looking result can be
	// told apart from a result that never happened, without a debugger.
	static bool reported = false;
	if (!reported)
	{
		reported = true;
		Printf("BB_TEXT: first SDF draw -- \"%s\", %d quads, %d missing glyphs, "
			"%d line(s), scale %.4f, first uv (%.4f,%.4f)-(%.4f,%.4f)\n",
			text, drawn, missing, metrics.lines, scale,
			firstUV.u0, firstUV.v0, firstUV.u1, firstUV.v1);
	}
}

//==========================================================================
//
// [BB] HWSprite::EmitBillboardSegments
//
// A plate quad, then one quad per character. Every quad binds the same white
// plate texture and none of them sample it -- func_segment.fp builds the
// shape from arithmetic. The texture is bound only because the material path
// requires something valid.
//
// The character travels in bbGlow's blue and alpha bytes as a sixteen-bit
// lit-segment mask. That is why this is a member: PutSprite copies the sprite
// into the draw list once per quad, so rewriting bbGlow between emits gives
// each quad its own character while everything else stays shared.
//
//==========================================================================

void HWSprite::EmitBillboardSegments(const char* text, double halfw, double halfh, PalEntry tint,
	const std::function<void(double, double, double, double, FGameTexture*, PalEntry, const FBillboardUV&)>& emit,
	bool inverted, double progress)
{
	if (text == nullptr || *text == 0) return;
	FGameTexture* white = GetBillboardShape("bbwhite");
	if (white == nullptr) return;

	int count = 0;
	for (const uint8_t* c = (const uint8_t*)text; *c != 0; ++c) count++;
	if (count <= 0) return;

	const PalEntry savedGlow = bbGlow;
	const uint8_t gr = savedGlow.r, gs = savedGlow.g;

	// Plate first, and that ORDER IS LOAD BEARING when inverted -- billboards
	// do not depth-test against each other, so submission order is draw order,
	// and a subtractive character drawn before its plate would have nothing to
	// subtract from.
	//
	// Mask 0 is the LED bed, mask 1 the LCD face. Both are plate sentinels.
	const PalEntry savedStyleGlow = bbGlow;
	const FRenderStyle savedStyle = RenderStyle;
	// THE PLATE OPENS. This is GITD's wgType 13 and it is the whole effect --
	// its plate divides the vertical extent by progress
	//
	//     nBox = length(vec2(abs(nAX)/nhW, abs(nAY)/(nProg*nhH)))
	//
	// so at low progress it is a thin horizontal slit and it widens vertically
	// into a full ellipse, like an eye opening. Drawing the finished ellipse
	// and skipping the reveal throws away the reason anyone liked it.
	//
	// Floored at 0.05, as the original does with max(wgMask.y, 0.05), so a
	// progress of zero is a hairline rather than nothing at all.
	const double open = clamp(progress, 0.05, 1.0);
	bbGlow = PalEntry(0, gr, gs, (uint8_t)(inverted ? 1 : 0));
	emit(0.0, 0.0, halfw, halfh * open, white, tint, FBillboardUV());

	// AND THE CHARACTERS WAIT UNTIL IT IS OPEN. The original gates them on
	// nProg > 0.55 -- the plate opens empty, then the number arrives. Showing
	// them from the first frame turns a reveal into a fade-in.
	if (progress < 0.55) return;

	// A character cell is taller than it is wide -- that ratio is most of what
	// makes a row of these read as a display rather than as text. Width is
	// whatever fits, capped so a two-character string does not produce two
	// absurdly fat glyphs.
	// The 0.52 caps how wide a cell may get relative to its height, so a short
	// string does not stretch into a few enormous glyphs floating in a wide
	// plate. Tighter than it first was: at 0.62 the characters sat too far
	// apart to read as one number.
	const double cellW = min((halfw * 2.0) * 0.88 / count, halfh * 2.0 * 0.62);
	const double halfCW = cellW * 0.5;
	const double halfCH = halfh * 0.78;		// inset, so glyphs sit inside the plate

	double pen = -(cellW * count) * 0.5 + halfCW;
	for (const uint8_t* c = (const uint8_t*)text; *c != 0; ++c)
	{
		const uint16_t mask = SegmentMask((int)*c);
		if (mask != 0)
		{
			// Blue is the low byte, alpha the high one. Alpha is free here:
			// nothing downstream reads uAddColor.a, and the draw path resets
			// the whole colour between sprites anyway.
			//
			// Inverted characters carry NO GLOW REACH, and that is not an
			// oversight: a subtractive halo would eat a dark ring out of the
			// plate around every glyph. The plate keeps its glow; the holes
			// punched in it should have hard edges, which is what an LCD
			// looks like.
			bbGlow = PalEntry((uint8_t)(mask >> 8), inverted ? 0 : gr, gs, (uint8_t)(mask & 0xff));
			RenderStyle = inverted ? LegacyRenderStyles[STYLE_Subtract] : savedStyle;
			emit(pen / halfw, 0.0, halfCW, halfCH, white, tint, FBillboardUV());
		}
		pen += cellW;
	}

	RenderStyle = savedStyle;
	bbGlow = savedGlow;
}

void HWSprite::EmitBillboardPayload(HWDrawInfo* di, const FBillboard* bb, double halfw, double halfh,
	const std::function<void(double, double, double, double, FGameTexture*, PalEntry, const FBillboardUV&)>& emit)
{
	const PalEntry tint = bb->color;

	// [BB] Doom's SmallFont is RED, and the bitmap glyph paths were fetching it
	// with CR_UNTRANSLATED and then multiplying the billboard's tint into it.
	// Green text came out dark maroon, and every other colour came out wrong in
	// its own way, because a multiply against red can only ever darken toward
	// red. A green 1337 rendering brown is what made this visible.
	//
	// Translating the glyphs to white first makes the multiply mean what the
	// caller intended: tint x white = tint. The SDF path never had this problem
	// -- its atlas is a greyscale field and the shader emits uObjectColor
	// directly -- which is why only the fallback was wrong.
	auto WhitenGlyphs = [this]() {
		if (SmallFont != nullptr) translation = SmallFont->GetColorTranslation(CR_WHITE);
	};

	switch (bb->payload)
	{
	case BB_TEXTURE:
	{
		FTextureID tid;
		tid.SetIndex(bb->data);
		if (!tid.isValid()) return;
		emit(0.0, 0.0, halfw, halfh, TexMan.GetGameTexture(tid, true), tint, FBillboardUV());
		return;
	}

	case BB_PANEL:
		emit(0.0, 0.0, halfw, halfh, GetBillboardShape("bbpanel"), tint, FBillboardUV());
		return;

	// [BB] The same plate solved as a field. Samples nothing -- bbwhite is
	// handed over only because a valid texture has to be bound -- and the
	// shader owns the whole shape, so the corner, the border and the halo all
	// come out of one distance test.
	case BB_SDFPANEL:
	{
		FGameTexture* white = GetBillboardShape("bbwhite");
		if (white == nullptr) return;
		const int savedPanel = OverrideShader;
		OverrideShader = SHADER_SDFPanel;
		emit(0.0, 0.0, halfw, halfh, white, tint, FBillboardUV());
		OverrideShader = savedPanel;
		return;
	}

	// [BB] A hexagon, same field machinery. Its own case rather than a flag on
	// the panel above because the two are different shapes with different
	// shaders -- see BB_SDFHEX's note for why a comb cannot be sampled.
	case BB_SDFHEX:
	{
		FGameTexture* whiteHex = GetBillboardShape("bbwhite");
		if (whiteHex == nullptr) return;
		const int savedHex = OverrideShader;
		OverrideShader = SHADER_SDFHex;
		emit(0.0, 0.0, halfw, halfh, whiteHex, tint, FBillboardUV());
		OverrideShader = savedHex;
		return;
	}

	// [BB] A star. Same field machinery and the same white stand-in texture;
	// the shader draws pure luminance and never samples it.
	case BB_SDFSTAR:
	{
		FGameTexture* whiteStar = GetBillboardShape("bbwhite");
		if (whiteStar == nullptr) return;
		const int savedStar = OverrideShader;
		OverrideShader = SHADER_SDFStar;
		emit(0.0, 0.0, halfw, halfh, whiteStar, tint, FBillboardUV());
		OverrideShader = savedStar;
		return;
	}

	case BB_RING:
		emit(0.0, 0.0, halfw, halfh, GetBillboardShape("bbring"), tint, FBillboardUV());
		return;

	case BB_BAR:
	{
		// data is fill percent, 0..100. The track is the same white plate at a
		// quarter alpha's worth of darkening rather than a second graphic, so a
		// bar is always exactly two quads whatever it is showing.
		FGameTexture* white = GetBillboardShape("bbwhite");
		if (white == nullptr) return;

		PalEntry track = tint;
		track.r = (uint8_t)(track.r / 4);
		track.g = (uint8_t)(track.g / 4);
		track.b = (uint8_t)(track.b / 4);
		emit(0.0, 0.0, halfw, halfh, white, track, FBillboardUV());

		const double fill = clamp(bb->data, 0, 100) / 100.0;
		if (fill <= 0.0) return;

		// Grows from the left edge, so the filled part stays put and only its
		// right end moves -- a bar whose centre slid around would be unreadable
		// at a glance, which is the only way these are ever read.
		const double fillHalf = halfw * fill;
		emit(-(1.0 - fill), 0.0, fillHalf, halfh, white, tint, FBillboardUV());
		return;
	}

	case BB_DIGITS:
	{
		char buffer[24];
		mysnprintf(buffer, countof(buffer), "%d", bb->data);
		WhitenGlyphs();
		EmitBillboardGlyphs(buffer, halfw, halfh, tint, emit);
		translation = NO_TRANSLATION;
		return;
	}

	case BB_GLYPH:
	{
		const char text[2] = { (char)clamp(bb->data, 1, 255), 0 };
		WhitenGlyphs();
		EmitBillboardGlyphs(text, halfw, halfh, tint, emit);
		translation = NO_TRANSLATION;
		return;
	}

	// [BB] Arbitrary text. Same glyph row as BB_DIGITS, but the string comes
	// from the billboard instead of being printed from an int, so there is no
	// length or alphabet limit -- a name, an ID, a label, whatever script
	// hands it.
	//
	// Distance field when one is available, bitmap when it is not. The fallback
	// is not politeness: bb_sdffont names lumps a mod is free not to ship, and
	// text that silently vanished because an atlas was missing would be a very
	// confusing way to find that out.
	case BB_TEXT:
	{
		// Slot, not the cvar: a card wants a display face on the name and a
		// clean one on the numbers, and the cvar could only ever answer with
		// one of those. Slot 0 IS the cvar, so a billboard that never set a
		// font draws exactly as it did before this existed.
		if (FSDFFont* sdf = FSDFFontRoster::Slot(bb->font))
		{
			// Set around the emit rather than in ProcessBillboard: PutSprite
			// copies the sprite into the draw list per quad, so the shader
			// choice travels with the glyphs and nothing else on this
			// billboard inherits it.
			const int saved = OverrideShader;
			OverrideShader = SHADER_SDFText;
			EmitBillboardSDFText(sdf, bb->text.GetChars(), halfw, halfh, tint, emit);
			OverrideShader = saved;
			return;
		}
		// One shot as well, and the more important of the two: if this fires,
		// everything on screen is the OLD path and any judgement about how the
		// field looks is being made about something else entirely.
		static bool reportedFallback = false;
		if (!reportedFallback)
		{
			reportedFallback = true;
			Printf(TEXTCOLOR_YELLOW "BB_TEXT: drawing through BITMAP glyphs, not the field "
				"(bb_sdffont = \"%s\")\n", *bb_sdffont);
		}
		WhitenGlyphs();
		EmitBillboardGlyphs(bb->text.GetChars(), halfw, halfh, tint, emit);
		translation = NO_TRANSLATION;
		return;
	}

	// [BB] The same string as a segment display. No atlas, no fallback, and no
	// way for it to be unavailable -- the glyphs are arithmetic, so this cannot
	// fail the way a missing font lump can.
	// [BB] A glowing slit. One quad, procedural, and it neither knows nor
	// cares which way it faces -- tilt 90 is a seam in the floor, tilt 0 is a
	// door standing up. Opening is ResizeBillboard, caller-side, so the easing
	// belongs to whoever owns the effect.
	case BB_SEAM:
	{
		FGameTexture* white = GetBillboardShape("bbwhite");
		if (white == nullptr) return;
		const int savedSeam = OverrideShader;
		OverrideShader = SHADER_Seam;
		emit(0.0, 0.0, halfw, halfh, white, tint, FBillboardUV());
		OverrideShader = savedSeam;
		return;
	}

	// [BB] The transcription. One quad, and the shader lays out everything
	// inside it -- no per-digit quads, which is precisely how the original
	// manages to punch its digits out of its own plate.
	case BB_WG13:
	{
		FGameTexture* white = GetBillboardShape("bbwhite");
		if (white == nullptr) return;

		// Progress in red, the number as 24 bits across the rest. The original
		// packed both into its glow spot the same way.
		const int num = clamp(bb->data, 0, 0xFFFFFF);
		const int pr = (int)(clamp(bb->progress, 0.0, 1.0) * 255.0 + 0.5);
		const PalEntry savedWG = bbGlow;
		bbGlow = PalEntry((uint8_t)(num & 0xff), (uint8_t)pr,
			(uint8_t)((num >> 16) & 0xff), (uint8_t)((num >> 8) & 0xff));

		const int savedShader = OverrideShader;
		OverrideShader = SHADER_WG13;
		emit(0.0, 0.0, halfw, halfh, white, tint, FBillboardUV());
		OverrideShader = savedShader;
		bbGlow = savedWG;
		return;
	}

	case BB_SEGMENT:
	case BB_SEGLCD:
	{
		const int saved = OverrideShader;
		OverrideShader = SHADER_Segment;
		EmitBillboardSegments(bb->text.GetChars(), halfw, halfh, tint, emit,
			bb->payload == BB_SEGLCD, bb->progress);
		OverrideShader = saved;
		return;
	}

	default:
		return;
	}
}

// [BB] ProcessBillboard
//
// A billboard becomes a real quad in the translucent draw lists, so it is
// depth-tested and distance-sorted against the rest of the scene exactly
// like a sprite -- it goes through the same HWSprite machinery.
//
// Billboards are UI-grade: fullbright, so a panel stays readable in a dark
// room rather than disappearing into it.
//
// Extent is per-axis, and the quad is built at whatever orientation the
// billboard asks for -- BBF_FIXED honours its stored yaw and tilt verbatim,
// and only the camera modes derive a yaw from the viewpoint.
//
// The four corners are solved HERE and handed straight back by
// CalculateVertices, which opens with an early return for isBillboard that
// skips the entire flat-sprite path. That bypass is load-bearing, not a
// shortcut: hinged BBF_FIXED panels hold a fixed angle to each other only
// because nothing downstream re-orients them. Routing billboards through the
// normal sprite orientation code would collapse every hinged assembly into
// parallel planes.
//
//==========================================================================

void HWSprite::ProcessBillboard(HWDrawInfo *di, const FBillboard *bb, const DVector3 &bpos, sector_t *sector, double groupScale)
{
	if (!sector || !bb) return;

	lightlevel = 255;
	foglevel = (uint8_t)clamp<short>(sector->lightlevel, 0, 255);
	Colormap = sector->Colormap;
	Colormap.ClearColor();
	fullbright = true;

	trans = (float)clamp(bb->alpha, 0.0, 1.0);
	if (trans <= 0.f) return;	// fully faded out: nothing to submit
	OverrideShader = 0;
	modelframe = nullptr;
	texture = nullptr;
	topclip = LARGE_VALUE;
	bottomclip = -LARGE_VALUE;
	index = 0;
	actor = nullptr;
	particle = nullptr;
	lightlist = nullptr;
	translation = NO_TRANSLATION;
	RenderStyle = STYLE_Translucent;
	hw_styleflags = STYLEHW_NoAlphaTest;
	dynlightindex = -1;
	polyoffset = false;
	offx = 0.f;
	offy = 0.f;
	Angles = DRotator();

	ThingColor = bb->color;
	ThingColor.a = 255;

	vt = 0;
	vb = 1;
	ul = bb_flipu ? 1 : 0;
	ur = bb_flipu ? 0 : 1;

	const auto &vp = di->Viewpoint;
	x = (float)bpos.X;
	y = (float)bpos.Y;
	z = (float)bpos.Z;

	// Orientation and extent, from the one shared solver. Facing is a mode,
	// not the definition of the primitive: BBF_FIXED honours the stored
	// yaw/tilt verbatim, which is what lets two panels hold a fixed angle to
	// each other, and only the camera modes derive a yaw from the viewpoint.
	//
	// RIGHT IS (-sy, cy), NOT (sy, -cy). Corrected 2026-08-08, and the
	// derivation is kept here because this is where the symptom shows.
	//
	// The old expression is the viewer's LEFT, and the ordinary sprite path
	// proves it. A sprite's horizontal extent runs leftfac -> rightfac along
	// (-V.y, V.x) (:1285-1308), and the UNMIRRORED branch maps v[0] -> u = UR
	// with UL=0, UR=1 (:1247-1248, gametexture.cpp:340). So the texture's LEFT
	// edge lands on the rightfac corner -- meaning (-V.y, V.x) points to the
	// viewer's left.
	//
	// A billboard yaws to face the eye, so its view direction is V = -F =
	// (-cy, -sy), giving (-V.y, V.x) = (sy, -cy) -- identical geometry to the
	// sprite path, and therefore also the viewer's LEFT.
	//
	// Consequence of getting this backwards: billboards inherited the sprite
	// path's MIRROR branch. Every billboard texture rendered horizontally
	// flipped, and BB_DIGITS -- which walks its own pen along `right`
	// (:1884-1897) -- laid multi-digit numbers out in reverse, so 120 read as
	// 021. `bb_flipu` was added as a workaround for the first symptom and
	// could never fix the second, because it flips U *inside* each quad
	// rather than changing the direction the pen travels.
	//
	// With the basis corrected, bb_flipu's default (off) is now the right
	// value and both symptoms go at once.
	//
	// The expression now lives in BillboardBasis (g_levellocals.h) rather than
	// here, because it was three copies and all three were wrong at once. The
	// queries call the same function with the same cvars, so the pointer and
	// the picture cannot drift apart again.
	DVector3 right, up, normal;
	double halfw, halfh;
	BillboardBasis(*bb, bpos, vp.Pos, bb_tiltbias, bb_scale * groupScale, right, up, normal, halfw, halfh,
		// +180: yaw is WHICH WAY THE FACE POINTS, and a view-locked panel is
		// parked AHEAD of the eye along the view vector. Biasing by the view
		// yaw alone aims its face the same way the viewer is looking -- i.e.
		// straight away from them -- so the panel welds to the head and shows
		// its BACK, which reads as every glyph mirrored.
		(bb->flags & BBFL_VIEWLOCKED) ? vp.Angles.Yaw.Degrees() + 180.0 : 0.0);

	DVector3 rw = right * halfw;
	DVector3 uh = up * halfh;

	DVector3 tl = bpos - rw + uh;
	DVector3 tr = bpos + rw + uh;
	DVector3 bl = bpos - rw - uh;
	DVector3 br = bpos + rw - uh;

	isBillboard = true;
	bbNoDepth = (bb->flags & BBFL_NODEPTH) != 0;

	// [BB] Pack the halo into a colour for uAddColor. Radius is clamped at 1
	// because that is the whole spread -- ask for more and the falloff runs
	// off the end of the field and stops dead in a square, so clamping here is
	// kinder than letting a caller discover the artifact.
	{
		// ONLY THE FIELD PAYLOADS. Fixed 2026-08-11.
		//
		// The comment on SetAddColor below says "every other payload leaves
		// this zero". It did not -- this packed unconditionally, so BB_PANEL,
		// BB_TEXTURE, BB_RING and BB_BAR carried a halo into a shader that has
		// no idea what a halo is. Those keep OverrideShader = 0 and land on
		// main.fp's `texel.rgb += uAddColor.rgb`, which is a plain ADD applied
		// BEFORE the object colour multiply.
		//
		// The damage was differential, which is why it read as a colour bug
		// rather than a brightness one: red carries glowRadius and green
		// carries glowStrength, blue is 0 unless BBFL_VOID. On a white plate
		// SetGlow(0.55, 0.7) is a per-channel gain of about (1.55, 1.70, 1.00)
		// -- so every tier hue got pulled toward yellow and clipped in its two
		// brightest channels, and with bloom on by default the frame bloomed
		// out the exact signal the tier colour exists to carry.
		//
		// BB_TEXT and above are the payloads that swap in a field shader
		// (SDFText, Segment, Seam, WG13) and actually read these two bytes.
		// Known remaining gap: BB_TEXT's bitmap FALLBACK, when no atlas is
		// loaded, also runs at OverrideShader = 0 and will still take the add.
		if (bb->payload >= BB_TEXT)
		{
			const int gr = (int)(clamp(bb->glowRadius, 0.0, 1.0) * 255.0 + 0.5);
			const int gs = (int)(clamp(bb->glowStrength, 0.0, 1.0) * 255.0 + 0.5);
			// Blue carries BBFL_VOID to the seam shader. The segment payload
			// overwrites blue per quad with its character mask, which is fine --
			// nothing is both a seam and a character.
			const int vd = (bb->flags & BBFL_VOID) ? 255 : 0;

			// [BB] ALPHA carries BB_SDFPANEL's corner radius and border width,
			// a nibble each. It is the only channel left -- rgb are spoken for
			// above -- and it was free: nothing downstream reads uAddColor.a.
			// Every other payload keeps the 255 it has always had.
			int alpha = 255;
			if (bb->payload == BB_SDFPANEL || bb->payload == BB_SDFHEX
			 || bb->payload == BB_SDFSTAR)
			{
				const int rad = clamp((bb->data >> 0) & 0xFF, 0, 15);
				const int bor = clamp((bb->data >> 8) & 0xFF, 0, 15);
				alpha = (rad << 4) | bor;
			}
			bbGlow = PalEntry((uint8_t)alpha, (uint8_t)gr, (uint8_t)gs, (uint8_t)vd);
		}
		else
		{
			bbGlow = PalEntry(255, 0, 0, 0);
		}
	}
	bbColor2 = bb->color2;

	// [BB] Submit one quad. A payload is free to call this more than once --
	// which is the whole reason the payloads below need no shaders. A bar is a
	// track and a fill; a number is a row of glyphs. Each is an ordinary
	// textured quad in the place the payload wants it, and the sprite lists
	// sort them like anything else.
	//
	// Offsets are in the billboard's own design space: +offRight is toward its
	// right edge, +offUp toward its top, both measured in half-extents so that
	// 1.0 is the edge.
	auto emitQuad = [&](double offRight, double offUp, double halfWidth, double halfHeight,
		FGameTexture* tex, PalEntry tint, const FBillboardUV& uv) -> void
	{
		if (tex == nullptr || !tex->isValid() || halfWidth <= 0.0 || halfHeight <= 0.0) return;

		texture = tex;
		ThingColor = tint;
		ThingColor.a = 255;

		// Per-quad UVs. bb_flipu still swaps the horizontal pair rather than
		// being folded into the caller's rect, so it keeps working as the
		// escape hatch it is for a texture authored mirrored.
		ul = bb_flipu ? uv.u1 : uv.u0;
		ur = bb_flipu ? uv.u0 : uv.u1;
		vt = uv.v0;
		vb = uv.v1;

		const DVector3 centre = bpos + right * (offRight * halfw) + up * (offUp * halfh);
		const DVector3 qr = right * halfWidth;
		const DVector3 qu = up * halfHeight;
		const DVector3 qtl = centre - qr + qu;
		const DVector3 qtr = centre + qr + qu;
		const DVector3 qbl = centre - qr - qu;
		const DVector3 qbr = centre + qr - qu;

		// Vertex components are (worldX, worldZ, worldY); corner order must
		// match the UV assignment in CreateVertices.
		bbVerts[0] = FVector3((float)qtl.X, (float)qtl.Z, (float)qtl.Y);
		bbVerts[1] = FVector3((float)qtr.X, (float)qtr.Z, (float)qtr.Y);
		bbVerts[2] = FVector3((float)qbl.X, (float)qbl.Z, (float)qbl.Y);
		bbVerts[3] = FVector3((float)qbr.X, (float)qbr.Z, (float)qbr.Y);

		// The draw lists still sort and clip against these, so give them the
		// quad's actual bounds rather than leaving them at the bare centre.
		x1 = (float)min(min(qtl.X, qtr.X), min(qbl.X, qbr.X));
		x2 = (float)max(max(qtl.X, qtr.X), max(qbl.X, qbr.X));
		y1 = (float)min(min(qtl.Y, qtr.Y), min(qbl.Y, qbr.Y));
		y2 = (float)max(max(qtl.Y, qtr.Y), max(qbl.Y, qbr.Y));
		z1 = (float)min(min(qtl.Z, qtr.Z), min(qbl.Z, qbr.Z));
		z2 = (float)max(max(qtl.Z, qtr.Z), max(qbl.Z, qbr.Z));

		// Depth from the billboard's own centre, not the sub-quad's, so every
		// piece of one panel sorts as one object and the fill cannot land
		// behind its own track.
		depth = (float)((x - vp.Pos.X) * vp.TanCos + (y - vp.Pos.Y) * vp.TanSin);

		PutSprite(di, true);
		rendered_sprites++;
	};

	EmitBillboardPayload(di, bb, halfw, halfh, emitQuad);
}

// [MC] VisualThinkers are to be rendered akin to actor sprites. The reason this whole system
// is hitching a ride on particle_t is because of the large number of checks with
// HWSprite elsewhere in the draw lists.
void HWSprite::AdjustVisualThinker(HWDrawInfo* di, DVisualThinker* spr, sector_t* sector)
{
	translation = spr->Translation;

	const auto& vp = di->Viewpoint;
	double timefrac = Net_ModifyObjectFrac(spr, vp.TicFrac);

	if (paused || spr->isFrozen())
		timefrac = 0.;

	bool custom_anim = ((spr->PT.flags & SPF_LOCAL_ANIM) && spr->PT.animData.ok);

	texture = TexMan.GetGameTexture(
			custom_anim
			? TexAnim.UpdateStandaloneAnimation(spr->PT.animData, di->Level->LocalWorldTimer + timefrac)
			: spr->PT.texture, !custom_anim);

	if (spr->flags & VTF_DontInterpolate)
		timefrac = 0.;

	FVector3 interp = spr->InterpolatedPosition(timefrac);
	x = interp.X;
	y = interp.Y;
	z = interp.Z;

	offx = (float)spr->GetOffset(false);
	offy = (float)spr->GetOffset(true);

	if (spr->PT.flags & SPF_ROLL)
		Angles.Roll = TAngle<double>::fromDeg(spr->InterpolatedRoll(timefrac));

	auto& spi = texture->GetSpritePositioning(0);

	vt = spi.GetSpriteVT();
	vb = spi.GetSpriteVB();
	ul = spi.GetSpriteUR();
	ur = spi.GetSpriteUL();

	auto r = spi.GetSpriteRect();
	r.Scale(spr->Scale.X, spr->Scale.Y);

	// Canvas-backed VisualThinkers are typically UI-like quads with no sprite offsets.
	// Center them horizontally so their world position behaves like a normal world tag.
	if (texture && texture->isHardwareCanvas())
	{
		ul = 1.f;
		ur = 0.f;
		vt = 0.f;
		vb = 1.f;
		r.width = texture->GetDisplayWidth() * spr->Scale.X;
		r.height = texture->GetDisplayHeight() * spr->Scale.Y;
		r.left = -r.width * 0.5f;
		r.top = -r.height;
	}

	if ((spr->PT.flags & SPF_ROLL) && !(spr->PT.flags & SPF_STRETCHPIXELS))
	{
		double ps = di->Level->pixelstretch;
		double mult = 1.0 / sqrt(ps); // shrink slightly
		r.Scale(mult * ps, mult);
	}
	if (spr->flags & VTF_FlipX)
	{
		std::swap(ul,ur);
		r.left = -r.width - r.left;	// mirror the sprite's x-offset
	}
	if (spr->flags & VTF_FlipY)	std::swap(vt,vb);

	float viewvecX = vp.ViewVector.X;
	float viewvecY = vp.ViewVector.Y;
	float rightfac = -r.left;
	float leftfac = rightfac - r.width;

	x1 = x - viewvecY * leftfac;
	x2 = x - viewvecY * rightfac;
	y1 = y + viewvecX * leftfac;
	y2 = y + viewvecX * rightfac;
	z1 = z - r.top;
	z2 = z1 - r.height;

	depth = (float)((x - vp.CenterEyePos.X) * vp.TanCos + (y - vp.CenterEyePos.Y) * vp.TanSin);

	// [BB] Translucent particles have to be rendered without the alpha test.
	hw_styleflags = STYLEHW_NoAlphaTest;
}

//==========================================================================
//
//
//
//==========================================================================

void HWDrawInfo::ProcessActorsInPortal(FLinePortalSpan *glport, area_t in_area)
{
	TMap<AActor*, bool> processcheck;
	if (glport->validcount == validcount) return;	// only process once per frame
	glport->validcount = validcount;
	const auto &vp = Viewpoint;
	for (auto port : glport->lines)
	{
		line_t *line = port->mOrigin;
		if (line->isLinePortal())	// only crossable ones
		{
			FLinePortal *port2 = port->mDestination->getPortal();
			// process only if the other side links back to this one.
			if (port2 != nullptr && port->mDestination == port2->mOrigin && port->mOrigin == port2->mDestination)
			{
				for (portnode_t *node = port->lineportal_thinglist; node != nullptr; node = node->m_snext)
				{
					AActor *th = node->m_thing;

					// process each actor only once per portal.
					bool *check = processcheck.CheckKey(th);
					if (check && *check) continue;
					processcheck[th] = true;

					DAngle savedangle = th->Angles.Yaw;
					DVector3 savedpos = th->Pos();
					DVector3 newpos = savedpos;
					sector_t fakesector;

					if (!vp.showviewer)
					{
						AActor *viewmaster = th;
						if ((th->flags8 & MF8_MASTERNOSEE) && th->master != nullptr)
						{
							viewmaster = th->master;
						}

						if (viewmaster == vp.camera)
						{
							DVector3 vieworigin = viewmaster->Pos();

							if (fabs(vieworigin.X - vp.ActorPos.X) < 2 && fabs(vieworigin.Y - vp.ActorPos.Y) < 2)
							{
								// Same as the original position
								continue;
							}

							P_TranslatePortalXY(line, vieworigin.X, vieworigin.Y);
							P_TranslatePortalZ(line, vieworigin.Z);

							if (fabs(vieworigin.X - vp.ActorPos.X) < 2 && fabs(vieworigin.Y - vp.ActorPos.Y) < 2)
							{
								// Same as the translated position
								// (This is required for MASTERNOSEE actors with 3D models)
								continue;
							}
						}
					}

					P_TranslatePortalXY(line, newpos.X, newpos.Y);
					P_TranslatePortalZ(line, newpos.Z);
					P_TranslatePortalAngle(line, th->Angles.Yaw);
					th->SetXYZ(newpos);
					th->Prev += newpos - savedpos;

					HWSprite spr;

					// [Nash] draw sprite shadow
					if (R_ShouldDrawSpriteShadow(th))
					{
						spr.Process(this, th, hw_FakeFlat(th->Sector, in_area, false, &fakesector), in_area, 2, true);
					}

					// This is called from the worker thread and must not alter the fake sector cache.
					spr.Process(this, th, hw_FakeFlat(th->Sector, in_area, false, &fakesector), in_area, 2);
					th->Angles.Yaw = savedangle;
					th->SetXYZ(savedpos);
					th->Prev -= newpos - savedpos;
				}
			}
		}
	}
}
