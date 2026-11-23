#pragma once
//
// roth_handstate.h -- L2, ONE OWNER PER HAND PER TIC (docs/VR_INTERACTION.md section 4, L2). Engine-free, so
// tools/handstate_test drives scripted hands through every state and every owner.
//
//   FREE -> TARGETING -> ACTING -> back to FREE or TARGETING
//                        \-> HOLDING (an item from a use) -> let go: stowed -> FREE
//   HOLDING a weapon: the weapon owns the hand's trigger until it is put away
//   SUPPORTING: the off hand on a held weapon (the Shotgun's fore-end today); it gives way the moment the off hand
//               starts a reload action (the Colt's magazine button, pouch, slide)
//
// WHO OWNS A HAND. Each tic the hand's owner is decided once, here, from what each owner says it is doing (Facts),
// and every owner asks it before it acts (roth_vrpick.h HandMayAct). The standing states come first, in this order,
// and keep the hand while they last:
//   1. the Colt's reload action (the off hand at the magazine button, the pouch or the slide, a magazine in it,
//      racking)                                                                     ACTING     Colt
//   2. a gesture held (a pull, a twist, the hour hand: roth_worldhands.h)            ACTING     Gesture
//   3. the off hand on a held weapon's support (the fore-end latch engaged)          SUPPORTING Weapon
//   4. a held object (the Lamp by its bail, the lit match, the open Book: roth_held.h) HOLDING  Held
//   5. an item squeezed into the hand (a pickup, or the hand closed on its item)     HOLDING    Item
//   6. a weapon drawn in the main hand                                               HOLDING    Weapon
//   7. the lit match in the off hand                                                 HOLDING    Held
//   8. the selected item drawn in this hand with the grip free (the game put it there) HOLDING  Item (carried)
// 6-8 hold the hand for its trigger and its drawing, not its grip: a closing grip still goes to the claims below.
// 1 comes before 3: the support gives way to a reload action. 2 comes before 5: a held item's twist is a gesture on it.
//
// A GRIP CLOSING on a hand that no standing state 1-5 holds asks, in this order, and the first that takes it owns it:
//   the Lamp's bail (Held) -> the Colt (Colt) -> the weapon (the fore-end: SUPPORTING; a body slot: ACTING) ->
//   a world gesture on what the hand touches (a lever's pull, a socket's twist: Gesture) -> the picker's use (Picker).
// The picker's use is D3 (the touch beats the point; docs/VR_INTERACTION.md 4, L2 and the rule below). If the use
// found no target and the selected item is in this hand (8), the hand closes on it: HOLDING, Item (owner 2026-11-23).
//
// A GRIP OPENING on HOLDING Item ends the hold: the item is stowed (D7) unless a world gesture set it down somewhere
// (the release: roth_worldhands.h). An item the game put in the hand and no grip closed on (7) is not stowed by an
// opening: no hold ended (owner 2026-11-23: during the auto-select the player lets go between the two squeezes).
//
// THE TARGETING RULE (owner 2026-11-23, D3 with numbers): a thing within kTouchM (5 cm) of the palm or the fingertip is
// TOUCHED: it is the target, it gets the rim and the light tick, and it beats whatever the index points at. With no
// touch, the extended index's ray is the target. With neither, a squeeze still searches the in-reach zone it always
// did (an object's box within 15 cm of the palm, a surface 20 cm along the hand, a wall or door within 10 cm of the
// palm, a flat 10 cm under it). The zone only chooses WHAT is used; HOW FAR the use reaches is always L1's, from the
// body (D2: 268 units).
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include <stdint.h>

