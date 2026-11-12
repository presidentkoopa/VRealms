/* dlgprobe.c -- probe mod for the ROTH.C oracle build (/home/claude/oracle_build): see README.md.
 * At gameplay frame DLGPROBE_AT it runs the script DLGPROBE_SCRIPT against the ORIGINAL's own
 * dialogue / inventory functions (addresses from DLGPROBE_SYMS = `nm roth`), writing one state
 * line after every command to DLGPROBE_OUT. tools/dialogue_test/dialogue_diff.cpp runs the same
 * script through the port and prints the same lines; `diff` is the test. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "roth_sdk.h"

static const struct roth_api_v1 *A;
static FILE *OUT;
static int g_at = 200, g_frames, g_done, g_quiet;
static char g_script[512], g_syms[512];

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
    fprintf(stderr, "[dlgprobe] NO SYMBOL %s\n", n); return 0;
}
typedef uint32_t (*f0)(void);
typedef uint32_t (*f1)(uint32_t);
typedef uint32_t (*f2)(uint32_t, uint32_t);
typedef uint32_t (*f3)(uint32_t, uint32_t, uint32_t);
typedef uint32_t (*f4)(uint32_t, uint32_t, uint32_t, uint32_t);
#define C0(n)          (((f0)(uintptr_t)fn(n))())
#define C1(n,a)        (((f1)(uintptr_t)fn(n))((uint32_t)(a)))
#define C2(n,a,b)      (((f2)(uintptr_t)fn(n))((uint32_t)(a),(uint32_t)(b)))
#define C3(n,a,b,c)    (((f3)(uintptr_t)fn(n))((uint32_t)(a),(uint32_t)(b),(uint32_t)(c)))
#define C4(n,a,b,c,d)  (((f4)(uintptr_t)fn(n))((uint32_t)(a),(uint32_t)(b),(uint32_t)(c),(uint32_t)(d)))

/* ---- overrides: log the calls that leave the two systems under test ---- */
static int g_stub_map = 1;
static int32_t ov_runobj(struct roth_chain *c, const struct roth_api_v1 *api, uint16_t ax)
{ (void)api; if (OUT) fprintf(OUT, "C run_object_commands_by_id %u\n", ax); if (g_stub_map) return 0; return roth_next_run_object_commands_by_id(c, ax); }
static int32_t ov_resetchain(struct roth_chain *c, const struct roth_api_v1 *api, uint16_t ax)
{ (void)api; if (OUT) fprintf(OUT, "C reset_command_chain_no_source %u\n", ax); if (g_stub_map) return 0; return roth_next_reset_command_chain_no_source(c, ax); }
static uint32_t ov_sfx(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t id, uint32_t p)
{ (void)api; (void)c; if (OUT && g_done != 2) { fprintf(OUT, "C play_sound_effect %u %u\n", id, p); return 0; } return roth_next_play_sound_effect(c, id, p); }
static uint32_t ov_d300(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t idx)
{ (void)api; if (OUT && g_done != 2) { fprintf(OUT, "C load_dbase300_chunk %u\n", idx); return 1; } return roth_next_load_dbase300_chunk(c, idx); }
static uint32_t ov_image(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t op)
{ (void)api; (void)c; if (OUT) fprintf(OUT, "C show_fullscreen_image %u\n", op); return 0; }
static uint32_t ov_gdv(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t rec)
{ (void)c; if (OUT && g_done != 2) { uint32_t base = api->game_ram->u32(0x81e1c); uint32_t tab = base + RD(base + 0x24);
    fprintf(OUT, "C play_record_gdv_cutscene %d\n", (int)((rec - tab) / 0x14)); } return 0; }
