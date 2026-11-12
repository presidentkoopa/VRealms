#pragma once
//
// Realms of the Haunting's sound, played through the engine's sound system.
//
// roth_sfx.{h,cpp} reads the retail files and holds the rules; this is the
// side that owns engine objects: it registers every sample of the effect bank
// with the SoundEngine, keeps the original's sixteen playing-sound records,
// runs the sound nodes each tick, follows the doors, and hands songs and
// speech to the music player and the sound engine. The entry points carry the
// names of the ROTH.C functions they stand for (engine/audio.c).
//
// Samples, songs and speech are read from the player's install into memory.
// Nothing is written to disk and no engine sound is ever substituted for a
// Realms one: a sound that cannot be played is counted and stays silent.
//

#include <stdint.h>

struct FLevelLocals;
template<class T> class TArray;

namespace roth
{

struct Map;
class Log;

// ---- level lifecycle (called by roth_runtime) ----------------------------

// Parse the map's sound nodes, find and open the effect bank on first use,
// register its samples, and resolve DBASE100's sound keys once.
void SoundBeginLevel(const Map &map, FLevelLocals *level, Log *log);
void SoundEndLevel();

// Once per engine tic, from the level tick: the node pump
// (play_nearby_sfx_emitters), update_active_sounds, the door followers, and
// the queued console commands.
void SoundTick(FLevelLocals *level);

// ---- the play entry points -----------------------------------------------
// `id` is a sample ROW, as every original caller passes it (stored id - 1).
// Each returns true when a voice actually started.

// play_sound_effect (audio.c:758): everywhere at once, no position.
bool SoundPlayEffect(uint32_t id, uint32_t param);
// start_persistent_looping_sound (audio.c:767): the same body, group 0x20.
bool SoundPlayPersistent(uint32_t id, uint32_t param);
// play_entity_sound (audio.c:829): at a map position, heard within 2000.
bool SoundPlayEntity(uint32_t id, int x, int y);
// play_entity_object_sound (audio.c:836): the same, heard within 1000.
bool SoundPlayEntityObject(uint32_t id, int x, int y);
// play_command_sound (audio.c:844): as play_entity_sound, group 0x64.
bool SoundPlayCommand(uint32_t id, int x, int y);

// ---- sound nodes ------------------------------------------------------------

// cmd_activate_sfx_node (raw_commands.c:4855-4879), all but the record's own
// disable bit: toggle or force the active bit of every node whose key is
// `key`, per the record's +0x06 byte, stopping a voice it just turned off
// when bit 0 asks. Returns how many nodes matched (0 = the command did
// nothing).
int SoundActivateNodes(uint16_t key, uint8_t recFlags6);

// find_sfx_node_by_key (renderer.c:10889): the index of the first node with
// this key, or -1. For the movers that link one (0x07 at +0x10, 0x03 at +0x14).
int SoundFindNodeByKey(uint16_t key);

// The movers' two sound exits, shared by tick_change_height and
// tick_modify_sector.
//   Finalize (0x32e2f, raw_commands.c:2545-2552, 2731-2741): the first sweep
//   that moved: mark the node active and play it as a world sound.
//   Exit (0x32fab / 0x3399c, :2516-2535, 2743-2764): the move ran out. If the
//   node had been started, and it is a looping kind that is still active,
//   deactivate it and stop its voice; then, if the record carries an end
//   sound (stored + 1), play that at the node -- play_entity_sound for 0x07,
//   play_command_sound for 0x03.
void SoundMoverFinalize(int node);
void SoundMoverExit(int node, bool wasStarted, uint16_t endSoundPlusOne, bool asCommandSound);

// ---- doors ------------------------------------------------------------------

// A door swing was just registered (alloc_door_record, doors.c:608-636). The
// three ids are the command record's +0x0c, +0x0e and +0x10, each stored one
// higher with 0 for silence: played while it opens, while it closes, and when
// it has shut (doors.c:1049-1063, 1087-1091, 1140-1145). `polyTag` is the
// polyobject the loader built for the leaf; the swing is followed on it.
void SoundDoorOpened(int polyTag, uint16_t openPlusOne, uint16_t closePlusOne, uint16_t shutPlusOne);

// ---- music (DBASE100 ops 0x1a / 0x1b) ---------------------------------------

// load_dbase300_chunk (file_config.c:596-625): 0 = freshly loaded (or index
// 0, music off), 1 = already current or failed.
uint32_t MusicLoadChunk(uint32_t index);
// process_audio_sequence_chunk (audio.c:1765-1784): start what was loaded.
void MusicProcess();
// g_current_dbase300_chunk_id 0x7f46c.
uint32_t MusicCurrentChunk();
// True while a Realms level is up: the engine must not start or stop music
// for the map, because the original never does (no map-load path calls
// load_dbase300_chunk; docs/GAME_audio_save_menu.md 2.6).
bool SoundLevelKeepsMusic(const FLevelLocals *level);

// ---- speech -----------------------------------------------------------------

// prime_voice_clip (audio.c:1208-1292): start DBASE500 clip `clip` unless one
// is already playing. 1 = started.
uint32_t SoundPrimeVoiceClip(uint32_t clip);

// ---- engine hook ------------------------------------------------------------

// DoomSoundEngine::ReadSound asks here first. True, with `out` holding a
// complete WAVE image, when `lumpnum` is one of the numbers this file gave a
// Realms sample; the engine then decodes it like any other sound.
bool SoundReadLump(int lumpnum, TArray<uint8_t> &out);

} // namespace roth
