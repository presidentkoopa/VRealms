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

//
// What TextureSet::Sprite worked out about one object picture, beyond the
// texture itself. All of it comes from the artwork's own modifier byte, so it
// is a property of the picture and is shared by every object that uses it.
//
struct SpriteInfo
{
	// The drawn size of one texel, in world units. Two, like a wall, unless the
	// artwork carries a size modifier: modifier bit 0x80 says one is present and
	// image_type bits 5-6 hold the exponent -- 0 means HALF size and 1/2/3 mean
	// x2/x4/x8 (renderer.c:6512-6525). It changes the drawn extent only; the
	// stored pixels are untouched.
	double unitsPerPixel = 2.0;

	// Anchored by its top rather than its bottom (modifier bit 0x10). The
	// picture then hangs DOWN from the object's z instead of standing on it
	// (renderer.c:6549-6561).
	bool hang = false;

	// An extra vertical shift in world units, moving the anchor DOWN for a
	// standing picture and UP for a hanging one (renderer.c:6552-6556). Two
	// terms, summed in 16 bits and then sign-extended:
	//
	//     2 * (modifier & 0xf)  +  the art entry's own anchor offset
	//
	// The second comes from roth::AnchorOffset::y and is zero for every entry
	// carrying no anchor, which is every entry in all five map packs. See the
	// caller in roth_objects.cpp.
	double anchorShift = 0.0;

	// A lateral shift in world units, from the same anchor offset's other half
	// (roth::AnchorOffset::x). The original applies it in VIEW space -- it
	// slides the picture across the screen rather than through the world -- so
	// it belongs on AActor::SpriteOffset and not on the object's position.
	//
	// POSITIVE IS RIGHTWARD, and that is read rather than assumed:
	//   - the original adds it to the view-space lateral, whose `+ext` edge is
	//     the one bounded as the RIGHT extent (renderer.c:6539-6548; the
	//     rotated path's swap at :6115 puts `+cos` in the same slot), and the
	//     projection's multiplier and divisor are both positive, so a larger
	//     lateral is a larger screen x;
	//   - here, HWAngles.Yaw is 270 - viewYaw (r_utility.cpp:845), so
	//     HandleSpriteOffsets' `yaw` is the view angle itself, and its
	//     FromAngles is a rotation about +Z (quaternion.h:345) carrying
	//     (0,1,0) to (-sin,cos,0) -- camera-LEFT. GetSpriteOffset then negates
	//     (actor.h:2274), so a positive SpriteOffset.X moves the sprite RIGHT.
	// The two agree, so this passes through unnegated -- except under an
	// x-flip, which the original negates it for (renderer.c:5669).
	double lateralOffset = 0.0;

	// True when nothing could be decoded; the caller should count it, not draw.
	bool failed = false;
};

// One DAS pack, registered into the texture manager under its own name space.
class TextureSet
{
public:
	// Reads the pack file and keeps its bytes alive. `log` may be null.
	bool Open(const char *packName, const std::string &packFile, Log *log);

	bool IsOpen() const { return mPack != nullptr; }
	const std::string &Name() const { return mName; }
	const std::string &Error() const { return mError; }

	// The map's sky picture, from the MAP metadata's `skyTexture` (+0x18) --
	// not from anything in the pack. An ordinary opaque picture; see the
	// definition for why the pack header's +0x22 word is not this.
	// The sky PICTURE, named by the map's own metadata (+0x18). Opaque and
	// unmasked: the parallax column renderer writes every texel it samples.
	FTextureID Sky(int metadataSkyIndex, Log *log);

	// Does this stored index mean "this surface is the sky"? That is the PACK's
	// marker, a different quantity from the picture above -- see
	// Pack::SkyMarkerIndex. 6,208 flats across 36 of the 44 retail maps use it,
	// so treating it as an ordinary texture paints the sky onto the ceiling.
	bool IsSkySurface(int index) const;

	// Shape and blend of a stored picture, for the flat scale rule.
	//
	// ROTH.C gives 256x256 OPAQUE flats a different units-per-texel (2^(s-1)
	// rather than 2^s). The branch is renderer.c:3367 --
	// `width == height && (uint8_t)width == 0`, i.e. 256x256 -- and then on
	// g_span_textured_mode_flag, which picks the opaque inner loop 0x3a220 over
	// the translucent 0x3a100 (renderer.c:3341). That flag comes from bit 26 of
	// the texture block's +8 dword (renderer.c:13281), which is this pack's
	// IT_TRANSLUCENT -- a property of the PICTURE, not of the surface, so the
	// loader can settle it once at load time.
	//
	// Returns false if the index does not decode, leaving the outputs untouched.
	bool ImageShape(int index, int &w, int &h, bool &translucent) const;

	// The engine texture for a world surface's stored index. Handles the
	// solid-colour sentinels; results are memoised.
	//
	// EVERY index in the picture range is a picture. Nothing here means "draw
	// nothing": an invalid id comes back only when the pack failed to open or
	// the image could not be decoded, and both are counted.
	//
	// `masked` asks for the variant in which stored index 0 is a hole. It is a
	// separate registration under its own name, because masking is a property of
	// the image and one image serves many faces, so it cannot be decided per
	// face at draw time.
	//
	// OPAQUE IS THE DEFAULT, and that is a deliberate correction: "transparent
	// unless told otherwise" is backwards.
	//
	// The bit that tells otherwise is now identified. renderer.c:13281-13286
	// tests 0x400 in the artwork block's flags word at block+0xa, and
	// renderer.c:6288 sets the same span mode from `byte[block+0xb] & 4` -- the
	// high byte of that word is image_type, so the bit is image_type bit 2,
	// which roth_das.h calls IT_TRANSLUCENT. It is NOT IT_ZERO_OPAQUE. An image
	// carrying it is drawn with holes AND a 50% blend on its high palette half;
	// this class handles that itself, so `masked` only ADDS masking on top.
	// See the long comment in roth_texture.cpp.
	// `flipped` asks for the X-mirrored variant. The original mirrors INSIDE the
	// texture's texel range (renderer.c:4734, u' = limit - u - 1), not across the
	// wall piece the way a negative Doom scale would, so the mirror has to live
	// in the pixels or the tiling seams stop meeting.
	FTextureID World(int index, Log *log, bool masked = false, bool flipped = false);

