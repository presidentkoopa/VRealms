/*
** roth_objects.cpp
**
** Realms of the Haunting's placed objects, spawned into the level from the
** player's own installation. See roth_objects.h for the two-phase shape.
**
** WHAT AN OBJECT IS. Each sector carries a list of 16-byte records: a position,
** an absolute Z, a rotation, some flags, and an artwork reference that is
** indirected through `textureSource` into either the map's own DAS pack or the
** shared one. What the artwork turns out to BE decides how the object is drawn
** -- ROTH.C chooses the draw mode from the resolved art block's flags word, not
** from anything in the object record (renderer.c:6489-6499). So an object whose
** art is a picture becomes a sprite, and one whose art is a 3D mesh becomes a
** model. Nothing in the record says which.
**
** NOTHING IS WRITTEN TO DISK, which is the whole constraint of this project.
** The pictures are registered from memory by roth::TextureSet; the meshes
** become FMemoryMeshModel objects registered with RegisterModel and bound to an
** actor class with AddSpriteModelFrame; the sprite definitions are appended to
** the engine's own sprite table at load. There is no lump anywhere.
**
** WHAT WAS READ OUT OF ROTH.C AND WHAT WAS NOT is commented at each decision.
** The two things to distrust first are the facing sense (ANGLE_SENSE below) and
** the mesh UVs, and both say so where they are.
**
**---------------------------------------------------------------------------
** SPDX-License-Identifier: GPL-3.0-or-later
**---------------------------------------------------------------------------
*/

#include "roth_objects.h"
#include "roth_raw.h"
#include "roth_das.h"
#include "roth_install.h"
#include "roth_texture.h"
#include "roth_log.h"

#include "actor.h"
#include "info.h"
#include "g_levellocals.h"
#include "texturemanager.h"
#include "gametexture.h"
#include "printf.h"
#include "c_cvars.h"
#include "r_state.h"
#include "r_data/sprites.h"
#include "r_data/models.h"
#include "model.h"
#include "model_memorymesh.h"

#include <map>
#include <math.h>
#include <stdio.h>
#include <string>
#include <utility>
#include <vector>

