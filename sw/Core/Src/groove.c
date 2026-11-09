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
#include <stdlib.h>
#include "core.h"
#include "gfx.h"
#include "groove.h"

#if GROOVE_ENABLE

#include "drums.h"

// ---------------------------------------------------------------------------
// Pattern data (format v1, see GROOVEBOX_PLAN.md section 5). One pattern = one
// 2 KB page of Plinky's flash store (logical index 136 + slot). Fields added
// later go at the end; bytes that were never written read back as 0xFF.

#define GROOVE_MAGIC 0x31565247u // "GRV1"
#define GROOVE_VERSION 1
#define GROOVE_LANES 8
#define GROOVE_STEPS 64
#define GROOVE_PAGE 16
#define NO_SLOT 255

#define STEP_TRIG 0x80 // b7 trig, b4-6 velocity (0 = default), b0-3 chance (later)
#define STEP_VEL_SHIFT 4
#define STEP_VEL_MASK 7
#define VEL_DEFAULT 5

#define LANE_MUTE 1

typedef struct GrooveLane { // 16 bytes
	u8 length; // 1..64
	u8 start;  // 0..63
	u8 flags;  // LANE_MUTE
	u8 rsv0;
	DrumParams snd; // 10 bytes: model, tune, sweep, decay, snap, tone, drive, level, delay send, reverb send
	u8 rsv[2];
} GrooveLane;

typedef struct GroovePattern { // 656 bytes
	u32 magic;
	u8 version, kit, preset_link, chain_next, nlocks, swing_override;
	u8 volume; // drum master volume
	u8 rsv[5];
	GrooveLane lane[GROOVE_LANES];
	u8 step[GROOVE_LANES][GROOVE_STEPS];
} GroovePattern;
_Static_assert(sizeof(DrumParams) == 10, "drum params layout");
_Static_assert(sizeof(GrooveLane) == 16, "lane layout");
_Static_assert(sizeof(GroovePattern) == 656, "pattern layout");
_Static_assert((sizeof(GroovePattern) & 7) == 0, "flash writes are 8-byte units");

#define VOLUME_DEFAULT 128

// default kit:                   model     tune sweep decay snap tone drive level  dly  rvb
static const DrumParams default_kit[GROOVE_LANES] = {
	{ DM_KICK,    0, 150, 120,  70, 128,  40, 230,   0,   0 },
	{ DM_SNARE,   0,  40,  90, 170, 150,   0, 200,   0,  40 },
	{ DM_CLAP,    0,   0, 100,   0, 130,   0, 200,   0,  60 },
	{ DM_CHAT,    0,   0,  40,   0, 200,   0, 150,   0,   0 },
	{ DM_OHAT,    0,   0, 120,   0, 200,   0, 140,   0,  20 },
	{ DM_TOM,   -40,  90, 120,  30, 100,   0, 210,   0,  30 },
	{ DM_TOM,    40,  90, 110,  30, 100,   0, 200,   0,  30 },
	{ DM_PERC,    0,   0,  70,   0, 128,  20, 150,  60,   0 },
};

// ---------------------------------------------------------------------------
// Pad layout in the drum grid
#define DRUM_ROWS 5 // R1-R5: drum pads, R1 loudest .. R5 quietest
#define ROW_MUTE 5  // R6: mute row
#define ROW_STEPS 6 // R7, R8: 16 steps of the selected lane
#define LONGPRESS_MS 400
#define NO_TOUCH (-2048) // TOUCH_MIN_PRES
static const u8 row_vel[DRUM_ROWS] = { 7, 5, 4, 3, 2 }; // 100, 71, 57, 43, 29 %

// ---------------------------------------------------------------------------
// From plinky.c (unity build globals)
extern u8 led_ram[9][8];
extern int lastencodertime;
void clearlatch(void);
void ShowMessage(EFont fnt, const char* msg, const char* submsg);
void halfrectangle(int x1, int y1, int x2, int y2);

// ---------------------------------------------------------------------------
// State
volatile unsigned char groove_active = 0;
volatile unsigned char groove_grid = 0;
int groove_rv_in;

static GroovePattern pat __attribute__((aligned(8)));

static u8 lane_pos[GROOVE_LANES]; // current step per lane
static u8 gate[GROOVE_LANES];     // ticks left of a "Sample" lane hit
static u8 hit_vel[GROOVE_LANES];  // velocity 1..7 of that hit
static u8 flash[GROOVE_LANES];    // LED/OLED activity, decays
static u8 lane_sel;               // lane being edited
static u8 page;                   // 16-step page shown on R7/R8
static u8 running;                // transport was running last tick
static u8 playing_now, recording_now;
static u8 gate_len = 8;
static u8 step_ticks_now = 60;
static u8 ticks_in_step;
static u16 master_step;           // 16ths since start, for bar-aligned pattern changes
static u8 tickdiv;
static u8 live_down;              // bitmask: drum pad touched last update
static u8 skip_next;              // bitmask: live-recorded onto the next step, don't play it twice

