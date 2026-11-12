#pragma once
//
// Item icons from DBASE200.DAT, decoded at run time from the player's own
// file. Engine-free. Nothing decoded here is ever written to disk.
//
// THE FORMAT, from ROTH.C:
//   An item record's +0x0C is a DBASE200 resource number; the resource lives
//   at byte offset number << 3 (update_selected_item_icon, inventory.c:1476-1482;
//   load_das_cache_resource, das_assets.c:421-428): a u32 byte size, then the
//   image.
//   The image (blit_das_image_to_buffer, blit_2d.c:1195-1216): byte 0 is the
//   flags, u16 width at +4, u16 height at +6. Flag 4 skips 8 more bytes, a
//   clear flag 2 skips a 0x300-byte palette, then 8 header bytes, then the
//   pixels: raw rows when flag 1 is clear, otherwise one continuous run-length
//   stream -- a byte below 0xF1 is a pixel, 0xF1..0xFF repeats the next byte
//   (b - 0xF0) times (rle_decode_scroll_segment, blit_2d.c:296-327) -- and a
//   run may cross a row's end (the window carries the overshoot over).
//   Index 0 is not drawn by the icon blitter (blit_item_icon,
//   inventory.c:2113: `if (al) *edi = al`): it is the hole.
//   Pixels index the game palette (the map's pack palette).
//

#include <stdint.h>
#include <stddef.h>
#include <vector>

namespace roth
{

struct DasImage
{
	int width = 0, height = 0;
	std::vector<uint8_t> pixels;        // row-major, width * height palette indices
	bool ok = false;
};

// Decode one image (starting at its flags byte), `size` bytes available.
DasImage DecodeDasImage(const uint8_t *img, size_t size);

// The icon resource `resource` (an item's +0x0C) out of DBASE200.DAT's bytes.
DasImage DecodeDbase200Resource(const std::vector<uint8_t> &dbase200, uint32_t resource);

} // namespace roth
