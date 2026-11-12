/* gdvprobe.c -- ROTH.C mod: play movies through the ORIGINAL's own GDV code and print, for every
 * record, the same lines tools/gdv_test/gdvtool.cpp prints from the port (src/roth/roth_gdv).
 *
 * Built against roth_sdk.h only (see run_probe.sh). NOT part of the engine. Writes its trace to
 * $GDV_OUT; nothing it writes may be committed (it is derived from the retail movies).
 *
 * How
 *   The first play_gdv_cutscene of the boot (action 1's first movie) is taken over: for each name in
 *   $GDV_MOVIES the ORIGINAL play_gdv_cutscene runs (descriptor, decode buffer, gdv_decoder_open with
 *   its header parse and preload, audio setup, subtitle stream handed over in EBX as
 *   play_record_gdv_cutscene hands it), with ONE function replaced: gdv_decode_frame, the timed
 *   playback. The replacement runs the original's own pieces in the order gdv_decode_frame runs them
 *   (gdv_cutscene.c:2991-3029) -- gdv_begin_playback, gdv_prime_first_frame, the loop's
 *   write_vga_palette, then per record gdv_read_frame_chunk as the ring allows, gdv_decode_video_chunk
 *   and gdv_advance_chunk_ptr (= gdv_emit_decoded_frame + the ring advance) -- but synchronously, with
 *   the timer blocked, so no frame can be skipped and nothing waits on a clock. The two spinning
 *   fades are left out (they only scale the DAC between two full uploads).
 *   Audio: for each record the image-free host's own stream decoder (audio_standalone_stream_ship,
 *   the replica of the [0x91898] decoder the original's buffer-complete callback runs) decodes that
 *   record's block from [0x91d70]; its voice-queue call is made a no-op by putting the voice table
 *   entry over the 0xe106 limit (os_audio.c:1071) and the decoded block is read from [0x91d4c].
 *   DAC: g_os_port_out is chained, so every 0x3c8/0x3c9 write the original makes (write_vga_palette,
 *   gdv_clear_vga_palette, the flash of coder 1) lands in a DAC copy here, exactly as VGA does.
 *   Subtitles: layout_timed_message_text and clear_cutscene_region are wrapped and logged with the
 *   record number, the colour byte g_timed_message_color holds, and the game palette's seed colour.
 *
 * Lines (record k, 0-based):
 *   M <name> ...                          once per movie
 *   F k pix=<crc d40[0..d94)> pal=<crc hdr+0x18> dac=<crc DAC> flags=<chunk flags low byte> aud=<crc> n=<samples>
 *   S k show col=<index> crc=<crc of text> seed=r,g,b <text>   /  S k clear
 *   T <name> frames=.. audio_samples=.. audio_crc=..
 *   D code w h                            the size-code table (0x72770), once
 *   P <0x300 bytes hex>                   the game palette (g_palette_rgb_ptr) the subtitle colour indexes
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "roth_sdk.h"

static FILE *g_out;
static const struct roth_api_v1 *g_api;
#define RAM (g_api->game_ram)
#define P8(a)  (*(volatile uint8_t  *)(uintptr_t)(a))
#define P16(a) (*(volatile uint16_t *)(uintptr_t)(a))
#define P32(a) (*(volatile uint32_t *)(uintptr_t)(a))

static uint32_t crc32b(const void *data, uint32_t n, uint32_t c)
{
    const uint8_t *p = (const uint8_t *)data;
    c = ~c;
    for (uint32_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1))); }
    return ~c;
}

/* engine entry points by address (run_probe.sh: nm) */
static uint32_t sym(const char *name)
{
    static char *tab; if (!tab) { const char *p = getenv("GDV_SYMS"); FILE *f = p ? fopen(p, "r") : 0;
        if (!f) { fprintf(stderr, "[gdvprobe] GDV_SYMS missing\n"); exit(3); }
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        tab = calloc(1, (size_t)sz + 1); size_t n = fread(tab, 1, (size_t)sz, f); tab[n] = 0; fclose(f); }
    char key[128]; snprintf(key, sizeof key, " %s\n", name);
    char *at = strstr(tab, key);
    if (!at) { fprintf(stderr, "[gdvprobe] no symbol %s\n", name); exit(3); }
    while (at > tab && at[-1] != '\n') at--;
    return (uint32_t)strtoul(at, 0, 16);
}
#define FN(ret, name, ...) ((ret (*)(__VA_ARGS__))(uintptr_t)sym(#name))

