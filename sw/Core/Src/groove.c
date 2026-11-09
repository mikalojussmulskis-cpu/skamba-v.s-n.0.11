// groove.c - Plinky Groovebox extension. See groove.h and GROOVEBOX_PLAN.md.
//
// plinky.c is a unity build, so its globals (non-static) are reachable from here
// through extern declarations. Its static inline helpers are not; hooks pass
// values in instead.
//
// Threading: groove_toggle/pad*/tick/reset/finger run in the audio ISR and
// must stay cheap (no flash access, no blocking). groove_draw_* run in the
// main loop and only read state.

#ifndef EMU
#include "main.h"
#endif
#include <string.h>
#include "core.h"
#include "gfx.h"
#include "groove.h"

#if GROOVE_ENABLE

// ---------------------------------------------------------------------------
// Pattern data (format v1, see GROOVEBOX_PLAN.md section 5). Phase 2 keeps the
// pattern in RAM only; p-locks are appended to this struct in a later phase.

#define GROOVE_MAGIC 0x31565247u // "GRV1"
#define GROOVE_VERSION 1
#define GROOVE_LANES 8
#define GROOVE_STEPS 64
#define GROOVE_PAGE 16

#define STEP_TRIG 0x80 // b7 trig, b4-6 velocity (0 = default), b0-3 chance (later)
#define STEP_VEL_SHIFT 4
#define STEP_VEL_MASK 7
#define VEL_DEFAULT 5

#define LANE_MUTE 1

typedef struct GrooveLane { // 16 bytes
	u8 length; // 1..64
	u8 start;  // 0..63
	u8 flags;  // LANE_MUTE
	u8 level;
	u8 nsound;
	u8 rsv[3];
	struct { u8 param; s8 val; } sound[4];
} GrooveLane;

typedef struct GroovePattern { // 656 bytes
	u32 magic;
	u8 version, kit, preset_link, chain_next, nlocks, swing_override;
	u8 rsv[6];
	GrooveLane lane[GROOVE_LANES];
	u8 step[GROOVE_LANES][GROOVE_STEPS];
} GroovePattern;
_Static_assert(sizeof(GrooveLane) == 16, "lane layout");
_Static_assert(sizeof(GroovePattern) == 656, "pattern layout");

// ---------------------------------------------------------------------------
// Pad layout in the Groove main view
#define ROW_MUTE 5  // R6: mute row
#define ROW_STEPS 6 // R7, R8: 16 steps of the selected lane
#define LONGPRESS_MS 400

// ---------------------------------------------------------------------------
// From plinky.c (unity build globals)
extern u8 led_ram[9][8];
extern u8 cur_sample1;
void clearlatch(void);
void ShowMessage(EFont fnt, const char* msg, const char* submsg);
void halfrectangle(int x1, int y1, int x2, int y2);

// ---------------------------------------------------------------------------
// State
volatile unsigned char groove_active = 0;

static GroovePattern pat;

static u8 lane_pos[GROOVE_LANES]; // current step per lane
static u8 gate[GROOVE_LANES];     // ticks left of the current sequenced hit
static u8 hit_vel[GROOVE_LANES];  // velocity 1..7 of the current hit
static u8 flash[GROOVE_LANES];    // LED/OLED activity, decays
static u8 lane_sel;               // lane being edited
static u8 page;                   // 16-step page shown on R7/R8
static u8 running;                // transport was running last tick
static u8 gate_len = 8;
static u8 tickdiv;
static u8 live_down;              // bitmask: string touched last update

static u8 pad_row[8] = { 255, 255, 255, 255, 255, 255, 255, 255 };
static u32 pad_time[8];
static u8 pad_long;               // bitmask: long press already handled

static const char* const lane_names[GROOVE_LANES] = {
	"lane 1", "lane 2", "lane 3", "lane 4", "lane 5", "lane 6", "lane 7", "lane 8",
};

static void clear_hits(void) {
	memset(gate, 0, sizeof(gate));
	running = 0;
}

void groove_init(void) {
	groove_active = 0; // always boot into normal Plinky
	memset(&pat, 0, sizeof(pat));
	pat.magic = GROOVE_MAGIC;
	pat.version = GROOVE_VERSION;
	pat.preset_link = 255;
	pat.chain_next = 255;
	pat.swing_override = 255;
	for (int l = 0; l < GROOVE_LANES; ++l) {
		pat.lane[l].length = GROOVE_PAGE;
		pat.lane[l].level = 255;
	}
	clear_hits();
}

void groove_toggle(void) {
	clear_hits();
	groove_active = !groove_active;
	if (groove_active)
		clearlatch();
	ShowMessage(F_32_BOLD, groove_active ? "groove on" : "groove off", 0);
}

// ---------------------------------------------------------------------------
// Pads

static int vel_from_pressure(int pressure) {
	return clampi(pressure / 300 + 1, 1, 7);
}

int groove_pad_is_edit(int fi, int row) {
	(void)fi;
	return row >= ROW_MUTE;
}

void groove_pad(int fi, int row, int trig, int pressure) {
	u8 bit = 1 << fi;
	if (row == ROW_MUTE) {
		if (trig) {
			pad_row[fi] = ROW_MUTE;
			pad_time[fi] = millis();
			pad_long &= ~bit;
		}
		else if (pad_row[fi] == ROW_MUTE && !(pad_long & bit) && millis() - pad_time[fi] > LONGPRESS_MS) {
			pad_long |= bit; // long press: select the lane without muting it
			lane_sel = fi;
			ShowMessage(F_20_BOLD, lane_names[fi], 0);
		}
	}
	else if (row >= ROW_STEPS && row < 8 && trig) {
		pad_row[fi] = row;
		int idx = page * GROOVE_PAGE + (row - ROW_STEPS) * 8 + fi;
		u8* s = &pat.step[lane_sel][idx];
		if (*s & STEP_TRIG)
			*s = 0;
		else
			*s = STEP_TRIG | (vel_from_pressure(pressure) << STEP_VEL_SHIFT);
	}
}

