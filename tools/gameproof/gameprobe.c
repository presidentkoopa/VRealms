/* gameprobe.c -- ROTH.C mod: drive the ORIGINAL's own game-logic entry points and log every
 * observable effect, in the line format tools/gameproof/replay.cpp reproduces from the port.
 *
 * Built against roth_sdk.h only (see run.sh). NOT part of the engine. Writes its trace to
 * $PROOF_OUT. Nothing it writes may be committed: the trace is derived from retail data.
 *
 * What it does
 *   boot      skips every GDV, answers the main menu with "new game" (0xa), lets the original
 *             boot and run its own new-game block (game_core.c:821-833), logging the calls.
 *   S0        on the first gameplay frame: blocks the timer signal, dumps the whole state
 *             (flags, inventory, hands, queue, RNG) and snapshots game_ram + the flag bitmap +
 *             the DBASE100 image. Every test below starts from that snapshot, restored.
 *   A         every DBASE100 action id through eval_dialogue_record_by_id (what RAW 0x2b
 *             reaches, raw_commands.c:4705), then finish_dialogue_record_eval, then "driven"
 *             to the end: a pending choice menu is answered, a busy line is ended with the
 *             original's own voice_stream_pump. One variant per choice of the first menu.
 *   I         inventory entry points over every item id, from four starting inventories.
 *   R         cmd_if_not_item / cmd_give_item / cmd_remove_item / cmd_set_inventory_filter /
 *             run_command_dbase100_record on record bytes taken from the retail maps
 *             ($PROOF_RAWRECS), from the same starting inventories.
 *
 * While a test runs, the callees that belong to presentation or to the map (dialogue windows,
 * GDV, images, sound, music, the map's command chains, the item icon, the weapon HUD) are
 * STUBBED with fixed answers and logged; everything that is game logic runs the original body.
 * The stub answers are the replayer's sink answers; $PROOF_MODE picks the set (see mode_*).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "roth_sdk.h"

static FILE *g_out;
static const struct roth_api_v1 *g_api;
static int g_probing;          /* stubs active */
static int g_mode;             /* PROOF_MODE: bit0 windows answer 0, bit1 music chunk "freshly loaded" */
static int g_depth;

#define RAM (g_api->game_ram)
#define P8(a)  (*(volatile uint8_t  *)(uintptr_t)(a))
#define P16(a) (*(volatile uint16_t *)(uintptr_t)(a))
#define P32(a) (*(volatile uint32_t *)(uintptr_t)(a))

/* canonical addresses (g_names.h) */
enum {
    A_BASE = 0x81e1c, A_INVTAB = 0x81e20, A_DLGTAB = 0x81e24, A_BITMAP = 0x81e28, A_BITMAPSZ = 0x81e2c,
    A_VALRED = 0x81e30, A_EASY = 0x81e34, A_RNG = 0x81e38, A_RNGCD = 0x81e3a, A_SEED = 0x71364,
    A_QACTIVE = 0x81e18, A_QCOUNT = 0x81e3e, A_QUEUE = 0x81e42,
    A_CHCUR = 0x81ea2, A_CHREM = 0x81ea6, A_CHMODE = 0x81eaa, A_CHCOUNT = 0x81eae, A_TOPIC = 0x81eb2, A_CHIDX = 0x81eb6,
    A_BUSY = 0x83aea, A_CTX = 0x83115, A_FREEZE = 0x83125, A_VOICE = 0x8200c, A_CHSEL = 0x71370, A_CHSAVED = 0x8313d,
    A_INVCOUNT = 0x80c2c, A_SLOTS = 0x80c30, A_LEFT = 0x81038, A_RIGHT = 0x81044, A_ADAMSLOT = 0x8104c, A_ADAMID = 0x81054,
    A_AUTOSEL = 0x89f60, A_LASTITEM = 0x89fa8, A_PICKUP = 0x7fd84, A_SLIDE = 0x819bc,
    A_L83B20 = 0x83b20, A_L80B2C = 0x80b2c, A_PANEL = 0x7fec4, A_PANELX = 0x7fecc, A_POPUP = 0x7fec0,
    A_HEALTH = 0x8a0f0, A_DEATH = 0x7feb4, A_HPMAX = 0x7fe44, A_SEQSTATE = 0x83c4c, A_CHUNKID = 0x7f46c,
    A_MUSVOL = 0x71124, A_OVERLAY = 0x83c70, A_HUDDIRTY = 0x810c4,
    A_AMMOID = 0x811e4, A_AMMO = 0x83e6c, A_AMMOFLAG = 0x83e80, A_WEAPONITEM = 0x83e70, A_AMMOCAP = 0x83e74,
    A_IFLISTN = 0x89f64, A_IFLIST = 0x89f68, A_PENDING = 0x8a0dc, A_INTERRUPT = 0x8a268, A_DLGLATCH = 0x8a26c,
    A_GIVECTX = 0x8a134, A_ACTOBJ2 = 0x8a0fc,
};

static void ev(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
/* A test that logs more than EVENT_LIMIT events is a script that never ends (the original has no
 * guard: it would hang the game). The test is cut there, by a jump back to the test loop. */
#define EVENT_LIMIT 4000
#include <setjmp.h>
static jmp_buf g_runaway; static int g_events;
static void ev(const char *fmt, ...)
{
    if (!g_out) return;
    if (g_probing && ++g_events > EVENT_LIMIT) { g_depth = 0; fprintf(g_out, "RUNAWAY\n"); longjmp(g_runaway, 1); }
    va_list ap; va_start(ap, fmt);
    for (int i = 0; i < g_depth; i++) fputc(' ', g_out);
    vfprintf(g_out, fmt, ap); fputc('\n', g_out);
    va_end(ap);
}

static uint32_t crc32b(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xffffffffu;
    for (uint32_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1))); }
    return ~c;
}

