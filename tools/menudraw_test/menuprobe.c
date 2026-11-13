/* menuprobe.c -- probe mod for the ROTH.C oracle build (/home/claude/oracle_build): see README.md.
 * At gameplay frame MENUPROBE_AT it runs the script MENUPROBE_SCRIPT against the ORIGINAL's own
 * message boxes and dialogue text renderers (show_message_box, run_main_menu, run_options_menu,
 * prompt_save_overwrite, render_dialogue_text_panel, render_text_ui, render_active_timed_message,
 * ...; addresses from MENUPROBE_SYMS = `nm roth`) and writes the WHOLE framebuffer (the indexed
 * bytes) to MENUPROBE_DIR/<name>.scr at every "dump", with the state lines to MENUPROBE_OUT.
 * tools/menudraw_test/menudraw_diff.cpp runs the same script through the port and compares.
 *
 * show_message_box is the original's blocking loop. Its keys come from a QUEUE this probe hands
 * out through input_ring_dequeue (one entry per call: a key, or an action done at that point --
 * a dump of the screen as the loop left it, the mouse moved, a hit-test grid, a wait for ticks).
 * An empty queue answers Esc (1), so every box ends.
 *
 * Every write to the log or a dump file holds SIGALRM blocked (the weapons lane's finding,
 * tools/actors_test/oracle/README.txt). */
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
    fprintf(stderr, "[menuprobe] NO SYMBOL %s\n", n); return 0;
}
typedef uint32_t (*f0)(void);
typedef uint32_t (*f1)(uint32_t);
typedef uint32_t (*f2)(uint32_t, uint32_t);
typedef uint32_t (*f4)(uint32_t, uint32_t, uint32_t, uint32_t);
#define C0(n)          (((f0)(uintptr_t)fn(n))())
#define C1(n,a)        (((f1)(uintptr_t)fn(n))((uint32_t)(a)))
#define C2(n,a,b)      (((f2)(uintptr_t)fn(n))((uint32_t)(a),(uint32_t)(b)))
#define C4(n,a,b,c,d)  (((f4)(uintptr_t)fn(n))((uint32_t)(a),(uint32_t)(b),(uint32_t)(c),(uint32_t)(d)))

/* ---- the key queue ---- */
enum { QK, QDUMP, QMOUSE, QHIT, QWAIT, QBTN };
struct qe { int kind; int a, b, c; char name[64]; };
static struct qe g_q[512]; static int g_qn, g_qi;
static int g_active;
static int g_fillv;              /* the last "fill": dtext fills the screen with it before drawing */                 /* 1 while a script command runs the original's loops */

static void dump_screen(const char *name);
static void hit_grid(int step);
static void log_pass(void);

static uint8_t ov_dequeue(struct roth_chain *c, const struct roth_api_v1 *api)
{
    (void)api;
    if (!g_active) return roth_next_input_ring_dequeue(c);
    log_pass();
    while (g_qi < g_qn) {
        struct qe *e = &g_q[g_qi];
        if (e->kind == QK) { g_qi++; return (uint8_t)e->a; }
        if (e->kind == QDUMP) { g_qi++; dump_screen(e->name); continue; }
        if (e->kind == QMOUSE) { g_qi++; S32(0x707b3, e->a); S32(0x707b7, e->b); return 0; }
        if (e->kind == QBTN) { g_qi++; if (e->a == 1) S8(0x7e938, 1); else S8(0x7e939, 1); return 0; }
        if (e->kind == QHIT) { g_qi++; hit_grid(e->a); continue; }
        if (e->kind == QWAIT) {   /* hand out 0 until the frame tick has moved a ticks */
            if (e->c == 0) { e->c = 1; e->b = G16(0x90bcc); }
            if ((uint16_t)(G16(0x90bcc) - e->b) >= (uint16_t)e->a) { g_qi++; continue; }
            return 0;
        }
        g_qi++;
    }
    return 1;      /* Esc: every box ends */
}
static uint32_t ov_sfx(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t id, uint32_t p)
{ (void)api; if (g_active) return 0; return roth_next_play_sound_effect(c, id, p); }
static uint32_t ov_gdv(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t rec)
{ (void)api; (void)c; (void)rec; log_on(); if (OUT) fprintf(OUT, "C play_record_gdv_cutscene\n"); log_off(); return 0; }
static uint32_t ov_prime(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t clip)
{ (void)api; (void)c; (void)clip; return 0; }   /* no speech device: what the real routine answers with no decode buffer (audio.c:1210) */

