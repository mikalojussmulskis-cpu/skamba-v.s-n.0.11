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
// Pattern data (format v6, see GROOVEBOX_PLAN.md section 5). One pattern = one
// 2 KB page of Plinky's flash store (logical index 136 + slot). Fields added
// later go at the end; bytes that were never written read back as 0xFF.

#define GROOVE_MAGIC 0x31565247u // "GRV1"
#define GROOVE_VERSION 6 // v2 filter; v3 locks+mods; v4 no locks; v5 srr, DJ filter; v6 filter env, step locks
#define GROOVE_MAX_MODS 24  // per pattern
#define GROOVE_MAX_LOCKS 48 // per pattern
#define LANE_ALL 8          // modulation slots for the whole drum bus (DJ filter)
#define V3_MODS_OFFSET 912 // where v3 stored the modulation slots (after 64 locks)
#define GROOVE_LANES 8
#define GROOVE_STEPS 64
#define GROOVE_PAGE 16
#define NO_SLOT 255

#define STEP_TRIG 0x80 // b7 trig, b4-6 velocity (0 = default), b3 slide and b0-2 note (Acid lanes)
#define STEP_VEL_SHIFT 4
#define STEP_VEL_MASK 7
#define STEP_SLIDE 8      // Acid: glide into the next step's note (no retrigger)
#define STEP_NOTE_MASK 7  // Acid: scale degree above the lane's tune
#define VEL_DEFAULT 5
#define ACID_SETTLE_MS 400 // the model choice must rest on Acid this long before notes are generated

#define LANE_MUTE 1
// b1-b6 of flags: first step of the lane's loop (0 in patterns saved before phase 14)
#define LANE_START(ln) (((ln)->flags >> 1) & 63)
#define LANE_SET_START(ln, s) ((ln)->flags = ((ln)->flags & 1) | (((s) & 63) << 1))

typedef struct GrooveLane { // 16 bytes
	u8 length; // 1..64
	u8 flags;  // b0 LANE_MUTE, b1-b6 loop start
	DrumParams snd; // 14 bytes: model, tune, sweep, decay, snap, colour, drive, level, sends, filter, reso, srr, fenv
} GrooveLane;

// step lock: on this step of this lane, this field has this value
typedef struct GrooveLock { u8 step, lane, field, value; } GrooveLock;
// modulation slot: this field of this lane is modulated by this source
typedef struct GrooveMod { u8 lane, field, source; s8 amount; } GrooveMod;

typedef struct GroovePattern { // 944 bytes
	u32 magic;
	u8 version, kit, preset_link, chain_next, nlocks, swing_override;
	u8 volume;   // drum master volume
	u8 nmods;
	u8 djfilter; // global drum filter (DRUM_FILTER_OFF = off)
	u8 comp;     // drum bus compressor (0 = off); was reserved (0) before
	u8 drive;    // global drum overdrive (0 = off); was reserved (0) before
	u8 srr;      // global drum sample rate reduction (0 = off); was reserved (0) before
	GrooveLane lane[GROOVE_LANES];
	u8 step[GROOVE_LANES][GROOVE_STEPS];
	GrooveMod mod[GROOVE_MAX_MODS];
	GrooveLock lock[GROOVE_MAX_LOCKS];
} GroovePattern;
_Static_assert(sizeof(DrumParams) == 14, "drum params layout");
_Static_assert(sizeof(GrooveLane) == 16, "lane layout");
_Static_assert(sizeof(GroovePattern) == 944, "pattern layout");
// GF_MODEL..GF_RESO are the byte offsets of the fields inside DrumParams
_Static_assert(__builtin_offsetof(DrumParams, model) == GF_MODEL && __builtin_offsetof(DrumParams, tone) == GF_COLOR
	&& __builtin_offsetof(DrumParams, fenv) == GF_FENV && sizeof(DrumParams) == GF_VOLUME, "field numbering");
_Static_assert((sizeof(GroovePattern) & 7) == 0, "flash writes are 8-byte units");

#define VOLUME_DEFAULT 128
#define COMP_DEFAULT 77 // 30 %

// default sound of each model: the default kit, a new lane of that model, and Clear held on a lane
#define FO DRUM_FILTER_OFF
static const DrumParams model_default[DM_LAST] = {
	//            model      tune sweep decay snap colour drive level  dly  rvb  filter reso srr  fenv
	[DM_KICK]   = { DM_KICK,     5, 165,  66,  70, 110,  55, 230,   0,   0,  FO,    0,  0,  FO },
	[DM_SNARE]  = { DM_SNARE,    0,  40,  95, 105, 115,   0, 190,   0,  13,  FO,    0,  0,  FO },
	[DM_CLAP]   = { DM_CLAP,     0,   0,  22,   0, 120,   0, 102,   0,  13,  FO,    0,  0,  FO },
	[DM_CHAT]   = { DM_CHAT,     0,   0,  95,  30, 120,   0, 110,   0,   0,  FO,    0,  0,  FO },
	[DM_OHAT]   = { DM_OHAT,     0,   0,  90,  30, 120,   0, 100,   0,   7,  77,    0,  0, 217 },
	[DM_TOM]    = { DM_TOM,    -40,  90, 120,  30,  60,   0, 210,   0,  10,  FO,    0,  0,  FO },
	[DM_PERC]   = { DM_PERC,     0,   0,  80,   0, 128,  20, 150,   0,   0,  FO,    0,  0,  FO },
	[DM_SAMPLE] = { DM_SAMPLE,   0,   0, 128,   0, 128,   0, 200,   0,   0,  FO,    0,  0,  FO },
	[DM_KICK2]  = { DM_KICK2,   10, 140,  68,  80, 120,  30, 230,   0,   0,  FO,    0,  0,  FO },
	[DM_CYMBAL] = { DM_CYMBAL,   0,   0, 135,  45, 110,   0, 110,   0,  40,  FO,    0,  0,  FO },
	[DM_RIM]    = { DM_RIM,      0,  20, 105, 120, 100,   0, 160,   0,  13,  FO,    0,  0,  FO },
	[DM_FM]     = { DM_FM,       0,  30, 120, 110, 180,   0, 160,  70,  17,  FO,    0,  0,  FO },
	[DM_ZAP]    = { DM_ZAP,      0, 160,  90,  40,  60,   0, 180,   0,   0,  FO,    0,  0,  FO },
	[DM_GLITCH] = { DM_GLITCH,   0, 100, 110, 120, 160,   0, 180,   0,  10,  FO,    0,  0,  FO },
	[DM_FOLD]   = { DM_FOLD,     0, 150, 150,  80, 140,   0, 200,   0,   0,  FO,    0,  0,  FO },
	[DM_CLANG]  = { DM_CLANG,    0,  20, 150, 140, 120,   0, 160,   0,  20,  FO,    0,  0,  FO },
	[DM_CHIRP]  = { DM_CHIRP,    0, 180, 110,  80,  60,   0, 170,   0,  10,  FO,    0,  0,  FO },
	[DM_CRUSH]  = { DM_CRUSH,    0,  80, 110, 100, 170,   0, 180,   0,   0,  FO,    0,  0,  FO },
	[DM_BASS]   = { DM_BASS,     0,  30, 115,   0,  13,   0, 128,   0,   0,  51,   77,  0, 166 },
	[DM_ACID]   = { DM_ACID,     0,  60,  77, 150,   0,  20, 128,   0,  10,  64,  190,  0, 208 },
	[DM_LXR_DRUM] = { DM_LXR_DRUM, 0, 110, 80,  90,  30,   0, 220,   0,   0,  FO,    0,  0,  FO },
	[DM_LXR_SNARE] = { DM_LXR_SNARE, 0, 50, 70,  60, 150,   0, 200,   0,  13,  FO,    0,  0,  FO },
};
#undef FO
static const u8 kit_models[GROOVE_LANES] = { DM_KICK, DM_SNARE, DM_CLAP, DM_CHAT, DM_OHAT, DM_RIM, DM_BASS, DM_ACID };

// ---------------------------------------------------------------------------
// Pad layout in the drum grid
#define DRUM_ROWS 5 // R1-R5: drum pads, R1 loudest .. R5 quietest
#define ROW_MUTE 5  // R6: mute row
#define ROW_STEPS 6 // R7, R8: 16 steps of the selected lane
#define ROW_LOOP 8  // pad_row: Prev / Next held, a step pad of the 64-step view
#define ROW_FX 9    // pad_row: a momentary FX pad (Shift + Arp / Seq row)
#define LONGPRESS_MS 400
#define NO_TOUCH (-2048) // TOUCH_MIN_PRES
static const u8 row_vel[DRUM_ROWS] = { 7, 5, 4, 2, 1 }; // 100, 50, 30, 8, 3 % (see vel_gain in drums.c)

