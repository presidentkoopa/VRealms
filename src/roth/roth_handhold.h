#pragma once
//
// WHAT ADAM'S VR HAND MAKES ROUND EACH REALMS THING IT HOLDS: an item (DBASE100 index) -> UZDXREMA's hold
// subject (EGripSubject, the pose hold_<subject>), or for a weapon its grip class (the pose grip_<class>), the
// owner's poses being r_data/vr_handpose_math.h kOwnerPoses (his fbx_poses.json). Engine-free.
//
// THE DESIGN IS docs/ITEM_DESIGN.md (the item dossiers, [P]: where Adam holds each item and the frame its pose
// needs). Each row maps the frame that dossier names to the subject whose pose IS that frame:
//     wrap_half (Blender 820)  -> Magazine   (hold_magazine)          relaxed (154) -> Inserting (hold_inserting)
//     wrap_tight (837)         -> Forend     (hold_forend)            fist (319)    -> Foregrip  (hold_foregrip)
//     cup (985), from below    -> Shell      (hold_shell)
//     a pinch (no such frame)  -> Round      (hold_round: the cup -- UZDXREMA's own answer, "NO PINCH IN THE FBX:
//                                            the nearest it has is the cup")   NEEDS A NEW AUTHORED FRAME (owner)
//     curl (206)               -> no subject's pose is curl: the frame by name (HeldState::pose, fbx_curl)
// A weapon (the original's left-hand slot, drawn at the VR main hand by RealmsHeld) by its grip class:
//     grip_pistol (the dossier names it) -> "pistol";  wrap_half -> "rocket" (grip_rocket IS wrap_half, its fire
//     pose closes the index to wrap_tight);  wrap_tight -> no class: gun_grip, UZDXREMA's own default (wrap_tight,
//     the index left to the trigger).
// Anything not listed has no design yet: no subject, and the hand keeps the controller's pose -- UZDXREMA's
// behaviour for a thing that names no pose. Item numbers and names: the dossiers' headings.
//
// WHICH HAND [P, docs/ITEM_DESIGN.md section 1, not yet the owner's decision]: the original's right-hand item
// (g_selected_item_primary) is held in the VR OFF hand; the weapon slot rides the main hand, where RealmsHeld is
// drawn (FollowHandMode 1). roth_hands.cpp applies it.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include "r_data/vr_handpose_math.h"

