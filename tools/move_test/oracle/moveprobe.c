/* moveprobe.c -- probe mod for the ROTH.C oracle (NOT part of the engine build).
 *
 * Drives the ORIGINAL's player with scripted held keys, one 70 Hz tick at a time, and logs the
 * player's movement state after every tick (player_movement_tick, the ISR body) and after every
 * frame's collision + vertical pass (move_player_with_collision). tools/move_test replays the same
 * events through src/roth/roth_playermove (the engine-free port) and diffs every line.
 *
 * Env: MOVP_SCRIPT (script), MOVP_OUT (log), MOVP_SYMS (nm symbol list).
 * Script: "<tick> <op> [args]" -- run before the tick numbered <tick> (the first gameplay tick is 1).
 *   keys <sc,sc,...|->   hold these scancodes (hex) from this tick on (the original's own held-key
 *                        dispatch runs on them: dispatch_held_key_actions over the table at 0x707f1)
 *   w8/w16/w32 VA VAL    poke a global (hex)
 *   pose X Y ANG SECOFF Z  put the player there (ints; SECOFF = the sector's geometry offset)
 *   pin VA VAL / unpin VA  re-poke a dword before every tick (the dialogue freeze, which the dialogue
 *                        code would otherwise clear)
 *   sech IDX CEIL FLOOR  set sector IDX's live heights (the step, fit, bonk and fall set-ups)
 *   isr SC              one scancode into the original's keyboard ISR (keyboard_int9_isr): make, or SC|0x80 break
 *   quit
 * Log lines (all numbers hex unless named):
 *   INIT ... / TAB ... / ES ... / GS ... / OB ... / DIM ...   the state and data the port starts from
 *   TK n <state>    after tick n         FR f fts=.. <state> ent=.. hit=..   after frame f's movement
 * The frame pass runs with SIGALRM blocked (a tick may not land inside it; the original's ISR could,
 * which only moves a tick by microseconds), so the tick/frame order in the log is the order to replay.
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
struct line { int tick; char op[16]; char args[200]; };
static struct line g_script[4096]; static int g_nscript, g_pc;
static int g_tick, g_frame, g_inited;
static uint8_t g_keys[16]; static int g_nkeys;
static uint32_t g_pinA[8], g_pinV[8]; static int g_npin;   /* globals re-poked before every tick (op pin) */
static char *g_syms;
static uint32_t sym(const char *name)
{
    size_t n = strlen(name); const char *p = g_syms;
    while (p && *p) { if (!strncmp(p, name, n) && p[n] == '=') return strtoul(p + n + 1, 0, 16);
        p = strchr(p, '\n'); if (p) p++; }
    fprintf(stderr, "[moveprobe] missing symbol %s\n", name); return 0;
}
static uint32_t P(uint32_t va) { return (uint32_t)(uintptr_t)g->to_ptr(va); }

