/* rothdiff_plugin.c -- ROTH.C's half of rothdiff.
 *
 * A HEADLESS COMMAND MODE FOR THE ORIGINAL. Given a camera pose it renders one
 * frame of the VISIBLE pass and writes a per-pixel identity buffer: which
 * surface the original drew at every pixel, and out of which texture. Then it
 * exits.
 *
 * This exists so that "is our floor right" stops being a question answered by
 * looking at two screenshots. It is answered by a match percentage and a list
 * of the surfaces that disagree.
 *
 * ONE LAUNCH, EVERY POSE. Loading the map is the expensive part and the camera
 * is only being moved between captures, so the whole pose set is walked inside a
 * single run: pin, settle, capture, write, advance. An earlier version exited
 * after one pose, which made the runner relaunch the game ten times over. There
 * was never a reason for that.
 *
 * RUN (no window needed):
 *
 *     set ROTHDIFF_POSEFILE=poses.csv   lines of  name,x,y,angle
 *     set ROTHDIFF_OUTDIR=captures      writes <outdir>/<name>.roth.ridb
 *     set ROTHDIFF_SETTLE=40            ticks before the FIRST capture
 *     set ROTHDIFF_HOLD=3               ticks to hold each later pose
 *     rothc.exe --headless --game-dir ... --c-root ...
 *
 * A single pose still works: ROTHDIFF_POSE=x,y,angle with ROTHDIFF_OUT=file.
 * With neither set the plugin does nothing, so it is safe to leave installed.
 *
 * The angle is in ROTH's own 512-units-per-turn, so a pose can be pasted
 * between the two engines without a hand conversion at each call site -- which
 * is where sign errors live.
 *
 * WHY A POSE IS FORCED RATHER THAN WALKED TO. A comparison is only meaningful
 * if both engines are at the SAME place, and walking there by sending keys is
 * neither exact nor repeatable -- an earlier attempt to move the player with
 * synthetic arrow keys moved it not at all. The pose is written straight into
 * the player position globals every tick, which pins it.
 *
 * Build (from this directory, shim dir + mingw bin on PATH):
 *     i686-w64-mingw32-gcc -shared -std=c11 -Wall -Wextra \
 *         -I<sdk>/include -o plugin.dll rothdiff_plugin.c
 * Install: <game dir>/mods/rothdiff/plugin.dll
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "roth_sdk.h"

/* ---- addresses ---------------------------------------------------------
 * The player pose. GAME_audio_save_menu.md's header states X = 0x90a8c,
 * height = 0x90a90, Y = 0x90a94, all 16.16, and warns that the engine's own
 * VA_g_player_* macro names are misleading about which is which. Two specs
 * agree on these, and the oracle tick log has been read against them.
 */
#define VA_POS_X       0x90a8cu   /* 16.16 */
#define VA_POS_H       0x90a90u   /* 16.16 */
#define VA_POS_Y       0x90a94u   /* 16.16 */
#define VA_ANGLE       0x90a8au   /* 512 units per turn */
#define VA_TICK        0x90bccu   /* g_frame_tick_counter, the 70 Hz heartbeat */

/* ---- the keyboard ring, for pressing quickload without a human -------------
 * Layout and enqueue semantics are the original's own, from input.c:13 and the
 * int-9 body at input.c:259-263:
 *
 *     buffer     0x90c1c
 *     write head u16 0x7e91c
 *     read tail  u16 0x7e91e
 *     mask       u16 0x7e91a
 *     enqueue:   next = (head + 1) & mask; drop if next == tail;
 *                buffer[head] = scancode; head = next
 *
 * The scancode comes from the game's OWN keymap table (0x7093d, {u8 sc, u32
 * handler}), whose initializer in obj3_owned.c reads
 *     0x43 -> key_quicksave     0x44 -> key_quickload
 * so F9 saves and F10 loads. Read, not assumed -- guessing a scancode here
 * would silently press something else.
 */
#define VA_KEY_RING    0x90c1cu
#define VA_RING_HEAD   0x7e91cu
#define VA_RING_TAIL   0x7e91eu
#define VA_RING_MASK   0x7e91au
#define SC_QUICKLOAD   0x44u      /* F10 */

#include "idbuffer.inc.c"
#include "wallpath.inc.c"
#include "uvcapture.inc.c"
#include "dascache.inc.c"

/* ---- command state ----------------------------------------------------- */

