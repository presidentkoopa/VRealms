#pragma once
//
// roth_img3d.h -- THE ITEM MODELS IN THE ENGINE (the owner's decision of 2026-11-16: "sprites are a no-go").
//
// A folder OUTSIDE the repository holds one GLB per item and a manifest.json (tools/img3d, docs/IMG3D_PIPELINE.md;
// the reader is roth_img3d_core.h). Where an item has a model it is drawn instead of the item's picture:
//   - lying in the world: the Realms object whose render id names the item (Dbase100::FindItemByRenderId, the
//     original's give_item_by_dbase_id search) wears the model at the object's place, base on the picture's bottom,
//     front along the object's facing;
//   - in a hand: roth_held's cards (the item hand's item, the palm, the Lamp, the Book) become a RealmsItemModel
//     actor riding the hand's grip frame at draw rate (FollowHandMode 3 / 4), or standing upright at the point
//     (hold "hang", and the cards roth_held places in the world).
// Where no model exists nothing changes: the picture or the card. Game logic is not touched: which object is
// where, what the hands hold and what a click does are all decided before any of this runs.
//
// THE FOLDER: the cvar roth_img3d_dir (NOT archived: never written to an ini), else the first line of img3d_path.txt
// beside the executable (RUN_IMG3D.bat writes it); a relative folder is the executable's. Empty or missing: no
// models, nothing logged beyond one line.
// Read at the first Realms map load and again by `roth_img3d reload` or when the folder named changes.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include <stdint.h>
#include "vectors.h"

struct FLevelLocals;
class AActor;

namespace roth
{

// roth_objects.cpp PrepareObjects: the runtime sprite table and the added model frames were just reset.
void Img3dNewLoad();
// Whether `item` has a model (reads the folder on first use).
bool Img3dHas(int item);

// A Realms object's actor: wear the model of the item its render id names, or give its picture back when it no
// longer names one (a repaint). Called after the caller set the actor's picture (sprite, Scale) for that render id.
// True while the model is worn.
bool Img3dWearWorld(AActor *mo, uint16_t renderId, uint8_t rotation, bool fixedAngle);
// What the game picks such an object by: the picture it would show (sprite, Scale, z), never the model
// (roth_runtime.cpp ObjectPickBox, as for a candle's per-wick picture). False when `mo` wears no model.
bool Img3dPickPicture(const AActor *mo, int &sprite, DVector2 &scale, double &z);

// The held cards (roth_held.cpp ShowCard). Slot `slot` (0 .. kImg3dHeldSlots - 1) shows `item`'s model this tic:
// role >= 0 rides that hand's grip frame (hold palm / fist) or stands upright at `at` (hold hang); role -1 stands at
// `at` facing `faceYawDeg`. `fallbackMetres` is the size when the manifest's is not the item's real one. False: no
// model (draw the card).
constexpr int kImg3dHeldSlots = 6;
bool Img3dShowHeld(FLevelLocals *level, int slot, int item, const DVector3 &at, int role, double fallbackMetres, double faceYawDeg);
// Once a tic after the cards: the slots not shown this tic are hidden.
void Img3dHeldEndTick(FLevelLocals *level);
void Img3dEndLevel();

} // namespace roth
