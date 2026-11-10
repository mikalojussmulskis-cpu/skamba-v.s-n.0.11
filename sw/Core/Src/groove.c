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
// Pattern data (format v5, see GROOVEBOX_PLAN.md section 5). One pattern = one
// 2 KB page of Plinky's flash store (logical index 136 + slot). Fields added
// later go at the end; bytes that were never written read back as 0xFF.

#define GROOVE_MAGIC 0x31565247u // "GRV1"
#define GROOVE_VERSION 5 // v2: lane filter; v3: locks + mods; v4: locks removed; v5: sample rate reduction, DJ filter
#define GROOVE_MAX_MODS 24 // per pattern
#define V3_MODS_OFFSET 912 // where v3 stored the modulation slots (after 64 locks)
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
	DrumParams snd; // 13 bytes: model, tune, sweep, decay, snap, colour, drive, level, delay/reverb send, filter, reso, srr
} GrooveLane;

// modulation slot: this field of this lane is modulated by this source
typedef struct GrooveMod { u8 lane, field, source; s8 amount; } GrooveMod;

typedef struct GroovePattern { // 752 bytes
	u32 magic;
	u8 version, kit, preset_link, chain_next, nlocks_unused, swing_override;
	u8 volume;   // drum master volume
	u8 nmods;
	u8 djfilter; // global drum filter (DRUM_FILTER_OFF = off)
	u8 rsv[3];
	GrooveLane lane[GROOVE_LANES];
	u8 step[GROOVE_LANES][GROOVE_STEPS];
	GrooveMod mod[GROOVE_MAX_MODS];
} GroovePattern;
_Static_assert(sizeof(DrumParams) == 13, "drum params layout");
_Static_assert(sizeof(GrooveLane) == 16, "lane layout");
_Static_assert(sizeof(GroovePattern) == 752, "pattern layout");
// GF_MODEL..GF_RESO are the byte offsets of the fields inside DrumParams
_Static_assert(__builtin_offsetof(DrumParams, model) == GF_MODEL && __builtin_offsetof(DrumParams, tone) == GF_COLOR
	&& __builtin_offsetof(DrumParams, srr) == GF_SRR && sizeof(DrumParams) == GF_VOLUME, "field numbering");
_Static_assert((sizeof(GroovePattern) & 7) == 0, "flash writes are 8-byte units");

#define VOLUME_DEFAULT 128

// default kit                 model      tune sweep decay snap colour drive level  dly  rvb  filter       reso srr
static const DrumParams default_kit[GROOVE_LANES] = {
	{ DM_KICK,       0, 150, 133,  70, 110,  40, 230,   0,   0, DRUM_FILTER_OFF, 0, 0 },
	{ DM_SNARE,      0,  40, 115, 130, 150,   0, 200,   0,  13, DRUM_FILTER_OFF, 0, 0 },
	{ DM_CLAP,       0,   0, 112,   0, 130,   0, 200,   0,  20, DRUM_FILTER_OFF, 0, 0 },
	{ DM_CHAT,       0,   0, 109,  40, 140,   0, 170,   0,   0, DRUM_FILTER_OFF, 0, 0 },
	{ DM_OHAT,       0,   0, 146,  40, 140,   0, 150,   0,   7, DRUM_FILTER_OFF, 0, 0 },
	{ DM_TOM,      -40,  90, 153,  30,  60,   0, 210,   0,  10, DRUM_FILTER_OFF, 0, 0 },
	{ DM_RIM,        0,  20, 118, 120, 100,   0, 170,   0,  13, DRUM_FILTER_OFF, 0, 0 },
	{ DM_FM,         0,  30, 148, 110, 180,   0, 160,  70,  17, DRUM_FILTER_OFF, 0, 0 },
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
extern int mod_cur[8];   // Plinky modulation sources (16.16): knob/CV/LFO A, B, X, Y ...
// modulation sources, numbered like Plinky's EModSources (checked in params.h)
enum { MOD_ENV = 1, MOD_PRESSURE, MOD_A, MOD_B, MOD_X, MOD_Y, MOD_RND };
#define HIT_PRESSURE 120 // a drum pad needs at least this much pressure (ignores sensor noise)
#define HIT_CONFIRM 2    // ...on the same row for this many string updates (4 ms apart)
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

// per lane: parameters used by the sounding hit when modulation applies
static DrumParams lane_eff[GROOVE_LANES];
static u8 lane_vel[GROOVE_LANES];
static s8 lane_rnd[GROOVE_LANES];  // random value of the hit (modulation source)
static u8 eff_live;                // lanes whose hit follows knobs / LFOs every block
static u32 rnd_state = 0x2545F491u;

static u8 hit_row[GROOVE_LANES], hit_count[GROOVE_LANES]; // drum pad touch being confirmed

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
	pat.djfilter = DRUM_FILTER_OFF;
	for (int l = 0; l < GROOVE_LANES; ++l) {
		pat.lane[l].length = GROOVE_PAGE;
		pat.lane[l].snd = default_kit[l];
	}
}

