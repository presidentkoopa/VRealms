#pragma once
//
// Realms of the Haunting's palette shading.
//
// The original never darkens a colour by scaling it. Every pixel is a palette
// index, and each of its 32 shade levels is a TABLE: index in, index out
// (render_world_col_*: gs[(row << 8) | texel]). The dark rows land on the
// palette's few deep navy and purple entries, which is where the original's
// visible banding and cold blue cast come from. Scaling true colour cannot
// produce either.
//
// So REMAROTH does the same lookup on the GPU. A Realms texture keeps its
// ordinary true-colour image (filtering, mipmaps and VR all unchanged) and is
// drawn with a material shader (func_roth.fp, define ROTH_PALETTE) that gets
// two extra textures:
//
//   rothinvlut   the 6-bit VGA colour cube, 64x64x64 tiled 8x8 into 512x512,
//                holding the palette index of each colour
//   rothcmap     256 x 65: rows 0-31 the pack's world ramp, 32-63 its tint
//                ramp, 64 the fog colour -- already resolved to colours
//
// main.fp maps the lit texel back to its index and reads the row the Realms
// shading picked (R_RothShade). Both tables come from the pack itself.
//

#include <string>

class FGameTexture;

namespace roth
{

class Pack;

// Register the material shader. Must run after GLDEFS is parsed and before
// the renderer compiles its shaders (d_main.cpp, right after ParseGLDefs).
void RegisterPaletteShader();

// Put a Realms texture on the palette-shading material for its pack. Cheap
// after the first call per pack: the two tables are built once and cached.
// Does nothing when the pack has no shading tables of its own or the shader
// is not registered.
void ApplyPaletteShading(FGameTexture *tex, const std::string &packName, const Pack &pack);

} // namespace roth
