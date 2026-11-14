/* boxprobe.c -- a mod for the headless ROTH.C build (/home/claude/oracle_build): the RAW records' SURFACE BOX,
 * through the ORIGINAL, for tools/surfbox_test.
 *
 * Overrides run_gameplay_frame (0x1691c), which runs on the game thread right after the frame's 3D pass -- where
 * the original itself picks (game_core.c:578) -- and steps a script (SB_SCRIPT), one step per line:
 *
 *   P x y angle sectorOffset z pitch label | dx0 dx1 dxs dy0 dy1 dys
 *       the player's pose (written every frame of an SB_SETTLE-frame wait, as tools/userange_test/rangeprobe.c),
 *       then for every screen point (dx, dy) of the grid, relative to the view centre: the mouse there and the
 *       original's render_world_view (0x10c8f) -- the cursor pick, renderer.c:9449 -- whose 0x24-byte record at
 *       0x90a48 is logged whole:
 *         PK label px py pz pa pitch dx dy <36 bytes hex> <the pitch the frame was drawn with, 0x89ee8>
 *       px py pz pa: the pose the frame was drawn with (seen(), below), not the one written
 *       the texel words the box is tested against are +4 and +6 (walls: exit_f_writeout, renderer.c:4482-4494;
 *       flats: the deferred tail of the floor/ceiling span driver, renderer.c:12997-13005).
 *   M type off8 offA band w4 w6 label
 *       a pick record made here (type at +1, the texel at +4/+6, the face's geometry offset at +8, the sector's
 *       at +0xa, the wall band at +0x1a, depth 0), handed to the original's dispatch_entry_command_trigger
 *       (raw_commands.c:3010, the use match) and dispatch_entry_command_trigger_b (:3102, the examine match). Both
 *       are read-only matchers (their flow pre-pass rcw_walk_from_firer is the lifted read-only walk); what they
 *       return is the matched record (object table header + ref) or 0. Logged as the record's byte offset in the
 *       object table (0 = none):
 *         MT label use exa
 *       Every consecutive M line runs in the same frame.
 *   X   clear the "spent" bit (+2 bit 3) of every record in the four boxed directories (object table header
 *       +0x18 use wall, +0x1c use floor, +0x30 examine wall, +0x34 examine floor), so the records disabled at load
 *       are walked too. Only the matchers read these bytes afterwards (the script ends with Q).
 *   Q   quit.
 * Log writes hold SIGALRM blocked (the tick reaches on_frame_game on this thread; musl stdio is not re-entrant --
 * tools/actors_test/oracle/README.txt). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "roth_sdk.h"

#define U8(a)  (*(volatile uint8_t  *)(uintptr_t)(a))
#define U16(a) (*(volatile uint16_t *)(uintptr_t)(a))
#define U32(a) (*(volatile uint32_t *)(uintptr_t)(a))

static const struct roth_game_ram_api_v1 *g;
static FILE *out;
static int g_logdepth; static sigset_t g_logold;
static void log_on(void) { if (g_logdepth++ == 0) { sigset_t s; sigemptyset(&s); sigaddset(&s, SIGALRM); sigprocmask(SIG_BLOCK, &s, &g_logold); } }
static void log_off(void) { if (--g_logdepth == 0) sigprocmask(SIG_SETMASK, &g_logold, 0); }

struct sym { char name[64]; uint32_t addr; };
static struct sym *g_sym; static int g_nsym;
static void load_syms(const char *path)
{
    FILE *f = fopen(path, "r"); if (!f) return;
    char line[512]; g_sym = malloc(sizeof(struct sym) * 16000);
    while (fgets(line, sizeof line, f) && g_nsym < 16000) {
        unsigned a; char t; char n[256];
        if (sscanf(line, "%x %c %255s", &a, &t, n) != 3) continue;
        if (t != 'T' && t != 't') continue;
        strncpy(g_sym[g_nsym].name, n, 63); g_sym[g_nsym].addr = a; g_nsym++;
    }
    fclose(f);
}
static uint32_t fn(const char *n)
{
    for (int i = 0; i < g_nsym; i++) if (!strcmp(g_sym[i].name, n)) return g_sym[i].addr;
    fprintf(stderr, "[boxprobe] NO SYMBOL %s\n", n); return 0;
}
typedef struct { uint32_t eax, ebx, ecx, edx, esi, edi, ebp, va, nstack, stack[8], eflags, es, fs, gs; } regs_t;   /* engine.h:24-39 */
typedef uint32_t (*rview_fn)(uint16_t, uint16_t, uint16_t);
typedef int32_t (*regs_fn)(const regs_t *);

