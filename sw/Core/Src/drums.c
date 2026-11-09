// drums.c - synthesised drums for Groove mode. See drums.h.
//
// Every model is one generic voice made of:
//   tone       sine, two sines, or 2-operator FM, with two pitch envelopes
// + excitation white noise or a bank of square oscillators ("metal"),
//              through a one-pole high-pass and a 2-pole low/band/high-pass,
//              with its own decay and optional clap re-triggers
// -> resonant state-variable filter (per lane: low-pass / off / high-pass)
// -> cubic soft clipper (drive)
// -> drum bus (dry + delay send + reverb send).
// Only sounding voices are computed, and per block only the parts in use.
// Each voice is rendered in short branch-free passes over a 64-sample buffer
// (tone, excitation, filter, drive, mix): the Cortex-M4 has no branch
// predictor, so per-sample flag tests would cost more than the DSP itself.
// No libm calls, no division per sample, no allocation; trigger and render
// run in the audio ISR. Original code (MIT, like the rest of Plinky).

#include "drums.h"
#include <string.h>

// drums.c is compiled for size (-Os, see the Makefile); the per-sample passes
// are compiled for speed
#define HOT __attribute__((optimize("O2")))

#define SR 31250.f        // Plinky's audio sample rate (ACCURATE_FS)
#define VOICES 6          // drum polyphony (kicks are never stolen)
#define OUT_GAIN 18000.f  // at master volume 255 and lane level 255
#define SILENT 5e-4f      // a voice stops below this amplitude (about -66 dB)
#define PH32 4294967296.f // 2^32: phase scale of the metal oscillators
#define SUB 32            // voices are rendered in sub-blocks of 32 samples (keeps the ISR stack small)

const uint8_t drum_model_order[DM_LAST] = {
	DM_KICK, DM_KICK2, DM_SNARE, DM_CLAP, DM_CHAT, DM_OHAT, DM_CYMBAL,
	DM_TOM, DM_RIM, DM_PERC, DM_FM, DM_ZAP, DM_SAMPLE,
};

const char* const drum_model_names[DM_LAST] = {
	[DM_KICK] = "Kick", [DM_KICK2] = "Kick2", [DM_SNARE] = "Snare", [DM_CLAP] = "Clap",
	[DM_CHAT] = "C.Hat", [DM_OHAT] = "O.Hat", [DM_CYMBAL] = "Cymbal", [DM_TOM] = "Tom",
	[DM_RIM] = "Rim", [DM_PERC] = "Cowbell", [DM_FM] = "FM", [DM_ZAP] = "Zap", [DM_SAMPLE] = "Sample",
};

enum { OSC_NONE, OSC_SINE, OSC_SINE2, OSC_FM };
enum { EF_OFF, EF_LP, EF_BP, EF_HP }; // excitation filter modes
enum { EXC_NOISE, EXC_METAL };

// square-oscillator banks (Hz at tune 0)
static const float metal_hat[6] = { 205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f }; // classic analogue hat ratios
static const float metal_cym[6] = { 263.0f, 400.0f, 421.0f, 531.0f, 588.0f, 913.0f }; // wider, more inharmonic
static const float metal_bell[2] = { 540.0f, 800.0f };                                // cowbell
static const float fm_ratios[8] = { 0.5f, 1.f, 1.4142f, 1.5f, 2.f, 2.7183f, 3.5f, 5.19f };
static const uint16_t clap_gaps[4] = { 0, 290, 380, 330 }; // samples to the next burst, by bursts left

typedef struct DrumVoice {
	const DrumParams* p;           // live: level, sends, drive, filter
	// tone
	float ph, ph2, dph, dph2, mix2;
	float penv, kp, penv2, kp2;    // pitch envelopes (multipliers above the base pitch)
	float at, kt;                  // tone amplitude and per-sample decay
	float idx, ki;                 // FM index (in cycles) and its decay
	// excitation
	float an, kn, kn_tail, a0;     // amplitude, decay, decay after the clap bursts, burst level
	float k1, z1;                  // one-pole high-pass (k1 = 0: bypass)
	float ea1, ea2, ea3, ek, ez1, ez2; // 2-pole state-variable filter (coefficients, state)
	float cl, cb, ch;              // its output mix: low-pass, band-pass (normalised), high-pass
	float nmix;                    // metal: amount of white noise blended in
	float mf;                      // metal: Hz -> 32-bit phase increment (includes tune)
	const float* mtab;             // metal: oscillator frequencies
	uint32_t mph[6];
	// resonant filter state
	float s1, s2;
	float drv0;                    // built-in drive of the model
	uint32_t rng;
	uint16_t clap_timer;
	uint8_t clap_left, osc, exc, nmetal, model, lane, active;
} DrumVoice;

