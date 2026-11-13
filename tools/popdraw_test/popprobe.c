/* popprobe.c -- probe mod for the ROTH.C oracle build (/home/claude/oracle_build): see README.md.
 * At gameplay frame POPPROBE_AT of STUDY1 it runs the script POPPROBE_SCRIPT against the ORIGINAL's
 * own inventory and examine-popup routines (give_item, render_inventory_panel,
 * dispatch_dialogue_ui_action, update_inventory_screen -> load_dbase300_resource_at_offset with its
 * film and its modal loop; addresses from POPPROBE_SYMS = `nm roth`) and writes the WHOLE
 * framebuffer (the indexed bytes) to POPPROBE_DIR/<name>.scr at every dump, with the popup's state
 * to POPPROBE_OUT. tools/popdraw_test/popdraw_diff.cpp runs the same script through the port.
 *
 * THE CLOCK. The popup's film and its modal loop are paced by two interrupts that do not run inside
 * this probe's call: the GDV timer (gdv_decode_timer_isr_noaudio, which decodes the next frame on
 * its budget) and the frame-tick counter 0x90bcc (the modal loop spins until it moves,
 * file_config.c:1010-1011). The probe stands in for both, deterministically:
 *   - the film: every gdv_callback_frame_boundary call is ONE timer tick: 0x90bcc + 1 and the
 *     lifted ISR body gdv_decode_timer_isr_noaudio once, then the original boundary;
 *   - the modal loop: a thread of the probe's own bumps 0x90bcc by one when it still equals
 *     g_last_frame_tick (0x85320) -- once per loop pass, wherever in the pass it lands, so every
 *     pass sees g_frame_time_scale 1. (The host's own tick does not reach the game while the
 *     probe's script runs inside its frame hook.)
 * Every key the original reads comes from a queue handed out through input_ring_dequeue (one entry
 * per call: a key, or an action done at that point). Outside the popup the queue answers 0. An
 * empty queue answers Esc (1).
 *
 * Every write to the log or a dump file holds SIGALRM blocked (the weapons lane's finding,
 * tools/actors_test/oracle/README.txt). */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
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
    fprintf(stderr, "[popprobe] NO SYMBOL %s\n", n); return 0;
}
typedef uint32_t (*f0)(void);
typedef uint32_t (*f1)(uint32_t);
typedef uint32_t (*f2)(uint32_t, uint32_t);
typedef uint32_t (*f4)(uint32_t, uint32_t, uint32_t, uint32_t);
#define C0(n)          (((f0)(uintptr_t)fn(n))())
#define C1(n,a)        (((f1)(uintptr_t)fn(n))((uint32_t)(a)))
#define C2(n,a,b)      (((f2)(uintptr_t)fn(n))((uint32_t)(a),(uint32_t)(b)))
#define C4(n,a,b,c,d)  (((f4)(uintptr_t)fn(n))((uint32_t)(a),(uint32_t)(b),(uint32_t)(c),(uint32_t)(d)))

/* ---- the clock (see the top) ---- */
static volatile uint16_t *g_tick, *g_last;      /* 0x90bcc, 0x85320 as host pointers (the SIGALRM side may not call the API) */
static volatile uint32_t *g_popup_active, *g_popup_state;
static volatile int g_clock_on;

static uint32_t g_isr_noaudio;                  /* gdv_decode_timer_isr_noaudio */
static volatile int g_in_film; static int g_boundaries, g_decodes, g_isr_decodes, g_stray;
static int g_in_isr;

/* ---- the key queue ---- */
enum { QK, QDUMP, QPRES, QMOUSE, QHIT, QWAIT, QBTN, QSTATE, QHOLD };
struct qe { int kind; int a, b, c; char name[64]; };
static struct qe g_q[1024]; static int g_qn, g_qi;
static int g_active;                            /* 1 while a script command runs the original's loops */
static char g_present[64];                      /* a "qp" waiting for the next present */

static void dump_screen(const char *name);
static void hit_grid(int step);
static void popstate(const char *why);

