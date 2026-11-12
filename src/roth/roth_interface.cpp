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
#include "roth_gamecore.h"
#include "roth_dialogue.h"
#include "roth_invscreen.h"
#include "roth_icons.h"
#include "roth_install.h"
#include "roth_texture.h"
#include "roth_das.h"
#include "roth_objects.h"
#include "roth_sound.h"
#include "roth_raw.h"

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

	void UnequipWeapon() override { notBuilt["weapon unequip"]++; }
	void EquipWeapon(int slot, int32_t itemId) override;
	void RebuildWeaponList() override { notBuilt["weapon list"]++; }
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
	E_NONE, E_BACKING, E_CHOICE, E_CELL, E_TAB, E_HAND, E_SCROLLUP, E_SCROLLDOWN, E_CLOSE,
	E_POP_MAG, E_POP_INFO, E_POP_TOPIC, E_POP_CLOSE, E_POP_PGUP, E_POP_PGDN, E_POP_BACKING,
};

struct Element
{
	int bb = 0;             // billboard handle
	int kind = E_NONE;
	int arg = 0;
	bool hittable = false;
};

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
};

Ui U;

void RuntimeInvSink::EquipWeapon(int slot, int32_t itemId)
{
	(void)slot; (void)itemId;
	notBuilt["weapon equip"]++;     // activate_weapon_item: the weapon system (M6)
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
	bb.flags = BBFL_PERSISTENT | (hittable ? 0 : BBFL_NOHIT);
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
	const char *text, double alpha)
{
	if (U.level == nullptr) return 0;
	FBillboard bb;
	bb.id = U.level->NextBillboardID++;
	bb.pos = DVector3(ahead, rightOff, up);
	bb.width = w;
	bb.height = h;
	bb.facing = BBF_FIXED;
	bb.payload = payload;
	bb.color = col;
	bb.alpha = alpha;
	bb.flags = BBFL_PERSISTENT | BBFL_VIEWLOCKED | BBFL_NODEPTH | BBFL_NOHIT;
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
		if (best != nullptr && fabs(t - bestT) < 0.5 && (e.kind == E_BACKING || e.kind == E_POP_BACKING)) continue;
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
	if (U.screen.PopupUp())
	{
		snprintf(buf, sizeof buf, "P%d/%d/%d/%d|", U.screen.PopupItem(), U.screen.PopupPage(), U.screen.PopupTopicsShown() ? 1 : 0,
			U.dialogue.TopicCount());
		s += buf;
		for (const TextLine &l : U.dialogue.TopicLines()) { s.append((const char *)l.text.data(), l.text.size()); }
	}
	return s;
}

