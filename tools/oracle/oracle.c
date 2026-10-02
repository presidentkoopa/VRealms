/* oracle.c — ROTH.C capture mod for VRealms.
 * Boots straight into gameplay (no GDV, menu -> new game), paints every plain DAS image with a
 * per-texel code, makes all shade/remap tables identity, then dumps the indexed frame.
 * Env: ORACLE_PASS=0..4 (which 6-bit slice of the code to paint), ORACLE_OUT=path,
 *      ORACLE_FRAMES=n (gameplay frames before capture), ORACLE_PAINT=0 to capture real art,
 *      ORACLE_POSE="x y angle" optional camera override (applied every frame). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "roth_sdk.h"

static int g_shade = 0;
static int g_pass = 0, g_paint = 1, g_target = 120, g_frames, g_done;
static const char *g_out = "/tmp/oracle_frame.bin";
static int g_pitch_set = 0, g_pitch = 0;
static FILE *g_phaselog;
static int g_pose = 0; static int g_px, g_py, g_pa, g_psec = -1, g_pz = -9999;
static uint8_t g_cap[640 * 480]; static uint32_t g_w, g_h; static volatile int g_have;
static FILE *g_texlog;
static uint32_t g_blkof[0x2000];

#define U32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define U16(a) (*(volatile uint16_t *)(uintptr_t)(a))
#define U8(a)  (*(volatile uint8_t  *)(uintptr_t)(a))

static uint32_t skip_gdv(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t a, uint32_t b, uint32_t d, uint32_t e)
{ (void)c;(void)api;(void)a;(void)b;(void)d;(void)e; return 0; }
static uint32_t skip_rec_gdv(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t a)
{ (void)c;(void)api;(void)a; return 0; }
static uint32_t menu_newgame(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t a)
{ (void)c;(void)api;(void)a; return 0xa; }

static int load_block(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t idx)
{
    uint32_t before = api->game_ram->u32(0x8c738);
    int cf = roth_next_load_das_block_for_fat_index(c, idx);
    if (cf) return cf;
    uint32_t fat = idx & 0xffff;
    /* Only paint a block this call actually loaded: placeholder / giveup paths return
     * success without touching the current-block handle, which would repaint the
     * previous block under this id. The status byte must name a cache slot (< 0xfc). */
    { uint8_t st = api->game_ram->u8(0x86d30 + (fat & 0x1fff) * 2);
      if (st >= 0xfc) return cf; }
    if (api->game_ram->u32(0x8c738) == before) return cf;
    uint32_t handle = api->game_ram->u32(0x8c738);
    if (!handle) return cf;
    uint32_t blk = U32(handle);
    if (!blk) return cf;
    g_blkof[fat & 0x1fff] = handle;
    uint8_t mod = U8(blk + 0xa), it = U8(blk + 0xb);
    uint16_t w = U16(blk + 0xc), h = U16(blk + 0xe);
    if (g_texlog) { uint32_t fb = api->game_ram->u32(0x90a38); uint32_t e = fb + (fat & 0x1fff) * 8;
        fprintf(g_texlog, "%u %u %u %u %u fat_off %u fat_sz %u f1 %02x blk %08x\n", fat, mod, it, w, h, fb ? U32(e) : 0, fb ? U16(e + 4) : 0, fb ? U8(e + 6) : 0, blk); }
    if (!g_paint) return cf;
    if (it & 0x81) return cf;            /* animated or 3D object */
    if (mod & 0x40) return cf;           /* image pack */
    if (w == 0 || h == 0 || w > 1024 || h > 1024) return cf;
    uint8_t *px = (uint8_t *)(uintptr_t)(blk + 0x10);
    for (uint32_t r = 0; r < h; r++)
        for (uint32_t col = 0; col < w; col++) {
            uint32_t word = (col & 0xff) | ((r & 0xff) << 8) | ((fat & 0x1fff) << 16);
            px[r * w + col] = (uint8_t)(((word >> (6 * g_pass)) & 63) + 1);
        }
    return cf;
}

static void ident(uint32_t base, uint32_t bytes)
{
    if (!base) return;
    for (uint32_t i = 0; i < bytes; i++) U8(base + i) = (uint8_t)i;
}