namespace roth
{

struct ItemGrip
{
	int item;					// DBASE100 index
	const char *name;			// the dossier's heading
	bool weapon;				// a weapon (category 1): grip class below; otherwise subject / pose
	int subject;				// VRHandPose::GRIPSUBJ_*
	const char *pose;			// a pose of the owner's table by name, or nullptr
	const char *gripClass;		// weapon: grip_<class>, or nullptr (gun_grip)
	bool needsFrame;			// the dossier wants a frame the take does not have (a pinch): owner
	const char *why;			// the dossier's words, and the fit
};

inline constexpr ItemGrip kItemGrips[] =
{
	// --- items (the original's right hand) ---
	{  15, "Lamp",                   false, VRHandPose::GRIPSUBJ_None,      "fbx_curl", nullptr, false, "the bail, fingers hooked: curl (206); no subject's pose is curl" },
	{  16, "Matchbox",               false, VRHandPose::GRIPSUBJ_Magazine,  nullptr, nullptr, false, "wrap_half (820) round the sleeve" },
	{   1, "Shards and Seals",       false, VRHandPose::GRIPSUBJ_Foregrip,  nullptr, nullptr, false, "the pouch by its neck in a fist (319)" },
	{ 175, "Magical Torch",          false, VRHandPose::GRIPSUBJ_Forend,    nullptr, nullptr, false, "the barrel, wrap_tight (837)" },
	{   4, "Statuette (serpent)",    false, VRHandPose::GRIPSUBJ_Magazine,  nullptr, nullptr, false, "round the coils, wrap_half (820)" },
	{ 113, "Earplugs (worn)",        false, VRHandPose::GRIPSUBJ_Round,     nullptr, nullptr, true,  "tip pinch (to be authored); fallback cup" },
	{ 223, "Brain",                  false, VRHandPose::GRIPSUBJ_Shell,     nullptr, nullptr, false, "cupped from below, cup (985)" },
	{   6, "Magnifying Glass",       false, VRHandPose::GRIPSUBJ_Forend,    nullptr, nullptr, false, "the handle, wrap_tight" },
	{  14, "The Shrive",             false, VRHandPose::GRIPSUBJ_Magazine,  nullptr, nullptr, false, "halfcurl (193) or wrap_half: wrap_half (halfcurl is no subject's pose)" },
	{  35, "Crystal (Gnarl)",        false, VRHandPose::GRIPSUBJ_Round,     nullptr, nullptr, true,  "lateral pinch (fallback cup)" },
	{  60, "Mask (Gnarl's mask)",    false, VRHandPose::GRIPSUBJ_Inserting, nullptr, nullptr, false, "both hands at the sides, relaxed (154); one hand here" },
	{ 131, "Fan",                    false, VRHandPose::GRIPSUBJ_Magazine,  nullptr, nullptr, false, "the pivot end, wrap_half" },
	{  37, "Crystal (CAVERNS)",      false, VRHandPose::GRIPSUBJ_Round,     nullptr, nullptr, true,  "lateral pinch or cup" },
	{  49, "Statuette (quartz)",     false, VRHandPose::GRIPSUBJ_Magazine,  nullptr, nullptr, false, "wrap_half round the figure" },
	{ 103, "Jewel (garnet)",         false, VRHandPose::GRIPSUBJ_Round,     nullptr, nullptr, true,  "lateral pinch (until then, cup)" },
	{ 106, "Gem (amethyst)",         false, VRHandPose::GRIPSUBJ_Round,     nullptr, nullptr, true,  "as 103" },
	{ 114, "Coloured Gem (emerald)", false, VRHandPose::GRIPSUBJ_Round,     nullptr, nullptr, true,  "as 103" },
	{ 115, "Coloured Gem (turquoise)", false, VRHandPose::GRIPSUBJ_Round,   nullptr, nullptr, true,  "as 103" },
	{ 122, "Coloured Gem (sapphire)", false, VRHandPose::GRIPSUBJ_Round,    nullptr, nullptr, true,  "as 103" },
	{ 123, "Coloured Gem (topaz)",   false, VRHandPose::GRIPSUBJ_Round,     nullptr, nullptr, true,  "as 103" },
	{ 154, "Amber",                  false, VRHandPose::GRIPSUBJ_Round,     nullptr, nullptr, true,  "as 103" },
	{ 116, "Ring",                   false, VRHandPose::GRIPSUBJ_Round,     nullptr, nullptr, true,  "lateral pinch on the bezel" },
	{ 128, "Feather (stained glass)", false, VRHandPose::GRIPSUBJ_Round,    nullptr, nullptr, true,  "tip pinch at the quill (the dossier's fallback: the trigger state)" },
	{ 152, "Bowl and Snow",          false, VRHandPose::GRIPSUBJ_Inserting, nullptr, nullptr, false, "under the base, palm up: relaxed (154) or cup" },
	{ 155, "Amber and Mortar",       false, VRHandPose::GRIPSUBJ_Shell,     nullptr, nullptr, false, "the mortar on the palm, cup" },
	{ 167, "Key (silver)",           false, VRHandPose::GRIPSUBJ_Round,     nullptr, nullptr, true,  "lateral pinch on the bow (the dossier's fallback: the trigger state)" },
	{ 171, "Key (floral)",           false, VRHandPose::GRIPSUBJ_Round,     nullptr, nullptr, true,  "as 167" },
	{ 193, "Key (plain)",            false, VRHandPose::GRIPSUBJ_Round,     nullptr, nullptr, true,  "as 167" },
	{ 194, "Tinder (Burning)",       false, VRHandPose::GRIPSUBJ_Shell,     nullptr, nullptr, false, "from below, cup" },
	{ 202, "Spectacles (worn)",      false, VRHandPose::GRIPSUBJ_Round,     nullptr, nullptr, true,  "tip pinch at each temple" },
	{  18, "Orb",                    false, VRHandPose::GRIPSUBJ_Shell,     nullptr, nullptr, false, "cup (985)" },
	// 216 Spirit Ring (worn): worn on the ring finger, not held. 68 Adam Randall: category 2, never in a hand.
	// --- weapons (the original's left-hand slot, RealmsHeld at the main hand) ---
	{   9, "Colt-45",                true,  VRHandPose::GRIPSUBJ_None, nullptr, "pistol",  false, "grip_pistol, the trigger finger grip_pistol_fire (the dossier)" },
	{ 150, "Wand",                   true,  VRHandPose::GRIPSUBJ_None, nullptr, "rocket",  true,  "lateral pinch plus wrap_half; until the pinch exists, wrap_half (grip_rocket)" },
	{ 156, "Chalice (AQUA1)",        true,  VRHandPose::GRIPSUBJ_None, nullptr, "rocket",  false, "the stem, wrap_half (grip_rocket)" },
	{ 127, "Chalice (ANUBIS)",       true,  VRHandPose::GRIPSUBJ_None, nullptr, "rocket",  false, "as 156" },
	{  33, "Staff (Florentine's)",   true,  VRHandPose::GRIPSUBJ_None, nullptr, nullptr,   false, "at the balance, wrap_tight (gun_grip)" },
	{  97, "Eternity (Dragon Sword)", true, VRHandPose::GRIPSUBJ_None, nullptr, nullptr,   false, "wrap_tight on the grip (gun_grip)" },
	{  28, "Shield (dragon)",        true,  VRHandPose::GRIPSUBJ_None, nullptr, nullptr,   false, "hand on the grip, wrap_tight (gun_grip); the dossier says off hand, RealmsHeld rides the main" },
};

inline const ItemGrip *ItemGripFor(int item)
{
	for (const ItemGrip &g : kItemGrips) if (g.item == item) return &g;
	return nullptr;
}

// The held state for a thing in the hand. `asWeapon`: it is the weapon slot's (RealmsHeld drawn), whose class `cls`
// is the prop class (UZDXREMA VRRig_HandPropClass; the table has no grip_<that class>, so it only shapes a hand if a
// pose by that name is added). An item with no design holds nothing: the controller's pose.
inline VRHandPose::HeldState HeldFor(int item, bool asWeapon, const char *cls = nullptr)
{
	VRHandPose::HeldState h;
	h.item = item;
	if (item <= 0) return h;
	const ItemGrip *g = ItemGripFor(item);
	if (asWeapon)
	{
		h.weapon = true;
		if (cls != nullptr) h.propClass = cls;
		if (g != nullptr && g->weapon && g->gripClass != nullptr) h.gripClass = g->gripClass;
		return h;
	}
	if (g == nullptr || g->weapon) return h;
	h.subject = g->subject;
	if (g->pose != nullptr) h.pose = g->pose;
	return h;
}

} // namespace roth
