/* cmdprobe.c -- probe mod for the ROTH.C oracle build (tools/cmdcore_test/run_diff.sh builds and runs it).
 * At gameplay frame CMDPROBE_AT it runs CMDPROBE_SCRIPT against the ORIGINAL engine: calls RAW-command
 * handlers and chains by symbol and prints byte diffs of the command table, geometry and objects, the
 * chain globals and the particle pool to CMDPROBE_OUT, then exits. Never part of the engine build. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "roth_sdk.h"
#define D 0x400000u
#define M8(va)  (*(volatile uint8_t  *)(uintptr_t)((va) + D))
#define M16(va) (*(volatile uint16_t *)(uintptr_t)((va) + D))
#define M32(va) (*(volatile uint32_t *)(uintptr_t)((va) + D))
#define H8(p)   (*(volatile uint8_t  *)(uintptr_t)(p))
#define H16(p)  (*(volatile uint16_t *)(uintptr_t)(p))
#define H32(p)  (*(volatile uint32_t *)(uintptr_t)(p))
static int g_at = 200, g_frames, g_done;
static FILE *out;
static struct { char name[64]; uint32_t addr; } syms[512]; static int nsyms;
static uint32_t sym(const char *n) { for (int i = 0; i < nsyms; i++) if (!strcmp(syms[i].name, n)) return syms[i].addr;
    fprintf(out, "!! no symbol %s\n", n); fflush(out); return 0; }
static uint32_t hdr(void) { return M32(0x85c30); }
static uint32_t ncmd(void) { uint32_t h = hdr(); return (h && H16(h) == 0x7533) ? H16(h + 6) : 0; }
static uint32_t recptr(uint32_t idx) { if (idx == 0 || idx > ncmd()) return 0; uint32_t arr = H32(M32(0x8a0d8)); return H32(arr + (idx - 1) * 4); }
static uint32_t recidx(uint32_t p) { if (!p) return 0; uint32_t n = ncmd(); for (uint32_t i = 1; i <= n; i++) if (recptr(i) == p) return i; return 0xffff; }
static uint8_t *snapC, *snapG, *snapO; static uint32_t szC, szG, szO;
static uint32_t geomsize(void) { return H16(M32(0x90aa8)); }
static uint32_t objsize(void) { return (uint32_t)H16(M32(0x90aa4)) + 2; }
static uint32_t cmdsize(void) { uint32_t h = hdr(); if (!h) return 0; uint32_t n = ncmd(); if (!n) return 0; uint32_t last = recptr(n); return last + H16(last) - h; }
static void snap(void)
{
    free(snapC); free(snapG); free(snapO);
    szC = cmdsize(); szG = geomsize(); szO = objsize();
    snapC = malloc(szC + 1); snapG = malloc(szG + 1); snapO = malloc(szO + 1);
    memcpy(snapC, (void *)(uintptr_t)hdr(), szC); memcpy(snapG, (void *)(uintptr_t)M32(0x90aa8), szG); memcpy(snapO, (void *)(uintptr_t)M32(0x90aa4), szO);
}
static int tri(uint32_t v) { return v == 0 ? 0 : (v & 1) ? 1 : 2; }
static void globals(void)
{
    uint32_t q = M32(0x71f40), qn = H32(q);
    fprintf(out, "S int=%u mode=%u delta=%u pend=%u res=%d next=%u ang=%u hp=%d spd=%u crng=%08x prng=%04x flash=%d q=%u",
        M8(0x8a268), M32(0x8a104), M32(0x8a108), recidx(M32(0x8a0dc)), tri(M32(0x8a138)), M32(0x8a0cc), M16(0x90a8a), (int32_t)M32(0x8a0f0),
        M32(0x89f54), M32(0x71f48), M16(0x7276c), (int32_t)M32(0x89f3b), qn);
    for (uint32_t i = 0; i < qn && i < 16; i++) fprintf(out, " [%u,%x]", H32(q + 4 + i * 8) & 0xffff, H32(q + 8 + i * 8));
    fprintf(out, "\n");
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
    else {
        uint32_t nsec = H16(g + H16(g + 4) - 2);
        for (uint32_t s = 0; s < nsec; s++) { uint32_t off = H16(ob + 2 + s * 2); if (!off) continue;
            uint32_t cnt = H8(ob + off);
            for (uint32_t j = 0; j < cnt; j++) for (uint32_t k = 0; k < 16; k++) { uint32_t a = off + 2 + j * 16 + k;
                if (a < szO && snapO[a] != H8(ob + a)) fprintf(out, "O %u %u %u %02x %02x\n", s, j, k, snapO[a], H8(ob + a)); } }
    }
    globals(); snap(); fflush(out);
}
static void parts(void)
{
    for (int i = 0; i < 0x20; i++) { uint32_t p = 0x911e4 + D + i * 0x34; if (!(H8(p + 0x30) & 1)) continue;
        fprintf(out, "P %d sec=%u x=%d y=%d tex=%04x rot=%u fl=%02x lt=%02x rt=%02x z=%d zv=%d xv=%d yv=%d xa=%d ya=%d za=%d st=%u life=%u\n", i, H32(p + 4),
            (int16_t)H16(p + 8), (int16_t)H16(p + 0xa), H16(p + 0xc), H8(p + 0xe), H8(p + 0xf), H8(p + 0x10), H8(p + 0x11), (int16_t)H16(p + 0x12),
            (int32_t)H32(p + 0x18), (int32_t)H32(p + 0x1c), (int32_t)H32(p + 0x20), (int32_t)H32(p + 0x24), (int32_t)H32(p + 0x28), (int32_t)H32(p + 0x2c), H8(p + 0x31), H16(p + 0x32)); }
    fprintf(out, "P end\n");
}
typedef int32_t (*fn1)(uint32_t); typedef uint32_t (*fn0)(void); typedef uint32_t (*fnk)(uint16_t);
static void run(void)
{
    const char *sp = getenv("CMDPROBE_SCRIPT"); FILE *f = sp ? fopen(sp, "r") : 0; if (!f) { fprintf(out, "!! no script\n"); return; }
    fprintf(out, "MAP ncmd=%u geom=%u\nINFO obj=%u fts=%u\n", ncmd(), geomsize(), objsize(), M32(0x85324));
    char line[256], a[64], b[64]; snap();
    while (fgets(line, sizeof line, f)) {
        unsigned x = 0, y = 0, z = 0; a[0] = b[0] = 0;
        if (sscanf(line, "%63s", a) != 1 || a[0] == '#') continue;
        if (!strcmp(a, "h")) { sscanf(line, "%*s %63s %u", b, &x); uint32_t fp = sym(b), r = recptr(x); if (fp && r) { int32_t ret = ((fn1)(uintptr_t)fp)(r); fprintf(out, "R h %s %u -> %d\n", b, x, ret == 0 ? 0 : (ret & 1) ? -1 : 2); } else fprintf(out, "R h %s %u -> BAD\n", b, x); }
        else if (!strcmp(a, "chain")) { sscanf(line, "%*s %u", &x); M32(0x8a0c8) = 0; ((fnk)(uintptr_t)sym("reset_command_chain_state"))((uint16_t)x); fprintf(out, "R chain %u\n", x); }
        else if (!strcmp(a, "callv")) { sscanf(line, "%*s %63s %u", b, &x); uint32_t fp = sym(b); if (fp) ((fn1)(uintptr_t)fp)(x); fprintf(out, "R callv %s %u\n", b, x); }
        else if (!strcmp(a, "callr")) { sscanf(line, "%*s %63s %u", b, &x); uint32_t fp = sym(b); int32_t r = fp ? ((fn1)(uintptr_t)fp)(x) : -99; fprintf(out, "R callr %s %u -> %d qc=%d topic=%x busy=%d\n", b, x, r, (int32_t)M32(0x81e3e), M32(0x81eb2), (int32_t)M32(0x83aea)); }
        else if (!strcmp(a, "dbpeek")) { sscanf(line, "%*s %x %u", &x, &y); uint32_t base = M32(0x81e1c); fprintf(out, "B %x", x); for (uint32_t i = 0; i < y; i++) fprintf(out, " %08x", H32(base + x + i * 4)); fprintf(out, "\n"); }
        else if (!strcmp(a, "clearq")) { H32(M32(0x71f40)) = 0; }
        else if (!strcmp(a, "drain")) { ((fn0)(uintptr_t)sym("process_deferred_command_queue"))(); fprintf(out, "R drain\n"); }
        else if (!strcmp(a, "call0")) { sscanf(line, "%*s %63s", b); uint32_t fp = sym(b); if (fp) ((fn0)(uintptr_t)fp)(); fprintf(out, "R call0 %s\n", b); }
        else if (!strcmp(a, "set8")) { sscanf(line, "%*s %x %i", &x, (int *)&y); M8(x) = (uint8_t)y; }
        else if (!strcmp(a, "set16")) { sscanf(line, "%*s %x %i", &x, (int *)&y); M16(x) = (uint16_t)y; }
        else if (!strcmp(a, "set32")) { sscanf(line, "%*s %x %i", &x, (int *)&y); M32(x) = y; }
        else if (!strcmp(a, "rec8")) { sscanf(line, "%*s %u %u %i", &x, &y, (int *)&z); uint32_t r = recptr(x); if (r) H8(r + y) = (uint8_t)z; }
        else if (!strcmp(a, "rec16")) { sscanf(line, "%*s %u %u %i", &x, &y, (int *)&z); uint32_t r = recptr(x); if (r) H16(r + y) = (uint16_t)z; }
        else if (!strcmp(a, "pend")) { sscanf(line, "%*s %u", &x); M32(0x8a0dc) = recptr(x); }
        else if (!strcmp(a, "src")) { sscanf(line, "%*s %u %u", &x, &y);   /* src <sector> <sub>, or src 65535 = none */
            if (x == 65535) M32(0x8a100) = 0; else { uint32_t ob = M32(0x90aa4), off = H16(ob + 2 + x * 2); M32(0x8a100) = off ? ob + off + 2 + y * 16 : 0; } }
        else if (!strcmp(a, "face")) { sscanf(line, "%*s %u", &x);          /* face <faceOffset>, 0 = none */
            if (!x) { M32(0x8a0f8) = 0; M32(0x8a0fc) = 0; } else { uint32_t g = M32(0x90aa8); M32(0x8a0fc) = g + x; M32(0x8a0f8) = g + H16(g + x + 4); } }
        else if (!strcmp(a, "dump16")) { sscanf(line, "%*s %x %u", &x, &y); fprintf(out, "W %x", x); for (uint32_t i = 0; i < y; i++) fprintf(out, " %d", (int16_t)M16(x + i * 2)); fprintf(out, "\n"); }
        else if (!strcmp(a, "dtab")) { uint32_t base = M32(0x81e1c), tbl = M32(0x81e24); fprintf(out, "T base=%x tbl-base=%x count=%d e0=%08x e1=%08x\n", base, tbl - base, base ? (int32_t)H32(base + 0x18) : -1, tbl ? H32(tbl) : 0, tbl ? H32(tbl + 4) : 0); }
        else if (!strcmp(a, "snap")) snap();
        else if (!strcmp(a, "diff")) { sscanf(line, "%*s %63s", b); diff(b); }
        else if (!strcmp(a, "parts")) parts();
        else if (!strcmp(a, "clearparts")) { for (int i = 0; i < 0x20; i++) H8(0x911e4 + D + i * 0x34 + 0x30) = 0; M32(0x91864) = 0; }
        else if (!strcmp(a, "flag")) { sscanf(line, "%*s %u", &x); uint32_t bm = M32(0x81e28); fprintf(out, "F %u %u\n", x, bm ? (H8(bm + (x >> 3)) >> (x & 7)) & 1 : 9); }
        else fprintf(out, "!! bad line %s", line);
        fflush(out);
    }
    fclose(f);
}
static void frame(const struct roth_api_v1 *api)
{
    if (api->game_ram->u8(0x7674a) != 1) return;
    g_frames++;
    if (g_done || g_frames < g_at) return;
    g_done = 1; run(); fprintf(out, "END\n"); fclose(out); _exit(0);
}
static void on_load(const struct roth_api_v1 *api)
{
    (void)api; const char *s;
    if ((s = getenv("CMDPROBE_AT"))) g_at = atoi(s);
    out = fopen((s = getenv("CMDPROBE_OUT")) ? s : "/tmp/cmdw/probe.out", "w");
    if ((s = getenv("CMDPROBE_SYMS"))) { FILE *f = fopen(s, "r"); char n[64]; unsigned a2; char t;
        while (f && nsyms < 512 && fscanf(f, "%x %c %63s", &a2, &t, n) == 3) { strcpy(syms[nsyms].name, n); syms[nsyms++].addr = a2; } if (f) fclose(f); }
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
    .id = "io.github.vrealms.cmdprobe", .name = "command probe", .version = "0.1",
    .sdk_req_major = ROTH_SDK_MAJOR, .sdk_req_minor = ROTH_SDK_MINOR, .api_use = ROTH_API_USE_ENGINE,
    .on_load = on_load, .on_frame_game = frame, .on_register_overrides = reg,
};
ROTH_PLUGIN_EXPORT const struct roth_plugin_info_v1 *roth_plugin_query_v1(void) { return &INFO; }