/* canonical addresses (g_names.h; gdv_cutscene.c) */
enum {
    A_ACTIVE = 0x91ddc, A_D40 = 0x91d40, A_HDR = 0x91d44, A_D4C = 0x91d4c, A_D50 = 0x91d50, A_D54 = 0x91d54,
    A_D5C = 0x91d5c, A_D60 = 0x91d60, A_D64 = 0x91d64, A_D68 = 0x91d68, A_D6C = 0x91d6c, A_D70 = 0x91d70,
    A_D94 = 0x91d94, A_D9C = 0x91d9c, A_D28 = 0x91d28, A_DC0 = 0x91dc0, A_DD8 = 0x91dd8, A_DE5 = 0x91de5,
    A_DE6 = 0x91de6, A_DDE = 0x91dde, A_CB8 = 0x91cb8, A_BUDGET = 0x91884, A_HANDLER = 0x91898,
    A_VOICETAB = 0x9740c, A_TMCOLOR = 0x82040, A_PALRGB = 0x85488, A_SUBON = 0x71384, A_DIRGDV = 0x764f0,
    A_SIZES = 0x72770,
};

/* ------------------------------------------------------------------ the DAC, from the port writes */
static uint8_t g_dac[0x300]; static unsigned g_dacIdx, g_dacSub;
static void (*g_portOrig)(uint16_t, uint8_t);
static void port_out(uint16_t port, uint8_t v)
{
    if (port == 0x3c8) { g_dacIdx = v; g_dacSub = 0; }
    else if (port == 0x3c9) { g_dac[(g_dacIdx & 0xff) * 3 + g_dacSub] = v; if (++g_dacSub == 3) { g_dacSub = 0; g_dacIdx = (g_dacIdx + 1) & 0xff; } }
    if (g_portOrig) g_portOrig(port, v);
}

/* ------------------------------------------------------------------ per-movie state */
static const char *g_movie = "";
static int g_frame = -1;
static uint32_t g_audioCrc; static unsigned long long g_audioSamples;

static void record_line(int k, uint32_t audioSrc)
{
    const uint8_t *pix = (const uint8_t *)(uintptr_t)RAM->u32(A_D40);
    const uint8_t *hdr = (const uint8_t *)(uintptr_t)RAM->u32(A_HDR);
    uint32_t n = 0, ac = crc32b("", 0, 0);
    /* the record's audio block, through the original's decoder [0x91898] */
    if (RAM->u8(0x91dc8) != 0 || RAM->u32(A_HANDLER) != 0) {
        uint32_t saved = RAM->u32(A_VOICETAB);
        RAM->set_u32(A_VOICETAB, 0xe106);              /* voice_load_to_slot's "no slot" arm (os_audio.c:1071) */
        RAM->set_u32(A_D70, audioSrc);
        FN(void, audio_standalone_stream_ship, void)();
        RAM->set_u32(A_VOICETAB, saved);
        uint32_t handler = RAM->u32(A_HANDLER) - 0x400000u;   /* stored code pointers are VA + OBJ_DELTA */
        if (handler == 0x4e460) {                      /* the 16-bit DPCM decoder: count = [0x91d28] samples */
            n = RAM->u32(A_D28);
            const void *out = (const void *)(uintptr_t)RAM->u32(A_D4C);
            ac = crc32b(out, n * 2, 0);
            g_audioCrc = crc32b(out, n * 2, g_audioCrc);
        }
    }
    g_audioSamples += n;
    fprintf(g_out, "F %d pix=%08x pal=%08x dac=%08x flags=%02x aud=%08x n=%u\n", k, crc32b(pix, RAM->u32(A_D94), 0),
        crc32b(hdr + 0x18, 0x300, 0), crc32b(g_dac, 0x300, 0), RAM->u8(A_DC0), ac, n);
}

