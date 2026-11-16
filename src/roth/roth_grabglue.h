#pragma once
//
// RSQ DISTANCE GRAB IN THE REALMS: the glue, and only the glue (roth_grabglue.cpp says what it is).
//

struct FLevelLocals;

namespace roth
{

// Once a tic from TickLevelLogic: which Realms objects the grab may take (ObjectIsPickupItem), made so.
void GrabGlueTick(FLevelLocals *level);

} // namespace roth
