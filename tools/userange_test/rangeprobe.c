/* rangeprobe.c -- a mod for the headless ROTH.C build (/home/claude/oracle_build): measures the cursor pick's
 * depth word and what the original's own cursor logic does with it, at poses a script names.
 *
 * Overrides run_gameplay_frame (0x1691c). That runs on the game thread right after gameplay_frame_step has
 * drawn the frame (the 3D pass), which is exactly where the original itself picks (run_gameplay_frame ->
 * render_world_view 0x10c8f, game_core.c:578). Before passing through, at each scripted step it:
 *   1. on the step's first frame writes the pose (x, y, angle, sector, z, pitch) and waits RP_SETTLE frames
 *      (the pose is re-written every frame of the wait, so the 3D pass of the measured frame used it);
 *   2. for every sample point (dx, dy from the view centre, in the units of g_mouse_x/y 0x707b3/0x707b7)
 *      sets the mouse, calls the original's render_world_view (which calls render_world_scene with
 *      mouse - view origin) and logs the 0x24-byte record at 0x90a48 it returns, then hands that record to
 *      the original's classify_cursor_target_object (input.c:1111) and logs the cursor shape and the
 *      interaction type 0x7e932 / flags 0x7fdac / counter 0x7fdb0 / latch 0x7fdb4 it left;
 *   3. ops A / U: activate_targeted_object (input.c:1026) on the pick with EDX = 0x268, as the type-4 click does
 *      (game_core.c:686-697), logging the return and what the hooks below saw; op E: examine_object_under_cursor
 *      (input.c:979) on the pick.
 *   Log lines: PK (one pick), INJ (classify with a replaced depth), ACT / USE / EXA (the call), HOOK (what it
 *   reached), DOOR (the door pool, after D), ENV, END.
 * The mouse is put back after the samples. Log writes hold SIGALRM blocked (the tick reaches on_frame_game
 * on this thread; musl stdio is not re-entrant -- see tools/actors_test/oracle/README.txt).
 *
 * Script (RP_SCRIPT), one step per line:
 *   P x y angle sectorOffset z pitch label | samples
 *     samples: "c" centre; "s dx dy" a point; "col N" the centre column every N; "row N" the centre row every N;
 *              "inj v1 v2 ..." (last): the previous sample's pick (the centre if none) re-made, its record handed to classify with its depth word set to each v
 *   A x y angle sectorOffset z pitch label [| s dx dy] -- pose, pick (centre or dx,dy), classify, then activate
 *   E x y angle sectorOffset z pitch label [| s dx dy] -- pose, pick (centre or dx,dy), classify, then examine
 *   U x y angle sectorOffset z pitch label [| s dx dy] -- as A, but what the click fires really runs (no suppression)
 *   D x y angle sectorOffset z pitch label            -- pose, then the Space key's dev_open_nearest_door (dev gate set for the call)
 *   W n                                               -- let n gameplay frames pass
 *   Q                                                 -- quit
 * Log (RP_OUT): ENV (view geometry), then per sample
 *   PK label px py pz pa pitch sx sy kind type w2 w4 w6 w8 wa de w1c depth cursor itype flags cnt latch
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "roth_sdk.h"

#define U32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define U16(a) (*(volatile uint16_t *)(uintptr_t)(a))
#define U8(a)  (*(volatile uint8_t  *)(uintptr_t)(a))

static const struct roth_game_ram_api_v1 *g;
static FILE *out;
static int g_logdepth; static sigset_t g_logold;
static void log_on(void) { if (g_logdepth++ == 0) { sigset_t s; sigemptyset(&s); sigaddset(&s, SIGALRM); sigprocmask(SIG_BLOCK, &s, &g_logold); } }
static void log_off(void) { if (--g_logdepth == 0) sigprocmask(SIG_SETMASK, &g_logold, 0); }

/* ---- symbols (nm of the roth binary) ---- */
struct sym { char name[64]; uint32_t addr; };
static struct sym *g_sym; static int g_nsym;
static void load_syms(const char *path)
{
    FILE *f = fopen(path, "r"); if (!f) return;
    char line[512]; g_sym = malloc(sizeof(struct sym) * 16000);
    while (fgets(line, sizeof line, f) && g_nsym < 16000) {
        unsigned a; char t; char n[256];
        if (sscanf(line, "%x %c %255s", &a, &t, n) != 3) continue;
        if (!strchr("TtBbDd", t)) continue;
        strncpy(g_sym[g_nsym].name, n, 63); g_sym[g_nsym].addr = a; g_nsym++;
    }
    fclose(f);
}
static uint32_t fn(const char *n)
{
    for (int i = 0; i < g_nsym; i++) if (!strcmp(g_sym[i].name, n)) return g_sym[i].addr;
    fprintf(stderr, "[rangeprobe] NO SYMBOL %s\n", n); return 0;
}
/* regs_t of the build (engine.h:24-39) */
typedef struct { uint32_t eax, ebx, ecx, edx, esi, edi, ebp, va, nstack, stack[8], eflags, es, fs, gs; } regs_t;
static uint32_t fn_any(const char *n) { return fn(n); }
typedef uint32_t (*rview_fn)(uint16_t, uint16_t, uint16_t);
typedef int32_t (*regs_fn)(const regs_t *);