struct step { char op; int a[7]; char label[64]; char samples[160]; };
static struct step *g_steps; static int g_nsteps, g_pc, g_wait = -1, g_settle = 3;
static uint32_t f_rview, f_use, f_exa;
static int g_geomDumped;

static void set_pose(const struct step *s)
{
    g->set_u16(0x90a8e, (uint16_t)s->a[0]); g->set_u16(0x90a96, (uint16_t)s->a[1]);
    g->set_u16(0x90a8a, (uint16_t)s->a[2]); g->set_u16(0x90c12, (uint16_t)s->a[3]);
    g->set_u16(0x90a92, (uint16_t)s->a[4]); g->set_u32(0x90a74, (uint32_t)s->a[5]);
    /* with g_view_pitch 0 the view takes g_view_pitch_applied (0x8c108, player.c:90-91), which the view bob eases
     * toward the floor clearance (player.c:353-361): held at 0 too, so pitch 0 means level */
    if (s->a[5] == 0) g->set_u32(0x8c108, 0);
}

/* The pose the frame was DRAWN with, read when run_gameplay_frame is entered, before this frame's set_pose: the
 * pick reuses the frame's view (render_world_view picks from the 3D pass just made), and the game's own
 * movement may have moved the player off the written pose (an object it collided with) before that pass. */
static int g_seenX, g_seenY, g_seenZ, g_seenA;
static void seen(void)
{
    g_seenX = (int16_t)g->u16(0x90a8e); g_seenY = (int16_t)g->u16(0x90a96);
    g_seenZ = (int16_t)g->u16(0x90a92); g_seenA = g->u16(0x90a8a);
}

static void pick(const struct step *s, int dx, int dy)
{
    int32_t mx0 = (int32_t)g->u32(0x707b3), my0 = (int32_t)g->u32(0x707b7);
    int32_t vw = (int32_t)g->u32(0x85cd8), vh = (int32_t)g->u32(0x85cdc);
    int32_t vx = (int32_t)g->u32(0x85ce0), vy = (int32_t)g->u32(0x85ce4);
    g->set_u32(0x707b3, (uint32_t)(vx + vw / 2 + dx)); g->set_u32(0x707b7, (uint32_t)(vy + vh / 2 + dy));
    uint32_t rec = ((rview_fn)(uintptr_t)f_rview)(0, 0, 0);
    char hex[0x24 * 2 + 1];
    for (int i = 0; i < 0x24; i++) sprintf(hex + 2 * i, "%02x", U8(rec + i));
    log_on();
    fprintf(out, "PK %s %d %d %d %d %d %d %d %s %d\n", s->label, g_seenX, g_seenY, g_seenZ, g_seenA, (int32_t)g->u32(0x90a74), dx, dy, hex,
        (int32_t)g->u32(0x89ee8));
    log_off();
    g->set_u32(0x707b3, (uint32_t)mx0); g->set_u32(0x707b7, (uint32_t)my0);
}

static void measure(const struct step *s)
{
    int x0 = 0, x1 = 0, xs = 1, y0 = 0, y1 = 0, ys = 1;
    if (sscanf(s->samples, "%d %d %d %d %d %d", &x0, &x1, &xs, &y0, &y1, &ys) != 6) { x0 = x1 = y0 = y1 = 0; }
    if (xs < 1) xs = 1;
    if (ys < 1) ys = 1;
    for (int dy = y0; dy <= y1; dy += ys)
        for (int dx = x0; dx <= x1; dx += xs) pick(s, dx, dy);
    log_on(); fflush(out); log_off();
}

