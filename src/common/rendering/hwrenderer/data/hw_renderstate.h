/*
** hw_renderstate.h
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#pragma once

#include "vectors.h"
#include "matrix.h"
#include "hw_material.h"
#include "texmanip.h"
#include "version.h"
#include "i_interface.h"
#include "v_video.h"
#include "hw_cvars.h"

struct FColormap;
class IVertexBuffer;
class IIndexBuffer;

enum EClearTarget
{
	CT_Depth = 1,
	CT_Stencil = 2,
	CT_Color = 4
};

enum ERenderEffect
{
	EFF_NONE = -1,
	EFF_FOGBOUNDARY,
	EFF_SPHEREMAP,
	EFF_BURN,
	EFF_STENCIL,
	EFF_DITHERTRANS,
	// [GPUPARTICLES] Stateless additive particles (gpuparticles.vp/.fp). Vulkan
	// only: the GL and GLES effect loaders skip this index.
	EFF_GPUPARTICLES,
	// [DRAWNLINES] Glowing lines drawn as boxes (drawnlines.vp/.fp). Vulkan only:
	// the GL and GLES effect loaders skip this index.
	EFF_DRAWNLINES,
	// [MESHPARTICLES] Particles drawn as small instanced meshes -- chunks, shards,
	// casings -- in the opaque pass (meshparticles.vp/.fp, hw_meshparticles.h).
	// Vulkan only: the GL and GLES effect loaders skip this index. APPENDED LAST, so
	// no existing effect keeps anything but the index it had.
	EFF_MESHPARTICLES,
	// [EYEFADE] A world model dissolving near the eye (main.fp MODEL_EYE_FADE), chosen per
	// draw by FHWModelRenderer::SetEyeFade from the model's <prefix>_eyefade_near/_far.
	// Selected on Vulkan only; GL compiles the entry and never selects it. APPENDED LAST,
	// like the three above.
	EFF_EYEFADE,
	MAX_EFFECTS
};

enum EAlphaFunc
{
	Alpha_GEqual = 0,
	Alpha_Greater = 1
};

enum EDrawType
{
	DT_Points = 0,
	DT_Lines = 1,
	DT_Triangles = 2,
	DT_TriangleFan = 3,
	DT_TriangleStrip = 4
};

enum EDepthFunc
{
	DF_Less,
	DF_LEqual,
	DF_Always
};

enum EStencilFlags
{
	SF_AllOn = 0,
	SF_ColorMaskOff = 1,
	SF_DepthMaskOff = 2,
};

enum EStencilOp
{
	SOP_Keep = 0,
	SOP_Increment = 1,
	SOP_Decrement = 2
};

enum ECull
{
	Cull_None,
	Cull_CCW,
	Cull_CW
};



struct FStateVec4
{
	float vec[4];

	void Set(float r, float g, float b, float a)
	{
		vec[0] = r;
		vec[1] = g;
		vec[2] = b;
		vec[3] = a;
	}
};

struct FMaterialState
{
	FMaterial *mMaterial = nullptr;
	int mClampMode;
	int mTranslation;
	int mOverrideShader;
	bool mChanged;

	void Reset()
	{
		mMaterial = nullptr;
		mTranslation = 0;
		mClampMode = CLAMP_NONE;
		mOverrideShader = -1;
		mChanged = false;
	}
};

struct FDepthBiasState
{
	float mFactor;
	float mUnits;
	bool mChanged;

	void Reset()
	{
		mFactor = 0;
		mUnits = 0;
		mChanged = false;
	}
};

enum EPassType
{
	NORMAL_PASS,
	GBUFFER_PASS,
	MAX_PASS_TYPES
};

struct FVector4PalEntry
{
	float r, g, b, a;

	bool operator==(const FVector4PalEntry &other) const
	{
		return r == other.r && g == other.g && b == other.b && a == other.a;
	}

	bool operator!=(const FVector4PalEntry &other) const
	{
		return r != other.r || g != other.g || b != other.b || a != other.a;
	}

	FVector4PalEntry &operator=(PalEntry newvalue)
	{
		const float normScale = 1.0f / 255.0f;
		r = newvalue.r * normScale;
		g = newvalue.g * normScale;
		b = newvalue.b * normScale;
		a = newvalue.a * normScale;
		return *this;
	}

	FVector4PalEntry& SetIA(PalEntry newvalue)
	{
		const float normScale = 1.0f / 255.0f;
		r = newvalue.r * normScale;
		g = newvalue.g * normScale;
		b = newvalue.b * normScale;
		a = newvalue.a;
		return *this;
	}

	FVector4PalEntry& SetFlt(float v1, float v2, float v3, float v4)
	{
		r = v1;
		g = v2;
		b = v3;
		a = v4;
		return *this;
	}

};

struct StreamData
{
	FVector4PalEntry uObjectColor;
	FVector4PalEntry uObjectColor2;
	FVector4 uDynLightColor;
	FVector4PalEntry uAddColor;
	FVector4PalEntry uTextureAddColor;
	FVector4PalEntry uTextureModulateColor;
	FVector4PalEntry uTextureBlendColor;
	FVector4PalEntry uFogColor;
	float uDesaturationFactor;
	float uInterpolationFactor;
	float timer;
	int useVertexData;
	FVector4 uVertexColor;
	FVector4 uVertexNormal;

	FVector4 uGlowTopPlane;
	FVector4 uGlowTopColor;
	FVector4 uGlowBottomPlane;
	FVector4 uGlowBottomColor;

	// [BB] The far end of each wall glow. A glow used to hold one colour and
	// only dim as it faded, so a wall met a floor at a hard edge: two
	// gradients nose to nose, each a different colour at the line they share.
	// With a far colour the glow RAMPS -- the primary colour sits at the
	// plane it grows from, this one is what it fades toward -- and the mod
	// can hand the junction colour to both surfaces so the corner becomes
	// continuous. Alpha 0 means unset and the glow stays flat, exactly as
	// before; same convention side_t's own glow already uses.
	FVector4 uGlowTopFar;
	FVector4 uGlowBottomFar;

	// [BB] Falloff shape and intensity for wall glow, matching flat-edge
	// glow below.
	int uGlowTopFalloff;
	int uGlowBottomFalloff;
	float uGlowTopIntensity;
	float uGlowBottomIntensity;

	// [BB] Sweep: up to eight world-space bands of light that wrap across
	// every surface. Origin xyz + mode is shared; each band carries its own
	// radius/thickness/softness and its own colour + intensity.
	FVector4 uSweepOrigin;
	FVector4 uSweepBands[8];      // radius, thickness, softness, packed word
	                              // w: drawmode (0 default/add, 1 add, 2 lift,
	                              // 3 crush, 4 recolour) + 16*fill (0-4)
	                              // + 256*passed. [round2 SW-21]
	FVector4 uSweepColors[8];     // r, g, b, intensity
	FVector4 uSweepBandOrigin[8]; // per band: xyz origin, w = shape (0 = off)
	int uSweepCount;
	float uSweepTrail;         // wake length, signed; 0 = symmetric band
	int uSweepPad1;
	int uSweepPad2;

	// [BB] Flat-edge glow: rgb + reach (alpha), and up to 64 of the sector's
	// linedef endpoints so the fragment shader can find distance to the
	// nearest edge per pixel.
	FVector4 uFlatGlowColor;
	// [BB] Far end of the flat glow, same deal as the wall pair above. Floor
	// and ceiling time-share this one slot because a draw only ever covers
	// one of them.
	FVector4 uFlatGlowFar;
	int uFlatGlowFalloff;
	int uFlatGlowLineCount;
	// [BB] Which of the two flats this draw is. Floor and ceiling time-share
	// uFlatGlowColor above, so the fragment shader has no way to tell them
	// apart -- and the glow wave gives them different phases, so it has to.
	// This was a pad, so it costs nothing: MAX_STREAM_DATA is unchanged.
	int uFlatGlowIsCeiling;

	// [BB] DARKNESS EXEMPTION, per draw. Not flat-glow business despite the
	// company it keeps -- it sits here because this was uFlatGlowPad2, so it
	// costs nothing and MAX_STREAM_DATA is unchanged. See SetDarknessExempt.
	float uDarknessExempt;
	// [G6] SLOT 0 IS ALSO THE SECTOR'S GLOW SEED ("Engine docs/GLOW_SEAM_SEED_G6_IMPL_NOTES.md").
	//
	// Every glow lane in main.fp -- both wall lanes and both flat lanes -- hashes
	// uFlatGlowLines[0].x + .y for its per-room scatter (GlowSectorSeedSrc). The wall lanes used to
	// hash the glow PLANE'S height instead, which is per plane rather than per sector and moves with a
	// lift, so a wall and the flat it met stepped in brightness across the corner. SetGlowSeedLine
	// below writes this slot on every draw that sets the glow planes, flat glow on or not, so the two
	// sides of a join read the same bytes. A flat WITH glow writes the same value again as its first
	// segment, because it is the same linedef.
	FVector4 uFlatGlowLines[64];

	FVector4 uGradientTopPlane;
	FVector4 uGradientBottomPlane;

	FVector4 uSplitTopPlane;
	FVector4 uSplitBottomPlane;

	FVector4 uDetailParms;
	FVector4 uNpotEmulation;

	FVector4PalEntry uGlobalFadeColor;
	int uGlobalFade;
	int uGlobalFadeMode;
	float uGlobalFadeDensity;
	float uGlobalFadeGradient;
	int uLightRangeLimit;

	// These three stay. std140 aligns a vec4 to 16 bytes and C++ does not, so
	// the outline block below MUST start on a lane boundary or the C++ struct
	// and the shader's view of it silently disagree from here down.
	// [EFFECTLIGHTS] padding2's VALUE is the per-draw effect-light mode (FRenderState::SetEffectLightMode); the slot and its
	// name stay, because the Vulkan prolog declares them.
	int padding1;
	int padding2;
	int padding3;

	// [OUTLINE] SPRITE OUTLINES, per draw call.
	//
	// An actor traced in neon by its own sprite -- see func_spriteoutline.fp for
	// what the numbers mean. Set from the actor in HWSprite::DrawSprite, so this
	// is per-sprite rather than scene-global: one corpse can be lit without every
	// monster in the room lighting up with it.
	//
	// uOutlineParms is (thickness, threshold, glow, mode). Mode 0 is off and is
	// what FRenderState::Reset leaves here, so every other draw in the level pays
	// one float compare in the fragment shader and nothing else.
	FVector4 uOutlineColorA;
	FVector4 uOutlineColorB;
	FVector4 uOutlineParms;

	// [BB] FOG DENSITY SCALE FOR THIS DRAW. 1 is the slab as configured.
	//
	// The fog is one slab over the whole level, which cannot tell a courtyard
	// from a cellar -- and those want opposite things. Doom already marks
	// outdoors: a sky ceiling. So the sector picks a multiplier and the shader
	// scales its own density by it.
	//
	// PER DRAW rather than per fragment, so the boundary is the surface, not a
	// blend. That reads correctly through a window: the far wall outside is an
	// outdoor draw and fogs like one, while the near wall of the room you are
	// standing in does not, so looking out sees weather you are not standing in.
	//
	// A plain scale rather than an indoor/outdoor flag, so the CPU picks which
	// number applies and anything else that wants to thin or thicken the fog for
	// one draw can use the same lane.
	//
	float uFogDensityScale;
	// [EYEFADE] Where a model drawn with EFF_EYEFADE starts to dissolve (far) and is gone
	// (near), map units from the eye. These were uFogPad0/uFogPad1: the same std140 slots,
	// so MAX_STREAM_DATA and every offset are unchanged. The Vulkan prolog names them only
	// inside #ifdef MODEL_EYE_FADE, so no other program's text or SPIR-V changes.
	float uEyeFadeNear;
	float uEyeFadeFar;
	// [SURFACEDAMAGE] This draw's lasting surface damage: the damaged surface's record slot + 1, or 0 for none
	// (hw_surfacedamage.h, "Engine docs/SURFACE_DAMAGE_17_IMPL_NOTES.md"). This was uFogPad2: the same std140 slot, so
	// MAX_STREAM_DATA and every offset are unchanged -- and 0, what the pad always held, means "none", so every draw that
	// never sets it uploads exactly the bytes it did. The Vulkan prolog keeps the slot's name; main.fp reads it as
	// uSurfaceDamageKey only inside #ifdef SURFACE_DAMAGE, which only the Vulkan backend defines.
	int uSurfaceDamageKey;
};

class FRenderState
{
protected:
	uint8_t mFogEnabled;
	uint8_t mTextureEnabled:1;
	uint8_t mGlowEnabled : 1;
	uint8_t mGradientEnabled : 1;
	uint8_t mModelMatrixEnabled : 1;
	uint8_t mTextureMatrixEnabled : 1;
	uint8_t mSplitEnabled : 1;
	uint8_t mBrightmapEnabled : 1;

	int mLightIndex;
	int mBoneIndexBase;
	int mSpecialEffect;
	int mTextureMode;
	int mTextureClamp;
	int mTextureModeFlags;
	int mTextureModeFlagsExtra;
	int mSoftLight;
	float mLightParms[4];

	float mAlphaThreshold;
	float mClipSplit[2];


	int mColorMapSpecial;
	float mColorMapFlash;

	StreamData mStreamData = {};
	PalEntry mFogColor;
	PalEntry mFadeColor;
	PalEntry mSceneColor;
	FStateVec4 mDetailParms;
	FRenderStyle mRenderStyle;

	FMaterialState mMaterial;
	FDepthBiasState mBias;

	IVertexBuffer *mVertexBuffer;
	int mVertexOffsets[2];	// one per binding point
	IIndexBuffer *mIndexBuffer;

	EPassType mPassType = NORMAL_PASS;

public:

	uint64_t firstFrame = 0;
	VSMatrix mModelMatrix;
	VSMatrix mTextureMatrix;

public:

	void Reset()
	{
		mTextureEnabled = true;
		mBrightmapEnabled = mGradientEnabled = mFogEnabled = mGlowEnabled = false;
		mFogColor = 0xffffffff;
		mStreamData.uFogColor = mFogColor;
		mTextureMode = -1;
		mTextureClamp = 0;
		mTextureModeFlags = 0;
		mTextureModeFlagsExtra = 0;
		mStreamData.uDesaturationFactor = 0.0f;
		mAlphaThreshold = 0.5f;
		mModelMatrixEnabled = false;
		mTextureMatrixEnabled = false;
		mSplitEnabled = false;
		mStreamData.uAddColor = 0;
		mStreamData.uObjectColor = 0xffffffff;
		mStreamData.uObjectColor2 = 0;
		mStreamData.uTextureBlendColor = 0;
		mStreamData.uTextureAddColor = 0;
		mStreamData.uTextureModulateColor = 0;
		mSoftLight = 0;
		mLightParms[0] = mLightParms[1] = mLightParms[2] = 0.0f;
		mLightParms[3] = -1.f;
		mSpecialEffect = EFF_NONE;
		mLightIndex = -1;
		mBoneIndexBase = -1;
		mStreamData.uInterpolationFactor = 0;
		mRenderStyle = DefaultRenderStyle();
		mMaterial.Reset();
		mBias.Reset();
		mPassType = NORMAL_PASS;

		mColorMapSpecial = 0;
		mColorMapFlash = 1;

		mVertexBuffer = nullptr;
		mVertexOffsets[0] = mVertexOffsets[1] = 0;
		mIndexBuffer = nullptr;

		mStreamData.uVertexColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		mStreamData.uGlowTopColor = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uGlowBottomColor = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uGlowTopFar = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uGlowBottomFar = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uGlowTopFalloff = 0;
		mStreamData.uGlowBottomFalloff = 0;
		mStreamData.uGlowTopIntensity = 1.0f;
		mStreamData.uGlowBottomIntensity = 1.0f;
		mStreamData.uGlowTopPlane = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uGlowBottomPlane = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uSweepOrigin = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uSweepCount = 0;
		mStreamData.uSweepTrail = 0.0f;
		for (int i = 0; i < 8; i++)
			mStreamData.uSweepBandOrigin[i] = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uFlatGlowColor = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uFlatGlowFar = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uFlatGlowFalloff = 0;
		mStreamData.uFlatGlowIsCeiling = 0;
		mStreamData.uDarknessExempt = 0.f;
		// [OUTLINE] Off. Every draw that is not an outlined sprite lands here,
		// and mode 0 is one float compare in the fragment shader.
		mStreamData.uOutlineColorA = { 0.f, 0.f, 0.f, 0.f };
		mStreamData.uOutlineColorB = { 0.f, 0.f, 0.f, 0.f };
		mStreamData.uOutlineParms = { 0.f, 0.f, 0.f, 0.f };
		mStreamData.uFogDensityScale = 1.f;
		mStreamData.uEyeFadeNear = mStreamData.uEyeFadeFar = 0.f;
		mStreamData.uFlatGlowLineCount = 0;
		mStreamData.uGradientTopPlane = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uGradientBottomPlane = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uSplitTopPlane = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uSplitBottomPlane = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uDynLightColor = { 0.0f, 0.0f, 0.0f, 1.0f };
		mStreamData.uDetailParms = { 0.0f, 0.0f, 0.0f, 0.0f };
#ifdef NPOT_EMULATION
		mStreamData.uNpotEmulation = { 0,0,0,0 };
#endif

		mStreamData.uGlobalFadeColor = 0;
		mStreamData.uGlobalFade = 0;
		mStreamData.uGlobalFadeMode = -1;
		mStreamData.uGlobalFadeDensity = 0.001f;
		mStreamData.uGlobalFadeGradient = 1.5f;
		mStreamData.uLightRangeLimit = 64;

		mModelMatrix.loadIdentity();
		mTextureMatrix.loadIdentity();
		ClearClipSplit();
	}

	void SetNormal(FVector3 norm)
	{
		mStreamData.uVertexNormal = { norm.X, norm.Y, norm.Z, 0.f };
	}

	void SetNormal(float x, float y, float z)
	{
		mStreamData.uVertexNormal = { x, y, z, 0.f };
	}

	void SetColor(float r, float g, float b, float a = 1.f, int desat = 0)
	{
		mStreamData.uVertexColor = { r, g, b, a };
		mStreamData.uDesaturationFactor = desat * (1.0f / 255.0f);
	}

	void SetColor(PalEntry pe, int desat = 0)
	{
		const float scale = 1.0f / 255.0f;
		mStreamData.uVertexColor = { pe.r * scale, pe.g * scale, pe.b * scale, pe.a * scale };
		mStreamData.uDesaturationFactor = desat * (1.0f / 255.0f);
	}

	void SetColorAlpha(PalEntry pe, float alpha = 1.f, int desat = 0)
	{
		const float scale = 1.0f / 255.0f;
		mStreamData.uVertexColor = { pe.r * scale, pe.g * scale, pe.b * scale, alpha };
		mStreamData.uDesaturationFactor = desat * (1.0f / 255.0f);
	}

	void ResetColor()
	{
		mStreamData.uVertexColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		mStreamData.uDesaturationFactor = 0.0f;
	}

	void SetTextureClamp(bool on)
	{
		if (on) mTextureClamp = TM_CLAMPY;
		else mTextureClamp = 0;
	}

	void SetTextureMode(int mode)
	{
		mTextureMode = mode;
	}

	void SetTextureMode(FRenderStyle style)
	{
		if (style.Flags & STYLEF_RedIsAlpha)
		{
			SetTextureMode(TM_ALPHATEXTURE);
		}
		else if (style.Flags & STYLEF_ColorIsFixed)
		{
			SetTextureMode(TM_STENCIL);
		}
		else if (style.Flags & STYLEF_InvertSource)
		{
			SetTextureMode(TM_INVERSE);
		}
	}

	int GetTextureMode()
	{
		return mTextureMode;
	}

	int GetTextureModeAndFlags(int tempTM)
	{
		int f = mTextureModeFlags | mTextureModeFlagsExtra;
		if (!mBrightmapEnabled) f &= ~(TEXF_Brightmap | TEXF_Glowmap);
		if (mTextureClamp) f |= TEXF_ClampY;
		return (mTextureMode == TM_NORMAL && tempTM == TM_OPAQUE ? TM_OPAQUE : mTextureMode) | f;
	}

	void SetTextureModeFlagsExtra(int flags)
	{
		mTextureModeFlagsExtra = flags;
	}

	void EnableTexture(bool on)
	{
		mTextureEnabled = on;
	}

	void EnableFog(uint8_t on)
	{
		mFogEnabled = on;
	}

	void SetEffect(int eff)
	{
		mSpecialEffect = eff;
	}

	// [EYEFADE] Which effect is set, so a caller that sets one for a moment can leave any other alone.
	int GetSpecialEffect() const
	{
		return mSpecialEffect;
	}

	void EnableGlow(bool on)
	{
		if (mGlowEnabled && !on)
		{
			mStreamData.uGlowTopColor = { 0.0f, 0.0f, 0.0f, 0.0f };
			mStreamData.uGlowBottomColor = { 0.0f, 0.0f, 0.0f, 0.0f };
			mStreamData.uGlowTopFar = { 0.0f, 0.0f, 0.0f, 0.0f };
			mStreamData.uGlowBottomFar = { 0.0f, 0.0f, 0.0f, 0.0f };
		}
		mGlowEnabled = on;
	}

	void EnableGradient(bool on)
	{
		mGradientEnabled = on;
	}

	void EnableBrightmap(bool on)
	{
		mBrightmapEnabled = on;
	}

	void EnableSplit(bool on)
	{
		if (mSplitEnabled && !on)
		{
			mStreamData.uSplitTopPlane = { 0.0f, 0.0f, 0.0f, 0.0f };
			mStreamData.uSplitBottomPlane = { 0.0f, 0.0f, 0.0f, 0.0f };
		}
		mSplitEnabled = on;
	}

	void EnableModelMatrix(bool on)
	{
		mModelMatrixEnabled = on;
	}

	void EnableTextureMatrix(bool on)
	{
		mTextureMatrixEnabled = on;
	}

	void SetGlowParams(float *t, float *b)
	{
		mStreamData.uGlowTopColor = { t[0], t[1], t[2], t[3] };
		mStreamData.uGlowBottomColor = { b[0], b[1], b[2], b[3] };
	}

	// [BB] Far end of each wall glow -- see uGlowTopFar. Alpha carries
	// "is set", not a reach: the near colour already owns the reach.
	void SetGlowFar(const FVector4 &t, const FVector4 &b)
	{
		mStreamData.uGlowTopFar = t;
		mStreamData.uGlowBottomFar = b;
	}

	// [BB] Falloff shape and intensity for wall glow. Kept separate from
	// SetGlowParams above rather than widening its signature, so nothing
	// that already calls it needs to change.
	void SetGlowFalloffIntensity(int topFalloff, float topIntensity, int bottomFalloff, float bottomIntensity)
	{
		mStreamData.uGlowTopFalloff = topFalloff;
		mStreamData.uGlowTopIntensity = topIntensity;
		mStreamData.uGlowBottomFalloff = bottomFalloff;
		mStreamData.uGlowBottomIntensity = bottomIntensity;
	}

	// [BB] Sweep: one world-space band applied to every surface. Set once
	// per frame rather than per draw -- it is a property of the world, not
	// of any sector or wall.
	// Sets the SHARED origin, which now means "the default every band starts
	// with". A band that never overrides it behaves exactly as before, which
	// is what keeps every existing caller correct.
	void SetSweepOrigin(int mode, float ox, float oy, float oz, int count, float trail = 0.0f)
	{
		mStreamData.uSweepOrigin = { ox, oy, oz, (float)mode };
		mStreamData.uSweepCount = count;
		mStreamData.uSweepTrail = trail;
		for (int i = 0; i < 8; i++)
			mStreamData.uSweepBandOrigin[i] = { ox, oy, oz, (float)mode };
	}

	// Overrides one band's origin and shape. Call AFTER SetSweepOrigin, which
	// seeds all eight. Shapes 0-9: 0 off, 1 ring, 2/3 bars, 4 sphere, 5 rise,
	// 6-9 signed crossings (SweepShapeDist in main.fp). [round2 SW-21]
	void SetSweepBandOrigin(int i, float ox, float oy, float oz, int mode)
	{
		if (i < 0 || i >= 8) return;
		mStreamData.uSweepBandOrigin[i] = { ox, oy, oz, (float)mode };
	}

	// What this band does to the pixels it covers: 0 default (add), 1 add,
	// 2 lift, 3 crush, 4 recolour. [round2 SW-21] This used to stop at 3.
	// Call after SetSweepBand, which writes the default.
	//
	// [BB] AND WHAT IS DRAWN INSIDE IT, PACKED INTO THE SAME FLOAT.
	//
	// drawmode is 0-4 and always will be -- it names the four things a band
	// can do to a pixel. So the rest of this component is free, and the fill
	// mode rides in it as drawmode + 16 * fill.
	//
	// This is the same trick this field already is: it began as a bare on/off
	// flag hardcoded to 1.0, four bytes of nothing, and became the draw mode
	// at no cost. Doing it again buys per-band fill without adding a
	// vec4[8] to StreamData -- which would divide the 64KB stream buffer into
	// fewer draws and cost batching in every frame of the game, permanently,
	// so that band 3 could have a different pattern from band 4.
	//
	// Decode in the shader with a modulo and a divide. Keep fill under 16.
	//
	// [round2 A6] CLAMPED HERE TOO, not only in the script setters. A draw
	// mode of 16 or more spilled into the fill bits and a negative fill made
	// the air lattice draw a band the surfaces did not, so anything outside
	// the vocabulary (draw 0-4, fill 0-4) is sent as 0 whoever calls this.
	//
	// [round2 B2] AND THE PASSED BIT, as + 256 * passed. Still an exact
	// integer in a float, still no StreamData growth. main.fp decodes it with
	// (word >> 8) & 1 in SweepPassedAt; every other decode site masks the fill
	// with (word >> 4) & 15 so this bit never reads as fill 16.
	void SetSweepBandDraw(int i, int drawmode, int fill = 0, int passed = 0)
	{
		if (i < 0 || i >= 8) return;
		if (drawmode < 0 || drawmode > 4) drawmode = 0;
		if (fill < 0 || fill > 4) fill = 0;
		passed = passed != 0 ? 1 : 0;
		mStreamData.uSweepBands[i].W = (float)(drawmode + 16 * fill + 256 * passed);
	}

	void SetSweepBand(int i, float radius, float thickness, float softness,
		float r, float g, float b, float intensity)
	{
		if (i < 0 || i >= 8) return;
		mStreamData.uSweepBands[i] = { radius, thickness, softness, 1.0f };  // mode 1 = add
		mStreamData.uSweepColors[i] = { r, g, b, intensity };
	}


	// [BB] Flat-edge glow: rgb + reach, a falloff curve, and up to 64 of the
	// sector's linedef endpoints for the shader's per-pixel distance check.
	// "farColor", not "far" -- windows.h defines `far` as an empty macro.
	// [BB] Take this draw OUT of world darkness.
	//
	// Darkness scales the light a room HAS. Some draws are not that. A
	// fullbright sprite is stating it is not lit by the room at all, and the
	// UI billboards are fullbright precisely so a panel stays readable in a
	// dark one -- scaling those by room darkness undoes the thing fullbright
	// is for. Same rule the glow already gets by running after the darkness
	// pass: emissive light is not room light.
	//
	// Additive and default-off: nothing is exempt until something asks, so
	// every existing caller is untouched. Any draw that is emissive rather
	// than lit can use it, not only sprites.
	// PARTIAL, not all-or-nothing. 0 = darkened with everything else, 1 =
	// untouched by it, and anything between is a partial. It was a yes/no;
	// actors need the middle, because a room taken to black takes its monsters
	// with it and a thing you cannot see at all is not atmosphere.
	void SetDarknessExempt(float exempt)
	{
		mStreamData.uDarknessExempt = exempt;
	}

	// [OUTLINE] Trace a sprite in neon -- see func_spriteoutline.fp. Set per
	// draw call from the actor in HWSprite::DrawSprite, which is what lets one
	// corpse light up without every monster in the room lighting up with it.
	//
	// Strength rides in ColorA.w and pulse speed in ColorB.w, so the whole effect
	// is three vec4s rather than a scatter of loose floats.
	void SetSpriteOutline(PalEntry colA, PalEntry colB, float strength,
		float thickness, float threshold, float glow, float pulse, int mode)
	{
		mStreamData.uOutlineColorA = { colA.r / 255.f, colA.g / 255.f, colA.b / 255.f, strength };
		mStreamData.uOutlineColorB = { colB.r / 255.f, colB.g / 255.f, colB.b / 255.f, pulse };
		mStreamData.uOutlineParms = { thickness, threshold, glow, (float)mode };
	}

	void ClearSpriteOutline()
	{
		mStreamData.uOutlineParms = { 0.f, 0.f, 0.f, 0.f };
	}

	// [BB] See uFogDensityScale. 1 is the slab as configured.
	void SetFogDensityScale(float s)
	{
		mStreamData.uFogDensityScale = s;
	}

	// [EYEFADE] See uEyeFadeNear. Read only by EFF_EYEFADE.
	void SetEyeFade(float nearDist, float farDist)
	{
		mStreamData.uEyeFadeNear = nearDist;
		mStreamData.uEyeFadeFar = farDist;
	}

	// [SURFACEDAMAGE] See uSurfaceDamageKey. RenderTexturedWall and DrawFlat set it only while non-zero and put 0 back after.
	void SetSurfaceDamageKey(int key)
	{
		mStreamData.uSurfaceDamageKey = key;
	}

	// [EFFECTLIGHTS] This draw's effect-light mode ("Engine docs/EFFECT_LIGHTS_LB_IMPL_NOTES.md"): 0 none, 1 lit with N.L (walls,
	// flats, decals, per-pixel-lit models), 2 lit without (sprites, particles, voxels -- a sprite's normal is zero). main.fp reads
	// it as uEffectLightMode only inside #ifdef EFFECT_LIGHTS, which only the Vulkan backend defines. It rides in StreamData's
	// padding2 (see the note there): 0, what the pad always held, means none, so a draw that never sets it uploads exactly the
	// bytes it did. The draws set it only on frames with an effect light on the GPU and put 0 back at the end of the same function.
	void SetEffectLightMode(int mode)
	{
		mStreamData.padding2 = mode;
	}

	void SetFlatGlowParams(float r, float g, float b, float reach, const FVector4 &farColor, int falloff, int lineCount, const FVector4* lines, int isCeiling = 0)
	{
		mStreamData.uFlatGlowColor = { r, g, b, reach };
		mStreamData.uFlatGlowFar = farColor;
		mStreamData.uFlatGlowFalloff = falloff;
		mStreamData.uFlatGlowIsCeiling = isCeiling;
		mStreamData.uFlatGlowLineCount = lineCount;
		for (int i = 0; i < lineCount; i++)
		{
			mStreamData.uFlatGlowLines[i] = lines[i];
		}
	}

	// [BB] Clears FLAT GLOW ONLY. It used to zero the sweep uniforms too,
	// which is a different feature that has nothing to do with it -- so any
	// flat WITHOUT glow silently killed the sweep for everything drawn after
	// it. Latent only because the sweep ships off by default.
	void ClearFlatGlow()
	{
		mStreamData.uFlatGlowColor = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uFlatGlowFar = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uFlatGlowFalloff = 0;
		mStreamData.uFlatGlowIsCeiling = 0;
		mStreamData.uFlatGlowLineCount = 0;
	}

	void ClearSweep()
	{
		mStreamData.uSweepOrigin = { 0.0f, 0.0f, 0.0f, 0.0f };
		mStreamData.uSweepCount = 0;
	}

	void SetSoftLightLevel(int llevel, int blendfactor = 0)
	{
		if (blendfactor == 0) mLightParms[3] = llevel / 255.f;
		else mLightParms[3] = -1.f;
	}

	void SetNoSoftLightLevel()
	{
		 mLightParms[3] = -1.f;
	}

	void SetGlowPlanes(const FVector4 &tp, const FVector4& bp)
	{
		mStreamData.uGlowTopPlane = tp;
		mStreamData.uGlowBottomPlane = bp;
	}

	// [G6] THIS DRAW'S SECTOR, as one number every glow lane can hash ("Engine docs/GLOW_SEAM_SEED_G6_IMPL_NOTES.md").
	//
	// firstLine is the sector's first linedef (v1 xy, v2 xy); main.fp's GlowSectorSeedSrc takes
	// v1.x + v1.y from it. Per sector rather than per plane, so a wall and the flat it meets scatter
	// their wave and their grain to the same offset and the corner holds one brightness -- and it does
	// not move when a plane does, so a lift no longer re-rolls the wave of the wall beside it.
	//
	// It rides in uFlatGlowLines[0] (see the member) rather than in a slot of its own: one slot cannot
	// drift from itself, and the shared shader prolog -- the uniform block EVERY program declares,
	// the grab lasers and the Lance included -- stays exactly as it is.
	//
	// Call it wherever the glow planes are set; SetGlowPlanes(FRenderState&, sector_t*) in
	// hw_walls.cpp does both at once so no scene path can set one and forget the other.
	void SetGlowSeedLine(const FVector4 &firstLine)
	{
		mStreamData.uFlatGlowLines[0] = firstLine;
	}

	void SetGradientPlanes(const FVector4& tp, const FVector4& bp)
	{
		mStreamData.uGradientTopPlane = tp;
		mStreamData.uGradientBottomPlane = bp;
	}

	void SetSplitPlanes(const FVector4& tp, const FVector4& bp)
	{
		mStreamData.uSplitTopPlane = tp;
		mStreamData.uSplitBottomPlane = bp;
	}

	void SetDetailParms(float xscale, float yscale, float bias)
	{
		mStreamData.uDetailParms = { xscale, yscale, bias, 0 };
	}

	void SetDynLight(float r, float g, float b)
	{
		mStreamData.uDynLightColor.X = r;
		mStreamData.uDynLightColor.Y = g;
		mStreamData.uDynLightColor.Z = b;
	}

	void SetScreenFade(float f)
	{
		// This component is otherwise unused.
		mStreamData.uDynLightColor.W = f;
	}

	void SetObjectColor(PalEntry pe)
	{
		mStreamData.uObjectColor = pe;
	}

	void SetObjectColor2(PalEntry pe)
	{
		mStreamData.uObjectColor2 = pe;
	}

	void SetSceneColor(PalEntry sceneColor)
	{
		mSceneColor = sceneColor;
	}

	void SetAddColor(PalEntry pe)
	{
		mStreamData.uAddColor = pe;
	}

	void SetNpotEmulation(float factor, float offset)
	{
#ifdef NPOT_EMULATION
		mStreamData.uNpotEmulation = { offset, factor, 0, 0 };
#endif
	}

	void ApplyTextureManipulation(TextureManipulation* texfx)
	{
		if (!texfx || texfx->AddColor.a == 0)
		{
			mStreamData.uTextureAddColor.a = 0;	// we only need to set the flags to 0
		}
		else
		{
			// set up the whole thing
			mStreamData.uTextureAddColor.SetIA(texfx->AddColor);
			auto pe = texfx->ModulateColor;
			mStreamData.uTextureModulateColor.SetFlt(pe.r * pe.a / 255.f, pe.g * pe.a / 255.f, pe.b * pe.a / 255.f, texfx->DesaturationFactor);
			mStreamData.uTextureBlendColor = texfx->BlendColor;
		}
	}
	void SetTextureColors(float* modColor, float* addColor, float* blendColor)
	{
		mStreamData.uTextureAddColor.SetFlt(addColor[0], addColor[1], addColor[2], addColor[3]);
		mStreamData.uTextureModulateColor.SetFlt(modColor[0], modColor[1], modColor[2], modColor[3]);
		mStreamData.uTextureBlendColor.SetFlt(blendColor[0], blendColor[1], blendColor[2], blendColor[3]);
	}

	void SetFog(PalEntry c, float d)
	{
		const float LOG2E = 1.442692f;	// = 1/log(2)
		mFogColor = c;
		mStreamData.uFogColor = mFogColor;
		if (d >= 0.0f) mLightParms[2] = d * (-LOG2E / 64000.f);
	}

	void SetLightParms(float f, float d)
	{
		mLightParms[1] = f;
		mLightParms[0] = d;
	}

	PalEntry GetFogColor() const
	{
		return mFogColor;
	}

	void AlphaFunc(int func, float thresh)
	{
		if (func == Alpha_Greater) mAlphaThreshold = thresh;
		else mAlphaThreshold = thresh - 0.001f;
	}

	void SetLightIndex(int index)
	{
		mLightIndex = index;
	}

	void SetBoneIndexBase(int index)
	{
		mBoneIndexBase = index;
	}

	void SetRenderStyle(FRenderStyle rs)
	{
		mRenderStyle = rs;
	}

	void SetRenderStyle(ERenderStyle rs)
	{
		mRenderStyle = rs;
	}

	auto GetDepthBias()
	{
		return mBias;
	}

	void SetDepthBias(float a, float b)
	{
		mBias.mChanged |= mBias.mFactor != a || mBias.mUnits != b;
		mBias.mFactor = a;
		mBias.mUnits = b;
	}

	void SetDepthBias(FDepthBiasState& bias)
	{
		SetDepthBias(bias.mFactor, bias.mUnits);
	}

	void ClearDepthBias()
	{
		mBias.mChanged |= mBias.mFactor != 0 || mBias.mUnits != 0;
		mBias.mFactor = 0;
		mBias.mUnits = 0;
	}

private:
	void SetMaterial(FMaterial *mat, int clampmode, int translation, int overrideshader)
	{
		mMaterial.mMaterial = mat;
		mMaterial.mClampMode = clampmode;
		mMaterial.mTranslation = translation;
		mMaterial.mOverrideShader = overrideshader;
		mMaterial.mChanged = true;
		mTextureModeFlags = mat->GetLayerFlags();
		auto scale = mat->GetDetailScale();
		mStreamData.uDetailParms = { scale.X, scale.Y, 2, 0 };
	}

public:
	void SetMaterial(FGameTexture* tex, EUpscaleFlags upscalemask, int scaleflags, int clampmode, int translation, int overrideshader)
	{
		tex->setSeen();
		if (!sysCallbacks.PreBindTexture || !sysCallbacks.PreBindTexture(this, tex, upscalemask, scaleflags, clampmode, translation, overrideshader))
		{
			if (shouldUpscale(tex, upscalemask)) scaleflags |= CTF_Upscale;
		}
		auto mat = FMaterial::ValidateTexture(tex, scaleflags);
		assert(mat);
		SetMaterial(mat, clampmode, translation, overrideshader);
	}

	void SetMaterial(FGameTexture* tex, EUpscaleFlags upscalemask, int scaleflags, int clampmode, FTranslationID translation, int overrideshader)
	{
		SetMaterial(tex, upscalemask, scaleflags, clampmode, translation.index(), overrideshader);
	}


	void SetClipSplit(float bottom, float top)
	{
		mClipSplit[0] = bottom;
		mClipSplit[1] = top;
	}

	void SetClipSplit(float *vals)
	{
		memcpy(mClipSplit, vals, 2 * sizeof(float));
	}

	void GetClipSplit(float *out)
	{
		memcpy(out, mClipSplit, 2 * sizeof(float));
	}

	void ClearClipSplit()
	{
		mClipSplit[0] = -1000000.f;
		mClipSplit[1] = 1000000.f;
	}

	void SetVertexBuffer(IVertexBuffer *vb, int offset0, int offset1)
	{
		assert(vb);
		mVertexBuffer = vb;
		mVertexOffsets[0] = offset0;
		mVertexOffsets[1] = offset1;
	}

	void SetIndexBuffer(IIndexBuffer *ib)
	{
		mIndexBuffer = ib;
	}

	template <class T> void SetVertexBuffer(T *buffer)
	{
		auto ptrs = buffer->GetBufferObjects();
		SetVertexBuffer(ptrs.first, 0, 0);
		SetIndexBuffer(ptrs.second);
	}

	void SetInterpolationFactor(float fac)
	{
		mStreamData.uInterpolationFactor = fac;
	}

	float GetInterpolationFactor()
	{
		return mStreamData.uInterpolationFactor;
	}

	void EnableDrawBufferAttachments(bool on) // Used by fog boundary drawer
	{
		EnableDrawBuffers(on ? GetPassDrawBufferCount() : 1);
	}

	int GetPassDrawBufferCount()
	{
		return mPassType == GBUFFER_PASS ? 3 : 1;
	}

	void SetPassType(EPassType passType)
	{
		mPassType = passType;
	}

	EPassType GetPassType()
	{
		return mPassType;
	}

	void SetSpecialColormap(int cm, float flash)
	{
		mColorMapSpecial = cm;
		mColorMapFlash = flash;
	}

	// API-dependent render interface

	// Draw commands
	virtual void ClearScreen() = 0;
	virtual void Draw(int dt, int index, int count, bool apply = true) = 0;
	virtual void DrawIndexed(int dt, int index, int count, bool apply = true) = 0;

	// [MESHPARTICLES] Instanced draws ("Engine docs/COLLISION_DEBRIS_MESH_PLAN.md" #10):
	// the same primitives `instances` times over, the vertex shader telling the copies
	// apart by gl_InstanceIndex, which runs from firstInstance to firstInstance +
	// instances - 1. For any effect that draws many copies of one mesh -- mesh particles
	// are the first. NOT pure, and empty here, so GL and GLES compile unchanged and draw
	// nothing through them; only Vulkan overrides them. Callers gate on a Vulkan-only
	// effect anyway. Triangle fans are not supported (VkRenderStateMolten converts fans
	// only for Draw).
	virtual void DrawInstanced(int dt, int index, int count, int instances, int firstInstance = 0, bool apply = true) {}
	virtual void DrawIndexedInstanced(int dt, int index, int count, int instances, int firstInstance = 0, bool apply = true) {}

	// Immediate render state change commands. These only change infrequently and should not clutter the render state.
	virtual bool SetDepthClamp(bool on) = 0;					// Deactivated only by skyboxes.
	virtual void SetDepthMask(bool on) = 0;						// Used by decals and indirectly by portal setup.
	virtual void SetDepthFunc(int func) = 0;					// Used by models, portals and mirror surfaces.
	virtual void SetDepthRange(float min, float max) = 0;		// Used by portal setup.
	virtual void SetColorMask(bool r, bool g, bool b, bool a) = 0;	// Used by portals.
	virtual void SetStencil(int offs, int op, int flags=-1) = 0;	// Used by portal setup and render hacks.
	virtual void SetCulling(int mode) = 0;						// Used by model drawer only.
	virtual void EnableClipDistance(int num, bool state) = 0;	// Use by sprite sorter for vertical splits.
	virtual void Clear(int targets) = 0;						// not used during normal rendering
	virtual void EnableStencil(bool on) = 0;					// always on for 3D, always off for 2D
	virtual void SetScissor(int x, int y, int w, int h) = 0;	// constant for 3D, changes for 2D
	virtual void SetViewport(int x, int y, int w, int h) = 0;	// constant for all 3D and all 2D
	virtual void EnableDepthTest(bool on) = 0;					// used by 2D, portals and render hacks.
	virtual void EnableMultisampling(bool on) = 0;				// only active for 2D
	virtual void EnableLineSmooth(bool on) = 0;					// constant setting for each 2D drawer operation
	virtual void EnableDrawBuffers(int count, bool apply = false) = 0;	// Used by SSAO and EnableDrawBufferAttachments

	// RS FORK -- r_perflog: named GPU timing groups around the scene and
	// effects passes (hw_drawinfo.cpp), read by "stat gpu" and perflog.txt.
	// No-op by default, so GL and GLES are untouched; VkRenderState routes them
	// to its command-buffer timestamp groups. Every Push needs its Pop on the
	// same path, or the timestamp indices after it are wrong.
	virtual void PushGroup(const FString& name) {}
	virtual void PopGroup() {}

	// [2a] READABLE SCENE DEPTH ("Engine docs/GPU_PARTICLES_STAGE2_PLAN.md" 2a).
	// true: make the scene depth readable by the effects drawn next -- on Vulkan,
	// end the scene pass and begin it again holding depth read-only (no writes, no
	// clears), where an effect's scene-depth shader variant samples it. false: back
	// to the ordinary writable pass. Returns whether depth is readable afterwards.
	// A backend or target that cannot -- GL, GLES, a camera texture, a save
	// picture -- returns false and changes nothing, and the caller draws exactly as
	// it did before 2a. Whatever is drawn in between must not need depth writes.
	// No-op by default, so GL and GLES are untouched. Every true needs its false on
	// the same path, before anything that writes depth.
	virtual bool SetSceneDepthReadable(bool on) { return false; }

	void SetColorMask(bool on)
	{
		SetColorMask(on, on, on, on);
	}

	void ResetFadeColor()
	{
		mFadeColor = gl_global_fade_color;
		mStreamData.uGlobalFadeColor = mFadeColor;
		mStreamData.uGlobalFade = gl_global_fade ? 1 : 0;
		mStreamData.uGlobalFadeMode = gl_global_fade_debug ? 2 : -1;
		mStreamData.uGlobalFadeDensity = gl_global_fade_density;
		mStreamData.uGlobalFadeGradient = gl_global_fade_gradient;
		mStreamData.uLightRangeLimit = gl_light_range_limit;
	}

	void InitSceneClearColor()
	{
		float r, g, b;
		if (gl_global_fade)
		{
			mSceneColor = mFadeColor;
		}
		r = g = b = 1.f;
		screen->mSceneClearColor[0] = mSceneColor.r * r / 255.f;
		screen->mSceneClearColor[1] = mSceneColor.g * g / 255.f;
		screen->mSceneClearColor[2] = mSceneColor.b * b / 255.f;
	}

};