namespace roth
{
namespace hands
{

constexpr double kTouchM = 0.05;            // D3: a touch, palm or fingertip
constexpr double kZoneObjectM = 0.15;       // the in-reach zone (a squeeze with no touch and no point)
constexpr double kZoneAlongM = 0.20;
constexpr double kZoneSurfaceM = 0.10;
constexpr double kStowSeconds = 0.2;        // D7: the item drawn into the coat

enum class State : uint8_t { Free, Targeting, Acting, Holding, Supporting };
enum class Owner : uint8_t { None, Picker, Held, Weapon, Colt, Gesture, Item };

inline const char *StateName(State s)
{
	switch (s)
	{
	case State::Free:       return "FREE";
	case State::Targeting:  return "TARGETING";
	case State::Acting:     return "ACTING";
	case State::Holding:    return "HOLDING";
	case State::Supporting: return "SUPPORTING";
	}
	return "?";
}

inline const char *OwnerName(Owner o)
{
	switch (o)
	{
	case Owner::None:    return "nobody";
	case Owner::Picker:  return "the picker";
	case Owner::Held:    return "a held object";
	case Owner::Weapon:  return "the weapon";
	case Owner::Colt:    return "the Colt";
	case Owner::Gesture: return "a world gesture";
	case Owner::Item:    return "the item";
	}
	return "?";
}

// What the hand and each owner say this tic. The engine fills it (roth_vrpick.cpp); the test scripts it.
struct Facts
{
	bool valid = false;         // a controller
	bool gripClosed = false;    // the grip, after its hysteresis
	bool gripEdge = false;      // it closed this tic
	bool releaseEdge = false;   // it opened this tic
	bool target = false;        // a touch or a point target (TARGETING)
	// the standing states (each owner's own truth)
	bool coltAction = false;
	bool gestureHeld = false;
	bool supportHeld = false;
	bool heldObject = false;
	bool weaponInHand = false;
	bool toolInHand = false;    // a held object whose grip stays the hand's own (the lit match: its point and grip light
	                            // a tall wick, roth_candles.h CandlesPointGrip)
	bool itemCarried = false;   // the selected item is in this hand (the game put it there)
};

// The grip-closing edge's claims, asked in order; each may take the thing it claims (side effects). Use() is the
// picker's squeeze: 1 it went to a target (it acted or the original refused it), 0 nothing to use.
struct Claims
{
	virtual ~Claims() {}
	virtual bool Held() { return false; }
	virtual bool Colt() { return false; }
	virtual bool Weapon(bool &supporting) { supporting = false; return false; }
	virtual bool Gesture() { return false; }
	virtual int Use() { return 0; }
};

struct Step
{
	Owner gripTo = Owner::None;   // who took this tic's grip-closing edge
	bool tookHold = false;        // the hand closed on its item (no target): HOLDING Item
	bool holdEnded = false;       // a hold of an item ended (the grip opened): stow it, unless set down
};

class Hand
{
public:
	State state = State::Free;
	Owner owner = Owner::None;
	bool itemHeld = false;        // an item squeezed into this hand (L2's own state)
	bool openGrip = false;        // this tic's holding is for the trigger and the drawing only (6-8): the grip is free
	bool landing = false;         // the pickup still flies to this hand: held when it lands

	void Reset() { *this = Hand(); }

	// A pickup the use just made from this hand: the item flies to it, HOLDING from now (the grip is closed).
	void BeginPickup() { itemHeld = true; landing = true; state = State::Holding; owner = Owner::Item; }
	void Landed() { landing = false; }
	// The item is gone from the hand by the game (consumed, given away, the selection changed): no hold.
	void Drop() { itemHeld = false; landing = false; }

	Step Tick(const Facts &f, Claims &c)
	{
		Step s;
		openGrip = false;
		if (!f.valid) { const bool had = itemHeld; Reset(); s.holdEnded = had; return s; }
		// THE RELEASE OF A HOLD: an item squeezed into the hand is let go
		if (itemHeld && f.releaseEdge) { itemHeld = false; landing = false; s.holdEnded = true; }
		// THE STANDING STATES, in order (see the header)
		if (Stand(f)) { if (f.gripEdge) s.gripTo = owner; return s; }
		// A GRIP CLOSING: the claims, in order
		if (f.gripEdge)
		{
			bool supporting = false;
			if (c.Held())                     { Set(State::Holding, Owner::Held); s.gripTo = Owner::Held; return s; }
			if (c.Colt())                     { Set(State::Acting, Owner::Colt); s.gripTo = Owner::Colt; return s; }
			if (c.Weapon(supporting))         { Set(supporting ? State::Supporting : State::Acting, Owner::Weapon); s.gripTo = Owner::Weapon; return s; }
			if (c.Gesture())                  { Set(State::Acting, Owner::Gesture); s.gripTo = Owner::Gesture; return s; }
			if (c.Use() != 0)                 { Set(State::Acting, Owner::Picker); s.gripTo = Owner::Picker; return s; }
			if (f.itemCarried)                { itemHeld = true; Set(State::Holding, Owner::Item); s.gripTo = Owner::Item; s.tookHold = true; return s; }
		}
		// NOTHING STANDS: the holdings whose grip stays open to the claims, or nobody's
		openGrip = true;
		if (f.weaponInHand)     Set(State::Holding, Owner::Weapon);
		else if (f.toolInHand)  Set(State::Holding, Owner::Held);
		else if (f.itemCarried) Set(State::Holding, Owner::Item);
		else Set(f.target ? State::Targeting : State::Free, Owner::None);
		return s;
	}

	// May `who` act with this hand this tic? Its owner, or nobody's hand -- and a hand held for its trigger and its
	// drawing only (the weapon drawn, the lit match, the carried item: openGrip) stays open to the claims and the picker.
	bool MayAct(Owner who) const
	{
		if (owner == who || owner == Owner::None) return true;
		if (owner == Owner::Picker) return true;   // the picker's act is one tic: nothing stands
		return openGrip;
	}

	// Is the hand empty for the fists' gestures (the punch, the knock)?
	bool Empty() const { return owner == Owner::None || owner == Owner::Picker; }

private:
	void Set(State s, Owner o) { state = s; owner = o; }
	bool Stand(const Facts &f)
	{
		if (f.coltAction)  { Set(State::Acting, Owner::Colt); return true; }
		if (f.gestureHeld) { Set(State::Acting, Owner::Gesture); return true; }
		if (f.supportHeld) { Set(State::Supporting, Owner::Weapon); return true; }
		if (f.heldObject)  { Set(State::Holding, Owner::Held); return true; }
		if (itemHeld)      { Set(State::Holding, Owner::Item); return true; }
		return false;
	}
};

} // namespace hands
} // namespace roth
