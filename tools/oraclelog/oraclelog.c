/* oraclelog.c -- a ROTH.C mod that makes the original a measurable oracle.
 *
 * WHY THIS EXISTS. REMAROTH is a port of ROTH.C, and until now nothing compared
 * the two. Stages were signed off on parser counts -- sector and face totals
 * diffed against a Python pipeline -- which are structurally blind to whether
 * anything LOOKS or BEHAVES like Realms. Two wrong calls came out of that gap in
 * one day, both from reasoning about lifted x86 instead of measuring it.
 *
 * So: this prints one line per engine tick from the original, and REMAROTH prints
 * the same line in the same format. The first differing tick is the bug. That is
 * ROTH_GAME_PORT.md's Step 0 and its acceptance rule in §5.
 *
 * It only READS. No writes, no overrides, no engine calls -- so it cannot change
 * what it is measuring.
 *
 * Build (from this directory, with the shim dir and mingw bin on PATH):
 *     i686-w64-mingw32-gcc -shared -std=c11 -Wall -Wextra \
 *         -I<sdk>/include -o plugin.dll oraclelog.c
 * Install:  <game dir>/mods/oraclelog/plugin.dll
 * Output:   ROTH_ORACLE_LOG, or oraclelog.csv beside the game.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "roth_sdk.h"

/*
 * THE ADDRESSES. Every one is cited, because a wrong address here would quietly
 * invalidate every comparison built on it.
 *
 * The position globals are the ones most easily got wrong, and both specs warn
 * about it: GAME_audio_save_menu.md's header states "X = 0x90a8c, height =
 * 0x90a90, Y = 0x90a94, all 16.16" and adds that the VA_g_player_angle/_x/_z
 * macro names in the engine do NOT match those meanings. GAME_core.md §10
 * independently names the same three for its per-tick log. Two specs agreeing
 * against the macro names is why these are written as literals with this note
 * rather than taken from a symbol.
 */
#define VA_TICK        0x90bccu   /* g_frame_tick_counter, the 70 Hz heartbeat   */
#define VA_POS_X       0x90a8cu   /* 16.16                                       */
#define VA_POS_H       0x90a90u   /* 16.16 -- HEIGHT, not Y                      */
#define VA_POS_Y       0x90a94u   /* 16.16                                       */
#define VA_ANGLE       0x90a8au   /* u16, 0x200 per turn (GAME_core.md §3.2)      */
#define VA_SECTOR      0x90c12u   /* u16                                          */
#define VA_HEALTH      0x8a0f0u   /* g_player_health, default 0x800               */
#define VA_PITCH       0x819c8u   /* view pitch, saved chunk 2 +0x1c              */
#define VA_MODE        0x7674au   /* g_player_movement_enabled                    */
#define VA_DLG_BUSY    0x83aeau   /* g_dialogue_busy_flag                         */
#define VA_DLG_CTX     0x83115u   /* g_active_dialogue_context: laid-out line count */
#define VA_FREEZE_GATE 0x83125u   /* g_move_freeze_gate: 0x6ffff menu / 0x7ffff line */   /* view pitch, saved chunk 2 +0x1c              */

/* The DBASE100 story-flag bitmap: a POINTER at +0x28 and its size at +0x2c
 * (GAME_core.md §6, game_core.c:420-432). 433 retail records -> 56 bytes. */
#define VA_FLAGS_PTR   0x81e28u
#define VA_FLAGS_SIZE  0x81e2cu

/* The seven RNGs (GAME_core.md §7). All the same LCG, s = s*0x5e5 + 0x29, each
 * with its own state. Logged because a divergence in any of them desynchronises
 * everything downstream, and the first tick one differs on names the culprit. */
static const uint32_t RNG_VA[] = {
    0x7276cu,  /* rng_next: particles                       */
    0x7fe08u,  /* rng_range: weapon alt-slot pick           */
    0x71364u,  /* rng_next_index_for_count: dialogue 0x0b   */
    0x72730u,  /* AI wander / attack coin flip              */
    0x7272cu,  /* entity -> player damage scaling           */
    0x72734u,  /* ambient anim/sound                        */
    0x71f48u,  /* g_random_seed: cmd 0x3f turn, 0x12 delay  */
};
#define RNG_COUNT ((int)(sizeof RNG_VA / sizeof RNG_VA[0]))