static void state(void)
{
    fprintf(out, "x=%08x y=%08x z=%08x a=%04x vx=%08x vy=%08x rc=%04x sa=%08x ta=%04x ib=%04x ra=%u qs=%04x qa=%u qb=%u lf=%02x"
        " ab=%08x,%08x,%08x,%08x,%08x h=%04x cam=%04x hp=%08x sec=%04x pa=%08x bob=%08x fl=%08x\n",
        g->u32(0x90a8c), g->u32(0x90a94), g->u32(0x90a90), g->u16(0x90a8a), g->u32(0x90a80), g->u32(0x90a84), g->u16(0x90bda),
        g->u32(0x7e8d8), g->u16(0x90bd8), g->u16(0x7e920), g->u32(0x7e8e0), g->u16(0x90bd6), g->u16(0x90abe), g->u16(0x90b42),
        g->u8(0x819c0), g->u32(0x819c1), g->u32(0x819c5), g->u32(0x819c9), g->u32(0x819cd), g->u32(0x819d1),
        g->u16(0x8c110), g->u16(0x8c112), g->u32(0x8a0f0), g->u16(0x90c12), g->u32(0x8c108), g->u32(0x8c10c), g->u32(0x89f3b));
}
static void hexdump(const char *tag, uint32_t base, uint32_t n)
{
    fprintf(out, "%s %x", tag, n);
    for (uint32_t i = 0; i < n; i++) { if ((i & 0x3f) == 0) fprintf(out, "\n%s+ %x ", tag, i); fprintf(out, "%02x", U8(base + i)); }
    fprintf(out, "\n");
}
static void dump_init(void)
{
    /* everything the port starts from */
    fprintf(out, "INIT tick=%d mode=%02x cur=%02x rel=%02x rt=%08x mod=%02x msi=%04x pheight=%04x rest=%08x climb=%04x fit=%04x shift=%08x clear=%08x vpitch=%08x busy=%08x gate=%08x thud=%04x doors=%u pool=%u\n",
        g_tick, g->u8(0x7674a), g->u8(0x7e932), g->u8(0x7e8d0), g->u32(0x7e8dc), g->u8(0x90c42), g->u16(0x12570), g->u16(0x8c110),
        g->u32(0x8c114), g->u16(0x8c148), g->u16(0x90be0), g->u32(0x89f58), g->u32(0x8497c), g->u32(0x90a74), g->u32(0x83aea), g->u32(0x83125),
        g->u16(0x7fe38 + 0x18), g->u8(0x8b3f4), g->u32(0x91e00));
    fprintf(out, "INITQ"); for (int q = 0; q < 2; q++) { uint32_t b = q ? 0x90b42 : 0x90abe; fprintf(out, " %u:", g->u16(b));
        for (int i = 0; i < 16; i++) fprintf(out, "%08x,%08x;", g->u32(b + 2 + i * 8), g->u32(b + 6 + i * 8)); } fprintf(out, "\n");
    fprintf(out, "INITS "); state();
    /* the original's tables the movement reads */
    fprintf(out, "TAB jump"); for (int i = 0; i <= 0x14; i++) fprintf(out, " %d", (int32_t)g->u32(0x71304 + i * 4)); fprintf(out, "\n");
    fprintf(out, "TAB dir"); for (int i = 0; i < 16; i++) fprintf(out, " %04x", g->u16(0x707c9 + i * 2)); fprintf(out, "\n");
    fprintf(out, "TAB bob"); for (int i = 0; i < 32; i++) fprintf(out, " %d", (int16_t)g->u16(0x724f0 + i * 2)); fprintf(out, "\n");
    fprintf(out, "TAB sin"); for (int i = 0; i < 512; i++) fprintf(out, " %04x", g->u16(0x72080 + i * 2)); fprintf(out, "\n");
    fprintf(out, "TAB keys"); for (uint32_t e = 0x707f1; g->u8(e); e += 5) fprintf(out, " %02x:%x", g->u8(e), g->u32(e + 1) - P(0)); fprintf(out, "\n");
    fprintf(out, "TAB keymask"); for (int i = 0; i < 8; i++) fprintf(out, " %02x", g->u8(0x707e9 + i)); fprintf(out, "\n");
    /* the live geometry: section 0 (es) and the vertex section (gs) as the running game holds them */
    uint32_t es = g->u32(0x90aa8), gs = g->u32(0x90aac);
    hexdump("ES", es, U16(es + 0));
    hexdump("GS", gs, U16(es + 0x14));
    /* the objects buffer: per-sector list words from +2, the lists themselves */
    uint32_t ob = g->u32(0x90aa4);
    hexdump("OB", ob, U16(es + 0x16));
    /* the art's collision dimensions (4 bytes an id) */
    hexdump("DIM", g->u32(0x85c50), 0x1000);
    fflush(out);
}
static void set_keys(int on)
{
    for (int i = 0; i < g_nkeys; i++) {
        uint8_t sc = g_keys[i], mask = g->u8(0x707e9 + (sc & 7));
        uint32_t a = 0x90c3c + (sc >> 3);
        g->set_u8(a, on ? (uint8_t)(g->u8(a) | mask) : (uint8_t)(g->u8(a) & ~mask));
    }
}
static void run(struct line *l)
{
    if (!strcmp(l->op, "keys")) {
        set_keys(0); g_nkeys = 0;
        if (l->args[0] != '-') { char *p = l->args; while (*p && g_nkeys < 16) { g_keys[g_nkeys++] = (uint8_t)strtoul(p, &p, 16); if (*p == ',') p++; else break; } }
    }
    else if (!strcmp(l->op, "w8"))  { uint32_t a, v; sscanf(l->args, "%x %x", &a, &v); g->set_u8(a, (uint8_t)v); }
    else if (!strcmp(l->op, "w16")) { uint32_t a, v; sscanf(l->args, "%x %x", &a, &v); g->set_u16(a, (uint16_t)v); }
    else if (!strcmp(l->op, "w32")) { uint32_t a, v; sscanf(l->args, "%x %x", &a, &v); g->set_u32(a, v); }
    else if (!strcmp(l->op, "pose")) { int x, y, a, sc, z; if (sscanf(l->args, "%d %d %d %d %d", &x, &y, &a, &sc, &z) == 5) {
        g->set_u32(0x90a8c, (uint32_t)x << 16); g->set_u32(0x90a94, (uint32_t)y << 16); g->set_u32(0x90a90, (uint32_t)z << 16);
        g->set_u16(0x90a8a, (uint16_t)a); g->set_u16(0x90c12, (uint16_t)sc); } }
    else if (!strcmp(l->op, "sech")) { int i, c, f; if (sscanf(l->args, "%d %d %d", &i, &c, &f) == 3) {   /* a sector's live heights */
        uint32_t es = g->u32(0x90aa8); uint32_t o = es + U16(es + 4) + (uint32_t)i * 0x1a; U16(o) = (uint16_t)c; U16(o + 2) = (uint16_t)f; } }
    else if (!strcmp(l->op, "pin")) { uint32_t a, v; if (sscanf(l->args, "%x %x", &a, &v) == 2) { int i = 0;
        while (i < g_npin && g_pinA[i] != a) i++; if (i == g_npin && g_npin < 8) g_npin++; if (i < 8) { g_pinA[i] = a; g_pinV[i] = v; } } }
    else if (!strcmp(l->op, "unpin")) { uint32_t a = strtoul(l->args, 0, 16); for (int i = 0; i < g_npin; i++) if (g_pinA[i] == a) { g_pinA[i] = g_pinA[--g_npin]; g_pinV[i] = g_pinV[g_npin]; break; } }
    else if (!strcmp(l->op, "isr")) { typedef void (*f1)(uint32_t); uint32_t f = sym("keyboard_int9_isr");   /* a key into the original's ISR */
        if (f) ((f1)(uintptr_t)f)(strtoul(l->args, 0, 16)); }
    else if (!strcmp(l->op, "quit")) { fflush(out); _Exit(0); }
    else fprintf(stderr, "[moveprobe] unknown op %s\n", l->op);
    fprintf(out, "OP %d %s %s\n", g_tick, l->op, l->args);
}
static void ov_ptick(struct roth_chain *c, const struct roth_api_v1 *api)
{
    g = api->game_ram;
    if (g->u8(0x7674a) != 1) { roth_next_player_movement_tick(c); return; }
    log_on();
    if (!g_inited) { g_inited = 1; dump_init(); }
    g_tick++;
    while (g_pc < g_nscript && g_script[g_pc].tick <= g_tick) { run(&g_script[g_pc]); g_pc++; }
    for (int i = 0; i < g_npin; i++) g->set_u32(g_pinA[i], g_pinV[i]);
    set_keys(1);
    log_off();
    roth_next_player_movement_tick(c);
    log_on();
    set_keys(0);
    /* sh: the shift this tick used (latched by tick_world_effects); bz/gt: the dialogue freeze (the dialogue's) */
    fprintf(out, "TK %d sh=%08x bz=%08x gt=%08x ", g_tick, g->u32(0x89f58), g->u32(0x83aea), g->u32(0x83125)); state();
    log_off();
}
static void ov_move(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t q)
{
    g = api->game_ram;
    if (!g_inited || g->u8(0x7674a) != 1) { roth_next_move_player_with_collision(c, q); return; }
    log_on();                       /* the whole frame pass with the tick held off */
    if (g->u8(0x8b3f4) != 0) {      /* the door pool as this frame's collision sees it: key, the leaf's four points */
        fprintf(out, "DOORS %d", g_frame + 1);
        for (int i = 0; i < g->u8(0x8b3f4); i++) { uint32_t r = P(0x8b3f8) + i * 0x1f6; uint32_t v = U32(r + 0x2e) + 0x82;
            fprintf(out, " %04x", U16(r)); for (int k = 0; k < 4; k++) fprintf(out, ",%d,%d", (int16_t)U16(v + k * 0x10), (int16_t)U16(v + k * 0x10 + 4)); }
        uint32_t es = g->u32(0x90aa8); uint16_t first = U16(es + 4); int cnt = U16(es + first - 2);
        fprintf(out, "\nDF"); for (int i = 0; i < cnt; i++) if (U8(es + first + i * 0x1a + 0x16) & 1) fprintf(out, " %d", i); fprintf(out, "\n");
    }
    roth_next_move_player_with_collision(c, q);
    g_frame++;
    fprintf(out, "FR %d fts=%u ent=%u hit=%02x doors=%u clr=%08x ", g_frame, g->u32(0x85324), g->u32(0x8c12c), g->u8(0x8c1e0), g->u8(0x8b3f4), g->u32(0x8497c)); state();
    fflush(out);
    log_off();
}
static void on_overrides(const struct roth_api_v1 *api, struct roth_registrar_v1 *reg)
{
    (void)api;
    roth_override(reg, ROTH_FN_player_movement_tick, ov_ptick, 0);
    roth_override(reg, ROTH_FN_move_player_with_collision, ov_move, 0);
}
static void on_load(const struct roth_api_v1 *api)
{
    (void)api; const char *s;
    out = fopen((s = getenv("MOVP_OUT")) ? s : "/tmp/movp.log", "w");
    if ((s = getenv("MOVP_SYMS"))) { FILE *f = fopen(s, "r"); if (f) { g_syms = calloc(1, 1 << 20); fread(g_syms, 1, (1 << 20) - 1, f); fclose(f); } }
    if ((s = getenv("MOVP_SCRIPT"))) { FILE *f = fopen(s, "r"); char ln[256];
        while (f && fgets(ln, sizeof ln, f) && g_nscript < 4096) { if (ln[0] == '#' || ln[0] == '\n') continue;
            struct line *l = &g_script[g_nscript]; l->args[0] = 0;
            if (sscanf(ln, "%d %15s %199[^\n]", &l->tick, l->op, l->args) >= 2) g_nscript++; }
        if (f) fclose(f); }
    fprintf(stderr, "[moveprobe] %d script lines\n", g_nscript);
}
static const struct roth_plugin_info_v1 INFO = {
    .abi_major = ROTH_ABI_MAJOR, .abi_minor = ROTH_ABI_MINOR, .struct_size = sizeof(struct roth_plugin_info_v1),
    .id = "io.github.vrealms.moveprobe", .name = "player movement probe", .version = "0.1",
    .sdk_req_major = ROTH_SDK_MAJOR, .sdk_req_minor = ROTH_SDK_MINOR, .api_use = ROTH_API_USE_ENGINE,
    .on_load = on_load, .on_register_overrides = on_overrides,
};
ROTH_PLUGIN_EXPORT const struct roth_plugin_info_v1 *roth_plugin_query_v1(void) { return &INFO; }