/* ---- script ---- */
struct step { char op; int x, y, a, sec, z, pitch; char label[48]; char samples[200]; };
static struct step g_steps[4096]; static int g_nsteps, g_pc, g_wait = -1;
static int g_settle = 3;

static void set_pose(const struct step *s)
{
    g->set_u16(0x90a8e, (uint16_t)s->x); g->set_u16(0x90a96, (uint16_t)s->y);
    g->set_u16(0x90a8a, (uint16_t)s->a); g->set_u16(0x90c12, (uint16_t)s->sec);
    g->set_u16(0x90a92, (uint16_t)s->z); g->set_u32(0x90a74, (uint32_t)s->pitch);
}

static int g_inact;
static uint32_t g_rec;   /* host pointer of 0x90a48, as render_world_view returns it */
static int g_lastdx, g_lastdy;
static void pick(const struct step *s, int dx, int dy, int classify)
{
    g_lastdx = dx; g_lastdy = dy;
    int32_t mx0 = (int32_t)g->u32(0x707b3), my0 = (int32_t)g->u32(0x707b7);
    int32_t vw = (int32_t)g->u32(0x85cd8), vh = (int32_t)g->u32(0x85cdc);
    int32_t vx = (int32_t)g->u32(0x85ce0), vy = (int32_t)g->u32(0x85ce4);
    int32_t sx = vx + vw / 2 + dx, sy = vy + vh / 2 + dy;
    g->set_u32(0x707b3, (uint32_t)sx); g->set_u32(0x707b7, (uint32_t)sy);
    g_rec = ((rview_fn)(uintptr_t)fn("render_world_view"))(0, 0, 0);
    uint8_t r[0x24]; for (int i = 0; i < 0x24; i++) r[i] = U8(g_rec + i);
    int32_t cursor = -1;
    if (classify && r[1]) { regs_t io; memset(&io, 0, sizeof io); io.eax = g_rec; io.edx = r[1];
        cursor = ((regs_fn)(uintptr_t)fn("classify_cursor_target_object"))(&io); }
    int ox = 0, oy = 0, oz = 0; uint32_t def = *(uint32_t *)(r + 0xe);
    if (r[1] == 4 && def) { ox = (int16_t)U16(def); oy = (int16_t)U16(def + 2); oz = (int16_t)U16(def + 10); }
    log_on();
    fprintf(out, "PK %s %d %d %d %d %d %d %d %u %u %04x %04x %04x %04x %04x %08x %04x %d %x %u %02x %d %d obj %d %d %d\n",
        s->label, (int16_t)g->u16(0x90a8e), (int16_t)g->u16(0x90a96), (int16_t)g->u16(0x90a92), g->u16(0x90a8a), (int32_t)g->u32(0x90a74),
        dx, dy, r[0], r[1], *(uint16_t *)(r + 2), *(uint16_t *)(r + 4), *(uint16_t *)(r + 6), *(uint16_t *)(r + 8), *(uint16_t *)(r + 0xa),
        *(uint32_t *)(r + 0xe), *(uint16_t *)(r + 0x1c), *(int32_t *)(r + 0x20),
        cursor, g->u8(0x7e932), g->u8(0x7fdac), (int32_t)g->u32(0x7fdb0), (int32_t)g->u32(0x7fdb4), ox, oy, oz);
    fflush(out);
    log_off();
    g->set_u32(0x707b3, (uint32_t)mx0); g->set_u32(0x707b7, (uint32_t)my0);
}

