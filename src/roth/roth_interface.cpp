//
// The Realms panels: dialogue window, choice menu, inventory screen, examine
// popup. See roth_interface.h. Rules live in roth_dialogue / roth_invscreen /
// roth_inventory; this file places billboards, turns a pointer into a row or
// a cell, and routes keys -- nothing it does changes what the rules decide.
//
// NOT COMPILED OR RUN in the lane that wrote it: type-checked with g++
// against the engine headers (tools/roth_syntaxcheck.sh). The rules it calls
// are differential-tested against the original (tools/dialogue_test).
//

#include "roth_interface.h"
#include "roth_actors_engine.h"
#include "roth_gamecore.h"
#include "roth_dialogue.h"
#include "roth_invscreen.h"
#include "roth_invdraw.h"
#include "roth_menudraw.h"
#include "roth_dlgdraw.h"
#include "roth_uidraw.h"
#include "roth_icons.h"
#include "roth_install.h"
#include "roth_texture.h"
#include "roth_das.h"
#include "roth_objects.h"
#include "roth_sound.h"
#include "roth_raw.h"
#include "roth_menus.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "g_levellocals.h"
#include "actor.h"
#include "d_player.h"
#include "c_dispatch.h"
#include "c_cvars.h"
#include "printf.h"
#include "textures.h"
#include "texturemanager.h"
#include "image.h"
#include "palettecontainer.h"
#include "r_data/r_translate.h"
#include "v_video.h"
#include "r_data/sprites.h"


EXTERN_CVAR(Float, bb_scale)
EXTERN_CVAR(Float, bb_tiltbias)
EXTERN_CVAR(Bool, bb_flipu)

namespace roth
{

// The level's copy of an object record (roth_runtime.cpp).
bool RuntimeObjectRecord(int rothSector, int rothIndex, Object &out);

namespace
{

//==========================================================================
//
// The dialogue with its outside service: the speech player (roth_sound).
// The dialogue owns the stream state and the timing, as voice_stream_pump
// does (audio.c:1300-1327); the player only plays, stops and reports.
//
//==========================================================================

class RuntimeDialogue : public DialogueUi
{
public:
	bool SpeechHeader(uint32_t clip, uint8_t hdr[0x2c]) override { return SoundSpeechHeader(clip, hdr); }
	void VoiceStarted(uint32_t clip, uint32_t samples, uint32_t rate) override
	{
		(void)samples; (void)rate;
		SoundPrimeVoiceClip(clip);                  // install_voice_sos_callback, audio.c:1288
	}
	void VoiceStopped() override { SoundVoiceStop(); }      // sos_stop_voice, audio.c:1338
	// The driver's end-of-stream callback (audio.c:1178), polled.
	void PollVoice()
	{
		speechAvailable = SoundSpeechAvailable();
		if (St().g_voice_stream_state == 1 && !SoundVoicePlaying()) VoiceEnded();
	}
};

//==========================================================================
//
// The inventory's outside services.
//
//==========================================================================

class RuntimeInvSink : public InventorySink
{
public:
	std::map<std::string, int> notBuilt;

	void UnequipWeapon() override;
	void EquipWeapon(int slot, int32_t itemId) override;
	void RebuildWeaponList() override { ActorsRebuildWeaponList(); }
	void ResetWeaponHud() override { notBuilt["weapon HUD"]++; }
	void RightHandChanged(int slot) override;
	void RefreshPanel() override;
	void ClearTabCursors() override;
	bool PanelCloseRequested() override;
	void PickupFlyStarted(int32_t x, int32_t y, int32_t z, uint16_t renderId) override;
	void ItemMessage(int interfaceIndex, int colorIndex, uint32_t nameA, uint32_t nameB) override;
};

//==========================================================================
//
// State.
//
//==========================================================================

enum ElementKind
{
	E_NONE,
	E_DLGPANEL,                                         // the dialogue's choice panel picture; a click is mapped to its rows
	E_INVPANEL,                                         // the inventory panel's picture; a click is mapped to its hotspots
	E_POP_MAG, E_POP_INFO, E_POP_TOPIC, E_POP_CLOSE, E_POP_PGUP, E_POP_PGDN, E_POP_BACKING,
	E_MENUBOX,                                          // a message box's picture; a click is mapped by hit_test_ui_element
	E_MENU_BACKING, E_MENU_ROW, E_MENU_BACK,            // the death screen's (an invented layout: the original has no box there)
	E_PICTURE,                                          // a picture shown, not clicked (the subtitle, the timed message, a box under another)
};

struct Element
{
	int bb = 0;             // billboard handle
	int kind = E_NONE;
	int arg = 0;
	bool hittable = false;
	// A picture's rectangle on the original's screen: a point on the billboard
	// maps to (left + u * picW, top + v * picH), the original's own mouse.
	int left = 0, top = 0, picW = 0, picH = 0;
	int picture = -1;       // which of the pictures (PIC_*) it shows
};

// The pictures drawn as the original draws them, each through its own pair of
// textures (see UploadPicture).
enum PictureId { PIC_INVENTORY, PIC_CHOICES, PIC_LINE, PIC_TMSG, PIC_BOX0, PIC_BOX1, PIC_COUNT };

struct Fly
{
	bool active = false;
	DVector3 from;
	int ticks = 0;          // 70 Hz ticks since start
	FTextureID tex;
	int bb = 0;
};

struct Ui
{
	GameCore *core = nullptr;
	Dbase100 *db = nullptr;
	RuntimeDialogue dialogue;
	InventoryScreen screen;
	RuntimeInvSink invSink;
	// The inventory panel's picture, drawn as the original draws it (roth_invdraw).
	InventoryPanelPainter painter;
	// The dialogue text (roth_dlgdraw) and the message boxes (roth_menudraw),
	// drawn as the original draws them.
	DialogueTextPainter choicePainter, linePainter, tmsgPainter;
	MessageBoxPainter boxes[2];                     // [0] the box on top, [1] the one under it when it stays on screen
	RealmsMenus::Box boxOf[2] = { RealmsMenus::NONE, RealmsMenus::NONE };
	uint32_t menuSerial = 0xffffffffu;              // RealmsMenus::ShowSerial() the box pictures were composed for
	std::vector<uint8_t> iconsAll;                  // DATA\ICONS.ALL
	RothFont font;                                  // out of the player's ROTH.EXE
	std::vector<uint8_t> shade, blend;              // the map pack's tables, copied (the pack may close first)
	// Each picture on screen: two textures in turn, as the movie player's
	// AnimTextures does (common/textures/animtexture.cpp), so the one the GPU
	// may still be drawing is never the one being rewritten.
	struct Picture
	{
		FGameTexture *tex[2] = { nullptr, nullptr };
		int active = 0;
		uint32_t shown = 0xffffffffu;               // the canvas' Version() last uploaded
		const UiCanvas *canvas = nullptr;
	};
	Picture pics[PIC_COUNT];
	uint8_t panelPal[0x300] = {};
	int lastHoverAction = 0;                        // [0x81300]: handle_cursor_click's last hovered action
	std::vector<uint8_t> dbase200;
	std::string dbase300Path;
	bool attached = false;

	std::vector<PalEntry> palette;                  // the map's pack palette, 8-bit
	std::map<uint32_t, FTextureID> iconTex;         // DBASE200 resource -> texture
	std::map<uint32_t, FTextureID> pageTex;         // DBASE300 resource -> texture
	int iconTranslation = -1;

	FLevelLocals *level = nullptr;
	std::vector<Element> elements;
	std::string signature;
	// Where the world panels stand: set when a panel opens, kept while it is up.
	bool anchored = false;
	DVector3 anchor;
	double anchorYaw = 0.0;

	int hoverBb = 0;                                // the element the pointer is on
	Fly fly;
	DVector3 pendingFlyFrom;
	bool havePendingFly = false;