#define POSE_MAX 64

struct pose {
    char     name[48];
    int32_t  x, y;
    uint16_t ang;
};

static int      g_active;          /* a pose was given, so we are in command mode */
static struct pose g_pose[POSE_MAX];
static int      g_pose_n;
static int      g_cur;             /* the pose being captured */
static char     g_outdir[512];
static char     g_single_out[512]; /* single-pose mode only */
static char     g_trigger[512];    /* wait for this file before capturing */
static int      g_triggered;

static int      g_quickload;       /* press F10 at startup */
static int      g_quickload_at = 60;  /* ticks to wait before pressing it */
static int      g_quickload_sent;
static int      g_quickload_tries;
static int      g_uv_warmup;   /* frames skipped so the paint can take effect */
static int      g_loaded;

static int      g_settle = 40;     /* ticks before the first capture */
static int      g_hold = 3;        /* ticks to hold each pose after the first */

static int      g_done;
static uint16_t g_pose_tick;       /* tick this pose was pinned at */
static int      g_have_tick;
static int      g_play_answered;

/* Where this pose's buffer goes. Single-pose mode keeps its explicit path so
 * an ad-hoc capture does not need a directory. */
static void pose_path(char *dst, size_t n, int i)
{
    if (g_single_out[0] != '\0') { snprintf(dst, n, "%s", g_single_out); return; }
    if (g_outdir[0] != '\0')
        snprintf(dst, n, "%s/%s.roth.ridb", g_outdir, g_pose[i].name);
    else
        snprintf(dst, n, "%s.roth.ridb", g_pose[i].name);
}

/* The new-game script opens a text line that waits for a click, which would
 * otherwise stall a headless run forever. Same pass-through the oracle log
 * uses: answer it, log nothing, change nothing else. */
static uint32_t ROTH_CDECL ov_show_message_box(struct roth_chain *chain,
                                               const struct roth_api_v1 *api,
                                               uint32_t desc, uint32_t flags)
{
    (void)api;
    g_play_answered++;
    return roth_next_show_message_box(chain, desc, flags);
}

static void on_register_overrides(const struct roth_api_v1 *api,
                                  struct roth_registrar_v1 *reg)
{
    (void)api;
    if (!g_active) return;   /* installed but not asked to do anything */

    if (roth_override(reg, ROTH_FN_show_message_box, ov_show_message_box, 0) != 0)
        fprintf(stderr, "[rothdiff] could not override show_message_box\n");
    if (roth_override(reg, ROTH_FN_draw_world_surface_spans, ov_wall_spans, 0) != 0)
        fprintf(stderr, "[rothdiff] could not override the wall span driver\n");
    if (g_uv_pass != UV_OFF && uv_register(reg) != 0)
        fprintf(stderr, "[rothdiff] could not override the span driver for UV\n");
    if (id_register(reg) != 0)
        fprintf(stderr, "[rothdiff] WARNING: one or more fill loops did not "
                        "override; the id buffer will have holes\n");
}

static int load_pose_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL)
    {
        fprintf(stderr, "[rothdiff] cannot open pose file %s\n", path);
        return 0;
    }
    char line[256];
    while (g_pose_n < POSE_MAX && fgets(line, sizeof line, f) != NULL)
    {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char nm[48];
        int x, y, a;
        if (sscanf(line, "%47[^,],%d,%d,%d", nm, &x, &y, &a) != 4) continue;
        struct pose *p = &g_pose[g_pose_n++];
        snprintf(p->name, sizeof p->name, "%s", nm);
        p->x = x; p->y = y; p->ang = (uint16_t)a;
    }
    fclose(f);
    return g_pose_n;
}

