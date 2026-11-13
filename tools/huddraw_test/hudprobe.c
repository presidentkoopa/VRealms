/* hudprobe.c -- probe mod for the ROTH.C oracle build (/home/claude/oracle_build): see README.md.
 * At gameplay frame HUDPROBE_AT it runs the script HUDPROBE_SCRIPT against the ORIGINAL's own HUD
 * routines and the game logic that drives them (refresh_hud_layout, render_weapon_hud,
 * draw_active_ui_panels, draw_held_item_icon, draw_character_portrait_corner, restore/clear of the
 * corner peek, activate_weapon_item, trigger_weapon_fire, tick_weapon_hud_ammo_anim,
 * apply_direct_damage_to_player, give_item, remove_item, update_selected_item_icon; addresses from
 * HUDPROBE_SYMS = `nm roth`). On "dump" it writes the whole framebuffer (the indexed bytes) to
 * HUDPROBE_DIR/<name>.scr. Every HUD routine the original's own logic reaches while the script runs
 * (render_weapon_view, redraw_weapon_hud_panel, render_weapon_hud, update_selected_item_icon,
 * render_player_health_bar) is logged as an "HC" line with the state it reads; the port must reach
 * the same ones with the same state (tools/huddraw_test/huddraw_diff.cpp).
 *
 * The "world" under the HUD is a known pattern ("world <seed>": xorshift32 indices over the whole
 * screen), written where the original renders its 3D view before the HUD is drawn over it
 * (run_gameplay_frame -> render_world_view, then gameplay_frame_step next loop, game_core.c:486-506).
 * The port draws over the same pattern, so the blended pixels (the corner's shadow, the held item's
 * translucent icon) are compared too.
 *
 * Every write to the log or a dump file holds SIGALRM blocked: the host's tick can land inside
 * musl's stdio, which is not re-entrant (tools/actors_test, the weapons lane's finding). */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "roth_sdk.h"

static const struct roth_api_v1 *A;
static FILE *OUT;
static int g_at = 200, g_frames, g_done;
static char g_script[512], g_syms[512], g_dir[512];

#define G8(a)  (A->game_ram->u8(a))
#define G16(a) (A->game_ram->u16(a))
#define G32(a) (A->game_ram->u32(a))
#define S8(a,v)  (A->game_ram->set_u8((a),(uint8_t)(v)))
#define S16(a,v) (A->game_ram->set_u16((a),(uint16_t)(v)))
#define S32(a,v) (A->game_ram->set_u32((a),(uint32_t)(v)))
#define HP(a)  ((uint32_t)(uintptr_t)A->game_ram->to_ptr(a))
#define RB(p)  (*(volatile uint8_t  *)(uintptr_t)(p))

static sigset_t g_logold; static int g_logdepth;
static void log_on(void) { if (g_logdepth++ == 0) { sigset_t s; sigemptyset(&s); sigaddset(&s, SIGALRM); sigprocmask(SIG_BLOCK, &s, &g_logold); } }
static void log_off(void) { if (--g_logdepth == 0) sigprocmask(SIG_SETMASK, &g_logold, 0); }

