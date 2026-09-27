/* staticdump.inc.c -- included by oraclelog.c.
 *
 * Dumps, ONCE, the loaded map's geometry and objects as the ORIGINAL holds them
 * in memory. This is the measurement half of the rig: REMAROTH parses the same
 * .RAW and can emit the same two tables, so a diff says exactly which field of
 * which record we read differently.
 *
 * It is the answer to the four visual defects, none of which need the game to be
 * in play:
 *   - furniture facing   -> every object's rotation byte, beside its position
 *   - flat texture scale -> every sector's flags byte, which carries the two
 *                           2-bit CEIL/FLOOR scale fields
 *   - "ceilings too tall"-> every sector's ceiling and floor height
 *   - sky on ceilings    -> every sector's ceiling texture index
 *
 * WHERE THE RECORDS ARE. Both cited from the original's own scans:
 *   sectors: mark_geometry_records_by_id (raw_commands.c) walks "every SECTOR
 *            record (geom 0x90aa8; section geom+(u32[geom+4]&0xffff), stride
 *            0x1a, count u16[section-2])".
 *   objects: resolve_command_objects (raw_commands.c:318) walks an index array at
 *            objbuf+2; each nonzero index gives a group record whose byte[0] is a
 *            sub-count, with 0x10-stride sub-records from +2.
 *
 * The buffers are stored as RUNTIME pointers (base + VA), so each is converted
 * back to a VA with to_ptr(0) and read through the bounded accessors rather than
 * dereferenced raw.
 */

/* Sector record fields, RAW.md "Sectors Section". Offsets from the record base. */
#define SEC_CEIL_H      0x00   /* s16 */
#define SEC_FLOOR_H     0x02   /* s16 */
#define SEC_CEIL_TEX    0x06   /* u16 */
#define SEC_FLOOR_TEX   0x08   /* u16 */
#define SEC_FLAGS       0x0a   /* u8: bit2-3 CEIL scale, bit4-5 FLOOR scale     */
#define SEC_LIGHT       0x0b   /* u8                                            */
#define SEC_TEXMAP_OVR  0x0c   /* s8: TEXTURE_MAP_OVERRIDE                       */
#define SEC_FACE_COUNT  0x0d   /* u8 */
#define SEC_FACE_OFF    0x0e   /* u16 */
#define SEC_CEIL_SHX    0x10
#define SEC_CEIL_SHY    0x11
#define SEC_FLOOR_SHX   0x12
#define SEC_FLOOR_SHY   0x13
#define SEC_COMMAND_ID  0x14   /* u16 */
#define SEC_STRIDE      0x1a

/* Object record, 0x10 stride (GAME_inventory.md §2.5, "RR:28"). */
#define OBJ_X           0x00   /* s16 */
#define OBJ_Y           0x02   /* s16 */
#define OBJ_TEX_INDEX   0x04   /* u8  */
#define OBJ_TEX_SOURCE  0x05   /* u8  */
#define OBJ_ROTATION    0x06   /* u8  <-- the facing byte the whole angle question turns on */
#define OBJ_FLAGS       0x07   /* u8  */
#define OBJ_LIGHT       0x08   /* u8  */
#define OBJ_RENDERTYPE  0x09   /* u8  */
#define OBJ_Z           0x0a   /* s16 */
#define OBJ_COMMAND_ID  0x0e   /* u16 */
#define OBJ_STRIDE      0x10

/* Diagnostics, rate-limited: a silent early return is what hid the first failure. */
#define DUMPDIAG(...) do { static int n; if (n < 3) { n++; fprintf(stderr, "[oraclelog] dump not ready: "); fprintf(stderr, __VA_ARGS__); fputc(10, stderr); } } while (0)

#define VA_GEOM_BUF     0x90aa8u
#define VA_OBJ_BUF      0x90aa4u
#define VA_VIEW_CX      0x90a70u
#define VA_VIEW_CY      0x90a72u
#define VA_PLAYER_H     0x8c110u   /* player height, 2*meta[0x0a] */
#define VA_MAX_CLIMB    0x8c148u
#define VA_MIN_FIT      0x90be0u