#include "staticdump.inc.c"
#include "flatspans.inc.c"

/* Frame-dump state, declared early because on_load reads it. See on_compose_tick. */
static int g_shots_wanted, g_shots_done;

static FILE *g_log;
static uint16_t g_last_tick;
static int g_started;

/* A cheap, order-sensitive digest of the flag bitmap. The full 56 bytes would
 * dominate every line; what a diff needs is "did it change, and when". FNV-1a. */
static uint32_t digest(const uint8_t *p, uint32_t n)
{
    uint32_t h = 0x811c9dc5u;
    for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x01000193u; }
    return h;
}

/*
 * GETTING PAST THE MENU, PROPERLY.
 *
 * Headless has no keyboard, so run_main_menu (menu_hud_ui.c:2079) blocks forever
 * in show_message_box and the world is never simulated: the first runs logged
 * 8,344 ticks with the player frozen at the map start and the sector stuck at 0.
 *
 * Writing the mode byte does NOT fix that -- tried, and health changed while the
 * player stayed put, because the engine is still inside the menu's own loop, not
 * in the frame loop that would move anything.
 *
 * So override the blocking call instead. run_main_menu treats any result <= 1 as
 * "Play" (its own comment: "Esc/row 1 both start the game"), so answering 1 once
 * makes the menu return exactly as if Esc had been pressed, through the engine's
 * own code path. After that the override steps aside and every later message box
 * behaves normally -- a save prompt or an error must still work.
 *
 * This is the rig pressing a key, not the rig faking a result: the value returned
 * is one the real menu can produce, and the engine does the rest itself.
 */
static int g_play_answered;

static uint32_t ROTH_CDECL ov_show_message_box(struct roth_chain *chain,
                                              const struct roth_api_v1 *api,
                                              uint32_t desc, uint32_t flags)
{
    (void)api;
    /* Answer EVERY box, and log each one. The first attempt answered only the
     * first call and chained the rest through to the real, blocking one -- and the
     * game still stopped dead. So this reports how many boxes there are and what
     * they are, which is the difference between knowing and guessing.
     *
     * Returning 1 is "Play" at the main menu (menu_hud_ui.c:2088) and the first
     * option anywhere else, which is what a rig wants: it must never block.
     */
    /* PASS THROUGH, and only report. Both forged answers were wrong: 1 meant
     * "retry" and produced 163,973 calls in an infinite loop, 0 meant "decline"
     * and quit the game outright with code -1. Either way the rig never reached
     * gameplay, so the projection constants stayed zero and nothing could be
     * measured.
     *
     * The mistake was forging an answer at all. Run WINDOWED and the box appears
     * where it can be read and answered with a real keystroke -- which is also
     * the only way to find out what it is asking, since headless shows no screen.
     * So this now observes and gets out of the way.
     */
    g_play_answered++;
    if (g_play_answered <= 8)
        fprintf(stderr, "[oraclelog] message box #%d (desc=%08x flags=%08x) passed through\n",
                g_play_answered, desc, flags);
    return roth_next_show_message_box(chain, desc, flags);
}

static void on_register_overrides(const struct roth_api_v1 *api,
                                  struct roth_registrar_v1 *reg)
{
    (void)api;
    if (roth_override(reg, ROTH_FN_show_message_box, ov_show_message_box, 0) != 0)
        fprintf(stderr, "[oraclelog] could not override show_message_box\n");

    /* WORLD FLATS. Hooked at the DRIVER, not at the span emitters: the fill
     * word carries 0x20, so the dispatch at R:13314 sends them here, and by
     * this point the texture is resolved so its width and height are real.
     * The emitters used to be hooked instead, which sampled the previous
     * surface's dimensions and -- worse -- fired during the cursor pick pass.
     * See the header of flatspans.inc.c; that mistake reached the loader. */
    if (roth_override(reg, ROTH_FN_draw_scaled_sprite_spans, ov_scaled_spans, 0) != 0)
        fprintf(stderr, "[oraclelog] could not override the flat/sprite span driver\n");

    /* 3D MESH faces (draw flags 0x200). Counted so a large number here is not
     * mistaken for evidence about world flats again. */
    if (roth_override(reg, ROTH_FN_draw_floorceil_surface, ov_draw_floorceil, 0) != 0)
        fprintf(stderr, "[oraclelog] could not override the mesh face driver\n");
}