/* One open show_message_box: its descriptor, and the arguments its loop last gave
 * hit_test_ui_element (its row table, rows, list y, flags, box x/y). */
struct boxf { uint32_t desc, flags; int32_t *hc; uint32_t hcount, hxbase, hflags; int32_t hybox, hxbox; int iter; };
static struct boxf g_bx[16]; static int g_nbx;
static int g_hbypass;
static int32_t ov_hit(struct roth_chain *c, const struct roth_api_v1 *api, int32_t *cells, int32_t y, uint32_t x, uint32_t count, uint32_t xbase, uint32_t flags, int32_t ybox, int32_t xbox)
{
    (void)api;
    if (!g_hbypass && g_nbx > 0) { struct boxf *b = &g_bx[g_nbx - 1]; b->hc = cells; b->hcount = count; b->hxbase = xbase; b->hflags = flags; b->hybox = ybox; b->hxbox = xbox; }
    return roth_next_hit_test_ui_element(c, cells, y, x, count, xbase, flags, ybox, xbox);
}
/* every box the original opens, and what it returns */
static uint32_t ov_box(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t desc, uint32_t flags)
{
    (void)api;
    if (!g_active || g_nbx >= 16) return roth_next_show_message_box(c, desc, flags);
    struct boxf *b = &g_bx[g_nbx++];
    memset(b, 0, sizeof *b);
    b->desc = desc - HP(0); b->flags = flags;
    log_on(); fprintf(OUT, "O %d %x %x sel=%d scroll=%d range=%d\n", g_nbx, b->desc, flags, (int16_t)RW(desc + 0xa), RW(desc + 0xc), RW(desc + 0xe)); fflush(OUT); log_off();
    uint32_t r = roth_next_show_message_box(c, desc, flags);
    log_on(); fprintf(OUT, "C %d %x -> %d sel=%d scroll=%d\n", g_nbx, b->desc, (int)r, (int16_t)RW(desc + 0xa), RW(desc + 0xc)); fflush(OUT); log_off();
    g_nbx--;
    return r;
}
/* the state the box's last loop pass left: which content row carries the marker colour
 * (menu_hud_ui.c:1683-1696), the scroll word; "pre" before its first pass */
static void log_pass(void)
{
    if (g_nbx == 0) return;
    struct boxf *b = &g_bx[g_nbx - 1];
    log_on();
    if (!b->hc) fprintf(OUT, "L %d pre\n", g_nbx);
    else {
        int mark = -1;
        for (uint32_t i = 0; i < b->hcount; i++) { uint32_t t = (uint32_t)b->hc[i * 5 + 4]; if (t && RB(t) == 1 && RB(t + 1) == G8(0x7675d)) { mark = (int)i; break; } }
        fprintf(OUT, "L %d mark=%d scroll=%d\n", g_nbx, mark, RW(HP(b->desc) + 0xc));
    }
    fflush(OUT);
    log_off();
}
/* update_frame_time_scale spins on the timer ISR's tick counter (game_core.c:329-336), which does not
 * run inside this probe's call: each call made while a script command runs advances the counter by
 * g_tickstep first, so every loop iteration of the original is exactly that many frame ticks. */
