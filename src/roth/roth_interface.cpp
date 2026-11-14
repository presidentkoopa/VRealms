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
#include "roth_popupdraw.h"
#include "roth_uidraw.h"
#include "roth_huddraw.h"
#include "roth_runtime.h"
#include "roth_icons.h"
#include "roth_install.h"
#include "roth_texture.h"
#include "roth_das.h"
#include "roth_objects.h"
#include "roth_sound.h"
#include "roth_raw.h"
#include "roth_menus.h"
#include "roth_invmouse.h"
#include "roth_panelmap.h"

#include <algorithm>
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
#include "r_utility.h"   // r_viewpoint.FieldOfView, for the flat screen scale
EXTERN_CVAR(Int, vr_mode) // flat or VR, for the flat screen scale (file scope on purpose)
EXTERN_CVAR(Float, r_view_vstretch) // the flat projection's FY/FX (hw_entrypoint.cpp)
#include "printf.h"
#include "textures.h"
#include "texturemanager.h"
#include "image.h"
#include "palettecontainer.h"
#include "r_data/r_translate.h"
#include "v_video.h"
#include "r_data/sprites.h"
#include "r_data/models.h"          // the off hand's wrist, for the mount
#include "r_data/vr_handpose.h"
#include "hw_vrmodes.h"             // VR_OFFHAND, VR_ScriptHaptic
#include "hwrenderer/postprocessing/hw_postprocessshader.h"   // the low-health edge (GLDEFS RothLowHealth)
#include "roth_hands.h"


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
	// The HUD's (inventory.c:263-267, 438-442, 486, 1411).
	bool IsActiveAmmo(int32_t itemId) override { return itemId == ActorsActiveAmmoId(); }
	void AmmoDisplayChanged(int delta) override;
	void HeldItemSlideReset() override;
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
	E_POPUP,                                            // the examine popup's picture; a click goes to its own hit test
	E_MENUBOX,                                          // a message box's picture; a click is mapped by hit_test_ui_element
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
enum PictureId { PIC_INVENTORY, PIC_CHOICES, PIC_LINE, PIC_TMSG, PIC_BOX0, PIC_BOX1, PIC_HUDTOP, PIC_HUDHELD, PIC_POPUP,
	PIC_HUDTOP_MOUNTED,     // the top strip without the health panel, while the panel is on the off-hand mount
	PIC_MOUNT_HEALTH,       // the health panel, cut from the top strip, on the mount
	PIC_COUNT };

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
	// The HUD, drawn as the original draws it (roth_huddraw): its top strip
	// and the held item's corner, head-locked where the original's screen has them.
	HudPainter hud;
	bool hudBound = false;
	FLevelLocals *hudLevel = nullptr;
	int32_t hudLastHealth = 0;
	int hudLastEquips = 0;
	bool hudInvOpen = false;
	// Over the rendered view, a shadow pixel of shade row r is black at
	// alpha hudBehind[r], measured from the map's own shade table and palette
	// (see InterfaceSetPalette); a translucent pixel is its colour at 128
	// (the blend table is a 50/50 mix, measured likewise).
	uint8_t hudBehind[32] = {};
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
	// The original's mouse on the inventory panel (handle_cursor_click's panel half, roth_invmouse.h):
	// every pointer on the panel -- the crosshair or aim beam with the use key, the VR hands -- goes
	// through it. Its g_hover_latch is [0x81300], the action the hover last moved the cursor for.
	InvMouse mouse;
	// What that mouse reads of the dialogue (a busy line takes every click: InvMouse 15).
	std::unique_ptr<InvMouseDialogueUi> mouseDlg;
	// A VR hand drove the pointer on the panel last tic (roth_vrpick.cpp): the aim-pose / crosshair
	// hover (UpdateHover) stands down, so the cursor follows the pointing finger alone.
	bool handsDrivePointer = false;
	std::vector<uint8_t> dbase200;
	std::string dbase300Path;
	bool attached = false;

	std::vector<PalEntry> palette;                  // the map's pack palette, 8-bit
	// The examine popup, drawn and run as the original draws and runs it
	// (roth_popupdraw: the window, its film, its pages, its topic menu).
	InspectPopupView popup;
	std::vector<uint8_t> popupFilm;                 // the close-up's GDV bytes (DBASE300 from its offset)
	double popupIsr = 0.0;                          // GDV timer ticks (60 Hz) owed to the film

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

	// THE OFF-HAND MOUNT (owner's S1 / H1, docs/VR_CHANGES_LEDGER.md row 34).
	// In VR the subtitle line and the health panel stand on a small panel over
	// the off hand's wrist, facing the eye, instead of on the original's screen
	// in front of it; flat keeps the original's screen. The pictures are the
	// original's own, drawn by its routines (roth_dlgdraw, roth_huddraw), only
	// placed and scaled here. `mounted` is decided at each rebuild (Signature).
	bool mounted = false;
	bool mountTest = false;                         // roth_mounttest: the flat test's stand-in (roth_handtest's off hand)
	HudCanvas hudTopMounted, mountHealth;           // cut from hud.TopStrip()
	uint32_t mountFrom = 0xffffffffu;               // the TopStrip() version they were cut from
	struct MountPiece { int bb = 0; double w = 0, h = 0; };
	MountPiece mountHealthBb, mountLineBb;
	// The low-health cue (H1): a red screen edge (the RothLowHealth post-process
	// shader) and a heartbeat on both controllers, below half -- standing in for
	// the original's "low" bar picture, which is on the mount too.
	bool lowCue = false;
	int heartPhase = 0;
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

void RuntimeInvSink::AmmoDisplayChanged(int delta)
{
	U.hud.g_weapon_panel_dirty = 1;                          // [0x83e80] = 1
	ActorsAddWeaponAmmo(delta);                             // g_active_weapon_ammo += delta
}

void RuntimeInvSink::HeldItemSlideReset() { U.hud.g_held_slide = 0; }

