/* clickprobe.c -- the ORIGINAL's secondary click on each cursor type (run_gameplay_frame,
 * E/game_core.c:515-732), for tools/handclick_test. At gameplay frame 200 of STUDY1 it runs, for every
 * case, one run_gameplay_frame with a secondary press edge pending and the cursor state set: the type
 * (1..5), the primary held or not, the steer clock [0x7fdcc], the frame time. trigger_weapon_fire is
 * overridden to log its call and the aim flag [0x7f568] and do nothing else; render_world_view (the type-4
 * use's pick) to log its call and hand back a pick record too far to use (depth 0x7fff, so
 * activate_targeted_object refuses at its first gate, E/input.c:1031). Log lines:
 *   C <type> <held> <clock> <fts> fire=<0|1> aimed=<flag> use=<0|1> type_after=<t>
 * Every log write holds SIGALRM blocked (tools/invdraw_test/invprobe.c). */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "roth_sdk.h"

static const struct roth_api_v1 *A;
static FILE *OUT;
static int g_frames, g_done;
static char g_syms[512];
#define G8(a)  (A->game_ram->u8(a))
#define G32(a) (A->game_ram->u32(a))
#define S8(a,v)  (A->game_ram->set_u8((a),(uint8_t)(v)))
#define S32(a,v) (A->game_ram->set_u32((a),(uint32_t)(v)))
static sigset_t g_old; static int g_depth;
static void log_on(void) { if (g_depth++ == 0) { sigset_t s; sigemptyset(&s); sigaddset(&s, SIGALRM); sigprocmask(SIG_BLOCK, &s, &g_old); } }
static void log_off(void) { if (--g_depth == 0) sigprocmask(SIG_SETMASK, &g_old, 0); }

struct sym { char name[64]; uint32_t addr; };
static struct sym *g_sym; static int g_nsym;
static void load_syms(void)
{
    FILE *f = fopen(g_syms, "r"); if (!f) return;
    char line[512]; g_sym = malloc(sizeof(struct sym) * 12000);
    while (fgets(line, sizeof line, f) && g_nsym < 12000) {
        unsigned a; char t; char n[256];
        if (sscanf(line, "%x %c %255s", &a, &t, n) != 3) continue;
        if (t != 'T' && t != 't') continue;
        strncpy(g_sym[g_nsym].name, n, 63); g_sym[g_nsym].addr = a; g_nsym++;
    }
    fclose(f);
}
static uint32_t fn(const char *n) { for (int i = 0; i < g_nsym; i++) if (!strcmp(g_sym[i].name, n)) return g_sym[i].addr; return 0; }
typedef void (*f0)(void);

static int g_capture, g_fired, g_aimed, g_used;
static uint8_t g_rec[0x40];
static uint32_t ov_fire(struct roth_chain *c, const struct roth_api_v1 *api, uint32_t eax, uint32_t edx, uint32_t ebx, uint32_t ecx)
{
    (void)api;
    if (g_capture) { g_fired = 1; g_aimed = (int)G32(0x7f568); return 0; }
    return roth_next_trigger_weapon_fire(c, eax, edx, ebx, ecx);
}
static uint32_t ov_render(struct roth_chain *c, const struct roth_api_v1 *api, uint16_t es, uint16_t fs, uint16_t gs)
{
    (void)api;
    if (g_capture) {
        g_used = 1;
        memset(g_rec, 0, sizeof g_rec);
        g_rec[1] = 1;                                         /* a ceiling */
        *(uint32_t *)(g_rec + 0x20) = 0x7fff;                 /* too far: activate refuses */
        return (uint32_t)(uintptr_t)g_rec;
    }
    return roth_next_render_world_view(c, es, fs, gs);
}
static void reg(const struct roth_api_v1 *api, struct roth_registrar_v1 *r)
{
    (void)api;
    roth_override(r, ROTH_FN_trigger_weapon_fire, ov_fire, 100);
    roth_override(r, ROTH_FN_render_world_view, ov_render, 100);
}

static void one(int type, int held, int clock, int fts)
{
    S32(0x707b3, 300); S32(0x707b7, 200);                    /* the mouse away from the corner peek (:521-522) */
    S32(0x7fdc0, 0);                                         /* the corner peek's timer */
    S32(0x7f564, 0);                                         /* [dev_mode + 4] */
    S32(0x83aea, 0); S32(0x81e3e, 0); S32(0x83125, 0);       /* no dialogue */
    S8(0x7e932, type);                                       /* the cursor type the hover left */
    S32(0x7fdb0, 0);                                         /* [cursor_interaction_flags + 4] */
    S8(0x7e938, 0); S8(0x7e939, 0xff);                       /* a SECONDARY press edge */
    S8(0x7e929, (held ? 1 : 0) | 2);                         /* the buttons held this frame (poll_mouse_input) */
    S8(0x7e8d0, 1);                                          /* already steering (the clock is not reset) */
    S32(0x7fdc8, clock);                                     /* the steer clock ([corner_icon_saveunder + 0xc]) */
    S32(0x7fdc4, 0);                                         /* the once-per-press latch */
    S32(0x85324, fts);                                       /* this frame's ticks */
    S32(0x7f568, 0);
    g_fired = g_aimed = g_used = 0;
    g_capture = 1;
    ((f0)(uintptr_t)fn("run_gameplay_frame"))();
    g_capture = 0;
    log_on();
    fprintf(OUT, "C %d %d %d %d fire=%d aimed=%d use=%d type_after=%d\n", type, held, clock, fts, g_fired, g_aimed, g_used, G8(0x7e932));
    fflush(OUT);
    log_off();
}

static void frame(const struct roth_api_v1 *api)
{
    A = api;
    if (api->game_ram->u8(0x7674a) != 1) return;
    if (g_done || ++g_frames < 200) return;
    g_done = 1;
    const char *o = getenv("CLICKPROBE_OUT");
    log_on(); OUT = fopen(o ? o : "/tmp/clickprobe.log", "w"); log_off();
    if (!OUT) _Exit(1);
    load_syms();
    static const int clocks[] = { 0, 4, 9, 10, 11, 12, 30 };
    for (int type = 1; type <= 5; type++)
        for (int held = 0; held <= 1; held++)
            for (unsigned k = 0; k < sizeof clocks / sizeof clocks[0]; k++)
                for (int fts = 1; fts <= 2; fts++)
                    one(type, held, clocks[k], fts);
    log_on(); fprintf(OUT, "END\n"); fclose(OUT); OUT = 0; log_off();
    _Exit(0);
}
static void on_load(const struct roth_api_v1 *api) { (void)api; const char *s = getenv("CLICKPROBE_SYMS"); if (s) strncpy(g_syms, s, sizeof g_syms - 1); }
static const struct roth_plugin_info_v1 INFO = {
    .abi_major = ROTH_ABI_MAJOR, .abi_minor = ROTH_ABI_MINOR, .struct_size = sizeof(struct roth_plugin_info_v1),
    .id = "io.github.vrealms.clickprobe", .name = "secondary click probe", .version = "0.1",
    .sdk_req_major = ROTH_SDK_MAJOR, .sdk_req_minor = ROTH_SDK_MINOR, .api_use = ROTH_API_USE_ENGINE | ROTH_API_USE_GAME_RAM,
    .on_load = on_load, .on_frame_game = frame, .on_register_overrides = reg,
};
ROTH_PLUGIN_EXPORT const struct roth_plugin_info_v1 *roth_plugin_query_v1(void) { return &INFO; }
