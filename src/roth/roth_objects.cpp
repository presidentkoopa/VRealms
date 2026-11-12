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
** The facing sense and the mesh face mapping were both re-derived on 2026-11-12
** and are checked over the retail data by tools/propcheck; what is still not
** the original's is said where it is (the horizontal-plane faces).
**
**---------------------------------------------------------------------------
** SPDX-License-Identifier: GPL-3.0-or-later
**---------------------------------------------------------------------------
*/

#include "roth_objects.h"
#include "roth_runtime.h"   // SectorShift: sectors moved by roth_portals
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
// WHICH WAY AN OBJECT FACES. Settled 2026-11-12 from ROTH.C alone, and checked
// against three things that do not depend on each other.
//
// THE ORIGINAL'S ANGLES RUN CLOCKWISE FROM +Y. Its view basis for a view angle
// V (512 a turn) is forward = (sin V, cos V), right = (cos V, -sin V): both the
// world-vertex transform (renderer.c:6752-6779 with :1278-1291) and the object
// origin transform (render_world.c:626-633 with renderer.c:12204-12217) reduce
// to exactly that, and its own atan2 returns 0 along +Y and 0x80 along +X
// (renderer.c:614-650). The PLAYER's stored angle is the negative of V -- the
// movement code negates it before every table lookup (renderer.c:1183-1185,
// :1203-1205) -- which is why the player start in rothmap.cpp is right to turn
// COUNTER-clockwise, and why reading "an object at rotation r is oriented as a
// player at angle 2r" gave this file the wrong sense for six weeks.
//
// WHAT THE ROTATION BYTE DOES, with R = rot * 360/256 degrees:
//
//   a mesh corner (x, y)      ang = V - 2*rot; lateral = x cos - y sin,
//                             depth = x sin + y cos   (render_world.c:637-655)
//                             => world offset ( x cos R + y sin R,
//                                              -x sin R + y cos R )
//   a fixed-angle picture     theta = 2*(rot + 0x40) - V; its right-hand edge is
//                             at lateral + sin(theta), depth + cos(theta)
//                             (renderer.c:6096-6110) => it lies along
//                             (cos R, -sin R), the SAME local x axis
//   a directional picture     view index ((2*rot + 0x120 - V) >> 6) & 7
//                             (renderer.c:5714-5719), which is the engine's own
//                             rotation index when the actor faces local +y
//
// So an object's own +y axis points along (sin R, cos R): CLOCKWISE with the
// byte, and at bearing 90 - R in this engine's counter-clockwise degrees. That
// is ObjectYaw. The three ways an object is drawn each sit at a fixed turn from
// it -- see YawOffsetMesh / YawOffsetFixedQuad below.
//
// THE CHECKS:
//   1. "Face the player" (compute_player_object_bearing, raw_commands.c:300)
//      stores atan2(player -> object) >> 1 in the byte. With the rule above that
//      turns a fixed-angle picture's readable side to the player and nothing
//      else does.
//   2. The two descriptions recorded here when the sign was last argued over --
//      "sense +1: a couch faced 12 o'clock, wanted 6" and "sense -1: the same
//      couch faced 3 o'clock", for a byte worth 45 degrees, both taken with a
//      zero of 0 -- are 180 and 90 degrees out from a face normal of 270 - R
//      and from nothing else.
//   3. tools/propcheck: a ring that is clockwise on screen is the visible side
//      in the original, and under this handedness the visible side of the
//      retail meshes' faces points OUT of the mesh 838 times to 268 (the 268
//      are cards and the inside faces of concave furniture).
//   4. THE ORIGINAL ITSELF. tools/propcheck/proprender draws a map's objects
//      on the host with exactly these rules and was laid over frames of the
//      original engine (tools/oracle) from the same poses on 2026-11-12:
//      STUDY1's courtyard trees, hall chandelier, couch, chair and table,
//      CHURCH1's trees and wall boxes, and a banner in MAS3 at rotation 19
//      all land on the original's, and the banner drawn with the old sense is
//      a broadside where the original shows it nearly edge-on.
//
// What that does NOT cover: the engine's own drawing of these actors has not
// been seen yet -- in particular that RF_WALLSPRITE and the model winding
// behave as read (YawOffsetFixedQuad, MDL_FORCECULLBACKFACES).
//
//==========================================================================

