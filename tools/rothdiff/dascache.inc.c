/* dascache.inc.c -- included by rothdiff_plugin.c.
 *
 * REPAINT EVERY LOADED TEXTURE, ONCE, WITH NO RENDERING INVOLVED.
 *
 * Ten earlier versions painted textures from inside the render path -- at the
 * span driver, then at the fill -- and every one of them raced the frame it had
 * just painted. The capture kept landing on the frame before the paint, or on a
 * black fade, and each fix moved the failure somewhere else rather than
 * removing it.
 *
 * This removes the race instead of tuning it. The DAS cache is walked directly
 * and every loaded texture is rewritten in place. After that, EVERY frame is a
 * coordinate frame, so it no longer matters which one is captured -- an
 * ordinary screenshot is as good as anything.
 *
 * THE STRUCTURE, all read out of das_assets.c and renderer.c rather than
 * guessed:
 *
 *   status table   0x86d30   0x1600 entries, 2 bytes each
 *                            low byte < 0xfc  ->  loaded; the value is a SLOT
 *                            index (reset_das_entry_status_table, 0x3001b)
 *   cache slots    0x89930   6 bytes per slot; the first dword is a Pool
 *                            HANDLE (free_das_cache_entry, 0x41413)
 *   blk = *handle            the block itself
 *     blk + 0x0c             width          (renderer.c:6344, :13288)
 *     blk + 0x0e             height         (renderer.c:13289)
 *     blk + 0x10             PIXEL DATA     (renderer.c:5690, :5732, :5974)
 *
 * The pointers are raw host addresses in the DPMI arena, outside the bounded
 * game_ram window -- the same way the static dump reads the map.
 */

#define VA_DC_STATUS   0x86d30u   /* g_das_entry_status_table */
#define VA_DC_SLOTS    0x89930u   /* g_das_cache_slots, 6 bytes each */
#define DC_STATUS_N    0x1600u
#define DC_LOADED_MAX  0xfcu      /* low byte < this means the entry is loaded */

static int      g_dc_done;
static uint32_t g_dc_painted, g_dc_skipped;

/* Paint one block. `patternV` selects which coordinate is written: the texel
 * ROW rather than the texel COLUMN. Both are derived from the linear index and
 * the block's own row length, so neither assumes anything about how the image
 * is stored -- which matters, because wall art is stored rotated. */
static void dc_paint_block(uint32_t blk, int patternV)
{
    /* SIMPLE BLOCKS ONLY.
     *
     * blk+0x0a carries the block kind. Bit 0x40 means GROUPED: the block is a
     * header followed by child sub-blocks, and its pixels are NOT a plain
     * width*height run at +0x10 -- renderer.c:5689 only takes that path when
     * the bit is clear (`if (!(b_a & 0x40))`), and free_das_cache_entry walks
     * a child table for it.
     *
     * Writing width*height bytes into one of those overruns into whatever
     * follows, which is what crashed the game the moment a menu drew: menu
     * artwork is exactly the grouped kind. Skip them; world textures are the
     * simple kind and are all this needs. */
    const uint16_t kind = *(volatile uint16_t *)(uintptr_t)(blk + 0x0a);
    if (kind & 0x40) { g_dc_skipped++; return; }

    const uint16_t w = *(volatile uint16_t *)(uintptr_t)(blk + 0x0c);
    const uint16_t h = *(volatile uint16_t *)(uintptr_t)(blk + 0x0e);
    if (w == 0 || h == 0 || w > 1024 || h > 1024) { g_dc_skipped++; return; }

    uint8_t *px = (uint8_t *)(uintptr_t)(blk + 0x10);
    const uint32_t n = (uint32_t)w * (uint32_t)h;

    if (patternV)
        for (uint32_t i = 0; i < n; i++) px[i] = (uint8_t)((i / w) & 0xffu);
    else
        for (uint32_t i = 0; i < n; i++) px[i] = (uint8_t)((i % w) & 0xffu);

    g_dc_painted++;
}

/* Walk the whole status table and repaint every loaded entry. Returns the
 * number painted; zero means nothing was loaded yet and it is worth trying
 * again on a later tick. */
static uint32_t dc_paint_all(const struct roth_api_v1 *api, int patternV)
{
    const struct roth_game_ram_api_v1 *m = api->game_ram;
    g_dc_painted = 0;
    g_dc_skipped = 0;

    for (uint32_t i = 0; i < DC_STATUS_N; i++)
    {
        const uint8_t idx = m->u8(VA_DC_STATUS + i * 2u);
        if (idx >= DC_LOADED_MAX) continue;              /* not loaded */

        const uint32_t handle = m->u32(VA_DC_SLOTS + (uint32_t)idx * 6u);
        if (handle == 0) continue;

        /* handle -> block. Both are raw host pointers in the arena. */
        const uint32_t blk = *(volatile uint32_t *)(uintptr_t)handle;
        if (blk == 0) continue;

        dc_paint_block(blk, patternV);
    }
    return g_dc_painted;
}
