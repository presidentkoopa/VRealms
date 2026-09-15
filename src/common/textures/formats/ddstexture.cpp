/*
** ddstexture.cpp
**
** Texture class for DDS images
**
**---------------------------------------------------------------------------
**
** Copyright 2006-2016 Marisa Heit
** Copyright 2006-2019 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Code written prior to 2026 is also licensed under:
**
** SPDX-License-Identifier: BSD-3-Clause
**
**---------------------------------------------------------------------------
**
** DDS is short for "DirectDraw Surface" and is essentially that. It's
** interesting to us because it is a standard file format for DXTC/S3TC
** encoded images. Look up "DDS File Reference" in the DirectX SDK or
** the online MSDN documentation to the specs for this file format. Look up
** "Compressed Texture Resources" for information about DXTC encoding.
**
** Perhaps the most important part of DXTC to realize is that every 4x4
** pixel block can only have four different colors, and only two of those
** are discrete. So depending on the texture, there may be very noticable
** quality degradation, or it may look virtually indistinguishable from
** the uncompressed texture.
**
** Note: Although this class supports reading RGB textures from a DDS,
** DO NOT use DDS images with plain RGB data. PNG does everything useful
** better. Since DDS lets the R, G, B, and A components lie anywhere in
** the pixel data, it is fairly inefficient to process.
**
** [DDS] UZDXREMA: block-compressed files (BC1, BC3, BC7) also go to the GPU
** as they are stored, with their own mip levels, when the Vulkan device has
** the format -- ported from GZSelaco 19a79ed90 (GPL v3): 9d6ab015a7,
** 490044c411, 1c5f0b120d, a46c31630a, 4e9bc832e1, 5c93e38c6c. The CPU
** decoders below stay for every other case. DDS_ClassifyCompressed says
** which files, and what their header's spare fields mean.
*/

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "files.h"
#include "filesystem.h"
#include "bitmap.h"
#include "imagehelpers.h"
#include "image.h"
#include "m_swap.h"

// Since we want this to compile under Linux too, we need to define this
// stuff ourselves instead of including a DirectX header.

// BEGIN S2 HARNESS: dds-header
enum
{
	ID_DDS = MAKE_ID('D', 'D', 'S', ' '),
	ID_DXT1 = MAKE_ID('D', 'X', 'T', '1'),
	ID_DXT2 = MAKE_ID('D', 'X', 'T', '2'),
	ID_DXT3 = MAKE_ID('D', 'X', 'T', '3'),
	ID_DXT4 = MAKE_ID('D', 'X', 'T', '4'),
	ID_DXT5 = MAKE_ID('D', 'X', 'T', '5'),
	ID_DX10 = MAKE_ID('D', 'X', '1', '0'),	// [DDS] a DDHEADERDX10 follows the surface description
	ID_BC7 = MAKE_ID('B', 'C', '7', ' '),	// [DDS] no file carries this: FDDSTexture::Format for BC7 blocks

	// Bits in dwFlags
	DDSD_CAPS = 0x00000001,
	DDSD_HEIGHT = 0x00000002,
	DDSD_WIDTH = 0x00000004,
	DDSD_PITCH = 0x00000008,
	DDSD_PIXELFORMAT = 0x00001000,
	DDSD_MIPMAPCOUNT = 0x00020000,
	DDSD_LINEARSIZE = 0x00080000,
	DDSD_DEPTH = 0x00800000,

	// Bits in ddpfPixelFormat
	DDPF_ALPHAPIXELS = 0x00000001,
	DDPF_FOURCC = 0x00000004,
	DDPF_RGB = 0x00000040,

	// Bits in DDSCAPS2.dwCaps1
	DDSCAPS_COMPLEX = 0x00000008,
	DDSCAPS_TEXTURE = 0x00001000,
	DDSCAPS_MIPMAP = 0x00400000,

	// Bits in DDSCAPS2.dwCaps2
	DDSCAPS2_CUBEMAP = 0x00000200,
	DDSCAPS2_CUBEMAP_POSITIVEX = 0x00000400,
	DDSCAPS2_CUBEMAP_NEGATIVEX = 0x00000800,
	DDSCAPS2_CUBEMAP_POSITIVEY = 0x00001000,
	DDSCAPS2_CUBEMAP_NEGATIVEY = 0x00002000,
	DDSCAPS2_CUBEMAP_POSITIVEZ = 0x00004000,
	DDSCAPS2_CUBEMAP_NEGATIZEZ = 0x00008000,
	DDSCAPS2_VOLUME = 0x00200000,

	// [DDS] Bits in DDHEADERDX10.miscFlag
	DDS_RESOURCE_MISC_TEXTURECUBE = 0x00000004,
};

//==========================================================================
//
// [DDS] The DX10 extension header, under GZSelaco's names. The enums are
// given a 32-bit underlying type so DDHEADERDX10 is exactly the 20 bytes
// on disk on every compiler.
//
//==========================================================================

enum D3D10_RESOURCE_DIMENSION_E : uint32_t {
	D3D10_RESOURCE_DIMENSION_UNKNOWN = 0,
	D3D10_RESOURCE_DIMENSION_BUFFER = 1,
	D3D10_RESOURCE_DIMENSION_TEXTURE1D = 2,
	D3D10_RESOURCE_DIMENSION_TEXTURE2D = 3,
	D3D10_RESOURCE_DIMENSION_TEXTURE3D = 4
};

