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

// 0 = normal Plinky (default at boot), 1 = Groove mode.
extern volatile unsigned char groove_active;
#define GROOVE_IS_ON (groove_active)

// Use for one-line hooks in existing code: GROOVE_HOOK(groove_foo());
#define GROOVE_HOOK(stmt) do { stmt; } while (0)

#else

#define GROOVE_IS_ON 0
#define GROOVE_HOOK(stmt) do { } while (0)

#endif // GROOVE_ENABLE

// Prototypes are always declared so hooks compile (as dead code) when GROOVE_ENABLE=0.
// --- lifecycle --------------------------------------------------------------
void groove_init(void);   // plinky_init(), after InitParamsOnBoot()
void groove_toggle(void); // audio ISR: Params 1 + blank pad (R2C7) tapped

// --- pad input (audio ISR, from edit.h finger_editing) ---------------------
// row = pad row 0 (top) .. 7 (bottom), fi = column 0..7
int  groove_pad_is_edit(int fi, int row); // nonzero: this touch is a groove control, not a drum hit
void groove_pad(int fi, int row, int trig, int pressure); // control pad held (trig = first stable frame)
void groove_pad_up(int fi);                               // finger lifted

// --- sequencer (audio ISR) --------------------------------------------------
// every block (2.048 ms). clock16 = a 16th-note clock pulse this block,
// step_ticks = blocks per 16th note
void groove_tick(int clock16, int playing, int preview, int step_ticks);
void groove_reset(void); // transport reset (Prev while playing, MIDI start, ...)
// called per string from finger_synth_update(): may replace pressure/position
// with a sequenced hit when the string isn't being touched
void groove_finger(int fi, int* pressure, int* position);

// --- drawing (main loop) ----------------------------------------------------
void groove_draw_oled(void);
void groove_draw_leds(void);

// ---------------------------------------------------------------------------
// Measurement build (make GROOVE_STATS=1). Never shipped as a release.
// Shows audio-ISR CPU load, core clock and stack high-water on the OLED.
#ifdef GROOVE_STATS
void groove_stats_init(void); // paint the free stack area; call first thing in plinky_init()
void groove_stats_draw(void); // overlay numbers on the OLED framebuffer; call before flip()
#endif