// copy a slot from flash into RAM (only when no flash write is in progress)
static void load_slot(int slot) {
	const GroovePattern* src = (const GroovePattern*)GetSavedGroovePattern(slot);
	if (src && src->magic == GROOVE_MAGIC && src->version >= 1 && src->version <= GROOVE_VERSION) {
		memcpy(&pat, src, sizeof(pat));
		if (pat.version < 5) { // v1-v4: the lane sound started one byte later and had no srr; no DJ filter
			for (int l = 0; l < GROOVE_LANES; ++l) {
				memmove(&pat.lane[l].snd, (u8*)&pat.lane[l] + 4, 12);
				pat.lane[l].snd.srr = 0;
			}
			pat.djfilter = DRUM_FILTER_OFF;
		}
		if (pat.version == 1) { // v1 -> v2: the filter bytes were reserved (0)
			for (int l = 0; l < GROOVE_LANES; ++l) {
				pat.lane[l].snd.filter = DRUM_FILTER_OFF;
				pat.lane[l].snd.reso = 0;
			}
		}
		if (pat.version < 3) // v1/v2: no modulation yet (those bytes were never written)
			pat.nmods = 0;
		else if (pat.version == 3) // v3 -> v4: the 64 locks are dropped, the mod slots move up
			memcpy(pat.mod, (const u8*)src + V3_MODS_OFFSET, sizeof(pat.mod));
		pat.version = GROOVE_VERSION;
		pat.nlocks_unused = 0;
		if (pat.nmods > GROOVE_MAX_MODS) pat.nmods = 0;
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
	eff_live = 0;
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
// Modulation

static int find_mod(int lane, int field, int source) {
	for (int i = 0; i < pat.nmods; ++i)
		if (pat.mod[i].lane == lane && pat.mod[i].field == field && pat.mod[i].source == source)
			return i;
	return -1;
}

static int lane_mods(int lane, int live_only) {
	for (int i = 0; i < pat.nmods; ++i)
		if (pat.mod[i].lane == lane && (!live_only || (pat.mod[i].source >= MOD_A && pat.mod[i].source <= MOD_Y)))
			return 1;
	return 0;
}

// lane sound + modulation -> lane_eff[l]
static void compute_eff(int l) {
	DrumParams* e = &lane_eff[l];
	*e = pat.lane[l].snd;
	u8* eb = (u8*)e;
	for (int i = 0; i < pat.nmods; ++i) {
		GrooveMod* m = &pat.mod[i];
		if (m->lane != l)
			continue;
		float src = 0.f;
		switch (m->source) {
		case MOD_PRESSURE: src = lane_vel[l] * (1.f / 7.f); break; // touch / step velocity
		case MOD_A: case MOD_B: case MOD_X: case MOD_Y: src = clampf(mod_cur[m->source] * (1.f / 65536.f), -1.f, 1.f); break;
		case MOD_RND: src = (m->amount > 0) ? (lane_rnd[l] + 128) * (1.f / 255.f) : lane_rnd[l] * (1.f / 127.f); break;
		}
		float delta = src * m->amount * (1.f / 127.f);
		if (m->field == GF_TUNE)
			e->tune = clampi((int)e->tune + (int)(delta * 127.f), -128, 127);
		else
			eb[m->field] = clampi((int)eb[m->field] + (int)(delta * 255.f), 0, 255);
	}
}

// trigger a drum lane
static void trigger_lane(int l, int vel) {
	u8 bit = 1 << l;
	lane_vel[l] = vel;
	rnd_state = rnd_state * 1664525u + 1013904223u;
	lane_rnd[l] = (s8)(rnd_state >> 24);
	if (lane_mods(l, 0)) {
		compute_eff(l);
		drums_trigger(l, &lane_eff[l], vel);
		if (lane_mods(l, 1)) eff_live |= bit; else eff_live &= ~bit;
	}
	else {
		drums_trigger(l, &pat.lane[l].snd, vel); // plain: follows edits live
		eff_live &= ~bit;
	}
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
	if (*pressure > HIT_PRESSURE) {
		if (!(live_down & bit)) {
			// a new hit counts once it reads the same drum row twice in a row (4 ms):
			// sensor noise and the first readings of a step / mute press don't fire drums
			int row = clampi(position >> 8, 0, 7);
			if (row >= DRUM_ROWS)
				hit_count[fi] = 0;
			else if (hit_count[fi] && hit_row[fi] == row) {
				if (++hit_count[fi] >= HIT_CONFIRM) {
					live_down |= bit;
					lane_sel = fi;
					flash[fi] = 255;
					int vel = row_vel[row]; // the row sets the velocity
					if (synth)
						trigger_lane(fi, vel);
					live_record(fi, vel);
				}
			}
			else {
				hit_row[fi] = row;
				hit_count[fi] = 1;
			}
		}
		if (synth)
			*pressure = NO_TOUCH; // the Plinky voice stays free for melody
	}
	else if (*pressure > 0) {
		if (synth)
			*pressure = NO_TOUCH; // light / noisy contact: never sounds the Plinky voice either
	}
	else {
		live_down &= ~bit;
		hit_count[fi] = 0;
	}
}

// ---------------------------------------------------------------------------
// Drum lane sound and drum volume (printed / unprinted param pads)

static int model_pos(int model) {
	for (int i = 0; i < DM_LAST; ++i)
		if (drum_model_order[i] == model)
			return i;
	return 0;
}

// floor(value / scale), rounded up instead when the value moved up from cur:
// the encoder moves 1 unit at a time, smaller than one stored step
static int qdiv(int value, int cur, int scale) {
	int q = value >= 0 ? value / scale : -((-value + scale - 1) / scale);
	if (value > cur && q * scale < value)
		q++;
	return q;
}

// stored byte of a field of the selected lane
static u8 field_byte(int field) {
	return ((u8*)&pat.lane[lane_sel].snd)[field];
}

static int byte_to_value(int field, u8 b) {
	switch (field) {
	case GF_MODEL: return ((model_pos(b) * 2 + 1) * 1024) / (DM_LAST * 2);
	case GF_TUNE:  return (s8)b * 8;
	case GF_COLOR: return b * 8 - 1024; // on a signed pad (osc interval)
	}
	return b * 4;
}

int groove_get_field(int field) {
	if (field == GF_VOLUME)
		return pat.volume * 4;
	if (field == GF_DJFILTER)
		return pat.djfilter * 4;
	if (field < 0 || field > GF_SRR)
		return 0;
	return byte_to_value(field, field_byte(field));
}

void groove_set_field(int field, int value) {
	if (field == GF_VOLUME || field == GF_DJFILTER) { // whole drum bus
		u8* b = (field == GF_VOLUME) ? &pat.volume : &pat.djfilter;
		*b = clampi(qdiv(value, *b * 4, 4), 0, 255);
		mark_dirty();
		return;
	}
	if (field < 0 || field > GF_SRR)
		return;
	u8 cur = field_byte(field);
	int curv = byte_to_value(field, cur);
	int b;
	switch (field) {
	case GF_MODEL: {
		int pos = model_pos(cur);
		int delta = value - curv;
		if (millis() - lastencodertime < 50 && delta != 0 && abs(delta) <= 64)
			pos += (delta > 0) ? 1 : -1; // encoder: one model per detent
		else
			pos = (maxi(value, 0) * DM_LAST) / 1024; // slider: absolute
		b = drum_model_order[clampi(pos, 0, DM_LAST - 1)];
		break; }
	case GF_TUNE:  b = (u8)(s8)clampi(qdiv(value, curv, 8), -128, 127); break;
	case GF_COLOR: b = clampi(qdiv(value + 1024, curv + 1024, 8), 0, 255); break;
	default:       b = clampi(qdiv(value, curv, 4), 0, 255); break;
	}
	if (b == cur)
		return;
	if (field == GF_MODEL)
		gate[lane_sel] = 0;
	((u8*)&pat.lane[lane_sel].snd)[field] = b;
	mark_dirty();
}

// modulation amount of a field of the selected lane (param units: +-1016)
int groove_get_mod(int field, int source) {
	if (field <= GF_MODEL || field > GF_SRR)
		return 0;
	int i = find_mod(lane_sel, field, source);
	return i < 0 ? 0 : pat.mod[i].amount * 8;
}

void groove_set_mod(int field, int source, int value) {
	if (field <= GF_MODEL || field > GF_SRR || source < MOD_ENV || source > MOD_RND)
		return;
	int i = find_mod(lane_sel, field, source);
	int cur = i < 0 ? 0 : pat.mod[i].amount * 8;
	int a = clampi(qdiv(value, cur, 8), -127, 127);
	if (i >= 0 && a == pat.mod[i].amount)
		return;
	if (a == 0) {
		if (i >= 0)
			pat.mod[i] = pat.mod[--pat.nmods];
	}
	else {
		if (i < 0) {
			if (pat.nmods >= GROOVE_MAX_MODS) {
				ShowMessage(F_20_BOLD, "mod slots full", 0);
				return;
			}
			i = pat.nmods++;
			pat.mod[i].lane = lane_sel;
			pat.mod[i].field = field;
			pat.mod[i].source = source;
		}
		pat.mod[i].amount = a;
	}
	mark_dirty();
}

static const char* const field_names[] = {
	[GF_MODEL] = "Drum Type", [GF_TUNE] = "Tune", [GF_SWEEP] = "Pitch Sweep", [GF_DECAY] = "Decay",
	[GF_SNAP] = "Snap", [GF_COLOR] = "Colour", [GF_DRIVE] = "Drive", [GF_LEVEL] = "Level",
	[GF_DLSEND] = "Delay Send", [GF_RVSEND] = "Reverb Send", [GF_FILTER] = "Filter", [GF_RESO] = "Resonance",
	[GF_SRR] = "Sample Rate", [GF_VOLUME] = "Drum Vol", [GF_DJFILTER] = "Drums Filter",
};

const char* groove_field_name(int field) {
	return (field >= 0 && field <= GF_DJFILTER) ? field_names[field] : "";
}

const char* groove_lane_label(void) {
	static char label[] = "Drum 1";
	label[5] = '1' + lane_sel;
	return label;
}

// value text for the OLED (buf has room for 32 chars)
const char* groove_field_str(int field, char* buf) {
	if (field == GF_VOLUME) {
		sprintf(buf, "%d", (pat.volume * 100 + 127) / 255);
		return buf;
	}
	u8 b = (field == GF_DJFILTER) ? pat.djfilter : field_byte(field);
	switch (field) {
	case GF_SRR:
		if (!b)
			return "Off";
		break;
	case GF_MODEL:
		return drum_model_names[b < DM_LAST ? b : 0];
	case GF_TUNE: { // semitones, one decimal
		int st10 = ((s8)b * 120) / 128;
		sprintf(buf, "%c%d.%d", st10 < 0 ? '-' : '+', abs(st10) / 10, abs(st10) % 10);
		return buf; }
	case GF_FILTER:
	case GF_DJFILTER:
		if (b > DRUM_FILTER_OFF - 5 && b < DRUM_FILTER_OFF + 5)
			return "Off";
		if (b < DRUM_FILTER_OFF)
			sprintf(buf, "LP %d", (b * 100) / 128);
		else
			sprintf(buf, "HP %d", ((b - DRUM_FILTER_OFF) * 100) / 127);
		return buf;
	}
	sprintf(buf, "%d", (b * 100 + 127) / 255);
	return buf;
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
		trigger_lane(l, vel);
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

// ---------------------------------------------------------------------------
// Plinky voice limit in Groove mode: at most 4 Plinky voices sound at once.
// A new note takes over the oldest one (released notes first); the stolen
// voice fades out in ~20 ms (see the hook in UpdateEnvelope) and stays silent
// until its string is played again.

#define PLINKY_VOICES_MAX 4
#define VOICE_SILENT 1e-4f

u8 groove_voice_kill;
static u32 voice_age[8], voice_clock;

u8 groove_voice_alloc(u8 down, u8 trig, const float* vol0, int stride) {
	u8 kill = groove_voice_kill;
	u8 sounding = 0;
	for (int i = 0; i < 8; ++i) {
		u8 bit = 1 << i;
		float vol = *(const float*)((const char*)vol0 + i * stride);
		if (trig & bit) {
			voice_age[i] = ++voice_clock;
			kill &= ~bit;
		}
		else if (!(down & bit) && vol < VOICE_SILENT)
			kill &= ~bit; // faded out and released: free again
		if (!(kill & bit) && ((trig & bit) || vol > VOICE_SILENT))
			sounding |= bit;
	}
	while (__builtin_popcount(sounding) > PLINKY_VOICES_MAX) {
		// steal: prefer released notes, then held ones; oldest first; never one triggered right now
		int best = -1, best_rank = 3;
		u32 best_age = 0;
		for (int i = 0; i < 8; ++i) {
			u8 bit = 1 << i;
			if (!(sounding & bit))
				continue;
			int rank = (trig & bit) ? 2 : (down & bit) ? 1 : 0;
			if (best < 0 || rank < best_rank || (rank == best_rank && voice_age[i] < best_age)) {
				best = i;
				best_rank = rank;
				best_age = voice_age[i];
			}
		}
		kill |= 1 << best;
		sounding &= ~(1 << best);
	}
	groove_voice_kill = kill;
	return kill;
}

void groove_audio(void) {
	// hits modulated by knobs / CV / LFOs follow them while they ring
	for (int l = 0; l < GROOVE_LANES; ++l)
		if (eff_live & (1 << l))
			compute_eff(l);
	drums_render(pat.volume, pat.djfilter);
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
#define HEAP_RESERVE 64    // the heap only holds newlib's rand() state (~30 bytes)
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