static DrumVoice dv[VOICES];
static uint32_t seed = 0x1234567u;

int16_t drum_bus_dry[DRUM_BLOCK];
int16_t drum_bus_dl[DRUM_BLOCK / 2];
int16_t drum_bus_rv[DRUM_BLOCK / 2];

// ---------------------------------------------------------------------------
// cheap maths (no libm)

static float fexp2(float x) { // 2^x, about 0.3 % error
	int i = (int)(x + 64.f) - 64; // floor for |x| < 64
	float f = x - (float)i;
	union { float f; int32_t i; } u;
	u.f = 1.f + f * (0.6565f + f * 0.344f);
	u.i += i << 23;
	return u.f;
}
static float kdecay(float seconds) { // per-sample multiplier that decays to 1/e in `seconds`
	return 1.f - 1.f / (seconds * SR);
}
static float kfilter(float fc) { // one-pole coefficient for cutoff fc (approximate)
	float w = 6.2831853f * fc / SR;
	return w / (1.f + w);
}
static float erange(uint8_t v, float lo, float octaves) { // 0..255 -> lo .. lo * 2^octaves
	return lo * fexp2(v * (octaves / 255.f));
}
static float tanpade(float x) { // tan(x) for 0 <= x < 1.45, Pade approximation
	float x2 = x * x;
	return x * (15.f - x2) / (15.f - 6.f * x2);
}
static inline float fabs_(float x) { return x < 0.f ? -x : x; }
static inline float psin(float ph) { // sin(2 pi ph) for ph in 0..1 (sign flipped; doesn't matter here)
	float x = ph * 2.f - 1.f;
	float y = 4.f * x * (1.f - fabs_(x));
	return y + 0.225f * y * (fabs_(y) - 1.f);
}
static inline int16_t sat16(int32_t x) {
	return (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
}

// set up the excitation filter: mode EF_LP / EF_BP / EF_HP, cutoff, Q.
// The output is a mix of the three responses, so the loop has no branches;
// EF_OFF is a 20 Hz high-pass (just removes DC).
static void exc_filter(DrumVoice* d, int mode, float fc, float q) {
	if (mode == EF_OFF) { fc = 20.f; q = 0.7f; mode = EF_HP; }
	if (fc > 13000.f) fc = 13000.f;
	float g = tanpade(3.14159265f * fc / SR);
	d->ek = 1.f / q;
	d->ea1 = 1.f / (1.f + g * (g + d->ek));
	d->ea2 = g * d->ea1;
	d->ea3 = g * d->ea2;
	d->cl = (mode == EF_LP) ? 1.f : 0.f;
	d->cb = (mode == EF_BP) ? d->ek : 0.f;
	d->ch = (mode == EF_HP) ? 1.f : 0.f;
}

// ---------------------------------------------------------------------------

void drums_stop_all(void) {
	for (int v = 0; v < VOICES; ++v)
		dv[v].active = 0;
}

static int is_kick(int model) {
	return model == DM_KICK || model == DM_KICK2;
}

// a lane retriggers its own voice; otherwise use a free voice, otherwise
// steal the quietest voice that isn't a kick
static int alloc_voice(int lane) {
	int free_v = -1;
	for (int v = 0; v < VOICES; ++v) {
		if (dv[v].active && dv[v].lane == lane)
			return v;
		if (!dv[v].active && free_v < 0)
			free_v = v;
	}
	if (free_v >= 0)
		return free_v;
	int best = -1;
	float best_amp = 1e9f;
	for (int v = 0; v < VOICES; ++v) {
		if (is_kick(dv[v].model))
			continue;
		float amp = dv[v].at + dv[v].an;
		if (amp < best_amp) {
			best_amp = amp;
			best = v;
		}
	}
	if (best < 0) // every voice is a kick: take the quietest one anyway
		for (int v = 0; v < VOICES; ++v) {
			float amp = dv[v].at + dv[v].an;
			if (best < 0 || amp < best_amp) {
				best_amp = amp;
				best = v;
			}
		}
	return best;
}

void drums_trigger(int lane, const DrumParams* p, int vel) {
	if (p->model == DM_SAMPLE || p->model >= DM_LAST)
		return;
	if (p->model == DM_CHAT) // a closed hat chokes open hats
		for (int i = 0; i < VOICES; ++i)
			if (dv[i].active && dv[i].model == DM_OHAT)
				dv[i].kn = 0.995f;

	DrumVoice* d = &dv[alloc_voice(lane)];
	memset(d, 0, sizeof(*d));
	seed = seed * 1664525u + 1013904223u;
	d->rng = seed | 1;
	d->p = p;
	d->lane = lane;
	d->model = p->model;
	exc_filter(d, EF_OFF, 0.f, 0.f); // models with noise / metal set their own below

	const float g = vel * (1.f / 7.f);
	const float t = fexp2(p->tune * (1.f / 128.f));
	const float s = p->sweep * (1.f / 255.f);
	const float n = p->snap * (1.f / 255.f);
	const float c = p->tone * (1.f / 255.f);
	const uint8_t dec = p->decay;

	switch (p->model) {
	case DM_KICK: // deep sine with a pitch drop, filtered click
		d->osc = OSC_SINE;
		d->dph = 46.f * t / SR;
		d->penv = 0.2f + s * 7.f;
		d->kp = kdecay(0.016f);
		d->at = g;
		d->kt = kdecay(erange(dec, 0.07f, 4.8f));
		d->an = g * n * 0.9f;
		d->kn = kdecay(0.0025f);
		d->k1 = kfilter(60.f);
		exc_filter(d, EF_LP, 1500.f + c * 6000.f, 0.7f);
		break;
	case DM_KICK2: // punchy: very fast pitch snap plus a slower body sweep, bright tick, built-in saturation
		d->osc = OSC_SINE;
		d->dph = 54.f * t / SR;
		d->penv = s * 9.f;
		d->kp = kdecay(0.005f);
		d->penv2 = 0.3f + c * 1.7f;
		d->kp2 = kdecay(0.04f);
		d->at = g;
		d->kt = kdecay(erange(dec, 0.05f, 4.3f));
		d->an = g * n * 0.8f;
		d->kn = kdecay(0.0012f);
		d->k1 = kfilter(1800.f);
		d->drv0 = 0.7f;
		break;
	case DM_SNARE: // two drum-head modes plus band-passed noise (the wires)
		d->osc = OSC_SINE2;
		d->dph = 180.f * t / SR;
		d->dph2 = d->dph * 1.59f;
		d->mix2 = 0.35f;
		d->penv = 0.15f + s * 1.5f;
		d->kp = kdecay(0.01f);
		d->at = g * (1.f - n * 0.6f);
		d->kt = kdecay(erange(dec, 0.03f, 2.5f));
		d->an = g * (0.4f + n * 1.0f);
		d->kn = kdecay(erange(dec, 0.06f, 3.f));
		exc_filter(d, EF_BP, 1500.f + c * 3500.f, 0.55f);
		break;
	case DM_CLAP: // four quick band-passed noise bursts and a tail
		d->a0 = d->an = g * 1.8f;
		d->kn = kdecay(0.006f);
		d->kn_tail = kdecay(erange(dec, 0.07f, 3.8f));
		d->clap_left = 3;
		d->clap_timer = clap_gaps[3];
		exc_filter(d, EF_BP, 1000.f + c * 1600.f, 1.4f);
		break;
	case DM_CHAT:
	case DM_OHAT:
	case DM_CYMBAL: // six detuned squares through a steep high-pass, with some noise
		d->exc = EXC_METAL;
		d->nmetal = 6;
		d->mf = t * PH32 / SR;
		if (p->model == DM_CYMBAL) {
			d->mtab = metal_cym;
			d->nmix = 0.1f + n * 0.6f;
			d->k1 = kfilter(2000.f);
			exc_filter(d, EF_HP, 4000.f + c * 4000.f, 0.7f);
			d->an = g * 2.2f;
			d->kn = kdecay(erange(dec, 0.3f, 3.3f));
		}
		else {
			d->mtab = metal_hat;
			d->nmix = n;
			d->k1 = kfilter(3000.f);
			exc_filter(d, EF_HP, 5000.f + c * 3500.f, 0.9f);
			d->an = g * 2.6f;
			d->kn = (p->model == DM_CHAT) ? kdecay(erange(dec, 0.012f, 3.5f)) : kdecay(erange(dec, 0.08f, 3.4f));
		}
		break;
	case DM_TOM: // pitch-dropping sine, optional fifth overtone (colour), stick noise
		d->osc = OSC_SINE2;
		d->dph = 100.f * t / SR;
		d->dph2 = d->dph * 1.5f;
		d->mix2 = c * 0.45f;
		d->penv = 0.1f + s * 2.5f;
		d->kp = kdecay(0.04f);
		d->at = g;
		d->kt = kdecay(erange(dec, 0.06f, 4.3f));
		d->an = g * n * 0.6f;
		d->kn = kdecay(0.004f);
		exc_filter(d, EF_BP, 1500.f, 0.7f);
		break;
	case DM_RIM: // two short high sines (colour: rimshot -> clave) and a click
		d->osc = OSC_SINE2;
		d->dph = 1650.f * t / SR;
		d->dph2 = d->dph * (1.25f + c);
		d->mix2 = 0.45f;
		d->penv = s * 0.5f;
		d->kp = kdecay(0.004f);
		d->at = g;
		d->kt = kdecay(erange(dec, 0.005f, 3.2f));
		d->an = g * (0.2f + n * 0.6f);
		d->kn = kdecay(0.0015f);
		exc_filter(d, EF_HP, 2500.f, 0.7f);
		break;
	case DM_PERC: // cowbell: two squares through a band-pass
		d->exc = EXC_METAL;
		d->nmetal = 2;
		d->mtab = metal_bell;
		d->mf = t * PH32 / SR;
		d->nmix = n * 0.3f;
		exc_filter(d, EF_BP, 800.f + c * 1600.f, 0.9f);
		d->an = g * 1.1f;
		d->kn = kdecay(erange(dec, 0.035f, 3.8f));
		break;
	case DM_FM: // two-operator FM: colour = ratio (bell, metal, wood), snap = index
		d->osc = OSC_FM;
		d->dph = 190.f * t / SR;
		d->dph2 = d->dph * fm_ratios[(int)(c * 7.99f)];
		d->idx = 0.05f + n * 1.4f;
		d->ki = kdecay(erange(dec, 0.015f, 4.f) * 0.7f);
		d->penv = s * 2.f;
		d->kp = kdecay(0.03f);
		d->at = g;
		d->kt = kdecay(erange(dec, 0.04f, 5.f));
		break;
	case DM_ZAP: // huge fast pitch sweep (colour = sweep time)
		d->osc = OSC_SINE;
		d->dph = 55.f * t / SR;
		d->penv = 3.f + s * 55.f;
		d->kp = kdecay(0.003f + c * 0.07f);
		d->at = g;
		d->kt = kdecay(erange(dec, 0.02f, 4.5f));
		d->an = g * n * 0.3f;
		d->kn = kdecay(0.003f);
		exc_filter(d, EF_HP, 1000.f, 0.7f);
		break;
	}
	d->active = 1;
}

// ---------------------------------------------------------------------------
// rendering passes (each loop is branch-free)

HOT static void pass_sine(DrumVoice* d, float* buf, int n) {
	float ph = d->ph, penv = d->penv, penv2 = d->penv2, at = d->at;
	const float dph = d->dph, kp = d->kp, kp2 = d->kp2, kt = d->kt;
	for (int i = 0; i < n; ++i) {
		ph += dph * (1.f + penv + penv2);
		ph -= (float)(ph >= 1.f);
		buf[i] = psin(ph) * at;
		at *= kt;
		penv *= kp;
		penv2 *= kp2;
	}
	d->ph = ph; d->penv = penv; d->penv2 = penv2; d->at = at;
}

HOT static void pass_sine2(DrumVoice* d, float* buf, int n) {
	float ph = d->ph, ph2 = d->ph2, penv = d->penv, at = d->at;
	const float dph = d->dph, dph2 = d->dph2, mix2 = d->mix2, kp = d->kp, kt = d->kt;
	for (int i = 0; i < n; ++i) {
		float pm = 1.f + penv;
		ph += dph * pm;
		ph -= (float)(ph >= 1.f);
		ph2 += dph2 * pm;
		ph2 -= (float)(ph2 >= 1.f);
		float a = psin(ph);
		buf[i] = (a + (psin(ph2) - a) * mix2) * at;
		at *= kt;
		penv *= kp;
	}
	d->ph = ph; d->ph2 = ph2; d->penv = penv; d->at = at;
}

HOT static void pass_fm(DrumVoice* d, float* buf, int n) {
	float ph = d->ph, ph2 = d->ph2, penv = d->penv, at = d->at, idx = d->idx;
	const float dph = d->dph, dph2 = d->dph2, kp = d->kp, kt = d->kt, ki = d->ki;
	for (int i = 0; i < n; ++i) {
		float pm = 1.f + penv;
		ph += dph * pm;
		ph -= (float)(ph >= 1.f);
		ph2 += dph2 * pm;
		ph2 -= (float)(ph2 >= 1.f);
		float q = ph + idx * psin(ph2) + 2.f; // +2 keeps it positive for the truncation
		q -= (float)(int)q;
		buf[i] = psin(q) * at;
		at *= kt;
		penv *= kp;
		idx *= ki;
	}
	d->ph = ph; d->ph2 = ph2; d->penv = penv; d->at = at; d->idx = idx;
}

// noise or metal -> one-pole high-pass -> 2-pole filter -> amplitude, added to buf
#define EXC_FILTER_AND_ADD(xin) do { \
		float x_ = (xin); \
		z1 += k1 * (x_ - z1); \
		x_ -= z1; \
		float v3 = x_ - s2; \
		float v1 = a1 * s1 + a2 * v3; \
		float v2 = s2 + a2 * s1 + a3 * v3; \
		s1 = 2.f * v1 - s1; \
		s2 = 2.f * v2 - s2; \
		buf[i] += (cl * v2 + cb * v1 + ch * (x_ - k * v1 - v2)) * an; \
		an *= kn; \
	} while (0)