// Clockwise, as derived above.
static const double ANGLE_SENSE = -1.0;

// Rotation 0 has the object's +y axis along the map's +Y, which is 90 degrees
// here.
static const double ANGLE_ZERO = 90.0;

// A MESH is built with its local x on the model's x and its local y on the
// model's z, and the engine turns model (x, z) counter-clockwise by the actor's
// yaw (models.cpp, `rotate(-yaw, 0, 1, 0)` after `translate(X, Z, Y)`). Local x
// has to land on (cos R, -sin R), so the yaw is -R: a quarter turn behind the
// facing.
static const double YawOffsetMesh = -90.0;

// A FIXED-ANGLE PICTURE is an RF_WALLSPRITE, whose Angles.Yaw is the direction
// its readable face looks (hw_sprites.cpp builds the quad along (sin yaw,
// -cos yaw), the mirror image of the camera-facing case). The original draws
// the picture unmirrored when its `-` edge is on screen-left
// (renderer.c:6113-6119 swaps and sets the mirror bit otherwise), which is the
// view from the object's -y side. So the face looks back along -y.
static const double YawOffsetFixedQuad = 180.0;

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
	// The original takes the bearing in its own 512 steps and then halves it
	// with an arithmetic shift (raw_commands.c:300-308), so the byte is the
	// nearest 512th FLOORED to a 256th, not the nearest 256th.
	double r = (deg - ANGLE_ZERO - double(roth_objectangle)) / ANGLE_SENSE;
	r = r * 512.0 / 360.0;
	r = fmod(r, 512.0);
	if (r < 0) r += 512.0;
	const int step512 = (int)(r + 0.5) & 0x1ff;
	return (uint8_t)(step512 >> 1);
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
	// Added to ObjectYaw for the actor: 0 for a picture that faces the camera
	// or picks a view, YawOffsetMesh, or YawOffsetFixedQuad.
	double yawOffset = 0;
	double scaleX = 1, scaleY = 1;
	double renderRadius = 0;
	int    spriteNum = -1;     // index into the engine's `sprites`
	int    renderFlags = 0;
	int    rothSector = -1;    // where it came from, for diagnosis
	int    rothIndex = -1;
	// The object's own light, already combined with its sector's -- see where
	// it is computed. -1 means "no light byte, use the sector's".
	int    lightLevel = -1;
	// The art's own lateral anchor, a VIEW-space slide -- see
	// SpriteInfo::lateralOffset. Zero for all but a handful of entries.
	double spriteOffsetX = 0;
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
	// The faces that are not polygons at all: a camera-facing picture hung on
	// one vertex (MeshFaceKind::Billboard). Kept so each placement can spawn
	// them; the chandelier's eight flames are these.
	struct Hung { int x, y, up; int texture; };
	std::vector<Hung> hung;
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
// TEXTURE COORDINATES are roth::MeshFaceTexCoords (roth_das.cpp), which holds
// the rule for each of the original's three span drivers and is the function
// tools/propcheck runs over the retail data. Until 2026-11-12 every face got
// the unit square in corner order -- a quarter turn out on every wall-type
// face, which is every face of every tree -- and flag bit 1 was read as a
// vertical flip when it is a horizontal mirror, so the back of each tree card
// was drawn upside down over its front.
//
// THE PICTURE is TextureSet::MeshSkin: the face's own choice out of an image
// pack, and the face's own say on whether texel 0 is a hole.
//
// ONE-SIDED. The original draws a face only when its ring runs clockwise on
// screen (renderer.c:12307-12314, :12371-12397), and its props depend on it: a
// card is stored as two faces over the same four corners, wound opposite ways,
// each wearing what that side should show (120 such pairs in the retail packs,
// 28 of them with different coordinates on the two sides). Drawn double-sided
// that is four coplanar quads fighting for the same depth, which is the smeared
// trees. A ring is emitted here in the file's order, the model is bound with
// MDL_FORCECULLBACKFACES, and the engine's front face for a model is the
// clockwise one (hw_models.cpp:57, Cull_CW) -- the same rule, so nothing is
// reversed.
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

	for (const MeshFace &f : mesh.faces)
	{
		// A Billboard face has no polygon; EnsureMesh hands it to the spawner.
		if (f.count != 3 && f.count != 4) continue;

		FMemoryMeshFace out;
		out.count = f.count;
		for (int i = 0; i < f.count; i++) out.vertex[i] = f.vertex[i];

		// Realms' own test for a flat colour is bit 15 of the face's texture
		// word, and the colour is its low byte (renderer.c:13102-13110).
		int texW = 0, texH = 0;
		out.skin = art.MeshSkin((int)f.texture, (int)f.subTexture, f.Masked(), log,
			&texW, &texH);
		if (f.texture & 0x8000) outFlatFaces++; else outTexturedFaces++;

		float u[4], v[4];
		if (MeshFaceTexCoords(mesh, f, texW, texH, u, v))
		{
			for (int i = 0; i < f.count; i++) out.uv[i] = FVector2(u[i], v[i]);
			if (f.Kind() == MeshFaceKind::Plane && log)
				log->Count("objects: horizontal-plane mesh face, picture fitted to the ring (not the original's planar mapping)");
		}
		else
		{
			// A flat colour, or a picture that did not decode: every corner on
			// one texel.
			for (int i = 0; i < f.count; i++) out.uv[i] = FVector2(0.5f, 0.5f);
		}

		data.faces.Push(out);
	}

	data.doubleSided = false;
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
	for (const MeshFace &f : mesh.faces)
	{
		if (f.Kind() != MeshFaceKind::Billboard) continue;
		const Mesh::Vertex &v = mesh.vertices[f.vertex[0]];
		MeshBinding::Hung h = { v.x, v.y, v.up, (int)f.texture };
		b.hung.push_back(h);
	}
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
	// One-sided, as the original draws them. See BuildMeshData.
	smf.setFlags(MDL_FORCECULLBACKFACES);
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
// BindMemoryMesh -- give a mesh some OTHER part of the loader built a sprite
// frame an actor of `cls` can wear. See roth_objects.h.
//
// The same three steps EnsureMesh takes for a prop -- register the model, make
// a sprite to key it by, bind the pair to a class -- but for geometry that did
// not come out of an art pack: roth_platforms builds a box from a moving
// sector's own floor, ceiling and slab.
//
// THE MODEL IS KEPT ACROSS LOADS AND THE BINDING IS NOT, deliberately. Models
// only ever grows, so a mesh with the same key is registered once per process
// and found again by name. The sprite and the model frame are the opposite:
// PrepareObjects truncates the sprite table and clears the added model frames
// at the start of every load (MarkSpriteTable, ClearAddedSpriteModelFrames), so
// both are made again each time this is called. It must therefore be called
// AFTER PrepareObjects in a load, which SpawnPreparedPlatforms is.
//
//==========================================================================

