// roth_held.cpp -- the held objects in the engine. See roth_held.h (what runs where) and roth_heldlogic.h (the rules).
//
// WHAT IS DRAWN, AND FROM WHAT (the owner's rule: the game is read from the native install):
//   the item in the item hand, the Lamp, the Book, what drops into the palm
//                  each item's own inventory picture, DBASE200 (load_item_icon_resource, inventory.c:973-996, through
//                  InterfaceItemIcon), decoded at its stored rows and drawn on a card facing the eye, in the level
//                  pack's palette (ActorsLoosePicture). Its height is the stored rows x 0.6: the icons are stored at
//                  twice the 320x200 screen's rows (DrawItemIconInSlot centres 0x38 rows in a 0x1c cell) and that
//                  screen's pixels are 1.2 times as tall as wide. The card's size is the object's own, from its
//                  dossier where one gives it (kRealSize).
//   the match      the original has no match in hand and no picture of one (ITEM_DESIGN 16: the matchbox's film shows
//                  the box; nothing in the packs is a match): a minimal procedural stick -- six small round brown
//                  dots from the end to the head -- and a flame -- one round warm dot with a lighter core, its size
//                  and brightness flickering -- the VR addition the owner approved (Q1). And a small light.
//   the lights     EffectLights frame lights (hw_effectlights.h), as the candles': the match (warm, short), the lamp
//                  (warm, around the player, only where the original shows its lantern light: a sector with the
//                  lantern bit, the engine's Colormap.Desaturation bit 0, SyncSectorLight), the niche (following
//                  light group 46's byte). Numbers are for the headset to judge.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------

#include "roth_held.h"
#include "roth_heldlogic.h"
#include "roth_hands.h"
#include "roth_handhold.h"
#include "roth_interface.h"
#include "roth_inventory.h"
#include "roth_actors_engine.h"
#include "roth_candles.h"
#include "roth_runtime.h"
#include "roth_vrpick.h"
#include "roth_uidraw.h"
#include "roth_raw.h"
#include "roth_dbase100.h"
#include "roth_panelmap.h"
#include "roth_objects.h"
#include "roth_movie.h"

#include "actor.h"
#include "actorinlines.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "printf.h"
#include "hw_vrmodes.h"
#include "hw_effectlights.h"
#include "r_utility.h"
#include "r_data/models.h"
#include "r_data/vr_handpose.h"
#include "r_data/vr_handpose_math.h"

#include <math.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

EXTERN_CVAR(Int, vr_mode)   // file scope, never inside a namespace
extern bool resetDoomYaw;   // the VR view's "take the player's yaw again" (QzDoom/qzdoom_common.cpp); file scope

