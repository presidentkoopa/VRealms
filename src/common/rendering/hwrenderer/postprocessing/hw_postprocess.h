/*
** hw_postprocess.h
**
** Postprocessing framework
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
** Copyright 2016-2020 Magnus Norddahl
**
** SPDX-License-Identifier: Zlib
**
**---------------------------------------------------------------------------
**
*/

#pragma once

#include "hwrenderer/data/shaderuniforms.h"
#include <memory>
#include <map>
#include "intrect.h"

#include "hwrenderer/postprocessing/hw_postprocessshader.h"

struct PostProcessShader;

typedef FRenderStyle PPBlendMode;
typedef IntRect PPViewport;

class PPTexture;
class PPShader;

// Binding point for automatic uniforms (separate from user uniforms)
// Chosen to not conflict with texture bindings (0-N) or shadow map buffers
constexpr int AUTOMATIC_UNIFORMS_BINDING = 15;

enum class ETonemapMode : uint8_t
{
	None,
	Uncharted2,
	HejlDawson,
	Reinhard,
	Linear,
	Palette,
	NumTonemapModes
};



enum class PPFilterMode { Nearest, Linear };
enum class PPWrapMode { Clamp, Repeat };
// [LIGHTMASK] LightMaskCurrent / LightMaskNext: the pair of images the scene's light mask is
// carried through post-processing in (PPLightMask below), used the way the pipeline images
// are. Appended, so no existing value moves; a later addition appends after them.
// [SMOKEVOLUME] ExternalImage: an image a backend owns for work of its own (the smoke volume's 3D
// density, say), read by a pass as an input. PPExternalImage below says which. Appended after the
// light mask's pair ("Engine docs/SMOKE_VOLUME_PLAN.md" 13c, review S4).
enum class PPTextureType { CurrentPipelineTexture, NextPipelineTexture, PPTexture, SceneColor, SceneFog, SceneNormal, SceneDepth, SwapChain, ShadowMap, LightMaskCurrent, LightMaskNext, ExternalImage };

// [SMOKEVOLUME] THE BACKEND-OWNED IMAGES A PASS MAY READ (PPTextureType::ExternalImage).
//
// Common code cannot name a backend object, so a pass names one of these and the backend resolves
// the name to its own image (Vulkan: VkTextureManager::GetTexture). A client that wants a pass to read
// another of its images appends the image here and resolves it there. Each is named for what it holds.
//
// The name travels as a TOKEN: the input's Texture pointer is the one fixed PPTexture kept for that
// name (PPExternalImageToken). Every backend path that passes a PPTextureInput's Type and Texture on,
// the descriptor writes among them, therefore carries it unchanged. A token never gets a backend.
enum class PPExternalImage
{
	SmokeDensityLatest,		// the smoke volume's density (r) and heat (g), latest simulation state (3D)
	SmokeDensityPrevious,	// the same one simulation step earlier; the march blends the two by TicFrac (3D)
	SmokeTileActive,		// one texel per SMOKE_TILE_CELLS^3 cells: 1 = the tile may hold smoke (3D, R8)
	SmokeLight,				// [13d] the smoke's light grid: rgb the light reaching each place, a its luminance weight (3D, RGBA16F)
	SmokeLightDirection,	// [13d] the same grid: xyz the direction light travels there, times its share (3D, RGBA8 SNORM)
	SmokeBeams,				// [13e] the beam lines that may meet the smoke this frame: SMOKE_BEAMS_MAX x 4 texels (2D, RGBA32F)
	EmissiveVolumeList,		// [EMISSIVEVOLUMES] this frame's drawn emissive volumes: EMISSIVE_VOLUMES_DRAWN_MAX x EMISSIVE_VOLUME_TEXELS (2D, RGBA32F)
	EmissiveNoise,			// [EMISSIVEVOLUMES] their baked noise: EMISSIVE_NOISE_SIZE^3, r and g (3D, RG8 or RGBA8; read linear, repeat)
	Count
};

PPTexture *PPExternalImageToken(PPExternalImage image);
PPExternalImage PPExternalImageFromToken(const PPTexture *token);	// Count when it is not a token

class PPTextureInput
{
public:
	PPFilterMode Filter = PPFilterMode::Nearest;
	PPWrapMode Wrap = PPWrapMode::Clamp;
	PPTextureType Type = PPTextureType::CurrentPipelineTexture;
	PPTexture *Texture = nullptr;
};

class PPOutput
{
public:
	PPTextureType Type = PPTextureType::NextPipelineTexture;
	PPTexture *Texture = nullptr;
};

class PPUniforms
{
public:
	PPUniforms()
	{
	}

	PPUniforms(const PPUniforms &src)
	{
		Data = src.Data;
	}

	~PPUniforms()
	{
		Clear();
	}

	PPUniforms &operator=(const PPUniforms &src)
	{
		Data = src.Data;
		return *this;
	}

	void Clear()
	{
		Data.Clear();
	}

	template<typename T>
	void Set(const T &v)
	{
		if (Data.Size() != (int)sizeof(T))
		{
			Data.Resize(sizeof(T));
			memcpy(Data.Data(), &v, Data.Size());
		}
	}

	TArray<uint8_t> Data;
};

class PPRenderState
{
public:
	virtual ~PPRenderState() = default;

	virtual void PushGroup(const FString &name) = 0;
	virtual void PopGroup() = 0;

	virtual void Draw() = 0;
	virtual void CopyToTexture(PPTexture* dst) = 0;

	void Clear()
	{
		Shader = nullptr;
		Textures = TArray<PPTextureInput>();
		Uniforms = PPUniforms();
		Viewport = PPViewport();
		BlendMode = PPBlendMode();
		Output = PPOutput();
		ShadowMapBuffers = false;
	}

	void SetInputTexture(int index, PPTexture *texture, PPFilterMode filter = PPFilterMode::Nearest, PPWrapMode wrap = PPWrapMode::Clamp)
	{
		if ((int)Textures.Size() < index + 1)
			Textures.Resize(index + 1);
		auto &tex = Textures[index];
		tex.Filter = filter;
		tex.Wrap = wrap;
		tex.Type = PPTextureType::PPTexture;
		tex.Texture = texture;
	}

	void SetInputCurrent(int index, PPFilterMode filter = PPFilterMode::Nearest, PPWrapMode wrap = PPWrapMode::Clamp)
	{
		SetInputSpecialType(index, PPTextureType::CurrentPipelineTexture, filter, wrap);
	}

	void SetInputSceneColor(int index, PPFilterMode filter = PPFilterMode::Nearest, PPWrapMode wrap = PPWrapMode::Clamp)
	{
		SetInputSpecialType(index, PPTextureType::SceneColor, filter, wrap);
	}

	void SetInputSceneFog(int index, PPFilterMode filter = PPFilterMode::Nearest, PPWrapMode wrap = PPWrapMode::Clamp)
	{
		SetInputSpecialType(index, PPTextureType::SceneFog, filter, wrap);
	}

	void SetInputSceneNormal(int index, PPFilterMode filter = PPFilterMode::Nearest, PPWrapMode wrap = PPWrapMode::Clamp)
	{
		SetInputSpecialType(index, PPTextureType::SceneNormal, filter, wrap);
	}

	void SetInputSceneDepth(int index, PPFilterMode filter = PPFilterMode::Nearest, PPWrapMode wrap = PPWrapMode::Clamp)
	{
		SetInputSpecialType(index, PPTextureType::SceneDepth, filter, wrap);
	}

	// [LIGHTMASK] The light mask as carried so far this eye (PPLightMask).
	void SetInputLightMask(int index, PPFilterMode filter = PPFilterMode::Nearest, PPWrapMode wrap = PPWrapMode::Clamp)
	{
		SetInputSpecialType(index, PPTextureType::LightMaskCurrent, filter, wrap);
	}

	// [SMOKEVOLUME] An image the backend owns (PPExternalImage), such as a 3D volume. The backend
	// resolves the token; a draw that names an image it does not have ready draws nothing.
	void SetInputExternalImage(int index, PPExternalImage image, PPFilterMode filter = PPFilterMode::Nearest, PPWrapMode wrap = PPWrapMode::Clamp)
	{
		SetInputSpecialType(index, PPTextureType::ExternalImage, filter, wrap);
		Textures[index].Texture = PPExternalImageToken(image);
	}

	void SetInputSpecialType(int index, PPTextureType type, PPFilterMode filter = PPFilterMode::Nearest, PPWrapMode wrap = PPWrapMode::Clamp)
	{
		if ((int)Textures.Size() < index + 1)
			Textures.Resize(index + 1);
		auto &tex = Textures[index];
		tex.Filter = filter;
		tex.Wrap = wrap;
		tex.Type = type;
		tex.Texture = nullptr;
	}

	void SetShadowMapBuffers(bool enable)
	{
		ShadowMapBuffers = enable;
	}

	void SetOutputTexture(PPTexture *texture)
	{
		Output.Type = PPTextureType::PPTexture;
		Output.Texture = texture;
	}

	void SetOutputCurrent()
	{
		Output.Type = PPTextureType::CurrentPipelineTexture;
		Output.Texture = nullptr;
	}

	void SetOutputNext()
	{
		Output.Type = PPTextureType::NextPipelineTexture;
		Output.Texture = nullptr;
	}

	void SetOutputSceneColor()
	{
		Output.Type = PPTextureType::SceneColor;
		Output.Texture = nullptr;
	}

	void SetOutputSwapChain()
	{
		Output.Type = PPTextureType::SwapChain;
		Output.Texture = nullptr;
	}

	void SetOutputShadowMap()
	{
		Output.Type = PPTextureType::ShadowMap;
		Output.Texture = nullptr;
	}

	// [LIGHTMASK] Write the light mask in place (a carry that adds to it), or into the other
	// image of its pair (a carry that moves it, as the heat warp does; the pair then swaps).
	void SetOutputLightMaskCurrent()
	{
		Output.Type = PPTextureType::LightMaskCurrent;
		Output.Texture = nullptr;
	}

	void SetOutputLightMaskNext()
	{
		Output.Type = PPTextureType::LightMaskNext;
		Output.Texture = nullptr;
	}

	void SetNoBlend()
	{
		BlendMode.BlendOp = STYLEOP_Add;
		BlendMode.SrcAlpha = STYLEALPHA_One;
		BlendMode.DestAlpha = STYLEALPHA_Zero;
		BlendMode.Flags = 0;
	}

	void SetAdditiveBlend()
	{
		BlendMode.BlendOp = STYLEOP_Add;
		BlendMode.SrcAlpha = STYLEALPHA_One;
		BlendMode.DestAlpha = STYLEALPHA_One;
		BlendMode.Flags = 0;
	}

	void SetAlphaBlend()
	{
		BlendMode.BlendOp = STYLEOP_Add;
		BlendMode.SrcAlpha = STYLEALPHA_Src;
		BlendMode.DestAlpha = STYLEALPHA_InvSrc;
		BlendMode.Flags = 0;
	}

	// [SMOKEVOLUME] Premultiplied: dst = src + dst x (1 - src.a). For a pass whose rgb is light it adds
	// and whose alpha is how much of what lies behind it hides. The smoke composite outputs
	// (light, 1 - T), which this blend turns into scene x T + light, linear in both.
	void SetPremultipliedAlphaBlend()
	{
		BlendMode.BlendOp = STYLEOP_Add;
		BlendMode.SrcAlpha = STYLEALPHA_One;
		BlendMode.DestAlpha = STYLEALPHA_InvSrc;
		BlendMode.Flags = 0;
	}

	PPShader *Shader;
	TArray<PPTextureInput> Textures;
	PPUniforms Uniforms;
	PPViewport Viewport;
	PPBlendMode BlendMode;
	PPOutput Output;
	bool ShadowMapBuffers = false;

	float TimeDelta = 0.0f;
	float Time = 0.0f;
	float TimeGame = 0.0f;
};

class PPResource
{
public:
	PPResource()
	{
		Next = First;
		First = this;
		if (Next) Next->Prev = this;
	}

	PPResource(const PPResource &)
	{
		Next = First;
		First = this;
		if (Next) Next->Prev = this;
	}

	virtual ~PPResource()
	{
		if (Next) Next->Prev = Prev;
		if (Prev) Prev->Next = Next;
		else First = Next;
	}

	PPResource &operator=(const PPResource &other)
	{
		return *this;
	}

	static void ResetAll()
	{
		for (PPResource *cur = First; cur; cur = cur->Next)
			cur->ResetBackend();
	}

	virtual void ResetBackend() = 0;

private:
	static PPResource *First;
	PPResource *Prev = nullptr;
	PPResource *Next = nullptr;
};

class PPTextureBackend
{
public:
	virtual ~PPTextureBackend() = default;
};

class PPTexture : public PPResource
{
public:
	PPTexture() = default;
	PPTexture(int width, int height, PixelFormat format, std::shared_ptr<void> data = {}) : Width(width), Height(height), Format(format), Data(data) { }

	void ResetBackend() override { Backend.reset(); }

	int Width;
	int Height;
	PixelFormat Format;
	std::shared_ptr<void> Data;

	std::unique_ptr<PPTextureBackend> Backend;
};

class PPShaderBackend
{
public:
	virtual ~PPShaderBackend() = default;
};

class PPShader : public PPResource
{
public:
	PPShader() = default;
	PPShader(const FString &fragment, const FString &defines, const std::vector<UniformFieldDesc> &uniforms, int version = 330) : FragmentShader(fragment), Defines(defines), Uniforms(uniforms), Version(version) { }

	void ResetBackend() override { Backend.reset(); }

	FString VertexShader = "shaders/pp/screenquad.vp";
	FString FragmentShader;
	FString Defines;
	std::vector<UniformFieldDesc> Uniforms;
	int Version = 330;

	std::unique_ptr<PPShaderBackend> Backend;
};

/////////////////////////////////////////////////////////////////////////////

struct ExtractUniforms
{
	FVector2 Scale;
	FVector2 Offset;
	float Threshold;
	float Knee;
	float padding0, padding1;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "Scale", UniformType::Vec2, offsetof(ExtractUniforms, Scale) },
			{ "Offset", UniformType::Vec2, offsetof(ExtractUniforms, Offset) },
			{ "Threshold", UniformType::Float, offsetof(ExtractUniforms, Threshold) },
			{ "Knee", UniformType::Float, offsetof(ExtractUniforms, Knee) },
			{ "padding0", UniformType::Float, offsetof(ExtractUniforms, padding0) },
			{ "padding1", UniformType::Float, offsetof(ExtractUniforms, padding1) }
		};
	}
};

// [PINNEDBLOOM] The bloom extract weighed by the light mask (bloomextract.fp's BLOOM_EXTRACT_SHARE and
// BLOOM_EXTRACT_DUAL; PPBloomPlan). Structs of their own, so ExtractUniforms -- today's extract program's
// prolog -- keeps its bytes. SHARE: one look times one share (WeightClass 1 the pinned share, 0 the rest);
// inputs 0 the image, 1 that look's exposure, 2 the light mask. DUAL: both looks, each with its tint;
// inputs 0 the image, 1 the rest look's exposure, 2 the pinned look's exposure, 3 the light mask.
struct ExtractShareUniforms
{
	FVector2 Scale;
	FVector2 Offset;
	float Threshold;
	float Knee;
	int WeightClass;
	float Padding0;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "Scale", UniformType::Vec2, offsetof(ExtractShareUniforms, Scale) },
			{ "Offset", UniformType::Vec2, offsetof(ExtractShareUniforms, Offset) },
			{ "Threshold", UniformType::Float, offsetof(ExtractShareUniforms, Threshold) },
			{ "Knee", UniformType::Float, offsetof(ExtractShareUniforms, Knee) },
			{ "WeightClass", UniformType::Int, offsetof(ExtractShareUniforms, WeightClass) },
			{ "Padding0", UniformType::Float, offsetof(ExtractShareUniforms, Padding0) },
		};
	}
};

static_assert(offsetof(ExtractShareUniforms, Threshold) == 16, "ExtractShareUniforms::Threshold offset");
static_assert(offsetof(ExtractShareUniforms, WeightClass) == 24, "ExtractShareUniforms::WeightClass offset");
static_assert(sizeof(ExtractShareUniforms) == 32, "ExtractShareUniforms must be 32 bytes (a push constant block)");

struct ExtractDualUniforms
{
	FVector2 Scale;
	FVector2 Offset;
	float RestThreshold;
	float RestKnee;
	float PinThreshold;
	float PinKnee;
	FVector3 RestTint;
	float Padding0;
	FVector3 PinTint;
	float Padding1;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "Scale", UniformType::Vec2, offsetof(ExtractDualUniforms, Scale) },
			{ "Offset", UniformType::Vec2, offsetof(ExtractDualUniforms, Offset) },
			{ "RestThreshold", UniformType::Float, offsetof(ExtractDualUniforms, RestThreshold) },
			{ "RestKnee", UniformType::Float, offsetof(ExtractDualUniforms, RestKnee) },
			{ "PinThreshold", UniformType::Float, offsetof(ExtractDualUniforms, PinThreshold) },
			{ "PinKnee", UniformType::Float, offsetof(ExtractDualUniforms, PinKnee) },
			{ "RestTint", UniformType::Vec3, offsetof(ExtractDualUniforms, RestTint) },
			{ "Padding0", UniformType::Float, offsetof(ExtractDualUniforms, Padding0) },
			{ "PinTint", UniformType::Vec3, offsetof(ExtractDualUniforms, PinTint) },
			{ "Padding1", UniformType::Float, offsetof(ExtractDualUniforms, Padding1) },
		};
	}
};