static int slot_index(uint32_t ptr)
{
    if (ptr == 0) return -1;
    uint32_t base = (uint32_t)(uintptr_t)RAM->to_ptr(A_SLOTS);
    return (int)((ptr - base) / 4);
}
static uint32_t img_off(uint32_t ptr) { return ptr ? ptr - RAM->u32(A_BASE) : 0; }

/* ------------------------------------------------------------------ wraps: game logic (real) */

static uint32_t w_give_item(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t eax, uint32_t edx)
{ (void)api; ev("give id=%d ctx=%d {", (int)(int16_t)eax, edx != 0); g_depth++;
  uint32_t r = roth_next_give_item(c, eax, edx); g_depth--; ev("} give ret=%d", (int)r); return r; }
static uint32_t w_remove_item(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t eax)
{ (void)api; ev("remove id=%d {", (int)(int16_t)eax); g_depth++;
  uint32_t r = roth_next_remove_item(c, eax); g_depth--; ev("} remove ret=%d", (int)r); return r; }
static uint32_t w_query(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t eax, uint32_t edx)
{ (void)api; ev("query id=%d flags=%u {", (int)(int16_t)eax, edx); g_depth++;
  uint32_t r = roth_next_query_player_inventory(c, eax, edx); g_depth--; ev("} query ret=%u", r); return r; }
static uint32_t w_find(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t list, uint32_t mode, uint32_t cnt)
{ (void)api; ev("find n=%u flags=%u {", cnt, mode); g_depth++;
  uint32_t r = roth_next_find_or_autoselect_inventory_item(c, list, mode, cnt); g_depth--; ev("} find ret=%u", r); return r; }
static void w_filter(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t mode)
{ (void)api; ev("filter mode=%u {", mode); g_depth++; roth_next_set_inventory_list_filter(c, mode); g_depth--; ev("} filter"); }
static int32_t w_rng(struct roth_chain *c, const struct roth_api_v1 *api, int32_t count)
{ (void)api; int32_t r = roth_next_rng_next_index_for_count(c, count); ev("rng count=%d ret=%d", count, r); return r; }
static uint32_t w_prime_voice(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t clip)
{ (void)api; ev("voice clip=%u", clip); if (g_probing) return 0; return roth_next_prime_voice_clip(c, clip); }
static uint32_t w_read_line(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t dest, uint32_t maxlen, uint32_t off, uint32_t flag)
{ (void)api; g_depth++; uint32_t r = roth_next_read_next_dialogue_line(c, dest, maxlen, off, flag); g_depth--;
  ev("line off=%u max=%u flag=%u ret=%u", off, maxlen, flag, r); return r; }
static void w_force_end(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t a, uint32_t d)
{ (void)api; ev("forceend %u %u", a, d); roth_next_dialogue_voice_force_end(c, a, d); }
static void w_clamp(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t a, uint32_t d)
{ (void)api; ev("health amount=%u max=%u", a, d); roth_next_advance_clamp_8a0f0(c, a, d); }
static void w_activate(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t slot, uint32_t id)
{ (void)api; ev("weapon slot=%d id=%d {", slot_index(slot), (int)id); g_depth++; roth_next_activate_weapon_item(c, slot, id); g_depth--; ev("} weapon"); }
static void w_weaponlist(struct roth_chain *c, const struct roth_api_v1 *api)
{ (void)api; ev("weaponlist"); roth_next_rebuild_weapon_inventory_list(c); }
static void w_pickup(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t x, uint32_t y, uint32_t z, uint16_t id)
{ (void)api; ev("pickupfly x=%d y=%d z=%d id=%u", (int)x, (int)y, (int)z, id); roth_next_begin_item_pickup_lock(c, x, y, z, id); }
static void w_tabclear(struct roth_chain *c, const struct roth_api_v1 *api)
{ (void)api; ev("tabclear"); roth_next_clear_dual_array_80afc(c); }
static void w_weaponhud(struct roth_chain *c, const struct roth_api_v1 *api)
{ (void)api; ev("weaponhud {"); g_depth++; roth_next_reset_weapon_hud(c); g_depth--; ev("} weaponhud"); }
static uint32_t w_eval_cond(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t rec, uint32_t flag, uint32_t *b, uint32_t *cx)
{ (void)api; ev("evalblock off=%u mode=%u {", img_off(rec), flag); g_depth++;
  uint32_t r = roth_next_eval_dialogue_record_condition_with_cleanup(c, rec, flag, b, cx); g_depth--; ev("} evalblock ret=%d", (int)r); return r; }

/* ------------------------------------------------------------------ wraps: presentation / map (stubbed while probing) */

static void w_icon(struct roth_chain *c, const struct roth_api_v1 *api)
{ (void)api; ev("icon"); if (g_probing) { RAM->set_u32(A_SLIDE, 0); return; }   /* inventory.c:1464, the one logic store */
  roth_next_update_selected_item_icon(c); }
