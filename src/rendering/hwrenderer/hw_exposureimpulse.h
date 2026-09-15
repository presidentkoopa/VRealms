/*
** hw_exposureimpulse.h
**
** [EXPOSUREIMPULSE] Flash blindness: a flash a mod marks washes the viewer's eyes out for a moment.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** "Engine docs/SENSORY_IMPULSES_PLAN.md" 2 (with its OWNER ANSWERS of 2026-09-15, which win) and
** "Engine docs/EXPOSURE_IMPULSE_SI_L_IMPL_NOTES.md". A presentation capability named for what it does: RS_Ballistics' flashes
** and impacts are its first callers; any explosion, lightning or scripted flash-bang can call the same thing.
**
** WHERE A FLASH COMES FROM.
**   - LevelLocals.ExposureImpulse (doombase.zs): fire and forget, queued one-way on the level (FExposureImpulseEvent,
**     g_levellocals.h; the native clamps it, vmthunks.cpp) and read here from this reader's own cursor.
**   - r_exposureimpulse_test: a local test source ahead of the head.
**
** ONE FRAME (BeginFrame; the main view drawn to the screen only, once, after VRMode::SetUp wrote the head pose):
**   1. A new level serial (a map change or savegame load) clears the wash and the cursor.
**   2. The queue: each event not read before and no more than 9 tics old is weighed for THIS viewer -- distance, facing, a
**      LevelRay sight test -- into one dose (hw_exposureimpulsecore.h); the test source adds its flashes.
**   3. The envelope merges the frame's doses, holds, rises, recovers and snaps to idle -- never a strobe.
**   4. The frame is published to PPExposureImpulse (hw_postprocess.h): the wash the pass after bloom draws in every eye, the
**      look's numbers, the burst (the pass latches the darkness once per burst) and the fallback darkness from the view
**      sector's light. Nothing live: an empty frame, and the pass does not run.
** A save picture publishes an empty frame (PublishNone); camera textures never post-process.
**
** THE OWNER'S SETTINGS, all renderer-read every frame (a menu freezes the game; these still change what is on screen):
**   r_exposureimpulse                 "Flash blindness": off by default (owner, 2026-09-15)
**   r_exposureimpulse_look            "Flash blindness look": 0 Strong wash (default), 1 Near white-out, 2 Quick sting
**   r_exposureimpulse_comfort         "Flash blindness comfort (slower, softer)"
**   r_exposureimpulse_strength        "Flash blindness strength": 0..2
**   r_exposureimpulse_cap             "Flash blindness limit": 0.1..0.6
**   r_exposureimpulse_recovery_scale  "Flash blindness recovery": 0.5..2 times the look's own
**   r_exposureimpulse_holdbeams       "Lasers stay crisp while flashed": on by default (the protected laser look)
**   r_exposureimpulse_test            a test source, not saved; r_exposureimpulse_debug one log line per burst, not saved
**
** NETPLAY. Presentation only: the native queues an event and returns nothing; this reads the LOCAL VIEW (the frame's
** viewpoint), never names the console player, writes nothing but its own state and the post-process frame, uses no RNG,
** reads nothing back from the GPU and saves nothing. The real clock drives it, so a menu or a pause does not freeze a wash.
**
*/

#pragma once

#include <cstdint>
#include "hw_exposureimpulsecore.h"

struct FLevelLocals;
struct FRenderViewpoint;

class ExposureImpulses
{
public:
	static ExposureImpulses& Get();

	// Once per displayed frame: the main view drawn to the screen, after VRMode::SetUp and before the eye loop.
	void BeginFrame(FLevelLocals* Level, const FRenderViewpoint& vp, uint64_t levelSerial);

	// A save picture (the main view drawn off screen): an empty frame for the pass, nothing else changed, so the picture shows
	// no wash and the next real frame goes on as before.
	void PublishNone();

private:
	ExposureImpulseCore::Envelope mEnvelope;
	uint64_t mLevelSerial = 0;
	uint64_t mQueueSerial = 0;
	int mQueueCount = 0;
	double mLastNow = -1.0;
	double mTestCarry = 0.0;
	double mBurstStart = 0.0;
	double mBurstPeak = 0.0;
};
