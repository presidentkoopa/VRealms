/* invprobe.c -- probe mod for the ROTH.C oracle build (/home/claude/oracle_build): see README.md.
 * At gameplay frame INVPROBE_AT it runs the script INVPROBE_SCRIPT against the ORIGINAL's own
 * inventory routines (render_inventory_panel, dispatch_dialogue_ui_action, update_inventory_screen
 * with a key fed through input_ring_dequeue, redraw_inventory_cursor_cell, close_inventory_panel,
 * give_item, ...; addresses from INVPROBE_SYMS = `nm roth`) and, on "dump", writes the panel
 * rectangle of the original's framebuffer -- the indexed bytes -- to INVPROBE_DIR/<name>.pan,
 * plus the hotspot lists and the drawing state to the log INVPROBE_OUT.
 * tools/invdraw_test/invdraw_diff.cpp runs the same script through the port (roth_invscreen +
 * roth_invdraw) and compares the bytes.
 *
 * Every write to the log or a dump file holds SIGALRM blocked: the host's tick (vsync_timer_tick ->
 * the software cursor's int 0x33 poll -> frame_pump) can land inside musl's stdio, which is not
 * re-entrant (tools/actors_test/oracle/README.txt, the weapons lane's finding). */
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
static int g_key;                 /* the scancode input_ring_dequeue hands out next; 0 = none */

#define G8(a)  (A->game_ram->u8(a))
#define G16(a) (A->game_ram->u16(a))
#define G32(a) (A->game_ram->u32(a))
#define S8(a,v)  (A->game_ram->set_u8((a),(uint8_t)(v)))
#define S16(a,v) (A->game_ram->set_u16((a),(uint16_t)(v)))
#define S32(a,v) (A->game_ram->set_u32((a),(uint32_t)(v)))
#define HP(a)  ((uint32_t)(uintptr_t)A->game_ram->to_ptr(a))
#define RB(p)  (*(volatile uint8_t  *)(uintptr_t)(p))
#define RW(p)  (*(volatile uint16_t *)(uintptr_t)(p))
#define RD(p)  (*(volatile uint32_t *)(uintptr_t)(p))

static sigset_t g_logold; static int g_logdepth;
static void log_on(void) { if (g_logdepth++ == 0) { sigset_t s; sigemptyset(&s); sigaddset(&s, SIGALRM); sigprocmask(SIG_BLOCK, &s, &g_logold); } }
static void log_off(void) { if (--g_logdepth == 0) sigprocmask(SIG_SETMASK, &g_logold, 0); }

