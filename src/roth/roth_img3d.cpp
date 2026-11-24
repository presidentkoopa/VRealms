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
#include <set>
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

// One colour 8 x 8: a part drawn in its material's colour (film_build.py's glass, liquid, gems), or grey for a mesh
// that came without a picture (it is still drawn, and says so).
class FImg3dColour : public FImageSource
{
	uint8_t R, G, B;
public:
	FImg3dColour(uint8_t r = 160, uint8_t g = 160, uint8_t b = 160) : R(r), G(g), B(b) { Width = Height = 8; bMasked = false; bTranslucent = 0; }
	int CopyPixels(FBitmap *bmp, int, int) override
	{
		uint8_t px[8 * 8 * 4];
		for (int i = 0; i < 64; i++) { px[i * 4] = R; px[i * 4 + 1] = G; px[i * 4 + 2] = B; px[i * 4 + 3] = 255; }
		bmp->CopyPixelDataRGB(0, 0, px, 8, 8, 4, 32, 0, CF_RGBA);
		return 0;
	}
};

// A companion (THE PARTS below): a RealmsItemModel drawing one more part of the model its master wears.
constexpr int kCompanionTag = 0x1A3D;
// A flight (Img3dFlyShow): a RealmsItemModel flying an item's model on its own.
constexpr int kFlightTag = 0x1A3E;

constexpr double kDeg = 3.14159265358979323846 / 180.0;

enum Kind { KIND_WORLD = 0, KIND_HAND_RIGHT = 1, KIND_HAND_LEFT = 2, KIND_LOOSE = 3 };   // LOOSE: the world's mesh, turned by the actor's pitch and roll (roth_physics)

struct ItemModel
{
	img3d::Entry entry;
	img3d::Mesh mesh;
	std::vector<FTextureID> tex;     // one a part
	std::vector<bool> texTried;
};

