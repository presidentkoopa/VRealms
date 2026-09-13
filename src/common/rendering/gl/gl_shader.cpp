/*
** gl_shader.cpp
**
** GLSL shader handling
**
**---------------------------------------------------------------------------
**
** Copyright 2004-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include "gl_system.h"
#include "c_cvars.h"
#include "v_video.h"
#include "filesystem.h"
#include "engineerrors.h"
#include "cmdlib.h"
#include "md5.h"
#include "gl_shader.h"
#include "hw_shaderpatcher.h"
#include "shaderuniforms.h"
#include "hw_viewpointuniforms.h"
#include "hw_lightbuffer.h"
#include "hw_bonebuffer.h"
#include "i_specialpaths.h"
#include "printf.h"
#include "version.h"
#include "stb_include.h"

#include "gl_interface.h"
#include "gl_debug.h"
#include "matrix.h"
#include "gl_renderer.h"
#include <map>
#include <memory>

EXTERN_CVAR(Bool, r_skipmats)
EXTERN_CVAR(Bool, gl_customshader)
CVAR(Bool, gl_lite_shader, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

namespace OpenGLRenderer
{

struct ProgramBinary
{
	uint32_t format;
	TArray<uint8_t> data;
};

static const char *ShaderMagic = "ZDSC";

static std::map<FString, std::unique_ptr<ProgramBinary>> ShaderCache; // Not a TMap because it doesn't support unique_ptr move semantics

bool IsShaderCacheActive()
{
	static bool active = true;
	static bool firstcall = true;

	if (firstcall)
	{
		const char *vendor = (const char *)glGetString(GL_VENDOR);
		active = strstr(vendor, "Intel") != nullptr;
		firstcall = false;
	}
	return active;
}

static FString CalcProgramBinaryChecksum(const FString &vertex, const FString &fragment)
{
	const GLubyte *vendor = glGetString(GL_VENDOR);
	const GLubyte *renderer = glGetString(GL_RENDERER);
	const GLubyte *version = glGetString(GL_VERSION);

	uint8_t digest[16];
	MD5Context md5;
	md5.Update(vendor, (unsigned int)strlen((const char*)vendor));
	md5.Update(renderer, (unsigned int)strlen((const char*)renderer));
	md5.Update(version, (unsigned int)strlen((const char*)version));
	md5.Update((const uint8_t *)vertex.GetChars(), (unsigned int)vertex.Len());
	md5.Update((const uint8_t *)fragment.GetChars(), (unsigned int)fragment.Len());
	md5.Final(digest);

	char hexdigest[33];
	for (int i = 0; i < 16; i++)
	{
		int v = digest[i] >> 4;
		hexdigest[i * 2] = v < 10 ? ('0' + v) : ('a' + v - 10);
		v = digest[i] & 15;
		hexdigest[i * 2 + 1] = v < 10 ? ('0' + v) : ('a' + v - 10);
	}
	hexdigest[32] = 0;
	return hexdigest;
}

static FString CreateProgramCacheName(bool create)
{
	FString path = M_GetCachePath(create);
	if (create) CreatePath(path.GetChars());
	path << "/glshadercache";
	return path;
}

static void LoadShaders()
{
	static bool loaded = false;
	if (loaded)
		return;
	loaded = true;

	try
	{
		FString path = CreateProgramCacheName(false);
		FileReader fr;
		if (!fr.OpenFile(path.GetChars()))
			I_Error("Could not open shader file");

		char magic[4];
		fr.Read(magic, 4);
		if (memcmp(magic, ShaderMagic, 4) != 0)
			I_Error("Not a shader cache file");

		uint32_t count = fr.ReadUInt32();
		if (count > 512)
			I_Error("Too many shaders cached");

		for (uint32_t i = 0; i < count; i++)
		{
			char hexdigest[33];
			if (fr.Read(hexdigest, 32) != 32)
				I_Error("Read error");
			hexdigest[32] = 0;

			std::unique_ptr<ProgramBinary> binary(new ProgramBinary());
			binary->format = fr.ReadUInt32();
			uint32_t size = fr.ReadUInt32();
			if (size > 1024 * 1024)
				I_Error("Shader too big, probably file corruption");

			binary->data.Resize(size);
			if (fr.Read(binary->data.Data(), binary->data.Size()) != binary->data.Size())
				I_Error("Read error");

			ShaderCache[hexdigest] = std::move(binary);
		}
	}
	catch (...)
	{
		ShaderCache.clear();
	}
}

static void SaveShaders()
{
	FString path = CreateProgramCacheName(true);
	std::unique_ptr<FileWriter> fw(FileWriter::Open(path.GetChars()));
	if (fw)
	{
		uint32_t count = (uint32_t)ShaderCache.size();
		fw->Write(ShaderMagic, 4);
		fw->Write(&count, sizeof(uint32_t));
		for (const auto &it : ShaderCache)
		{
			uint32_t size = it.second->data.Size();
			fw->Write(it.first.GetChars(), 32);
			fw->Write(&it.second->format, sizeof(uint32_t));
			fw->Write(&size, sizeof(uint32_t));
			fw->Write(it.second->data.Data(), it.second->data.Size());
		}
	}
}

TArray<uint8_t> LoadCachedProgramBinary(const FString &vertex, const FString &fragment, uint32_t &binaryFormat)
{
	LoadShaders();

	auto it = ShaderCache.find(CalcProgramBinaryChecksum(vertex, fragment));
	if (it != ShaderCache.end())
	{
		binaryFormat = it->second->format;
		return it->second->data;
	}
	else
	{
		binaryFormat = 0;
		return {};
	}
}

void SaveCachedProgramBinary(const FString &vertex, const FString &fragment, const TArray<uint8_t> &binary, uint32_t binaryFormat)
{
	auto &entry = ShaderCache[CalcProgramBinaryChecksum(vertex, fragment)];
	entry.reset(new ProgramBinary());
	entry->format = binaryFormat;
	entry->data = binary;

	SaveShaders();
}

FString ProcessShaderError(const char * shaderError, TArray<FString> &filenames_for_error)
{
	//ugh, intel, amd and nvidia handle things differently so this has to be a mess
	enum
	{
		READING_LUMP,
		READING_LINE_COLON,
		READING_LINE_PARENTHESES,
		SKIP_TO_NEWLINE,
	};

	FString err(shaderError);
	size_t cur = 0;
	size_t state_start = 0;

	size_t line_start = 0;
	size_t num_end = 0;

	int state = READING_LUMP;

	int64_t lump_num = 0;

	while(cur < err.Len())
	{
		if(state != SKIP_TO_NEWLINE)
		{
			while(err[cur] >= '0' && err[cur] <= '9')
			{
				cur++;
			}

			if(cur == state_start)
			{
				state = SKIP_TO_NEWLINE;
			}
			else if(state == READING_LUMP && (err[cur] == '(' || err[cur] == ':'))
			{
				FString lump_num_str = err.Mid(state_start, cur - state_start);
				lump_num = lump_num_str.ToLong();
				line_start = state_start;
				state = (err[cur] == ':') ? READING_LINE_COLON : READING_LINE_PARENTHESES;
				cur++;
				state_start = cur;
			}
			else if((state == READING_LINE_COLON && err[cur] == ':') || (state == READING_LINE_PARENTHESES && err[cur] == ')'))
			{
				FString line_num_str = err.Mid(state_start, cur - state_start);

				if(state == READING_LINE_PARENTHESES)
				{
					cur+= 3; // skip ") :"
				}
				else
				{
					cur++; // skip ":"
				}

				int64_t old_len = cur - line_start;
				// The driver's source-string number is 1-based into the include list, but
				// core shaders are one glShaderSource string, so NVIDIA reports 0 and the
				// list may hold a single name or none. An unchecked [lump_num - 1] read
				// index -1 and turned a shader compile error into a crash with no message.
				// Out of range, leave the driver's own "0(83) :" prefix as it is.
				if (lump_num < 1 || lump_num > (int64_t)filenames_for_error.Size())
				{
					state = SKIP_TO_NEWLINE;
					continue;
				}
				FString new_err = "File '" + filenames_for_error[lump_num - 1] + "', Line " + line_num_str + ": ";

				int64_t diff = new_err.Len() - old_len;

				err = err.Left(line_start) + new_err + err.Mid(line_start + old_len);

				cur += diff;
				state = SKIP_TO_NEWLINE;
			}
			else
			{ // couldn't find a valid num, skip line
				state = SKIP_TO_NEWLINE;
			}
		}
		//not 'else if' to allow this to run immediately after
		if(state == SKIP_TO_NEWLINE)
		{
			if(err[cur] == '\n' || err[cur] == '\r')
			{
				while(cur < err.Len() && (err[cur] == '\n' || err[cur] == '\r'))
				{
					cur++;
				}
				state_start = cur;
				state = READING_LUMP;
			}
			else
			{
				cur++;
			}
		}
	}
	return err;
}

bool FShader::Load(const char * name, const char * vert_prog_lump, const char * frag_prog_lump, const char * proc_prog_lump, const char * light_fragprog, const char * defines)
{
	FString error;

	FString i_data = R"(
		// these settings are actually pointless but there seem to be some old ATI drivers that fail to compile the shader without setting the precision here.
		precision highp int;
		precision highp float;
		precision highp sampler2DArray;

		// This must match the HWViewpointUniforms struct
		layout(std140) uniform ViewpointUBO {
			mat4 ProjectionMatrix;
			mat4 ViewMatrix;
			mat4 NormalViewMatrix;

			vec4 uCameraPos;
			vec4 uClipLine;

			float uGlobVis;			// uGlobVis = R_GetGlobVis(r_visibility) / 32.0
			int uPalLightLevels;
			int uViewHeight;		// Software fuzz scaling
			float uClipHeight;
			float uClipHeightDirection;
			int uShadowmapFilter;

			int uLightBlendMode;

			// [BB] Glow wave -- see hw_viewpointuniforms.h. The scalars above
			// plus mPadding0 end at 28 bytes and std140 aligns a vec4 to 16,
			// so these land on the C++ struct's offsets. Do not insert a
			// scalar before them.
			vec4 uGlowWave;
			vec4 uGlowWaveDepth;
			vec4 uGlowWavePhase;
			vec4 uGlowWaveOrigin;

			// [BB] Darkness as a shader term.
			vec4 uDarkness;
			vec4 uDarkness2;
			vec4 uDarkness3;

			// [BB] Fog slab -- fog with a top.
			vec4 uFogSlab;
			vec4 uFogSlabColor;
			vec4 uFogSlabWake;
			vec4 uFogBeamPos;
			vec4 uFogBeamDir;
			vec4 uFogBeamCol;
			vec4 uFogSlabExtra;

			// [BB] Sweep fill -- the pattern inside a band.
			vec4 uSweepFill;
			vec4 uSweepFill2;
			vec4 uSweepFill3;
			vec4 uSweepFillCol;

			// [BB] Beams -- real segment lasers.
			vec4 uBeamA[128];
			vec4 uBeamB[128];
			vec4 uBeamCol[128];
			vec4 uBeamParams;
			vec4 uBeamFX;
			vec4 uFogSurf;
			vec4 uSweepAir;
			vec4 uFogSlab2;
			vec4 uTornado;
			vec4 uTornado2;
			vec4 uTornado3;
			vec4 uTornadoCol;
			vec4 uFogDisturbA[32];
			vec4 uFogDisturbB[32];
			vec4 uFogNoise;
			vec4 uFogTendril;
			vec4 uFogTendril2;
			vec4 uFogWake2;
			vec4 uFogBow;
			vec4 uFogColor2;
			vec4 uGlowTex;
			vec4 uGlowTex2;
			vec4 uGlowTex3;
			vec4 uGlowTex4;
			vec4 uDesatKeep;
			vec4 uShapeA[128];
			vec4 uShapeB[128];
			vec4 uShapeCol[128];
			vec4 uShapeD[128];
			vec4 uShapeParams;
			vec4 uShapeUnder;
			vec4 uFogFollow;

			// Upstream 5.0.0 thick-fog knobs. They are APPENDED here, after the
			// last vec4, because that is where HWViewpointUniforms puts
			// mThickFogDistance/mThickFogMultiplier and where vk_shader.cpp's
			// ViewpointData puts them -- a uniform block is matched by OFFSET.
			float uThickFogDistance;
			float uThickFogMultiplier;

			// [BB] Standing shape pitch/roll -- appended after the thick-fog
			// pair for the identical reason THEY are appended after
			// uFogFollow: matched to hw_viewpointuniforms.h by offset, and
			// this is where that header's mShapeE actually sits (past its
			// std140 padding). std140 supplies this array's own leading
			// padding implicitly; nothing to declare by hand here.
			vec4 uShapeE[128];
			vec4 uSweepRoomMin;
			vec4 uSweepRoomMax;

			// [STAMP] Surface stamps. Appended last, matching
			// HWViewpointUniforms::mStamp* by offset.
			// 64 -- must equal MAX_SURFACE_STAMPS (func_surfacestamps.fp).
			vec4 uSurfaceStampPos[64];
			vec4 uSurfaceStampCol[64];
			vec4 uSurfaceStampArg[64];
			vec4 uSurfaceStampMod[64];
			vec4 uSurfaceStampParams;

			// [GPUPARTICLES] APPENDED LAST, matching HWViewpointUniforms by
			// offset. GL never draws particles, but the block must still
			// agree with the C++ struct it is uploaded from.
			vec4 uLevelTime;
			vec4 uGpuParticleParams;

			// [BEAMLINES] APPENDED LAST, matching HWViewpointUniforms::mBeamLook
			// by offset: per uploaded beam line, x air glow, y halo, z taper,
			// w flare. main.fp reads it on GL too.
			vec4 uBeamLook[128];

			// [round2 B2] APPENDED LAST, matching HWViewpointUniforms::
			// mSweepPassed / mSweepPassedColor by offset. The passed-region look:
			// x tint mix, y darken, z desaturate, w soft; rgb tint, w enable.
			vec4 uSweepPassed;
			vec4 uSweepPassedColor;
		};

		uniform int uTextureMode;
		uniform vec2 uClipSplit;
		uniform float uAlphaThreshold;

		// colors
		uniform vec4 uObjectColor;
		uniform vec4 uObjectColor2;
		uniform vec4 uDynLightColor;
		uniform vec4 uAddColor;
		uniform vec4 uTextureBlendColor;
		uniform vec4 uTextureModulateColor;
		uniform vec4 uTextureAddColor;
		uniform vec4 uFogColor;
		uniform float uDesaturationFactor;
		uniform float uInterpolationFactor;

		// Glowing walls stuff
		uniform vec4 uGlowTopPlane;
		uniform vec4 uGlowTopColor;
		uniform vec4 uGlowBottomPlane;
		uniform vec4 uGlowBottomColor;
		uniform vec4 uGlowTopFar;
		uniform vec4 uGlowBottomFar;
		uniform int uGlowTopFalloff;
		uniform int uGlowBottomFalloff;
		uniform float uGlowTopIntensity;
		uniform float uGlowBottomIntensity;

		// [BB] Sweep: up to eight world-space bands across every surface
		uniform vec4 uSweepOrigin;
		uniform vec4 uSweepBands[8];
		uniform vec4 uSweepColors[8];
		uniform vec4 uSweepBandOrigin[8];
		uniform int uSweepCount;
		uniform float uSweepTrail;

		// [BB] Flat-edge glow: floors/ceilings glow inward from their own edges
		uniform vec4 uFlatGlowColor;
		uniform vec4 uFlatGlowFar;
		uniform int uFlatGlowFalloff;
		uniform int uFlatGlowIsCeiling;
		uniform float uDarknessExempt;
		// [OUTLINE] Sprite outlines -- see func_spriteoutline.fp.
		uniform vec4 uOutlineColorA;
		uniform vec4 uOutlineColorB;
		uniform vec4 uOutlineParms;
		uniform float uFogDensityScale;
		uniform int uFlatGlowLineCount;
		uniform vec4 uFlatGlowLines[64];

		uniform vec4 uGradientTopPlane;
		uniform vec4 uGradientBottomPlane;

		uniform vec4 uSplitTopPlane;
		uniform vec4 uSplitBottomPlane;

		uniform vec4 uDetailParms;
		// Lighting + Fog
		uniform vec4 uLightAttr;
		#define uLightLevel uLightAttr.a
		#define uFogDensity uLightAttr.b
		#define uLightFactor uLightAttr.g
		#define uLightDist uLightAttr.r
		uniform int uFogEnabled;
		uniform int uGlobalFade;
		uniform int uGlobalFadeMode;
		uniform float uGlobalFadeDensity;
		uniform float uGlobalFadeGradient;
		uniform vec4 uGlobalFadeColor;
		uniform int uLightRangeLimit;

		// dynamic lights
		uniform int uLightIndex;

		// bone animation
		uniform int uBoneIndexBase;

		// Blinn glossiness and specular level
		uniform vec2 uSpecularMaterial;

		// matrices
		uniform mat4 ModelMatrix;
		uniform mat4 NormalModelMatrix;
		uniform mat4 TextureMatrix;

		// light buffers
		#ifdef SHADER_STORAGE_LIGHTS
		layout(std430, binding = 1) buffer LightBufferSSO
		{
			vec4 lights[];
		};
		#elif defined NUM_UBO_LIGHTS
		uniform LightBufferUBO
		{
			vec4 lights[NUM_UBO_LIGHTS];
		};
		#endif

		// bone matrix buffers
		#ifdef SHADER_STORAGE_BONES
		layout(std430, binding = 7) buffer BoneBufferSSO
		{
			mat4 bones[];
		};
		#elif defined NUM_UBO_BONES
		uniform BoneBufferUBO
		{
			mat4 bones[NUM_UBO_BONES];
		};
		#endif

		// textures
		uniform sampler2D tex;
		uniform sampler2D ShadowMap;
		uniform sampler2DArray LightMap;
		uniform sampler2D texture2;
		uniform sampler2D texture3;
		uniform sampler2D texture4;
		uniform sampler2D texture5;
		uniform sampler2D texture6;
		uniform sampler2D texture7;
		uniform sampler2D texture8;
		uniform sampler2D texture9;
		uniform sampler2D texture10;
		uniform sampler2D texture11;
		uniform sampler2D texture12;

		// timer data
		uniform float timer;

		// material types
		#if defined(SPECULAR)
		#define normaltexture texture2
		#define speculartexture texture3
		#define brighttexture texture4
		#define detailtexture texture5
		#define glowtexture texture6
		#elif defined(PBR)
		#define normaltexture texture2
		#define metallictexture texture3
		#define roughnesstexture texture4
		#define aotexture texture5
		#define brighttexture texture6
		#define detailtexture texture7
		#define glowtexture texture8
		#else
		#define brighttexture texture2
		#define detailtexture texture3
		#define glowtexture texture4
		#endif

	)";


#ifdef __APPLE__
	// The noise functions are completely broken in macOS OpenGL drivers
	// Garbage values are returned, and their infrequent usage causes extreme slowdown
	// Also, these functions must return zeroes since GLSL 4.4
	i_data += "#define noise1(unused) 0.0\n";
	i_data += "#define noise2(unused) vec2(0)\n";
	i_data += "#define noise3(unused) vec3(0)\n";
	i_data += "#define noise4(unused) vec4(0)\n";
#endif // __APPLE__

#ifdef NPOT_EMULATION
	i_data += "#define NPOT_EMULATION\nuniform vec2 uNpotEmulation;\n";
#endif

	int vp_lump = fileSystem.CheckNumForFullName(vert_prog_lump, 0);
	if (vp_lump == -1) I_Error("Unable to load '%s'", vert_prog_lump);

	int fp_lump = fileSystem.CheckNumForFullName(frag_prog_lump, 0);
	if (fp_lump == -1) I_Error("Unable to load '%s'", frag_prog_lump);



//
// The following code uses GetChars on the strings to get rid of terminating 0 characters. Do not remove or the code may break!
//
	FString vp_comb;

	assert(screen->mLights != NULL);
	assert(screen->mBones != NULL);


#ifdef __MOBILE__
	vp_comb << "#version 310 es\n";
	if (gl.flags & ~RFL_NO_CLIP_PLANES)
		vp_comb << "#extension GL_EXT_clip_cull_distance : enable\n";
#else
	if ((gl.flags & RFL_SHADER_STORAGE_BUFFER) && screen->allowSSBO())
		vp_comb << "#version 430 core\n";
	else
		vp_comb << "#version 330 core\n";
#endif
	vp_comb << "#define SUPPORTS_SHADOWMAPS\n";
	if (gl.flags & RFL_NO_CLIP_PLANES)
		vp_comb << "#define NO_CLIPDISTANCE_SUPPORT\n";

	bool lightbuffertype = screen->mLights->GetBufferType();
	if (!lightbuffertype)
		vp_comb.AppendFormat("#define NUM_UBO_LIGHTS %d\n#define NUM_UBO_BONES %d\n", screen->mLights->GetBlockSize(), screen->mBones->GetBlockSize());
	else
		vp_comb << "#define SHADER_STORAGE_LIGHTS\n#define SHADER_STORAGE_BONES\n";

	FString fp_comb = vp_comb;
	vp_comb << defines << i_data.GetChars();
	fp_comb << "$placeholder$\n" << defines << i_data.GetChars();

	vp_comb << "#line 1\n";
	fp_comb << "#line 1\n";

	vp_comb << RemoveLayoutLocationDecl(GetStringFromLump(vp_lump), "out").GetChars() << "\n";
	// [STAMP] The surface-stamp shape library, ahead of main.fp so main.fp can
	// call it without a forward declaration. It needs only the preamble above
	// (timer, the viewpoint block, gl_FragCoord), never anything main.fp
	// declares, which is what makes prepending safe.
	int stamp_lump = fileSystem.CheckNumForFullName("shaders/glsl/func_surfacestamps.fp", 0);
	if (stamp_lump == -1) I_Error("Unable to load 'shaders/glsl/func_surfacestamps.fp'");
	fp_comb << GetStringFromLump(stamp_lump).GetChars() << "\n";
	fp_comb << "#line 1\n";
	// [OUTLINE] Same deal as the stamps above -- prepended so main.fp can call
	// it with no forward declaration. It reads the `tex` sampler and timer from
	// the preamble and takes its texture coordinate as a parameter, because
	// vTexCoord is declared by main.fp and does not exist yet at this point.
	int outline_lump = fileSystem.CheckNumForFullName("shaders/glsl/func_spriteoutline.fp", 0);
	if (outline_lump == -1) I_Error("Unable to load 'shaders/glsl/func_spriteoutline.fp'");
	fp_comb << GetStringFromLump(outline_lump).GetChars() << "\n";
	fp_comb << RemoveLayoutLocationDecl(GetStringFromLump(fp_lump), "in").GetChars() << "\n";
	FString placeholder = "\n";
	TArray<FString> filenames_for_error;

	if (proc_prog_lump != NULL)
	{
		fp_comb << "#line 1\n";

		if (*proc_prog_lump != '#')
		{
			FString lump_filename(proc_prog_lump);
			FString pp_data;
			int pp_lump = fileSystem.CheckNumForFullName(proc_prog_lump, 0);	// if it's a core shader, ignore overrides by user mods.
			if (pp_lump == -1)
			{
				pp_lump = fileSystem.CheckNumForFullName(proc_prog_lump);
				if (pp_lump == -1)
				{
					I_Error("Unable to load '%s'", proc_prog_lump);
				}
				else
				{
					FString error = "";

					pp_data = stb_include_string(GetStringFromLump(pp_lump), lump_filename, filenames_for_error, error);

					if(!error.IsEmpty())
					{
						I_Error("Unable to load '%s': %s", proc_prog_lump, error.GetChars());
					}
				}
			}
			else
			{ // skip includes processing for code shaders
				pp_data = GetStringFromLump(pp_lump);
				filenames_for_error.Push(lump_filename);
			}

			if (pp_data.IndexOf("ProcessMaterial") < 0 && pp_data.IndexOf("SetupMaterial") < 0)
			{
				// this looks like an old custom hardware shader.

				if (pp_data.IndexOf("GetTexCoord") >= 0)
				{
					int pl_lump = fileSystem.CheckNumForFullName("shaders/glsl/func_defaultmat2.fp", 0);
					if (pl_lump == -1) I_Error("Unable to load '%s'", "shaders/glsl/func_defaultmat2.fp");
					fp_comb << "\n" << GetStringFromLump(pl_lump);
				}
				else
				{
					int pl_lump = fileSystem.CheckNumForFullName("shaders/glsl/func_defaultmat.fp", 0);
					if (pl_lump == -1) I_Error("Unable to load '%s'", "shaders/glsl/func_defaultmat.fp");
					fp_comb << "\n" << GetStringFromLump(pl_lump);

					if (pp_data.IndexOf("ProcessTexel") < 0)
					{
						// this looks like an even older custom hardware shader.
						// We need to replace the ProcessTexel call to make it work.

						fp_comb.Substitute("material.Base = ProcessTexel();", "material.Base = Process(vec4(1.0));");
					}
				}

				if (pp_data.IndexOf("ProcessLight") >= 0)
				{
					// The ProcessLight signatured changed. Forward to the old one.
					fp_comb << "\nvec4 ProcessLight(vec4 color);\n";
					fp_comb << "\nvec4 ProcessLight(Material material, vec4 color) { return ProcessLight(color); }\n";
				}
			}

			fp_comb << RemoveLegacyUserUniforms(pp_data).GetChars();
			fp_comb.Substitute("gl_TexCoord[0]", "vTexCoord");	// fix old custom shaders.

			if (pp_data.IndexOf("ProcessLight") < 0)
			{
				int pl_lump = fileSystem.CheckNumForFullName("shaders/glsl/func_defaultlight.fp", 0);
				if (pl_lump == -1) I_Error("Unable to load '%s'", "shaders/glsl/func_defaultlight.fp");
				fp_comb << "\n" << GetStringFromLump(pl_lump);
			}

			// ProcessMaterial must be considered broken because it requires the user to fill in data they possibly cannot know all about.
			if (pp_data.IndexOf("ProcessMaterial") >= 0 && pp_data.IndexOf("SetupMaterial") < 0)
			{
				// This reactivates the old logic and disables all features that cannot be supported with that method.
				placeholder << "#define LEGACY_USER_SHADER\n";
			}
		}
		else
		{
			// Proc_prog_lump is not a lump name but the source itself (from generated shaders)
			fp_comb << proc_prog_lump + 1;
		}
	}
	fp_comb.Substitute("$placeholder$", placeholder);

	if (light_fragprog)
	{
		int pp_lump = fileSystem.CheckNumForFullName(light_fragprog, 0);
		if (pp_lump == -1) I_Error("Unable to load '%s'", light_fragprog);
		fp_comb << GetStringFromLump(pp_lump) << "\n";
	}

	if (gl.flags & RFL_NO_CLIP_PLANES)
	{
		// On ATI's GL3 drivers we have to disable gl_ClipDistance because it's hopelessly broken.
		// This will cause some glitches and regressions but is the only way to avoid total display garbage.
		vp_comb.Substitute("gl_ClipDistance", "//");
	}

	hShader = glCreateProgram();
	FGLDebug::LabelObject(GL_PROGRAM, hShader, name);

	uint32_t binaryFormat = 0;
	TArray<uint8_t> binary;
	if (IsShaderCacheActive())
		binary = LoadCachedProgramBinary(vp_comb, fp_comb, binaryFormat);

	bool linked = false;
	if (binary.Size() > 0 && glProgramBinary)
	{
		glProgramBinary(hShader, binaryFormat, binary.Data(), binary.Size());
		GLint status = 0;
		glGetProgramiv(hShader, GL_LINK_STATUS, &status);
		linked = (status == GL_TRUE);
	}

	if (!linked)
	{
		hVertProg = glCreateShader(GL_VERTEX_SHADER);
		hFragProg = glCreateShader(GL_FRAGMENT_SHADER);

		FGLDebug::LabelObject(GL_SHADER, hVertProg, vert_prog_lump);
		FGLDebug::LabelObject(GL_SHADER, hFragProg, frag_prog_lump);

		int vp_size = (int)vp_comb.Len();
		int fp_size = (int)fp_comb.Len();

		const char *vp_ptr = vp_comb.GetChars();
		const char *fp_ptr = fp_comb.GetChars();

		glShaderSource(hVertProg, 1, &vp_ptr, &vp_size);
		glShaderSource(hFragProg, 1, &fp_ptr, &fp_size);

		GLint status = 0;

		bool errored = false;

		glCompileShader(hVertProg);

		if (glGetShaderiv(hVertProg, GL_COMPILE_STATUS, &status); status == GL_FALSE)
		{
			TArray<char> buffer;
			GLint info_log_length = 1;
			glGetShaderiv(hVertProg, GL_INFO_LOG_LENGTH, &info_log_length);
			buffer.Resize(info_log_length + 1);

			glGetShaderInfoLog(hVertProg, info_log_length + 1, NULL, buffer.Data());
			if (*buffer.Data())
			{
				//error << "Vertex shader:\n" << buffer.Data() << "\n";
				error << "Vertex shader:\n" << ProcessShaderError(buffer.Data(), filenames_for_error) << "\n";
			}

			errored = true;
		}

		glCompileShader(hFragProg);

		if (glGetShaderiv(hFragProg, GL_COMPILE_STATUS, &status); status == GL_FALSE)
		{
			TArray<char> buffer;
			GLint info_log_length = 1;
			glGetShaderiv(hFragProg, GL_INFO_LOG_LENGTH, &info_log_length);
			buffer.Resize(info_log_length + 1);

			glGetShaderInfoLog(hFragProg, info_log_length + 1, NULL, buffer.Data());
			if (*buffer.Data())
			{
				error << "Fragment shader:\n" << ProcessShaderError(buffer.Data(), filenames_for_error) << "\n";
			}

			errored = true;
		}

		if(errored)
		{
			// only print message if there's an error.
			I_Error("Errors Compiliong Shader '%s':\n%s\n", name, error.GetChars());
		}

		glAttachShader(hShader, hVertProg);
		glAttachShader(hShader, hFragProg);

		glLinkProgram(hShader);

		if (glGetProgramiv(hShader, GL_LINK_STATUS, &status); status == GL_FALSE)
		{
			TArray<char> buffer;
			GLint info_log_length = 1;
			glGetProgramiv(hShader, GL_INFO_LOG_LENGTH, &info_log_length);
			buffer.Resize(info_log_length + 1);

			glGetProgramInfoLog(hShader, info_log_length + 1, NULL, buffer.Data());
			if (*buffer.Data())
			{
				error << "Linking:\n" << buffer.Data() << "\n";
			}

			I_Error("Errors Linking Shader '%s':\n%s\n", name, error.GetChars());
		}
		else if (glProgramBinary && IsShaderCacheActive())
		{
			int binaryLength = 0;
			glGetProgramiv(hShader, GL_PROGRAM_BINARY_LENGTH, &binaryLength);
			binary.Resize(binaryLength);
			glGetProgramBinary(hShader, binary.Size(), &binaryLength, &binaryFormat, binary.Data());
			binary.Resize(binaryLength);
			SaveCachedProgramBinary(vp_comb, fp_comb, binary, binaryFormat);
		}
	}
	else
	{
		hVertProg = 0;
		hFragProg = 0;
	}

	muDesaturation.Init(hShader, "uDesaturationFactor");
	muFogEnabled.Init(hShader, "uFogEnabled");
	muTextureMode.Init(hShader, "uTextureMode");
	muLightParms.Init(hShader, "uLightAttr");
	muClipSplit.Init(hShader, "uClipSplit");
	muLightIndex.Init(hShader, "uLightIndex");
	muBoneIndexBase.Init(hShader, "uBoneIndexBase");
	muFogColor.Init(hShader, "uFogColor");
	muDynLightColor.Init(hShader, "uDynLightColor");
	muObjectColor.Init(hShader, "uObjectColor");
	muObjectColor2.Init(hShader, "uObjectColor2");
	muGlowBottomColor.Init(hShader, "uGlowBottomColor");
	muGlowTopColor.Init(hShader, "uGlowTopColor");
	muGlowBottomFar.Init(hShader, "uGlowBottomFar");
	muGlowTopFar.Init(hShader, "uGlowTopFar");
	muGlowBottomPlane.Init(hShader, "uGlowBottomPlane");
	muGlowTopPlane.Init(hShader, "uGlowTopPlane");
	muGlowTopFalloff.Init(hShader, "uGlowTopFalloff");
	muGlowBottomFalloff.Init(hShader, "uGlowBottomFalloff");
	muGlowTopIntensity.Init(hShader, "uGlowTopIntensity");
	muGlowBottomIntensity.Init(hShader, "uGlowBottomIntensity");
	muSweepOrigin.Init(hShader, "uSweepOrigin");
	muSweepCount.Init(hShader, "uSweepCount");
	muSweepTrail.Init(hShader, "uSweepTrail");
	muSweepBandsLoc = glGetUniformLocation(hShader, "uSweepBands");
	muSweepColorsLoc = glGetUniformLocation(hShader, "uSweepColors");
	muSweepBandOriginLoc = glGetUniformLocation(hShader, "uSweepBandOrigin");
	muFlatGlowColor.Init(hShader, "uFlatGlowColor");
	muFlatGlowFar.Init(hShader, "uFlatGlowFar");
	muFlatGlowFalloff.Init(hShader, "uFlatGlowFalloff");
	muFlatGlowIsCeiling.Init(hShader, "uFlatGlowIsCeiling");
	muDarknessExempt.Init(hShader, "uDarknessExempt");
	muFogDensityScale.Init(hShader, "uFogDensityScale");
	muOutlineColorA.Init(hShader, "uOutlineColorA");
	muOutlineColorB.Init(hShader, "uOutlineColorB");
	muOutlineParms.Init(hShader, "uOutlineParms");
	muFlatGlowLineCount.Init(hShader, "uFlatGlowLineCount");
	muFlatGlowLinesLoc = glGetUniformLocation(hShader, "uFlatGlowLines");
	muGradientBottomPlane.Init(hShader, "uGradientBottomPlane");
	muGradientTopPlane.Init(hShader, "uGradientTopPlane");
	muSplitBottomPlane.Init(hShader, "uSplitBottomPlane");
	muSplitTopPlane.Init(hShader, "uSplitTopPlane");
	muDetailParms.Init(hShader, "uDetailParms");
#ifdef NPOT_EMULATION
	muNpotEmulation.Init(hShader, "uNpotEmulation");
#endif
	muInterpolationFactor.Init(hShader, "uInterpolationFactor");
	muAlphaThreshold.Init(hShader, "uAlphaThreshold");
	muSpecularMaterial.Init(hShader, "uSpecularMaterial");
	muAddColor.Init(hShader, "uAddColor");
	muTextureAddColor.Init(hShader, "uTextureAddColor");
	muTextureModulateColor.Init(hShader, "uTextureModulateColor");
	muTextureBlendColor.Init(hShader, "uTextureBlendColor");
	muTimer.Init(hShader, "timer");
	muGlobalFadeMode.Init(hShader, "uGlobalFadeMode");
	muGlobalFade.Init(hShader, "uGlobalFade");
	muGlobalFadeDensity.Init(hShader, "uGlobalFadeDensity");
	muGlobalFadeGradient.Init(hShader, "uGlobalFadeGradient");
	muGlobalFadeColor.Init(hShader, "uGlobalFadeColor");
	muLightRangeLimit.Init(hShader, "uLightRangeLimit");

	lights_index = glGetUniformLocation(hShader, "lights");
	modelmatrix_index = glGetUniformLocation(hShader, "ModelMatrix");
	texturematrix_index = glGetUniformLocation(hShader, "TextureMatrix");
	normalmodelmatrix_index = glGetUniformLocation(hShader, "NormalModelMatrix");

	if (!lightbuffertype)
	{
		int tempindex = glGetUniformBlockIndex(hShader, "LightBufferUBO");
		if (tempindex != -1) glUniformBlockBinding(hShader, tempindex, LIGHTBUF_BINDINGPOINT);

		tempindex = glGetUniformBlockIndex(hShader, "BoneBufferUBO");
		if (tempindex != -1) glUniformBlockBinding(hShader, tempindex, BONEBUF_BINDINGPOINT);
	}
	int tempindex = glGetUniformBlockIndex(hShader, "ViewpointUBO");
	if (tempindex != -1) glUniformBlockBinding(hShader, tempindex, VIEWPOINT_BINDINGPOINT);

	glUseProgram(hShader);

	// set up other texture units (if needed by the shader)
	for (int i = 2; i<16; i++)
	{
		char stringbuf[20];
		mysnprintf(stringbuf, 20, "texture%d", i);
		tempindex = glGetUniformLocation(hShader, stringbuf);
		if (tempindex != -1) glUniform1i(tempindex, i - 1);
	}

	int shadowmapindex = glGetUniformLocation(hShader, "ShadowMap");
	if (shadowmapindex != -1) glUniform1i(shadowmapindex, 16);

	int lightmapindex = glGetUniformLocation(hShader, "LightMap");
	if (lightmapindex != -1) glUniform1i(lightmapindex, 17);

	glUseProgram(0);
	return true;
}

//==========================================================================
//
//
//
//==========================================================================

FShader::~FShader()
{
	glDeleteProgram(hShader);
	if (hVertProg != 0)
		glDeleteShader(hVertProg);
	if (hFragProg != 0)
		glDeleteShader(hFragProg);
}


//==========================================================================
//
//
//
//==========================================================================

bool FShader::Bind()
{
	GLRenderer->mShaderManager->SetActiveShader(this);
	return true;
}

//==========================================================================
//
// Since all shaders are REQUIRED, any error here needs to be fatal
//
//==========================================================================

FShader *FShaderCollection::Compile (const char *ShaderName, const char *ShaderPath, const char *LightModePath, const char *shaderdefines, bool usediscard, EPassType passType)
{
	FString defines;
	if (shaderdefines) defines += shaderdefines;
	// this can't be in the shader code due to ATI strangeness.
	if (!usediscard) defines += "#define NO_ALPHATEST\n";
	if (passType == GBUFFER_PASS) defines += "#define GBUFFER_PASS\n";

	if(gl_lite_shader)
		defines += "#define SHADER_LITE\n";

	FShader *shader = NULL;
	try
	{
		shader = new FShader(ShaderName);
		if (!shader->Load(ShaderName, "shaders/glsl/main.vp", "shaders/glsl/main.fp", ShaderPath, LightModePath, defines.GetChars()))
		{
			I_FatalError("Unable to load shader %s\n", ShaderName);
		}
	}
	catch(CRecoverableError &err)
	{
		if (shader != NULL) delete shader;
		shader = NULL;
		I_FatalError("Unable to load shader %s:\n%s\n", ShaderName, err.GetMessage());
	}
	return shader;
}

//==========================================================================
//
//
//
//==========================================================================

FShaderManager::FShaderManager()
{
	for (int passType = 0; passType < MAX_PASS_TYPES; passType++)
		mPassShaders.Push(new FShaderCollection((EPassType)passType));
}

bool FShaderManager::CompileNextShader()
{
	if (mPassShaders[mCompilePass]->CompileNextShader())
	{
		mCompilePass++;
		if (mCompilePass >= MAX_PASS_TYPES)
		{
			mCompilePass = -1;
			return true;
		}
	}
	return false;
}

FShaderManager::~FShaderManager()
{
	glUseProgram(0);
	mActiveShader = NULL;

	for (auto collection : mPassShaders)
		delete collection;
}

void FShaderManager::SetActiveShader(FShader *sh)
{
	if (mActiveShader != sh)
	{
		glUseProgram(sh!= NULL? sh->GetHandle() : 0);
		mActiveShader = sh;
	}
}

FShader *FShaderManager::BindEffect(int effect, EPassType passType)
{
	if (passType < mPassShaders.Size() && mCompilePass == -1)
		return mPassShaders[passType]->BindEffect(effect);
	else
		return nullptr;
}

FShader *FShaderManager::Get(unsigned int eff, bool alphateston, EPassType passType)
{
	if (mCompilePass > -1)
	{
		return mPassShaders[0]->Get(0, false);
	}
	if ((r_skipmats && eff >= 3 && eff <= 4))
		eff = 0;

	if (passType < mPassShaders.Size())
		return mPassShaders[passType]->Get(eff, alphateston);
	else
		return nullptr;
}

//==========================================================================
//
//
//
//==========================================================================

FShaderCollection::FShaderCollection(EPassType passType)
{
	mPassType = passType;
	mMaterialShaders.Clear();
	mMaterialShadersNAT.Clear();
	for (int i = 0; i < MAX_EFFECTS; i++)
	{
		mEffectShaders[i] = NULL;
	}
}

//==========================================================================
//
//
//
//==========================================================================

FShaderCollection::~FShaderCollection()
{
	Clean();
}

//==========================================================================
//
//
//
//==========================================================================

bool FShaderCollection::CompileNextShader()
{
	int i = mCompileIndex;
	if (mCompileState == 0)
	{
		FShader *shc = Compile(defaultshaders[i].ShaderName, defaultshaders[i].gettexelfunc, defaultshaders[i].lightfunc, defaultshaders[i].Defines, true, mPassType);
		mMaterialShaders.Push(shc);
		mCompileIndex++;
		if (defaultshaders[mCompileIndex].ShaderName == nullptr)
		{
			mCompileIndex = 0;
			mCompileState++;

		}
	}
	else if (mCompileState == 1)
	{
		FShader *shc1 = Compile(defaultshaders[i].ShaderName, defaultshaders[i].gettexelfunc, defaultshaders[i].lightfunc, defaultshaders[i].Defines, false, mPassType);
		mMaterialShadersNAT.Push(shc1);
		mCompileIndex++;
		if (mCompileIndex >= SHADER_NoTexture)
		{
			mCompileIndex = 0;
			mCompileState++;
			if (usershaders.Size() == 0 || !gl_customshader) mCompileState++;
		}
	}
	else if (mCompileState == 2)
	{
		FString name = ExtractFileBase(usershaders[i].shader.GetChars());
		FString defines = defaultshaders[usershaders[i].shaderType].Defines + usershaders[i].defines;
		FShader *shc = Compile(name.GetChars(), usershaders[i].shader.GetChars(), defaultshaders[usershaders[i].shaderType].lightfunc, defines.GetChars(), true, mPassType);
		mMaterialShaders.Push(shc);
		mCompileIndex++;
		if (mCompileIndex >= (int)usershaders.Size())
		{
			mCompileIndex = 0;
			mCompileState++;
		}
	}
	else if (mCompileState == 3)
	{
		// [GPUPARTICLES] Never loaded on GL. gpuparticles reads GpuParticleSSO,
		// a storage block only the Vulkan prolog declares, so it would fail
		// here -- and a failed GL effect is deleted silently, BindEffect then
		// returns null, and FGLRenderState::ApplyShader would take that null as
		// its active shader. Skipping keeps mEffectShaders[i] null and that path
		// unreachable; nothing on GL ever selects EFF_GPUPARTICLES.
		// [DRAWNLINES] drawnlines likewise: it reads DrawnLineSSO, Vulkan's alone.
		FShader *eff = (i == EFF_GPUPARTICLES || i == EFF_DRAWNLINES) ? nullptr : new FShader(effectshaders[i].ShaderName);
		if (eff == nullptr)
		{
		}
		else if (!eff->Load(effectshaders[i].ShaderName, effectshaders[i].vp, effectshaders[i].fp1,
						effectshaders[i].fp2, effectshaders[i].fp3, effectshaders[i].defines))
		{
			delete eff;
		}
		else mEffectShaders[i] = eff;
		mCompileIndex++;
		if (mCompileIndex >= MAX_EFFECTS)
		{
			return true;
		}
	}
	return false;
}

//==========================================================================
//
//
//
//==========================================================================

void FShaderCollection::Clean()
{
	for (unsigned int i = 0; i < mMaterialShadersNAT.Size(); i++)
	{
		if (mMaterialShadersNAT[i] != NULL) delete mMaterialShadersNAT[i];
	}
	for (unsigned int i = 0; i < mMaterialShaders.Size(); i++)
	{
		if (mMaterialShaders[i] != NULL) delete mMaterialShaders[i];
	}
	for (int i = 0; i < MAX_EFFECTS; i++)
	{
		if (mEffectShaders[i] != NULL) delete mEffectShaders[i];
		mEffectShaders[i] = NULL;
	}
	mMaterialShaders.Clear();
	mMaterialShadersNAT.Clear();
}

//==========================================================================
//
//
//
//==========================================================================

int FShaderCollection::Find(const char * shn)
{
	FName sfn = shn;

	for(unsigned int i=0;i<mMaterialShaders.Size();i++)
	{
		if (mMaterialShaders[i]->mName == sfn)
		{
			return i;
		}
	}
	return -1;
}


//==========================================================================
//
//
//
//==========================================================================

FShader *FShaderCollection::BindEffect(int effect)
{
	if (effect >= 0 && effect < MAX_EFFECTS && mEffectShaders[effect] != NULL)
	{
		mEffectShaders[effect]->Bind();
		return mEffectShaders[effect];
	}
	return NULL;
}


//==========================================================================
//
//
//
//==========================================================================

void gl_DestroyUserShaders()
{
	// todo
}

}