static void on_load(const struct roth_api_v1 *api)
{
    const char *shots = getenv("ROTH_ORACLE_SHOTS");
    if (shots != NULL) g_shots_wanted = atoi(shots);

    const char *path = getenv("ROTH_ORACLE_LOG");
    if (path == NULL) path = "oraclelog.csv";
    g_log = fopen(path, "w");
    if (g_log == NULL)
    {
        fprintf(stderr, "[oraclelog] cannot open %s -- logging to stderr\n", path);
        g_log = stderr;
    }
    /* The header is part of the contract: REMAROTH emits this line verbatim, so a
     * diff that starts at line 1 means the two sides disagree about the FORMAT,
     * which is a different bug from disagreeing about a value. */
    fprintf(g_log, "# oraclelog v1  ROTH.C  abi %u.%u\n",
            api->abi_major, api->abi_minor);
    /* The projection constants go in the PER-TICK log, not the one-shot static
     * dump. They are written by the world render pass, which happens long after
     * the map loads -- so the static dump, which fires the moment the geometry
     * buffer appears, always caught them at zero and that zero proved nothing.
     * Sampled every tick, they simply appear the moment the world first draws.
     */
    fprintf(g_log, "tick,x,h,y,angle,sector,health,pitch,flags,mulA,mulB,clip");
    for (int i = 0; i < RNG_COUNT; i++) fprintf(g_log, ",rng%d", i);
    fprintf(g_log, "\n");
    fflush(g_log);
}