enum DXGI_FORMAT_E : uint32_t {
	DXGI_FORMAT_UNKNOWN = 0,
	DXGI_FORMAT_R32G32B32A32_TYPELESS = 1,
	DXGI_FORMAT_R32G32B32A32_FLOAT = 2,
	DXGI_FORMAT_R32G32B32A32_UINT = 3,
	DXGI_FORMAT_R32G32B32A32_SINT = 4,
	DXGI_FORMAT_R32G32B32_TYPELESS = 5,
	DXGI_FORMAT_R32G32B32_FLOAT = 6,
	DXGI_FORMAT_R32G32B32_UINT = 7,
	DXGI_FORMAT_R32G32B32_SINT = 8,
	DXGI_FORMAT_R16G16B16A16_TYPELESS = 9,
	DXGI_FORMAT_R16G16B16A16_FLOAT = 10,
	DXGI_FORMAT_R16G16B16A16_UNORM = 11,
	DXGI_FORMAT_R16G16B16A16_UINT = 12,
	DXGI_FORMAT_R16G16B16A16_SNORM = 13,
	DXGI_FORMAT_R16G16B16A16_SINT = 14,
	DXGI_FORMAT_R32G32_TYPELESS = 15,
	DXGI_FORMAT_R32G32_FLOAT = 16,
	DXGI_FORMAT_R32G32_UINT = 17,
	DXGI_FORMAT_R32G32_SINT = 18,
	DXGI_FORMAT_R32G8X24_TYPELESS = 19,
	DXGI_FORMAT_D32_FLOAT_S8X24_UINT = 20,
	DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS = 21,
	DXGI_FORMAT_X32_TYPELESS_G8X24_UINT = 22,
	DXGI_FORMAT_R10G10B10A2_TYPELESS = 23,
	DXGI_FORMAT_R10G10B10A2_UNORM = 24,
	DXGI_FORMAT_R10G10B10A2_UINT = 25,
	DXGI_FORMAT_R11G11B10_FLOAT = 26,
	DXGI_FORMAT_R8G8B8A8_TYPELESS = 27,
	DXGI_FORMAT_R8G8B8A8_UNORM = 28,
	DXGI_FORMAT_R8G8B8A8_UNORM_SRGB = 29,
	DXGI_FORMAT_R8G8B8A8_UINT = 30,
	DXGI_FORMAT_R8G8B8A8_SNORM = 31,
	DXGI_FORMAT_R8G8B8A8_SINT = 32,
	DXGI_FORMAT_R16G16_TYPELESS = 33,
	DXGI_FORMAT_R16G16_FLOAT = 34,
	DXGI_FORMAT_R16G16_UNORM = 35,
	DXGI_FORMAT_R16G16_UINT = 36,
	DXGI_FORMAT_R16G16_SNORM = 37,
	DXGI_FORMAT_R16G16_SINT = 38,
	DXGI_FORMAT_R32_TYPELESS = 39,
	DXGI_FORMAT_D32_FLOAT = 40,
	DXGI_FORMAT_R32_FLOAT = 41,
	DXGI_FORMAT_R32_UINT = 42,
	DXGI_FORMAT_R32_SINT = 43,
	DXGI_FORMAT_R24G8_TYPELESS = 44,
	DXGI_FORMAT_D24_UNORM_S8_UINT = 45,
	DXGI_FORMAT_R24_UNORM_X8_TYPELESS = 46,
	DXGI_FORMAT_X24_TYPELESS_G8_UINT = 47,
	DXGI_FORMAT_R8G8_TYPELESS = 48,
	DXGI_FORMAT_R8G8_UNORM = 49,
	DXGI_FORMAT_R8G8_UINT = 50,
	DXGI_FORMAT_R8G8_SNORM = 51,
	DXGI_FORMAT_R8G8_SINT = 52,
	DXGI_FORMAT_R16_TYPELESS = 53,
	DXGI_FORMAT_R16_FLOAT = 54,
	DXGI_FORMAT_D16_UNORM = 55,
	DXGI_FORMAT_R16_UNORM = 56,
	DXGI_FORMAT_R16_UINT = 57,
	DXGI_FORMAT_R16_SNORM = 58,
	DXGI_FORMAT_R16_SINT = 59,
	DXGI_FORMAT_R8_TYPELESS = 60,
	DXGI_FORMAT_R8_UNORM = 61,
	DXGI_FORMAT_R8_UINT = 62,
	DXGI_FORMAT_R8_SNORM = 63,
	DXGI_FORMAT_R8_SINT = 64,
	DXGI_FORMAT_A8_UNORM = 65,
	DXGI_FORMAT_R1_UNORM = 66,
	DXGI_FORMAT_R9G9B9E5_SHAREDEXP = 67,
	DXGI_FORMAT_R8G8_B8G8_UNORM = 68,
	DXGI_FORMAT_G8R8_G8B8_UNORM = 69,
	DXGI_FORMAT_BC1_TYPELESS = 70,
	DXGI_FORMAT_BC1_UNORM = 71,
	DXGI_FORMAT_BC1_UNORM_SRGB = 72,
	DXGI_FORMAT_BC2_TYPELESS = 73,
	DXGI_FORMAT_BC2_UNORM = 74,
	DXGI_FORMAT_BC2_UNORM_SRGB = 75,
	DXGI_FORMAT_BC3_TYPELESS = 76,
	DXGI_FORMAT_BC3_UNORM = 77,
	DXGI_FORMAT_BC3_UNORM_SRGB = 78,
	DXGI_FORMAT_BC4_TYPELESS = 79,
	DXGI_FORMAT_BC4_UNORM = 80,
	DXGI_FORMAT_BC4_SNORM = 81,
	DXGI_FORMAT_BC5_TYPELESS = 82,
	DXGI_FORMAT_BC5_UNORM = 83,
	DXGI_FORMAT_BC5_SNORM = 84,
	DXGI_FORMAT_B5G6R5_UNORM = 85,
	DXGI_FORMAT_B5G5R5A1_UNORM = 86,
	DXGI_FORMAT_B8G8R8A8_UNORM = 87,
	DXGI_FORMAT_B8G8R8X8_UNORM = 88,
	DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM = 89,
	DXGI_FORMAT_B8G8R8A8_TYPELESS = 90,
	DXGI_FORMAT_B8G8R8A8_UNORM_SRGB = 91,
	DXGI_FORMAT_B8G8R8X8_TYPELESS = 92,
	DXGI_FORMAT_B8G8R8X8_UNORM_SRGB = 93,
	DXGI_FORMAT_BC6H_TYPELESS = 94,
	DXGI_FORMAT_BC6H_UF16 = 95,
	DXGI_FORMAT_BC6H_SF16 = 96,
	DXGI_FORMAT_BC7_TYPELESS = 97,
	DXGI_FORMAT_BC7_UNORM = 98,
	DXGI_FORMAT_BC7_UNORM_SRGB = 99,
	DXGI_FORMAT_AYUV = 100,
	DXGI_FORMAT_Y410 = 101,
	DXGI_FORMAT_Y416 = 102,
	DXGI_FORMAT_NV12 = 103,
	DXGI_FORMAT_P010 = 104,
	DXGI_FORMAT_P016 = 105,
	DXGI_FORMAT_420_OPAQUE = 106,
	DXGI_FORMAT_YUY2 = 107,
	DXGI_FORMAT_Y210 = 108,
	DXGI_FORMAT_Y216 = 109,
	DXGI_FORMAT_NV11 = 110,
	DXGI_FORMAT_AI44 = 111,
	DXGI_FORMAT_IA44 = 112,
	DXGI_FORMAT_P8 = 113,
	DXGI_FORMAT_A8P8 = 114,
	DXGI_FORMAT_B4G4R4A4_UNORM = 115,
	DXGI_FORMAT_P208 = 130,
	DXGI_FORMAT_V208 = 131,
	DXGI_FORMAT_V408 = 132,
	DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE,
	DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE,
	DXGI_FORMAT_FORCE_UINT = 0xffffffff
};

typedef struct {
	DXGI_FORMAT_E				dxgiFormat;
	D3D10_RESOURCE_DIMENSION_E	resourceDimension;
	uint32_t                 miscFlag;
	uint32_t                 arraySize;
	uint32_t                 miscFlags2;
} DDHEADERDX10;

//==========================================================================
//
//
//
//==========================================================================

struct DDPIXELFORMAT
{
	uint32_t			Size;		// Must be 32
	uint32_t			Flags;
	uint32_t			FourCC;
	uint32_t			RGBBitCount;
	uint32_t			RBitMask, GBitMask, BBitMask;
	uint32_t			RGBAlphaBitMask;
};

struct DDCAPS2
{
	uint32_t			Caps1, Caps2;
	uint32_t			Reserved[2];
};

struct DDSURFACEDESC2
{
	uint32_t			Size;		// Must be 124. DevIL claims some writers set it to 'DDS ' instead.
	uint32_t			Flags;
	uint32_t			Height;
	uint32_t			Width;
	union
	{
		int32_t		Pitch;
		uint32_t		LinearSize;
	};
	uint32_t			Depth;
	uint32_t			MipMapCount;
	// [DDS] GZSelaco keeps a sprite's offsets and translucency in the unused words (see FDDSTexture's constructor)
	union
	{
		int32_t				Offsets[11];
		uint32_t			Reserved1[11];
	};
	DDPIXELFORMAT		PixelFormat;
	DDCAPS2				Caps;
	uint32_t			Reserved2;
};

struct DDSFileHeader
{
	uint32_t			Magic;
	DDSURFACEDESC2		Desc;
};

// [DDS] The on-disk layout the compressed path and every DDS writer rely on.
static_assert(sizeof(DDSURFACEDESC2) == 124, "DDS surface description must be 124 bytes");
static_assert(sizeof(DDHEADERDX10) == 20, "DDS DX10 header must be 20 bytes");

//==========================================================================
//
// [DDS] Which files go to the GPU as they are stored, and what their header's
// spare fields mean. Kept free of the file system so it can be tested alone.
//
//   DX10 header, BC1 / BC3 / BC7 (UNORM, or the _SRGB tag on the same blocks,
//   which this engine reads like every other texture: as stored)
//       GZSelaco's conventions. Reserved1[0] and [1] (file bytes 32 and 36)
//       are the left and top offsets; Reserved1[2] (byte 40) is 0 for a
//       translucent image -- the default most exporters leave -- and 1 for
//       one that is not; BC1 is never translucent. Drawn without the sprite
//       filtering frame. A DX10 BC1 file is RGB only, as Selaco uploads it.
//   Legacy FourCC DXT1 / DXT5
//       Uploaded compressed too, but otherwise read as they always were:
//       no header offsets, translucency found from the pixels, the frame kept,
//       and DXT1's 1-bit alpha kept (BC1_RGBA). Selaco's own DXT1 files never
//       use the transparent index, so they read the same either way.
//   Everything else (DXT2 / DXT3 / DXT4, RGB, other DX10 formats)
//       Decoded on the CPU exactly as before; other DX10 formats are still
//       not recognised.
//
//==========================================================================

struct DDSCompressedInfo
{
	int vkFormat = 0;			// the VkFormat it uploads as; 0 = only decoded on the CPU
	int glFormat = 0;
	int blockSize = 0;			// bytes per 4x4 block
	uint32_t cpuFormat = 0;		// the CPU decoder for it: ID_DXT1, ID_DXT5 or ID_BC7
	bool hasDX10Header = false;	// the pixels start at byte 148, not 128
	bool headerFields = false;	// offsets and translucency come from Reserved1
	bool opaqueBC1 = false;		// the 3-colour block's fourth index is opaque black, not transparent
};

static size_t DDS_HeaderSize(bool hasDX10Header)
{
	return 4 + sizeof(DDSURFACEDESC2) + (hasDX10Header ? sizeof(DDHEADERDX10) : 0);
}

// One mip level's bytes: ceil(w/4) x ceil(h/4) blocks, never fewer than one each way.
static size_t DDS_LevelBytes(uint32_t width, uint32_t height, int blockSize)
{
	return (size_t)std::max<uint32_t>(1, (width + 3) / 4) * (size_t)std::max<uint32_t>(1, (height + 3) / 4) * (size_t)blockSize;
}

