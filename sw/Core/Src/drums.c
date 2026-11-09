// drums.c - synthesised drums for Groove mode. See drums.h.
//
// Every model is one generic voice:
//   tone oscillator (sine or two squares) with a decaying pitch envelope
// + white noise through a one-pole low-pass and a one-pole high-pass
// each with its own exponential decay, optional clap re-triggers and a soft
// clipper. Only sounding voices are computed. No libm calls (cheap
// approximations), no allocation; trigger and render run in the audio ISR.

#include "drums.h"
#include <string.h>

#define SR 31250.f   // Plinky's audio sample rate (ACCURATE_FS)
#define BLOCK 64     // samples per audio block
#define VOICES 8
#define OUT_GAIN 11000.f
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
	float drv, gain;
	uint32_t rng;
	uint16_t clap_timer;
	uint8_t clap_left, osc, model, active;
} DrumVoice;

static DrumVoice dv[VOICES];

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

void drums_render(int16_t* dst) {
	for (int v = 0; v < VOICES; ++v) {
		DrumVoice* d = &dv[v];
		if (!d->active)
			continue;
		float ph = d->ph, ph2 = d->ph2, penv = d->penv, at = d->at, an = d->an, lp = d->lp, hp = d->hp;
		const float dph = d->dph, kp = d->kp, kt = d->kt, lpk = d->lpk, hpk = d->hpk, drv = d->drv, gain = d->gain;
		float kn = d->kn;
		uint32_t rng = d->rng;
		const int osc = d->osc;
		int16_t* out = dst;
		for (int i = 0; i < BLOCK; ++i) {
			rng = rng * 1664525u + 1013904223u;
			float n = (float)(int32_t)rng * (1.f / 2147483648.f);
			lp += lpk * (n - lp);
			hp += hpk * (lp - hp);
			float o = (lp - hp) * an;
			if (osc) {
				ph += dph * (1.f + penv);
				if (ph >= 1.f) ph -= 1.f;
				float s;
				if (osc == 1) { // parabolic sine
					float p = ph * 2.f - 1.f;
					s = 4.f * p * (1.f - (p < 0.f ? -p : p));
				}
				else {
					ph2 += dph * d->ratio2;
					if (ph2 >= 1.f) ph2 -= 1.f;
					s = (ph < 0.5f ? 0.5f : -0.5f) + (ph2 < 0.5f ? 0.5f : -0.5f);
				}
				o += s * at;
			}
			at *= kt;
			an *= kn;
			penv *= kp;
			if (d->clap_left && --d->clap_timer == 0) {
				an = d->a0;
				d->clap_timer = CLAP_GAP;
				if (--d->clap_left == 0)
					kn = d->kn_tail;
			}
			if (drv > 0.f)
				o = o * (1.f + drv) / (1.f + (o < 0.f ? -o : o) * drv);
			int32_t s = (int32_t)(o * gain);
			out[0] = sat16(out[0] + s);
			out[1] = sat16(out[1] + s);
			out += 2;
		}
		d->ph = ph; d->ph2 = ph2; d->penv = penv; d->at = at; d->an = an; d->kn = kn;
		d->lp = lp; d->hp = hp; d->rng = rng;
		if (at < 2e-4f && an < 2e-4f && !d->clap_left)
			d->active = 0;
	}
}