/* ---- symbols ---- */
struct sym { char name[64]; uint32_t addr; };
static struct sym *g_sym; static int g_nsym;
static void load_syms(void)
{
    FILE *f = fopen(g_syms, "r"); if (!f) return;
    char line[512]; g_sym = malloc(sizeof(struct sym) * 12000);
    while (fgets(line, sizeof line, f) && g_nsym < 12000) {
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
    fprintf(stderr, "[invprobe] NO SYMBOL %s\n", n); return 0;
}
typedef uint32_t (*f0)(void);
typedef uint32_t (*f1)(uint32_t);
typedef uint32_t (*f2)(uint32_t, uint32_t);
typedef uint32_t (*f4)(uint32_t, uint32_t, uint32_t, uint32_t);
#define C0(n)          (((f0)(uintptr_t)fn(n))())
#define C1(n,a)        (((f1)(uintptr_t)fn(n))((uint32_t)(a)))
#define C2(n,a,b)      (((f2)(uintptr_t)fn(n))((uint32_t)(a),(uint32_t)(b)))
#define C4(n,a,b,c,d)  (((f4)(uintptr_t)fn(n))((uint32_t)(a),(uint32_t)(b),(uint32_t)(c),(uint32_t)(d)))

/* ---- overrides: the keyboard ring, and the calls that would leave the screen ---- */
static uint8_t ov_dequeue(struct roth_chain *c, const struct roth_api_v1 *api)
{ (void)api; if (g_done == 1) { uint8_t k = (uint8_t)g_key; g_key = 0; return k; } return roth_next_input_ring_dequeue(c); }
static uint32_t ov_sfx(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t id, uint32_t p)
{ (void)api; if (g_done == 1) return 0; return roth_next_play_sound_effect(c, id, p); }
static int32_t ov_runobj(struct roth_chain *c, const struct roth_api_v1 *api, uint16_t ax)
{ (void)api; if (g_done == 1) return 0; return roth_next_run_object_commands_by_id(c, ax); }
static uint32_t ov_d300(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t idx)
{ (void)api; if (g_done == 1) return 1; return roth_next_load_dbase300_chunk(c, idx); }
static uint32_t ov_prime(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t clip)
{ (void)api; if (g_done == 1) return 0; return roth_next_prime_voice_clip(c, clip); }
/* set_cursor_shape while the probe's mouse runs: the shape is recorded and nothing else is done (no
 * cursor decode, no software cursor over the framebuffer the dumps read). */
static int g_shape_capture; static uint32_t g_shape;
static void ov_shape(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t id)
{ (void)api; if (g_shape_capture) { g_shape = id & 0xffff; S16(0x708e4, id); return; } roth_next_set_cursor_shape(c, id); }
static void reg(const struct roth_api_v1 *api, struct roth_registrar_v1 *r)
{
    roth_override(r, ROTH_FN_set_cursor_shape, ov_shape, 100);
    (void)api;
    roth_override(r, ROTH_FN_input_ring_dequeue, ov_dequeue, 100);
    roth_override(r, ROTH_FN_play_sound_effect, ov_sfx, 100);
    roth_override(r, ROTH_FN_run_object_commands_by_id, ov_runobj, 100);
    roth_override(r, ROTH_FN_load_dbase300_chunk, ov_d300, 100);
    roth_override(r, ROTH_FN_prime_voice_clip, ov_prime, 100);
}

static uint32_t fnv(uint32_t p, uint32_t n) { uint32_t h = 2166136261u; for (uint32_t i = 0; i < n; i++) h = (h ^ RB(p + i)) * 16777619u; return h; }
static int entidx(uint32_t p) { return p ? (int)((p - HP(0x7fef4)) / 12) : -1; }
static int slotidx(uint32_t p) { return p ? (int)((p - HP(0x80c30)) / 4) : -1; }

static void hotspots(uint32_t list)
{
    for (int i = 0; i < 40; i++) {
        uint32_t s = list + i * 8;
        if (G16(s) == 0) { fprintf(OUT, " end"); break; }
        fprintf(OUT, " [%d,%d,%u,%u,%d]", (int16_t)G16(s), (int16_t)G16(s + 2), G8(s + 4), G8(s + 5), (int16_t)G16(s + 6));
    }
    fprintf(OUT, "\n");
}

/* The panel rectangle, (x, y) logical: x in [ax, ax+0x120), y in [ay, ay+0x50); in the hi-res
 * mode every logical row is two framebuffer rows (screen_xy_to_framebuffer_ptr, renderer.c:579). */
static void dump(const char *name)
{
    int ax = (int)G32(0x80b24), ay = (int)G32(0x80b28), hires = G8(0x90cbd) != 0, pitch = (int)G32(0x85498);
    int rows = hires ? 0xa0 : 0x50, y0 = hires ? ay * 2 : ay;
    uint32_t fb = G32(0x90a98);
    char path[700]; snprintf(path, sizeof path, "%s/%s.pan", g_dir, name);
    log_on();
    FILE *f = fopen(path, "wb");
    if (f) {
        uint32_t hdr[4] = { 0x4e415049u /* "IPAN" */, 0x120u, (uint32_t)rows, (uint32_t)hires };
        fwrite(hdr, 4, 4, f);
        for (int r = 0; r < rows; r++) fwrite((const void *)(uintptr_t)(fb + (uint32_t)((y0 + r) * pitch + ax)), 1, 0x120, f);
        fclose(f);
    }
    uint32_t h = 2166136261u;
    for (int r = 0; r < rows; r++) for (int x = 0; x < 0x120; x++) h = (h ^ RB(fb + (uint32_t)((y0 + r) * pitch + ax + x))) * 16777619u;
    fprintf(OUT, "D %s %08x\n", name, h);
    fprintf(OUT, " state list=%d tab=%d pos=%d scroll=%d count=%d total=%d held=%d lh=%d rh=%d dl=%d dr=%d adam=%d/%d dirty=%u arrows=%x lab=%x/%d labx=%d\n",
        (int)G32(0x80b38), (int)G32(0x80b70), (int)G32(0x80afc + G32(0x80b38) * 4), (int)G32(0x80b10 + G32(0x80b38) * 4),
        (int)G32(0x80af4), (int)G32(0x80af8), entidx(G32(0x7fef0)), entidx(G32(0x81030)), entidx(G32(0x8103c)),
        entidx(G32(0x81034)), entidx(G32(0x81040)), slotidx(G32(0x8104c)), (int)G32(0x81054), G8(0x7f571),
        G32(0x811a0), G32(0x811a4), (int)G32(0x811a8), (int)G32(0x81058));
    fprintf(OUT, " hands L=%d R=%d entries:", slotidx(G32(0x81038)), slotidx(G32(0x81044)));
    for (int i = 0; i < (int)G32(0x80af8) && i < 256; i++) fprintf(OUT, " %d/%d", slotidx(G32(0x7fef4 + i * 12 + 4)), (int)G32(0x7fef4 + i * 12 + 8));
    fprintf(OUT, "\n label=\"");
    for (int i = 0; i < 0x62 && G8(0x8105e + i); i++) fputc(G8(0x8105e + i), OUT);
    fprintf(OUT, "\"\n hot0"); hotspots(0x80b74);
    fprintf(OUT, " hot1"); hotspots(0x80ba4);
    fflush(OUT);
    log_off();
}

static void env(void)
{
    log_on();
    fprintf(OUT, "ENV pitch=%d h=%d hires=%u fb=%x ax=%d ay=%d view=%d,%d,%d,%d\n", (int)G32(0x85498), (int)G32(0x8549c), G8(0x90cbd),
        G32(0x90a98) - HP(0) , (int)G32(0x80b24), (int)G32(0x80b28), (int)G32(0x85ce0), (int)G32(0x85ce4), (int)G32(0x85cd8), (int)G32(0x85cdc));
    fprintf(OUT, "TABLES blend=%08x ramp=%08x\n", fnv(G32(0x85d08), 0x10000), fnv(G32(0x86d14), 0x4000));
    /* the hit test's list of hotspot lists (dialogue_ui.c:578): pointers at 0x71241 */
    fprintf(OUT, "HOTLISTS");
    for (int i = 0; i < 4; i++) { uint32_t p = G32(0x71241 + i * 4); fprintf(OUT, " %x", p ? p - HP(0) : 0); if (!p) break; }
    fprintf(OUT, "\n");
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

static void fill_panel(int v)
{
    int ax = (int)G32(0x80b24) , ay = (int)G32(0x80b28);
    int hires = G8(0x90cbd) != 0, pitch = (int)G32(0x85498);
    /* the anchor is computed by render_inventory_panel; use the same formula (renderer.c:9617) */
    ax = (int)((G32(0x85498) - 0x130) >> 1); ay = (int)(G32(0x8549c) >> 1);
    int rows = hires ? 0xa0 : 0x50, y0 = hires ? ay * 2 : ay;
    uint32_t fb = G32(0x90a98);
    for (int r = -8; r < rows + 8; r++) memset((void *)(uintptr_t)(fb + (uint32_t)((y0 + r) * pitch + ax - 4)), v, 0x128);
}

/* The state the port seeds itself from, in tools/dialogue_test's format: the DBASE100 flag bitmap,
 * the carried slots, the hands. */
static void initial(void)
{
    uint32_t bm = G32(0x81e28), bmsz = G32(0x81e2c);
    log_on();
    fprintf(OUT, "S initial\n flags=");
    for (uint32_t i = 0; bm && i < bmsz; i++) fprintf(OUT, "%02x", RB(bm + i));
    fprintf(OUT, "\n inv=%d:", (int)G32(0x80c2c));
    for (int i = 0; i < 256; i++) { uint16_t id = G16(0x80c30 + i * 4), q = G16(0x80c32 + i * 4); if (id) fprintf(OUT, " %d:%u.%u", i, id, q); }
    fprintf(OUT, "\n hands L=%d R=%d adam=%d/%d auto=%d last=%d lock=%x/%d\n", slotidx(G32(0x81038)), slotidx(G32(0x81044)),
        slotidx(G32(0x8104c)), (int)G32(0x81054), (int)G32(0x89f60), (int)G32(0x89fa8), G32(0x7fd84), (int)G32(0x7fda0));
    fprintf(OUT, "E initial-end\n");
    fflush(OUT);
    log_off();
}

/* The ORIGINAL's mouse at panel pixel (x, y): what handle_cursor_click (input.c:1260-1358) does with
 * the mouse there -- the hover (code 0), or the hover and then a click of the primary (1) or the
 * secondary (2) button, as a mouse that is moved onto the point and clicked makes it. The button's
 * press edge and its held bit are set as poll_mouse_input leaves them on the click's frame
 * (input.c:858-868). set_cursor_shape is overridden to record the shape and do nothing else (no
 * cursor decode, no software cursor drawn over the panel), with the frame tick's 0x10 bit clear while
 * it runs (put back after). Prints a P line: the shape, the hover latch, and the inventory state a click changes. */
static void mouse_call(int x, int y, int code)
{
    int ax = (int)G32(0x80b24), ay = (int)G32(0x80b28);
    S32(0x707b3, ax + x); S32(0x707b7, ay + y);           /* g_mouse_x / g_mouse_y */
    S8(0x7e938, code == 1 ? 0xff : 0); S8(0x7e939, code == 2 ? 0xff : 0);
    S8(0x7e929, code & 3);                                /* the buttons held */
    S32(0x80b30, 0);                                      /* no key-made click */
    uint16_t tk = G16(0x90bcc);
    S16(0x90bcc, 0);
    g_shape_capture = 1; g_shape = 0xffffffffu;
    C0("handle_cursor_click");
    g_shape_capture = 0;
    S16(0x90bcc, tk);
    S8(0x7e929, 0);
}
static void ptr(int x, int y, int code)
{
    if (code != 0) mouse_call(x, y, 0);                   /* the mouse comes onto the point */
    mouse_call(x, y, code);
    uint32_t v = G32(0x80b3c);
    char insp[32];
    /* in the port's terms (roth_invscreen.h): -1 none (the original's 0), an entry index, -2 / -3 the
     * left / right hand box's item */
    if (v == 0) strcpy(insp, "-1");
    else if (v == G32(0x81034)) strcpy(insp, "-2");
    else if (v == G32(0x81040)) strcpy(insp, "-3");
    else snprintf(insp, sizeof insp, "%d", entidx(v));
    int list = (int)G32(0x80b38);
    log_on();
    if (g_shape == 0xffffffffu) fprintf(OUT, "! set_cursor_shape was not reached through the override\n");
    fprintf(OUT, "P %d %d %d shape=%x latch=%d list=%d pos=%d scroll=%d count=%d held=%d L=%d R=%d insp=%s close=%d opt=%d busy=%d\n",
        x, y, code, g_shape & 0xffff, (int)G32(0x81300), list, (int)G32(0x80afc + list * 4), (int)G32(0x80b10 + list * 4),
        (int)G32(0x80af4), entidx(G32(0x7fef0)), slotidx(G32(0x81038)), slotidx(G32(0x81044)), insp, (int)G32(0x80b2c),
        (int)G32(0x7febc), (int)G32(0x83aea));
    fflush(OUT);
    log_off();
}

static void run_script(void)
{
    FILE *f = fopen(g_script, "r");
    if (!f) { fprintf(stderr, "[invprobe] no script %s\n", g_script); return; }
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
        else if (!strcmp(cmd, "fill")) fill_panel(a[0]);
        else if (!strcmp(cmd, "hires")) {  /* the line-doubled modes' drawing (video_display.c:790-795) on this 640x480 screen:
                                             * g_hires_line_doubling_flag set, the logical height halved */
            S8(0x90cbd, 1); S32(0x8549c, (int)G32(0x8549c) / 2); }
        else if (!strcmp(cmd, "give")) C2("give_item", a[0], 0);
        else if (!strcmp(cmd, "remove")) C1("remove_item", a[0]);
        else if (!strcmp(cmd, "adam")) C0("restore_active_held_item");
        else if (!strcmp(cmd, "open")) {   /* update_ui_overlay's open (menu_hud_ui.c:169-181) */
            C0("render_inventory_panel"); S32(0x7fec4, 2); }
        else if (!strcmp(cmd, "close")) C0("close_inventory_panel");
        else if (!strcmp(cmd, "frame")) {  /* one UI frame (game_core.c:886-894) at frame tick a[0] */
            S16(0x90bcc, a[0]); C0("redraw_inventory_cursor_cell"); g_key = 0; C0("update_inventory_screen"); }
        else if (!strcmp(cmd, "cursorcell")) { S16(0x90bcc, a[0]); C0("redraw_inventory_cursor_cell"); }
        else if (!strcmp(cmd, "key")) {    /* a key, then the rest of that frame */
            g_key = a[0]; C0("update_inventory_screen"); g_key = 0; }
        else if (!strcmp(cmd, "tabclick")) {   /* a click on tab a[0]'s hotspot: action 0x17 + its category */
            S8(0x7e938, 1); C4("dispatch_dialogue_ui_action", 0x17 + G8(0x7123c + a[0]), 1, 0, 0); }
        else if (!strcmp(cmd, "cell")) {   /* a click on grid cell a[0]: a[1]=1 primary, 2 secondary */
            S8(0x7e938, a[1] == 1); S8(0x7e939, a[1] == 2);
            C4("dispatch_dialogue_ui_action", 0x1c + a[0], a[1], 0, 0); }
        else if (!strcmp(cmd, "handbox")) {    /* a[0]=0 left (0x2a) / 1 right (0x2b); a[1]=1 primary, 2 secondary */
            S8(0x7e938, a[1] == 1); S8(0x7e939, a[1] == 2);
            C4("dispatch_dialogue_ui_action", 0x2a + a[0], a[1], 0, 0); }
        else if (!strcmp(cmd, "arrow")) {  /* a[0]=0 up (0x28) / 1 down (0x29) */
            S8(0x7e938, 1); C4("dispatch_dialogue_ui_action", 0x28 + a[0], 1, 0, 0); }
        else if (!strcmp(cmd, "hitgrid")) {   /* hit_test_dialogue_ui_action over the panel and a margin, every a[0] pixels */
            int ax = (int)G32(0x80b24), ay = (int)G32(0x80b28), st = a[0] > 0 ? a[0] : 3;
            log_on();
            for (int y = -3; y <= 0x53; y += st) for (int x = -3; x <= 0x123; x += st) {
                S32(0x707b3, ax + x); S32(0x707b7, ay + y);   /* g_mouse_x / g_mouse_y: the bounds test reads these */
                int r = (int)C4("hit_test_dialogue_ui_action", (uint32_t)(ax + x), (uint32_t)(ay + y), 0, 0);
                fprintf(OUT, "H %d %d %d\n", x, y, r);
            }
            fflush(OUT); log_off(); }
        else if (!strcmp(cmd, "ptr")) ptr(a[0], a[1], a[2]);   /* a[3] = how the port's hand makes it (ignored here) */
        else if (!strcmp(cmd, "ptrsweep")) {   /* the hover over the panel and a margin, every a[0] pixels */
            int st = a[0] > 0 ? a[0] : 5;
            for (int y = -3; y <= 0x53; y += st) for (int x = -3; x <= 0x123; x += st) ptr(x, y, 0); }
        else if (!strcmp(cmd, "noinspect")) S32(0x80b3c, 0);   /* drop an examine request unopened (inventory.c:1834) */
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
    const char *o = getenv("INVPROBE_OUT");
    log_on();
    OUT = fopen(o ? o : "/tmp/invprobe.log", "w");
    log_off();
    if (!OUT) { g_done = 2; return; }
    g_done = 1;
    load_syms();
    log_on(); fprintf(OUT, "# invprobe at gameplay frame %d\n", g_frames); fflush(OUT); log_off();
    initial();
    run_script();
    log_on(); fclose(OUT); OUT = 0; log_off();
    g_done = 2;
    fprintf(stderr, "[invprobe] script done\n");
    _Exit(0);
}
static void on_load(const struct roth_api_v1 *api)
{
    (void)api; const char *s;
    if ((s = getenv("INVPROBE_AT"))) g_at = atoi(s);
    if ((s = getenv("INVPROBE_SCRIPT"))) strncpy(g_script, s, sizeof g_script - 1);
    if ((s = getenv("INVPROBE_SYMS"))) strncpy(g_syms, s, sizeof g_syms - 1);
    if ((s = getenv("INVPROBE_DIR"))) strncpy(g_dir, s, sizeof g_dir - 1); else strcpy(g_dir, "/tmp");
}
static const struct roth_plugin_info_v1 INFO = {
    .abi_major = ROTH_ABI_MAJOR, .abi_minor = ROTH_ABI_MINOR, .struct_size = sizeof(struct roth_plugin_info_v1),
    .id = "io.github.vrealms.invprobe", .name = "inventory panel probe", .version = "0.1",
    .sdk_req_major = ROTH_SDK_MAJOR, .sdk_req_minor = ROTH_SDK_MINOR, .api_use = ROTH_API_USE_ENGINE | ROTH_API_USE_GAME_RAM,
    .on_load = on_load, .on_frame_game = frame, .on_register_overrides = reg,
};
ROTH_PLUGIN_EXPORT const struct roth_plugin_info_v1 *roth_plugin_query_v1(void) { return &INFO; }