/* THE PROJECTION. rwss_proj (renderer.c:5350) is a plain perspective divide,
 *
 *     screen = world * mul / depth + centre,  clamped to +/-0x3ffe
 *
 * and its own comment names the constants it models: "imul [0x8527c]/[0x85288];
 * idiv [0x85264]; add [0x909a*]". Those resolve to named globals:
 *
 *     0x8527c = g_view_params_block + 0x0c   (block at 0x85270, extent 0x18)
 *     0x85288 = g_perspective_scale
 *     0x85264 = g_view_clip_plane            (the depth divisor)
 *
 * The RATIO of the two scales is the vertical-to-horizontal projection
 * relationship -- which is what decides whether a correct-height ceiling reads
 * as too tall. The heights themselves are already proven identical to ours, so
 * this is the remaining candidate. Dumped raw, with the ratio worked out, rather
 * than reasoned about: reasoning about this renderer is what produced two wrong
 * answers already. */
#define VA_VIEW_PARAMS  0x85270u
#define VA_PERSP_SCALE  0x85288u
#define VA_CLIP_PLANE   0x85264u

/* THE MAP BUFFERS ARE NOT IN game_ram. The geometry and object buffers are
 * allocated from the DPMI arena at run time, so their stored pointers land well
 * outside the bounded game_ram window -- measured: geom resolves to 0x078ad388
 * while game_ram is a few megabytes. The bounded accessors therefore cannot read
 * them, and the first attempt read nsec=0 because of it.
 *
 * A mod runs in the game's own address space, so the stored value IS a usable
 * host pointer. These readers dereference it directly. That is deliberate and
 * confined to this file: every NAMED GLOBAL still goes through the bounded
 * accessors, and everything here is read-only, so the worst a bad pointer can do
 * is fault loudly rather than corrupt the thing being measured.
 */
static const uint8_t *g_map;   /* host pointer to the geometry buffer */

static uint8_t  R8 (uint32_t o) { return g_map[o]; }
static uint16_t R16(uint32_t o) { return (uint16_t)(g_map[o] | (g_map[o + 1] << 8)); }
static uint32_t R32(uint32_t o)
{
    return (uint32_t)g_map[o] | ((uint32_t)g_map[o+1] << 8)
         | ((uint32_t)g_map[o+2] << 16) | ((uint32_t)g_map[o+3] << 24);
}