void groove_pad_up(int fi) {
	if (pad_row[fi] == ROW_MUTE && !(pad_long & (1 << fi)))
		pat.lane[fi].flags ^= LANE_MUTE;
	pad_row[fi] = 255;
}

// ---------------------------------------------------------------------------
// Sequencer

static void fire(int l) {
	u8 s = pat.step[l][lane_pos[l]];
	if (!(s & STEP_TRIG) || (pat.lane[l].flags & LANE_MUTE))
		return;
	int vel = (s >> STEP_VEL_SHIFT) & STEP_VEL_MASK;
	hit_vel[l] = vel ? vel : VEL_DEFAULT;
	gate[l] = gate_len;
	flash[l] = 255;
}

void groove_reset(void) {
	running = 0; // next tick restarts all lanes from their first step
}

void groove_tick(int clock16, int playing, int preview, int step_ticks) {
	for (int l = 0; l < GROOVE_LANES; ++l)
		if (gate[l])
			gate[l]--;
	if (++tickdiv >= 4) {
		tickdiv = 0;
		for (int l = 0; l < GROOVE_LANES; ++l)
			flash[l] -= flash[l] >> 3;
	}
	// the gate must end at least one string update (2 ticks) before the next step,
	// so the next hit is seen as a new trigger
	gate_len = clampi(mini(step_ticks / 2, 250), 2, maxi(2, step_ticks - 3));

	int run = playing || preview;
	if (run && !running) {
		for (int l = 0; l < GROOVE_LANES; ++l) {
			lane_pos[l] = pat.lane[l].start;
			fire(l);
		}
	}
	else if (playing && clock16) {
		for (int l = 0; l < GROOVE_LANES; ++l) {
			GrooveLane* ln = &pat.lane[l];
			int p = lane_pos[l] + 1;
			if (p >= ln->start + ln->length || p >= GROOVE_STEPS)
				p = ln->start;
			lane_pos[l] = p;
			fire(l);
		}
	}
	running = run;
}

void groove_finger(int fi, int* pressure, int* position) {
	u8 bit = 1 << fi;
	if (*pressure > 0) {
		// live hit on a drum pad: it selects that lane for editing
		if (!(live_down & bit)) {
			live_down |= bit;
			if ((*position >> 8) < ROW_MUTE) {
				lane_sel = fi;
				flash[fi] = 255;
			}
		}
		return;
	}
	live_down &= ~bit;
	if (gate[fi]) {
		*pressure = hit_vel[fi] * 292; // velocity 7 = full pressure (2048)
		*position = 32;                // top row = start of the slice
	}
}

// ---------------------------------------------------------------------------
// Drawing (main loop)

static inline u8 gamma8(int i) {
	i = clampi(i, 0, 255);
	return (i * i) >> 8;
}

void groove_draw_leds(void) {
	GrooveLane* sel = &pat.lane[lane_sel];
	int playhead = running ? lane_pos[lane_sel] : -1;
	for (int fi = 0; fi < 8; ++fi) {
		int k = maxi(flash[fi], fi == lane_sel ? 40 : 0);
		for (int y = 0; y < ROW_MUTE; ++y)
			led_ram[fi][y] = gamma8(k);

		int muted = pat.lane[fi].flags & LANE_MUTE;
		k = muted ? 0 : 120;
		if (fi == lane_sel)
			k = muted ? 50 : 255;
		led_ram[fi][ROW_MUTE] = gamma8(k);

		for (int y = ROW_STEPS; y < 8; ++y) {
			int idx = page * GROOVE_PAGE + (y - ROW_STEPS) * 8 + fi;
			u8 s = pat.step[lane_sel][idx];
			int inloop = idx >= sel->start && idx < sel->start + sel->length;
			k = inloop ? 14 : 0;
			if (s & STEP_TRIG) {
				int vel = (s >> STEP_VEL_SHIFT) & STEP_VEL_MASK;
				k = 80 + (vel ? vel : VEL_DEFAULT) * 25;
			}
			if (idx == playhead)
				k = 255;
			led_ram[fi][y] = gamma8(k);
		}
	}
}

void groove_draw_oled(void) {
	GrooveLane* sel = &pat.lane[lane_sel];
	drawstr(0, 0, F_16_BOLD, "Groove");
	fdrawstr(-128, 2, F_12, "L%d %d/%d", lane_sel + 1, lane_pos[lane_sel] - sel->start + 1, sel->length);
	if (!cur_sample1) {
		drawstr(0, 20, F_8, "no kit: Preset + sample pad");
		return;
	}
	// one activity meter per lane; dithered = muted, top line = selected
	for (int l = 0; l < GROOVE_LANES; ++l) {
		int x0 = l * 16 + 1, x1 = x0 + 13;
		if (pat.lane[l].flags & LANE_MUTE)
			halfrectangle(x0, 20, x1, 32);
		else
			fillrectangle(x0, 31 - (flash[l] * 11) / 255, x1, 32);
		if (l == lane_sel)
			hline(x0, 18, x1, 1);
	}
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