HOT static void pass_excitation(DrumVoice* d, float* buf, int n) {
	uint32_t rng = d->rng;
	float z1 = d->z1, s1 = d->ez1, s2 = d->ez2, an = d->an;
	const float k1 = d->k1, a1 = d->ea1, a2 = d->ea2, a3 = d->ea3, k = d->ek, cl = d->cl, cb = d->cb, ch = d->ch, kn = d->kn;
	if (d->exc == EXC_METAL) {
		const int nm = d->nmetal;
		uint32_t mph[6], minc[6];
		for (int j = 0; j < nm; ++j) {
			mph[j] = d->mph[j];
			minc[j] = (uint32_t)(d->mf * d->mtab[j]);
		}
		const float scale = 1.f / nm, nmix = d->nmix;
		for (int i = 0; i < n; ++i) {
			int sgn = 0;
			for (int j = 0; j < nm; ++j) {
				mph[j] += minc[j];
				sgn += mph[j] >> 31;
			}
			rng = rng * 1664525u + 1013904223u;
			float m = (float)(nm - 2 * sgn) * scale;
			EXC_FILTER_AND_ADD(m + ((float)(int32_t)rng * (1.f / 2147483648.f) - m) * nmix);
		}
		for (int j = 0; j < nm; ++j)
			d->mph[j] = mph[j];
	}
	else {
		for (int i = 0; i < n; ++i) {
			rng = rng * 1664525u + 1013904223u;
			EXC_FILTER_AND_ADD((float)(int32_t)rng * (1.f / 2147483648.f));
		}
	}
	d->rng = rng;
	d->z1 = z1; d->ez1 = s1; d->ez2 = s2; d->an = an;
}