static_assert(offsetof(ExtractDualUniforms, RestTint) == 32, "ExtractDualUniforms::RestTint must start at 32 (std140 vec3)");
static_assert(offsetof(ExtractDualUniforms, PinTint) == 48, "ExtractDualUniforms::PinTint must start at 48 (std140 vec3)");
static_assert(sizeof(ExtractDualUniforms) == 64, "ExtractDualUniforms must be 64 bytes (a push constant block)");

struct BlurUniforms
{
	float SampleWeights[8];

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "SampleWeights0", UniformType::Float, offsetof(BlurUniforms, SampleWeights[0]) },
			{ "SampleWeights1", UniformType::Float, offsetof(BlurUniforms, SampleWeights[1]) },
			{ "SampleWeights2", UniformType::Float, offsetof(BlurUniforms, SampleWeights[2]) },
			{ "SampleWeights3", UniformType::Float, offsetof(BlurUniforms, SampleWeights[3]) },
			{ "SampleWeights4", UniformType::Float, offsetof(BlurUniforms, SampleWeights[4]) },
			{ "SampleWeights5", UniformType::Float, offsetof(BlurUniforms, SampleWeights[5]) },
			{ "SampleWeights6", UniformType::Float, offsetof(BlurUniforms, SampleWeights[6]) },
			{ "SampleWeights7", UniformType::Float, offsetof(BlurUniforms, SampleWeights[7]) },
		};
	}
};

// [BLOOMSTEP] The wide bloom blur's uniforms (blur.fp's BLUR_STEPPED variants; E3 in
// "Engine docs/REVIEW_BLOOM_PLAN.md"). A struct of its own on purpose: BlurUniforms is
// the prolog of today's two blur programs, which the menu blur (PPBloom::RenderBlur)
// shares, and adding a field to it would change their SPIR-V. TexelStep is how many
// texels apart the seven taps sit; ReadsPerTap is how many linear reads each tap
// averages over its own stretch (PPBloom::ComputeBlurSamplesStepped). Never name a
// GLSL uniform `step`: it hides the builtin step().
struct BlurSteppedUniforms
{
	float SampleWeights[8];
	float TexelStep;
	int ReadsPerTap;
	float Padding0, Padding1;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "SampleWeights0", UniformType::Float, offsetof(BlurSteppedUniforms, SampleWeights[0]) },
			{ "SampleWeights1", UniformType::Float, offsetof(BlurSteppedUniforms, SampleWeights[1]) },
			{ "SampleWeights2", UniformType::Float, offsetof(BlurSteppedUniforms, SampleWeights[2]) },
			{ "SampleWeights3", UniformType::Float, offsetof(BlurSteppedUniforms, SampleWeights[3]) },
			{ "SampleWeights4", UniformType::Float, offsetof(BlurSteppedUniforms, SampleWeights[4]) },
			{ "SampleWeights5", UniformType::Float, offsetof(BlurSteppedUniforms, SampleWeights[5]) },
			{ "SampleWeights6", UniformType::Float, offsetof(BlurSteppedUniforms, SampleWeights[6]) },
			{ "SampleWeights7", UniformType::Float, offsetof(BlurSteppedUniforms, SampleWeights[7]) },
			{ "TexelStep", UniformType::Float, offsetof(BlurSteppedUniforms, TexelStep) },
			{ "ReadsPerTap", UniformType::Int, offsetof(BlurSteppedUniforms, ReadsPerTap) },
			{ "Padding0", UniformType::Float, offsetof(BlurSteppedUniforms, Padding0) },
			{ "Padding1", UniformType::Float, offsetof(BlurSteppedUniforms, Padding1) },
		};
	}
};

static_assert(offsetof(BlurSteppedUniforms, TexelStep) == 32, "BlurSteppedUniforms: TexelStep must follow the eight weights");
static_assert(offsetof(BlurSteppedUniforms, ReadsPerTap) == 36, "BlurSteppedUniforms: ReadsPerTap offset");
static_assert(offsetof(BlurSteppedUniforms, Padding1) == 44, "BlurSteppedUniforms: padding offset");
static_assert(sizeof(BlurSteppedUniforms) == 48, "BlurSteppedUniforms must be 48 bytes (a push constant block)");

/////////////////////////////////////////////////////////////////////////////

// [BB] Volumetric beam -- see shaders/pp/volumetricbeam.fp. Lights the air
// inside a cone rather than the surfaces it lands on, so the beam itself is
// visible. Values arrive already in VIEW space: the CPU resolves world to
// view per eye, which is what makes this correct in stereo for free.
struct VolumetricBeamUniforms
{
	FVector3 BeamPos;
	float BeamLength;
	FVector3 BeamDir;
	float CosInner;
	FVector3 BeamColor;
	float CosOuter;
	FVector2 TanHalfFov;
	float Density;
	float Falloff;
	int StepCount;
	float DustAmount;
	float DustScale;
	float DustDrift;
	float DustTime;

	// TURNING THE DEPTH BUFFER INTO A DISTANCE.
	//
	// The pass used to clamp its march against the RAW depth sample, which is
	// a nonlinear value in 0..1, as though it were a view-space distance in map
	// units. Anything at all in front of the camera therefore capped the march
	// at under one map unit, and the beam integrated across almost nothing.
	// Same two constants lineardepth.fp uses, computed the same way.
	float LinearizeDepthA;
	float LinearizeDepthB;

	// How much the beam fades as your VIEW lines up with it. See the note in
	// volumetricbeam.fp -- a cone seen end-on is a disc, and on a flat screen
	// the default mount points exactly where you look, so end-on is the only
	// way you ever see it. 0 restores the old behaviour.
	float AxisFade;

	// ---- AND THE ROW ENDS EXACTLY HERE ------------------------------------
	//
	// std140 aligns a mat4 to sixteen bytes and the C++ struct does not, so
	// the three floats above have to fill out the row DustTime opened, and
	// ViewToWorld then starts at 96, which is 16 x 6, in both.
	//
	// Count it, do not eyeball it. The previous attempt at this comment added
	// TWO pad floats instead of one, pushing the matrix to offset 100 where
	// std140 expects 112, and it did that while claiming in its own text to be
	// fixing the alignment. World-space dust was being sampled through a
	// matrix assembled from twelve bytes of the wrong floats for a day.
	//
	//   BeamPos 0    BeamLength 12                        -> row 0 ends 16
	//   BeamDir 16   CosInner 28                          -> row 1 ends 32
	//   BeamColor 32 CosOuter 44                           -> row 2 ends 48
	//   TanHalfFov 48 Density 56 Falloff 60                -> row 3 ends 64
	//   StepCount 64 DustAmount 68 DustScale 72 Drift 76   -> row 4 ends 80
	//   DustTime 80  DepthA 84  DepthB 88  AxisFade 92     -> row 5 ends 96
	//   ViewToWorld 96                                     -> aligned
	float ViewToWorld[16];   // plain floats: VSMatrix is not visible in this header

	// ---- APPENDED AFTER THE MATRIX, which ends at 160 ----------------------
	//
	//   ProjOffset 160  SceneScale 168  SceneOffset 176                 vec2s
	//   AxisFadeReach 184  BeamPadding0 188                   -> block ends 192
	//
	// The static_asserts below the struct hold these offsets; add fields after
	// BeamPadding0 (or replace it) rather than in the middle.

	// Projection m[8], m[9]: the off-centre terms of an asymmetric (headset)
	// frustum. The ray rebuild ignored them and each eye's rays were shifted
	// sideways. Zero on a symmetric projection. See volumetricbeam.fp.
	FVector2 ProjOffset;

	// Where the scene viewport sits inside the depth texture, so the depth
	// lookup reads the right texels with a status bar or reduced screen size.
	// Filled in PPVolumetricBeam::Render from screen->SceneScale()/SceneOffset(),
	// the same pair the bloom extract and lineardepth.fp use.
	FVector2 SceneScale;
	FVector2 SceneOffset;

	// Map units: the axis fade only applies when the beam's axis passes within
	// this distance of the eye (vol_beam_axisfade_reach). 0 = old behaviour.
	float AxisFadeReach;
	float BeamPadding0;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "BeamPos", UniformType::Vec3, offsetof(VolumetricBeamUniforms, BeamPos) },
			{ "BeamLength", UniformType::Float, offsetof(VolumetricBeamUniforms, BeamLength) },
			{ "BeamDir", UniformType::Vec3, offsetof(VolumetricBeamUniforms, BeamDir) },
			{ "CosInner", UniformType::Float, offsetof(VolumetricBeamUniforms, CosInner) },
			{ "BeamColor", UniformType::Vec3, offsetof(VolumetricBeamUniforms, BeamColor) },
			{ "CosOuter", UniformType::Float, offsetof(VolumetricBeamUniforms, CosOuter) },
			{ "TanHalfFov", UniformType::Vec2, offsetof(VolumetricBeamUniforms, TanHalfFov) },
			{ "Density", UniformType::Float, offsetof(VolumetricBeamUniforms, Density) },
			{ "Falloff", UniformType::Float, offsetof(VolumetricBeamUniforms, Falloff) },
			{ "StepCount", UniformType::Int, offsetof(VolumetricBeamUniforms, StepCount) },
			{ "DustAmount", UniformType::Float, offsetof(VolumetricBeamUniforms, DustAmount) },
			{ "DustScale", UniformType::Float, offsetof(VolumetricBeamUniforms, DustScale) },
			{ "DustDrift", UniformType::Float, offsetof(VolumetricBeamUniforms, DustDrift) },
			{ "DustTime", UniformType::Float, offsetof(VolumetricBeamUniforms, DustTime) },
			{ "LinearizeDepthA", UniformType::Float, offsetof(VolumetricBeamUniforms, LinearizeDepthA) },
			{ "LinearizeDepthB", UniformType::Float, offsetof(VolumetricBeamUniforms, LinearizeDepthB) },
			{ "AxisFade", UniformType::Float, offsetof(VolumetricBeamUniforms, AxisFade) },
			{ "ViewToWorld", UniformType::Mat4, offsetof(VolumetricBeamUniforms, ViewToWorld) },
			{ "ProjOffset", UniformType::Vec2, offsetof(VolumetricBeamUniforms, ProjOffset) },
			{ "SceneScale", UniformType::Vec2, offsetof(VolumetricBeamUniforms, SceneScale) },
			{ "SceneOffset", UniformType::Vec2, offsetof(VolumetricBeamUniforms, SceneOffset) },
			{ "AxisFadeReach", UniformType::Float, offsetof(VolumetricBeamUniforms, AxisFadeReach) },
			{ "BeamPadding0", UniformType::Float, offsetof(VolumetricBeamUniforms, BeamPadding0) },
		};
	}
};

// std140 guard rails for VolumetricBeamUniforms. UniformBlockDecl::Create emits
// the fields in declaration order with no explicit offsets, so the C++ layout IS
// the GLSL layout, and a mismatch is silent -- see the ViewToWorld note above,
// which cost a day. These make the compiler count instead of a comment.
static_assert(offsetof(VolumetricBeamUniforms, ViewToWorld) == 96,
	"VolumetricBeamUniforms::ViewToWorld must start at 96 for std140");
static_assert(offsetof(VolumetricBeamUniforms, ProjOffset) == 160,
	"VolumetricBeamUniforms::ProjOffset must start at 160 for std140");
static_assert(offsetof(VolumetricBeamUniforms, SceneScale) == 168,
	"VolumetricBeamUniforms::SceneScale must start at 168 for std140");
static_assert(offsetof(VolumetricBeamUniforms, SceneOffset) == 176,
	"VolumetricBeamUniforms::SceneOffset must start at 176 for std140");
static_assert(offsetof(VolumetricBeamUniforms, AxisFadeReach) == 184,
	"VolumetricBeamUniforms::AxisFadeReach must start at 184 for std140");
static_assert(sizeof(VolumetricBeamUniforms) == 192,
	"VolumetricBeamUniforms must be 192 bytes; pad to a 16-byte row");

class PPVolumetricBeam
{
public:
	void Render(PPRenderState *renderstate, int sceneWidth, int sceneHeight);

	// Set per scene draw, in view space, by the renderer. Cleared when no beam
	// is live so a switched-off flashlight costs nothing at all.
	//
	// SEVERAL, and the pass simply runs once per beam. That works with no
	// blending machinery and no shader change because the pass is already
	// ADDITIVE -- it emits only its own light and never reads the scene back --
	// so N draws composite exactly as one draw of N beams would.
	//
	// It is also cheaper than it sounds. Every beam bounds itself with an
	// analytic ray/cone intersection before marching, so one that is off-screen
	// or behind you costs a depth sample and a few dot products per pixel and
	// returns black. Only beams actually lighting the same pixels cost twice.
	void ClearBeams() { count = 0; }
	void AddBeam(const VolumetricBeamUniforms &u)
	{
		if (count < MAX_BEAMS) uniforms[count++] = u;
	}

	// [13e] Whether Render will draw any cone this eye (its own skip test), so the smoke pass knows to draw the
	// transmittance curve the cones read in smoke (PPSmokeVolume::ConesDimmedByHaze).
	bool HasCones() const
	{
		for (int i = 0; i < count; i++)
		{
			if (uniforms[i].Density > 0.0f && uniforms[i].BeamLength > 0.0f)
				return true;
		}
		return false;
	}

private:
	// 32, AND IT MUST MATCH FLevelLocals::MAX_VOL_BEAMS EXACTLY.
	//
	// AddBeam above drops silently when this is the smaller of the two -- the
	// caller in hw_drawinfo.cpp iterates to the level's constant and has no way
	// to learn that anything was refused. A mismatch is a beam that never
	// renders with nothing anywhere explaining it.
	static const int MAX_BEAMS = 32;
	VolumetricBeamUniforms uniforms[MAX_BEAMS] = {};
	int count = 0;

	PPShader Beam = { "shaders/pp/volumetricbeam.fp", "", VolumetricBeamUniforms::Desc() };
	// MSAA variant: with gl_multisample > 1 the scene depth is multisampled and
	// must be read with texelFetch on a sampler2DMS, as lineardepth.fp does.
	// Render() picks between the two. Without it the beam did not draw with MSAA.
	PPShader BeamMS = { "shaders/pp/volumetricbeam.fp", "#define MULTISAMPLE\n", VolumetricBeamUniforms::Desc() };
	// [13e] In smoke (PPSmokeVolume::ConesDimmedByHaze): each step of the cone dimmed by the haze in front of it, read from
	// the smoke's transmittance curve. The same uniforms; inputs 1-3 the smoke's march, depth and curve.
	PPShader BeamSmoke = { "shaders/pp/volumetricbeam.fp", "#define SMOKE_TRANSMITTANCE\n", VolumetricBeamUniforms::Desc() };
	PPShader BeamSmokeMS = { "shaders/pp/volumetricbeam.fp", "#define MULTISAMPLE\n#define SMOKE_TRANSMITTANCE\n", VolumetricBeamUniforms::Desc() };
};

struct HeatmapUniforms
{
	FVector3 HeatColorLow;
	float HeatScale;
	FVector3 HeatColorHigh;
	float HeatCeiling;
	FVector2 TanHalfFov;
	FVector2 HeatOrigin;
	FVector2 HeatInvSize;
	float HeatTolerance;
	float LinearizeDepthA;
	float LinearizeDepthB;
	float pad0;
	float pad1;
	float pad2;
	float ViewToWorld[16];

	// ---- APPENDED AFTER THE MATRIX (80..144), same fixes as the beam pass ----
	//   ProjOffset 144  SceneScale 152  SceneOffset 160  heatPad3 168  heatPad4 172
	// ProjOffset: projection m[8], m[9], the off-centre terms of a headset eye's
	// frustum (set in HWDrawInfo::SetupHeatmap). SceneScale/SceneOffset: where
	// the scene viewport sits in the depth texture (set in PPHeatmap::Render).
	FVector2 ProjOffset;
	FVector2 SceneScale;
	FVector2 SceneOffset;
	float heatPad3;
	float heatPad4;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "HeatColorLow", UniformType::Vec3, offsetof(HeatmapUniforms, HeatColorLow) },
			{ "HeatScale", UniformType::Float, offsetof(HeatmapUniforms, HeatScale) },
			{ "HeatColorHigh", UniformType::Vec3, offsetof(HeatmapUniforms, HeatColorHigh) },
			{ "HeatCeiling", UniformType::Float, offsetof(HeatmapUniforms, HeatCeiling) },
			{ "TanHalfFov", UniformType::Vec2, offsetof(HeatmapUniforms, TanHalfFov) },
			{ "HeatOrigin", UniformType::Vec2, offsetof(HeatmapUniforms, HeatOrigin) },
			{ "HeatInvSize", UniformType::Vec2, offsetof(HeatmapUniforms, HeatInvSize) },
			{ "HeatTolerance", UniformType::Float, offsetof(HeatmapUniforms, HeatTolerance) },
			{ "LinearizeDepthA", UniformType::Float, offsetof(HeatmapUniforms, LinearizeDepthA) },
			{ "LinearizeDepthB", UniformType::Float, offsetof(HeatmapUniforms, LinearizeDepthB) },
			{ "pad0", UniformType::Float, offsetof(HeatmapUniforms, pad0) },
			{ "pad1", UniformType::Float, offsetof(HeatmapUniforms, pad1) },
			{ "pad2", UniformType::Float, offsetof(HeatmapUniforms, pad2) },
			{ "ViewToWorld", UniformType::Mat4, offsetof(HeatmapUniforms, ViewToWorld) },
			{ "ProjOffset", UniformType::Vec2, offsetof(HeatmapUniforms, ProjOffset) },
			{ "SceneScale", UniformType::Vec2, offsetof(HeatmapUniforms, SceneScale) },
			{ "SceneOffset", UniformType::Vec2, offsetof(HeatmapUniforms, SceneOffset) },
			{ "heatPad3", UniformType::Float, offsetof(HeatmapUniforms, heatPad3) },
			{ "heatPad4", UniformType::Float, offsetof(HeatmapUniforms, heatPad4) },
		};
	}
};

