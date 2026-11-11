// engine.h - Groove Drums for macOS: the Plinky groove sequencer and drum engine
// (sw/Core/Src/groove.c + drums.c, unchanged) running on the Mac, behind a small
// thread-safe API for the window (main.m). Every call takes the engine lock.
#pragma once
#include <stdint.h>

#define ENG_LANES 8
#define ENG_STEPS 64
#define ENG_SR 31250 // Plinky's audio rate; CoreAudio resamples to the output device

void eng_init(const char* save_path); // load saved patterns, Groove mode on, drum grid shown
void eng_render(float* stereo, int frames); // audio thread: interleaved L/R floats

// transport
void eng_play_toggle(void);
int  eng_playing(void);       // 0 stopped, 1 playing, 2 waiting to start
void eng_set_bpm(float bpm);
float eng_bpm(void);

// lanes and steps
void eng_select_lane(int l);
int  eng_lane_sel(void);
int  eng_step(int l, int step);        // raw step byte (b7 trig, b4-6 velocity, b3 slide, b0-2 note)
int  eng_step_locked(int l, int step); // number of locks / conditions on the step
int  eng_lane_pos(int l);              // playhead step, -1 when stopped
int  eng_flash(int l);                 // hit activity 0..255
void eng_toggle_step(int l, int step);
void eng_step_wheel(int l, int step, int delta); // velocity (drums) or note degree (Acid)
void eng_toggle_accent(int l, int step);         // velocity 7 <-> default
void eng_toggle_slide(int l, int step);          // Acid slide
int  eng_muted(int l);
void eng_toggle_mute(int l);
void eng_trigger(int l, int vel);                // live hit (keys 1-8)
int  eng_is_acid(int l);
int  eng_view_page(void);                        // 16-step page shown for the selected lane
int  eng_page_in_loop(int page);
void eng_page(int delta);
int  eng_loop_start(int l);
int  eng_loop_len(int l);
void eng_set_loop_start(int l, int step);
void eng_set_loop_end(int l, int step);
void eng_clear_lane(void);                       // like holding Clear: steps first, then the sound

// sound of the selected lane (or of the locked step in step lock mode)
int  eng_model_count(void);
const char* eng_model_name_at(int pos);          // in the Drum Type order
int  eng_model_pos(int l);
void eng_set_model(int l, int pos);
void eng_acid_new_line(void);                    // selected Acid lane: a new generated line (sound kept)
const char* eng_lane_model_name(int l);
int  eng_field_get(int field);                   // stored byte (tune: -120..120)
void eng_field_set(int field, int value);
const char* eng_field_str(int field, char* buf32);
const char* eng_field_name(int field);

// step lock mode (right click on a step)
void eng_lock_step(int l, int step);
void eng_lock_exit(void);
int  eng_lock_step_idx(void);                    // -1 = not in lock mode
int  eng_lock_lane(void);
void eng_lock_clear(void);
int  eng_cond_count(void);
const char* eng_cond_name(int c);
int  eng_cond_get(void);
void eng_cond_set(int c);

// whole drum bus
int  eng_bus_get(int which);                     // 0 volume, 1 compressor, 2 DJ filter, 3 drive, 4 sample rate (bytes)
void eng_bus_set(int which, int value);
const char* eng_bus_str(int which, char* buf32);

// patterns (24 slots, saved to the file given to eng_init)
int  eng_slot(void);
int  eng_pending_slot(void);                     // -1 none
void eng_select_slot(int slot);                  // switches on the next bar while playing
void eng_pump(void);                             // main thread, ~30 times a second: autosave
void eng_save_now(void);                         // on quit

// the drum screen (meters + dancing cat; text not drawn): 32 rows of 128 bytes, 1 = lit
void eng_oled(uint8_t* out128x32);

// last on-screen message of the firmware ("acid line", "lane 3 cleared" ...)
int  eng_message(char* buf, int n, char* sub, int nsub); // returns ms since it was shown