static void env(void)
{
    log_on();
    fprintf(out, "ENV vw=%d vh=%d vx=%d vy=%d scale=%02x mouse=%d,%d eye=%d cx=%d\n",
        (int32_t)g->u32(0x85cd8), (int32_t)g->u32(0x85cdc), (int32_t)g->u32(0x85ce0), (int32_t)g->u32(0x85ce4), g->u8(0x90bd4),
        (int32_t)g->u32(0x707b3), (int32_t)g->u32(0x707b7), (int16_t)g->u16(0x8c110), (int16_t)g->u16(0x90a70));
    for (int i = 0; i < g->u8(0x8b3f4); i++) {        /* the door pool (doors.c:12-13): id = byte offset */
        fprintf(out, "DOOR id=%04x", i * 0x1f6);
        for (int k = 0; k < 0x20; k += 2) fprintf(out, " %04x", g->u16(0x8b3f8 + i * 0x1f6 + k));
        fprintf(out, "\n");
    }
    fflush(out);
    log_off();
}

static void measure(const struct step *s)
{
    char buf[200]; strcpy(buf, s->samples);
    g_lastdx = g_lastdy = 0;
    char *tok = strtok(buf, " ");
    while (tok) {
        if (!strcmp(tok, "c")) pick(s, 0, 0, 1);
        else if (!strcmp(tok, "s")) { char *a = strtok(0, " "), *b = strtok(0, " "); if (a && b) pick(s, atoi(a), atoi(b), 1); }
        else if (!strcmp(tok, "col") || !strcmp(tok, "row")) {
            int col = tok[0] == 'c'; char *a = strtok(0, " "); int n = a ? atoi(a) : 4; if (n < 1) n = 1;
            int half = col ? (int)g->u32(0x85cdc) / 2 : (int)g->u32(0x85cd8) / 2;
            for (int k = -half; k < half; k += n) { if (col) pick(s, 0, k, 1); else pick(s, k, 0, 1); }
        }
        else if (!strcmp(tok, "inj")) {
            /* the ORIGINAL's classify on the previous sample point's record with its depth word replaced: a test of the
             * thresholds in the code, not of the geometry (logged as INJ, never as PK) */
            pick(s, g_lastdx, g_lastdy, 0);
            int32_t d0 = (int32_t)U32(g_rec + 0x20);
            char *v;
            while ((v = strtok(0, " "))) {
                U32(g_rec + 0x20) = (uint32_t)atoi(v);
                regs_t io; memset(&io, 0, sizeof io); io.eax = g_rec; io.edx = U8(g_rec + 1);
                int32_t cur = U8(g_rec + 1) ? ((regs_fn)(uintptr_t)fn("classify_cursor_target_object"))(&io) : -1;
                log_on();
                fprintf(out, "INJ %s type=%u depth=%d cursor=%x itype=%u flags=%02x cnt=%d latch=%d\n", s->label, U8(g_rec + 1), atoi(v), cur,
                    g->u8(0x7e932), g->u8(0x7fdac), (int32_t)g->u32(0x7fdb0), (int32_t)g->u32(0x7fdb4));
                fflush(out); log_off();
            }
            U32(g_rec + 0x20) = (uint32_t)d0;
            break;
        }
        tok = strtok(0, " ");
    }
}

static uint32_t door_state(uint16_t id)
{
    /* the door pool: count 0x8b3f4, records of 0x1f6 at 0x8b3f8 (actprobe.c DOORS) */
    for (int i = 0; i < g->u8(0x8b3f4); i++) if (g->u16(0x8b3f8 + i * 0x1f6) == id) return 0x10000u | (uint32_t)i;
    return 0;
}

