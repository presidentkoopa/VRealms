/*
** fbx_image.h
**
** RS FORK -- AN IMAGE EMBEDDED IN AN FBX, DECODED. Engine-free (stb_image only), so tools/fbxload_test decodes
** the same bytes with the same code the engine runs (models_fbx.cpp FFBXEmbeddedImage).
**
** The bytes are the file's own embedded copy (FBXSkinned::EmbeddedImage.content), an encoded PNG/JPG/... They
** are decoded by the engine's own stb_image (src/common/thirdparty/stb, implemented in stbtexture.cpp, the
** engine's decoder for 16-bit PNGs) to 8-bit RGBA, the engine's texture format. A 16-bit channel is ROUNDED to
** 8 bits (v * 255 / 65535, nearest), not truncated as stb's own 16 -> 8 conversion does (v >> 8): a normal map's
** 32768 ("0.5", no tilt) becomes 128 either way, but truncation would bias every other value down by up to one
** step.
**
**---------------------------------------------------------------------------
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#ifndef STBI_NO_STDIO
#define STBI_NO_STDIO
#endif
#include "stb_image.h"

namespace FBXSkinned
{

struct ImageInfo
{
	int width = 0, height = 0;
	int channels = 0;	// in the file
	int bits = 0;		// per channel in the file: 8 or 16
};

inline bool ImageInfoOf(const uint8_t *data, size_t size, ImageInfo &out)
{
	out = ImageInfo();
	if (data == nullptr || size == 0 || size > 0x7fffffff) return false;
	if (!stbi_info_from_memory(data, (int)size, &out.width, &out.height, &out.channels)) return false;
	out.bits = stbi_is_16_bit_from_memory(data, (int)size) ? 16 : 8;
	return out.width > 0 && out.height > 0;
}

// Rows top first, 4 bytes per pixel R G B A (a grey image: R = G = B; no alpha in the file: A = 255).
inline bool DecodeRGBA8(const uint8_t *data, size_t size, int &w, int &h, std::vector<uint8_t> &rgba)
{
	w = h = 0;
	rgba.clear();
	if (data == nullptr || size == 0 || size > 0x7fffffff) return false;
	int comp = 0;
	if (stbi_is_16_bit_from_memory(data, (int)size))
	{
		stbi_us *p = stbi_load_16_from_memory(data, (int)size, &w, &h, &comp, 4);
		if (p == nullptr) return false;
		const size_t n = (size_t)w * (size_t)h * 4;
		rgba.resize(n);
		for (size_t i = 0; i < n; i++) rgba[i] = (uint8_t)(((uint32_t)p[i] * 255u + 32767u) / 65535u);
		stbi_image_free(p);
	}
	else
	{
		stbi_uc *p = stbi_load_from_memory(data, (int)size, &w, &h, &comp, 4);
		if (p == nullptr) return false;
		rgba.assign(p, p + (size_t)w * (size_t)h * 4);
		stbi_image_free(p);
	}
	return true;
}

} // namespace FBXSkinned
