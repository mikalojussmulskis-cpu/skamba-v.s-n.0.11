// groove.h - Plinky Groovebox extension (see GROOVEBOX_PLAN.md in the repo root).
//
// Rules:
//  - All groove code lives in groove.c / groove.h.
//  - Existing code calls in only through small hooks marked "GROOVE HOOK".
//  - Every runtime hook is guarded by GROOVE_IS_ON (the groove_active flag),
//    so with Groove mode off the normal Plinky code paths behave as before.
//  - Building with -DGROOVE_ENABLE=0 compiles every hook out. Together with
//    -DFACTORY_TEST_JIG that build is byte-identical to upstream v0.B4.
#pragma once

#ifndef GROOVE_ENABLE
#define GROOVE_ENABLE 1
#endif

#if GROOVE_ENABLE

// groove_active: 0 = normal Plinky (default at boot), 1 = Groove mode (drum engine running).
// groove_grid:   1 = the pads show the drum grid, 0 = the pads play Plinky as usual
//                while the drums keep running. Only ever 1 while groove_active is 1.
extern volatile unsigned char groove_active;
extern volatile unsigned char groove_grid;
#define GROOVE_IS_ON (groove_active)
#define GROOVE_GRID (groove_grid)

// Momentary drum FX (roll, ratchet, reverse, pitch fall, gate, crush on the Arp / Seq
// rows, phase 14). Switched off for now; build with -DGROOVE_FX=1 to bring them back.
#ifndef GROOVE_FX
#define GROOVE_FX 0
#endif

// firmware name of Groove builds (config.h keeps Plinky's own, "v0.B4", for GROOVE=0)
#undef VERSION2
#define VERSION2 "v.s-n.0.1"

// Use for one-line hooks in existing code: GROOVE_HOOK(groove_foo());
#define GROOVE_HOOK(stmt) do { stmt; } while (0)

#else

#define GROOVE_IS_ON 0
#define GROOVE_GRID 0
#define GROOVE_HOOK(stmt) do { } while (0)

#endif // GROOVE_ENABLE

// Prototypes are always declared so hooks compile (as dead code) when GROOVE_ENABLE=0.
// --- lifecycle --------------------------------------------------------------
void groove_init(void);   // plinky_init(), after InitParamsOnBoot()
void groove_toggle(void);      // audio ISR: Params 1 + blank pad (R2C7): Groove mode on/off
void groove_view_toggle(void); // audio ISR: Params 2 + blank pad (R2C7): drum grid <-> Plinky pads

// --- pad input (audio ISR, from edit.h finger_editing) ---------------------
// row = pad row 0 (top) .. 7 (bottom), fi = column 0..7
int  groove_pad_is_edit(int fi, int row); // nonzero: this touch is a groove control, not a drum hit
void groove_pad(int fi, int row, int trig, int pressure); // control pad held (trig = first stable frame)
void groove_pad_up(int fi);                               // finger lifted

// --- sequencer (audio ISR) --------------------------------------------------
// every block (2.048 ms). clock16 = a 16th-note clock pulse this block,
// step_ticks = blocks per 16th note, recording = Plinky's record flag
// playing = Plinky's sequencer is running. Returns 1 when the Plinky sequencer
// must start now (its start was waiting for the drums' next bar).
int  groove_tick(int clock16, int playing, int step_ticks, int recording);
void groove_reset(void); // transport reset (Prev while playing, MIDI start, ...)
// --- transport: the drums have their own play/stop -------------------------
// Play pressed: in the drum grid it starts/stops the drums; in the Plinky view
// with the drums running it makes Plinky start on the drums' next bar.
// Returns 1 when handled (Plinky's own play logic must be skipped).
int  groove_play_button(int plinky_playing);
void groove_transport(int on); // MIDI / clock start, stop: both sequencers
int  groove_play_led(int plinky_playing, int glow); // Play LED (drums in the grid, Plinky otherwise)
// finger_synth_update(), right after the touch is read (drum grid only):
// live drum hits (row 1 = loudest .. row 5 = quietest, recorded when Record +
// Play are on); swallows the touch so it doesn't play the Plinky voice
void groove_touch(int fi, int* pressure, int position);
// finger_synth_update(), before MIDI: sequenced hits of "Sample" lanes
// replace pressure/position when the string isn't being touched
void groove_finger(int fi, int* pressure, int* position);
// DoAudio(), after the 8 Plinky voices: render the drum bus, which the FX loop
// then mixes in (dry after the synth compressor, plus delay / reverb sends)
void groove_audio(void);
extern short drum_bus_dry[64], drum_bus_dl[32], drum_bus_rv[32];
// FX loop, last step: Plinky's stereo output sample (low half left, high half right)
// plus the dry drum sample, through a soft knee (linear below 80 % of full scale)
// instead of Plinky's hard saturation, so loud synth + loud drums round off, not clip
unsigned long groove_mix(unsigned long lr, int drum);
extern int groove_rv_in; // drum reverb send for the current Reverb2() call