static void w_render_hud(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t a, uint32_t b)
{ (void)api; if (g_probing) return; roth_next_render_weapon_hud(c, a, b); }
static void w_panelrefresh(struct roth_chain *c, const struct roth_api_v1 *api)
{ (void)api; ev("panelrefresh"); if (g_probing) return; roth_next_refresh_inventory_panel(c); }
static uint32_t w_window(struct roth_chain *c, const struct roth_api_v1 *api, uint8_t *t)
{ (void)api; uint32_t n = (uint32_t)strnlen((const char *)t, 0xc00);
  if (g_probing) { uint32_t r = (g_mode & 1) ? 0 : 1; ev("window len=%u crc=%08x ret=%u", n, crc32b(t, n), r); return r; }
  uint32_t r = roth_next_dbase100_open_dialogue_window(c, t); ev("window len=%u crc=%08x ret=%u", n, crc32b(t, n), r); return r; }
static uint32_t w_window_alt(struct roth_chain *c, const struct roth_api_v1 *api, uint8_t *t)
{ (void)api; uint32_t n = (uint32_t)strnlen((const char *)t, 0x3fc);
  if (g_probing) { uint32_t r = (g_mode & 1) ? 0 : 1; ev("windowalt len=%u crc=%08x ret=%u", n, crc32b(t, n), r); return r; }
  uint32_t r = roth_next_dbase100_open_dialogue_window_alt(c, t); ev("windowalt len=%u crc=%08x ret=%u", n, crc32b(t, n), r); return r; }
static uint32_t w_gdv(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t a, uint32_t b, uint32_t d, uint32_t e)
{ (void)c; (void)api; (void)a; (void)b; (void)d; (void)e; return 0; }
static uint32_t w_rec_gdv(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t rec)
{ (void)c; (void)api; uint32_t base = RAM->u32(A_BASE);
  ev("gdv index=%d", base ? (int)((rec - (base + P32(base + 0x24))) / 0x14) : -1); return 0; }
static uint32_t w_menu(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t a)
{ (void)c; (void)api; (void)a; return 0xa; }
static uint32_t w_image(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t op)
{ (void)api; ev("image op=%u", op); if (g_probing) return 0; return roth_next_show_fullscreen_image(c, op); }
static void w_exit_overlay(struct roth_chain *c, const struct roth_api_v1 *api)
{ (void)api; ev("exitoverlay"); if (g_probing) return; roth_next_exit_cutscene_overlay_mode(c); }
static uint32_t w_sfx(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t id, uint32_t param)
{ (void)api; ev("sfx id=%u param=%u", id, param); if (g_probing) return 0; return roth_next_play_sound_effect(c, id, param); }
static uint32_t w_db300(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t index)
{ (void)api; if (g_probing) { uint32_t r = (g_mode & 2) ? 0 : 1; ev("db300 index=%u ret=%u", index, r); return r; }
  uint32_t r = roth_next_load_dbase300_chunk(c, index); ev("db300 index=%u ret=%u", index, r); return r; }
static uint32_t w_seqfinal(struct roth_chain *c, const struct roth_api_v1 *api)
{ (void)api; ev("seqfinal"); if (g_probing) return 0; return roth_next_finalize_audio_sequence_ref(c); }
static uint32_t w_seqevent(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t e)
{ (void)api; ev("seqevent %u", e); if (g_probing) return 0; return roth_next_emit_music_sequence_event(c, e); }
static void w_seqprocess(struct roth_chain *c, const struct roth_api_v1 *api)
{ (void)api; ev("seqprocess"); if (g_probing) return; roth_next_process_audio_sequence_chunk(c); }
static int32_t w_resetchain(struct roth_chain *c, const struct roth_api_v1 *api, uint16_t ax)
{ (void)api; ev("resetchain %u", ax); if (g_probing) return 0; return roth_next_reset_command_chain_no_source(c, ax); }
static int32_t w_runobj(struct roth_chain *c, const struct roth_api_v1 *api, uint16_t ax)
{ (void)api; ev("runobj %u", ax); if (g_probing) return 0; return roth_next_run_object_commands_by_id(c, ax); }

static void reg(const struct roth_api_v1 *api, struct roth_registrar_v1 *r)
{
    (void)api;
#define OV(fn, cb) do { if (roth_override(r, ROTH_FN_##fn, cb, 200) != 0) fprintf(stderr, "[gameprobe] override refused: " #fn "\n"); } while (0)
    OV(play_gdv_cutscene, w_gdv); OV(play_record_gdv_cutscene, w_rec_gdv); OV(run_main_menu, w_menu);
    OV(give_item, w_give_item); OV(remove_item, w_remove_item); OV(query_player_inventory, w_query);
    OV(find_or_autoselect_inventory_item, w_find); OV(set_inventory_list_filter, w_filter);
    OV(rng_next_index_for_count, w_rng); OV(prime_voice_clip, w_prime_voice); OV(read_next_dialogue_line, w_read_line);
    OV(dialogue_voice_force_end, w_force_end); OV(advance_clamp_8a0f0, w_clamp);
    OV(activate_weapon_item, w_activate); OV(rebuild_weapon_inventory_list, w_weaponlist);
    OV(begin_item_pickup_lock, w_pickup); OV(clear_dual_array_80afc, w_tabclear); OV(reset_weapon_hud, w_weaponhud);
    OV(eval_dialogue_record_condition_with_cleanup, w_eval_cond);
    OV(update_selected_item_icon, w_icon); OV(render_weapon_hud, w_render_hud); OV(refresh_inventory_panel, w_panelrefresh);
    OV(dbase100_open_dialogue_window, w_window); OV(dbase100_open_dialogue_window_alt, w_window_alt);
    OV(show_fullscreen_image, w_image); OV(exit_cutscene_overlay_mode, w_exit_overlay);
    OV(play_sound_effect, w_sfx); OV(load_dbase300_chunk, w_db300); OV(finalize_audio_sequence_ref, w_seqfinal);
    OV(emit_music_sequence_event, w_seqevent); OV(process_audio_sequence_chunk, w_seqprocess);
    OV(reset_command_chain_no_source, w_resetchain); OV(run_object_commands_by_id, w_runobj);
#undef OV
}

