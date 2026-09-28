/* flatspans.inc.c -- included by oraclelog.c.
 *
 * WHAT THE ORIGINAL STAGES FOR EVERY WORLD FLAT, READ FROM THE VISIBLE PASS.
 *
 * THIS FILE PREVIOUSLY LOGGED THE WRONG RENDER PASS, and every rule taken from
 * it was wrong. Read this before changing it.
 *
 * ROTH.C walks the world face list TWICE:
 *
 *   render_world_face_list         0x2ad21  renderer.c:9147   DRAWS THE PICTURE
 *       called from render_world.c:117, :155, :187
 *   render_world_face_list_subpass 0x28dbe  renderer.c:8903   CURSOR PICK
 *       one caller, renderer.c:9565, inside the function that sets a
 *       ONE-PIXEL view window (renderer.c:9493-9497)
 *
 * The two look almost identical, which is the trap. They differ where it
 * matters: the pick pass writes a BARE fill word, the visible pass ORs the
 * sector's mirror bits into it.
 *
 *   visible ceiling:  fill = 0x38 | ((sector[0x17] & 0x0c) >> 1)   R:9229
 *   visible floor:    fill = 0xb8 | ((sector[0x17] & 0x03) << 1)   R:9249
 *   pick    ceiling:  fill = 0x38                                  R:8968
 *   pick    floor:    fill = 0xb8                                  R:8987
 *
 * Logging the pick pass therefore says flats have no mirrors. They do. That
 * mistake reached the loader, where the correct mirror code was deleted on the
 * strength of it and had to be restored.
 *
 * THE PASS FILTER: g_world_render_subpass_kind (0x90a48) is 0xff on entry to
 * the pick pass and 1..8 within it (R:8915, R:8962-9090), and is cleared to 0
 * at R:9598 (0x28dac). The visible pass never writes it. So 0 means visible.
 *
 * WHERE FLATS ARE ACTUALLY DRAWN: the span dispatch at R:13314 tests the draw
 * flags -- which ARE the fill word, span record 0x84f18 + 0x16 = 0x84f2e:
 *
 *     flags & 0x020 -> draw_scaled_sprite_spans  0x39610   <-- WORLD FLATS
 *     flags & 0x200 -> draw_floorceil_surface    0x3a84e   <-- 3D MESH FACES
 *
 * 0x38 and 0xb8 both have 0x20 set, so world flats take the first. A large
 * draw_floorceil_surface count is EXPECTED, is mesh faces, and is not evidence
 * about flats either way. It is counted below only to keep that straight.
 *
 * So this probe hooks draw_scaled_sprite_spans rather than the span emitters:
 * by then the texture is RESOLVED, so width and height are the real ones.
 * Reading them at the emitter -- as this file used to -- samples whatever the
 * previous surface happened to leave behind.
 */

#define VA_SUBPASS_KIND  0x90a48u   /* 0 = visible pass; 0xff / 1..8 = pick    */
#define VA_FACE_REC      0x8528cu   /* g_perspective_scale+4: the surface record
                                     * the pass is on. The VISIBLE pass writes
                                     * it at R:9179 (0x2ad81), before its span
                                     * emitters. 0x90a42 is pick-only -- which
                                     * is what this file used to read.         */
#define VA_FILL_MODE     0x84f2eu   /* span record +0x16, the draw flags:
                                     *   0x80 = floor (else ceiling)
                                     *   0x08 = textured (else solid fill)
                                     *   0x06 = the two MIRROR bits            */
#define VA_SRC_ROW_W     0x90978u   /* texture width, valid once resolved      */
#define VA_WRAP_REOFF    0x9097cu   /* +0x0c height, +0x10 the scale s         */
#define VA_VIEW_OFF_X    0x90a04u   /* texture origin, first axis              */
#define VA_VIEW_OFF_Y    0x90a06u   /* +0x03 / +0x05: the per-face shift pair  */

#define FLATSPAN_MAX 256

struct flatspan_row {
    uint16_t tick;
    int32_t  px, py;
    uint32_t face, width, height, s, fill;
    int32_t  ox, oy;
    uint32_t shu, shv;
};
static struct flatspan_row g_fs[FLATSPAN_MAX];
static int g_fs_n;
static int g_fs_overflow;

static uint16_t g_fs_tick;
static int32_t  g_fs_px, g_fs_py;

/* Counted, not judged: these are mesh faces (0x200), not world flats. */
static uint32_t g_fc_calls;
/* Spans rejected for being in the pick pass. A large number is NORMAL, and is
 * exactly what this probe used to log as though it were the picture. */
static uint32_t g_pick_rejected;