// std140 guard rails for HeatmapUniforms: the C++ layout IS the GLSL layout
// (UniformBlockDecl::Create, declaration order, no explicit offsets).
static_assert(offsetof(HeatmapUniforms, ViewToWorld) == 80,
	"HeatmapUniforms::ViewToWorld must start at 80 for std140");
static_assert(offsetof(HeatmapUniforms, ProjOffset) == 144,
	"HeatmapUniforms::ProjOffset must start at 144 for std140");
static_assert(offsetof(HeatmapUniforms, SceneScale) == 152,
	"HeatmapUniforms::SceneScale must start at 152 for std140");
static_assert(offsetof(HeatmapUniforms, SceneOffset) == 160,
	"HeatmapUniforms::SceneOffset must start at 160 for std140");
static_assert(sizeof(HeatmapUniforms) == 176,
	"HeatmapUniforms must be 176 bytes; pad to a 16-byte row");

// [BB] Where the fighting happened, painted on the floor.
//
// A postprocess pass rather than a term in the scene shader. The scene-shader
// route would let this tint the LIGHT rather than paint over the frame, at the
// price of four coordinated edits inside the Vulkan backend where missing the
// descriptor pool size fails silently. This touches no backend file at all.
class PPHeatmap
{
public:
	void Render(PPRenderState *renderstate, int sceneWidth, int sceneHeight);

	void SetHeat(const HeatmapUniforms &u) { uniforms = u; active = true; }
	void ClearHeat() { active = false; }

	// The grid itself, re-uploaded only when it changes. Deaths are rare, so
	// most frames this costs nothing beyond the sample.
	void SetGrid(int res, std::shared_ptr<void> intensity, std::shared_ptr<void> height)
	{
		Intensity = { res, res, PixelFormat::R32f, intensity };
		Height = { res, res, PixelFormat::R32f, height };
		Intensity.ResetBackend();
		Height.ResetBackend();
		haveGrid = true;
	}

	bool HasGrid() const { return haveGrid; }

private:
	HeatmapUniforms uniforms = {};
	bool active = false;
	bool haveGrid = false;

	PPTexture Intensity;
	PPTexture Height;

	PPShader Heat = { "shaders/pp/heatmap.fp", "", HeatmapUniforms::Desc() };
	// MSAA variant (texelFetch on sampler2DMS), picked in Render() when
	// gl_multisample > 1 -- a plain sampler2D reads multisampled depth as 0.
	PPShader HeatMS = { "shaders/pp/heatmap.fp", "#define MULTISAMPLE\n", HeatmapUniforms::Desc() };
};

/////////////////////////////////////////////////////////////////////////////

// [HEATREFRACTION] HEAT SHIMMER ("Engine docs/FLAME_ENGINE_PLAN.md" F2): hot air bends
// the image behind it. Two passes, see shaders/pp/heatoffset.fp and heatwarp.fp.
//
// One heat source for one eye. Positions are RELATIVE TO THAT EYE, in world axes
// (GL: y up) and map units -- not view space, whose pixel stretch would squash the
// source -- and ViewToWorld is that eye's, so the uniforms are per eye. Filled by
// SetupHeatSources (hw_drawinfo.cpp); SceneScale/SceneOffset are set in Render.
struct HeatOffsetUniforms
{
	FVector3 SourceStart;
	float RadiusStart;
	FVector3 SourceEnd;
	float RadiusEnd;
	FVector2 TanHalfFov;      // 1 / projection m[0], m[5], as the beam pass
	FVector2 ProjOffset;      // projection m[8], m[9]: an asymmetric (headset) eye
	FVector2 SceneScale;
	FVector2 SceneOffset;
	float Bend;               // radians per map unit of hot air: strength x fade x scale x BEND_PER_UNIT
	float NoiseScale;         // noise cells per map unit
	float NoiseRise;          // map units per second
	float NoiseTime;          // level seconds: pauses with the game
	float LinearizeDepthA;
	float LinearizeDepthB;
	float HeatPad0;
	float HeatPad1;
	float ViewToWorld[16];    // plain floats: VSMatrix is not visible in this header

	//   SourceStart 0   RadiusStart 12   SourceEnd 16   RadiusEnd 28
	//   TanHalfFov 32   ProjOffset 40    SceneScale 48  SceneOffset 56
	//   Bend 64  NoiseScale 68  NoiseRise 72  NoiseTime 76
	//   LinearizeDepthA 80  LinearizeDepthB 84  HeatPad0 88  HeatPad1 92
	//   ViewToWorld 96 -> block ends 160 (the beam's block is 192)
	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "SourceStart", UniformType::Vec3, offsetof(HeatOffsetUniforms, SourceStart) },
			{ "RadiusStart", UniformType::Float, offsetof(HeatOffsetUniforms, RadiusStart) },
			{ "SourceEnd", UniformType::Vec3, offsetof(HeatOffsetUniforms, SourceEnd) },
			{ "RadiusEnd", UniformType::Float, offsetof(HeatOffsetUniforms, RadiusEnd) },
			{ "TanHalfFov", UniformType::Vec2, offsetof(HeatOffsetUniforms, TanHalfFov) },
			{ "ProjOffset", UniformType::Vec2, offsetof(HeatOffsetUniforms, ProjOffset) },
			{ "SceneScale", UniformType::Vec2, offsetof(HeatOffsetUniforms, SceneScale) },
			{ "SceneOffset", UniformType::Vec2, offsetof(HeatOffsetUniforms, SceneOffset) },
			{ "Bend", UniformType::Float, offsetof(HeatOffsetUniforms, Bend) },
			{ "NoiseScale", UniformType::Float, offsetof(HeatOffsetUniforms, NoiseScale) },
			{ "NoiseRise", UniformType::Float, offsetof(HeatOffsetUniforms, NoiseRise) },
			{ "NoiseTime", UniformType::Float, offsetof(HeatOffsetUniforms, NoiseTime) },
			{ "LinearizeDepthA", UniformType::Float, offsetof(HeatOffsetUniforms, LinearizeDepthA) },
			{ "LinearizeDepthB", UniformType::Float, offsetof(HeatOffsetUniforms, LinearizeDepthB) },
			{ "HeatPad0", UniformType::Float, offsetof(HeatOffsetUniforms, HeatPad0) },
			{ "HeatPad1", UniformType::Float, offsetof(HeatOffsetUniforms, HeatPad1) },
			{ "ViewToWorld", UniformType::Mat4, offsetof(HeatOffsetUniforms, ViewToWorld) },
		};
	}
};

// std140 guard rails: UniformBlockDecl::Create emits the fields in declaration order
// with no explicit offsets, so the C++ layout IS the GLSL layout.
static_assert(offsetof(HeatOffsetUniforms, SourceEnd) == 16, "HeatOffsetUniforms::SourceEnd must start at 16 for std140");
static_assert(offsetof(HeatOffsetUniforms, TanHalfFov) == 32, "HeatOffsetUniforms::TanHalfFov must start at 32 for std140");
static_assert(offsetof(HeatOffsetUniforms, Bend) == 64, "HeatOffsetUniforms::Bend must start at 64 for std140");
static_assert(offsetof(HeatOffsetUniforms, LinearizeDepthA) == 80, "HeatOffsetUniforms::LinearizeDepthA must start at 80 for std140");
static_assert(offsetof(HeatOffsetUniforms, ViewToWorld) == 96, "HeatOffsetUniforms::ViewToWorld must start at 96 for std140");
static_assert(sizeof(HeatOffsetUniforms) == 160, "HeatOffsetUniforms must be 160 bytes; pad to a 16-byte row");

// The bend pass: nothing per eye (the offsets already are, and depth is read from
// the eye's own layer), so one set.
struct HeatWarpUniforms
{
	FVector2 SceneScale;
	FVector2 SceneOffset;
	float LinearizeDepthA;
	float LinearizeDepthB;
	float MaxShift;           // scene UV units
	float DepthMargin;        // map units

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "SceneScale", UniformType::Vec2, offsetof(HeatWarpUniforms, SceneScale) },
			{ "SceneOffset", UniformType::Vec2, offsetof(HeatWarpUniforms, SceneOffset) },
			{ "LinearizeDepthA", UniformType::Float, offsetof(HeatWarpUniforms, LinearizeDepthA) },
			{ "LinearizeDepthB", UniformType::Float, offsetof(HeatWarpUniforms, LinearizeDepthB) },
			{ "MaxShift", UniformType::Float, offsetof(HeatWarpUniforms, MaxShift) },
			{ "DepthMargin", UniformType::Float, offsetof(HeatWarpUniforms, DepthMargin) },
		};
	}
};

static_assert(offsetof(HeatWarpUniforms, LinearizeDepthA) == 16, "HeatWarpUniforms::LinearizeDepthA must start at 16 for std140");
static_assert(sizeof(HeatWarpUniforms) == 32, "HeatWarpUniforms must be 32 bytes");

// Pass1 runs it after the volumetric beam and the heatmap, where the smoke volume
// (#13) will also go, and before bloom: the HDR image bends before it glows, so the
// glows bend with it.
//
// SKIPPED, NOT ZERO STRENGTH. With r_heatrefraction off (the default) or no source
// published for the eye, Render returns before it pushes a group, allocates the
// offset texture or draws, so the frame is exactly the frame without this pass.
//
// PER EYE (review S8). Under a multiview scene the second eye post-processes
// without a scene of its own, and anything filled from HWDrawInfo::VPUniforms would
// carry the first eye's view into it. So the renderer publishes one set of sources
// per eye of a multiview scene -- set 0 for eye 0, set 1 for eye 1 -- or one set
// when each eye draws its own scene, and hw_entrypoint.cpp says which eye is being
// post-processed (SetEye) before each PostProcessScene. The layered post path draws
// per eye too (every PP pass is keyed Layers 1, ViewMask 0 and writes the current
// eye's layer), so the eye index covers it.
//
// BOTH EYES OF A MULTIVIEW SCENE GET THE SAME SOURCES. Layered post-processing shares
// one pair of pipeline images between the eyes, so both must run exactly the same
// passes: a source visible to either eye is published to both sets
// (SetupHeatSources), and the two sets always have equal counts.
class PPHeatRefraction
{
public:
	void Render(PPRenderState *renderstate, int sceneWidth, int sceneHeight);

	void ClearSources() { counts[0] = counts[1] = 0; eyeSets = 0; }
	void SetEyeSets(int sets) { eyeSets = sets < 0 ? 0 : (sets > 2 ? 2 : sets); }
	bool AddSource(int eyeSet, const HeatOffsetUniforms &u)
	{
		if (eyeSet < 0 || eyeSet > 1 || counts[eyeSet] >= MAX_SOURCES) return false;
		sources[eyeSet][counts[eyeSet]++] = u;
		return true;
	}
	void SetEye(int eye) { currentEye = eye; }

	// FLevelLocals::MAX_HEAT_SOURCES level slots plus the r_heatrefraction_test source.
	// hw_drawinfo.cpp static_asserts that relation, so a mismatch is a compile error
	// rather than a source that silently never draws.
	static const int MAX_SOURCES = 65;

	// Radians of bend per map unit of hot air at strength 1, per unit of noise
	// gradient: strength 1 through 64 units of air shifts the image behind by about
	// three pixels at a headset's field of view and resolution.
	static constexpr float BEND_PER_UNIT = 1.0e-4f;
	// The bend pass caps a pixel's summed shift here (scene UV units, ~3% of the view)
	// and ignores depth differences smaller than DEPTH_MARGIN map units.
	static constexpr float MAX_SHIFT = 0.03f;
	static constexpr float DEPTH_MARGIN = 2.0f;

private:
	void UpdateTexture(int sceneWidth, int sceneHeight);

	HeatOffsetUniforms sources[2][MAX_SOURCES] = {};
	int counts[2] = {};
	int eyeSets = 0;
	int currentEye = 0;

	// Half the scene's size, like bloom's first level: the bend is smooth, and the
	// bend pass's full-resolution depth tests keep the edges.
	PPTexture OffsetTexture;
	PPViewport OffsetViewport;
	int lastWidth = 0;
	int lastHeight = 0;

	PPShader OffsetShader = { "shaders/pp/heatoffset.fp", "", HeatOffsetUniforms::Desc() };
	PPShader OffsetShaderMS = { "shaders/pp/heatoffset.fp", "#define MULTISAMPLE\n", HeatOffsetUniforms::Desc() };
	PPShader WarpShader = { "shaders/pp/heatwarp.fp", "", HeatWarpUniforms::Desc() };
	PPShader WarpShaderMS = { "shaders/pp/heatwarp.fp", "#define MULTISAMPLE\n", HeatWarpUniforms::Desc() };
};

/////////////////////////////////////////////////////////////////////////////

// [SMOKEVOLUME] THE SMOKE VOLUME'S DRAWING ("Engine docs/SMOKE_VOLUME_PLAN.md" 13c): a raymarch per eye
// through the volume the compute step simulates (vk_smokevolume.h). Its shaders are shaders/pp/
// smokedepth.fp, smokemarch.fp, smokeblur.fp and smokecomposite.fp.
//
// The scene depth inside the scene viewport, read the way the heat and beam passes read it: by the depth
// downsample (smokedepth.fp) and by the composite's depth-aware upsample (smokecomposite.fp).
struct SmokeDepthUniforms
{
	FVector2 SceneScale;
	FVector2 SceneOffset;
	float LinearizeDepthA;
	float LinearizeDepthB;
	float DepthPad0;
	float DepthPad1;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "SceneScale", UniformType::Vec2, offsetof(SmokeDepthUniforms, SceneScale) },
			{ "SceneOffset", UniformType::Vec2, offsetof(SmokeDepthUniforms, SceneOffset) },
			{ "LinearizeDepthA", UniformType::Float, offsetof(SmokeDepthUniforms, LinearizeDepthA) },
			{ "LinearizeDepthB", UniformType::Float, offsetof(SmokeDepthUniforms, LinearizeDepthB) },
			{ "DepthPad0", UniformType::Float, offsetof(SmokeDepthUniforms, DepthPad0) },
			{ "DepthPad1", UniformType::Float, offsetof(SmokeDepthUniforms, DepthPad1) },
		};
	}
};

// std140 guard rails: UniformBlockDecl::Create emits the fields in declaration order with no explicit
// offsets, so the C++ layout IS the GLSL layout.
static_assert(offsetof(SmokeDepthUniforms, LinearizeDepthA) == 16, "SmokeDepthUniforms::LinearizeDepthA must start at 16 for std140");
static_assert(sizeof(SmokeDepthUniforms) == 32, "SmokeDepthUniforms must be 32 bytes");

// One eye's march (smokemarch.fp). Positions are RELATIVE TO THAT EYE in GL world axes (map x, map z,
// map y) and map units, as the heat pass's are. GridSize and TileCount are in the volume's texel axes,
// Doom's x, y, z. Filled by SetupSmokeVolume (hw_drawinfo.cpp).
struct SmokeMarchUniforms
{
	float ViewToWorld[16];    // plain floats: VSMatrix is not visible in this header
	FVector2 TanHalfFov;      // 1 / projection m[0], m[5], as the beam pass
	FVector2 ProjOffset;      // projection m[8], m[9]: an asymmetric (headset) eye
	FVector3 BoxMin;          // the grid's minimum corner, eye-relative GL axes, map units
	float CellSize;           // map units
	FVector3 GridSize;        // cells per axis, texel axes
	float TicFrac;            // where the frame sits between the last two simulation steps, 0..1
	FVector3 TileCount;       // tiles per axis (GridSize / SMOKE_TILE_CELLS), texel axes
	int StepCount;            // the most samples a ray takes (r_smoke_steps)
	FVector3 LightColor;      // [13d] the look's tint: the colour the light the smoke scatters takes (x the light grid)
	float Extinction;         // per map unit at density 1: absorption x SMOKE_EXTINCTION_PER_MAP_UNIT x r_smoke_density_scale
	float MinStep;            // map units: no two samples closer than this
	float SliceHeight;        // r_smoke_debugslice's level plane, eye-relative GL y, map units
	int DebugSlice;           // r_smoke_debugslice
	float MarchPad0;