static void ov_exitov(struct roth_chain *c, const struct roth_api_v1 *api)
{ (void)api; (void)c; if (OUT && g_done != 2) { fprintf(OUT, "C exit_cutscene_overlay_mode\n"); return; } roth_next_exit_cutscene_overlay_mode(c); }
static void ov_health(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t a, uint32_t m)
{ (void)api; if (OUT && g_done != 2) fprintf(OUT, "C advance_clamp_8a0f0 %u %u\n", a, m); roth_next_advance_clamp_8a0f0(c, a, m); }
static void ov_tmsg(struct roth_chain *c, const struct roth_api_v1 *api, const char *msg, uint8_t col)
{ (void)api; if (OUT && g_done != 2) fprintf(OUT, "C queue_timed_message_color %u \"%s\"\n", col, msg); roth_next_queue_timed_message_color(c, msg, col); }
static void ov_lock(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t x, uint32_t y, uint32_t z, uint16_t id)
{ (void)api; if (OUT && g_done != 2) fprintf(OUT, "C begin_item_pickup_lock %d %d %d %u\n", (int)x, (int)y, (int)z, id); roth_next_begin_item_pickup_lock(c, x, y, z, id); }
static int32_t ov_rng(struct roth_chain *c, const struct roth_api_v1 *api, int32_t n)
{ (void)api; int32_t r = roth_next_rng_next_index_for_count(c, n); if (OUT && g_done != 2) fprintf(OUT, "C rng_next_index_for_count %d -> %d\n", n, r); return r; }
static uint32_t g_voice_mode;   /* 0 = as the engine does; 1 = force "no speech" */
static uint32_t ov_prime(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t clip)
{ if (OUT && g_done != 2) fprintf(OUT, "C prime_voice_clip %u\n", clip); if (g_voice_mode != 2) return 0;   /* no speech device: what the real routine answers when it has no decode buffer (audio.c:1210) */
  if (g_voice_mode == 2 && api->game_ram->u32(0x85490) == 0) {   /* give the engine its two decode buffers, as init_audio would (audio.c:383-384) */
      api->game_ram->set_u32(0x8548c, (uint32_t)(uintptr_t)malloc(0x10000)); api->game_ram->set_u32(0x85490, (uint32_t)(uintptr_t)malloc(0x10000)); }
  return roth_next_prime_voice_clip(c, clip); }
/* mode 2: the clip is "playing" as far as the engine knows, with no sound driver behind it */
static uint32_t ov_install(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t voice)
{ (void)api; if (g_voice_mode == 2) return 1; return roth_next_install_voice_sos_callback(c, voice); }
static uint32_t ov_stopvoice(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t voice)
{ (void)api; if (g_voice_mode == 2) return 0; return roth_next_sos_stop_voice(c, voice); }

static void reg(const struct roth_api_v1 *api, struct roth_registrar_v1 *r)
{
    (void)api;
    roth_override(r, ROTH_FN_run_object_commands_by_id, ov_runobj, 100);
    roth_override(r, ROTH_FN_reset_command_chain_no_source, ov_resetchain, 100);
    roth_override(r, ROTH_FN_play_sound_effect, ov_sfx, 100);
    roth_override(r, ROTH_FN_load_dbase300_chunk, ov_d300, 100);
    roth_override(r, ROTH_FN_show_fullscreen_image, ov_image, 100);
    roth_override(r, ROTH_FN_play_record_gdv_cutscene, ov_gdv, 200);
    roth_override(r, ROTH_FN_exit_cutscene_overlay_mode, ov_exitov, 100);
    roth_override(r, ROTH_FN_advance_clamp_8a0f0, ov_health, 100);
    roth_override(r, ROTH_FN_queue_timed_message_color, ov_tmsg, 100);
    roth_override(r, ROTH_FN_begin_item_pickup_lock, ov_lock, 100);
    roth_override(r, ROTH_FN_rng_next_index_for_count, ov_rng, 100);
    roth_override(r, ROTH_FN_prime_voice_clip, ov_prime, 100);
    roth_override(r, ROTH_FN_install_voice_sos_callback, ov_install, 100);
    roth_override(r, ROTH_FN_sos_stop_voice, ov_stopvoice, 100);
}

