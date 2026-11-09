// drums.c - synthesised drums for Groove mode. See drums.h.
//
// Every model is one generic voice:
//   tone oscillator (sine or two squares) with a decaying pitch envelope
// + white noise through a one-pole low-pass and a one-pole high-pass
// each with its own exponential decay, optional clap re-triggers and a soft
// clipper. Only sounding voices are computed, and per block only the parts
// (tone / noise / sends / drive) that are in use. No libm calls, no division
// per sample, no allocation; trigger and render run in the audio ISR.

#include "drums.h"
#include <string.h>

#define SR 31250.f   // Plinky's audio sample rate (ACCURATE_FS)
#define BLOCK 64     // samples per audio block
#define VOICES 8
#define OUT_GAIN 9000.f // at master volume 255 and lane level 255
#define SILENT 5e-4f     // a voice stops below this amplitude (about -66 dB)
#define CLAP_GAP 330 // samples between clap bursts (~10.5 ms)

const char* const drum_model_names[DM_LAST] = {
	"Kick", "Snare", "Clap", "C.Hat", "O.Hat", "Tom", "Perc", "Sample",
};

typedef struct DrumVoice {
	float ph, ph2, dph, ratio2; // oscillator phases (0..1) and increment
	float penv, kp;             // pitch envelope and its per-sample decay
	float at, kt;               // tone amplitude and decay
	float an, kn, kn_tail, a0;  // noise amplitude, decay, decay after the clap bursts, burst level
	float lpk, hpk, lp, hp;     // noise filters (lpk = 1 bypasses the low-pass, hpk = 0 the high-pass)
	float drv, gain, dl, rv;    // drive, output gain, delay / reverb send (0..0.5)
	uint32_t rng;
	uint16_t clap_timer;
	uint8_t clap_left, osc, model, active;
} DrumVoice;

static DrumVoice dv[VOICES];

int16_t drum_bus_dry[DRUM_BLOCK];
int16_t drum_bus_dl[DRUM_BLOCK / 2];
int16_t drum_bus_rv[DRUM_BLOCK / 2];

// 2^x, about 0.3 % error, no libm
static float fexp2(float x) {
	int i = (int)(x + 64.f) - 64; // floor for |x| < 64
	float f = x - (float)i;
	union { float f; int32_t i; } u;
	u.f = 1.f + f * (0.6565f + f * 0.344f);
	u.i += i << 23;
	return u.f;
}
// per-sample multiplier that decays to 1/e in `seconds`
static float kdecay(float seconds) {
	return 1.f - 1.f / (seconds * SR);
}
// one-pole filter coefficient for cutoff fc (approximation, fine for drums)
static float kfilter(float fc) {
	float w = 6.2831853f * fc / SR;
	return w / (1.f + w);
}
// map 0..255 exponentially onto lo .. lo * 2^octaves
static float erange(uint8_t v, float lo, float octaves) {
	return lo * fexp2(v * (octaves / 255.f));
}

void drums_stop_all(void) {
	for (int v = 0; v < VOICES; ++v)
		dv[v].active = 0;
}

void drums_trigger(int v, const DrumParams* p, int vel) {
	if (v < 0 || v >= VOICES || p->model >= DM_SAMPLE)
		return;
	DrumVoice* d = &dv[v];
	uint32_t rng = d->rng + 0x9E3779B9u * (uint32_t)(v + 1);
	memset(d, 0, sizeof(*d));
	d->rng = rng;
	d->lpk = 1.f;
	d->model = p->model;
	float g = vel * (1.f / 7.f);
	float tune = fexp2(p->tune * (1.f / 128.f));
	float sweep = p->sweep * (1.f / 255.f);
	float snap = p->snap * (1.f / 255.f);
	float tone = p->tone * (1.f / 255.f);
	d->drv = p->drive * (4.f / 255.f);
	d->gain = p->level * (OUT_GAIN / 255.f);
	d->dl = p->dlsend * (0.5f / 255.f);
	d->rv = p->rvsend * (0.5f / 255.f);

	switch (p->model) {
	case DM_KICK:
		d->osc = 1;
		d->dph = 45.f * tune / SR;
		d->penv = sweep * 8.f;
		d->kp = kdecay(0.02f);
		d->at = g;
		d->kt = kdecay(erange(p->decay, 0.05f, 5.f));
		d->an = g * snap * 0.6f; // click
		d->kn = kdecay(0.004f);
		d->lpk = kfilter(800.f + tone * 6000.f);
		break;
	case DM_SNARE:
		d->osc = 1;
		d->dph = 180.f * tune / SR;
		d->penv = sweep * 2.f;
		d->kp = kdecay(0.015f);
		d->at = g * (1.f - snap * 0.8f);
		d->kt = kdecay(erange(p->decay, 0.02f, 3.f));
		d->an = g * (0.3f + snap * 0.7f);
		d->kn = kdecay(erange(p->decay, 0.04f, 3.5f));
		d->hpk = kfilter(600.f + tone * 5000.f);
		break;
	case DM_CLAP:
		d->a0 = d->an = g;
		d->kn = kdecay(0.007f);
		d->kn_tail = kdecay(erange(p->decay, 0.05f, 4.f));
		d->clap_left = 3;
		d->clap_timer = CLAP_GAP;
		d->lpk = kfilter(1500.f + tone * 3000.f);
		d->hpk = kfilter(700.f);
		break;
	case DM_CHAT:
		for (int i = 0; i < VOICES; ++i) // a closed hat chokes open hats
			if (dv[i].active && dv[i].model == DM_OHAT)
				dv[i].kn = dv[i].kt = 0.99f;
		d->an = g;
		d->kn = kdecay(erange(p->decay, 0.01f, 4.f));
		d->hpk = kfilter(3000.f + tone * 7000.f);
		break;
	case DM_OHAT:
		d->an = g;
		d->kn = kdecay(erange(p->decay, 0.08f, 3.5f));
		d->hpk = kfilter(3000.f + tone * 7000.f);
		break;
	case DM_TOM:
		d->osc = 1;
		d->dph = 90.f * tune / SR;
		d->penv = sweep * 3.f;
		d->kp = kdecay(0.04f);
		d->at = g;
		d->kt = kdecay(erange(p->decay, 0.05f, 4.5f));
		d->an = g * snap * 0.4f;
		d->kn = kdecay(0.01f);
		d->lpk = kfilter(500.f + tone * 4000.f);
		break;
	case DM_PERC: // cowbell-ish: two detuned squares
		d->osc = 2;
		d->dph = 540.f * tune / SR;
		d->ratio2 = 1.4815f;
		d->at = g * 0.5f;
		d->kt = kdecay(erange(p->decay, 0.02f, 4.5f));
		d->an = g * snap * 0.3f;
		d->kn = kdecay(0.005f);
		d->hpk = kfilter(2000.f + tone * 4000.f);
		break;
	}
	d->active = 1;
}