static uint8_t ov_dequeue(struct roth_chain *c, const struct roth_api_v1 *api)
{
    (void)api;
    if (!g_active) return roth_next_input_ring_dequeue(c);
    if (G32(0x7fec0) == 0) return 0;           /* not in the popup: nothing pressed */
    while (g_qi < g_qn) {
        struct qe *e = &g_q[g_qi];
        if (e->kind == QK) { g_qi++; log_on(); fprintf(OUT, "K %d at boundary %d decodes %d\n", e->a, g_boundaries, g_decodes); fflush(OUT); log_off(); return (uint8_t)e->a; }
        if (e->kind == QDUMP) { g_qi++; dump_screen(e->name); continue; }
        if (e->kind == QPRES) { g_qi++; snprintf(g_present, sizeof g_present, "%s", e->name); continue; }
        if (e->kind == QSTATE) { g_qi++; popstate("q"); continue; }
        if (e->kind == QMOUSE) { g_qi++; S32(0x707b3, e->a); S32(0x707b7, e->b); continue; }
        if (e->kind == QBTN) { g_qi++; if (e->a == 1) S8(0x7e938, 1); else S8(0x7e939, 1); return 0; }
        if (e->kind == QHOLD) { g_qi++; S8(0x7e929, (uint8_t)e->a); continue; }
        if (e->kind == QHIT) { g_qi++; hit_grid(e->a); continue; }
        if (e->kind == QWAIT) {   /* hand out 0 until this many dequeues have passed */
            if (e->c == 0) { e->c = 1; e->b = e->a; }
            if (e->b <= 0) { g_qi++; continue; }
            e->b--;
            return 0;
        }
        g_qi++;
    }
    return 1;      /* Esc */
}
static uint32_t ov_sfx(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t id, uint32_t p)
{ (void)api; if (g_done == 1) return 0; return roth_next_play_sound_effect(c, id, p); }
static int32_t ov_runobj(struct roth_chain *c, const struct roth_api_v1 *api, uint16_t ax)
{ (void)api; if (g_done == 1) return 0; return roth_next_run_object_commands_by_id(c, ax); }
static uint32_t ov_d300(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t idx)
{ (void)api; if (g_done == 1) return 1; return roth_next_load_dbase300_chunk(c, idx); }
static uint32_t ov_prime(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t clip)
{ (void)api; if (g_done != 1) return roth_next_prime_voice_clip(c, clip); return 0; }   /* no speech device: what the real routine answers with no decode buffer (audio.c:1210) */
static uint32_t ov_gdv(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t rec)
{ (void)api; if (g_done != 1) return roth_next_play_record_gdv_cutscene(c, rec); log_on(); if (OUT) fprintf(OUT, "C play_record_gdv_cutscene\n"); log_off(); return 0; }

/* the film: one timer tick per boundary */
static int ov_boundary(struct roth_chain *c, const struct roth_api_v1 *api)
{
    (void)api;
    if (g_active && g_in_film) {
        *g_tick = (uint16_t)(*g_tick + 1);
        g_in_isr = 1;
        ((void (*)(void))(uintptr_t)g_isr_noaudio)();
        g_in_isr = 0;
        g_boundaries++;
    }
    return roth_next_gdv_callback_frame_boundary(c);
}
static void ov_codec(struct roth_chain *c, const struct roth_api_v1 *api)
{
    (void)api;
    if (g_active) { g_decodes++; if (g_in_isr) g_isr_decodes++; else if (g_decodes > 1) g_stray++; }
    roth_next_gdv_decode_video_chunk(c);
}
static uint32_t ov_decode_frame(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t x)
{
    (void)api;
    if (!g_active) return roth_next_gdv_decode_frame(c, x);
    g_in_film = 1;
    log_on(); fprintf(OUT, "F start decodes %d flags %x\n", g_decodes, G32(0x91d0c)); fflush(OUT); log_off();
    uint32_t r = roth_next_gdv_decode_frame(c, x);
    g_in_film = 0;
    log_on(); fprintf(OUT, "F end -> %x boundaries %d decodes %d state %d\n", r, g_boundaries, g_decodes, (int)G32(0x810c0)); fflush(OUT); log_off();
    return r;
}
/* the present: what update_inspect_popup_choices drew this pass, before mark_overlay_dirty_rects
 * puts the saved blocks back (dialogue_ui.c:1279-1281) */