// ---------------------------------------------------------------------------
// From plinky.c (unity build globals)
extern u8 led_ram[9][8];
extern int lastencodertime;
extern int mod_cur[8];   // Plinky modulation sources (16.16): knob/CV/LFO A, B, X, Y ...
// modulation sources, numbered like Plinky's EModSources (checked in params.h)
enum { MOD_ENV = 1, MOD_PRESSURE, MOD_A, MOD_B, MOD_X, MOD_Y, MOD_RND };
#define NO_STEP 255
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
static u8 page;                   // 16-step page shown on R7/R8 (see view_page)
static int page_in_loop(int p);
static int view_page(void);
static u8 running;                // transport was running last tick
static u8 recording_now;
static u8 drums_play;             // drum transport running (own Play button in the drum grid)
static u8 drums_pending;          // PEND_*: a start waiting for the next bar
static u8 gate_len = 8;
static u8 step_ticks_now = 60;
static u8 ticks_in_step;
static u16 bar_step;              // shared bar clock: 16ths since the first sequencer started
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
static u8 lane_step[GROOVE_LANES];           // step that triggered the hit (NO_STEP: live)
static u8 lock_step = NO_STEP, lock_lane;    // step lock mode: edits go to this step
static u8 lane_loop[GROOVE_LANES];           // times the lane has wrapped since the start (conditions)
static u8 slide_from;                        // bitmask: the lane's last fired step was a slide (Acid)
static u8 acid_gen;                          // lane + 1 whose model was just set to Bass / Acid (starting sound, notes), 0 none
static u32 acid_gen_ms;
static u8 acid_prev_model;                   // the lane's model before the current scroll through the models
static u8 model_edit_lane = 255;             // lane / time of the last model change (a scroll = changes < 400 ms apart)
static u32 model_edit_ms;
static u8 fired;                             // bitmask: lanes whose current step triggered a synth hit (ratchet)

// momentary FX (GF_ROLL .. GF_CRUSH), on while their pad is held
enum { FX_ROLL, FX_RATCHET, FX_REVERSE, FX_PITCHFALL, FX_GATE, FX_CRUSH, FX_COUNT };
#define FX_BIT(field) (1 << ((field) - GF_ROLL))
#define FX_IS(field) ((field) >= GF_ROLL && (field) <= GF_CRUSH)
#if GROOVE_FX
static u8 fx_on;                             // bitmask of active FX
static u8 fx_set[FX_COUNT] = { 1, 0, 0, 128, 1, 128 }; // settings (not saved): roll 1/8, ratchet x2, -, fall 50 %, gate 1/16, crush 50 %
static u8 fx_field[8];                       // FX field held by each column's finger
static u8 roll_base[GROOVE_LANES];           // lane positions when the roll started
static u8 roll_count;                        // 16ths since the roll started
static u8 ratchet_n;                         // retriggers done in the current step
static u8 gate_phase;                        // trance gate: blocks since the gate period started
static float fx_pitch;                       // pitch fall: semitones (<= 0) added to every hit
static const u8 roll_len[4] = { 1, 2, 4, 8 };      // steps repeated: 1/16, 1/8, 1/4, 1/2
static const u8 ratchet_rate[4] = { 2, 3, 4, 8 };  // retriggers per step
static const char* const fx_div_names[4] = { "1/16", "1/8", "1/4", "1/2" };
static const char* const fx_gate_names[4] = { "1/8", "1/16", "1/32", "1/64" };
static const char* const fx_ratchet_names[4] = { "x2", "x3", "x4", "x8" };
static const char* const fx_short[FX_COUNT] = { "roll", "ratchet", "reverse", "fall", "gate", "crush" };
static int fx_get(int field);
static void fx_setv(int field, int value);
#else
#define fx_on 0
#define fx_pitch 0.f
#endif
static u8 dj_eff = DRUM_FILTER_OFF;          // DJ filter after modulation

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

static void fx_all_off(void) {
#if GROOVE_FX
	fx_on = 0;
	fx_pitch = 0.f;
#endif
	fired = 0;
}

static void stop_sound(void) {
	memset(gate, 0, sizeof(gate));
	skip_next = 0;
	running = 0;
	fx_all_off();
	drums_play = drums_pending = 0;
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
	pat.comp = COMP_DEFAULT;
	pat.djfilter = DRUM_FILTER_OFF;
	for (int l = 0; l < GROOVE_LANES; ++l) {
		pat.lane[l].length = GROOVE_PAGE;
		pat.lane[l].snd = model_default[kit_models[l]];
	}
}

