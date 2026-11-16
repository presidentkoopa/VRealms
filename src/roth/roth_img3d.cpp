//
// roth_img3d.cpp -- THE ITEM MODELS IN THE ENGINE. See roth_img3d.h; the file formats are roth_img3d_core.h.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include "roth_img3d.h"
#include "roth_img3d_core.h"
#include "roth_objects.h"
#include "roth_dbase100.h"
#include "roth_vrpick.h"
#include "roth_colt.h"

#include "actor.h"
#include "actorinlines.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "cmdlib.h"
#include "printf.h"
#include "texturemanager.h"
#include "gametexture.h"
#include "textures.h"
#include "image.h"
#include "bitmap.h"
#include "r_data/sprites.h"
#include "r_data/models.h"
#include "r_data/vr_handpose.h"
#include "r_data/vr_handpose_math.h"
#include "model.h"
#include "model_memorymesh.h"
#include "fbx_image.h"
#include "hw_vrmodes.h"

#include <math.h>
#include <stdio.h>
#include <map>
#include <string>
#include <tuple>
#include <vector>

// The folder of the models (roth_img3d.h). Not archived: nothing here is ever written to the shared ini.
CVAR(String, roth_img3d_dir, "", 0)

namespace roth
{

namespace
{

// A picture kept encoded (the GLB's own PNG/JPG bytes, copied into ImageArena) and decoded by the engine's
// stb_image when the texture is first uploaded -- as models_fbx.cpp does for an FBX's embedded pictures.
class FImg3dImage : public FImageSource
{
	const uint8_t *Bytes;
	size_t Size;
public:
	FImg3dImage(const uint8_t *bytes, size_t size, int w, int h) : Bytes(bytes), Size(size)
	{
		Width = w;
		Height = h;
		bMasked = false;
		bTranslucent = 0;
	}
	int CopyPixels(FBitmap *bmp, int /*conversion*/, int /*frame*/) override
	{
		int w = 0, h = 0;
		std::vector<uint8_t> rgba;
		if (!FBXSkinned::DecodeRGBA8(Bytes, Size, w, h, rgba) || w != Width || h != Height) return 0;
		bmp->CopyPixelDataRGB(0, 0, rgba.data(), w, h, 4, w * 4, 0, CF_RGBA);
		return 0;
	}
};

// One grey 8 x 8 for a mesh that came without a picture (it is still drawn, and says so).
class FImg3dGrey : public FImageSource
{
public:
	FImg3dGrey() { Width = Height = 8; bMasked = false; bTranslucent = 0; }
	int CopyPixels(FBitmap *bmp, int, int) override
	{
		uint8_t px[8 * 8 * 4];
		for (int i = 0; i < 64; i++) { px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = 160; px[i * 4 + 3] = 255; }
		bmp->CopyPixelDataRGB(0, 0, px, 8, 8, 4, 32, 0, CF_RGBA);
		return 0;
	}
};

constexpr double kDeg = 3.14159265358979323846 / 180.0;

enum Kind { KIND_WORLD = 0, KIND_HAND_RIGHT = 1, KIND_HAND_LEFT = 2 };

struct ItemModel
{
	img3d::Entry entry;
	img3d::Mesh mesh;
	FTextureID tex = FNullTextureID();
	bool texTried = false;
};

struct State
{
	bool read = false;
	std::string folder;               // the folder last read
	int generation = 0;               // bumped on every read: the model and texture names carry it
	std::map<int, ItemModel> models;      // item -> its model
	std::map<std::tuple<int, int, const PClass *>, int> sprites;   // (item, kind, class) -> this load's sprite
	// The objects wearing a model: what to give back when their render id stops naming one.
	// The picture is what the game picks by (roth_runtime.cpp ObjectPickBox): its sprite, scale and z are kept.
	struct Worn { int item = 0; int pictureSprite = -1; DVector2 scale; double z = 0; double renderRadius = 0; int key = 0;
		int modelSprite = -1; DVector2 modelScale; };
	std::map<AActor *, Worn> worn;
	AActor *held[kImg3dHeldSlots] = {};
	bool shown[kImg3dHeldSlots] = {};
	int loggedSlot[kImg3dHeldSlots] = {};
	FLevelLocals *sweptLevel = nullptr;
};
State S;

// img3d_path.txt beside the executable, read once (and again by `roth_img3d reload`): this is asked on every
// object a load spawns and every object the actor rules dress, so it must not open a file each time.
std::string g_pathFile;
bool g_pathFileRead = false;

std::string FolderNow()
{
	std::string dir = *roth_img3d_dir;
	if (dir.empty())
	{
		if (!g_pathFileRead)
		{
			g_pathFileRead = true;
			g_pathFile.clear();
			const std::string file = std::string(progdir.GetChars()) + "img3d_path.txt";
			if (FILE *f = fopen(file.c_str(), "rb"))
			{
				char line[1024] = {};
				if (fgets(line, sizeof line, f) != nullptr) g_pathFile = line;
				fclose(f);
			}
		}
		dir = g_pathFile;
	}
	while (!dir.empty() && (dir.back() == '\n' || dir.back() == '\r' || dir.back() == ' ' || dir.back() == '"')) dir.pop_back();
	while (!dir.empty() && (dir.front() == ' ' || dir.front() == '"')) dir.erase(dir.begin());
	for (char &c : dir) if (c == '\\') c = '/';
	// A relative folder is the executable's, whatever the working directory is (the bridge's test cfg names the
	// repository's placeholder boxes as ../../tools/img3d/placeholder from build-dxr/<config>/).
	const bool absolute = !dir.empty() && (dir[0] == '/' || (dir.size() > 1 && dir[1] == ':'));
	if (!dir.empty() && !absolute)
	{
		std::string base = progdir.GetChars();
		for (char &c : base) if (c == '\\') c = '/';
		if (!base.empty() && base.back() != '/') base += '/';
		dir = base + dir;
	}
	if (!dir.empty() && dir.back() != '/') dir += '/';
	return dir;
}

std::vector<uint8_t> ReadAll(const std::string &path)
{
	std::vector<uint8_t> b;
	FILE *f = fopen(path.c_str(), "rb");
	if (f == nullptr) return b;
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n > 0 && n < (256l << 20))
	{
		b.resize((size_t)n);
		if (fread(b.data(), 1, (size_t)n, f) != (size_t)n) b.clear();
	}
	fclose(f);
	return b;
}

void ReadFolder(bool force)
{
	const std::string dir = FolderNow();
	if (S.read && !force && dir == S.folder) return;
	S.read = true;
	S.folder = dir;
	S.generation++;
	S.models.clear();
	S.sprites.clear();
	if (dir.empty()) { Printf("[roth_img3d] no model folder (roth_img3d_dir empty, no img3d_path.txt beside the exe): every item keeps its picture\n"); return; }
	const std::vector<uint8_t> man = ReadAll(dir + "manifest.json");
	if (man.empty()) { Printf("[roth_img3d] %smanifest.json: missing or empty -- every item keeps its picture\n", dir.c_str()); return; }
	std::vector<img3d::Entry> entries;
	std::vector<std::string> skipped;
	std::string err;
	if (!img3d::ParseManifest((const char *)man.data(), man.size(), entries, err, &skipped))
	{
		Printf(TEXTCOLOR_RED "[roth_img3d] %smanifest.json: %s -- every item keeps its picture\n", dir.c_str(), err.c_str());
		return;
	}
	for (const std::string &s : skipped) Printf(TEXTCOLOR_RED "[roth_img3d] manifest %s: skipped\n", s.c_str());
	for (const img3d::Entry &e : entries)
	{
		const std::vector<uint8_t> glb = ReadAll(dir + e.file);
		ItemModel m;
		m.entry = e;
		if (glb.empty()) { Printf(TEXTCOLOR_RED "[roth_img3d] item %d: %s%s missing -- it keeps its picture\n", e.item, dir.c_str(), e.file.c_str()); continue; }
		if (!img3d::ReadGlb(glb.data(), glb.size(), m.mesh, err))
		{
			Printf(TEXTCOLOR_RED "[roth_img3d] item %d: %s: %s -- it keeps its picture\n", e.item, e.file.c_str(), err.c_str());
			continue;
		}
		if (m.mesh.Longest() <= 1e-5f) { Printf(TEXTCOLOR_RED "[roth_img3d] item %d: %s has no size -- it keeps its picture\n", e.item, e.file.c_str()); continue; }
		Printf("[roth_img3d] item %d (%s): %s, %d triangles, %d vertices, %.3f m longest (%s), %s picture %s, hold %s\n", e.item,
			e.name.empty() ? "?" : e.name.c_str(), e.file.c_str(), (int)m.mesh.Triangles(), (int)m.mesh.Vertices(), m.mesh.Longest(),
			e.sizeFromTable ? "its real size" : "sized to its picture", m.mesh.image.empty() ? "NO" : m.mesh.imageMime.c_str(),
			m.mesh.image.empty() ? "(grey)" : FStringf("%u bytes", (unsigned)m.mesh.image.size()).GetChars(), img3d::HoldName(e.hold));
		S.models[e.item] = std::move(m);
	}
	Printf("[roth_img3d] %s: %d model(s) read\n", dir.c_str(), (int)S.models.size());
}

ItemModel *FindModelFor(int item)
{
	ReadFolder(false);
	auto it = S.models.find(item);
	return it == S.models.end() ? nullptr : &it->second;
}

FTextureID ModelTexture(int item, ItemModel &m)
{
	if (m.texTried) return m.tex;
	m.texTried = true;
	const FString name = FStringf("IMG3D_G%d_%d", S.generation, item);
	FTextureID have = TexMan.CheckForTexture(name.GetChars(), ETextureType::Any);
	if (have.isValid()) return m.tex = have;
	FImageSource *src = nullptr;
	FBXSkinned::ImageInfo info;
	if (!m.mesh.image.empty() && FBXSkinned::ImageInfoOf(m.mesh.image.data(), m.mesh.image.size(), info))
	{
		uint8_t *bytes = (uint8_t *)ImageArena.Alloc(m.mesh.image.size());
		memcpy(bytes, m.mesh.image.data(), m.mesh.image.size());
		src = new FImg3dImage(bytes, m.mesh.image.size(), info.width, info.height);
	}
	else
	{
		Printf(TEXTCOLOR_RED "[roth_img3d] item %d: %s -- drawn grey\n", item, m.mesh.image.empty() ? "the GLB has no base colour picture" : "its picture does not decode");
		src = new FImg3dGrey();
	}
	auto *tex = MakeGameTexture(new FImageTexture(src), name.GetChars(), ETextureType::Override);
	m.tex = TexMan.AddGameTexture(tex);
	return m.tex;
}

// The mesh in the engine's model space for `kind` (roth_img3d_core.h): world in metres (the actor's Scale makes map
// units), hands in the grip frame's metres.
FMemoryMeshData Build(ItemModel &m, int kind, FTextureID tex)
{
	FMemoryMeshData d;
	const img3d::Mesh &me = m.mesh;
	const size_t nv = me.Vertices();
	d.vertices.Resize((unsigned)nv);
	std::vector<FVector3> normals(me.nrm.empty() ? 0 : nv);
	for (size_t v = 0; v < nv; v++)
	{
		float o[3];
		const float *g = &me.pos[v * 3];
		if (kind == KIND_WORLD) img3d::WorldPoint(g, 1.0, 1.0, o);
		else img3d::HandPoint(m.entry, kind == KIND_HAND_RIGHT, g, o);
		d.vertices[(unsigned)v] = FVector3(o[0], o[1], o[2]);
		if (!normals.empty())
		{
			const float *n = &me.nrm[v * 3];
			if (kind == KIND_WORLD) img3d::WorldDirection(n, o);
			else img3d::HandDirection(m.entry, kind == KIND_HAND_RIGHT, n, o);
			normals[v] = FVector3(o[0], o[1], o[2]);
		}
	}
	const size_t nt = me.Triangles();
	d.faces.Resize((unsigned)nt);
	for (size_t t = 0; t < nt; t++)
	{
		FMemoryMeshFace &f = d.faces[(unsigned)t];
		f.count = 3;
		f.skin = tex;
		f.hasNormals = !normals.empty();
		for (int k = 0; k < 3; k++)
		{
			const uint32_t i = me.idx[t * 3 + k];
			f.vertex[k] = (int)i;
			f.uv[k] = me.uv.empty() ? FVector2(0.f, 0.f) : FVector2(me.uv[i * 2], me.uv[i * 2 + 1]);
			if (f.hasNormals) f.normal[k] = normals[i];
		}
	}
	d.doubleSided = false;   // opaque models are not culled (hw_models.cpp BeginDrawModel), so the winding does not matter
	return d;
}

int SpriteFor(int item, ItemModel &m, int kind, PClassActor *cls)
{
	if (cls == nullptr) return -1;
	const auto key = std::make_tuple(item, kind, static_cast<const PClass *>(cls));
	auto it = S.sprites.find(key);
	if (it != S.sprites.end()) return it->second;
	const FTextureID tex = ModelTexture(item, m);
	const char *kn = kind == KIND_WORLD ? "world" : kind == KIND_HAND_RIGHT ? "handR" : "handL";
	const FString mkey = FStringf("img3d/g%d/%d/%s", S.generation, item, kn);
	const int sn = BindMemoryMesh(mkey.GetChars(), Build(m, kind, tex), cls, nullptr);
	if (sn < 0) Printf(TEXTCOLOR_RED "[roth_img3d] item %d: the %s model could not be bound to %s -- its picture is kept\n", item, kn, cls->TypeName.GetChars());
	S.sprites[key] = sn;
	return sn;
}

double UnitsPerMetre() { return VRPickUnitsPerMetre(); }
double Stretch(FLevelLocals *level) { return level != nullptr && level->pixelstretch > 0 ? level->pixelstretch : 1.0; }

// The picture an actor wears now: its longest side in metres and its bottom (map z). False when it has none.
bool PictureNow(AActor *mo, double &longestMetres, double &bottomZ)
{
	if (mo == nullptr || mo->sprite < 0 || (unsigned)mo->sprite >= sprites.Size()) return false;
	const spritedef_t &sd = sprites[mo->sprite];
	if (sd.numframes < 1 || sd.spriteframes >= SpriteFrames.Size()) return false;
	FGameTexture *t = TexMan.GetGameTexture(SpriteFrames[sd.spriteframes].Texture[0], false);
	if (t == nullptr || t->GetDisplayHeight() <= 0) return false;
	const double upm = UnitsPerMetre(), ps = Stretch(mo->Level);
	const double w = t->GetDisplayWidth() * mo->Scale.X / upm;
	const double h = t->GetDisplayHeight() * mo->Scale.Y * ps / upm;
	longestMetres = w > h ? w : h;
	bottomZ = mo->Z() + (t->GetDisplayTopOffset() - t->GetDisplayHeight()) * mo->Scale.Y;
	return true;
}

void GiveBack(AActor *mo, const State::Worn &w)
{
	mo->Scale = w.scale;
	mo->PrevScale = w.scale;
	mo->renderradius = w.renderRadius;
	mo->SetOrigin(DVector3(mo->X(), mo->Y(), w.z), false);
	mo->SetZ(w.z);
	mo->ClearInterpolation();
}

PClassActor *HeldClass()
{
	static PClassActor *cls = nullptr;
	if (cls == nullptr) cls = PClass::FindActor("RealmsItemModel");
	return cls;
}

} // namespace