static void on_load(const struct roth_api_v1 *api)
{
    (void)api;

    const char *outdir = getenv("ROTHDIFF_OUTDIR");
    if (outdir != NULL) snprintf(g_outdir, sizeof g_outdir, "%s", outdir);

    const char *trg = getenv("ROTHDIFF_TRIGGER");
    if (trg != NULL)
    {
        snprintf(g_trigger, sizeof g_trigger, "%s", trg);
        remove(g_trigger);        /* a stale trigger would fire on startup */
    }

    const char *pf = getenv("ROTHDIFF_POSEFILE");
    if (pf != NULL)
    {
        if (load_pose_file(pf) == 0) return;
    }
    else
    {
        const char *pose = getenv("ROTHDIFF_POSE");
        if (pose == NULL) return;                 /* installed, but idle */
        int x = 0, y = 0, a = 0;
        if (sscanf(pose, "%d,%d,%d", &x, &y, &a) != 3)
        {
            fprintf(stderr, "[rothdiff] ROTHDIFF_POSE must be x,y,angle -- got '%s'\n", pose);
            return;
        }
        struct pose *p = &g_pose[g_pose_n++];
        snprintf(p->name, sizeof p->name, "pose");
        p->x = x; p->y = y; p->ang = (uint16_t)a;
        const char *o = getenv("ROTHDIFF_OUT");
        snprintf(g_single_out, sizeof g_single_out, "%s",
                 o != NULL ? o : "rothdiff.ridb");
    }

    const char *s = getenv("ROTHDIFF_SETTLE");
    if (s != NULL) g_settle = atoi(s);
    if (g_settle < 1) g_settle = 1;

    /* UV MODE. Each camera spot is captured twice -- once with every texture
     * repainted as its own column, once as its own row -- so the composed
     * frame is literally the u buffer and then the v buffer. */
    if (getenv("ROTHDIFF_UV") != NULL) g_uv_pass = UV_PASS_U;
    {
        const char *pat = getenv("ROTHDIFF_PATTERN");
        g_uv_pattern_v = (pat != NULL && (*pat == 'v' || *pat == 'V'));
    }

    const char *ql = getenv("ROTHDIFF_QUICKLOAD");
    if (ql != NULL && atoi(ql) != 0)
    {
        g_quickload = 1;
        const char *qa = getenv("ROTHDIFF_QUICKLOAD_AT");
        if (qa != NULL) g_quickload_at = atoi(qa);
        if (g_quickload_at < 1) g_quickload_at = 1;
    }

    const char *hd = getenv("ROTHDIFF_HOLD");
    if (hd != NULL) g_hold = atoi(hd);
    if (g_hold < 1) g_hold = 1;

    g_active = 1;
    fprintf(stderr, "[rothdiff] %d pose(s), settle %d, hold %d -- ONE run\n",
            g_pose_n, g_settle, g_hold);
}

/* The pose is pinned every tick. Writing it once is not enough: the player
 * think runs afterwards and would move or re-clamp it. */
static void pin_pose(const struct roth_api_v1 *api, int i)
{
    const struct roth_game_ram_api_v1 *m = api->game_ram;
    m->set_u32(VA_POS_X, (uint32_t)(g_pose[i].x << 16));
    m->set_u32(VA_POS_Y, (uint32_t)(g_pose[i].y << 16));
    m->set_u16(VA_ANGLE, g_pose[i].ang);
}

/* TAKE OVER A RUNNING GAME. With ROTHDIFF_TRIGGER set the plugin sits idle in a
 * normal session -- you play to wherever you want -- and starts capturing the
 * moment that file appears. Nothing restarts, nothing is saved and reloaded,
 * and the capture happens in the session already on screen.
 *
 * Polled once a tick. The file is deleted on pickup so the same run can be
 * triggered again without restarting the game. */
static int trigger_ready(void)
{
    if (g_trigger[0] == '\0') return 1;      /* no trigger: start immediately */
    if (g_triggered) return 1;
    FILE *f = fopen(g_trigger, "rb");
    if (f == NULL) return 0;
    fclose(f);
    remove(g_trigger);
    g_triggered = 1;
    fprintf(stderr, "[rothdiff] triggered -- capturing %d camera spot(s) "
                    "without restarting\n", g_pose_n);
    return 1;
}

/* Press a key the way the int-9 handler would. */
static int ring_push(const struct roth_api_v1 *api, uint8_t sc)
{
    const struct roth_game_ram_api_v1 *m = api->game_ram;
    const uint16_t head = m->u16(VA_RING_HEAD);
    const uint16_t mask = m->u16(VA_RING_MASK);
    const uint16_t next = (uint16_t)((head + 1) & mask);
    if (next == m->u16(VA_RING_TAIL)) return 0;      /* full: the game drops it too */
    m->set_u8(VA_KEY_RING + head, sc);
    m->set_u16(VA_RING_HEAD, next);
    return 1;
}