	//   ViewToWorld 0   TanHalfFov 64   ProjOffset 72
	//   BoxMin 80   CellSize 92   GridSize 96   TicFrac 108   TileCount 112   StepCount 124
	//   LightColor 128   Extinction 140   MinStep 144   SliceHeight 148   DebugSlice 152   MarchPad0 156
	//   -> block ends 160 (the beam's is 192)
	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "ViewToWorld", UniformType::Mat4, offsetof(SmokeMarchUniforms, ViewToWorld) },
			{ "TanHalfFov", UniformType::Vec2, offsetof(SmokeMarchUniforms, TanHalfFov) },
			{ "ProjOffset", UniformType::Vec2, offsetof(SmokeMarchUniforms, ProjOffset) },
			{ "BoxMin", UniformType::Vec3, offsetof(SmokeMarchUniforms, BoxMin) },
			{ "CellSize", UniformType::Float, offsetof(SmokeMarchUniforms, CellSize) },
			{ "GridSize", UniformType::Vec3, offsetof(SmokeMarchUniforms, GridSize) },
			{ "TicFrac", UniformType::Float, offsetof(SmokeMarchUniforms, TicFrac) },
			{ "TileCount", UniformType::Vec3, offsetof(SmokeMarchUniforms, TileCount) },
			{ "StepCount", UniformType::Int, offsetof(SmokeMarchUniforms, StepCount) },
			{ "LightColor", UniformType::Vec3, offsetof(SmokeMarchUniforms, LightColor) },
			{ "Extinction", UniformType::Float, offsetof(SmokeMarchUniforms, Extinction) },
			{ "MinStep", UniformType::Float, offsetof(SmokeMarchUniforms, MinStep) },
			{ "SliceHeight", UniformType::Float, offsetof(SmokeMarchUniforms, SliceHeight) },
			{ "DebugSlice", UniformType::Int, offsetof(SmokeMarchUniforms, DebugSlice) },
			{ "MarchPad0", UniformType::Float, offsetof(SmokeMarchUniforms, MarchPad0) },
		};
	}
};

static_assert(offsetof(SmokeMarchUniforms, TanHalfFov) == 64, "SmokeMarchUniforms::TanHalfFov must start at 64 for std140");
static_assert(offsetof(SmokeMarchUniforms, BoxMin) == 80, "SmokeMarchUniforms::BoxMin must start at 80 for std140");
static_assert(offsetof(SmokeMarchUniforms, GridSize) == 96, "SmokeMarchUniforms::GridSize must start at 96 for std140");
static_assert(offsetof(SmokeMarchUniforms, TileCount) == 112, "SmokeMarchUniforms::TileCount must start at 112 for std140");
static_assert(offsetof(SmokeMarchUniforms, LightColor) == 128, "SmokeMarchUniforms::LightColor must start at 128 for std140");
static_assert(offsetof(SmokeMarchUniforms, MinStep) == 144, "SmokeMarchUniforms::MinStep must start at 144 for std140");
static_assert(sizeof(SmokeMarchUniforms) == 160, "SmokeMarchUniforms must be 160 bytes; pad to a 16-byte row");

// [13e] The transmittance curve and the beam scatter passes (smokemarch.fp with SMOKE_TRANSMITTANCE_CURVE or
// SMOKE_BEAM_SCATTER): the march's own uniforms, member for member at the same offsets (both walk the same ray through
// the same volume), then the beams'. Filled from the eye's march set by PPSmokeVolume::Render.
struct SmokeBeamScatterUniforms
{
	float ViewToWorld[16];
	FVector2 TanHalfFov;
	FVector2 ProjOffset;
	FVector3 BoxMin;
	float CellSize;
	FVector3 GridSize;
	float TicFrac;
	FVector3 TileCount;
	int StepCount;
	FVector3 LightColor;
	float Extinction;
	float MinStep;
	float SliceHeight;
	int DebugSlice;
	float MarchPad0;
	int BeamCount;            // beams in the beam list image (PPExternalImage::SmokeBeams)
	float BeamScatter;        // scatter: the look's scatter (SetSmokeLook), 0..1; 0 when r_smoke_beams is off (the pass writes nothing)
	float NearBeamsOnly;      // curve: 1 = only texels a listed beam's glow can reach (no cone reads the curve this eye); 0 = all
	float BeamPad1;

	//   the march's 0..160, then BeamCount 160   BeamScatter 164   NearBeamsOnly 168   BeamPad1 172   -> block ends 176
	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "ViewToWorld", UniformType::Mat4, offsetof(SmokeBeamScatterUniforms, ViewToWorld) },
			{ "TanHalfFov", UniformType::Vec2, offsetof(SmokeBeamScatterUniforms, TanHalfFov) },
			{ "ProjOffset", UniformType::Vec2, offsetof(SmokeBeamScatterUniforms, ProjOffset) },
			{ "BoxMin", UniformType::Vec3, offsetof(SmokeBeamScatterUniforms, BoxMin) },
			{ "CellSize", UniformType::Float, offsetof(SmokeBeamScatterUniforms, CellSize) },
			{ "GridSize", UniformType::Vec3, offsetof(SmokeBeamScatterUniforms, GridSize) },
			{ "TicFrac", UniformType::Float, offsetof(SmokeBeamScatterUniforms, TicFrac) },
			{ "TileCount", UniformType::Vec3, offsetof(SmokeBeamScatterUniforms, TileCount) },
			{ "StepCount", UniformType::Int, offsetof(SmokeBeamScatterUniforms, StepCount) },
			{ "LightColor", UniformType::Vec3, offsetof(SmokeBeamScatterUniforms, LightColor) },
			{ "Extinction", UniformType::Float, offsetof(SmokeBeamScatterUniforms, Extinction) },
			{ "MinStep", UniformType::Float, offsetof(SmokeBeamScatterUniforms, MinStep) },
			{ "SliceHeight", UniformType::Float, offsetof(SmokeBeamScatterUniforms, SliceHeight) },
			{ "DebugSlice", UniformType::Int, offsetof(SmokeBeamScatterUniforms, DebugSlice) },
			{ "MarchPad0", UniformType::Float, offsetof(SmokeBeamScatterUniforms, MarchPad0) },
			{ "BeamCount", UniformType::Int, offsetof(SmokeBeamScatterUniforms, BeamCount) },
			{ "BeamScatter", UniformType::Float, offsetof(SmokeBeamScatterUniforms, BeamScatter) },
			{ "NearBeamsOnly", UniformType::Float, offsetof(SmokeBeamScatterUniforms, NearBeamsOnly) },
			{ "BeamPad1", UniformType::Float, offsetof(SmokeBeamScatterUniforms, BeamPad1) },
		};
	}
};

// The march's part must be SmokeMarchUniforms byte for byte: Render copies it in whole.
static_assert(offsetof(SmokeBeamScatterUniforms, TanHalfFov) == offsetof(SmokeMarchUniforms, TanHalfFov) &&
	offsetof(SmokeBeamScatterUniforms, BoxMin) == offsetof(SmokeMarchUniforms, BoxMin) &&
	offsetof(SmokeBeamScatterUniforms, GridSize) == offsetof(SmokeMarchUniforms, GridSize) &&
	offsetof(SmokeBeamScatterUniforms, TileCount) == offsetof(SmokeMarchUniforms, TileCount) &&
	offsetof(SmokeBeamScatterUniforms, LightColor) == offsetof(SmokeMarchUniforms, LightColor) &&
	offsetof(SmokeBeamScatterUniforms, MinStep) == offsetof(SmokeMarchUniforms, MinStep) &&
	offsetof(SmokeBeamScatterUniforms, MarchPad0) == offsetof(SmokeMarchUniforms, MarchPad0),
	"SmokeBeamScatterUniforms must start with SmokeMarchUniforms' layout");
static_assert(offsetof(SmokeBeamScatterUniforms, BeamCount) == 160, "SmokeBeamScatterUniforms::BeamCount must start at 160");
static_assert(sizeof(SmokeBeamScatterUniforms) == 176, "SmokeBeamScatterUniforms must be 176 bytes; pad to a 16-byte row");

// [EMISSIVEVOLUMES] The transmittance curve's near-volumes variant (smokemarch.fp SMOKE_TRANSMITTANCE_CURVE with
// SMOKE_CURVE_NEAR_VOLUMES; "Engine docs/EMISSIVE_VOLUMES_15_IMPL_NOTES.md"): SmokeBeamScatterUniforms member for member at the
// same offsets (Render copies it in whole), then the emissive volume list's origin relative to this eye and its count
// (EmissiveVolumeUniforms' ListOrigin and VolumeCount), so with no cone the curve is marched only where a beam or a volume can
// read it.
struct SmokeCurveNearVolumesUniforms
{
	float ViewToWorld[16];
	FVector2 TanHalfFov;
	FVector2 ProjOffset;
	FVector3 BoxMin;
	float CellSize;
	FVector3 GridSize;
	float TicFrac;
	FVector3 TileCount;
	int StepCount;
	FVector3 LightColor;
	float Extinction;
	float MinStep;
	float SliceHeight;
	int DebugSlice;
	float MarchPad0;
	int BeamCount;
	float BeamScatter;
	float NearBeamsOnly;
	float BeamPad1;
	FVector3 VolumeOrigin;    // the emissive volume list's origin minus this eye, GL axes, map units
	int VolumeCount;          // volumes in the list image (PPExternalImage::EmissiveVolumeList)

	//   SmokeBeamScatterUniforms' 0..176, then VolumeOrigin 176   VolumeCount 188   -> block ends 192
	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "ViewToWorld", UniformType::Mat4, offsetof(SmokeCurveNearVolumesUniforms, ViewToWorld) },
			{ "TanHalfFov", UniformType::Vec2, offsetof(SmokeCurveNearVolumesUniforms, TanHalfFov) },
			{ "ProjOffset", UniformType::Vec2, offsetof(SmokeCurveNearVolumesUniforms, ProjOffset) },
			{ "BoxMin", UniformType::Vec3, offsetof(SmokeCurveNearVolumesUniforms, BoxMin) },
			{ "CellSize", UniformType::Float, offsetof(SmokeCurveNearVolumesUniforms, CellSize) },
			{ "GridSize", UniformType::Vec3, offsetof(SmokeCurveNearVolumesUniforms, GridSize) },
			{ "TicFrac", UniformType::Float, offsetof(SmokeCurveNearVolumesUniforms, TicFrac) },
			{ "TileCount", UniformType::Vec3, offsetof(SmokeCurveNearVolumesUniforms, TileCount) },
			{ "StepCount", UniformType::Int, offsetof(SmokeCurveNearVolumesUniforms, StepCount) },
			{ "LightColor", UniformType::Vec3, offsetof(SmokeCurveNearVolumesUniforms, LightColor) },
			{ "Extinction", UniformType::Float, offsetof(SmokeCurveNearVolumesUniforms, Extinction) },
			{ "MinStep", UniformType::Float, offsetof(SmokeCurveNearVolumesUniforms, MinStep) },
			{ "SliceHeight", UniformType::Float, offsetof(SmokeCurveNearVolumesUniforms, SliceHeight) },
			{ "DebugSlice", UniformType::Int, offsetof(SmokeCurveNearVolumesUniforms, DebugSlice) },
			{ "MarchPad0", UniformType::Float, offsetof(SmokeCurveNearVolumesUniforms, MarchPad0) },
			{ "BeamCount", UniformType::Int, offsetof(SmokeCurveNearVolumesUniforms, BeamCount) },
			{ "BeamScatter", UniformType::Float, offsetof(SmokeCurveNearVolumesUniforms, BeamScatter) },
			{ "NearBeamsOnly", UniformType::Float, offsetof(SmokeCurveNearVolumesUniforms, NearBeamsOnly) },
			{ "BeamPad1", UniformType::Float, offsetof(SmokeCurveNearVolumesUniforms, BeamPad1) },
			{ "VolumeOrigin", UniformType::Vec3, offsetof(SmokeCurveNearVolumesUniforms, VolumeOrigin) },
			{ "VolumeCount", UniformType::Int, offsetof(SmokeCurveNearVolumesUniforms, VolumeCount) },
		};
	}
};

static_assert(offsetof(SmokeCurveNearVolumesUniforms, TanHalfFov) == offsetof(SmokeBeamScatterUniforms, TanHalfFov) &&
	offsetof(SmokeCurveNearVolumesUniforms, BoxMin) == offsetof(SmokeBeamScatterUniforms, BoxMin) &&
	offsetof(SmokeCurveNearVolumesUniforms, TileCount) == offsetof(SmokeBeamScatterUniforms, TileCount) &&
	offsetof(SmokeCurveNearVolumesUniforms, MinStep) == offsetof(SmokeBeamScatterUniforms, MinStep) &&
	offsetof(SmokeCurveNearVolumesUniforms, BeamCount) == offsetof(SmokeBeamScatterUniforms, BeamCount) &&
	offsetof(SmokeCurveNearVolumesUniforms, BeamPad1) == offsetof(SmokeBeamScatterUniforms, BeamPad1),
	"SmokeCurveNearVolumesUniforms must start with SmokeBeamScatterUniforms' layout");
static_assert(offsetof(SmokeCurveNearVolumesUniforms, VolumeOrigin) == 176, "SmokeCurveNearVolumesUniforms::VolumeOrigin must start at 176");
static_assert(offsetof(SmokeCurveNearVolumesUniforms, VolumeCount) == 188, "SmokeCurveNearVolumesUniforms::VolumeCount must start at 188");
static_assert(sizeof(SmokeCurveNearVolumesUniforms) == 192, "SmokeCurveNearVolumesUniforms must be 192 bytes");

// [13e] The composite with beams (smokecomposite.fp with SMOKE_BEAMS): the depth uniforms the composite has always had,
// the eye's ray (to find where each beam passes this pixel), and the beams' scroll, as main.fp's BeamAirGlow reads it.
struct SmokeBeamCompositeUniforms
{
	FVector2 SceneScale;
	FVector2 SceneOffset;
	float LinearizeDepthA;
	float LinearizeDepthB;
	float DepthPad0;
	float DepthPad1;
	float ViewToWorld[16];    // the eye's march set's
	FVector2 TanHalfFov;
	FVector2 ProjOffset;
	FVector3 BoxMin;          // the grid's minimum corner, eye-relative GL axes: the beam list's positions are from it
	int BeamCount;
	float BeamScrollSpeed;    // FLevelLocals::BeamScrollSpeed (main.fp's uBeamFX.x)
	float BeamScrollDepth;    // FLevelLocals::BeamScrollDepth (uBeamFX.y)
	float BeamTimer;          // the scroll's clock, seconds (main.fp's timer)
	int BeamDepth;            // 1: a beam's glow keeps the haze behind it from dimming it (r_smoke_beams_depth)

	//   SceneScale 0  SceneOffset 8  LinearizeDepthA 16  B 20  DepthPad0 24  DepthPad1 28  ViewToWorld 32
	//   TanHalfFov 96  ProjOffset 104  BoxMin 112  BeamCount 124  BeamScrollSpeed 128  BeamScrollDepth 132
	//   BeamTimer 136  BeamDepth 140  -> block ends 144
	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "SceneScale", UniformType::Vec2, offsetof(SmokeBeamCompositeUniforms, SceneScale) },
			{ "SceneOffset", UniformType::Vec2, offsetof(SmokeBeamCompositeUniforms, SceneOffset) },
			{ "LinearizeDepthA", UniformType::Float, offsetof(SmokeBeamCompositeUniforms, LinearizeDepthA) },
			{ "LinearizeDepthB", UniformType::Float, offsetof(SmokeBeamCompositeUniforms, LinearizeDepthB) },
			{ "DepthPad0", UniformType::Float, offsetof(SmokeBeamCompositeUniforms, DepthPad0) },
			{ "DepthPad1", UniformType::Float, offsetof(SmokeBeamCompositeUniforms, DepthPad1) },
			{ "ViewToWorld", UniformType::Mat4, offsetof(SmokeBeamCompositeUniforms, ViewToWorld) },
			{ "TanHalfFov", UniformType::Vec2, offsetof(SmokeBeamCompositeUniforms, TanHalfFov) },
			{ "ProjOffset", UniformType::Vec2, offsetof(SmokeBeamCompositeUniforms, ProjOffset) },
			{ "BoxMin", UniformType::Vec3, offsetof(SmokeBeamCompositeUniforms, BoxMin) },
			{ "BeamCount", UniformType::Int, offsetof(SmokeBeamCompositeUniforms, BeamCount) },
			{ "BeamScrollSpeed", UniformType::Float, offsetof(SmokeBeamCompositeUniforms, BeamScrollSpeed) },
			{ "BeamScrollDepth", UniformType::Float, offsetof(SmokeBeamCompositeUniforms, BeamScrollDepth) },
			{ "BeamTimer", UniformType::Float, offsetof(SmokeBeamCompositeUniforms, BeamTimer) },
			{ "BeamDepth", UniformType::Int, offsetof(SmokeBeamCompositeUniforms, BeamDepth) },
		};
	}
};

static_assert(offsetof(SmokeBeamCompositeUniforms, LinearizeDepthA) == offsetof(SmokeDepthUniforms, LinearizeDepthA), "SmokeBeamCompositeUniforms must start with SmokeDepthUniforms' layout");
static_assert(offsetof(SmokeBeamCompositeUniforms, ViewToWorld) == 32, "SmokeBeamCompositeUniforms::ViewToWorld must start at 32 for std140");
static_assert(offsetof(SmokeBeamCompositeUniforms, TanHalfFov) == 96, "SmokeBeamCompositeUniforms::TanHalfFov must start at 96 for std140");
static_assert(offsetof(SmokeBeamCompositeUniforms, BoxMin) == 112, "SmokeBeamCompositeUniforms::BoxMin must start at 112 for std140");
static_assert(offsetof(SmokeBeamCompositeUniforms, BeamScrollSpeed) == 128, "SmokeBeamCompositeUniforms::BeamScrollSpeed must start at 128 for std140");
static_assert(sizeof(SmokeBeamCompositeUniforms) == 144, "SmokeBeamCompositeUniforms must be 144 bytes; pad to a 16-byte row");