static void ov_mark(struct roth_chain *c, const struct roth_api_v1 *api)
{
    (void)api;
    if (g_active && g_present[0]) { dump_screen(g_present); g_present[0] = 0; }
    roth_next_mark_overlay_dirty_rects(c);
}
/* the document page's descriptor (file_config.c:887-898): its fields at every redraw */
static void ov_slide(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t rec)
{
    (void)api;
    if (g_active) {
        log_on();
        fprintf(OUT, "SLIDE x=%d y=%d pw=%d ph=%d w=%d h=%d sx=%d sy=%d redraw=%d\n", (int)RD(rec + 4), (int)RD(rec + 8), (int)RD(rec + 0xc), (int)RD(rec + 0x10),
            (int)RD(rec + 0x14), (int)RD(rec + 0x18), (int)RD(rec + 0x20), (int)RD(rec + 0x24), (int)RD(rec + 0x28));
        fflush(OUT);
        log_off();
    }
    roth_next_draw_das_panel_slide_reveal(c, rec);
}
static void reg(const struct roth_api_v1 *api, struct roth_registrar_v1 *r)
{
    (void)api;
    roth_override(r, ROTH_FN_input_ring_dequeue, ov_dequeue, 100);
    roth_override(r, ROTH_FN_play_sound_effect, ov_sfx, 100);
    roth_override(r, ROTH_FN_run_object_commands_by_id, ov_runobj, 100);
    roth_override(r, ROTH_FN_load_dbase300_chunk, ov_d300, 100);
    roth_override(r, ROTH_FN_prime_voice_clip, ov_prime, 100);
    roth_override(r, ROTH_FN_gdv_callback_frame_boundary, ov_boundary, 100);
    roth_override(r, ROTH_FN_gdv_decode_video_chunk, ov_codec, 100);
    roth_override(r, ROTH_FN_gdv_decode_frame, ov_decode_frame, 100);
    roth_override(r, ROTH_FN_mark_overlay_dirty_rects, ov_mark, 100);
    roth_override(r, ROTH_FN_draw_das_panel_slide_reveal, ov_slide, 100);
    roth_override(r, ROTH_FN_play_record_gdv_cutscene, ov_gdv, 200);
}
/* the modal loop's clock (see the top): a thread of the probe's own, polling. The probe's script
 * runs inside the host's frame hook, where the host's own tick does not reach the game. */
static void *clock_thread(void *arg)
{
    (void)arg;
    struct timespec ts = { 0, 200000 };
    for (;;) {
        if (g_clock_on && !g_in_film && *g_popup_active != 0 && *g_popup_state == 3 && *g_tick == *g_last)
            *g_tick = (uint16_t)(*g_tick + 1);
        nanosleep(&ts, 0);
    }
    return 0;
}

static int slotidx(uint32_t p) { return p ? (int)((p - HP(0x80c30)) / 4) : -1; }
static uint32_t fnv(uint32_t p, uint32_t n) { uint32_t h = 2166136261u; for (uint32_t i = 0; i < n; i++) h = (h ^ RB(p + i)) * 16777619u; return h; }