static bool DDS_ClassifyCompressed(const DDSURFACEDESC2 &surf, const DDHEADERDX10 *dx10, DDSCompressedInfo &info)
{
	info = DDSCompressedInfo();
	if (!(surf.PixelFormat.Flags & DDPF_FOURCC) || surf.Width > 65535 || surf.Height > 65535)
	{
		return false;
	}

	uint32_t dxgiFormat;
	if (surf.PixelFormat.FourCC == ID_DX10)
	{
		if (dx10 == nullptr || dx10->arraySize > 1 || dx10->resourceDimension != D3D10_RESOURCE_DIMENSION_TEXTURE2D || (dx10->miscFlag & DDS_RESOURCE_MISC_TEXTURECUBE))
		{
			return false;
		}
		info.hasDX10Header = true;
		info.headerFields = true;
		dxgiFormat = dx10->dxgiFormat;
	}
	else if (surf.PixelFormat.FourCC == ID_DXT1)
	{
		dxgiFormat = DXGI_FORMAT_BC1_UNORM;
	}
	else if (surf.PixelFormat.FourCC == ID_DXT5)
	{
		dxgiFormat = DXGI_FORMAT_BC3_UNORM;
	}
	else
	{
		return false;
	}

	switch (dxgiFormat)
	{
	case DXGI_FORMAT_BC1_UNORM:
	case DXGI_FORMAT_BC1_UNORM_SRGB:
		info.vkFormat = info.hasDX10Header ? 131 : 133;			// VK_FORMAT_BC1_RGB_UNORM_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK
		info.glFormat = info.hasDX10Header ? 0x83F0 : 0x83F1;	// GL_COMPRESSED_RGB_S3TC_DXT1_EXT : GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
		info.blockSize = 8;
		info.cpuFormat = ID_DXT1;
		info.opaqueBC1 = info.hasDX10Header;
		break;

	case DXGI_FORMAT_BC3_UNORM:
	case DXGI_FORMAT_BC3_UNORM_SRGB:
		info.vkFormat = 137;		// VK_FORMAT_BC3_UNORM_BLOCK
		info.glFormat = 0x83F3;		// GL_COMPRESSED_RGBA_S3TC_DXT5_EXT (GZSelaco writes 0x83F2, DXT3; GL only)
		info.blockSize = 16;
		info.cpuFormat = ID_DXT5;
		break;

	case DXGI_FORMAT_BC7_UNORM:
	case DXGI_FORMAT_BC7_UNORM_SRGB:
		info.vkFormat = 145;		// VK_FORMAT_BC7_UNORM_BLOCK
		info.glFormat = 0x8E8C;		// GL_COMPRESSED_RGBA_BPTC_UNORM
		info.blockSize = 16;
		info.cpuFormat = ID_BC7;
		break;

	default:
		info = DDSCompressedInfo();
		return false;
	}
	return true;
}
// END S2 HARNESS: dds-header

//==========================================================================
//
// [DDS] The classification, plus: the top level must be all there, or there
// is nothing to upload (a legacy file that is short reads as it always did).
//
//==========================================================================

static bool DDS_ReadCompressedInfo(FileReader &file, const DDSURFACEDESC2 &surf, const DDHEADERDX10 *dx10, DDSCompressedInfo &info)
{
	if (!DDS_ClassifyCompressed(surf, dx10, info))
	{
		return false;
	}
	const auto length = file.GetLength();
	if (length < 0 || (size_t)length < DDS_HeaderSize(info.hasDX10Header) + DDS_LevelBytes(surf.Width, surf.Height, info.blockSize))
	{
		info = DDSCompressedInfo();
		return false;
	}
	return true;
}

// BEGIN S2 HARNESS: bc7
//==========================================================================
//
// [DDS] BC7 (BPTC) block decoder for the CPU path: a device without BC
// support, a request that changes pixels (a translation, the sprite frame),
// and every CPU reader (software renderer, particle atlas, translucency).
// Follows the BPTC format of the Khronos Data Format Specification; each
// texel is (e0 * (64 - w) + e1 * w + 32) >> 6 per channel.
//
//==========================================================================

namespace DDS_BC7
{
	struct ModeInfo
	{
		uint8_t subsets, partitionBits, rotationBits, indexSelectionBits;
		uint8_t colorBits, alphaBits, endpointPBits, sharedPBits, indexBits, index2Bits;
	};

	static const ModeInfo Modes[8] =
	{
		{ 3, 4, 0, 0, 4, 0, 1, 0, 3, 0 },
		{ 2, 6, 0, 0, 6, 0, 0, 1, 3, 0 },
		{ 3, 6, 0, 0, 5, 0, 0, 0, 2, 0 },
		{ 2, 6, 0, 0, 7, 0, 1, 0, 2, 0 },
		{ 1, 0, 2, 1, 5, 6, 0, 0, 2, 3 },
		{ 1, 0, 2, 0, 7, 8, 0, 0, 2, 2 },
		{ 1, 0, 0, 0, 7, 7, 1, 0, 4, 0 },
		{ 2, 6, 0, 0, 5, 5, 1, 0, 2, 0 },
	};

	static const uint8_t Weights2[4] = { 0, 21, 43, 64 };
	static const uint8_t Weights3[8] = { 0, 9, 18, 27, 37, 46, 55, 64 };
	static const uint8_t Weights4[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };

	static const uint8_t Partition2[64][16] =
	{
		{ 0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1 }, { 0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,1 }, { 0,1,1,1,0,1,1,1,0,1,1,1,0,1,1,1 }, { 0,0,0,1,0,0,1,1,0,0,1,1,0,1,1,1 },
		{ 0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,1 }, { 0,0,1,1,0,1,1,1,0,1,1,1,1,1,1,1 }, { 0,0,0,1,0,0,1,1,0,1,1,1,1,1,1,1 }, { 0,0,0,0,0,0,0,1,0,0,1,1,0,1,1,1 },
		{ 0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,1 }, { 0,0,1,1,0,1,1,1,1,1,1,1,1,1,1,1 }, { 0,0,0,0,0,0,0,1,0,1,1,1,1,1,1,1 }, { 0,0,0,0,0,0,0,0,0,0,0,1,0,1,1,1 },
		{ 0,0,0,1,0,1,1,1,1,1,1,1,1,1,1,1 }, { 0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1 }, { 0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1 }, { 0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1 },
		{ 0,0,0,0,1,0,0,0,1,1,1,0,1,1,1,1 }, { 0,1,1,1,0,0,0,1,0,0,0,0,0,0,0,0 }, { 0,0,0,0,0,0,0,0,1,0,0,0,1,1,1,0 }, { 0,1,1,1,0,0,1,1,0,0,0,1,0,0,0,0 },
		{ 0,0,1,1,0,0,0,1,0,0,0,0,0,0,0,0 }, { 0,0,0,0,1,0,0,0,1,1,0,0,1,1,1,0 }, { 0,0,0,0,0,0,0,0,1,0,0,0,1,1,0,0 }, { 0,1,1,1,0,0,1,1,0,0,1,1,0,0,0,1 },
		{ 0,0,1,1,0,0,0,1,0,0,0,1,0,0,0,0 }, { 0,0,0,0,1,0,0,0,1,0,0,0,1,1,0,0 }, { 0,1,1,0,0,1,1,0,0,1,1,0,0,1,1,0 }, { 0,0,1,1,0,1,1,0,0,1,1,0,1,1,0,0 },
		{ 0,0,0,1,0,1,1,1,1,1,1,0,1,0,0,0 }, { 0,0,0,0,1,1,1,1,1,1,1,1,0,0,0,0 }, { 0,1,1,1,0,0,0,1,1,0,0,0,1,1,1,0 }, { 0,0,1,1,1,0,0,1,1,0,0,1,1,1,0,0 },
		{ 0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1 }, { 0,0,0,0,1,1,1,1,0,0,0,0,1,1,1,1 }, { 0,1,0,1,1,0,1,0,0,1,0,1,1,0,1,0 }, { 0,0,1,1,0,0,1,1,1,1,0,0,1,1,0,0 },
		{ 0,0,1,1,1,1,0,0,0,0,1,1,1,1,0,0 }, { 0,1,0,1,0,1,0,1,1,0,1,0,1,0,1,0 }, { 0,1,1,0,1,0,0,1,0,1,1,0,1,0,0,1 }, { 0,1,0,1,1,0,1,0,1,0,1,0,0,1,0,1 },
		{ 0,1,1,1,0,0,1,1,1,1,0,0,1,1,1,0 }, { 0,0,0,1,0,0,1,1,1,1,0,0,1,0,0,0 }, { 0,0,1,1,0,0,1,0,0,1,0,0,1,1,0,0 }, { 0,0,1,1,1,0,1,1,1,1,0,1,1,1,0,0 },
		{ 0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0 }, { 0,0,1,1,1,1,0,0,1,1,0,0,0,0,1,1 }, { 0,1,1,0,0,1,1,0,1,0,0,1,1,0,0,1 }, { 0,0,0,0,0,1,1,0,0,1,1,0,0,0,0,0 },
		{ 0,1,0,0,1,1,1,0,0,1,0,0,0,0,0,0 }, { 0,0,1,0,0,1,1,1,0,0,1,0,0,0,0,0 }, { 0,0,0,0,0,0,1,0,0,1,1,1,0,0,1,0 }, { 0,0,0,0,0,1,0,0,1,1,1,0,0,1,0,0 },
		{ 0,1,1,0,1,1,0,0,1,0,0,1,0,0,1,1 }, { 0,0,1,1,0,1,1,0,1,1,0,0,1,0,0,1 }, { 0,1,1,0,0,0,1,1,1,0,0,1,1,1,0,0 }, { 0,0,1,1,1,0,0,1,1,1,0,0,0,1,1,0 },
		{ 0,1,1,0,1,1,0,0,1,1,0,0,1,0,0,1 }, { 0,1,1,0,0,0,1,1,0,0,1,1,1,0,0,1 }, { 0,1,1,1,1,1,1,0,1,0,0,0,0,0,0,1 }, { 0,0,0,1,1,0,0,0,1,1,1,0,0,1,1,1 },
		{ 0,0,0,0,1,1,1,1,0,0,1,1,0,0,1,1 }, { 0,0,1,1,0,0,1,1,1,1,1,1,0,0,0,0 }, { 0,0,1,0,0,0,1,0,1,1,1,0,1,1,1,0 }, { 0,1,0,0,0,1,0,0,0,1,1,1,0,1,1,1 },
	};