static void on_frame_game(const struct roth_api_v1 *api)
{
    if (!g_active || g_done) return;

    /* LOAD THE SAVE FIRST, once. Starting from a save skips the intro film and
     * the menus entirely, and puts the camera somewhere known. The poses are
     * absolute world coordinates, so the save only has to be in the right MAP;
     * where it stands inside it does not matter. */
    if (g_quickload && !g_quickload_sent)
    {
        const uint16_t t = api->game_ram->u16(VA_TICK);
        if (!g_have_tick) { g_pose_tick = t; g_have_tick = 1; }
        if ((uint16_t)(t - g_pose_tick) < (uint16_t)g_quickload_at) return;
        if (!ring_push(api, SC_QUICKLOAD)) return;   /* ring full: try next tick */
        /* NOT ONCE. The menu is not necessarily up at a fixed tick, and a
         * single F10 sent before it exists is simply lost -- which left a run
         * parked on the Options menu with nothing captured. Keep pressing,
         * every half second, until world spans actually draw. */
        g_quickload_tries++;
        g_pose_tick = api->game_ram->u16(VA_TICK);
        if (g_uv_world_spans == 0 && g_quickload_tries < 40)
        {
            g_quickload_at = 35;                     /* retry in half a second */
            return;
        }
        g_quickload_sent = 1;
        g_have_tick = 0;
        fprintf(stderr, "[rothdiff] pressed F10 (quickload)\n");
        return;
    }
    /* Give the load time to finish before the camera is pinned or anything is
     * captured -- a buffer taken mid-load is a picture of the wrong level. */
    if (g_quickload && g_quickload_sent && !g_loaded)
    {
        const uint16_t t = api->game_ram->u16(VA_TICK);
        if (!g_have_tick) { g_pose_tick = t; g_have_tick = 1; }
        if ((uint16_t)(t - g_pose_tick) < (uint16_t)g_settle) return;
        g_loaded = 1;
        g_have_tick = 0;
    }

    /* PAINT THE WHOLE CACHE ONCE, then never again. After this every frame is a
     * coordinate frame, so the capture below has no timing to get wrong. */
    /* EVERY FRAME, not once.
     *
     * Painting once ran while the game was still on the Options menu and
     * repainted 32 textures -- the MENU's. The level then loaded and decoded
     * fresh textures that had never been touched, which is why the capture kept
     * coming back as artwork. The cache is small and the walk is cheap, so it
     * is simply redone each frame and newly loaded textures are picked up
     * automatically. */
    /* THE CACHE WALK IS OFF.
     *
     * Walking every loaded block and rewriting it crashed the game the moment a
     * menu drew, and skipping the grouped kind (blk+0x0a bit 0x40) was not
     * enough -- menu artwork evidently comes in more shapes than that, and
     * writing width*height bytes into the wrong one corrupts whatever follows.
     *
     * Painting from inside the world FILL never crashed and visibly striped the
     * whole world, because a block only gets written when the world renderer is
     * about to sample it as a plain texture. That is the safe path, so it is
     * the one kept. ROTHDIFF_CACHEWALK re-enables this if it is ever worth
     * another look. */
    if (g_uv_pass != UV_OFF && getenv("ROTHDIFF_CACHEWALK") != NULL)
    {
        const uint32_t n = dc_paint_all(api, g_uv_pattern_v);
        if (n == 0) return;                                  /* nothing loaded yet */
        if (n != g_dc_done) {
            g_dc_done = (int)n;
        fprintf(stderr, "[rothdiff] repainted %u cached texture(s), %u skipped, pattern %s\n",
                g_dc_painted, g_dc_skipped, g_uv_pattern_v ? "v (row)" : "u (column)");
        }
    }

    if (!trigger_ready()) return;
    const struct roth_game_ram_api_v1 *m = api->game_ram;
    const uint16_t tick = m->u16(VA_TICK);

    if (!g_have_tick) { g_pose_tick = tick; g_have_tick = 1; }
    const uint16_t held = (uint16_t)(tick - g_pose_tick);

    /* Pinned EVERY tick: the player think runs after this and would otherwise
     * move or re-clamp the camera back off the pose. */
    pin_pose(api, g_cur);

    /* The first pose waits for the map to settle; later ones only need the
     * camera to take effect, because the level is already up. */
    const uint16_t wait = (g_cur == 0) ? (uint16_t)g_settle : (uint16_t)g_hold;
    if (held >= wait)
        id_begin_frame(api);
}

/* on_compose_tick fires after the frame has been drawn, which is when the id
 * buffer for that frame is complete. */
