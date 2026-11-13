/* tools/vrpick_test: the environment ROTH.C's classify_cursor_target_object
 * needs, and nothing else. The function's own text is cut out of input.c at
 * build time (run.sh) and compiled after this, unchanged. Memory is a flat
 * array: the pick record at 0x1000, the object def at 0x2000, the geometry
 * buffer at 0x3000; the callees are stubs answering what the test sets. */
#include <stdint.h>
typedef struct {
	uint32_t eax, ebx, ecx, edx, esi, edi, ebp, va, nstack, stack[8], eflags, es, fs, gs;
} regs_t;
extern uint8_t fakemem[0x10000];
#define GADDR(a) (&fakemem[(uint32_t)(a)])
#define G8(a)  (*(volatile uint8_t  *)GADDR(a))
#define G16(a) (*(volatile uint16_t *)GADDR(a))
#define G32(a) (*(volatile int32_t  *)GADDR(a))
#define RD8(a)  (*(uint8_t  *)&fakemem[(uint32_t)(a)])
#define RD16(a) (*(uint16_t *)&fakemem[(uint32_t)(a)])
#define RD32(a) (*(uint32_t *)&fakemem[(uint32_t)(a)])
enum { VA_g_cursor_interaction_flags = 0x100, VA_g_interaction_cursor_type = 0x120, VA_g_map_geometry_buffer = 0x130 };
uint32_t find_unflagged_object_by_key(uint8_t *rec);
int32_t  object_has_active_trigger_link(uint32_t rec);
int32_t  dispatch_entry_command_trigger_b(const regs_t *in);
int32_t  dispatch_entry_command_trigger(const regs_t *in);
uint32_t find_oninspect_block_by_id(uint32_t id);
uint32_t is_item_id_pickable(uint32_t id);