namespace roth
{

namespace
{

using held::V3;

constexpr int kMatchbox = 16;   // the item the match is struck on (the Matchbox, ITEM_DESIGN 16)
constexpr int kLampItem = 15;   // the Lamp (ITEM_DESIGN 15)

// The objects' own sizes for their cards, metres, the longest side (the dossiers: ITEM_DESIGN 15 "30 cm tall",
// 16 "5.5 x 3.6 x 1.6 cm"; the Book's world sprite is drawn 0.39 m: a novel is about 0.20 m). Others: 0.10 m.
struct RealSize { int item; double metres; };
constexpr RealSize kRealSize[] = { { 15, 0.30 }, { 16, 0.055 }, { 17, 0.20 }, { 20, 0.06 }, { 21, 0.06 } };
double CardMetres(int item)
{
	for (const RealSize &r : kRealSize) if (r.item == item) return r.metres;
	return 0.10;
}

// THE FOLLOWED LIGHT GROUPS (owner Q10): STUDY1's niche, light group 46 (the sectors whose command id is 46: s58),
// ramped by the tall candles' chain (#9 -> #430 +40, #428 wait 310, #431 +30, #432 wait 70, #433 -30).
struct Followed { const char *map; int group; const char *what; };
constexpr Followed kFollowed[] = { { "STUDY1", 46, "the painting niche (tall candles' chain #9: #430 +40, #431 +30, #433 -30)" } };
// The niche light's colour and size (tuning numbers for the headset).
constexpr float kNicheColour[3] = { 1.0f, 0.80f, 0.55f };
constexpr double kNichePeak = 0.9, kNicheRadiusM = 1.4;
// The match: warm, short (ITEM_DESIGN 16 step 5: "radius 30 units (0.28 m), warm, short").
constexpr float kMatchColour[3] = { 1.0f, 0.62f, 0.25f };
constexpr double kMatchPeak = 0.45, kMatchRadiusM = 0.30;
// The lamp: warm, around the player (owner Q2: "it lights the area around the player from the hip").
constexpr float kLampColour[3] = { 1.0f, 0.70f, 0.38f };
constexpr double kLampPeak = 0.40, kLampRadiusM = 1.8;
constexpr float kGripClose = 0.70f, kGripOpen = 0.35f;   // the picker's (roth_vrpick.cpp)

struct Hand
{
	bool ok = false;
	DVector3 tip, base, palm, wrist, middle;   // map units
};

struct Group
{
	int group = 0;
	const char *what = "";
	std::vector<int> sectors;
	held::LightFollower follow;
	double intensity = 0.0;
};

struct HeldWorld
{
	FLevelLocals *level = nullptr;
	held::StrikeDetector strike;
	held::Match match;
	held::LampHook lamp;
	held::BookView book;
	held::EyeExamine eye;
	std::vector<Group> groups;
	std::map<int, int> iconTex;              // item -> texture index (-1: none)
	std::map<int, std::pair<int, int>> iconSize;
	bool gripClosed[2] = { false, false };
	bool live = false;
	bool lampCarried = false;
	bool lampLit = false;                    // the player stands where the original shows the lantern light
	DVector3 lampGlobe, flame;
	bool haveFlame = false;
	bool popupPlaced = false;
	int lastRight = -1;
	bool turnHeld = false;
	double turnTo = 0.0;
	int loggedLantern = -1;
	// billboards
	int card[2] = { 0, 0 }, stick[6] = {}, flameBB = 0, flameCore = 0, lampBB = 0, bookBB = 0, palmBB = 0;
	std::vector<int> shown, shownLast;
	// the flat test's scripted motion (roth_heldtest)
	int scriptStroke = 0;                    // tics left in a scripted stroke
	double strokeStep[3] = { 0, 0, 0 };      // camera right / up / ahead metres a tic
	int strokeWait = 0;
};
HeldWorld H;

double UPM() { return VRPickUnitsPerMetre(); }
double PS() { return H.level != nullptr && H.level->pixelstretch > 0 ? H.level->pixelstretch : 1.0; }
V3 M(const DVector3 &w) { const double u = UPM(); return V3(w.X / u, w.Y / u, w.Z * PS() / u); }
DVector3 W(const V3 &m) { const double u = UPM(); return DVector3(m.x * u, m.y * u, m.z * u / PS()); }

bool HeadsetOn()
{
	const VRMode *vm = VRMode::GetVRModeCached(true);
	return vr_mode != 0 && vm != nullptr && vm->IsVR();
}

void Haptic(int role, double amp, double ms) { if (HeadsetOn()) VR_ScriptHaptic(role, amp, ms); }

int ItemRole() { return VRHandPose_RoleForSide(VRHandPose::SIDE_RIGHT); }
int OffRole() { return 1 - ItemRole(); }
const char *RoleWord(int role) { return role == VR_MAINHAND ? "main" : "off"; }

bool ReadHand(int role, Hand &h)
{
	h.ok = false;
	const int side = VRHandPose_SideForRole(role);
	AActor *ha = HandActor(side);
	FModel *model = ha != nullptr ? Model_ActorModel(ha) : nullptr;
	FVRHandPoints pts;
	if (model == nullptr || !VRHandPose_FingerPoints(model, side, pts)) return false;
	h.ok = Model_ActorPointToWorld(ha, pts.tip, h.tip) && Model_ActorPointToWorld(ha, pts.indexBase, h.base)
		&& Model_ActorPointToWorld(ha, pts.palm, h.palm) && Model_ActorPointToWorld(ha, pts.wrist, h.wrist)
		&& Model_ActorPointToWorld(ha, pts.middleBase, h.middle);
	return h.ok;
}

// The grip of a role, with the picker's hysteresis.
bool ReadGrip(int role)
{
	float g = 0.f;
	bool valid = false;
	if (VRHandTest_Active())
	{
		FVRHandTestInputs in;
		if (VRHandTest_Inputs(in)) { g = in.grip; valid = true; }
	}
	else
	{
		const VRMode *vm = VRMode::GetVRModeCached(true);
		VSMatrix gm;
		VRFingerInput fi;
		if (vm != nullptr && vm->GetGripTransform(role, &gm)) { valid = true; if (vm->GetFingerInput(role, fi)) g = fi.grip; }
	}
	bool &c = H.gripClosed[role];
	if (!valid) c = false;
	else if (g >= kGripClose) c = true;
	else if (g <= kGripOpen) c = false;
	return c;
}

// The Matchbox in the item hand: its centre at the palm, its long axis along the hand (wrist to knuckles), its
// striking sides facing the thumb side and the little-finger side (held as one holds a box to strike it).
held::BoxFrame BoxOf(const Hand &h)
{
	held::BoxFrame b;
	b.centre = M(h.palm);
	b.along = (M(h.middle) - M(h.wrist)).Unit();
	V3 t = M(h.base) - M(h.middle);
	t = (t - b.along * t.Dot(b.along)).Unit();
	b.across = t;
	b.thick = b.along.Cross(b.across).Unit();
	return b;
}

V3 FingerDir(const Hand &h) { return (M(h.tip) - M(h.base)).Unit(); }

//--------------------------------------------------------------------------
// The pictures
//--------------------------------------------------------------------------

int IconTexture(int item, int &w, int &h)
{
	auto it = H.iconTex.find(item);
	if (it != H.iconTex.end()) { w = H.iconSize[item].first; h = H.iconSize[item].second; return it->second; }
	int tex = -1;
	w = h = 0;
	const uint8_t *img = nullptr;
	size_t size = 0;
	if (item > 0 && InterfaceItemIcon(item, &img, &size) && size >= 8)
	{
		const int iw = img[4] | (img[5] << 8), ih = img[6] | (img[7] << 8);
		const size_t p = DasPayloadAt(img);
		std::vector<uint8_t> px;
		if (iw > 0 && ih > 0 && p < size && UiDecodeRle(img + p, size - p, (size_t)iw * ih, px))
		{
			const FString name = FStringf("ROTH_HELDICON_%s_%d", H.level != nullptr ? H.level->MapName.GetChars() : "", item);
			tex = ActorsLoosePicture(name.GetChars(), px, iw, ih);
			w = iw; h = ih;
		}
	}
	if (tex < 0) Printf("[roth_held] item %d: no inventory picture in DBASE200 -- nothing drawn for it\n", item);
	H.iconTex[item] = tex;
	H.iconSize[item] = std::make_pair(w, h);
	return tex;
}

//--------------------------------------------------------------------------
// Billboards (as roth_vrpick.cpp draws its beam: hand-locked in VR, so they ride the controller between tics)
//--------------------------------------------------------------------------

FBillboard *FindBB(int id)
{
	if (H.level == nullptr || id == 0) return nullptr;
	for (FBillboard &b : H.level->Billboards) if (b.id == id) return &b;
	return nullptr;
}

FBillboard *EnsureBB(int &id)
{
	FBillboard *b = FindBB(id);
	if (b != nullptr) return b;
	if (H.level == nullptr) return nullptr;
	FBillboard bb;
	bb.id = H.level->NextBillboardID++;
	bb.facing = BBF_CAMERA;
	bb.flags = BBFL_PERSISTENT | BBFL_NOSAVE | BBFL_NOHIT;
	bb.alpha = 0.0;
	bb.spawntic = H.level->maptime;
	H.level->Billboards.Push(bb);
	id = bb.id;
	return FindBB(id);
}

struct Lock
{
	bool on = false;
	int flag = 0;
	DVector3 origin, fwd, right, up;
	DVector3 Local(const DVector3 &w) const { const DVector3 d = w - origin; return DVector3(d | fwd, d | right, d | up); }
};

Lock LockFor(int role)
{
	Lock L;
	AActor *pmo = players[consoleplayer].mo;
	if (role < 0 || pmo == nullptr || !pmo->OverrideAttackPosDir) return L;   // no VR pose published: world points
	FBillboard probe;
	probe.flags = role == VR_MAINHAND ? BBFL_HANDMAIN : BBFL_HANDOFF;
	double yaw, pitch, roll;
	if (!BillboardHandFrame(probe, L.origin, yaw, pitch, roll)) return L;
	BillboardHandAxes(yaw, pitch, roll, L.fwd, L.right, L.up);
	L.on = true;
	L.flag = probe.flags;
	return L;
}

// Show billboard `id` this tic at world point `at`, riding hand `role` (-1: the world).
void Show(int &id, int payload, int data, const DVector3 &at, double w, double h, PalEntry col, double alpha, int role)
{
	FBillboard *b = EnsureBB(id);
	if (b == nullptr) return;
	const Lock L = LockFor(role);
	b->payload = payload;
	b->data = data;
	b->flags = (b->flags & ~BBFL_HANDANY) | (L.on ? L.flag : 0);
	b->pos = L.on ? L.Local(at) : at;
	b->drawPos = b->pos;
	b->width = w;
	b->height = h;
	b->color = col;
	b->alpha = alpha;
	b->facing = BBF_CAMERA;
	H.shown.push_back(id);
}

void HideUnshown()
{
	for (int id : H.shownLast)
	{
		if (std::find(H.shown.begin(), H.shown.end(), id) != H.shown.end()) continue;
		if (FBillboard *b = FindBB(id)) b->alpha = 0.0;
	}
	H.shownLast.swap(H.shown);
	H.shown.clear();
}

// An item's card at `at`: its own picture, its own size.
void ShowCard(int &id, int item, const DVector3 &at, int role, double scale = 1.0)
{
	int w = 0, h = 0;
	const int tex = IconTexture(item, w, h);
	if (tex < 0 || w <= 0 || h <= 0) return;
	const double aspectH = h * 0.6;                                 // the stored rows at the 320x200 screen's aspect
	const double longest = CardMetres(item) * scale;
	const double mw = w >= aspectH ? longest : longest * w / aspectH;
	const double mh = w >= aspectH ? longest * aspectH / w : longest;
	Show(id, BB_TEXTURE, tex, at, mw * UPM(), mh * UPM() / PS(), PalEntry(255, 255, 255, 255), 1.0, role);
}

//--------------------------------------------------------------------------
// The inventory, read
//--------------------------------------------------------------------------

int RightItem(Inventory *inv)
{
	if (inv == nullptr || inv->RightHand() < 0) return 0;
	return (int16_t)inv->Slot(inv->RightHand()).id;
}

bool Carries(Inventory *inv, int item)
{
	if (inv == nullptr) return false;
	for (int i = 0; i < Inventory::SLOT_COUNT; i++) if ((int16_t)inv->Slot(i).id == item) return true;
	return false;
}

//--------------------------------------------------------------------------
// The flat test's scripted motion (roth_heldtest)
//--------------------------------------------------------------------------

// A world displacement as the test grip's camera frame takes it: metres right / up / ahead of the view.
void CameraLocal(const V3 &dm, double out[3])
{
	AActor *pmo = players[consoleplayer].mo;
	const double yaw = pmo != nullptr ? pmo->Angles.Yaw.Radians() : 0.0;
	const V3 fwd(cos(yaw), sin(yaw), 0.0), right(sin(yaw), -cos(yaw), 0.0);
	out[0] = dm.Dot(right); out[1] = dm.z; out[2] = dm.Dot(fwd);
}

void MoveTestHand(int role, const V3 &dm)
{
	const int side = VRHandPose_SideForRole(role);
	double f, r, u, d[3];
	VRHandTest_Offset(side, f, r, u);           // (fwd, right, up), the gestures' convention (vr_handpose.h)
	CameraLocal(dm, d);                          // d: right, up, ahead
	VRHandTest_SetOffset(side, f + d[2], r + d[0], u + d[1]);
}

void ScriptTick()
{
	if (H.strokeWait > 0) { H.strokeWait--; return; }
	if (H.scriptStroke <= 0) return;
	const int side = VRHandPose_SideForRole(OffRole());
	double f, r, u;
	VRHandTest_Offset(side, f, r, u);
	VRHandTest_SetOffset(side, f + H.strokeStep[2], r + H.strokeStep[0], u + H.strokeStep[1]);
	if (--H.scriptStroke == 0) Printf("[roth_held] test: the scripted stroke is done\n");
}

//--------------------------------------------------------------------------
// The lights' places
//--------------------------------------------------------------------------

bool GroupCentre(const Group &g, DVector3 &at)
{
	if (H.level == nullptr || g.sectors.empty()) return false;
	DVector3 sum(0, 0, 0);
	int n = 0;
	for (int s : g.sectors)
	{
		if (s < 0 || (size_t)s >= H.level->sectors.Size()) continue;
		sector_t &sec = H.level->sectors[s];
		const DVector2 c = sec.centerspot;
		const double f = sec.floorplane.ZatPoint(c), ce = sec.ceilingplane.ZatPoint(c);
		sum += DVector3(c.X, c.Y, (f + ce) * 0.5);
		n++;
	}
	if (n == 0) return false;
	at = sum / n;
	return true;
}

void Light(const DVector3 &at, const float colour[3], double intensity, double radiusUnits)
{
	if (!(intensity > 0.0) || !(radiusUnits >= 1.0)) return;
	EffectLights::FrameLight f;
	f.A = { at.X, at.Y, at.Z };
	f.B = f.A;
	for (int ch = 0; ch < 3; ch++) f.Color[ch] = (float)(colour[ch] * intensity);
	f.Radius = radiusUnits;
	f.Flags = 0;
	EffectLights::Get().AddFrameLight(f);
}

} // namespace (anonymous)

//==========================================================================
// Level
//==========================================================================

void HeldBeginLevel(const Map &map, FLevelLocals *level)
{
	HeldEndLevel();
	H.level = level;
	if (level == nullptr) return;
	for (const Followed &f : kFollowed)
	{
		if (stricmp(level->MapName.GetChars(), f.map) != 0) continue;
		Group g;
		g.group = f.group;
		g.what = f.what;
		int sum = 0;
		for (size_t s = 0; s < map.sectors.size(); s++)
			if (map.sectors[s].commandID == f.group) { g.sectors.push_back((int)s); sum += map.sectors[s].light; }
		if (g.sectors.empty()) continue;
		g.follow.Begin(sum / (int)g.sectors.size());
		Printf("[roth_held] light group %d (%s): %d sector(s), the pack's light %d; a light follows its byte\n", g.group, g.what,
			(int)g.sectors.size(), g.follow.Base());
		H.groups.push_back(g);
	}
}

void HeldEndLevel()
{
	// The billboards go with the level.
	const bool turnHeld = H.turnHeld;
	const double turnTo = H.turnTo;
	H = HeldWorld();
	H.turnHeld = turnHeld;    // a forced turn waiting for the black survives the warp's reload
	H.turnTo = turnTo;
}

//==========================================================================
// Each tic
//==========================================================================

void HeldTick(FLevelLocals *level, bool worldTicks)
{
	if (level == nullptr) return;
	if (H.level != level) { HeldEndLevel(); H.level = level; }
	player_t *player = level->GetConsolePlayer();
	AActor *pmo = player != nullptr ? player->mo : nullptr;
	Inventory *inv = InterfaceInventory();
	const int itemRole = ItemRole(), offRole = OffRole();
	H.live = pmo != nullptr && player->health > 0 && (HeadsetOn() || VRHandTest_Active());

	if (VRHandTest_Active()) ScriptTick();

	Hand hand[2];
	if (H.live) for (int r = 0; r < 2; r++) ReadHand(r, hand[r]);
	bool grip[2] = { false, false };
	for (int r = 0; r < 2; r++) grip[r] = H.live && ReadGrip(r);
	const int rightItem = RightItem(inv);
	if (rightItem != H.lastRight)
	{
		if (H.live) Printf("[roth_held] the original's right-hand item is now %d: %s\n", rightItem,
			rightItem > 0 ? "shown in the item hand" : "the item hand shows nothing");
		H.lastRight = rightItem;
	}

	// ---- the match (owner Q1) ----
	if (worldTicks && H.match.Tick())
	{
		Printf("[roth_held] the match burns out after 20 s (%d struck so far)\n", H.match.Struck());
		Haptic(offRole, 0.3, 20);
	}
	if (worldTicks && H.live && hand[itemRole].ok && hand[offRole].ok && rightItem == kMatchbox && !H.match.Lit()
		&& H.lamp.Hand() != offRole)
	{
		const held::BoxFrame box = BoxOf(hand[itemRole]);
		const V3 head = M(hand[offRole].tip);
		const bool wasContact = H.strike.InContact();
		const int strip = H.strike.Strip();
		if (H.strike.Sample(box, head, held::kTicSeconds))
		{
			H.match.Strike();
			Printf("[roth_held] STRUCK: the %s hand's stroke along the Matchbox's %s striker at %.2f m/s: a match lit in the off hand for 20 s\n",
				RoleWord(offRole), strip > 0 ? "thumb-side" : strip < 0 ? "little-finger-side" : "", H.strike.LastSpeed());
			Haptic(offRole, 0.6, 40);
			Haptic(itemRole, 0.3, 30);
		}
		else if (H.strike.InContact() && !wasContact)
		{
			Printf("[roth_held] the %s hand's fingertip is on the Matchbox's striker (%s side), %.2f m/s along it\n", RoleWord(offRole),
				H.strike.Strip() > 0 ? "thumb" : H.strike.Strip() < 0 ? "little-finger" : "a", H.strike.LastSpeed());
			Haptic(offRole, 0.15, 8);
		}
	}
	else H.strike.Reset();
	if (H.match.Warning() && worldTicks && (H.match.Age() % 10) == 0) Haptic(offRole, 0.2, 15);
	H.haveFlame = false;
	if (H.match.Lit() && H.live && hand[offRole].ok)
	{
		H.flame = W(held::Match::FlamePoint(M(hand[offRole].tip), FingerDir(hand[offRole])));
		H.haveFlame = true;
	}
	else if (H.match.Lit() && !H.live) { H.match.PutOut(); Printf("[roth_held] the hands are gone: the match is out\n"); }

	// ---- the lamp (owner Q2) ----
	H.lampCarried = Carries(inv, kLampItem);
	const bool lampSelected = rightItem == kLampItem;   // the original's right hand holds it: it hangs from the item hand
	DVector3 bail(0, 0, 0);
	if (H.lampCarried && H.live && !lampSelected)
	{
		const DAngle yaw = pmo->Angles.Yaw;
		const V3 fwd(yaw.Cos(), yaw.Sin(), 0.0), right(yaw.Sin(), -yaw.Cos(), 0.0), up(0, 0, 1);
		const int offRight = VRHandPose_SideForRole(offRole) == VRHandPose::SIDE_RIGHT ? 1 : -1;
		const double eyeM = player->viewheight * PS() / UPM();
		const V3 hook = held::LampHook::HookPoint(M(pmo->Pos()), fwd, right, up, eyeM, offRight);
		const int holder = H.lamp.Hand();
		const V3 bailM = holder >= 0 && hand[holder].ok ? M(hand[holder].palm) : hook;
		bool valid[2];
		V3 palm[2];
		for (int r = 0; r < 2; r++) { valid[r] = hand[r].ok; palm[r] = hand[r].ok ? M(hand[r].palm) : V3(); }
		// Taken and let go only while the world runs: on the inventory or the popup the hands are its mouse, and a grip
		// there is the panel's click (roth_vrpick.cpp TickPanelHand), never the lamp's.
		const int ev = worldTicks ? H.lamp.Tick(valid, palm, grip, bailM) : 0;
		if (ev > 0)
		{
			Printf("[roth_held] LAMP: the %s hand takes the Lamp off its hook (palm %.2f m from the bail)\n", RoleWord(H.lamp.Hand()),
				(palm[H.lamp.Hand()] - hook).Length());
			Haptic(H.lamp.Hand(), 0.35, 25);
			if (H.lamp.Hand() == offRole && H.match.Lit()) { H.match.PutOut(); Printf("[roth_held] the off hand let the match go for the lamp: the match is out\n"); }
		}
		else if (ev < 0) { Printf("[roth_held] LAMP: let go, the Lamp hangs on its hook again\n"); Haptic(holder >= 0 ? holder : offRole, 0.2, 15); }
		const V3 b = H.lamp.Hand() >= 0 && hand[H.lamp.Hand()].ok ? M(hand[H.lamp.Hand()].palm) : hook;
		bail = W(b);
		H.lampGlobe = W(held::LampHook::GlobeBelow(b, up));
	}
	else
	{
		if (H.lamp.Hand() >= 0) Printf("[roth_held] LAMP: back on its hook (%s)\n", lampSelected ? "the original's right hand took it" : "the hands are gone");
		H.lamp.Reset();
		if (lampSelected && H.live && hand[itemRole].ok)
		{
			bail = hand[itemRole].palm;
			H.lampGlobe = W(held::LampHook::GlobeBelow(M(bail), V3(0, 0, 1)));
		}
	}
	// The original shows its lantern light only in a sector with the lantern bit (SyncSectorLight's Desaturation bit 0).
	H.lampLit = H.lampCarried && H.live && pmo->Sector != nullptr && (pmo->Sector->Colormap.Desaturation & 1) != 0;
	if ((int)H.lampLit != H.loggedLantern && H.lampCarried && H.live)
	{
		Printf("[roth_held] the player stands in sector %d: %s\n", pmo->Sector != nullptr ? pmo->Sector->Index() : -1,
			H.lampLit ? "a lantern sector: the Lamp's light is on" : "no lantern light here in the original: the Lamp adds none");
		H.loggedLantern = H.lampLit;
	}

	// ---- the Book (owner Q8) ----
	{
		int counts[400] = {};
		if (inv != nullptr)
			for (int i = 0; i < Inventory::SLOT_COUNT; i++)
			{
				const int id = (int16_t)inv->Slot(i).id;
				if (id > 0 && id < 400) counts[id] += inv->Slot(i).quantity > 0 ? inv->Slot(i).quantity : 1;
			}
		const int ev = H.book.Tick(InterfacePopupItem(), TheDbase100().ItemCount(), [&](int id) { return counts[id]; });
		if (ev & held::BookView::CLOSED) Printf("[roth_held] BOOK: the popup closed; the hands let the Book go\n");
		if (ev & held::BookView::OPENED) Printf("[roth_held] BOOK: the original's examine popup of item %d is up: held in both hands, the popup beside it\n", H.book.Item());
		if (ev & held::BookView::GIVEN)
		{
			Printf("[roth_held] BOOK: the original gave item %d during the look: it drops into the %s hand's palm (in the inventory already)\n",
				H.book.Palm(), RoleWord(offRole));
			Haptic(offRole, 0.3, 25);
		}
	}
	if (H.book.Item() != 0 && H.live && hand[0].ok && hand[1].ok)
	{
		// Beside the Book, squared to the eye, at the size it stood at on the inventory screen as the eye sees it.
		const DVector3 eye = pmo->Pos() + DVector3(0, 0, player->viewheight);
		const DVector3 mid = (hand[0].palm + hand[1].palm) * 0.5;
		DVector3 toEye = eye - mid;
		toEye.Z = 0;
		const double d = toEye.Length();
		const DVector3 right = d > 1e-6 ? DVector3(toEye.Y / d, -toEye.X / d, 0.0) * -1.0 : DVector3(0, 0, 0);
		const DVector3 at = mid + right * (0.24 * UPM()) + DVector3(0, 0, 0.04 * UPM() / PS());
		const double dist = (eye - at).Length();
		InterfacePopupPlace(true, PanelV3(at.X, at.Y, at.Z), dist / 44.0);   // 44: roth_interface.cpp DIST
		if (!H.popupPlaced) Printf("[roth_held] BOOK: the popup stands beside the Book, %.2f m from the eye\n", dist / UPM());
		H.popupPlaced = true;
	}
	else if (H.popupPlaced) { InterfacePopupPlace(false, PanelV3(0, 0, 0), 1.0); H.popupPlaced = false; }

	// ---- the niche (owner Q10) ----
	for (Group &g : H.groups)
	{
		int sum = 0, n = 0;
		for (int s : g.sectors) { const int l = RuntimeSectorLight(s); if (l >= 0) { sum += l; n++; } }
		if (n == 0) continue;
		const int light = sum / n;
		g.intensity = g.follow.Sample(light);
		if (g.follow.ChangeStarted())
			Printf("[roth_held] light group %d: the original's light change %d starts (%s, byte %d, base %d): the light at %.2f\n", g.group,
				g.follow.Changes(), g.follow.Direction() > 0 ? "rising" : "falling", light, g.follow.Base(), g.intensity);
		if (g.follow.ChangeEnded())
			Printf("[roth_held] light group %d: change %d done at byte %d (%+d over the pack's): the light at %.2f\n", g.group,
				g.follow.Changes(), light, light - g.follow.Base(), g.intensity);
	}

	// ---- what each hand shows ----
	held::HandShows shows[2];
	const int lampHand = lampSelected ? itemRole : H.lamp.Hand();
	held::WhatHandsShow(lampSelected ? 0 : rightItem, lampHand, H.match.Lit(), H.book.Item(), H.book.Palm(), itemRole, offRole, shows);

	// ---- a held item brought to the eyes plays its examine (ledger row 37, the owner's X1) ----
	if (worldTicks)
	{
		const bool haveItem = H.live && hand[itemRole].ok && shows[itemRole].item > 0;
		double dist = 1e9;
		if (H.live && hand[itemRole].ok)
		{
			const DVector3 eye = r_viewpoint.ViewLevel == level ? r_viewpoint.Pos : pmo->Pos() + DVector3(0, 0, player->viewheight);
			dist = (M(hand[itemRole].palm) - M(eye)).Length();
		}
		const bool wasArmed = H.eye.Armed();
		if (H.eye.Tick(haveItem, dist))
		{
			const bool started = InterfaceExamineRightHand(shows[itemRole].item);
			Printf("[roth_held] EYES: item %d held %.2f m from the eyes for %.1f s: the original's examine of it %s\n", shows[itemRole].item, dist,
				held::kEyeHoldTics / 35.0, started ? "(the inventory's right-hand box)" : "NOT started: a panel or a line owns the pointer");
			if (started) Haptic(itemRole, 0.25, 20);
		}
		else if (!wasArmed && H.eye.Armed()) Printf("[roth_held] EYES: the item hand left the eyes (%.2f m): armed again\n", dist);
	}
	if (H.live)
	{
		for (int r = 0; r < 2; r++)
		{
			if (!hand[r].ok) continue;
			if (shows[r].item > 0) ShowCard(H.card[r], shows[r].item, hand[r].palm, r);
			if (shows[r].palm > 0) ShowCard(H.palmBB, shows[r].palm, hand[r].palm, r);
			if (shows[r].match && H.haveFlame)
			{
				const V3 tip = M(hand[r].tip), dir = FingerDir(hand[r]);
				const V3 a = held::Match::StickEnd(tip, dir), b = held::Match::StickHead(tip, dir);
				for (int k = 0; k < 6; k++)
					Show(H.stick[k], BB_PANEL, 0xff, W(a + (b - a) * (k / 5.0)), 0.004 * UPM(), 0.004 * UPM() / PS(),
						k == 5 ? PalEntry(255, 70, 30, 20) : PalEntry(255, 120, 86, 50), 1.0, r);
				const double t = (level->maptime) / 35.0;
				const double fl = held::Match::Flicker(t);
				Show(H.flameBB, BB_PANEL, 0xff, H.flame, 0.010 * fl * UPM(), 0.016 * fl * UPM() / PS(), PalEntry(255, 255, 150, 40), 0.85 * fl, r);
				Show(H.flameCore, BB_PANEL, 0xff, H.flame - DVector3(0, 0, 0.002 * UPM() / PS()), 0.005 * UPM(), 0.008 * UPM() / PS(),
					PalEntry(255, 255, 240, 190), 0.95, r);
			}
		}
		if (H.lampCarried && (lampHand < 0 || hand[lampHand].ok))
		{
			// The lamp hangs by its bail: the card (30 cm) centred about half its height below the bail. On the hook it
			// rides the body (world points), in a hand that hand.
			const DVector3 centre = bail - DVector3(0, 0, held::kGlobeBelowBail * UPM() / PS());
			ShowCard(H.lampBB, kLampItem, centre, lampHand);
		}
		if (H.book.Item() != 0 && hand[0].ok && hand[1].ok)
			ShowCard(H.bookBB, H.book.Item(), (hand[0].palm + hand[1].palm) * 0.5, -1);
	}
	HideUnshown();
}

//==========================================================================
// The light, once a frame
//==========================================================================

void HeldFrameLights(FLevelLocals *level, double ticFrac)
{
	if (level == nullptr || level != H.level) return;
	const double t = (level->maptime + ticFrac) / 35.0;
	const double upm = UPM();
	if (H.haveFlame && H.match.Lit())
		Light(H.flame, kMatchColour, kMatchPeak * held::Match::Flicker(t), kMatchRadiusM * upm);
	if (H.lampLit)
		Light(H.lampGlobe, kLampColour, kLampPeak * held::Match::Flicker(t * 0.5 + 3.0), kLampRadiusM * upm);
	for (const Group &g : H.groups)
	{
		DVector3 at;
		if (g.intensity > 0.0 && GroupCentre(g, at)) Light(at, kNicheColour, kNichePeak * g.intensity, kNicheRadiusM * upm);
	}
}

//==========================================================================
// The others' questions
//==========================================================================

bool HeldMatchFlame(DVector3 &at)
{
	if (!H.haveFlame || !H.match.Lit()) return false;
	at = H.flame;
	return true;
}

bool HeldGripTaken(int role)
{
	if (!H.live || !H.lampCarried || role < 0 || role > 1) return false;
	Inventory *inv = InterfaceInventory();
	if (RightItem(inv) == kLampItem) return false;
	Hand h;
	if (!ReadHand(role, h)) return false;
	player_t *player = H.level != nullptr ? H.level->GetConsolePlayer() : nullptr;
	AActor *pmo = player != nullptr ? player->mo : nullptr;
	if (pmo == nullptr) return false;
	const DAngle yaw = pmo->Angles.Yaw;
	const V3 fwd(yaw.Cos(), yaw.Sin(), 0.0), right(yaw.Sin(), -yaw.Cos(), 0.0), up(0, 0, 1);
	const int offRight = VRHandPose_SideForRole(OffRole()) == VRHandPose::SIDE_RIGHT ? 1 : -1;
	const V3 hook = held::LampHook::HookPoint(M(pmo->Pos()), fwd, right, up, player->viewheight * PS() / UPM(), offRight);
	const bool mine = H.lamp.WantsGrip(role, M(h.palm), hook);
	if (mine) Printf("[roth_held] the %s hand's grip is the lamp's: the picker leaves it\n", RoleWord(role));
	return mine;
}

bool HeldHandCarries(int role)
{
	if (!H.live || role < 0 || role > 1) return false;
	if (H.book.Item() != 0) return true;
	const Inventory *inv = InterfaceInventory();
	const bool lampSelected = inv != nullptr && inv->RightHand() >= 0 && (int16_t)inv->Slot(inv->RightHand()).id == kLampItem;
	if (!lampSelected && H.lamp.Hand() == role) return true;
	return role == OffRole() && H.match.Lit();
}

void HeldPose(int role, VRHandPose::HeldState &held)
{
	if (!H.live || role < 0 || role > 1) return;
	const int itemRole = ItemRole(), offRole = OffRole();
	if (H.book.Item() != 0) { held = HeldFor(H.book.Item(), false); return; }       // both hands round the Book
	const Inventory *inv = InterfaceInventory();
	const bool lampSelected = inv != nullptr && inv->RightHand() >= 0 && (int16_t)inv->Slot(inv->RightHand()).id == kLampItem;
	if (!lampSelected && H.lamp.Hand() == role) { held = HeldFor(kLampItem, false); return; }
	if (role == offRole && role != itemRole && H.match.Lit())
	{
		held = VRHandPose::HeldState();
		held.pose = "fbx_pinch";    // the match pinched (owner Q1: fbx_pinch = Blender 694)
	}
}

void HeldForcedTurn(double fromYawDeg, double toYawDeg)
{
	const held::TurnView v = held::ForcedTurnView(HeadsetOn(), MovieBusy());
	Printf("[roth_held] FORCED TURN (0x3f): the player turned from %.1f to %.1f -- %s\n", fromYawDeg, toYawDeg, held::TurnViewName(v));
	if (v == held::TurnView::Now) { resetDoomYaw = true; H.turnHeld = false; }
	else if (v == held::TurnView::AtBlack) { H.turnHeld = true; H.turnTo = toYawDeg; }
}

void HeldMovieOver()
{
	if (!H.turnHeld) return;
	resetDoomYaw = true;
	H.turnHeld = false;
	Printf("[roth_held] FORCED TURN: the movies are over, the screen still black: the VR view takes the turn to %.1f\n", H.turnTo);
}

} // namespace roth

