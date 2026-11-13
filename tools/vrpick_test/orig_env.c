/* tools/vrpick_test: the stubs and the driver's C half (see orig_prelude.h). */
#include "orig_prelude.h"
uint8_t fakemem[0x10000];
int s_click, s_link, s_exb, s_use, s_insp, s_pick;
uint32_t find_unflagged_object_by_key(uint8_t *rec) { (void)rec; return s_click ? 0xffffffffu : 0; }
int32_t  object_has_active_trigger_link(uint32_t rec) { (void)rec; return s_link ? -1 : 0; }
int32_t  dispatch_entry_command_trigger_b(const regs_t *in) { (void)in; return s_exb ? 0x4444 : 0; }
int32_t  dispatch_entry_command_trigger(const regs_t *in) { (void)in; return s_use ? 0x5555 : 0; }
uint32_t find_oninspect_block_by_id(uint32_t id) { (void)id; return s_insp ? 0x6666 : 0; }
uint32_t is_item_id_pickable(uint32_t id) { (void)id; return s_pick ? 1 : 0; }

int32_t classify_cursor_target_object(const regs_t *in);

/* One call, as run_gameplay_frame makes it: the cursor type zeroed first
 * (game_core.c L169f3), EAX = the pick record. */
void orig_classify(int recType, int defFlags, int faceMarked, int depth,
	int *type, int *shape, int *flags, int *count, int *oor)
{
	for (int i = 0; i < 0x10000; i++) fakemem[i] = 0;
	G8(VA_g_interaction_cursor_type) = 0;
	G32(VA_g_map_geometry_buffer) = 0x3000;
	RD8(0x1000 + 1) = (uint8_t)recType;
	RD16(0x1000 + 8) = 0;                 /* face offset: geom+0+4 -> k */
	RD32(0x1000 + 0xe) = 0x2000;          /* the object def */
	RD32(0x1000 + 0x20) = (uint32_t)depth;
	RD8(0x2000 + 9) = (uint8_t)defFlags;
	RD16(0x2000 + 4) = 0x0234;            /* its render id */
	RD16(0x2000 + 0xe) = 0x77;            /* its command id */
	RD16(0x3000 + 4) = 0x40;              /* k */
	RD8(0x3000 + 0x40 + 9) = faceMarked ? 2 : 0;
	regs_t r = {0};
	r.eax = 0x1000;
	*shape = classify_cursor_target_object(&r);
	*type = G8(VA_g_interaction_cursor_type);
	*flags = G8(VA_g_cursor_interaction_flags);
	*count = G32(VA_g_cursor_interaction_flags + 4);
	*oor = G32(VA_g_cursor_interaction_flags + 8);
}