	static const uint8_t Partition3[64][16] =
	{
		{ 0,0,1,1,0,0,1,1,0,2,2,1,2,2,2,2 }, { 0,0,0,1,0,0,1,1,2,2,1,1,2,2,2,1 }, { 0,0,0,0,2,0,0,1,2,2,1,1,2,2,1,1 }, { 0,2,2,2,0,0,2,2,0,0,1,1,0,1,1,1 },
		{ 0,0,0,0,0,0,0,0,1,1,2,2,1,1,2,2 }, { 0,0,1,1,0,0,1,1,0,0,2,2,0,0,2,2 }, { 0,0,2,2,0,0,2,2,1,1,1,1,1,1,1,1 }, { 0,0,1,1,0,0,1,1,2,2,1,1,2,2,1,1 },
		{ 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2 }, { 0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2 }, { 0,0,0,0,1,1,1,1,2,2,2,2,2,2,2,2 }, { 0,0,1,2,0,0,1,2,0,0,1,2,0,0,1,2 },
		{ 0,1,1,2,0,1,1,2,0,1,1,2,0,1,1,2 }, { 0,1,2,2,0,1,2,2,0,1,2,2,0,1,2,2 }, { 0,0,1,1,0,1,1,2,1,1,2,2,1,2,2,2 }, { 0,0,1,1,2,0,0,1,2,2,0,0,2,2,2,0 },
		{ 0,0,0,1,0,0,1,1,0,1,1,2,1,1,2,2 }, { 0,1,1,1,0,0,1,1,2,0,0,1,2,2,0,0 }, { 0,0,0,0,1,1,2,2,1,1,2,2,1,1,2,2 }, { 0,0,2,2,0,0,2,2,0,0,2,2,1,1,1,1 },
		{ 0,1,1,1,0,1,1,1,0,2,2,2,0,2,2,2 }, { 0,0,0,1,0,0,0,1,2,2,2,1,2,2,2,1 }, { 0,0,0,0,0,0,1,1,0,1,2,2,0,1,2,2 }, { 0,0,0,0,1,1,0,0,2,2,1,0,2,2,1,0 },
		{ 0,1,2,2,0,1,2,2,0,0,1,1,0,0,0,0 }, { 0,0,1,2,0,0,1,2,1,1,2,2,2,2,2,2 }, { 0,1,1,0,1,2,2,1,1,2,2,1,0,1,1,0 }, { 0,0,0,0,0,1,1,0,1,2,2,1,1,2,2,1 },
		{ 0,0,2,2,1,1,0,2,1,1,0,2,0,0,2,2 }, { 0,1,1,0,0,1,1,0,2,0,0,2,2,2,2,2 }, { 0,0,1,1,0,1,2,2,0,1,2,2,0,0,1,1 }, { 0,0,0,0,2,0,0,0,2,2,1,1,2,2,2,1 },
		{ 0,0,0,0,0,0,0,2,1,1,2,2,1,2,2,2 }, { 0,2,2,2,0,0,2,2,0,0,1,2,0,0,1,1 }, { 0,0,1,1,0,0,1,2,0,0,2,2,0,2,2,2 }, { 0,1,2,0,0,1,2,0,0,1,2,0,0,1,2,0 },
		{ 0,0,0,0,1,1,1,1,2,2,2,2,0,0,0,0 }, { 0,1,2,0,1,2,0,1,2,0,1,2,0,1,2,0 }, { 0,1,2,0,2,0,1,2,1,2,0,1,0,1,2,0 }, { 0,0,1,1,2,2,0,0,1,1,2,2,0,0,1,1 },
		{ 0,0,1,1,1,1,2,2,2,2,0,0,0,0,1,1 }, { 0,1,0,1,0,1,0,1,2,2,2,2,2,2,2,2 }, { 0,0,0,0,0,0,0,0,2,1,2,1,2,1,2,1 }, { 0,0,2,2,1,1,2,2,0,0,2,2,1,1,2,2 },
		{ 0,0,2,2,0,0,1,1,0,0,2,2,0,0,1,1 }, { 0,2,2,0,1,2,2,1,0,2,2,0,1,2,2,1 }, { 0,1,0,1,2,2,2,2,2,2,2,2,0,1,0,1 }, { 0,0,0,0,2,1,2,1,2,1,2,1,2,1,2,1 },
		{ 0,1,0,1,0,1,0,1,0,1,0,1,2,2,2,2 }, { 0,2,2,2,0,1,1,1,0,2,2,2,0,1,1,1 }, { 0,0,0,2,1,1,1,2,0,0,0,2,1,1,1,2 }, { 0,0,0,0,2,1,1,2,2,1,1,2,2,1,1,2 },
		{ 0,2,2,2,0,1,1,1,0,1,1,1,0,2,2,2 }, { 0,0,0,2,1,1,1,2,1,1,1,2,0,0,0,2 }, { 0,1,1,0,0,1,1,0,0,1,1,0,2,2,2,2 }, { 0,0,0,0,0,0,0,0,2,1,1,2,2,1,1,2 },
		{ 0,1,1,0,0,1,1,0,2,2,2,2,2,2,2,2 }, { 0,0,2,2,0,0,1,1,0,0,1,1,0,0,2,2 }, { 0,0,2,2,1,1,2,2,1,1,2,2,0,0,2,2 }, { 0,0,0,0,0,0,0,0,0,0,0,0,2,1,1,2 },
		{ 0,0,0,2,0,0,0,1,0,0,0,2,0,0,0,1 }, { 0,2,2,2,1,2,2,2,0,2,2,2,1,2,2,2 }, { 0,1,0,1,2,2,2,2,2,2,2,2,2,2,2,2 }, { 0,1,1,1,2,0,1,1,2,2,0,1,2,2,2,0 },
	};

	// The texel of each subset past the first whose index is stored one bit short (the first subset's is texel 0).
	static const uint8_t Anchor2[64] =
	{
		15,15,15,15,15,15,15,15, 15,15,15,15,15,15,15,15, 15, 2, 8, 2, 2, 8, 8,15,  2, 8, 2, 2, 8, 8, 2, 2,
		15,15, 6, 8, 2, 8,15,15,  2, 8, 2, 2, 2,15,15, 6,  6, 2, 6, 8,15,15, 2, 2, 15,15,15,15,15, 2, 2,15,
	};
	static const uint8_t Anchor3a[64] =
	{
		 3, 3,15,15, 8, 3,15,15,  8, 8, 6, 6, 6, 5, 3, 3,  3, 3, 8,15, 3, 3, 6,10,  5, 8, 8, 6, 8, 5,15,15,
		 8,15, 3, 5, 6,10, 8,15, 15, 3,15, 5,15,15,15,15,  3,15, 5, 5, 5, 8, 5,10,  5,10, 8,13,15,12, 3, 3,
	};
	static const uint8_t Anchor3b[64] =
	{
		15, 8, 8, 3,15,15, 3, 8, 15,15,15,15,15,15,15, 8, 15, 8,15, 3,15, 8,15, 8,  3,15, 6,10,15,15,10, 8,
		15, 3,15,10,10, 8, 9,10,  6,15, 8,15, 3, 6, 6, 8, 15, 3,15,15,15,15,15,15, 15,15,15,15, 3,15,15, 8,
	};

	static inline const uint8_t *WeightsFor(int bits)
	{
		return bits == 2 ? Weights2 : bits == 3 ? Weights3 : Weights4;
	}

	static inline uint8_t Interpolate(int e0, int e1, int weight)
	{
		return (uint8_t)((e0 * (64 - weight) + e1 * weight + 32) >> 6);
	}

	struct BitReader
	{
		const uint8_t *data;
		int pos;

		int Read(int count)
		{
			int value = 0;
			for (int i = 0; i < count; i++, pos++)
			{
				value |= ((data[pos >> 3] >> (pos & 7)) & 1) << i;
			}
			return value;
		}
	};

