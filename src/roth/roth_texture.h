#pragma once
//
// Turning Realms of the Haunting artwork into engine textures.
//
// This is the ONE file in src/roth that touches engine types. Its neighbours
// (roth_raw, roth_das, roth_palette, roth_install) are deliberately engine-free
// so roth_selftest can exercise them without starting the engine; this one sits
// on the boundary and does the registration.
//
// Nothing is written to disk. A pack's bytes are read from the player's install,
// decoded in memory, and handed to FPalettedMemoryImage, which borrows the
// decoded buffer. The buffers therefore have to outlive the textures, so this
// module owns them for the process lifetime.
//
// TEXTURE NAMES ARE NAMESPACED PER PACK, and that is not optional: TEX0001 in
// DEMO and TEX0001 in DEMO3 are different pictures. A single flat name space
// silently shows one map's art on another map's walls as soon as two packs are
// in play.
//

#include <stdint.h>
#include <string>
#include <map>
#include <vector>

#include "textureid.h"

namespace roth
{

class Pack;
class Log;

// One DAS pack, registered into the texture manager under its own name space.
class TextureSet
{
public:
	// Reads the pack file and keeps its bytes alive. `log` may be null.
	bool Open(const char *packName, const std::string &packFile, Log *log);

	bool IsOpen() const { return mPack != nullptr; }
	const std::string &Name() const { return mName; }
	const std::string &Error() const { return mError; }
	uint16_t SkyIndex() const;

	// The engine texture for a world surface's stored index. Handles the
	// solid-colour sentinels; results are memoised.
	//
	// Returns an INVALID FTextureID for the pack's sky index, which Realms uses
	// to mean "draw nothing here". What that should become is the caller's
	// decision, because it depends on the surface: a wall drawing nothing is the
	// ordinary no-step-against-my-neighbour case, whereas a floor or ceiling
	// drawing nothing is open to the sky -- and a floor must have SOME flat or
	// it renders hall of mirrors. Deciding that here would drag the renderer's
	// sky flat into this folder for no gain.
	//
	// `masked` asks for the variant in which stored index 0 is a hole. It is a
	// separate registration under its own name, because masking is a property of
	// the image and one image serves many faces, so it cannot be decided per
	// face at draw time.
	//
	// OPAQUE IS THE DEFAULT, and that is a deliberate correction. ROTH.C's 3D
	// render path chooses the hole-skipping mapper only when the artwork block's
	// own flags word says to (renderer.c:13281-13286, bit 0x400 at block+0xa);
	// nothing there tests the image_type bit our reader calls IT_ZERO_OPAQUE, so
	// "transparent unless told otherwise" is backwards. Defaulting to opaque is
	// also the lower-risk reading: a wrongly masked wall or flat is see-through
	// or renders hall of mirrors, while a wrongly opaque decal is only a dark
	// patch.
	FTextureID World(int index, Log *log, bool masked = false);

	// True when this index is the pack's "draw nothing" index.
	bool IsNothing(int index) const;

	// Counters, so a load report can say what was not understood.
	int Registered() const { return mRegistered; }
	int Animated() const { return mAnimated; }
	int SolidColours() const { return mSolidColours; }
	int Failed() const { return mFailed; }

private:
	// A world surface's art is stored rotated a quarter turn, so the decoded
	// bytes go in as column-major with the dimensions exchanged. See
	// FPalettedMemoryImage.
	FTextureID Build(int index, Log *log, bool masked);
	FTextureID SolidColour(int paletteIndex, Log *log);

	std::string mName, mError;
	std::vector<uint8_t> mBytes;      // the pack file, kept alive
	Pack *mPack = nullptr;            // owned
	// Two, because Realms decides per image whether stored index 0 is a hole,
	// and transparency lives in the palette's alpha.
	int mTranslation = -1;            // index 0 transparent
	int mOpaqueTranslation = -1;      // index 0 is an ordinary colour
	// Cached by stored index. The texture manager is the real authority (it can
	// be rebuilt under us), so a miss re-resolves by name rather than trusting
	// this blindly.
	std::map<int, FTextureID> mByIndex, mByIndexMasked;
	int mRegistered = 0, mAnimated = 0, mSolidColours = 0, mFailed = 0;
	int mOutOfRange = 0;

public:
	// Stored indices that are neither a picture nor a colour we can derive.
	int OutOfRange() const { return mOutOfRange; }
};

// ROTH.C gates a surface texture id to the picture table with
// `if ((uint16_t)id >= 0x1200) goto solid;` (renderer.c:13122), so 0x1200 is
// where pictures stop and flat colours begin.
static const uint16_t TEX_PICTURE_LIMIT = 0x1200;  // 4608

// Above the picture limit an index is a flat colour, and there are TWO bases it
// can be written against. Measured over all 44 retail maps: every high index
// falls in 32768..33016 (836 uses) or 65280..65535 (32679 uses), and NOTHING
// lands between the picture limit and 32768. Subtracting the nearest base turns
// both clusters into palette indices 0..255 exactly, which is what identifies
// them as `base + palette entry`; any other base puts one cluster out of range.
//
// 0xFF00 is the dominant form by a factor of forty, and it is also the one
// roth_das.h already documents for 3D mesh faces ("or >= 65280 meaning a flat
// colour"). The Python pipeline only knew the 0x8000 base, so it silently drew
// NOTHING on all 32679 of the 0xFF00 surfaces -- the single largest correctness
// difference between this loader and that oracle.
//
// NOT VERIFIED: why there are two bases. Reading ROTH.C established that the id
// is gated with `if (id >= 0x1200) goto solid` (renderer.c:13122) but the solid
// branch was not followed far enough to read the colour out, so the derivation
// below rests on the range evidence above, not on the original's code.
static const uint16_t TEX_COLOUR_BASE_HI = 65280;  // 0xFF00 + palette entry
static const uint16_t TEX_COLOUR_BASE_LO = 32768;  // 0x8000 + palette entry

} // namespace roth