/* the popup's state as the original holds it */
static void hexbytes(uint32_t p, uint32_t n) { for (uint32_t i = 0; i < n; i++) fprintf(OUT, "%02x", RB(p + i)); }
static void popstate(const char *why)
{
    log_on();
    uint32_t fb = G32(0x90a98);
    fprintf(OUT, "P %s active=%d state=%d content=%d w=%d h=%d", why, (int)G32(0x7fec0), (int)G32(0x810c0),
        G32(0x810d0) ? (int)(G32(0x810d0) - fb) : -1, (int)G32(0x810d4), (int)G32(0x810d8));
    fprintf(OUT, " words");
    for (int i = 0x1c; i < 0x34; i += 2) fprintf(OUT, " %d", (int16_t)G16(0x810c0 + i));
    fprintf(OUT, " latch=%d marks=%d mode=%d sel=%d lines=%d origin=%d,%d bbox=%d,%d,%d reveal=%d alt=%d page=%d/%d pend=%d info=%d exit=%d busy=%d ctx=%d gate=%x\n",
        (int)G32(0x810f4), (int)G32(0x80b34), (int)G32(0x83acd), (int)G32(0x71370), (int)G32(0x83ac9), (int)G32(0x83add), (int)G32(0x83ae1),
        (int)G32(0x83ad1), (int)G32(0x83ad5), (int)G32(0x83ad9), (int)G32(0x83ae5), G8(0x83ae9), (int)G32(0x81184), (int)G32(0x810fc),
        (int)G32(0x81180), (int)G32(0x81188), (int)G32(0x7fecc), (int)G32(0x83aea), (int)G32(0x83115), G32(0x83125));
    fprintf(OUT, " cells");
    for (int i = 0; i < 6; i++) fprintf(OUT, " [%u,%u,%u,%u]", G16(0x80b40 + i * 8), G16(0x80b42 + i * 8), G16(0x80b44 + i * 8), G16(0x80b46 + i * 8));
    fprintf(OUT, "\n film boundaries=%d decodes=%d isr=%d stray=%d dba=%d tick=%u\n", g_boundaries, g_decodes, g_isr_decodes, g_stray,
        (int)(int16_t)G16(0x91dba), G16(0x90bcc));
    int lc = (int)G32(0x83ac9);
    for (int i = 0; i < lc && i < 16; i++) {
        uint32_t r = HP(0x83189) + i * 0x14;
        fprintf(OUT, " TL %d %d %d %d %d ", i, (int)RD(r), (int)RD(r + 4), (int)RD(r + 8), (int)RD(r + 12));
        if (RD(r + 16)) hexbytes(RD(r + 16), RD(r + 12) > 600 ? 600 : RD(r + 12));
        fprintf(OUT, "\n");
    }
    fprintf(OUT, " label ");
    hexbytes(HP(0x8105c), 0x40);
    fprintf(OUT, "\n");
    { uint32_t rh = G32(0x81044); fprintf(OUT, " RH slot=%d id=%d dirty=%u\n", slotidx(rh), rh ? (int)(int16_t)RW(rh) : 0, G8(0x7f571)); }
    fflush(OUT);
    log_off();
}

/* the whole screen: pitch x (h, doubled in the hi-res modes) indexed bytes */
static void dump_screen(const char *name)
{
    int pitch = (int)G32(0x85498), h = (int)G32(0x8549c), hires = G8(0x90cbd) != 0;
    int rows = hires ? h * 2 : h;
    uint32_t fb = G32(0x90a98);
    char path[700]; snprintf(path, sizeof path, "%s/%s.scr", g_dir, name);
    log_on();
    FILE *f = fopen(path, "wb");
    if (f) {
        uint32_t hdr[4] = { 0x52435349u /* "ISCR" */, (uint32_t)pitch, (uint32_t)rows, (uint32_t)hires };
        fwrite(hdr, 4, 4, f);
        fwrite((const void *)(uintptr_t)fb, 1, (size_t)pitch * rows, f);
        fclose(f);
    }
    fprintf(OUT, "D %s %08x\n", name, fnv(fb, (uint32_t)(pitch * rows)));
    fflush(OUT);
    log_off();
    popstate(name);
}

/* hit_test_dialogue_ui_action over the popup and a margin, every `step` pixels (click code 0);
 * the choice markers it writes are put back after each point */
static void hit_grid(int step)
{
    if (step <= 0) step = 3;
    int x0 = (int16_t)G16(0x810e4) - 6, y0 = (int16_t)G16(0x810e6) - 6;
    int x1 = x0 + (int16_t)G16(0x810e8) + 12, y1 = y0 + (int16_t)G16(0x810ea) + 12;
    uint8_t col[16]; uint32_t sel = G32(0x71370);
    int lc = (int)G32(0x83ac9);
    for (int i = 0; i < lc && i < 16; i++) col[i] = RB(RD(HP(0x83189) + i * 0x14 + 16) + 1);
    log_on();
    fprintf(OUT, "HG %d %d %d %d step=%d\n", x0, y0, x1, y1, step);
    for (int y = y0; y <= y1; y += step)
        for (int x = x0; x <= x1; x += step) {
            int r = (int)C4("hit_test_dialogue_ui_action", (uint32_t)x, (uint32_t)y, 0, 0);
            int mk = -1;
            for (int i = 0; i < lc && i < 16; i++) { uint32_t t = RD(HP(0x83189) + i * 0x14 + 16); if (RB(t) == 1 && RB(t + 1) == G8(0x7675d) && col[i] != G8(0x7675d)) mk = i; }
            if (r) fprintf(OUT, "H %d %d %d %d\n", x, y, r, mk);
            for (int i = 0; i < lc && i < 16; i++) { uint32_t t = RD(HP(0x83189) + i * 0x14 + 16); if (RB(t) == 1) *(volatile uint8_t *)(uintptr_t)(t + 1) = col[i]; }
            S32(0x71370, sel);
        }
    fflush(OUT);
    log_off();
}