/* ------------------------------------------------------------------ gdv_decode_frame, synchronous */
static uint32_t w_decode_frame(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t ignored)
{
    (void)c; (void)api; (void)ignored;
    if (RAM->u8(A_ACTIVE) == 0) return 1;
    const uint8_t *hdr = (const uint8_t *)(uintptr_t)RAM->u32(A_HDR);
    const int nframes = (int)*(const uint16_t *)(hdr + 6);
    fprintf(g_out, "M %s %dx%d frames=%d fps=%d rate=%d ver=%d spf=%u abytes=%u handler=%x subon=%u\n", g_movie,
        *(const uint16_t *)(hdr + 0x14), *(const uint16_t *)(hdr + 0x16), nframes, *(const uint16_t *)(hdr + 8),
        *(const uint16_t *)(hdr + 0xc), *(const uint16_t *)(hdr + 0xe), RAM->u32(0x91d58), RAM->u32(A_D28),
        RAM->u32(A_HANDLER) ? RAM->u32(A_HANDLER) - 0x400000u : 0, RAM->u32(A_SUBON));
    int nread = (int)RAM->u32(A_CB8);                   /* records gdv_preload_frame_window read */
    memset(g_dac, 0, sizeof g_dac);                     /* the mode set blacks the DAC (gdv_cutscene.c:2177) */

    if (FN(uint32_t, gdv_begin_playback, void)()) { fprintf(g_out, "E begin_playback failed\n"); return 1; }
    g_frame = 0;
    FN(void, gdv_prime_first_frame, void)();            /* record 0: decode + emit + advance */
    FN(void, write_vga_palette, uint32_t, uint32_t, uint32_t, uint32_t)(0, RAM->u32(A_HDR) + 0x18, 0, 0x100);  /* :2905 */
    RAM->set_u8(A_DE5, 0);                              /* :2906 */
    record_line(0, RAM->u32(A_D50));

    for (int k = 1; k < nframes; k++) {
        /* the read side of gdv_run_playback_loop (:2919-2930): a record is read while the ring has room
         * behind the consumer; here only as far as the record about to be decoded */
        while (nread <= k) {
            uint32_t d60 = RAM->u32(A_D60), d68 = RAM->u32(A_D68);
            if (!(d60 > d68 || d60 + RAM->u32(A_D9C) < d68)) { fprintf(g_out, "E ring full at %d\n", k); return 1; }
            RAM->set_u32(A_D64, d60);
            RAM->set_u32(A_BUDGET, 0x7fffffff);         /* the read's pacing spin (:1411-1414) never waits */
            uint32_t ebp = 0, ecx = 0;
            if (FN(uint32_t, gdv_read_frame_chunk, uint32_t *, uint32_t *)(&ebp, &ecx)) { fprintf(g_out, "E read %x at %d\n", ebp, k); return 1; }
            nread++;
        }
        uint32_t src = RAM->u32(A_D6C);
        g_frame = k;
        FN(void, gdv_decode_video_chunk, void)();
        FN(void, gdv_advance_chunk_ptr, void)();         /* emit (publish, callback, blit, settle) + advance */
        if (RAM->u8(A_DDE) == 0xff) { fprintf(g_out, "E end of stream at %d\n", k); break; }
        record_line(k, src);
    }
    RAM->set_u8(A_DD8, 0);                              /* :3021 */
    fprintf(g_out, "T %s frames=%d audio_samples=%llu audio_crc=%08x\n", g_movie, g_frame + 1, g_audioSamples, g_audioCrc);
    return 0;
}

/* ------------------------------------------------------------------ the subtitle machine's output */
static int32_t w_layout(struct roth_chain *c, const struct roth_api_v1 *api, int32_t *meta, uint8_t *out, const uint8_t *src, int32_t maxw, int32_t maxlines)
{
    (void)api;
    int32_t r = roth_next_layout_timed_message_text(c, meta, out, src, maxw, maxlines);
    if (g_frame >= 0) {
        const uint8_t col = RAM->u8(A_TMCOLOR);
        const uint8_t *e = src - 5;                     /* the entry: {size, time, colour, text} */
        const uint8_t *gp = (const uint8_t *)(uintptr_t)RAM->u32(A_PALRGB) + e[4] * 3;
        uint32_t len = (uint32_t)strlen((const char *)src);
        fprintf(g_out, "S %d show col=%u crc=%08x seed=%u,%u,%u x0=%d x1=%d h=%d %s\n", g_frame, col, crc32b(src, len, 0),
            gp[0], gp[1], gp[2], meta[0], meta[1], meta[2], (const char *)src);
    }
    return r;
}
static void w_clear(struct roth_chain *c, const struct roth_api_v1 *api)
{
    (void)api;
    if (g_frame >= 0) fprintf(g_out, "S %d clear\n", g_frame);
    roth_next_clear_cutscene_region(c);
}