/* ---- state dump ---- */
static void hexbytes(uint32_t p, uint32_t n) { for (uint32_t i = 0; i < n; i++) fprintf(OUT, "%02x", RB(p + i)); }
static int slotidx(uint32_t p) { if (!p) return -1; return (int)((p - HP(0x80c30)) / 4); }
static void dump(const char *tag)
{
    uint32_t base = G32(0x81e1c);
    uint32_t bm = G32(0x81e28), bmsz = G32(0x81e2c);
    fprintf(OUT, "S %s\n flags=", tag);
    if (bm) hexbytes(bm, bmsz);
    fprintf(OUT, "\n inv=%d:", (int)G32(0x80c2c));
    for (int i = 0; i < 256; i++) { uint16_t id = G16(0x80c30 + i * 4), q = G16(0x80c32 + i * 4); if (id) fprintf(OUT, " %d:%u.%u", i, id, q); }
    fprintf(OUT, "\n hands L=%d R=%d adam=%d/%d auto=%d last=%d lock=%x/%d\n", slotidx(G32(0x81038)), slotidx(G32(0x81044)),
        slotidx(G32(0x8104c)), (int)G32(0x81054), (int)G32(0x89f60), (int)G32(0x89fa8), G32(0x7fd84), (int)G32(0x7fda0));
    fprintf(OUT, " q=%d:", (int)G32(0x81e3e));
    for (int i = 0; i < (int)G32(0x81e3e) && i < 8; i++)
        fprintf(OUT, " [%x,%d,%x]", G32(0x81e42 + i * 12) - base, (int)G32(0x81e46 + i * 12), G32(0x81e4a + i * 12));
    /* The panel's own drawing reads the cursor item's label, which leaves its colour in 0x82040: a
     * draw-side leftover, not compared for the screen verbs. */
    int screenverb = !strcmp(tag, "tab") || !strcmp(tag, "hold") || !strcmp(tag, "cell") || !strcmp(tag, "handbox") || !strcmp(tag, "commit") || !strcmp(tag, "useself") || !strcmp(tag, "combine");
    fprintf(OUT, "\n dlg busy=%d ctx=%d gate=%x seg=%d dwell=%d div=%d vstate=%d vsamp=%d qa=%d topic=%x col=",
        (int)G32(0x83aea), (int)G32(0x83115), G32(0x83125), (int)G32(0x83121), (int)G32(0x83119), (int)G32(0x8311d),
        (int)G32(0x8200c), (int)G32(0x8202c), (int)G32(0x81e18), G32(0x81eb2));
    if (screenverb) fprintf(OUT, "-\n"); else fprintf(OUT, "%u\n", G8(0x82040));
    fprintf(OUT, " choice cur=%x rem=%d mode=%x count=%d sel=%d saved=%d idx=", G32(0x81ea2) ? G32(0x81ea2) - base : 0, (int)G32(0x81ea6),
        G32(0x81eaa), (int)G32(0x81eae), (int)G32(0x71370), (int)G32(0x8313d));
    for (int i = 0; i < 17; i++) fprintf(OUT, "%d,", (int)G32(0x81eb6 + i * 4));
    fprintf(OUT, "\n misc l83b20=%d l80b2c=%d health=%d dead=%d overlay=%d red=%d tm=%d/%d panel=%d popup=%d\n", (int)G32(0x83b20), (int)G32(0x80b2c),
        (int)G32(0x8a0f0), (int)G32(0x7feb4), (int)G32(0x83c70), (int)G32(0x81e30), (int)G32(0x827e1), (int)G32(0x827e5), (int)G32(0x7fec4), (int)G32(0x7fec0));
    int ctx = (int)G32(0x83115);
    for (int i = 0; i < ctx && i < 16; i++) {
        uint32_t s = HP(0x827fd) + i * 0x14;
        fprintf(OUT, " seg %d x0=%d x1=%d h=%d len=%d text=", i, (int)RD(s), (int)RD(s + 4), (int)RD(s + 8), (int)RD(s + 12));
        if (RD(s + 16)) hexbytes(RD(s + 16), RD(s + 12) > 600 ? 600 : RD(s + 12));
        fprintf(OUT, "\n");
    }
    int tl = (int)G32(0x827e1);
    for (int i = 0; i < tl && i < 14; i++) {
        uint32_t s = HP(0x824c9) + i * 0x14;
        fprintf(OUT, " tml %d x0=%d x1=%d h=%d len=%d text=", i, (int)RD(s), (int)RD(s + 4), (int)RD(s + 8), (int)RD(s + 12));
        if (RD(s + 16)) hexbytes(RD(s + 16), RD(s + 12) > 600 ? 600 : RD(s + 12));
        fprintf(OUT, "\n");
    }
    fflush(OUT);
}