static void on_rgf(struct roth_chain *c, const struct roth_api_v1 *api)
{
    g = api->game_ram;
    if (g_pc < g_nsteps) {
        struct step *s = &g_steps[g_pc];
        if (s->op == 'Q') { log_on(); fprintf(out, "END\n"); fflush(out); log_off(); _Exit(0); }
        if (s->op == 'D') {   /* the retail Space key's dev_open_nearest_door (input.c:185-194), with its dev gate 0x7f560 bit 0 set */
            set_pose(s);
            if (g_wait < 0) g_wait = g_settle;
            if (g_wait-- == 0) {
                typedef uint32_t (*selfn)(uint16_t); selfn sel = *(selfn *)(uintptr_t)fn_any("g_os_sel_base");
                typedef void (*dfn)(uint32_t, uint32_t, uint32_t, uint32_t);
                uint8_t dev = g->u8(0x7f560); g->set_u8(0x7f560, dev | 1);
                ((dfn)(uintptr_t)fn("dev_open_nearest_door"))(sel(g->u16(0x90be8)), sel(g->u16(0x90bec)), sel(g->u16(0x852c8)), sel(g->u16(0x852cc)));
                g->set_u8(0x7f560, dev);
                env();
                g_pc++; g_wait = -1;
            }
            roth_next_run_gameplay_frame(c); return;
        }
        if (s->op == 'W') { if (g_wait < 0) g_wait = s->x; if (g_wait-- == 0) { g_pc++; g_wait = -1; } roth_next_run_gameplay_frame(c); return; }
        if (g_wait < 0) { if (g_pc == 0) env(); g_wait = g_settle; }
        set_pose(s);
        if (g_wait-- == 0) {
            if (s->op == 'P') measure(s);
            else {
                int adx = 0, ady = 0; sscanf(s->samples, " s %d %d", &adx, &ady);
                pick(s, adx, ady, 1);
                uint8_t type = U8(g_rec + 1); uint16_t door = U16(g_rec + 0x1c); uint32_t def = U32(g_rec + 0xe);
                uint32_t d0 = type == 6 ? door_state(door) : 0;
                uint32_t pool0 = type == 6 && d0 ? g->u32(0x8b3f8 + (d0 & 0xffff) * 0x1f6 + 4) : 0;
                uint8_t obj7 = type == 4 && def ? U8(def + 7) : 0;
                int32_t mx0 = (int32_t)g->u32(0x707b3), my0 = (int32_t)g->u32(0x707b7);
                g->set_u32(0x707b3, (uint32_t)((int32_t)g->u32(0x85ce0) + (int32_t)g->u32(0x85cd8) / 2 + adx));
                g->set_u32(0x707b7, (uint32_t)((int32_t)g->u32(0x85ce4) + (int32_t)g->u32(0x85cdc) / 2 + ady));
                regs_t io; memset(&io, 0, sizeof io); io.eax = g_rec; io.edx = 0x268;
                int32_t r;
                g_inact = s->op != 'U';
                if (s->op == 'A' || s->op == 'U') r = ((regs_fn)(uintptr_t)fn("activate_targeted_object"))(&io);
                else r = ((regs_fn)(uintptr_t)fn("examine_object_under_cursor"))(&io);
                g_inact = 0;
                g->set_u32(0x707b3, (uint32_t)mx0); g->set_u32(0x707b7, (uint32_t)my0);
                uint32_t pool1 = type == 6 && d0 ? g->u32(0x8b3f8 + (d0 & 0xffff) * 0x1f6 + 4) : 0;
                log_on();
                fprintf(out, "%s %s ret=%x type=%u door=%04x doorrec=%x pool=%08x->%08x obj7=%02x->%02x pending=%08x\n",
                    s->op == 'E' ? "EXA" : s->op == 'U' ? "USE" : "ACT", s->label, (uint32_t)r, type, door, d0, pool0, pool1, obj7, type == 4 && def ? U8(def + 7) : 0,
                    g->u32(0x8a0dc));
                fflush(out);
                log_off();
            }
            g_pc++; g_wait = -1;
        }
    }
    roth_next_run_gameplay_frame(c);
}

/* While the probe's own activate/examine call runs (g_inact), these log what the original reached. The chain
 * (fire_command_contact_trigger), the examine dialogue (examine_world_object) and the OnInspect evaluation are
 * logged and NOT run: they can open a dialogue or a movie, which stops gameplay frames and so the script. The
 * door toggle and the pickup run (they are what the step measures). */