	// Counters, so a load report can say what was not understood.
	int Registered() const { return mRegistered; }
	int Animated() const { return mAnimated; }
	int SolidColours() const { return mSolidColours; }
	int Failed() const { return mFailed; }
	// Images the original draws with its transparency+translucency span
	// function: index 0 is a hole and index >= 0x80 is a 50% blend. Drawing one
	// of these opaque is what turned Realms' mirrors and glazed doors into black
	// rectangles with hard diagonal highlights.
	int Translucent() const { return mTranslucent; }

	// The engine texture for OBJECT art: a placed prop's sprite, or one face of
	// a 3D prop's mesh. Handles the solid-colour sentinels exactly as World
	// does; memoised separately.
	//
	// OBJECT ART IS STORED ROTATED, THE SAME QUARTER TURN AS WALL ART, and this
	// contradicts what the handoff says ("sprites and model skins do not
	// transpose"). Two independent lines of evidence:
	//
	//  * ROTH.C's object draw takes the sprite's HORIZONTAL world extent from
	//    the art block's `blk[0xe]` and its VERTICAL extent from `blk[0xc]`
	//    (renderer.c:6505-6511, 6538-6561) -- crossed relative to the wall path,
	//    which takes U from `blk[0xc]` (renderer.c:13288-13290). `blk[0xc]` is
	//    the stored row width. So the stored rows run VERTICALLY up the drawn
	//    picture, which is what "rotated" means.
	//  * The retail art decoded both ways and looked at: DEMO entry 4105 is the
	//    suit of armour and ADEMO entry 45 is a candelabra. Read row-major they
	//    lie on their sides; transposed they stand up.
	//
	// So this shares World's quarter turn. It is still a separate entry point,
	// because a name is what the texture manager keys on and object art needs
	// its own masking rule and its own sprite offsets, which a wall must not get.
	//
	// Index 0 is a HOLE here, the opposite of World's default, because a prop's
	// picture is a cut-out and drawn opaque it is a rectangle of background.
	// No object image in the retail packs sets IT_ZERO_OPAQUE (measured across
	// all 44 maps), so nothing in the retail data contradicts that.
	//
	// Sprite offsets are set on the registered texture from the image's own
	// anchoring bit: bottom-centre normally, top-centre for an IM_HANG image.
	// That is what makes a placed prop stand on the Z the map gave it.
	//
	// `info`, when given, receives the drawn size and anchoring the caller needs
	// to place the thing; see SpriteInfo.
	//
	// Defined in roth_texture_object.cpp rather than in this class's own .cpp
	// only because that file was owned by another lane when this was written. It
	// belongs in roth_texture.cpp and should be moved there.
	FTextureID Sprite(int index, Log *log, struct SpriteInfo *info = nullptr);

	// The decoded pack, for callers that need something other than a picture out
	// of it -- a 3D mesh, or just the kind of an entry. Null until Open succeeds.
	const Pack *PackData() const { return mPack; }

	// What Sprite() did, kept apart from the world-surface counters so a load
	// report can tell "no wall art" from "no prop art".
	int SpritesRegistered() const { return mSpritesRegistered; }
	int SpritesFailed() const { return mSpritesFailed; }

private:
	// A world surface's art is stored rotated a quarter turn, so the decoded
	// bytes go in as column-major with the dimensions exchanged. See
	// FPalettedMemoryImage.
	FTextureID Build(int index, Log *log, bool masked, bool flipped);
	FTextureID SolidColour(int paletteIndex, Log *log);

	std::string mName, mError;
	std::vector<uint8_t> mBytes;      // the pack file, kept alive
	Pack *mPack = nullptr;            // owned
	// Three, because Realms decides PER IMAGE how its texels reach the
	// framebuffer, and both the hole and the blend live in the palette's alpha.
	// See the long comment in roth_texture.cpp.
	int mTranslation = -1;            // index 0 transparent
	int mOpaqueTranslation = -1;      // index 0 is an ordinary colour
	// index 0 transparent AND index >= 0x80 at 50% alpha: what the original's
	// transparency+translucency span function does for an IT_TRANSLUCENT image.
	int mTranslucentTranslation = -1;
	// Cached by stored index. The texture manager is the real authority (it can
	// be rebuilt under us), so a miss re-resolves by name rather than trusting
	// this blindly.
	std::map<int, FTextureID> mByIndex, mByIndexMasked;
	std::map<int, FTextureID> mByIndexFlipped, mByIndexMaskedFlipped;
	// Object art is its own name space: same quarter turn as a wall, but its own
	// masking rule and its own sprite offsets.
	std::map<int, FTextureID> mByIndexSprite;
	std::map<int, SpriteInfo> mSpriteInfo;
	int mSpritesRegistered = 0, mSpritesFailed = 0;
	int mRegistered = 0, mAnimated = 0, mSolidColours = 0, mFailed = 0;
	int mOutOfRange = 0;
	int mTranslucent = 0;

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