// THE COLT'S OWN MESH LYING IN THE WORLD (owner 2026-11-16: "lying in the world as the same mesh"): item 9's object
// wears vrealms_iwad/models/weapons/colt1911 (the mesh drawn in the hand, CC BY 4.0), not a generated model: the whole
// mesh at rest on its left side (MODELDEF block angles: pitch -90 about the bore lays the gun's -X side up), 0.216 m long,
// its lowest point on the picture's bottom. The picture is kept for picking, as every world model here.
namespace
{
constexpr double kColtUnit = 0.000271900171;                 // metres a file unit (vrealms_iwad/MODELDEF.txt)
constexpr double kColtRightSide = 56.73;                     // the mesh's +X extent: on its left side, the depth below its origin
std::map<const PClass *, int> gColtSprites;
int ColtWorldSprite(PClassActor *cls)
{
	auto it = gColtSprites.find(cls);
	if (it != gColtSprites.end()) return it->second;
	const int sn = BindModelFile("colt1911/world", "models/weapons/colt1911/", "colt1911.fbx:Default_1", "colt1911_diffuse.jpg", cls, 0.f, -90.f, 0.f, nullptr);
	if (sn < 0) Printf(TEXTCOLOR_RED "[roth_img3d] the Colt's mesh could not be bound to %s -- its picture is kept\n", cls->TypeName.GetChars());
	gColtSprites[cls] = sn;
	return sn;
}
}