static u8 pad_row[8] = { 255, 255, 255, 255, 255, 255, 255, 255 };
static u32 pad_time[8];
static u8 pad_long;               // bitmask: long press already handled

// pattern slots and saving
static volatile u8 cur_slot;
static volatile u8 pending_slot = NO_SLOT; // selected, switches on the next bar
static volatile u8 clear_on_load;          // the pending slot is cleared once loaded
static volatile u8 pad_slot = NO_SLOT;     // pattern pad held in Preset mode
static volatile u8 copy_req = NO_SLOT;     // copy the current pattern into this slot (main loop)
static volatile u8 dirty, saving;
static u32 first_dirty_ms, last_edit_ms;

static const char* const lane_names[GROOVE_LANES] = {
	"lane 1", "lane 2", "lane 3", "lane 4", "lane 5", "lane 6", "lane 7", "lane 8",
};

static void mark_dirty(void) {
	u32 now = millis();
	if (!dirty)
		first_dirty_ms = now;
	last_edit_ms = now;
	dirty = 1;
}

static void stop_sound(void) {
	memset(gate, 0, sizeof(gate));
	skip_next = 0;
	running = 0;
	drums_stop_all();
}

static void default_pattern(void) {
	memset(&pat, 0, sizeof(pat));
	pat.magic = GROOVE_MAGIC;
	pat.version = GROOVE_VERSION;
	pat.preset_link = 255;
	pat.chain_next = 255;
	pat.swing_override = 255;
	pat.volume = VOLUME_DEFAULT;
	for (int l = 0; l < GROOVE_LANES; ++l) {
		pat.lane[l].length = GROOVE_PAGE;
		pat.lane[l].snd = default_kit[l];
	}
}

// copy a slot from flash into RAM (only when no flash write is in progress)
static void load_slot(int slot) {
	const GroovePattern* src = (const GroovePattern*)GetSavedGroovePattern(slot);
	if (src && src->magic == GROOVE_MAGIC && src->version == GROOVE_VERSION) {
		memcpy(&pat, src, sizeof(pat));
		for (int l = 0; l < GROOVE_LANES; ++l) { // never trust flash blindly
			GrooveLane* ln = &pat.lane[l];
			ln->start &= GROOVE_STEPS - 1;
			if (ln->length < 1 || ln->start + ln->length > GROOVE_STEPS)
				ln->length = mini(GROOVE_PAGE, GROOVE_STEPS - ln->start);
			if (ln->snd.model >= DM_LAST)
				ln->snd.model = DM_KICK;
		}
	}
	else
		default_pattern();
	cur_slot = slot;
	memset(gate, 0, sizeof(gate));
	skip_next = 0;
	if (clear_on_load) {
		clear_on_load = 0;
		default_pattern();
		mark_dirty();
	}
}

static int can_load(void) {
	return !dirty && !saving && !GrooveFlashBusy();
}

void groove_init(void) {
	groove_active = 0; // always boot into normal Plinky
	groove_grid = 0;
	load_slot(0);
	stop_sound();
}

void groove_toggle(void) {
	stop_sound();
	groove_active = !groove_active;
	groove_grid = groove_active;
	if (groove_active)
		clearlatch();
	ShowMessage(F_20_BOLD, groove_active ? "groove on" : "groove off", 0);
}

void groove_view_toggle(void) {
	if (!groove_active) {
		groove_toggle();
		return;
	}
	groove_grid = !groove_grid;
	ShowMessage(F_20_BOLD, groove_grid ? "drum pads" : "plinky pads", 0);
}

// ---------------------------------------------------------------------------
// Drum grid pads

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
			ShowMessage(F_20_BOLD, lane_names[fi], drum_model_names[pat.lane[fi].snd.model]);
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
		mark_dirty();
	}
}

void groove_pad_up(int fi) {
	if (pad_row[fi] == ROW_MUTE && !(pad_long & (1 << fi))) {
		pat.lane[fi].flags ^= LANE_MUTE;
		mark_dirty();
	}
	pad_row[fi] = 255;
}

static int next_pos(int l) {
	GrooveLane* ln = &pat.lane[l];
	int p = lane_pos[l] + 1;
	if (p >= ln->start + ln->length || p >= GROOVE_STEPS)
		p = ln->start;
	return p;
}

// Record + Play: write a live hit onto the nearest step
static void live_record(int l, int vel) {
	if (!recording_now || !playing_now || !running)
		return;
	int s = lane_pos[l];
	if (ticks_in_step * 2 >= step_ticks_now) { // closer to the next step
		s = next_pos(l);
		skip_next |= 1 << l; // it just sounded live; don't play it again when the step comes
	}
	pat.step[l][s] = STEP_TRIG | (vel << STEP_VEL_SHIFT);
	mark_dirty();
}