HOT void drums_render(int master) {
	memset(drum_bus_dry, 0, sizeof(drum_bus_dry));
	memset(drum_bus_dl, 0, sizeof(drum_bus_dl));
	memset(drum_bus_rv, 0, sizeof(drum_bus_rv));
	const float mg = master * (OUT_GAIN / (255.f * 255.f));
	float buf[SUB];
	for (int v = 0; v < VOICES; ++v) {
		DrumVoice* d = &dv[v];
		if (!d->active)
			continue;
		const DrumParams* p = d->p;

		// clap bursts, scheduled on the block grid (2 ms)
		if (d->clap_left) {
			d->clap_timer = d->clap_timer > DRUM_BLOCK ? d->clap_timer - DRUM_BLOCK : 0;
			if (!d->clap_timer) {
				d->an = d->a0;
				--d->clap_left;
				d->clap_timer = clap_gaps[d->clap_left];
				if (!d->clap_left)
					d->kn = d->kn_tail;
			}
		}

		// per block: which parts run, and the live filter / drive / level
		const int use_tone = d->osc != OSC_NONE && d->at > 1e-5f;
		const int use_exc = d->an > 1e-5f;
		if (!use_tone) d->at = 0.f;
		if (!use_exc) d->an = 0.f;
		int fmode = 0; // resonant filter: 0 off, 1 low-pass, 2 high-pass
		float a1 = 0.f, a2 = 0.f, a3 = 0.f, k = 2.f;
		if (p->filter < DRUM_FILTER_OFF - 4 || p->filter > DRUM_FILTER_OFF + 4) {
			fmode = p->filter < DRUM_FILTER_OFF ? 1 : 2;
			float fc = (fmode == 1) ? 18000.f * fexp2((p->filter - DRUM_FILTER_OFF) * (9.f / 128.f)) // 35 Hz .. open
			                        : 20.f * fexp2((p->filter - DRUM_FILTER_OFF) * (9.f / 127.f));   // 20 Hz .. 10 kHz
			if (fc > 13000.f) fc = 13000.f;
			float gc = tanpade(3.14159265f * fc / SR);
			k = 2.f - p->reso * (1.95f / 255.f);
			a1 = 1.f / (1.f + gc * (gc + k));
			a2 = gc * a1;
			a3 = gc * a2;
		}
		const float cl = (fmode == 1) ? 1.f : 0.f, ch = 1.f - cl;
		const float drv = d->drv0 + p->drive * (4.f / 255.f), dg = 1.f + drv;
		const float gd = p->level * mg;
		const float gdl = gd * p->dlsend * (0.5f / 255.f), grv = gd * p->rvsend * (0.5f / 255.f);

		for (int h = 0; h < DRUM_BLOCK; h += SUB) {
			// 1) tone
			if (use_tone) {
				if (d->osc == OSC_SINE) pass_sine(d, buf, SUB);
				else if (d->osc == OSC_SINE2) pass_sine2(d, buf, SUB);
				else pass_fm(d, buf, SUB);
			}
			else
				memset(buf, 0, sizeof(buf));
			// 2) noise / metal
			if (use_exc)
				pass_excitation(d, buf, SUB);
			// 3) resonant filter
			if (fmode) {
				float s1 = d->s1, s2 = d->s2;
				for (int i = 0; i < SUB; ++i) {
					float x = buf[i];
					float v3 = x - s2;
					float v1 = a1 * s1 + a2 * v3;
					float v2 = s2 + a2 * s1 + a3 * v3;
					s1 = 2.f * v1 - s1;
					s2 = 2.f * v2 - s2;
					buf[i] = cl * v2 + ch * (x - k * v1 - v2);
				}
				d->s1 = s1; d->s2 = s2;
			}
			// 4) drive: cubic soft clip
			if (drv > 0.001f)
				for (int i = 0; i < SUB; ++i) {
					float x = buf[i] * dg;
					x = x > 1.5f ? 1.5f : x < -1.5f ? -1.5f : x;
					buf[i] = x - 0.148148f * x * x * x;
				}
			// 5) mix into the drum bus
			int16_t* dry = drum_bus_dry + h;
			for (int i = 0; i < SUB; ++i)
				dry[i] = sat16(dry[i] + (int32_t)(buf[i] * gd));
			if (gdl > 0.f || grv > 0.f) {
				int16_t* dl = drum_bus_dl + h / 2;
				int16_t* rv = drum_bus_rv + h / 2;
				for (int i = 0; i < SUB / 2; ++i) {
					float x = buf[i * 2] + buf[i * 2 + 1];
					dl[i] = sat16(dl[i] + (int32_t)(x * gdl));
					rv[i] = sat16(rv[i] + (int32_t)(x * grv));
				}
			}
		}

		if (d->at < SILENT && d->an < SILENT && !d->clap_left)
			d->active = 0;
	}
}