static std::map<std::string, unsigned> gBoundModels;   // pseudo name -> Models index

int BindMemoryMesh(const char *key, FMemoryMeshData &&data, PClassActor *cls, Log *log)
{
	if (key == nullptr || cls == nullptr || data.faces.Size() == 0) return -1;
	if (!gSpriteMarkValid) return -1;       // PrepareObjects has not run this load

	const std::string pseudo = std::string("*memory/roth/") + key;
	unsigned modelIndex = unsigned(-1);
	auto it = gBoundModels.find(pseudo);
	if (it != gBoundModels.end() && it->second < Models.Size()
		&& Models[it->second]->mFileName.Compare(pseudo.c_str()) == 0)
	{
		modelIndex = it->second;            // same key, same geometry: reuse it
	}
	else
	{
		modelIndex = RegisterModel(new FMemoryMeshModel(std::move(data)), pseudo.c_str());
		if (modelIndex == unsigned(-1)) return -1;
		gBoundModels[pseudo] = modelIndex;
	}

	const int spriteNum = MakeRuntimeSprite(FNullTextureID(), log);
	if (spriteNum < 0) return -1;

	FSpriteModelFrame smf;
	memset((void *)&smf, 0, sizeof(smf));
	smf.modelsAmount = 1;
	smf.modelIDs.Alloc(1);
	smf.modelIDs[0] = (int)modelIndex;
	smf.modelframes.Alloc(1);
	smf.modelframes[0] = -1;                // frame numbers are ignored, as for props
	smf.skinIDs.Alloc(1);
	smf.skinIDs[0] = FNullTextureID();      // each face draws with the texture it named
	smf.animationIDs.Alloc(1);
	smf.animationIDs[0] = -1;
	smf.surfaceskinIDs.Alloc(MD3_MAX_SURFACES);
	for (unsigned i = 0; i < smf.surfaceskinIDs.Size(); i++)
		smf.surfaceskinIDs[i] = FNullTextureID();
	smf.xscale = smf.yscale = smf.zscale = 1.f;
	smf.type = cls;
	smf.sprite = (short)spriteNum;
	smf.frame = 0;
	AddSpriteModelFrame(smf);

	if (FindModelFrame(cls, spriteNum, 0, false) == nullptr)
	{
		if (log) log->Warn("platforms: model binding for %s did not take", key);
		return -1;
	}
	return spriteNum;
}