void groove_touch(int fi, int* pressure, int position) {
	u8 bit = 1 << fi;
	int synth = pat.lane[fi].snd.model != DM_SAMPLE;
	if (*pressure > 0) {
		if (!(live_down & bit)) {
			// new hit: the row sets the velocity, so trigger right away
			live_down |= bit;
			lane_sel = fi;
			flash[fi] = 255;
			int vel = row_vel[clampi(position >> 8, 0, DRUM_ROWS - 1)];
			if (synth)
				drums_trigger(fi, &pat.lane[fi].snd, vel);
			live_record(fi, vel);
		}
		if (synth)
			*pressure = NO_TOUCH; // the Plinky voice stays free for melody
	}
	else
		live_down &= ~bit;
}

// ---------------------------------------------------------------------------
// Drum lane sound and drum volume (printed / unprinted param pads)

int groove_get_field(int field) {
	DrumParams* d = &pat.lane[lane_sel].snd;
	switch (field) {
	case GF_MODEL:  return ((d->model * 2 + 1) * 1024) / (DM_LAST * 2);
	case GF_TUNE:   return d->tune * 8;
	case GF_SWEEP:  return d->sweep * 4;
	case GF_DECAY:  return d->decay * 4;
	case GF_SNAP:   return d->snap * 4;
	case GF_TONE:   return d->tone * 4;
	case GF_DRIVE:  return d->drive * 4;
	case GF_LEVEL:  return d->level * 4;
	case GF_DLSEND: return d->dlsend * 4;
	case GF_RVSEND: return d->rvsend * 4;
	case GF_VOLUME: return pat.volume * 4;
	}
	return 0;
}

void groove_set_field(int field, int value) {
	DrumParams* d = &pat.lane[lane_sel].snd;
	u8 v8 = clampi(value / 4, 0, 255);
	switch (field) {
	case GF_MODEL: {
		int m = d->model;
		int delta = value - groove_get_field(GF_MODEL);
		if (millis() - lastencodertime < 50 && delta != 0 && abs(delta) <= 64)
			m += (delta > 0) ? 1 : -1; // encoder: one model per detent
		else
			m = (maxi(value, 0) * DM_LAST) / 1024; // slider: absolute
		m = clampi(m, 0, DM_LAST - 1);
		if (m == d->model)
			return;
		gate[lane_sel] = 0;
		d->model = m;
		break; }
	case GF_TUNE:   d->tune = clampi(value / 8, -128, 127); break;
	case GF_SWEEP:  d->sweep = v8; break;
	case GF_DECAY:  d->decay = v8; break;
	case GF_SNAP:   d->snap = v8; break;
	case GF_TONE:   d->tone = v8; break;
	case GF_DRIVE:  d->drive = v8; break;
	case GF_LEVEL:  d->level = v8; break;
	case GF_DLSEND: d->dlsend = v8; break;
	case GF_RVSEND: d->rvsend = v8; break;
	case GF_VOLUME: pat.volume = v8; break;
	default: return;
	}
	mark_dirty();
}

const char* groove_model_name(void) {
	return drum_model_names[pat.lane[lane_sel].snd.model];
}

// ---------------------------------------------------------------------------
// Pattern slots (Preset mode, middle 24 pads)

void groove_preset_pad(int slot) {
	pad_slot = slot;
}

void groove_preset_pad_up(void) {
	int slot = pad_slot;
	pad_slot = NO_SLOT;
	if (slot == NO_SLOT)
		return;
	pending_slot = (slot == cur_slot) ? NO_SLOT : slot; // tapping the current one cancels a queued change
}

void groove_preset_longpress(int slot) {
	pad_slot = NO_SLOT;
	pending_slot = NO_SLOT;
	copy_req = slot;
}

void groove_clear_slot(int slot) {
	if (slot == cur_slot) {
		pending_slot = NO_SLOT;
		default_pattern();
		mark_dirty();
	}
	else {
		pending_slot = slot;
		clear_on_load = 1;
	}
}

int groove_preset_led(int slot, int flicker) {
	if (slot == pad_slot || slot == pending_slot)
		return flicker;
	if (slot == cur_slot)
		return 255;
	return 64;
}

int groove_slot(void) {
	return cur_slot;
}