/* ------------------------------------------------------------------ engine entry points, by address (run.sh: nm) */

static uint32_t sym(const char *name)
{
    static char *tab; if (!tab) { const char *p = getenv("PROOF_SYMS"); FILE *f = p ? fopen(p, "r") : 0;
        if (!f) { fprintf(stderr, "[gameprobe] PROOF_SYMS missing\n"); exit(3); }
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        tab = calloc(1, (size_t)sz + 1); size_t n = fread(tab, 1, (size_t)sz, f); tab[n] = 0; fclose(f); }
    char key[128]; snprintf(key, sizeof key, " %s\n", name);
    char *at = strstr(tab, key);
    if (!at) { fprintf(stderr, "[gameprobe] no symbol %s\n", name); exit(3); }
    while (at > tab && at[-1] != '\n') at--;
    return (uint32_t)strtoul(at, 0, 16);
}
#define FN(ret, name, ...) ((ret (*)(__VA_ARGS__))(uintptr_t)sym(#name))

/* ------------------------------------------------------------------ state */

#define RAM_LO 0x70000u   /* the data arena (obj3) starts here; below it nothing is mapped */
struct snap { uint8_t *ram, *bits, *img; };
static struct snap g_s0, g_base[8], *g_cur;      /* S0; the starting states built from it; the one in use */
static uint32_t g_ramsz, g_bitsz, g_imgsz;

static void snapshot(struct snap *sn)
{
    g_ramsz = RAM->size() - RAM_LO; g_bitsz = RAM->u32(A_BITMAPSZ); g_imgsz = P32(RAM->u32(A_BASE) + 8);
    sn->ram = malloc(g_ramsz); RAM->read_block(RAM_LO, sn->ram, g_ramsz);
    sn->bits = malloc(g_bitsz ? g_bitsz : 1); memcpy(sn->bits, (void *)(uintptr_t)RAM->u32(A_BITMAP), g_bitsz);
    sn->img = malloc(g_imgsz); memcpy(sn->img, (void *)(uintptr_t)RAM->u32(A_BASE), g_imgsz);
}
static void restore(const struct snap *sn)
{
    RAM->write_block(RAM_LO, sn->ram, g_ramsz);
    memcpy((void *)(uintptr_t)RAM->u32(A_BITMAP), sn->bits, g_bitsz);
    memcpy((void *)(uintptr_t)RAM->u32(A_BASE), sn->img, g_imgsz);
}

/* The state both sides print. `full`: everything; else flags, slots, the equip counter and the
 * DBASE100 image as differences from the starting state in use. */
static void dump_state(const char *tag, int full)
{
    uint32_t bits = RAM->u32(A_BITMAP), nb = RAM->u32(A_BITMAPSZ);
    fprintf(g_out, "%s flags", tag);
    for (uint32_t i = 0; i < nb * 8; i++) {
        int now = (P8(bits + (i >> 3)) >> (i & 7)) & 1, was = full ? 0 : (g_cur->bits[i >> 3] >> (i & 7)) & 1;
        if (now != was) fprintf(g_out, " %c%u", now ? '+' : '-', i);
    }
    fprintf(g_out, "\n%s inv count=%d left=%d right=%d adam=%d/%d auto=%u easy=%u last=%d pickup=%u/%d slide=%d slots", tag,
        (int)RAM->u32(A_INVCOUNT), slot_index(RAM->u32(A_LEFT)), slot_index(RAM->u32(A_RIGHT)),
        slot_index(RAM->u32(A_ADAMSLOT)), (int)RAM->u32(A_ADAMID), RAM->u32(A_AUTOSEL), RAM->u32(A_EASY),
        (int)RAM->u32(A_LASTITEM), RAM->u32(A_PICKUP), (int)RAM->u32(A_PICKUP + 0x1c), (int)RAM->u32(A_SLIDE));
    for (int i = 0; i < 256; i++) { uint16_t id = RAM->u16(A_SLOTS + i * 4), q = RAM->u16(A_SLOTS + i * 4 + 2);
        const uint8_t *b = g_cur->ram + (A_SLOTS - RAM_LO) + i * 4;
        if (full ? (id || q) : (id != (uint16_t)(b[0] | b[1] << 8) || q != (uint16_t)(b[2] | b[3] << 8))) fprintf(g_out, " %d:%04x:%u", i, id, q); }
    fprintf(g_out, "\n%s dlg active=%d qcount=%d", tag, (int)RAM->u32(A_QACTIVE), (int)RAM->u32(A_QCOUNT));
    for (int i = 0; i < 8 && i < (int)RAM->u32(A_QCOUNT); i++)
        fprintf(g_out, " q%d=%u/%d/%u", i, img_off(RAM->u32(A_QUEUE + i * 12)), (int)RAM->u32(A_QUEUE + i * 12 + 4), RAM->u32(A_QUEUE + i * 12 + 8));
    fprintf(g_out, " ch=%u/%d/%u/%d topic=%u idx", img_off(RAM->u32(A_CHCUR)), (int)RAM->u32(A_CHREM), RAM->u32(A_CHMODE),
        (int)RAM->u32(A_CHCOUNT), RAM->u32(A_TOPIC));
    { int n = RAM->u32(A_CHCUR) ? (int)RAM->u32(A_CHCOUNT) : 0; if (n > 16) n = 16;
      for (int i = 0; i < n; i++) fprintf(g_out, " %d", (int)RAM->u32(A_CHIDX + i * 4)); }
    fprintf(g_out, " drain=%d busy=%d ctx=%d freeze=%d voice=%d sel=%d valred=%d\n", (int)RAM->u32(A_CHIDX + 0x40),
        (int)RAM->u32(A_BUSY), (int)RAM->u32(A_CTX), (int)RAM->u32(A_FREEZE), (int)RAM->u32(A_VOICE),
        (int)RAM->u32(A_CHSEL), (int)RAM->u32(A_VALRED));
    fprintf(g_out, "%s rng seed=%04x rng=%04x cd=%d\n", tag, RAM->u16(A_SEED), RAM->u16(A_RNG), (int)RAM->u32(A_RNGCD));
    fprintf(g_out, "%s ext l83b20=%d l80b2c=%d panel=%d/%d hp=%d death=%d seq=%d hud=%d ammo=%d/%d/%d img", tag,
        (int)RAM->u32(A_L83B20), (int)RAM->u32(A_L80B2C), (int)RAM->u32(A_PANEL), (int)RAM->u32(A_PANELX),
        (int)RAM->u32(A_HEALTH), (int)RAM->u32(A_DEATH), (int)RAM->u32(A_SEQSTATE),
        (int)(RAM->u32(A_HUDDIRTY) - *(uint32_t *)(g_cur->ram + A_HUDDIRTY - RAM_LO)),
        (int)RAM->u32(A_AMMOID), (int)RAM->u32(A_AMMO), (int)RAM->u32(A_AMMOFLAG));
    /* the DBASE100 image: bytes that differ (the item "seen" bits, the gallery bits) */
    { const uint8_t *img = (const uint8_t *)(uintptr_t)RAM->u32(A_BASE); int shown = 0;
      for (uint32_t i = 0; i < g_imgsz && shown < 24; i++) if (img[i] != g_cur->img[i]) { fprintf(g_out, " %u:%02x", i, img[i]); shown++; } }
    fputc('\n', g_out);
}