void BuildDialogue()
{
	const DialogueView v = U.dialogue.View();
	if (v.kind == DialogueView::LINE)
	{
		// The original's subtitle sits under the view, centred
		// (render_text_ui, dialogue_ui.c:1021-1028). Head-locked here so it
		// is read wherever the player looks; not hittable -- a click anywhere
		// is what dismisses it (game_core.c:612-632).
		const int n = (int)v.rows.size();
		double widest = 0;
		for (const std::string &r : v.rows) widest = std::max(widest, (double)TextPixels(r));
		const double up0 = -9.0;
		AddViewLocked(DIST, 0, up0 - (n - 1) * ROWH * 0.65, widest * PX + 2.0, n * ROWH * 1.3 + 1.0, BB_PANEL, PalEntry(255, 0, 0, 0), nullptr, 0.55);
		for (int i = 0; i < n; i++)
		{
			const double w = TextPixels(v.rows[(size_t)i]) * PX;
			if (w <= 0) continue;
			AddViewLocked(DIST - 0.2, 0, up0 - i * ROWH * 1.3, w, ROWH, BB_TEXT, Color(v.color), v.rows[(size_t)i].c_str(), 1.0);
		}
		return;
	}
	if (v.kind != DialogueView::CHOICES) return;
	// The choice menu: centred (render_dialogue_text_panel, dialogue_ui.c:814-850),
	// one row per choice, the highlighted one in g_map_menu_marker_selected.
	const std::vector<TextLine> &seg = U.dialogue.Segments();
	double total = 0, widest = 0;
	for (size_t i = 0; i < v.rows.size() && i < seg.size(); i++)
	{
		total += seg[i].height + 4;
		widest = std::max(widest, (double)(seg[i].x1 - seg[i].x0));
	}
	total = (total - 4) * PX;
	Add(0, 0, 0, widest * PX + 3.0, total + 3.0, BB_PANEL, 0, PalEntry(255, 0, 0, 0), nullptr, 0.7, E_BACKING, 0, true);
	double z = total / 2;
	for (size_t i = 0; i < v.rows.size() && i < seg.size(); i++)
	{
		const std::vector<std::string> lines = seg[i].Rows();
		const double h = seg[i].height * PX;
		const bool lit = (int)i == v.highlighted;
		const PalEntry col = Color(lit ? v.highlightColor : v.color);
		// The row is ONE hittable quad (the record's rectangle,
		// dialogue_ui.c:255-266); its wrapped lines are drawn on it.
		Add(0, z - h / 2, 0.1, (seg[i].x1 - seg[i].x0) * PX + 1.0, h + 0.3, BB_PANEL, 0,
			lit ? PalEntry(255, 60, 60, 40) : PalEntry(255, 20, 20, 20), nullptr, lit ? 0.6 : 0.01, E_CHOICE, (int)i, true);
		for (size_t k = 0; k < lines.size(); k++)
		{
			const double w = TextPixels(lines[k]) * PX;
			if (w > 0) Add(0, z - (k + 0.5) * ROWH, 0.2, w, ROWH, BB_TEXT, 0, col, lines[k].c_str(), 1.0, E_NONE, 0, false);
		}
		z -= h + 4 * PX;
	}
}

