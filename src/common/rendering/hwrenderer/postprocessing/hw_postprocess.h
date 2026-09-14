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
enum class PPTextureType { CurrentPipelineTexture, NextPipelineTexture, PPTexture, SceneColor, SceneFog, SceneNormal, SceneDepth, SwapChain, ShadowMap };

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

class PPBloom
{
public:
	void RenderBloom(PPRenderState *renderstate, int sceneWidth, int sceneHeight, int fixedcm);
	void RenderBlur(PPRenderState *renderstate, int sceneWidth, int sceneHeight, float gameinfobluramount);

private:
	void BlurStep(PPRenderState *renderstate, const BlurUniforms &blurUniforms, PPTexture &input, PPTexture &output, PPViewport viewport, bool vertical);
	void UpdateTextures(int width, int height);

	static float ComputeBlurGaussian(float n, float theta);
	static void ComputeBlurSamples(int sampleCount, float blurAmount, float *sampleWeights);

	PPBlurLevel levels[NumBloomLevels];
	int lastWidth = 0;
	int lastHeight = 0;

	PPShader BloomCombine = { "shaders/pp/bloomcombine.fp", "", BloomCombineUniforms::Desc() };
	PPShader BloomExtract = { "shaders/pp/bloomextract.fp", "", ExtractUniforms::Desc() };
	PPShader BlurVertical = { "shaders/pp/blur.fp", "#define BLUR_VERTICAL\n", BlurUniforms::Desc() };
	PPShader BlurHorizontal = { "shaders/pp/blur.fp", "#define BLUR_HORIZONTAL\n", BlurUniforms::Desc() };
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

private:
	void UpdateTextures(int width, int height);

	std::vector<PPExposureLevel> ExposureLevels;
	bool FirstExposureFrame = true;

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

class PPCustomShaderInstance
{
public:
	PPCustomShaderInstance(PostProcessShader *desc, std::unique_ptr<PPPersistentBuffer> *lastInputTexture);

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
};

class PPCustomShaders
{
public:
	void Run(PPRenderState *renderstate, FString target);
	void UpdateLastInputTexture(PPRenderState *renderstate);

private:
	void CreateShaders();

	std::vector<std::unique_ptr<PPCustomShaderInstance>> mShaders;
	std::unique_ptr<PPPersistentBuffer> mLastInputTexture;
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

class Postprocess
{
public:
	PPBloom bloom;
	PPVolumetricBeam volbeam;
	PPHeatmap heatmap;
	PPHeatRefraction heatrefraction;	// [HEATREFRACTION] heat shimmer
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
