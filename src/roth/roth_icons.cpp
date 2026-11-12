//
// Item icons from DBASE200.DAT. See roth_icons.h.
//

#include "roth_icons.h"

namespace roth
{

DasImage DecodeDasImage(const uint8_t *img, size_t size)
{
	DasImage out;
	if (img == nullptr || size < 8) return out;
	const uint8_t flags = img[0];
	const int w = img[4] | (img[5] << 8);                   // blit_2d.c:1197
	const int h = img[6] | (img[7] << 8);                   // :1206
	size_t p = 0;
	if (flags & 4) p += 8;                                  // :1210
	if (!(flags & 2)) p += 0x300;                           // :1211
	p += 8;                                                 // :1215
	if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return out;
	const size_t total = (size_t)w * (size_t)h;
	out.width = w;
	out.height = h;
	out.pixels.assign(total, 0);
	if (!(flags & 1))                                       // plain rows, :1310-1318
	{
		if (p + total > size) return out;
		for (size_t i = 0; i < total; i++) out.pixels[i] = img[p + i];
		out.ok = true;
		return out;
	}
	size_t o = 0;                                           // the RLE stream, blit_2d.c:296-327
	while (o < total && p < size)
	{
		const uint8_t b = img[p++];
		if (b < 0xf1) { out.pixels[o++] = b; continue; }
		if (p >= size) break;
		const uint8_t v = img[p++];
		for (int r = b - 0xf0; r > 0 && o < total; r--) out.pixels[o++] = v;
	}
	out.ok = o == total;
	return out;
}

DasImage DecodeDbase200Resource(const std::vector<uint8_t> &dbase200, uint32_t resource)
{
	DasImage out;
	if (resource == 0) return out;
	const uint64_t off = (uint64_t)resource << 3;           // inventory.c:1478
	if (off + 4 > dbase200.size()) return out;
	const uint8_t *q = dbase200.data() + off;
	const uint32_t size = (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);   // :1480
	if (off + 4 + size > dbase200.size()) return out;
	return DecodeDasImage(q + 4, size);                     // :1481, 1489
}

} // namespace roth