static int g_tickstep = 1;
static void ov_fts(struct roth_chain *c, const struct roth_api_v1 *api)
{ (void)api; if (g_active) S16(0x90bcc, (uint16_t)(G16(0x90bcc) + g_tickstep)); roth_next_update_frame_time_scale(c); }
static void reg(const struct roth_api_v1 *api, struct roth_registrar_v1 *r)
{
    roth_override(r, ROTH_FN_update_frame_time_scale, ov_fts, 100);
    (void)api;
    roth_override(r, ROTH_FN_input_ring_dequeue, ov_dequeue, 100);
    roth_override(r, ROTH_FN_play_sound_effect, ov_sfx, 100);
    roth_override(r, ROTH_FN_play_record_gdv_cutscene, ov_gdv, 200);
    roth_override(r, ROTH_FN_prime_voice_clip, ov_prime, 100);
    roth_override(r, ROTH_FN_hit_test_ui_element, ov_hit, 100);
    roth_override(r, ROTH_FN_show_message_box, ov_box, 100);
}

static uint32_t fnv(uint32_t p, uint32_t n) { uint32_t h = 2166136261u; for (uint32_t i = 0; i < n; i++) h = (h ^ RB(p + i)) * 16777619u; return h; }

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
}

/* hit_test_ui_element over the last box's frame and a margin, every `step` pixels, with the
 * arguments show_message_box last gave it (its row table, rows, list y, flags, box x/y). */
static void hit_grid(int step)
{
    typedef int32_t (*hf)(int32_t *, int32_t, uint32_t, uint32_t, uint32_t, uint32_t, int32_t, int32_t);
    hf h = (hf)(uintptr_t)fn("hit_test_ui_element");
    if (step <= 0) step = 3;
    if (g_nbx == 0 || !g_bx[g_nbx - 1].hc) { log_on(); fprintf(OUT, "HG none\n"); log_off(); return; }
    struct boxf *b = &g_bx[g_nbx - 1];
    /* the frame and the rows the hit test knows: the box, then the list (x is the screen's) */
    int x0 = b->hybox - 6, y0 = b->hxbox - 6;
    int x1 = b->hybox + 0x130, y1 = (int)b->hxbase + (int)b->hcount * 0xb + 0x50;
    log_on();
    fprintf(OUT, "HG box=%d,%d rows=%u listy=%u flags=%x step=%d\n", b->hybox, b->hxbox, b->hcount, b->hxbase, b->hflags, step);
    g_hbypass = 1;
    for (int y = y0; y <= y1; y += step)
        for (int x = x0; x <= x1; x += step) {
            int r = h(b->hc, x, (uint32_t)y, b->hcount, b->hxbase, b->hflags, b->hybox, b->hxbox);
            if (r) fprintf(OUT, "H %d %d %d\n", x, y, r);
        }
    g_hbypass = 0;
    fflush(OUT);
    log_off();
}