/* ------------------------------------------------------------------ the drive: bring a started conversation to its end */

/* After an entry returned: answer a pending choice menu (pick `pick` at the first one, 0 after),
 * end a busy line with the original's own per-frame voice_stream_pump (audio.c:1299-1327). */
static void drive(int pick)
{
    for (int step = 0; step < 200; step++) {
        if (RAM->u32(A_CHCUR) != 0) {
            int n = (int)RAM->u32(A_CHCOUNT), k = pick < n ? pick : 0; pick = 0;
            ev("pick %d of %d {", k, n); g_depth++;
            RAM->set_u32(A_CHSAVED, (uint32_t)k);                          /* dialogue_ui.c:224 stores the picked row here */
            FN(void, close_dialogue_and_run_branch, void)();
            g_depth--; ev("}");
        } else if (RAM->u32(A_BUSY) != 0 || RAM->u32(A_QCOUNT) != 0) {
            ev("pump {"); g_depth++;
            FN(void, voice_stream_pump, void)();
            g_depth--; ev("}");
        } else return;
    }
    ev("DRIVE-LIMIT");
}

/* Start a test from starting state `b`: bits 0-1 = the inventory setup (setup_inventory), bit 2 =
 * every flag set. Built once from S0 with the original's own give_item, then restored each time. */
static void setup_inventory(int k);
static void begin(int b)
{
    g_probing = 1; g_depth = 0; g_events = 0;
    if (!g_base[b].ram) {
        restore(&g_s0); g_cur = &g_s0;
        setup_inventory(b & 3);
        if (b & 4) memset((void *)(uintptr_t)RAM->u32(A_BITMAP), 0xff, RAM->u32(A_BITMAPSZ));
        if (g_mode & 4) RAM->set_u32(A_SEQSTATE, 2);          /* "the music is faded out" (dbase100.c:310, 794) */
        snapshot(&g_base[b]);
    }
    g_cur = &g_base[b]; restore(g_cur); g_events = 0;
}

/* ------------------------------------------------------------------ section A: the DBASE100 actions */

/* Section A's starting states ("var"): 0 = S0, 1 = every flag set, 2 = inventory setup 2,
 * 3 = every flag set + the full inventory (setup 3). */
static const int kVarBase[4] = { 0, 4, 2, 7 };

static void section_actions(volatile int lo, volatile int hi)
{
    for (volatile int id = lo; id <= hi; id++) for (volatile int var = 0; var < 4; var++) {
        volatile int variants = 1;
        for (volatile int v = 0; v < variants; v++) {
            begin(kVarBase[var]);
            fprintf(g_out, "A %d var=%d pick=%d\n", id, var, v);
            if (setjmp(g_runaway)) continue;
            uint32_t r = FN(uint32_t, eval_dialogue_record_by_id, uint32_t)((uint32_t)id);
            ev("= %d", (int)r);
            if (v == 0 && RAM->u32(A_CHCUR) != 0) { variants = (int)RAM->u32(A_CHCOUNT); if (variants > 16) variants = 16; }
            FN(void, finish_dialogue_record_eval, void)();
            drive(v);
            dump_state("S", 0);
        }
    }
}

/* ------------------------------------------------------------------ section B: the item trigger blocks */

