// tools/fbxload_test/stb_image_impl.cpp -- the engine's stb_image, implemented with the engine's own options
// (src/common/textures/formats/stbtexture.cpp), so fbx_image.h decodes here exactly as in the engine.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_TGA
#define STBI_NO_PSD
#define STBI_NO_HDR
#define STBI_NO_PNM
#include "stb_image.h"
