//
// Realms of the Haunting's sound, through the engine's sound system.
// See roth_sound.h for the shape and roth_sfx.h for the formats and rules.
//
// HOW A REALMS SAMPLE BECOMES AN ENGINE SOUND. Each row of the effect bank is
// registered with the SoundEngine as "roth/sfxNNN" and given a lump number
// from a range no real file can reach. The engine loads sounds lazily, and
// when it wants one of ours DoomSoundEngine::ReadSound asks SoundReadLump,
// which reads that row from the player's install and returns its PCM as it
// stands. Each entry is marked RAW with its own rate and width (bLoadRAW,
// RawRate, b16bit), so the engine hands the bytes straight to the backend.
// So the engine caches, resamples, evicts and reloads our samples exactly as
// it does its own, and nothing is ever written out.
//
// NOT A WAVE IMAGE, which is what this did first and why NOTHING PLAYED on the
// owner's PC (2026-11-12, 1,405 refusals): the only thing in the engine that
// can read a WAVE is libsndfile, loaded at run time if its DLL is there, and
// Doom's own sounds never need it. With no decoder the load failed, the
// engine marked the entry empty, and StartSound returned nothing. Raw PCM
// needs no decoder at all.
//
// HOW A REALMS VOICE BECOMES AN ENGINE CHANNEL. The original mixes in 2D: a
// volume from distance (compute_sound_volume_pan) and a left/right pan from
// the bearing. Here the VOLUME is still the original's, computed by SfxVolume
// and set on the channel every tic, so range, sample volume, zones and the
// 0x200 slew behave as they did. The PAN is the engine's: every positional
// voice is a real 3D source at its map position, started with a rolloff that
// leaves the gain alone (ROLLOFF_Log, factor 0 -> gain 1 at any distance), so
// the spatialiser places it and never attenuates it a second time. The
// original has no height in its sound; a source is put at ear height above
// the floor where it stands.
//
// THE SIXTEEN RECORDS. g_active_sound_handles (0x83ed4, 16 x 0x9a) is kept as
// sixteen Handle structs with the same rules: a new sound that finds none
// free is DROPPED, not stolen (find_free_slot_83ed4, renderer.c:1434); a node
// that is already in the table does not start again (is_in_83ed4_table,
// renderer.c:10877); a record is freed when its voice ends.
//
// CADENCE. update_active_sounds runs once per rendered frame in the original
// and its slew is 0x200 per call, not per unit of time (audio.c:1012-1048).
// It runs once per engine tic here: 35 Hz, which is the original at a frame
// scale of 2, the same scale the rest of this runtime ticks at
// (ROTH_FRAME_TICKS_PER_TIC).
//
// NOT COMPILED BY MSVC in the lane that wrote it. Syntax-checked with g++
// against the engine headers (tools/roth_syntaxcheck.sh), never linked, never
// run, and never run against a retail FXSCRIPT.SFX, DBASE300.DAT or
// DBASE500.DAT: none of the three was in the data that lane could read.
//

#include "roth_sound.h"
#include "roth_sfx.h"
#include "roth_raw.h"
#include "roth_log.h"
#include "roth_install.h"
#include "roth_dbase100.h"

#include <map>
#include <string>
#include <vector>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "g_levellocals.h"
#include "d_player.h"
#include "actor.h"
#include "doomstat.h"
#include "s_soundinternal.h"
#include "i_sound.h"
#include "s_music.h"
#include "i_music.h"
#include "files.h"
#include "playsim/po_man.h"
#include "printf.h"
#include "c_dispatch.h"
#include "c_cvars.h"

// music.cpp's read-from-memory hook; see the note there.
extern bool (*S_MemoryMusicHook)(const char *name, FileReader &reader);

namespace roth
{

// Prints every play request and what became of it. A trace, off by default
// because the node pump asks thirty-five times a second. Not archived.
CVAR(Bool, roth_sound_debug, false, 0)

namespace
{

// Lump numbers for Realms samples. Far above anything a file system holds.
const int LUMP_BASE  = 0x52000000;
const int LUMP_VOICE = 0x52100000;
const int FRAME_SCALE = 2;              // 70 Hz ticks per engine tic

enum EmitterKind
{
	EMIT_EFFECT,        // the static "effect" emitter 0x84907: no position
	EMIT_ENTITY,        // static emitter 0x83eb0, range 2000 (audio.c:1062-1063)
	EMIT_ENTITY_OBJ,    // static emitter 0x83ec2, range 1000
	EMIT_NODE,          // a map sound node
	EMIT_DOOR,          // a door's own sub-record (doors.c:618)
};

// One of the sixteen playing-sound records.
struct Handle
{
	bool used = false;
	EmitterKind kind = EMIT_EFFECT;
	int emitter = 0;            // node index, or door polyobject tag
	uint8_t flags = 0;          // rec+4: 0x20 static, 0x80 own coords, else the node's mode
	uint8_t zone = 0;           // rec+6
	int row = 0;
	int x = 0, y = 0;
	int range = 0;
	uint8_t vol8 = 0x40;
	uint32_t dist2 = 0;         // rec+8
	uint32_t dist = 0;          // rec+0xc
	int32_t applied = 0;        // rec+0x10, what the voice is at
	int32_t target = 0;         // rec+0x12, what it is heading for
	FSoundHandle chan;
	bool loops = false;         // a mode-1 node: requeued at each end while audible
	int lengthTicks = 0;        // the sample, in 70 Hz ticks
	int ticksLeft = 0;          // to the end of this pass through it
};

struct DoorSound
{
	int tag = 0;
	uint16_t closePlusOne = 0, shutPlusOne = 0;
	bool marker = false;        // word[e+0xa] != 0: a door sound is latched
	int phase = 0;              // 0 opening, 1 open, 2 closing
	double startAngle = 0.;
	double lastSwing = 0.;
	int x = 0, y = 0;
};

struct Pending { int what; int a, b, c; };

struct State
{
	SfxBank bank;
	bool bankSearched = false;
	std::string songPath, voicePath;
	bool songSearched = false, voiceSearched = false;
	const uint8_t *resolvedImage = nullptr;     // the DBASE100 image the keys were resolved in

	std::vector<FSoundID> ids;                  // row -> engine sound
	FSoundID voiceId = NO_SOUND;
	std::vector<char> handed;                   // row -> the engine has pulled it at least once

	FLevelLocals *level = nullptr;
	bool levelUp = false;
	SfxSection section;
	Handle handles[16];
	std::vector<DoorSound> doors;
	uint16_t rng = 0;                           // the word at 0x85328
	int rngTicks = 0;

	// music
	uint32_t songIndex = 0;                     // g_current_dbase300_chunk_id
	int songPhase = 0;                          // bit 1 loaded, bit 4 playing (audio.c:1761)
	std::map<uint32_t, std::vector<uint8_t>> songs;

	// speech
	std::vector<uint8_t> voicePcm;              // the current clip, 16-bit mono
	uint32_t voiceClip = 0;
	FSoundHandle voiceChan;

	std::vector<Pending> pending;