/* ---- snapshot / restore of the state the two systems own ---- */
struct range { uint32_t va, n; uint8_t *save; };
static struct range g_rng[] = {
    { 0x71364, 2, 0 }, { 0x71370, 4, 0 }, { 0x7fd84, 0x28, 0 }, { 0x7feb4, 0x40, 0 }, { 0x80af4, 0x50, 0 }, { 0x80c2c, 0x430, 0 },
    { 0x81304, 0xc, 0 }, { 0x819b0, 0x10, 0 }, { 0x81e14, 0xf2, 0 }, { 0x8200c, 0x35, 0 }, { 0x824c9, 0x1630, 0 }, { 0x83b20, 4, 0 },
    { 0x83c4c, 4, 0 }, { 0x83c70, 4, 0 }, { 0x89f60, 0x50, 0 }, { 0x8a0f0, 4, 0 }, { 0x8a26c, 4, 0 }, { 0x7f564, 4, 0 }, { 0x811e4, 4, 0 }, { 0x83e6c, 0x28, 0 },
};
static uint8_t *g_bmsave; static uint32_t g_bmsz; static uint8_t *g_dbsave; static uint32_t g_dbsz;
static void snapshot(void)
{
    for (unsigned i = 0; i < sizeof g_rng / sizeof g_rng[0]; i++) {
        if (!g_rng[i].save) g_rng[i].save = malloc(g_rng[i].n);
        A->game_ram->read_block(g_rng[i].va, g_rng[i].save, g_rng[i].n);
    }
    g_bmsz = G32(0x81e2c); g_bmsave = realloc(g_bmsave, g_bmsz); memcpy(g_bmsave, (void *)(uintptr_t)G32(0x81e28), g_bmsz);
    /* item runtime bytes live in the DBASE100 image itself (record +7 bit 0x80) */
    uint32_t base = G32(0x81e1c); g_dbsz = RD(base + 8) ? 0 : 0; (void)base;
}
static void restore(void)
{
    for (unsigned i = 0; i < sizeof g_rng / sizeof g_rng[0]; i++) A->game_ram->write_block(g_rng[i].va, g_rng[i].save, g_rng[i].n);
    memcpy((void *)(uintptr_t)G32(0x81e28), g_bmsave, g_bmsz);
}

static void give_ctx(int item, int rid, int x, int y, int z, int f7)
{
    static uint8_t ctx[16];
    memset(ctx, 0, sizeof ctx);
    *(int16_t *)(ctx + 0) = (int16_t)x; *(int16_t *)(ctx + 2) = (int16_t)y; *(uint16_t *)(ctx + 4) = (uint16_t)rid;
    ctx[7] = (uint8_t)f7; *(int16_t *)(ctx + 10) = (int16_t)z;
    uint32_t r = C2("give_item", (uint32_t)item, (uint32_t)(uintptr_t)ctx);
    fprintf(OUT, "R %d\n", (int)r);
}

/* panel helpers: the cursor entry table 0x7fef4, stride 0xc {icon, slotptr, id} */
static void dump_entries(void)
{
    int n = (int)G32(0x80af4);
    fprintf(OUT, "E count=%d list=%d held=%d lh=%d rh=%d dl=%d dr=%d adam=%d:", n, (int)G32(0x80b38),
        G32(0x7fef0) ? (int)((G32(0x7fef0) - HP(0x7fef4)) / 12) : -1,
        G32(0x81030) ? (int)((G32(0x81030) - HP(0x7fef4)) / 12) : -1, G32(0x8103c) ? (int)((G32(0x8103c) - HP(0x7fef4)) / 12) : -1,
        G32(0x81034) != 0, G32(0x81040) != 0,
        G32(0x81048) ? (int)((G32(0x81048) - HP(0x7fef4)) / 12) : -1);
    for (int i = 0; i < n && i < 256; i++) fprintf(OUT, " %d/%d", slotidx(G32(0x7fef4 + i * 12 + 4)), (int)G32(0x7fef4 + i * 12 + 8));
    fprintf(OUT, "\n");
}