void RuntimeInvSink::RightHandChanged(int slot)
{
	if (U.core == nullptr || U.db == nullptr) return;
	// update_selected_item_icon (inventory.c:1461-1493): the HUD's held icon.
	if (U.hudBound) U.hud.UpdateSelectedItemIcon(slot < 0 ? 0 : (int16_t)U.core->Items().Slot(slot).id);
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

// load_das_cache_resource on DBASE300.DAT (das_assets.c:421-454): resource
// `res`'s bytes (a size dword at res * 8, then the bytes), read on demand (the
// file is 22 MB). Empty when it cannot be read.
std::vector<uint8_t> Dbase300Resource(uint32_t res)
{
	std::vector<uint8_t> out;
	FILE *f = U.dbase300Path.empty() ? nullptr : fopen(U.dbase300Path.c_str(), "rb");
	if (f == nullptr) return out;
	uint8_t sz[4] = {};
	if (fseek(f, (long)res << 3, SEEK_SET) == 0 && fread(sz, 1, 4, f) == 4)
	{
		const uint32_t n = sz[0] | (sz[1] << 8) | (sz[2] << 16) | ((uint32_t)sz[3] << 24);
		if (n > 8 && n < (8u << 20))
		{
			out.resize(n);
			if (fread(out.data(), 1, n, f) != n) out.clear();
		}
	}
	fclose(f);
	return out;
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
	std::string s = U.mounted ? "V|" : "";
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
	if (U.popup.painter.IsOpen())
	{
		// The popup's picture changes in place (UpdatePictureBillboards); a
		// window opened afresh is a new billboard.
		const InspectPopupPainter &pp = U.popup.painter;
		snprintf(buf, sizeof buf, "P%d/%d,%d,%d,%d|", U.screen.PopupItem(), pp.WinX(), pp.WinY(), pp.WinW(), pp.WinH());
		s += buf;
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
	// `behind`: null, a pixel not drawn by the routines is transparent; else
	// (the HUD) a pixel blended over the view is drawn as that blend over the
	// engine's view: mask 2 (blend table, the picture's index the source) its
	// colour at alpha 128, mask 0x20 + r (shade row r) black at alpha behind[r].
	void SetPicture(const UiCanvas &c, const uint8_t *pal, const uint8_t *behind = nullptr)
	{
		const int w = c.Width() > 0 ? c.Width() : 1, h = c.Height() > 0 ? c.Height() : 1;
		FTexture::SetSize(w, h);
		Image.Resize(w * h * 4);
		memset(Image.Data(), 0, Image.Size());
		const std::vector<uint8_t> &px = c.Pixels(), &mk = c.Mask();
		for (size_t i = 0; i < px.size() && i < mk.size() && i < (size_t)w * h; i++)
		{
			uint8_t *d = Image.Data() + i * 4;
			if (behind != nullptr && mk[i] == 2)
			{
				d[0] = pal[px[i] * 3 + 2]; d[1] = pal[px[i] * 3 + 1]; d[2] = pal[px[i] * 3 + 0]; d[3] = 128;
				continue;
			}
			if (behind != nullptr && mk[i] >= 0x20)
			{
				d[0] = d[1] = d[2] = 0; d[3] = behind[(mk[i] - 0x20) & 31];
				continue;
			}
			if (mk[i] != 1) continue;
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
	case PIC_HUDTOP: return &U.hud.TopStrip();
	case PIC_HUDHELD: return &U.hud.HeldIcon();
	case PIC_POPUP: return &U.popup.painter.Presented();
	case PIC_HUDTOP_MOUNTED: return &U.hudTopMounted;
	case PIC_MOUNT_HEALTH: return &U.mountHealth;
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
		const bool hud = id == PIC_HUDTOP || id == PIC_HUDHELD || id == PIC_HUDTOP_MOUNTED || id == PIC_MOUNT_HEALTH;
		static_cast<PictureTexture *>(g->GetTexture())->SetPicture(*c, U.panelPal, hud ? U.hudBehind : nullptr);
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
	// Text runs (roth_uidraw.h UiSetTextAsRuns), drawn in the JFO font on the boxes the original's
	// glyphs would fill. Nothing turns runs on today, so there are none: kept for the letters and
	// maps (owner, 2026-11-13), whose low-res cursive may later be overlaid in this font.
	for (const UiTextRun &r : c->TextRuns())
	{
		const double sx = ox + c->Left() + r.x + r.w * 0.5, sy = oy + c->Top() + r.y + r.h * 0.5;
		Add((sx - PANEL_CX) * PXW, (PANEL_CY - sy) * PXW, lift + 0.1, r.w * PXW, r.h * PXW, BB_TEXT, 0,
			Color(r.colour), r.text.c_str(), 1.0, E_PICTURE, 0, false);
	}
}

// A HEAD-LOCKED picture: where it is on the original's 640 x 480 screen,
// that screen standing DIST ahead of the eye; the view is the whole screen
// (ViewSize 0), so its centre (320, 240) is ahead.
//
// FLAT: the original's screen IS its view, and the flat projection is built
// to be the original's (rothmap.cpp: fov 91.82, r_view_vstretch FY/FX), so a
// screen pixel spans, at DIST, exactly what it spans in the original:
//   across  2*DIST*tan(fov/2)/640                     (FX = 309.77)
//   down    2*DIST*tan(fov/2)/(fovratio)/480           (FY = 355.06)
// with fovratio as the hardware renderer forms it (hw_entrypoint.cpp:746-764:
// 4/3 on a widescreen view, the view's ratio otherwise, times r_view_vstretch).
// A fixed 0.1 unit/pixel put the HUD small and in from the corners (run h1);
// one square scale from the horizontal term put it above the view (run h2).
// VR keeps PX: no VR adaptation is decided yet (CLAUDE.md). vr_mode and
// r_view_vstretch are declared at file scope above: inside this anonymous
// namespace they would name different variables.
static void ScreenUnitsPerPixel(double &across, double &down)
{
	across = down = PX;
	if (vr_mode != 0)
	{
		// The headset stretches the world vertically by the level's pixelstretch (rothmap.cpp,
		// the original's FY/FX); a head-locked picture is drawn in the world, so its height is
		// divided by it to keep the original's pixels square, as on its own screen.
		const double ps = r_viewpoint.ViewLevel ? r_viewpoint.ViewLevel->pixelstretch : 1.0;
		if (ps > 0.0) down = PX / ps;
		return;
	}
	const double fov = r_viewpoint.GetFieldOfView().Degrees();
	if (!(fov > 1.0 && fov < 179.0)) return;
	const double t = tan(fov * (M_PI / 360.0));
	const double wr = r_viewwindow.WidescreenRatio;
	double fovratio = wr >= 1.3 ? 1.333333 : (wr > 0.1 ? wr : 1.333333);
	if (r_view_vstretch > 0.0f) fovratio *= r_view_vstretch;
	across = 2.0 * DIST * t / 640.0;
	down = 2.0 * DIST * (t / fovratio) / 480.0;
}

void AddHeadPicture(int id)
{
	const UiCanvas *c = CanvasOf(id);
	if (c == nullptr || c->Width() <= 0 || c->Height() <= 0) return;
	const FTextureID tex = UploadPicture(id);
	if (!tex.isValid()) return;
	double ux = PX, uy = PX;
	ScreenUnitsPerPixel(ux, uy);
	const double cx = c->Left() + c->Width() * 0.5, cy = c->Top() + c->Height() * 0.5;
	AddViewLocked(DIST, (cx - 320) * ux, (240 - cy) * uy, c->Width() * ux, c->Height() * uy, BB_TEXTURE, PalEntry(255, 255, 255, 255), nullptr, 1.0, tex.GetIndex());
	if (U.elements.empty()) return;
	Element &e = U.elements.back();
	e.kind = E_PICTURE; e.left = c->Left(); e.top = c->Top(); e.picW = c->Width(); e.picH = c->Height(); e.picture = id;
	for (const UiTextRun &r : c->TextRuns())       // the owner's font, as AddWorldPicture
	{
		const double sx = c->Left() + r.x + r.w * 0.5, sy = c->Top() + r.y + r.h * 0.5;
		AddViewLocked(DIST * 0.999, (sx - 320) * ux, (240 - sy) * uy, r.w * ux, r.h * uy, BB_TEXT,
			Color(r.colour), r.text.c_str(), 1.0);
	}
}

//==========================================================================
//
// THE OFF-HAND MOUNT (owner's S1 and H1, docs/VR_CHAPTER01_SHADOWS.md 8.7,
// docs/VR_CHANGES_LEDGER.md row 34). Not the original's: a VR placement.
// What stands on it is the original's own -- the subtitle line as
// render_text_ui draws it (roth_dlgdraw) and the health panel as
// render_health_status_panel / render_player_health_bar draw it, "low"
// picture and all (roth_huddraw), cut from the top strip where
// blit_panel_image puts it -- scaled onto a panel over the off hand's wrist.
// Its own: where it stands (above the wrist, the health panel over the line,
// both centred), its size (roth_mount_scale map units an original pixel) and
// that it turns to face the eye. Flat keeps the original's screen.
//
// THE OFF HAND, named so (owner: left-hand support comes later): the
// controller VR_OFFHAND names, the left while the main hand is the right.
//
//==========================================================================

// Live tuning for the headset; NOT archived (the owner's ini is shared).
CUSTOM_CVAR(Float, roth_mount_scale, 0.05f, CVAR_NOINITCALL)
{
	if (self < 0.005f) self = 0.005f;
	if (self > 0.5f) self = 0.5f;
}
CVAR(Float, roth_mount_lift, 3.0f, 0)   // map units from the wrist to the panel's foot

// The mount is up: in VR with the off hand's actor drawn, or in the flat test
// (roth_mounttest with roth_handtest's hands).
bool MountWanted()
{
	if (U.level == nullptr) return false;
	player_t *pl = U.level->GetConsolePlayer();
	if (pl == nullptr || pl->mo == nullptr) return false;
	const bool vr = pl->mo->OverrideAttackPosDir;
	if (!vr && !(U.mountTest && VRHandTest_Active())) return false;
	AActor *ha = HandActor(VRHandPose_SideForRole(VR_OFFHAND));
	return ha != nullptr && !(ha->renderflags & RF_INVISIBLE);
}

// The wrist of the off hand's drawn model (Root_joint), else its actor.
bool MountWrist(DVector3 &out)
{
	const int side = VRHandPose_SideForRole(VR_OFFHAND);
	AActor *ha = HandActor(side);
	if (ha == nullptr) return false;
	out = ha->Pos();
	FModel *model = Model_ActorModel(ha);
	FVRHandPoints pts;
	DVector3 w;
	if (model != nullptr && VRHandPose_FingerPoints(model, side, pts) && Model_ActorPointToWorld(ha, pts.wrist, w)) out = w;
	return true;
}

// The frame a hand-locked billboard rides (g_levellocals.h BillboardHandFrame,
// resolved by the renderer each frame): a point found this tic is stored in it,
// so the panel stays on the wrist between tics (roth_vrpick.cpp does the same
// for its beam). With no VR pose published (the flat test), world points.
struct MountLock
{
	bool on = false;
	DVector3 origin, fwd, right, up;
	DVector3 Local(const DVector3 &w) const { const DVector3 d = w - origin; return DVector3(d | fwd, d | right, d | up); }
};

MountLock MountLockNow()
{
	MountLock L;
	player_t *pl = U.level != nullptr ? U.level->GetConsolePlayer() : nullptr;
	if (pl == nullptr || pl->mo == nullptr || !pl->mo->OverrideAttackPosDir) return L;
	FBillboard probe;
	probe.flags = BBFL_HANDOFF;
	double yaw, pitch, roll;
	if (!BillboardHandFrame(probe, L.origin, yaw, pitch, roll)) return L;
	BillboardHandAxes(yaw, pitch, roll, L.fwd, L.right, L.up);
	L.on = true;
	return L;
}

// One of the original's pictures on the mount: a billboard that turns to the
// eye, drawn over the world (the hand must not hide it), placed by TickMount.
void AddMountPicture(int id, Ui::MountPiece &piece)
{
	piece = Ui::MountPiece();
	const UiCanvas *c = CanvasOf(id);
	if (c == nullptr || c->Width() <= 0 || c->Height() <= 0 || U.level == nullptr) return;
	const FTextureID tex = UploadPicture(id);
	if (!tex.isValid()) return;
	// Square pixels in the headset's vertically stretched world (as ScreenUnitsPerPixel).
	const double ps = r_viewpoint.ViewLevel ? r_viewpoint.ViewLevel->pixelstretch : 1.0;
	const double u = roth_mount_scale;
	FBillboard bb;
	bb.id = U.level->NextBillboardID++;
	bb.width = c->Width() * u;
	bb.height = c->Height() * u / (ps > 0.0 ? ps : 1.0);
	bb.facing = BBF_CAMERA;
	bb.payload = BB_TEXTURE;
	bb.data = tex.GetIndex();
	bb.color = PalEntry(255, 255, 255, 255);
	bb.alpha = 1.0;
	bb.flags = BBFL_PERSISTENT | BBFL_NOSAVE | BBFL_NODEPTH | BBFL_NOHIT;
	bb.spawntic = U.level->maptime;
	U.level->Billboards.Push(bb);
	Element e;
	e.bb = bb.id;
	e.kind = E_PICTURE;
	e.left = c->Left(); e.top = c->Top(); e.picW = c->Width(); e.picH = c->Height(); e.picture = id;
	U.elements.push_back(e);
	piece.bb = bb.id;
	piece.w = bb.width;
	piece.h = bb.height;
}

// Every tic: the mount's pieces over the wrist, stacked upward on the eye's
// own up (the health panel over the line, as on the original's screen).
void TickMount()
{
	if (!U.mounted || U.level == nullptr) return;
	DVector3 wrist;
	if (!MountWrist(wrist)) return;
	player_t *pl = U.level->GetConsolePlayer();
	if (pl == nullptr || pl->mo == nullptr) return;
	const DVector3 eye = pl->mo->Pos() + DVector3(0, 0, pl->viewz - pl->mo->Z());
	DVector3 view = wrist - eye;
	const double vl = view.Length();
	view = vl > 1e-6 ? view / vl : DVector3(1, 0, 0);
	DVector3 up = DVector3(0, 0, 1) - view * view.Z;
	const double ul = up.Length();
	up = ul > 1e-6 ? up / ul : DVector3(0, 0, 1);
	const MountLock L = MountLockNow();
	const double gap = 4.0 * roth_mount_scale;           // four of the original's pixels between them
	double at = roth_mount_lift;
	auto place = [&](Ui::MountPiece &p) {
		if (p.bb == 0) return;
		const DVector3 centre = wrist + up * (at + p.h * 0.5);
		at += p.h + gap;
		for (FBillboard &b : U.level->Billboards)
		{
			if (b.id != p.bb) continue;
			b.flags = (b.flags & ~BBFL_HANDANY) | (L.on ? BBFL_HANDOFF : 0);
			b.pos = L.on ? L.Local(centre) : centre;
			b.drawPos = centre;
			break;
		}
	};
	place(U.mountLineBb);
	place(U.mountHealthBb);
}

// The two pictures the mount needs from the top strip, cut again whenever the
// HUD draws a new one: the health panel alone, and the strip without it.
void DeriveMountCanvases()
{
	const HudCanvas &top = U.hud.TopStrip();
	if (top.Version() == U.mountFrom) return;
	U.mountFrom = top.Version();
	int x = 0, y = 0, w = 0, h = 0;
	U.hudTopMounted = top;
	if (!U.hud.HealthPanelRect(x, y, w, h)) { U.mountHealth.SetRect(0, 0, 0, 0, false); return; }
	const UiCanvas::Saved cut = top.SaveRegion(x, y, w, h);
	U.mountHealth.SetRect(x, y, w, h, false);
	U.mountHealth.RestoreRegion(cut);
	UiCanvas::Saved none = cut;
	std::fill(none.pix.begin(), none.pix.end(), (uint8_t)0);
	std::fill(none.mask.begin(), none.mask.end(), (uint8_t)0);
	U.hudTopMounted.RestoreRegion(none);
}

// THE LOW-HEALTH CUE (owner's H1). Below half -- the original's own test for
// its "low" bar picture (HudPainter::HealthBarLow) -- while the mount is up:
// the red edge (GLDEFS "RothLowHealth", a post-process pass over each eye) and
// a heartbeat on both controllers. Its beat (a stronger then a weaker pulse,
// 26 tics apart, about 80 a minute) is ours: the original has none.
static const int HEART_PERIOD = 26, HEART_SECOND = 6;

void SetLowHealthEdge(bool on, double strength)
{
	for (unsigned i = 0; i < PostProcessShaders.Size(); i++)
	{
		PostProcessShader &sh = PostProcessShaders[i];
		if (sh.Name.CompareNoCase("RothLowHealth") != 0) continue;
		sh.Enabled = on;
		double *v = sh.Uniforms["uStrength"].Values;
		v[0] = strength; v[1] = 0.0; v[2] = 0.0; v[3] = 1.0;
	}
}

void TickLowHealthCue()
{
	const int32_t hp = PlayerHealth(), maxhp = PlayerMaxHealth();
	const bool low = U.hudBound && U.mounted && hp > 0 && HudPainter::HealthBarLow(hp, maxhp);
	if (low != U.lowCue)
	{
		U.lowCue = low;
		U.heartPhase = 0;
		Printf("[roth_mount] low-health cue %s (health %d of %d)\n", low ? "on" : "off", (int)hp, (int)maxhp);
		if (!low) SetLowHealthEdge(false, 0.0);
	}
	if (!low) return;
	const int ph = U.heartPhase;
	U.heartPhase = (U.heartPhase + 1) % HEART_PERIOD;
	if (ph == 0) { VR_ScriptHaptic(VR_MAINHAND, 0.7, 60.0); VR_ScriptHaptic(VR_OFFHAND, 0.7, 60.0); }
	if (ph == HEART_SECOND) { VR_ScriptHaptic(VR_MAINHAND, 0.45, 45.0); VR_ScriptHaptic(VR_OFFHAND, 0.45, 45.0); }
	const bool beat = ph < 3 || (ph >= HEART_SECOND && ph < HEART_SECOND + 3);
	SetLowHealthEdge(true, beat ? 0.8 : 0.55);
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
	// VR: on the off-hand mount (owner's S1), the same picture.
	if (U.mounted) AddMountPicture(PIC_LINE, U.mountLineBb);
	else AddHeadPicture(PIC_LINE);
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

// A picture's billboard as roth_panelmap.h takes it: BillboardBasis' axes (the
// renderer's), its front on the side of `eye`, the picture's own pixel rectangle.
bool PlaneOf(const Element &e, const DVector3 &eye, PanelPlane &p)
{
	if (U.level == nullptr || e.picW <= 0 || e.picH <= 0) return false;
	const FBillboard *bb = nullptr;
	for (const FBillboard &b : U.level->Billboards) if (b.id == e.bb) { bb = &b; break; }
	if (bb == nullptr) return false;
	DVector3 right, up, normal;
	double hw, hh;
	BillboardBasis(*bb, bb->pos, eye, bb_tiltbias, bb_scale, right, up, normal, hw, hh);
	if (hw <= 0 || hh <= 0) return false;
	p.centre = PanelV3(bb->pos.X, bb->pos.Y, bb->pos.Z);
	p.right = PanelV3(right.X, right.Y, right.Z);
	p.up = PanelV3(up.X, up.Y, up.Z);
	const double side = (eye - bb->pos) | normal;
	p.front = side >= 0 ? PanelV3(normal.X, normal.Y, normal.Z) : PanelV3(-normal.X, -normal.Y, -normal.Z);
	p.halfw = hw;
	p.halfh = hh;
	p.flipu = bb_flipu;
	// The picture's rows: the canvas' height, which is the original's logical rows while no
	// line doubling is drawn (the engine binds every canvas without it: VideoMode 8).
	p.left = e.left; p.top = e.top; p.w = e.picW; p.h = e.picH;
	return true;
}

// Where a ray meets a picture's billboard, in the original's own pixels:
// (left, top) its top left, as the original's mouse is (roth_panelmap.h; the
// same arithmetic this always did, tools/invhand_test M2).
bool PanelPixel(const Ray &r, const Element &e, int &px, int &py)
{
	PanelPlane p;
	if (!PlaneOf(e, r.start, p)) return false;
	PanelPoint pt;
	if (!PanelRay(p, PanelV3(r.start.X, r.start.Y, r.start.Z), PanelV3(r.dir.X, r.dir.Y, r.dir.Z), pt)) return false;
	px = pt.px;
	py = pt.py;
	return true;
}

// A click on the panel at pixel (px, py): the original's mouse click there
// (handle_cursor_click: its hit test, then dispatch_dialogue_ui_action),
// roth_invmouse.h -- the one path every pointer's click on the panel takes.
void PanelClick(int px, int py, int button)
{
	U.mouse.Click(U.painter, U.screen, px, py, button);
	Printf("[roth_ui] panel click at %d,%d (button %d): action 0x%x\n", px, py, button, U.mouse.lastAction);
}

void BuildPopup()
{
	if (!U.popup.painter.IsOpen()) return;
	// ONE billboard: the original's own window (roth_popupdraw), as the
	// screen shows it -- the close-up film's frame, the document page, the
	// topic menu over them -- placed where the original's screen has it, in
	// front of the inventory panel it opens over. A click on it is the
	// original's hit test (InspectPopupPainter::HitTest).
	AddWorldPicture(PIC_POPUP, 0, 0, 2.0, E_POPUP, true);
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
	// The death wait draws nothing: the original shows the world and reads
	// two keys (game_core.c:978-1003; FlowResponder has them).
	if (U.menus->Current() == RealmsMenus::DEATH) return;
	// show_message_box's own picture of each box on screen (roth_menudraw):
	// the one under, when it stays, then the one on top, a little in front;
	// only the top one takes the pointer, through hit_test_ui_element.
	if (U.boxOf[1] != RealmsMenus::NONE) AddWorldPicture(PIC_BOX1, 0, 0, 3.0, E_PICTURE, false);
	if (U.boxOf[0] != RealmsMenus::NONE) AddWorldPicture(PIC_BOX0, 0, 0, 3.5, E_MENUBOX, true);
}

// The HUD's two pictures, head-locked where the original's screen has them
// (the same screen model as the subtitle, AddHeadPicture): the top strip (the
// health panel, the weapon panel, the corner peek) and the bottom right corner
// (the selected item's icon). Shown once a level has drawn them; under the
// inventory, a dialogue or a menu they keep what the last game frame drew.
void BuildHud()
{
	if (!U.hudBound || U.hudLevel == nullptr) return;
	if (U.mounted)
	{
		// VR: the health panel on the off-hand mount (owner's H1); the rest of
		// the strip (the weapon panel) where it was.
		AddHeadPicture(PIC_HUDTOP_MOUNTED);
		AddMountPicture(PIC_MOUNT_HEALTH, U.mountHealthBb);
	}
	else AddHeadPicture(PIC_HUDTOP);
	AddHeadPicture(PIC_HUDHELD);
}

HudWeapon HudWeaponNow()
{
	HudWeapon w;
	int equips = 0;
	ActorsHudWeapon(w.item, w.ammo, w.ammoCap, w.panel, equips);
	return w;
}

// render_weapon_hud(1, &attrs), its write-back and its tail (WC:994-1058).
void HudRenderWeaponHud()
{
	const HudWeapon w = HudWeaponNow();
	const int32_t p = U.hud.RenderWeaponHud(w);
	if (p != w.panel) ActorsSetHudPanel(p);
	if (p != 0) ActorsRebuildWeaponList();                  // WC:1057
}

// The HUD's part of the original's frame, once a tic (two of its 70 Hz ticks).
void TickHud(FLevelLocals *level)
{
	if (!U.hudBound) return;
	const int32_t hp = PlayerHealth(), maxhp = PlayerMaxHealth();
	int32_t item = 0, ammo = 0, cap = 0, panel = 0;
	int equips = 0;
	ActorsHudWeapon(item, ammo, cap, panel, equips);
	if (U.hudLevel != level)
	{
		// The game's start or a load (game_core.c:838-843): the health bar's
		// flag set, repaint_hud_and_present's refresh_hud_layout, the held
		// icon enabled and rebuilt.
		U.hudLevel = level;
		U.hud.g_health_bar_dirty = 1;
		U.hud.RefreshHudLayout(hp, maxhp);
		U.hud.g_held_icon_enabled = 1;
		const int r = U.core != nullptr ? U.core->Items().RightHand() : -1;
		U.hud.UpdateSelectedItemIcon(r < 0 ? 0 : (int16_t)U.core->Items().Slot(r).id);
		U.hudLastHealth = hp;
		U.hudLastEquips = equips - 1;                       // the weapon panel drawn on the first frame
	}
	// Every write of the health raises 0x83e7c (WC:32, R:9820, R:10015) but
	// DBASE100 op 0x26's zero (dbase100.c:904), which goes to death.
	if (hp != U.hudLastHealth) { U.hudLastHealth = hp; if (hp != 0) U.hud.g_health_bar_dirty = 1; }
	// The inventory closing returns to the game through repaint_hud_and_present
	// (menu_hud_ui.c:468-478).
	const bool invOpen = U.screen.IsOpen();
	if (U.hudInvOpen && !invOpen) U.hud.RefreshHudLayout(hp, maxhp);
	U.hudInvOpen = invOpen;
	// gameplay_frame_step runs in the game's own mode only (game_core.c:486-490,
	// 870-878): not under the inventory, a dialogue or a menu, whose screens
	// keep the HUD the last frame drew.
	// The death wait (mode 0x20) draws them too (game_core.c:490).
	const bool dying = U.menus != nullptr && U.menus->IsOpen() && U.menus->Current() == RealmsMenus::DEATH;
	const bool gameplay = !invOpen && (!(U.menus != nullptr && U.menus->IsOpen()) || dying) && U.dialogue.View().kind == DialogueView::NONE;
	if (!gameplay) return;
	if (equips != U.hudLastEquips) { U.hudLastEquips = equips; HudRenderWeaponHud(); }   // game_core.c:873-876
	// run_gameplay_frame's corner timer: the original's pointer in the top
	// left 0x20 x 0x20 of its screen. This engine has no pointer on the view
	// in play (the hands and the view aim), so the peek is never asked for.
	U.hud.CornerPeekTimer(false, 2);
	// tick_weapon_hud_ammo_anim already ran in ActorsTick (ActorWorld::RechargeFrame).
	U.hud.DrawFrame(2, hp, maxhp, [] { return HudWeaponNow(); }, U.core != nullptr ? (int32_t)U.core->Items().AdamItemId() : 0);
	for (const std::string &pr : U.hud.Problems())
	{
		static std::vector<std::string> said;
		if (std::find(said.begin(), said.end(), pr) == said.end()) { said.push_back(pr); Printf("[roth_hud] %s\n", pr.c_str()); }
	}
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
	U.mountHealthBb = Ui::MountPiece();
	U.mountLineBb = Ui::MountPiece();
	// The death wait is the original's mode 0x20, in which gameplay_frame_step
	// still draws the HUD and render_text_ui(0) -- the death record's line and
	// any timed message (game_core.c:477-497, 960-1003): those, and no box.
	if (menuUp && U.menus->Current() == RealmsMenus::DEATH) { BuildDialogue(); BuildTimedMessage(); BuildHud(); return; }
	if (menuUp) { BuildMenu(); BuildHud(); return; }   // modal: nothing else is clicked under it; the HUD the last frame drew stays (the box draws over the original's frozen frame)
	BuildDialogue();
	BuildTimedMessage();
	BuildInventory();
	BuildPopup();
	BuildHud();
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
	default: break;
	}
}

// The pointer over the popup's picture, in the original's screen pixels; off
// it, a point no rectangle of the popup holds.
void PopupPointerAt(const Ray *rays, int n, int *which)
{
	if (which != nullptr) *which = 0;
	for (int i = 0; i < n; i++)
	{
		const Element *e = HitElement(rays[i]);
		int px = 0, py = 0;
		if (e != nullptr && e->kind == E_POPUP && PanelPixel(rays[i], *e, px, py))
		{
			U.popup.SetPointer(px, py, 0);
			if (which != nullptr) *which = i;
			return;
		}
	}
	U.popup.SetPointer(-0x4000, -0x4000, 0);
}

// Where a pointer is, as the original's mouse on the INVENTORY screen reads it:
// panel pixels (screen minus the panel's anchor). On the inventory's picture;
// on the choice menu's (the original's screen coordinates it is drawn at); or
// nowhere (off both: the original's "outside the panel").
void InventoryPointerAt(const Ray &r, int &px, int &py)
{
	px = py = PANEL_NOWHERE;
	const Element *e = HitElement(r);
	if (e == nullptr) return;
	int x = 0, y = 0;
	if (e->kind == E_INVPANEL && PanelPixel(r, *e, x, y)) { px = x; py = y; }
	else if (e->kind == E_DLGPANEL && PanelPixel(r, *e, x, y)) { px = x - U.mouse.anchorX; py = y - U.mouse.anchorY; }
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
	if (!menuUp && U.popup.Active())
	{
		// The popup is modal: every press is its handle_cursor_click's, at the
		// pointer (input.c:1310-1347), taken by its next frame.
		int which = 0;
		PopupPointerAt(rays, n, &which);
		U.popup.Press(forcedButton ? forcedButton : (which == 0 ? InventoryScreen::PRIMARY : InventoryScreen::SECONDARY));
		return true;
	}
	if (!menuUp && U.screen.IsOpen() && !U.screen.PopupUp())
	{
		// THE INVENTORY SCREEN: every click is its handle_cursor_click's at the
		// pointer (input.c:1310-1347, roth_invmouse.h) -- on the panel, beside it
		// (0x26: commit and close), or anywhere while a line is busy (0x27: the
		// line is skipped, dialogue_ui.c:552-553). The main hand (or the view) is
		// the primary button, the off hand the secondary.
		int which = 0, px = PANEL_NOWHERE, py = PANEL_NOWHERE;
		for (int i = 0; i < n; i++)
		{
			InventoryPointerAt(rays[i], px, py);
			if (px != PANEL_NOWHERE) { which = i; break; }
		}
		PanelClick(px, py, forcedButton ? forcedButton : (which == 0 ? InventoryScreen::PRIMARY : InventoryScreen::SECONDARY));
		return true;
	}
	for (int i = 0; i < n; i++)
	{
		const Element *e = HitElement(rays[i]);
		if (e == nullptr) continue;
		if (menuUp) return false;                 // the death wait takes no clicks (game_core.c:986-1000)
		// The main hand (or the view) is the primary button, the off hand the
		// secondary: the original needs both on the inventory.
		const int button = forcedButton ? forcedButton : (i == 0 ? InventoryScreen::PRIMARY : InventoryScreen::SECONDARY);
		if (e->kind == E_DLGPANEL) return ClickDialogue(&rays[i], e);
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
		if (U.menus->Current() != RealmsMenus::DEATH)   // the death wait has no pointer (game_core.c:986-1000)
			U.menus->PointerAt(n > 0 ? MenuHit(rays[0], e) : 0);   // every frame, as the loop does (:1591-1607)
		U.hoverBb = bb;
		return;
	}
	if (U.popup.Active())
	{
		// The popup's own pointer: its frames read it (handle_cursor_click's hover).
		// A VR hand that is the pointer sets it itself (InterfaceHandPointer).
		if (!U.handsDrivePointer) PopupPointerAt(rays, n, nullptr);
		U.hoverBb = bb;
		return;
	}
	// The dialogue's hover (game_core.c:556-569; dialogue_ui.c:231-271): the
	// pointer's position and the row the original's rectangles put it on.
	int pos = 0, row = -1;
	if (n > 0) ChoicePointer(rays[0], e, pos, row);
	U.dialogue.SetPointer(pos, row);
	// THE INVENTORY SCREEN's mouse is handle_cursor_click's (game_core.c:883-885):
	// its hover (roth_invmouse.h) -- the cursor cell follows the pointer over the
	// grid, and while a line is busy it is the dialogue's choice highlight -- every
	// frame, wherever the pointer is. Not while a VR hand is the pointer
	// (roth_vrpick.cpp). Outside the inventory, the game frame's own (game_core.c:556-569).
	if (U.screen.IsOpen() && !U.screen.PopupUp())
	{
		if (n > 0 && !U.handsDrivePointer)
		{
			int px = PANEL_NOWHERE, py = PANEL_NOWHERE;
			InventoryPointerAt(rays[0], px, py);
			U.mouse.Hover(U.painter, U.screen, px, py);
		}
	}
	else if (U.dialogue.PointerBelongsToDialogue())
		U.dialogue.UpdateChoiceHighlight(pos, row);
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

void InterfaceHudWeaponView(int arg) { if (U.hudBound) U.hud.RenderWeaponView(arg, HudWeaponNow()); }
void InterfaceHudWeaponPanel() { if (U.hudBound) U.hud.RedrawWeaponHudPanel(HudWeaponNow()); }
void InterfaceHudRenderWeaponHud() { if (U.hudBound) HudRenderWeaponHud(); }

void InterfaceAttach(GameCore &core, Dbase100 &db, const Install &install)
{
	U.core = &core;
	U.db = &db;
	U.dialogue.Bind(&core.Interp(), &db);
	U.screen.Bind(&core, &U.dialogue, &db);
	U.popup.Bind(&U.screen, &U.dialogue, &core);
	U.popup.painter.dbase300 = [](uint32_t res) { return Dbase300Resource(res); };
	U.popup.painter.screenPitch = 640;                      // the retail ROTH.INI's VideoMode 8
	U.popup.painter.screenHeight = 480;
	U.popup.painter.anchorY = 480 >> 1;                     // the panel anchor (compute_viewport_half_extents)
	U.popup.textUi = [](int scale) { U.dialogue.TickTextUi(scale); };   // render_text_ui's sequencer, in the popup's frames
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
	U.mouse.openOptions = [] { if (U.menus != nullptr) U.menus->OpenOptions(); };   // action 0x2c (inventory.c:1799-1817)
	// The dialogue under the mouse: a busy line takes the click (dialogue_ui.c:552-553). The
	// choice rows are where the choice picture stands on the original's screen (ChoiceRowAt).
	U.mouseDlg.reset(new InvMouseDialogueUi(U.dialogue, core.Interp().State(),
		[](int sx, int sy) { return U.dialogue.MenuUp() ? U.choicePainter.ChoiceRowAt(sx, sy) : -1; }));
	U.mouse.dlg = U.mouseDlg.get();
	Printf("[roth_ui] inventory panel: ICONS.ALL %s, font %s\n", U.iconsAll.empty() ? "MISSING" : "ok",
		U.font.ok() ? "ok" : exeErr.c_str());
	{
		HudAssets ha;
		ha.ui = U.painter.Assets();
		ha.dbase200 = &U.dbase200;
		ha.db = &db;
		U.hud.Bind(ha);
		U.hudBound = true;
		U.hudLevel = nullptr;
	}
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
		U.hud.SetPackTables(U.shade.data(), U.blend.data());
		// What a shade row does to the view, in RGB, for the HUD's shadow over
		// the engine's view: the brightness each row keeps, averaged over the
		// palette's colours (on the retail packs a row keeps a fixed fraction,
		// within 4 levels a channel; the blend table is a 50/50 mix, within 9).
		for (int r = 0; r < 32; r++)
		{
			double sum = 0.0; int n = 0;
			for (int d = 1; d < 256; d++)
			{
				const Colour &D = (*pal)[(size_t)d], &B = (*pal)[(size_t)U.shade[((size_t)r << 8) | (size_t)d]];
				const double ld = (double)D.r + D.g + D.b, lb = (double)B.r + B.g + B.b;
				if (ld > 30.0) { sum += lb / ld; n++; }
			}
			const double keep = n > 0 ? sum / n : 1.0;
			const double a = (1.0 - (keep > 1.0 ? 1.0 : keep)) * 255.0 + 0.5;
			U.hudBehind[r] = (uint8_t)(a < 0 ? 0 : a > 255 ? 255 : a);
		}
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
	if (U.lowCue) { U.lowCue = false; SetLowHealthEdge(false, 0.0); }
	U.mountHealthBb = Ui::MountPiece();
	U.mountLineBb = Ui::MountPiece();
	// The level's billboard array dies with it.
	U.elements.clear();
	U.signature.clear();
	U.anchored = false;
	U.fly.active = false;
	U.level = nullptr;
	U.hoverBb = 0;
}

static void RunQueued(player_t *player);

// The popup's frame tick: the original's 70 Hz g_frame_tick_counter.
static uint16_t PopupTick()
{
	return (uint16_t)(U.level != nullptr ? U.level->maptime * 2 : 0);
}

// load_dbase300_resource_at_offset's loops (file_config.c:900-1144), run by
// roth_popupdraw: the film one GDV timer tick at a time (60 Hz, the native
// timer's rate: traps.c's measurement), then the modal loop one pass a tic.
// The keys reach it as they come (KeyVerb).
static void TickPopup(FLevelLocals *level)
{
	(void)level;
	InspectPopupView &v = U.popup;
	const uint16_t tick = PopupTick();
	if (U.screen.PopupUp() && !v.Active())
	{
		// The popup the rules just opened: the film's bytes at the item's
		// close-up offset (inventory.c:1827-1834; +4 past the size dword),
		// its icon, the inventory label as it stands.
		const DbItem *rec = U.db != nullptr ? U.db->Item(U.screen.PopupItem()) : nullptr;
		U.popupFilm = rec != nullptr ? Dbase300Resource(rec->closeup) : std::vector<uint8_t>();
		const uint8_t *icon = nullptr; size_t iconSize = 0;
		if (U.db != nullptr) ItemIconPicture(*U.db, U.dbase200, U.screen.PopupItem(), &icon, &iconSize);
		v.painter.SetAssets(U.painter.Assets());
		U.popupIsr = 0.0;
		if (!v.Begin(U.popupFilm.empty() ? nullptr : U.popupFilm.data(), U.popupFilm.size(), icon, iconSize, U.painter.g_label, sizeof U.painter.g_label, tick))
		{
			for (const std::string &pr : v.painter.Problems()) Printf(TEXTCOLOR_RED "[roth_ui] popup: %s\n", pr.c_str());
			if (U.screen.PopupUp()) U.screen.PopupFinish();      // the original's popup returns at once (file_config.c:920, 958)
			return;
		}
		Printf("[roth_ui] popup: item %d, film %dx%d, %s, %d page(s)\n", U.screen.PopupItem(), v.film.Width(), v.film.Height(),
			v.film.Looping() ? "looping" : "once", v.painter.PageCount());
	}
	if (v.InFilm())
	{
		U.popupIsr += 60.0 / TICRATE;
		while (U.popupIsr >= 1.0 && v.InFilm())
		{
			U.popupIsr -= 1.0;
			if (!v.FilmBoundary(tick)) { v.FinishFilm(true, tick); break; }
			if (v.FilmRanOut()) v.FinishFilm(false, tick);
		}
	}
	else if (v.InModal())
	{
		v.ModalPassBegin(tick);
		v.ModalPassEnd();
	}
}

// A key the popup reads (gdv_inspect_frame_callback's, gdv_cutscene.c:1631-1655,
// or the modal loop's drain, file_config.c:1066-1119), as a scancode.
static void PopupKey(uint8_t sc)
{
	InspectPopupView &v = U.popup;
	if (v.InFilm())
	{
		if (!v.KeyWanted()) return;
		if (!v.FilmKey(sc)) v.FinishFilm(true, PopupTick());
	}
	else if (v.InModal())
	{
		v.ModalKey(sc);
		v.ModalPassEnd();
	}
}

void InterfaceTick(FLevelLocals *level)
{
	if (!U.attached || level == nullptr) return;
	if (U.level != level) { U.level = level; U.elements.clear(); U.signature.clear(); U.anchored = false; }
	player_t *player = level->GetConsolePlayer();
	RunQueued(player);
	// One tic is two of the original's 70 Hz frame ticks.
	U.dialogue.PollVoice();
	// While the popup is up the original's game loop does not run (the popup
	// blocks inside update_inventory_screen): its own frames pump the voice
	// and run render_text_ui (TickPopup).
	if (!U.popup.Active()) U.dialogue.Frame(2);          // voice_stream_pump + render_text_ui
	// redraw_inventory_cursor_cell (game_core.c:886), before the keys' frame
	// (:894), at the original's 70 Hz frame tick.
	if (U.screen.IsOpen() && !U.popup.Active()) U.painter.RedrawInventoryCursorCell((uint16_t)(level->maptime * 2));
	if (!U.popup.Active()) U.screen.Post();              // update_inventory_screen's tail
	TickPopup(level);                                    // the popup's own loops
	// tick_item_pickup_lock and the fly-in it times: gameplay_frame_step's
	// (game_core.c:489), which the inventory screen's mode 3 does not run.
	if (!U.screen.IsOpen() && !U.screen.PopupUp())
	{
		U.core->Items().TickPickupLock(2);
		TickFly(player);
	}
	UpdateHover(player);
	TickMenuPictures();
	TickHud(level);
	DeriveMountCanvases();
	U.mounted = MountWanted();
	Rebuild(player);
	UpdatePictureBillboards();
	TickMount();
	TickLowHealthCue();
}

bool InterfaceUse(player_t *player)
{
	if (!U.attached) return false;
	return Press(player, 0);
}

const char *InterfacePointerOwner()
{
	if (!U.attached) return nullptr;
	if (U.menus != nullptr && U.menus->IsOpen()) return "menu";
	if (U.popup.Active() || U.screen.PopupUp()) return "popup";
	if (U.screen.IsOpen()) return "inventory";
	if (U.dialogue.PointerBelongsToDialogue()) return "dialogue";
	return nullptr;
}

bool InterfacePointerBusy() { return InterfacePointerOwner() != nullptr; }

//==========================================================================
//
// The VR hands on the panels (roth_vrpick.cpp; roth_invhand.h). The hand is
// the original's mouse: where it points is a pixel of the panel's picture,
// and its clicks go through the same code as every other pointer's.
//
//==========================================================================

namespace
{
const Element *HandPanelElement(int &which)
{
	which = HANDPANEL_NONE;
	const char *owner = InterfacePointerOwner();
	if (owner == nullptr) return nullptr;
	int kind = -1;
	if (!strcmp(owner, "inventory")) { kind = E_INVPANEL; which = HANDPANEL_INVENTORY; }
	else if (!strcmp(owner, "popup") && U.popup.Active()) { kind = E_POPUP; which = HANDPANEL_POPUP; }
	if (kind < 0) { which = HANDPANEL_NONE; return nullptr; }
	for (const Element &e : U.elements) if (e.kind == kind && e.picture >= 0) return &e;
	which = HANDPANEL_NONE;
	return nullptr;
}
} // namespace

int InterfaceHandPanel(const PanelV3 &eye, PanelPlane &out)
{
	if (!U.attached) return HANDPANEL_NONE;
	int which = HANDPANEL_NONE;
	const Element *e = HandPanelElement(which);
	if (e == nullptr || !PlaneOf(*e, DVector3(eye.x, eye.y, eye.z), out)) return HANDPANEL_NONE;
	return which;
}

uint16_t InterfaceHandPointer(int panel, int x, int y, bool onPicture)
{
	if (!U.attached) return 0;
	if (panel == HANDPANEL_INVENTORY && U.screen.IsOpen() && !U.screen.PopupUp())
		return U.mouse.Hover(U.painter, U.screen, x, y);
	if (panel == HANDPANEL_POPUP && U.popup.Active())
	{
		// As PopupPointerAt: off the popup's picture the pointer is on none of its rectangles.
		if (onPicture) U.popup.SetPointer(x, y, 0);
		else U.popup.SetPointer(-0x4000, -0x4000, 0);
	}
	return 0;
}

int InterfaceHandClick(int panel, int x, int y, bool onPicture, int button)
{
	if (!U.attached) return -1;
	if (panel == HANDPANEL_INVENTORY && U.screen.IsOpen() && !U.screen.PopupUp())
	{
		PanelClick(x, y, button);
		return U.mouse.lastAction;
	}
	if (panel == HANDPANEL_POPUP && U.popup.Active())
	{
		// As Press does for the popup (input.c:1310-1347, taken by its next frame).
		InterfaceHandPointer(panel, x, y, onPicture);
		U.popup.Press(button);
		return -1;
	}
	return -1;
}

void InterfaceHandsDrivePointer(bool on) { U.handsDrivePointer = on; }

bool InterfaceHandDialogueClick(const PanelV3 &start, const PanelV3 &dir)
{
	if (!U.attached || !U.dialogue.PointerBelongsToDialogue()) return false;
	Ray r;
	r.start = DVector3(start.x, start.y, start.z);
	r.dir = DVector3(dir.x, dir.y, dir.z);
	return ClickDialogue(&r, HitElement(r));
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
	if (U.popup.Active())                                   // the popup's keys, as scancodes
	{
		if (verb == "up") PopupKey(0x48);
		else if (verb == "down") PopupKey(0x50);
		else if (verb == "left") PopupKey(0x4b);
		else if (verb == "right") PopupKey(0x4d);
		else if (verb == "enter") PopupKey(0x1c);
		else if (verb == "skip") PopupKey(0x01);                // Esc
		else if (verb == "close") PopupKey(0x17);               // I
		else if (verb == "examine") PopupKey(0x39);             // Space
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
		else if (verb == "hold") U.mouse.KeyClick(s, false);    // a click with code 8: a busy line is skipped
		else if (verb == "use") U.mouse.KeyClick(s, true);
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
	if (U.screen.PopupUp())
	{
		const InspectPopupPainter &pp = U.popup.painter;
		Printf("[roth_ui] popup: item %d, %d page(s) (page %d), %d topic(s), %s, state %d, window %d,%d %dx%d, film %d decode(s), picture version %u\n",
			U.screen.PopupItem(), pp.PageCount(), U.screen.PopupPage(), U.dialogue.TopicCount(),
			U.popup.InFilm() ? "film" : U.popup.InModal() ? "modal" : "not running", U.popup.State(), pp.WinX(), pp.WinY(), pp.WinW(), pp.WinH(),
			U.popup.film.Decodes(), pp.Presented().Version());
		for (const std::string &pr : pp.Problems()) Printf("[roth_ui] popup picture: %s\n", pr.c_str());
	}
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

//===========================================================================
//
// roth_mounttest -- THE FLAT TEST of the off-hand mount. Test-only: inert
// until typed. With roth_handtest's hands up, roth_handtest's off hand stands
// in for the controller: the subtitle line and the health panel go onto the
// mount over its wrist and the low-health edge shows below half, as in VR,
// so rothdiff_shot can photograph them in flat.
//   roth_mounttest on | off
//
//===========================================================================

CCMD(roth_mounttest)
{
	if (argv.argc() < 2) { Printf("roth_mounttest on | off\n"); return; }
	roth::U.mountTest = !stricmp(argv[1], "on");
	Printf("roth_mounttest: %s%s\n", roth::U.mountTest ? "on" : "off", roth::U.mountTest ? " (needs roth_handtest)" : "");
}