/* ---- surface tags: which surface wrote each pixel ---- */
static uint32_t g_tag[640 * 480], g_tagcap[640 * 480];
static uint32_t g_cur_tag;
static const struct roth_api_v1 *g_api;
static inline void tag_run(uint32_t off, uint32_t n, uint32_t step)
{
    if (!g_cur_tag) return;
    for (uint32_t k = 0; k < n; k++) { uint32_t o = off + k * step; if (o < 640 * 480) g_tag[o] = g_cur_tag; }
}
static uint32_t pitch(void) { uint32_t p = g_api->game_ram->u32(0x85498); return p ? p : 640; }
static uint32_t mk_tag(uint32_t kind)
{
    uint32_t sec = g_api->game_ram->u16(0x8528c);
    uint32_t fill = g_api->game_ram->u16(0x84f2e) & 0xff;
    return (kind << 28) | (fill << 16) | (sec & 0xffff);
}
static void w_draw_scaled(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t a, uint32_t b, uint32_t d, uint32_t e, uint16_t f, uint16_t g)
{ (void)api; uint32_t save = g_cur_tag; g_cur_tag = mk_tag(1); roth_next_draw_scaled_sprite_spans(c, a, b, d, e, f, g); g_cur_tag = save; }
static uint32_t g_idpairs[4096][2]; static int g_nidpairs;
static void w_raster(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t a, uint32_t b, uint32_t d, uint32_t e, uint32_t f, uint16_t g, uint16_t h, uint16_t i, uint16_t j)
{
    uint32_t given = U16(a + 0xc);
    uint32_t save = g_cur_tag; g_cur_tag = mk_tag(2); roth_next_rasterize_world_spans_scanline(c, a, b, d, e, f, g, h, i, j); g_cur_tag = save;
    uint32_t got = api->game_ram->u32(0x90a78) & 0xffff;
    if (given != got) { int k; for (k = 0; k < g_nidpairs; k++) if (g_idpairs[k][0] == given && g_idpairs[k][1] == got) break;
        if (k == g_nidpairs && g_nidpairs < 4096) { g_idpairs[k][0] = given; g_idpairs[k][1] = got; g_nidpairs++; } }
}
#define HWRAP3(name) static void w_##name(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t cx, uint32_t di, const uint8_t *p) \
    { (void)api; tag_run(di, cx & 0xffff, 1); roth_next_##name(c, cx, di, p); }
HWRAP3(render_sprite_span_tex_3a220)
HWRAP3(render_span_texmap_3a368)
HWRAP3(render_sprite_span_tex_3a100)
static void w_render_sprite_span_tex_shaded_3a4f8(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t cx, uint32_t di, const uint8_t *p, const uint8_t *q)
{ (void)api; tag_run(di, cx & 0xffff, 1); roth_next_render_sprite_span_tex_shaded_3a4f8(c, cx, di, p, q); }
static void w_render_sprite_span_tex_blend_3a700(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t cx, uint32_t di, const uint8_t *p, const uint8_t *q)
{ (void)api; tag_run(di, cx & 0xffff, 1); roth_next_render_sprite_span_tex_blend_3a700(c, cx, di, p, q); }
static void w_render_world_span_390ac(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t a, uint32_t b, uint32_t cx, uint32_t bx, uint32_t si, uint32_t di, const uint8_t *g, const uint8_t *e)
{ (void)api; uint32_t n = cx & 0xffff; if (!n) n = 1; tag_run(di, n, pitch()); roth_next_render_world_span_390ac(c, a, b, cx, bx, si, di, g, e); }
static void w_render_world_span_wrapped_391d0(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t a, uint32_t b, uint32_t cx, uint32_t bx, uint32_t si, uint32_t di, uint32_t m, const uint8_t *g, const uint8_t *e)
{ (void)api; uint32_t n = cx & 0xffff; if (!n) n = 1; tag_run(di, n, pitch()); roth_next_render_world_span_wrapped_391d0(c, a, b, cx, bx, si, di, m, g, e); }

static FILE *g_shlog; static FILE *g_cmdlog;
static uint16_t w_project(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t esi, uint16_t ax)
{
    uint16_t r = roth_next_project_floorceil_edge_texcoord(c, esi, ax);
    if (g_shlog && g_frames >= g_target) {
        const struct roth_game_ram_api_v1 *g = api->game_ram;
        fprintf(g_shlog, "V %d %d %u ax %04x -> %04x sec %u fl %04x alt %04x bias %d cap %d L %d %d k %u %u eb %d lt %u lvl %u\n",
            (int16_t)U16(esi), (int16_t)U16(esi + 2), U16(esi + 4), ax, r, g->u16(0x8528c), g->u16(0x9093c), g->u16(0x909ae),
            (int16_t)g->u16(0x90a1e), (int16_t)g->u16(0x90a20), (int16_t)g->u16(0x90990), (int16_t)g->u16(0x90992),
            g->u16(0x90994), g->u16(0x90996), (int16_t)g->u16(0x3b794), g->u8(0x90970), g->u16(0x90c1a));
    }
    return r;
}

static int32_t w_exec_chain(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t edi)
{
    if (g_cmdlog) {
        uint32_t arr = U32(api->game_ram->u32(0x8a0d8));
        fprintf(g_cmdlog, "F %d chain", g_frames);
        uint16_t ax = U16(edi + 4); int n = 0;
        while (ax && n++ < 64) { uint32_t rec = U32(arr + (ax - 1) * 4);
            fprintf(g_cmdlog, " [%u op %02x fl %02x k %u a %u c %d]", ax, U8(rec + 3) & 0x7f, U8(rec + 6), U16(rec + 8), U16(rec + 0xa), (int8_t)U8(rec + 0xc));
            if (U8(rec + 3) == 0x12) break; ax = U16(rec + 4); }
        fprintf(g_cmdlog, "\n"); fflush(g_cmdlog);
    }
    return roth_next_execute_command_chain(c, edi);
}
static int32_t w_cmd_flash_lights(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t rec)
{ (void)api; if (g_cmdlog) { fprintf(g_cmdlog, "F %d cmd_flash_lights op %02x fl %02x m %02x k %u a %u c %d\n", g_frames, U8(rec+3)&0x7f, U8(rec+6), U8(rec+2), U16(rec+8), U16(rec+0xa), (int8_t)U8(rec+0xc)); fflush(g_cmdlog); }
  return roth_next_cmd_flash_lights(c, rec); }
static int32_t w_cmd_change_lighting(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t rec)
{ (void)api; if (g_cmdlog) { fprintf(g_cmdlog, "F %d cmd_change_lighting op %02x fl %02x m %02x k %u a %u c %d\n", g_frames, U8(rec+3)&0x7f, U8(rec+6), U8(rec+2), U16(rec+8), U16(rec+0xa), (int8_t)U8(rec+0xc)); fflush(g_cmdlog); }
  return roth_next_cmd_change_lighting(c, rec); }
static int32_t w_cmd_light_switch(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t rec)
{ (void)api; if (g_cmdlog) { fprintf(g_cmdlog, "F %d cmd_light_switch op %02x fl %02x m %02x k %u a %u c %d\n", g_frames, U8(rec+3)&0x7f, U8(rec+6), U8(rec+2), U16(rec+8), U16(rec+0xa), (int8_t)U8(rec+0xc)); fflush(g_cmdlog); }
  return roth_next_cmd_light_switch(c, rec); }
static int32_t w_tick_change_lighting(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t rec)
{ (void)api; if (g_cmdlog) { fprintf(g_cmdlog, "F %d tick_change_lighting op %02x fl %02x m %02x k %u a %u c %d\n", g_frames, U8(rec+3)&0x7f, U8(rec+6), U8(rec+2), U16(rec+8), U16(rec+0xa), (int8_t)U8(rec+0xc)); fflush(g_cmdlog); }
  return roth_next_tick_change_lighting(c, rec); }
static int32_t w_tick_light_switch(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t rec)
{ (void)api; if (g_cmdlog) { fprintf(g_cmdlog, "F %d tick_light_switch op %02x fl %02x m %02x k %u a %u c %d\n", g_frames, U8(rec+3)&0x7f, U8(rec+6), U8(rec+2), U16(rec+8), U16(rec+0xa), (int8_t)U8(rec+0xc)); fflush(g_cmdlog); }
  return roth_next_tick_light_switch(c, rec); }
static int32_t w_apply_light_delta_to_record_list(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t list, uint32_t d)
{ (void)api; if (g_cmdlog) { fprintf(g_cmdlog, "F %d apply_delta %d\n", g_frames, (int)(int16_t)d); fflush(g_cmdlog); }
  return roth_next_apply_light_delta_to_record_list(c, list, d); }
static FILE *g_walllog;
static void w_wallspans(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t a, uint32_t b, uint32_t d, uint32_t e)
{
    const struct roth_game_ram_api_v1 *g = api->game_ram;
    roth_next_draw_world_surface_spans(c, a, b, d, e);
    if (g_walllog && g_frames >= g_target) {
        fprintf(g_walllog, "W xl %d xr %d ytl %d ybl %d ytr %d ybr %d zl %u zr %u cxl %u cxr %u clip %d %d sh %04x %04x %04x %04x fl %04x alt %04x lt %u bias %d vb %d %d %d %d\n",
            (int16_t)g->u16(0x90958), (int16_t)g->u16(0x90960), (int16_t)g->u16(0x9095a), (int16_t)g->u16(0x9095c), (int16_t)g->u16(0x90962), (int16_t)g->u16(0x90964),
            g->u16(0x8a316 + 0x14), g->u16(0x8a316 + 0x16), g->u16(0x8a316 + 0xa), g->u16(0x8a316 + 0xc),
            (int16_t)g->u16(0x8a2f0 + 0x1e), (int16_t)g->u16(0x8a2f0 + 0x1c),
            g->u16(0x90948), g->u16(0x9094a), g->u16(0x9094c), g->u16(0x9094e), g->u16(0x9093c), g->u16(0x909ae), g->u8(0x90970), (int16_t)g->u16(0x90a1e),
            (int16_t)g->u16(0x9096a), (int16_t)g->u16(0x90968), (int16_t)g->u16(0x9096e), (int16_t)g->u16(0x9096c));
        fflush(g_walllog);
    }
}
static void w_render_world_col_shaded_gs_37ec8(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t p1, uint32_t p2, uint32_t p3, uint32_t ebx, uint32_t esi, uint32_t edi, const uint8_t *gs)
{ (void)api; uint32_t off = edi; if (off >= 640*480) { uint32_t fb = g_api->game_ram->u32(0x90a98); off = edi - fb; } uint32_t save = g_cur_tag;
  g_cur_tag = (0xCu << 28) | (1u << 20) | (((ebx) >> 8) & 0xff) << 8 | (g_api->game_ram->u8(0x90970)); 
  if (off < 640*480) tag_run(off, (p3) & 0xffff ? (p3) & 0xffff : 1, pitch()); g_cur_tag = save;
  roth_next_render_world_col_shaded_gs_37ec8(c, p1, p2, p3, ebx, esi, edi, gs); }
static void w_render_world_col_shaded_masked_gs_38198(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t p1, uint32_t p2, uint32_t p3, uint32_t ebx, uint32_t esi, uint32_t edi, const uint8_t *gs)
{ (void)api; uint32_t off = edi; if (off >= 640*480) { uint32_t fb = g_api->game_ram->u32(0x90a98); off = edi - fb; } uint32_t save = g_cur_tag;
  g_cur_tag = (0xCu << 28) | (2u << 20) | (((ebx) >> 8) & 0xff) << 8 | (g_api->game_ram->u8(0x90970)); 
  if (off < 640*480) tag_run(off, (p3) & 0xffff ? (p3) & 0xffff : 1, pitch()); g_cur_tag = save;
  roth_next_render_world_col_shaded_masked_gs_38198(c, p1, p2, p3, ebx, esi, edi, gs); }
static void w_render_world_col_shaded_gs_wrapped_38434(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t p1, uint32_t p2, uint32_t p3, uint32_t ebx, uint32_t esi, uint32_t edi, const uint8_t *gs)
{ (void)api; uint32_t off = edi; if (off >= 640*480) { uint32_t fb = g_api->game_ram->u32(0x90a98); off = edi - fb; } uint32_t save = g_cur_tag;
  g_cur_tag = (0xCu << 28) | (3u << 20) | (((ebx) >> 8) & 0xff) << 8 | (g_api->game_ram->u8(0x90970)); 
  if (off < 640*480) tag_run(off, (p3) & 0xffff ? (p3) & 0xffff : 1, pitch()); g_cur_tag = save;
  roth_next_render_world_col_shaded_gs_wrapped_38434(c, p1, p2, p3, ebx, esi, edi, gs); }
static void w_render_world_col_shaded_masked_gs_wrapped_384fc(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t p1, uint32_t p2, uint32_t p3, uint32_t ebx, uint32_t esi, uint32_t edi, const uint8_t *gs)
{ (void)api; uint32_t off = edi; if (off >= 640*480) { uint32_t fb = g_api->game_ram->u32(0x90a98); off = edi - fb; } uint32_t save = g_cur_tag;
  g_cur_tag = (0xCu << 28) | (4u << 20) | (((ebx) >> 8) & 0xff) << 8 | (g_api->game_ram->u8(0x90970)); 
  if (off < 640*480) tag_run(off, (p3) & 0xffff ? (p3) & 0xffff : 1, pitch()); g_cur_tag = save;
  roth_next_render_world_col_shaded_masked_gs_wrapped_384fc(c, p1, p2, p3, ebx, esi, edi, gs); }
static void w_render_world_col_solid_fill_38684(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t ecx, uint32_t ebx, uint32_t edi, const uint8_t *gs)
{ (void)api; uint32_t off = edi; if (off >= 640*480) { uint32_t fb = g_api->game_ram->u32(0x90a98); off = edi - fb; } uint32_t save = g_cur_tag;
  g_cur_tag = (0xCu << 28) | (5u << 20) | (((ebx) >> 8) & 0xff) << 8 | (g_api->game_ram->u8(0x90970)); 
  if (off < 640*480) tag_run(off, (ecx) & 0xffff ? (ecx) & 0xffff : 1, pitch()); g_cur_tag = save;
  roth_next_render_world_col_solid_fill_38684(c, ecx, ebx, edi, gs); }
static void w_render_world_col_solid_gradient_387f0(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t ecx, uint32_t ebx, uint32_t edi, const uint8_t *gs)
{ (void)api; uint32_t off = edi; if (off >= 640*480) { uint32_t fb = g_api->game_ram->u32(0x90a98); off = edi - fb; } uint32_t save = g_cur_tag;
  g_cur_tag = (0xCu << 28) | (6u << 20) | (((ebx) >> 8) & 0xff) << 8 | (g_api->game_ram->u8(0x90970)); 
  if (off < 640*480) tag_run(off, (ecx) & 0xffff ? (ecx) & 0xffff : 1, pitch()); g_cur_tag = save;
  roth_next_render_world_col_solid_gradient_387f0(c, ecx, ebx, edi, gs); }
static void w_render_world_col_unshaded_opaque_37cec(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t p1, uint32_t ecx, uint32_t p2, uint32_t ebx, uint32_t esi, uint32_t edi)
{ (void)api; uint32_t off = edi; if (off >= 640*480) { uint32_t fb = g_api->game_ram->u32(0x90a98); off = edi - fb; } uint32_t save = g_cur_tag;
  g_cur_tag = (0xCu << 28) | (7u << 20) | (((ebx) >> 8) & 0xff) << 8 | (g_api->game_ram->u8(0x90970)); 
  if (off < 640*480) tag_run(off, (ecx) & 0xffff ? (ecx) & 0xffff : 1, pitch()); g_cur_tag = save;
  roth_next_render_world_col_unshaded_opaque_37cec(c, p1, ecx, p2, ebx, esi, edi); }
static void w_render_world_col_unshaded_opaque_37fac(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t p1, uint32_t p2, uint32_t p3, uint32_t ebx, uint32_t esi, uint32_t edi)
{ (void)api; uint32_t off = edi; if (off >= 640*480) { uint32_t fb = g_api->game_ram->u32(0x90a98); off = edi - fb; } uint32_t save = g_cur_tag;
  g_cur_tag = (0xCu << 28) | (8u << 20) | (((ebx) >> 8) & 0xff) << 8 | (g_api->game_ram->u8(0x90970)); 
  if (off < 640*480) tag_run(off, (p3) & 0xffff ? (p3) & 0xffff : 1, pitch()); g_cur_tag = save;
  roth_next_render_world_col_unshaded_opaque_37fac(c, p1, p2, p3, ebx, esi, edi); }
static void w_render_world_col_unshaded_masked_38964(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t p1, uint32_t ecx, uint32_t p2, uint32_t ebx, uint32_t esi, uint32_t edi)
{ (void)api; uint32_t off = edi; if (off >= 640*480) { uint32_t fb = g_api->game_ram->u32(0x90a98); off = edi - fb; } uint32_t save = g_cur_tag;
  g_cur_tag = (0xCu << 28) | (9u << 20) | (((ebx) >> 8) & 0xff) << 8 | (g_api->game_ram->u8(0x90970)); 
  if (off < 640*480) tag_run(off, (ecx) & 0xffff ? (ecx) & 0xffff : 1, pitch()); g_cur_tag = save;
  roth_next_render_world_col_unshaded_masked_38964(c, p1, ecx, p2, ebx, esi, edi); }
static void w_render_world_col_unshaded_masked_2axis_38288(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t p1, uint32_t p2, uint32_t p3, uint32_t ebx, uint32_t esi, uint32_t edi)
{ (void)api; uint32_t off = edi; if (off >= 640*480) { uint32_t fb = g_api->game_ram->u32(0x90a98); off = edi - fb; } uint32_t save = g_cur_tag;
  g_cur_tag = (0xCu << 28) | (10u << 20) | (((ebx) >> 8) & 0xff) << 8 | (g_api->game_ram->u8(0x90970)); 
  if (off < 640*480) tag_run(off, (p3) & 0xffff ? (p3) & 0xffff : 1, pitch()); g_cur_tag = save;
  roth_next_render_world_col_unshaded_masked_2axis_38288(c, p1, p2, p3, ebx, esi, edi); }
static void frame(const struct roth_api_v1 *api)
{
    if (g_done) return;
    uint8_t mode = api->game_ram->u8(0x7674a);
    if (mode != 1) return;
    g_frames++;
    if (g_shade) {   /* shade probe: every colormap row r maps all texels to r */
        uint32_t t[3] = { api->game_ram->u32(0x85d08), api->game_ram->u32(0x86d28), api->game_ram->u32(0x86d1c) };
        uint32_t n[3] = { 0x10000, 0x2000, 0x2000 };
        for (int k = 0; k < 3; k++) if (t[k]) for (uint32_t i = 0; i < n[k]; i++) U8(t[k] + i) = (uint8_t)(i >> 8);
    } else if (g_paint) {
        ident(api->game_ram->u32(0x85d08), 0x10000);
        ident(api->game_ram->u32(0x86d28), 0x2000);
        ident(api->game_ram->u32(0x86d1c), 0x2000);
        ident(api->game_ram->u32(0x86d18), 0x200);
    }
    if (g_phaselog) fprintf(g_phaselog, "%d %u %u %u\n", g_frames, api->game_ram->u8(0x8a355), api->game_ram->u16(0x85328), api->game_ram->u16(0x90bcc));
    if (g_pitch_set) api->game_ram->set_u32(0x90a74, (uint32_t)g_pitch);   /* g_view_pitch, clamped to +-0x7e by the game */
    if (g_pose) {
        api->game_ram->set_u16(0x90a8e, (uint16_t)g_px);
        api->game_ram->set_u16(0x90a96, (uint16_t)g_py);
        api->game_ram->set_u16(0x90a8a, (uint16_t)g_pa);
        if (g_psec >= 0) api->game_ram->set_u16(0x90c12, (uint16_t)g_psec);
        if (g_pz != -9999) api->game_ram->set_u16(0x90a92, (uint16_t)g_pz);
    }
    if (g_frames == g_target) g_have = 1;            /* ask the compositor for the next frame */
    if (g_have == 2) {
        FILE *f = fopen(g_out, "wb");
        if (f) {
            uint32_t hdr[8] = { g_w, g_h, (uint32_t)g_pass,
                api->game_ram->u16(0x90a8e), api->game_ram->u16(0x90a96), api->game_ram->u16(0x90a92),
                api->game_ram->u16(0x90a8a), api->game_ram->u16(0x90c12) };
            fwrite(hdr, 4, 8, f); fwrite(g_cap, 1, g_w * g_h, f); fwrite(g_tagcap, 4, g_w * g_h, f); fclose(f);
        }
        fprintf(stderr, "[oracle] captured pass %d -> %s (%ux%u) pos %u,%u z %u ang %u sec %u\n", g_pass, g_out, g_w, g_h,
            api->game_ram->u16(0x90a8e), api->game_ram->u16(0x90a96), api->game_ram->u16(0x90a92),
            api->game_ram->u16(0x90a8a), api->game_ram->u16(0x90c12));
        { const char *ip = getenv("ORACLE_IDPAIRS"); if (ip) { FILE *pf = fopen(ip, "w"); if (pf) { for (int k = 0; k < g_nidpairs; k++) fprintf(pf, "%u %u\n", g_idpairs[k][0], g_idpairs[k][1]); fclose(pf); } } }
        { const char *cp = getenv("ORACLE_CMP"); if (cp) { int a2, b2; if (sscanf(cp, "%d %d", &a2, &b2) == 2 && g_blkof[a2] && g_blkof[b2]) {
              uint32_t A = U32(g_blkof[a2]), B = U32(g_blkof[b2]); uint32_t w = U16(A + 0xc), h = U16(A + 0xe), diff = 0;
              for (uint32_t k = 0; k < w * h; k++) if (U8(A + 0x10 + k) != U8(B + 0x10 + k)) diff++;
              fprintf(stderr, "[oracle] cmp %d vs %d: %ux%u, %u of %u bytes differ\n", a2, b2, w, h, diff, w * h); } } }
        if (g_texlog) fclose(g_texlog), g_texlog = NULL;
        { const char *rp = getenv("ORACLE_REMAPDUMP"); uint32_t rec = api->game_ram->u32(0x85c30);
          if (rp && rec) { FILE *rf = fopen(rp, "w");
            if (rf) { uint16_t off = U16(rec + 0x3c), cnt = U16(rec + 0x3e);
              fprintf(rf, "magic %04x off %u cnt %u\n", U16(rec), off, cnt);
              for (uint32_t i = 0; i < cnt; i++) { uint32_t ent = U16(rec + off + 2 * i);
                fprintf(rf, "%u flags %02x key %u src %u dst %u w0 %04x w6 %04x\n", ent, U8(rec + ent + 2), U16(rec + ent + 4), U16(rec + ent + 8), U16(rec + ent + 0xa), U16(rec + ent), U16(rec + ent + 6)); }
              fclose(rf); } } }
        { const char *gp = getenv("ORACLE_GEOMDUMP"); uint32_t gb = api->game_ram->u32(0x90aa8);
          if (gp && gb) { FILE *gf = fopen(gp, "wb"); if (gf) { fwrite((void *)(uintptr_t)gb, 1, 0x10000, gf); fclose(gf); } } }
        { const char *tp = getenv("ORACLE_TABDUMP"); if (tp) { FILE *tf = fopen(tp, "wb"); if (tf) {
              uint32_t t[4] = { api->game_ram->u32(0x85d08), api->game_ram->u32(0x86d28), api->game_ram->u32(0x86d1c), api->game_ram->u32(0x86d18) };
              uint32_t n[4] = { 0x10000, 0x2000, 0x2000, 0x200 };
              for (int k = 0; k < 4; k++) { fwrite(&t[k], 4, 1, tf); fwrite(&n[k], 4, 1, tf); if (t[k]) fwrite((void *)(uintptr_t)t[k], 1, n[k], tf); }
              fclose(tf); } } }
        g_done = 1;
        raise(SIGTERM);
    }
}

static void compose(const struct roth_api_v1 *api, uint8_t *pixels, uint32_t w, uint32_t h)
{
    (void)api;
    if (g_have == 1 && w * h <= sizeof g_cap) { memcpy(g_cap, pixels, w * h); memcpy(g_tagcap, g_tag, sizeof g_tag); g_w = w; g_h = h; g_have = 2; }
}

static void reg(const struct roth_api_v1 *api, struct roth_registrar_v1 *r)
{
    (void)api;
    roth_override(r, ROTH_FN_play_gdv_cutscene, skip_gdv, 100);
    roth_override(r, ROTH_FN_play_record_gdv_cutscene, skip_rec_gdv, 100);
    roth_override(r, ROTH_FN_run_main_menu, menu_newgame, 100);
    roth_override(r, ROTH_FN_load_das_block_for_fat_index, load_block, 100);
    roth_override(r, ROTH_FN_draw_scaled_sprite_spans, w_draw_scaled, 100);
    roth_override(r, ROTH_FN_rasterize_world_spans_scanline, w_raster, 100);
    roth_override(r, ROTH_FN_render_sprite_span_tex_3a220, w_render_sprite_span_tex_3a220, 100);
    roth_override(r, ROTH_FN_render_span_texmap_3a368, w_render_span_texmap_3a368, 100);
    roth_override(r, ROTH_FN_render_sprite_span_tex_3a100, w_render_sprite_span_tex_3a100, 100);
    roth_override(r, ROTH_FN_render_sprite_span_tex_shaded_3a4f8, w_render_sprite_span_tex_shaded_3a4f8, 100);
    roth_override(r, ROTH_FN_render_sprite_span_tex_blend_3a700, w_render_sprite_span_tex_blend_3a700, 100);
    roth_override(r, ROTH_FN_render_world_span_390ac, w_render_world_span_390ac, 100);
    roth_override(r, ROTH_FN_project_floorceil_edge_texcoord, w_project, 100);
    roth_override(r, ROTH_FN_execute_command_chain, w_exec_chain, 100);
    roth_override(r, ROTH_FN_draw_world_surface_spans, w_wallspans, 100);
    roth_override(r, ROTH_FN_render_world_col_shaded_gs_37ec8, w_render_world_col_shaded_gs_37ec8, 100);
    roth_override(r, ROTH_FN_render_world_col_shaded_masked_gs_38198, w_render_world_col_shaded_masked_gs_38198, 100);
    roth_override(r, ROTH_FN_render_world_col_shaded_gs_wrapped_38434, w_render_world_col_shaded_gs_wrapped_38434, 100);
    roth_override(r, ROTH_FN_render_world_col_shaded_masked_gs_wrapped_384fc, w_render_world_col_shaded_masked_gs_wrapped_384fc, 100);
    roth_override(r, ROTH_FN_render_world_col_solid_fill_38684, w_render_world_col_solid_fill_38684, 100);
    roth_override(r, ROTH_FN_render_world_col_solid_gradient_387f0, w_render_world_col_solid_gradient_387f0, 100);
    roth_override(r, ROTH_FN_render_world_col_unshaded_opaque_37cec, w_render_world_col_unshaded_opaque_37cec, 100);
    roth_override(r, ROTH_FN_render_world_col_unshaded_opaque_37fac, w_render_world_col_unshaded_opaque_37fac, 100);
    roth_override(r, ROTH_FN_render_world_col_unshaded_masked_38964, w_render_world_col_unshaded_masked_38964, 100);
    roth_override(r, ROTH_FN_render_world_col_unshaded_masked_2axis_38288, w_render_world_col_unshaded_masked_2axis_38288, 100);
    roth_override(r, ROTH_FN_cmd_flash_lights, w_cmd_flash_lights, 100);
    roth_override(r, ROTH_FN_cmd_change_lighting, w_cmd_change_lighting, 100);
    roth_override(r, ROTH_FN_cmd_light_switch, w_cmd_light_switch, 100);
    roth_override(r, ROTH_FN_tick_change_lighting, w_tick_change_lighting, 100);
    roth_override(r, ROTH_FN_tick_light_switch, w_tick_light_switch, 100);
    roth_override(r, ROTH_FN_apply_light_delta_to_record_list, w_apply_light_delta_to_record_list, 100);
    roth_override(r, ROTH_FN_render_world_span_wrapped_391d0, w_render_world_span_wrapped_391d0, 100);
}

static void on_load(const struct roth_api_v1 *api)
{
    g_api = api;
    const char *s;
    if ((s = getenv("ORACLE_PASS"))) g_pass = atoi(s);
    if ((s = getenv("ORACLE_PAINT"))) g_paint = atoi(s);
    if ((s = getenv("ORACLE_SHADE"))) g_shade = atoi(s);
    if ((s = getenv("ORACLE_FRAMES"))) g_target = atoi(s);
    if ((s = getenv("ORACLE_OUT"))) g_out = s;
    if ((s = getenv("ORACLE_POSE"))) { int n = sscanf(s, "%d %d %d %d %d", &g_px, &g_py, &g_pa, &g_psec, &g_pz); if (n >= 3) g_pose = 1; }
    if ((s = getenv("ORACLE_PHASELOG"))) g_phaselog = fopen(s, "w");
    if ((s = getenv("ORACLE_PITCH"))) { g_pitch = atoi(s); g_pitch_set = 1; }
    if ((s = getenv("ORACLE_WALLLOG"))) g_walllog = fopen(s, "w");
    if ((s = getenv("ORACLE_CMDLOG"))) g_cmdlog = fopen(s, "w");
    if ((s = getenv("ORACLE_SHLOG"))) g_shlog = fopen(s, "w");
    if ((s = getenv("ORACLE_TEXLOG"))) g_texlog = fopen(s, "w");
}

static const struct roth_plugin_info_v1 INFO = {
    .abi_major = ROTH_ABI_MAJOR, .abi_minor = ROTH_ABI_MINOR, .struct_size = sizeof(struct roth_plugin_info_v1),
    .id = "io.github.vrealms.oracle", .name = "VRealms oracle", .version = "0.2",
    .sdk_req_major = ROTH_SDK_MAJOR, .sdk_req_minor = ROTH_SDK_MINOR, .api_use = ROTH_API_USE_ENGINE,
    .on_load = on_load, .on_frame_game = frame, .on_compose_tick = compose, .on_register_overrides = reg,
};
ROTH_PLUGIN_EXPORT const struct roth_plugin_info_v1 *roth_plugin_query_v1(void) { return &INFO; }