static void flatspan_record(const struct roth_api_v1 *api)
{
    const struct roth_game_ram_api_v1 *m = api->game_ram;

    if (m->u8(VA_SUBPASS_KIND) != 0) { g_pick_rejected++; return; }

    struct flatspan_row r;
    r.tick   = g_fs_tick;
    r.px     = g_fs_px;
    r.py     = g_fs_py;
    r.face   = m->u16(VA_FACE_REC);
    r.width  = m->u32(VA_SRC_ROW_W);
    r.height = m->u16(VA_WRAP_REOFF + 0x0c);
    r.s      = m->u16(VA_WRAP_REOFF + 0x10);
    r.fill   = m->u16(VA_FILL_MODE);
    r.ox     = (int16_t)m->u16(VA_VIEW_OFF_X);
    r.oy     = (int16_t)m->u16(VA_VIEW_OFF_Y);
    r.shu    = m->u8 (VA_VIEW_OFF_Y + 3);
    r.shv    = m->u8 (VA_VIEW_OFF_Y + 5);

    for (int i = 0; i < g_fs_n; i++)
    {
        const struct flatspan_row *o = &g_fs[i];
        if (o->face == r.face && o->fill == r.fill && o->s == r.s
            && o->width == r.width && o->height == r.height
            && o->ox == r.ox && o->oy == r.oy
            && o->px == r.px && o->py == r.py)
            return;
    }
    if (g_fs_n >= FLATSPAN_MAX) { g_fs_overflow = 1; return; }
    g_fs[g_fs_n++] = r;
}

/* draw_scaled_sprite_spans (0x39610) -- the driver world flats reach. */
static void ROTH_CDECL ov_scaled_spans(struct roth_chain *chain,
                                       const struct roth_api_v1 *api,
                                       uint32_t esi, uint32_t gs_base, uint32_t es_base,
                                       uint32_t fs_base, uint16_t es_sel, uint16_t fs_sel)
{
    flatspan_record(api);
    roth_next_draw_scaled_sprite_spans(chain, esi, gs_base, es_base, fs_base, es_sel, fs_sel);
}

/* draw_floorceil_surface (0x3a84e) -- 3D mesh faces. Counted only. */
static void ROTH_CDECL ov_draw_floorceil(struct roth_chain *chain,
                                         const struct roth_api_v1 *api,
                                         uint32_t esi, uint32_t gs_base, uint32_t fs_base,
                                         uint32_t es_fb_base, uint32_t blend_base,
                                         uint16_t es_sel, uint16_t fs_sel)
{
    (void)api;
    g_fc_calls++;
    roth_next_draw_floorceil_surface(chain, esi, gs_base, fs_base,
                                     es_fb_base, blend_base, es_sel, fs_sel);
}

static void flatspan_dump(void)
{
    if (g_fs_n == 0) return;
    const char *path = getenv("ROTH_ORACLE_FLATS");
    if (path == NULL) path = "oracle_flats.csv";
    FILE *f = fopen(path, "w");
    if (f == NULL) return;

    fprintf(f, "tick,surface,textured,mirror_u,mirror_v,face,tex_w,tex_h,s,"
               "player_x,player_y,origin_u,origin_v,shift_u,shift_v,fill_hex\n");
    for (int i = 0; i < g_fs_n; i++)
    {
        const struct flatspan_row *r = &g_fs[i];
        fprintf(f, "%u,%s,%u,%u,%u,%u,%u,%u,%u,%ld,%ld,%ld,%ld,%u,%u,%04x\n",
                (unsigned)r->tick,
                (r->fill & 0x80u) ? "floor" : "ceiling",
                (r->fill & 0x08u) ? 1u : 0u,
                (r->fill & 0x02u) ? 1u : 0u,
                (r->fill & 0x04u) ? 1u : 0u,
                r->face, r->width, r->height, r->s,
                (long)r->px, (long)r->py, (long)r->ox, (long)r->oy,
                r->shu, r->shv, r->fill);
    }

    fprintf(f, "\n# visible-pass rows %d   pick-pass rows rejected %lu\n",
            g_fs_n, (unsigned long)g_pick_rejected);
    fprintf(f, "# draw_floorceil_surface (3D MESH faces, not flats) %lu\n",
            (unsigned long)g_fc_calls);
    fclose(f);

    fprintf(stderr, "[oraclelog] %d visible flat row(s), %lu pick rejected, "
                    "%lu mesh-face draws -> %s%s\n",
            g_fs_n, (unsigned long)g_pick_rejected, (unsigned long)g_fc_calls,
            path, g_fs_overflow ? "  (TRUNCATED)" : "");
}