/* ---- symbols ---- */
struct sym { char name[64]; uint32_t addr; };
static struct sym *g_sym; static int g_nsym;
static void load_syms(void)
{
    FILE *f = fopen(g_syms, "r"); if (!f) return;
    char line[512]; g_sym = malloc(sizeof(struct sym) * 14000);
    while (fgets(line, sizeof line, f) && g_nsym < 14000) {
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
    fprintf(stderr, "[hudprobe] NO SYMBOL %s\n", n); return 0;
}
typedef uint32_t (*f0)(void);
typedef uint32_t (*f1)(uint32_t);
typedef uint32_t (*f2)(uint32_t, uint32_t);
typedef uint32_t (*f4)(uint32_t, uint32_t, uint32_t, uint32_t);
#define C0(n)          (((f0)(uintptr_t)fn(n))())
#define C1(n,a)        (((f1)(uintptr_t)fn(n))((uint32_t)(a)))
#define C2(n,a,b)      (((f2)(uintptr_t)fn(n))((uint32_t)(a),(uint32_t)(b)))
#define C4(n,a,b,c,d)  (((f4)(uintptr_t)fn(n))((uint32_t)(a),(uint32_t)(b),(uint32_t)(c),(uint32_t)(d)))

/* ---- the state every HUD routine reads, one line ---- */
static int slotidx(uint32_t p) { return p ? (int)((p - HP(0x80c30)) / 4) : -1; }
static void hudstate(void)
{
    /* 0x83e70 item, 0x83e6c ammo, 0x83e74 cap, 0x811b4+0x48 the panel number, 0x83d7c the cell counter,
     * 0x83e80 / 0x83e7c the two redraw flags, 0x8a0f0 / 0x7fe44 health / max, 0x81038 / 0x81044 the hands,
     * 0x7fdc0 the corner peek, 0x819bc the held icon's slide, 0x8130c its cached id */
    fprintf(OUT, " item=%d ammo=%d cap=%d panel=%d cell=%d f80=%d f7c=%d hp=%d max=%d L=%d R=%d peek=%d slide=%d icon=%d\n",
        (int)G32(0x83e70), (int)G32(0x83e6c), (int)G32(0x83e74), (int)G32(0x811b4 + 0x48), (int)G32(0x83d7c),
        (int)G32(0x83e80), (int)G32(0x83e7c), (int)G32(0x8a0f0), (int)G32(0x7fe44), slotidx(G32(0x81038)),
        slotidx(G32(0x81044)), (int)G32(0x7fdc0), (int)G32(0x819bc), (int)G32(0x8130c));
}
static void hc(const char *name, int arg)
{
    if (g_done != 1 || !OUT) return;
    log_on(); fprintf(OUT, "HC %s %d", name, arg); hudstate(); fflush(OUT); log_off();
}

/* ---- overrides: log the HUD routines the original's own logic reaches, keep the script quiet ---- */
static uint32_t ov_rwv(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t eax)
{ (void)api; hc("render_weapon_view", (int)eax); return roth_next_render_weapon_view(c, eax); }
static void ov_rwhp(struct roth_chain *c, const struct roth_api_v1 *api)
{ (void)api; hc("redraw_weapon_hud_panel", 0); roth_next_redraw_weapon_hud_panel(c); }
static void ov_rwh(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t eax, uint32_t edx)
{ (void)api; hc("render_weapon_hud", (int)eax); roth_next_render_weapon_hud(c, eax, edx); }
static void ov_usii(struct roth_chain *c, const struct roth_api_v1 *api)
{ (void)api; hc("update_selected_item_icon", 0); roth_next_update_selected_item_icon(c); }
static void ov_rphb(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t base, uint32_t desc)
{ (void)api; hc("render_player_health_bar", 0); roth_next_render_player_health_bar(c, base, desc); }
static void ov_noammo(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t eax)
{ (void)api; if (g_done == 1 && OUT) { log_on(); fprintf(OUT, "NOAMMO %d\n", (int)eax); fflush(OUT); log_off(); return; } roth_next_show_no_ammo_message(c, eax); }
static uint32_t ov_sfx(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t id, uint32_t p)
{ (void)api; if (g_done == 1) return 0; return roth_next_play_sound_effect(c, id, p); }
static int32_t ov_runobj(struct roth_chain *c, const struct roth_api_v1 *api, uint16_t ax)
{ (void)api; if (g_done == 1) return 0; return roth_next_run_object_commands_by_id(c, ax); }
static uint32_t ov_d300(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t idx)
{ (void)api; if (g_done == 1) return 1; return roth_next_load_dbase300_chunk(c, idx); }
static uint32_t ov_prime(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t clip)
{ (void)api; if (g_done == 1) return 0; return roth_next_prime_voice_clip(c, clip); }
static void reg(const struct roth_api_v1 *api, struct roth_registrar_v1 *r)
{
    (void)api;
    roth_override(r, ROTH_FN_render_weapon_view, ov_rwv, 100);
    roth_override(r, ROTH_FN_redraw_weapon_hud_panel, ov_rwhp, 100);
    roth_override(r, ROTH_FN_render_weapon_hud, ov_rwh, 100);
    roth_override(r, ROTH_FN_update_selected_item_icon, ov_usii, 100);
    roth_override(r, ROTH_FN_render_player_health_bar, ov_rphb, 100);
    roth_override(r, ROTH_FN_show_no_ammo_message, ov_noammo, 100);   /* logged, the message itself not shown */
    roth_override(r, ROTH_FN_play_sound_effect, ov_sfx, 100);
    roth_override(r, ROTH_FN_run_object_commands_by_id, ov_runobj, 100);
    roth_override(r, ROTH_FN_load_dbase300_chunk, ov_d300, 100);
    roth_override(r, ROTH_FN_prime_voice_clip, ov_prime, 100);
}

static uint32_t fnv(uint32_t p, uint32_t n) { uint32_t h = 2166136261u; for (uint32_t i = 0; i < n; i++) h = (h ^ RB(p + i)) * 16777619u; return h; }

/* The whole screen: (pitch x height) indexed bytes; "HSCR" pitch height hires. */
static void dump(const char *name)
{
    int pitch = (int)G32(0x85498), h = (int)G32(0x8549c), hires = G8(0x90cbd) != 0;
    int rows = hires ? h * 2 : h;
    uint32_t fb = G32(0x90a98);
    char path[700]; snprintf(path, sizeof path, "%s/%s.scr", g_dir, name);
    log_on();
    FILE *f = fopen(path, "wb");
    if (f) {
        uint32_t hdr[4] = { 0x52435348u /* "HSCR" */, (uint32_t)pitch, (uint32_t)rows, (uint32_t)hires };
        fwrite(hdr, 4, 4, f);
        fwrite((const void *)(uintptr_t)fb, 1, (size_t)pitch * rows, f);
        fclose(f);
    }
    fprintf(OUT, "D %s %08x\n", name, fnv(fb, (uint32_t)(pitch * rows)));
    fprintf(OUT, " state"); hudstate();
    fflush(OUT);
    log_off();
}

static void env(void)
{
    log_on();
    fprintf(OUT, "ENV pitch=%d h=%d hires=%u fb=%x view=%d,%d,%d,%d res=%d\n", (int)G32(0x85498), (int)G32(0x8549c), G8(0x90cbd),
        G32(0x90a98) - HP(0), (int)G32(0x85ce0), (int)G32(0x85ce4), (int)G32(0x85cd8), (int)G32(0x85cdc), (int)G32(0x7f358));
    fprintf(OUT, "TABLES blend=%08x ramp=%08x\n", fnv(G32(0x85d08), 0x10000), fnv(G32(0x86d14), 0x4000));
    fflush(OUT);
    log_off();
}

/* bytes of the executable's initialised data, for the port's transcribed tables */
static void exebytes(uint32_t va, uint32_t n)
{
    log_on();
    fprintf(OUT, "X %x %u:", va, n);
    for (uint32_t i = 0; i < n; i++) fprintf(OUT, " %02x", G8(va + i));
    fprintf(OUT, "\n"); fflush(OUT);
    log_off();
}

/* The weapon panels' descriptor table (0x718b8: 16 pointers, render_weapon_hud WC:1052): each
 * pointer as an executable address, then the 0x54 bytes it points at. */
static void wtable(void)
{
    for (int i = 0; i < 16; i++) {
        uint32_t p = G32(0x718b8 + i * 4);
        uint32_t va = p ? p - HP(0) : 0;
        log_on(); fprintf(OUT, "WT %d %x\n", i, va); fflush(OUT); log_off();
        if (va) exebytes(va, 0x54);
    }
}

/* The inventory and the hands, in the port's terms (slot indices). */
static void invstate(void)
{
    log_on();
    fprintf(OUT, "INV n=%d:", (int)G32(0x80c2c));
    for (int i = 0; i < 256; i++) { uint16_t id = G16(0x80c30 + i * 4), q = G16(0x80c32 + i * 4); if (id) fprintf(OUT, " %d:%u.%u", i, id, q); }
    fprintf(OUT, " adam=%d/%d\n", slotidx(G32(0x8104c)), (int)G32(0x81054));
    fflush(OUT);
    log_off();
}

static uint32_t g_rng = 1;
static uint32_t xs(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static void world(uint32_t seed)
{
    int pitch = (int)G32(0x85498), h = (int)G32(0x8549c), hires = G8(0x90cbd) != 0;
    int rows = hires ? h * 2 : h;
    uint8_t *fb = (uint8_t *)(uintptr_t)G32(0x90a98);
    g_rng = seed ? seed : 1;
    for (int i = 0; i < pitch * rows; i++) fb[i] = (uint8_t)(xs() >> 24);
}

static uint32_t slotptr(int id)
{
    for (int i = 0; i < 256; i++) if (G16(0x80c30 + i * 4) == (uint16_t)id) return HP(0x80c30 + i * 4);
    return 0;
}

static void run_script(void)
{
    FILE *f = fopen(g_script, "r");
    if (!f) { fprintf(stderr, "[hudprobe] no script %s\n", g_script); return; }
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char cmd[64]; int a[8] = {0};
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        if (sscanf(line, "%63s %i %i %i %i %i %i %i %i", cmd, &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6], &a[7]) < 1) continue;
        log_on(); fprintf(OUT, "> %s\n", line); fflush(OUT); log_off();
        if (!strcmp(cmd, "env")) env();
        else if (!strcmp(cmd, "exe")) exebytes((uint32_t)a[0], (uint32_t)a[1]);
        else if (!strcmp(cmd, "font")) exebytes(0x70f12 - 0x4d6, 0x4d6 + 0x206);
        else if (!strcmp(cmd, "wtable")) wtable();
        else if (!strcmp(cmd, "inv")) invstate();
        else if (!strcmp(cmd, "world")) world((uint32_t)a[0]);
        else if (!strcmp(cmd, "fts")) S32(0x85324, a[0]);                      /* g_frame_time_scale */
        else if (!strcmp(cmd, "give")) { C2("give_item", a[0], 0); invstate(); }
        else if (!strcmp(cmd, "remove")) { C1("remove_item", a[0]); invstate(); }
        else if (!strcmp(cmd, "health")) S32(0x8a0f0, a[0]);                   /* a starting state, no routine */
        else if (!strcmp(cmd, "maxhealth")) S32(0x7fe44, a[0]);
        else if (!strcmp(cmd, "damage")) C1("apply_direct_damage_to_player", a[0]);
        else if (!strcmp(cmd, "layout")) C0("refresh_hud_layout");          /* repaint_hud_and_present's rebuild (menu_hud_ui.c:478) */
        else if (!strcmp(cmd, "equip")) {      /* commit_held_cursor_item's equip (inventory.c:2185-2195): activate_weapon_item(slot, id) */
            uint32_t s = slotptr(a[0]); if (s) C2("activate_weapon_item", s, a[0]); }
        else if (!strcmp(cmd, "unequip")) C2("activate_weapon_item", 0, 0);
        else if (!strcmp(cmd, "equipfirst")) C0("equip_first_usable_weapon");
        else if (!strcmp(cmd, "weaponhud")) {  /* the game loop's (game_core.c:875): after an equip, render_weapon_hud(1, &attrs) */
            if (G32(0x810c4) != 0) { S32(0x810c4, 0); C2("render_weapon_hud", 1, HP(0x811b4)); } }
        else if (!strcmp(cmd, "select")) {     /* commit_held_cursor_item's select (inventory.c:2197-2198) */
            S32(0x81044, slotptr(a[0])); C0("update_selected_item_icon"); }
        else if (!strcmp(cmd, "deselect")) { S32(0x81044, 0); C0("update_selected_item_icon"); }
        else if (!strcmp(cmd, "fire")) {       /* key_fire_weapon (WC:912-916); the fire lock released first, as the end of a cycle does */
            S32(0x7fdd0, 0); S32(0x7f568, 0); C4("trigger_weapon_fire", 0, 0, 0, 0); }
        else if (!strcmp(cmd, "recharge")) {   /* tick_weapon_hud_ammo_anim with g_frame_time_scale = a[0] (draw_active_ui_panels' first call) */
            S32(0x85324, a[0]); C0("tick_weapon_hud_ammo_anim"); }
        else if (!strcmp(cmd, "mouse")) { S32(0x707b3, a[0]); S32(0x707b7, a[1]); }
        else if (!strcmp(cmd, "peek")) {       /* run_gameplay_frame's corner timer (game_core.c:520-533) with g_frame_time_scale = a[0] */
            S32(0x85324, a[0]);
            if ((int32_t)G32(0x707b3) < 0x20 && (int32_t)G32(0x707b7) < 0x20) {
                int32_t t = (int32_t)G32(0x7fdc0) + a[0]; if (t > 0x19) t = 0x19; S32(0x7fdc0, t);
            } else if (G32(0x7fdc0) != 0) {
                int32_t t = (int32_t)G32(0x7fdc0) - a[0]; S32(0x7fdc0, t);
                if (t <= 0) C0("clear_corner_peek_icon");
            } }
        else if (!strcmp(cmd, "hud")) {        /* gameplay_frame_step's drawing (game_core.c:494-500), g_frame_time_scale = a[0] */
            S32(0x85324, a[0]);
            C0("draw_held_item_icon");
            C0("draw_active_ui_panels");
            if (G32(0x7fdc0) != 0) C1("draw_character_portrait_corner", G32(0x7fdc0)); }
        else if (!strcmp(cmd, "unpeek")) {     /* after the flip (game_core.c:501) */
            if (G32(0x7fdc0) != 0) C0("restore_corner_peek_icon"); }
        else if (!strcmp(cmd, "dump")) { char *n = strchr(line, ' '); dump(n ? n + 1 : "unnamed"); }
        else { log_on(); fprintf(OUT, "? unknown command\n"); fflush(OUT); log_off(); }
    }
    fclose(f);
}