static void run_script(void)
{
    FILE *f = fopen(g_script, "r");
    if (!f) { fprintf(stderr, "[dlgprobe] no script %s\n", g_script); return; }
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char cmd[64]; int a[8] = {0}; int n;
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        n = sscanf(line, "%63s %i %i %i %i %i %i %i %i", cmd, &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6], &a[7]) - 1;
        (void)n;
        fprintf(OUT, "> %s\n", line);
        if (!strcmp(cmd, "adv")) { fprintf(OUT, "ADV"); for (int c = 0; c < 256; c++) fprintf(OUT, " %u", C1("measure_font_char_advance", c)); fprintf(OUT, "\n"); continue; }
        else if (!strcmp(cmd, "env")) { fprintf(OUT, "ENV pitch=%d rate=%d rng=%04x,%04x,%d decodeb=%d sub=%d base300=%x hmax=%u mc=%u,%u,%u,%u,%u,%u,%u mouse=%d,%d screenh=%d marker=%u\n", (int)G32(0x85498), (int)G32(0x82809),
            G16(0x71364), G16(0x81e38), (int)G32(0x81e3a), G32(0x85490) != 0, (int)G32(0x83e90), G32(0x7f46c), G32(0x7fe44),
            G8(0x7675e), G8(0x7675f), G8(0x76760), G8(0x76761), G8(0x76762), G8(0x76763), G8(0x76764), (int)G32(0x707b3), (int)G32(0x707b7), (int)G32(0x8549c), G8(0x7675d)); continue; }
        else if (!strcmp(cmd, "snap")) { snapshot(); continue; }
        else if (!strcmp(cmd, "pitch")) { S32(0x85498, a[0]); continue; }
        else if (!strcmp(cmd, "alllines")) {
            /* every op-0x05 / op-0x08 text of every action record through the original's own reader and layout */
            typedef int32_t (*lay_fn)(int32_t *, uint8_t *, const uint8_t *, int32_t, int32_t);
            lay_fn lay = (lay_fn)(uintptr_t)fn("layout_timed_message_text");
            static int32_t meta[64 * 5]; static uint8_t obuf[0x2000]; static uint8_t ibuf[0x800];
            uint32_t base = G32(0x81e1c); int nact = (int)RD(base + 0x18); uint32_t tab = base + RD(base + 0x1c);
            for (int i = 1; i <= nact; i++) { uint32_t off = RD(tab + (i - 1) * 4); if (!off) continue;
                uint32_t rec = base + off; int n = (int)((RD(rec) & 0xffff) >> 2) - 1;
                for (int k = 0; k < n; k++) { uint32_t w = RD(rec + 4 + k * 4); uint32_t op = (w >> 24) & 0x7f; if (op != 5 && op != 8) continue;
                    memset(ibuf, 0, sizeof ibuf);
                    uint32_t len = C4("read_next_dialogue_line", (uint32_t)(uintptr_t)ibuf, 0x3fc, w & 0xffffff, 0);
                    int ctx = lay(meta, obuf, ibuf, (int32_t)G32(0x85498), a[0]);
                    fprintf(OUT, "L %d %d %u %x n=%u col=%u ctx=%d:", i, k, op, w & 0xffffff, len, G8(0x82040), ctx);
                    for (int s = 0; s < ctx && s < 64; s++) { uint32_t h = 2166136261u; uint8_t *tp = (uint8_t *)(uintptr_t)meta[s * 5 + 4];
                        for (int q = 0; q < meta[s * 5 + 3]; q++) h = (h ^ tp[q]) * 16777619u;
                        fprintf(OUT, " %d,%d,%d,%d,%08x", meta[s * 5], meta[s * 5 + 1], meta[s * 5 + 2], meta[s * 5 + 3], h); }
                    fprintf(OUT, "\n"); } }
            continue; }
        else if (!strcmp(cmd, "reset")) { restore(); }
        else if (!strcmp(cmd, "voice")) { g_voice_mode = (uint32_t)a[0]; continue; }
        else if (!strcmp(cmd, "mapcalls")) { g_stub_map = !a[0]; continue; }
        else if (!strcmp(cmd, "quiet")) { g_quiet = a[0]; continue; }
        else if (!strcmp(cmd, "act")) { uint32_t r = C1("eval_dialogue_record_by_id", a[0]); C0("finish_dialogue_record_eval"); fprintf(OUT, "R %d\n", (int)r); }
        else if (!strcmp(cmd, "qact")) { if (!G32(0x8a26c)) { S32(0x8a26c, 1); S32(0x81e18, 1); } uint32_t r = C1("eval_dialogue_record_by_id", a[0]); fprintf(OUT, "R %d\n", (int)r); }
        else if (!strcmp(cmd, "fin")) { C0("finish_dialogue_record_eval"); S32(0x8a26c, 0); S32(0x89f60, 0); }
        else if (!strcmp(cmd, "pump")) { C0("voice_stream_pump"); }
        else if (!strcmp(cmd, "tick")) { S32(0x85324, a[0]); C1("render_text_ui", 1); }
        else if (!strcmp(cmd, "frames")) { for (int i = 0; i < a[0]; i++) { C0("voice_stream_pump"); S32(0x85324, a[1] ? a[1] : 1); C1("render_text_ui", 1); } }
        else if (!strcmp(cmd, "vend")) { if (G32(0x8200c) == 1) S32(0x8200c, 2); }   /* the driver's end-of-stream callback, audio.c:1178 */
        else if (!strcmp(cmd, "next")) { C0("choice_select_next"); }
        else if (!strcmp(cmd, "prev")) { C0("choice_select_prev"); }
        else if (!strcmp(cmd, "accept")) { C0("choice_accept_selected"); }
        else if (!strcmp(cmd, "enter")) { C0("use_enter_key_handler"); }
        else if (!strcmp(cmd, "skip")) { uint32_t r = C0("try_interrupt_dialogue_voice"); fprintf(OUT, "R %d\n", (int)r); }
        else if (!strcmp(cmd, "forceend")) { C2("dialogue_voice_force_end", 0, 0); }
        else if (!strcmp(cmd, "stopall")) { C0("dialogue_voice_stop_all"); }
        else if (!strcmp(cmd, "clickrow")) {
            /* a pointer click on choice row a[0]: a point inside that row's rectangle, by the
             * original's own hit test (update_dialogue_choice_highlight, dialogue_ui.c:231-271) */
            int ctx = (int)G32(0x83115); uint32_t u = (G32(0x8549c) - G32(0x83135)) >> 1; int x = -1, y = -1;
            for (int i = 0; i < ctx; i++) { uint32_t s = HP(0x827fd) + i * 0x14;
                if (i == a[0]) { x = (int)RD(s) - 0x28 + (int)G32(0x83add) + 1; y = (int)u + 1; break; }
                u += RD(s + 8) + 4; }
            fprintf(OUT, "X %d %d\n", x, y);
            C2("dialogue_voice_force_end", x, y);
        }
        else if (!strcmp(cmd, "give")) { uint32_t r = C2("give_item", a[0], 0); fprintf(OUT, "R %d\n", (int)r); }
        else if (!strcmp(cmd, "givectx")) { give_ctx(a[0], a[1], a[2], a[3], a[4], a[5]); }
        else if (!strcmp(cmd, "remove")) { uint32_t r = C1("remove_item", a[0]); fprintf(OUT, "R %d\n", (int)r); }
        else if (!strcmp(cmd, "query")) { uint32_t r = C2("query_player_inventory", a[0], a[1]); fprintf(OUT, "R %d\n", (int)r); }
        else if (!strcmp(cmd, "list")) { static uint32_t ids[16]; int cnt = a[1]; for (int i = 0; i < cnt && i < 6; i++) ids[i] = (uint32_t)a[2 + i];
            uint32_t r = C3("find_or_autoselect_inventory_item", (uint32_t)(uintptr_t)ids, a[0], cnt); fprintf(OUT, "R %d\n", (int)r); }
        else if (!strcmp(cmd, "filter")) { C1("set_inventory_list_filter", a[0]); }
        else if (!strcmp(cmd, "easy")) { S32(0x81e34, a[0]); continue; }
        else if (!strcmp(cmd, "flagset")) { uint32_t r = C1("dbase100_bitmap_test_set", a[0]); fprintf(OUT, "R %d\n", (int)r); }
        else if (!strcmp(cmd, "flagclr")) { uint32_t r = C1("dbase100_bitmap_test_clear", a[0]); fprintf(OUT, "R %d\n", (int)r); }
        else if (!strcmp(cmd, "tmsg")) { C2("queue_timed_message_color", (uint32_t)(uintptr_t)(strchr(line, ' ') + 1), 0x20); }
        else if (!strcmp(cmd, "examine")) {   /* RMB on a world object with no trigger: input.c:995-1012 */
            uint32_t blk = C1("find_oninspect_block_by_id", a[0]); fprintf(OUT, "R %d\n", blk ? (int)(blk - G32(0x81e1c)) : 0);
            if (blk) C4("eval_dialogue_record_condition_with_cleanup", blk, 1, 0, 0); }
        else if (!strcmp(cmd, "pickup")) { /* give_item_by_dbase_id(render id, ctx) : input.c:1077 */
            static uint8_t ctx[16]; memset(ctx, 0, 16); *(int16_t *)ctx = (int16_t)a[1]; *(int16_t *)(ctx + 2) = (int16_t)a[2]; *(uint16_t *)(ctx + 4) = (uint16_t)a[0]; ctx[7] = (uint8_t)a[4]; *(int16_t *)(ctx + 10) = (int16_t)a[3];
            uint32_t pk = C1("is_item_id_pickable", a[0]);
            uint32_t r = C2("give_item_by_dbase_id", a[0], (uint32_t)(uintptr_t)ctx); fprintf(OUT, "R %d pickable=%d\n", (int)r, (int)pk); }
        else if (!strcmp(cmd, "locktick")) { S32(0x85324, a[0]); C0("tick_item_pickup_lock"); }
        /* ---- the inventory screen's verbs, on the original's own entry table ---- */
        else if (!strcmp(cmd, "tab")) { S32(0x80b38, a[0]); C1("build_inventory_entry_list", a[0]); dump_entries(); }
        else if (!strcmp(cmd, "hold")) { /* dialogue_ui.c slot_pickup with a click: entry a[0] */
            S32(0x7fec4, 1); S8(0x7e938, 0); S8(0x7e939, 1); S32(0x80b10 + G32(0x80b38) * 4, 0);
            C4("dispatch_dialogue_ui_action", 0x1c + a[0], 2, 0, 0); dump_entries(); }
        else if (!strcmp(cmd, "cell")) { /* a click on grid cell a[0]: a[1]=1 primary, 2 secondary */
            S8(0x7e938, a[1] == 1); S8(0x7e939, a[1] == 2); S32(0x80b10 + G32(0x80b38) * 4, 0);
            C4("dispatch_dialogue_ui_action", 0x1c + a[0], a[1], 0, 0); dump_entries(); }
        else if (!strcmp(cmd, "handbox")) { /* a[0]=0 left (0x2a) / 1 right (0x2b); a[1]=1 primary, 2 secondary */
            S8(0x7e938, a[1] == 1); S8(0x7e939, a[1] == 2);
            C4("dispatch_dialogue_ui_action", 0x2a + a[0], a[1], 0, 0); dump_entries(); }
        else if (!strcmp(cmd, "commit")) { C0("commit_held_cursor_item"); dump_entries(); }
        else if (!strcmp(cmd, "useself")) { C0("use_item_on_self"); dump_entries(); }
        else if (!strcmp(cmd, "combine")) { C1("combine_held_item_with_target", a[0]); dump_entries(); }
        else if (!strcmp(cmd, "topics") || !strcmp(cmd, "topic")) {
            if (!strcmp(cmd, "topics")) { uint32_t base = G32(0x81e1c); uint32_t rec = base + RD(G32(0x81e20) + (uint32_t)a[0] * 4);
                uint32_t r = C1("build_available_choice_menu", rec); fprintf(OUT, "R %d\n", (int)r); }
            else C1("activate_selected_choice_record", a[0]);
            int lc = (int)G32(0x83ac9);
            fprintf(OUT, "T mode=%d lines=%d pending=%d\n", (int)G32(0x83acd), lc, (int)G32(0x81304));
            for (int i = 0; i < lc && i < 16; i++) { uint32_t s = HP(0x83189) + i * 0x14;
                fprintf(OUT, " cl %d x0=%d x1=%d h=%d len=%d text=", i, (int)RD(s), (int)RD(s + 4), (int)RD(s + 8), (int)RD(s + 12));
                if (RD(s + 16)) hexbytes(RD(s + 16), RD(s + 12) > 600 ? 600 : RD(s + 12)); fprintf(OUT, "\n"); }
        }
        else if (!strcmp(cmd, "mem")) { fprintf(OUT, "M %x:", a[0]); for (int i = 0; i < a[1]; i++) fprintf(OUT, " %u", G32(a[0] + i * 4)); fprintf(OUT, "\n"); continue; }
        else if (!strcmp(cmd, "icons")) {
            /* every item's icon through the original's own blit_das_image_to_buffer (mode 1, 1:1), hashed */
            FILE *d2 = fopen("DBASE200.DAT", "rb");
            if (!d2) { fprintf(OUT, "? no DBASE200\n"); continue; }
            typedef void (*blit_fn)(uint32_t, uint32_t, uint32_t, uint32_t);
            blit_fn blit = (blit_fn)(uintptr_t)fn("blit_das_image_to_buffer");
            uint32_t base = G32(0x81e1c); uint32_t tab = G32(0x81e20); int n = (int)RD(base + 0x10);
            static uint8_t res[0x20000]; static uint8_t dst[0x40000];
            for (int i = 1; i <= n; i++) { uint32_t off = RD(tab + i * 4); if (!off) continue; uint32_t icon = RD(base + off + 0xc); if (!icon) continue;
                uint32_t sz = 0; fseek(d2, (long)icon << 3, SEEK_SET); if (fread(&sz, 4, 1, d2) != 1 || sz > sizeof res) { fprintf(OUT, "I %d bad\n", i); continue; }
                if (fread(res, 1, sz, d2) != sz) continue;
                int w = res[4] | res[5] << 8, h = res[6] | res[7] << 8; memset(dst, 0, sizeof dst);
                if (w * h > (int)sizeof dst) continue;
                blit((uint32_t)(uintptr_t)res, (uint32_t)(uintptr_t)dst, (uint32_t)w, 1);
                uint32_t hsh = 2166136261u; for (int k = 0; k < w * h; k++) hsh = (hsh ^ dst[k]) * 16777619u;
                fprintf(OUT, "I %d %d %d %08x\n", i, w, h, hsh); }
            fclose(d2); continue; }
        else if (!strcmp(cmd, "label")) { uint32_t id = (uint32_t)a[0]; uint32_t base = G32(0x81e1c); uint32_t tab = G32(0x81e20);
            uint32_t off = RD(tab + id * 4); uint32_t rec = base + off; uint32_t t = RD(rec + 0x10);
            uint32_t ov = C1("resolve_record_conditional_op2", rec); fprintf(OUT, "R %x %x tabidx=%d choice=%d\n", t, ov, (int)C1("get_item_tab_index", id), (int)C1("node_has_available_choice", rec)); continue; }
        else { fprintf(OUT, "? unknown command\n"); continue; }
        if (!g_quiet) dump(cmd);
    }
    fclose(f);
}