void BuildTimedMessage()
{
	if (U.dialogue.TimedMessageLineCount() == 0) return;
	// render_active_timed_message (dialogue_ui.c:900-968): at the top of the view.
	double up = 10.0;
	for (const TextLine &l : U.dialogue.TimedMessageLines())
		for (const std::string &r : l.Rows())
		{
			const double w = TextPixels(r) * PX;
			if (w > 0) AddViewLocked(DIST, 0, up, w, ROWH, BB_TEXT, Color(l.Color()), r.c_str(), 1.0);
			up -= ROWH * 1.3;
		}
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

void BuildInventory()
{
	if (!U.screen.IsOpen() || U.core == nullptr) return;
	const InventoryEntryList &l = U.screen.Entries();
	const Inventory &inv = U.core->Items();
	// The original's panel is 0x120 x 0x50 pixels (inventory.c:1872-1877):
	// two hand boxes on the left, the tab strip, five-by-two cells, the label.
	const double W = 0x120 * PX * 1.4, H = 0x50 * PX * 1.6;
	Add(0, 0, 0, W, H, BB_PANEL, 0, PalEntry(255, 10, 10, 14), nullptr, 0.8, E_BACKING, 0, true);
	const double cell = 0x1c * PX * 1.4;
	const int list = U.screen.ActiveList();
	// Tabs: the five categories, in the original's order (0x7123c).
	static const char *tabName[5] = { "Items", "Weapons", "Clothes", "Documents", "Characters" };
	for (int t = 0; t < 5; t++)
	{
		const bool lit = TabCategory(t) == list;
		const double x = -W / 2 + cell * 1.8 + t * (W - cell * 2.2) / 5 + (W - cell * 2.2) / 10;
		Add(x, H / 2 - 1.6, 0.1, (W - cell * 2.4) / 5, 2.4, BB_PANEL, 0, lit ? PalEntry(255, 120, 90, 40) : PalEntry(255, 40, 40, 40),
			nullptr, 0.9, E_TAB, t, true);
		Add(x, H / 2 - 1.6, 0.2, (W - cell * 2.4) / 5 * 0.9, 1.4, BB_TEXT, 0, PalEntry(255, 230, 220, 200), tabName[t], 1.0, E_NONE, 0, false);
	}
	// The hand boxes (actions 0x2a / 0x2b): left = the equipped weapon,
	// right = the selected item.
	for (int hnd = 0; hnd < 2; hnd++)
	{
		const int slot = hnd ? l.shownRight : l.shownLeft;
		const double x = -W / 2 + cell * 0.8, z = H / 2 - 4.5 - hnd * (cell + 1.0);
		Add(x, z, 0.1, cell, cell, BB_PANEL, 0, PalEntry(255, 50, 45, 40), nullptr, 0.9, E_HAND, hnd, true);
		if (slot >= 0)
		{
			FTextureID tex = IconTexture((int16_t)inv.Slot(slot).id);
			if (tex.isValid()) Add(x, z, 0.2, cell * 0.85, cell * 0.85, BB_TEXTURE, tex.GetIndex(), PalEntry(255, 255, 255, 255), nullptr, 1.0, E_NONE, 0, false);
		}
	}
	// The grid: ten cells from the scroll offset.
	const int held = U.screen.HeldEntry();
	const int cursor = U.screen.CursorEntry();
	for (int c = 0; c < InventoryScreen::CELLS; c++)
	{
		const int entry = U.screen.ScrollOffset() + c;
		const double x = -W / 2 + cell * 2.4 + (c % 5) * (cell + 0.6) + cell / 2;
		const double z = H / 2 - 4.5 - (c / 5) * (cell + 0.6) - cell / 2 + 1.6;
		// The cell colours of redraw_inventory_cursor_cell (inventory.c:1625-1630):
		// held = the marker colour, left hand = message colour 0, right hand = 5.
		PalEntry back = PalEntry(255, 30, 30, 30);
		if (entry == cursor) back = PalEntry(255, 70, 70, 50);
		if (entry == l.leftEntry) back = Color(U.dialogue.defaultMessageColor[0]);
		if (entry == l.rightEntry) back = Color(U.dialogue.defaultMessageColor[5]);
		if (entry == held) back = Color(U.dialogue.markerSelected);
		Add(x, z, 0.1, cell, cell, BB_PANEL, 0, back, nullptr, 0.9, E_CELL, c, true);
		if (entry < U.screen.EntryCount())
		{
			FTextureID tex = IconTexture((int16_t)inv.Slot(l.slots[(size_t)entry]).id);
			if (tex.isValid()) Add(x, z, 0.2, cell * 0.85, cell * 0.85, BB_TEXTURE, tex.GetIndex(), PalEntry(255, 255, 255, 255), nullptr,
				entry == held ? 0.5 : 1.0, E_NONE, 0, false);
		}
	}
	// Scroll arrows (actions 0x28 / 0x29) and the label of the cursor's item.
	Add(W / 2 - 1.5, H / 2 - 5, 0.1, 2.4, 2.4, BB_TEXT, 0, PalEntry(255, 230, 220, 200), "^", 1.0, E_SCROLLUP, 0, true);
	Add(W / 2 - 1.5, -H / 2 + 5, 0.1, 2.4, 2.4, BB_TEXT, 0, PalEntry(255, 230, 220, 200), "v", 1.0, E_SCROLLDOWN, 0, true);
	std::string label;
	if (cursor >= 0 && cursor < U.screen.EntryCount()) label = ItemLabel(l.slots[(size_t)cursor]);
	if (!label.empty())
		Add(0, -H / 2 + 1.6, 0.2, TextPixels(label) * PX * 1.2, ROWH * 1.2, BB_TEXT, 0, Color(U.dialogue.markerSelected), label.c_str(), 1.0, E_NONE, 0, false);
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

void Rebuild(player_t *player)
{
	const std::string sig = Signature();
	if (sig == U.signature) return;
	const bool worldPanel = U.dialogue.MenuUp() || U.screen.IsOpen();
	if (!worldPanel) U.anchored = false;
	else if (!U.anchored) Anchor(player);
	ClearBillboards();
	U.signature = sig;
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
	switch (e.kind)
	{
	case E_CHOICE:
		U.dialogue.PointerClick(e.bb, e.arg);
		Printf("[roth_ui] choice %d picked\n", e.arg);
		break;
	case E_CELL: U.screen.ClickCell(e.arg, button); break;
	case E_TAB: U.screen.ClickTab(e.arg); break;
	case E_HAND: U.screen.ClickHandBox(e.arg, button); break;
	case E_SCROLLUP: U.screen.ScrollUp(); break;
	case E_SCROLLDOWN: U.screen.ScrollDown(); break;
	case E_POP_MAG: U.screen.PopupMagnifier(); break;
	case E_POP_INFO: U.screen.PopupToggleTopics(); break;
	case E_POP_TOPIC: U.screen.PopupTopic(e.arg); break;
	case E_POP_CLOSE: U.screen.PopupClose(); break;
	case E_POP_PGUP: U.screen.PopupPageUp(); break;
	case E_POP_PGDN: U.screen.PopupPageDown(); break;
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

// game_core.c:607-632: a click while the dialogue owns the pointer.
bool ClickDialogue(const Element *e)
{
	if (!U.dialogue.PointerBelongsToDialogue()) return false;
	if (e != nullptr && e->kind == E_CHOICE) { ClickElement(*e, InventoryScreen::PRIMARY); return true; }
	U.dialogue.PointerClick(e != nullptr ? e->bb : 0, -1);
	return true;
}

bool Press(player_t *player, int forcedButton)
{
	if (!U.attached || player == nullptr) return false;
	Ray rays[2];
	int n = 0;
	if (!PointerRays(player, rays, n)) return false;
	for (int i = 0; i < n; i++)
	{
		const Element *e = HitElement(rays[i]);
		if (e == nullptr) continue;
		// The main hand (or the view) is the primary button, the off hand the
		// secondary: the original needs both on the inventory.
		const int button = forcedButton ? forcedButton : (i == 0 ? InventoryScreen::PRIMARY : InventoryScreen::SECONDARY);
		if (U.screen.PopupUp() && e->kind < E_POP_MAG) continue;    // only the popup takes clicks while it is up
		if (e->kind == E_CHOICE) return ClickDialogue(e);
		if (e->kind == E_BACKING || e->kind == E_POP_BACKING) return true;
		ClickElement(*e, button);
		return true;
	}
	if (ClickDialogue(nullptr)) return true;
	return ClickNothing();
}

void UpdateHover(player_t *player)
{
	Ray rays[2];
	int n = 0;
	const Element *e = nullptr;
	if (PointerRays(player, rays, n) && n > 0) e = HitElement(rays[0]);
	const int bb = e ? e->bb : 0;
	// The dialogue's hover (game_core.c:556-569; dialogue_ui.c:231-271): the
	// "position" is the element under the pointer, so a keyboard selection
	// survives until the pointer moves to another row.
	U.dialogue.SetPointer(bb, (e && e->kind == E_CHOICE) ? e->arg : -1);
	if (U.dialogue.PointerBelongsToDialogue())
		U.dialogue.UpdateChoiceHighlight(bb, (e && e->kind == E_CHOICE) ? e->arg : -1);
	// The inventory cursor follows the pointer over the grid (input.c:1290-1297).
	if (bb != U.hoverBb && e != nullptr && e->kind == E_CELL && !U.screen.PopupUp()) U.screen.HoverCell(e->arg);
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
	bb.flags = BBFL_PERSISTENT | BBFL_NOHIT;
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
	U.dialogue.speechAvailable = SoundSpeechAvailable();          // audio.c:1210
	U.dialogue.screenPitch = 640;           // the retail ROTH.INI's VideoMode 8
	U.attached = true;
	Printf("[roth_ui] attached: text %s, icons %s, documents %s, speech %s\n",
		U.dialogue.text.empty() ? "MISSING" : "ok", U.dbase200.empty() ? "MISSING" : "ok",
		U.dbase300Path.empty() ? "MISSING" : "ok", U.dialogue.speechAvailable ? "ok" : "MISSING");
}

InventorySink *InterfaceInventorySink() { return &U.invSink; }

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
	U.screen.Post();                                     // update_inventory_screen's tail
	if (U.screen.PopupUp()) U.screen.PopupFrame();       // the popup's own loop
	U.core->Items().TickPickupLock(2);                   // tick_item_pickup_lock
	TickFly(player);
	UpdateHover(player);
	Rebuild(player);
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

} // namespace roth
