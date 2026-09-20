#pragma once

// ============================================================================
// MAIN-LOOP HANG WATCHDOG
//
// WHY THIS EXISTS. A hang and a crash look identical from outside a process:
// it stops. A crash at least leaves a report; a hang leaves nothing at all --
// no window, no log line, no exit code -- and on a machine with no debugger
// attached there is no way to ask where it stopped. That is exactly the state
// the respawn hang left us in: every one of the engine's 25 threads blocked,
// 0.11 seconds of CPU across six seconds, and not one clue which wait it was.
//
// So: the main loop leaves a heartbeat, and a second thread watches it. When
// the beat stops for longer than a level load could ever take, the watcher
// walks EVERY thread's stack -- with real function names and line numbers,
// from the PDB beside the exe -- and writes them to `hang_stack.txt` next to
// the working directory. Then the process can be killed and the answer is
// already on disk.
//
// IT NEVER INTERVENES. It does not kill, restart, unblock or recover anything.
// It reports, once, and goes quiet. A watchdog that takes action on a guess is
// a watchdog that eventually breaks a working game.
//
// STAGES, because "the main loop stopped" is a poor answer on its own. Each
// beat carries the name of the step about to run, so the report opens with
// "stopped in D_Display" rather than leaving that to be inferred.
//
// GENERAL, NOT FOR ONE BUG. Any future hang -- a shader compile, a sound
// device, a model load, a netgame wait -- lands in the same file in the same
// shape. Nothing here knows what it is hunting.
// ============================================================================

#ifdef _WIN32

// Start the watcher. Safe to call more than once; the second call does nothing.
void I_HangWatchStart();

// The main loop is alive and about to run `stage`. MUST be a string literal or
// other long-lived pointer: the watcher reads it from another thread without a lock.
void I_HangWatchBeat(const char *stage);

// Stop watching (shutdown). Safe to call when it was never started.
void I_HangWatchStop();

#else

// Not Windows: the stack walk is dbghelp's, and nothing else here is portable.
// The calls stay in the shared code and cost nothing.
inline void I_HangWatchStart() {}
inline void I_HangWatchBeat(const char *) {}
inline void I_HangWatchStop() {}

#endif
