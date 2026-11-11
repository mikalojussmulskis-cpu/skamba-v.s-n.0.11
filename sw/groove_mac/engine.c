// engine.c - Groove Drums for macOS. Compiles the firmware's groove.c (sequencer,
// patterns, step locks, conditions, Acid lines) and drums.c (drum engine)
// unchanged, and supplies what the Plinky would: the 16th-note clock, the flash
// store (a file), the scale, the on-screen message, and a simple delay / reverb
// for the drum sends (on the Plinky those go into Plinky's own delay and reverb).
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "core.h"
#include "gfx.h"
#include "engine.h"

// --- what plinky.c provides on the hardware --------------------------------
u8 led_ram[9][8]; int lastencodertime; int mod_cur[8]; u8 textcol;
void clearlatch(void) {}
// the 128 x 32 screen: pixels, rectangles and lines (text is not drawn here)
static u8 oled[32][128];
void putpixel(int x, int y, int c) { if (x >= 0 && x < 128 && y >= 0 && y < 32) oled[y][x] = c ? 1 : 0; }
void fillrectangle(int x1, int y1, int x2, int y2) { for (int y = y1; y < y2; ++y) for (int x = x1; x < x2; ++x) putpixel(x, y, 1); }
void halfrectangle(int x1, int y1, int x2, int y2) { for (int y = y1; y < y2; ++y) for (int x = x1; x < x2; ++x) if ((x + y) & 1) putpixel(x, y, 1); }
void hline(int x1, int y, int x2, int c) { for (int x = x1; x < x2; ++x) putpixel(x, y, c); }
int fdrawstr(int x, int y, EFont f, const char* fmt, ...) { (void)x; (void)y; (void)f; (void)fmt; return 0; }
int drawstr(int x, int y, EFont f, const char* b) { (void)x; (void)y; (void)f; (void)b; return 0; }

uint32_t plat_ms(void); void plat_lock(void); void plat_unlock(void); // platform.c
static u32 eng_millis(void) { return plat_ms(); }

static char msg_main[48], msg_sub[48];
static u32 msg_ms;
void ShowMessage(EFont f, const char* m, const char* s) {
	(void)f;
	snprintf(msg_main, sizeof(msg_main), "%s", m ? m : "");
	snprintf(msg_sub, sizeof(msg_sub), "%s", s ? s : "");
	msg_ms = eng_millis();
}

// flash store: 24 pages of 2 KB, kept in a file
#define PAGE 2048
static uint8_t store[24][PAGE];
static uint8_t stored[24];
static char store_path[1024];
static void store_write(void) {
	if (!store_path[0]) return;
	FILE* f = fopen(store_path, "wb");
	if (!f) return;
	fwrite("GRVM", 1, 4, f);
	fwrite(stored, 1, sizeof(stored), f);
	fwrite(store, 1, sizeof(store), f);
	fclose(f);
}
static void store_read(void) {
	FILE* f = fopen(store_path, "rb");
	if (!f) return;
	char magic[4];
	if (fread(magic, 1, 4, f) == 4 && !memcmp(magic, "GRVM", 4) &&
		fread(stored, 1, sizeof(stored), f) == sizeof(stored))
		if (fread(store, 1, sizeof(store), f) != sizeof(store))
			memset(stored, 0, sizeof(stored));
	fclose(f);
}
const void* GetSavedGroovePattern(int slot) { return (slot >= 0 && slot < 24 && stored[slot]) ? store[slot] : 0; }
void SaveGroovePattern(int slot, const void* d, int n) {
	if (slot < 0 || slot >= 24 || n > PAGE) return;
	memset(store[slot], 0xff, PAGE);
	memcpy(store[slot], d, n);
	stored[slot] = 1;
	store_write();
}
int GrooveFlashBusy(void) { return 0; }