	// counters, for roth_sound_status
	int asked = 0, started = 0, dropNoSlot = 0, dropSilent = 0, dropNoSample = 0, dropEngine = 0;
	int enginePulls = 0;
	const char *lastRefusal = "";
	std::map<std::string, int> refusals;        // why the engine said no, by reason
};

State S;

//==========================================================================
//
// Finding the retail files. The rules, and what was measured on the retail
// set, are in roth_sfx.h ("WHERE THE FILES ARE"); the host tool uses the same
// two functions, so what it reports is what the engine will open.
//
//==========================================================================

std::string FindFile(const char *name, std::string *tried)
{
	const Install &inst = TheInstall();
	if (!inst.IsOpen()) { if (tried) *tried = "\n    (no install open)"; return ""; }
	return FindRetailFile(inst.Path(), name, tried);
}

void OpenBank(Log *log)
{
	if (S.bank.IsOpen() || S.bankSearched) return;
	S.bankSearched = true;

	std::string tried, how;
	const std::string path = TheInstall().IsOpen()
		? FindEffectBank(TheInstall().Path(), &how, &tried) : std::string();
	if (path.empty())
	{
		Printf(TEXTCOLOR_RED "roth_sound: NO EFFECT BANK. FXSCRIPT.SFX was not found; Realms sound effects are silent."
			" Looked in:%s\n", tried.c_str());
		if (log) log->Warn("sound: effect bank FXSCRIPT.SFX not found; every sound effect is silent");
		return;
	}
	Printf("roth_sound: effect bank chosen by: %s\n", how.c_str());
	if (!S.bank.Open(path))
	{
		Printf(TEXTCOLOR_RED "roth_sound: %s is not a usable effect bank: %s\n", path.c_str(), S.bank.Error().c_str());
		if (log) log->Warn("sound: %s: %s", path.c_str(), S.bank.Error().c_str());
		return;
	}

	int n11 = 0, n22 = 0, n8 = 0, empty = 0;
	for (int i = 0; i < S.bank.Count(); i++)
	{
		const SfxRow *r = S.bank.Row(i);
		if (!r->Loadable()) { empty++; continue; }
		if (r->flags & 2) n22++; else n11++;
		if (!r->Is16Bit()) n8++;
	}
	Printf("roth_sound: bank %s: %d samples (%d at 11025 Hz, %d at 22050 Hz, %d of them 8-bit, %d empty rows)\n",
		path.c_str(), S.bank.Count(), n11, n22, n8, empty);
	if (log) log->Line("sound: bank %s, %d samples (%d at 11025 Hz, %d at 22050 Hz, %d 8-bit, %d empty)",
		path.c_str(), S.bank.Count(), n11, n22, n8, empty);
}

//==========================================================================
//
// Registration with the SoundEngine.
//
// The engine throws its sound table away and rebuilds it whenever SNDINFO is
// reparsed (S_Start does that on some level changes), so the registration is
// CHECKED before every use and redone when our first sound is no longer ours.
//
//==========================================================================

bool RegistrationIsLive()
{
	if (soundEngine == nullptr || S.ids.empty()) return false;
	// By NAME: the engine sets a sound's lump to "empty" when a load fails,
	// which must not read as "the table was rebuilt".
	const sfxinfo_t *sfx = soundEngine->GetSfx(S.ids[0]);
	return sfx != nullptr && sfx->name == FName("roth/sfx000");
}

bool EnsureRegistered()
{
	if (!S.bank.IsOpen() || soundEngine == nullptr) return false;
	if (RegistrationIsLive()) return true;

	S.ids.clear();
	S.ids.reserve(S.bank.Count());
	for (int i = 0; i < S.bank.Count(); i++)
	{
		char name[32];
		snprintf(name, sizeof(name), "roth/sfx%03d", i);
		// Near limit 0: the original's only limit is its sixteen records.
		const FSoundID id = soundEngine->AddSoundLump(name, LUMP_BASE + i, 0, -1, 0);
		S.ids.push_back(id);
	}
	S.voiceId = soundEngine->AddSoundLump("roth/voice", LUMP_VOICE, 0, -1, 0);
	// Raw PCM, each at its own rate and width. Set after every Add: adding
	// grows the engine's table and moves its entries.
	for (int i = 0; i < S.bank.Count(); i++)
	{
		sfxinfo_t *sfx = soundEngine->GetWritableSfx(S.ids[i]);
		const SfxRow *r = S.bank.Row(i);
		if (sfx == nullptr || r == nullptr) continue;
		sfx->bLoadRAW = true;
		sfx->b16bit = r->Is16Bit();
		sfx->RawRate = r->Rate();
	}
	if (sfxinfo_t *sfx = soundEngine->GetWritableSfx(S.voiceId))
	{
		sfx->bLoadRAW = true;
		sfx->b16bit = true;
		sfx->RawRate = 22050;
	}
	S.handed.assign(S.bank.Count(), 0);
	soundEngine->HashSounds();
	Printf("roth_sound: registered %d samples with the sound engine as roth/sfx000..roth/sfx%03d"
		" (engine sound ids %d..%d)\n", S.bank.Count(), S.bank.Count() - 1,
		S.ids.empty() ? -1 : S.ids.front().index(), S.ids.empty() ? -1 : S.ids.back().index());
	return true;
}

// resolve_dbase100_sound_ids, once per DBASE100 image (game_core.c:428).
void ResolveDbase100Keys(Log *log)
{
	Dbase100 &db = TheDbase100();
	if (!S.bank.IsOpen() || !db.IsLoaded()) return;
	if (S.resolvedImage == db.Image()) return;
	const int n = S.bank.ResolveDbase100(db.MutableImage(), db.Size());
	S.resolvedImage = db.Image();
	Printf("roth_sound: DBASE100 sound keys resolved to sample rows: %d words\n", n);
	if (log) log->Line("sound: DBASE100 sound keys resolved to sample rows: %d words", n);
}

//==========================================================================
//
// The listener, in the original's terms: the player's map position as whole
// units, which is what every distance in audio.c is measured from
// ([0x90a8c] >> 16 and [0x90a94] >> 16).
//
//==========================================================================

AActor *Listener()
{
	if (S.level == nullptr) return nullptr;
	if (consoleplayer >= 0 && consoleplayer < MAXPLAYERS && S.level->PlayerInGame(consoleplayer)
		&& S.level->Players[consoleplayer]->mo != nullptr)
		return S.level->Players[consoleplayer]->mo;
	for (int i = 0; i < MAXPLAYERS; i++)
		if (S.level->PlayerInGame(i) && S.level->Players[i]->mo != nullptr)
			return S.level->Players[i]->mo;
	return nullptr;
}

bool ListenerXY(int &px, int &py)
{
	AActor *mo = Listener();
	if (mo == nullptr) return false;
	px = (int)floor(mo->X());
	py = (int)floor(mo->Y());
	return true;
}

// Where a 2D Realms position is in the engine's 3D: ear height above the floor
// it stands on. The engine's sound points are (x, height, y).
FVector3 SourcePoint(int x, int y)
{
	double z = 0.;
	if (S.level != nullptr)
	{
		sector_t *sec = S.level->PointInSector(DVector2(double(x), double(y)));
		if (sec != nullptr) z = sec->floorplane.ZatPoint(double(x), double(y));
		AActor *mo = Listener();
		z += (mo != nullptr && mo->player != nullptr && mo->player->viewheight > 0.)
			? mo->player->viewheight : 41.;
	}
	return FVector3((float)x, (float)z, (float)y);
}

//==========================================================================
//
// Starting and stopping an engine channel for a record.
//
//==========================================================================

int FindFree()
{
	for (int i = 0; i < 16; i++) if (!S.handles[i].used) return i;
	return -1;
}

bool EmitterIsPlaying(EmitterKind kind, int emitter)
{
	for (const Handle &h : S.handles)
		if (h.used && h.kind == kind && h.emitter == emitter) return true;
	return false;
}

void Release(Handle &h)
{
	if (h.used && soundEngine != nullptr && h.chan.IsValid()) soundEngine->StopSound(h.chan);
	h = Handle();
}

// resolve_sound_sample's outcome (audio.c:423-435): a row that exists and
// has data. The cache it manages is the engine's own here.
bool RowPlayable(int row)
{
	const SfxRow *r = S.bank.Row(row);
	return r != nullptr && r->Loadable();
}

const char *KindName(EmitterKind k)
{
	switch (k)
	{
	case EMIT_EFFECT:     return "effect";
	case EMIT_ENTITY:     return "entity";
	case EMIT_ENTITY_OBJ: return "entity object";
	case EMIT_NODE:       return "node";
	default:              return "door";
	}
}

// WHY THE ENGINE SAID NO. SoundEngine::StartSound returns nothing for a dozen
// different reasons and says which for none of them. This walks the same
// tests in the same order (s_sound.cpp, StartSound) on what can be seen from
// outside, AFTER a refusal, and names the first that fails. Read-only.
const char *RefusalReason(FSoundID id, float vol, bool loops)
{
	if (soundEngine == nullptr) return "there is no sound engine";
	if (!soundEngine->isValidSoundId(id)) return "the engine no longer knows this sound id (its table was rebuilt)";
	if (vol <= 0.f) return "the volume asked for is 0";
	if (nosfx) return "sound effects are off (-nosfx / nosfx)";
	if (!SoundEnabled()) return "sound is disabled (snd_enabled false, or the app is muted in the background)";
	if (soundEngine->NewSoundsBlocked()) return "the engine is blocking new sounds (BlockNewSounds)";
	const sfxinfo_t *sfx = soundEngine->GetSfx(id);
	if (sfx == nullptr) return "the engine has no entry for this sound id";
	if (sfx->Volume <= 0.f) return "the engine's entry has volume 0";
	if (sfx->link != sfxinfo_t::NO_LINK && !sfx->data.isValid()) return "the engine's entry is a link to another sound";
	if (GSnd == nullptr || GSnd->IsNull()) return "the sound device is the null device (no audio output was opened)";
	if (sfx->lumpnum == sfx_empty)
		return "the engine could not load the sample: the backend rejected its data and the entry was marked empty";
	if (!sfx->data.isValid()) return "the sample is not loaded and the engine did not try to load it";
	if (!loops && soundEngine->IsSoundPaused())
		return "sound is PAUSED (the game is paused or the window lost focus); a one-shot is not started while paused";
	return "the sample is loaded and nothing forbids it, but the backend gave no voice (out of sources?)";
}

// start_sound_voice / start_sound_voice_vol. `vol` is 0..0x7fff.
bool StartChannel(Handle &h, int32_t vol, bool positional)
{
	if (!EnsureRegistered() || h.row < 0 || h.row >= (int)S.ids.size()) return false;
	const SfxRow *row = S.bank.Row(h.row);
	// A load that failed once (sound was off, say) leaves the engine's entry
	// marked empty for good. The data is still in the bank: let it ask again.
	if (sfxinfo_t *sfx = soundEngine->GetWritableSfx(S.ids[h.row]))
		if (!sfx->data.isValid()) sfx->lumpnum = LUMP_BASE + h.row;
	const float fvol = float(vol) / 32767.f;
	FSoundHandle handle;
	FSoundChan *chan;
	// TRANSIENT: these are restarted by this file, never by a savegame.
	EChanFlags flags = CHANF_OVERLAP | CHANF_TRANSIENT;
	if (h.loops) flags |= CHANF_LOOP;
	if (positional)
	{
		// Gain 1 at every distance: the volume is ours. See the top of the file.
		FRolloffInfo flat;
		flat.RolloffType = ROLLOFF_Log;
		flat.MinDistance = 1.f;
		flat.RolloffFactor = 0.f;
		const FVector3 pt = SourcePoint(h.x, h.y);
		chan = soundEngine->StartSound(SOURCE_Unattached, nullptr, &pt, CHAN_AUTO, flags,
			S.ids[h.row], fvol, ATTN_NORM, &flat, 1.f, 0.f, &handle);
	}
	else
	{
		chan = soundEngine->StartSound(SOURCE_None, nullptr, nullptr, CHAN_AUTO, flags,
			S.ids[h.row], fvol, ATTN_NONE, nullptr, 1.f, 0.f, &handle);
	}
	if (chan == nullptr)
	{
		S.dropEngine++;
		S.lastRefusal = RefusalReason(S.ids[h.row], fvol, h.loops);
		S.refusals[S.lastRefusal]++;
		if (roth_sound_debug)
			Printf("roth_sound: row %d (%s) REFUSED: %s\n", h.row, KindName(h.kind), S.lastRefusal);
		return false;
	}
	h.chan = handle;
	h.applied = vol;                                     // st16(rec+0x10, vol), audio.c:683
	h.lengthTicks = row != nullptr ? (int)row->Ticks70() : 0;
	h.ticksLeft = h.lengthTicks;
	S.started++;
	if (roth_sound_debug)
	{
		if (positional)
			Printf("roth_sound: play row %d (%s) vol %d/32767 at (%d,%d) range %d -> engine sound %s, handle %d\n",
				h.row, KindName(h.kind), (int)vol, h.x, h.y, h.range,
				soundEngine->GetSoundName(S.ids[h.row]), (int)handle);
		else
			Printf("roth_sound: play row %d (%s) vol %d/32767, no position -> engine sound %s, handle %d\n",
				h.row, KindName(h.kind), (int)vol, soundEngine->GetSoundName(S.ids[h.row]), (int)handle);
	}
	return true;
}

//==========================================================================
//
// The play-entry tower, audio.c:735-921.
//
//==========================================================================

// au_play_effect_body (audio.c:735-754). Full volume; the SFX master the
// original scales by is the engine's own effects volume here.
bool PlayEffectBody(uint32_t idIn)
{
	S.asked++;
	if (!S.bank.IsOpen()) { S.dropNoSample++; return false; }
	const int idx = FindFree();
	const int id = (int)(idIn & 0x7fff);                 // :740
	if (idx < 0) { S.dropNoSlot++; return false; }       // :741
	if (!RowPlayable(id)) { S.dropNoSample++; return false; }    // :748-749
	Handle &h = S.handles[idx];
	h = Handle();
	h.used = true;
	h.kind = EMIT_EFFECT;
	h.flags = 0x20;                                      // :745
	h.row = id;
	if (!StartChannel(h, 0x7fff, false)) { h = Handle(); return false; }
	return true;
}

// play_object_sound (audio.c:778-809).
bool PlayObject(uint32_t idIn, int x, int y, EmitterKind kind)
{
	S.asked++;
	if (!S.bank.IsOpen()) { S.dropNoSample++; return false; }
	const int idx = FindFree();
	const int id = (int)(idIn & 0x7fff);
	if (idx < 0) { S.dropNoSlot++; return false; }       // :783
	int px = 0, py = 0;
	if (!ListenerXY(px, py)) { S.dropSilent++; return false; }

	Handle &h = S.handles[idx];
	h = Handle();
	h.kind = kind;
	h.flags = 0x80;                                      // own coords, :788
	h.row = id;
	h.x = (int)(int16_t)x;
	h.y = (int)(int16_t)y;
	h.range = kind == EMIT_ENTITY_OBJ ? 0x3e8 : 0x7d0;   // audio.c:1062-1063
	h.vol8 = 0x40;
	h.zone = 0;                                          // :795
	h.dist2 = (uint32_t)((h.y - py) * (h.y - py) + (h.x - px) * (h.x - px));    // :794
	const int32_t vol = SfxVolume(h.range, h.vol8, nullptr, h.x, h.y, px, py, h.dist2, 0x7fff, &h.dist);
	if (vol == 0) { S.dropSilent++; h = Handle(); return false; }        // :799
	if (!RowPlayable(id)) { S.dropNoSample++; h = Handle(); return false; }  // :800-801
	h.used = true;
	h.target = vol;
	if (!StartChannel(h, vol, true)) { h = Handle(); return false; }
	return true;
}

// play_sound_unique (audio.c:859-893), for a node.
bool PlayNodeUnique(int nodeIndex, uint32_t dist2)
{
	S.asked++;
	if (!S.bank.IsOpen()) { S.dropNoSample++; return false; }            // :861
	if (nodeIndex < 0 || nodeIndex >= (int)S.section.nodes.size()) return false;
	SfxNode &n = S.section.nodes[nodeIndex];
	if (EmitterIsPlaying(EMIT_NODE, nodeIndex)) { S.asked--; return false; }    // :863, not a request
	const int idx = FindFree();
	if (idx < 0) { S.dropNoSlot++; return false; }       // :865
	int px = 0, py = 0;
	if (!ListenerXY(px, py)) { S.dropSilent++; return false; }

	Handle &h = S.handles[idx];
	h = Handle();
	h.kind = EMIT_NODE;
	h.emitter = nodeIndex;
	h.flags = (uint8_t)(n.flags & 7);                    // :869
	h.zone = n.zone;                                     // :870
	h.row = n.sound;
	h.x = n.x; h.y = n.y;
	h.range = n.range;
	h.vol8 = n.volume;
	h.dist2 = dist2;
	const int mode = h.flags & 7;
	const int32_t vol = SfxVolume(h.range, h.vol8, S.section.Zone(h.zone), h.x, h.y, px, py,
		h.dist2, 0x7fff, &h.dist);
	if (vol == 0)                                        // :874-878
	{
		if (mode == 0) n.flags &= 0x7f;                  // a silent one-shot is spent all the same
		S.dropSilent++;
		h = Handle();
		return false;
	}
	h.target = vol;                                      // :879
	const int32_t evol = (mode == 1) ? SfxLoopStartVolume(vol, n.range) : vol;   // :880-885
	if (!RowPlayable(h.row)) { S.dropNoSample++; h = Handle(); return false; }   // :887-888
	h.used = true;
	h.loops = (mode == 1);
	if (!StartChannel(h, evol, true)) { h = Handle(); return false; }
	if (mode == 0) n.flags &= 0x7f;                      // :891, the one-shot has fired
	return true;
}

// play_sound_unique for a door's sub-record: mode 0, no zone, range 1000,
// volume 0x40 (doors.c:618-634; restart_door_open_sound, audio.c:956-970).
bool PlayDoorUnique(DoorSound &d, int row)
{
	S.asked++;
	if (!S.bank.IsOpen()) { S.dropNoSample++; return false; }
	if (EmitterIsPlaying(EMIT_DOOR, d.tag)) { S.asked--; return false; }
	const int idx = FindFree();
	if (idx < 0) { S.dropNoSlot++; return false; }
	int px = 0, py = 0;
	if (!ListenerXY(px, py)) { S.dropSilent++; return false; }

	Handle &h = S.handles[idx];
	h = Handle();
	h.kind = EMIT_DOOR;
	h.emitter = d.tag;
	h.flags = 0;
	h.row = row;
	h.x = d.x; h.y = d.y;
	h.range = 0x3e8;
	h.vol8 = 0x40;
	h.dist2 = (uint32_t)((h.x - px) * (h.x - px) + (h.y - py) * (h.y - py));     // audio.c:899-903
	const int32_t vol = SfxVolume(h.range, h.vol8, nullptr, h.x, h.y, px, py, h.dist2, 0x7fff, &h.dist);
	if (vol == 0) { S.dropSilent++; h = Handle(); return false; }
	if (!RowPlayable(row)) { S.dropNoSample++; h = Handle(); return false; }
	h.used = true;
	h.target = vol;
	if (!StartChannel(h, vol, true)) { h = Handle(); return false; }
	return true;
}

// stop_sound_handle_voice (audio.c:565-577) / stop_sound_by_id (:581-597):
// the first record of that emitter, stopped and cleared.
bool StopEmitter(EmitterKind kind, int emitter)
{
	for (Handle &h : S.handles)
		if (h.used && h.kind == kind && h.emitter == emitter) { Release(h); return true; }
	return false;
}

//==========================================================================
//
// update_active_sounds, audio.c:1012-1048, and the end of a voice
// (sos_user_callback_trampoline, audio.c:1951-1975).
//
//==========================================================================

void UpdateActive()
{
	int px = 0, py = 0;
	const bool haveListener = ListenerXY(px, py);

	for (Handle &h : S.handles)
	{
		if (!h.used) continue;

		// The voice ran out. A looping node is requeued while it is still
		// active, still mode 1 and still audible (:1962-1968); anything else
		// is done and its record is freed (:1973).
		bool ended = !soundEngine->IsPlaying(h.chan);
		if (!ended && h.loops)
		{
			h.ticksLeft -= FRAME_SCALE;
			if (h.ticksLeft <= 0)
			{
				const SfxNode *n = (h.kind == EMIT_NODE && h.emitter < (int)S.section.nodes.size())
					? &S.section.nodes[h.emitter] : nullptr;
				const bool requeue = n != nullptr && n->Active() && h.applied != 0 && n->Mode() == 1;
				if (requeue) h.ticksLeft += h.lengthTicks > 0 ? h.lengthTicks : 1;
				else ended = true;
			}
		}
		if (ended) { Release(h); continue; }

		if (h.flags & 0x20) continue;                    // :1020, the static emitter is never updated
		if (!haveListener) continue;

		const int32_t relX = h.x - px, relY = h.y - py;
		const int32_t d2 = relX * relX + relY * relY;
		const int32_t delta = (int32_t)h.dist2 - d2;
		int32_t vol = h.target;
		if ((abs(delta) >> 4) != 0)                      // :1030-1034
		{
			vol = SfxVolume(h.range, h.vol8, S.section.Zone(h.zone), h.x, h.y, px, py,
				(uint32_t)d2, 0x7fff, &h.dist);
			h.target = vol;
			h.dist2 = (uint32_t)d2;
		}
		if (vol != h.applied)                            // :1035-1040
		{
			h.applied = SfxClampDiff200(vol, h.applied);
			soundEngine->SetVolume(h.chan, float(h.applied) / 32767.f);
		}
		// The pan (:1041-1047) is the engine's: the source has a position.
	}
}

//==========================================================================
//
// Doors. tick_swinging_doors (doors.c:1030-1147) plays and stops a door's
// sounds at the turns of its swing. The swing itself is the engine's
// polyobject door here (SwingDoor in roth_runtime.cpp), so the turns are read
// off the polyobject: how far it has rotated from where it stood.
//
//   opening -> fully open   stop the door's latched sound (:1087-1091)
//   dwell over, closing     play the close sound, latched (:1049-1063)
//   fully closed            stop the latched sound, then the shut sound as
//                           an entity sound at the door (:1140-1145)
//
// [approximation] The original's position for these is a vertex of the door
// face (doors.c:546-547); this uses the polyobject's start spot, its hinge.
// Which end of the face the original's vertex is was not established.
//
//==========================================================================

double DoorSwing(const DoorSound &d, FPolyObj *po)
{
	double a = fabs(po->Angle.Degrees() - d.startAngle);
	while (a > 180.) a = fabs(a - 360.);
	return a;
}

void TickDoors()
{
	if (S.level == nullptr) return;
	for (size_t i = 0; i < S.doors.size(); )
	{
		DoorSound &d = S.doors[i];
		FPolyObj *po = S.level->GetPolyobj(d.tag);
		if (po == nullptr) { S.doors.erase(S.doors.begin() + i); continue; }
		const double swing = DoorSwing(d, po);
		const bool moving = fabs(swing - d.lastSwing) > 1e-6;
		const bool growing = swing > d.lastSwing + 1e-6;
		bool done = false;

		if (d.phase == 0)                                // opening
		{
			if (!moving && swing > 1.)
			{
				if (d.marker) StopEmitter(EMIT_DOOR, d.tag);
				d.phase = 1;
			}
			else if (!moving && po->specialdata == nullptr) done = true;    // never got going
		}
		else if (d.phase == 1)                           // open, dwelling
		{
			if (moving && !growing)
			{
				d.marker = false;                        // WW(e + 0xa, 0), doors.c:1056
				if (d.closePlusOne != 0)
				{
					d.marker = true;                     // WW(e + 0xa, 0x3e8), :1060
					PlayDoorUnique(d, (int)d.closePlusOne - 1);
				}
				d.phase = 2;
			}
			else if (po->specialdata == nullptr) done = true;   // the engine dropped the door thinker
		}
		else                                             // closing
		{
			if (growing) d.phase = 0;                    // blocked and swung back (doors.c "revert")
			else if (!moving && swing < 0.01)
			{
				if (d.marker) StopEmitter(EMIT_DOOR, d.tag);
				if (d.shutPlusOne != 0) PlayObject((uint32_t)d.shutPlusOne - 1, d.x, d.y, EMIT_ENTITY);
				done = true;
			}
		}
		d.lastSwing = swing;
		if (done) S.doors.erase(S.doors.begin() + i); else i++;
	}
}

//==========================================================================
//
// Music.
//
//==========================================================================

std::string FindSongFile()
{
	if (!S.songSearched)
	{
		S.songSearched = true;
		std::string tried;
		S.songPath = FindFile("DBASE300.DAT", &tried);
		if (S.songPath.empty())
			Printf(TEXTCOLOR_RED "roth_sound: NO MUSIC. DBASE300.DAT was not found. Looked in:%s\n", tried.c_str());
		else
			Printf("roth_sound: songs from %s\n", S.songPath.c_str());
	}
	return S.songPath;
}

// "roth:song:<chunk index>" -> the chunk's bytes, for the music player.
bool MemoryMusic(const char *name, FileReader &reader)
{
	if (name == nullptr || strncmp(name, "roth:song:", 10) != 0) return false;
	const uint32_t index = (uint32_t)strtoul(name + 10, nullptr, 10);
	auto it = S.songs.find(index);
	if (it == S.songs.end())
	{
		const std::string path = FindSongFile();
		if (path.empty()) return false;
		std::string why;
		std::vector<uint8_t> bytes = ReadDbase300Chunk(path, index, &why);
		if (bytes.empty()) return false;
		it = S.songs.emplace(index, std::move(bytes)).first;
	}
	// The map keeps the bytes alive for as long as any reader could want them.
	return reader.OpenMemory(it->second.data(), (FileReader::Size)it->second.size());
}

//==========================================================================
//
// The console commands' queue. Typed from an +exec script they run before
// the deferred `map`, so they are queued and drained from the level tick, as
// roth_dialogue's are.
//
//==========================================================================

enum { P_ROW, P_ROW_AT, P_KEY, P_MUSIC, P_VOICE, P_STATUS, P_NODES };

void PrintStatus()
{
	Printf("roth_sound_status: bank %s\n", S.bank.IsOpen() ? S.bank.Path().c_str() : "NOT LOADED");
	Printf("  samples %d, registered with the engine: %s (%d ids), pulled by the engine so far: %d\n",
		S.bank.Count(), RegistrationIsLive() ? "yes" : "NO", (int)S.ids.size(), S.enginePulls);
	int active = 0, mode[8] = { 0 };
	for (const SfxNode &n : S.section.nodes) { if (n.Active()) { active++; mode[n.Mode()]++; } }
	Printf("  this map: %d sound nodes (%d active: %d one-shot, %d looping, %d periodic, %d random-periodic), %d zones\n",
		(int)S.section.nodes.size(), active, mode[0], mode[1], mode[2],
		mode[3] + mode[4] + mode[5] + mode[6] + mode[7], (int)S.section.zones.size());
	int used = 0;
	for (const Handle &h : S.handles)
	{
		if (!h.used) continue;
		used++;
		Printf("  record: %s %d row %d at (%d,%d) range %d vol %d -> %d, engine handle %d %s\n",
			KindName(h.kind), h.emitter, h.row, h.x, h.y, h.range, (int)h.applied, (int)h.target,
			(int)h.chan, soundEngine != nullptr && soundEngine->IsPlaying(h.chan) ? "playing" : "ended");
	}
	Printf("  records in use %d of 16; doors followed %d\n", used, (int)S.doors.size());
	Printf("  asked %d, started %d, dropped: %d no free record, %d silent or out of range,"
		" %d no such sample, %d refused by the engine\n",
		S.asked, S.started, S.dropNoSlot, S.dropSilent, S.dropNoSample, S.dropEngine);
	for (const auto &kv : S.refusals)
		Printf("  refused x%d: %s\n", kv.second, kv.first.c_str());
	Printf("  music: chunk %u, %s; file %s\n", (unsigned)S.songIndex,
		(S.songPhase & 4) ? "playing" : (S.songPhase & 1) ? "loaded" : "none",
		S.songPath.empty() ? "(not found or not looked for)" : S.songPath.c_str());
}

void DrainPending()
{
	if (S.pending.empty()) return;
	std::vector<Pending> batch;
	batch.swap(S.pending);
	for (const Pending &p : batch)
	{
		switch (p.what)
		{
		case P_ROW:
		case P_ROW_AT:
		{
			const SfxRow *r = S.bank.Row(p.a);
			if (r == nullptr)
			{
				Printf("roth_sound: no sample row %d (the bank has %d)\n", p.a, S.bank.Count());
				break;
			}
			const int before = S.enginePulls;
			const int refusedBefore = S.dropEngine;
			const bool ok = p.what == P_ROW ? SoundPlayEffect((uint32_t)p.a, 0)
				: SoundPlayEntity((uint32_t)p.a, p.b, p.c);
			Printf("roth_sound: row %d (key %u, %u bytes, %d Hz, %d-bit, %.2f s) %s%s\n", p.a,
				(unsigned)r->key, (unsigned)r->size, r->Rate(), r->Is16Bit() ? 16 : 8,
				double(r->Frames()) / double(r->Rate()),
				ok ? "STARTED on an engine channel" : "did NOT start",
				S.enginePulls != before ? " (the engine loaded its data just now)" : "");
			if (!ok && S.dropEngine != refusedBefore) Printf("roth_sound:   why: %s\n", S.lastRefusal);
			break;
		}
		case P_KEY:
		{
			const int idx = S.bank.IndexForKey((uint16_t)p.a);
			Printf("roth_sound_key: key %d is %s", p.a, idx == 0 ? "not in the bank\n" : "");
			if (idx != 0) { Printf("row %d\n", idx - 1); S.pending.push_back({ P_ROW, idx - 1, 0, 0 }); }
			break;
		}
		case P_MUSIC:
		{
			const uint32_t r = MusicLoadChunk((uint32_t)p.a);
			if (r == 0) MusicProcess();
			Printf("roth_music: chunk %d: load returned %u (%s); now %s\n", p.a, (unsigned)r,
				r == 0 ? "freshly loaded" : "already current, or failed",
				(S.songPhase & 4) ? "PLAYING through the music player" : "not playing");
			break;
		}
		case P_VOICE:
			Printf("roth_voice: clip %d %s\n", p.a,
				SoundPrimeVoiceClip((uint32_t)p.a) ? "STARTED on an engine channel" : "did not start");
			break;
		case P_NODES:
		{
			int px = 0, py = 0;
			ListenerXY(px, py);
			Printf("roth_sound_nodes: player at (%d,%d)\n", px, py);
			for (size_t i = 0; i < S.section.nodes.size(); i++)
			{
				const SfxNode &n = S.section.nodes[i];
				const double d = sqrt(double(n.x - px) * (n.x - px) + double(n.y - py) * (n.y - py));
				if (p.a == 0 && d > double(n.range)) continue;
				Printf("  node %d at (%d,%d) row %u key %u mode %d %s range %u period %u vol %u zone %u  dist %.0f%s\n",
					(int)i, n.x, n.y, (unsigned)n.sound, (unsigned)n.key, n.Mode(),
					n.Active() ? "active" : "off", (unsigned)n.range, (unsigned)n.period,
					(unsigned)n.volume, (unsigned)n.zone, d,
					EmitterIsPlaying(EMIT_NODE, (int)i) ? "  PLAYING" : "");
			}
			break;
		}
		default:
			PrintStatus();
			break;
		}
	}
}

} // namespace

//==========================================================================
//
// Public: level lifecycle
//
//==========================================================================

void SoundBeginLevel(const Map &map, FLevelLocals *level, Log *log)
{
	S.level = level;
	S.levelUp = true;
	for (Handle &h : S.handles) h = Handle();    // the engine stops every channel at level start
	S.doors.clear();
	S_MemoryMusicHook = MemoryMusic;

	OpenBank(log);
	EnsureRegistered();
	ResolveDbase100Keys(log);

	S.section.Parse(map.sfxSection.data(), map.sfxSection.size());
	// The loader recentres a map that straddles the 16-bit wrap; nodes follow
	// the vertices (roth_raw.cpp, RecentreWrappedMap).
	if (map.wrapShiftX != 0 || map.wrapShiftY != 0)
		for (SfxNode &n : S.section.nodes)
		{
			n.x = (int16_t)(uint16_t)((n.x + map.wrapShiftX) & 0xFFFF);
			n.y = (int16_t)(uint16_t)((n.y + map.wrapShiftY) & 0xFFFF);
		}
	S.section.Setup();                           // setup_sfx_nodes, map_load.c:916

	int active = 0, mode[8] = { 0 }, badRow = 0;
	for (const SfxNode &n : S.section.nodes)
	{
		if (n.Active()) { active++; mode[n.Mode()]++; }
		if (S.bank.IsOpen() && !RowPlayable(n.sound)) badRow++;
	}
	Printf("roth_sound: %d sound nodes (%d active: %d one-shot, %d looping, %d periodic, %d random-periodic), %d zones\n",
		(int)S.section.nodes.size(), active, mode[0], mode[1], mode[2],
		mode[3] + mode[4] + mode[5] + mode[6] + mode[7], (int)S.section.zones.size());
	if (log)
	{
		log->Line("sound: %d nodes (%d active: mode0 %d, mode1 %d, mode2 %d, mode3+ %d), %d zones",
			(int)S.section.nodes.size(), active, mode[0], mode[1], mode[2],
			mode[3] + mode[4] + mode[5] + mode[6] + mode[7], (int)S.section.zones.size());
		if (!S.section.error.empty()) log->Warn("sound: section 7: %s", S.section.error.c_str());
		if (badRow != 0) log->Count("sound: node names a sample row the bank does not have", badRow);
	}
}

void SoundEndLevel()
{
	for (Handle &h : S.handles) Release(h);
	S.doors.clear();
	S.section = SfxSection();
	S.levelUp = false;
	S.level = nullptr;
}

void SoundTick(FLevelLocals *level)
{
	if (!S.levelUp || level == nullptr || level != S.level || soundEngine == nullptr) return;

	DrainPending();

	// The word at 0x85328: one LCG step whenever seven or more 70 Hz ticks
	// have gone by since the last (renderer.c:13413-13417).
	S.rngTicks += FRAME_SCALE;
	if (S.rngTicks >= 7) { S.rngTicks = 0; S.rng = SfxRngStep(S.rng); }

	// play_nearby_sfx_emitters (audio.c:940-950), from update_player_tick.
	int px = 0, py = 0;
	if (ListenerXY(px, py) && !S.section.nodes.empty())
	{
		const std::vector<SfxQueryEntry> found =
			SfxNodePump(S.section, px, py, (uint16_t)FRAME_SCALE, S.rng);
		for (const SfxQueryEntry &e : found) PlayNodeUnique(e.node, e.dist2);   // :925-937
	}

	TickDoors();
	UpdateActive();

	// The speech record is not one of the sixteen; forget it when it ends.
	if (S.voiceChan.IsValid() && !soundEngine->IsPlaying(S.voiceChan)) S.voiceChan = FSoundHandle();
}

//==========================================================================
//
// Public: play entries
//
//==========================================================================

bool SoundPlayEffect(uint32_t id, uint32_t param) { (void)param; return PlayEffectBody(id); }
bool SoundPlayPersistent(uint32_t id, uint32_t param) { (void)param; return PlayEffectBody(id); }
bool SoundPlayEntity(uint32_t id, int x, int y) { return PlayObject(id, x, y, EMIT_ENTITY); }
bool SoundPlayEntityObject(uint32_t id, int x, int y) { return PlayObject(id, x, y, EMIT_ENTITY_OBJ); }
bool SoundPlayCommand(uint32_t id, int x, int y) { return PlayObject(id, x, y, EMIT_ENTITY); }

//==========================================================================
//
// Public: nodes
//
//==========================================================================

int SoundActivateNodes(uint16_t key, uint8_t dh)
{
	// collect_sfx_nodes_by_key (audio.c:154-177) with a capacity of 0xc8
	// words, 0xc6 entries, then the loop at raw_commands.c:4864-4874.
	//
	// The original's scan tests node 0 even when the list is empty (the count
	// is checked at the bottom). An empty list has no node 0 to test here.
	int matched = 0;
	for (size_t i = 0; i < S.section.nodes.size() && matched < 0xc6; i++)
	{
		SfxNode &n = S.section.nodes[i];
		if (n.key != key) continue;
		matched++;
		const uint8_t old = n.flags;
		n.flags ^= 0x80;                                 // :4868
		if (dh & 0x6)                                    // :4869-4872
		{
			n.flags |= 0x80;
			if (dh & 0x4) n.flags &= 0x7f;
		}
		if (!(n.flags & 0x80) && (old & 0x80) && (dh & 1))       // :4873-4874
			StopEmitter(EMIT_NODE, (int)i);
		if (roth_sound_debug)
			Printf("roth_sound: node %d (key %u) %s\n", (int)i, (unsigned)key,
				(n.flags & 0x80) ? "ACTIVE" : "off");
	}
	return matched;
}

int SoundFindNodeByKey(uint16_t key)
{
	return S.section.FindByKey(key);
}

void SoundMoverFinalize(int node)
{
	if (node < 0 || node >= (int)S.section.nodes.size()) return;
	SfxNode &n = S.section.nodes[node];
	n.flags |= 0x80;                                     // raw_commands.c:2549
	// play_world_sound_squared_dist (audio.c:916-921): dist^2 from the player,
	// then play_sound_unique.
	int px = 0, py = 0;
	if (!ListenerXY(px, py)) return;
	const int dx = n.x - px, dy = n.y - py;
	PlayNodeUnique(node, (uint32_t)(dx * dx + dy * dy));
}

void SoundMoverExit(int node, bool wasStarted, uint16_t endSoundPlusOne, bool asCommandSound)
{
	if (node < 0 || node >= (int)S.section.nodes.size()) return;
	SfxNode &n = S.section.nodes[node];
	if (wasStarted)                                      // raw_commands.c:2520-2527
	{
		if ((n.flags & 3) && (n.flags & 0x80))
		{
			n.flags = (uint8_t)(n.flags - 0x80);
			StopEmitter(EMIT_NODE, node);
		}
	}
	if (endSoundPlusOne != 0)                            // :2528-2533, 2756-2762
	{
		if (asCommandSound) SoundPlayCommand((uint32_t)endSoundPlusOne - 1, n.x, n.y);
		else SoundPlayEntity((uint32_t)endSoundPlusOne - 1, n.x, n.y);
	}
}

//==========================================================================
//
// Public: doors
//
//==========================================================================

void SoundDoorOpened(int polyTag, uint16_t openPlusOne, uint16_t closePlusOne, uint16_t shutPlusOne)
{
	if (S.level == nullptr) return;
	FPolyObj *po = S.level->GetPolyobj(polyTag);
	if (po == nullptr) return;
	for (const DoorSound &d : S.doors) if (d.tag == polyTag) return;    // already swinging

	DoorSound d;
	d.tag = polyTag;
	d.closePlusOne = closePlusOne;
	d.shutPlusOne = shutPlusOne;
	// Called after the swing is registered and before it has moved, so the
	// angle now is the closed one.
	d.startAngle = po->Angle.Degrees();
	d.x = (int)floor(po->StartSpot.pos.X);
	d.y = (int)floor(po->StartSpot.pos.Y);
	if (openPlusOne != 0)                                // doors.c:628-634
	{
		d.marker = true;
		PlayDoorUnique(d, (int)openPlusOne - 1);
	}
	S.doors.push_back(d);
}

//==========================================================================
//
// Public: music
//
//==========================================================================

uint32_t MusicLoadChunk(uint32_t index)
{
	S_MemoryMusicHook = MemoryMusic;
	if (index == S.songIndex) return 1;                  // file_config.c:598
	S.songIndex = index;                                 // :599
	// stop_music_sequence (:600; audio.c:1787-1797).
	if (S.songPhase & 4) S_StopMusic(true);
	S.songPhase = 0;
	if (index == 0) return 0;                            // :601

	const std::string path = FindSongFile();
	if (path.empty()) return 1;                          // the open failed, :604-612
	if (S.songs.find(index) == S.songs.end())
	{
		std::string why;
		std::vector<uint8_t> bytes = ReadDbase300Chunk(path, index, &why);
		if (bytes.empty())
		{
			Printf(TEXTCOLOR_RED "roth_sound: song chunk %u: %s\n", (unsigned)index, why.c_str());
			return 1;                                    // :617-621 (the original answers 0 for
			                                             // "too big" and loads nothing; 1 here so
			                                             // the caller does not start a song that
			                                             // is not there)
		}
		const bool hmp = bytes.size() >= 8 && memcmp(bytes.data(), "HMIMIDIP", 8) == 0;
		Printf("roth_sound: song chunk %u loaded: %u bytes, %s\n", (unsigned)index,
			(unsigned)bytes.size(), hmp ? "HMIMIDIP (HMP)" : "NOT an HMP song");
		S.songs.emplace(index, std::move(bytes));
	}
	S.songPhase = 1;                                     // :622
	return 0;
}

void MusicProcess()
{
	if (!(S.songPhase & 1)) return;                      // audio.c:1767
	if (S.songPhase & 4) return;                         // :1768
	char name[40];
	snprintf(name, sizeof(name), "roth:song:%u", (unsigned)S.songIndex);
	// Every song loops for ever: service_audio_sequence restarts it whenever
	// the driver reports it ended (audio.c:1826-1833).
	const bool ok = S_ChangeMusic(name, 0, true, true);
	if (ok) S.songPhase |= 4;                            // :1783
	Printf("roth_sound: music chunk %u %s (%s)\n", (unsigned)S.songIndex,
		ok ? "handed to the music player" : "was REFUSED by the music player", name);
}

uint32_t MusicCurrentChunk() { return S.songIndex; }

bool SoundLevelKeepsMusic(const FLevelLocals *level)
{
	if (!S.levelUp || level != S.level) return false;
	// Realms' own song carries on across the load. Anything else that was
	// playing (the engine's title music) is not Realms' and must not follow
	// the player in: the original is silent until a script asks for a song.
	if (mus_playing.handle != nullptr && strncmp(mus_playing.name.GetChars(), "roth:song:", 10) != 0)
		S_StopMusic(true);
	return true;
}

//==========================================================================
//
// Public: speech
//
//==========================================================================

uint32_t SoundPrimeVoiceClip(uint32_t clip)
{
	if (soundEngine == nullptr || !EnsureRegistered()) return 0;
	// state 1 = a clip is playing: refuse (audio.c:1211).
	if (S.voiceChan.IsValid() && soundEngine->IsPlaying(S.voiceChan)) return 0;

	if (!SoundSpeechAvailable()) return 0;

	VoiceClip v = ReadDbase500Clip(S.voicePath, clip, true);
	if (!v.ok || v.pcm.empty())
	{
		Printf("roth_sound: speech clip %u: %s\n", (unsigned)clip, v.error.empty() ? "empty" : v.error.c_str());
		return 0;
	}

	// One engine sound holds whichever clip is current: drop the old data so
	// the engine asks SoundReadLump for the new.
	sfxinfo_t *sfx = soundEngine->GetWritableSfx(S.voiceId);
	if (sfx == nullptr) return 0;
	soundEngine->UnloadSound(sfx);
	sfx->lumpnum = LUMP_VOICE;
	sfx->bLoadRAW = true;
	sfx->b16bit = true;
	sfx->RawRate = v.rate;
	S.voicePcm.assign((const uint8_t *)v.pcm.data(), (const uint8_t *)v.pcm.data() + v.pcm.size() * 2);
	S.voiceClip = clip;

	FSoundHandle handle;
	FSoundChan *chan = soundEngine->StartSound(SOURCE_None, nullptr, nullptr, CHAN_VOICE,
		CHANF_OVERLAP | CHANF_TRANSIENT, S.voiceId, 1.f, ATTN_NONE, nullptr, 1.f, 0.f, &handle);
	if (chan == nullptr)
	{
		S.lastRefusal = RefusalReason(S.voiceId, 1.f, false);
		S.refusals[S.lastRefusal]++;
		Printf("roth_sound: speech clip %u REFUSED: %s\n", (unsigned)clip, S.lastRefusal);
		return 0;
	}
	S.voiceChan = handle;
	if (roth_sound_debug)
		Printf("roth_sound: speech clip %u: %u samples at %d Hz (%s), engine handle %d\n", (unsigned)clip,
			(unsigned)v.pcm.size(), v.rate, v.dpcm ? "DPCM" : "PCM", (int)handle);
	return 1;
}

// The speech player as the dialogue layer needs it (roth_dialogue's
// SpeechHeader / VoiceStarted / VoiceStopped / VoiceEnded). The dialogue owns
// the stream STATE (g_voice_stream_state); these only read and play.

bool SoundSpeechAvailable()
{
	if (!S.voiceSearched)
	{
		S.voiceSearched = true;
		std::string tried;
		S.voicePath = FindFile("DBASE500.DAT", &tried);
		if (S.voicePath.empty())
			Printf(TEXTCOLOR_RED "roth_sound: NO SPEECH. DBASE500.DAT was not found. Looked in:%s\n", tried.c_str());
		else
			Printf("roth_sound: speech from %s\n", S.voicePath.c_str());
	}
	return !S.voicePath.empty();
}

bool SoundSpeechHeader(uint32_t clip, uint8_t hdr[0x2c])
{
	if (!SoundSpeechAvailable()) return false;
	return ReadDbase500Header(S.voicePath, clip, hdr);
}

bool SoundVoicePlaying()
{
	return soundEngine != nullptr && S.voiceChan.IsValid() && soundEngine->IsPlaying(S.voiceChan);
}

void SoundVoiceStop()
{
	if (soundEngine != nullptr && S.voiceChan.IsValid()) soundEngine->StopSound(S.voiceChan);
	S.voiceChan = FSoundHandle();
}

//==========================================================================
//
// Public: the engine's request for a sample's bytes
//
//==========================================================================

bool SoundReadLump(int lumpnum, TArray<uint8_t> &out)
{
	if (lumpnum < LUMP_BASE || lumpnum > LUMP_VOICE) return false;
	std::vector<uint8_t> pcm;
	if (lumpnum == LUMP_VOICE)
	{
		pcm = S.voicePcm;
	}
	else
	{
		const int row = lumpnum - LUMP_BASE;
		pcm = S.bank.Read(row);
		const SfxRow *r = S.bank.Row(row);
		S.enginePulls++;
		if (r != nullptr && row < (int)S.handed.size() && !S.handed[row])
		{
			S.handed[row] = 1;
			if (roth_sound_debug)
				Printf("roth_sound: sample row %d handed to the sound engine: %u bytes raw PCM, %d Hz, %d-bit\n",
					row, (unsigned)pcm.size(), r->Rate(), r->Is16Bit() ? 16 : 8);
		}
	}
	out.Resize((unsigned)pcm.size());
	if (!pcm.empty()) memcpy(out.Data(), pcm.data(), pcm.size());
	return true;
}

//==========================================================================
//
// Console commands. All queued; see DrainPending.
//
//   roth_sound <row>            play_sound_effect(row): no position
//   roth_sound <row> <x> <y>    play_entity_sound(row, x, y): a 3D source there
//   roth_sound_key <key>        the row a DBASE100 key names, then play it
//   roth_sound_nodes [all]      the map's nodes within their own range of you
//   roth_sound_status           the bank, the registration, the 16 records
//   roth_music <chunk>          load_dbase300_chunk + start, as DBASE100 0x1a
//   roth_voice <clip>           prime_voice_clip
//
//==========================================================================

CCMD(roth_sound)
{
	if (argv.argc() < 2) { Printf("roth_sound <row> [x y]\n"); return; }
	if (argv.argc() >= 4) S.pending.push_back({ P_ROW_AT, atoi(argv[1]), atoi(argv[2]), atoi(argv[3]) });
	else S.pending.push_back({ P_ROW, atoi(argv[1]), 0, 0 });
}

CCMD(roth_sound_key)
{
	if (argv.argc() < 2) { Printf("roth_sound_key <DBASE100 sound key>\n"); return; }
	S.pending.push_back({ P_KEY, atoi(argv[1]), 0, 0 });
}

CCMD(roth_sound_nodes)
{
	S.pending.push_back({ P_NODES, argv.argc() >= 2 ? 1 : 0, 0, 0 });
}

CCMD(roth_sound_status)
{
	S.pending.push_back({ P_STATUS, 0, 0, 0 });
}

CCMD(roth_music)
{
	if (argv.argc() < 2) { Printf("roth_music <DBASE300 chunk index>; 0 stops\n"); return; }
	S.pending.push_back({ P_MUSIC, atoi(argv[1]), 0, 0 });
}

CCMD(roth_voice)
{
	if (argv.argc() < 2) { Printf("roth_voice <DBASE500 clip index>\n"); return; }
	S.pending.push_back({ P_VOICE, atoi(argv[1]), 0, 0 });
}

} // namespace roth
