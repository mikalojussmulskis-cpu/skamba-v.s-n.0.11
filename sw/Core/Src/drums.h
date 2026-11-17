// drums.h - synthesised drum engine for Groove mode (see GROOVEBOX_PLAN.md).
// Up to 6 drum voices (kicks are never stolen), rendered in the audio ISR into
// a separate drum bus: a dry signal that is added after Plinky's synth
// compressor, plus delay and reverb sends that feed Plinky's delay and reverb
// independently of the synth's own send levels.
#pragma once
#include <stdint.h>

// Stored model numbers: never reorder (saved patterns use them); add at the end.
enum EDrumModels {
	DM_KICK, DM_SNARE, DM_CLAP, DM_CHAT, DM_OHAT, DM_TOM, DM_PERC,
	DM_SAMPLE, // not synthesised: the lane plays its slice of the preset's sample on a Plinky voice
	DM_KICK2, DM_CYMBAL, DM_RIM, DM_FM, DM_ZAP,
	DM_GLITCH, DM_FOLD, DM_CLANG, DM_CHIRP, DM_CRUSH,
	DM_BASS, // sustained bass note; its tune snaps to the preset's scale
	DM_ACID, // acid line: saw / square through a resonant ladder filter with slides and accents (notes per step)
	DM_LXR_DRUM, // LXR-02 drum voice (port of its algorithm, GPL-3.0): sine with FM following the pitch EG, slope amp EG, snap
	DM_LXR_SNARE, // LXR-02 snare voice: tone plus sample-and-hold noise through its own filter, one amp EG
	DM_LAST
};
// order in which the Shape pad / encoder steps through the models
extern const uint8_t drum_model_order[DM_LAST];
extern const char* const drum_model_names[DM_LAST];

#define DRUM_FILTER_OFF 128 // filter byte: <128 low-pass, 128 off, >128 high-pass

typedef struct DrumParams { // 14 bytes, stored per lane in the groove pattern
	uint8_t model;  // EDrumModels
	int8_t tune;    // -120..120 = -24..+24 semitones (5 per semitone)
	uint8_t sweep;  // pitch envelope depth (DM_ACID: slide time)
	uint8_t decay;  // (DM_ACID: filter envelope decay)
	uint8_t snap;   // transient / noise amount (FM: modulation index; DM_ACID: accent)
	uint8_t tone;   // "colour": model specific (click tone, noise brightness, FM ratio, DM_ACID: saw -> square)
	uint8_t drive;
	uint8_t level;
	uint8_t dlsend; // delay send
	uint8_t rvsend; // reverb send
	uint8_t filter; // resonant filter: 0..127 low-pass (cutoff), 128 off, 129..255 high-pass (DM_ACID: ladder cutoff, 0..255)
	uint8_t reso;   // filter resonance
	uint8_t srr;    // sample rate reduction: 0 off .. 255 = 1/64 of the rate
	uint8_t fenv;   // filter envelope: 128 off, >128 opens on the hit and closes, <128 starts closed and opens (DM_ACID: envelope depth)
} DrumParams;

// drum bus, filled by drums_render() every audio block
#define DRUM_BLOCK 64
extern int16_t drum_bus_dry[DRUM_BLOCK];    // mono, full rate
extern int16_t drum_bus_dl[DRUM_BLOCK / 2]; // delay send, half rate (Plinky's FX run at half rate)
extern int16_t drum_bus_rv[DRUM_BLOCK / 2]; // reverb send, half rate

// lane = which groove lane is playing (a lane retriggers its own voice).
// p must stay valid while the voice sounds: level, sends, drive and the filter
// are read live every block, so turning them affects ringing hits.
void drums_trigger(int lane, const DrumParams* p, int vel); // vel 1..7 (audio ISR)
// sequenced note of a DM_ACID lane: gate = blocks the note is held (0 = 200 ms);
// legato = the previous step was a slide: if that note still sounds, glide to the new pitch
// instead of retriggering. Other models ignore gate and legato.
void drums_note(int lane, const DrumParams* p, int vel, int gate, int legato);
// master volume 0..255; djfilter: global drum filter like the lane filter byte;
// comp: drum bus compressor amount 0 (off) .. 255 (kicks bypass it);
// drive: global overdrive / wavefolder 0 (off) .. 255;
// srr: global sample rate reduction 0 (off) .. 255 = 1/64 of the rate (audio ISR)
void drums_render(int master, int djfilter, int comp, int drive, int srr);
void drums_stop_all(void);
