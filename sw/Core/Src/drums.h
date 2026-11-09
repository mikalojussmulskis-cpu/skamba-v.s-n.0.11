// drums.h - small synthesised drum engine for Groove mode (see GROOVEBOX_PLAN.md).
// 8 drum voices (one per lane), rendered in the audio ISR and mixed into the
// synth bus before the global HPF / compressor / delay / reverb.
#pragma once
#include <stdint.h>

enum EDrumModels {
	DM_KICK, DM_SNARE, DM_CLAP, DM_CHAT, DM_OHAT, DM_TOM, DM_PERC,
	DM_SAMPLE, // not synthesised: the lane plays its slice of the preset's sample on a Plinky voice
	DM_LAST
};

typedef struct DrumParams { // 8 bytes, stored per lane in the groove pattern
	uint8_t model;  // EDrumModels
	int8_t tune;    // -128..127 = -1..+1 octave
	uint8_t sweep;  // pitch envelope depth
	uint8_t decay;
	uint8_t snap;   // click / noise amount
	uint8_t tone;   // brightness (filter)
	uint8_t drive;
	uint8_t level;
} DrumParams;

extern const char* const drum_model_names[DM_LAST];

void drums_trigger(int voice, const DrumParams* p, int vel); // vel 1..7 (audio ISR)
void drums_render(int16_t* dst);                             // 64 interleaved stereo frames (audio ISR)
void drums_stop_all(void);