static void frame(const struct roth_api_v1 *api)
{
    A = api;
    if (api->game_ram->u8(0x7674a) != 1) return;
    g_frames++;
    if (g_done || g_frames < g_at) return;
    const char *o = getenv("DLGPROBE_OUT");
    OUT = fopen(o ? o : "/tmp/dlgprobe.log", "w");
    if (!OUT) { g_done = 2; return; }
    g_done = 1;
    load_syms();
    fprintf(OUT, "# dlgprobe at gameplay frame %d\n", g_frames);
    dump("initial");
    run_script();
    fclose(OUT); OUT = 0; g_done = 2;
    fprintf(stderr, "[dlgprobe] script done\n");
    _Exit(0);
}
static void on_load(const struct roth_api_v1 *api)
{
    (void)api; const char *s;
    if ((s = getenv("DLGPROBE_AT"))) g_at = atoi(s);
    if ((s = getenv("DLGPROBE_SCRIPT"))) strncpy(g_script, s, sizeof g_script - 1);
    if ((s = getenv("DLGPROBE_SYMS"))) strncpy(g_syms, s, sizeof g_syms - 1);
}
static const struct roth_plugin_info_v1 INFO = {
    .abi_major = ROTH_ABI_MAJOR, .abi_minor = ROTH_ABI_MINOR, .struct_size = sizeof(struct roth_plugin_info_v1),
    .id = "io.github.vrealms.dlgprobe", .name = "dialogue probe", .version = "0.1",
    .sdk_req_major = ROTH_SDK_MAJOR, .sdk_req_minor = ROTH_SDK_MINOR, .api_use = ROTH_API_USE_ENGINE | ROTH_API_USE_GAME_RAM,
    .on_load = on_load, .on_frame_game = frame, .on_register_overrides = reg,
};
ROTH_PLUGIN_EXPORT const struct roth_plugin_info_v1 *roth_plugin_query_v1(void) { return &INFO; }