static uint8_t g_rec[0x40];
static void match(const struct step *s)
{
    uint32_t oth = g->u32(0x85c30);                         /* VA_g_object_table_header (host pointer) */
    memset(g_rec, 0, sizeof g_rec);
    g_rec[1] = (uint8_t)s->a[0];
    *(uint16_t *)(g_rec + 4) = (uint16_t)s->a[4];
    *(uint16_t *)(g_rec + 6) = (uint16_t)s->a[5];
    *(uint16_t *)(g_rec + 8) = (uint16_t)s->a[1];
    *(uint16_t *)(g_rec + 0xa) = (uint16_t)s->a[2];
    g_rec[0x1a] = (uint8_t)s->a[3];
    regs_t io; memset(&io, 0, sizeof io); io.eax = (uint32_t)(uintptr_t)g_rec;
    uint32_t u = (uint32_t)((regs_fn)(uintptr_t)f_use)(&io);
    memset(&io, 0, sizeof io); io.eax = (uint32_t)(uintptr_t)g_rec; io.edx = io.eax;
    uint32_t e = (uint32_t)((regs_fn)(uintptr_t)f_exa)(&io);
    log_on();
    fprintf(out, "MT %s %u %u\n", s->label, u ? u - oth : 0, e ? e - oth : 0);
    log_off();
}

static void on_rgf(struct roth_chain *c, const struct roth_api_v1 *api)
{
    g = api->game_ram;
    if (g_pc < g_nsteps) {
        struct step *s = &g_steps[g_pc];
        if (s->op == 'Q') { log_on(); fprintf(out, "END\n"); fflush(out); log_off(); _Exit(0); }
        if (s->op == 'X') {
            uint32_t oth = g->u32(0x85c30);
            const uint32_t dirs[4] = { 0x18, 0x1c, 0x30, 0x34 };
            for (int k = 0; k < 4; k++) {
                uint32_t list = oth + U16(oth + dirs[k]); int n = U16(oth + dirs[k] + 2);
                for (int i = 0; i < n; i++) U8(oth + U16(list + 2u * (uint32_t)i) + 2) &= (uint8_t)~8u;
            }
            g_pc++;
        }
        if (s->op == 'M') {
            while (g_pc < g_nsteps && g_steps[g_pc].op == 'M') match(&g_steps[g_pc++]);
            log_on(); fflush(out); log_off();
            roth_next_run_gameplay_frame(c); return;
        }
        if (g_wait < 0 && !g_geomDumped) {
            /* the live geometry buffer (sectors, faces, texture maps, mid-platforms: the RAW file's bytes up to its
             * vertex section, as map_load.c loads them, after the load-time handlers and any run since), so the
             * port side reads the heights and slots the original draws with. Once, before the first pose. */
            uint32_t geom = g->u32(0x90aa8);
            uint32_t n = U16(geom);                             /* word 0 = the vertex section's offset */
            log_on();
            fprintf(out, "GEOM %u ", n);
            for (uint32_t i = 0; i < n; i++) fprintf(out, "%02x", U8(geom + i));
            fprintf(out, "\n");
            log_off();
            g_geomDumped = 1;
        }
        if (g_wait < 0) {
            g_wait = g_settle;
            log_on();
            fprintf(out, "ENV vw=%d vh=%d vx=%d vy=%d scale=%02x\n", (int32_t)g->u32(0x85cd8), (int32_t)g->u32(0x85cdc),
                (int32_t)g->u32(0x85ce0), (int32_t)g->u32(0x85ce4), g->u8(0x90bd4));
            log_off();
        }
        seen();
        set_pose(s);
        if (g_wait-- == 0) { measure(s); g_pc++; g_wait = -1; }
    }
    roth_next_run_gameplay_frame(c);
}