//===========================================================================
//
// roth_heldtest -- THE FLAT TEST'S SCRIPTED HAND MOTION for the held objects. Test-only, inert until typed. Works on
// the hand test's hands (roth_handtest, roth_vrpicktest on both), moving ONE hand by VRHandTest_SetOffset, so what
// the rules see is the hands as drawn.
//
//   roth_heldtest strike [m/s]          the off hand's index fingertip to the start of the Matchbox's thumb-side
//                                       striker (in the item hand), then along it 7 cm at that speed (default 0.6)
//   roth_heldtest flame <sector> <index> <wick>
//                                       the off hand moved so the lit match's FLAME is at that wick (1-based)
//   roth_heldtest lamp                  the off hand's palm to the Lamp's bail (on its hook)
//   roth_heldtest eyes [metres]         the item hand's palm (its card) <metres> (default 0.08) in front of the eyes
//   roth_heldtest reach <sector> <index> [metres]
//                                       the item hand's palm <metres> (default 0.03) from that object's middle,
//                                       toward the eye (a grip then takes it as the picker's grip does)
//   roth_heldtest stand <sector> <index> <units> <bearing> [pitch]
//                                       stand <units> from that object at <bearing> (object -> camera, degrees;
//                                       0 east, 90 north), facing it, the view pitched <pitch> degrees down (0)
//   roth_heldtest offset <left|right> <ahead> <right> <up>
//                                       one hand's offset from its place, metres in the camera's frame
//                                       (VRHandTest_SetOffset's order, shared with roth_gesturetest move)
//   roth_heldtest home                  both hands back to their places
//   roth_heldtest state                 a line with what the held objects are doing
//
//===========================================================================

