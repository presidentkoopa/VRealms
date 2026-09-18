
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

// ============================================================================
// [SCREENTILES] A SCREEN TILE MASK ("Engine docs/EFFECTS_OPTIMIZATION_PLAN.md" E5; the C++ is PPScreenTileMask, hw_postprocess.h,
// and the numbers are hw_screentiles.h's).
//
// Drawn at tile resolution: this texel is one SCREEN_TILE_TEXELS x SCREEN_TILE_TEXELS tile of a pass's target. Its value is a
// 32-bit mask with bit i set when item i's rectangle of tiles contains this tile. The rectangles come four a row in TileRects0..7,
// each packed from the low byte up as first tile x, first tile y, last tile x, last tile y (ScreenTileRectPack); an unused item
// contains no tile. The mask is written one byte a channel into RGBA8 UNORM -- b / 255 stores b exactly -- and a reader takes
// uint(value * 255.0 + 0.5) back.
// ============================================================================

// The bits of one row of four rectangles that contain the tile: bit k for rectangle k.
uint TileRectBits(uvec4 rects, uvec2 tile)
{
	uint bits = 0u;
	for (int k = 0; k < 4; k++)
	{
		uint rect = rects[k];
		uvec2 first = uvec2(rect & 255u, (rect >> 8u) & 255u);
		uvec2 last = uvec2((rect >> 16u) & 255u, rect >> 24u);
		if (all(greaterThanEqual(tile, first)) && all(lessThanEqual(tile, last)))
			bits |= 1u << uint(k);
	}
	return bits;
}

void main()
{
	uvec2 tile = uvec2(gl_FragCoord.xy);
	uint mask = TileRectBits(TileRects0, tile) | (TileRectBits(TileRects1, tile) << 4u) | (TileRectBits(TileRects2, tile) << 8u) |
		(TileRectBits(TileRects3, tile) << 12u) | (TileRectBits(TileRects4, tile) << 16u) | (TileRectBits(TileRects5, tile) << 20u) |
		(TileRectBits(TileRects6, tile) << 24u) | (TileRectBits(TileRects7, tile) << 28u);
	FragColor = vec4(float(mask & 255u), float((mask >> 8u) & 255u), float((mask >> 16u) & 255u), float(mask >> 24u)) / 255.0;
}
