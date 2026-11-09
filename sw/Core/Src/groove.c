// groove.c - Plinky Groovebox extension. See groove.h and GROOVEBOX_PLAN.md.
//
// plinky.c is a unity build, so its globals (non-static) are reachable from here
// through extern declarations. Its static inline helpers are not; hooks pass
// values in instead.

#ifndef EMU
#include "main.h"
#endif
#include "core.h"
#include "gfx.h"
#include "groove.h"

#if GROOVE_ENABLE

volatile unsigned char groove_active = 0;

void groove_init(void) {
	groove_active = 0; // always boot into normal Plinky
}

#endif // GROOVE_ENABLE

// ---------------------------------------------------------------------------
// Measurement build
#ifdef GROOVE_STATS

#define STACK_PAINT 0xC5C5C5C5u
#define HEAP_RESERVE 512   // keep in sync with _Min_Heap_Size in the linker script
#define PAINT_MARGIN 256   // don't paint right under our own frame

extern u32 _end;              // linker: end of .bss (heap starts here)
extern TickCounter _tc_budget; // cycles between successive audio interrupts
extern TickCounter _tc_all;    // cycles spent inside the audio interrupt

static u32* paint_start;
static u32* paint_end;
static int stat_cpu_avg, stat_cpu_peak, stat_cpu_peak_hold, stat_mhz;
static u32 stat_next_ms;

void groove_stats_init(void) {
#ifndef EMU
	// make sure the DWT cycle counter runs even without a debugger attached
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	paint_start = (u32*)(((u32)&_end + HEAP_RESERVE + 3) & ~3u);
	paint_end = (u32*)((__get_MSP() - PAINT_MARGIN) & ~3u);
	for (u32* p = paint_start; p < paint_end; ++p)
		*p = STACK_PAINT;
#endif
}

static int stack_free_bytes(void) {
	u32* p = paint_start;
	while (p < paint_end && *p == STACK_PAINT)
		++p;
	return (int)((u8*)p - (u8*)paint_start);
}

void groove_stats_draw(void) {
	u32 now = millis();
	if ((int)(now - stat_next_ms) >= 0) {
		stat_next_ms = now + 500;
		TickCounter all, budget;
#ifndef EMU
		__disable_irq();
#endif
		all = _tc_all;
		budget = _tc_budget;
		_tc_all.total = _tc_all.max = _tc_all.n = 0;
		_tc_budget.total = _tc_budget.max = _tc_budget.n = 0;
#ifndef EMU
		__enable_irq();
#endif
		if (all.n > 0 && budget.n > 0 && budget.total > 0) {
			int period = budget.total / budget.n; // cycles per 2.048 ms block
			stat_cpu_avg = (int)((100LL * all.total) / budget.total);
			stat_cpu_peak = (int)((100LL * all.max) / period);
			stat_mhz = (period + 1024) / 2048;    // cycles per microsecond
			if (stat_cpu_peak > stat_cpu_peak_hold)
				stat_cpu_peak_hold = stat_cpu_peak;
		}
	}
	// bottom-right corner: clear a box, then white text
	int x = 52;
	for (int y = 16; y < 32; ++y)
		for (int i = x; i < 128; ++i)
			putpixel(i, y, 0);
	u8 oldtextcol = textcol;
	textcol = 1;
	fdrawstr(x + 1, 16, F_8, "cpu%2d pk%2d/%2d", stat_cpu_avg, stat_cpu_peak, stat_cpu_peak_hold);
	fdrawstr(x + 1, 24, F_8, "stk%4d %3dMHz", stack_free_bytes(), stat_mhz);
	textcol = oldtextcol;
}

#endif // GROOVE_STATS