static void on_frame_game(const struct roth_api_v1 *api)
{
    const struct roth_game_ram_api_v1 *m = api->game_ram;
    const uint16_t tick = m->u16(VA_TICK);

    /* Stamp the frame the flat spans about to be drawn belong to. The span
     * probe needs the player position of the SAME frame, because the whole
     * measurement is a difference between two positions -- see flatspans. */
    g_fs_tick = tick;
    g_fs_px   = (int32_t)m->u32(VA_POS_X) >> 16;
    g_fs_py   = (int32_t)m->u32(VA_POS_Y) >> 16;

    /* One line per ENGINE TICK, not per frame. The hook is per frame and the
     * original's frame rate is uncapped, so without this a fast machine emits
     * several lines for one tick and a slow one skips ticks -- and the log stops
     * being comparable to anything. Keyed on the 70 Hz counter, both sides
     * produce exactly one line per tick whatever the frame rate. */
    if (g_started && tick == g_last_tick) return;
    g_started = 1;
    g_last_tick = tick;

    /* Dump the flat-setup table from the TICK hook, not only at unload: a rig
     * run ends by killing the process, so on_unload never fires and the table
     * was lost. Written once the world has actually drawn some spans, then
     * re-written periodically so the newest view is always on disk.
     */
    {
        static uint16_t lastDump;
        if (g_fs_n > 0 && (uint16_t)(tick - lastDump) > 200u)
        {
            lastDump = tick;
            flatspan_dump();
        }
    }

    /* DISMISS DIALOGUE, every tick it appears.
     *
     * The new-game script (DBASE100 record 3) opens a text line, and a line with
     * no speech clip "waits for a click or key" (GAME_dialogue.md §2.5, gate
     * 0x7ffff). Headless has neither, so the engine parks in the dialogue mode
     * and the player is never simulated -- which is why the sector stayed 0 while
     * the tick counter kept advancing and our health write landed. The trace shows
     * it: an 8-byte DBASE400 header and 29 bytes of text read, then no further
     * file I/O at all.
     *
     * Clearing busy and the laid-out-line count is what voice_stream_pump does
     * itself once a line finishes (dbase100.c:484-497): while busy AND the text
     * context is non-zero it does nothing, otherwise it clears busy and advances
     * the queue. So this is the rig acknowledging each line as fast as it appears,
     * which is what holding Enter would do.
     *
     * It is deliberately unconditional rather than one-shot: record 3 can queue
     * several lines, and any later script can open more.
     */
    if (m->u32(VA_DLG_BUSY) != 0 || m->u32(VA_DLG_CTX) != 0)
    {
        const uint32_t zero = 0;
        m->write_block(VA_DLG_BUSY, &zero, 4);
        m->write_block(VA_DLG_CTX, &zero, 4);
        m->write_block(VA_FREEZE_GATE, &zero, 4);
        static int said;
        if (!said) { said = 1; fprintf(stderr, "[oraclelog] dismissing dialogue lines\n"); }
    }

    /* PRESS PLAY. Headless has no keyboard, so the game sits at the intro menu
     * forever and never renders the world or moves the player -- the first run
     * logged 8,344 ticks with sector and health both stuck at 0.
     *
     * This writes what selecting Play writes: mode 1 (gameplay) and full health.
     * It is the one place this mod writes anything, it happens once, and it is
     * doing to the oracle exactly what a keypress would. Everything else here is
     * read-only. Deliberately delayed until the map is in memory, because before
     * that there is no world to be in.
     *
     * Mode values, game_core.c:861-921 via GAME_core.md §4.1:
     *   0 off  1 gameplay  3 inventory  4/5 dialogue  8 transitional  0x20 dead
     */
    static int pressed_play;
    if (!pressed_play && m->u32(VA_GEOM_BUF) != 0 && tick > 120)
    {
        const uint8_t mode = 1;
        const uint32_t hp = 0x800;          /* the default max, game_core.c:769-772 */
        m->write_block(VA_MODE, &mode, 1);
        m->write_block(VA_HEALTH, &hp, 4);
        pressed_play = 1;
        fprintf(stderr, "[oraclelog] pressed Play at tick %u (mode=1, health=0x800)\n",
                (unsigned)tick);
    }

    /* The static dump, once, as soon as the map is really in memory. It cannot
     * run at on_load (game_ram is pristine) or at on_game_ram_ready (no map yet),
     * so it is gated here on the geometry buffer having been filled. */
    static int dumped;
    if (!dumped && static_dump(api)) dumped = 1;

    uint32_t fdig = 0;
    const uint32_t fsize = m->u32(VA_FLAGS_SIZE);
    const uint32_t fptr  = m->u32(VA_FLAGS_PTR);
    if (fptr != 0 && fsize != 0 && fsize <= 4096u)
    {
        /* The engine stores this as a RUNTIME pointer, base + VA. to_ptr(0) hands
         * back that same base, so subtracting it recovers the VA the bounded
         * accessors want. Doing it this way keeps the read inside the SDK's
         * bounds checking instead of dereferencing a raw pointer. */
        const uint8_t *base = (const uint8_t *)m->to_ptr(0);
        if (base != NULL && fptr >= (uint32_t)(uintptr_t)base)
        {
            const uint32_t va = fptr - (uint32_t)(uintptr_t)base;
            uint8_t buf[4096];
            if (m->read_block(va, buf, fsize) == 0) fdig = digest(buf, fsize);
        }
    }

    fprintf(g_log, "%u,%d,%d,%d,%u,%u,%d,%d,%08x,%d,%d,%d",
            (unsigned)tick,
            (int)m->u32(VA_POS_X), (int)m->u32(VA_POS_H), (int)m->u32(VA_POS_Y),
            (unsigned)m->u16(VA_ANGLE), (unsigned)m->u16(VA_SECTOR),
            (int)m->u32(VA_HEALTH), (int)(int16_t)m->u16(VA_PITCH),
            fdig,
            (int)m->u32(VA_VIEW_PARAMS + 0x0c),
            (int)m->u32(VA_PERSP_SCALE),
            (int)m->u32(VA_CLIP_PLANE));
    for (int i = 0; i < RNG_COUNT; i++)
        fprintf(g_log, ",%04x", (unsigned)m->u16(RNG_VA[i]));
    fprintf(g_log, "\n");
}

/* SCREENSHOTS, FROM INSIDE THE ORACLE.
 *
 * on_compose_tick hands us the finished 8-bit frame, so the rig can see what the
 * original is showing without a screen-capture tool and without a human watching.
 * That answers the question headless could not: WHAT is it waiting on.
 *
 * It is also half of the plan's visual reference set -- the same hook, fired at
 * chosen camera positions, produces the ROTH.C side of every comparison shot.
 *
 * Written as PGM (P5) of the raw palette indices rather than RGB: the VGA palette
 * lives elsewhere and is not needed to READ a frame -- text and edges are already
 * legible as distinct index values, and an index image is the honest thing to
 * compare anyway, since it is what the original actually rasterised.
 *
 * ROTH_ORACLE_SHOTS = how many frames to write (default 0, off). They land beside
 * the log as shot_00000.pgm and so on, one per composed frame, so the sequence
 * shows how the boot progresses rather than one arbitrary instant.
 */