	// One 16-byte block to 16 RGBA texels, row by row.
	static void DecodeBlock(const uint8_t *block, uint8_t texels[16][4])
	{
		int mode = 0;
		while (mode < 8 && !(block[0] & (1 << mode)))
		{
			mode++;
		}
		if (mode == 8)
		{
			// The reserved mode: every channel zero except alpha, which is opaque (the D3D BC7 mode
			// reference, and what Pillow's decoder gives).
			for (int t = 0; t < 16; t++)
			{
				texels[t][0] = texels[t][1] = texels[t][2] = 0;
				texels[t][3] = 255;
			}
			return;
		}

		const ModeInfo &m = Modes[mode];
		BitReader bits = { block, mode + 1 };
		const int partition = bits.Read(m.partitionBits);
		const int rotation = bits.Read(m.rotationBits);
		const int indexSelection = bits.Read(m.indexSelectionBits);

		const int numEndpoints = m.subsets * 2;
		int endpoints[6][4] = {};
		for (int c = 0; c < 3; c++)
		{
			for (int e = 0; e < numEndpoints; e++)
			{
				endpoints[e][c] = bits.Read(m.colorBits);
			}
		}
		for (int e = 0; e < numEndpoints; e++)
		{
			endpoints[e][3] = bits.Read(m.alphaBits);
		}

		int colorPrecision = m.colorBits;
		int alphaPrecision = m.alphaBits;
		if (m.endpointPBits || m.sharedPBits)
		{
			for (int e = 0; e < numEndpoints; e++)
			{
				// one bit per endpoint, or one per subset shared by both of its endpoints
				const int p = (m.endpointPBits || (e & 1) == 0) ? bits.Read(1) : endpoints[e - 1][0] & 1;
				for (int c = 0; c < 4; c++)
				{
					endpoints[e][c] = (endpoints[e][c] << 1) | p;
				}
			}
			colorPrecision++;
			if (m.alphaBits) alphaPrecision++;
		}

		for (int e = 0; e < numEndpoints; e++)
		{
			for (int c = 0; c < 3; c++)
			{
				const int v = endpoints[e][c] << (8 - colorPrecision);
				endpoints[e][c] = v | (v >> colorPrecision);
			}
			if (m.alphaBits)
			{
				const int v = endpoints[e][3] << (8 - alphaPrecision);
				endpoints[e][3] = v | (v >> alphaPrecision);
			}
			else
			{
				endpoints[e][3] = 255;
			}
		}

		int subset[16] = {};
		bool anchor[16] = {};
		anchor[0] = true;
		for (int t = 0; t < 16; t++)
		{
			if (m.subsets == 2)
			{
				subset[t] = Partition2[partition][t];
			}
			else if (m.subsets == 3)
			{
				subset[t] = Partition3[partition][t];
			}
		}
		if (m.subsets == 2)
		{
			anchor[Anchor2[partition]] = true;
		}
		else if (m.subsets == 3)
		{
			anchor[Anchor3a[partition]] = true;
			anchor[Anchor3b[partition]] = true;
		}

		int index[16] = {}, index2[16] = {};
		for (int t = 0; t < 16; t++)
		{
			index[t] = bits.Read(m.indexBits - (anchor[t] ? 1 : 0));
		}
		if (m.index2Bits)
		{
			for (int t = 0; t < 16; t++)
			{
				index2[t] = bits.Read(m.index2Bits - (t == 0 ? 1 : 0));
			}
		}

		for (int t = 0; t < 16; t++)
		{
			const int *e0 = endpoints[subset[t] * 2];
			const int *e1 = endpoints[subset[t] * 2 + 1];
			uint8_t *out = texels[t];

			if (m.index2Bits == 0)
			{
				const uint8_t *weights = WeightsFor(m.indexBits);
				for (int c = 0; c < 4; c++)
				{
					out[c] = Interpolate(e0[c], e1[c], weights[index[t]]);
				}
			}
			else
			{
				// Modes 4 and 5: one index set for colour and one for alpha; the selection bit swaps them.
				int colorIndex = index[t], alphaIndex = index2[t];
				int colorIndexBits = m.indexBits, alphaIndexBits = m.index2Bits;
				if (indexSelection)
				{
					std::swap(colorIndex, alphaIndex);
					std::swap(colorIndexBits, alphaIndexBits);
				}
				const uint8_t *colorWeights = WeightsFor(colorIndexBits);
				const uint8_t *alphaWeights = WeightsFor(alphaIndexBits);
				for (int c = 0; c < 3; c++)
				{
					out[c] = Interpolate(e0[c], e1[c], colorWeights[colorIndex]);
				}
				out[3] = Interpolate(e0[3], e1[3], alphaWeights[alphaIndex]);
			}

			switch (rotation)
			{
			case 1: std::swap(out[0], out[3]); break;
			case 2: std::swap(out[1], out[3]); break;
			case 3: std::swap(out[2], out[3]); break;
			default: break;
			}
		}
	}
}
// END S2 HARNESS: bc7

//==========================================================================
//
// A DDS image, with DXTx compression
//
//==========================================================================

class FDDSTexture : public FImageSource
{
	enum
	{
		PIX_Palette = 0,
		PIX_Alphatex = 1,
		PIX_ARGB = 2
	};
public:
	FDDSTexture (FileReader &lump, int lumpnum, void *surfdesc, void *dx10header);

	PalettedPixels CreatePalettedPixels(int conversion, int frame = 0) override;

	// [DDS] GZSelaco's names; see image.h
	int ReadCompressedPixels(FileReader* reader, unsigned char** data, size_t& size, size_t& unitSize, int& mipLevels) override;
	bool IsGPUOnly() override { return vkFormat != 0; }
	int getGLFormat() const override { return glFormat; }
	int getVKFormat() const override { return vkFormat; }
	bool CanExpandSprite() override { return !hasDX10Header; }
	bool HasPremultipliedAlpha() override { return premultipliedAlpha; }	// [ATLASBC7] see image.h
	int GetStoredMipLevels() override { return vkFormat != 0 ? storedMips : 0; }	// [ATLASBC7] the header's MipMapCount

protected:
	uint32_t Format;

	uint32_t RMask, GMask, BMask, AMask;
	uint8_t RShiftL, GShiftL, BShiftL, AShiftL;
	uint8_t RShiftR, GShiftR, BShiftR, AShiftR;

	int32_t Pitch;
	uint32_t LinearSize;

	// [DDS] The compressed upload (all zero / false for a file only decoded on the CPU).
	int32_t BlockSize = 0;		// bytes per 4x4 block
	uint8_t storedMips = 0;		// the header's MipMapCount
	bool hasDX10Header = false;	// the pixels start at byte 148
	bool opaqueBC1 = false;		// DX10 BC1: decode the 3-colour block's fourth index as opaque black, as the GPU does
	bool premultipliedAlpha = false;	// [ATLASBC7] the DX10 alpha mode is DDS_ALPHA_MODE_PREMULTIPLIED
	int glFormat = 0;
	int vkFormat = 0;

	static void CalcBitShift (uint32_t mask, uint8_t *lshift, uint8_t *rshift);

	void ReadRGB (FileReader &lump, uint8_t *buffer, int pixelmode);
	void DecompressDXT1 (FileReader &lump, uint8_t *buffer, int pixelmode);
	void DecompressDXT3 (FileReader &lump, bool premultiplied, uint8_t *buffer, int pixelmode);
	void DecompressDXT5 (FileReader &lump, bool premultiplied, uint8_t *buffer, int pixelmode);
	void DecompressBC7 (FileReader &lump, uint8_t *buffer, int pixelmode);	// [DDS]

	int CopyPixels(FBitmap *bmp, int conversion, int frame = 0) override;

	friend class FTexture;
};


//==========================================================================
//
//
//
//==========================================================================

static bool CheckDDS (FileReader &file)
{
	DDSFileHeader Header;

	file.Seek(0, FileReader::SeekSet);
	if (file.Read (&Header, sizeof(Header)) != sizeof(Header))
	{
		return false;
	}
	return Header.Magic == ID_DDS &&
		(LittleLong(Header.Desc.Size) == sizeof(DDSURFACEDESC2) || Header.Desc.Size == ID_DDS) &&
		LittleLong(Header.Desc.PixelFormat.Size) == sizeof(DDPIXELFORMAT) &&
		(LittleLong(Header.Desc.Flags) & (DDSD_CAPS | DDSD_PIXELFORMAT | DDSD_WIDTH | DDSD_HEIGHT)) == (DDSD_CAPS | DDSD_PIXELFORMAT | DDSD_WIDTH | DDSD_HEIGHT) &&
		Header.Desc.Width != 0 &&
		Header.Desc.Height != 0;
}

//==========================================================================
//
//
//
//==========================================================================

