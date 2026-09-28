/* uvcapture.inc.c -- included by rothdiff_plugin.c.
 *
 * PER-PIXEL (u, v) OUT OF ROTH.C, WITHOUT COMPUTING ANYTHING.
 *
 * THE PROBLEM THIS SOLVES. To compare textures properly we need, for every
 * pixel, which texel the original sampled. The obvious way is to reproduce the
 * fill loops' index arithmetic in the plugin. That is not possible and would
 * not be trustworthy if it were:
 *
 *   - the shift amounts live in g_tex_shl / g_tex_shr_even, which are C globals
 *     inside the lifted engine, NOT game RAM, so a plugin cannot read them;
 *   - they are self-modified per texture (renderer.c:3378-3382);
 *   - and reconstructing this project's fixed point by hand has already
 *     produced three wrong constants for a single scale value.
 *
 * ROTH.C itself must not be edited -- a modified reference is not an oracle.
 *
 * THE TRICK. Do not compute the texel. Make the original draw it.
 *
 * The plugin can write game memory, so before a frame we overwrite the TEXTURE
 * PIXELS with a gradient: in the U pass every texel's value is its own column,
 * in the V pass its own row. The original then samples exactly as it always
 * does, and the framebuffer it produces IS the u (or v) buffer. No arithmetic
 * of ours is anywhere in the path, so there is nothing for us to get wrong, and
 * every self-modifying trick in the fills is handled by the fills themselves.
 *
 * WHY NO COLORMAP WORK IS NEEDED. The opaque textured fill writes the texel
 * straight to the framebuffer -- renderer.c:2727-2731,
 *
 *     uint8_t texel = texbase[ebx];
 *     di[0] = texel;
 *
 * with no gs[] lookup. That is the loop opaque flats take
 * (g_span_textured_mode_flag == 0 -> 0x3a220, renderer.c:3341), so the gradient
 * survives to the screen unaltered. The SHADED loop (0x3a4f8) does apply a
 * colormap and would need an identity table; it is not used by the surfaces
 * this is measuring, and a pixel drawn by it is reported as unmeasured rather
 * than quietly wrong.
 *
 * LIMIT, STATED PLAINLY. A gradient byte is 8 bits, so u and v wrap every 256
 * texels. Every Realms flat is at most 256 across, so within one texture this
 * is exact; it cannot distinguish texel 5 from texel 261 on a wider wall
 * texture, and the comparison treats that as a wrap rather than a match.
 */

#define VA_UV_SRC_BASE   0x84980u   /* g_render_source_base_ptr: the texture    */
#define VA_UV_ROW_W      0x90978u   /* g_span_src_row_width                     */
#define VA_UV_TEX_H      0x90988u   /* g_span_src_wrap_reoffset + 0x0c          */
#define VA_UV_SUBPASS    0x90a48u   /* 0 = the visible pass                     */
#define VA_UV_TEXMODE    0x8a352u   /* g_span_textured_mode_flag: 0 = opaque    */

/* Which gradient this frame is painting. */
enum { UV_OFF = 0, UV_PASS_U = 1, UV_PASS_V = 2 };
static int      g_uv_pass;

/* PROOF THE WORLD IS ACTUALLY BEING DRAWN.
 *
 * A capture used to start on a tick count, which meant it could fire while the
 * game was still sitting on the Options menu -- producing a file full of menu
 * with not one world pixel in it. The span fills are the only honest signal
 * that a world frame is happening, and this file is already inside them: every
 * paint bumps this counter, and the capture waits for it to be non-zero.
 *
 * Reset per frame by the capture side, so it means "spans drew in THIS frame",
 * not "spans drew at some point". */
static uint32_t g_uv_world_spans;
static uint32_t g_uv_painted;      /* textures rewritten this frame            */
static uint32_t g_uv_skipped;      /* surfaces we cannot vouch for             */
static int      g_uv_pattern_v;    /* 0 = paint the column, 1 = the row */
static uint32_t g_uv_readbacks;    /* how many read-back lines logged so far  */

/* Paint one texture with its own coordinates.
 *
 * Done per surface, at the driver, because the texture the fills will read is
 * only settled by then -- g_render_source_base_ptr is re-resolved per surface
 * (renderer.c:13332). Painting once up front would paint the wrong block. */