static void env(void)
{
    log_on();
    fprintf(OUT, "ENV pitch=%d h=%d hires=%u ax=%d ay=%d view=%d,%d,%d,%d\n", (int)G32(0x85498), (int)G32(0x8549c), G8(0x90cbd),
        (int)G32(0x80b24), (int)G32(0x80b28), (int)G32(0x85ce0), (int)G32(0x85ce4), (int)G32(0x85cd8), (int)G32(0x85cdc));
    fprintf(OUT, "TABLES blend=%08x ramp=%08x world=%08x worldoff=%d fs=%08x\n", fnv(G32(0x85d08), 0x10000), fnv(G32(0x86d14), 0x4000),
        fnv(G32(0x86d28), 0x2000), (int)(G32(0x86d28) - G32(0x86d14)), 0u);
    fflush(OUT);
    log_off();
}
static void exebytes(uint32_t va, uint32_t n)
{
    log_on();
    fprintf(OUT, "X %x %u:", va, n);
    for (uint32_t i = 0; i < n; i++) fprintf(OUT, " %02x", G8(va + i));
    fprintf(OUT, "\n"); fflush(OUT);
    log_off();
}
static void fill_screen(int v)
{
    int pitch = (int)G32(0x85498), h = (int)G32(0x8549c), hires = G8(0x90cbd) != 0;
    memset((void *)(uintptr_t)G32(0x90a98), v, (size_t)pitch * (hires ? h * 2 : h));
}
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