FImageSource *DDSImage_TryCreate (FileReader &data, int lumpnum)
{
	union
	{
		DDSURFACEDESC2	surfdesc;
		uint32_t			byteswapping[sizeof(DDSURFACEDESC2) / 4];
	};
	// [DDS] GZSelaco 9d6ab015a7
	union
	{
		DDHEADERDX10	dx10header;
		uint32_t		dx10Byteswapping[sizeof(DDHEADERDX10) / 4];
	};

	if (!CheckDDS(data)) return NULL;

	data.Seek(4, FileReader::SeekSet);
	data.Read (&surfdesc, sizeof(surfdesc));

#ifdef __BIG_ENDIAN__
	// Every single element of the header is a uint32_t
	for (unsigned int i = 0; i < sizeof(DDSURFACEDESC2) / 4; ++i)
	{
		byteswapping[i] = LittleLong(byteswapping[i]);
	}
	// Undo the byte swap for the pixel format
	surfdesc.PixelFormat.FourCC = LittleLong(surfdesc.PixelFormat.FourCC);
#endif

	// [DDS] A DX10 file names its format in the extension header after the surface description.
	memset(&dx10header, 0, sizeof(dx10header));
	const bool hasDX10 = (surfdesc.PixelFormat.Flags & DDPF_FOURCC) && surfdesc.PixelFormat.FourCC == ID_DX10;
	if (hasDX10)
	{
		if ((size_t)data.Read (&dx10header, sizeof(dx10header)) != sizeof(dx10header))
		{
			return NULL;
		}
#ifdef __BIG_ENDIAN__
		// Every element of the DX10 header is a uint32_t too
		for (unsigned int i = 0; i < sizeof(DDHEADERDX10) / 4; ++i)
		{
			dx10Byteswapping[i] = LittleLong(dx10Byteswapping[i]);
		}
#endif
	}

	DDSCompressedInfo compressed;
	if (DDS_ReadCompressedInfo(data, surfdesc, hasDX10 ? &dx10header : nullptr, compressed))
	{
		return new FDDSTexture (data, lumpnum, &surfdesc, &dx10header);
	}
	if (hasDX10)
	{
		// Not BC1, BC3 or BC7, or not all there: not recognised, as before.
		return NULL;
	}

	if (surfdesc.PixelFormat.Flags & DDPF_FOURCC)
	{
		// Check for supported FourCC
		if (surfdesc.PixelFormat.FourCC != ID_DXT1 &&
			surfdesc.PixelFormat.FourCC != ID_DXT2 &&
			surfdesc.PixelFormat.FourCC != ID_DXT3 &&
			surfdesc.PixelFormat.FourCC != ID_DXT4 &&
			surfdesc.PixelFormat.FourCC != ID_DXT5)
		{
			return NULL;
		}
		if (!(surfdesc.Flags & DDSD_LINEARSIZE))
		{
			return NULL;
		}
	}
	else if (surfdesc.PixelFormat.Flags & DDPF_RGB)
	{
		if ((surfdesc.PixelFormat.RGBBitCount >> 3) < 1 ||
			(surfdesc.PixelFormat.RGBBitCount >> 3) > 4)
		{
			return NULL;
		}
		if ((surfdesc.Flags & DDSD_PITCH) && (surfdesc.Pitch <= 0))
		{
			return NULL;
		}
	}
	else
	{
		return NULL;
	}
	return new FDDSTexture (data, lumpnum, &surfdesc, nullptr);
}

//==========================================================================
//
//
//
//==========================================================================

FDDSTexture::FDDSTexture (FileReader &lump, int lumpnum, void *vsurfdesc, void *vdx10header)
: FImageSource(lumpnum)
{
	DDSURFACEDESC2 *surf = (DDSURFACEDESC2 *)vsurfdesc;
	DDHEADERDX10 *dx10 = (DDHEADERDX10 *)vdx10header;
	DDSCompressedInfo compressed;

	bMasked = false;
	Width = uint16_t(surf->Width);
	Height = uint16_t(surf->Height);

	if (DDS_ReadCompressedInfo(lump, *surf, dx10, compressed))
	{
		// [DDS] GZSelaco 9d6ab015a7, 1c5f0b120d: the stored blocks and levels go to the GPU as they are.
		Format = compressed.cpuFormat;
		Pitch = 0;
		LinearSize = surf->LinearSize;
		BlockSize = compressed.blockSize;
		vkFormat = compressed.vkFormat;
		glFormat = compressed.glFormat;
		hasDX10Header = compressed.hasDX10Header;
		opaqueBC1 = compressed.opaqueBC1;
		storedMips = (uint8_t)std::min<uint32_t>(surf->MipMapCount, 255);
		// [ATLASBC7] The DX10 header's alpha mode, its miscFlags2's low three bits: DDS_ALPHA_MODE_PREMULTIPLIED (2) says the
		// colour is stored multiplied by alpha. Nothing read it before; the compressed particle atlas takes such frames as they are.
		premultipliedAlpha = compressed.hasDX10Header && dx10 != nullptr && (dx10->miscFlags2 & 7) == 2;

		if (compressed.headerFields)
		{
			// GZSelaco's convention, set by its Offsetter tool: most exporters leave the word at zero
			// and translucency is the default, so 0 = translucent and 1 = not. BC1 has no alpha to be
			// translucent with.
			bTranslucent = BlockSize == 8 ? false : surf->Offsets[2] == 0;
			SetOffsets(surf->Offsets[0], surf->Offsets[1]);
		}
	}
	else if (surf->PixelFormat.Flags & DDPF_FOURCC)
	{
		Format = surf->PixelFormat.FourCC;
		Pitch = 0;
		LinearSize = surf->LinearSize;
	}
	else	// DDPF_RGB
	{
		Format = surf->PixelFormat.RGBBitCount >> 3;
		CalcBitShift (RMask = surf->PixelFormat.RBitMask, &RShiftL, &RShiftR);
		CalcBitShift (GMask = surf->PixelFormat.GBitMask, &GShiftL, &GShiftR);
		CalcBitShift (BMask = surf->PixelFormat.BBitMask, &BShiftL, &BShiftR);
		if (surf->PixelFormat.Flags & DDPF_ALPHAPIXELS)
		{
			CalcBitShift (AMask = surf->PixelFormat.RGBAlphaBitMask, &AShiftL, &AShiftR);
		}
		else
		{
			AMask = 0;
			AShiftL = AShiftR = 0;
		}
		if (surf->Flags & DDSD_PITCH)
		{
			Pitch = surf->Pitch;
		}
		else
		{
			Pitch = (Width * Format + 3) & ~3;
		}
		LinearSize = Pitch * Height;
	}
}

//==========================================================================
//
// [DDS] GZSelaco 490044c411, 1c5f0b120d: every stored level, top first, as
// the GPU takes it. The top level's size is worked out from the dimensions,
// since LinearSize is often wrong in files without a DX10 header.
//
//==========================================================================

int FDDSTexture::ReadCompressedPixels(FileReader* reader, unsigned char** data, size_t& size, size_t& unitSize, int& mipLevels)
{
	*data = nullptr;
	size = 0;
	unitSize = vkFormat != 0 ? DDS_LevelBytes(Width, Height, BlockSize) : 0;
	mipLevels = storedMips;

	if (vkFormat == 0 || reader == nullptr)
	{
		return 0;
	}

	const size_t headerSize = DDS_HeaderSize(hasDX10Header);
	const auto lumpSize = reader->GetLength();
	if (lumpSize < 0 || (size_t)lumpSize < headerSize + unitSize)
	{
		return 0;
	}

	const size_t pixelDataSize = (size_t)lumpSize - headerSize;
	unsigned char *pixels = (unsigned char *)malloc(pixelDataSize);
	if (pixels == nullptr)
	{
		return 0;
	}

	// The reader starts at the top of the lump and may be a decompressor stream (a deflated zip/pk3 entry), which cannot
	// seek and throws if asked. Step over the header by reading it, and treat a stream error as an unreadable lump.
	try
	{
		uint8_t skip[256];
		size_t toSkip = headerSize;
		while (toSkip > 0)
		{
			const size_t chunk = std::min(toSkip, sizeof(skip));
			if ((size_t)reader->Read(skip, chunk) != chunk)
			{
				free(pixels);
				return 0;
			}
			toSkip -= chunk;
		}
		if ((size_t)reader->Read(pixels, pixelDataSize) != pixelDataSize)
		{
			free(pixels);
			return 0;
		}
	}
	catch (...)
	{
		free(pixels);
		return 0;
	}

	*data = pixels;
	size = pixelDataSize;
	return (int)bTranslucent;
}

//==========================================================================
//
// Returns the number of bits the color must be shifted to produce
// an 8-bit value, as in:
//
// c   = (color & mask) << lshift;
// c  |= c >> rshift;
// c >>= 24;
//
// For any color of at least 4 bits, this ensures that the result
// of the calculation for c will be fully saturated, given a maximum
// value for the input bit mask.
//
//==========================================================================

void FDDSTexture::CalcBitShift (uint32_t mask, uint8_t *lshiftp, uint8_t *rshiftp)
{
	uint8_t shift;

	if (mask == 0)
	{
		*lshiftp = *rshiftp = 0;
		return;
	}

	shift = 0;
	while ((mask & 0x80000000) == 0)
	{
		mask <<= 1;
		shift++;
	}
	*lshiftp = shift;

	shift = 0;
	while (mask & 0x80000000)
	{
		mask <<= 1;
		shift++;
	}
	*rshiftp = shift;
}

//==========================================================================
//
//
//
//==========================================================================