/* Every trigger block of every item record through eval_dialogue_record_condition_with_cleanup
 * (dbase100.c:569-576), the entry the inventory, examine and topic code use, in its three modes:
 * 0 run, 1 run with flag 1, 2 scan only. From S0 and from "every flag set + full inventory". */
static void section_blocks(void)
{
    uint32_t (*evalc)(uint32_t, uint32_t, uint32_t *, uint32_t *) = FN(uint32_t, eval_dialogue_record_condition_with_cleanup, uint32_t, uint32_t, uint32_t *, uint32_t *);
    int n = (int)P32(RAM->u32(A_BASE) + 0x10);
    for (volatile int id = 1; id <= n; id++) {
        uint32_t off = P32(RAM->u32(A_INVTAB) + (uint32_t)id * 4);
        if (off == 0) continue;
        for (volatile uint32_t blk = off + 0x14; ; ) {
            uint32_t hdr = P32(RAM->u32(A_BASE) + blk), size = hdr & 0xffffff;
            if (size < 4) break;
            for (volatile int mode = 0; mode < 3; mode++) for (volatile int b = 0; b < 8; b += 7) {
                begin(b);
                fprintf(g_out, "B item=%d block=%u trigger=%u mode=%d base=%d\n", (int)id, (unsigned)blk, hdr >> 24, (int)mode, (int)b);
                if (setjmp(g_runaway)) continue;
                uint32_t r = evalc(RAM->u32(A_BASE) + blk, (uint32_t)mode, 0, 0);
                ev("= %d", (int)r);
                drive(0);
                dump_state("S", 0);
            }
            blk += (size >> 2) << 2;
        }
    }
}

/* ------------------------------------------------------------------ section I: inventory */

/* Starting inventories, built with the original's own give_item from S0. Kept identical in replay.cpp. */
static void setup_inventory(int k)
{
    int g_nitems = (int)P32(RAM->u32(A_BASE) + 0x10);
    uint32_t (*give)(uint32_t, uint32_t) = FN(uint32_t, give_item, uint32_t, uint32_t);
    FILE *keep = g_out; g_out = 0;                 /* setup is not part of the trace */
    if (k == 1) { for (int id = 1; id <= g_nitems; id += 3) give((uint32_t)id, 0); }
    if (k == 2) { for (int id = g_nitems; id >= 1; id -= 2) give((uint32_t)id, 0); for (int id = 2; id <= g_nitems; id += 5) give((uint32_t)id, 0); }
    if (k == 3) { for (int rep = 0; rep < 4 && RAM->u32(A_INVCOUNT) < 0x100; rep++) for (int id = 1; id <= g_nitems && RAM->u32(A_INVCOUNT) < 0x100; id++) give((uint32_t)id, 0); }
    g_out = keep;
}

static void section_inventory(void)
{
    uint32_t base = RAM->u32(A_BASE); int g_nitems = (int)P32(base + 0x10);
    for (int k = 0; k < 4; k++) {
        begin(k);
        fprintf(g_out, "I setup=%d\n", k); dump_state("S", 1);
        for (volatile int id = 0; id <= g_nitems + 1; id++) {
            static const char *const opname[] = { "give", "query0", "query1", "query2", "query3", "remove", "find0", "find1", "give+remove", "filter0", "filter1" };
            for (volatile int op = 0; op < 11; op++) {
                if (op >= 9 && id > 1) continue;
                begin(k);
                fprintf(g_out, "I setup=%d id=%d op=%s\n", k, id, opname[op]);
                if (setjmp(g_runaway)) continue;
                int32_t list[3] = { id, id + 1, id + 7 };
                switch (op) {
                case 0: if (id == 0) { ev("skipped: give 0 reads its context"); break; }
                        ev("= %d", (int)FN(uint32_t, give_item, uint32_t, uint32_t)((uint32_t)id, 0)); break;
                case 1: case 2: case 3: case 4:
                        ev("= %d", (int)FN(uint32_t, query_player_inventory, uint32_t, uint32_t)((uint32_t)id, (uint32_t)(op - 1))); break;
                case 5: ev("= %d", (int)FN(uint32_t, remove_item, uint32_t)((uint32_t)id)); break;
                case 6: case 7:
                        ev("= %d", (int)FN(uint32_t, find_or_autoselect_inventory_item, uint32_t, uint32_t, uint32_t)((uint32_t)(uintptr_t)list, (uint32_t)(op - 6), 3)); break;
                case 8: if (id == 0) { ev("skipped: give 0 reads its context"); break; }
                        ev("= %d", (int)FN(uint32_t, give_item, uint32_t, uint32_t)((uint32_t)id, 0));
                        ev("= %d", (int)FN(uint32_t, give_item, uint32_t, uint32_t)((uint32_t)id, 0));
                        ev("= %d", (int)FN(uint32_t, remove_item, uint32_t)((uint32_t)id));
                        ev("= %d", (int)FN(uint32_t, query_player_inventory, uint32_t, uint32_t)((uint32_t)id, 1));
                        ev("= %d", (int)FN(uint32_t, remove_item, uint32_t)((uint32_t)id));
                        ev("= %d", (int)FN(uint32_t, remove_item, uint32_t)((uint32_t)id)); break;
                case 9: case 10:
                        FN(void, set_inventory_list_filter, uint32_t)((uint32_t)(op - 9) ^ (uint32_t)id);
                        ev("= %d", (int)FN(uint32_t, query_player_inventory, uint32_t, uint32_t)(1, 1));
                        FN(void, set_inventory_list_filter, uint32_t)(1); break;
                }
                dump_state("S", 0);
            }
        }
    }
}