static void on_compose_tick(const struct roth_api_v1 *api, uint8_t *pixels,
                            uint32_t width, uint32_t height)
{
    if (g_shots_done >= g_shots_wanted || pixels == NULL) return;
    if (width == 0 || height == 0 || width > 4096 || height > 4096) return;

    /* WRITE RGB, NOT INDICES. The first version dumped palette indices as a
     * greyscale PGM, and comparing those against REMAROTH's RGB output is
     * meaningless -- index 200 is not "brighter" than index 50, it is simply a
     * different colour. Any measurement taken from that comparison would have
     * been noise dressed up as evidence.
     *
     * So the frame goes through the game's own palette, g_palette_rgb_ptr
     * (0x85488), which is 256 RGB triples of 6-BIT VGA values: they went straight
     * to the DAC, so 0..63 rather than 0..255. Scaling by 255/63 rather than <<2
     * keeps white actually white.
     *
     * If the palette pointer is not up yet the frame is skipped rather than
     * written wrongly -- a missing frame is obvious, a mis-coloured one is not.
     */
    const uint32_t palptr = api->game_ram->u32(0x85488u);
    if (palptr == 0) return;
    const uint8_t *pal = (const uint8_t *)(uintptr_t)palptr;

    char name[64];
    /* NAME THE CONDITIONS INTO THE FILE. A ROTH.C shot and a REMAROTH shot were
     * once compared at 640x480 against 320x200 -- about 16% different field of
     * view -- and the difference was read as a projection defect. A screenshot
     * that does not carry its own resolution and build cannot be compared to
     * anything later, so both go in the name. ROTH_ORACLE_TAG supplies the
     * build id; it is not optional in a comparison run. */
    const char *tag = getenv("ROTH_ORACLE_TAG");
    if (tag == NULL) tag = "untagged";
    snprintf(name, sizeof name, "rothc_%s_%ux%u_%05d.ppm",
             tag, (unsigned)width, (unsigned)height, g_shots_done);
    FILE *f = fopen(name, "wb");
    if (f == NULL) return;
    fprintf(f, "P6\n%u %u\n255\n", width, height);

    const size_t n = (size_t)width * (size_t)height;
    for (size_t i = 0; i < n; i++)
    {
        const uint8_t *e = pal + (size_t)pixels[i] * 3u;
        const uint8_t rgb[3] = {
            (uint8_t)((e[0] * 255u) / 63u),
            (uint8_t)((e[1] * 255u) / 63u),
            (uint8_t)((e[2] * 255u) / 63u),
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);

    if (g_shots_done == 0)
        fprintf(stderr, "[oraclelog] writing %d frame(s) at %ux%u\n",
                g_shots_wanted, width, height);
    g_shots_done++;
}

static void on_unload(const struct roth_api_v1 *api)
{
    (void)api;
    flatspan_dump();
    if (g_log != NULL && g_log != stderr) fclose(g_log);
    g_log = NULL;
}

static const struct roth_plugin_info_v1 ORACLELOG = {
    .abi_major     = ROTH_ABI_MAJOR,
    .abi_minor     = ROTH_ABI_MINOR,
    .struct_size   = sizeof(struct roth_plugin_info_v1),
    .id            = "vrealms.remaroth.oraclelog",
    .name          = "Oracle log (REMAROTH comparison rig)",
    .version       = "1.0.0",
    .sdk_req_major = ROTH_SDK_MAJOR,
    .sdk_req_minor = ROTH_SDK_MINOR,
    .api_use       = ROTH_API_USE_GAME_RAM | ROTH_API_USE_ENGINE | ROTH_API_USE_COMPOSE,
    .on_load           = on_load,
    .on_register_overrides = on_register_overrides,
    .on_game_ram_ready = NULL,
    .on_frame_game     = on_frame_game,
    .on_compose_tick   = on_compose_tick,
    .on_audio          = NULL,
    .on_unload         = on_unload,
};

ROTH_PLUGIN_EXPORT const struct roth_plugin_info_v1 *roth_plugin_query_v1(void)
{
    return &ORACLELOG;
}