namespace
{

bool TestHand(int role, roth::Hand &h)
{
	if (!roth::ReadHand(role, h)) { Printf("roth_heldtest: no %s hand drawn yet (roth_handtest, roth_vrpicktest on both first)\n", roth::RoleWord(role)); return false; }
	return true;
}

} // namespace

CCMD(roth_heldtest)
{
	using namespace roth;
	using held::V3;
	if (argv.argc() < 2)
	{
		Printf("roth_heldtest strike [m/s] | flame <sector> <index> <wick> | lamp | eyes [metres] | reach <sector> <index> [metres]"
			" | stand <sector> <index> <units> <bearing> [pitch] | offset <left|right> <ahead> <right> <up> | home | state\n");
		return;
	}
	AActor *pmo = players[consoleplayer].mo;
	if (pmo == nullptr || H.level == nullptr) { Printf("roth_heldtest: no level\n"); return; }
	const int itemRole = ItemRole(), offRole = OffRole();
	const char *cmd = argv[1];
	if (!stricmp(cmd, "home"))
	{
		for (int s = 0; s < VRHandPose::SIDE_COUNT; s++) VRHandTest_SetOffset(s, 0, 0, 0);
		H.scriptStroke = 0;
		Printf("roth_heldtest: both hands at their places\n");
		return;
	}
	if (!stricmp(cmd, "offset") && argv.argc() >= 6)
	{
		const int side = !stricmp(argv[2], "right") ? VRHandPose::SIDE_RIGHT : VRHandPose::SIDE_LEFT;
		VRHandTest_SetOffset(side, atof(argv[3]), atof(argv[4]), atof(argv[5]));
		Printf("roth_heldtest: the %s hand offset %s %s %s m (ahead, right, up)\n", argv[2], argv[3], argv[4], argv[5]);
		return;
	}
	if (!stricmp(cmd, "state"))
	{
		Printf("roth_heldtest: hands %s; right-hand item %d; match %s (%d tics left, %d struck); lamp %s%s; book %d, palm %d; groups %d\n",
			H.live ? "live" : "not live", RightItem(InterfaceInventory()), H.match.Lit() ? "lit" : "out", H.match.TicsLeft(), H.match.Struck(),
			H.lamp.Hand() >= 0 ? FStringf("in the %s hand", RoleWord(H.lamp.Hand())).GetChars() : "on its hook", H.lampLit ? ", lit (a lantern sector)" : "",
			H.book.Item(), H.book.Palm(), (int)H.groups.size());
		Printf("roth_heldtest:   the eyes: %s, held %d of %d tics\n", H.eye.Armed() ? "armed" : "not armed (leave 0.25 m)", H.eye.Held(), held::kEyeHoldTics);
		if (H.haveFlame)
		{
			Hand oh;
			if (ReadHand(offRole, oh))
				Printf("roth_heldtest:   the match: flame at (%.1f %.1f %.1f), %.3f m beyond the %s hand's index fingertip (%.1f %.1f %.1f); stick %.3f m\n",
					H.flame.X, H.flame.Y, H.flame.Z, (M(H.flame) - M(oh.tip)).Length(), RoleWord(offRole), oh.tip.X, oh.tip.Y, oh.tip.Z,
					held::kStickAhead + held::kStickBehind);
		}
		for (const Group &g : H.groups) Printf("roth_heldtest:   light group %d: intensity %.2f, %d change(s), peak %+d\n", g.group, g.intensity, g.follow.Changes(), g.follow.Peak() - g.follow.Base());
		return;
	}
	if (!stricmp(cmd, "strike"))
	{
		Hand ih, oh;
		if (!TestHand(itemRole, ih) || !TestHand(offRole, oh)) return;
		const double speed = argv.argc() >= 3 ? atof(argv[2]) : 0.6;
		const held::BoxFrame box = BoxOf(ih);
		const double startA = -(held::kBoxLength * 0.5 + 0.008), length = held::kBoxLength + 0.016;
		const V3 start = box.centre + box.along * startA + box.across * (held::kBoxWidth * 0.5 + 0.004);
		MoveTestHand(offRole, start - M(oh.tip));
		const int tics = std::max(1, (int)ceil(length / (speed * held::kTicSeconds)));
		const V3 step = box.along * (length / tics);
		CameraLocal(step, H.strokeStep);
		H.scriptStroke = tics;
		H.strokeWait = 2;    // two tics for the hand to be drawn at the start
		Printf("roth_heldtest strike: the %s hand's fingertip to the striker's start, then %d tic(s) along it at %.2f m/s (%.3f m)\n",
			RoleWord(offRole), tics, length / (tics * held::kTicSeconds), length);
		return;
	}
	if (!stricmp(cmd, "flame") && argv.argc() >= 5)
	{
		Hand oh;
		if (!TestHand(offRole, oh)) return;
		DVector3 wick;
		if (!CandleWickWorld(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]) - 1, pmo->Angles.Yaw.Degrees(), wick))
		{ Printf("roth_heldtest flame: no wick %s on object %s/%s\n", argv[4], argv[2], argv[3]); return; }
		const V3 flame = held::Match::FlamePoint(M(oh.tip), FingerDir(oh));
		MoveTestHand(offRole, M(wick) - flame);
		Printf("roth_heldtest flame: the %s hand moved %.2f m so the match's flame is at wick %s of object %s/%s (%s)\n", RoleWord(offRole),
			(M(wick) - flame).Length(), argv[4], argv[2], argv[3], H.match.Lit() ? "the match is lit" : "NO MATCH LIT");
		return;
	}
	if (!stricmp(cmd, "eyes"))
	{
		// The item hand's palm (its card) <metres> (default 0.08) in front of the eyes.
		Hand ih;
		if (!TestHand(itemRole, ih)) return;
		const double metres = argv.argc() >= 3 ? atof(argv[2]) : 0.08;
		const DVector3 eye = r_viewpoint.ViewLevel == H.level ? r_viewpoint.Pos : pmo->Pos() + DVector3(0, 0, players[consoleplayer].viewheight);
		const DAngle yaw = pmo->Angles.Yaw;
		const V3 target = M(eye) + V3(yaw.Cos(), yaw.Sin(), 0.0) * metres;
		MoveTestHand(itemRole, target - M(ih.palm));
		Printf("roth_heldtest eyes: the %s hand's palm moved %.2f m to %.2f m in front of the eyes\n", RoleWord(itemRole), (target - M(ih.palm)).Length(), metres);
		return;
	}
	if (!stricmp(cmd, "lamp"))
	{
		Hand oh;
		if (!TestHand(offRole, oh)) return;
		player_t *player = &players[consoleplayer];
		const DAngle yaw = pmo->Angles.Yaw;
		const V3 fwd(yaw.Cos(), yaw.Sin(), 0.0), right(yaw.Sin(), -yaw.Cos(), 0.0), up(0, 0, 1);
		const int offRight = VRHandPose_SideForRole(offRole) == VRHandPose::SIDE_RIGHT ? 1 : -1;
		const V3 hook = held::LampHook::HookPoint(M(pmo->Pos()), fwd, right, up, player->viewheight * PS() / UPM(), offRight);
		MoveTestHand(offRole, hook - M(oh.palm));
		Printf("roth_heldtest lamp: the %s hand's palm moved %.2f m to the Lamp's bail on its hook\n", RoleWord(offRole), (hook - M(oh.palm)).Length());
		return;
	}
	if (!stricmp(cmd, "reach") && argv.argc() >= 4)
	{
		Hand ih;
		if (!TestHand(itemRole, ih)) return;
		AActor *mo = FindObjectActor(atoi(argv[2]), atoi(argv[3]));
		if (mo == nullptr) { Printf("roth_heldtest reach: object %s/%s has no actor\n", argv[2], argv[3]); return; }
		DVector3 lo, hi;
		ObjectPickBox(mo, lo, hi);
		const DVector3 mid = (lo + hi) * 0.5;
		const double metres = argv.argc() >= 5 ? atof(argv[4]) : 0.03;
		const DVector3 eye = pmo->Pos() + DVector3(0, 0, players[consoleplayer].viewheight);
		const V3 toward = (M(eye) - M(mid)).Unit();
		const V3 target = M(mid) + toward * metres;
		MoveTestHand(itemRole, target - M(ih.palm));
		Printf("roth_heldtest reach: the %s hand's palm moved %.2f m to %.2f m from object %s/%s's middle (%.0f %.0f %.0f)\n", RoleWord(itemRole),
			(target - M(ih.palm)).Length(), metres, argv[2], argv[3], mid.X, mid.Y, mid.Z);
		return;
	}
	if (!stricmp(cmd, "stand") && argv.argc() >= 6)
	{
		AActor *mo = FindObjectActor(atoi(argv[2]), atoi(argv[3]));
		if (mo == nullptr) { Printf("roth_heldtest stand: object %s/%s has no actor\n", argv[2], argv[3]); return; }
		const double d = atof(argv[4]);
		const DAngle b = DAngle::fromDeg(atof(argv[5]));
		const double pitch = argv.argc() >= 7 ? atof(argv[6]) : 0.0;
		pmo->SetOrigin(DVector3(mo->X() + b.Cos() * d, mo->Y() + b.Sin() * d, pmo->Z()), false);
		pmo->SetZ(pmo->floorz);
		pmo->Vel.Zero();
		pmo->Angles.Yaw = b + DAngle::fromDeg(180.0);
		pmo->Angles.Pitch = DAngle::fromDeg(pitch);
		pmo->ClearInterpolation();
		Printf("roth_heldtest stand: %.0f units from object %s/%s at bearing %s, at (%.0f %.0f %.0f) in sector %d, pitch %.0f\n", d, argv[2], argv[3],
			argv[5], pmo->X(), pmo->Y(), pmo->Z(), pmo->Sector != nullptr ? pmo->Sector->Index() : -1, pitch);
		return;
	}
	Printf("roth_heldtest: unknown '%s'\n", cmd);
}
