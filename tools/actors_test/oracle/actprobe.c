/* actprobe.c -- probe mod for the ROTH.C oracle (NOT part of the repo patch).
 * Drives the original through a script (ACTP_SCRIPT) and logs actor/object state (ACTP_OUT).
 * Script lines: "<gameframe> <op> [args]"; ops: dump LABEL | cmd IDX | src SEC IDX | w8/w16/w32 VA VAL |
 *   trace 0/1 | call NAME a b c d | quit */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "roth_sdk.h"
#define U32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define U16(a) (*(volatile uint16_t *)(uintptr_t)(a))
#define U8(a)  (*(volatile uint8_t  *)(uintptr_t)(a))
static const struct roth_game_ram_api_v1 *g;
static FILE *out;
static int g_frames, g_trace, g_ticktrace;
struct line { int frame; char op[16]; char args[200]; };
static struct line g_script[4096]; static int g_nscript, g_pc;
static char *g_syms;
static uint32_t sym(const char *name)
{
    size_t n = strlen(name); const char *p = g_syms;
    while (p && *p) { if (!strncmp(p, name, n) && p[n] == '=') return strtoul(p + n + 1, 0, 16);
        p = strchr(p, '\n'); if (p) p++; }
    fprintf(stderr, "[actprobe] missing symbol %s\n", name); return 0;
}
static uint32_t P(uint32_t va) { return (uint32_t)(uintptr_t)g->to_ptr(va); }
static uint32_t arena(void) { return g->u32(0x90aa4); }
/* object host ptr -> "sector.index" */
static void objref(uint32_t obj, char *buf)
{
    uint32_t a = arena(); int nsec = (int)g->u32(0x91df8);
    if (!obj) { strcpy(buf, "-"); return; }
    for (int s = 0; s < nsec; s++) { uint16_t off = U16(a + 2 + s * 2); if (!off) continue;
        uint32_t l = a + off; int cnt = U8(l);
        if (obj >= l + 2 && obj < l + 2 + (uint32_t)cnt * 16 && ((obj - l - 2) & 15) == 0) { sprintf(buf, "%d.%d", s, (int)((obj - l - 2) / 16)); return; } }
    if (obj >= P(0x89fac) && obj < P(0x89fac) + 0x12c) { sprintf(buf, "drop+%x", obj - P(0x89fac)); return; }
    sprintf(buf, "?%x", obj - a);
}
static uint32_t objptr(int s, int i) { uint32_t a = arena(); uint16_t off = U16(a + 2 + s * 2); return off ? a + off + 2 + i * 16 : 0; }
static void dump_objects(void)
{
    uint32_t a = arena(); int nsec = (int)g->u32(0x91df8);
    for (int s = 0; s < nsec; s++) { uint16_t off = U16(a + 2 + s * 2); if (!off) continue;
        uint32_t l = a + off; int cnt = U8(l);
        for (int i = 0; i < cnt; i++) { uint32_t o = l + 2 + i * 16;
            fprintf(out, "OBJ %d.%d x=%d y=%d id=%04x rot=%02x f7=%02x lt=%02x f9=%02x z=%d c=%04x cmd=%04x\n", s, i,
                (int16_t)U16(o), (int16_t)U16(o + 2), U16(o + 4), U8(o + 6), U8(o + 7), U8(o + 8), U8(o + 9), (int16_t)U16(o + 10), U16(o + 12), U16(o + 14)); } }
}
static void dump_pools(void)
{
    char b[32]; uint32_t mt = g->u32(0x85cf4);
    for (int i = 0; i < 16; i++) { uint32_t s = P(0x91e04) + i * 0x22; if (!U32(s) && !U32(s + 4)) continue;
        uint32_t rec = U32(s); uint32_t def = rec ? U32(rec) : 0; objref(U32(s + 4), b);
        fprintf(out, "ACT %d mon=%d def=%04x obj=%s st=%02x f9=%02x fa=%02x fb=%02x hp=%d lo=%04x,%04x,%04x sec=%04x vel=%04x ctr=%04x proj=%04x dly=%02x acc=%02x atk=%02x pain=%02x\n", i,
            rec ? (int)((rec - mt) / 0x68) : -1, def ? U16(def + 4) : 0, b, U8(s + 8), U8(s + 9), U8(s + 10), U8(s + 11), (int32_t)U32(s + 12),
            U16(s + 0x10), U16(s + 0x12), U16(s + 0x14), U16(s + 0x16), U16(s + 0x18), U16(s + 0x1a), U16(s + 0x1c), U8(s + 0x1e), U8(s + 0x1f), U8(s + 0x20), U8(s + 0x21)); }
    for (int i = 0; i < 16; i++) { uint32_t r = P(0x90fe4) + i * 0x1c; if (!U32(r)) continue; objref(U32(r), b);
        fprintf(out, "PRJ %d obj=%s ang=%04x sec=%04x mode=%02x w=%02x fx=%04x fy=%04x fz=%04x zv=%d type=%04x dmg=%04x spd=%02x ttl=%02x own=%04x\n", i, b,
            U16(r + 4), U16(r + 6), U8(r + 8), U8(r + 9), U16(r + 10), U16(r + 12), U16(r + 14), (int32_t)U32(r + 0x10), U16(r + 0x14), U16(r + 0x16), U8(r + 0x18), U8(r + 0x19), U16(r + 0x1a)); }
}
static void dump_globals(void)
{
    char b[32]; objref(g->u32(0x8a100), b);
    fprintf(out, "GLB hp=%d maxhp=%d arm=%d flash=%d rngd=%04x rnga=%08x rngs=%08x,%08x sched=%d src=%s intr=%02x nextid=%08x na=%u np=%u\n",
        (int32_t)g->u32(0x8a0f0), (int32_t)g->u32(0x7fe44), (int32_t)g->u32(0x81e30), (int32_t)g->u32(0x89f3b), g->u16(0x7272c), g->u32(0x72730), g->u32(0x72734), g->u32(0x72738),
        (int32_t)g->u32(0x7273c), b, g->u8(0x8a268), g->u32(0x8a0c0), g->u32(0x91e00), g->u32(0x90fe0));
}
static void dump_env(void)
{
    uint32_t selp = g->u32(0x81044);
    fprintf(out, "ENV sel=%d frame=%d fts=%u free=%u px=%d py=%d pz=%d pa=%d ph=%d psec=%04x tflags=%08x light=%u pzf=%u pitch=%d pitcha=%d\n",
        selp ? (int)(int16_t)U16(selp) : 0, g_frames, g->u32(0x85324), g->u32(0x85c28),
        (int16_t)g->u16(0x90a8e), (int16_t)g->u16(0x90a96), (int16_t)g->u16(0x90a92), g->u16(0x90a8a), g->u16(0x8c110), g->u16(0x90c12), g->u32(0x81f02), g->u8(0x853f6), g->u16(0x90a90), (int32_t)g->u32(0x90a74), (int32_t)g->u32(0x8c108));
}
typedef uint32_t (*fn1)(uint32_t);
typedef uint32_t (*fn4)(uint32_t, uint32_t, uint32_t, uint32_t);
static void run(struct line *l)
{
    if (!strcmp(l->op, "dump")) { dump_env(); fprintf(out, "== %s\n", l->args); dump_globals(); dump_objects(); dump_pools(); }
    else if (!strcmp(l->op, "glb")) { dump_env(); fprintf(out, "== %s\n", l->args); dump_globals(); dump_pools(); }
    else if (!strcmp(l->op, "trace")) g_trace = atoi(l->args);
    else if (!strcmp(l->op, "w8")) { uint32_t a, v; sscanf(l->args, "%x %x", &a, &v); g->set_u8(a, (uint8_t)v); }
    else if (!strcmp(l->op, "w16")) { uint32_t a, v; sscanf(l->args, "%x %x", &a, &v); g->set_u16(a, (uint16_t)v); }
    else if (!strcmp(l->op, "w32")) { uint32_t a, v; sscanf(l->args, "%x %x", &a, &v); g->set_u32(a, v); }
    else if (!strcmp(l->op, "src")) { int s = -1, i = 0; sscanf(l->args, "%d %d", &s, &i); g->set_u32(0x8a100, s < 0 ? 0 : objptr(s, i)); }
    else if (!strcmp(l->op, "cmd")) { int idx = atoi(l->args);
        uint32_t rec = ((fn1)(uintptr_t)sym("resolve_command_by_index"))((uint32_t)idx);
        if (!rec) { fprintf(out, "CMD %d norec\n", idx); return; }
        uint8_t op = U8(rec + 3) & 0x7f; const char *h = op == 0x16 ? "cmd_spawn_object" : op == 0x3c ? "cmd_spawn_object_adv" : op == 0x3a ? "cmd_change_object_id" : op == 0x33 ? "cmd_apply_damage" : 0;
        if (!h) { fprintf(out, "CMD %d op=%02x unsupported\n", idx, op); return; }
        g->set_u8(0x8a268, 0);
        int32_t r = (int32_t)((fn1)(uintptr_t)sym(h))(rec);
        char b[32]; objref(g->u32(0x8a100), b);
        fprintf(out, "CMD %d op=%02x ret=%d intr=%u mod=%02x src=%s\n", idx, op, r, g->u8(0x8a268), U8(rec + 2), b); }
    else if (!strcmp(l->op, "call")) { char name[64]; uint32_t a = 0, b = 0, c = 0, d = 0; sscanf(l->args, "%63s %x %x %x %x", name, &a, &b, &c, &d);
        uint32_t f = sym(name); if (f) { uint32_t r = ((fn4)(uintptr_t)f)(a, b, c, d); fprintf(out, "CALL %s(%x,%x,%x,%x)=%x\n", name, a, b, c, d, r); } }
    else if (!strcmp(l->op, "unhide")) { int s2 = 0, i2 = 0; sscanf(l->args, "%d %d", &s2, &i2); uint32_t o = objptr(s2, i2); if (o) U8(o + 7) &= 0x7f; }
    else if (!strcmp(l->op, "ticktrace")) g_ticktrace = atoi(l->args);
    else if (!strcmp(l->op, "fire")) { typedef uint32_t (*f3)(uint32_t, uint32_t, uint32_t); uint32_t item = strtoul(l->args, 0, 0);
        uint32_t r = ((f3)(uintptr_t)sym("spawn_projectile_from_aim"))(item, 0, 0); fprintf(out, "FIRE %u %d\n", item, r != 0); }
    else if (!strcmp(l->op, "fall")) { int fl, ce, st; sscanf(l->args, "%d %d %d", &fl, &ce, &st); typedef uint32_t (*f3)(uint32_t, uint32_t, uint32_t);
        f3 f = (f3)(uintptr_t)sym("update_player_vertical_physics"); uint32_t sv[6]; for (int i = 0; i < 6; i++) sv[i] = g->u32(0x819c1 + i * 4 - (i ? 0 : 0));
        uint16_t svh = g->u16(0x8c110); int32_t hp0 = (int32_t)g->u32(0x8a0f0), fl0 = (int32_t)g->u32(0x89f3b);
        g->set_u32(0x819c1, 0); g->set_u32(0x819c5, 0); g->set_u32(0x819c9, 0); g->set_u32(0x819cd, 0); g->set_u32(0x819d1, 0);
        int32_t pos = st, maxacc = 0, n = 0;
        for (; n < 2000; n++) { pos = (int32_t)f((uint32_t)fl, (uint32_t)ce, (uint32_t)pos); int32_t acc = (int32_t)g->u32(0x819d1); if (acc > maxacc) maxacc = acc;
            if ((int32_t)g->u32(0x8a0f0) != hp0 || (n > 2 && g->u32(0x819c5) == 0)) break; }
        fprintf(out, "FALL drop=%d acc=%d dmg=%d\n", st - fl, maxacc, hp0 - (int32_t)g->u32(0x8a0f0));
        g->set_u32(0x8a0f0, (uint32_t)hp0); g->set_u32(0x89f3b, (uint32_t)fl0); g->set_u16(0x8c110, svh);
        for (int i = 0; i < 5; i++) g->set_u32(0x819c1 + i * 4, 0); (void)sv; }
    else if (!strcmp(l->op, "trig")) { typedef uint32_t (*fb)(int16_t, int16_t, int16_t, int16_t);
        fprintf(out, "SIN"); for (int i = 0; i < 512; i++) fprintf(out, " %04x", g->u16(0x72080 + i * 2)); fprintf(out, "\n");
        fb f = (fb)(uintptr_t)sym("atan2_bearing"); fn1 sq = (fn1)(uintptr_t)sym("isqrt_fixed");
        fprintf(out, "ATN"); for (int i = 0; i < 512; i++) fprintf(out, " %03x", f(0, (int16_t)((int16_t)g->u16(0x72080 + ((i * 2 + 0x100) & 0x3fe)) / 16), 0, (int16_t)((int16_t)g->u16(0x72080 + i * 2) / 16))); fprintf(out, "\n");
        fprintf(out, "SQR"); for (uint32_t v = 1; v < 0x40000000u; v = v * 3 + 7) fprintf(out, " %x", sq(v)); fprintf(out, "\n"); }
    else if (!strcmp(l->op, "quit")) { fflush(out); _Exit(0); }
    else fprintf(stderr, "[actprobe] unknown op %s\n", l->op);
}
/* ---- tick-level trace: overrides ---- */
static int g_tick, g_left, g_inmove, g_prevact[16];
static void dump_pool_objects(void)
{
    char b[32];
    for (int i = 0; i < 16; i++) { uint32_t s = P(0x91e04) + i * 0x22; uint32_t o = U32(s + 4); if (!o) continue; objref(o, b);
        fprintf(out, "AOB %s x=%d y=%d id=%04x rot=%02x f7=%02x lt=%02x f9=%02x z=%d c=%04x cmd=%04x\n", b, (int16_t)U16(o), (int16_t)U16(o + 2), U16(o + 4), U8(o + 6), U8(o + 7), U8(o + 8), U8(o + 9), (int16_t)U16(o + 10), U16(o + 12), U16(o + 14)); }
    for (int i = 0; i < 16; i++) { uint32_t r = P(0x90fe4) + i * 0x1c; uint32_t o = U32(r); if (!o) continue; objref(o, b);
        fprintf(out, "AOB %s x=%d y=%d id=%04x rot=%02x f7=%02x lt=%02x f9=%02x z=%d c=%04x cmd=%04x\n", b, (int16_t)U16(o), (int16_t)U16(o + 2), U16(o + 4), U8(o + 6), U8(o + 7), U8(o + 8), U8(o + 9), (int16_t)U16(o + 10), U16(o + 12), U16(o + 14)); }
}
static void run(struct line *l);
static void ov_tick(struct roth_chain *c, const struct roth_api_v1 *api)
{
    g = api->game_ram;
    if (g->u8(0x7674a) == 1) {
        if (g_left <= 0) { g_left = (int)(int8_t)g->u32(0x85324); if (g_left < 1) g_left = 1; if (g_ticktrace) fprintf(out, "FRAME fts=%u\n", g->u32(0x85324)); }
        g_left--;
        g_tick++;
        while (g_pc < g_nscript && g_script[g_pc].frame >= 100000 && g_script[g_pc].frame - 100000 <= g_tick) { run(&g_script[g_pc]); g_pc++; }
        if (g_ticktrace) {
            char b[32];
            for (int i = 0; i < 16; i++) { uint32_t s = P(0x91e04) + i * 0x22; int occ = U32(s + 4) != 0;
                if (occ && !g_prevact[i]) { objref(U32(s + 4), b); fprintf(out, "WAKE %s slot=%d\n", b, i); } }
            fprintf(out, "TICK %d\n", g_tick); dump_env(); dump_globals(); dump_pools(); dump_pool_objects();
        }
    }
    roth_next_update_dynamic_entities(c);
    for (int i = 0; i < 16; i++) g_prevact[i] = U32(P(0x91e04) + i * 0x22 + 4) != 0;
    if (out) fflush(out);
}
static uint32_t g_mv_ent;
static void ov_move(struct roth_chain *c, const struct roth_api_v1 *api, int32_t dx, int32_t dy, uint32_t ebx, uint32_t ecx)
{
    (void)api; g_inmove = 1; g_mv_ent = ebx;
    roth_next_move_entity_with_collision(c, dx, dy, ebx, ecx);
    g_inmove = 0;
}
static int ov_collide(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t q, void *frame)
{
    g = api->game_ram;
    if (!g_inmove || !g_ticktrace) return roth_next_find_sector_and_collide(c, q, frame);
    char b[32]; objref(U32(g_mv_ent + 4), b);
    uint8_t *fp = (uint8_t *)frame;
    fprintf(out, "COL self=%s from=%08x,%08x,%08x to=%08x,%08x sec=%04x step=%04x hgt=%04x rad=%04x bud=%08x", b,
        g->u32(0x90ab4), g->u32(0x90ab8), g->u32(0x8c124), g->u32(0x8c120), g->u32(0x8c128), g->u16(0x90c12), g->u16(0x90bde), g->u16(0x8c0e8), (uint16_t)g->u32(0x8c130), *(uint32_t *)(fp + 0x24));
    int cf = roth_next_find_sector_and_collide(c, q, frame);
    fprintf(out, " -> loc=%d x=%08x y=%08x z=%08x back=%08x,%08x hit=%02x fz=%04x cz=%04x sec=%04x\n", !cf, g->u32(0x8c120), g->u32(0x8c128), g->u32(0x8c124),
        g->u32(0x90ab4), g->u32(0x90ab8), g->u8(0x8c1e0), *(uint16_t *)fp, *(uint16_t *)(fp + 2), g->u16(0x90c12));
    return cf;
}
static void ov_sweep(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t vx, uint32_t dest, uint32_t vy, uint32_t mag, uint32_t vz, uint32_t ent)
{
    g = api->game_ram;
    if (!g_ticktrace) { roth_next_sweep_move_with_collision(c, vx, dest, vy, mag, vz, ent); return; }
    char b[32], hb[32]; uint32_t o = U32(ent); objref(o, b);
    fprintf(out, "SWP self=%s x=%08x y=%08x z=%08x sec=%04x vx=%d vy=%d vz=%d spd=%u w=%u own=%04x", b,
        ((uint32_t)U16(o) << 16) | U16(ent + 10), ((uint32_t)U16(o + 2) << 16) | U16(ent + 12), ((uint32_t)U16(o + 10) << 16) | U16(ent + 14), U16(ent + 6), (int32_t)vx, (int32_t)vy, (int32_t)vz, mag, g->u32(0x90fdc), U16(ent + 0x1a));
    roth_next_sweep_move_with_collision(c, vx, dest, vy, mag, vz, ent);
    uint32_t ho = U32(dest + 0x1c); objref(ho, hb);
    fprintf(out, " -> x=%08x y=%08x z=%08x sec=%04x hit=%02x ho=%s\n", U32(dest), U32(dest + 4), U32(dest + 8), U16(dest + 0x18), U8(dest + 0x1a), hb);
}
static uint32_t ov_sound(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t id, uint32_t param, uint32_t bx, uint32_t cx)
{
    (void)api; if (g_ticktrace) fprintf(out, "SND %d at %d,%d\n", (int)id, (int16_t)bx, (int16_t)cx);
    return roth_next_play_entity_sound(c, id, param, bx, cx);
}
static int32_t ov_chain(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t src)
{
    (void)api; if (g_ticktrace) fprintf(out, "CHAIN\n");
    return roth_next_begin_object_command_chain(c, src);
}
static uint32_t ov_anim(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t rec, uint32_t surf)
{
    g = api->game_ram;
    if (!g_ticktrace || rec == 0 || !(U8(rec + 9) & 3)) return roth_next_select_surface_anim_frame(c, rec, surf);
    char b[32]; objref(rec, b);
    uint32_t idx = U16(rec + 0xc), slot = 0;
    if (idx) slot = (U8(rec + 9) & 1) ? P(0x91e03) + idx : P(0x90fe3) + idx;
    uint16_t before = slot ? U16(slot + 0x1a) : 0, last = U16(surf + 0x18);
    uint32_t r = roth_next_select_surface_anim_frame(c, rec, surf);
    fprintf(out, "ANIM obj=%s key=%x tpf=%u cnt=%u last=%u magic=%u ctr=%u -> %u\n", b, surf, U16(surf + 0x14), U16(surf + 0x22), last, U16(surf + 0x1a), before, slot ? U16(slot + 0x1a) : 0);
    return r;
}
static void on_overrides(const struct roth_api_v1 *api, struct roth_registrar_v1 *reg)
{
    (void)api;
    roth_override(reg, ROTH_FN_update_dynamic_entities, ov_tick, 0);
    roth_override(reg, ROTH_FN_move_entity_with_collision, ov_move, 0);
    roth_override(reg, ROTH_FN_find_sector_and_collide, ov_collide, 0);
    roth_override(reg, ROTH_FN_sweep_move_with_collision, ov_sweep, 0);
    roth_override(reg, ROTH_FN_play_entity_sound, ov_sound, 0);
    roth_override(reg, ROTH_FN_begin_object_command_chain, ov_chain, 0);
    roth_override(reg, ROTH_FN_select_surface_anim_frame, ov_anim, 0);
}
static void frame(const struct roth_api_v1 *api)
{
    g = api->game_ram;
    if (g->u8(0x7674a) != 1) return;
    g_frames++;
    while (g_pc < g_nscript && g_script[g_pc].frame < 100000 && g_script[g_pc].frame <= g_frames) { run(&g_script[g_pc]); g_pc++; fflush(out); }
    if (g_trace) { dump_env(); dump_globals(); dump_pools(); }
    fflush(out);
}
static void on_load(const struct roth_api_v1 *api)
{
    (void)api; const char *s;
    out = fopen((s = getenv("ACTP_OUT")) ? s : "/tmp/actp.log", "w");
    if ((s = getenv("ACTP_SYMS"))) { FILE *f = fopen(s, "r"); if (f) { g_syms = calloc(1, 1 << 20); fread(g_syms, 1, (1 << 20) - 1, f); fclose(f); } }
    if ((s = getenv("ACTP_SCRIPT"))) { FILE *f = fopen(s, "r"); char ln[256];
        while (f && fgets(ln, sizeof ln, f) && g_nscript < 4096) { if (ln[0] == '#' || ln[0] == '\n') continue;
            struct line *l = &g_script[g_nscript]; l->args[0] = 0;
            if (sscanf(ln, "%d %15s %199[^\n]", &l->frame, l->op, l->args) >= 2) g_nscript++; }
        if (f) fclose(f); }
    fprintf(stderr, "[actprobe] %d script lines\n", g_nscript);
}
static const struct roth_plugin_info_v1 INFO = {
    .abi_major = ROTH_ABI_MAJOR, .abi_minor = ROTH_ABI_MINOR, .struct_size = sizeof(struct roth_plugin_info_v1),
    .id = "io.github.vrealms.actprobe", .name = "actor probe", .version = "0.1",
    .sdk_req_major = ROTH_SDK_MAJOR, .sdk_req_minor = ROTH_SDK_MINOR, .api_use = ROTH_API_USE_ENGINE,
    .on_load = on_load, .on_frame_game = frame, .on_register_overrides = on_overrides,
};
ROTH_PLUGIN_EXPORT const struct roth_plugin_info_v1 *roth_plugin_query_v1(void) { return &INFO; }