// Plinky's scale: major here
static const int major[8] = { 0, 2, 4, 5, 7, 9, 11, 12 };
int GrooveScaleQuantize(int st) {
	int o = st >= 0 ? st / 12 : -((11 - st) / 12), r = st - o * 12, b = 0, bd = 99;
	for (int i = 0; i < 8; ++i)
		if (abs(major[i] - r) < bd) { bd = abs(major[i] - r); b = major[i]; }
	return o * 12 + b;
}
int GrooveScaleDegree(int d) { return major[d & 7]; }

// --- the firmware sources ---------------------------------------------------
#define millis() eng_millis()
#include "groove.c"
#undef millis

// --- engine state -------------------------------------------------------------
#define LOCK() plat_lock()
#define UNLOCK() plat_unlock()
static float bpm = 120.f;
static float clock_acc;

// sends: tempo-synced delay (3/16, like a dotted eighth) and a small reverb
#define DL_MAX 32768
static float dl_buf[DL_MAX];
static int dl_pos;
static const int comb_len[8] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
static const int ap_len[4] = { 556, 441, 341, 225 };
static float comb_buf[8][1700], comb_lp[8];
static int comb_pos[8];
static float ap_buf[4][600];
static int ap_pos[4];

static float reverb(float x, float* out_r) {
	float l = 0.f, r = 0.f;
	for (int c = 0; c < 8; ++c) {
		int len = comb_len[c] * 31250 / 44100;
		float y = comb_buf[c][comb_pos[c]];
		comb_lp[c] = y * 0.7f + comb_lp[c] * 0.3f; // damping
		comb_buf[c][comb_pos[c]] = x + comb_lp[c] * 0.82f;
		if (++comb_pos[c] >= len) comb_pos[c] = 0;
		if (c & 1) r += y; else l += y;
	}
	for (int a = 0; a < 4; ++a) {
		int len = ap_len[a] * 31250 / 44100;
		float* s = (a & 1) ? &r : &l;
		float b = ap_buf[a][ap_pos[a]];
		ap_buf[a][ap_pos[a]] = *s + b * 0.5f;
		*s = b - *s * 0.5f;
		if (++ap_pos[a] >= len) ap_pos[a] = 0;
	}
	*out_r = r * 0.12f;
	return l * 0.12f;
}

static float blockbuf[DRUM_BLOCK * 2];
static int blockpos = DRUM_BLOCK;

static void render_block(void) {
	float bps = (ENG_SR / (float)DRUM_BLOCK) * 60.f / (bpm * 4.f); // blocks per 16th
	clock_acc += 1.f;
	int clock16 = 0;
	if (clock_acc >= bps) {
		clock_acc -= bps;
		clock16 = 1;
	}
	groove_tick(clock16, 0, (int)(bps + 0.5f), 0);
	groove_audio();
	int dl_len = (int)(bps * DRUM_BLOCK * 3.f);
	if (dl_len >= DL_MAX) dl_len = DL_MAX - 1;
	for (int i = 0; i < DRUM_BLOCK; ++i) {
		float dry = drum_bus_dry[i] * (1.f / 32768.f);
		float dsend = drum_bus_dl[i >> 1] * (1.f / 32768.f);
		float rsend = drum_bus_rv[i >> 1] * (1.f / 32768.f);
		int rp = dl_pos - dl_len;
		if (rp < 0) rp += DL_MAX;
		float echo = dl_buf[rp];
		dl_buf[dl_pos] = dsend + echo * 0.4f;
		if (++dl_pos >= DL_MAX) dl_pos = 0;
		float rr, rl = reverb(rsend + echo * 0.2f, &rr);
		float l = dry + echo * 0.6f + rl, r = dry + echo * 0.6f + rr;
		blockbuf[i * 2] = l > 1.f ? 1.f : l < -1.f ? -1.f : l;
		blockbuf[i * 2 + 1] = r > 1.f ? 1.f : r < -1.f ? -1.f : r;
	}
}

