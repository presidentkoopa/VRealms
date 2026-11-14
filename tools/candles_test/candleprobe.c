/* candleprobe.c -- probe mod for the ROTH.C oracle (tools/candles_test/run_oracle.sh builds and runs it).
 * Never part of the engine build. Drives the ORIGINAL through a frame-scheduled script and prints what a
 * candle's use changed: every byte of the command table, the geometry (sector light bytes included) and
 * the objects that differs from the snapshot, the chain globals, the deferred queue and the flag bitmap.
 *
 * Script lines (CANDLE_SCRIPT): "<gameframe> <op> [args]", in frame order.
 *   give ID         give_item(ID, 0) (inventory.c:372): the item enters the inventory as the game gives it
 *   right ID        the right hand (g_selected_item_primary 0x81044) takes the first slot holding ID
 *   click S I       run_leftclick_object_trigger (raw_commands.c:2920) on object I of sector S: the
 *                   original's left click on an object, with whatever the right hand holds
 *   snap            the reference for diff
 *   diff TAG        every byte changed since snap (C cmd, G geometry, O object), the globals, the flags
 *   quit
 * Output: CANDLE_OUT. Symbols: CANDLE_SYMS ("addr T name" lines, nm format). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include "roth_sdk.h"
#define D 0x400000u
#define M8(va)  (*(volatile uint8_t  *)(uintptr_t)((va) + D))
#define M16(va) (*(volatile uint16_t *)(uintptr_t)((va) + D))
#define M32(va) (*(volatile uint32_t *)(uintptr_t)((va) + D))
#define H8(p)   (*(volatile uint8_t  *)(uintptr_t)(p))
#define H16(p)  (*(volatile uint16_t *)(uintptr_t)(p))
#define H32(p)  (*(volatile uint32_t *)(uintptr_t)(p))
static FILE *out;
static int g_frames;
static struct { char name[64]; uint32_t addr; } syms[1024]; static int nsyms;
static uint32_t sym(const char *n) { for (int i = 0; i < nsyms; i++) if (!strcmp(syms[i].name, n)) return syms[i].addr;
    fprintf(out, "!! no symbol %s\n", n); return 0; }
struct line { int frame; char op[16]; char args[128]; };
static struct line g_script[512]; static int g_n, g_pc;
static uint32_t hdr(void) { return M32(0x85c30); }
static uint32_t ncmd(void) { uint32_t h = hdr(); return (h && H16(h) == 0x7533) ? H16(h + 6) : 0; }
static uint32_t recptr(uint32_t idx) { if (idx == 0 || idx > ncmd()) return 0; uint32_t arr = H32(M32(0x8a0d8)); return H32(arr + (idx - 1) * 4); }
static uint8_t *snapC, *snapG, *snapO, *snapF; static uint32_t szC, szG, szO, szF;
static uint32_t geomsize(void) { return H16(M32(0x90aa8)); }
static uint32_t objsize(void) { return (uint32_t)H16(M32(0x90aa4)) + 2; }
static uint32_t cmdsize(void) { uint32_t h = hdr(); if (!h) return 0; uint32_t n = ncmd(); if (!n) return 0; uint32_t last = recptr(n); return last + H16(last) - h; }
static uint32_t objptr(uint32_t s, uint32_t i) { uint32_t ob = M32(0x90aa4), off = H16(ob + 2 + s * 2); return off ? ob + off + 2 + i * 16 : 0; }
static void snap(void)
{
    free(snapC); free(snapG); free(snapO); free(snapF);
    szC = cmdsize(); szG = geomsize(); szO = objsize(); szF = 0x100;
    snapC = malloc(szC + 1); snapG = malloc(szG + 1); snapO = malloc(szO + 1); snapF = malloc(szF);
    memcpy(snapC, (void *)(uintptr_t)hdr(), szC); memcpy(snapG, (void *)(uintptr_t)M32(0x90aa8), szG); memcpy(snapO, (void *)(uintptr_t)M32(0x90aa4), szO);
    uint32_t bm = M32(0x81e28); if (bm) memcpy(snapF, (void *)(uintptr_t)bm, szF); else memset(snapF, 0, szF);
}
static void diff(const char *tag)
{
    fprintf(out, "D %s\n", tag);
    uint32_t n = ncmd(), h = hdr();
    for (uint32_t i = 1; i <= n; i++) { uint32_t p = recptr(i), o = p - h, sz = H16(p);
        for (uint32_t k = 0; k < sz && o + k < szC; k++) if (snapC[o + k] != H8(p + k)) fprintf(out, "C %u %u %02x %02x\n", i, k, snapC[o + k], H8(p + k)); }
    uint32_t g = M32(0x90aa8);
    for (uint32_t k = 0; k < szG && k < geomsize(); k++) if (snapG[k] != H8(g + k)) fprintf(out, "G %u %02x %02x\n", k, snapG[k], H8(g + k));
    uint32_t ob = M32(0x90aa4);
    if (objsize() != szO) fprintf(out, "O! size %u -> %u\n", szO, objsize());
    else for (uint32_t k = 0; k < szO; k++) if (snapO[k] != H8(ob + k)) fprintf(out, "O %u %02x %02x\n", k, snapO[k], H8(ob + k));
    uint32_t bm = M32(0x81e28);
    for (uint32_t k = 0; bm && k < szF; k++) if (snapF[k] != H8(bm + k)) fprintf(out, "F %u %02x %02x\n", k, snapF[k], H8(bm + k));
    uint32_t q = M32(0x71f40), qn = H32(q);
    fprintf(out, "S int=%u pend=%x mirror=%08x nextid=%08x src=%s crng=%08x q=%u\n", M8(0x8a268), M32(0x8a0dc) ? 1u : 0u,
        M32(0x8a0c4), M32(0x8a0c0), M32(0x8a100) ? "set" : "-", M32(0x71f48), qn);
}
typedef uint32_t (*fn2)(uint32_t, uint32_t);
typedef int32_t (*fnregs)(const void *);
static void run(struct line *l)
{
    unsigned a = 0, b = 0;
    if (!strcmp(l->op, "give")) { sscanf(l->args, "%u", &a); uint32_t r = ((fn2)(uintptr_t)sym("give_item"))(a, 0); fprintf(out, "R give %u -> %u\n", a, r != 0); }
    else if (!strcmp(l->op, "right")) { sscanf(l->args, "%u", &a); int slot = -1;
        for (int i = 0; i < 256; i++) if (M16(0x80c30 + i * 4) == a) { slot = i; break; }
        M32(0x81044) = slot < 0 ? 0 : (uint32_t)(0x80c30 + D + slot * 4); fprintf(out, "R right %u slot %d\n", a, slot); }
    else if (!strcmp(l->op, "click")) { sscanf(l->args, "%u %u", &a, &b);
        uint32_t regs[24]; memset(regs, 0, sizeof regs); regs[0] = objptr(a, b);
        int32_t r = ((fnregs)(uintptr_t)sym("run_leftclick_object_trigger"))(regs);
        fprintf(out, "R click %u %u -> %d (frame %d)\n", a, b, r, g_frames); }
    else if (!strcmp(l->op, "snap")) snap();
    else if (!strcmp(l->op, "diff")) diff(l->args);
    else if (!strcmp(l->op, "quit")) { fprintf(out, "END\n"); fclose(out); _exit(0); }
    else fprintf(out, "!! bad op %s\n", l->op);
    fflush(out);
}
static void frame(const struct roth_api_v1 *api)
{
    if (api->game_ram->u8(0x7674a) != 1) return;
    g_frames++;
    sigset_t s, old; sigemptyset(&s); sigaddset(&s, SIGALRM); sigprocmask(SIG_BLOCK, &s, &old);
    while (g_pc < g_n && g_script[g_pc].frame <= g_frames) run(&g_script[g_pc++]);
    sigprocmask(SIG_SETMASK, &old, 0);
}
static void on_load(const struct roth_api_v1 *api)
{
    (void)api; const char *s;
    out = fopen((s = getenv("CANDLE_OUT")) ? s : "/tmp/candleprobe.out", "w");
    if ((s = getenv("CANDLE_SYMS"))) { FILE *f = fopen(s, "r"); char n[64]; unsigned a2; char t;
        while (f && nsyms < 1024 && fscanf(f, "%x %c %63s", &a2, &t, n) == 3) { strcpy(syms[nsyms].name, n); syms[nsyms++].addr = a2; } if (f) fclose(f); }
    if ((s = getenv("CANDLE_SCRIPT"))) { FILE *f = fopen(s, "r"); char line[256];
        while (f && g_n < 512 && fgets(line, sizeof line, f)) { struct line *l = &g_script[g_n]; l->args[0] = 0;
            if (sscanf(line, "%d %15s %127[^\n]", &l->frame, l->op, l->args) >= 2) g_n++; } if (f) fclose(f); }
}
static uint32_t skip_gdv(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t a, uint32_t b, uint32_t d, uint32_t e)
{ (void)c;(void)api;(void)a;(void)b;(void)d;(void)e; return 0; }
static uint32_t skip_rec_gdv(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t a)
{ (void)c;(void)api;(void)a; return 0; }
static uint32_t menu_newgame(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t a)
{ (void)c;(void)api;(void)a; return 0xa; }
static void reg(const struct roth_api_v1 *api, struct roth_registrar_v1 *r)
{
    (void)api;
    roth_override(r, ROTH_FN_play_gdv_cutscene, skip_gdv, 100);
    roth_override(r, ROTH_FN_play_record_gdv_cutscene, skip_rec_gdv, 100);
    roth_override(r, ROTH_FN_run_main_menu, menu_newgame, 100);
}
static const struct roth_plugin_info_v1 INFO = {
    .abi_major = ROTH_ABI_MAJOR, .abi_minor = ROTH_ABI_MINOR, .struct_size = sizeof(struct roth_plugin_info_v1),
    .id = "io.github.vrealms.candleprobe", .name = "candle probe", .version = "0.1",
    .sdk_req_major = ROTH_SDK_MAJOR, .sdk_req_minor = ROTH_SDK_MINOR, .api_use = ROTH_API_USE_ENGINE,
    .on_load = on_load, .on_frame_game = frame, .on_register_overrides = reg,
};
ROTH_PLUGIN_EXPORT const struct roth_plugin_info_v1 *roth_plugin_query_v1(void) { return &INFO; }
