// drums.h - small synthesised drum engine for Groove mode (see GROOVEBOX_PLAN.md).
// 8 drum voices (one per lane), rendered in the audio ISR into a separate drum
// bus: a dry signal that is added after Plinky's synth compressor, plus delay
// and reverb sends that feed Plinky's delay and reverb independently of the
// synth's own send levels.
#pragma once
#include <stdint.h>

enum EDrumModels {
	DM_KICK, DM_SNARE, DM_CLAP, DM_CHAT, DM_OHAT, DM_TOM, DM_PERC,
	DM_SAMPLE, // not synthesised: the lane plays its slice of the preset's sample on a Plinky voice
	DM_LAST
};

typedef struct DrumParams { // 10 bytes, stored per lane in the groove pattern
	uint8_t model;  // EDrumModels
	int8_t tune;    // -128..127 = -1..+1 octave
	uint8_t sweep;  // pitch envelope depth
	uint8_t decay;
	uint8_t snap;   // click / noise amount
	uint8_t tone;   // brightness (filter)
	uint8_t drive;
	uint8_t level;
	uint8_t dlsend; // delay send
	uint8_t rvsend; // reverb send
} DrumParams;

extern const char* const drum_model_names[DM_LAST];

// drum bus, filled by drums_render() every audio block
#define DRUM_BLOCK 64
extern int16_t drum_bus_dry[DRUM_BLOCK];    // mono, full rate
extern int16_t drum_bus_dl[DRUM_BLOCK / 2]; // delay send, half rate (Plinky's FX run at half rate)
extern int16_t drum_bus_rv[DRUM_BLOCK / 2]; // reverb send, half rate

void drums_trigger(int voice, const DrumParams* p, int vel); // vel 1..7 (audio ISR)
void drums_render(int master);                               // master volume 0..255 (audio ISR)
void drums_stop_all(void);