void Img3dNewLoad()
{
	gColtSprites.clear();
	S.sprites.clear();
	S.worn.clear();
}

bool Img3dHas(int item) { return FindModelFor(item) != nullptr; }

bool Img3dWearWorld(AActor *mo, uint16_t renderId, uint8_t rotation, bool fixedAngle)
{
	if (mo == nullptr) return false;
	ReadFolder(false);
	auto was = S.worn.find(mo);
	const bool coltItem = TheDbase100().FindItemByRenderId(renderId) == colt::kColt;
	if (!coltItem && S.models.empty() && was == S.worn.end()) return false;   // the common case: no models, nothing worn
	const bool keyMatch = was != S.worn.end() && was->second.key == mo->special2;
	if (was != S.worn.end() && !keyMatch) { S.worn.erase(was); was = S.worn.end(); }   // another actor at that address
	const int item = TheDbase100().FindItemByRenderId(renderId);
	if (coltItem && item == colt::kColt)
	{
		const int sn = ColtWorldSprite(mo->GetClass());
		if (sn >= 0)
		{
			if (keyMatch && mo->sprite == sn && was->second.item == item) return true;
			State::Worn w = keyMatch ? was->second : State::Worn();
			w.item = item;
			w.pictureSprite = mo->sprite;
			if (!keyMatch) { w.scale = mo->Scale; w.z = mo->Z(); w.renderRadius = mo->renderradius; w.key = mo->special2; }
			double picMetres = 0.0, bottom = mo->Z();
			const bool havePic = PictureNow(mo, picMetres, bottom);
			const double upm = UnitsPerMetre(), ps = Stretch(mo->Level);
			const double k = upm * kColtUnit;
			mo->sprite = sn;
			mo->frame = 0;
			mo->Scale = DVector2(k, k / ps);
			mo->PrevScale = mo->Scale;
			w.modelSprite = sn;
			w.modelScale = mo->Scale;
			S.worn[mo] = w;
			double z = (havePic ? bottom : mo->Z()) + kColtRightSide * k / ps;
			if (mo->Sector != nullptr) { const double fz = mo->Sector->floorplane.ZatPoint(mo->Pos()); if (z < fz) z = fz; }
			mo->Angles.Yaw = DAngle::fromDeg(ObjectYaw(rotation));
			mo->renderradius = 0.5 * 0.216 * upm;
			mo->SetOrigin(DVector3(mo->X(), mo->Y(), z), false);
			mo->SetZ(z);
			mo->ClearInterpolation();
			if (!keyMatch) Printf("[roth_img3d] object (key %d) shows item 9: the Colt's own mesh (colt1911.fbx), 0.216 m on its left side, its origin at z %.1f (the picture's bottom %.1f), yaw %.0f\n",
				mo->special2 - 1, z, bottom, ObjectYaw(rotation));
			return true;
		}
	}
	ItemModel *m = item > 0 ? FindModelFor(item) : nullptr;
	if (m == nullptr)
	{
		// No model for what it shows now: if it wore one, the caller has set the picture's sprite; give back the rest.
		if (keyMatch) { GiveBack(mo, was->second); S.worn.erase(was); }
		return false;
	}
	const int sn = SpriteFor(item, *m, KIND_WORLD, mo->GetClass());
	if (sn < 0) return false;
	if (keyMatch && mo->sprite == sn && was->second.item == item) return true;   // still wearing it

	// The caller has just put the item's picture on (its sprite; the actor rules' Dress its scale too): that picture
	// is what the model stands in for, and what the game keeps picking by.
	State::Worn w = keyMatch ? was->second : State::Worn();
	w.item = item;
	w.pictureSprite = mo->sprite;
	if (!keyMatch)
	{
		w.scale = mo->Scale;
		w.z = mo->Z();
		w.renderRadius = mo->renderradius;
		w.key = mo->special2;
	}
	else
	{
		if (mo->Scale != w.modelScale) w.scale = mo->Scale;   // Dress set the picture's own scale
		mo->Scale = w.scale;
		mo->SetZ(w.z);
	}
	double picMetres = 0.0, bottom = mo->Z();
	const bool havePic = PictureNow(mo, picMetres, bottom);

	const double upm = UnitsPerMetre(), ps = Stretch(mo->Level);
	const double meshLong = m->mesh.Longest();
	const double metres = m->entry.sizeFromTable ? meshLong : (havePic ? picMetres : meshLong);
	const double k = upm * metres / meshLong;          // map units per mesh metre
	mo->sprite = sn;
	mo->frame = 0;
	mo->Scale = DVector2(k, k / ps);
	mo->PrevScale = mo->Scale;
	w.modelSprite = sn;
	w.modelScale = mo->Scale;
	S.worn[mo] = w;
	// The base on the picture's bottom, never under the floor.
	double z = havePic ? bottom : mo->Z();
	if (mo->Sector != nullptr)
	{
		const double fz = mo->Sector->floorplane.ZatPoint(mo->Pos());
		if (z < fz) z = fz;
	}
	// The original's facing; a picture hung flat on a wall (renderType bit 7) stands its model's back on the wall.
	const double yaw = ObjectYaw(rotation) + m->entry.worldYaw;
	DVector3 pos(mo->X(), mo->Y(), z);
	if (fixedAngle)
	{
		const double halfDepth = 0.5 * (m->mesh.hi[2] - m->mesh.lo[2]) * k;
		pos.X += cos(yaw * kDeg) * halfDepth;
		pos.Y += sin(yaw * kDeg) * halfDepth;
	}
	mo->Angles.Yaw = DAngle::fromDeg(yaw);
	const double ex = (m->mesh.hi[0] - m->mesh.lo[0]) * k, ez = (m->mesh.hi[2] - m->mesh.lo[2]) * k;
	mo->renderradius = 0.5 * sqrt(ex * ex + ez * ez);
	mo->SetOrigin(pos, false);
	mo->SetZ(z);
	mo->ClearInterpolation();
	if (!keyMatch)
		Printf("[roth_img3d] object (key %d) shows item %d: its model, %.2f m (%s), base at z %.1f (the record's %.1f), yaw %.0f%s\n", mo->special2 - 1, item,
			metres, m->entry.sizeFromTable ? "its real size" : (havePic ? "its picture's size" : "the mesh's own size"), z, w.z, yaw,
			fixedAngle ? ", back on the wall" : "");
	return true;
}

