/* idbuffer.inc.c -- included by rothdiff_plugin.c.
 *
 * A PER-PIXEL IDENTITY BUFFER FOR ONE FRAME OF ROTH.C'S VISIBLE PASS.
 *
 * The point of this file is to end texture arguments that are settled by
 * looking at pictures. For a given camera pose it records, for EVERY PIXEL,
 * which surface the original drew there and out of which texture -- so the two
 * engines can be compared as data rather than as screenshots.
 *
 * WHY THIS IS EXACT AND NOT DERIVED
 *
 * Every one of ROTH.C's span fill inner loops takes the same two arguments:
 *
 *     f(ecx = PIXEL COUNT, edi = DESTINATION OFFSET, ...)
 *
 * and writes exactly `count` bytes at `g_render_target_buffer + edi`
 * (renderer.c:2884 and the sibling loops). So the pixel range a span covers is
 * handed to us; nothing about it is reconstructed. We shadow that same range in
 * a parallel buffer with the identity of the surface being drawn.
 *
 * The seven loops are the full set the sprite/flat driver dispatches to
 * (0x3a000 solid, 0x3a0b1 gradient, 0x3a100 / 0x3a220 textured, 0x3a368
 * texmapped, 0x3a4f8 shaded, 0x3a700 blended).
 *
 * WHAT IS RECORDED, AND WHAT IS DELIBERATELY NOT
 *
 * Recorded per pixel, all read straight from live globals at fill time:
 *
 *     fill   g_span_fill_mode_word  0x84f2e   span record +0x16
 *     flags  g_world_surface_draw_flags 0x9093c
 *     id     g_perspective_scale+4  0x8528c   the surface record
 *     tex    g_current_das_entry_id 0x90a78   the resolved texture
 *
 * NOT recorded: per-pixel u,v. Deriving those means replaying the fills'
 * self-modified shift immediates, and that fixed-point split is precisely where
 * this project has already produced three wrong constants. Per-span texture
 * state is dumped separately by flatspans.inc.c, where every term is read
 * rather than reconstructed. If per-pixel u,v is wanted later it should come
 * from a shadow fill loop that steps the real accumulators, not from arithmetic
 * written out by hand here.
 *
 * THE PASS FILTER. Same rule as flatspans: only g_world_render_subpass_kind
 * (0x90a48) == 0 is the visible frame. The cursor pick pass renders a ONE-PIXEL
 * window and would otherwise scribble a single bogus pixel into every buffer.
 */

#define VA_SUBPASS_KIND   0x90a48u
#define VA_FILL_MODE      0x84f2eu
#define VA_DRAW_FLAGS     0x9093cu
#define VA_FACE_REC       0x8528cu
#define VA_DAS_ENTRY_ID   0x90a78u
#define VA_SCREEN_PITCH   0x85498u   /* +0x00 pitch, +0x04 height */

/* One pixel of identity. Kept to 8 bytes: at 640x480 that is 2.4 MB, which is
 * nothing, and it keeps the on-disk format trivially memory-mappable. */
struct id_px {
    uint16_t fill;
    uint16_t flags;
    uint16_t id;
    uint16_t tex;
};

static struct id_px *g_id;
static uint32_t g_id_w, g_id_h, g_id_n;
static int      g_id_armed;        /* capturing this frame */
static uint64_t g_id_written;      /* pixels stamped this frame */

static void id_ensure(const struct roth_api_v1 *api)
{
    const uint32_t pitch  = api->game_ram->u32(VA_SCREEN_PITCH);
    const uint32_t height = api->game_ram->u32(VA_SCREEN_PITCH + 4);
    if (pitch == 0 || height == 0 || pitch > 4096 || height > 4096) return;
    if (g_id != NULL && pitch == g_id_w && height == g_id_h) return;
    free(g_id);
    g_id_w = pitch; g_id_h = height; g_id_n = pitch * height;
    g_id = (struct id_px *)calloc(g_id_n, sizeof *g_id);
}

static void id_begin_frame(const struct roth_api_v1 *api)
{
    id_ensure(api);
    if (g_id == NULL) return;
    memset(g_id, 0, (size_t)g_id_n * sizeof *g_id);
    g_id_written = 0;
    g_id_armed = 1;
}

/* Stamp one span's identity across the pixels it is about to write. */
static void id_span(const struct roth_api_v1 *api, uint32_t edi, uint32_t count)
{
    if (!g_id_armed || g_id == NULL) return;
    const struct roth_game_ram_api_v1 *m = api->game_ram;
    if (m->u8(VA_SUBPASS_KIND) != 0) return;          /* cursor pick, not the picture */

    if (edi >= g_id_n) return;
    uint32_t n = count;
    if (edi + n > g_id_n) n = g_id_n - edi;           /* never trust a count blindly */

    struct id_px p;
    p.fill  = m->u16(VA_FILL_MODE);
    p.flags = m->u16(VA_DRAW_FLAGS);
    p.id    = m->u16(VA_FACE_REC);
    p.tex   = m->u16(VA_DAS_ENTRY_ID);

    for (uint32_t i = 0; i < n; i++) g_id[edi + i] = p;
    g_id_written += n;
}