void eng_render(float* out, int frames) {
	LOCK();
	for (int i = 0; i < frames; ++i) {
		if (blockpos >= DRUM_BLOCK) {
			render_block();
			blockpos = 0;
		}
		out[i * 2] = blockbuf[blockpos * 2];
		out[i * 2 + 1] = blockbuf[blockpos * 2 + 1];
		blockpos++;
	}
	UNLOCK();
}

void eng_init(const char* path) {
	LOCK();
	snprintf(store_path, sizeof(store_path), "%s", path ? path : "");
	store_read();
	rnd_state ^= plat_ms() * 2654435761u; // a different first acid line every launch
	groove_init();
	groove_toggle(); // Groove mode on, drum grid shown
	ShowMessage(F_20_BOLD, "groove drums", "space = play");
	UNLOCK();
}

// --- transport ----------------------------------------------------------------
void eng_play_toggle(void) { LOCK(); groove_play_button(0); UNLOCK(); }
int eng_playing(void) { LOCK(); int r = drums_pending == PEND_DRUMS ? 2 : drums_play ? 1 : 0; UNLOCK(); return r; }
void eng_set_bpm(float b) { LOCK(); bpm = b < 30.f ? 30.f : b > 300.f ? 300.f : b; UNLOCK(); }
float eng_bpm(void) { return bpm; }

// --- lanes and steps ----------------------------------------------------------
static int lane_ok(int l) { return l >= 0 && l < GROOVE_LANES; }
void eng_select_lane(int l) { LOCK(); if (lane_ok(l) && lock_step == NO_STEP) lane_sel = l; UNLOCK(); }
int eng_lane_sel(void) { return lane_sel; }
int eng_step(int l, int s) { return lane_ok(l) ? pat.step[l][s & 63] : 0; }
int eng_step_locked(int l, int s) { LOCK(); int n = lane_ok(l) ? step_locks(l, s & 63) : 0; UNLOCK(); return n; }
int eng_lane_pos(int l) { return (running && lane_ok(l)) ? lane_pos[l] : -1; }
int eng_flash(int l) { return lane_ok(l) ? flash[l] : 0; }
int eng_is_acid(int l) { return lane_ok(l) && pat.lane[l].snd.model == DM_ACID; }

void eng_toggle_step(int l, int s) {
	if (!lane_ok(l)) return;
	LOCK();
	u8* p = &pat.step[l][s & 63];
	if (*p & STEP_TRIG) {
		*p = 0;
		clear_step_locks(l, s & 63);
		if (lock_step == (s & 63) && lock_lane == l) lock_step = NO_STEP;
	}
	else
		*p = STEP_TRIG | ((pat.lane[l].snd.model == DM_ACID ? 6 : VEL_DEFAULT) << STEP_VEL_SHIFT);
	mark_dirty();
	UNLOCK();
}

void eng_step_wheel(int l, int s, int delta) {
	if (!lane_ok(l) || !delta) return;
	LOCK();
	u8* p = &pat.step[l][s & 63];
	if (*p & STEP_TRIG) {
		if (pat.lane[l].snd.model == DM_ACID) { // note degree 0..7
			int n = clampi((*p & STEP_NOTE_MASK) + (delta > 0 ? 1 : -1), 0, 7);
			*p = (*p & ~STEP_NOTE_MASK) | n;
		}
		else {
			int v = (*p >> STEP_VEL_SHIFT) & STEP_VEL_MASK;
			if (!v) v = VEL_DEFAULT;
			v = clampi(v + (delta > 0 ? 1 : -1), 1, 7);
			*p = (*p & ~(STEP_VEL_MASK << STEP_VEL_SHIFT)) | (v << STEP_VEL_SHIFT);
		}
		mark_dirty();
	}
	UNLOCK();
}

void eng_toggle_accent(int l, int s) {
	if (!lane_ok(l)) return;
	LOCK();
	u8* p = &pat.step[l][s & 63];
	if (*p & STEP_TRIG) {
		int v = (*p >> STEP_VEL_SHIFT) & STEP_VEL_MASK;
		int nv = v == 7 ? (pat.lane[l].snd.model == DM_ACID ? 6 : VEL_DEFAULT) : 7;
		*p = (*p & ~(STEP_VEL_MASK << STEP_VEL_SHIFT)) | (nv << STEP_VEL_SHIFT);
		mark_dirty();
	}
	UNLOCK();
}