/* ------------------------------------------------------------------ section R: RAW command records */

static void section_raw(void)
{
    const char *path = getenv("PROOF_RAWRECS"); FILE *f = path ? fopen(path, "r") : 0;
    if (!f) { fprintf(g_out, "R none\n"); return; }
    char line[256];
    static uint8_t rec[32], pend[32];
    while (fgets(line, sizeof line, f)) {
        volatile unsigned op, mod, f6, key, pendkey; unsigned n;
        if (sscanf(line, "%x %x %x %u %u %u", &op, &mod, &f6, &key, &pendkey, &n) != 6) continue;
        /* 0x29 runs twice: with no context, and with a world object as cmd_give_item's context
         * (0x8a134, raw_commands.c:4801-4803): x 100, y -200, z 64, showing item 15's world id.
         * A 0x29 with key 0 gives "that object" and reads the context, so it only runs with one. */
        for (volatile int withctx = 0; withctx < (op == 0x29 ? 2 : 1); withctx++)
        for (volatile int k = 0; k < 4; k++) {
            if (op == 0x29 && key == 0 && !withctx) continue;
            begin(k);
            memset(rec, 0, sizeof rec); memset(pend, 0, sizeof pend);
            rec[2] = (uint8_t)mod; rec[3] = (uint8_t)op; rec[6] = (uint8_t)f6; rec[8] = (uint8_t)key; rec[9] = (uint8_t)(key >> 8);
            pend[3] = 0x2b; pend[6] = 0x10; pend[8] = (uint8_t)pendkey; pend[9] = (uint8_t)(pendkey >> 8);
            /* the chain state a fresh chain starts with (raw_commands.c:1313-1328) */
            RAM->set_u32(A_PENDING, pendkey ? (uint32_t)(uintptr_t)pend : 0); RAM->set_u8(A_INTERRUPT, 0); RAM->set_u8(A_DLGLATCH, 0);
            RAM->set_u32(A_IFLISTN, 0); RAM->set_u32(A_GIVECTX, 0); RAM->set_u32(A_ACTOBJ2, 0);
            if (withctx) {
                static uint8_t obj[16]; uint32_t base = RAM->u32(A_BASE);
                uint16_t rid = P16(base + P32(RAM->u32(A_INVTAB) + 15 * 4) + 2);
                memset(obj, 0, sizeof obj);
                obj[0] = 100; obj[2] = (uint8_t)(-200 & 0xff); obj[3] = 0xff; obj[4] = (uint8_t)rid; obj[5] = (uint8_t)(rid >> 8); obj[10] = 64;
                RAM->set_u32(A_GIVECTX, (uint32_t)(uintptr_t)obj);
            }
            if (op == 0x27 && (f6 & 8) && (f6 & 4)) {         /* a list query: stage the list the map's appends would have built */
                RAM->set_u32(A_IFLIST, key); RAM->set_u32(A_IFLIST + 4, key + 1); RAM->set_u32(A_IFLISTN, 2); }
            fprintf(g_out, "R op=%02x mod=%02x f6=%02x key=%u pend=%u ctx=%d setup=%d\n", op, mod, f6, key, pendkey, (int)withctx, (int)k);
            if (setjmp(g_runaway)) continue;
            int32_t r = 0;
            switch (op) {
            case 0x27: r = FN(int32_t, cmd_if_not_item, uint32_t)((uint32_t)(uintptr_t)rec); break;
            case 0x29: r = FN(int32_t, cmd_give_item, uint32_t)((uint32_t)(uintptr_t)rec); break;
            case 0x2a: r = FN(int32_t, cmd_remove_item, uint32_t)((uint32_t)(uintptr_t)rec); break;
            case 0x42: r = FN(int32_t, cmd_set_inventory_filter, uint32_t)((uint32_t)(uintptr_t)rec); break;
            case 0x2b: r = FN(int32_t, run_command_dbase100_record, uint32_t)((uint32_t)(uintptr_t)rec); break;
            }
            uint32_t p = RAM->u32(A_PENDING);
            ev("= %d mod=%02x pendmod=%02x pending=%d interrupt=%u latch=%u listn=%d", r == 0 ? 0 : (r == -1 ? -1 : 1), rec[2], pend[2],
               p == 0 ? 0 : 1, RAM->u8(A_INTERRUPT), RAM->u8(A_DLGLATCH), (int)RAM->u32(A_IFLISTN));
            /* the chain's tail (raw_commands.c:1313-1315) is the port's Fire; here only what 0x2b started is driven */
            FN(void, finish_dialogue_record_eval, void)();
            drive(0);
            dump_state("S", 0);
        }
    }
    fclose(f);
}

/* ------------------------------------------------------------------ section Q: three 0x2b in one chain */

/* Two chains with nothing ending the first one's line in between, as when the player sets off a
 * second trigger while a line is still up: chain 1 fires action id through
 * run_command_dbase100_record and ends (the chain tail, raw_commands.c:1313-1315); chain 2 fires
 * id+1 and id+2. The second chain's first 0x2b sets the queue-active latch again while the dialogue
 * is busy, which is where the queue is drained and the line force-ended (dbase100.c:452-480). */