static inline int16_t sat16(int32_t x) {
	return (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
}

void drums_render(int master) {
	memset(drum_bus_dry, 0, sizeof(drum_bus_dry));
	memset(drum_bus_dl, 0, sizeof(drum_bus_dl));
	memset(drum_bus_rv, 0, sizeof(drum_bus_rv));
	const float mg = master * (1.f / 255.f);
	for (int v = 0; v < VOICES; ++v) {
		DrumVoice* d = &dv[v];
		if (!d->active)
			continue;
		// everything the loop touches lives in locals (registers)
		float ph = d->ph, ph2 = d->ph2, penv = d->penv, at = d->at, an = d->an, lp = d->lp, hp = d->hp, kn = d->kn;
		const float dph = d->dph, dph2 = d->dph * d->ratio2, kp = d->kp, kt = d->kt, lpk = d->lpk, hpk = d->hpk;
		const float a0 = d->a0, kn_tail = d->kn_tail;
		const float drv1 = 1.f + d->drv;
		const float gd = d->gain * mg, gdl = gd * d->dl, grv = gd * d->rv;
		uint32_t rng = d->rng;
		int clap_left = d->clap_left, clap_timer = d->clap_timer;
		const int osc = d->osc;
		// decide per block which parts are needed
		const int use_noise = an > 1e-5f || clap_left;
		const int use_tone = osc && at > 1e-5f;
		const int use_drive = d->drv > 0.f;
		const int use_dl = gdl > 0.f, use_rv = grv > 0.f;
		for (int i = 0; i < DRUM_BLOCK; ++i) {
			float o = 0.f;
			if (use_noise) {
				rng = rng * 1664525u + 1013904223u;
				float n = (float)(int32_t)rng * (1.f / 2147483648.f);
				lp += lpk * (n - lp);
				hp += hpk * (lp - hp);
				o = (lp - hp) * an;
				an *= kn;
				if (clap_left && --clap_timer == 0) {
					an = a0;
					clap_timer = CLAP_GAP;
					if (--clap_left == 0)
						kn = kn_tail;
				}
			}
			if (use_tone) {
				ph += dph * (1.f + penv);
				if (ph >= 1.f) ph -= 1.f;
				float s;
				if (osc == 1) { // parabolic sine
					float p = ph * 2.f - 1.f;
					s = 4.f * p * (1.f - (p < 0.f ? -p : p));
				}
				else { // two squares
					ph2 += dph2;
					if (ph2 >= 1.f) ph2 -= 1.f;
					s = (ph < 0.5f ? 0.5f : -0.5f) + (ph2 < 0.5f ? 0.5f : -0.5f);
				}
				o += s * at;
				at *= kt;
				penv *= kp;
			}
			if (use_drive) { // cubic soft clip, no division
				float x = o * drv1;
				x = x > 1.5f ? 1.5f : x < -1.5f ? -1.5f : x;
				o = x - 0.148148f * x * x * x;
			}
			drum_bus_dry[i] = sat16(drum_bus_dry[i] + (int32_t)(o * gd));
			if (use_dl)
				drum_bus_dl[i >> 1] = sat16(drum_bus_dl[i >> 1] + (int32_t)(o * gdl));
			if (use_rv)
				drum_bus_rv[i >> 1] = sat16(drum_bus_rv[i >> 1] + (int32_t)(o * grv));
		}
		if (!use_tone) at = 0.f; // below audibility (or no oscillator): let the voice finish
		if (!use_noise) an = 0.f;
		d->ph = ph; d->ph2 = ph2; d->penv = penv; d->at = at; d->an = an; d->kn = kn;
		d->lp = lp; d->hp = hp; d->rng = rng;
		d->clap_left = clap_left; d->clap_timer = clap_timer;
		if (at < SILENT && an < SILENT && !clap_left)
			d->active = 0;
	}
}