	std::vector<std::vector<std::string>> queue;    // test commands, drained per tic
	RealmsMenus *menus = nullptr;                   // roth_menus, owned by the runtime
};

Ui U;

void RuntimeInvSink::EquipWeapon(int slot, int32_t itemId)
{
	// activate_weapon_item (weapon_combat.c:426): the actor rules parse the
	// weapon and say which ammo item it takes; the count comes from the
	// inventory, through Query, whose side effect (it clears the
	// auto-selected flag) the original has here too (weapon_combat.c:450).
	const int ammoItem = ActorsEquipWeapon(slot, (int)itemId);
	if (ammoItem != 0 && U.core != nullptr && U.db != nullptr)
		ActorsSetWeaponAmmo(U.core->Items().Query(*U.db, (uint16_t)ammoItem, 0, this));
}

void RuntimeInvSink::UnequipWeapon()
{
	// activate_weapon_item(0, 0): Adam's own item (0x81054, latched by
	// restore_active_held_item) is what the original parses.
	ActorsUnequipWeapon(U.core != nullptr ? (int)U.core->Items().AdamItemId() : 0);
}

void RuntimeInvSink::RightHandChanged(int slot)
{
	if (U.core == nullptr || U.db == nullptr) return;
	// The original refreshes the hand icon every frame (update_selected_item_icon,
	// inventory.c:1676); the log line is only for a change.
	static int32_t lastLogged = INT32_MIN;
	const int32_t id = slot < 0 ? -1 : (int16_t)U.core->Items().Slot(slot).id;
	if (id == lastLogged) return;
	lastLogged = id;
	if (slot < 0) { Printf("[roth_ui] right hand: empty\n"); return; }
	const DbItem *rec = U.db->Item(id);
	Printf("[roth_ui] right hand: item %d (%s)\n", (int)id,
		rec ? U.dialogue.LineText(rec->name, 0x61).c_str() : "?");
}

void RuntimeInvSink::RefreshPanel() { U.screen.RefreshPanel(); }
void RuntimeInvSink::ClearTabCursors() { U.screen.ClearTabCursors(); }
bool RuntimeInvSink::PanelCloseRequested() { return U.screen.PanelCloseRequested(); }

void RuntimeInvSink::ItemMessage(int interfaceIndex, int colorIndex, uint32_t nameA, uint32_t nameB)
{
	U.dialogue.ItemMessage(interfaceIndex, colorIndex, nameA, nameB);
	for (const TextLine &l : U.dialogue.TimedMessageLines())
		for (const std::string &r : l.Rows()) Printf("[roth_ui] message: %s\n", r.c_str());
}

//==========================================================================
//
// Textures from palette images. The pixels must outlive the image
// (FImageSource is freed in bulk without its destructor), as in
// roth_texture.cpp.
//
//==========================================================================

std::vector<std::unique_ptr<std::vector<uint8_t>>> g_pixelStore;

int IconTranslation()
{
	if (U.iconTranslation >= 0) return U.iconTranslation;
	if (U.palette.size() < 256) return -1;
	FRemapTable t;
	for (int c = 0; c < 256; c++)
	{
		t.Palette[c] = PalEntry(255, U.palette[c].r, U.palette[c].g, U.palette[c].b);
		t.Remap[c] = ColorMatcher.Pick(U.palette[c].r, U.palette[c].g, U.palette[c].b);
	}
	t.Palette[0] = 0;       // index 0 is the hole (inventory.c:2113)
	t.Remap[0] = 0;
	U.iconTranslation = GetTranslationIndex(GPalette.StoreTranslation(TRANSLATION_Standard, &t));
	return U.iconTranslation;
}

FTextureID MakeImageTexture(const DasImage &im, const char *name)
{
	FTextureID existing = TexMan.CheckForTexture(name, ETextureType::Override);
	if (existing.isValid()) return existing;
	if (!im.ok) return FNullTextureID();
	const int tr = IconTranslation();
	if (tr < 0) return FNullTextureID();
	FRemapTable *remap = const_cast<FRemapTable *>(GPalette.GetTranslation(TRANSLATION_Standard, tr));
	g_pixelStore.push_back(std::make_unique<std::vector<uint8_t>>(im.pixels));
	auto *image = new FPalettedMemoryImage(g_pixelStore.back()->data(), remap, im.width, im.height, false, true);
	auto *tex = MakeGameTexture(new FImageTexture(image), name, ETextureType::Override);
	tex->SetNoMipmap(true);
	return TexMan.AddGameTexture(tex);
}

FTextureID IconTexture(int32_t itemId)
{
	if (U.db == nullptr) return FNullTextureID();
	const DbItem *rec = U.db->Item(itemId);
	if (rec == nullptr || !rec->ok || rec->icon == 0) return FNullTextureID();
	auto it = U.iconTex.find(rec->icon);
	if (it != U.iconTex.end()) return it->second;
	FString name;
	name.Format("RTHICN%05X", (unsigned)rec->icon);
	const FTextureID t = MakeImageTexture(DecodeDbase200Resource(U.dbase200, rec->icon), name.GetChars());
	U.iconTex[rec->icon] = t;
	return t;
}

// A document page: a DBASE300 resource, read from the file on demand (the
// file is 22 MB; one page is a few tens of KB). load_dbase300 page path,
// load_inspect_document_page (dialogue_ui.c:1815-1830).
FTextureID PageTexture(uint32_t resource)
{
	auto it = U.pageTex.find(resource);
	if (it != U.pageTex.end()) return it->second;
	FTextureID t = FNullTextureID();
	FILE *f = U.dbase300Path.empty() ? nullptr : fopen(U.dbase300Path.c_str(), "rb");
	if (f != nullptr)
	{
		uint32_t size = 0;
		if (fseek(f, (long)resource << 3, SEEK_SET) == 0 && fread(&size, 4, 1, f) == 1 && size > 8 && size < (8u << 20))
		{
			std::vector<uint8_t> bytes(size);
			if (fread(bytes.data(), 1, size, f) == size)
			{
				FString name;
				name.Format("RTHDOC%06X", (unsigned)resource);
				t = MakeImageTexture(DecodeDasImage(bytes.data(), bytes.size()), name.GetChars());
			}
		}
		fclose(f);
	}
	U.pageTex[resource] = t;
	return t;
}

PalEntry Color(uint8_t index)
{
	if (U.palette.size() < 256) return PalEntry(255, 220, 220, 200);
	return PalEntry(255, U.palette[index].r, U.palette[index].g, U.palette[index].b);
}

// The width of a string in the original's font, in its pixels
// (measure_control_text_width, renderer.c:438-452).
int TextPixels(const std::string &s)
{
	int w = 0;
	for (unsigned char c : s) if (c >= 0x20) w += FontCharAdvance(c);
	return w;
}

//==========================================================================
//
// Billboards. All of this module's billboards are persistent, world placed,
// fixed-facing quads in one plane in front of the player, or view-locked
// subtitles. Everything is rebuilt when what is shown changes.
//
//==========================================================================

static const double PX = 0.1;           // map units per original screen pixel
static const double DIST = 44.0;        // how far in front of the eye a panel stands
static const double ROWH = 11 * PX;     // one text line, the original's 11 pixels

void ClearBillboards()
{
	if (U.level != nullptr)
	{
		for (const Element &e : U.elements)
		{
			for (unsigned i = 0; i < U.level->Billboards.Size(); i++)
				if (U.level->Billboards[i].id == e.bb) { U.level->Billboards.Delete(i); break; }
		}
	}
	U.elements.clear();
	U.hoverBb = 0;
}

// Add one billboard at panel coordinates (dx right, dz up, from the anchor;
// `lift` moves it toward the player so it sits in front of the backing).
int Add(double dx, double dz, double lift, double w, double h, int payload, int data, PalEntry col,
	const char *text, double alpha, int kind, int arg, bool hittable)
{
	if (U.level == nullptr) return 0;
	const double yr = U.anchorYaw * (M_PI / 180.0);
	const DVector3 right(-sin(yr), cos(yr), 0.0);           // BillboardBasis: the viewer's right
	const DVector3 toward(cos(yr), sin(yr), 0.0);           // the face points back at the player
	FBillboard bb;
	bb.id = U.level->NextBillboardID++;
	bb.pos = U.anchor + right * dx + DVector3(0, 0, dz) + toward * lift;
	bb.width = w;
	bb.height = h;
	bb.yaw = U.anchorYaw;
	bb.facing = BBF_FIXED;
	bb.payload = payload;
	bb.data = data;
	bb.color = col;
	bb.alpha = alpha;
	bb.flags = BBFL_PERSISTENT | BBFL_NOSAVE | (hittable ? 0 : BBFL_NOHIT);   // NOSAVE: rebuilt from live state
	if (text != nullptr) bb.text = text;
	bb.spawntic = U.level->maptime;
	bb.drawPos = bb.pos;
	U.level->Billboards.Push(bb);
	Element e;
	e.bb = bb.id;
	e.kind = kind;
	e.arg = arg;
	e.hittable = hittable;
	U.elements.push_back(e);
	return bb.id;
}

// A view-locked billboard (subtitle, timed message): X ahead, Y right, Z up.
int AddViewLocked(double ahead, double rightOff, double up, double w, double h, int payload, PalEntry col,
	const char *text, double alpha, int data = 0)
{
	if (U.level == nullptr) return 0;
	FBillboard bb;
	bb.id = U.level->NextBillboardID++;
	bb.pos = DVector3(ahead, rightOff, up);
	bb.width = w;
	bb.height = h;
	bb.facing = BBF_FIXED;
	bb.payload = payload;
	bb.data = data;
	bb.color = col;
	bb.alpha = alpha;
	bb.flags = BBFL_PERSISTENT | BBFL_NOSAVE | BBFL_VIEWLOCKED | BBFL_NODEPTH | BBFL_NOHIT;
	if (text != nullptr) bb.text = text;
	bb.spawntic = U.level->maptime;
	U.level->Billboards.Push(bb);
	Element e;
	e.bb = bb.id;
	U.elements.push_back(e);
	return bb.id;
}

void Anchor(player_t *player)
{
	if (player == nullptr || player->mo == nullptr) return;
	AActor *mo = player->mo;
	const DAngle yaw = mo->Angles.Yaw;
	const DVector3 eye = mo->Pos() + DVector3(0, 0, player->viewheight);
	U.anchor = eye + DVector3(yaw.Cos(), yaw.Sin(), 0) * DIST - DVector3(0, 0, 4);
	U.anchorYaw = yaw.Degrees() + 180.0;
	U.anchored = true;
}

//==========================================================================
//
// The pointer: the main hand's beam in VR, the view flat.
//
//==========================================================================

struct Ray { DVector3 start, dir; };

bool PointerRays(player_t *player, Ray out[2], int &n)
{
	n = 0;
	if (player == nullptr || player->mo == nullptr) return false;
	AActor *mo = player->mo;
	if (mo->OverrideAttackPosDir)
	{
		// The runtime's hand convention (UseByHand): the stored angle carries +90.
		const DAngle y0 = mo->AttackAngle + DAngle::fromDeg(90.);
		out[n].start = mo->AttackPos;
		out[n].dir = DVector3(y0.Cos() * mo->AttackPitch.Cos(), y0.Sin() * mo->AttackPitch.Cos(), -mo->AttackPitch.Sin());
		n++;
		const DAngle y1 = mo->OffhandAngle + DAngle::fromDeg(90.);
		out[n].start = mo->OffhandPos;
		out[n].dir = DVector3(y1.Cos() * mo->OffhandPitch.Cos(), y1.Sin() * mo->OffhandPitch.Cos(), -mo->OffhandPitch.Sin());
		n++;
		return true;
	}
	const DAngle y = mo->Angles.Yaw, p = mo->Angles.Pitch;
	out[n].start = mo->Pos() + DVector3(0, 0, player->viewheight);
	out[n].dir = DVector3(y.Cos() * p.Cos(), y.Sin() * p.Cos(), -p.Sin());
	n++;
	return true;
}

// The nearest hittable element on a ray, with the same basis the renderer
// draws with (BillboardBasis, g_levellocals.h).
const Element *HitElement(const Ray &r)
{
	if (U.level == nullptr) return nullptr;
	const Element *best = nullptr;
	double bestT = 1e9;
	for (const Element &e : U.elements)
	{
		if (!e.hittable) continue;
		const FBillboard *bb = nullptr;
		for (const FBillboard &b : U.level->Billboards) if (b.id == e.bb) { bb = &b; break; }
		if (bb == nullptr) continue;
		DVector3 right, up, normal;
		double hw, hh;
		BillboardBasis(*bb, bb->pos, r.start, bb_tiltbias, bb_scale, right, up, normal, hw, hh);
		const double denom = normal | r.dir;
		if (fabs(denom) < 1e-9) continue;
		const double t = (normal | (bb->pos - r.start)) / denom;
		if (t <= 0.0 || t >= bestT) continue;
		const DVector3 rel = (r.start + r.dir * t) - bb->pos;
		if (fabs(rel | right) > hw || fabs(rel | up) > hh) continue;
		// A backing loses to anything drawn on it at the same depth.
		if (best != nullptr && fabs(t - bestT) < 0.5 && (e.kind == E_POP_BACKING || e.kind == E_MENU_BACKING)) continue;
		bestT = t;
		best = &e;
	}
	return best;
}

//==========================================================================
//
// What is on screen, as a signature: when it changes, the billboards are
// rebuilt.
//
//==========================================================================

std::string Signature()
{
	std::string s;
	const DialogueView v = U.dialogue.View();
	char buf[96];
	snprintf(buf, sizeof buf, "D%d/%d/%d/%d/%d|", (int)v.kind, v.page, v.highlighted, (int)v.color, (int)v.rows.size());
	s += buf;
	for (const std::string &r : v.rows) { s += r; s += '\x1f'; }
	snprintf(buf, sizeof buf, "T%d/%d|", U.dialogue.TimedMessageLineCount(), U.dialogue.TimedMessageLineCount() ? (int)U.dialogue.TimedMessageLines().size() : 0);
	s += buf;
	if (U.dialogue.TimedMessageLineCount() != 0)
		for (const TextLine &l : U.dialogue.TimedMessageLines()) for (const std::string &r : l.Rows()) s += r;
	if (U.screen.IsOpen() && U.core != nullptr)
	{
		const Inventory &inv = U.core->Items();
		snprintf(buf, sizeof buf, "I%d/%d/%d/%d/%d/%d/%d/%d|", U.screen.ActiveList(), U.screen.CursorEntry(), U.screen.ScrollOffset(),
			U.screen.HeldEntry(), inv.LeftHand(), inv.RightHand(), inv.Count(), U.screen.ActiveTab());
		s += buf;
		for (int sl : U.screen.Entries().slots) { snprintf(buf, sizeof buf, "%d:%d.%d,", sl, inv.Slot(sl).id, inv.Slot(sl).quantity); s += buf; }
	}
	if (U.menus != nullptr && U.menus->IsOpen())
	{
		if (U.menus->Current() == RealmsMenus::DEATH)
		{
			s += "M" + U.menus->Describe() + "|";
			for (const RealmsMenus::Row &r : U.menus->Rows())
			{
				snprintf(buf, sizeof buf, "%u/%d/", r.textId, r.enabled ? 1 : 0);
				s += buf; s += r.label; s += '\x1f';
			}
		}
		else
		{
			// The box pictures change in place (UpdatePictureBillboards); a box
			// shown afresh is new billboards.
			snprintf(buf, sizeof buf, "B%u/%d/%d|", U.menuSerial, (int)U.boxOf[0], (int)U.boxOf[1]);
			s += buf;
		}
	}
	if (U.screen.PopupUp())
	{
		snprintf(buf, sizeof buf, "P%d/%d/%d/%d|", U.screen.PopupItem(), U.screen.PopupPage(), U.screen.PopupTopicsShown() ? 1 : 0,
			U.dialogue.TopicCount());
		s += buf;
		for (const TextLine &l : U.dialogue.TopicLines()) { s.append((const char *)l.text.data(), l.text.size()); }
	}
	return s;
}

std::string ItemLabel(int slot)
{
	if (U.core == nullptr || U.db == nullptr || slot < 0) return std::string();
	const InventorySlot &s = U.core->Items().Slot(slot);
	const int32_t id = (int16_t)s.id;
	const uint32_t text = ItemLabelText(*U.db, id, U.core->InvSink());     // inventory.c:1031-1039
	std::string name = U.dialogue.LineText(text, 0x61);
	const int32_t q = ItemLabelQuantity(*U.db, s);                         // :1573-1584
	if (q != 0)
	{
		char buf[32];
		snprintf(buf, sizeof buf, " (%d)", (int)q);                         // "%s (%D)", 0x75e2c
		name += buf;
	}
	return name;
}

// A picture with holes: palette indices through the map's palette, alpha 0
// where the original's routines wrote nothing (UiCanvas::Mask: the world
// behind), kept as BGRA the way AnimTexture keeps its frames.
class PictureTexture : public FTexture
{
	TArray<uint8_t> Image;
public:
	PictureTexture() { Masked = true; bTranslucent = 1; }
	void SetPicture(const UiCanvas &c, const uint8_t *pal)
	{
		const int w = c.Width() > 0 ? c.Width() : 1, h = c.Height() > 0 ? c.Height() : 1;
		FTexture::SetSize(w, h);
		Image.Resize(w * h * 4);
		memset(Image.Data(), 0, Image.Size());
		const std::vector<uint8_t> &px = c.Pixels(), &mk = c.Mask();
		for (size_t i = 0; i < px.size() && i < mk.size() && i < (size_t)w * h; i++)
		{
			if (mk[i] != 1) continue;
			uint8_t *d = Image.Data() + i * 4;
			d[0] = pal[px[i] * 3 + 2];
			d[1] = pal[px[i] * 3 + 1];
			d[2] = pal[px[i] * 3 + 0];
			d[3] = 255;
		}
	}
	FBitmap GetBgraBitmap(const PalEntry *remap, int *trans) override
	{
		(void)remap;
		if (trans != nullptr) *trans = 1;
		return FBitmap(Image.Data(), Width * 4, Width, Height);
	}
};

// The canvas each picture shows.
const UiCanvas *CanvasOf(int id)
{
	switch (id)
	{
	case PIC_INVENTORY: return &U.painter;
	case PIC_CHOICES: return &U.choicePainter;
	case PIC_LINE: return &U.linePainter;
	case PIC_TMSG: return &U.tmsgPainter;
	case PIC_BOX0: return &U.boxes[0];
	case PIC_BOX1: return &U.boxes[1];
	default: return nullptr;
	}
}

// A picture as a texture, through the map's palette: the other one of its
// two textures takes a new picture (AnimTextures::SetFrame does the same).
FTextureID UploadPicture(int id)
{
	const UiCanvas *c = CanvasOf(id);
	if (c == nullptr) return FNullTextureID();
	Ui::Picture &p = U.pics[id];
	if (p.tex[0] == nullptr)
	{
		for (int i = 0; i < 2; i++)
		{
			FString name;
			name.Format("RTHPIC%d_%d", id, i);
			FGameTexture *t = MakeGameTexture(new PictureTexture(), name.GetChars(), ETextureType::Override);
			t->SetUpscaleFlag(false, true);
			TexMan.AddGameTexture(t);
			p.tex[i] = t;
		}
		p.shown = 0xffffffffu;
	}
	if (p.canvas != c || p.shown != c->Version())
	{
		for (int i = 0; i < 256; i++)
		{
			const PalEntry col = Color((uint8_t)i);
			U.panelPal[i * 3 + 0] = col.r;
			U.panelPal[i * 3 + 1] = col.g;
			U.panelPal[i * 3 + 2] = col.b;
		}
		p.active ^= 1;
		FGameTexture *g = p.tex[p.active];
		static_cast<PictureTexture *>(g->GetTexture())->SetPicture(*c, U.panelPal);
		g->SetSize(c->Width() > 0 ? c->Width() : 1, c->Height() > 0 ? c->Height() : 1);
		g->CleanHardwareData();
		p.shown = c->Version();
		p.canvas = c;
	}
	return p.tex[p.active]->GetID();
}

// Where the pictures stand. A WORLD panel is placed as the original's screen
// places it around the inventory panel, whose centre on that screen is
// (168 + 0x90, 240 + 0x28) = (312, 280) (compute_viewport_half_extents,
// renderer.c:9617-9621): the message boxes the options open over the inventory
// cover it as they do there. The original's pixels are square (VideoMode 8 is
// 640 x 480), PX map units each, at the scale the panel stood at before.
static const double PANEL_SCALE = 1.4;
static const double PXW = PX * PANEL_SCALE;
static const int PANEL_CX = 312, PANEL_CY = 280;

// `ox` / `oy` place a canvas whose own coordinates are relative (the
// inventory's are its anchor's).
void AddWorldPicture(int id, int ox, int oy, double lift, int kind, bool hittable)
{
	const UiCanvas *c = CanvasOf(id);
	if (c == nullptr || c->Width() <= 0 || c->Height() <= 0) return;
	const FTextureID tex = UploadPicture(id);
	if (!tex.isValid()) return;
	const double cx = ox + c->Left() + c->Width() * 0.5, cy = oy + c->Top() + c->Height() * 0.5;
	Add((cx - PANEL_CX) * PXW, (PANEL_CY - cy) * PXW, lift, c->Width() * PXW, c->Height() * PXW, BB_TEXTURE, tex.GetIndex(),
		PalEntry(255, 255, 255, 255), nullptr, 1.0, kind, 0, hittable);
	if (U.elements.empty()) return;
	Element &e = U.elements.back();
	e.left = c->Left(); e.top = c->Top(); e.picW = c->Width(); e.picH = c->Height(); e.picture = id;
}

// A HEAD-LOCKED picture: where it is on the original's 640 x 480 screen,
// that screen standing DIST ahead of the eye, PX map units a pixel; the view
// is the whole screen (ViewSize 0), so its centre (320, 240) is ahead.
void AddHeadPicture(int id)
{
	const UiCanvas *c = CanvasOf(id);
	if (c == nullptr || c->Width() <= 0 || c->Height() <= 0) return;
	const FTextureID tex = UploadPicture(id);
	if (!tex.isValid()) return;
	const double cx = c->Left() + c->Width() * 0.5, cy = c->Top() + c->Height() * 0.5;
	AddViewLocked(DIST, (cx - 320) * PX, (240 - cy) * PX, c->Width() * PX, c->Height() * PX, BB_TEXTURE, PalEntry(255, 255, 255, 255), nullptr, 1.0, tex.GetIndex());
	if (U.elements.empty()) return;
	Element &e = U.elements.back();
	e.kind = E_PICTURE; e.left = c->Left(); e.top = c->Top(); e.picW = c->Width(); e.picH = c->Height(); e.picture = id;
}

// The dialogue text: the original's renderers (roth_dlgdraw) over the
// records roth_dialogue keeps (byte for byte the original's), each a picture.
void BuildDialogue()
{
	if (U.core == nullptr) return;
	const DialogueView v = U.dialogue.View();
	const int ctx = U.core->Interp().State().g_active_dialogue_context;
	U.choicePainter.SetAssets(U.painter.Assets());
	U.linePainter.SetAssets(U.painter.Assets());
	if (v.kind == DialogueView::CHOICES)
	{
		// render_dialogue_text_panel (dialogue_ui.c:814-848): the choices with
		// the marker colour on the highlighted record, centred on the screen;
		// the panel's x origin 0x83add is 0x28 while no topic menu is up
		// (dialogue_ui.c:1209). A world panel; a click is mapped to a row by
		// the original's own rectangles (ChoiceRowAt).
		U.choicePainter.RenderDialogueTextPanel(U.dialogue.Segments(), ctx, 0x28);
		AddWorldPicture(PIC_CHOICES, 0, 0, 0.5, E_DLGPANEL, true);
		return;
	}
	if (v.kind != DialogueView::LINE) return;
	// render_text_ui's drawing (dialogue_ui.c:1016-1047): the current segment
	// laid out to the whole screen pitch (layout_timed_message_text), under the
	// view -- the foot of the screen with the full view -- or under the open
	// inventory panel. Head-locked; not hittable: a click anywhere is what
	// dismisses it (game_core.c:612-632). The picture is the text and its
	// outline only: the original darkens the 3D view behind it
	// (apply_ui_palette_rect), which is the world's pixels, not drawn here.
	const int seg = U.dialogue.SegmentIndex();
	if (seg < 0 || seg >= (int)U.dialogue.Segments().size()) return;
	U.linePainter.RenderTextUiSegment(U.dialogue.Segments()[(size_t)seg], U.screen.IsOpen(), 480 >> 1, 0, 480);
	AddHeadPicture(PIC_LINE);
}

void BuildTimedMessage()
{
	const int n = U.dialogue.TimedMessageLineCount();
	if (n == 0) return;
	// render_active_timed_message (dialogue_ui.c:900-964): one line at the top
	// of the view, or above the open inventory panel; more lines from y 0x20.
	U.tmsgPainter.SetAssets(U.painter.Assets());
	U.tmsgPainter.RenderActiveTimedMessage(U.dialogue.TimedMessageLines(), n, U.screen.IsOpen(), 480 >> 1, 0);
	AddHeadPicture(PIC_TMSG);
}

void BuildInventory()
{
	if (!U.screen.IsOpen() || U.core == nullptr) return;
	// ONE billboard: the original's own panel, drawn by roth_invdraw from the
	// install's pictures. What a click on it means is the original's hotspot
	// rectangles (InventoryPanelPainter::HitTest), not a layout of ours. Its
	// picture is panel-relative; the panel's anchor is (168, 240).
	AddWorldPicture(PIC_INVENTORY, (640 - 0x130) >> 1, 480 >> 1, 0, E_INVPANEL, true);
}

// After the billboards were built: a new picture goes onto the billboard
// already standing, without a rebuild.
void UpdatePictureBillboards()
{
	if (U.level == nullptr) return;
	for (const Element &e : U.elements)
	{
		if (e.picture < 0) continue;
		const UiCanvas *c = CanvasOf(e.picture);
		if (c == nullptr || (U.pics[e.picture].canvas == c && U.pics[e.picture].shown == c->Version())) continue;
		const FTextureID tex = UploadPicture(e.picture);
		for (FBillboard &b : U.level->Billboards) if (b.id == e.bb) b.data = tex.GetIndex();
	}
}

// Where a ray meets a picture's billboard, in the original's own pixels:
// (left, top) its top left, as the original's mouse is. The renderer puts u
// along the basis' right and v down from its top (hw_sprites.cpp emitQuad),
// mirrored by bb_flipu.
bool PanelPixel(const Ray &r, const Element &e, int &px, int &py)
{
	if (U.level == nullptr || e.picW <= 0 || e.picH <= 0) return false;
	const FBillboard *bb = nullptr;
	for (const FBillboard &b : U.level->Billboards) if (b.id == e.bb) { bb = &b; break; }
	if (bb == nullptr) return false;
	DVector3 right, up, normal;
	double hw, hh;
	BillboardBasis(*bb, bb->pos, r.start, bb_tiltbias, bb_scale, right, up, normal, hw, hh);
	const double denom = normal | r.dir;
	if (fabs(denom) < 1e-9 || hw <= 0 || hh <= 0) return false;
	const double t = (normal | (bb->pos - r.start)) / denom;
	if (t <= 0.0) return false;
	const DVector3 rel = (r.start + r.dir * t) - bb->pos;
	double u = ((rel | right) / hw + 1.0) * 0.5;
	const double v = (1.0 - (rel | up) / hh) * 0.5;
	if (bb_flipu) u = 1.0 - u;
	px = e.left + (int)floor(u * e.picW);
	py = e.top + (int)floor(v * e.picH);
	return true;
}

// A click on the panel at pixel (px, py): the action the original's hit test
// gives there, fed to the rules as dispatch_dialogue_ui_action takes it
// (dialogue_ui.c:1556-1808).
void PanelClick(int px, int py, int button)
{
	const int action = U.painter.HitTest(px, py);
	if (action >= 0x17 && action <= 0x1b)                   // a tab: 0x17 + its category
	{
		for (int t = 0; t < 5; t++) if (TabCategory(t) == action - 0x17) U.screen.ClickTab(t);
		U.lastHoverAction = 0x20;                           // :1638
	}
	else if (action >= 0x1c && action <= 0x25) U.screen.ClickCell(action - 0x1c, button);
	else if (action == 0x26) U.screen.ClickOutside();       // the held item's box: commit and close
	else if (action == 0x28) { U.screen.ScrollUp(); U.lastHoverAction = 0x20; }      // :1660
	else if (action == 0x29) { U.screen.ScrollDown(); U.lastHoverAction = 0x20; }    // :1670
	else if (action == 0x2a || action == 0x2b) U.screen.ClickHandBox(action - 0x2a, button);
	else if (action == 0x2c) { if (U.menus != nullptr) U.menus->OpenOptions(); }      // the options: inventory.c:1799-1817
	Printf("[roth_ui] panel click at %d,%d (button %d): action 0x%x\n", px, py, button, action);
}

void BuildPopup()
{
	if (!U.screen.PopupUp() || U.db == nullptr) return;
	// render_inspect_popup_window (dialogue_ui.c:1294-1470): the close-up (its
	// FILM is the cutscene decoder's and is not ported: the icon stands in),
	// the name, the magnifier and "i" buttons, the topics, the document
	// pages with their arrows, the close box. In front of the panel.
	const int32_t id = U.screen.PopupItem();
	const DbItem *rec = U.db->Item(id);
	const double W = 46, H = 34, L = 2.0;
	Add(0, 0, L, W, H, BB_PANEL, 0, PalEntry(255, 8, 8, 10), nullptr, 0.92, E_POP_BACKING, 0, true);
	const std::string name = rec ? U.dialogue.LineText(rec->name, 0x61) : std::string();
	if (!name.empty()) Add(0, H / 2 - 2, L + 0.1, TextPixels(name) * PX * 1.3, ROWH * 1.3, BB_TEXT, 0, Color(U.dialogue.markerSelected), name.c_str(), 1.0, E_NONE, 0, false);
	const std::vector<uint32_t> &pages = U.screen.PopupPages();
	if (!pages.empty())
	{
		FTextureID tex = PageTexture(pages[(size_t)U.screen.PopupPage()]);
		if (tex.isValid()) Add(-4, -1, L + 0.1, 24, 27, BB_TEXTURE, tex.GetIndex(), PalEntry(255, 255, 255, 255), nullptr, 1.0, E_NONE, 0, false);
		if (pages.size() > 1)
		{
			Add(W / 2 - 4, 6, L + 0.1, 3, 3, BB_TEXT, 0, PalEntry(255, 230, 220, 200), "^", 1.0, E_POP_PGUP, 0, true);
			Add(W / 2 - 4, -6, L + 0.1, 3, 3, BB_TEXT, 0, PalEntry(255, 230, 220, 200), "v", 1.0, E_POP_PGDN, 0, true);
		}
	}
	else
	{
		FTextureID tex = IconTexture(id);
		if (tex.isValid()) Add(-8, 0, L + 0.1, 16, 16, BB_TEXTURE, tex.GetIndex(), PalEntry(255, 255, 255, 255), nullptr, 1.0, E_NONE, 0, false);
	}
	Add(-W / 2 + 2.5, -H / 2 + 2.5, L + 0.1, 3.5, 3.5, BB_TEXT, 0, PalEntry(255, 230, 220, 200), "?", 1.0, E_POP_MAG, 0, true);
	if (U.dialogue.TopicCount() != 0)
		Add(-W / 2 + 7, -H / 2 + 2.5, L + 0.1, 3.5, 3.5, BB_TEXT, 0, PalEntry(255, 230, 220, 200), "i", 1.0, E_POP_INFO, 0, true);
	Add(W / 2 - 2.5, H / 2 - 2.5, L + 0.1, 3.0, 3.0, BB_TEXT, 0, PalEntry(255, 230, 220, 200), "x", 1.0, E_POP_CLOSE, 0, true);
	if (U.screen.PopupTopicsShown())
	{
		const std::vector<TextLine> &tl = U.dialogue.TopicLines();
		double z = 8;
		for (size_t i = 0; i < tl.size(); i++)
		{
			std::string row;
			for (const std::string &r : tl[i].Rows()) { if (!row.empty()) row += " "; row += r; }
			const double w = TextPixels(row) * PX;
			Add(10, z, L + 0.2, std::max(w, 4.0), ROWH * 1.2, BB_TEXT, 0, Color(tl[i].Color()), row.c_str(), 1.0, E_POP_TOPIC, (int)i, true);
			z -= tl[i].height * PX + 0.6;
		}
	}
}

// A DBASE100 interface text, resolved as resolve_dbase100_text does
// (dbase100.c:408-422), at most 0x50 bytes as show_message_box asks
// (menu_hud_ui.c:1320).
std::string InterfaceText(uint32_t id)
{
	uint32_t off = 0;
	if (U.db == nullptr || !U.db->InterfaceEntry((int)id, off) || off == 0) return "Missing Text";
	return U.dialogue.LineText(off, 0x50);
}

// The death screen. AN INVENTED LAYOUT, kept until the owner decides: the
// original draws no box here -- its death wait (game_core.c:978-1003) shows
// the world for up to 300 ticks and takes Esc or the quickload key, nothing
// else -- so there is no picture of the original's to draw. Its rows are
// this port's way to give those two keys to a player with no keyboard.
void BuildDeathBox()
{
	if (U.menus == nullptr || !U.menus->IsOpen()) return;
	RealmsMenus &m = *U.menus;
	const std::vector<RealmsMenus::Row> rows = m.Rows();
	std::vector<std::string> text;
	for (const RealmsMenus::Row &r : rows) text.push_back(r.textId ? InterfaceText(r.textId) : r.label);
	const std::string title = m.TitleText();
	const std::string body;
	double widest = TextPixels(title);
	if (!body.empty()) widest = std::max(widest, (double)TextPixels(body));
	for (const std::string &t : text) widest = std::max(widest, (double)TextPixels(t));
	const double S = 1.6;                                   // the menu reads larger than a subtitle
	const double rowH = ROWH * S * 1.5;
	const double W = widest * PX * S + 8.0;
	const int head = body.empty() ? 1 : 2;
	const double H = (rows.size() + head) * rowH + 4.0;
	const double L = 3.0;
	Add(0, 0, L, W, H, BB_PANEL, 0, PalEntry(255, 8, 8, 12), nullptr, 0.9, E_MENU_BACKING, 0, true);
	double z = H / 2 - rowH * 0.5 - 1.0;
	Add(0, z, L + 0.1, TextPixels(title) * PX * S, ROWH * S, BB_TEXT, 0, Color(U.dialogue.markerSelected), title.c_str(), 1.0, E_NONE, 0, false);
	z -= rowH;
	if (!body.empty())
	{
		Add(0, z, L + 0.1, TextPixels(body) * PX * S, ROWH * S, BB_TEXT, 0, Color(U.dialogue.defaultMessageColor[0]), body.c_str(), 1.0, E_NONE, 0, false);
		z -= rowH;
	}
	for (size_t i = 0; i < rows.size(); i++)
	{
		const bool lit = (int)i == m.Selected();
		const bool on = rows[i].enabled;
		Add(0, z, L + 0.1, W - 3.0, rowH * 0.9, BB_PANEL, 0, lit ? PalEntry(255, 70, 60, 40) : PalEntry(255, 24, 24, 24), nullptr,
			lit ? 0.7 : 0.3, E_MENU_ROW, (int)i, true);
		const PalEntry col = !on ? PalEntry(255, 110, 110, 110) : Color(lit ? U.dialogue.markerSelected : U.dialogue.defaultMessageColor[0]);
		if (!text[i].empty())
			Add(0, z, L + 0.2, TextPixels(text[i]) * PX * S, ROWH * S, BB_TEXT, 0, col, text[i].c_str(), 1.0, E_NONE, 0, false);
		z -= rowH;
	}
	Add(W / 2 - 2.0, H / 2 - 2.0, L + 0.2, 3, 3, BB_TEXT, 0, PalEntry(255, 230, 220, 200), "x", 1.0, E_MENU_BACK, 0, true);
}

// The slot names buffer read_savegame_slot_names fills (savegame.c:1262-1294):
// 9 cells of 0x30 bytes, {0, '~'} for a slot with no save.
std::vector<uint8_t> SlotNamesBuffer(const RealmsMenus &m)
{
	std::vector<uint8_t> n(0x1b0, 0);
	const std::vector<std::string> &names = m.SlotNames();
	for (int s = 0; s < 9; s++)
	{
		uint8_t *cell = n.data() + s * 0x30;
		cell[1] = 0x7e;
		if (s >= (int)names.size() || names[(size_t)s].empty()) continue;
		const std::string &nm = names[(size_t)s];
		for (size_t i = 0; i < nm.size() && i < 0x2f; i++) cell[i] = (uint8_t)nm[i];
		cell[nm.size() < 0x2f ? nm.size() : 0x2f] = 0;
	}
	return n;
}

// show_message_box's picture for `box` into U.boxes[i] (phases 1-5).
void OpenBoxPicture(int i, RealmsMenus::Box box, uint32_t flags, int sel, int scroll)
{
	RealmsMenus &m = *U.menus;
	const MsgBoxDesc *d = FindMsgBoxDesc(RealmsMenus::DescriptorOf(box, m.FrontEnd()));
	U.boxOf[i] = d != nullptr ? box : RealmsMenus::NONE;
	if (d == nullptr) return;
	MessageBoxPainter &b = U.boxes[i];
	b.SetAssets(U.painter.Assets());
	b.screenPitch = 640;                    // the retail ROTH.INI's VideoMode 8
	b.screenHeight = 480;
	b.anchorX = (640 - 0x130) >> 1;         // the panel anchor (compute_viewport_half_extents)
	b.anchorY = 480 >> 1;
	b.resolveText = [](uint8_t *dest, uint32_t maxlen, uint32_t id) { return U.dialogue.ResolveDbase100TextBytes(dest, maxlen, (int)id, 0); };
	b.slotNames = SlotNamesBuffer(m);
	// The engine's saves hold no picture of the original's kind, and there is
	// no snapshot of the view (g_ui_panel_scratch_handle): the original's
	// state when it has neither -- nothing drawn there.
	b.thumbnail = nullptr;
	b.panelSnapshot.clear();
	b.Open(*d, flags, sel, scroll);
	for (const std::string &pr : b.Problems()) Printf("[roth_menu] box picture: %s\n", pr.c_str());
}

// The box pictures follow the menus: composed when a box is shown afresh,
// then one pass of its loop's drawing every tic (two of the original's frame
// ticks): the selection, the scroll, the zoom fade.
void TickMenuPictures()
{
	RealmsMenus *m = U.menus;
	if (m == nullptr || !m->IsOpen() || m->Current() == RealmsMenus::DEATH)
	{
		U.boxOf[0] = U.boxOf[1] = RealmsMenus::NONE;
		U.menuSerial = 0xffffffffu;
		return;
	}
	// The slot lists read the save folder when they open (the runtime's
	// TickMenus may hand the names over after this tic's picture was composed).
	const bool list = m->Current() == RealmsMenus::LOAD || m->Current() == RealmsMenus::SAVE;
	if (list && U.boxOf[0] == m->Current() && SlotNamesBuffer(*m) != U.boxes[0].slotNames) U.menuSerial = 0xffffffffu;
	if (m->ShowSerial() != U.menuSerial)
	{
		if (m->ParentVisible())
		{
			// The box under stays as it stood: it was the top one until now.
			if (U.boxOf[0] == m->Parent()) { U.boxes[1] = U.boxes[0]; U.boxOf[1] = U.boxOf[0]; }
			else OpenBoxPicture(1, m->Parent(), m->Parent() == RealmsMenus::MAIN ? 1u : 0x8c1u, m->ParentSelected(), 0);
		}
		else U.boxOf[1] = RealmsMenus::NONE;
		OpenBoxPicture(0, m->Current(), m->ShowFlags(), m->Selected(), m->Scroll());
		U.menuSerial = m->ShowSerial();
		U.pics[PIC_BOX0].shown = U.pics[PIC_BOX1].shown = 0xffffffffu;   // new pictures, whatever their versions say
	}
	if (U.boxOf[0] != RealmsMenus::NONE)
		U.boxes[0].Pass(m->Selected(), m->Scroll() != U.boxes[0].Scroll() ? m->Scroll() : -1, 2);
}

void BuildMenu()
{
	if (U.menus == nullptr || !U.menus->IsOpen()) return;
	if (U.menus->Current() == RealmsMenus::DEATH) { BuildDeathBox(); return; }
	// show_message_box's own picture of each box on screen (roth_menudraw):
	// the one under, when it stays, then the one on top, a little in front;
	// only the top one takes the pointer, through hit_test_ui_element.
	if (U.boxOf[1] != RealmsMenus::NONE) AddWorldPicture(PIC_BOX1, 0, 0, 3.0, E_PICTURE, false);
	if (U.boxOf[0] != RealmsMenus::NONE) AddWorldPicture(PIC_BOX0, 0, 0, 3.5, E_MENUBOX, true);
}

void Rebuild(player_t *player)
{
	const std::string sig = Signature();
	if (sig == U.signature) return;
	const bool menuUp = U.menus != nullptr && U.menus->IsOpen();
	const bool worldPanel = U.dialogue.MenuUp() || U.screen.IsOpen() || menuUp;
	if (!worldPanel) U.anchored = false;
	else if (!U.anchored) Anchor(player);
	ClearBillboards();
	U.signature = sig;
	if (menuUp) { BuildMenu(); return; }        // modal: nothing else is shown or clicked under it
	BuildDialogue();
	BuildTimedMessage();
	BuildInventory();
	BuildPopup();
}

//==========================================================================
//
// Clicks.
//
//==========================================================================

void ClickElement(const Element &e, int button)
{
	(void)button;           // the inventory's clicks carry their button through PanelClick
	switch (e.kind)
	{
	case E_POP_MAG: U.screen.PopupMagnifier(); break;
	case E_POP_INFO: U.screen.PopupToggleTopics(); break;
	case E_POP_TOPIC: U.screen.PopupTopic(e.arg); break;
	case E_POP_CLOSE: U.screen.PopupClose(); break;
	case E_POP_PGUP: U.screen.PopupPageUp(); break;
	case E_POP_PGDN: U.screen.PopupPageDown(); break;
	case E_MENU_ROW: if (U.menus != nullptr) { U.menus->Choose(e.arg); Printf("[roth_menu] row %d chosen: %s\n", e.arg, U.menus->Describe().c_str()); } break;
	case E_MENU_BACK: if (U.menus != nullptr) U.menus->Back(); break;
	default: break;
	}
}

// A click with nothing of ours under the pointer.
bool ClickNothing()
{
	if (U.screen.PopupUp()) return true;                    // the popup is modal
	if (U.screen.IsOpen()) { U.screen.ClickOutside(); return true; }   // action 0x26
	return false;
}

// Where the pointer is on the choice panel, as the original reads its mouse:
// (x + y) is the "position" (dialogue_ui.c:237), the row its own rectangles
// give (ChoiceRowAt, dialogue_ui.c:255-266).
bool ChoicePointer(const Ray &r, const Element *e, int &pos, int &row)
{
	pos = 0; row = -1;
	if (e == nullptr || e->kind != E_DLGPANEL) return false;
	int px = 0, py = 0;
	if (!PanelPixel(r, *e, px, py)) return false;
	pos = px + py;
	row = U.choicePainter.ChoiceRowAt(px, py);
	return true;
}

// game_core.c:607-632: a click while the dialogue owns the pointer.
bool ClickDialogue(const Ray *r, const Element *e)
{
	if (!U.dialogue.PointerBelongsToDialogue()) return false;
	int pos = 0, row = -1;
	if (r != nullptr) ChoicePointer(*r, e, pos, row);
	U.dialogue.PointerClick(pos, row);
	if (row >= 0) Printf("[roth_ui] choice %d picked\n", row);
	return true;
}

// The top message box's own hit test at the pointer (hit_test_ui_element, as
// show_message_box's loop calls it): 0 off the box.
int MenuHit(const Ray &r, const Element *e)
{
	if (e == nullptr || e->kind != E_MENUBOX || U.boxOf[0] == RealmsMenus::NONE) return 0;
	int px = 0, py = 0;
	if (!PanelPixel(r, *e, px, py)) return 0;
	return U.boxes[0].LoopHit(px, py);
}

bool Press(player_t *player, int forcedButton)
{
	if (!U.attached || player == nullptr) return false;
	Ray rays[2];
	int n = 0;
	if (!PointerRays(player, rays, n)) return false;
	const bool menuUp = U.menus != nullptr && U.menus->IsOpen();
	if (menuUp && U.menus->Current() != RealmsMenus::DEATH)
	{
		// A box is modal: the click goes to its loop with the hit where the
		// pointer is (menu_hud_ui.c:1626-1638); off the box it is a click on nothing.
		int hit = 0;
		for (int i = 0; i < n && hit == 0; i++) hit = MenuHit(rays[i], HitElement(rays[i]));
		U.menus->PointerClick(hit);
		Printf("[roth_menu] click, hit %d: %s\n", hit, U.menus->IsOpen() ? U.menus->Describe().c_str() : "closed");
		return true;
	}
	for (int i = 0; i < n; i++)
	{
		const Element *e = HitElement(rays[i]);
		if (e == nullptr) continue;
		if (menuUp)
		{
			// The death screen's rows (an invented layout; see BuildDeathBox).
			if (e->kind < E_MENU_ROW) continue;
			ClickElement(*e, InventoryScreen::PRIMARY);
			return true;
		}
		// The main hand (or the view) is the primary button, the off hand the
		// secondary: the original needs both on the inventory.
		const int button = forcedButton ? forcedButton : (i == 0 ? InventoryScreen::PRIMARY : InventoryScreen::SECONDARY);
		if (U.screen.PopupUp() && e->kind < E_POP_MAG) continue;    // only the popup takes clicks while it is up
		if (e->kind == E_DLGPANEL) return ClickDialogue(&rays[i], e);
		if (e->kind == E_POP_BACKING) return true;
		if (e->kind == E_INVPANEL)
		{
			int px = 0, py = 0;
			if (PanelPixel(rays[i], *e, px, py)) PanelClick(px, py, button);
			return true;
		}
		ClickElement(*e, button);
		return true;
	}
	if (menuUp) return true;                    // a press beside the box does nothing, and goes no further
	if (ClickDialogue(nullptr, nullptr)) return true;
	return ClickNothing();
}

void UpdateHover(player_t *player)
{
	Ray rays[2];
	int n = 0;
	const Element *e = nullptr;
	if (PointerRays(player, rays, n) && n > 0) e = HitElement(rays[0]);
	const int bb = e ? e->bb : 0;
	if (U.menus != nullptr && U.menus->IsOpen())
	{
		if (U.menus->Current() == RealmsMenus::DEATH)
		{
			if (bb != U.hoverBb && e != nullptr && e->kind == E_MENU_ROW) U.menus->Hover(e->arg);
		}
		else U.menus->PointerAt(n > 0 ? MenuHit(rays[0], e) : 0);   // every frame, as the loop does (:1591-1607)
		U.hoverBb = bb;
		return;
	}
	// The dialogue's hover (game_core.c:556-569; dialogue_ui.c:231-271): the
	// pointer's position and the row the original's rectangles put it on.
	int pos = 0, row = -1;
	if (n > 0) ChoicePointer(rays[0], e, pos, row);
	U.dialogue.SetPointer(pos, row);
	if (U.dialogue.PointerBelongsToDialogue())
		U.dialogue.UpdateChoiceHighlight(pos, row);
	// The inventory cursor follows the pointer over the grid (handle_cursor_click,
	// input.c:1290-1297): a cell's action, when it is not the last one hovered.
	if (e != nullptr && e->kind == E_INVPANEL && !U.screen.PopupUp() && n > 0)
	{
		int px = 0, py = 0;
		if (PanelPixel(rays[0], *e, px, py))
		{
			const int act = U.painter.HitTest(px, py);
			if (act >= 0x1c && act <= 0x25 && act != U.lastHoverAction)
			{
				U.lastHoverAction = act;
				U.screen.HoverCell(act - 0x1c);
			}
		}
	}
	U.hoverBb = bb;
}

//==========================================================================
//
// The pickup fly-in: tick_item_pickup_lock (inventory.c:106-139). The item
// travels from where it lay to 64 units in front of the player, 0x50 up,
// over 0x1c ticks, along the fraction table at 0x7115c (EXE data, read out of
// the running original: 127 126 124 122 120 115 110 ... 5 -- each /128 of the
// way still to go).
//
//==========================================================================

static const uint8_t kFlyFrac[0x1c] = {
	127, 126, 124, 122, 120, 115, 110, 105, 100, 95, 90, 85, 80, 75,
	70, 65, 60, 55, 50, 45, 40, 35, 30, 25, 20, 15, 10, 5,
};

void TickFly(player_t *player)
{
	if (!U.fly.active || U.level == nullptr) return;
	FBillboard *bb = nullptr;
	for (FBillboard &b : U.level->Billboards) if (b.id == U.fly.bb) { bb = &b; break; }
	U.fly.ticks += 2;
	if (U.fly.ticks >= 0x1c || bb == nullptr || player == nullptr || player->mo == nullptr)
	{
		if (bb != nullptr)
			for (unsigned i = 0; i < U.level->Billboards.Size(); i++)
				if (U.level->Billboards[i].id == U.fly.bb) { U.level->Billboards.Delete(i); break; }
		U.fly.active = false;
		return;
	}
	AActor *mo = player->mo;
	const DVector3 target = mo->Pos() + DVector3(mo->Angles.Yaw.Cos(), mo->Angles.Yaw.Sin(), 0) * 64.0 + DVector3(0, 0, 0x50 * 0.5);
	const double f = kFlyFrac[U.fly.ticks] / 128.0;
	bb->pos = target + (U.fly.from - target) * f;
}

void StartFly(const DVector3 &from, uint16_t renderId)
{
	if (U.level == nullptr) return;
	FTextureID tex = FNullTextureID();
	const int sn = FindLogicSprite(renderId);
	if (sn >= 0 && sn < (int)sprites.Size() && sprites[sn].numframes > 0)
		tex = SpriteFrames[sprites[sn].spriteframes].Texture[0];
	if (!tex.isValid()) return;
	if (U.fly.active)
		for (unsigned i = 0; i < U.level->Billboards.Size(); i++)
			if (U.level->Billboards[i].id == U.fly.bb) { U.level->Billboards.Delete(i); break; }
	FBillboard bb;
	bb.id = U.level->NextBillboardID++;
	bb.pos = from;
	bb.width = 16;
	bb.height = 16;
	bb.facing = BBF_CAMERA;
	bb.payload = BB_TEXTURE;
	bb.data = tex.GetIndex();
	bb.color = PalEntry(255, 255, 255, 255);
	bb.flags = BBFL_PERSISTENT | BBFL_NOSAVE | BBFL_NOHIT;
	bb.spawntic = U.level->maptime;
	U.level->Billboards.Push(bb);
	U.fly.active = true;
	U.fly.from = from;
	U.fly.ticks = 0;
	U.fly.bb = bb.id;
}

} // namespace

void RuntimeInvSink::PickupFlyStarted(int32_t x, int32_t y, int32_t z, uint16_t renderId)
{
	const DVector3 from = U.havePendingFly ? U.pendingFlyFrom : DVector3(x, y, z);
	U.havePendingFly = false;
	StartFly(from, renderId);
}

//==========================================================================
//
// The hooks.
//
//==========================================================================

void InterfaceAttach(GameCore &core, Dbase100 &db, const Install &install)
{
	U.core = &core;
	U.db = &db;
	U.dialogue.Bind(&core.Interp(), &db);
	U.screen.Bind(&core, &U.dialogue, &db);
	const std::string text = install.DataFile("DBASE400.DAT");
	if (!text.empty()) U.dialogue.text = Install::ReadWholeFile(text);
	const std::string icons = install.DataFile("DBASE200.DAT");
	if (!icons.empty()) U.dbase200 = Install::ReadWholeFile(icons);
	U.dbase300Path = install.DataFile("DBASE300.DAT");
	// The inventory panel's pictures (load_icons_all, das_assets.c:183-200:
	// DATA\ICONS.ALL beside the databases) and the original's font, which is
	// in ROTH.EXE (see roth_invdraw.h). ROTH.EXE sits in the game's ROTH folder.
	const std::string iconsAll = install.DataFile("DATA/ICONS.ALL");
	if (!iconsAll.empty()) U.iconsAll = Install::ReadWholeFile(iconsAll);
	std::string exeErr = "ROTH.EXE not found beside the install";
	{
		std::vector<std::string> tries;
		const std::string e0 = install.DataFile("ROTH.EXE");
		if (!e0.empty()) tries.push_back(e0);
		for (const char *rel : { "/../ROTH/ROTH.EXE", "/../ROTH/roth.exe", "/../ROTH.EXE", "/../roth.exe", "/ROTH/ROTH.EXE" })
			tries.push_back(install.Path() + rel);
		for (const std::string &t : tries)
		{
			const std::vector<uint8_t> exe = Install::ReadWholeFile(t);
			if (exe.empty()) continue;
			if (FindRothExeFont(exe, U.font, exeErr)) { Printf("[roth_ui] font: %s\n", t.c_str()); break; }
			exeErr = t + ": " + exeErr;
		}
	}
	InvPanelAssets pa;
	pa.iconsAll = &U.iconsAll;
	pa.dbase200 = &U.dbase200;
	pa.shade = U.shade.empty() ? nullptr : U.shade.data();
	pa.blend = U.blend.empty() ? nullptr : U.blend.data();
	pa.font = U.font.ok() ? &U.font : nullptr;
	pa.readLine = [](uint8_t *dest, uint32_t maxlen, uint32_t off) { return U.dialogue.ReadNextDialogueLine(dest, maxlen, off, 0); };
	pa.labelText = [](int32_t id) { return (U.db != nullptr && U.core != nullptr) ? ItemLabelText(*U.db, id, U.core->InvSink()) : 0u; };
	U.painter.Bind(&U.screen, &core, &db, pa, false);       // the retail ROTH.INI's VideoMode 8: no line doubling
	Printf("[roth_ui] inventory panel: ICONS.ALL %s, font %s\n", U.iconsAll.empty() ? "MISSING" : "ok",
		U.font.ok() ? "ok" : exeErr.c_str());
	U.dialogue.speechAvailable = SoundSpeechAvailable();          // audio.c:1210
	U.dialogue.screenPitch = 640;           // the retail ROTH.INI's VideoMode 8
	U.attached = true;
	Printf("[roth_ui] attached: text %s, icons %s, documents %s, speech %s\n",
		U.dialogue.text.empty() ? "MISSING" : "ok", U.dbase200.empty() ? "MISSING" : "ok",
		U.dbase300Path.empty() ? "MISSING" : "ok", U.dialogue.speechAvailable ? "ok" : "MISSING");
}

InventorySink *InterfaceInventorySink() { return &U.invSink; }
Inventory *InterfaceInventory() { return U.core != nullptr ? &U.core->Items() : nullptr; }

void InterfaceSetMenus(RealmsMenus *menus) { U.menus = menus; }

uint32_t InterfaceReadNextDialogueLine(uint8_t *dest, uint32_t maxlen, uint32_t voiceOffset, uint32_t flag)
{
	return U.dialogue.ReadNextDialogueLine(dest, maxlen, voiceOffset, flag);
}

uint32_t InterfaceOpenDialogueWindow(const uint8_t *text, uint32_t bytes)
{
	const uint32_t r = U.dialogue.Dbase100OpenDialogueWindow(text, bytes);
	const DialogueView v = U.dialogue.View();
	for (size_t i = 0; i < v.rows.size(); i++) Printf("[roth_ui] choice %d: %s\n", (int)i, v.rows[i].c_str());
	return r;
}

uint32_t InterfaceOpenDialogueWindowAlt(const uint8_t *text, uint32_t bytes)
{
	const uint32_t r = U.dialogue.Dbase100OpenDialogueWindowAlt(text, bytes);
	for (const TextLine &l : U.dialogue.Segments())
		for (const std::string &row : l.Rows()) Printf("[roth_ui] line (colour %d): %s\n", (int)l.Color(), row.c_str());
	return r;
}

void InterfaceDialogueVoiceForceEnd(uint32_t eax, uint32_t edx) { U.dialogue.DialogueVoiceForceEnd(eax, edx); }
int32_t InterfaceInventoryPanelOpen() { return U.screen.g_inventory_panel_open; }
int32_t InterfaceInspectPopupActive() { return U.screen.g_inspect_popup_active; }
void InterfaceArmPanelExit() { U.screen.ArmExit(); }
void InterfaceRequestPanelClose() { U.screen.RequestClose(); }

void InterfaceSetPalette(const TextureSet &art)
{
	const std::vector<Colour> *pal = art.PackPalette();
	if (pal == nullptr || pal->size() < 256) return;
	std::vector<PalEntry> p(256);
	for (int i = 0; i < 256; i++) p[(size_t)i] = PalEntry(255, (*pal)[(size_t)i].r, (*pal)[(size_t)i].g, (*pal)[(size_t)i].b);
	if (p != U.palette)
	{
		// The icons are drawn in the map's own palette, as the original's are.
		U.palette = p;
		U.iconTranslation = -1;
		U.iconTex.clear();
		U.pageTex.clear();
		for (Ui::Picture &pic : U.pics) pic.shown = 0xffffffffu;   // the pictures go up again in the new colours
	}
	// The pack's shade and blend tables (map_load.c:376-377), which the panel's
	// tints read: copied, since the pack may be closed before the panel is.
	const Pack *pk = art.PackData();
	if (pk != nullptr && pk->ShadeTables() != nullptr && pk->BlendTable() != nullptr)
	{
		U.shade.assign(pk->ShadeTables(), pk->ShadeTables() + 0x4000);
		U.blend.assign(pk->BlendTable(), pk->BlendTable() + 0x10000);
		U.painter.SetPackTables(U.shade.data(), U.blend.data());
	}
}

bool InterfaceGamePalette6(uint8_t out[0x300])
{
	if (U.palette.size() < 256) return false;
	// The pack palette was widened with Expand6 (roth_das.cpp), which keeps
	// the 6-bit value in the top six bits.
	for (int i = 0; i < 256; i++)
	{
		out[i * 3 + 0] = (uint8_t)(U.palette[(size_t)i].r >> 2);
		out[i * 3 + 1] = (uint8_t)(U.palette[(size_t)i].g >> 2);
		out[i * 3 + 2] = (uint8_t)(U.palette[(size_t)i].b >> 2);
	}
	return true;
}

void InterfaceEndLevel()
{
	// The level's billboard array dies with it.
	U.elements.clear();
	U.signature.clear();
	U.anchored = false;
	U.fly.active = false;
	U.level = nullptr;
	U.hoverBb = 0;
}

static void RunQueued(player_t *player);

void InterfaceTick(FLevelLocals *level)
{
	if (!U.attached || level == nullptr) return;
	if (U.level != level) { U.level = level; U.elements.clear(); U.signature.clear(); U.anchored = false; }
	player_t *player = level->GetConsolePlayer();
	RunQueued(player);
	// One tic is two of the original's 70 Hz frame ticks.
	U.dialogue.PollVoice();
	U.dialogue.Frame(2);                                 // voice_stream_pump + render_text_ui
	// redraw_inventory_cursor_cell (game_core.c:886), before the keys' frame
	// (:894), at the original's 70 Hz frame tick.
	if (U.screen.IsOpen()) U.painter.RedrawInventoryCursorCell((uint16_t)(level->maptime * 2));
	U.screen.Post();                                     // update_inventory_screen's tail
	if (U.screen.PopupUp()) U.screen.PopupFrame();       // the popup's own loop
	U.core->Items().TickPickupLock(2);                   // tick_item_pickup_lock
	TickFly(player);
	UpdateHover(player);
	TickMenuPictures();
	Rebuild(player);
	UpdatePictureBillboards();
}

bool InterfaceUse(player_t *player)
{
	if (!U.attached) return false;
	return Press(player, 0);
}

bool InterfaceObjectAction(AActor *mo, int rothSector, int rothIndex, bool use)
{
	if (!U.attached || U.db == nullptr || U.core == nullptr || mo == nullptr) return false;
	// The object record itself (16 bytes: x, y, render word, rotation,
	// flags, light, +9, z, ...).
	Object o;
	if (!RuntimeObjectRecord(rothSector, rothIndex, o)) return false;
	const uint16_t renderId = (uint16_t)(o.textureIndex | (o.textureSource << 8));
	if (!use)
	{
		// examine_object_under_cursor, input.c:995-1012: OnInspect, mode 1.
		const DbTriggerBlock *blk = nullptr;
		if (FindInspectByRenderId(*U.db, renderId, &blk) == 0 || blk == nullptr) return false;
		U.core->Interp().EvalDialogueRecordConditionWithCleanup(blk->offset, 1);
		return true;
	}
	// activate_targeted_object's type-4 branch, input.c:1034-1086.
	const uint8_t b9 = o.renderType;                         // def + 9
	if (b9 & 0x12) return false;                             // :1037
	if (b9 & 1) return false;                                // keyed: a command object, the runtime's
	if (U.core->Items().PickupLockArmed()) return false;     // :1055, an item is still flying
	if (b9 & 0x10) return false;                             // :1056
	GiveContext ctx;
	ctx.x = o.x; ctx.y = o.y; ctx.id = renderId; ctx.flags7 = o.flags; ctx.z = o.z;
	U.pendingFlyFrom = mo->Pos() + DVector3(0, 0, mo->Height / 2);
	U.havePendingFly = true;
	const int32_t r = U.core->Items().GiveItemByRenderId(*U.db, renderId, &ctx, U.core->InvSink());   // :1077
	U.havePendingFly = false;
	if (r == 0) return false;                                // not an item: nothing happens (:1078)
	Printf("[roth_ui] picked up item %d (%s)\n", (int)r, U.dialogue.LineText(U.db->Item(r) ? U.db->Item(r)->name : 0, 0x61).c_str());
	mo->Destroy();                                           // destroy_dynamic_entity, :1085
	return true;
}

//==========================================================================
//
// Console commands.
//
// The panels' keys, for binding (flat) or for a VR controller binding:
//   roth_ui up | down | left | right | enter | skip | close | tab <0-4> |
//           nexttab | examine | hold | use | primary | secondary
//   roth_invpanel                       open / close the inventory (the 'I' key)
// Test instruments (queued like roth_dialogue: +exec runs before the map):
//   roth_ui_open inventory | dialogue <action id>
//   roth_ui_choice <row>                click choice row <row> of the open menu
//   roth_ui_select <item id>            put an item in the right hand from the panel
//   roth_ui_dump                        print what the panels show
//   roth_ui_invclick <x> <y> [button]   click the inventory panel at its pixel (x, y)
//
//==========================================================================

static void KeyVerb(player_t *player, const std::string &verb, int arg)
{
	InventoryScreen &s = U.screen;
	DialogueUi &d = U.dialogue;
	if (verb == "primary" || verb == "secondary")
	{
		Press(player, verb == "primary" ? InventoryScreen::PRIMARY : InventoryScreen::SECONDARY);
		return;
	}
	if (U.menus != nullptr && U.menus->IsOpen())
	{
		if (verb == "up") U.menus->Up();
		else if (verb == "down") U.menus->Down();
		else if (verb == "enter") U.menus->Enter();
		else if (verb == "skip" || verb == "close") U.menus->Back();
		return;
	}
	if (s.PopupUp())                                        // file_config.c:1063-1117
	{
		if (verb == "up") s.PopupKeyUp();
		else if (verb == "down") s.PopupKeyDown();
		else if (verb == "enter") s.PopupKeyEnter();
		else if (verb == "skip" || verb == "close" || verb == "examine") s.PopupKeyExit();
		return;
	}
	if (s.IsOpen())                                         // inventory.c:1698-1817
	{
		if (verb == "left") s.KeyLeft();
		else if (verb == "right") s.KeyRight();
		else if (verb == "up") s.KeyUp();
		else if (verb == "down") s.KeyDown();
		else if (verb == "enter") s.KeyEnter();
		else if (verb == "skip" || verb == "close") s.KeySelect();
		else if (verb == "tab") s.KeyTab(arg);
		else if (verb == "nexttab") s.KeyNextTab();
		else if (verb == "examine") s.KeyExamine();
		else if (verb == "hold") s.KeyHold();
		else if (verb == "use") s.KeyUse();
		return;
	}
	const Dbase100InterpState &st = U.core->Interp().State();
	if (st.g_dialogue_busy_flag == 0) return;               // game_core.c:907-920
	if (verb == "up") d.ChoiceSelectPrev();
	else if (verb == "down") d.ChoiceSelectNext();
	else if (verb == "enter") d.UseEnterKey();
	else if (verb == "skip") d.TryInterruptDialogueVoice();
}

static void Dump()
{
	const DialogueView v = U.dialogue.View();
	Printf("[roth_ui] dialogue: %s, %d row(s), highlighted %d, colour %d, page %d/%d%s\n",
		v.kind == DialogueView::NONE ? "none" : v.kind == DialogueView::LINE ? "line" : "choices",
		(int)v.rows.size(), v.highlighted, (int)v.color, v.page + 1, v.pages, v.waitsForClick ? ", waits for a click" : "");
	for (size_t i = 0; i < v.rows.size(); i++) Printf("[roth_ui]   %d: %s\n", (int)i, v.rows[i].c_str());
	if (U.screen.IsOpen())
	{
		const InventoryEntryList &l = U.screen.Entries();
		Printf("[roth_ui] inventory: list %d, %d entries, cursor %d, held %d\n", U.screen.ActiveList(), (int)l.slots.size(),
			U.screen.CursorEntry(), U.screen.HeldEntry());
		for (size_t i = 0; i < l.slots.size(); i++) Printf("[roth_ui]   %d: %s\n", (int)i, ItemLabel(l.slots[i]).c_str());
	}
	else Printf("[roth_ui] inventory: closed\n");
	if (U.screen.PopupUp()) Printf("[roth_ui] popup: item %d, %d page(s), %d topic(s)\n", U.screen.PopupItem(),
		(int)U.screen.PopupPages().size(), U.dialogue.TopicCount());
	Printf("[roth_ui] %d panel billboard(s)\n", (int)U.elements.size());
	for (const auto &kv : U.invSink.notBuilt) Printf("[roth_ui] asked for, not built: %s x%d\n", kv.first.c_str(), kv.second);
	for (const std::string &pr : U.painter.Problems()) Printf("[roth_ui] inventory panel: %s\n", pr.c_str());
	for (int i = 0; i < 2; i++)
		if (U.boxOf[i] != RealmsMenus::NONE)
			Printf("[roth_ui] message box %d: %x flags %x at %d,%d %dx%d, rows %d, selection %d, scroll %d, picture version %u\n", i, U.boxes[i].Desc().va,
				U.boxes[i].Flags(), U.boxes[i].BoxX(), U.boxes[i].BoxY(), U.boxes[i].BoxW(), U.boxes[i].BoxH(), U.boxes[i].Rows(), U.boxes[i].Selected(),
				U.boxes[i].Scroll(), U.boxes[i].Version());
	for (const UiCanvas *c : { (const UiCanvas *)&U.choicePainter, (const UiCanvas *)&U.linePainter, (const UiCanvas *)&U.tmsgPainter, (const UiCanvas *)&U.boxes[0] })
		for (const std::string &pr : c->Problems()) Printf("[roth_ui] picture: %s\n", pr.c_str());
	if (U.screen.IsOpen())
	{
		Printf("[roth_ui] inventory panel picture %dx%d, version %u; hotspots:", U.painter.Width(), U.painter.Height(), U.painter.Version());
		for (const InvHotspot &h : U.painter.TabHotspots()) Printf(" [%d,%d,%u,%u,0x%x]", h.x, h.y, h.w, h.h, h.action);
		for (const InvHotspot &h : U.painter.PanelHotspots()) Printf(" [%d,%d,%u,%u,0x%x]", h.x, h.y, h.w, h.h, h.action);
		Printf("\n");
	}
}

static void RunQueued(player_t *player)
{
	if (U.queue.empty() || U.core == nullptr || !U.core->IsAttached()) return;
	std::vector<std::vector<std::string>> batch;
	batch.swap(U.queue);
	for (const std::vector<std::string> &c : batch)
	{
		const std::string &verb = c[0];
		const int arg = c.size() > 1 ? atoi(c[1].c_str()) : 0;
		if (verb == "open")
		{
			if (c.size() > 1 && c[1] == "inventory") { U.screen.Open(); Printf("[roth_ui] inventory opened\n"); }
			else if (c.size() > 2 && c[1] == "dialogue")
			{
				const uint32_t r = U.core->RunAction((uint32_t)atoi(c[2].c_str()));
				Printf("[roth_ui] dialogue: action %s returned %d\n", c[2].c_str(), (int)r);
			}
		}
		else if (verb == "invpanel")
		{
			if (U.screen.IsOpen()) U.screen.KeySelect();
			else U.screen.Open();
		}
		else if (verb == "choice")
		{
			if (!U.dialogue.MenuUp()) { Printf("[roth_ui] choice: no menu is up\n"); continue; }
			// As the beam does it: hover the row, then click on it.
			U.dialogue.UpdateChoiceHighlight(1000 + arg, arg);
			U.dialogue.PointerClick(1000 + arg, arg);
			Printf("[roth_ui] choice %d picked\n", arg);
		}
		else if (verb == "select")
		{
			const int32_t id = arg;
			if (!U.screen.IsOpen()) U.screen.Open();
			U.screen.KeyTab(ItemTabIndex(*U.db, id));
			int entry = -1;
			for (size_t i = 0; i < U.screen.Entries().slots.size(); i++)
				if ((int16_t)U.core->Items().Slot(U.screen.Entries().slots[i]).id == id) { entry = (int)i; break; }
			if (entry < 0) { Printf("[roth_ui] select: item %d is not carried (or not on its tab)\n", id); continue; }
			U.screen.CursorTo(entry);
			U.screen.KeyEnter();                // Enter on the entry: it goes into its hand, the panel closes
			const int rh = U.core->Items().RightHand(), lh = U.core->Items().LeftHand();
			Printf("[roth_ui] select: item %d -> right hand slot %d, left hand slot %d\n", id, rh, lh);
		}
		else if (verb == "invclick")
		{
			// A click at panel pixel (x, y), as a pointer on the picture makes it.
			if (!U.screen.IsOpen()) { Printf("[roth_ui] invclick: the inventory is not open\n"); continue; }
			const int y = c.size() > 2 ? atoi(c[2].c_str()) : 0;
			const int b = c.size() > 3 ? atoi(c[3].c_str()) : InventoryScreen::PRIMARY;
			PanelClick(arg, y, b);
		}
		else if (verb == "dump") Dump();
		else KeyVerb(player, verb, arg);
	}
}

static void Queue(FCommandLine &argv, const char *verb, int first)
{
	std::vector<std::string> c;
	c.push_back(verb);
	for (int i = first; i < argv.argc(); i++) c.push_back(argv[i]);
	U.queue.push_back(c);
}

CCMD(roth_ui)
{
	if (argv.argc() < 2) { Printf("roth_ui up|down|left|right|enter|skip|close|tab <n>|nexttab|examine|hold|use|primary|secondary\n"); return; }
	Queue(argv, argv[1], 2);
}
CCMD(roth_invpanel) { Queue(argv, "invpanel", 1); }
CCMD(roth_ui_open)
{
	if (argv.argc() < 2) { Printf("roth_ui_open inventory | dialogue <action id>\n"); return; }
	Queue(argv, "open", 1);
}
CCMD(roth_ui_choice)
{
	if (argv.argc() < 2) { Printf("roth_ui_choice <row>\n"); return; }
	Queue(argv, "choice", 1);
}
CCMD(roth_ui_select)
{
	if (argv.argc() < 2) { Printf("roth_ui_select <item id>\n"); return; }
	Queue(argv, "select", 1);
}
CCMD(roth_ui_dump) { Queue(argv, "dump", 1); }
CCMD(roth_ui_invclick)
{
	if (argv.argc() < 3) { Printf("roth_ui_invclick <x> <y> [1 primary | 2 secondary] -- a click on the inventory panel's pixel\n"); return; }
	Queue(argv, "invclick", 1);
}

} // namespace roth