/* ------------------------------------------------------------------ the battery */
static int g_started;
static uint32_t w_play(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t eax, uint32_t edx, uint32_t ebx, uint32_t ecx)
{
    (void)api; (void)eax; (void)edx; (void)ebx; (void)ecx;
    if (g_started) return 0;
    g_started = 1;
    { sigset_t t; sigemptyset(&t); sigaddset(&t, SIGALRM); sigaddset(&t, SIGVTALRM); sigaddset(&t, SIGPROF);
      sigprocmask(SIG_BLOCK, &t, 0); }
    { void (**pp)(uint16_t, uint8_t) = (void (**)(uint16_t, uint8_t))(uintptr_t)sym("g_os_port_out");
      g_portOrig = *pp; *pp = port_out; }

    /* the size-code table gdv_read_file_header falls back on (gdv_cutscene.c:1281-1291) */
    for (uint32_t a = A_SIZES; RAM->u16(a) != 0xffff && a < A_SIZES + 0x600; a += 6)
        fprintf(g_out, "D %u %u %u\n", RAM->u16(a), RAM->u16(a + 2), RAM->u16(a + 4));

    /* the game palette the subtitle colour bytes index (g_palette_rgb_ptr, dialogue_ui.c:1927-1929) */
    { const uint8_t *gp = (const uint8_t *)(uintptr_t)RAM->u32(A_PALRGB); fprintf(g_out, "P ");
      for (int i = 0; i < 0x300; i++) fprintf(g_out, "%02x", gp ? gp[i] : 0);
      fprintf(g_out, "\n"); }
    const char *dir = (const char *)RAM->to_ptr(A_DIRGDV);
    fprintf(g_out, "DIR %s\n", dir ? dir : "?");
    FILE *f100 = fopen("DBASE100.DAT", "rb"), *f400 = fopen("DBASE400.DAT", "rb");
    static uint8_t d100[1 << 20];
    size_t n100 = f100 ? fread(d100, 1, sizeof d100, f100) : 0;
    char list[512]; snprintf(list, sizeof list, "%s", getenv("GDV_MOVIES") ? getenv("GDV_MOVIES") : "GREMLOGO CHPTR1 TUNNWARP BELEXTRA REFLECTA RAYSEL1F EPILOGUE INTRO");
    for (char *name = strtok(list, " "); name; name = strtok(0, " ")) {
        static char path[256]; snprintf(path, sizeof path, "%s%s.GDV", dir ? dir : "", name);
        /* the subtitle stream (gdv_cutscene.c:2777-2797), handed over as play_record_gdv_cutscene does */
        static uint8_t *subs; static uint32_t holder; uint32_t handle = 0;
        if (n100 > 0x28 && f400) {
            uint32_t cnt = *(uint32_t *)(d100 + 0x20), tab = *(uint32_t *)(d100 + 0x24);
            for (uint32_t i = 0; i < cnt; i++) {
                const uint8_t *r = d100 + tab + i * 0x14;
                if (strncmp((const char *)r, name, 8) != 0) continue;
                int16_t sz = (int16_t)(r[0xa] | (r[0xb] << 8)); uint32_t off = *(const uint32_t *)(r + 0x10) & 0xffffff;
                if (sz > 0) { free(subs); subs = malloc((size_t)sz); fseek(f400, (long)off, SEEK_SET);
                    if (fread(subs, 1, (size_t)sz, f400) == (size_t)sz) { holder = (uint32_t)(uintptr_t)subs; handle = (uint32_t)(uintptr_t)&holder; } }
                break;
            }
        }
        g_movie = name; g_frame = -1; g_audioCrc = 0; g_audioSamples = 0;
        roth_next_play_gdv_cutscene(c, (uint32_t)(uintptr_t)path, 0, handle, 0);
        g_frame = -1;
        fflush(g_out);
    }
    fprintf(g_out, "END\n"); fclose(g_out);
    fprintf(stderr, "[gdvprobe] done\n");
    _Exit(0);
}

static void reg(const struct roth_api_v1 *api, struct roth_registrar_v1 *r)
{
    (void)api;
#define OV(fn, cb) do { if (roth_override(r, ROTH_FN_##fn, cb, 200) != 0) fprintf(stderr, "[gdvprobe] override refused: " #fn "\n"); } while (0)
    OV(play_gdv_cutscene, w_play);
    OV(gdv_decode_frame, w_decode_frame);
    OV(layout_timed_message_text, w_layout);
    OV(clear_cutscene_region, w_clear);
#undef OV
}

static void on_load(const struct roth_api_v1 *api)
{
    g_api = api;
    const char *s = getenv("GDV_OUT"); g_out = fopen(s ? s : "/tmp/gdvprobe.trace", "w");
    if (g_out) fprintf(g_out, "BOOT\n");
}

static const struct roth_plugin_info_v1 INFO = {
    .abi_major = ROTH_ABI_MAJOR, .abi_minor = ROTH_ABI_MINOR, .struct_size = sizeof(struct roth_plugin_info_v1),
    .id = "io.github.vrealms.gdvprobe", .name = "VRealms GDV probe", .version = "0.1",
    .sdk_req_major = ROTH_SDK_MAJOR, .sdk_req_minor = ROTH_SDK_MINOR, .api_use = ROTH_API_USE_ENGINE | ROTH_API_USE_GAME_RAM,
    .on_load = on_load, .on_register_overrides = reg,
};
ROTH_PLUGIN_EXPORT const struct roth_plugin_info_v1 *roth_plugin_query_v1(void) { return &INFO; }