namespace roth
{

//==========================================================================
//
// THE FACING SENSE. The single most likely thing here to be wrong, and the
// cheapest to correct: flip this constant and every prop in the game turns the
// other way.
//
// What ROTH.C says. The object's rotation byte is doubled before it is used
// (renderer.c:6097 for the fixed-angle quad, render_world.c:637 for a mesh) and
// then indexes the same 512-entry sincos table the PLAYER's angle indexes. So
// an object at rotation r is oriented exactly as a player at angle 2r: same
// zero direction, same rotational sense, 256 units per turn against the
// player's 512. The fixed-angle quad's own plane is a quarter turn off that,
// and its NORMAL is the 2r bearing -- which is what GZDoom's RF_WALLSPRITE
// wants in Angles.Yaw (hw_sprites.cpp:1635-1644 builds the quad along
// Yaw - 90 degrees).
//
// What that contradicts. ROTH_STATE.md and the Python oracle both say object
// facing runs in the OPPOSITE sense to the player's, and the oracle subtracts
// where this adds. One of the two is wrong and the code above is the only
// first-hand evidence either way, so this follows the code -- but it has not
// been seen on a screen, and 114 of STUDY1's 274 objects are fixed-angle, so a
// wrong sign here is very visible and very cheap to fix.
//
// The rule that must hold whatever the sign is: an object's facing and the
// player start's facing (rothmap.cpp) live in the SAME space, with the object's
// doubled. If one is changed the other must change with it.
//
//==========================================================================

// +1, as read out of ROTH.C above. This was briefly flipped to -1 and that was
// wrong; the evidence is worth keeping because it separates the two kinds of
// error cleanly:
//
//   sense +1  ->  a couch faced 12 o'clock, wanted 6   = 180 degrees out
//   sense -1  ->  the same couch faced 3 o'clock       = 90 degrees out
//
// A wrong SENSE is rotation-DEPENDENT: it reflects each object about the zero,
// so it moves different objects by different amounts (that couch's byte is 45
// degrees worth, hence 90). A wrong ZERO is rotation-INDEPENDENT: every object
// is out by the same amount. The observed error was a uniform 180, so the sense
// was right all along and the quarter turn below was the culprit.
static const double ANGLE_SENSE = +1.0;

// ROTH.C builds an object's world angle as
//
//     angle512 = 2 * (rotationByte + 0x40) - viewAngle          (renderer.c:6096)
//
// so the object contributes rotationByte*2 units of a 512-unit turn, plus a
// fixed 0x80 units == 90 degrees.
//
// SO THE ORIGIN IS 90 DEGREES, and it is now used. It was derived correctly in
// the comment above and then thrown away: the constant sat at 0.0 with the
// derivation written directly over it, and the 90 was left for the user to
// rediscover by dialling a cvar. That is the exact failure the project rule
// forbids -- a value that ROTH.C states, replaced by a placeholder.
//
// The citation is renderer.c:6096, rwss_rotated_tail, which is the path that
// orients a flat object rather than billboarding it:
//
//     angle512 = (2 * (rotationByte + 0x40) - viewAngle) & 0x1ff
//
// GZDoom subtracts the view angle itself, so the ABSOLUTE yaw we owe it is the
// object's own term, 2*rot + 0x80, converted from 512-per-turn to degrees:
//
//     (2*rot + 128) * 360/512  ==  rot * 360/256 + 90
//
// which is the per-step factor already below plus this 90.
//
// ONE DISAGREEMENT, recorded rather than smoothed over: the sense note above
// says "the observed error was a uniform 180". That came from reading
// descriptions of which way furniture pointed, which is the method that gave
// three contradictory answers. This 90 comes from the code. If furniture is
// still wrong after this, the remaining error is a MEASUREMENT against the
// oracle, not another guess -- and it should be a clean 90, since a wrong
// origin is rotation-independent.
static const double ANGLE_ZERO = 90.0;

// A TRIM on top of the measured origin, defaulting to 0 because the origin is
// now known. Kept only so a disagreement can be confirmed without a rebuild: if
// this ends up needing a value, that value is evidence the derivation above is
// wrong and belongs in a bug report, not in the cvar.
CUSTOM_CVAR(Float, roth_objectangle, 0.f, CVAR_ARCHIVE)
{
	Printf("roth_objectangle %.1f -- reload the map to apply\n", (float)self);
}

double ObjectYaw(uint8_t rotation)
{
	double deg = ANGLE_ZERO + double(roth_objectangle)
		+ ANGLE_SENSE * (double(rotation) * 360.0 / 256.0);
	deg = fmod(deg, 360.0);
	if (deg < 0) deg += 360.0;
	return deg;
}

// The exact inverse of ObjectYaw, for the level logic: "face the player" arrives
// as a world bearing in degrees and has to become the rotation BYTE, because the
// Realms byte is what the map and the command records hold. Kept beside its
// forward direction so a change to the convention cannot update only one of them.
uint8_t RotationFromYaw(double deg)
{
	double r = (deg - ANGLE_ZERO - double(roth_objectangle)) / ANGLE_SENSE;
	r = r * 256.0 / 360.0;
	r = fmod(r, 256.0);
	if (r < 0) r += 256.0;
	return (uint8_t)(int)(r + 0.5);
}

//==========================================================================
//
// What Prepare decided, waiting for a world to be spawned into.
//
//==========================================================================

namespace
{

struct Pending
{
	double x = 0, y = 0, z = 0;
	double yaw = 0;
	double scaleX = 1, scaleY = 1;
	double renderRadius = 0;
	int    spriteNum = -1;     // index into the engine's `sprites`
	int    renderFlags = 0;
	int    rothSector = -1;    // where it came from, for diagnosis
	int    rothIndex = -1;
	// The object's own light, already combined with its sector's -- see where
	// it is computed. -1 means "no light byte, use the sector's".
	int    lightLevel = -1;
	bool   isMesh = false;
};

std::vector<Pending> gPending;

// Sprite definitions this loader appended to the engine's table, so a second
// load does not stack another set on top of the first. Recorded on the first
// Realms load of the process; R_InitSprites can shrink the table underneath us
// (a full game reload), which is detected by the table being shorter than the
// mark rather than assumed not to happen.
unsigned gSpriteMark = 0;
unsigned gSpriteFrameMark = 0;
bool     gSpriteMarkValid = false;

// One FMemoryMeshModel per distinct mesh entry, kept across loads: the geometry
// does not change and rebuilding it per load would grow Models without bound.
// Revalidated against Models exactly as the mesh-particle cache does, because
// InitModels empties Models on a full reload.
struct MeshBinding
{
	unsigned modelIndex = unsigned(-1);
	int      spriteNum = -1;
	std::string pseudoName;
};
std::map<std::string, MeshBinding> gMeshes;

PClassActor *PropClass()
{
	// Declared in the engine's own zscript, not in anything derived from the
	// game: see wadsrc/static/zscript/actors/loaderprop.zs.
	static PClassActor *cls = nullptr;
	if (cls == nullptr) cls = PClass::FindActor("LoaderProp");
	return cls;
}

} // namespace

//==========================================================================
//
// A sprite definition built at load, for a picture that has no lump.
//
// `sprites` is an ordinary TArray the renderer indexes with AActor::sprite, and
// R_InitSkins already appends to it at runtime, so this needs no engine change.
//
// A plain picture fills all sixteen view angles with the one texture. Realms'
// DIRECTIONAL art instead names a picture per view, and the two systems turn
// out to agree exactly: Realms picks its frame with
//
//     ((2*rot + 0x120 - viewAngle) >> 6) & 7        renderer.c:5897-5911
//
// where a turn is 512 units, so its 0x120 offset is 202.5 degrees -- which is
// GZDoom's own `45.0/2*9` rounding offset at hw_sprites.cpp:1436, to the
// degree. So a Realms view index is a GZDoom rotation index, and the mirror
// bit on each frame is GZDoom's per-rotation Flip. Nothing needs converting.
//
// An eight-view entry fills the pairs (0,1), (2,3) ... because GZDoom tells an
// eight-rotation sprite from a sixteen by testing Texture[0] == Texture[1]
// (hw_sprites.cpp:1432) and then indexes all sixteen slots regardless.
//
//==========================================================================

static void MarkSpriteTable()
{
	if (!gSpriteMarkValid || sprites.Size() < gSpriteMark ||
		SpriteFrames.Size() < gSpriteFrameMark)
	{
		// First load, or the table was rebuilt under us and our mark means
		// nothing any more.
		gSpriteMark = sprites.Size();
		gSpriteFrameMark = SpriteFrames.Size();
		gSpriteMarkValid = true;
		gMeshes.clear();
	}
	else
	{
		sprites.Resize(gSpriteMark);
		SpriteFrames.Resize(gSpriteFrameMark);
	}
}

// `views` is 1 (the same picture from everywhere), 8 or 16. `flip` may be null
// when no view is mirrored.
static int MakeRuntimeSprite(const FTextureID *tex, const bool *flip, int views,
	Log *log)
{
	// spritedef_t::spriteframes is a uint16_t, so the frame table has a hard
	// ceiling. Reported rather than wrapped silently.
	if (SpriteFrames.Size() >= 0xffff)
	{
		if (log) log->Count("objects: sprite frame table full");
		return -1;
	}

	spriteframe_t sf;
	memset(&sf, 0, sizeof(sf));
	sf.Voxel = nullptr;
	sf.Flip = 0;
	for (int i = 0; i < 16; i++)
	{
		// 1 view -> every slot; 8 -> each view fills two adjacent slots;
		// 16 -> one slot each.
		const int view = (views == 1) ? 0 : (views == 8) ? (i >> 1) : i;
		sf.Texture[i] = tex[view];
		if (flip != nullptr && flip[view]) sf.Flip |= (uint16_t)(1 << i);
	}
	unsigned frameIndex = SpriteFrames.Push(sf);

	spritedef_t sd;
	memset(&sd, 0, sizeof(sd));
	// Nothing looks these up by name -- the actor carries the index -- but the
	// name is printed by debugging commands, so make it recognisable.
	memcpy(sd.name, "ROTH", 4);
	sd.name[4] = 0;
	sd.numframes = 1;
	sd.spriteframes = (uint16_t)frameIndex;
	return (int)sprites.Push(sd);
}

static int MakeRuntimeSprite(FTextureID tex, Log *log)
{
	return MakeRuntimeSprite(&tex, nullptr, 1, log);
}

//==========================================================================
//
// One Realms mesh, turned into a model the engine can draw.
//
// Vertices arrive as (x, up, y) with the MIDDLE value vertical (handoff 5.5),
// one mesh unit to one world unit, and ROTH.C adds them to the object position
// with no scaling at all (render_world.c:653-676). The engine's model space
// also puts the vertical in the middle -- ObjectToWorldMatrix maps it with
// translate(X, Z, Y) -- so the components go straight across with no negation
// and no swap.
//
// UVs ARE NOT VERIFIED AND CANNOT BE. Realms' mesh vertices carry no texture
// coordinates at all; the original derives them per face inside its span
// drivers from stored extents (renderer.c:13116-13117, 13334-13360). What is
// used here is the unit square per face, in corner order, which is what the
// Python oracle used and what its screenshots were taken with. Expect the
// texture ORIENTATION on a face to be wrong somewhere; expect the geometry not
// to be.
//
//==========================================================================

static FMemoryMeshData BuildMeshData(const Mesh &mesh, TextureSet &art, Log *log,
	int &outFlatFaces, int &outTexturedFaces)
{
	FMemoryMeshData data;
	data.vertices.Reserve((unsigned)mesh.vertices.size());
	for (size_t i = 0; i < mesh.vertices.size(); i++)
	{
		const Mesh::Vertex &v = mesh.vertices[i];
		data.vertices[(unsigned)i] = FVector3((float)v.x, (float)v.up, (float)v.y);
	}

	// Realms' own test is bit 15 of the face's texture word, and the colour is
	// its low byte (renderer.c:13102-13110) -- looser than either of the two
	// bases roth_texture.h documents, but agreeing with both on every value the
	// retail data contains. TextureSet::Sprite applies the documented bases.
	for (const MeshFace &f : mesh.faces)
	{
		if (f.count != 3 && f.count != 4) continue;

		FMemoryMeshFace out;
		out.count = f.count;
		for (int i = 0; i < f.count; i++) out.vertex[i] = f.vertex[i];
		out.skin = art.Sprite((int)f.texture, log);
		if (f.texture & 0x8000) outFlatFaces++; else outTexturedFaces++;

		// The unit square, in corner order. flipV mirrors it vertically, which
		// is the one per-face texturing bit the reader exposes.
		static const float U[4] = { 0.f, 0.f, 1.f, 1.f };
		static const float V[4] = { 0.f, 1.f, 1.f, 0.f };
		for (int i = 0; i < f.count; i++)
			out.uv[i] = FVector2(U[i], f.flipV ? 1.f - V[i] : V[i]);

		data.faces.Push(out);
	}

	// Realms culls by a screen-space winding test with Y downward
	// (renderer.c:12307-12314), which is not a winding this can be translated
	// into with any confidence, and its props include open single-sided panels.
	// Drawing both ways is wrong only in lighting; getting the winding backwards
	// makes the prop invisible or inside out.
	data.doubleSided = true;
	return data;
}

//==========================================================================
//
// Bind a mesh to a (class, sprite, frame) the renderer can find it by.
//
// The model index and the sprite are both allocated at load, so the binding has
// to be made at load too -- which is what AddSpriteModelFrame exists for.
//
//==========================================================================

static const MeshBinding *EnsureMesh(const std::string &packName, int index,
	TextureSet &art, Log *log, int &outMeshesBuilt, int &outFlatFaces,
	int &outTexturedFaces)
{
	std::string key = packName + "/" + std::to_string(index);
	auto it = gMeshes.find(key);
	if (it != gMeshes.end())
	{
		// Still there? InitModels empties Models on a full reload, so the index
		// is only trustworthy while the name at it still matches.
		const MeshBinding &b = it->second;
		if (b.modelIndex < Models.Size() &&
			Models[b.modelIndex]->mFileName.Compare(b.pseudoName.c_str()) == 0)
			return &it->second;
		gMeshes.erase(it);
	}

	const Pack *pack = art.PackData();
	if (pack == nullptr) return nullptr;
	Mesh mesh = pack->ReadMesh(index);
	if (!mesh.ok())
	{
		if (log) log->Count("objects: 3D mesh could not be read");
		return nullptr;
	}

	FMemoryMeshData data = BuildMeshData(mesh, art, log, outFlatFaces, outTexturedFaces);
	if (data.faces.Size() == 0)
	{
		if (log) log->Count("objects: 3D mesh had no usable faces");
		return nullptr;
	}

	MeshBinding b;
	// Prefixed so it can never collide with a real model path, which is what
	// both FindModel's dedupe and the revalidation above key on.
	b.pseudoName = "*memory/roth/" + key;
	b.modelIndex = RegisterModel(new FMemoryMeshModel(std::move(data)),
		b.pseudoName.c_str());
	if (b.modelIndex == unsigned(-1)) return nullptr;

	// A model still needs a sprite frame to be keyed by, even though the model
	// is what gets drawn. Nothing ever samples this texture.
	b.spriteNum = MakeRuntimeSprite(FNullTextureID(), log);
	if (b.spriteNum < 0) return nullptr;

	FSpriteModelFrame smf;
	memset((void *)&smf, 0, sizeof(smf));
	smf.modelsAmount = 1;
	smf.modelIDs.Alloc(1);
	smf.modelIDs[0] = (int)b.modelIndex;
	// -1, as the voxel defs use: this model ignores frame numbers, and a real
	// frame index would make the shared path look one up.
	smf.modelframes.Alloc(1);
	smf.modelframes[0] = -1;
	// Null, so each surface uses the texture the mesh face named.
	smf.skinIDs.Alloc(1);
	smf.skinIDs[0] = FNullTextureID();
	smf.animationIDs.Alloc(1);
	smf.animationIDs[0] = -1;
	smf.surfaceskinIDs.Alloc(MD3_MAX_SURFACES);
	for (unsigned i = 0; i < smf.surfaceskinIDs.Size(); i++)
		smf.surfaceskinIDs[i] = FNullTextureID();
	// One mesh unit is one world unit. Zero here would make the prop invisible,
	// which is what memset leaves behind.
	smf.xscale = smf.yscale = smf.zscale = 1.f;
	smf.type = PropClass();
	smf.sprite = (short)b.spriteNum;
	smf.frame = 0;
	AddSpriteModelFrame(smf);

	// Verify the binding took. If the lookup cannot find what was just added,
	// the actor draws its (empty) sprite instead and the prop is simply missing
	// -- a failure with no symptom other than absence, which is exactly the kind
	// this log exists to make visible.
	if (FindModelFrame(PropClass(), b.spriteNum, 0, false) == nullptr)
	{
		if (log) log->Warn("objects: model binding for mesh %s did not take", key.c_str());
		if (log) log->Count("objects: 3D mesh bound but not findable");
	}

	outMeshesBuilt++;
	auto ins = gMeshes.emplace(key, b);
	return &ins.first->second;
}

//==========================================================================
//
//
//
//==========================================================================

// Packed texture word -> sprite, for the textures the level LOGIC names rather
// than the ones its objects wear. See FindLogicSprite.
static std::map<uint16_t, int> gLogicSprites;

int FindLogicSprite(uint16_t textureWord)
{
	auto it = gLogicSprites.find(textureWord);
	return it == gLogicSprites.end() ? -1 : it->second;
}

void PrepareObjects(const Map &rm, TextureSet &levelArt, Log *log)
{
	gPending.clear();
	MarkSpriteTable();
	ClearAddedSpriteModelFrames();

	if (PropClass() == nullptr)
	{
		if (log) log->Warn("objects: the LoaderProp actor class is missing -- nothing spawned");
		return;
	}

	// The shared pack. Only objects use it, so it is opened here rather than in
	// the map loader. It is a local: the textures it registers keep their own
	// copies of the pixels, and nothing needs the pack after this function.
	TextureSet sharedArt;
	const std::string sharedName = TheInstall().SharedPack();
	bool haveShared = sharedArt.Open(sharedName.c_str(),
		TheInstall().PackFile(sharedName.c_str()), log);
	if (!haveShared && log)
		log->Warn("objects: shared pack %s did not open: %s",
			sharedName.c_str(), sharedArt.Error().c_str());

	// Texture id -> our sprite index, so one picture makes one sprite definition
	// however many objects use it.
	std::map<int, int> spriteByTexture;

	int total = 0, spawnedSprites = 0, spawnedMeshes = 0;
	int hiddenFlag = 0, noArt = 0, emptyEntry = 0, creatures = 0;
	int meshesBuilt = 0, flatFaces = 0, texturedFaces = 0;
	int fixedAngle = 0, flipped = 0, hanging = 0, scaled = 0, lit = 0, nibbleShift = 0;
	int fromShared = 0, sharedUnavailable = 0, noSpriteSlot = 0;
	int oversized = 0;
	int directionalPlaced = 0, directionalUnresolved = 0, directionalFrames = 0;
	int directionalFixedAngle = 0;

	// Sprite definitions for directional art, keyed by the entry rather than by
	// a texture: one entry is eight pictures, and two entries may share a
	// picture without sharing a frame table.
	std::map<std::pair<bool, int>, int> dirSpriteByEntry;

	// Everything a picture sprite needs after its sprite definition exists:
	// the drawn size, the object's own render flags, and the cull radius. Both
	// the plain and the directional paths end here, so a rule added to one is
	// not silently missing from the other.
	auto finishPictureSprite = [&](Pending &p, FTextureID tex, const SpriteInfo &info,
		const roth::Object &o, const std::string &artName, int artIndex)
	{
		// One texel is two world units, like a wall, unless the artwork
		// carries its own size modifier.
		p.scaleX = p.scaleY = info.unitsPerPixel;
		if (info.unitsPerPixel != 2.0) scaled++;
		if (info.hang) hanging++;

		// THE MODIFIER NIBBLE SHIFT, which this loader refused to apply until
		// 2026-10-01 on a premise that turned out to be false.
		//
		// renderer.c:7756-7760 and 6553-6555 admit one reading and no other.
		// For a standing picture (modifier bit 0x10 clear):
		//
		//     near = base + shift          far = near - height
		//     => the picture's BOTTOM sits at objZ - shift
		//
		// so the shift TRANSLATES the whole picture and does not change its
		// height, and a standing prop's base really does end up as much as 30
		// world units below its own Z.
		//
		// THE REFUSAL. This used to read: of STUDY1's 243 drawn objects, 175
		// have a Z exactly equal to their sector's floor height, 71 carry a
		// non-zero nibble, and applying the shift sinks 68 of those below the
		// floor they stand on -- "a prop buried in the floor is not what the
		// original draws". That last sentence is the false half. Three readers
		// went through the subtree independently and the burial is real in the
		// original: both alternatives are plainly worse, since flipping the
		// sign floats every standing prop up to 30 units ABOVE its floor, and
		// anchoring the top instead buries it by its entire height.
		//
		// THE SECOND TERM, still missing. The full shift is
		//
		//     shift = (int16)(2 * (modifier & 0x0f) + u16[0x84aba])
		//
		// added in 16 bits and then sign-extended. The old comment said no
		// writer for 0x84aba could be found anywhere; that was a search
		// artefact -- nothing writes 16 bits there, every write is a 32-BIT
		// store to 0x84ab8 whose high half lands on it. It is read in exactly
		// two places, both this shift (renderer.c:6553, :7758), and it comes
		// from the art entry's own size prefix (FAT flags bit 3 -> dword at
		// block+4, das_assets.c:601).
		//
		// It is ZERO for every one of STUDY1's 243 objects, because DEMO.DAS
		// has no entry carrying that flag -- which is why leaving it out looked
		// harmless. It is NOT zero for the shared sprite pack: 321 of ADEMO's
		// ~550 live entries carry a prefix, with values spanning roughly
		// -10..130. So shared-pack sprites are still placed wrongly until the
		// reader exposes it, and that is counted below rather than hidden.
		if (info.anchorShift != 0.0)
		{
			nibbleShift++;
			// Standing: the bottom drops by the shift. Hanging (IM_HANG): the
			// picture is anchored by its top, which rises by the same amount.
			p.z += info.hang ? info.anchorShift : -info.anchorShift;
		}

		// renderType bit 7: the picture hangs at its own orientation instead of
		// turning to face the camera, as a vertical quad whose normal is the
		// object's bearing (renderer.c:6095-6147). That is exactly
		// RF_WALLSPRITE, which builds its quad perpendicular to Angles.Yaw.
		if (o.FixedAngle())
		{
			p.renderFlags |= RF_WALLSPRITE;
			fixedAngle++;
		}
		if (o.HorizontalFlip())
		{
			p.renderFlags |= RF_XFLIP;
			flipped++;
		}

		auto gtex = TexMan.GetGameTexture(tex, false);
		if (gtex != nullptr)
		{
			double w = gtex->GetDisplayWidth() * p.scaleX;
			double h = gtex->GetDisplayHeight() * p.scaleY;
			p.renderRadius = (w > h ? w : h) * 0.5;

			// Same oversize check the mesh path gets. A SPRITE drawn far taller
			// than a 154-unit player is either an architectural backdrop or a
			// units-per-texel decode we have got wrong, and "one of them is
			// enormous" cannot be chased without knowing WHICH. Named with its
			// index, its decoded scale and its position so it can be looked up.
			if (h > 3.0 * 154.0 && log)
			{
				log->Line("  OVERSIZED sprite  %s[%d]  %.0f tall  upp=%.1f at (%d, %d)",
					artName.c_str(), artIndex, h, info.unitsPerPixel,
					(int)o.x, (int)o.y);
				oversized++;
			}
		}
		spawnedSprites++;
	};

	for (size_t si = 0; si < rm.objects.size(); si++)
	{
		for (size_t oi = 0; oi < rm.objects[si].size(); oi++)
		{
			const Object &o = rm.objects[si][oi];
			total++;

			// Never drawn. The very first test in the original's per-sector
			// object walk (renderer.c:8035): flags bit 7 skips the record
			// outright. 31 of STUDY1's 274 carry it.
			if (o.flags & 0x80)
			{
				hiddenFlag++;
				if (log) log->Count("objects: marked not-drawn (flags & 0x80)");
				continue;
			}

			int artIndex = 0;
			const bool useShared = Pack::ResolveObjectArt(o.textureIndex, o.textureSource,
				artIndex);
			if (useShared && !haveShared)
			{
				sharedUnavailable++;
				if (log) log->Count("objects: wanted the shared pack, which is not open");
				continue;
			}
			TextureSet &art = useShared ? sharedArt : levelArt;
			if (useShared) fromShared++;

			const Pack *pack = art.PackData();
			const FatEntry *entry = pack ? pack->Entry(artIndex) : nullptr;
			if (entry == nullptr)
			{
				noArt++;
				if (log) log->Count("objects: art index outside the pack");
				continue;
			}

			Pending p;
			p.x = o.x;
			p.y = o.y;
			p.z = o.z;               // ABSOLUTE, not relative to the floor
			p.yaw = ObjectYaw(o.rotation);
			p.rothSector = (int)si;
			p.rothIndex = (int)oi;
			if (o.light != 0) lit++;

			// THE OBJECT'S OWN LIGHT BYTE, which this loader read and ignored
			// until 2026-10-01.
			//
			// It is a SIGNED OFFSET on the sector's light, not a brightness and
			// not a radius -- there is no multiply and no distance term
			// anywhere in the path. 0x80 is neutral, so an object carrying 0x80
			// shades exactly as its sector does. Combined per drawn object at
			// renderer.c:6180-6182 and :6659-6661, both in the VISIBLE pass:
			//
			//     L = object +0x08          S = the sector's effective light
			//     L != 0 && S != 0  ->  clip = (uint8)((uint8)(L - 0x80) + S)
			//     otherwise         ->  clip = L
			//
			// All of it is 8-bit and WRAPS; the original clamps nothing. The
			// two guards are not symmetric and both matter: L == 0 DISCARDS the
			// sector's contribution entirely, and S == 0 makes L an absolute
			// row rather than an offset.
			//
			// S here is the sector's authored light. The original adds the
			// global flash bonus at 0x853f6 to it first, but nothing in this
			// port can raise that yet -- it is driven by weapon fire and
			// creature attacks, which belong to the game layer -- so it is zero
			// and the sum is unaffected.
			//
			// Only placed-object billboards take this. Not walls, floors,
			// ceilings or doors, and not the player's weapon sprite, which the
			// original hard-codes to a neutral 0x80 (renderer.c:7853).
			{
				const uint8_t L = o.light;
				const uint8_t S = (si < rm.sectors.size())
					? (uint8_t)rm.sectors[si].light : (uint8_t)0;
				p.lightLevel = (L != 0 && S != 0)
					? (int)(uint8_t)((uint8_t)(L - 0x80) + S)
					: (int)L;
			}

			switch (entry->kind)
			{
			case EntryKind::Empty:
				emptyEntry++;
				if (log) log->Count("objects: art entry is empty");
				continue;

			case EntryKind::Indirection:
			{
				// Not a picture: the original resolves these through the
				// creature/directional tables (das_assets.c:886-891,
				// renderer.c:5827-5927). A CREATURE entry spawns a LIVE ACTOR
				// the first time it is drawn, with its own AI, which this stage
				// cannot place. A DIRECTIONAL entry is a decoration whose
				// picture is chosen from the view angle, and that this stage
				// can place: it is eight ordinary pictures and a rotation.
				if (pack->Indirect(artIndex) != IndirectKind::Directional)
				{
					creatures++;
					if (log) log->Count("objects: creature art -- spawns an actor, not a decoration");
					continue;
				}

				const Directional dir = pack->ReadDirectional(artIndex);
				if (!dir.ok())
				{
					// A record the file does not resolve: a fixed frame chosen
					// through a table the original keeps in engine state. Left
					// unplaced and counted rather than guessed at a view.
					directionalUnresolved++;
					if (log) log->Count("objects: directional art with no frame table in the file");
					continue;
				}

				// One sprite definition per ENTRY. Memoised on which pack the
				// entry came from as well as its index, because the two packs
				// number their entries from zero independently.
				const std::pair<bool, int> entryKey(useShared, artIndex);
				auto haveDir = dirSpriteByEntry.find(entryKey);
				SpriteInfo info;
				FTextureID first = FNullTextureID();

				// The frames are named by GLOBAL das ids, so a view may live in
				// the other pack -- the shared pack's directional entries point
				// almost entirely into the map's own. Resolved the way the
				// original's select_das_fat_entry does (renderer.c:730-733).
				bool badFrame = false;
				FTextureID views[16];
				bool flips[16];
				for (int v = 0; v < dir.count; v++)
				{
					int frameIndex = 0;
					const bool frameShared =
						Pack::ResolveDasId(dir.frames[v].entry, frameIndex);
					if (frameShared && !haveShared) { badFrame = true; break; }
					TextureSet &frameArt = frameShared ? sharedArt : levelArt;

					SpriteInfo frameInfo;
					views[v] = frameArt.Sprite(frameIndex, log, &frameInfo);
					if (!views[v].isValid()) { badFrame = true; break; }
					flips[v] = dir.frames[v].mirror;
					// The views of one prop are the same artwork from different
					// sides, so the first view's size and anchor speak for all.
					if (v == 0) { info = frameInfo; first = views[v]; }
				}
				if (badFrame)
				{
					noArt++;
					if (log) log->Count("objects: a directional view's picture is missing");
					continue;
				}
				directionalFrames += dir.count;

				if (haveDir != dirSpriteByEntry.end())
				{
					p.spriteNum = haveDir->second;
				}
				else
				{
					p.spriteNum = MakeRuntimeSprite(views, flips, dir.count, log);
					if (p.spriteNum < 0) { noSpriteSlot++; continue; }
					dirSpriteByEntry[entryKey] = p.spriteNum;
				}

				// p.yaw is already the object's own facing, which is what
				// GZDoom measures its rotation against -- exactly as the
				// original measures its view index against the object's
				// rotation byte. Nothing else to set: the rotation IS the
				// facing.
				//
				// A directional object carrying renderType bit 7 as well would
				// contradict itself: bit 7 draws the picture as a fixed-angle
				// quad instead of turning it to face the camera, and a
				// view-dependent frame is meaningless on a quad that does not
				// turn. MEASURED over all 44 maps (tools/rothdiff/dircheck):
				// of the 53 placed directional objects, NONE carries bit 7 and
				// none carries the x-flip either. So the case does not arise
				// and is not special-cased -- but it is counted, because that
				// is a fact about the retail data and not a guarantee.
				if (o.FixedAngle()) directionalFixedAngle++;

				directionalPlaced++;
				finishPictureSprite(p, first, info, o, art.Name(), artIndex);
				break;
			}

			case EntryKind::Object3D:
			{
				const MeshBinding *b = EnsureMesh(art.Name(), artIndex, art, log,
					meshesBuilt, flatFaces, texturedFaces);
				if (b == nullptr) { noArt++; continue; }
				p.isMesh = true;
				p.spriteNum = b->spriteNum;
				// A mesh is drawn at one unit per unit with no picture to scale,
				// so only the model's own extent decides how far it overhangs.
				float ex = 0, ey = 0, ez = 0;
				if (b->modelIndex < Models.Size() &&
					Models[b->modelIndex]->GetLocalExtent(&ex, &ey, &ez))
				{
					p.renderRadius = (ex > ez ? ex : ez);
					// NAME anything implausibly large. Realms' player is 154
					// units, so a prop over three times that is either genuinely
					// architectural or a scale we have got wrong -- and "one of
					// them is enormous" is impossible to chase without knowing
					// WHICH. Reported with its art index and position so it can
					// be looked up rather than hunted.
					if (ey > 3.f * 154.f && log)
					{
						log->Line("  OVERSIZED prop  %s[%d]  %.0f tall at (%d, %d)",
							art.Name().c_str(), artIndex, ey, (int)o.x, (int)o.y);
						oversized++;
					}
				}
				spawnedMeshes++;
				break;
			}

			default:
			{
				SpriteInfo info;
				FTextureID tex = art.Sprite(artIndex, log, &info);
				if (!tex.isValid())
				{
					noArt++;
					continue;
				}

				auto found = spriteByTexture.find(tex.GetIndex());
				if (found != spriteByTexture.end())
				{
					p.spriteNum = found->second;
				}
				else
				{
					p.spriteNum = MakeRuntimeSprite(tex, log);
					if (p.spriteNum < 0) { noSpriteSlot++; continue; }
					spriteByTexture[tex.GetIndex()] = p.spriteNum;
				}

				finishPictureSprite(p, tex, info, o, art.Name(), artIndex);
				break;
			}
			}

			gPending.push_back(p);
		}
	}

	if (log)
	{
		log->Section("Objects");
		log->Line("  objects in map        %d", total);
		log->Line("  to spawn              %d  (%d sprites, %d 3D props)",
			(int)gPending.size(), spawnedSprites, spawnedMeshes);
		log->Line("  meshes built          %d  (%d textured faces, %d flat-colour)",
			meshesBuilt, texturedFaces, flatFaces);
		log->Line("  sprite definitions    %d registered at load",
			(int)(sprites.Size() - gSpriteMark));
		log->Line("  from the shared pack  %d", fromShared);
		log->Line("  directional           %d placed from %d view frames, %d unresolved%s",
			directionalPlaced, directionalFrames, directionalUnresolved,
			directionalFixedAngle != 0 ? "  (SOME ALSO FIXED-ANGLE -- see the comment)" : "");
		log->Line("  fixed angle           %d  (drawn as wall sprites)", fixedAngle);
		log->Line("  x-flipped             %d", flipped);
		log->Line("  top-anchored (HANG)   %d", hanging);
		log->Line("  non-default draw size %d", scaled);
		log->Count("objects: implausibly large (over 3x the player)", oversized);
		log->Line("  carrying a light byte %d applied  (signed offset on the sector, 0x80 neutral; the 0x853f6 flash term needs the game layer)", lit);
		log->Line("  artwork nibble shift  %d applied  (the 0x84aba second term is"
			" still missing -- zero on this pack's own art, NOT on shared sprites)",
			nibbleShift);
		log->Line("  not spawned           %d  = %d not-drawn flag, %d empty art,"
			" %d creature, %d directional unresolved, %d no art, %d shared pack missing,"
			" %d no sprite slot",
			total - (int)gPending.size(), hiddenFlag, emptyEntry, creatures,
			directionalUnresolved, noArt, sharedUnavailable, noSpriteSlot);
		log->Line("  object art registered %d pictures in %s, %d in %s",
			levelArt.SpritesRegistered(), levelArt.Name().c_str(),
			sharedArt.SpritesRegistered(), sharedName.c_str());
		log->Count("objects: carrying their own light byte", lit);
	}

	//------------------------------------------------------------------
	// SPRITES THE LOGIC CAN NAME, which are not the set its objects wear.
	//
	// Opcode 0x0d repaints a prop to the texture word its record carries at
	// +0x0c, and that texture may be on no object anywhere in the map. The
	// packs are locals here and the runtime cannot open one later, so the
	// sprite is built now. Only the record's INITIAL word needs this: the
	// swap puts the prop's own previous texture back into the record, and
	// that one already has a sprite because the prop is wearing it.
	//------------------------------------------------------------------
	gLogicSprites.clear();
	for (const Command &c : rm.commands)
	{
		if ((c.opcode & 0x7f) != 0x0D) continue;
		const uint16_t word = c.Word(0x0C);
		if (word == 0 || gLogicSprites.count(word) != 0) continue;

		int artIndex = 0;
		const bool useShared = Pack::ResolveObjectArt((uint8_t)(word & 0xFF),
			(uint8_t)(word >> 8), artIndex);
		if (useShared && !haveShared) continue;
		TextureSet &art = useShared ? sharedArt : levelArt;

		const Pack *pack = art.PackData();
		if (pack == nullptr || pack->Entry(artIndex) == nullptr) continue;

		SpriteInfo info;
		FTextureID tex = art.Sprite(artIndex, log, &info);
		if (!tex.isValid()) continue;

		auto found = spriteByTexture.find(tex.GetIndex());
		int sn = (found != spriteByTexture.end()) ? found->second
		                                         : MakeRuntimeSprite(tex, log);
		if (sn < 0) continue;
		spriteByTexture[tex.GetIndex()] = sn;
		gLogicSprites[word] = sn;
	}
	if (log && !gLogicSprites.empty())
		log->Line("  logic sprites    %d texture word(s) pre-built for opcode 0x0d",
			(int)gLogicSprites.size());
}

//==========================================================================
//
// Phase two. Everything decided; put the actors in.
//
//==========================================================================

// Realms (sector, index) -> the actor it became. Keyed as one int so the map is
// a plain lookup; sectors and their object counts are both well under 16 bits.
static std::map<uint32_t, AActor *> gObjectActors;

static inline uint32_t ObjectKey(int rothSector, int rothIndex)
{
	return ((uint32_t)(rothSector & 0xFFFF) << 16) | (uint32_t)(rothIndex & 0xFFFF);
}

AActor *FindObjectActor(int rothSector, int rothIndex)
{
	if (rothSector < 0 || rothIndex < 0) return nullptr;
	auto it = gObjectActors.find(ObjectKey(rothSector, rothIndex));
	return it == gObjectActors.end() ? nullptr : it->second;
}

void SpawnPreparedObjects(FLevelLocals *Level)
{
	gObjectActors.clear();
	if (gPending.empty() || Level == nullptr) return;

	PClassActor *cls = PropClass();
	if (cls == nullptr) { gPending.clear(); return; }

	// THE PLACEMENT CHECK, and the only one available without looking at a
	// screen. Realms sector i became GZDoom sector i, one for one, and every
	// object record is stored INSIDE the sector that owns it. So dropping each
	// prop at its stated x,y and asking the finished BSP which sector that is
	// re-derives a fact the map already asserts. A mismatch means the position
	// is being read or transformed wrongly; agreement means it is not.
	//
	// The Z check is the same idea a level up: an absolute Z that lands outside
	// its own sector's floor-to-ceiling span is either a Z convention error or a
	// prop the original hangs through a ceiling. Counted, not corrected.
	int spawned = 0, failed = 0, wrongSector = 0, outsideSpan = 0;
	for (const Pending &p : gPending)
	{
		AActor *mo = AActor::StaticSpawn(Level, cls, DVector3(p.x, p.y, p.z), NO_REPLACE);
		if (mo == nullptr) { failed++; continue; }

		// So the level logic can find this prop again. See FindObjectActor.
		gObjectActors[ObjectKey(p.rothSector, p.rothIndex)] = mo;

		// After the spawn, not before: the spawn state sets the sprite, and
		// LoaderProp's spawn state has tics -1 so nothing ever sets it again.
		mo->sprite = p.spriteNum;
		mo->frame = 0;
		mo->Angles.Yaw = DAngle::fromDeg(p.yaw);
		mo->Scale = DVector2(p.scaleX, p.scaleY);
		mo->renderflags |= ActorRenderFlags::FromInt(p.renderFlags);
		// The object's own light, already combined with its sector's. The
		// Realms light byte goes into lightlevel unchanged for sectors
		// (rothmap.cpp), so it goes in unchanged here too, and AActor's
		// LightLevel overrides the sector for exactly this purpose
		// (hw_sprites.cpp:1729).
		if (p.lightLevel >= 0) mo->LightLevel = (int16_t)p.lightLevel;
		// So a prop that overhangs its own sector is still drawn when the
		// neighbouring sector is the visible one.
		if (p.renderRadius > mo->radius) mo->renderradius = p.renderRadius;
		// StaticSpawn may have clamped Z to the floor; the map's Z is absolute
		// and authoritative.
		mo->SetZ(p.z);

		if (mo->Sector != nullptr && p.rothSector >= 0)
		{
			const int landed = mo->Sector->Index();
			if (landed != p.rothSector) wrongSector++;
			double fz = mo->Sector->floorplane.ZatPoint(p.x, p.y);
			double cz = mo->Sector->ceilingplane.ZatPoint(p.x, p.y);
			if (p.z < fz - 1.0 || p.z > cz + 1.0) outsideSpan++;
		}

		spawned++;
	}

	// The spawn happens long after the load report was closed -- LoadRothMap
	// ends it so that a crash between the two still leaves a readable file --
	// but this is the half that says whether the actors actually exist, so it
	// has to reach the same place. Appended directly rather than by reopening
	// the Log, which would rewrite the header and lose the counters.
	const std::string &path = TheLog().Path();
	if (!path.empty())
	{
		if (FILE *f = fopen(path.c_str(), "a"))
		{
			fprintf(f, "\nObjects spawned\n---------------\n");
			fprintf(f, "  actors spawned        %d\n", spawned);
			fprintf(f, "  spawns that failed    %d\n", failed);
			fprintf(f, "  landed in a DIFFERENT sector than the one that owns them  %d\n",
				wrongSector);
			fprintf(f, "  z outside their own sector's floor-to-ceiling span        %d\n",
				outsideSpan);
			fclose(f);
		}
	}

	Printf("Realms: %d objects spawned%s\n", spawned,
		failed ? FStringf(", %d failed", failed).GetChars() : "");

	gPending.clear();
}

} // namespace roth