// main loop: autosave (5 s after the last edit, at most 60 s old, or right away
// when another slot is waiting to load) and copy
void groove_flash_pump(void) {
#ifdef DISABLE_AUTOSAVE
	dirty = 0;
	copy_req = NO_SLOT;
#else
	if (copy_req != NO_SLOT) {
		int target = copy_req;
		saving = 1;
		copy_req = NO_SLOT;
		if (dirty) {
			dirty = 0;
			SaveGroovePattern(cur_slot, &pat, sizeof(pat));
		}
		dirty = 0;
		SaveGroovePattern(target, &pat, sizeof(pat));
		cur_slot = target;
		saving = 0;
		ShowMessage(F_20_BOLD, "groove copied", 0);
		return;
	}
	if (!dirty)
		return;
	u32 now = millis();
	if (pending_slot != NO_SLOT || now - last_edit_ms > 5000 || now - first_dirty_ms > 60000) {
		saving = 1;
		dirty = 0; // an edit during the write sets it again and saves later
		SaveGroovePattern(cur_slot, &pat, sizeof(pat));
		saving = 0;
	}
#endif
}

// ---------------------------------------------------------------------------
// Sequencer

static void fire(int l) {
	u8 bit = 1 << l;
	if (skip_next & bit) {
		skip_next &= ~bit;
		return;
	}
	u8 s = pat.step[l][lane_pos[l]];
	if (!(s & STEP_TRIG) || (pat.lane[l].flags & LANE_MUTE))
		return;
	int vel = (s >> STEP_VEL_SHIFT) & STEP_VEL_MASK;
	if (!vel)
		vel = VEL_DEFAULT;
	if (pat.lane[l].snd.model == DM_SAMPLE) {
		hit_vel[l] = vel;
		gate[l] = gate_len;
	}
	else
		drums_trigger(l, &pat.lane[l].snd, vel);
	flash[l] = 255;
}

static void restart_lanes(void) {
	for (int l = 0; l < GROOVE_LANES; ++l) {
		lane_pos[l] = pat.lane[l].start;
		fire(l);
	}
}

void groove_reset(void) {
	running = 0; // next tick restarts all lanes from their first step
}

void groove_tick(int clock16, int playing, int preview, int step_ticks, int recording) {
	playing_now = playing;
	recording_now = recording;
	step_ticks_now = clampi(step_ticks, 1, 255);
	if (ticks_in_step < 255)
		ticks_in_step++;
	for (int l = 0; l < GROOVE_LANES; ++l)
		if (gate[l])
			gate[l]--;
	if (++tickdiv >= 4) {
		tickdiv = 0;
		for (int l = 0; l < GROOVE_LANES; ++l)
			flash[l] -= flash[l] >> 3;
	}
	// a "Sample" lane hit must end at least one string update (2 ticks) before
	// the next step, so the next hit is seen as a new trigger
	gate_len = clampi(mini(step_ticks / 2, 250), 2, maxi(2, step_ticks - 3));

	int run = playing || preview;
	if (run && !running) {
		if (pending_slot != NO_SLOT && can_load()) {
			load_slot(pending_slot);
			pending_slot = NO_SLOT;
		}
		master_step = 0;
		ticks_in_step = 0;
		restart_lanes();
	}
	else if (playing && clock16) {
		master_step++;
		ticks_in_step = 0;
		if ((master_step & 15) == 0 && pending_slot != NO_SLOT && can_load()) {
			load_slot(pending_slot); // pattern change on the bar
			pending_slot = NO_SLOT;
			restart_lanes();
		}
		else {
			for (int l = 0; l < GROOVE_LANES; ++l) {
				lane_pos[l] = next_pos(l);
				fire(l);
			}
		}
	}
	else if (!run && pending_slot != NO_SLOT && can_load()) {
		load_slot(pending_slot); // stopped: change right away
		pending_slot = NO_SLOT;
	}
	running = run;
}

void groove_finger(int fi, int* pressure, int* position) {
	if (gate[fi] && *pressure <= 0) {
		*pressure = hit_vel[fi] * 292; // velocity 7 = full pressure (2048)
		*position = 32;                // top row = start of the slice
	}
}

void groove_audio(void) {
	drums_render(pat.volume);
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
		for (int y = 0; y < DRUM_ROWS; ++y) {
			// selected lane: dim gradient showing the velocity rows
			int k = (fi == lane_sel) ? 56 - y * 8 : 0;
			led_ram[fi][y] = gamma8(maxi(k, flash[fi]));
		}

		int muted = pat.lane[fi].flags & LANE_MUTE;
		int k = muted ? 0 : 120;
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
	fdrawstr(0, 0, F_16_BOLD, "%d %s", lane_sel + 1, drum_model_names[sel->snd.model]);
	if (pending_slot != NO_SLOT)
		fdrawstr(-128, 2, F_12, "P%d>%d", cur_slot + 1, pending_slot + 1);
	else
		fdrawstr(-128, 2, F_12, "%sP%d %d/%d", recording_now ? "\x9c" : "", cur_slot + 1,
			lane_pos[lane_sel] - sel->start + 1, sel->length);
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
