#pragma once
//
// Realms of the Haunting's movies in the engine: the game flow asks for a
// movie (FlowHost::PlayMovie), it is queued here, and on the next level tic
// the queue is played as one blocking cutscene (gamestate GS_INTRO: the level
// does not tick while it runs, as the original's play_gdv_cutscene does not
// return until the movie is over, gdv_cutscene.c:2840). The decoder is
// roth_gdv (engine-free, differential-tested against the original); the
// player is GdvPlayer in common/cutscenes/movieplayer.cpp, opened by the name
// "rothgdv:<NAME>", which reads the movie whole from the player's install
// into memory -- it is never written anywhere.
//
// Flat, the cutscene fills the screen like every engine movie; in VR the
// engine shows any non-level gamestate on its virtual screen
// (vk_openxrdevice.cpp DetermineFrameRenderMode), which is where it appears.
//

#include <stdint.h>
#include <string>
#include <vector>

namespace roth
{

// What the player needs for one movie.
struct MovieSource
{
	std::string name;                   // the GDV file stem, e.g. "INTRO"
	std::vector<uint8_t> bytes;         // the whole .GDV
	std::vector<uint8_t> subtitles;     // the cutscene entry's DBASE400 range; empty = none
	uint8_t gamePalette[0x300] = {};    // 6-bit: what the subtitle colour byte indexes
	bool haveGamePalette = false;
};

// Read a queued (or test) movie: <install>/GDV/<name>.GDV, and its subtitle
// stream if one was queued with it. False with `error` set when it cannot.
bool MovieFetch(const char *name, MovieSource &out, std::string &error);

// The player reports a movie's end: how many records it decoded and whether
// the player skipped it (Esc, run_timed_message_sequence mode 2,
// dialogue_ui.c:1890-1893).
void MovieFinished(const char *name, int records, int recordCount, bool skipped);

// The game flow's request (gdv_cutscene.c:2840): queued, played on the next tic.
void MovieQueue(const char *name, uint32_t subtitleOffset, int32_t subtitleSize);

// Once per level tic (roth_runtime.cpp TickLevelLogic): start what is queued.
void MoviePump();

// A movie cutscene is queued or running.
bool MovieBusy();

} // namespace roth