bool Img3dPickPicture(const AActor *mo, int &sprite, DVector2 &scale, double &z)
{
	auto it = S.worn.find(const_cast<AActor *>(mo));
	if (mo == nullptr || it == S.worn.end() || it->second.key != mo->special2 || mo->sprite != it->second.modelSprite) return false;
	sprite = it->second.pictureSprite;
	scale = it->second.scale;
	z = it->second.z;
	return true;
}

bool Img3dShowHeld(FLevelLocals *level, int slot, int item, const DVector3 &at, int role, double fallbackMetres, double faceYawDeg)
{
	if (level == nullptr || slot < 0 || slot >= kImg3dHeldSlots) return false;
	ItemModel *m = FindModelFor(item);
	PClassActor *cls = HeldClass();
	if (m == nullptr || cls == nullptr) return false;
	const bool handLocked = role >= 0 && m->entry.hold != img3d::HOLD_HANG;
	const bool right = role >= 0 && VRHandPose_SideForRole(role) == VRHandPose::SIDE_RIGHT;
	const int kind = handLocked ? (right ? KIND_HAND_RIGHT : KIND_HAND_LEFT) : KIND_WORLD;
	const int sn = SpriteFor(item, *m, kind, cls);
	if (sn < 0) return false;

	AActor *&mo = S.held[slot];
	if (mo != nullptr && ((mo->ObjectFlags & OF_EuthanizeMe) || mo->Level != level)) mo = nullptr;
	if (mo == nullptr) mo = AActor::StaticSpawn(level, cls, at, NO_REPLACE);
	if (mo == nullptr) return false;
	mo->flags5 |= MF5_NOTIMEFREEZE;   // the hands work the inventory while it pauses the world
	mo->sprite = sn;
	mo->frame = 0;
	mo->renderflags &= ~RF_INVISIBLE;
	const double meshLong = m->mesh.Longest();
	const double metres = m->entry.sizeFromTable ? meshLong : (fallbackMetres > 0 ? fallbackMetres : meshLong);
	if (handLocked)
	{
		// The grip frame is metres: the mesh's own size, or the card's size when the manifest's is not the real one.
		const double s = metres / meshLong;
		mo->FollowHandMode = (role == VR_MAINHAND) ? FOLLOWHAND_GRIP_MAIN : FOLLOWHAND_GRIP_OFF;
		mo->Scale = DVector2(s, s);
		mo->SetOrigin(at, true);
	}
	else
	{
		// Upright at the point: the mesh's middle where the card's centre was (roth_held puts a hanging Lamp's card
		// centre below its bail), front toward `faceYawDeg`. Placed each tic, so in a headset it follows the hand at
		// the tic rate, not the draw rate (reasoning only: a hand-locked frame would swing it with the wrist).
		const double upm = UnitsPerMetre(), ps = Stretch(level);
		const double k = upm * metres / meshLong;
		mo->FollowHandMode = 0;
		mo->Scale = DVector2(k, k / ps);
		const double z = at.Z - 0.5 * (m->mesh.lo[1] + m->mesh.hi[1]) * k / ps;
		mo->Angles.Yaw = DAngle::fromDeg(faceYawDeg);
		mo->SetOrigin(DVector3(at.X, at.Y, z), true);
	}
	mo->PrevScale = mo->Scale;
	S.shown[slot] = true;
	if (S.loggedSlot[slot] != item * 4 + kind)
	{
		Printf("[roth_img3d] held slot %d: item %d's model, %s, %.3f m\n", slot, item,
			kind == KIND_WORLD ? (role >= 0 ? "hanging upright at the hand" : "standing at the card's place") : (right ? "in the right hand's grip frame" : "in the left hand's grip frame"),
			metres);
		S.loggedSlot[slot] = item * 4 + kind;
	}
	return true;
}