// [13e] What the renderer publishes each frame for beams and cones in the smoke (SetupSmokeVolume, hw_drawinfo.cpp).
// The same for both eyes. The default -- no beams, every switch off -- draws exactly 13d's smoke.
struct PPSmokeBeamSettings
{
	int BeamCount = 0;          // beams in the backend's beam list image (SmokeVolumeBackendStatus::BeamCount), 0..16
	bool Scatter = false;       // r_smoke_beams: beams scatter light in the smoke
	bool Depth = false;         // r_smoke_beams_depth: the haze behind a beam does not dim it
	bool Cones = false;         // r_smoke_cones_depth: the haze in front of a volumetric beam cone dims it
	float LookScatter = 0.f;    // SetSmokeLook's scatter, 0..1
	float ScrollSpeed = 0.f;    // FLevelLocals::BeamScrollSpeed
	float ScrollDepth = 0.f;    // FLevelLocals::BeamScrollDepth
	float Timer = 0.f;          // the beams' scroll clock, seconds (SyncDrawnLines hands the drawn-line path the same)
};

// SKIPPED, NOT ZERO. Render returns on its first line unless the renderer published a march for this
// eye (SetupSmokeVolume, hw_drawinfo.cpp: Vulkan, r_smoke, the volume allocated, and the CPU side's bound
// saying visible smoke may exist). With no smoke, or smoke off, the frame is exactly the frame without
// this pass: no group, no texture, no draw. Even with smoke present, a pixel with no smoke near it is
// discarded by the composite, never blended at zero, so everything outside the smoke (the lasers among
// it) keeps its look bit for bit.
//
// WHERE (Pass1): after beforebloom and before the volumetric beam, the heatmap, heat refraction and
// bloom. A flashlight cone's air glow goes on after the haze, so haze behind the cone never dims it;
// the image bends and blooms with its smoke.
//
// THE COMPOSITE ("Engine docs/EMISSIVE_BLOOM_PLAN.md", the contract for 13c/13e) is scene x T +
// inscatter, premultiplied, with the per-pixel transmittance T kept to the end. The light mask
// (PPLightMask) gets the same composite with LIGHT_MASK_CARRY, so its amounts dim by the same T. 13e's
// beam scatter must stay separable: its own term, not folded into this rgb.
//
// PER EYE (review S8), as heat refraction: one march set per eye of a multiview scene, one otherwise,
// and hw_entrypoint.cpp says which eye is being post-processed (SetEye). The grid box is placed around
// the viewer, so both sets are always published together and both eyes of a layered post path run the
// same passes. The composite writes the current pipeline image in place (no advance) either way.
//
// FOUR PASSES, all but the last at half the scene's resolution:
//   1. smokedepth.fp      linear depth, the nearest or farthest of each 2x2 in a checkerboard (R32F)
//   2. smokemarch.fp      the march: rgb light scattered toward the eye, a transmittance (RGBA16F)
//   3. smokeblur.fp       a separable 5-tap blur that keeps to its depth (two draws, ping-pong)
//   4. smokecomposite.fp  the depth-aware upsample and the premultiplied composite
//
// [13e] BEAMS AND CONES ("Engine docs/SMOKE_13E_IMPL_NOTES.md"). Only while this frame has beams in the smoke with
// r_smoke_beams or r_smoke_beams_depth on, or a volumetric beam cone with r_smoke_cones_depth on; otherwise every draw
// above is 13d's, program for program. Between 3 and 4, in group pp.smokebeams:
//   3a. smokemarch.fp SMOKE_TRANSMITTANCE_CURVE  the TRANSMITTANCE CURVE: where each ray's optical depth reaches 0, 1/3,
//       2/3 and all of it, so a pass can ask how much haze lies in front of any depth (RGBA16F, half resolution) -- over
//       every smoke texel when a cone reads it, otherwise only where a listed beam's glow can reach
//   3b. smokemarch.fp SMOKE_BEAM_SCATTER  (beams only) the light each beam scatters in the smoke, dimmed by the haze in
//       front of it (RGBA16F rgb, half resolution) -- its own texture, so it stays separable (EMISSIVE contract 3)
// and the composite and its light mask carry take their SMOKE_BEAMS variants: + the beam scatter, + each beam's own air
// glow times (the transmittance to the beam - the pixel's), which undoes the dimming by haze BEHIND the beam. Still
// (light, 1 - T) with the premultiplied blend: scene x T + inscatter. The volumetric beam pass reads the curve after
// this (TransmittanceReady).
class PPSmokeVolume
{
public:
	void Render(PPRenderState *renderstate, int sceneWidth, int sceneHeight);

	void ClearEyes() { eyeSets = 0; beams = PPSmokeBeamSettings(); }
	void SetEyeMarch(int eyeSet, const SmokeMarchUniforms &u)
	{
		if (eyeSet >= 0 && eyeSet < 2) marches[eyeSet] = u;
	}
	void SetEyeSets(int sets) { eyeSets = sets < 0 ? 0 : (sets > 2 ? 2 : sets); }
	void SetEye(int eye) { currentEye = eye; }
	// [13e] The frame's beams and switches (after SetEyeSets), and the beam count last published (the perf log's label).
	void SetBeams(const PPSmokeBeamSettings &settings) { beams = settings; }
	int PublishedBeamCount() const { return beams.BeamCount; }

	// [13e] For the volumetric beam pass, later in the same eye's Pass1: true once this eye's smoke drew its transmittance
	// curve AND r_smoke_cones_depth is on. Reset at the top of every Render, so an eye with no smoke says false. The
	// textures the cone pass reads it through: the blurred march (a = the whole ray's T), the depth each texel marched
	// to, and the curve -- all half resolution, sampled with the composite's own upsample.
	bool ConesDimmedByHaze() const { return transmittanceReady && beams.Cones; }
	// [EMISSIVEVOLUMES] For the emissive volumes, later in the same eye's Pass1: true once this eye's smoke drew its depth and its
	// transmittance curve (which it does whenever a volume is published for the eye). Reset at the top of every Render.
	bool TransmittanceReady() const { return transmittanceReady; }
	PPTexture *GetMarchTexture() { return &MarchTexture; }
	PPTexture *GetDepthTexture() { return &DepthTexture; }
	PPTexture *GetCurveTexture() { return &CurveTexture; }

private:
	void UpdateTextures(int sceneWidth, int sceneHeight);

	SmokeMarchUniforms marches[2] = {};
	int eyeSets = 0;
	int currentEye = 0;
	PPSmokeBeamSettings beams;			// [13e]
	bool transmittanceReady = false;	// [13e] this eye drew its transmittance curve

	// Half the scene's size, like bloom's first level and the heat offsets. Rewritten whole by every eye
	// before it is read, so the eyes can share them.
	PPTexture DepthTexture;
	PPTexture MarchTexture;
	PPTexture BlurTexture;
	PPTexture CurveTexture;		// [13e] RGBA16F: the distances where the ray's optical depth reaches 0, 1/3, 2/3, all (0 = no smoke)
	PPTexture BeamTexture;		// [13e] RGBA16F: rgb the light the beams scatter, a 1
	PPViewport HalfViewport;
	int lastWidth = 0;
	int lastHeight = 0;

	PPShader DepthShader = { "shaders/pp/smokedepth.fp", "", SmokeDepthUniforms::Desc() };
	PPShader DepthShaderMS = { "shaders/pp/smokedepth.fp", "#define MULTISAMPLE\n", SmokeDepthUniforms::Desc() };
	PPShader MarchShader = { "shaders/pp/smokemarch.fp", "", SmokeMarchUniforms::Desc() };
	PPShader BlurHorizontal = { "shaders/pp/smokeblur.fp", "#define BLUR_HORIZONTAL\n", {} };
	PPShader BlurVertical = { "shaders/pp/smokeblur.fp", "#define BLUR_VERTICAL\n", {} };
	PPShader CompositeShader = { "shaders/pp/smokecomposite.fp", "", SmokeDepthUniforms::Desc() };
	PPShader CompositeShaderMS = { "shaders/pp/smokecomposite.fp", "#define MULTISAMPLE\n", SmokeDepthUniforms::Desc() };
	PPShader MaskCarryShader = { "shaders/pp/smokecomposite.fp", "#define LIGHT_MASK_CARRY\n", SmokeDepthUniforms::Desc() };
	PPShader MaskCarryShaderMS = { "shaders/pp/smokecomposite.fp", "#define MULTISAMPLE\n#define LIGHT_MASK_CARRY\n", SmokeDepthUniforms::Desc() };
	// [13e] Beams and cones.
	PPShader CurveShader = { "shaders/pp/smokemarch.fp", "#define SMOKE_TRANSMITTANCE_CURVE\n", SmokeBeamScatterUniforms::Desc() };
	PPShader BeamScatterShader = { "shaders/pp/smokemarch.fp", "#define SMOKE_BEAM_SCATTER\n", SmokeBeamScatterUniforms::Desc() };
	PPShader CompositeBeamsShader = { "shaders/pp/smokecomposite.fp", "#define SMOKE_BEAMS\n", SmokeBeamCompositeUniforms::Desc() };
	PPShader CompositeBeamsShaderMS = { "shaders/pp/smokecomposite.fp", "#define MULTISAMPLE\n#define SMOKE_BEAMS\n", SmokeBeamCompositeUniforms::Desc() };
	PPShader MaskCarryBeamsShader = { "shaders/pp/smokecomposite.fp", "#define LIGHT_MASK_CARRY\n#define SMOKE_BEAMS\n", SmokeBeamCompositeUniforms::Desc() };
	PPShader MaskCarryBeamsShaderMS = { "shaders/pp/smokecomposite.fp", "#define MULTISAMPLE\n#define LIGHT_MASK_CARRY\n#define SMOKE_BEAMS\n", SmokeBeamCompositeUniforms::Desc() };
	// [EMISSIVEVOLUMES] The curve while an emissive volume is drawn this eye: marched only where a beam or a volume can read it
	// (with a cone, everywhere, as CurveShader), binding 8 the volume list.
	PPShader CurveNearVolumesShader = { "shaders/pp/smokemarch.fp", "#define SMOKE_TRANSMITTANCE_CURVE\n#define SMOKE_CURVE_NEAR_VOLUMES\n", SmokeCurveNearVolumesUniforms::Desc() };
};

/////////////////////////////////////////////////////////////////////////////

// [EMISSIVEVOLUMES] EMISSIVE VOLUMES' DRAWING ("Engine docs/VOLUMETRIC_FLASH_15_PLAN.md" 2d; "Engine docs/
// EMISSIVE_VOLUMES_15_IMPL_NOTES.md"): short-lived glowing gas volumes -- muzzle flashes, explosion cores -- raymarched per
// eye from the list the renderer uploads each frame (hw_emissivevolumeframe.h; vk_emissivevolumes.h).
//
// One eye's march (emissivevolume.fp). Positions are RELATIVE TO THAT EYE in GL world axes, as the smoke's. Filled by
// SetupEmissiveVolumes (hw_drawinfo.cpp).
struct EmissiveVolumeUniforms
{
	float ViewToWorld[16];    // plain floats: VSMatrix is not visible in this header
	FVector2 TanHalfFov;      // 1 / projection m[0], m[5]
	FVector2 ProjOffset;      // projection m[8], m[9]: an asymmetric (headset) eye
	FVector3 ListOrigin;      // the list's origin minus this eye, GL axes, map units: a volume's base is ListOrigin + its row
	int VolumeCount;          // volumes in the list image, 1..EMISSIVE_VOLUMES_DRAWN_MAX
	FVector2 RectMin;         // TexCoord 0..1: where on this eye's screen any volume's bounding sphere can be seen
	FVector2 RectMax;
	int StepCount;            // r_emissivevolumes_steps: steps across a volume's diameter
	float VolumePad0;
	float VolumePad1;
	float VolumePad2;

	//   ViewToWorld 0   TanHalfFov 64   ProjOffset 72   ListOrigin 80   VolumeCount 92   RectMin 96   RectMax 104
	//   StepCount 112   VolumePad0 116   VolumePad1 120   VolumePad2 124   -> block ends 128
	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "ViewToWorld", UniformType::Mat4, offsetof(EmissiveVolumeUniforms, ViewToWorld) },
			{ "TanHalfFov", UniformType::Vec2, offsetof(EmissiveVolumeUniforms, TanHalfFov) },
			{ "ProjOffset", UniformType::Vec2, offsetof(EmissiveVolumeUniforms, ProjOffset) },
			{ "ListOrigin", UniformType::Vec3, offsetof(EmissiveVolumeUniforms, ListOrigin) },
			{ "VolumeCount", UniformType::Int, offsetof(EmissiveVolumeUniforms, VolumeCount) },
			{ "RectMin", UniformType::Vec2, offsetof(EmissiveVolumeUniforms, RectMin) },
			{ "RectMax", UniformType::Vec2, offsetof(EmissiveVolumeUniforms, RectMax) },
			{ "StepCount", UniformType::Int, offsetof(EmissiveVolumeUniforms, StepCount) },
			{ "VolumePad0", UniformType::Float, offsetof(EmissiveVolumeUniforms, VolumePad0) },
			{ "VolumePad1", UniformType::Float, offsetof(EmissiveVolumeUniforms, VolumePad1) },
			{ "VolumePad2", UniformType::Float, offsetof(EmissiveVolumeUniforms, VolumePad2) },
		};
	}
};

static_assert(offsetof(EmissiveVolumeUniforms, TanHalfFov) == 64, "EmissiveVolumeUniforms::TanHalfFov must start at 64 for std140");
static_assert(offsetof(EmissiveVolumeUniforms, ListOrigin) == 80, "EmissiveVolumeUniforms::ListOrigin must start at 80 for std140");
static_assert(offsetof(EmissiveVolumeUniforms, VolumeCount) == 92, "EmissiveVolumeUniforms::VolumeCount must start at 92 for std140");
static_assert(offsetof(EmissiveVolumeUniforms, RectMin) == 96, "EmissiveVolumeUniforms::RectMin must start at 96 for std140");
static_assert(offsetof(EmissiveVolumeUniforms, StepCount) == 112, "EmissiveVolumeUniforms::StepCount must start at 112 for std140");
static_assert(sizeof(EmissiveVolumeUniforms) == 128, "EmissiveVolumeUniforms must be 128 bytes; pad to a 16-byte row");

// What the renderer publishes each frame for the drawing, the same for both eyes. The default draws nothing.
struct PPEmissiveVolumeSettings
{
	int Count = 0;            // volumes the backend's list image holds for this frame (EmissiveVolumeBackendStatus::Count)
	bool Absorbs = false;     // a drawn volume hides what is behind it: the light mask carry runs
	int Resolution = 2;       // r_emissivevolumes_resolution: 2 half, 1 full
};

// SKIPPED, NOT ZERO. Render returns on its first line unless the renderer published a march for this eye (SetupEmissiveVolumes:
// Vulkan, volumes in this frame's list, and the backend holding exactly that list). With no volume the frame is exactly the
// frame without this pass: no group, no texture, no draw. Even with volumes, a pixel no volume's bound reaches is discarded by
// the composite, never blended at zero, so everything outside them -- the lasers among it -- keeps its look bit for bit.
//
// WHERE (Pass1): right after the volumetric beam and before the heatmap -- exposure, beforebloom, smoke, volbeam, EMISSIVE
// VOLUMES, heatmap, heat refraction, bloom. After the smoke composite, so haze behind a volume never dims it (haze in front does,
// through the smoke's transmittance curve); before heat refraction, so its own shimmer bends it; before bloom, so it glows with
// the rest look. The light mask contract's order (volbeam, heatmap, heat refraction, bloom) is kept.
//
// PER EYE, as the smoke: one march set per eye of a multiview scene, one otherwise; hw_entrypoint.cpp says which eye (SetEye).
// Both eyes read the one list.
//
// FIVE PASSES, all but the composite and carry at the march resolution (half by default), in group pp.emissive:
//   1. smokedepth.fp      the depth to march to -- this eye's smoke DepthTexture when the smoke drew it at half resolution
//   2. emissivevolume.fp  the march (SMOKE_TRANSMITTANCE when the smoke drew its curve): rgb light, a transmittance (RGBA16F)
//   3. smokeblur.fp       the blur that keeps to its depth, across then down (it keeps clear texels exactly clear)
//   4. smokecomposite.fp  the depth-aware upsample onto the current image, premultiplied: scene x T + light
//   5. smokecomposite.fp  LIGHT_MASK_CARRY onto the light mask, only while a drawn volume absorbs and the mask came in
// The reused passes are new PPShader instances of the same lumps and defines as the smoke's, so they are the smoke's programs.
class PPEmissiveVolumes
{
public:
	void Render(PPRenderState *renderstate, int sceneWidth, int sceneHeight);

	void ClearEyes() { eyeSets = 0; settings = PPEmissiveVolumeSettings(); }
	void SetEyeMarch(int eyeSet, const EmissiveVolumeUniforms &u)
	{
		if (eyeSet >= 0 && eyeSet < 2) marches[eyeSet] = u;
	}
	void SetEyeSets(int sets) { eyeSets = sets < 0 ? 0 : (sets > 2 ? 2 : sets); }
	void SetEye(int eye) { currentEye = eye; }
	void SetSettings(const PPEmissiveVolumeSettings &s) { settings = s; }

