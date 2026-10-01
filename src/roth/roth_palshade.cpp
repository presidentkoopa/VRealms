/*
** roth_palshade.cpp
** Realms of the Haunting's palette shading -- see roth_palshade.h.
*/

#include "roth_palshade.h"
#include "roth_das.h"

#include "textures.h"
#include "image.h"
#include "texturemanager.h"
#include "gametexture.h"
#include "palettecontainer.h"
#include "r_data/r_translate.h"
#include "c_cvars.h"
#include "printf.h"

#include <map>
#include <vector>
#include <memory>

CVAR(Bool, roth_palette_shading, true, CVAR_ARCHIVE | CVAR_NOINITCALL)

namespace roth
{

namespace
{

const char *SHADER_FILE = "shaders/glsl/func_roth.fp";
int g_shaderIndex = -1;

struct Tables
{
	FGameTexture *inv = nullptr;
	FGameTexture *cmap = nullptr;
};
std::map<std::string, Tables> g_byPack;

// The pixel stores must outlive their images, exactly as roth_texture.cpp's.
std::vector<std::unique_ptr<std::vector<uint8_t>>> g_store;

const uint8_t *Keep(std::vector<uint8_t> &&v)
{
	g_store.push_back(std::make_unique<std::vector<uint8_t>>(std::move(v)));
	return g_store.back()->data();
}

FRemapTable *StoreRemap(FRemapTable t)
{
	const int idx = GetTranslationIndex(GPalette.StoreTranslation(TRANSLATION_Standard, &t));
	return const_cast<FRemapTable *>(GPalette.GetTranslation(TRANSLATION_Standard, idx));
}

// An image whose true-colour pixels are exactly the bytes given, read through
// `pal`. Row-major.
FGameTexture *MakeTable(const char *name, std::vector<uint8_t> &&pixels, FRemapTable *pal, int w, int h)
{
	auto *image = new FPalettedMemoryImage(Keep(std::move(pixels)), pal, w, h, false, false);
	auto *tex = MakeGameTexture(new FImageTexture(image), name, ETextureType::Override);
	tex->SetNoMipmap(true);
	TexMan.AddGameTexture(tex);
	return tex;
}

Tables Build(const std::string &packName, const Pack &pack)
{
	Tables t;
	const uint8_t *shade = pack.ShadeTables();
	const auto &palette = pack.Palette();
	if (shade == nullptr || palette.size() < 256) return t;

	// The 6-bit VGA value each palette channel came from. Expand6 is
	// monotonic, so this recovers it exactly.
	auto six = [](uint8_t c) { return (int)((c * 63 + 127) / 255); };

	// rothinvlut: colour cube -> palette index. The FIRST index carrying a
	// colour wins, then every other cell takes its nearest palette colour so
	// a filtered texel still resolves to something sensible.
	std::vector<int> owner(64 * 64 * 64, -1);
	for (int i = 0; i < 256; i++)
	{
		const int cell = six(palette[i].r) | (six(palette[i].g) << 6) | (six(palette[i].b) << 12);
		if (owner[cell] < 0) owner[cell] = i;
	}
	std::vector<uint8_t> inv(512 * 512);
	for (int b = 0; b < 64; b++)
		for (int g = 0; g < 64; g++)
			for (int r = 0; r < 64; r++)
			{
				int best = owner[r | (g << 6) | (b << 12)];
				if (best < 0)
				{
					int bestd = 1 << 30;
					for (int i = 0; i < 256; i++)
					{
						const int dr = six(palette[i].r) - r, dg = six(palette[i].g) - g, db = six(palette[i].b) - b;
						const int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
						if (d < bestd) { bestd = d; best = i; }
					}
				}
				const int x = r + (b & 7) * 64, y = g + (b >> 3) * 64;
				inv[(size_t)y * 512 + x] = (uint8_t)best;
			}
	FRemapTable grey;
	for (int c = 0; c < 256; c++) { grey.Palette[c] = PalEntry(255, c, c, c); grey.Remap[c] = (uint8_t)c; }

	// rothcmap: row r, column i = palette[table[r][i]]. Rows 0-63 are the two
	// ramps as the pack stores them; row 64 is the fog colour everywhere; row
	// 65 is the GLOW remap.
	//
	// The glow row is not a shade level and belongs to neither ramp. A glowing
	// surface is drawn as glow[texel] with no depth term, no sector light and
	// no flash bonus -- it ignores lighting completely, which is why the shader
	// returns from that row before any lighting runs. A pack with no glow
	// section gets an identity row, so selecting it changes nothing rather than
	// painting the world black.
	std::vector<uint8_t> cm(256 * 66);
	memcpy(cm.data(), shade, 256 * 64);
	memset(cm.data() + 256 * 64, pack.FogIndex() & 0xff, 256);
	if (const uint8_t *glow = pack.GlowTable())
		memcpy(cm.data() + 256 * 65, glow, 256);
	else
		for (int i = 0; i < 256; i++) cm[256 * 65 + i] = (uint8_t)i;
	FRemapTable pal;
	for (int c = 0; c < 256; c++) { pal.Palette[c] = PalEntry(255, palette[c].r, palette[c].g, palette[c].b); pal.Remap[c] = (uint8_t)c; }

	FString invName, cmName;
	invName.Format("%s_PALINV", packName.c_str());
	cmName.Format("%s_PALCMAP", packName.c_str());
	t.inv = MakeTable(invName.GetChars(), std::move(inv), StoreRemap(grey), 512, 512);
	t.cmap = MakeTable(cmName.GetChars(), std::move(cm), StoreRemap(pal), 256, 66);
	return t;
}

} // namespace

void RegisterPaletteShader()
{
	for (unsigned i = 0; i < usershaders.Size(); i++)
		if (!usershaders[i].shader.CompareNoCase(SHADER_FILE)) { g_shaderIndex = (int)i + FIRST_USER_SHADER; return; }

	UserShaderDesc desc;
	desc.shader = SHADER_FILE;
	desc.shaderType = SHADER_Default;
	// Custom textures follow the default material's own four layers, so the
	// first is texture5 (gldefs.cpp, firstUserTexture).
	desc.defines = "#define ROTH_PALETTE\n#define rothinvlut texture5\n#define rothcmap texture6\n";
	g_shaderIndex = (int)usershaders.Push(desc) + FIRST_USER_SHADER;
}

void ApplyPaletteShading(FGameTexture *tex, const std::string &packName, const Pack &pack)
{
	if (tex == nullptr || g_shaderIndex < 0 || !roth_palette_shading) return;
	auto it = g_byPack.find(packName);
	if (it == g_byPack.end()) it = g_byPack.emplace(packName, Build(packName, pack)).first;
	const Tables &t = it->second;
	if (t.inv == nullptr || t.cmap == nullptr) return;

	MaterialLayers lay = {};
	lay.Glossiness = -1000.f;
	lay.SpecularLevel = -1000.f;
	lay.CustomShaderTextures[0] = t.inv;
	lay.CustomShaderTextures[1] = t.cmap;
	tex->SetShaderLayers(lay);
	tex->SetShaderIndex(g_shaderIndex);
}

} // namespace roth
