/* wallpath.inc.c -- included by rothdiff_plugin.c.
 *
 * WHICH WALL FACES TAKE THE STORED-EXTENT PATH.
 *
 * ROTH.C textures a wall's horizontal axis two different ways, chosen by
 * bit 0x100 of g_world_surface_draw_flags (renderer.c:13334):
 *
 *   SET    stored-extent path. The along-wall coordinate is used UNSCALED
 *          (renderer.c:4940: u = wrap_reoff[0x0a] << 15), so the density is a
 *          fixed 2 world units per texel -- 1 under HALF_PIXEL, which doubles
 *          the coordinate itself at renderer.c:13336.
 *
 *   CLEAR  computed-extent path. The coordinate is rescaled by
 *          extent_out / storedExtent, where extent_out = 2 * texture width
 *          (renderer.c:4943, :13347). HERE the stored extent sets the density.
 *
 * ROTH_SURFACES_FIX.md 3.3 gives "texels across = extent/2" for every wall,
 * which is the CLEAR case only. Getting this backwards would stretch or squash
 * every wall in the game, so which faces take which path is worth measuring
 * rather than inferring -- and the flag is built through three record hops that
 * would each have to be confirmed separately by reading.
 *
 * The flag word is loaded from the span record's +0x16 at the top of
 * draw_world_surface_spans (renderer.c:13101), so it is that surface's own
 * value for the whole call. This samples it after the body has run, which is
 * the same surface -- there is no nested surface draw in between.
 */

#define VA_WALL_DRAW_FLAGS  0x9093cu   /* g_world_surface_draw_flags          */
#define VA_WALL_STORED_EXT  0x90980u   /* g_span_src_wrap_reoffset + 0x04     */
#define VA_WALL_TEX_W       0x90978u   /* g_span_src_row_width                */
#define VA_WALL_TEX_H       0x90988u   /* g_span_src_wrap_reoffset + 0x0c     */
#define VA_WALL_DAS_ID      0x90a78u   /* g_current_das_entry_id              */
#define VA_WALL_SUBPASS     0x90a48u   /* 0 = visible pass                    */

#define WALLPATH_MAX 256

struct wallpath_row {
    uint32_t flags;      /* the whole word, so nothing is decoded away here */
    uint32_t stored;     /* the stored extent                               */
    uint32_t texW, texH;
    uint32_t das;
    uint32_t hits;
};
static struct wallpath_row g_wp[WALLPATH_MAX];
static int g_wp_n;
static int g_wp_overflow;
static uint32_t g_wp_stored_path, g_wp_computed_path;

static void wallpath_sample(const struct roth_api_v1 *api)
{
    const struct roth_game_ram_api_v1 *m = api->game_ram;
    if (m->u8(VA_WALL_SUBPASS) != 0) return;      /* cursor pick, not the picture */

    struct wallpath_row r;
    r.flags  = m->u16(VA_WALL_DRAW_FLAGS);
    r.stored = m->u16(VA_WALL_STORED_EXT);
    r.texW   = m->u32(VA_WALL_TEX_W);
    r.texH   = m->u16(VA_WALL_TEX_H);
    r.das    = m->u16(VA_WALL_DAS_ID);
    r.hits   = 1;

    if (r.flags & 0x100u) g_wp_stored_path++; else g_wp_computed_path++;

    for (int i = 0; i < g_wp_n; i++)
    {
        struct wallpath_row *o = &g_wp[i];
        if (o->flags == r.flags && o->stored == r.stored && o->texW == r.texW
            && o->texH == r.texH && o->das == r.das) { o->hits++; return; }
    }
    if (g_wp_n >= WALLPATH_MAX) { g_wp_overflow = 1; return; }
    g_wp[g_wp_n++] = r;
}

static void ROTH_CDECL ov_wall_spans(struct roth_chain *chain,
                                     const struct roth_api_v1 *api,
                                     uint32_t ecx_entry, uint32_t gs_base,
                                     uint32_t es_base, uint32_t fs_base)
{
    roth_next_draw_world_surface_spans(chain, ecx_entry, gs_base, es_base, fs_base);
    wallpath_sample(api);
}

static void wallpath_dump(void)
{
    if (g_wp_n == 0) return;
    const char *path = getenv("ROTHDIFF_WALLS");
    if (path == NULL) path = "wallpath.csv";
    FILE *f = fopen(path, "w");
    if (f == NULL) return;

    fprintf(f, "path,flags_hex,stored_extent,tex_w,tex_h,das_id,spans\n");
    for (int i = 0; i < g_wp_n; i++)
    {
        const struct wallpath_row *r = &g_wp[i];
        fprintf(f, "%s,%04x,%u,%u,%u,%u,%u\n",
                (r->flags & 0x100u) ? "stored" : "computed",
                r->flags, r->stored, r->texW, r->texH, r->das, r->hits);
    }
    fprintf(f, "\n# stored-extent path %lu span(s); computed-extent path %lu span(s)%s\n",
            (unsigned long)g_wp_stored_path, (unsigned long)g_wp_computed_path,
            g_wp_overflow ? "   (TABLE TRUNCATED)" : "");
    fclose(f);

    fprintf(stderr, "[rothdiff] walls: stored %lu / computed %lu -> %s\n",
            (unsigned long)g_wp_stored_path, (unsigned long)g_wp_computed_path, path);
}