void uv_paint_now(const struct roth_api_v1 *api)
{
    const struct roth_game_ram_api_v1 *m = api->game_ram;
    if (g_uv_pass == UV_OFF) return;

    /* NO PASS GATE, NO FRAME GATE, NO ALTERNATION.
     *
     * Every surface is repainted every time it is about to be sampled, for the
     * whole run, with ONE fixed pattern chosen at startup. That makes every
     * frame a coordinate frame, so the capture no longer has to land on a
     * particular one -- which is what defeated eight earlier versions, each of
     * which raced the frame it had just painted.
     *
     * The cursor-pick pass is still excluded: it renders a one-pixel window and
     * is not the picture. */
    if (m->u8(VA_UV_SUBPASS) != 0) return;

    /* Only the opaque fill writes the texel straight through. Anything else
     * would come back through a colormap and could not be read as a
     * coordinate, so it is counted, not painted. */
    if (m->u8(VA_UV_TEXMODE) != 0) { g_uv_skipped++; return; }

    const uint32_t base = m->u32(VA_UV_SRC_BASE);
    const uint32_t w = m->u32(VA_UV_ROW_W);
    const uint32_t h = m->u16(VA_UV_TEX_H);
    if (base == 0 || w == 0 || h == 0 || w > 1024 || h > 1024) { g_uv_skipped++; return; }

    /* A raw host pointer: the DAS cache lives in the DPMI arena, outside the
     * bounded game_ram window, exactly as the static dump reads the map. */
    uint8_t *tex = (uint8_t *)(uintptr_t)base;
    const uint32_t n = w * h;

    /* ASSUME NOTHING ABOUT THE LAYOUT.
     *
     * The first version painted tex[row*w + col] = col, i.e. it assumed the
     * stored image is row-major with row length w. Realms stores wall art
     * ROTATED 90 degrees -- the along-wall index picks the image ROW -- so that
     * ramp ran along whichever axis the fill was holding constant, and the U
     * pass came back with fifteen distinct values and a mid-row of solid zero.
     *
     * So the two passes now paint the LINEAR INDEX itself, low byte then high
     * byte. The framebuffer then carries the actual index the fill computed,
     * and the decomposition into (u, v) is done afterwards against the texture
     * dimensions -- from the data, rather than from an assumption of ours about
     * which way the image is stored.
     *
     * Two passes because one byte cannot hold an index into a 256x256 image:
     * pass 1 is idx & 0xff, pass 2 is (idx >> 8) & 0xff, and together they
     * recover indices up to 65535, which covers every image in the game.
     */
    /* WHICH COORDINATE THIS RUN PAINTS.
     *
     * One pass per run, chosen by ROTHDIFF_PATTERN, instead of two passes in
     * one run. The second pass never captured reliably -- it kept running
     * hundreds of frames before writing, and captured a frame that was no
     * longer the one it had painted -- and two short runs are worth more than
     * one clever one that does not work.
     *
     * u: the texel COLUMN.  v: the texel ROW. Both derived from the linear
     * index and the row length the fill itself is using, so neither depends on
     * an assumption about how the image is stored. */
    if (g_uv_pattern_v)
        for (uint32_t i = 0; i < n; i++) tex[i] = (uint8_t)((i / w) & 0xffu);
    else
        for (uint32_t i = 0; i < n; i++) tex[i] = (uint8_t)((i % w) & 0xffu);

    /* READ BACK WHAT WE JUST WROTE.
     *
     * The captured frames showed the original artwork, not our ramp, which has
     * exactly two possible causes: either the write did not land where the fill
     * reads, or the frame being captured is not the frame we painted for. One
     * read-back separates them, and neither answer needs a guess:
     *
     *   values come back as ours  -> the pointer is right, the FRAME is wrong
     *   values come back as art   -> the pointer is wrong
     *
     * Logged for the first few surfaces only; a line per surface per frame
     * would be thousands of lines and tell us nothing the first few do not. */
    if (g_uv_readbacks < 6)
    {
        g_uv_readbacks++;
        fprintf(stderr,
            "[uv] base=%08x w=%u h=%u  wrote[0..3]=%02x %02x %02x %02x"
            "  readback=%02x %02x %02x %02x  %s\n",
            base, w, h,
            (unsigned)(g_uv_pass == UV_PASS_U ? 0 : 0),
            (unsigned)(g_uv_pass == UV_PASS_U ? 1 : 0),
            (unsigned)(g_uv_pass == UV_PASS_U ? 2 : 0),
            (unsigned)(g_uv_pass == UV_PASS_U ? 3 : 0),
            tex[0], tex[1], tex[2], tex[3],
            g_uv_pass == UV_PASS_U ? "pass U" : "pass V");
    }

    g_uv_painted++;
    g_uv_world_spans++;   /* a world span drew in this frame */
}

#define VA_UV_TARGET     0x85414u   /* g_render_target_buffer */
#define VA_UV_PITCH      0x85498u   /* +0x00 pitch, +0x04 height */