void Img3dHeldEndTick(FLevelLocals *level)
{
	// Once a level: the held models a loaded save brought back are not this tic's; they go (roth_hands.cpp Adopt).
	if (level != nullptr && S.sweptLevel != level)
	{
		S.sweptLevel = level;
		if (PClassActor *cls = HeldClass())
		{
			auto it = level->GetThinkerIterator<AActor>(cls->TypeName);
			TArray<AActor *> stray;
			AActor *mo;
			while ((mo = it.Next()) != nullptr)
			{
				bool ours = false;
				for (AActor *h : S.held) if (h == mo) ours = true;
				if (!ours && !(mo->ObjectFlags & OF_EuthanizeMe)) stray.Push(mo);
			}
			for (AActor *x : stray) x->Destroy();
		}
	}
	for (int s = 0; s < kImg3dHeldSlots; s++)
	{
		AActor *&mo = S.held[s];
		if (mo != nullptr && ((mo->ObjectFlags & OF_EuthanizeMe) || mo->Level != level)) mo = nullptr;
		if (!S.shown[s] && mo != nullptr)
		{
			mo->renderflags |= RF_INVISIBLE;
			if (S.loggedSlot[s] != 0) { Printf("[roth_img3d] held slot %d: hidden\n", s); S.loggedSlot[s] = 0; }
		}
		S.shown[s] = false;
	}
}