void eng_toggle_slide(int l, int s) {
	if (!lane_ok(l)) return;
	LOCK();
	u8* p = &pat.step[l][s & 63];
	if (*p & STEP_TRIG) { *p ^= STEP_SLIDE; mark_dirty(); }
	UNLOCK();
}

int eng_muted(int l) { return lane_ok(l) && (pat.lane[l].flags & LANE_MUTE); }
void eng_toggle_mute(int l) { if (!lane_ok(l)) return; LOCK(); pat.lane[l].flags ^= LANE_MUTE; mark_dirty(); UNLOCK(); }

void eng_trigger(int l, int vel) {
	if (!lane_ok(l)) return;
	LOCK();
	if (lock_step == NO_STEP) lane_sel = l;
	flash[l] = 255;
	if (pat.lane[l].snd.model != DM_SAMPLE)
		trigger_lane(l, clampi(vel, 1, 7), NO_STEP, 0, 0);
	UNLOCK();
}

int eng_view_page(void) { LOCK(); int p = view_page(); UNLOCK(); return p; }
int eng_page_in_loop(int p) { LOCK(); int r = page_in_loop(p & 3); UNLOCK(); return r; }
void eng_page(int d) { LOCK(); groove_page(d); UNLOCK(); }
int eng_loop_start(int l) { return lane_ok(l) ? LANE_START(&pat.lane[l]) : 0; }
int eng_loop_len(int l) { return lane_ok(l) ? pat.lane[l].length : 16; }
void eng_set_loop_start(int l, int s) { if (!lane_ok(l)) return; LOCK(); set_loop(l, s & 63, 1); mark_dirty(); UNLOCK(); }
void eng_set_loop_end(int l, int s) { if (!lane_ok(l)) return; LOCK(); set_loop(l, s & 63, 0); mark_dirty(); UNLOCK(); }
void eng_clear_lane(void) { LOCK(); groove_clear_lane(); UNLOCK(); }

// --- sound -------------------------------------------------------------------
int eng_model_count(void) { return DM_LAST; }
const char* eng_model_name_at(int pos) { return drum_model_names[drum_model_order[clampi(pos, 0, DM_LAST - 1)]]; }
int eng_model_pos(int l) { return lane_ok(l) ? model_pos(pat.lane[l].snd.model) : 0; }
const char* eng_lane_model_name(int l) { return lane_ok(l) ? drum_model_names[pat.lane[l].snd.model] : ""; }
void eng_set_model(int l, int pos) {
	if (!lane_ok(l)) return;
	LOCK();
	lock_step = NO_STEP;
	lane_sel = l;
	lastencodertime = eng_millis() - 1000; // absolute choice, like the slider
	groove_set_field(GF_MODEL, ((clampi(pos, 0, DM_LAST - 1) * 2 + 1) * 1024) / (DM_LAST * 2));
	UNLOCK();
}

void eng_acid_new_line(void) {
	LOCK();
	if (pat.lane[lane_sel].snd.model == DM_ACID) {
		lock_step = NO_STEP;
		acid_prev_model = DM_ACID; // keeps the sound
		note_model_chosen(lane_sel);
	}
	UNLOCK();
}

int eng_field_get(int field) {
	if (field <= GF_MODEL || field > GF_FENV) return 0;
	LOCK();
	u8 b = field_byte(field);
	UNLOCK();
	return field == GF_TUNE ? (s8)b : b;
}

void eng_field_set(int field, int v) {
	if (field <= GF_MODEL || field > GF_FENV) return;
	LOCK();
	u8 b = field == GF_TUNE ? (u8)(s8)clampi(v, -120, 120) : (u8)clampi(v, 0, 255);
	if (lock_step != NO_STEP)
		set_lock(field, b);
	else
		((u8*)&pat.lane[lane_sel].snd)[field] = b;
	mark_dirty();
	UNLOCK();
}