/* ---- the seven fill inner loops ---------------------------------------- */

/* uv_paint_now() paints the texture THIS fill is about to sample. It must
 * happen here, not at the driver: g_render_source_base_ptr is re-resolved per
 * texture inside the span path (renderer.c:13332), so at driver entry it still
 * names the PREVIOUS surface -- which is why the ramp landed (the read-back
 * proved it) while the screen kept showing ordinary artwork. */
void uv_paint_now(const struct roth_api_v1 *api);

#define ID_LOOP2(NAME, NEXT)                                                   \
static void ROTH_CDECL NAME(struct roth_chain *chain,                          \
                            const struct roth_api_v1 *api,                     \
                            uint32_t ecx, uint32_t edi, const uint8_t *a)      \
{ uv_paint_now(api); id_span(api, edi, ecx & 0xffffu); NEXT(chain, ecx, edi, a); }

#define ID_LOOP3(NAME, NEXT)                                                   \
static void ROTH_CDECL NAME(struct roth_chain *chain,                          \
                            const struct roth_api_v1 *api,                     \
                            uint32_t ecx, uint32_t edi,                        \
                            const uint8_t *a, const uint8_t *b)                \
{ uv_paint_now(api); id_span(api, edi, ecx & 0xffffu); NEXT(chain, ecx, edi, a, b); }

/* The solid and gradient loops take their SECOND pointer non-const (it is the
 * destination selector base, not a source), so they get their own wrapper
 * rather than a cast that would hide a real signature change later. They are
 * registered too: an untextured fill is exactly the colour-key / solid-colour
 * surface this project has been getting wrong, so those pixels must be in the
 * buffer and not holes. */
#define ID_LOOP3W(NAME, NEXT)                                                  \
static void ROTH_CDECL NAME(struct roth_chain *chain,                          \
                            const struct roth_api_v1 *api,                     \
                            uint32_t ecx, uint32_t edi,                        \
                            const uint8_t *a, uint8_t *b)                      \
{ uv_paint_now(api); id_span(api, edi, ecx & 0xffffu); NEXT(chain, ecx, edi, a, b); }

ID_LOOP3W(ov_fill_3a000, roth_next_render_sprite_span_solid_3a000)
ID_LOOP3W(ov_fill_3a0b1, roth_next_render_sprite_span_gradient_3a0b1)
ID_LOOP2(ov_fill_3a100, roth_next_render_sprite_span_tex_3a100)
ID_LOOP2(ov_fill_3a220, roth_next_render_sprite_span_tex_3a220)
ID_LOOP2(ov_fill_3a368, roth_next_render_span_texmap_3a368)
ID_LOOP3(ov_fill_3a4f8, roth_next_render_sprite_span_tex_shaded_3a4f8)
ID_LOOP3W(ov_fill_3a700, roth_next_render_sprite_span_tex_blend_3a700)

static int id_register(struct roth_registrar_v1 *reg)
{
    int bad = 0;
    bad |= roth_override(reg, ROTH_FN_render_sprite_span_solid_3a000,      ov_fill_3a000, 0);
    bad |= roth_override(reg, ROTH_FN_render_sprite_span_gradient_3a0b1,   ov_fill_3a0b1, 0);
    bad |= roth_override(reg, ROTH_FN_render_sprite_span_tex_3a100,        ov_fill_3a100, 0);
    bad |= roth_override(reg, ROTH_FN_render_sprite_span_tex_3a220,        ov_fill_3a220, 0);
    bad |= roth_override(reg, ROTH_FN_render_span_texmap_3a368,            ov_fill_3a368, 0);
    bad |= roth_override(reg, ROTH_FN_render_sprite_span_tex_shaded_3a4f8, ov_fill_3a4f8, 0);
    bad |= roth_override(reg, ROTH_FN_render_sprite_span_tex_blend_3a700,  ov_fill_3a700, 0);
    return bad;
}

/* ---- output ------------------------------------------------------------ */

/* Binary, because a 640x480 CSV is 300k lines and the diff reads it per pixel.
 *
 *   magic "RIDB"  u32
 *   version 1     u32
 *   width         u32
 *   height        u32
 *   pose x,y,ang  i32 x3
 *   then w*h records of {u16 fill, u16 flags, u16 id, u16 tex}
 */
static int id_write(const char *path, int32_t px, int32_t py, int32_t pang)
{
    if (g_id == NULL) return -1;
    FILE *f = fopen(path, "wb");
    if (f == NULL) return -1;
    const uint32_t magic = 0x42444952u; /* 'RIDB' little-endian */
    const uint32_t ver = 1u;
    fwrite(&magic, 4, 1, f);
    fwrite(&ver,   4, 1, f);
    fwrite(&g_id_w, 4, 1, f);
    fwrite(&g_id_h, 4, 1, f);
    fwrite(&px,  4, 1, f);
    fwrite(&py,  4, 1, f);
    fwrite(&pang, 4, 1, f);
    fwrite(g_id, sizeof *g_id, g_id_n, f);
    fclose(f);
    return 0;
}
