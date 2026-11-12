/* doorprobe.c -- host-side probe mod for the ROTH.C oracle (NOT part of the repo patch).
 * At gameplay frame DOORPROBE_AT it stands the player at DOORPROBE_OPENER="x y sector" and calls the
 * original's own dev_open_nearest_door (doors.c), so the original opens the door in front of the camera. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "roth_sdk.h"
static int g_at = -1, g_frames, g_ox, g_oy, g_osec, g_done;
static uint32_t g_fn, g_selvar;
typedef void (*devopen_fn)(uint32_t, uint32_t, uint32_t, uint32_t);
typedef uint32_t (*selbase_fn)(uint16_t);
static void frame(const struct roth_api_v1 *api)
{
    const struct roth_game_ram_api_v1 *g = api->game_ram;
    if (g->u8(0x7674a) != 1) return;
    g_frames++;
    if (g_done || g_at < 0 || g_frames < g_at) return;
    g_done = 1;
    uint16_t sx = g->u16(0x90a8e), sy = g->u16(0x90a96), ss = g->u16(0x90c12);
    g->set_u16(0x90a8e, (uint16_t)g_ox); g->set_u16(0x90a96, (uint16_t)g_oy); g->set_u16(0x90c12, (uint16_t)g_osec);
    g->set_u8(0x7f560, g->u8(0x7f560) | 1);
    selbase_fn sb = *(selbase_fn *)(uintptr_t)g_selvar;
    uint8_t before = g->u8(0x8b3f4);
    ((devopen_fn)(uintptr_t)g_fn)(sb(g->u16(0x90be8)), sb(g->u16(0x90bec)), sb(g->u16(0x852c8)), sb(g->u16(0x852cc)));
    fprintf(stderr, "[doorprobe] frame %d: door count %u -> %u\n", g_frames, before, g->u8(0x8b3f4));
    for (int i = 0; i < g->u8(0x8b3f4); i++) { uint32_t r = 0x8b3f8 + i * 0x1f6;
        fprintf(stderr, "[doorprobe] rec %d: sector %04x state %02x angle %d far %04x near %04x pivot %d,%d M %u dwell %u\n", i,
            g->u16(r), g->u8(r+2), (int8_t)g->u8(r+3), g->u16(r+0x10), g->u16(r+0x12), (int16_t)g->u16(r+0x14), (int16_t)g->u16(r+0x16), g->u32(r+0xc), g->u16(r+0xa)); }
    g->set_u16(0x90a8e, sx); g->set_u16(0x90a96, sy); g->set_u16(0x90c12, ss);
}
static void on_load(const struct roth_api_v1 *api)
{
    (void)api; const char *s;
    if ((s = getenv("DOORPROBE_AT"))) g_at = atoi(s);
    if ((s = getenv("DOORPROBE_OPENER"))) sscanf(s, "%d %d %d", &g_ox, &g_oy, &g_osec);
    if ((s = getenv("DOORPROBE_FN"))) g_fn = strtoul(s, 0, 16);
    if ((s = getenv("DOORPROBE_SEL"))) g_selvar = strtoul(s, 0, 16);
}
static const struct roth_plugin_info_v1 INFO = {
    .abi_major = ROTH_ABI_MAJOR, .abi_minor = ROTH_ABI_MINOR, .struct_size = sizeof(struct roth_plugin_info_v1),
    .id = "io.github.vrealms.doorprobe", .name = "door probe", .version = "0.1",
    .sdk_req_major = ROTH_SDK_MAJOR, .sdk_req_minor = ROTH_SDK_MINOR, .api_use = ROTH_API_USE_ENGINE,
    .on_load = on_load, .on_frame_game = frame,
};
ROTH_PLUGIN_EXPORT const struct roth_plugin_info_v1 *roth_plugin_query_v1(void) { return &INFO; }