PalettedPixels FDDSTexture::CreatePalettedPixels(int conversion, int frame)
{
	auto lump = fileSystem.OpenFileReader (SourceLump);

	PalettedPixels Pixels(Width*Height);

	lump.Seek (DDS_HeaderSize(hasDX10Header), FileReader::SeekSet);

	int pmode = conversion == luminance ? PIX_Alphatex : PIX_Palette;
	if (Format >= 1 && Format <= 4)		// RGB: Format is # of bytes per pixel
	{
		ReadRGB (lump, Pixels.Data(), pmode);
	}
	else if (Format == ID_DXT1)
	{
		DecompressDXT1 (lump, Pixels.Data(), pmode);
	}
	else if (Format == ID_DXT3 || Format == ID_DXT2)
	{
		DecompressDXT3 (lump, Format == ID_DXT2, Pixels.Data(), pmode);
	}
	else if (Format == ID_DXT5 || Format == ID_DXT4)
	{
		DecompressDXT5 (lump, Format == ID_DXT4, Pixels.Data(), pmode);
	}
	else if (Format == ID_BC7)
	{
		DecompressBC7 (lump, Pixels.Data(), pmode);
	}
	return Pixels;
}

//==========================================================================
//
// Note that pixel size == 8 is column-major, but 32 is row-major!
//
//==========================================================================

void FDDSTexture::ReadRGB (FileReader &lump, uint8_t *buffer, int pixelmode)
{
	uint32_t x, y;
	uint32_t amask = AMask == 0 ? 0 : 0x80000000 >> AShiftL;
	uint8_t *linebuff = new uint8_t[Pitch];

	for (y = Height; y > 0; --y)
	{
		uint8_t *buffp = linebuff;
		uint8_t *pixelp = pixelmode == PIX_ARGB ? buffer + 4 * (y - 1)*Width : buffer + y - 1;
		lump.Read (linebuff, Pitch);
		for (x = Width; x > 0; --x)
		{
			uint32_t c;
			if (Format == 4)
			{
				c = LittleLong(*(uint32_t *)buffp); buffp += 4;
			}
			else if (Format == 2)
			{
				c = LittleShort(*(uint16_t *)buffp); buffp += 2;
			}
			else if (Format == 3)
			{
				c = buffp[0] | (buffp[1] << 8) | (buffp[2] << 16); buffp += 3;
			}
			else //  Format == 1
			{
				c = *buffp++;
			}
			if (pixelmode != PIX_ARGB)
			{
				if (amask == 0 || (c & amask))
				{
					uint32_t r = (c & RMask) << RShiftL; r |= r >> RShiftR;
					uint32_t g = (c & GMask) << GShiftL; g |= g >> GShiftR;
					uint32_t b = (c & BMask) << BShiftL; b |= b >> BShiftR;
					uint32_t a = (c & AMask) << AShiftL; a |= a >> AShiftR;
					*pixelp = ImageHelpers::RGBToPalette(pixelmode == PIX_Alphatex, r >> 24, g >> 24, b >> 24, a >> 24);
				}
				else
				{
					*pixelp = 0;
					bMasked = true;
				}
				pixelp += Height;
			}
			else
			{
				uint32_t r = (c & RMask) << RShiftL; r |= r >> RShiftR;
				uint32_t g = (c & GMask) << GShiftL; g |= g >> GShiftR;
				uint32_t b = (c & BMask) << BShiftL; b |= b >> BShiftR;
				uint32_t a = (c & AMask) << AShiftL; a |= a >> AShiftR;
				pixelp[0] = (uint8_t)(b>>24);
				pixelp[1] = (uint8_t)(g>>24);
				pixelp[2] = (uint8_t)(r>>24);
				pixelp[3] = (uint8_t)(a>>24);
				pixelp+=4;
			}
		}
	}
	delete[] linebuff;
}

//==========================================================================
//
//
//
//==========================================================================

void FDDSTexture::DecompressDXT1 (FileReader &lump, uint8_t *buffer, int pixelmode)
{
	const long blocklinelen = ((Width + 3) >> 2) << 3;
	uint8_t *blockbuff = new uint8_t[blocklinelen];
	uint8_t *block;
	PalEntry color[4];
	uint8_t palcol[4] = { 0,0,0,0 };	// shut up compiler warnings.
	int ox, oy, x, y, i;

	color[0].a = 255;
	color[1].a = 255;
	color[2].a = 255;

	for (oy = 0; oy < Height; oy += 4)
	{
		lump.Read (blockbuff, blocklinelen);
		block = blockbuff;
		for (ox = 0; ox < Width; ox += 4)
		{
			uint16_t color16[2] = { LittleShort(((uint16_t *)block)[0]), LittleShort(((uint16_t *)block)[1]) };

			// Convert color from R5G6B5 to R8G8B8.
			for (i = 1; i >= 0; --i)
			{
				color[i].r = ((color16[i] & 0xF800) >> 8) | (color16[i] >> 13);
				color[i].g = ((color16[i] & 0x07E0) >> 3) | ((color16[i] & 0x0600) >> 9);
				color[i].b = ((color16[i] & 0x001F) << 3) | ((color16[i] & 0x001C) >> 2);
			}
			if (color16[0] > color16[1])
			{ // Four-color block: derive the other two colors.
				color[2].r = (color[0].r + color[0].r + color[1].r + 1) / 3;
				color[2].g = (color[0].g + color[0].g + color[1].g + 1) / 3;
				color[2].b = (color[0].b + color[0].b + color[1].b + 1) / 3;

				color[3].r = (color[0].r + color[1].r + color[1].r + 1) / 3;
				color[3].g = (color[0].g + color[1].g + color[1].g + 1) / 3;
				color[3].b = (color[0].b + color[1].b + color[1].b + 1) / 3;
				color[3].a = 255;
			}
			else
			{ // Three-color block: derive the other color.
				color[2].r = (color[0].r + color[1].r) / 2;
				color[2].g = (color[0].g + color[1].g) / 2;
				color[2].b = (color[0].b + color[1].b) / 2;

				color[3].a = color[3].b = color[3].g = color[3].r = 0;

				if (opaqueBC1)
				{
					// [DDS] A DX10 BC1 file is RGB only, as the GPU reads it: the fourth colour is opaque black.
					color[3].a = 255;
				}
				else
				{
					// If you have a three-color block, presumably that transparent
					// color is going to be used.
					bMasked = true;
				}
			}
			// Pick colors from the palette for each of the four colors.
			if (pixelmode != PIX_ARGB) for (i = 3; i >= 0; --i)
			{
				palcol[i] = ImageHelpers::RGBToPalette(pixelmode == PIX_Alphatex, color[i]);
			}
			// Now decode this 4x4 block to the pixel buffer.
			for (y = 0; y < 4; ++y)
			{
				if (oy + y >= Height)
				{
					break;
				}
				uint8_t yslice = block[4 + y];
				for (x = 0; x < 4; ++x)
				{
					if (ox + x >= Width)
					{
						break;
					}
					int ci = (yslice >> (x + x)) & 3;
					if (pixelmode != PIX_ARGB)
					{
						buffer[oy + y + (ox + x) * Height] = palcol[ci];
					}
					else
					{
						uint8_t * tcp = &buffer[(ox + x)*4 + (oy + y) * Width*4];
						tcp[0] = color[ci].b;
						tcp[1] = color[ci].g;
						tcp[2] = color[ci].r;
						tcp[3] = color[ci].a;
					}
				}
			}
			block += 8;
		}
	}
	delete[] blockbuff;
}

//==========================================================================
//
// DXT3: Decompression is identical to DXT1, except every 64-bit block is
// preceded by another 64-bit block with explicit alpha values.
//
//==========================================================================

void FDDSTexture::DecompressDXT3 (FileReader &lump, bool premultiplied, uint8_t *buffer, int pixelmode)
{
	const long blocklinelen = ((Width + 3) >> 2) << 4;
	uint8_t *blockbuff = new uint8_t[blocklinelen];
	uint8_t *block;
	PalEntry color[4];
	uint8_t palcol[4] = { 0,0,0,0 };
	int ox, oy, x, y, i;

	for (oy = 0; oy < Height; oy += 4)
	{
		lump.Read (blockbuff, blocklinelen);
		block = blockbuff;
		for (ox = 0; ox < Width; ox += 4)
		{
			uint16_t color16[2] = { LittleShort(((uint16_t *)block)[4]), LittleShort(((uint16_t *)block)[5]) };

			// Convert color from R5G6B5 to R8G8B8.
			for (i = 1; i >= 0; --i)
			{
				color[i].r = ((color16[i] & 0xF800) >> 8) | (color16[i] >> 13);
				color[i].g = ((color16[i] & 0x07E0) >> 3) | ((color16[i] & 0x0600) >> 9);
				color[i].b = ((color16[i] & 0x001F) << 3) | ((color16[i] & 0x001C) >> 2);
			}
			// Derive the other two colors.
			color[2].r = (color[0].r + color[0].r + color[1].r + 1) / 3;
			color[2].g = (color[0].g + color[0].g + color[1].g + 1) / 3;
			color[2].b = (color[0].b + color[0].b + color[1].b + 1) / 3;

			color[3].r = (color[0].r + color[1].r + color[1].r + 1) / 3;
			color[3].g = (color[0].g + color[1].g + color[1].g + 1) / 3;
			color[3].b = (color[0].b + color[1].b + color[1].b + 1) / 3;

			// Pick colors from the palette for each of the four colors.
			if (pixelmode != PIX_ARGB) for (i = 3; i >= 0; --i)
			{
				palcol[i] = ImageHelpers::RGBToPalette(pixelmode == PIX_Alphatex, color[i], false);
			}

			// Now decode this 4x4 block to the pixel buffer.
			for (y = 0; y < 4; ++y)
			{
				if (oy + y >= Height)
				{
					break;
				}
				uint8_t yslice = block[12 + y];
				uint16_t yalphaslice = LittleShort(((uint16_t *)block)[y]);
				for (x = 0; x < 4; ++x)
				{
					if (ox + x >= Width)
					{
						break;
					}
					if (pixelmode == PIX_Palette)
					{
						buffer[oy + y + (ox + x) * Height] = ((yalphaslice >> (x*4)) & 15) < 8 ?
							(bMasked = true, 0) : palcol[(yslice >> (x + x)) & 3];
					}
					else if (pixelmode == PIX_Alphatex)
					{
						int alphaval = ((yalphaslice >> (x * 4)) & 15);
						int palval = palcol[(yslice >> (x + x)) & 3];
						buffer[oy + y + (ox + x) * Height] = palval * alphaval / 15;
					}
					else
					{
						uint8_t * tcp = &buffer[(ox + x)*4 + (oy + y) * Width*4];
						int c = (yslice >> (x + x)) & 3;
						tcp[0] = color[c].b;
						tcp[1] = color[c].g;
						tcp[2] = color[c].r;
						tcp[3] = ((yalphaslice >> (x * 4)) & 15) * 0x11;
					}
				}
			}
			block += 16;
		}
	}
	delete[] blockbuff;
}