static void on_compose_tick(const struct roth_api_v1 *api, uint8_t *pixels,
                            uint32_t width, uint32_t height)
{
    (void)api; (void)pixels; (void)width; (void)height;
    if (!g_active || g_done || !g_id_armed) return;

    if (g_id_written == 0)
    {
        /* Nothing was drawn: the world is not up yet. Try again next frame
         * rather than writing an empty buffer that looks like a result. */
        g_id_armed = 0;
        return;
    }

    char path[600];

    /* UV MODE: the frame in front of us IS the coordinate buffer, because every
     * texture was repainted as its own column (or row) before it was sampled.
     * Each camera spot takes two frames -- U, then V -- and only then advances. */
    /* FRAME SWEEP. Dump every composed frame for a while and find the painted
     * ones afterwards, instead of picking a frame and hoping. The captures kept
     * landing on a black fade or on an unpainted frame, and guessing which tick
     * is the right one has cost several runs. The files answer it. */
    if (getenv("ROTHDIFF_SWEEP") != NULL)
    {
        static int sweep;
        if (sweep < 60)
        {
            char sp[600];
            snprintf(sp, sizeof sp, "%s/sweep_%03d.pgm",
                     g_outdir[0] ? g_outdir : ".", sweep);
            FILE *sf = fopen(sp, "wb");
            if (sf != NULL)
            {
                fprintf(sf, "P5%c%u %u%c255%c", 10, width, height, 10, 10);
                fwrite(pixels, 1, (size_t)width * (size_t)height, sf);
                fclose(sf);
            }
            /* Alternate the ramp every frame, so a painted frame is obvious:
             * consecutive painted frames MUST differ from each other. */
            g_uv_pass = (sweep & 1) ? UV_PASS_U : UV_PASS_V;
            g_uv_world_spans = 0;
            g_uv_snap_valid = 0;
            sweep++;
            return;
        }
        fprintf(stderr, "[rothdiff] sweep complete: 60 frame(s) written\n");
        exit(0);
    }

    if (g_uv_pass != UV_OFF)
    {
        /* A frame with no world spans is the menu, or a load in progress.
         * Capturing it produces a file with not one measured pixel in it, which
         * is worse than no file because it looks like a result. */
        if (g_uv_world_spans == 0)
        {
            g_id_armed = 0;
            g_have_tick = 0;
            return;
        }

        /* ONE FRAME OF LATENCY.
         *
         * The frame sweep settled this: painting during frame N shows up in
         * frame N+1, so every capture so far grabbed the frame the paint was
         * applied DURING and came back with ordinary artwork. Skip one painted
         * frame, then keep the next.
         *
         * Counted per pass, and reset when the pass changes, so U and V each
         * get their own warm-up rather than sharing one. */
        /* No warm-up any more. Painting is unconditional and permanent, so a
         * frame that drew world spans is necessarily a painted frame. */

        const char *suffix = (g_uv_pass == UV_PASS_U) ? "u" : "v";
        snprintf(path, sizeof path, "%s/%s.%s.rvuv",
                 g_outdir[0] ? g_outdir : ".", g_pose[g_cur].name, suffix);
        /* The fills' own framebuffer, not the compose hook's buffer -- see
         * uv_framebuffer. Falls back to the compose buffer only so a failure
         * to resolve it is visible as wrong data rather than as no data. */
        /* SELF-TEST FIRST. Two buffers are written for the same frame:
         *   .rvuv  -- the snapshot taken as the world pass returned
         *   .pgm   -- on_compose_tick's own pixels, the path that produced
         *             correct pictures of the Study earlier today
         * If the .pgm shows the ramps and the .rvuv does not, the capture was
         * reading the wrong buffer all along and the compose path is the one to
         * keep. Looking at both costs one run and settles it. */
        uint32_t fbw = width, fbh = height;
        const uint8_t *fb = NULL;
        if (g_uv_snap_valid) { fb = g_uv_snap; fbw = g_uv_snap_w; fbh = g_uv_snap_h; }
        if (fb == NULL) { fb = pixels; fbw = width; fbh = height; }
        {
            char pg[600];
            snprintf(pg, sizeof pg, "%s/%s.%s.pgm",
                     g_outdir[0] ? g_outdir : ".", g_pose[g_cur].name,
                     (g_uv_pass == UV_PASS_U) ? "u" : "v");
            FILE *pf = fopen(pg, "wb");
            if (pf != NULL)
            {
                /* Raw palette indices as greyscale: the byte IS the number we
                 * painted, so nothing must go through the palette here. */
                fprintf(pf, "P5%c%u %u%c255%c", 10, width, height, 10, 10);
                fwrite(pixels, 1, (size_t)width * (size_t)height, pf);
                fclose(pf);
            }
        }
        if (uv_write(path, fb, fbw, fbh, g_uv_pass,
                     g_pose[g_cur].x, g_pose[g_cur].y, (int32_t)g_pose[g_cur].ang) == 0)
            fprintf(stderr, "[rothdiff] %2d/%d  %-18s %s  %ux%u  painted %u skipped %u\n",
                    g_cur + 1, g_pose_n, g_pose[g_cur].name, suffix,
                    fbw, fbh, g_uv_painted, g_uv_skipped);
        g_uv_painted = 0;
        g_uv_world_spans = 0;
        g_uv_snap_valid = 0;
        g_uv_skipped = 0;
        g_id_armed = 0;

        /* Switch to V and STAY ARMED. Resetting g_have_tick here restarted the
         * per-spot hold, which disarmed the capture while the fills carried on
         * painting -- the V pass logged 493,341 paints against U's 3,437 and
         * then captured a frame that was no longer the one it painted. The
         * frame sweep alternated correctly for exactly this reason: it never
         * touched the arming. V now follows U immediately, with its own
         * one-frame warm-up and nothing else in between. */
        if (0)   /* one pattern per run -- see ROTHDIFF_PATTERN */
        {
            g_uv_pass = UV_PASS_V;
            g_uv_warmup = 0;
            g_uv_world_spans = 0;
            g_uv_snap_valid = 0;
            id_begin_frame(api);     /* keep it armed for the next frame */
            return;
        }
        g_uv_pass = UV_PASS_U;      /* next spot starts on U again */
    }
    else
    {
    pose_path(path, sizeof path, g_cur);
    if (id_write(path, g_pose[g_cur].x, g_pose[g_cur].y,
                 (int32_t)g_pose[g_cur].ang) == 0)
        fprintf(stderr, "[rothdiff] %2d/%d  %-20s %ux%u  %llu px\n",
                g_cur + 1, g_pose_n, g_pose[g_cur].name, g_id_w, g_id_h,
                (unsigned long long)g_id_written);
    else
        fprintf(stderr, "[rothdiff] FAILED to write %s\n", path);
    }

    g_id_armed = 0;

    /* Next pose in the SAME run. The map stays loaded; only the camera moves. */
    // Written after EVERY spot, not only at the end. One wedged camera spot --
    // one placed inside geometry, say -- used to take the whole table with it.
    wallpath_dump();

    g_cur++;
    g_have_tick = 0;
    if (g_cur < g_pose_n) return;

    fprintf(stderr, "[rothdiff] %d camera spot(s) captured\n", g_pose_n);
    wallpath_dump();

    /* NEVER KILL A SESSION SOMEONE IS SITTING IN FRONT OF.
     *
     * The capture is done and the files are on disk; quitting at that moment
     * takes the window away from whoever is looking at it, which is exactly
     * what it did while someone was trying to see the gradients for themselves.
     *
     * A batch run is the only case that must not leave the game up, and it says
     * so explicitly with ROTHDIFF_QUIT. Everything else stays open and simply
     * stops capturing. */
    if (g_trigger[0] != '\0')
    {
        g_cur = 0;
        g_triggered = 0;
        g_have_tick = 0;
        fprintf(stderr, "[rothdiff] idle again; touch the trigger to recapture\n");
        return;
    }

    g_done = 1;
    if (getenv("ROTHDIFF_QUIT") != NULL)
        exit(0);
    fprintf(stderr, "[rothdiff] capture complete -- files written, game left "
                    "running. Look around; close it when you are done.\n");
}

static void on_unload(const struct roth_api_v1 *api)
{
    (void)api;
    free(g_id);
    g_id = NULL;
}

static const struct roth_plugin_info_v1 ROTHDIFF = {
    .abi_major     = ROTH_ABI_MAJOR,
    .abi_minor     = ROTH_ABI_MINOR,
    .struct_size   = sizeof(struct roth_plugin_info_v1),
    .id            = "vrealms.remaroth.rothdiff",
    .name          = "rothdiff (per-pixel identity capture)",
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
    return &ROTHDIFF;
}