	// Whether Render will draw this eye (its own skip test): the smoke pass asks, to draw the curve the march reads.
	bool HasVolumes() const { return eyeSets > 0 && settings.Count > 0; }
	int PublishedCount() const { return eyeSets > 0 ? settings.Count : 0; }
	// The current eye's list origin and count, for the smoke's near-volumes curve.
	void FillCurveUniforms(SmokeCurveNearVolumesUniforms &u) const
	{
		const EmissiveVolumeUniforms &m = marches[CurrentSet()];
		u.VolumeOrigin = m.ListOrigin;
		u.VolumeCount = m.VolumeCount;
	}

private:
	int CurrentSet() const { return (eyeSets >= 2 && currentEye == 1) ? 1 : 0; }
	void UpdateTextures(int sceneWidth, int sceneHeight, int resolution);

	EmissiveVolumeUniforms marches[2] = {};
	int eyeSets = 0;
	int currentEye = 0;
	PPEmissiveVolumeSettings settings;

	// At the march resolution, rewritten whole by every eye before it is read, so the eyes can share them. A PPTexture takes no
	// memory until a draw first uses it.
	PPTexture DepthTexture;
	PPTexture MarchTexture;
	PPTexture BlurTexture;
	PPViewport MarchViewport;
	int lastWidth = 0;
	int lastHeight = 0;
	int lastResolution = 0;

	PPShader DepthShader = { "shaders/pp/smokedepth.fp", "", SmokeDepthUniforms::Desc() };
	PPShader DepthShaderMS = { "shaders/pp/smokedepth.fp", "#define MULTISAMPLE\n", SmokeDepthUniforms::Desc() };
	PPShader MarchShader = { "shaders/pp/emissivevolume.fp", "", EmissiveVolumeUniforms::Desc() };
	PPShader MarchSmokeShader = { "shaders/pp/emissivevolume.fp", "#define SMOKE_TRANSMITTANCE\n", EmissiveVolumeUniforms::Desc() };
	PPShader BlurHorizontal = { "shaders/pp/smokeblur.fp", "#define BLUR_HORIZONTAL\n", {} };
	PPShader BlurVertical = { "shaders/pp/smokeblur.fp", "#define BLUR_VERTICAL\n", {} };
	PPShader CompositeShader = { "shaders/pp/smokecomposite.fp", "", SmokeDepthUniforms::Desc() };
	PPShader CompositeShaderMS = { "shaders/pp/smokecomposite.fp", "#define MULTISAMPLE\n", SmokeDepthUniforms::Desc() };
	PPShader MaskCarryShader = { "shaders/pp/smokecomposite.fp", "#define LIGHT_MASK_CARRY\n", SmokeDepthUniforms::Desc() };
	PPShader MaskCarryShaderMS = { "shaders/pp/smokecomposite.fp", "#define MULTISAMPLE\n#define LIGHT_MASK_CARRY\n", SmokeDepthUniforms::Desc() };
};


/////////////////////////////////////////////////////////////////////////////

// [BB] The combine pass doubles as the downscale step, so these must be set
// neutral (tint 1,1,1 and no fringing) during downscaling and only carry real
// values on the final combine -- otherwise the tint would be applied once per
// level and compound.
struct BloomCombineUniforms
{
	FVector3 Tint;
	float Chromatic;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "Tint", UniformType::Vec3, offsetof(BloomCombineUniforms, Tint) },
			{ "Chromatic", UniformType::Float, offsetof(BloomCombineUniforms, Chromatic) }
		};
	}
};

enum { NumBloomLevels = 4 };

class PPBlurLevel
{
public:
	PPViewport Viewport;
	PPTexture VTexture;
	PPTexture HTexture;
};

// [BLOOMOVERRIDE] One frame's copy of the level's bloom override
// (FLevelLocals::BloomOverride*, set by LevelLocals.SetBloomOverride; E1 in
// "Engine docs/REVIEW_BLOOM_PLAN.md"). hw_postprocess is common code and cannot see
// the level, so the game side copies it in (SyncBloomOverride, hw_drawinfo.cpp) and
// PPBloom eases, pulses and blends it. The natives have already clamped every value.
struct PPBloomOverride
{
	float Spread = 1.4f;      // blur theta, gl_bloom_amount's meaning
	float Threshold = 1.0f;
	float Knee = 0.5f;
	float TintR = 1.0f;
	float TintG = 1.0f;
	float TintB = 1.0f;
	float Mix = 0.0f;         // 0 = the cvars, 1 = these values
	float Intensity = 1.0f;   // on the bloom added back to the scene
	float Fade = 0.0f;        // seconds a change eases over; 0 snaps
	float Pulse = 0.0f;       // throb depth on the intensity, 0..1
	float PulseRate = 0.0f;   // Hz; 0 = the glow alarm pulse's rate and phase

	// Not part of the look: the glow alarm pulse's inputs as StartScene uploads them
	// (mGlowTex4.z / .w). Taken live every frame, never eased, never restart a fade.
	float GlowPulseLevel = 0.0f;
	float GlowPulseRate = 1.0f;

	bool SameLook(const PPBloomOverride &other) const
	{
		return Spread == other.Spread && Threshold == other.Threshold && Knee == other.Knee
			&& TintR == other.TintR && TintG == other.TintG && TintB == other.TintB
			&& Mix == other.Mix && Intensity == other.Intensity && Fade == other.Fade
			&& Pulse == other.Pulse && PulseRate == other.PulseRate;
	}
};

// [PINNEDBLOOM] ONE BLOOM LOOK: every value one bloom chain uses, as the chain uses it
// ("Engine docs/EMISSIVE_BLOOM_PLAN.md" 2f). PPBloom builds the REST look every frame -- the
// gl_bloom_* / gl_exposure_* cvars, moved by any SetBloomOverride -- and, while pinned bloom is on,
// the PINNED look from the gl_bloom_pin_* cvars, which no override reaches. Two looks that send the
// same bytes draw the same bloom.
struct PPBloomLook
{
	float Threshold = 1.0f;         // the extract's threshold
	float Knee = 0.5f;              // the extract's knee, already capped at the threshold (E2)
	float Amount = 1.4f;            // the blur's width (gl_bloom_amount's meaning)
	bool Anamorphic = false;
	float AnamorphicRatio = 3.0f;
	float Step = 1.0f;              // gl_bloom_step's meaning (E3)
	FVector3 Tint = FVector3(1.0f, 1.0f, 1.0f);   // on the bloom added back; carries an override's intensity and pulse
	float Chromatic = 0.0f;
	// The exposure combine's values as the camera exposure sends them (PPCameraExposure::CombineUniforms).
	float ExposureBase = 0.35f;
	float ExposureMin = 0.35f;
	float ExposureScale = 1.3f;
	float ExposureSpeed = 0.05f;
};

// [PINNEDBLOOM] The blur a look asks for: today's seven-texel blur or E3's wide one, with its
// weights. Two looks with the same chain and the same fringing can share one chain (PPBloomPlan).
struct PPBloomChain
{
	bool Stepped = false;
	BlurUniforms Horizontal, Vertical;                  // !Stepped; SampleWeights[7] is never set, as today
	BlurSteppedUniforms HorizontalWide, VerticalWide;   // Stepped
};

// [PINNEDBLOOM] Which passes bloom runs for an eye (PPBloom::ChoosePlan; EMISSIVE_BLOOM_PLAN.md 2f's A, B, C):
//   Legacy     today's bloom, draw for draw: pinned bloom off, no light mask for this eye's scene, no
//              pinned light live, or the pinned look sends the same values as the rest look.
//   OneChain   the looks differ only before the blur (threshold, knee, exposure, tint and intensity):
//              one extract carrying both looks (bloomextract.fp BLOOM_EXTRACT_DUAL), one chain, a
//              neutral tint on the final combine.
//   TwoChains  the blur (spread, streaks, step) or the fringing differs: the pinned share gets a
//              chain of its own (BLOOM_EXTRACT_SHARE twice), about today's bloom again in cost.
enum class PPBloomPlan { Legacy, OneChain, TwoChains };

class PPBloom
{
public:
	// [EXPOSUREIMPULSE] True when bloom drew for this eye -- levels[0] then holds this eye's bloom (RestBloomTexture), which
	// flash blindness's wash re-adds as glare (PPExposureImpulse, Pass1). Its draws are exactly what they were.
	bool RenderBloom(PPRenderState *renderstate, int sceneWidth, int sceneHeight, int fixedcm);
	void RenderBlur(PPRenderState *renderstate, int sceneWidth, int sceneHeight, float gameinfobluramount);

	// [BLOOMOVERRIDE] The level's override, handed over once per scene eye with the frame
	// clock (screen->FrameTime, milliseconds). A repeat of the current look changes
	// nothing; a new look eases in over its fade, a clear eases out over the last one.
	void SetOverride(const PPBloomOverride &target, uint64_t now);
	void ClearOverride(uint64_t now);

	// [PINNEDBLOOM] The plan the last RenderBloom ran (a label for the performance log).
	PPBloomPlan LastPlan() const { return Plan; }

	// [EXPOSUREIMPULSE] The rest look's bloom as the last RenderBloom that drew left it (level 0 after its chain's last blur):
	// all of the bloom under the Legacy and OneChain plans, the room's share under TwoChains. Read right after that call.
	PPTexture *RestBloomTexture() { return &levels[0].VTexture; }

private:
	void BlurStep(PPRenderState *renderstate, const BlurUniforms &blurUniforms, PPTexture &input, PPTexture &output, PPViewport viewport, bool vertical);
	void UpdateTextures(int width, int height);

	static float ComputeBlurGaussian(float n, float theta);
	static void ComputeBlurSamples(int sampleCount, float blurAmount, float *sampleWeights);

	// [BLOOMSTEP] The wide blur (gl_bloom_step > 1): the BLUR_STEPPED programs, Linear.
	void BlurStepWide(PPRenderState *renderstate, const BlurSteppedUniforms &blurUniforms, PPTexture &input, PPTexture &output, PPViewport viewport, bool vertical);
	static void ComputeBlurSamplesStepped(float theta, float texelStep, BlurSteppedUniforms &uniforms);
	// A tap no further than this many level texels from the last: at the smallest level
	// (1/32 of the scene) 32 texels already puts the outer taps past the whole image,
	// and it bounds the reads when the step meets a high anamorphic ratio.
	static constexpr float MAX_BLUR_TEXEL_STEP = 32.0f;
	// Linear reads per tap: at most two texels apart, so none is skipped (16 at the cap).
	static constexpr int MAX_BLUR_READS_PER_TAP = 16;

	// [BLOOMOVERRIDE] The look at `now` and its eased mix (0 when nothing applies).
	float EvaluateOverride(uint64_t now, PPBloomOverride &out) const;
	static float OverridePulseFactor(const PPBloomOverride &look);
	static void BlendOverride(const PPBloomOverride &look, float mixAmount, float &threshold, float &knee, float &amount, FVector3 &tint);

	// [PINNEDBLOOM] The looks, the plan and the passes they share (PPBloomLook, PPBloomChain,
	// PPBloomPlan above; hw_postprocess.cpp). Legacy runs exactly the passes RenderBloom always ran.
	void RestLook(PPBloomLook &look) const;
	static void PinnedLook(PPBloomLook &look);
	static void ComputeChain(const PPBloomLook &look, PPBloomChain &chain);
	static bool SameChain(const PPBloomChain &a, const PPBloomChain &b);
	PPBloomPlan ChoosePlan(const PPBloomLook &rest, const PPBloomChain &restChain, PPBloomLook &pinned, PPBloomChain &pinnedChain, bool &pinnedExposure) const;
	void RenderExtractShare(PPRenderState *renderstate, PPBlurLevel &level0, const PPBloomLook &look, int weightClass, PPTexture *exposureTexture);
	void RenderChain(PPRenderState *renderstate, PPBlurLevel *chainLevels, const PPBloomChain &chain);
	void RenderFinalCombine(PPRenderState *renderstate, PPBlurLevel &level0, const FVector3 &tint, float chromatic);
	void UpdatePinnedTextures();

	PPBlurLevel levels[NumBloomLevels];
	int lastWidth = 0;
	int lastHeight = 0;

	// [BLOOMOVERRIDE] Ease state. Live: an override may still be on screen (set, or a
	// clear still fading). TargetActive: the level's override is set.
	bool OverrideLive = false;
	bool OverrideTargetActive = false;
	PPBloomOverride OverrideFrom;
	PPBloomOverride OverrideTo;
	uint64_t OverrideStartMs = 0;
	float OverrideFadeSeconds = 0.0f;

	// [PINNEDBLOOM] The pinned share's own chain (TwoChains), sized like `levels` the first time it runs;
	// its textures cost nothing until then.
	PPBlurLevel pinLevels[NumBloomLevels];
	int pinLastWidth = 0;
	int pinLastHeight = 0;
	PPBloomPlan Plan = PPBloomPlan::Legacy;

	PPShader BloomCombine = { "shaders/pp/bloomcombine.fp", "", BloomCombineUniforms::Desc() };
	PPShader BloomExtract = { "shaders/pp/bloomextract.fp", "", ExtractUniforms::Desc() };
	PPShader BlurVertical = { "shaders/pp/blur.fp", "#define BLUR_VERTICAL\n", BlurUniforms::Desc() };
	PPShader BlurHorizontal = { "shaders/pp/blur.fp", "#define BLUR_HORIZONTAL\n", BlurUniforms::Desc() };
	// [BLOOMSTEP] Compiled on first use, so they cost nothing until gl_bloom_step leaves 1.
	PPShader BlurVerticalStepped = { "shaders/pp/blur.fp", "#define BLUR_VERTICAL\n#define BLUR_STEPPED\n", BlurSteppedUniforms::Desc() };
	PPShader BlurHorizontalStepped = { "shaders/pp/blur.fp", "#define BLUR_HORIZONTAL\n#define BLUR_STEPPED\n", BlurSteppedUniforms::Desc() };
	// [PINNEDBLOOM] The extract weighed by the light mask. Compiled on first use, so they cost nothing
	// until pinned bloom first runs a OneChain or TwoChains plan.
	PPShader BloomExtractShare = { "shaders/pp/bloomextract.fp", "#define BLOOM_EXTRACT_SHARE\n", ExtractShareUniforms::Desc() };
	PPShader BloomExtractDual = { "shaders/pp/bloomextract.fp", "#define BLOOM_EXTRACT_DUAL\n", ExtractDualUniforms::Desc() };
};

/////////////////////////////////////////////////////////////////////////////

struct LensUniforms
{
	float AspectRatio;
	float Scale;
	float Padding0, Padding1;
	FVector4 LensDistortionCoefficient;
	FVector4 CubicDistortionValue;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "Aspect", UniformType::Float, offsetof(LensUniforms, AspectRatio) },
			{ "Scale", UniformType::Float, offsetof(LensUniforms, Scale) },
			{ "Padding0", UniformType::Float, offsetof(LensUniforms, Padding0) },
			{ "Padding1", UniformType::Float, offsetof(LensUniforms, Padding1) },
			{ "k", UniformType::Vec4, offsetof(LensUniforms, LensDistortionCoefficient) },
			{ "kcube", UniformType::Vec4, offsetof(LensUniforms, CubicDistortionValue) }
		};
	}
};

class PPLensDistort
{
public:
	void Render(PPRenderState *renderstate);

private:
	PPShader Lens = { "shaders/pp/lensdistortion.fp", "", LensUniforms::Desc() };
};

/////////////////////////////////////////////////////////////////////////////

struct FXAAUniforms
{
	FVector2 ReciprocalResolution;
	float Padding0, Padding1;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "ReciprocalResolution", UniformType::Vec2, offsetof(FXAAUniforms, ReciprocalResolution) },
			{ "Padding0", UniformType::Float, offsetof(FXAAUniforms, Padding0) },
			{ "Padding1", UniformType::Float, offsetof(FXAAUniforms, Padding1) }
		};
	}
};

class PPFXAA
{
public:
	void Render(PPRenderState *renderstate);

private:
	void CreateShaders();
	int GetMaxVersion();
	FString GetDefines();

	PPShader FXAALuma;
	PPShader FXAA;
	int LastQuality = -1;
};

/////////////////////////////////////////////////////////////////////////////

struct ExposureExtractUniforms
{
	FVector2 Scale;
	FVector2 Offset;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "Scale", UniformType::Vec2, offsetof(ExposureExtractUniforms, Scale) },
			{ "Offset", UniformType::Vec2, offsetof(ExposureExtractUniforms, Offset) }
		};
	}
};

struct ExposureCombineUniforms
{
	float ExposureBase;
	float ExposureMin;
	float ExposureScale;
	float ExposureSpeed;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "ExposureBase", UniformType::Float, offsetof(ExposureCombineUniforms, ExposureBase) },
			{ "ExposureMin", UniformType::Float, offsetof(ExposureCombineUniforms, ExposureMin) },
			{ "ExposureScale", UniformType::Float, offsetof(ExposureCombineUniforms, ExposureScale) },
			{ "ExposureSpeed", UniformType::Float, offsetof(ExposureCombineUniforms, ExposureSpeed) }
		};
	}
};

class PPExposureLevel
{
public:
	PPViewport Viewport;
	PPTexture Texture;
};

class PPCameraExposure
{
public:
	void Render(PPRenderState *renderstate, int sceneWidth, int sceneHeight);

	PPTexture CameraTexture = { 1, 1, PixelFormat::R32f };