static const uint8_t *uv_framebuffer(const struct roth_api_v1 *api,
                                     uint32_t *outW, uint32_t *outH)
{
    const struct roth_game_ram_api_v1 *m = api->game_ram;
    const uint32_t base = m->u32(VA_UV_TARGET);
    const uint32_t pitch = m->u32(VA_UV_PITCH);
    const uint32_t height = m->u32(VA_UV_PITCH + 4);
    if (base == 0 || pitch == 0 || height == 0 || pitch > 4096 || height > 4096)
        return NULL;
    *outW = pitch;
    *outH = height;
    return (const uint8_t *)(uintptr_t)base;
}


/* SNAPSHOT THE FRAME WHILE THE WORLD IS STILL ON IT.
 *
 * Reading the framebuffer at compose time gave a picture with almost none of
 * our ramp in it, even though the ramp was plainly on screen -- by then the
 * world pass has been overwritten or flipped away.
 *
 * render_world_face_list (0x2ad21) IS the visible world pass -- three callers,
 * all in render_world.c. The instant it returns, the world is drawn and nothing
 * else has touched the buffer yet, so that is when the copy is taken.
 */
static uint8_t *g_uv_snap;
static uint32_t g_uv_snap_w, g_uv_snap_h;
static int      g_uv_snap_valid;

static void ROTH_CDECL ov_uv_world(struct roth_chain *chain,
                                   const struct roth_api_v1 *api,
                                   uint32_t ecx, uint16_t gs)
{
    roth_next_render_world_face_list(chain, ecx, gs);

    if (g_uv_pass == UV_OFF || g_uv_world_spans == 0) return;

    uint32_t w = 0, h = 0;
    const uint8_t *fb = uv_framebuffer(api, &w, &h);
    if (fb == NULL) return;

    const size_t n = (size_t)w * (size_t)h;
    if (g_uv_snap == NULL || w != g_uv_snap_w || h != g_uv_snap_h)
    {
        free(g_uv_snap);
        g_uv_snap = (uint8_t *)malloc(n);
        g_uv_snap_w = w; g_uv_snap_h = h;
    }
    if (g_uv_snap == NULL) return;
    memcpy(g_uv_snap, fb, n);
    g_uv_snap_valid = 1;
}

/* Hooked at the DRIVER, not at the fills: this has to run after the texture for
 * this surface is resolved and before its spans are drawn. */
static void ROTH_CDECL ov_uv_driver(struct roth_chain *chain,
                                    const struct roth_api_v1 *api,
                                    uint32_t esi, uint32_t gs_base, uint32_t es_base,
                                    uint32_t fs_base, uint16_t es_sel, uint16_t fs_sel)
{
    (void)api;   /* the paint happens at the FILL now -- see uv_paint_now */
    roth_next_draw_scaled_sprite_spans(chain, esi, gs_base, es_base, fs_base, es_sel, fs_sel);
}

static int uv_register(struct roth_registrar_v1 *reg)
{
    int bad = roth_override(reg, ROTH_FN_draw_scaled_sprite_spans, ov_uv_driver, 0);
    bad |= roth_override(reg, ROTH_FN_render_world_face_list, ov_uv_world, 0);
    return bad;
}

/* THE FRAMEBUFFER THE FILLS ACTUALLY WROTE TO.
 *
 * Not on_compose_tick's `pixels`. The read-back proved our ramp lands in the
 * texture the fills sample, yet the composed buffer still came back as ordinary
 * artwork -- so the buffer handed to the compose hook is not the one the span
 * fills write. They write to g_render_target_buffer + edi (renderer.c:2884 and
 * every sibling loop), so that is what has to be read.
 *
 * Pitch and height come from g_screen_pitch, which video_display.c:838 uses to
 * build the scanline offset table, so it is the same geometry the fills index
 * against. */
/* The composed frame IS the coordinate buffer. Written raw: one byte per pixel,
 * the same value the original sampled. */
static int uv_write(const char *path, const uint8_t *pixels,
                    uint32_t width, uint32_t height, int pass,
                    int32_t px, int32_t py, int32_t pang)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) return -1;
    const uint32_t magic = 0x56555652u;   /* 'RVUV' */
    const uint32_t ver = 1u;
    const uint32_t p = (uint32_t)pass;
    fwrite(&magic, 4, 1, f);
    fwrite(&ver, 4, 1, f);
    fwrite(&width, 4, 1, f);
    fwrite(&height, 4, 1, f);
    fwrite(&p, 4, 1, f);
    fwrite(&px, 4, 1, f);
    fwrite(&py, 4, 1, f);
    fwrite(&pang, 4, 1, f);
    fwrite(pixels, 1, (size_t)width * (size_t)height, f);
    fclose(f);
    return 0;
}