static void hook(const char *n, uint32_t a, uint32_t b)
{
    log_on(); fprintf(out, "HOOK %s %x %x\n", n, a, b); fflush(out); log_off();
}
static int32_t ov_contact(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t rec)
{
    (void)api; if (!g_inact) return roth_next_fire_command_contact_trigger(c, rec);
    hook("fire_command_contact_trigger(suppressed)", rec ? U16(rec + 8) : 0, rec ? U8(rec + 3) : 0); return -1;
}
static void ov_exa(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t rec)
{
    (void)api; if (!g_inact) { roth_next_examine_world_object(c, rec); return; }
    hook("examine_world_object(suppressed)", rec ? U16(rec + 8) : 0, rec ? U8(rec + 3) : 0);
}
static uint32_t ov_eval(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t blk, uint32_t e, uint32_t *p1, uint32_t *p2)
{
    (void)api; if (!g_inact) return roth_next_eval_dialogue_record_condition_with_cleanup(c, blk, e, p1, p2);
    hook("eval_dialogue_record_condition_with_cleanup(suppressed)", blk, e); return 0;
}
static uint32_t ov_give(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t id, uint32_t def)
{
    (void)api; uint32_t r = roth_next_give_item_by_dbase_id(c, id, def);
    if (g_inact) hook("give_item_by_dbase_id", id, r);
    return r;
}
static uint32_t ov_door(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t id)
{
    (void)api; uint32_t r = roth_next_toggle_door_open_state(c, id);
    if (g_inact) hook("toggle_door_open_state", id, r);
    return r;
}

static void on_overrides(const struct roth_api_v1 *api, struct roth_registrar_v1 *reg)
{
    (void)api;
    roth_override(reg, ROTH_FN_run_gameplay_frame, on_rgf, 0);
    roth_override(reg, ROTH_FN_fire_command_contact_trigger, ov_contact, 0);
    roth_override(reg, ROTH_FN_examine_world_object, ov_exa, 0);
    roth_override(reg, ROTH_FN_eval_dialogue_record_condition_with_cleanup, ov_eval, 0);
    roth_override(reg, ROTH_FN_give_item_by_dbase_id, ov_give, 0);
    roth_override(reg, ROTH_FN_toggle_door_open_state, ov_door, 0);
}

static void on_load(const struct roth_api_v1 *api)
{
    (void)api; const char *s;
    out = fopen((s = getenv("RP_OUT")) ? s : "/tmp/rangeprobe.log", "w");
    if ((s = getenv("RP_SYMS"))) load_syms(s);
    if ((s = getenv("RP_SETTLE"))) g_settle = atoi(s);
    if ((s = getenv("RP_SCRIPT"))) {
        FILE *f = fopen(s, "r"); char ln[400];
        while (f && fgets(ln, sizeof ln, f) && g_nsteps < 4096) {
            if (ln[0] == '#' || ln[0] == '\n') continue;
            struct step *st = &g_steps[g_nsteps]; memset(st, 0, sizeof *st);
            st->op = ln[0];
            if (st->op == 'Q') { g_nsteps++; continue; }
            if (st->op == 'W') { st->x = atoi(ln + 1); g_nsteps++; continue; }
            char *bar = strchr(ln, '|'); if (bar) { *bar = 0; strncpy(st->samples, bar + 1, sizeof st->samples - 1);
                char *nl = strchr(st->samples, '\n'); if (nl) *nl = 0; }
            if (sscanf(ln + 1, "%d %d %d %d %d %d %47s", &st->x, &st->y, &st->a, &st->sec, &st->z, &st->pitch, st->label) == 7) g_nsteps++;
        }
        if (f) fclose(f);
    }
    fprintf(stderr, "[rangeprobe] %d steps\n", g_nsteps);
}

static const struct roth_plugin_info_v1 INFO = {
    .abi_major = ROTH_ABI_MAJOR, .abi_minor = ROTH_ABI_MINOR, .struct_size = sizeof(struct roth_plugin_info_v1),
    .id = "io.github.vrealms.rangeprobe", .name = "use-range probe", .version = "0.1",
    .sdk_req_major = ROTH_SDK_MAJOR, .sdk_req_minor = ROTH_SDK_MINOR, .api_use = ROTH_API_USE_ENGINE,
    .on_load = on_load, .on_register_overrides = on_overrides,
};
ROTH_PLUGIN_EXPORT const struct roth_plugin_info_v1 *roth_plugin_query_v1(void) { return &INFO; }