static void env(void)
{
    log_on();
    fprintf(OUT, "ENV pitch=%d h=%d hires=%u ax=%d ay=%d view=%d,%d,%d,%d frontend=%d\n", (int)G32(0x85498), (int)G32(0x8549c), G8(0x90cbd),
        (int)G32(0x80b24), (int)G32(0x80b28), (int)G32(0x85ce0), (int)G32(0x85ce4), (int)G32(0x85cd8), (int)G32(0x85cdc), (int)G32(0x83ea0));
    fprintf(OUT, "TABLES blend=%08x ramp=%08x world=%08x worldoff=%d\n", fnv(G32(0x85d08), 0x10000), fnv(G32(0x86d14), 0x4000),
        fnv(G32(0x86d28), 0x2000), (int)(G32(0x86d28) - G32(0x86d14)));
    fprintf(OUT, "SNAPSHOT handle=%x\n", G32(0x811ac));
    fprintf(OUT, "STR");
    static const uint32_t s[] = { 0x764f0 + 0x50, 0x75610 + 0x8a4, 0x75610 + 0x8ad, 0x75610 + 0x890, 0x75610 + 0x899, 0x75ef9, 0x75ef4, 0x75f00, 0x71374 };
    for (unsigned i = 0; i < sizeof s / sizeof s[0]; i++) fprintf(OUT, " [%s]", (const char *)(uintptr_t)HP(s[i]));
    fprintf(OUT, "\n");
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

/* a save file the original's read_savegame_slot_names / read_savegame_thumbnail read:
 * tag 0xa {name}, tag 0xb {a DAS picture w x h, flags 2 (no palette), plain rows}, tag 0 */
static void savefile(int slot, int w, int h, int seed, const char *name)
{
    /* the original's path: <0x76540 = "">\savegame\SAVE%D.SAV, relative to the game folder */
    char path[64];
    snprintf(path, sizeof path, "savegame/SAVE%d.SAV", slot);
    log_on();
    FILE *f = fopen(path, "wb");
    fprintf(OUT, "SAVEFILE %s %s\n", path, f ? "written" : "NOT WRITTEN");
    if (f) {
        uint16_t t[2];
        size_t nl = strlen(name) + 1;
        if (name[0] == '-') nl = 0;            /* "-": a file with no name record */
        if (nl) { t[0] = 0xa; t[1] = (uint16_t)nl; fwrite(t, 4, 1, f); fwrite(name, 1, nl, f); }
        if (w > 0) {
            uint8_t hdr[8] = { 2, 0, 0, 0, (uint8_t)w, (uint8_t)(w >> 8), (uint8_t)h, (uint8_t)(h >> 8) };
            t[0] = 0xb; t[1] = (uint16_t)(8 + w * h); fwrite(t, 4, 1, f); fwrite(hdr, 8, 1, f);
            uint32_t r = (uint32_t)seed * 2654435761u;
            for (int i = 0; i < w * h; i++) { r = r * 1103515245u + 12345u; fputc((int)((r >> 16) & 0xff), f); }
        }
        t[0] = 0; t[1] = 0; fwrite(t, 4, 1, f);
        fclose(f);
    }
    fflush(OUT);
    log_off();
}

/* ---- the dialogue text: the records the renderers read, and their pictures ---- */
static void hexbytes(uint32_t p, uint32_t n) { for (uint32_t i = 0; i < n; i++) fprintf(OUT, "%02x", RB(p + i)); }
static void text_state(void)
{
    log_on();
    fprintf(OUT, "T gate=%x ctx=%d seg=%d mode=%d mode10=%d inv=%d ay=%d vy=%d vh=%d tm=%d timer=%d busy=%d panel=%d,%d,%d\n", G32(0x83125), (int)G32(0x83115),
        (int)G32(0x83121), (int)G32(0x83acd), (int)G32(0x83add), (int)G32(0x7fec4), (int)G32(0x80b28), (int)G32(0x85ce4), (int)G32(0x85cdc),
        (int)G32(0x827e1), (int)G32(0x827e5), (int)G32(0x83aea), (int)G32(0x8312d), (int)G32(0x83131), (int)G32(0x83135));
    int ctx = (int)G32(0x83115);
    for (int i = 0; i < ctx && i < 16; i++) {
        uint32_t r = HP(0x827fd) + i * 0x14;
        fprintf(OUT, "TS %d %d %d %d %d ", i, (int)RD(r), (int)RD(r + 4), (int)RD(r + 8), (int)RD(r + 12));
        if (RD(r + 16)) hexbytes(RD(r + 16), RD(r + 12) > 600 ? 600 : RD(r + 12));
        fprintf(OUT, "\n");
    }
    int tl = (int)G32(0x827e1);
    for (int i = 0; i < tl && i < 14; i++) {
        uint32_t r = HP(0x824c9) + i * 0x14;
        fprintf(OUT, "TT %d %d %d %d %d ", i, (int)RD(r), (int)RD(r + 4), (int)RD(r + 8), (int)RD(r + 12));
        if (RD(r + 16)) hexbytes(RD(r + 16), RD(r + 12) > 600 ? 600 : RD(r + 12));
        fprintf(OUT, "\n");
    }
    fflush(OUT);
    log_off();
}
/* update_dialogue_choice_highlight over a grid around the choice panel, its state put back after */
static void choice_grid(int step)
{
    if (step <= 0) step = 3;
    if (G32(0x83aea) == 0 || G32(0x83125) != 0x6ffff) { log_on(); fprintf(OUT, "CHG none\n"); log_off(); return; }
    int ctx = (int)G32(0x83115);
    uint8_t col[16]; uint32_t sel = G32(0x71370), saved = G32(0x8313d), last = G32(0x83b16);
    for (int i = 0; i < ctx && i < 16; i++) col[i] = RB(RD(HP(0x827fd) + i * 0x14 + 16) + 1);
    int x0 = (int)G32(0x8312d), x1 = (int)G32(0x83131), h = (int)G32(0x83135);
    int top = (int)((G32(0x8549c) - (uint32_t)h) >> 1);
    log_on();
    fprintf(OUT, "CHG %d %d %d %d step=%d\n", x0 - 8, top - 8, x1 + 8, top + h + 9, step);
    for (int y = top - 8; y <= top + h + 9; y += step)
        for (int x = x0 - 8; x <= x1 + 8; x += step) {
            S32(0x83b16, 0x7fffffff);
            uint32_t r = C2("update_dialogue_choice_highlight", (uint32_t)x, (uint32_t)y);
            if (r == 1) fprintf(OUT, "CH %d %d %d\n", x, y, (int)G32(0x8313d));
        }
    fflush(OUT);
    log_off();
    for (int i = 0; i < ctx && i < 16; i++) { uint32_t t = RD(HP(0x827fd) + i * 0x14 + 16); if (RB(t) == 1) *(volatile uint8_t *)(uintptr_t)(t + 1) = col[i]; }
    S32(0x71370, sel); S32(0x8313d, saved); S32(0x83b16, last);
}

static void run_script(void)
{
    FILE *f = fopen(g_script, "r");
    if (!f) { fprintf(stderr, "[menuprobe] no script %s\n", g_script); return; }
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
        else if (!strcmp(cmd, "fill")) { g_fillv = a[0]; fill_screen(a[0]); }
        else if (!strcmp(cmd, "frontend")) S32(0x83ea0, a[0]);
        else if (!strcmp(cmd, "tickstep")) g_tickstep = a[0];
        else if (!strcmp(cmd, "snapshot")) {   /* g_ui_panel_scratch_handle (0x811ac): a handle to a DAS picture, or 0 */
            static uint8_t img[8 + 0x4e * 0x38 + 16]; static uint32_t ptr, hnd;
            if (a[0] <= 0) S32(0x811ac, 0);
            else {
                int w = a[0], h = a[1]; img[0] = 2; img[4] = (uint8_t)w; img[5] = (uint8_t)(w >> 8); img[6] = (uint8_t)h; img[7] = (uint8_t)(h >> 8);
                uint32_t r = (uint32_t)a[2] * 2654435761u;
                for (int i = 0; i < w * h && i < 0x4e * 0x38; i++) { r = r * 1103515245u + 12345u; img[8 + i] = (uint8_t)((r >> 16) & 0xff); }
                ptr = (uint32_t)(uintptr_t)img; hnd = (uint32_t)(uintptr_t)&ptr; S32(0x811ac, hnd);
            } }
        else if (!strcmp(cmd, "rmsaves")) { for (int k = 1; k <= 9; k++) { char p[64]; snprintf(p, sizeof p, "savegame/SAVE%d.SAV", k); remove(p); } }
        else if (!strcmp(cmd, "mouse")) { S32(0x707b3, a[0]); S32(0x707b7, a[1]); }
        else if (!strcmp(cmd, "dump")) { char *n = strchr(line, ' '); dump_screen(n ? n + 1 : "unnamed"); }
        else if (!strcmp(cmd, "savefile")) { char *n = line; for (int k = 0; k < 5 && n; k++) n = strchr(n + 1, ' '); savefile(a[0], a[1], a[2], a[3], n ? n + 1 : "-"); }
        /* ---- the dialogue (the dialogue test's verbs: tools/dialogue_test/dlgprobe.c) ---- */
        else if (!strcmp(cmd, "act")) { uint32_t r = C1("eval_dialogue_record_by_id", a[0]); C0("finish_dialogue_record_eval"); log_on(); fprintf(OUT, "R %d\n", (int)r); log_off(); }
        else if (!strcmp(cmd, "qact")) { if (!G32(0x8a26c)) { S32(0x8a26c, 1); S32(0x81e18, 1); } uint32_t r = C1("eval_dialogue_record_by_id", a[0]); log_on(); fprintf(OUT, "R %d\n", (int)r); log_off(); }
        else if (!strcmp(cmd, "fin")) { C0("finish_dialogue_record_eval"); S32(0x8a26c, 0); S32(0x89f60, 0); }
        else if (!strcmp(cmd, "pump")) C0("voice_stream_pump");
        else if (!strcmp(cmd, "tick")) { S32(0x85324, a[0]); C1("render_text_ui", 1); }
        else if (!strcmp(cmd, "next")) C0("choice_select_next");
        else if (!strcmp(cmd, "prev")) C0("choice_select_prev");
        else if (!strcmp(cmd, "skip")) C0("try_interrupt_dialogue_voice");
        else if (!strcmp(cmd, "stopall")) C0("dialogue_voice_stop_all");
        else if (!strcmp(cmd, "flagset")) C1("dbase100_bitmap_test_set", a[0]);
        else if (!strcmp(cmd, "tmsg")) C2("queue_timed_message_color", (uint32_t)(uintptr_t)(strchr(line, ' ') + 1), 0x20);
        else if (!strcmp(cmd, "hover")) { uint32_t r = C2("update_dialogue_choice_highlight", (uint32_t)a[0], (uint32_t)a[1]); log_on(); fprintf(OUT, "R %d\n", (int)r); log_off(); }
        else if (!strcmp(cmd, "invopen")) { S32(0x7fec4, a[0]); S32(0x80b28, a[1]); }
        else if (!strcmp(cmd, "chgrid")) choice_grid(a[0]);
        else if (!strcmp(cmd, "dtext")) {   /* render_text_ui(1) at frame scale 0 (nothing advances) on a filled screen */
            char *n = strchr(line, ' ');
            text_state();
            fill_screen(g_fillv);
            S32(0x85324, 0);
            C1("render_text_ui", 1);
            dump_screen(n ? n + 1 : "unnamed");
        }
        /* the queue */
        else if (!strcmp(cmd, "q")) { g_q[g_qn].kind = QK; g_q[g_qn++].a = a[0]; }
        else if (!strcmp(cmd, "qd")) { char *n = strchr(line, ' '); g_q[g_qn].kind = QDUMP; snprintf(g_q[g_qn++].name, 64, "%s", n ? n + 1 : "unnamed"); }
        else if (!strcmp(cmd, "qm")) { g_q[g_qn].kind = QMOUSE; g_q[g_qn].a = a[0]; g_q[g_qn++].b = a[1]; }
        else if (!strcmp(cmd, "qb")) { g_q[g_qn].kind = QBTN; g_q[g_qn++].a = a[0]; }
        else if (!strcmp(cmd, "qh")) { g_q[g_qn].kind = QHIT; g_q[g_qn++].a = a[0]; }
        else if (!strcmp(cmd, "qw")) { g_q[g_qn].kind = QWAIT; g_q[g_qn].a = a[0]; g_q[g_qn].c = 0; g_qn++; }
        /* the original's boxes and menus, fed from the queue */
        else if (!strcmp(cmd, "box") || !strcmp(cmd, "slotbox") || !strcmp(cmd, "mainmenu") || !strcmp(cmd, "optionsmenu")) {
            static uint8_t names[0x1b0];
            uint32_t r = 0;
            g_active = 1;
            S8(0x7e938, 0); S8(0x7e939, 0);
            if (!strcmp(cmd, "box")) r = C2("show_message_box", HP((uint32_t)a[0]), (uint32_t)a[1]);
            else if (!strcmp(cmd, "slotbox")) {   /* run_options_menu's load / save branch (menu_hud_ui.c:2044-2049) */
                for (uint32_t i = 0; i < 5; i++) S32((uint32_t)a[0] + 0x18 + i * 0xc, (uint32_t)(uintptr_t)names);
                C1("read_savegame_slot_names", (uint32_t)(uintptr_t)names);
                r = C2("show_message_box", HP((uint32_t)a[0]), (uint32_t)a[1]);
            }
            else if (!strcmp(cmd, "mainmenu")) {
                /* run_main_menu (menu_hud_ui.c:2079-2107), as the original's loop: the headless host
                 * answers run_main_menu itself (it starts the game at boot), so the probe runs its body */
                uint32_t fl = 0x201;
                for (;;) {
                    S32(0x83ea0, 1);
                    uint32_t raw = C2("show_message_box", HP(0x71bbd), fl);
                    fl = 1;
                    uint32_t m = raw & 0x1fff;
                    if (m <= 1) { r = 0xa; break; }
                    if (m == 2) {
                        for (uint32_t i = 0; i < 5; i++) S32(0x71c5d + 0x18 + i * 0xc, (uint32_t)(uintptr_t)names);
                        C1("read_savegame_slot_names", (uint32_t)(uintptr_t)names);
                        uint32_t rr = C2("show_message_box", HP(0x71c5d), 0xd);
                        S16(0x71c19, G16(0x71c69));
                        if (rr != 0) { r = (rr << 16) + 3; break; }
                    } else if (m == 0x3e8) { uint32_t hi = raw >> 16; if (hi != 0) { r = hi - 1; break; } }
                    if (raw == 0) { r = 0; break; }
                }
            }
            else r = C1("run_options_menu", 0);
            g_active = 0;
            log_on(); fprintf(OUT, "R %d queue %d/%d\n", (int)r, g_qi, g_qn); fflush(OUT); log_off();
            g_qn = g_qi = 0;
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
    const char *o = getenv("MENUPROBE_OUT");
    log_on();
    OUT = fopen(o ? o : "/tmp/menuprobe.log", "w");
    log_off();
    if (!OUT) { g_done = 2; return; }
    g_done = 1;
    load_syms();
    log_on(); fprintf(OUT, "# menuprobe at gameplay frame %d\n", g_frames); fflush(OUT); log_off();
    run_script();
    log_on(); fclose(OUT); OUT = 0; log_off();
    g_done = 2;
    fprintf(stderr, "[menuprobe] script done\n");
    _Exit(0);
}
static void on_load(const struct roth_api_v1 *api)
{
    (void)api; const char *s;
    if ((s = getenv("MENUPROBE_AT"))) g_at = atoi(s);
    if ((s = getenv("MENUPROBE_SCRIPT"))) strncpy(g_script, s, sizeof g_script - 1);
    if ((s = getenv("MENUPROBE_SYMS"))) strncpy(g_syms, s, sizeof g_syms - 1);
    if ((s = getenv("MENUPROBE_DIR"))) strncpy(g_dir, s, sizeof g_dir - 1); else strcpy(g_dir, "/tmp");
}
static const struct roth_plugin_info_v1 INFO = {
    .abi_major = ROTH_ABI_MAJOR, .abi_minor = ROTH_ABI_MINOR, .struct_size = sizeof(struct roth_plugin_info_v1),
    .id = "io.github.vrealms.menuprobe", .name = "menu / dialogue picture probe", .version = "0.1",
    .sdk_req_major = ROTH_SDK_MAJOR, .sdk_req_minor = ROTH_SDK_MINOR, .api_use = ROTH_API_USE_ENGINE | ROTH_API_USE_GAME_RAM,
    .on_load = on_load, .on_frame_game = frame, .on_register_overrides = reg,
};
ROTH_PLUGIN_EXPORT const struct roth_plugin_info_v1 *roth_plugin_query_v1(void) { return &INFO; }