/* SB_NOCHAINS=1 (the texel scripts): no command chain runs and no movie plays while the script poses the
 * player. A pose drops the player into sectors whose enter triggers (twe_link_state) would otherwise run their
 * chains -- move sectors, swap pictures, start a cutscene that stops gameplay frames -- after the geometry the
 * port side reads (GEOM) was taken. The picks themselves run no chain. */
static int g_noChains;
static int32_t ov_chain(struct roth_chain *c, const struct roth_api_v1 *api, uint16_t ax)
{
    (void)api; if (g_noChains) return 0;
    return roth_next_reset_command_chain_state(c, ax);
}
static uint32_t ov_gdv(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t a, uint32_t d, uint32_t b, uint32_t cx)
{
    (void)api; if (g_noChains) return 0;
    return roth_next_play_gdv_cutscene(c, a, d, b, cx);
}

static void on_overrides(const struct roth_api_v1 *api, struct roth_registrar_v1 *reg)
{
    (void)api;
    roth_override(reg, ROTH_FN_run_gameplay_frame, on_rgf, 0);
    roth_override(reg, ROTH_FN_reset_command_chain_state, ov_chain, 0);
    roth_override(reg, ROTH_FN_play_gdv_cutscene, ov_gdv, 0);
}

static void on_load(const struct roth_api_v1 *api)
{
    (void)api; const char *s;
    out = fopen((s = getenv("SB_OUT")) ? s : "/tmp/boxprobe.log", "w");
    if ((s = getenv("SB_SYMS"))) load_syms(s);
    if ((s = getenv("SB_SETTLE"))) g_settle = atoi(s);
    if ((s = getenv("SB_NOCHAINS"))) g_noChains = atoi(s);
    f_rview = fn("render_world_view"); f_use = fn("dispatch_entry_command_trigger"); f_exa = fn("dispatch_entry_command_trigger_b");
    int cap = 1 << 20;
    g_steps = malloc(sizeof(struct step) * (size_t)cap);
    if ((s = getenv("SB_SCRIPT")) && g_steps) {
        FILE *f = fopen(s, "r"); char ln[400];
        while (f && fgets(ln, sizeof ln, f) && g_nsteps < cap) {
            if (ln[0] == '#' || ln[0] == '\n') continue;
            struct step *st = &g_steps[g_nsteps]; memset(st, 0, sizeof *st);
            st->op = ln[0];
            if (st->op == 'Q' || st->op == 'X') { g_nsteps++; continue; }
            char *bar = strchr(ln, '|');
            if (bar) { *bar = 0; strncpy(st->samples, bar + 1, sizeof st->samples - 1); }
            if (st->op == 'M') {
                if (sscanf(ln + 1, "%d %d %d %d %d %d %63s", &st->a[0], &st->a[1], &st->a[2], &st->a[3], &st->a[4], &st->a[5], st->label) == 7) g_nsteps++;
            } else if (st->op == 'P') {
                if (sscanf(ln + 1, "%d %d %d %d %d %d %63s", &st->a[0], &st->a[1], &st->a[2], &st->a[3], &st->a[4], &st->a[5], st->label) == 7) g_nsteps++;
            }
        }
        if (f) fclose(f);
    }
    fprintf(stderr, "[boxprobe] %d steps\n", g_nsteps);
}

static const struct roth_plugin_info_v1 INFO = {
    .abi_major = ROTH_ABI_MAJOR, .abi_minor = ROTH_ABI_MINOR, .struct_size = sizeof(struct roth_plugin_info_v1),
    .id = "io.github.vrealms.boxprobe", .name = "surface box probe", .version = "0.1",
    .sdk_req_major = ROTH_SDK_MAJOR, .sdk_req_minor = ROTH_SDK_MINOR, .api_use = ROTH_API_USE_ENGINE,
    .on_load = on_load, .on_register_overrides = on_overrides,
};
ROTH_PLUGIN_EXPORT const struct roth_plugin_info_v1 *roth_plugin_query_v1(void) { return &INFO; }