//==========================================================================
//
// DXT5: Decompression is identical to DXT3, except every 64-bit alpha block
// contains interpolated alpha values, similar to the 64-bit color block.
//
//==========================================================================

void FDDSTexture::DecompressDXT5 (FileReader &lump, bool premultiplied, uint8_t *buffer, int pixelmode)
{
	const size_t blocklinelen = ((Width + 3) >> 2) << 4;
	uint8_t *blockbuff = new uint8_t[blocklinelen];
	uint8_t *block;
	PalEntry color[4];
	uint8_t palcol[4] = { 0,0,0,0 };
	uint32_t yalphaslice = 0;
	int ox, oy, x, y, i;

	for (oy = 0; oy < Height; oy += 4)
	{
		lump.Read (blockbuff, blocklinelen);
		block = blockbuff;
		for (ox = 0; ox < Width; ox += 4)
		{
			uint16_t color16[2] = { LittleShort(((uint16_t *)block)[4]), LittleShort(((uint16_t *)block)[5]) };
			uint8_t alpha[8];

			// Calculate the eight alpha values.
			alpha[0] = block[0];
			alpha[1] = block[1];

			if (alpha[0] >= alpha[1])
			{ // Eight-alpha block: derive the other six alphas.
				for (i = 0; i < 6; ++i)
				{
					alpha[i + 2] = ((6 - i) * alpha[0] + (i + 1) * alpha[1] + 3) / 7;
				}
			}
			else
			{ // Six-alpha block: derive the other four alphas.
				for (i = 0; i < 4; ++i)
				{
					alpha[i + 2] = ((4 - i) * alpha[0] + (i + 1) * alpha[1] + 2) / 5;
				}
				alpha[6] = 0;
				alpha[7] = 255;
			}

			// Convert color from R5G6B5 to R8G8B8.
			for (i = 1; i >= 0; --i)
			{
				color[i].r = ((color16[i] & 0xF800) >> 8) | (color16[i] >> 13);
				color[i].g = ((color16[i] & 0x07E0) >> 3) | ((color16[i] & 0x0600) >> 9);
				color[i].b = ((color16[i] & 0x001F) << 3) | ((color16[i] & 0x001C) >> 2);
			}
			// Derive the other two colors.
			color[2].r = (color[0].r + color[0].r + color[1].r + 1) / 3;
			color[2].g = (color[0].g + color[0].g + color[1].g + 1) / 3;
			color[2].b = (color[0].b + color[0].b + color[1].b + 1) / 3;

			color[3].r = (color[0].r + color[1].r + color[1].r + 1) / 3;
			color[3].g = (color[0].g + color[1].g + color[1].g + 1) / 3;
			color[3].b = (color[0].b + color[1].b + color[1].b + 1) / 3;

			// Pick colors from the palette for each of the four colors.
			if (pixelmode != PIX_ARGB) for (i = 3; i >= 0; --i)
			{
				palcol[i] = ImageHelpers::RGBToPalette(pixelmode == PIX_Alphatex, color[i], false);
			}
			// Now decode this 4x4 block to the pixel buffer.
			for (y = 0; y < 4; ++y)
			{
				if (oy + y >= Height)
				{
					break;
				}
				// Alpha values are stored in 3 bytes for 2 rows
				if ((y & 1) == 0)
				{
					yalphaslice = block[y*3] | (block[y*3+1] << 8) | (block[y*3+2] << 16);
				}
				else
				{
					yalphaslice >>= 12;
				}
				uint8_t yslice = block[12 + y];
				for (x = 0; x < 4; ++x)
				{
					if (ox + x >= Width)
					{
						break;
					}
					if (pixelmode == PIX_Palette)
					{
						buffer[oy + y + (ox + x) * Height] = alpha[((yalphaslice >> (x*3)) & 7)] < 128 ?
							(bMasked = true, 0) : palcol[(yslice >> (x + x)) & 3];
					}
					else if (pixelmode == PIX_Alphatex)
					{
						int alphaval = alpha[((yalphaslice >> (x * 3)) & 7)];
						int palval = palcol[(yslice >> (x + x)) & 3];
						buffer[oy + y + (ox + x) * Height] = palval * alphaval / 255;
					}
					else
					{
						uint8_t * tcp = &buffer[(ox + x)*4 + (oy + y) * Width*4];
						int c = (yslice >> (x + x)) & 3;
						tcp[0] = color[c].b;
						tcp[1] = color[c].g;
						tcp[2] = color[c].r;
						tcp[3] = alpha[((yalphaslice >> (x*3)) & 7)];
					}
				}
			}
			block += 16;
		}
	}
	delete[] blockbuff;
}

//==========================================================================
//
// [DDS] BC7: every block decoded to 16 RGBA texels (DDS_BC7::DecodeBlock),
// then written the way the DXT5 path writes them.
//
//==========================================================================

void FDDSTexture::DecompressBC7 (FileReader &lump, uint8_t *buffer, int pixelmode)
{
	const size_t blocklinelen = ((Width + 3) >> 2) << 4;
	uint8_t *blockbuff = new uint8_t[blocklinelen];
	uint8_t texels[16][4];

	for (int oy = 0; oy < Height; oy += 4)
	{
		// A short read leaves zeros: reserved-mode blocks, which decode to opaque black, never to stale data.
		memset(blockbuff, 0, blocklinelen);
		lump.Read (blockbuff, blocklinelen);
		const uint8_t *block = blockbuff;
		for (int ox = 0; ox < Width; ox += 4, block += 16)
		{
			DDS_BC7::DecodeBlock(block, texels);
			for (int y = 0; y < 4 && oy + y < Height; ++y)
			{
				for (int x = 0; x < 4 && ox + x < Width; ++x)
				{
					const uint8_t *c = texels[y * 4 + x];
					if (pixelmode == PIX_Palette)
					{
						buffer[oy + y + (ox + x) * Height] = c[3] < 128 ?
							(bMasked = true, 0) : ImageHelpers::RGBToPalette(false, c[0], c[1], c[2]);
					}
					else if (pixelmode == PIX_Alphatex)
					{
						buffer[oy + y + (ox + x) * Height] = ImageHelpers::RGBToPalette(true, c[0], c[1], c[2]) * c[3] / 255;
					}
					else
					{
						uint8_t * tcp = &buffer[(ox + x)*4 + (oy + y) * Width*4];
						tcp[0] = c[2];
						tcp[1] = c[1];
						tcp[2] = c[0];
						tcp[3] = c[3];
					}
				}
			}
		}
	}
	delete[] blockbuff;
}

//===========================================================================
//
// FDDSTexture::CopyPixels
//
//===========================================================================

int FDDSTexture::CopyPixels(FBitmap *bmp, int conversion, int frame)
{
	auto lump = fileSystem.OpenFileReader (SourceLump);

	uint8_t *TexBuffer = bmp->GetPixels();

	lump.Seek (DDS_HeaderSize(hasDX10Header), FileReader::SeekSet);

	if (Format >= 1 && Format <= 4)		// RGB: Format is # of bytes per pixel
	{
		ReadRGB (lump, TexBuffer, PIX_ARGB);
	}
	else if (Format == ID_DXT1)
	{
		DecompressDXT1 (lump, TexBuffer, PIX_ARGB);
	}
	else if (Format == ID_DXT3 || Format == ID_DXT2)
	{
		DecompressDXT3 (lump, Format == ID_DXT2, TexBuffer, PIX_ARGB);
	}
	else if (Format == ID_DXT5 || Format == ID_DXT4)
	{
		DecompressDXT5 (lump, Format == ID_DXT4, TexBuffer, PIX_ARGB);
	}
	else if (Format == ID_BC7)
	{
		DecompressBC7 (lump, TexBuffer, PIX_ARGB);
	}

	return -1;
}