static int static_dump(const struct roth_api_v1 *api)
{
    const struct roth_game_ram_api_v1 *m = api->game_ram;

    const uint32_t rawgeom = m->u32(VA_GEOM_BUF);
    if (rawgeom == 0) { DUMPDIAG("geometry buffer pointer still null"); return 0; }
    g_map = (const uint8_t *)(uintptr_t)rawgeom;

    /* section base + count, exactly as the original's own sector scan computes. */
    const uint32_t sec_base = R32(4) & 0xffffu;   /* offsets are buffer-relative */
    const uint32_t nsec     = sec_base >= 2 ? R16(sec_base - 2) : 0;
    if (nsec == 0 || nsec > 4096u)
    { DUMPDIAG("geom %08x sec_base %u nsec %u -- not ready", rawgeom, sec_base, nsec); return 0; }

    const char *path = getenv("ROTH_ORACLE_STATIC");
    if (path == NULL) path = "oracle_static.csv";
    FILE *f = fopen(path, "w");
    if (f == NULL) { DUMPDIAG("fopen %s failed", path); return 0; }

    /* The view/player parameters that bear on "ceilings look too tall". */
    fprintf(f, "# oracle static dump v1\n");
    fprintf(f, "view,center_x,%d\n", (int)(int16_t)m->u16(VA_VIEW_CX));
    fprintf(f, "view,center_y,%d\n", (int)(int16_t)m->u16(VA_VIEW_CY));
    fprintf(f, "view,player_height,%d\n", (int)(int16_t)m->u16(VA_PLAYER_H));
    fprintf(f, "view,max_climb,%d\n", (int)(int16_t)m->u16(VA_MAX_CLIMB));
    fprintf(f, "view,min_fit,%d\n", (int)(int16_t)m->u16(VA_MIN_FIT));

    /* The projection block, raw. Six dwords of view_params_block, the three of
     * g_perspective_scale, and the clip plane. */
    for (int i = 0; i < 6; i++)
        fprintf(f, "proj,view_params_%02x,%d\n", i * 4, (int)m->u32(VA_VIEW_PARAMS + (uint32_t)i * 4));
    for (int i = 0; i < 3; i++)
        fprintf(f, "proj,persp_scale_%02x,%d\n", i * 4, (int)m->u32(VA_PERSP_SCALE + (uint32_t)i * 4));
    fprintf(f, "proj,clip_plane,%d\n", (int)m->u32(VA_CLIP_PLANE));

    /* The two scales rwss_proj multiplies by, and their ratio -- the number that
     * matters. A ratio of 1 means square pixels; anything else is the aspect
     * correction the original applied and we may not. */
    {
        const int32_t mulA = (int32_t)m->u32(VA_VIEW_PARAMS + 0x0c);
        const int32_t mulB = (int32_t)m->u32(VA_PERSP_SCALE);
        fprintf(f, "proj,mul_8527c,%d\n", mulA);
        fprintf(f, "proj,mul_85288,%d\n", mulB);
        if (mulA != 0)
            fprintf(f, "proj,ratio_B_over_A,%.6f\n", (double)mulB / (double)mulA);
    }

    fprintf(f, "sec,idx,ceil_h,floor_h,ceil_tex,floor_tex,flags,"
               "ceil_scale,floor_scale,light,texmap_ovr,faces,"
               "ceil_shx,ceil_shy,floor_shx,floor_shy,cmd_id\n");
    for (uint32_t i = 0; i < nsec; i++)
    {
        const uint32_t r = sec_base + i * SEC_STRIDE;
        const uint8_t fl = R8(r + SEC_FLAGS);
        fprintf(f, "sec,%u,%d,%d,%u,%u,%02x,%u,%u,%u,%d,%u,%u,%u,%u,%u,%u\n",
                i,
                (int)(int16_t)R16(r + SEC_CEIL_H),
                (int)(int16_t)R16(r + SEC_FLOOR_H),
                (unsigned)R16(r + SEC_CEIL_TEX),
                (unsigned)R16(r + SEC_FLOOR_TEX),
                (unsigned)fl,
                (unsigned)((fl >> 2) & 3u),   /* CEIL_A/CEIL_B   */
                (unsigned)((fl >> 4) & 3u),   /* FLOOR_A/FLOOR_B */
                (unsigned)R8(r + SEC_LIGHT),
                (int)(int8_t)R8(r + SEC_TEXMAP_OVR),
                (unsigned)R8(r + SEC_FACE_COUNT),
                (unsigned)R8(r + SEC_CEIL_SHX), (unsigned)R8(r + SEC_CEIL_SHY),
                (unsigned)R8(r + SEC_FLOOR_SHX), (unsigned)R8(r + SEC_FLOOR_SHY),
                (unsigned)R16(r + SEC_COMMAND_ID));
    }

    /* Objects, walked the way resolve_command_objects walks them: an index array
     * at objbuf+2 whose entries point at group records, each a sub-count byte
     * followed by 0x10-stride sub-records. The group index is emitted too, since
     * a Realms object is addressed by (sector, index) on our side. */
    const uint8_t *objp = (const uint8_t *)(uintptr_t)m->u32(VA_OBJ_BUF);
    if (objp != NULL)
    {
        fprintf(f, "obj,group,sub,x,y,z,rotation,tex_index,tex_source,"
                   "flags,light,rendertype,cmd_id\n");
        for (uint32_t g = 0; g < nsec; g++)
        {
            const uint32_t idx = (uint16_t)(objp[2 + g*2] | (objp[3 + g*2] << 8));
            if (idx == 0) continue;
            const uint8_t *grp = objp + idx;
            const uint32_t n = grp[0];
            if (n == 0 || n > 255u) continue;
            for (uint32_t s = 0; s < n; s++)
            {
                const uint8_t *o = grp + 2 + s * OBJ_STRIDE;
                fprintf(f, "obj,%u,%u,%d,%d,%d,%u,%u,%u,%02x,%u,%02x,%u\n",
                        g, s,
                        (int)(int16_t)(uint16_t)(o[OBJ_X]|(o[OBJ_X+1]<<8)),
                        (int)(int16_t)(uint16_t)(o[OBJ_Y]|(o[OBJ_Y+1]<<8)),
                        (int)(int16_t)(uint16_t)(o[OBJ_Z]|(o[OBJ_Z+1]<<8)),
                        (unsigned)o[OBJ_ROTATION],
                        (unsigned)o[OBJ_TEX_INDEX],
                        (unsigned)o[OBJ_TEX_SOURCE],
                        (unsigned)o[OBJ_FLAGS],
                        (unsigned)o[OBJ_LIGHT],
                        (unsigned)o[OBJ_RENDERTYPE],
                        (unsigned)(uint16_t)(o[OBJ_COMMAND_ID]|(o[OBJ_COMMAND_ID+1]<<8)));
            }
        }
    }

    fclose(f);
    fprintf(stderr, "[oraclelog] static dump written: %u sectors -> %s\n",
            nsec, path);
    return 1;
}