// copy a slot from flash into RAM (only when no flash write is in progress)
static void load_slot(int slot) {
	const GroovePattern* src = (const GroovePattern*)GetSavedGroovePattern(slot);
	if (src && src->magic == GROOVE_MAGIC && src->version >= 1 && src->version <= GROOVE_VERSION) {
		memcpy(&pat, src, sizeof(pat));
		if (pat.version < 6) { // old lanes: length, start, flags, (pad,) sound; no srr before v5, no filter env
			for (int l = 0; l < GROOVE_LANES; ++l) {
				u8 old[16];
				memcpy(old, &pat.lane[l], 16);
				pat.lane[l].flags = old[2];
				memcpy(&pat.lane[l].snd, old + (pat.version < 5 ? 4 : 3), pat.version < 5 ? 12 : 13);
				if (pat.version < 5) pat.lane[l].snd.srr = 0;
				pat.lane[l].snd.fenv = DRUM_FILTER_OFF;
			}
			if (pat.version < 5) pat.djfilter = DRUM_FILTER_OFF;
			pat.nlocks = 0;
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
		if (pat.nmods > GROOVE_MAX_MODS) pat.nmods = 0;
		if (pat.nlocks > GROOVE_MAX_LOCKS) pat.nlocks = 0;
		for (int l = 0; l < GROOVE_LANES; ++l) { // never trust flash blindly
			GrooveLane* ln = &pat.lane[l];
			if (ln->length < 1 || ln->length > GROOVE_STEPS)
				ln->length = GROOVE_PAGE;
			if (ln->snd.model >= DM_LAST)
				ln->snd.model = DM_KICK;
		}
	}
	else
		default_pattern();
	cur_slot = slot;
	memset(gate, 0, sizeof(gate));
	skip_next = 0;
	slide_from = 0;
	acid_gen = 0;
	eff_live = 0;
	lock_step = NO_STEP;
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
	lock_step = NO_STEP;
	groove_active = !groove_active;
	groove_grid = groove_active;
	if (groove_active) {
		clearlatch();
		pat.volume = VOLUME_DEFAULT; // drum master volume starts at 50 % every time
	}
	ShowMessage(F_20_BOLD, groove_active ? "groove on" : "groove off", 0);
}

void groove_view_toggle(void) {
	if (!groove_active) {
		groove_toggle();
		return;
	}
	groove_grid = !groove_grid;
	lock_step = NO_STEP;
	fx_all_off();
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

static int find_lock(int lane, int step, int field) {
	for (int i = 0; i < pat.nlocks; ++i)
		if (pat.lock[i].step == step && pat.lock[i].lane == lane && pat.lock[i].field == field)
			return i;
	return -1;
}

static int step_locks(int lane, int step) {
	int n = 0;
	for (int i = 0; i < pat.nlocks; ++i)
		n += pat.lock[i].step == step && pat.lock[i].lane == lane;
	return n;
}

static void clear_step_locks(int lane, int step) {
	for (int i = pat.nlocks - 1; i >= 0; --i)
		if (pat.lock[i].step == step && pat.lock[i].lane == lane)
			pat.lock[i] = pat.lock[--pat.nlocks];
}

static void set_lock(int field, u8 value) {
	int i = find_lock(lock_lane, lock_step, field);
	u8 base = field <= GF_FENV ? ((u8*)&pat.lane[lock_lane].snd)[field] : 0;
	if (value == base) { // same as the lane: no lock needed
		if (i >= 0)
			pat.lock[i] = pat.lock[--pat.nlocks];
		return;
	}
	if (i < 0) {
		if (pat.nlocks >= GROOVE_MAX_LOCKS) {
			ShowMessage(F_20_BOLD, "locks full", 0);
			return;
		}
		i = pat.nlocks++;
		pat.lock[i].step = lock_step;
		pat.lock[i].lane = lock_lane;
		pat.lock[i].field = field;
	}
	pat.lock[i].value = value;
}

static int lane_mods(int lane, int live_only) {
	for (int i = 0; i < pat.nmods; ++i)
		if (pat.mod[i].lane == lane && (!live_only || (pat.mod[i].source >= MOD_A && pat.mod[i].source <= MOD_Y)))
			return 1;
	return 0;
}

static float mod_value(GrooveMod* m, int l) {
	switch (m->source) {
	case MOD_PRESSURE: return lane_vel[l] * (1.f / 7.f); // touch / step velocity
	case MOD_A: case MOD_B: case MOD_X: case MOD_Y: return clampf(mod_cur[m->source] * (1.f / 65536.f), -1.f, 1.f);
	case MOD_RND: return (m->amount > 0) ? (lane_rnd[l] + 128) * (1.f / 255.f) : lane_rnd[l] * (1.f / 127.f);
	}
	return 0.f;
}

// lane sound + the step's locks + modulation -> lane_eff[l]
// step conditions (stored as a lock with field GF_COND; 0 = always = no lock)
enum { COND_ALWAYS, COND_25, COND_50, COND_75, COND_1_2, COND_2_2, COND_1_3, COND_2_3, COND_3_3,
	COND_1_4, COND_2_4, COND_3_4, COND_4_4, COND_FIRST, COND_NOT_FIRST, COND_LAST };
static const char* const cond_names[COND_LAST] = { "always", "25%", "50%", "75%", "1:2", "2:2", "1:3", "2:3", "3:3",
	"1:4", "2:4", "3:4", "4:4", "1st", "not 1st" };
static int cond_ok(int c, int l) {
	static const u8 ab[9] = { 0x12, 0x22, 0x13, 0x23, 0x33, 0x14, 0x24, 0x34, 0x44 }; // a:b nibbles
	if (c >= COND_25 && c <= COND_75) {
		rnd_state = rnd_state * 1664525u + 1013904223u;
		return (rnd_state >> 24) < (u32)(c * 64);
	}
	if (c >= COND_1_2 && c <= COND_4_4)
		return lane_loop[l] % (ab[c - COND_1_2] & 15) == (ab[c - COND_1_2] >> 4) - 1;
	if (c == COND_FIRST) return lane_loop[l] == 0;
	if (c == COND_NOT_FIRST) return lane_loop[l] != 0;
	return 1;
}

static void compute_eff(int l) {
	DrumParams* e = &lane_eff[l];
	*e = pat.lane[l].snd;
	u8* eb = (u8*)e;
	int rnd_tune = 0, tune_locked = 0;
	if (lane_step[l] != NO_STEP)
		for (int i = 0; i < pat.nlocks; ++i)
			if (pat.lock[i].lane == l && pat.lock[i].step == lane_step[l] && pat.lock[i].field <= GF_FENV) {
				eb[pat.lock[i].field] = pat.lock[i].value;
				tune_locked |= pat.lock[i].field == GF_TUNE;
			}
	for (int i = 0; i < pat.nmods; ++i) {
		GrooveMod* m = &pat.mod[i];
		if (m->lane != l)
			continue;
		float delta = mod_value(m, l) * m->amount * (1.f / 127.f);
		if (m->field == GF_TUNE) {
			e->tune = clampi((int)e->tune + (int)(delta * 120.f), -120, 120);
			rnd_tune |= m->source == MOD_RND;
		}
		else
			eb[m->field] = clampi((int)eb[m->field] + (int)(delta * 255.f), 0, 255);
	}
	if (fx_pitch < 0.f) // pitch fall FX
		e->tune = clampi(e->tune + (int)(fx_pitch * 5.f), -120, 120);
	if (rnd_tune || e->model == DM_BASS || e->model == DM_ACID) { // random pitch, and bass / acid notes, follow Plinky's scale (tune 0 = root)
		int st = (e->tune + (e->tune >= 0 ? 2 : -2)) / 5;
		if (e->model == DM_ACID && lane_step[l] != NO_STEP && !tune_locked) // the step's note (a Tune lock replaces it)
			st += GrooveScaleDegree(pat.step[l][lane_step[l]] & STEP_NOTE_MASK);
		e->tune = clampi(GrooveScaleQuantize(st) * 5, -120, 120);
	}
}

// trigger a drum lane; step = sequencer step (NO_STEP for live hits);
// gate / legato: Acid notes (see drums_note)
static void trigger_lane(int l, int vel, int step, int gate, int legato) {
	u8 bit = 1 << l;
	lane_vel[l] = vel;
	lane_step[l] = step;
	rnd_state = rnd_state * 1664525u + 1013904223u;
	lane_rnd[l] = (s8)(rnd_state >> 24);
	int m = pat.lane[l].snd.model;
	int note = m == DM_BASS || m == DM_ACID; // tune snapped to the scale (and the step's note) by compute_eff
	if (note || fx_pitch < 0.f || lane_mods(l, 0) || (step != NO_STEP && step_locks(l, step))) {
		compute_eff(l);
		drums_note(l, &lane_eff[l], vel, gate, legato);
		if (note || lane_mods(l, 1)) eff_live |= bit; else eff_live &= ~bit; // notes: edits still apply while they hold
	}
	else {
		drums_note(l, &pat.lane[l].snd, vel, gate, legato); // plain: follows edits live
		eff_live &= ~bit;
	}
}

// the lane's model was set to Bass or Acid (and the choice settled): a lane that
// comes from a drum model (or has Filter Env off) gets the model's starting
// sound; coming from Bass / Acid keeps the sound. Acid also gets a new line: steps get a note (scale degree, mostly the
// root with steps and octave jumps), accents and slides; old locks of the lane
// go (a Tune lock would replace the note).
static void note_model_chosen(int l) {
	DrumParams* d = &pat.lane[l].snd;
	if (d->fenv == DRUM_FILTER_OFF || (acid_prev_model != DM_ACID && acid_prev_model != DM_BASS)) {
		*d = model_default[d->model];
		mark_dirty();
	}
	if (d->model != DM_ACID)
		return;
	for (int i = pat.nlocks - 1; i >= 0; --i)
		if (pat.lock[i].lane == l)
			pat.lock[i] = pat.lock[--pat.nlocks];
	int deg = 0;
	for (int i = 0; i < GROOVE_STEPS; ++i) {
		rnd_state = rnd_state * 1664525u + 1013904223u;
		u32 r = rnd_state >> 8;
		int trig = (i & 15) == 0 || (r & 255) < 168; // two thirds of the steps, always the first of a bar
		int a = (r >> 8) & 255;
		if (a < 80) deg = 0;      // back to the root ...
		else if (a < 110) deg = 7; // ... up to the octave ...
		else deg = clampi(deg + ((a & 1) ? 1 : -1) * (1 + ((a >> 1) & 1)), 0, 7); // ... or a degree or two away
		int b = (r >> 16) & 255;
		int acc = b < 70, sl = b >= 190; // accents on 27 %, slides on 25 %
		pat.step[l][i] = trig ? STEP_TRIG | ((acc ? 7 : 6) << STEP_VEL_SHIFT) | (sl ? STEP_SLIDE : 0) | deg : 0;
	}
	mark_dirty();
	ShowMessage(F_20_BOLD, "acid line", "new notes");
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
	else if (row >= ROW_STEPS && row < 8) {
		int idx = view_page() * GROOVE_PAGE + (row - ROW_STEPS) * 8 + fi;
		if (trig) {
			pad_row[fi] = row;
			pad_time[fi] = millis();
			pad_long &= ~bit;
			hit_row[fi] = vel_from_pressure(pressure); // velocity for the toggle on release
		}
		else if (pad_row[fi] == row && !(pad_long & bit) && millis() - pad_time[fi] > LONGPRESS_MS) {
			pad_long |= bit; // long press: enter / leave step lock mode for this step
			if (lock_step == idx && lock_lane == lane_sel)
				lock_step = NO_STEP;
			else {
				lock_step = idx;
				lock_lane = lane_sel;
				u8* s = &pat.step[lane_sel][idx];
				if (!(*s & STEP_TRIG)) { // a locked step needs a hit
					*s = STEP_TRIG | (hit_row[fi] << STEP_VEL_SHIFT);
					mark_dirty();
				}
			}
		}
	}
}

#if GROOVE_FX
static int roll_pos(int l, int k) { // k steps after the roll's base position, inside the lane's loop
	GrooveLane* ln = &pat.lane[l];
	int start = LANE_START(ln);
	int rel = (((roll_base[l] - start) & 63) + k) % ln->length;
	return (start + rel) & 63;
}

static void fx_release(int field) {
	if (!(fx_on & FX_BIT(field)))
		return;
	fx_on &= ~FX_BIT(field);
	if (field == GF_ROLL) // carry on where the pattern would have been
		for (int l = 0; l < GROOVE_LANES; ++l)
			lane_pos[l] = roll_pos(l, roll_count);
	else if (field == GF_PITCHFALL)
		fx_pitch = 0.f;
}

#endif

void groove_fx_down(int field, int fi) {
#if !GROOVE_FX
	(void)field; (void)fi;
#else
	if (!FX_IS(field))
		return;
	pad_row[fi] = ROW_FX;
	fx_field[fi] = field;
	if (fx_on & FX_BIT(field))
		return;
	fx_on |= FX_BIT(field);
	if (field == GF_ROLL) {
		memcpy(roll_base, lane_pos, sizeof(roll_base));
		roll_count = 0;
	}
	else if (field == GF_PITCHFALL)
		fx_pitch = 0.f;
#endif
}

// Prev / Next held: the 64 pads are the selected lane's steps; tap = loop start / end
// of the selected lane, hold the pad = of all lanes
static void set_loop(int l, int step, int start) {
	GrooveLane* ln = &pat.lane[l];
	if (start)
		LANE_SET_START(ln, step);
	else {
		int n = ((step - LANE_START(ln)) & 63) + 1;
		ln->length = n;
	}
	if (((lane_pos[l] - LANE_START(ln)) & 63) >= ln->length)
		lane_pos[l] = LANE_START(ln);
}

void groove_loop_pad(int fi, int step, int start, int trig) {
	u8 bit = 1 << fi;
	if (trig) {
		pad_row[fi] = ROW_LOOP;
		pad_time[fi] = millis();
		pad_long &= ~bit;
		set_loop(lane_sel, step & 63, start);
		mark_dirty();
	}
	else if (pad_row[fi] == ROW_LOOP && !(pad_long & bit) && millis() - pad_time[fi] > LONGPRESS_MS) {
		pad_long |= bit;
		for (int l = 0; l < GROOVE_LANES; ++l)
			set_loop(l, step & 63, start);
		mark_dirty();
		ShowMessage(F_20_BOLD, start ? "start: all lanes" : "end: all lanes", 0);
	}
}

void groove_page(int delta) { // only pages the selected lane's loop reaches
	int p = view_page();
	for (int i = 1; i < 4; ++i) {
		int q = (p + delta * i) & 3;
		if (page_in_loop(q)) {
			page = q;
			ShowMessage(F_20_BOLD, (const char*[]){ "steps 1-16", "steps 17-32", "steps 33-48", "steps 49-64" }[q], 0);
			return;
		}
	}
}

void groove_pad_up(int fi) {
	u8 bit = 1 << fi;
	int row = pad_row[fi];
#if GROOVE_FX
	if (row == ROW_FX)
		fx_release(fx_field[fi]);
	else
#endif
	if (row == ROW_MUTE && !(pad_long & bit)) {
		pat.lane[fi].flags ^= LANE_MUTE;
		mark_dirty();
	}
	else if (row >= ROW_STEPS && row < 8 && !(pad_long & bit)) { // short tap: toggle the step
		int idx = view_page() * GROOVE_PAGE + (row - ROW_STEPS) * 8 + fi;
		u8* s = &pat.step[lane_sel][idx];
		if (*s & STEP_TRIG) {
			*s = 0;
			clear_step_locks(lane_sel, idx);
			if (lock_step == idx && lock_lane == lane_sel)
				lock_step = NO_STEP;
		}
		else
			*s = STEP_TRIG | (hit_row[fi] << STEP_VEL_SHIFT);
		mark_dirty();
	}
	pad_row[fi] = 255;
}

// Clear held in the drum grid, on the selected lane. Two stages:
// 1) the lane has steps: they go, with their locks / conditions, and the loop goes back to 1-16;
// 2) the lane is already empty: its sound goes back to its model's default and its modulation goes.
// Mute and model stay.
void groove_clear_lane(void) {
	int l = lane_sel;
	GrooveLane* ln = &pat.lane[l];
	int has_steps = 0;
	for (int i = 0; i < GROOVE_STEPS; ++i)
		has_steps |= pat.step[l][i] & STEP_TRIG;
	if (has_steps) {
		memset(pat.step[l], 0, sizeof(pat.step[l]));
		for (int i = pat.nlocks - 1; i >= 0; --i)
			if (pat.lock[i].lane == l)
				pat.lock[i] = pat.lock[--pat.nlocks];
		ln->length = GROOVE_PAGE;
		LANE_SET_START(ln, 0);
	}
	else {
		for (int i = pat.nmods - 1; i >= 0; --i)
			if (pat.mod[i].lane == l)
				pat.mod[i] = pat.mod[--pat.nmods];
		ln->snd = model_default[ln->snd.model < DM_LAST ? ln->snd.model : DM_KICK];
	}
	if (lock_step != NO_STEP && lock_lane == l)
		lock_step = NO_STEP;
	if (acid_gen == l + 1)
		acid_gen = 0; // no generated line: the lane stays empty
	gate[l] = 0;
	slide_from &= ~(1 << l);
	eff_live &= ~(1 << l);
	if (((lane_pos[l] - LANE_START(ln)) & 63) >= ln->length)
		lane_pos[l] = 0;
	mark_dirty();
	ShowMessage(F_20_BOLD, lane_names[l], has_steps ? "steps cleared" : "sound reset");
}

int groove_lock_active(void) {
	return lock_step != NO_STEP;
}

// encoder click in step lock mode: remove the step's locks
void groove_lock_clear(void) {
	clear_step_locks(lock_lane, lock_step);
	mark_dirty();
	ShowMessage(F_20_BOLD, "locks cleared", 0);
}

// the 16-step pages (R7 / R8) the selected lane's loop reaches; another page is never shown
static int page_in_loop(int p) {
	GrooveLane* ln = &pat.lane[lane_sel];
	int start = LANE_START(ln);
	for (int i = 0; i < GROOVE_PAGE; ++i)
		if (((p * GROOVE_PAGE + i - start) & 63) < ln->length)
			return 1;
	return 0;
}
static int view_page(void) {
	return page_in_loop(page) ? page : LANE_START(&pat.lane[lane_sel]) / GROOVE_PAGE;
}

static int next_pos(int l) {
	GrooveLane* ln = &pat.lane[l];
	int start = LANE_START(ln), rel = (lane_pos[l] - start) & 63;
	if (fx_on & FX_BIT(GF_REVERSE))
		rel = (rel == 0 || rel >= ln->length) ? ln->length - 1 : rel - 1;
	else
		rel = (rel + 1 >= ln->length) ? 0 : rel + 1;
	return (start + rel) & 63;
}

// Record + Play: write a live hit onto the nearest step
static void live_record(int l, int vel) {
	if (!recording_now || !drums_play || !running)
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
					if (lock_step == NO_STEP) // in step lock mode the lane stays put
						lane_sel = fi;
					flash[fi] = 255;
					int vel = row_vel[row]; // the row sets the velocity
					if (synth)
						trigger_lane(fi, vel, NO_STEP, 0, 0);
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

// stored byte of a field of the selected lane (the lock of the step in lock mode)
static u8 field_byte(int field) {
	if (lock_step != NO_STEP) {
		int i = find_lock(lock_lane, lock_step, field);
		return i >= 0 ? pat.lock[i].value : ((u8*)&pat.lane[lock_lane].snd)[field];
	}
	return ((u8*)&pat.lane[lane_sel].snd)[field];
}

static int cond_value(void) { // condition of the locked step
	int i = lock_step != NO_STEP ? find_lock(lock_lane, lock_step, GF_COND) : -1;
	return i >= 0 ? pat.lock[i].value : 0;
}

static int byte_to_value(int field, u8 b) {
	switch (field) {
	case GF_MODEL: return ((model_pos(b) * 2 + 1) * 1024) / (DM_LAST * 2);
	case GF_COND:  return ((b * 2 + 1) * 1024) / (COND_LAST * 2);
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
	if (field == GF_COMP)
		return pat.comp * 4;
	if (field == GF_BUSDRIVE)
		return pat.drive * 4;
	if (field == GF_BUSSRR)
		return pat.srr * 4;
	if (field == GF_COND)
		return byte_to_value(GF_COND, cond_value());
#if GROOVE_FX
	if (FX_IS(field))
		return fx_get(field);
#endif
	if (field < 0 || field > GF_FENV)
		return 0;
	return byte_to_value(field, field_byte(field));
}

void groove_set_field(int field, int value) {
#if GROOVE_FX
	if (FX_IS(field)) {
		fx_setv(field, value);
		return;
	}
#endif
	if (field == GF_COND) { // condition of the locked step: encoder = one step per detent, slider = absolute
		if (lock_step == NO_STEP) {
			ShowMessage(F_16_BOLD, "Condition", "hold a step first");
			return;
		}
		int c = cond_value(), curv = byte_to_value(GF_COND, c), nc;
		if (millis() - lastencodertime < 50 && value != curv)
			nc = c + (value > curv ? 1 : -1);
		else
			nc = (maxi(value, 0) * COND_LAST) / 1024;
		nc = clampi(nc, 0, COND_LAST - 1);
		if (nc == c)
			return;
		set_lock(GF_COND, nc);
		mark_dirty();
		return;
	}
	if (field >= GF_VOLUME) { // whole drum bus
		u8* b = (field == GF_VOLUME) ? &pat.volume : (field == GF_DJFILTER) ? &pat.djfilter : (field == GF_COMP) ? &pat.comp : (field == GF_BUSDRIVE) ? &pat.drive : &pat.srr;
		if (field > GF_COMP && field != GF_BUSDRIVE && field != GF_BUSSRR) return;
		*b = clampi(qdiv(value, *b * 4, 4), 0, 255);
		mark_dirty();
		return;
	}
	if (field < 0 || field > GF_FENV)
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
	case GF_TUNE:  b = (u8)(s8)clampi(qdiv(value, curv, 8), -120, 120); break;
	case GF_COLOR: b = clampi(qdiv(value + 1024, curv + 1024, 8), 0, 255); break;
	default:       b = clampi(qdiv(value, curv, 4), 0, 255); break;
	}
	if (b == cur)
		return;
	if (lock_step != NO_STEP) // step lock mode: the value goes to the step
		set_lock(field, b);
	else {
		if (field == GF_MODEL) {
			gate[lane_sel] = 0;
			if (model_edit_lane != lane_sel || millis() - model_edit_ms > ACID_SETTLE_MS)
				acid_prev_model = cur; // the model that had settled before this scroll
			model_edit_lane = lane_sel;
			model_edit_ms = millis();
			if (b == DM_ACID || b == DM_BASS) { // starting sound / new notes once the choice has settled (see groove_tick)
				acid_gen = lane_sel + 1;
				acid_gen_ms = millis();
			}
			else if (acid_gen == lane_sel + 1)
				acid_gen = 0;
		}
		((u8*)&pat.lane[lane_sel].snd)[field] = b;
	}
	mark_dirty();
}

// modulation amount of a field of the selected lane (param units: +-1016)
// drum bus fields: one value for the whole pattern, modulated by knobs / CV / LFOs only
static int bus_field(int field) {
	return field == GF_DJFILTER || field == GF_VOLUME || field == GF_COMP || field == GF_BUSDRIVE || field == GF_BUSSRR;
}

int groove_get_mod(int field, int source) {
	if ((field <= GF_MODEL || field > GF_FENV) && !bus_field(field))
		return 0;
	int i = find_mod(bus_field(field) ? LANE_ALL : lane_sel, field, source);
	return i < 0 ? 0 : pat.mod[i].amount * 8;
}

void groove_set_mod(int field, int source, int value) {
	if ((field <= GF_MODEL || field > GF_FENV) && !bus_field(field))
		return;
	if (source < MOD_ENV || source > MOD_RND)
		return;
	if (bus_field(field) && (source < MOD_A || source > MOD_Y)) {
		ShowMessage(F_16_BOLD, "all drums", "knobs, CV, LFOs only");
		return;
	}
	int lane = bus_field(field) ? LANE_ALL : lane_sel;
	int i = find_mod(lane, field, source);
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
			pat.mod[i].lane = lane;
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
	[GF_SRR] = "Sample Rate", [GF_FENV] = "Filter Env", [GF_VOLUME] = "Drum Vol", [GF_DJFILTER] = "Drums Filter", [GF_COMP] = "Drum Comp",
	[GF_COND] = "Condition",
	[GF_ROLL] = "Roll", [GF_RATCHET] = "Ratchet", [GF_REVERSE] = "Reverse", [GF_PITCHFALL] = "Pitch Fall", [GF_GATE] = "Gate", [GF_CRUSH] = "Crush",
	[GF_BUSDRIVE] = "Drum Drive", [GF_BUSSRR] = "Drum Rate",
};

const char* groove_field_name(int field) {
	return (field >= 0 && field <= GF_BUSSRR) ? field_names[field] : "";
}

const char* groove_field_page(int field) {
	if (FX_IS(field))
		return "hold pad = on";
	return GROOVE_FIELD_IS_LANE(field) ? groove_lane_label() : "all drums";
}

#if GROOVE_FX
static int fx_is_choice(int field) { // 4-way settings, as opposed to amounts
	return field == GF_ROLL || field == GF_RATCHET || field == GF_GATE;
}

static int fx_get(int field) {
	u8 v = fx_set[field - GF_ROLL];
	if (field == GF_REVERSE) return 0;
	return fx_is_choice(field) ? ((v * 2 + 1) * 1024) / 8 : v * 4;
}

static void fx_setv(int field, int value) {
	u8* v = &fx_set[field - GF_ROLL];
	if (field == GF_REVERSE)
		return;
	if (fx_is_choice(field)) {
		int cur = fx_get(field), n;
		if (millis() - lastencodertime < 50 && value != cur)
			n = *v + (value > cur ? 1 : -1);
		else
			n = (maxi(value, 0) * 4) / 1024;
		*v = clampi(n, 0, 3);
	}
	else
		*v = clampi(qdiv(maxi(value, 0), *v * 4, 4), 0, 255);
}

static const char* fx_str(int field, char* buf) {
	u8 v = fx_set[field - GF_ROLL];
	switch (field) {
	case GF_REVERSE: return "hold";
	case GF_ROLL: return fx_div_names[v & 3];
	case GF_RATCHET: return fx_ratchet_names[v & 3];
	case GF_GATE: return fx_gate_names[v & 3];
	}
	sprintf(buf, "%d", (v * 100 + 127) / 255);
	return buf;
}
#endif

const char* groove_lane_label(void) {
	static char label[16];
	if (lock_step != NO_STEP)
		sprintf(label, "Lock step %d", (lock_step + 1) & 63);
	else
		sprintf(label, "Drum %d", (lane_sel + 1) & 7);
	return label;
}

// value text for the OLED (buf has room for 32 chars)
const char* groove_field_str(int field, char* buf) {
	if (field == GF_VOLUME) {
		sprintf(buf, "%d", (pat.volume * 100 + 127) / 255);
		return buf;
	}
	if (field == GF_COND)
		return lock_step != NO_STEP ? cond_names[cond_value()] : "-";
#if GROOVE_FX
	if (FX_IS(field))
		return fx_str(field, buf);
#endif
	u8 b = (field == GF_DJFILTER) ? pat.djfilter : (field == GF_COMP) ? pat.comp : (field == GF_BUSDRIVE) ? pat.drive : (field == GF_BUSSRR) ? pat.srr : field_byte(field);
	switch (field) {
	case GF_FENV:
		if (b == DRUM_FILTER_OFF)
			return "Off";
		sprintf(buf, "%+d", ((b - DRUM_FILTER_OFF) * 100) / 127);
		return buf;
	case GF_COMP:
	case GF_BUSDRIVE:
	case GF_BUSSRR:
	case GF_SRR:
		if (!b)
			return "Off";
		break;
	case GF_MODEL:
		return drum_model_names[b < DM_LAST ? b : 0];
	case GF_TUNE: { // semitones, one decimal
		int st10 = (s8)b * 2;
		sprintf(buf, "%c%d.%d", st10 < 0 ? '-' : '+', abs(st10) / 10, abs(st10) % 10);
		return buf; }
	case GF_FILTER:
		if (pat.lane[lock_step != NO_STEP ? lock_lane : lane_sel].snd.model == DM_ACID)
			break; // acid: plain cutoff 0..100
		// fall through
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
	int ci = find_lock(l, lane_pos[l], GF_COND);
	fired &= ~bit;
	if (!(s & STEP_TRIG) || (pat.lane[l].flags & LANE_MUTE) || (ci >= 0 && !cond_ok(pat.lock[ci].value, l))) {
		slide_from &= ~bit; // a rest ends a slide
		return;
	}
	int vel = (s >> STEP_VEL_SHIFT) & STEP_VEL_MASK;
	if (!vel)
		vel = VEL_DEFAULT;
	int m = pat.lane[l].snd.model;
	if (m == DM_SAMPLE) {
		hit_vel[l] = vel;
		gate[l] = gate_len;
	}
	else if (m == DM_ACID) { // notes: a slide step holds into the next step, which then glides to its note
		int gate_blocks = (s & STEP_SLIDE) ? step_ticks_now * 5 / 4 : step_ticks_now * 11 / 20;
		trigger_lane(l, vel, lane_pos[l], maxi(gate_blocks, 2), (slide_from & bit) != 0);
		if (s & STEP_SLIDE) slide_from |= bit; else slide_from &= ~bit;
		fired |= bit;
	}
	else {
		trigger_lane(l, vel, lane_pos[l], 0, 0);
		fired |= bit;
	}
	flash[l] = 255;
}

static void restart_lanes(void) {
	slide_from = 0;
	for (int l = 0; l < GROOVE_LANES; ++l) {
		lane_pos[l] = LANE_START(&pat.lane[l]);
		lane_loop[l] = 0;
		fire(l);
	}
}

void groove_reset(void) {
	running = 0; // next tick restarts all lanes from their first step
	bar_step = 0;
}

// Transport. The drums have their own play/stop: the Play button while the
// drum grid is shown. In the Plinky view the button works as usual. Both
// sequencers count the same bar clock (bar_step), anchored where the first of
// them started; the second one to start waits for the next bar so they line up.
enum { PEND_NONE, PEND_DRUMS, PEND_PLINKY };
static inline u8 gamma8(int i);

int groove_play_button(int plinky_playing) {
	if (GROOVE_GRID) {
		if (drums_pending == PEND_DRUMS)
			drums_pending = PEND_NONE; // cancel the waiting start
		else if (drums_play) {
			drums_play = 0; // stop: hits already sounding ring out
			memset(gate, 0, sizeof(gate));
		}
		else
			drums_pending = PEND_DRUMS; // starts now, or on Plinky's next bar
		return 1;
	}
	if (drums_pending == PEND_PLINKY) {
		drums_pending = PEND_NONE;
		return 1;
	}
	if (!plinky_playing && drums_play) {
		drums_pending = PEND_PLINKY; // Plinky starts on the drums' next bar
		return 1;
	}
	return 0;
}

void groove_transport(int on) {
	drums_pending = PEND_NONE;
	if (on)
		drums_play = 1;
	else {
		drums_play = 0;
		memset(gate, 0, sizeof(gate));
	}
}

int groove_play_led(int plinky_playing, int glow) {
	if (drums_pending == (GROOVE_GRID ? PEND_DRUMS : PEND_PLINKY))
		return ((millis() >> 7) & 1) ? 255 : 0; // waiting for the bar
	return (GROOVE_GRID ? drums_play : plinky_playing) ? gamma8(glow) : 0;
}

int groove_tick(int clock16, int playing, int step_ticks, int recording) {
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
	if (acid_gen && millis() - acid_gen_ms > ACID_SETTLE_MS) { // the model choice rested on Acid: new notes
		int l = acid_gen - 1;
		acid_gen = 0;
		if (pat.lane[l].snd.model == DM_ACID || pat.lane[l].snd.model == DM_BASS)
			note_model_chosen(l);
	}

	// shared bar clock, and the starts waiting for it
	if (clock16 && (playing || drums_play))
		bar_step++;
	int bar = clock16 && (bar_step & 15) == 0;
	int start_plinky = 0;
	if (drums_pending == PEND_DRUMS && (!playing || bar)) {
		drums_pending = PEND_NONE;
		drums_play = 1;
	}
	else if (drums_pending == PEND_PLINKY && (!drums_play || bar)) {
		drums_pending = PEND_NONE;
		start_plinky = 1;
	}
	if (!playing && !drums_play)
		bar_step = 0; // the next start anchors the bar clock

	int run = drums_play;
	if (run && !running) {
		if (pending_slot != NO_SLOT && can_load()) {
			load_slot(pending_slot);
			pending_slot = NO_SLOT;
		}
		ticks_in_step = 0;
		restart_lanes();
	}
	else if (run && clock16) {
		ticks_in_step = 0;
		if (bar && pending_slot != NO_SLOT && can_load()) {
			load_slot(pending_slot); // pattern change on the bar
			pending_slot = NO_SLOT;
			restart_lanes();
		}
		else {
#if GROOVE_FX
			int roll = fx_on & FX_BIT(GF_ROLL);
#else
			const int roll = 0;
#endif
			for (int l = 0; l < GROOVE_LANES; ++l) {
#if GROOVE_FX
				if (roll)
					lane_pos[l] = roll_pos(l, roll_count % roll_len[fx_set[FX_ROLL] & 3]);
				else
#endif
				{
					lane_pos[l] = next_pos(l);
					if (lane_pos[l] == LANE_START(&pat.lane[l]))
						lane_loop[l]++;
				}
				fire(l);
			}
#if GROOVE_FX
			if (roll)
				roll_count++;
#endif
			(void)roll;
		}
#if GROOVE_FX
		ratchet_n = 0;
		gate_phase = (fx_set[FX_GATE] == 0 && (bar_step & 1)) ? step_ticks_now : 0; // 1/8: period = 2 steps
		if (fx_on & FX_BIT(GF_PITCHFALL)) // falls up to 24 semitones over a bar at 100 %
			fx_pitch = maxf(fx_pitch - fx_set[FX_PITCHFALL] * (24.f / 255.f / 16.f), -24.f);
#endif
	}
#if GROOVE_FX
	else if (run)
		gate_phase++;
	if (run && (fx_on & FX_BIT(GF_RATCHET)) && fired) { // retrigger the step's hits, velocity rolling off
		int r = ratchet_rate[fx_set[FX_RATCHET] & 3];
		int sub = (ticks_in_step * r) / maxi(step_ticks_now, 1);
		if (sub > ratchet_n && sub < r) {
			ratchet_n = sub;
			for (int l = 0; l < GROOVE_LANES; ++l)
				if (fired & (1 << l)) {
					trigger_lane(l, maxi(1, lane_vel[l] - 1), lane_pos[l], 0, 0);
					flash[l] = 255;
				}
		}
	}
#endif
	else if (!run && pending_slot != NO_SLOT && can_load()) {
		load_slot(pending_slot); // stopped: change right away
		pending_slot = NO_SLOT;
	}
	running = run;
	return start_plinky;
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

static int soft_sum(int x) {
	const int t = 26214; // 0.8 of full scale
	int a = x < 0 ? -x : x;
	if (a <= t)
		return x;
	float z = (a - t) * (1.f / (32767 - t));
	int y = t + (int)((32767 - t) * z / (1.f + z));
	return x < 0 ? -y : y;
}

unsigned long groove_mix(unsigned long lr, int drum) {
	int l = (s16)(lr & 0xffff) + drum, r = (s16)(lr >> 16) + drum;
	return (unsigned long)(u16)soft_sum(l) | ((unsigned long)(u16)soft_sum(r) << 16);
}

void groove_audio(void) {
	// hits modulated by knobs / CV / LFOs follow them while they ring
	for (int l = 0; l < GROOVE_LANES; ++l)
		if (eff_live & (1 << l))
			compute_eff(l);
	// drum bus settings + knob / CV / LFO modulation
	int dj = pat.djfilter, vol = pat.volume, comp = pat.comp, drive = pat.drive, srr = pat.srr;
	for (int i = 0; i < pat.nmods; ++i) {
		GrooveMod* m = &pat.mod[i];
		if (m->lane != LANE_ALL || m->source < MOD_A || m->source > MOD_Y)
			continue;
		int d = (int)(mod_value(m, 0) * m->amount * (255.f / 127.f));
		switch (m->field) {
		case GF_DJFILTER: dj += d; break;
		case GF_VOLUME:   vol += d; break;
		case GF_COMP:     comp += d; break;
		case GF_BUSDRIVE: drive += d; break;
		case GF_BUSSRR:   srr += d; break;
		}
	}
	dj_eff = clampi(dj, 0, 255);
	drums_render(clampi(vol, 0, 255), dj_eff, clampi(comp, 0, 255), clampi(drive, 0, 255), clampi(srr, 0, 255));
#if GROOVE_FX
	if (fx_on & FX_BIT(GF_GATE)) { // trance gate: open for the first half of each period, 2 ms ramps
		int period = maxi((step_ticks_now * 4) >> (fx_set[FX_GATE] + 1), 2);
		int g0 = (gate_phase % period) * 2 < period, g1 = ((gate_phase + 1) % period) * 2 < period;
		if (!(g0 && g1)) {
			float m = g0, dm = (g1 - g0) * (1.f / 64.f);
			for (int i = 0; i < 64; ++i, m += dm)
				drum_bus_dry[i] = (short)(drum_bus_dry[i] * m);
			m = g0; dm *= 2.f;
			for (int i = 0; i < 32; ++i, m += dm) {
				drum_bus_dl[i] = (short)(drum_bus_dl[i] * m);
				drum_bus_rv[i] = (short)(drum_bus_rv[i] * m);
			}
		}
	}
	if (fx_on & FX_BIT(GF_CRUSH)) { // bit depth 16 -> 4 bits and sample rate 1 -> 1/16
		int a = fx_set[FX_CRUSH];
		int sh = (a * 12) / 255, n = 1 + (a * 15) / 255;
		short hold = 0;
		for (int i = 0; i < 64; ++i) {
			if (i % n == 0) hold = (short)((drum_bus_dry[i] >> sh) << sh);
			drum_bus_dry[i] = hold;
		}
		for (int i = 0; i < 32; ++i) {
			drum_bus_dl[i] = (short)((drum_bus_dl[i] >> sh) << sh);
			drum_bus_rv[i] = (short)((drum_bus_rv[i] >> sh) << sh);
		}
	}
#endif
}

// ---------------------------------------------------------------------------
// Drawing (main loop)

static inline u8 gamma8(int i) {
	i = clampi(i, 0, 255);
	return (i * i) >> 8;
}

void groove_draw_leds(int mode) {
	GrooveLane* sel = &pat.lane[lane_sel];
	int playhead = running ? lane_pos[lane_sel] : -1;
	int start = LANE_START(sel);
	if (mode) { // Prev / Next held: the 64 steps of the selected lane, loop start / end bright
		int end = (start + sel->length - 1) & 63;
		for (int y = 0; y < 8; ++y)
			for (int fi = 0; fi < 8; ++fi) {
				int idx = y * 8 + fi;
				int k = ((idx - start) & 63) < sel->length ? 40 : 0;
				if (pat.step[lane_sel][idx] & STEP_TRIG) k = 110;
				if (idx == playhead) k = 255;
				if (idx == (mode == 1 ? start : end)) k = ((millis() >> 7) & 1) ? 255 : 120;
				led_ram[fi][y] = gamma8(k);
			}
		return;
	}
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
			int idx = view_page() * GROOVE_PAGE + (y - ROW_STEPS) * 8 + fi;
			u8 s = pat.step[lane_sel][idx];
			int inloop = ((idx - start) & 63) < sel->length;
			k = inloop ? 14 : 0;
			if (s & STEP_TRIG) {
				int vel = (s >> STEP_VEL_SHIFT) & STEP_VEL_MASK;
				k = 80 + (vel ? vel : VEL_DEFAULT) * 25;
				if (step_locks(lane_sel, idx)) // has step locks: dim flicker
					k = ((millis() >> 6) & 3) ? k : 60;
			}
			if (idx == playhead)
				k = 255;
			if (lock_step == idx && lock_lane == lane_sel) // step lock mode: slow blink
				k = ((millis() >> 9) & 1) ? 255 : 0;
			led_ram[fi][y] = gamma8(k);
		}
	}
}

// the stage: a dancing cat (42 x 24, six poses) and a boombox, right of the lane
// name (x 66..127, y 0..23). The cat takes one pose per 16th (stand, nod, tail
// swish, nod, paw, nod, swish, nod), jumps while a kick sounds and winces on other
// loud hits; the boombox speakers pump with the kicks and its reels turn with the
// steps. Everything stands still while the drums are stopped or no unmuted lane
// has steps. Drawn with putpixel only.
#define STAGE_X 66
enum { CAT_STAND, CAT_NOD, CAT_SWISH, CAT_PAW, CAT_JUMP, CAT_WINCE };
static const uint64_t cat_pose[6][24] = {
	{ // stand
		0x0000000000000000ull, 0x0000000000000000ull, 0x2010000000800000ull, 0x3030000000400000ull, 0x2850000000800000ull, 0x3ff0000000800000ull, 0x2010000001000000ull, 0x4008000002000000ull, 0x4cc8000002000000ull, 0x4ccfffffe4000000ull, 0xc30c000018000000ull, 0x4488000010000000ull, 0x2010000010000000ull, 0x1fe0000020000000ull, 0x10000000a0000000ull, 0x10000000a0000000ull, 0x10000000a0000000ull, 0x285fff8fa0000000ull, 0x28500050a0000000ull, 0x28500050a0000000ull, 0x28500050a0000000ull, 0x28500050a0000000ull, 0x28500050a0000000ull, 0x3c780078f0000000ull },
	{ // bob
		0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000800000ull, 0x0000000000400000ull, 0x2010000000800000ull, 0x3030000000800000ull, 0x2850000001000000ull, 0x3ff0000002000000ull, 0x2010000002000000ull, 0x400fffffe4000000ull, 0x4cc8000018000000ull, 0x4cc8000010000000ull, 0xc30c000010000000ull, 0x4488000020000000ull, 0x30100000a0000000ull, 0x1fe00000a0000000ull, 0x10000000a0000000ull, 0x285fff8fa0000000ull, 0x28500050a0000000ull, 0x28500050a0000000ull, 0x28500050a0000000ull, 0x28500050a0000000ull, 0x28500050a0000000ull, 0x3c780078f0000000ull },
	{ // swish
		0x0000000000000000ull, 0x0000000000000000ull, 0x2010000000000000ull, 0x3030000000000000ull, 0x28500000c0000000ull, 0x3ff0000120000000ull, 0x2010000010000000ull, 0x4008000008000000ull, 0x4cc8000004000000ull, 0x4ccfffffe4000000ull, 0xc30c000018000000ull, 0x4488000010000000ull, 0x2010000010000000ull, 0x1fe0000020000000ull, 0x10000000a0000000ull, 0x10000000a0000000ull, 0x10000000a0000000ull, 0x285fff8fa0000000ull, 0x28500050a0000000ull, 0x28500050a0000000ull, 0x28500050a0000000ull, 0x28500050a0000000ull, 0x28500050a0000000ull, 0x3c780078f0000000ull },
	{ // paw
		0x0000000000000000ull, 0x0000000000000000ull, 0x2010000000800000ull, 0x3030000000400000ull, 0x2850000000800000ull, 0x3ff0000000800000ull, 0x2010000001000000ull, 0x4008000002000000ull, 0x4cc8000002000000ull, 0x4ccfffffe4000000ull, 0xc30c000018000000ull, 0x4488000010000000ull, 0x2010000010000000ull, 0x1fe0000020000000ull, 0x10000000a0000000ull, 0x10000000a0000000ull, 0x10000000a0000000ull, 0x505fff8fa0000000ull, 0x50500050a0000000ull, 0x78500050a0000000ull, 0x00500050a0000000ull, 0x00500050a0000000ull, 0x00500050a0000000ull, 0x00780078f0000000ull },
	{ // jump
		0x2010000000000000ull, 0x3030000000000000ull, 0x2850000000000000ull, 0x3ff0000000000000ull, 0x2010000000000000ull, 0x4008000000000000ull, 0x4cc8000000400000ull, 0x4ccfffffe0800000ull, 0xc30c000013000000ull, 0x448800001c000000ull, 0x2010000010000000ull, 0x1fe0000020000000ull, 0x10000000a0000000ull, 0x10000000a0000000ull, 0x10000000a0000000ull, 0x285fff8fa0000000ull, 0x28500050a0000000ull, 0x28500050a0000000ull, 0x3c780078f0000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull },
	{ // hit
		0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x4008000000000000ull, 0xfffc000000000000ull, 0x2010000000000000ull, 0x4008000000000000ull, 0x4848000000400000ull, 0x448fffffe0800000ull, 0xcb4c000013000000ull, 0x430800001c000000ull, 0x2790000010000000ull, 0x1fe0000020000000ull, 0x10000000a0000000ull, 0x10000000a0000000ull, 0x10000000a0000000ull, 0x285fff8fa0000000ull, 0x28500050a0000000ull, 0x28500050a0000000ull, 0x28500050a0000000ull, 0x28500050a0000000ull, 0x28500050a0000000ull, 0x3c780078f0000000ull },
};

static void px_hl(int x1, int x2, int y) { for (int x = x1; x <= x2; ++x) putpixel(x, y, 1); }
static void px_vl(int x, int y1, int y2) { for (int y = y1; y <= y2; ++y) putpixel(x, y, 1); }
static void px_ring(int cx, int cy, int r) { // midpoint circle
	int x = r, y = 0, e = 1 - r;
	while (x >= y) {
		putpixel(cx + x, cy + y, 1); putpixel(cx - x, cy + y, 1); putpixel(cx + x, cy - y, 1); putpixel(cx - x, cy - y, 1);
		putpixel(cx + y, cy + x, 1); putpixel(cx - y, cy + x, 1); putpixel(cx + y, cy - x, 1); putpixel(cx - y, cy - x, 1);
		++y;
		if (e < 0) e += 2 * y + 1; else { --x; e += 2 * (y - x) + 1; }
	}
}

static void draw_stage(int boombox) {
	int pose = CAT_STAND, kick = 0, moving = 0;
	for (int l = 0; l < GROOVE_LANES && !moving; ++l)
		if (!(pat.lane[l].flags & LANE_MUTE))
			for (int i = 0; i < GROOVE_STEPS; ++i)
				if (pat.step[l][i] & STEP_TRIG) { moving = running; break; }
	if (moving) {
		static const u8 beat[8] = { CAT_STAND, CAT_NOD, CAT_SWISH, CAT_NOD, CAT_PAW, CAT_NOD, CAT_SWISH, CAT_NOD };
		pose = beat[bar_step & 7];
		for (int l = 0; l < GROOVE_LANES; ++l) {
			int m = pat.lane[l].snd.model;
			if ((m == DM_KICK || m == DM_KICK2) && flash[l] > 60) { pose = CAT_JUMP; kick = 1; break; }
			if (flash[l] > 120 && m != DM_CHAT && m != DM_OHAT) pose = CAT_WINCE;
		}
	}
	for (int y = 0; y < 24; ++y) {
		uint64_t row = cat_pose[pose][y];
		for (int x = 0; x < 42; ++x)
			if (row & (0x8000000000000000ull >> x))
				putpixel(STAGE_X + x, y, 1);
	}
	if (!boombox)
		return;
	const int bx = STAGE_X + 43, by = 10;
	px_hl(bx, bx + 18, by); px_hl(bx, bx + 18, by + 12); px_vl(bx, by, by + 12); px_vl(bx + 18, by, by + 12); // box
	px_hl(bx + 5, bx + 13, by - 3); px_vl(bx + 5, by - 3, by); px_vl(bx + 13, by - 3, by);                   // handle
	for (int s = 0; s < 2; ++s) {                                                                             // speakers
		int sx = bx + 4 + s * 10;
		px_ring(sx, by + 7, 3 + kick);
		putpixel(sx, by + 7, 1);
	}
	px_hl(bx + 7, bx + 11, by + 2); px_hl(bx + 7, bx + 11, by + 4); px_vl(bx + 7, by + 2, by + 4); px_vl(bx + 11, by + 2, by + 4); // tape
	int r = moving && (bar_step & 1);
	putpixel(bx + 8 + r, by + 3, 1); putpixel(bx + 10 - r, by + 3, 1);                                         // reels
}

// start-up screen: instead of the Plinky logo, the cat gallops in from the right,
// across the screen and out on the left (gallop cycle: stretch, mid, gather, mid;
// the art faces left). The firmware name sits where Plinky's version always was
// (x 32, y 20, same font and style); the cat runs in front of it with a black outline.
static const uint64_t cat_run[3][24] = {
	{ // stretch
		0x0000000000000000ull, 0x0000000000000000ull, 0x2010000000000000ull, 0x3030000000000000ull, 0x2850000000000000ull, 0x3ff0000000000000ull, 0x2010000000000000ull, 0x4008000000000000ull, 0x4cc8000000400000ull, 0x4ccfffffe0800000ull, 0xc30c000013000000ull, 0x448800001c000000ull, 0x2010000010000000ull, 0x1fe0000020000000ull, 0x10000000a0000000ull, 0x10000000a0000000ull, 0x1000000050000000ull, 0x145fff8f28000000ull, 0x2850005028000000ull, 0x28a0002828000000ull, 0x50a0001414000000ull, 0x514000140a000000ull, 0xa140000a0a000000ull, 0xf1e0000f0f000000ull },
	{ // mid
		0x0000000000000000ull, 0x0000000000000000ull, 0x2010000000000000ull, 0x3030000000000000ull, 0x2850000000000000ull, 0x3ff0000000000000ull, 0x2010000000000000ull, 0x4008000000000000ull, 0x4cc8000000400000ull, 0x4ccfffffe0800000ull, 0xc30c000013000000ull, 0x448800001c000000ull, 0x2010000010000000ull, 0x1fe0000020000000ull, 0x10000000a0000000ull, 0x10000000a0000000ull, 0x10000000a0000000ull, 0x145fff8fa0000000ull, 0x14500050a0000000ull, 0x1450005140000000ull, 0x0aa0005140000000ull, 0x0aa0002940000000ull, 0x0aa0002940000000ull, 0x0ff0003de0000000ull },
	{ // gather
		0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x2010000000000000ull, 0x3030000000000000ull, 0x2850000000000000ull, 0x3ff0000000000000ull, 0x2010000000000000ull, 0x4008000000000000ull, 0x4cc8000000400000ull, 0x4ccfffffe0800000ull, 0xc30c000013000000ull, 0x448800001c000000ull, 0x2010000010000000ull, 0x1fe0000020000000ull, 0x10000000a0000000ull, 0x1000000140000000ull, 0x1000000280000000ull, 0x145fff8f80000000ull, 0x0a28005500000000ull, 0x0294014a00000000ull, 0x014a051400000000ull, 0x01ef079e00000000ull, 0x0000000000000000ull },
};
#define BOOT_RUN_MS 1600
int groove_boot_frame(void) {
	static u32 t0;
	static u8 started;
	if (!started) {
		t0 = millis();
		started = 1;
	}
	u32 t = millis() - t0;
	int x0 = 128 - (int)((t * (128 + 42)) / BOOT_RUN_MS);
	if (x0 <= -42)
		return 0;
	static const u8 cycle[4] = { 0, 1, 2, 1 };
	int f = cycle[(t / 70) & 3];
	clear();
	u8 oldcol = textcol;
	textcol = 3;
	drawstr(32, 20, F_12, VERSION2); // where Plinky draws its version
	textcol = oldcol;
	for (int pass = 0; pass < 2; ++pass) // pass 0: black outline around the cat, pass 1: the cat
		for (int y = 0; y < 24; ++y) {
			uint64_t row = cat_run[f][y];
			for (int x = 0; x < 42; ++x) {
				if (!(row & (0x8000000000000000ull >> x)))
					continue;
				int sx = x0 + x;
				if (pass == 1) {
					if (sx >= 0 && sx < 128)
						putpixel(sx, y, 1);
					continue;
				}
				for (int dy = -1; dy <= 1; ++dy)
					for (int dx = -1; dx <= 1; ++dx)
						if (sx + dx >= 0 && sx + dx < 128 && y + dy >= 0 && y + dy < 32)
							putpixel(sx + dx, y + dy, 0);
			}
		}
	return 1;
}

void groove_draw_oled(int mode) {
	GrooveLane* sel = &pat.lane[lane_sel];
	if (mode) { // Prev / Next held: the lane's loop
		int start = LANE_START(sel);
		if (mode == 1)
			fdrawstr(0, 0, F_16_BOLD, "Lane %d start %d", lane_sel + 1, start + 1);
		else
			fdrawstr(0, 0, F_16_BOLD, "Lane %d end %d", lane_sel + 1, ((start + sel->length - 1) & 63) + 1);
		fdrawstr(0, 18, F_8, "length %d    hold pad = all lanes", sel->length);
		return;
	}
	if (lock_step != NO_STEP) { // step lock mode
		int n = step_locks(lock_lane, lock_step);
		int ci = find_lock(lock_lane, lock_step, GF_COND);
		const char* cn = cond_names[ci >= 0 ? clampi(pat.lock[ci].value, 0, COND_LAST - 1) : 0];
		fdrawstr(0, 0, F_16_BOLD, "Lock step %d", lock_step + 1);
		fdrawstr(0, 18, F_8, "%d locked  cond %s  click=clear", n, cn);
		return;
	}
	fdrawstr(0, 0, F_16_BOLD, "%d %s", lane_sel + 1, drum_model_names[sel->snd.model]);
	// one thin activity meter per lane (16 px wide, up to 5 px tall); dotted = muted, line above = selected
	for (int l = 0; l < GROOVE_LANES; ++l) {
		int x0 = l * 16 + 1, x1 = x0 + 13;
		if (pat.lane[l].flags & LANE_MUTE)
			halfrectangle(x0, 27, x1, 32);
		else
			fillrectangle(x0, 31 - (flash[l] * 4) / 255, x1, 32);
		if (l == lane_sel)
			hline(x0, 25, x1, 1);
	}
	// a status (pattern change, waiting for the bar, recording) takes the boombox's corner
	char st[16];
	st[0] = 0;
#if GROOVE_FX
	if (fx_on) {
		int f = __builtin_ctz(fx_on);
		sprintf(st, "%s", fx_short[f < FX_COUNT ? f : 0]);
	}
	else
#endif
	if (pending_slot != NO_SLOT)
		sprintf(st, "P%d>%d", cur_slot + 1, pending_slot + 1);
	else if (drums_pending == PEND_DRUMS)
		sprintf(st, ">bar");
	else if (recording_now)
		sprintf(st, "\x9c");
	draw_stage(!st[0]);
	if (st[0] == '\x9c' && !st[1]) // recording: the record icon (15 x 15 lit) in the middle of the boombox's place
		drawstr(STAGE_X + 52 - 7, 8, F_12, st);
	else if (st[0]) { // on a black box, over the cat's tail
		int w = strwidth(F_12, st) + 2;
		for (int y = 0; y < 16; ++y)
			for (int x = 128 - w; x < 128; ++x)
				putpixel(x, y, 0);
		drawstr(-128, 2, F_12, st);
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