//==========================================================================
//
//
//
//==========================================================================

// Packed texture word -> sprite, for the textures the level LOGIC names rather
// than the ones its objects wear. See FindLogicSprite.
static std::map<uint16_t, int> gLogicSprites;
struct LogicSpriteLook { double scale = 2.0, zShift = 0.0, offsetX = 0.0; };
static std::map<uint16_t, LogicSpriteLook> gLogicSpriteLook;

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
	int lateralOffsets = 0, lateralPerView = 0, lateralViewsDiffer = 0;
	int fromShared = 0, sharedUnavailable = 0, noSpriteSlot = 0;
	int oversized = 0;
	int directionalPlaced = 0, directionalUnresolved = 0, directionalFrames = 0;
	int directionalFixedAngle = 0;
	int hungPictures = 0;

	// Sprite definitions for directional art, keyed by the entry rather than by
	// a texture: one entry is eight pictures, and two entries may share a
	// picture without sharing a frame table.
	std::map<std::pair<bool, int>, int> dirSpriteByEntry;

	// Everything a picture sprite needs after its sprite definition exists:
	// the drawn size, the object's own render flags, and the cull radius. Both
	// the plain and the directional paths end here, so a rule added to one is
	// not silently missing from the other.
	auto finishPictureSprite = [&](Pending &p, FTextureID tex, const SpriteInfo &info,
		const roth::Object &o, const std::string &artName, int artIndex,
		bool viewDependent)
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
		// THE SECOND TERM, now present. The full shift is
		//
		//     shift = (int16)(2 * (modifier & 0x0f) + u16[0x84aba])
		//
		// added in 16 bits and then sign-extended. An older comment here said
		// no writer for 0x84aba could be found anywhere; that was a search
		// artefact -- nothing writes 16 bits there, every write is a 32-BIT
		// store to 0x84ab8 whose high half lands on it (renderer.c:5666, :5676,
		// :5692, :5731, :5976, :6015, zeroed at :6663 and :7703). It is read in
		// exactly two places, both this shift (renderer.c:6553, :7758), and it
		// comes from the art entry's own anchor offset -- see roth::AnchorOffset
		// for where that lives and what was measured about it.
		//
		// It is ZERO for every one of STUDY1's 243 objects, because no entry in
		// any of the five map packs carries the flag -- which is why leaving it
		// out looked harmless for so long. It is not zero in the shared pack,
		// and that is now read.
		//
		// NOTE for whoever chases the DEMO[4102] discrepancy: that entry has
		// flags_1 0x80 and NO anchor, so this term is not the explanation and
		// the two faults are independent. Measured, not assumed --
		// tools/rothdiff/prefixcheck.cpp prints it.
		if (info.anchorShift != 0.0)
		{
			nibbleShift++;
			// Standing: the bottom drops by the shift. Hanging (IM_HANG): the
			// picture is anchored by its top, which rises by the same amount.
			p.z += info.hang ? info.anchorShift : -info.anchorShift;
		}

		// THE ANCHOR'S LATERAL HALF. A view-space slide, so it belongs on the
		// sprite and not on the position -- see SpriteInfo::lateralOffset for
		// the sign, which is read out of both engines rather than guessed. The
		// original negates it under the x-flip bit (renderer.c:5669), which
		// here is the object's own HorizontalFlip, read from `o` because
		// RF_XFLIP is not set on `p` until further down this same function.
		//
		// NOT APPLIED TO VIEW-DEPENDENT ART, and that is a measured decision
		// rather than an omission. A directional object gets ONE SpriteInfo --
		// view 0's -- but each view is a different art entry with its own
		// anchor and its own mirror bit, and the original negates and applies
		// them per view. MEASURED (tools/rothdiff/prefixcheck.cpp) over all 44
		// maps, of the 9 directional entries whose every frame carries an
		// anchor:
		//
		//   - the VERTICAL anchor is the same across all views for 9 of 9, so
		//     taking it from view 0 above is sound;
		//   - the LATERAL anchor DIFFERS across views for 8 of 9;
		//   - all 9 mirror some views and not others.
		//
		// So one actor-level offset cannot express this for 8 of the 9. Doing
		// it properly means carrying a per-view offset on the sprite
		// definition, which is a bigger change than this one and is written up
		// in the handoff rather than guessed at here. Counted so the number is
		// visible instead of silently zero.
		if (info.lateralOffset != 0.0)
		{
			if (viewDependent)
			{
				lateralPerView++;
			}
			else
			{
				lateralOffsets++;
				p.spriteOffsetX = o.HorizontalFlip() ? -info.lateralOffset
					: info.lateralOffset;
			}
		}

		// renderType bit 7: the picture hangs at its own orientation instead of
		// turning to face the camera, as a vertical quad whose normal is the
		// object's bearing (renderer.c:6095-6147). That is exactly
		// RF_WALLSPRITE, which builds its quad perpendicular to Angles.Yaw.
		if (o.FixedAngle())
		{
			p.renderFlags |= RF_WALLSPRITE;
			p.yawOffset = YawOffsetFixedQuad;
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
			{
				// An object belongs to the sector whose list it is in; if the
				// loader moved that sector (roth_portals) the object goes too.
				double sx = 0., sy = 0.;
				if (SectorShift((int)si, sx, sy)) { p.x += sx; p.y += sy; }
			}
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
				// EVERY VIEW'S LATERAL ANCHOR, not view 0's. The counter below used to
				// test info.lateralOffset, which IS view 0's, so an entry whose view 0
				// is zero and whose other views are not read as "no lateral anchor
				// here" -- and 59 of the 321 prefixes in the files carry x = 0. A zero
				// that cannot move is not a measurement, and whether a per-view offset
				// is worth building at all rests on this number.
				bool latAnyNonZero = false, latDiffer = false;
				double lat0 = 0.0;
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
					if (frameInfo.lateralOffset != 0.0) latAnyNonZero = true;
					if (v == 0) lat0 = frameInfo.lateralOffset;
					else if (frameInfo.lateralOffset != lat0) latDiffer = true;
				}
				if (badFrame)
				{
					noArt++;
					if (log) log->Count("objects: a directional view's picture is missing");
					continue;
				}
				directionalFrames += dir.count;
				if (latAnyNonZero) lateralPerView++;
				if (latDiffer) lateralViewsDiffer++;

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
				finishPictureSprite(p, first, info, o, art.Name(), artIndex, true);
				break;
			}

			case EntryKind::Object3D:
			{
				const MeshBinding *b = EnsureMesh(art.Name(), artIndex, art, log,
					meshesBuilt, flatFaces, texturedFaces);
				if (b == nullptr) { noArt++; continue; }
				p.isMesh = true;
				p.yawOffset = YawOffsetMesh;
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

				// THE PICTURES HUNG ON THIS MESH'S VERTICES. A face with no
				// corners is drawn by render_world_sprite's side entry
				// (render_world.c:701-714 -> renderer.c:6591-6608): the vertex,
				// already turned and placed with the rest of the mesh, becomes
				// the position of an ordinary camera-facing object picture --
				// lateral, height and depth are seeded from the vertex record
				// and the draw joins the object path at its resolver, with the
				// plain mode word 0x19 and no mirror. So each one is spawned as
				// what it is: a billboard at the vertex's world position. The
				// corner turns clockwise with the rotation byte, as every mesh
				// corner does (render_world.c:650-651).
				//
				// These are separate actors with no entry in the object index,
				// so a command that later turns or moves the mesh does not
				// carry them. No retail command targets a mesh that has any
				// (DEMO's 4115, the chandelier, is the only one placed).
				for (const MeshBinding::Hung &h : b->hung)
				{
					// The face's texture word is an OBJECT ART WORD on this
					// path, not a DAS id: the side entry stores it where the
					// object record's +4 dword goes (renderer.c:6603-6605), so
					// its low byte is the art index and its high byte the
					// source, exactly as for a placed object. 0x0012 on the
					// chandelier is DEMO's entry 4114, the lamp globe.
					int hungIndex = 0;
					const bool hungShared = Pack::ResolveObjectArt((uint8_t)(h.texture & 0xff),
						(uint8_t)(h.texture >> 8), hungIndex);
					if (hungShared && !haveShared) continue;
					SpriteInfo hinfo;
					FTextureID htex = (hungShared ? sharedArt : levelArt).Sprite(hungIndex, log, &hinfo);
					if (!htex.isValid()) continue;
					Pending hp = p;
					hp.isMesh = false;
					hp.yawOffset = 0;
					hp.renderRadius = 0;
					hp.rothIndex = -1;
					const double R = double(o.rotation) * (M_PI / 128.0);
					hp.x = p.x + h.x * cos(R) + h.y * sin(R);
					hp.y = p.y - h.x * sin(R) + h.y * cos(R);
					hp.z = p.z + h.up;
					auto found = spriteByTexture.find(htex.GetIndex());
					if (found != spriteByTexture.end()) hp.spriteNum = found->second;
					else
					{
						hp.spriteNum = MakeRuntimeSprite(htex, log);
						if (hp.spriteNum < 0) { noSpriteSlot++; continue; }
						spriteByTexture[htex.GetIndex()] = hp.spriteNum;
					}
					hp.scaleX = hp.scaleY = hinfo.unitsPerPixel;
					if (hinfo.anchorShift != 0.0)
						hp.z += hinfo.hang ? hinfo.anchorShift : -hinfo.anchorShift;
					auto gtex = TexMan.GetGameTexture(htex, false);
					if (gtex != nullptr)
					{
						const double w = gtex->GetDisplayWidth() * hp.scaleX;
						const double hh = gtex->GetDisplayHeight() * hp.scaleY;
						hp.renderRadius = (w > hh ? w : hh) * 0.5;
					}
					hungPictures++;
					gPending.push_back(hp);
				}
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

				finishPictureSprite(p, tex, info, o, art.Name(), artIndex, false);
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
		log->Line("  pictures hung on mesh vertices  %d  (each its own camera-facing actor)",
			hungPictures);
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
		log->Line("  artwork anchor shift  %d applied  (both terms: the modifier"
			" nibble and the art entry's own vertical anchor)", nibbleShift);
		log->Line("  lateral anchor        %d applied, %d directional props with one"
			" on ANY view, %d whose views DISAGREE"
			" -- only the last would be fixed by a per-view offset",
			lateralOffsets, lateralPerView, lateralViewsDiffer);
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
	// sprite is built now. For 0x0d only the record's INITIAL word needs this: the
	// swap puts the prop's own previous texture back into the record, and
	// that one already has a sprite because the prop is wearing it.
	//------------------------------------------------------------------
	gLogicSprites.clear();
	gLogicSpriteLook.clear();
	// The words the OTHER object-texture commands can put on a prop or a
	// particle (roth_cmdcore.cpp): 0x2d draws particles whose texture word is
	// the record's key (raw_commands.c:4885-4911); 0x22 writes count - 1 +
	// word[+0x10] while its count runs 0..limit (:780-788); 0x20 writes the
	// reference object's word plus one (:1184-1206).
	std::vector<uint16_t> logicWords;
	for (const Command &c : rm.commands)
	{
		const uint8_t op = (uint8_t)(c.opcode & 0x7f);
		if (op == 0x0D) logicWords.push_back(c.Word(0x0C));
		else if (op == 0x2D) logicWords.push_back(c.Word(0x08));
		else if (op == 0x22)
		{
			const int top = (c.Word(0x08) > c.Word(0x0C) ? c.Word(0x08) : c.Word(0x0C)) + 1;
			for (int k = 0; k <= top && k < 0x100; k++)
				logicWords.push_back((uint16_t)(k - 1 + c.Word(0x10)));
		}
		else if (op == 0x20)
		{
			const uint16_t av = c.Word(0x0A), key = c.Word(0x08);
			const uint16_t refId = (av != 0 && av != key) ? av : key;
			for (const auto &list : rm.objects)
				for (const Object &o : list)
					if (refId != 0 && o.commandID == refId)
						logicWords.push_back((uint16_t)((o.textureIndex | (o.textureSource << 8)) + 1));
		}
	}
	for (const uint16_t word : logicWords)
	{
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
		// The same placement rules a placed object gets (finishPictureSprite):
		// the drawn size, and the anchor shift down for a standing picture or
		// up for a hanging one.
		LogicSpriteLook look;
		look.scale = info.unitsPerPixel;
		look.zShift = info.hang ? info.anchorShift : -info.anchorShift;
		look.offsetX = info.lateralOffset;
		gLogicSpriteLook[word] = look;
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

// The reverse index, built beside the forward one at spawn. A map rather than a
// field on the actor because LoaderProp is shared with anything else that wants
// a posed prop, and an engine class should not grow a Realms-shaped member for
// one caller's benefit.
static std::map<const AActor *, uint32_t> gActorObjects;

bool ObjectForActor(const AActor *mo, int &rothSector, int &rothIndex)
{
	if (mo == nullptr) return false;
	auto it = gActorObjects.find(mo);
	if (it == gActorObjects.end()) return false;
	rothSector = (int)(it->second >> 16);
	rothIndex  = (int)(it->second & 0xFFFF);
	return true;
}

// The actor rules (roth_actors.h) remove records from a sector's list and move
// them between lists, as the original does (raw_commands.c:3703, 3849), which
// changes the (sector, index) of every record after them. They re-key both
// indexes here afterwards, so FindObjectActor keeps naming the right actor.
void RekeyObjectActors(const std::vector<std::pair<std::pair<int, int>, AActor *>> &current)
{
	gObjectActors.clear();
	gActorObjects.clear();
	for (const auto &e : current)
	{
		if (e.second == nullptr) continue;
		const uint32_t key = ObjectKey(e.first.first, e.first.second);
		gObjectActors[key] = e.second;
		gActorObjects[e.second] = key;
	}
}

// How far each spawned actor's yaw sits from its object's facing: the mesh and
// fixed-angle offsets above. Absent means zero. Kept so a later turn goes
// through the same sum the spawn did.
static std::map<const AActor *, double> gActorYawOffset;

double ObjectActorYaw(const AActor *mo, uint8_t rotation)
{
	double deg = ObjectYaw(rotation);
	auto it = gActorYawOffset.find(mo);
	if (it != gActorYawOffset.end()) deg += it->second;
	deg = fmod(deg, 360.0);
	if (deg < 0) deg += 360.0;
	return deg;
}

void RebindObjectActors(FLevelLocals *Level)
{
	gObjectActors.clear();
	gActorObjects.clear();
	if (Level == nullptr) return;
	PClassActor *cls = PropClass();
	if (cls == nullptr) return;
	auto it = Level->GetThinkerIterator<AActor>();
	AActor *mo;
	int found = 0;
	while ((mo = it.Next()) != nullptr)
	{
		if (mo->GetClass() != cls || mo->special2 == 0) continue;
		const uint32_t key = (uint32_t)(mo->special2 - 1);
		gObjectActors[key] = mo;
		gActorObjects[mo] = key;
		found++;
	}
	if (found > 0) Printf("Realms: %d props found again after a restore\n", found);
}

void SpawnPreparedObjects(FLevelLocals *Level)
{
	gObjectActors.clear();
	gActorObjects.clear();
	gActorYawOffset.clear();
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

		// So the level logic can find this prop again. See FindObjectActor. A
		// picture hung on a mesh vertex (rothIndex -1) is not an object of the
		// map and is deliberately not findable.
		if (p.rothIndex >= 0)
		{
			gObjectActors[ObjectKey(p.rothSector, p.rothIndex)] = mo;
			// And the reverse, for anything starting from the world: a ray that
			// hits this prop, or a hand that closes on it. See ObjectForActor.
			gActorObjects[mo] = ObjectKey(p.rothSector, p.rothIndex);
		// And on the actor itself, so it survives a hub snapshot or a save
		// (RebindObjectActors). +1 so that 0 stays "not a Realms prop".
		mo->special2 = (int)ObjectKey(p.rothSector, p.rothIndex) + 1;
		}

		// After the spawn, not before: the spawn state sets the sprite, and
		// LoaderProp's spawn state has tics -1 so nothing ever sets it again.
		mo->sprite = p.spriteNum;
		mo->frame = 0;
		mo->Angles.Yaw = DAngle::fromDeg(p.yaw + p.yawOffset);
		if (p.yawOffset != 0) gActorYawOffset[mo] = p.yawOffset;
		mo->Scale = DVector2(p.scaleX, p.scaleY);
		mo->renderflags |= ActorRenderFlags::FromInt(p.renderFlags);
		// The object's own light, already combined with its sector's. The
		// Realms light byte goes into lightlevel unchanged for sectors
		// (rothmap.cpp), so it goes in unchanged here too, and AActor's
		// LightLevel overrides the sector for exactly this purpose
		// (hw_sprites.cpp:1729).
		if (p.lightLevel >= 0) mo->LightLevel = (int16_t)p.lightLevel;
		// The art's lateral anchor: a slide across the screen, which is what
		// SpriteOffset is for. Only X -- the vertical half of the same anchor
		// went into p.z, where it combines with the modifier nibble.
		if (p.spriteOffsetX != 0) mo->SpriteOffset.X = p.spriteOffsetX;
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

//==========================================================================
//
// An actor wearing a logic sprite: a particle of opcode 0x2d. The original
// draws its particle list through the same object path as placed objects
// (renderer.c:8010-8011), so it gets the same size, anchor and light rule as a
// placed picture -- see finishPictureSprite and the light block in Prepare.
//
//==========================================================================

AActor *SpawnObjectActor(FLevelLocals *Level, int rothSector, int rothIndex, double x, double y, double z)
{
	if (Level == nullptr || rothSector < 0 || rothIndex < 0) return nullptr;
	PClassActor *cls = PropClass();
	if (cls == nullptr) return nullptr;
	AActor *mo = AActor::StaticSpawn(Level, cls, DVector3(x, y, z), NO_REPLACE);
	if (mo == nullptr) return nullptr;
	const uint32_t key = ObjectKey(rothSector, rothIndex);
	gObjectActors[key] = mo;
	gActorObjects[mo] = key;
	mo->special2 = (int)key + 1;
	mo->SetZ(z);
	return mo;
}

int MakeObjectSprite(const FTextureID *tex, const bool *flip, int views, Log *log)
{
	return MakeRuntimeSprite(tex, flip, views, log);
}

AActor *SpawnLogicSpriteActor(FLevelLocals *Level, uint16_t textureWord, double x, double y, double z, int lightLevel)
{
	if (Level == nullptr) return nullptr;
	auto it = gLogicSprites.find(textureWord);
	if (it == gLogicSprites.end()) return nullptr;
	PClassActor *cls = PropClass();
	if (cls == nullptr) return nullptr;
	LogicSpriteLook look;
	auto lk = gLogicSpriteLook.find(textureWord);
	if (lk != gLogicSpriteLook.end()) look = lk->second;
	AActor *mo = AActor::StaticSpawn(Level, cls, DVector3(x, y, z + look.zShift), NO_REPLACE);
	if (mo == nullptr) return nullptr;
	mo->sprite = it->second;
	mo->frame = 0;
	mo->Scale = DVector2(look.scale, look.scale);
	if (look.offsetX != 0.0) mo->SpriteOffset.X = look.offsetX;
	if (lightLevel >= 0) mo->LightLevel = (int16_t)lightLevel;
	mo->SetZ(z + look.zShift);
	return mo;
}

bool MoveLogicSpriteActor(AActor *mo, uint16_t textureWord, double x, double y, double z)
{
	if (mo == nullptr) return false;
	double shift = 0.0;
	auto lk = gLogicSpriteLook.find(textureWord);
	if (lk != gLogicSpriteLook.end()) shift = lk->second.zShift;
	mo->SetOrigin(DVector3(x, y, z + shift), true);
	return true;
}

} // namespace roth