static void run_script(void)
{
    FILE *f = fopen(g_script, "r");
    if (!f) { fprintf(stderr, "[popprobe] no script %s\n", g_script); return; }
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
        else if (!strcmp(cmd, "fill")) fill_screen(a[0]);
        else if (!strcmp(cmd, "give")) C2("give_item", a[0], 0);
        else if (!strcmp(cmd, "examined")) {   /* set (1) or clear (0) item a[0]'s "examined" bit (record +7 & 0x80) */
            uint32_t rec = G32(0x81e1c) + RD(G32(0x81e20) + (uint32_t)a[0] * 4);
            if (a[1]) *(volatile uint8_t *)(uintptr_t)(rec + 7) |= 0x80; else *(volatile uint8_t *)(uintptr_t)(rec + 7) &= 0x7f; }
        else if (!strcmp(cmd, "open")) { C0("render_inventory_panel"); S32(0x7fec4, 2); }
        else if (!strcmp(cmd, "tab")) { S8(0x7e938, 1); C4("dispatch_dialogue_ui_action", 0x17 + a[0], 1, 0, 0); }
        else if (!strcmp(cmd, "dump")) { char *n = strchr(line, ' '); dump_screen(n ? n + 1 : "unnamed"); }
        else if (!strcmp(cmd, "state")) popstate("cmd");
        /* the queue */
        else if (!strcmp(cmd, "q")) { g_q[g_qn].kind = QK; g_q[g_qn++].a = a[0]; }
        else if (!strcmp(cmd, "qd")) { char *n = strchr(line, ' '); g_q[g_qn].kind = QDUMP; snprintf(g_q[g_qn++].name, 64, "%s", n ? n + 1 : "unnamed"); }
        else if (!strcmp(cmd, "qp")) { char *n = strchr(line, ' '); g_q[g_qn].kind = QPRES; snprintf(g_q[g_qn++].name, 64, "%s", n ? n + 1 : "unnamed"); }
        else if (!strcmp(cmd, "qs")) { g_q[g_qn++].kind = QSTATE; }
        else if (!strcmp(cmd, "qm")) { g_q[g_qn].kind = QMOUSE; g_q[g_qn].a = a[0]; g_q[g_qn++].b = a[1]; }
        else if (!strcmp(cmd, "qb")) { g_q[g_qn].kind = QBTN; g_q[g_qn++].a = a[0]; }
        else if (!strcmp(cmd, "qhold")) { g_q[g_qn].kind = QHOLD; g_q[g_qn++].a = a[0]; }
        else if (!strcmp(cmd, "qh")) { g_q[g_qn].kind = QHIT; g_q[g_qn++].a = a[0]; }
        else if (!strcmp(cmd, "qw")) { g_q[g_qn].kind = QWAIT; g_q[g_qn].a = a[0]; g_q[g_qn].c = 0; g_q[g_qn++].b = 0; }
        /* a click on grid cell a[0] (primary): the examine request; then the frame that opens the
         * popup (update_inventory_screen -> load_dbase300_resource_at_offset), fed from the queue */
        else if (!strcmp(cmd, "cell")) {   /* a primary click on grid cell a[0]: the examine request */
            S8(0x7e938, 1); S8(0x7e939, 0);
            C4("dispatch_dialogue_ui_action", 0x1c + a[0], 1, 0, 0); }
        else if (!strcmp(cmd, "examine")) {  /* the frame that follows, the pointer at (a[0], a[1]) */
            S32(0x707b3, a[0]); S32(0x707b7, a[1]);
            g_boundaries = g_decodes = g_isr_decodes = g_stray = 0;
            log_on(); fprintf(OUT, "EX tick=%u last=%u\n", G16(0x90bcc), G16(0x85320)); fflush(OUT); log_off();
            g_active = 1; g_clock_on = 1;
            S8(0x7e938, 0); S8(0x7e939, 0);
            C0("update_inventory_screen");
            g_clock_on = 0; g_active = 0;
            log_on(); fprintf(OUT, "R examine queue %d/%d active=%d\n", g_qi, g_qn, (int)G32(0x7fec0)); fflush(OUT); log_off();
            g_qn = g_qi = 0;
            popstate("after");
        }
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
    const char *o = getenv("POPPROBE_OUT");
    log_on();
    OUT = fopen(o ? o : "/tmp/popprobe.log", "w");
    log_off();
    if (!OUT) { g_done = 2; return; }
    load_syms();
    g_tick = (volatile uint16_t *)(uintptr_t)HP(0x90bcc);
    g_last = (volatile uint16_t *)(uintptr_t)HP(0x85320);
    g_popup_active = (volatile uint32_t *)(uintptr_t)HP(0x7fec0);
    g_popup_state = (volatile uint32_t *)(uintptr_t)HP(0x810c0);
    g_isr_noaudio = fn("gdv_decode_timer_isr_noaudio");
    { pthread_t t; pthread_create(&t, 0, clock_thread, 0); }
    g_done = 1;
    log_on(); fprintf(OUT, "# popprobe at gameplay frame %d\n", g_frames); fflush(OUT); log_off();
    initial();
    run_script();
    log_on(); fclose(OUT); OUT = 0; log_off();
    g_done = 2;
    fprintf(stderr, "[popprobe] script done\n");
    _Exit(0);
}
static void on_load(const struct roth_api_v1 *api)
{
    (void)api; const char *s;
    if ((s = getenv("POPPROBE_AT"))) g_at = atoi(s);
    if ((s = getenv("POPPROBE_SCRIPT"))) strncpy(g_script, s, sizeof g_script - 1);
    if ((s = getenv("POPPROBE_SYMS"))) strncpy(g_syms, s, sizeof g_syms - 1);
    if ((s = getenv("POPPROBE_DIR"))) strncpy(g_dir, s, sizeof g_dir - 1); else strcpy(g_dir, "/tmp");
}
static const struct roth_plugin_info_v1 INFO = {
    .abi_major = ROTH_ABI_MAJOR, .abi_minor = ROTH_ABI_MINOR, .struct_size = sizeof(struct roth_plugin_info_v1),
    .id = "io.github.vrealms.popprobe", .name = "examine popup picture probe", .version = "0.1",
    .sdk_req_major = ROTH_SDK_MAJOR, .sdk_req_minor = ROTH_SDK_MINOR, .api_use = ROTH_API_USE_ENGINE | ROTH_API_USE_GAME_RAM,
    .on_load = on_load, .on_frame_game = frame, .on_register_overrides = reg,
};
ROTH_PLUGIN_EXPORT const struct roth_plugin_info_v1 *roth_plugin_query_v1(void) { return &INFO; }