static void section_queue(volatile int lo, volatile int hi)
{
    static uint8_t rec[3][32];
    int n = (int)P32(RAM->u32(A_BASE) + 0x18);
    for (volatile int id = lo; id <= hi; id++) for (volatile int b = 0; b < 8; b += 7) {
        begin(b);
        RAM->set_u32(A_PENDING, 0); RAM->set_u8(A_INTERRUPT, 0); RAM->set_u8(A_DLGLATCH, 0);
        fprintf(g_out, "Q %d base=%d\n", (int)id, (int)b);
        if (setjmp(g_runaway)) continue;
        for (int j = 0; j < 3; j++) {
            unsigned key = (unsigned)((id - 1 + j) % n) + 1;
            memset(rec[j], 0, 32); rec[j][3] = 0x2b; rec[j][6] = (uint8_t)(j == 1 ? 0x10 : 0); rec[j][8] = (uint8_t)key; rec[j][9] = (uint8_t)(key >> 8);
            int32_t r = FN(int32_t, run_command_dbase100_record, uint32_t)((uint32_t)(uintptr_t)rec[j]);
            ev("= %d mod=%02x latch=%u", (int)r, rec[j][2], RAM->u8(A_DLGLATCH));
            if (j == 0) { FN(void, finish_dialogue_record_eval, void)(); RAM->set_u8(A_DLGLATCH, 0); RAM->set_u32(A_AUTOSEL, 0); ev("chain end"); }
        }
        FN(void, finish_dialogue_record_eval, void)();
        drive(0);
        dump_state("S", 0);
    }
}

/* ------------------------------------------------------------------ frame hook */

static int g_done;
static void frame(const struct roth_api_v1 *api)
{
    if (g_done || api->game_ram->u8(0x7674a) != 1) return;
    g_done = 1;
    { sigset_t t; sigemptyset(&t); sigaddset(&t, SIGALRM); sigaddset(&t, SIGVTALRM); sigaddset(&t, SIGPROF);
      sigprocmask(SIG_BLOCK, &t, 0); }                                   /* no timer tick from here on */
    const char *what = getenv("PROOF_SECTIONS"); if (!what || !*what) what = "ABQIR";
    int lo = 1, hi = (int)P32(RAM->u32(A_BASE) + 0x18);
    { const char *s = getenv("PROOF_ACTIONS"); if (s) sscanf(s, "%d %d", &lo, &hi); }
    fprintf(g_out, "ENV mode=%d actions=%u items=%u bitmap=%u seq=%d chunk=%u musvol=%u overlay=%d panel=%d popup=%d hpmax=%u plan=%s %d %d\n", g_mode,
        P32(RAM->u32(A_BASE) + 0x18), P32(RAM->u32(A_BASE) + 0x10), RAM->u32(A_BITMAPSZ), (int)RAM->u32(A_SEQSTATE),
        RAM->u32(A_CHUNKID), RAM->u8(A_MUSVOL), (int)RAM->u32(A_OVERLAY), (int)RAM->u32(A_PANEL), (int)RAM->u32(A_POPUP), RAM->u32(A_HPMAX), what, lo, hi);
    /* What the original's loader did to the DBASE100 image: resolve_dbase100_sound_ids
     * (audio.c:1898-1924) rewrites the sound operands of the item records in place. That is the
     * sound system's work, not the interpreter's; the replayer applies these words as given. */
    { FILE *f = fopen("DBASE100.DAT", "rb"); uint32_t base = RAM->u32(A_BASE), n = P32(base + 8);
      if (f) { for (uint32_t off = 0; off + 4 <= n; off += 4) { uint32_t w = 0; if (fread(&w, 4, 1, f) != 1) break;
                 if (w != P32(base + off)) fprintf(g_out, "PATCH %u %08x\n", off, P32(base + off)); } fclose(f); }
      else fprintf(g_out, "PATCH-UNKNOWN (DBASE100.DAT not readable from the game folder)\n"); }
    snapshot(&g_s0); g_cur = &g_s0;
    dump_state("S0", 1);
    if (strchr(what, 'A')) section_actions(lo, hi);
    if (strchr(what, 'B')) section_blocks();
    if (strchr(what, 'I')) section_inventory();
    if (strchr(what, 'Q')) section_queue(lo, hi);
    if (strchr(what, 'R')) section_raw();
    fprintf(g_out, "END\n"); fclose(g_out);
    fprintf(stderr, "[gameprobe] done\n");
    _Exit(0);
}

static void on_load(const struct roth_api_v1 *api)
{
    g_api = api;
    const char *s = getenv("PROOF_OUT"); g_out = fopen(s ? s : "/tmp/gameprobe.trace", "w");
    if ((s = getenv("PROOF_MODE"))) g_mode = atoi(s);
    if (g_out) { if (getenv("PROOF_LINEBUF")) setvbuf(g_out, 0, _IOLBF, 0); fprintf(g_out, "BOOT\n"); }
}

static const struct roth_plugin_info_v1 INFO = {
    .abi_major = ROTH_ABI_MAJOR, .abi_minor = ROTH_ABI_MINOR, .struct_size = sizeof(struct roth_plugin_info_v1),
    .id = "io.github.vrealms.gameprobe", .name = "VRealms game-logic probe", .version = "0.1",
    .sdk_req_major = ROTH_SDK_MAJOR, .sdk_req_minor = ROTH_SDK_MINOR, .api_use = ROTH_API_USE_ENGINE | ROTH_API_USE_GAME_RAM,
    .on_load = on_load, .on_frame_game = frame, .on_register_overrides = reg,
};
ROTH_PLUGIN_EXPORT const struct roth_plugin_info_v1 *roth_plugin_query_v1(void) { return &INFO; }