const char* eng_field_str(int field, char* buf) {
	LOCK();
	const char* s = groove_field_str(field, buf);
	if (s != buf) { snprintf(buf, 32, "%s", s); s = buf; }
	UNLOCK();
	return s;
}
const char* eng_field_name(int field) { return groove_field_name(field); }

// --- step lock mode ------------------------------------------------------------
void eng_lock_step(int l, int s) {
	if (!lane_ok(l)) return;
	LOCK();
	s &= 63;
	if (lock_step == s && lock_lane == l)
		lock_step = NO_STEP;
	else {
		lane_sel = l;
		lock_step = s;
		lock_lane = l;
		u8* p = &pat.step[l][s];
		if (!(*p & STEP_TRIG)) { // a locked step needs a hit
			*p = STEP_TRIG | ((pat.lane[l].snd.model == DM_ACID ? 6 : VEL_DEFAULT) << STEP_VEL_SHIFT);
			mark_dirty();
		}
	}
	UNLOCK();
}
void eng_lock_exit(void) { LOCK(); lock_step = NO_STEP; UNLOCK(); }
int eng_lock_step_idx(void) { return lock_step == NO_STEP ? -1 : lock_step; }
int eng_lock_lane(void) { return lock_lane; }
void eng_lock_clear(void) { LOCK(); if (lock_step != NO_STEP) groove_lock_clear(); UNLOCK(); }
int eng_cond_count(void) { return COND_LAST; }
const char* eng_cond_name(int c) { return cond_names[clampi(c, 0, COND_LAST - 1)]; }
int eng_cond_get(void) { LOCK(); int c = cond_value(); UNLOCK(); return c; }
void eng_cond_set(int c) {
	LOCK();
	if (lock_step != NO_STEP) { set_lock(GF_COND, clampi(c, 0, COND_LAST - 1)); mark_dirty(); }
	UNLOCK();
}

// --- drum bus ------------------------------------------------------------------
static u8* bus_byte(int w) { return w == 0 ? &pat.volume : w == 1 ? &pat.comp : w == 2 ? &pat.djfilter : &pat.drive; }
int eng_bus_get(int w) { return *bus_byte(w); }
void eng_bus_set(int w, int v) { LOCK(); *bus_byte(w) = clampi(v, 0, 255); mark_dirty(); UNLOCK(); }
const char* eng_bus_str(int w, char* buf) {
	LOCK();
	const char* s = groove_field_str(w == 0 ? GF_VOLUME : w == 1 ? GF_COMP : w == 2 ? GF_DJFILTER : GF_BUSDRIVE, buf);
	if (s != buf) { snprintf(buf, 32, "%s", s); s = buf; }
	UNLOCK();
	return s;
}

// --- patterns ----------------------------------------------------------------------
int eng_slot(void) { return cur_slot; }
int eng_pending_slot(void) { return pending_slot == NO_SLOT ? -1 : pending_slot; }
void eng_select_slot(int slot) { LOCK(); groove_preset_pad(clampi(slot, 0, 23)); groove_preset_pad_up(); UNLOCK(); }
void eng_pump(void) { LOCK(); groove_flash_pump(); UNLOCK(); }
void eng_save_now(void) {
	LOCK();
	if (dirty) { dirty = 0; SaveGroovePattern(cur_slot, &pat, sizeof(pat)); }
	UNLOCK();
}

void eng_oled(uint8_t* out) {
	LOCK();
	memset(oled, 0, sizeof(oled));
	groove_draw_oled(0);
	memcpy(out, oled, sizeof(oled));
	UNLOCK();
}

int eng_message(char* buf, int n, char* sub, int nsub) {
	LOCK();
	snprintf(buf, n, "%s", msg_main);
	snprintf(sub, nsub, "%s", msg_sub);
	int age = (int)(eng_millis() - msg_ms);
	UNLOCK();
	return age;
}