void Img3dEndLevel()
{
	// The actors go with the level (as roth_hands.cpp HandsEndLevel): only the pointers are dropped.
	S.sweptLevel = nullptr;
	for (int s = 0; s < kImg3dHeldSlots; s++)
	{
		S.held[s] = nullptr;
		S.shown[s] = false;
		S.loggedSlot[s] = 0;
	}
	S.worn.clear();
}

} // namespace roth

//===========================================================================
//
// roth_img3d -- the item models' folder.
//   roth_img3d status    the folder, and each model read (item, file, triangles, size, hold)
//   roth_img3d reload    read the folder again (a map reload binds the new meshes; objects already wearing a model
//                        keep the old one until then)
//
//===========================================================================

CCMD(roth_img3d)
{
	using namespace roth;
	if (argv.argc() >= 2 && !stricmp(argv[1], "reload"))
	{
		g_pathFileRead = false;
		ReadFolder(true);
		return;
	}
	ReadFolder(false);
	Printf("[roth_img3d] folder '%s' (generation %d): %d model(s)\n", S.folder.c_str(), S.generation, (int)S.models.size());
	for (const auto &p : S.models)
	{
		const img3d::Entry &e = p.second.entry;
		Printf("  item %d %s: %s, %d tris, %.3f m (%s), grip (%.3f %.3f %.3f) turn (%.0f %.0f %.0f), hold %s\n", e.item, e.name.c_str(), e.file.c_str(),
			(int)p.second.mesh.Triangles(), p.second.mesh.Longest(), e.sizeFromTable ? "real" : "to the picture", e.grip[0], e.grip[1], e.grip[2],
			e.gripRot[0], e.gripRot[1], e.gripRot[2], img3d::HoldName(e.hold));
	}
}