struct State
{
	bool read = false;
	std::string folder;               // the folder last read
	int generation = 0;               // bumped on every read: the model and texture names carry it
	std::map<int, ItemModel> models;      // item -> its model
	std::map<std::tuple<int, int, const PClass *, int>, int> sprites;   // (item, kind, class, part) -> this load's sprite
	// THE PARTS: a sprite wearing the primary part of a model with more parts -> which model and placement, so the
	// companions that draw the other parts can follow whoever wears it (PartsTick).
	struct Multi { int item = 0; int kind = 0; };
	std::map<int, Multi> multi;
	// The objects wearing a model: what to give back when their render id stops naming one.
	// The picture is what the game picks by (roth_runtime.cpp ObjectPickBox): its sprite, scale and z are kept.
	struct Worn { int item = 0; int pictureSprite = -1; DVector2 scale; double z = 0; double renderRadius = 0; int key = 0;
		int modelSprite = -1; DVector2 modelScale;
		bool styled = false; FRenderStyle style; double alpha = 1.0; bool fullbright = false; };   // a gem's own look, given back
	std::map<AActor *, Worn> worn;
	AActor *held[kImg3dHeldSlots] = {};
	bool shown[kImg3dHeldSlots] = {};
	AActor *flight[kImg3dFlights] = {};
	AActor *loose[kImg3dLoose] = {};
	bool looseShown[kImg3dLoose] = {};
	bool heldRelSet[2] = { false, false };
	double heldRel[2][12] = {};   // body frame -> hand frame: rotation (3 x 3, rows) then translation, metres
	bool flown[kImg3dFlights] = {};
	int loggedFlight[kImg3dFlights] = {};
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
	S.multi.clear();
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
		if (m.mesh.parts.size() > 1 || !m.mesh.parts[0].Plain())
			for (size_t k = 0; k < m.mesh.parts.size(); k++)
			{
				const img3d::Part &p = m.mesh.parts[k];
				Printf("[roth_img3d] item %d part %d '%s': %d triangles, %s, colour %.2f %.2f %.2f%s%s%s\n", e.item, (int)k, p.name.c_str(),
					(int)(p.indexCount / 3), p.image.empty() ? "its colour" : "its picture", p.color[0], p.color[1], p.color[2],
					p.blend ? FStringf(", see-through %.2f", p.color[3]).GetChars() : "", p.glow ? ", glowing (fullbright)" : "",
					(int)k == m.mesh.Primary() ? ", worn by the object itself" : ", drawn by a companion");
			}
		m.tex.assign(m.mesh.parts.size(), FNullTextureID());
		m.texTried.assign(m.mesh.parts.size(), false);
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

FTextureID ModelTexture(int item, ItemModel &m, int part)
{
	if (part < 0 || (size_t)part >= m.tex.size()) return FNullTextureID();
	if (m.texTried[part]) return m.tex[part];
	m.texTried[part] = true;
	const FString name = part == 0 ? FStringf("IMG3D_G%d_%d", S.generation, item) : FStringf("IMG3D_G%d_%d_P%d", S.generation, item, part);
	FTextureID have = TexMan.CheckForTexture(name.GetChars(), ETextureType::Any);
	if (have.isValid()) return m.tex[part] = have;
	const img3d::Part &p = m.mesh.parts[part];
	FImageSource *src = nullptr;
	FBXSkinned::ImageInfo info;
	if (!p.image.empty() && FBXSkinned::ImageInfoOf(p.image.data(), p.image.size(), info))
	{
		uint8_t *bytes = (uint8_t *)ImageArena.Alloc(p.image.size());
		memcpy(bytes, p.image.data(), p.image.size());
		src = new FImg3dImage(bytes, p.image.size(), info.width, info.height);
	}
	else if (p.image.empty() && p.material >= 0)
	{
		// a part whose material is a colour alone (film_build.py)
		auto c8 = [](float v) { return (uint8_t)(v <= 0.f ? 0 : v >= 1.f ? 255 : (int)(v * 255.f + 0.5f)); };
		src = new FImg3dColour(c8(p.color[0]), c8(p.color[1]), c8(p.color[2]));
	}
	else
	{
		Printf(TEXTCOLOR_RED "[roth_img3d] item %d part %d: %s -- drawn grey\n", item, part, p.image.empty() ? "the GLB has no base colour picture" : "its picture does not decode");
		src = new FImg3dColour();
	}
	auto *tex = MakeGameTexture(new FImageTexture(src), name.GetChars(), ETextureType::Override);
	m.tex[part] = TexMan.AddGameTexture(tex);
	return m.tex[part];
}

// The mesh in the engine's model space for `kind` (roth_img3d_core.h): world in metres (the actor's Scale makes map
// units), hands in the grip frame's metres.
FMemoryMeshData Build(ItemModel &m, int kind, FTextureID tex, int part)
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
		if (kind == KIND_WORLD || kind == KIND_LOOSE) img3d::WorldPoint(g, 1.0, 1.0, o);
		else img3d::HandPoint(m.entry, kind == KIND_HAND_RIGHT, g, o);
		d.vertices[(unsigned)v] = FVector3(o[0], o[1], o[2]);
		if (!normals.empty())
		{
			const float *n = &me.nrm[v * 3];
			if (kind == KIND_WORLD || kind == KIND_LOOSE) img3d::WorldDirection(n, o);
			else img3d::HandDirection(m.entry, kind == KIND_HAND_RIGHT, n, o);
			normals[v] = FVector3(o[0], o[1], o[2]);
		}
	}
	const img3d::Part &pa = me.parts[part];
	const size_t t0 = pa.firstIndex / 3, nt = pa.indexCount / 3;
	d.faces.Resize((unsigned)nt);
	for (size_t t = t0; t < t0 + nt; t++)
	{
		FMemoryMeshFace &f = d.faces[(unsigned)(t - t0)];
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

// The sprite wearing `part` of the item's model (-1: the primary part, the one its own object wears).
int SpriteFor(int item, ItemModel &m, int kind, PClassActor *cls, int part = -1)
{
	if (cls == nullptr) return -1;
	const int primary = m.mesh.Primary();
	if (part < 0) part = primary;
	if ((size_t)part >= m.mesh.parts.size()) return -1;
	const auto key = std::make_tuple(item, kind, static_cast<const PClass *>(cls), part);
	auto it = S.sprites.find(key);
	if (it != S.sprites.end()) return it->second;
	const FTextureID tex = ModelTexture(item, m, part);
	const char *kn = kind == KIND_WORLD ? "world" : kind == KIND_LOOSE ? "loose" : kind == KIND_HAND_RIGHT ? "handR" : "handL";
	const FString mkey = part == 0 ? FStringf("img3d/g%d/%d/%s", S.generation, item, kn) : FStringf("img3d/g%d/%d/%s/p%d", S.generation, item, kn, part);
	const int sn = BindMemoryMesh(mkey.GetChars(), Build(m, kind, tex, part), cls, nullptr, kind == KIND_LOOSE ? (MDL_USEACTORPITCH | MDL_USEACTORROLL) : 0);
	if (sn < 0) Printf(TEXTCOLOR_RED "[roth_img3d] item %d: the %s model (part %d) could not be bound to %s -- its picture is kept\n", item, kn, part, cls->TypeName.GetChars());
	S.sprites[key] = sn;
	if (sn >= 0 && part == primary && m.mesh.parts.size() > 1) S.multi[sn] = State::Multi{ item, kind };
	return sn;
}

// A part's look on the actor drawing it: see-through at its alpha, glowing fullbright, or plain.
void ApplyStyle(AActor *mo, const img3d::Part &p)
{
	if (p.blend) { mo->RenderStyle = LegacyRenderStyles[STYLE_Translucent]; mo->Alpha = p.color[3]; }
	else { mo->RenderStyle = LegacyRenderStyles[STYLE_Normal]; mo->Alpha = 1.0; }
	if (p.glow) mo->renderflags |= RF_FULLBRIGHT; else mo->renderflags &= ~RF_FULLBRIGHT;
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
	if (w.styled)
	{
		mo->RenderStyle = w.style;
		mo->Alpha = w.alpha;
		if (w.fullbright) mo->renderflags |= RF_FULLBRIGHT; else mo->renderflags &= ~RF_FULLBRIGHT;
	}
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
// mesh at rest on its right side (MODELDEF block angles: pitch +90 about the bore lays the gun's +X side down: its grip toward the viewer, as the original's picture lies), 0.216 m long,
// its lowest point on the picture's bottom. The picture is kept for picking, as every world model here.
namespace
{
constexpr double kColtUnit = 0.000271900171;                 // metres a file unit (vrealms_iwad/MODELDEF.txt)
constexpr double kColtRightSide = 62.72;                     // the larger of the mesh's side extents (-X 62.72, +X 56.73): on its side it never sinks into the desk
std::map<const PClass *, int> gColtSprites;
int ColtWorldSprite(PClassActor *cls)
{
	auto it = gColtSprites.find(cls);
	if (it != gColtSprites.end()) return it->second;
	const int sn = BindModelFile("colt1911/world", "models/weapons/colt1911/", "colt1911.fbx:Default_1", "colt1911_diffuse.jpg", cls, 0.f, 90.f, 0.f, nullptr);
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
			// THE ORIGINAL'S SIZE: the mesh's length (794.41 file units, muzzle to tang) on the longest side of the
			// picture the original draws there (as every world model here sized "picture"), so the desk shows the Colt as
			// big as the original's frame does; without a picture, its real 0.216 m at the VR metre.
			const double upm = UnitsPerMetre(), ps = Stretch(mo->Level);
			const double k = havePic ? picMetres * upm / 794.41 : upm * kColtUnit;
			mo->sprite = sn;
			mo->frame = 0;
			mo->Scale = DVector2(k, k / ps);
			mo->PrevScale = mo->Scale;
			w.modelSprite = sn;
			w.modelScale = mo->Scale;
			S.worn[mo] = w;
			double z = (havePic ? bottom : mo->Z()) + kColtRightSide * k / ps;
			if (mo->Sector != nullptr) { const double fz = mo->Sector->floorplane.ZatPoint(mo->Pos()); if (z < fz) z = fz; }
			// the barrel along the picture's own long side, as the original draws the gun (its frame: the muzzle to the right)
			mo->Angles.Yaw = DAngle::fromDeg(ObjectYaw(rotation) - 90.0);
			mo->renderradius = 0.5 * 794.41 * k;
			mo->SetOrigin(DVector3(mo->X(), mo->Y(), z), false);
			mo->SetZ(z);
			mo->ClearInterpolation();
			if (!keyMatch) Printf("[roth_img3d] object (key %d) shows item 9: the Colt's own mesh (colt1911.fbx) on its side, %.1f units long (%s), its origin at z %.1f (the picture's bottom %.1f), yaw %.0f\n",
				mo->special2 - 1, 794.41 * k, havePic ? "the original's picture's longest side" : "0.216 m at the VR metre", z, bottom, ObjectYaw(rotation) - 90.0);
			if (!keyMatch) Printf("[roth_img3d] the Colt on the desk is %.1f units = %.3f m at the VR metre (%.1f units a metre); in the hand it is 0.216 m = %.1f units: the desk's %.2f times the hand's\n",
				794.41 * k, 794.41 * k / upm, upm, 0.216 * upm, 794.41 * k / (0.216 * upm));
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
	// A primary part with a look of its own (a cut gem, see-through): the object takes it, and gives its own back
	// with the picture (GiveBack). The other parts are drawn by companions (PartsTick).
	const img3d::Part &prim = m->mesh.parts[m->mesh.Primary()];
	if (!prim.Plain() && !w.styled) { w.styled = true; w.style = mo->RenderStyle; w.alpha = mo->Alpha; w.fullbright = (mo->renderflags & RF_FULLBRIGHT) != 0; }
	if (w.styled) ApplyStyle(mo, prim);
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

namespace
{
// An affine map, rows: m[r][0..2] the linear part, m[r][3] the translation.
struct Aff
{
	double m[3][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };
	void Apply(const double in[3], double out[3]) const
	{
		for (int r = 0; r < 3; r++) out[r] = m[r][0] * in[0] + m[r][1] * in[1] + m[r][2] * in[2] + m[r][3];
	}
	Aff operator*(const Aff &o) const   // this after o
	{
		Aff a;
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 4; c++)
			{
				double v = (c == 3) ? m[r][3] : 0.0;
				for (int k = 0; k < 3; k++) v += m[r][k] * o.m[k][c];
				a.m[r][c] = v;
			}
		return a;
	}
	bool Inverse(Aff &out) const
	{
		const double (&a)[3][4] = m;
		const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
			+ a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
		if (fabs(det) < 1e-12) return false;
		const double id = 1.0 / det;
		double inv[3][3];
		inv[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) * id;
		inv[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) * id;
		inv[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * id;
		inv[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) * id;
		inv[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * id;
		inv[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) * id;
		inv[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) * id;
		inv[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) * id;
		inv[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * id;
		for (int r = 0; r < 3; r++)
		{
			for (int c = 0; c < 3; c++) out.m[r][c] = inv[r][c];
			out.m[r][3] = -(inv[r][0] * a[0][3] + inv[r][1] * a[1][3] + inv[r][2] * a[2][3]);
		}
		return true;
	}
	static Aff Scale(double s) { Aff a; for (int r = 0; r < 3; r++) a.m[r][r] = s; return a; }
	static Aff From12(const double *v) { Aff a; for (int r = 0; r < 3; r++) { for (int c = 0; c < 3; c++) a.m[r][c] = v[r * 3 + c]; a.m[r][3] = v[9 + r]; } return a; }
};

// glTF -> the body frame (metres per mesh metre sigma)
Aff GltfToBody(double sigma)
{
	Aff a;
	for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) a.m[r][c] = 0.0;
	a.m[0][2] = sigma;   // x = g.z
	a.m[1][0] = sigma;   // y = g.x
	a.m[2][1] = sigma;   // z = g.y
	return a;
}

// glTF -> the hand frame by the manifest's grip (img3d_core.h HandPoint), read off as an affine map
Aff GltfToHand(const img3d::Entry &e, bool right)
{
	float o[3], x[3], y[3], z[3];
	const float g0[3] = { 0, 0, 0 }, gx[3] = { 1, 0, 0 }, gy[3] = { 0, 1, 0 }, gz[3] = { 0, 0, 1 };
	img3d::HandPoint(e, right, g0, o);
	img3d::HandPoint(e, right, gx, x);
	img3d::HandPoint(e, right, gy, y);
	img3d::HandPoint(e, right, gz, z);
	Aff a;
	for (int r = 0; r < 3; r++) { a.m[r][0] = x[r] - o[r]; a.m[r][1] = y[r] - o[r]; a.m[r][2] = z[r] - o[r]; a.m[r][3] = o[r]; }
	return a;
}

double Sigma(const ItemModel &m, double metres) { const double L = m.mesh.Longest(); return L > 0 ? metres / L : 1.0; }
} // namespace

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
	ApplyStyle(mo, m->mesh.parts[m->mesh.Primary()]);
	const double meshLong = m->mesh.Longest();
	const double metres = m->entry.sizeFromTable ? meshLong : (fallbackMetres > 0 ? fallbackMetres : meshLong);
	if (handLocked)
	{
		// The grip frame is metres: the mesh's own size, or the card's size when the manifest's is not the real one.
		const double s = metres / meshLong;
		mo->FollowHandMode = (role == VR_MAINHAND) ? FOLLOWHAND_GRIP_MAIN : FOLLOWHAND_GRIP_OFF;
		mo->Scale = DVector2(s, s);
		mo->SetOrigin(at, true);
		// GRABBED WHERE IT WAS TOUCHED (roth_physics.h): its own place in the hand, not the manifest's grip. The mesh
		// is the manifest grip's (KIND_HAND); the seat turns it from there to `rel` (the body frame in the hand frame),
		// before the actor's scale s: seat = scale(1/s) o rel o body(g) o (s o grip)^-1 ... applied to grip(g).
		if (S.heldRelSet[role])
		{
			Aff gripInv, bodyAff = GltfToBody(Sigma(*m, metres));
			const Aff grip = GltfToHand(m->entry, right);
			if (grip.Inverse(gripInv))
			{
				const Aff seatA = Aff::Scale(1.0 / s) * Aff::From12(S.heldRel[role]) * bodyAff * gripInv;
				float seat[16] = {};
				for (int r = 0; r < 3; r++) { for (int c = 0; c < 3; c++) seat[c * 4 + r] = (float)seatA.m[r][c]; seat[12 + r] = (float)seatA.m[r][3]; }
				seat[15] = 1.f;
				VRHandPose_SetActorSeat(mo, seat);
			}
		}
		else VRHandPose_SetActorSeat(mo, nullptr);
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

// ---------------------------------------------------------------------------------------------------- THE LOOSE ITEMS
// roth_physics.h: an item's model lying free and turned any way. Its BODY FRAME is the model's own as it is drawn at
// yaw 0 in the world (KIND_WORLD), in the map's axes and metres: b(g) = (g.z, g.x, g.y) * sigma, sigma the metres per
// mesh metre (img3d_core.h WorldPoint maps glTF to the model's render axes (z, y, x), which are the map's with y and z
// swapped). The actor's origin is the body's origin.


bool Img3dItemHull(int item, double metres, std::vector<DVector3> &out)
{
	out.clear();
	ItemModel *m = FindModelFor(item);
	if (m == nullptr || metres <= 0) return false;
	const Aff b = GltfToBody(Sigma(*m, metres));
	const size_t nv = m->mesh.Vertices();
	const size_t stride = nv > 400 ? nv / 400 : 1;
	for (size_t v = 0; v < nv; v += stride)
	{
		const double g[3] = { m->mesh.pos[v * 3], m->mesh.pos[v * 3 + 1], m->mesh.pos[v * 3 + 2] };
		double o[3];
		b.Apply(g, o);
		out.push_back(DVector3(o[0], o[1], o[2]));
	}
	// the extremes, so the stride does not shave a corner off
	const double lo[3] = { m->mesh.lo[0], m->mesh.lo[1], m->mesh.lo[2] }, hi[3] = { m->mesh.hi[0], m->mesh.hi[1], m->mesh.hi[2] };
	for (size_t v = 0; v < nv; v++)
	{
		const float *p = &m->mesh.pos[v * 3];
		bool ext = false;
		for (int k = 0; k < 3; k++) if (p[k] == (float)lo[k] || p[k] == (float)hi[k]) ext = true;
		if (!ext) continue;
		const double g[3] = { p[0], p[1], p[2] };
		double o[3];
		b.Apply(g, o);
		out.push_back(DVector3(o[0], o[1], o[2]));
	}
	return !out.empty();
}

bool Img3dGripRel(int item, bool rightHand, double metres, double rel[12])
{
	ItemModel *m = FindModelFor(item);
	if (m == nullptr || metres <= 0) return false;
	const double sigma = Sigma(*m, metres);
	// hand(u) = sigma-scaled manifest grip of g; body(g) = GltfToBody; rel = hand o body^-1 (rigid: sigma cancels)
	Aff bodyInv;
	if (!GltfToBody(sigma).Inverse(bodyInv)) return false;
	const Aff r = Aff::Scale(sigma) * GltfToHand(m->entry, rightHand) * bodyInv;
	for (int a = 0; a < 3; a++) { for (int c = 0; c < 3; c++) rel[a * 3 + c] = r.m[a][c]; rel[9 + a] = r.m[a][3]; }
	return true;
}

void Img3dHeldRel(int role, const double *rel12)
{
	if (role < 0 || role > 1) return;
	S.heldRelSet[role] = rel12 != nullptr;
	if (rel12 != nullptr) memcpy(S.heldRel[role], rel12, sizeof(S.heldRel[role]));
}

bool Img3dLooseShow(FLevelLocals *level, int slot, int item, const DVector3 &origin, double yaw, double pitch, double roll, double metres)
{
	if (level == nullptr || slot < 0 || slot >= kImg3dLoose || metres <= 0) return false;
	ItemModel *m = FindModelFor(item);
	PClassActor *cls = HeldClass();
	if (m == nullptr || cls == nullptr) return false;
	const int sn = SpriteFor(item, *m, KIND_LOOSE, cls);
	if (sn < 0) return false;
	AActor *&mo = S.loose[slot];
	if (mo != nullptr && ((mo->ObjectFlags & OF_EuthanizeMe) || mo->Level != level)) mo = nullptr;
	const bool fresh = mo == nullptr || (mo->renderflags & RF_INVISIBLE) || mo->sprite != sn;
	if (mo == nullptr) mo = AActor::StaticSpawn(level, cls, origin, NO_REPLACE);
	if (mo == nullptr) return false;
	mo->special1 = kFlightTag;   // not stray (the sweep), not a companion
	mo->flags5 |= MF5_NOTIMEFREEZE;
	mo->FollowHandMode = 0;
	mo->sprite = sn;
	mo->frame = 0;
	mo->renderflags &= ~RF_INVISIBLE;
	ApplyStyle(mo, m->mesh.parts[m->mesh.Primary()]);
	const double k = UnitsPerMetre() * Sigma(*m, metres);
	mo->Scale = DVector2(k, k / Stretch(level));
	mo->Angles.Yaw = DAngle::fromDeg(yaw);
	mo->Angles.Pitch = DAngle::fromDeg(pitch);
	mo->Angles.Roll = DAngle::fromDeg(roll);
	mo->renderradius = std::max(16.0, k * m->mesh.Longest());
	mo->SetOrigin(origin, true);
	if (fresh) { mo->PrevScale = mo->Scale; mo->ClearInterpolation(); }
	S.looseShown[slot] = true;
	return true;
}

AActor *Img3dLooseActor(int slot)
{
	if (slot < 0 || slot >= kImg3dLoose) return nullptr;
	AActor *mo = S.loose[slot];
	return mo != nullptr && !(mo->ObjectFlags & OF_EuthanizeMe) && !(mo->renderflags & RF_INVISIBLE) ? mo : nullptr;
}

// THE PARTS (roth_img3d_core.h): a model of several parts -- a vial's glass, its glowing liquid, its cork -- is drawn by
// its wearer (the primary part) and one COMPANION a further part: a RealmsItemModel whose master is the wearer, taking
// the part's own look (see-through, fullbright), following the wearer's place, turn, scale and hand every tic, and
// hidden with it. GZDoom draws a look per actor, not per surface, so each look needs its own actor; the translucent
// ones are then drawn in the translucent pass after everything opaque, as any see-through sprite. Kept by the level
// (saved and loaded with it); one whose wearer no longer wears a model of parts goes.
static void PartsTick(FLevelLocals *level)
{
	PClassActor *cls = HeldClass();
	if (level == nullptr || cls == nullptr) return;
	std::set<std::pair<AActor *, int>> have;
	TArray<AActor *> gone;
	auto follow = [&](AActor *co, AActor *ma, const State::Multi &mu, int part) -> bool
	{
		ItemModel *m = FindModelFor(mu.item);
		if (m == nullptr || part < 0 || (size_t)part >= m->mesh.parts.size() || part == m->mesh.Primary()) return false;
		const int sn = SpriteFor(mu.item, *m, mu.kind, cls, part);
		if (sn < 0) return false;
		co->sprite = sn;
		co->frame = 0;
		ApplyStyle(co, m->mesh.parts[part]);
		co->flags5 |= MF5_NOTIMEFREEZE;
		co->FollowHandMode = ma->FollowHandMode;
		co->Scale = ma->Scale;
		co->PrevScale = ma->PrevScale;
		co->Angles = ma->Angles;
		co->PrevAngles = ma->PrevAngles;
		co->renderradius = ma->renderradius;
		if (co->Pos() != ma->Pos()) co->SetOrigin(ma->Pos(), true);
		co->Prev = ma->Prev;
		if (ma->renderflags & RF_INVISIBLE) co->renderflags |= RF_INVISIBLE; else co->renderflags &= ~RF_INVISIBLE;
		return true;
	};
	{
		auto it = level->GetThinkerIterator<AActor>(cls->TypeName);
		AActor *co;
		while ((co = it.Next()) != nullptr)
		{
			if (co->special1 != kCompanionTag || (co->ObjectFlags & OF_EuthanizeMe)) continue;
			AActor *ma = co->master;
			auto mu = ma != nullptr && !(ma->ObjectFlags & OF_EuthanizeMe) ? S.multi.find(ma->sprite) : S.multi.end();
			if (mu == S.multi.end() || have.count({ ma, co->special2 }) || !follow(co, ma, mu->second, co->special2)) { gone.Push(co); continue; }
			have.insert({ ma, co->special2 });
		}
	}
	for (AActor *x : gone) x->Destroy();
	if (S.multi.empty()) return;
	auto all = level->GetThinkerIterator<AActor>();
	TArray<AActor *> wearers;
	AActor *ma;
	while ((ma = all.Next()) != nullptr)
		if (ma->special1 != kCompanionTag && S.multi.count(ma->sprite) && !(ma->ObjectFlags & OF_EuthanizeMe)) wearers.Push(ma);
	for (AActor *w : wearers)
	{
		const State::Multi mu = S.multi[w->sprite];
		ItemModel *m = FindModelFor(mu.item);
		if (m == nullptr) continue;
		for (int part = 0; part < (int)m->mesh.parts.size(); part++)
		{
			if (part == m->mesh.Primary() || have.count({ w, part })) continue;
			AActor *co = AActor::StaticSpawn(level, cls, w->Pos(), NO_REPLACE);
			if (co == nullptr) continue;
			co->special1 = kCompanionTag;
			co->special2 = part;
			co->master = w;
			if (!follow(co, w, mu, part)) { co->Destroy(); continue; }
			co->ClearInterpolation();
			Printf("[roth_img3d] item %d: part %d '%s' drawn by a companion of %s%s%s\n", mu.item, part, m->mesh.parts[part].name.c_str(),
				w->GetClass()->TypeName.GetChars(), m->mesh.parts[part].blend ? FStringf(", see-through %.2f", m->mesh.parts[part].color[3]).GetChars() : "",
				m->mesh.parts[part].glow ? ", glowing" : "");
		}
	}
}

double Img3dWornMetres(const AActor *mo)
{
	auto it = S.worn.find(const_cast<AActor *>(mo));
	if (mo == nullptr || it == S.worn.end() || it->second.key != mo->special2 || mo->sprite != it->second.modelSprite) return 0.0;
	ItemModel *m = FindModelFor(it->second.item);
	return m != nullptr ? m->mesh.Longest() * mo->Scale.X / UnitsPerMetre() : 0.0;
}

double Img3dHandMetres(int item, double cardMetres)
{
	ItemModel *m = FindModelFor(item);
	if (m == nullptr) return cardMetres;
	return m->entry.sizeFromTable ? m->mesh.Longest() : (cardMetres > 0 ? cardMetres : m->mesh.Longest());
}

bool Img3dFlyShow(FLevelLocals *level, int flight, int item, const DVector3 &centre, double yawDeg, double metres)
{
	if (level == nullptr || flight < 0 || flight >= kImg3dFlights || metres <= 0) return false;
	ItemModel *m = FindModelFor(item);
	PClassActor *cls = HeldClass();
	if (m == nullptr || cls == nullptr) return false;
	const int sn = SpriteFor(item, *m, KIND_WORLD, cls);
	if (sn < 0) return false;
	AActor *&mo = S.flight[flight];
	if (mo != nullptr && ((mo->ObjectFlags & OF_EuthanizeMe) || mo->Level != level)) mo = nullptr;
	const bool fresh = mo == nullptr || (mo->renderflags & RF_INVISIBLE) || mo->sprite != sn;
	if (mo == nullptr) mo = AActor::StaticSpawn(level, cls, centre, NO_REPLACE);
	if (mo == nullptr) return false;
	mo->special1 = kFlightTag;
	mo->flags5 |= MF5_NOTIMEFREEZE;   // the inventory pauses the world; a pickup still lands
	mo->FollowHandMode = 0;
	mo->sprite = sn;
	mo->frame = 0;
	mo->renderflags &= ~RF_INVISIBLE;
	ApplyStyle(mo, m->mesh.parts[m->mesh.Primary()]);
	const double upm = UnitsPerMetre(), ps = Stretch(level);
	const double k = upm * metres / m->mesh.Longest();
	mo->Scale = DVector2(k, k / ps);
	mo->Angles.Yaw = DAngle::fromDeg(yawDeg);
	const double z = centre.Z - 0.5 * (m->mesh.lo[1] + m->mesh.hi[1]) * k / ps;
	mo->SetOrigin(DVector3(centre.X, centre.Y, z), true);
	if (fresh) { mo->PrevScale = mo->Scale; mo->ClearInterpolation(); }
	S.flown[flight] = true;
	if (S.loggedFlight[flight] != item)
	{
		Printf("[roth_img3d] flight %d: item %d's model flies (%.3f m now)\n", flight, item, metres);
		S.loggedFlight[flight] = item;
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
				bool ours = mo->special1 == kCompanionTag;   // a companion (PartsTick) keeps itself or goes on its own
				for (AActor *h : S.held) if (h == mo) ours = true;
				for (AActor *f : S.flight) if (f == mo) ours = true;
				for (AActor *f : S.loose) if (f == mo) ours = true;
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
	for (int f = 0; f < kImg3dFlights; f++)
	{
		AActor *&mo = S.flight[f];
		if (mo != nullptr && ((mo->ObjectFlags & OF_EuthanizeMe) || mo->Level != level)) mo = nullptr;
		if (!S.flown[f] && mo != nullptr && !(mo->renderflags & RF_INVISIBLE))
		{
			mo->renderflags |= RF_INVISIBLE;
			if (S.loggedFlight[f] != 0) { Printf("[roth_img3d] flight %d: landed (hidden)\n", f); S.loggedFlight[f] = 0; }
		}
		S.flown[f] = false;
	}
	for (int f = 0; f < kImg3dLoose; f++)
	{
		AActor *&mo = S.loose[f];
		if (mo != nullptr && ((mo->ObjectFlags & OF_EuthanizeMe) || mo->Level != level)) mo = nullptr;
		if (!S.looseShown[f] && mo != nullptr) mo->renderflags |= RF_INVISIBLE;
		S.looseShown[f] = false;
	}
	PartsTick(level);
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
	for (int f = 0; f < kImg3dFlights; f++) { S.flight[f] = nullptr; S.flown[f] = false; S.loggedFlight[f] = 0; }
	for (int f = 0; f < kImg3dLoose; f++) { S.loose[f] = nullptr; S.looseShown[f] = false; }
	S.heldRelSet[0] = S.heldRelSet[1] = false;
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
		Printf("  item %d %s: %s, %d tris in %d part(s), %.3f m (%s), grip (%.3f %.3f %.3f) turn (%.0f %.0f %.0f), hold %s\n", e.item, e.name.c_str(), e.file.c_str(),
			(int)p.second.mesh.Triangles(), (int)p.second.mesh.parts.size(), p.second.mesh.Longest(), e.sizeFromTable ? "real" : "to the picture", e.grip[0], e.grip[1], e.grip[2],
			e.gripRot[0], e.gripRot[1], e.gripRot[2], img3d::HoldName(e.hold));
	}
}