// --- drum lane sound, edited through printed param pads in the drum grid ----
enum EGrooveFields { GF_MODEL, GF_TUNE, GF_SWEEP, GF_DECAY, GF_SNAP, GF_COLOR, GF_DRIVE, GF_LEVEL,
	GF_DLSEND, GF_RVSEND, // delay / reverb send of the selected lane
	GF_FILTER, GF_RESO,   // resonant filter of the selected lane
	GF_SRR,               // sample rate reduction of the selected lane
	GF_FENV,              // filter envelope of the selected lane
	GF_VOLUME,            // drum master volume (whole groove pattern)
	GF_DJFILTER,          // global drum filter, DJ style (whole groove pattern)
	GF_COMP,              // drum bus compressor (whole groove pattern)
	GF_COND,              // step condition of the locked step (Arp / Seq order pads; stored as a step lock)
	// momentary drum FX (Arp row: Divide, Prob, Euclid Len, Octaves; Seq row: Divide, Prob):
	// the effect runs while the pad is held under Shift; encoder / slider = its setting
	GF_ROLL, GF_RATCHET, GF_REVERSE, GF_PITCHFALL, GF_GATE, GF_CRUSH,
	GF_BUSDRIVE,          // global drum overdrive (whole groove pattern)
	GF_BUSSRR };          // global drum sample rate reduction (whole groove pattern)
// encoder click puts these back to the middle (off)
#define GROOVE_FIELD_IS_FILTER(f) ((f) == GF_FILTER || (f) == GF_DJFILTER || (f) == GF_FENV)
// fields of one lane (the OLED shows the lane / locked step), as opposed to the whole drum bus
#define GROOVE_FIELD_IS_LANE(f) ((f) >= 0 && ((f) <= GF_FENV || (f) == GF_COND))
int  groove_lock_active(void);              // step lock mode (long press on a step)
void groove_lock_clear(void);               // encoder click in step lock mode
void groove_clear_lane(void);               // Clear held in the drum grid: selected lane's steps + default sound
int  GrooveScaleQuantize(int semitones);    // params.h: nearest note of Plinky's scale
int  GrooveScaleDegree(int degree);         // params.h: semitones of the degree (0 = root, 7 = usually the octave)
// value of a field of the selected lane, in param units (FULL = 1024)
int  groove_get_field(int field);
void groove_set_field(int field, int value);
// modulation amount (source = Plinky mod source M_ENV..M_RND) of a lane field
int  groove_get_mod(int field, int source);
void groove_set_mod(int field, int source, int value);
const char* groove_model_name(void);        // model of the selected lane
const char* groove_field_name(int field);   // OLED name, e.g. "Pitch Sweep"
const char* groove_field_str(int field, char* buf); // OLED value text
const char* groove_lane_label(void);        // "Drum 3"
const char* groove_field_page(int field);   // OLED second line: lane label, "all drums", "hold pad = on"
void groove_fx_down(int field, int fi);     // a param pad was touched (fi = column): starts a momentary FX
// Prev / Next held in the drum grid: the 64 pads are the selected lane's steps
void groove_loop_pad(int fi, int step, int start, int trig); // start = Prev (loop start), else Next (loop end)
void groove_page(int delta);                // Prev / Next short press: the 16-step page shown on R7 / R8

// --- Plinky voice limit in Groove mode (4 notes), DoAudio + UpdateEnvelope --
extern unsigned char groove_voice_kill;     // stolen voices: fade out, stay silent until replayed
unsigned char groove_voice_alloc(unsigned char down, unsigned char trig, const float* vol0, int stride);

// --- groove patterns: 24 slots in flash, chosen with the Preset-mode pattern pads
#define GROOVE_SLOTS 24
void groove_preset_pad(int slot);       // pattern pad touched (audio ISR)
void groove_preset_pad_up(void);        // ...and released: select it (queued to the bar)
void groove_preset_longpress(int slot); // long press: copy the current pattern there
void groove_clear_slot(int slot);       // Preset + hold Clear on a pattern pad
int  groove_preset_led(int slot, int flicker);
int  groove_slot(void);                 // current pattern slot 0..23
void groove_flash_pump(void);           // main loop (PumpFlashWrites): autosave, copy
// provided by params.h (flash store)
const void* GetSavedGroovePattern(int slot); // 0 if the slot was never saved
void SaveGroovePattern(int slot, const void* data, int size);
int  GrooveFlashBusy(void);

// --- drawing (main loop) ----------------------------------------------------
int  groove_boot_frame(void); // start-up: a cat runs across the empty screen; 1 while running, 0 when done
// mode: 0 = drum grid, 1 = Prev held (lane start), 2 = Next held (lane end)
void groove_draw_oled(int mode);
void groove_draw_leds(int mode);

// ---------------------------------------------------------------------------
// Measurement build (make GROOVE_STATS=1). Never shipped as a release.
// Shows audio-ISR CPU load, core clock and stack high-water on the OLED.
#ifdef GROOVE_STATS
void groove_stats_init(void); // paint the free stack area; call first thing in plinky_init()
void groove_stats_draw(void); // overlay numbers on the OLED framebuffer; call before flip()
#endif