	// [BLOOMSAFETY] [PINNEDBLOOM] The exposure combine's uniforms from four exposure settings, through E2's
	// guards (Min floored at 0.01, Speed 0..1, a non-finite value to its default). The live exposure and
	// the pinned bloom look both take theirs from here, so a captured look sends the very same bits.
	static ExposureCombineUniforms CombineUniforms(float base, float minimum, float scale, float speed);

	// [PINNEDBLOOM] The pinned bloom look's own exposure, while it differs from the live one: the same
	// combine over this eye's measured average, into PinnedCameraTexture, adapting at the pinned speed.
	// Its first frame after a frame without it (or after the levels were rebuilt) draws with no blend.
	void RenderPinned(PPRenderState *renderstate, const ExposureCombineUniforms &combineUniforms);
	PPTexture PinnedCameraTexture = { 1, 1, PixelFormat::R32f };

private:
	void UpdateTextures(int width, int height);

	std::vector<PPExposureLevel> ExposureLevels;
	bool FirstExposureFrame = true;
	// [PINNEDBLOOM] PinnedCameraTexture holds a running value, and the displayed frame
	// (DFrameBuffer::FrameCount) that last drew it.
	bool PinnedHistory = false;
	uint64_t PinnedLastFrame = 0;

	PPShader ExposureExtract = { "shaders/pp/exposureextract.fp", "", ExposureExtractUniforms::Desc() };
	PPShader ExposureAverage = { "shaders/pp/exposureaverage.fp", "", {}, 400 };
	PPShader ExposureCombine = { "shaders/pp/exposurecombine.fp", "", ExposureCombineUniforms::Desc() };
};

/////////////////////////////////////////////////////////////////////////////

struct ColormapUniforms
{
	FVector4 MapStart;
	FVector4 MapRange;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "uFixedColormapStart", UniformType::Vec4, offsetof(ColormapUniforms, MapStart) },
			{ "uFixedColormapRange", UniformType::Vec4, offsetof(ColormapUniforms, MapRange) },
		};
	}
};

class PPColormap
{
public:
	void Render(PPRenderState *renderstate, int fixedcm, float flash);

private:
	PPShader Colormap = { "shaders/pp/colormap.fp", "", ColormapUniforms::Desc() };
};

/////////////////////////////////////////////////////////////////////////////

class PPTonemap
{
public:
	void SetTonemapMode(ETonemapMode tm) { level_tonemap = tm; }
	void Render(PPRenderState *renderstate);
	void ClearTonemapPalette() { PaletteTexture = {}; }

private:
	void UpdateTextures();

	PPTexture PaletteTexture;

	PPShader LinearShader = { "shaders/pp/tonemap.fp", "#define LINEAR\n", {} };
	PPShader ReinhardShader = { "shaders/pp/tonemap.fp", "#define REINHARD\n", {} };
	PPShader HejlDawsonShader = { "shaders/pp/tonemap.fp", "#define HEJLDAWSON\n", {} };
	PPShader Uncharted2Shader = { "shaders/pp/tonemap.fp", "#define UNCHARTED2\n", {} };
	PPShader PaletteShader = { "shaders/pp/tonemap.fp", "#define PALETTE\n", {} };
	ETonemapMode level_tonemap = ETonemapMode::None;
};

/////////////////////////////////////////////////////////////////////////////

struct LinearDepthUniforms
{
	int SampleIndex;
	float LinearizeDepthA;
	float LinearizeDepthB;
	float InverseDepthRangeA;
	float InverseDepthRangeB;
	float Padding0, Padding1, Padding2;
	FVector2 Scale;
	FVector2 Offset;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "SampleIndex", UniformType::Int, offsetof(LinearDepthUniforms, SampleIndex) },
			{ "LinearizeDepthA", UniformType::Float, offsetof(LinearDepthUniforms, LinearizeDepthA) },
			{ "LinearizeDepthB", UniformType::Float, offsetof(LinearDepthUniforms, LinearizeDepthB) },
			{ "InverseDepthRangeA", UniformType::Float, offsetof(LinearDepthUniforms, InverseDepthRangeA) },
			{ "InverseDepthRangeB", UniformType::Float, offsetof(LinearDepthUniforms, InverseDepthRangeB) },
			{ "Padding0", UniformType::Float, offsetof(LinearDepthUniforms, Padding0) },
			{ "Padding1", UniformType::Float, offsetof(LinearDepthUniforms, Padding1) },
			{ "Padding2", UniformType::Float, offsetof(LinearDepthUniforms, Padding2) },
			{ "Scale", UniformType::Vec2, offsetof(LinearDepthUniforms, Scale) },
			{ "Offset", UniformType::Vec2, offsetof(LinearDepthUniforms, Offset) }
		};
	}
};

struct SSAOUniforms
{
	FVector2 UVToViewA;
	FVector2 UVToViewB;
	FVector2 InvFullResolution;
	float NDotVBias;
	float NegInvR2;
	float RadiusToScreen;
	float AOMultiplier;
	float AOStrength;
	int SampleIndex;
	float Padding0, Padding1;
	FVector2 Scale;
	FVector2 Offset;
	int GlobalFade;
	float GlobalFadeDensity;
	float GlobalFadeGradient;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "UVToViewA", UniformType::Vec2, offsetof(SSAOUniforms, UVToViewA) },
			{ "UVToViewB", UniformType::Vec2, offsetof(SSAOUniforms, UVToViewB) },
			{ "InvFullResolution", UniformType::Vec2, offsetof(SSAOUniforms, InvFullResolution) },
			{ "NDotVBias", UniformType::Float, offsetof(SSAOUniforms, NDotVBias) },
			{ "NegInvR2", UniformType::Float, offsetof(SSAOUniforms, NegInvR2) },
			{ "RadiusToScreen", UniformType::Float, offsetof(SSAOUniforms, RadiusToScreen) },
			{ "AOMultiplier", UniformType::Float, offsetof(SSAOUniforms, AOMultiplier) },
			{ "AOStrength", UniformType::Float, offsetof(SSAOUniforms, AOStrength) },
			{ "SampleIndex", UniformType::Int, offsetof(SSAOUniforms, SampleIndex) },
			{ "Padding0", UniformType::Float, offsetof(SSAOUniforms, Padding0) },
			{ "Padding1", UniformType::Float, offsetof(SSAOUniforms, Padding1) },
			{ "Scale", UniformType::Vec2, offsetof(SSAOUniforms, Scale) },
			{ "Offset", UniformType::Vec2, offsetof(SSAOUniforms, Offset) },
			{ "GlobalFade", UniformType::Int, offsetof(SSAOUniforms, GlobalFade) },
			{ "GlobalFadeDensity", UniformType::Float, offsetof(SSAOUniforms, GlobalFadeDensity) },
			{ "GlobalFadeGradient", UniformType::Float, offsetof(SSAOUniforms, GlobalFadeGradient) },
		};
	}
};

struct DepthBlurUniforms
{
	float BlurSharpness;
	float PowExponent;
	float Padding0, Padding1;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "BlurSharpness", UniformType::Float, offsetof(DepthBlurUniforms, BlurSharpness) },
			{ "PowExponent", UniformType::Float, offsetof(DepthBlurUniforms, PowExponent) },
			{ "Padding0", UniformType::Float, offsetof(DepthBlurUniforms, Padding0) },
			{ "Padding1", UniformType::Float, offsetof(DepthBlurUniforms, Padding1) }
		};
	}
};

struct AmbientCombineUniforms
{
	int SampleCount;
	int DebugMode, Padding1, Padding2;
	FVector2 Scale;
	FVector2 Offset;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "SampleCount", UniformType::Int, offsetof(AmbientCombineUniforms, SampleCount) },
			{ "DebugMode", UniformType::Int, offsetof(AmbientCombineUniforms, DebugMode) },
			{ "Padding1", UniformType::Int, offsetof(AmbientCombineUniforms, Padding1) },
			{ "Padding2", UniformType::Int, offsetof(AmbientCombineUniforms, Padding2) },
			{ "Scale", UniformType::Vec2, offsetof(AmbientCombineUniforms, Scale) },
			{ "Offset", UniformType::Vec2, offsetof(AmbientCombineUniforms, Offset) }
		};
	}
};

class PPAmbientOcclusion
{
public:
	PPAmbientOcclusion();
	void Render(PPRenderState *renderstate, float m5, int sceneWidth, int sceneHeight);
	void SetNoAmbientOcclusion() { level_noAmbientOcclusion = true; }

private:
	void CreateShaders();
	void UpdateTextures(int width, int height);

	enum Quality
	{
		Off,
		LowQuality,
		MediumQuality,
		HighQuality,
		NumQualityModes
	};

	int AmbientWidth = 0;
	int AmbientHeight = 0;

	int LastQuality = -1;
	int LastWidth = 0;
	int LastHeight = 0;

	bool level_noAmbientOcclusion = false;

	PPShader LinearDepth;
	PPShader LinearDepthMS;
	PPShader AmbientOcclude;
	PPShader AmbientOccludeMS;
	PPShader BlurVertical;
	PPShader BlurHorizontal;
	PPShader Combine;
	PPShader CombineMS;

	PPTexture LinearDepthTexture;
	PPTexture Ambient0;
	PPTexture Ambient1;

	enum { NumAmbientRandomTextures = 3 };
	PPTexture AmbientRandomTexture[NumAmbientRandomTextures];
};

struct PresentUniforms
{
	// LAYOUT IS LOAD-BEARING. UniformBlockDecl::Create emits these fields to GLSL
	// in declaration order under plain std140 with no explicit offsets, so this
	// struct must match std140 byte for byte. Scale and Offset are vec2 and need
	// an 8-byte boundary; anything that changes the byte count before them must
	// be balanced by padding, or the present pass samples a garbage UV rect and
	// the screen goes black. The static_asserts below the struct enforce it.
	float InvGamma;
	float Contrast;
	float Brightness;	// UZDXREMA: additive brightness lift from vid_brightness.
						// Separate stage from upstream's BlackPoint/WhitePoint.
	float Saturation;
	float BlackPoint;
	float WhitePoint;
	float ColorScale;
	int GrayFormula;
	int WindowPositionParity; // top-of-window might not be top-of-screen
	float padding0;		// balances Brightness above; see the note at the top
	FVector2 Scale;
	FVector2 Offset;
	int HdrMode;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "InvGamma", UniformType::Float, offsetof(PresentUniforms, InvGamma) },
			{ "Contrast", UniformType::Float, offsetof(PresentUniforms, Contrast) },
			{ "Brightness", UniformType::Float, offsetof(PresentUniforms, Brightness) },
			{ "Saturation", UniformType::Float, offsetof(PresentUniforms, Saturation) },
			{ "BlackPoint", UniformType::Float, offsetof(PresentUniforms, BlackPoint) },
			{ "WhitePoint", UniformType::Float, offsetof(PresentUniforms, WhitePoint) },
			{ "ColorScale", UniformType::Float, offsetof(PresentUniforms, ColorScale) },
			{ "GrayFormula", UniformType::Int, offsetof(PresentUniforms, GrayFormula) },
			{ "WindowPositionParity", UniformType::Int, offsetof(PresentUniforms, WindowPositionParity) },
			{ "padding0", UniformType::Float, offsetof(PresentUniforms, padding0) },
			{ "UVScale", UniformType::Vec2, offsetof(PresentUniforms, Scale) },
			{ "UVOffset", UniformType::Vec2, offsetof(PresentUniforms, Offset) },
			{ "HdrMode", UniformType::Int, offsetof(PresentUniforms, HdrMode) }
		};
	}
};

// std140 guard rails for PresentUniforms. UniformBlockDecl::Create emits the
// fields in declaration order with no explicit offsets, so the C++ layout IS the
// GLSL layout. vec2 needs 8-byte alignment; if these fire, add or remove a
// padding float rather than reordering the block.
static_assert(offsetof(PresentUniforms, Scale) % 8 == 0,
	"PresentUniforms::Scale must be 8-byte aligned for std140 - add a padding float");
static_assert(offsetof(PresentUniforms, Offset) % 8 == 0,
	"PresentUniforms::Offset must be 8-byte aligned for std140 - add a padding float");

class PPPresent
{
public:
	PPPresent();

	PPTexture Dither;

	PPShader Present = { "shaders/pp/present.fp", "", PresentUniforms::Desc() };
	PPShader Checker3D = { "shaders/pp/present_checker3d.fp", "", PresentUniforms::Desc() };
	PPShader Column3D = { "shaders/pp/present_column3d.fp", "", PresentUniforms::Desc() };
	PPShader Row3D = { "shaders/pp/present_row3d.fp", "", PresentUniforms::Desc() };
};

struct ShadowMapUniforms
{
	float ShadowmapQuality;
	int NodesCount;
	float Padding0, Padding1;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "ShadowmapQuality", UniformType::Float, offsetof(ShadowMapUniforms, ShadowmapQuality) },
			{ "NodesCount", UniformType::Int, offsetof(ShadowMapUniforms, NodesCount) },
			{ "Padding0", UniformType::Float, offsetof(ShadowMapUniforms, Padding0) },
			{ "Padding1", UniformType::Float, offsetof(ShadowMapUniforms, Padding1) },
		};
	}
};

class PPPersistentBuffer
{
public:
	PPPersistentBuffer() = default;
	PPPersistentBuffer(int width, int height, PixelFormat format)
	{
		Buffers[0] = PPTexture(width, height, format);
		Buffers[1] = PPTexture(width, height, format);
	}

	void Swap() { CurrentIndex = 1 - CurrentIndex; }
	PPTexture* GetRead() { return &Buffers[CurrentIndex]; }
	PPTexture* GetWrite() { return &Buffers[1 - CurrentIndex]; }

private:
	PPTexture Buffers[2];
	int CurrentIndex = 0;
};

// [CUSTOMDEPTH] The scene depth for GLDEFS custom post-process shaders (a texture named "SceneDepth"): resolved per eye
// into a single-sample R32F texture over the screen viewport by PPCustomShaders::Run (shaders/pp/customdepth.fp).
struct CustomDepthUniforms
{
	FVector2 SceneScale;
	FVector2 SceneOffset;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "SceneScale", UniformType::Vec2, offsetof(CustomDepthUniforms, SceneScale) },
			{ "SceneOffset", UniformType::Vec2, offsetof(CustomDepthUniforms, SceneOffset) },
		};
	}
};

class PPCustomShaderInstance
{
public:
	PPCustomShaderInstance(PostProcessShader *desc, std::unique_ptr<PPPersistentBuffer> *lastInputTexture, PPTexture *resolvedDepth = nullptr);

	// [CUSTOMDEPTH] Whether the definition names a texture "SceneDepth".
	bool UsesSceneDepth() const { return NeedsSceneDepth; }

	void Run(PPRenderState *renderstate);

	PostProcessShader *Desc = nullptr;

private:
	void AddUniformField(size_t &offset, const FString &name, UniformType type, size_t fieldsize, size_t alignment = 0);
	void SetTextures(PPRenderState *renderstate);
	void SetUniforms(PPRenderState *renderstate);

	PPShader Shader;
	int UniformStructSize = 0;
	std::vector<UniformFieldDesc> Fields;
	std::vector<std::unique_ptr<FString>> FieldNames;
	std::map<FTexture*, std::unique_ptr<PPTexture>> Textures;
	std::map<FString, size_t> FieldOffset;

	std::unique_ptr<PPPersistentBuffer> *LastInputTexture;
	int LastInputTextureBinding = -1;

	PPTexture *ResolvedDepth = nullptr;	// [CUSTOMDEPTH] owned by PPCustomShaders
	bool NeedsSceneDepth = false;
};

class PPCustomShaders
{
public:
	void Run(PPRenderState *renderstate, FString target);

	// [PPPROJECT] Each eye's world-to-clip, for uniforms projected per eye (PostProcessUniformValue::Projection).
	// Published for the main view by HWDrawInfo::ProcessScene: one set per eye of a multiview scene, else one set, since
	// each eye then draws its own scene just before its post-process. hw_entrypoint.cpp sets the eye being post-processed.
	struct EyeView
	{
		float WorldToClip[16] = {};	// projection x view, column-major, GL world axes (x, z, y)
		float FocalY = 1.0f;		// the projection's [1][1]: 1 / tan(half the vertical field)
	};
	static void SetEyeView(int set, const float *projection, const float *view);
	static void SetEyeSets(int sets) { EyeSets = sets < 0 ? 0 : (sets > 2 ? 2 : sets); }
	static void SetEye(int eye) { CurrentEye = eye; }
	static int CurrentEyeSet() { return (EyeSets >= 2 && CurrentEye == 1) ? 1 : 0; }
	// Projects a world point (game axes) with one set: u, v in [0,1] across the view, clip z unscaled. False if none.
	static bool ProjectWorld(int set, double worldX, double worldY, double worldZ, double &u, double &v, double &clipZ, double &focalY);
	static EyeView Eyes[2];
	static int EyeSets;
	static int CurrentEye;
	void UpdateLastInputTexture(PPRenderState *renderstate);

private:
	void CreateShaders();
	void ResolveSceneDepth(PPRenderState *renderstate);	// [CUSTOMDEPTH]

	std::vector<std::unique_ptr<PPCustomShaderInstance>> mShaders;
	std::unique_ptr<PPPersistentBuffer> mLastInputTexture;

	// [CUSTOMDEPTH] The resolved scene depth, the screen viewport's size, and its two shader variants.
	PPTexture mResolvedDepth;
	int mDepthWidth = 0;
	int mDepthHeight = 0;
	PPShader mDepthShader = { "shaders/pp/customdepth.fp", "", CustomDepthUniforms::Desc() };
	PPShader mDepthShaderMS = { "shaders/pp/customdepth.fp", "#define MULTISAMPLE\n", CustomDepthUniforms::Desc() };
	int mLastWidth = 0;
	int mLastHeight = 0;
};