static void frame(const struct roth_api_v1 *api)
{
    A = api;
    if (api->game_ram->u8(0x7674a) != 1) return;
    g_frames++;
    if (g_done || g_frames < g_at) return;
    const char *o = getenv("HUDPROBE_OUT");
    log_on();
    OUT = fopen(o ? o : "/tmp/hudprobe.log", "w");
    log_off();
    if (!OUT) { g_done = 2; return; }
    g_done = 1;
    load_syms();
    log_on(); fprintf(OUT, "# hudprobe at gameplay frame %d\n", g_frames); fflush(OUT); log_off();
    invstate();
    log_on(); fprintf(OUT, "START"); hudstate(); fflush(OUT); log_off();
    run_script();
    log_on(); fclose(OUT); OUT = 0; log_off();
    g_done = 2;
    fprintf(stderr, "[hudprobe] script done\n");
    _Exit(0);
}
static void on_load(const struct roth_api_v1 *api)
{
    (void)api; const char *s;
    if ((s = getenv("HUDPROBE_AT"))) g_at = atoi(s);
    if ((s = getenv("HUDPROBE_SCRIPT"))) strncpy(g_script, s, sizeof g_script - 1);
    if ((s = getenv("HUDPROBE_SYMS"))) strncpy(g_syms, s, sizeof g_syms - 1);
    if ((s = getenv("HUDPROBE_DIR"))) strncpy(g_dir, s, sizeof g_dir - 1); else strcpy(g_dir, "/tmp");
}
static const struct roth_plugin_info_v1 INFO = {
    .abi_major = ROTH_ABI_MAJOR, .abi_minor = ROTH_ABI_MINOR, .struct_size = sizeof(struct roth_plugin_info_v1),
    .id = "io.github.vrealms.hudprobe", .name = "HUD probe", .version = "0.1",
    .sdk_req_major = ROTH_SDK_MAJOR, .sdk_req_minor = ROTH_SDK_MINOR, .api_use = ROTH_API_USE_ENGINE | ROTH_API_USE_GAME_RAM,
    .on_load = on_load, .on_frame_game = frame, .on_register_overrides = reg,
};
ROTH_PLUGIN_EXPORT const struct roth_plugin_info_v1 *roth_plugin_query_v1(void) { return &INFO; }