class PPShadowMap
{
public:
	void Update(PPRenderState* renderstate);

private:
	PPShader ShadowMap = { "shaders/pp/shadowmap.fp", "", ShadowMapUniforms::Desc() };
};




/////////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////////
//
// [LIGHTMASK] THE LIGHT MASK ("Engine docs/EMISSIVE_BLOOM_PLAN.md" 2a-2e; step E6a).
//
// One more colour attachment of the main scene pass records, per pixel, how much of the
// light is of two classes:
//   R  emissive light -- light from things that give light (E4 fills it in main.fp; drawn
//      lines, GPU particles and mesh chunks already write their glow here)
//   G  pinned light   -- light whose bloom E6b pins to its own look; today BEAM light: the
//      per-pixel SetBeam field (its surface light, the mist it lights, its glow in the air)
//      and the beams r_beams_drawn routes to drawn lines
// An AMOUNT is linear light summed over r + g + b, in the colour's own HDR units, written by
// every scene program with the SAME alpha as its colour -- so blending, the multisample
// resolve and linear filtering treat colour and amounts alike, and a pixel's share of a class
// is its amount over the colour's r + g + b at any point. B and A of the mask carry nothing.
//
// THE FRAME'S DECISION, TAKEN ONCE. The backend decides before anything of the frame renders
// (VulkanRenderDevice::BeginFrame: the cvars, the device's support, the mask programs compiled
// for the pass in use, the attachment created) and everything after reads Active(): the scene
// target, the program choice, SyncDrawnLines and Pass1. So both eyes, the layered post path
// and the post-only eye agree. GL and GLES never call BeginFrame: never active there.
//
// WHO READS IT. The debug view (r_lightmask_debug), drawn in bloom's place, and [PINNEDBLOOM] E6b's
// pinned bloom (PPBloom::RenderBloom, PPBloomPlan), which weighs the bloom extract by each pixel's
// pinned share. The heat shimmer moves it with the image (PPHeatRefraction::Render) and the smoke
// dims it by its own transmittance (PPSmokeVolume::Render), so at bloom LightMaskCurrent still
// describes the pixels bloom reads. E4's emissive-only bloom will read it too.
//
struct LightMaskDebugUniforms
{
	int DebugMode;	// r_lightmask_debug: 1 overlay, 2 pinned share alone
	int Padding0;
	float Padding1;
	float Padding2;

	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "DebugMode", UniformType::Int, offsetof(LightMaskDebugUniforms, DebugMode) },
			{ "Padding0", UniformType::Int, offsetof(LightMaskDebugUniforms, Padding0) },
			{ "Padding1", UniformType::Float, offsetof(LightMaskDebugUniforms, Padding1) },
			{ "Padding2", UniformType::Float, offsetof(LightMaskDebugUniforms, Padding2) },
		};
	}
};

static_assert(offsetof(LightMaskDebugUniforms, Padding2) == 12, "LightMaskDebugUniforms::Padding2 must start at 12 for std140");
static_assert(sizeof(LightMaskDebugUniforms) == 16, "LightMaskDebugUniforms must be 16 bytes");

class PPLightMask
{
public:
	// What the cvars ask for: gl_bloom_pin_beams while gl_bloom is on, or the debug view.
	// The backend adds its own conditions (support, programs, memory) before BeginFrame.
	static bool WantedByCvars();

	// Once per displayed frame, before anything renders; `active` = this frame's scene pass
	// carries the mask (VulkanRenderDevice::BeginFrame).
	void BeginFrame(bool active);
	bool Active() const { return FrameActive; }

	// Set by the backend's scene transfer (VkPostprocess::BlitSceneToPostprocess) for the eye
	// being post-processed: this eye's scene drew the mask, and LightMaskCurrent now holds it.
	// False for a scene without the mask (a save picture, the software renderer's scene).
	void SetPostInput(bool valid) { PostInput = FrameActive && valid; }
	bool PostInputValid() const { return PostInput; }

	// [PINNEDBLOOM] Pinned bloom this frame ("Keep legacy lasers"): gl_bloom_pin_beams, with gl_bloom,
	// as BeginFrame saw it, while the scene draws the mask. PPBloom reads this and never the cvar, so
	// every eye of the frame weighs its bloom alike.
	bool PinnedBloomOn() const { return PinnedBloom; }

	// [PINNEDBLOOM] Whether pinned light can be on screen this frame -- today, whether any beam slot is
	// live with a non-zero intensity (the beam upload's own test). The game side reports it once per
	// displayed frame, before the eye loop (hw_entrypoint.cpp); BeginFrame clears the report. A frame
	// with no report counts as live: a missing report can cost PPBloom's second chain, never the look.
	void SetPinnedLightLive(bool live) { PinnedLightReported = true; PinnedLight = live; }
	bool PinnedLightMayBeLive() const { return !PinnedLightReported || PinnedLight; }

	// The debug view, in bloom's place (Pass1). True when it drew, and then bloom does not run.
	bool RenderDebug(PPRenderState *renderstate);

private:
	bool FrameActive = false;
	bool PostInput = false;
	int DebugMode = 0;
	bool PinnedBloom = false;	// [PINNEDBLOOM]
	bool PinnedLightReported = false;
	bool PinnedLight = false;

	PPShader DebugShader = { "shaders/pp/lightmaskdebug.fp", "", LightMaskDebugUniforms::Desc() };
};

/////////////////////////////////////////////////////////////////////////////

// [EXPOSUREIMPULSE] FLASH BLINDNESS: THE WASH ("Engine docs/SENSORY_IMPULSES_PLAN.md" 2d with its owner answers;
// "Engine docs/EXPOSURE_IMPULSE_SI_L_IMPL_NOTES.md"). A flash a mod marks (LevelLocals.ExposureImpulse), weighed for this viewer
// by hw_exposureimpulse.cpp, washes the view out for a moment: brighter, toward grey, a haze in the flash's colour and extra
// glare, strongest where the eye was dark-adapted. hw_postprocess is common code: the game side hands in one frame's numbers
// (PPExposureImpulseFrame) and this pass only draws them.
//
// SKIPPED, NOT ZERO. Render returns on its first line unless the published frame is Live: no group, no texture, no draw -- the
// frame without this pass, which is every frame with "Flash blindness" off or no flash in the last seconds. While live, a pixel
// with no wash (Amount 0, a pure beam pixel under HOLD_BEAMS) and every pixel outside the scene rectangle is written as read.
//
// WHERE (Pass1): last, after bloom -- exposure, beforebloom, smoke, volbeam, emissive volumes, heatmap, heat refraction, bloom,
// EXPOSURE IMPULSE -- and only where the light mask's debug view did not draw in bloom's place. After bloom, so it can re-add
// this eye's bloom as glare; the exposure meter measured the scene first, so the meter never sees the wash. Over the whole
// screen viewport into the next pipeline image (the lens pattern). Every eye of a layered post path draws it or none: whether
// it draws is the frame's (Live), never the eye's.
//
// THE DARKNESS, LATCHED. How dark-adapted the eye was is the exposure meter's own value (PPCameraExposure::CameraTexture). The
// first draw of a burst copies it into LatchTexture before its wash; every later eye and frame of the burst reads that copy, so
// the burst's own muzzle light cannot brighten the meter mid-wash and both eyes agree. With bloom off (the meter does not run)
// or exposure settings whose dark and lit values are too close, the darkness is the game side's guess from the view sector.
//
// DRAWS (group pp.exposureimpulse):
//   1. exposureimpulse.fp EXPOSURE_IMPULSE_LATCH  a burst's first draw, metered only: CameraTexture -> LatchTexture (1x1).
//   2. exposureimpulse.fp [METER] [GLARE] [HOLD_BEAMS]  every eye while live: Current, then in this order the latch (METER), this
//      eye's bloom (GLARE) and the light mask (HOLD_BEAMS) -> Next, no blend. METER: this burst's latch was taken and the meter
//      is still usable. GLARE: bloom drew for this eye and the look has glare. HOLD_BEAMS: the player keeps lasers crisp and
//      pinned bloom ("Keep legacy lasers") carries the light mask for this eye.
// Nine programs at most, each compiled on first use; no existing program changes.
struct ExposureImpulseUniforms
{
	FVector2 Scale;             // screen->SceneScale(): where the scene sits in the pipeline image
	FVector2 Offset;            // screen->SceneOffset()
	float Amount;               // the wash now, 0..0.6: the envelope times its limit; 0 writes every pixel as read
	float DarkFloor;            // the share of the wash a fully lit room keeps
	float FallbackDarkness;     // 0 bright .. 1 dark, when not METER
	float LitExposure;          // METER: the meter's value in a lit room ...
	float DarkExposure;         // ... and in the dark
	float Gain;                 // the look: brighter by 1 + Gain x wash
	float Veil;                 //   the haze added, VeilTint x Veil x wash
	float Desaturate;           //   toward grey by Desaturate x wash
	FVector3 VeilTint;          //   the haze's colour
	float Glare;                //   GLARE: this eye's bloom x Glare x wash

	//   Scale 0   Offset 8   Amount 16   DarkFloor 20   FallbackDarkness 24   LitExposure 28   DarkExposure 32   Gain 36
	//   Veil 40   Desaturate 44   VeilTint 48   Glare 60   -> block ends 64
	static std::vector<UniformFieldDesc> Desc()
	{
		return
		{
			{ "Scale", UniformType::Vec2, offsetof(ExposureImpulseUniforms, Scale) },
			{ "Offset", UniformType::Vec2, offsetof(ExposureImpulseUniforms, Offset) },
			{ "Amount", UniformType::Float, offsetof(ExposureImpulseUniforms, Amount) },
			{ "DarkFloor", UniformType::Float, offsetof(ExposureImpulseUniforms, DarkFloor) },
			{ "FallbackDarkness", UniformType::Float, offsetof(ExposureImpulseUniforms, FallbackDarkness) },
			{ "LitExposure", UniformType::Float, offsetof(ExposureImpulseUniforms, LitExposure) },
			{ "DarkExposure", UniformType::Float, offsetof(ExposureImpulseUniforms, DarkExposure) },
			{ "Gain", UniformType::Float, offsetof(ExposureImpulseUniforms, Gain) },
			{ "Veil", UniformType::Float, offsetof(ExposureImpulseUniforms, Veil) },
			{ "Desaturate", UniformType::Float, offsetof(ExposureImpulseUniforms, Desaturate) },
			{ "VeilTint", UniformType::Vec3, offsetof(ExposureImpulseUniforms, VeilTint) },
			{ "Glare", UniformType::Float, offsetof(ExposureImpulseUniforms, Glare) },
		};
	}
};

static_assert(offsetof(ExposureImpulseUniforms, Amount) == 16, "ExposureImpulseUniforms::Amount must start at 16 for std140");
static_assert(offsetof(ExposureImpulseUniforms, Desaturate) == 44, "ExposureImpulseUniforms::Desaturate must start at 44 for std140");
static_assert(offsetof(ExposureImpulseUniforms, VeilTint) == 48, "ExposureImpulseUniforms::VeilTint must start at 48 for std140");
static_assert(offsetof(ExposureImpulseUniforms, Glare) == 60, "ExposureImpulseUniforms::Glare must start at 60 for std140");
static_assert(sizeof(ExposureImpulseUniforms) == 64, "ExposureImpulseUniforms must be 64 bytes");

// One frame's wash, handed in once per displayed frame by hw_exposureimpulse.cpp (the envelope and the look,
// hw_exposureimpulsecore.h); every eye reads the same copy. The default draws nothing.
struct PPExposureImpulseFrame
{
	bool Live = false;                  // a wash is on screen this frame: Render draws
	uint64_t Burst = 0;                 // which burst it belongs to: the first draw of a new one latches the darkness
	float Amount = 0.0f;                // the wash now (the envelope times its limit)
	float Gain = 0.0f;                  // the look's numbers (ExposureImpulseUniforms)
	float Veil = 0.0f;
	float Desaturate = 0.0f;
	float Glare = 0.0f;                 // 0: no GLARE program (Comfort)
	FVector3 VeilTint = FVector3(1.0f, 1.0f, 1.0f);
	float DarkFloor = 0.0f;
	float FallbackDarkness = 0.0f;      // 0 bright .. 1 dark, from the view sector's light
	float LitLight = 0.5f;              // the meter's lit reference, in exposureextract.fp's units
	float MeterMinSpan = 0.05f;         // the meter is used only when its dark and lit values differ by at least this
	bool HoldBeams = false;             // the player keeps lasers crisp (r_exposureimpulse_holdbeams)
};

class PPExposureImpulse
{
public:
	// Once per displayed frame, before the eye loop (hw_entrypoint.cpp through hw_exposureimpulse.cpp).
	void SetFrame(const PPExposureImpulseFrame &frame) { Frame = frame; }
	bool Live() const { return Frame.Live; }

	// Pass1, after bloom. `bloomed`: RenderBloom drew for this eye.
	void Render(PPRenderState *renderstate, int sceneWidth, int sceneHeight, bool bloomed);

	// The exposure meter's values in the dark (no light) and in a lit room (litLight), from the live exposure settings through E2's
	// guards (PPCameraExposure::CombineUniforms). True when the meter can tell them apart: bloom on (the meter runs), both finite,
	// dark above lit by at least minSpan. The debug line asks too.
	static bool MeterReferences(float litLight, float minSpan, float &darkExposure, float &litExposure);

private:
	PPShader *WashShader(bool meter, bool glare, bool holdBeams);

	PPExposureImpulseFrame Frame;
	uint64_t LatchBurst = 0;    // the burst LatchTexture was taken for (0: none yet)
	bool LatchValid = false;    // whether that burst was metered, so LatchTexture holds its darkness

	// One texel; takes no memory until a burst is first metered.
	PPTexture LatchTexture = { 1, 1, PixelFormat::R32f };

	PPShader LatchShader = { "shaders/pp/exposureimpulse.fp", "#define EXPOSURE_IMPULSE_LATCH\n", {} };
	PPShader Wash = { "shaders/pp/exposureimpulse.fp", "", ExposureImpulseUniforms::Desc() };
	PPShader WashMeter = { "shaders/pp/exposureimpulse.fp", "#define EXPOSURE_IMPULSE_METER\n", ExposureImpulseUniforms::Desc() };
	PPShader WashGlare = { "shaders/pp/exposureimpulse.fp", "#define EXPOSURE_IMPULSE_GLARE\n", ExposureImpulseUniforms::Desc() };
	PPShader WashMeterGlare = { "shaders/pp/exposureimpulse.fp", "#define EXPOSURE_IMPULSE_METER\n#define EXPOSURE_IMPULSE_GLARE\n", ExposureImpulseUniforms::Desc() };
	PPShader WashHold = { "shaders/pp/exposureimpulse.fp", "#define EXPOSURE_IMPULSE_HOLD_BEAMS\n", ExposureImpulseUniforms::Desc() };
	PPShader WashMeterHold = { "shaders/pp/exposureimpulse.fp", "#define EXPOSURE_IMPULSE_METER\n#define EXPOSURE_IMPULSE_HOLD_BEAMS\n", ExposureImpulseUniforms::Desc() };
	PPShader WashGlareHold = { "shaders/pp/exposureimpulse.fp", "#define EXPOSURE_IMPULSE_GLARE\n#define EXPOSURE_IMPULSE_HOLD_BEAMS\n", ExposureImpulseUniforms::Desc() };
	PPShader WashMeterGlareHold = { "shaders/pp/exposureimpulse.fp", "#define EXPOSURE_IMPULSE_METER\n#define EXPOSURE_IMPULSE_GLARE\n#define EXPOSURE_IMPULSE_HOLD_BEAMS\n", ExposureImpulseUniforms::Desc() };
};

/////////////////////////////////////////////////////////////////////////////

class Postprocess
{
public:
	PPBloom bloom;
	PPVolumetricBeam volbeam;
	PPHeatmap heatmap;
	PPHeatRefraction heatrefraction;	// [HEATREFRACTION] heat shimmer
	PPSmokeVolume smokevolume;	// [SMOKEVOLUME] the smoke volume's drawing (13c)
	PPEmissiveVolumes emissivevolumes;	// [EMISSIVEVOLUMES] the emissive volumes' drawing (#15)
	PPLightMask lightmask;	// [LIGHTMASK] the frame's light mask decision and its debug view
	PPExposureImpulse exposureimpulse;	// [EXPOSUREIMPULSE] flash blindness's wash, last in Pass1 (hw_exposureimpulse.cpp publishes its frame)
	PPLensDistort lens;
	PPFXAA fxaa;
	PPCameraExposure exposure;
	PPColormap colormap;
	PPTonemap tonemap;
	PPAmbientOcclusion ssao;
	PPPresent present;
	PPShadowMap shadowmap;
	PPCustomShaders customShaders;


	void SetTonemapMode(ETonemapMode tm) { tonemap.SetTonemapMode(tm); }
	void SetNoAmbientOcclusion() { ssao.SetNoAmbientOcclusion(); }
	void Pass1(PPRenderState *state, int fixedcm, int sceneWidth, int sceneHeight);
	void Pass2(PPRenderState* state, int fixedcm, float flash, int sceneWidth, int sceneHeight);
};


extern Postprocess hw_postprocess;
