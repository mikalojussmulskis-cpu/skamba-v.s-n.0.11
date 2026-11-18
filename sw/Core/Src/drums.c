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
// run in the audio ISR. Original code (MIT, like the rest of Plinky), except the LXR models
// (DM_LXR_*): ports of the LXR-02 voice algorithms by Erica Synths, GPL-3.0-only (sw/lxr_license/NOTICE.md),
// which makes a firmware built with them GPL-3.0 as a whole.

#include "drums.h"
#include <string.h>

// drums.c is compiled for size (-Os, see the Makefile); the per-sample passes
// are compiled for speed
#define HOT __attribute__((optimize("O2")))

#define SR 31250.f        // Plinky's audio sample rate (ACCURATE_FS)
#define VOICES 6          // drum polyphony (kicks are never stolen)
#define OUT_GAIN 48000.f  // at master volume 255 and lane level 255 (the bus holds half of it, the output stage x3 with a soft knee)
#define BUS_SCALE 0.5f    // voice -> bus: 6 dB headroom in the 16-bit bus before the output soft clip
#define SILENT 5e-4f      // a voice stops below this amplitude (about -66 dB)
#define PH32 4294967296.f // 2^32: phase scale of the metal oscillators
#define SUB 32            // voices are rendered in sub-blocks of 32 samples (keeps the ISR stack small)

// follows the default kit (kick, snare, clap, hats, rim, bass, acid), then the colour models
const uint8_t drum_model_order[DM_LAST] = {
	DM_KICK, DM_KICK2, DM_SNARE, DM_CLAP, DM_CHAT, DM_OHAT, DM_CYMBAL, DM_RIM, DM_PERC, DM_TOM,
	DM_BASS, DM_ACID, DM_FM, DM_CLANG, DM_ZAP, DM_CHIRP, DM_FOLD, DM_GLITCH, DM_CRUSH, DM_LXR_DRUM, DM_LXR_SNARE, DM_LXR_HAT, DM_LXR_CYMBAL, DM_SAMPLE,
};

const char* const drum_model_names[DM_LAST] = {
	[DM_KICK] = "Kick", [DM_KICK2] = "Kick2", [DM_SNARE] = "Snare", [DM_CLAP] = "Clap",
	[DM_CHAT] = "C.Hat", [DM_OHAT] = "O.Hat", [DM_CYMBAL] = "Cymbal", [DM_TOM] = "Tom",
	[DM_RIM] = "Rim", [DM_PERC] = "Cowbell", [DM_FM] = "FM", [DM_ZAP] = "Zap", [DM_SAMPLE] = "Sample",
	[DM_GLITCH] = "Glitch", [DM_FOLD] = "Fold", [DM_CLANG] = "Clang", [DM_CHIRP] = "Chirp", [DM_CRUSH] = "Crush",
	[DM_BASS] = "Bass", [DM_ACID] = "Acid", [DM_LXR_DRUM] = "LXR Drum", [DM_LXR_SNARE] = "LXR Snare",
	[DM_LXR_HAT] = "LXR Hat", [DM_LXR_CYMBAL] = "LXR Cymbal",
};

enum { OSC_NONE, OSC_SINE, OSC_SINE2, OSC_FM, OSC_FOLD, OSC_ACID, OSC_LXR };
enum { EF_OFF, EF_LP, EF_BP, EF_HP }; // excitation filter modes
enum { EXC_NOISE, EXC_METAL };

// square-oscillator banks (Hz at tune 0)
static const float metal_hat[6] = { 205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f }; // classic analogue hat ratios
static const float metal_cym[6] = { 263.0f, 400.0f, 421.0f, 531.0f, 588.0f, 913.0f }; // wider, more inharmonic
static const float metal_bell[2] = { 540.0f, 800.0f };                                // cowbell
static const float fm_ratios[8] = { 0.5f, 1.f, 1.4142f, 1.5f, 2.f, 2.7183f, 3.5f, 5.19f };
static const float clang_ratios[4] = { 1.4142f, 2.7183f, 3.873f, 7.13f }; // inharmonic: metal
static const uint16_t clap_gaps[4] = { 0, 290, 380, 330 }; // samples to the next burst, by bursts left

typedef struct DrumVoice {
	const DrumParams* p;           // live: level, sends, drive, filter
	// tone
	float ph, ph2, dph, dph2, mix2;
	float penv, kp, penv2, kp2;    // pitch envelopes (multipliers above the base pitch)
	float at, kt;                  // tone amplitude and per-sample decay
	float idx, ki;                 // FM index (in cycles) and its decay
	// excitation
	float an, kn, kn_tail, a0;     // amplitude, decay, decay after the clap bursts, burst level (DM_ACID: full level of the attack)
	const float* mtab;             // metal: oscillator frequencies
	union {
		struct { // noise / metal excitation
			float k1, z1;                  // one-pole high-pass (k1 = 0: bypass)
			float ea1, ea2, ea3, ek, ez1, ez2; // 2-pole state-variable filter (coefficients, state)
			float nmix;                    // metal: amount of white noise blended in
			float mf;                      // metal: Hz -> 32-bit phase increment (includes tune)
			uint32_t mph[6];
		};
		struct { // DM_ACID has no excitation: the same bytes hold its ladder filter, oscillator and glide
			float y1, y2, y3, y4;          // ladder stages
			float sq1, sq2;                // DPW saw oscillators: previous squared ramp values
			float dph_t, gk;               // glide: target phase increment, per-sample coefficient
			float env, kenv;               // filter envelope (1 -> 0; above 1 on accents) and its per-block decay
			float g;                       // ladder coefficient at the end of the last block
		};
		struct { // LXR models (lxr_* below) have no excitation either: their envelope shapes and amounts
			// (no bigger than the excitation struct: the LXR models cost no RAM; envelope slopes are constants)
			float pamt, snamt, fmamt;      // pitch EG amount, snap amount, FM amount (the LXR's fmModAmount)
			float sn;                      // snap EG value (1 -> 0)
			float ga, vg;                  // amp EG gain at the end of the last sub-block, velocity gain
			float ns1, ns2, na1, na2, na3, nk; // the voice's own 2-pole high-pass (snare: on the noise; hat / cymbal: on all)
			union {
				struct {                   // snare: noise oscillator, a new random value each time its phase wraps
					uint32_t nph, ninc;
					float nhold, lnmix;        // ... its current value, and the noise / tone mix
				};
				struct {                   // hat / cymbal: second modulator (phase, increment, amount)
					float ph3, dph3, fm2;
				};
			};
		};
	};
	// resonant filter state
	float s1, s2;
	float drv0;                    // built-in drive of the model
	float srr_acc, srr_hold;       // sample rate reduction state
	float fold, rm;                // OSC_FOLD: wavefolder gain, ring modulation depth
	float crush, srr0;             // built-in bit crush (levels, 0 = off) and maximum rate
	float fenv, fenv_k;            // filter envelope (1 -> 0) and its per-block decay
	float last, dc;                // last output sample; declick tail of the hit this voice replaced
	float kt_rel;                  // tone decay after the hold (DM_BASS)
	uint16_t hold;                 // blocks the tone holds its level before decaying
	uint32_t rng;
	uint16_t clap_timer;
	uint8_t clap_left, osc, exc, nmetal, emode, model, lane, active;
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
static float flog2(float x) { // log2(x) for x > 0, about 0.5 % error
	union { float f; int32_t i; } u = { x };
	int e = ((u.i >> 23) & 255) - 127;
	u.i = (u.i & 0x7fffff) | 0x3f800000; // mantissa in 1..2
	float m = u.f - 1.f;
	return e + m * (1.4427f - m * (0.7213f - m * 0.3f));
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
static inline float clampf_(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static inline int clampi_(int x, int a, int b) { return x < a ? a : x > b ? b : x; }
static inline float psin(float ph) { // sin(2 pi ph) for ph in 0..1 (sign flipped; doesn't matter here)
	float x = ph * 2.f - 1.f;
	float y = 4.f * x * (1.f - fabs_(x));
	return y + 0.225f * y * (fabs_(y) - 1.f);
}
static inline int16_t sat16(int32_t x) {
	return (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
}

// LXR-02 envelope maths (port of SlopeEg2.c / Decay.c / snapEg.c, GPL-3.0-only). The LXR steps its
// envelopes once per block of 64 samples at 44.14 kHz; here they step once per sub-block (SUB samples)
// with the step scaled to the same time in seconds.
#define LXR_BLK ((44140.f / 64.f) / (SR / SUB))
static float lxr_curve(float v, float k) { return (1.f + k) * v / (1.f + k * fabs_(v)); }
// step per sub-block for a time setting x = 0..1 (the LXR's value / 127); K = 1998 amp EG, 198 pitch EG
static float lxr_step(float x, float K) {
	float r = 2.f * (1.f - lxr_curve(x, K));
	return (r > 1.f ? 1.f : r) * LXR_BLK;
}
// slope setting (the LXR's 0..127, 64 = linear) -> k of lxr_curve (constant expression)
#define LXR_SLOPE(v, scale) (2.f * (((v) / 127.f - 0.5f) * (scale)) / (1.f - (((v) / 127.f - 0.5f) * (scale))))
#define LXR_KA LXR_SLOPE(40.f, 1.999f)     // amp EG slope (hat: LXR_KA_HAT)
#define LXR_KA_HAT LXR_SLOPE(30.f, 1.999f)
#define LXR_KPE LXR_SLOPE(20.f, 2.f)       // pitch EG slope
#define LXR_KSN (0.2f * LXR_BLK)           // snap EG step per sub-block (snap time 1, about 7 ms)

// 2-pole state-variable filter coefficients (the same filter as exc_filter / the lane filter)
static void svf_coefs(float fc, float q, float* a1, float* a2, float* a3, float* k) {
	if (fc > 13000.f) fc = 13000.f;
	float g = tanpade(3.14159265f * fc / SR);
	*k = 1.f / q;
	*a1 = 1.f / (1.f + g * (g + *k));
	*a2 = g * *a1;
	*a3 = g * *a2;
}

// Bass / Acid notes: exact equal temperament from C2 (Plinky's scales are built on C), tune in
// 1/5 semitones (whole semitones once snapped to the scale; a remainder uses the fast fexp2)
#define NOTE_C2 65.4064f
static float note_hz(int tune) {
	static const float semi[12] = { 1.f, 1.0594631f, 1.1224620f, 1.1892071f, 1.2599210f, 1.3348399f,
		1.4142136f, 1.4983071f, 1.5874011f, 1.6817928f, 1.7817974f, 1.8877486f };
	int st = (tune + 600) / 5 - 120, r = tune - st * 5; // floor division
	int oct = (st + 120) / 12 - 10, i = st - oct * 12;
	float hz = NOTE_C2 * semi[i] * (oct >= 0 ? (float)(1 << oct) : 1.f / (float)(1 << -oct));
	return r ? hz * fexp2(r * (1.f / 60.f)) : hz;
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
	d->emode = mode;
}

// ---------------------------------------------------------------------------

static float sub_s[2]; // 25 Hz high-pass on the dry bus (DC and sub rumble), see drums_render
static float shelf_lp;  // high shelf of the drum output
static float bus_srr_acc[2], bus_srr_hold[3]; // global sample rate reduction
void drums_stop_all(void) {
	for (int v = 0; v < VOICES; ++v)
		dv[v].active = 0;
	sub_s[0] = sub_s[1] = 0.f;
	shelf_lp = 0.f;
	bus_srr_acc[0] = bus_srr_acc[1] = 0.f;
	bus_srr_hold[0] = bus_srr_hold[1] = bus_srr_hold[2] = 0.f;
}

static int is_kick(int model) {
	return model == DM_KICK || model == DM_KICK2;
}

#define SHED_K 0.98413f   // kdecay(0.002): a shed voice fades by 1/e every 2 ms
#define SHED_LXR 0.25f    // LXR models: linear amp EG step per sub-block (gone in 4 ms)
void drums_shed(void) {
	int best = -1, n = 0;
	float best_amp = 1e9f;
	for (int v = 0; v < VOICES; ++v) {
		DrumVoice* d = &dv[v];
		const int lxr = d->osc == OSC_LXR;
		if (!d->active || d->kt == (lxr ? SHED_LXR : SHED_K)) // silent or already fading
			continue;
		++n;
		const int m = d->model;
		if (is_kick(m) || m == DM_LXR_DRUM || m == DM_BASS || m == DM_ACID) // the low end and notes stay
			continue;
		float amp = ((lxr ? d->ga * d->vg : d->at) + d->an) * d->p->level;
		if (amp < best_amp) {
			best_amp = amp;
			best = v;
		}
	}
	if (n < 2 || best < 0)
		return;
	DrumVoice* d = &dv[best];
	d->hold = 0;      // held notes (Bass, Acid) stop holding
	d->clap_left = 0;
	if (d->osc == OSC_LXR)
		d->kt = SHED_LXR;
	else {
		d->kt = d->kn = SHED_K;
		if (d->a0 > d->at) d->a0 = d->at; // Bass / Acid attack: stop growing
	}
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
	drums_note(lane, p, vel, 0, 0);
}

// DM_ACID: cutoff in Hz from the lane filter byte (60 Hz .. 5.8 kHz), slide time in seconds from sweep
static float acid_cutoff(uint8_t filter) { return 60.f * fexp2(filter * (6.6f / 255.f)); }
static float acid_env0(const DrumParams* p, int vel) { // filter envelope start: accents (full velocity) hit harder
	const float n = p->snap * (1.f / 255.f);
	return vel >= 7 ? 1.f + n * 0.6f : 1.f - n * 0.45f;
}

void drums_note(int lane, const DrumParams* p, int vel, int gate, int legato) {
	if (p->model == DM_SAMPLE || p->model >= DM_LAST)
		return;
	if (p->model == DM_CHAT) // a closed hat chokes open hats
		for (int i = 0; i < VOICES; ++i)
			if (dv[i].active && dv[i].model == DM_OHAT)
				dv[i].kn = 0.995f;
	if (!gate)
		gate = (int)(0.2f * SR / DRUM_BLOCK);
	if (p->model == DM_ACID && legato) // slide: the held note glides to the new pitch
		for (int i = 0; i < VOICES; ++i) {
			DrumVoice* d = &dv[i];
			if (d->active && d->lane == lane && d->model == DM_ACID && d->hold) {
				d->dph_t = note_hz(p->tune) / SR;
				d->hold = gate;
				float e0 = acid_env0(p, vel);
				if (e0 > d->env) d->env = e0; // an accented slide pushes the filter again
				return;
			}
		}

	DrumVoice* d = &dv[alloc_voice(lane)];
	const float last = d->active ? d->last : 0.f; // declick: where the replaced hit left off
	memset(d, 0, sizeof(*d));
	d->dc = last;
	seed = seed * 1664525u + 1013904223u;
	d->rng = seed | 1;
	d->p = p;
	d->lane = lane;
	d->model = p->model;
	d->srr0 = 1.f;
	exc_filter(d, EF_OFF, 0.f, 0.f); // models with noise / metal set their own below

	// velocity 1..7 -> gain: wide range, row 5 of the drum pads is barely audible; the
	// middle (a normal tap records 4..5) sits a little higher, full velocity stays 1
	static const float vel_gain[8] = { 0.f, 0.012f, 0.035f, 0.1f, 0.38f, 0.6f, 0.8f, 1.f };
	const float g = vel_gain[vel & 7];
	const float t = fexp2(p->tune * (1.f / 60.f)); // 5 units per semitone
	const float s = p->sweep * (1.f / 255.f);
	const float n = p->snap * (1.f / 255.f);
	const float c = p->tone * (1.f / 255.f);
	const uint8_t dec = p->decay;

	switch (p->model) {
	case DM_KICK: // deep sine with a pitch drop, filtered click
		d->osc = OSC_SINE;
		d->dph = 46.f * t / SR;
		d->penv = 0.2f + s * 7.f;
		d->kp = kdecay(0.01f + s * 0.02f); // pitch drop 10 .. 30 ms: a deeper sweep also lasts longer
		d->at = g;
		d->kt = kdecay(erange(dec, 0.025f, 6.5f));
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
		d->kt = kdecay(erange(dec, 0.02f, 5.8f));
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
		d->at = g * (1.f - n * 0.5f);
		d->kt = kdecay(erange(dec, 0.012f, 4.f));
		d->an = g * (0.25f + n * 0.7f);
		d->kn = kdecay(erange(dec, 0.02f, 4.6f));
		exc_filter(d, EF_BP, 1500.f + c * 3500.f, 0.55f);
		break;
	case DM_CLAP: // four quick band-passed noise bursts and a tail
		d->a0 = d->an = g * 1.8f;
		d->kn = kdecay(0.006f);
		d->kn_tail = kdecay(erange(dec, 0.025f, 5.4f));
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
			d->kn = kdecay(erange(dec, 0.1f, 5.f));
		}
		else {
			d->mtab = metal_hat;
			d->nmix = n;
			d->k1 = kfilter(3000.f);
			exc_filter(d, EF_HP, 5000.f + c * 3500.f, 1.3f); // a little peak at the cutoff: the "ting"
			d->an = g * 2.6f;
			d->kn = (p->model == DM_CHAT) ? kdecay(erange(dec, 0.004f, 5.3f)) : kdecay(erange(dec, 0.03f, 5.f));
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
		d->kt = kdecay(erange(dec, 0.02f, 6.f));
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
		d->kt = kdecay(erange(dec, 0.002f, 5.f));
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
		d->kn = kdecay(erange(dec, 0.012f, 5.5f));
		break;
	case DM_FM: // two-operator FM: colour = ratio (bell, metal, wood), snap = index
		d->osc = OSC_FM;
		d->dph = 190.f * t / SR;
		d->dph2 = d->dph * fm_ratios[(int)(c * 7.99f)];
		d->idx = 0.03f + n * (0.3f + n * 0.5f);        // gentle at low snap (was up to 1.45 cycles: harsh)
		d->ki = kdecay(erange(dec, 0.006f, 5.4f) * 0.5f); // the brightness fades faster than the tone
		d->penv = s * 2.f;
		d->kp = kdecay(0.03f);
		d->at = g;
		d->kt = kdecay(erange(dec, 0.015f, 6.5f));
		break;
	case DM_GLITCH: { // random per hit: resonant noise band, blip pitch, lo-fi stutter (colour = randomness)
		seed = seed * 1664525u + 1013904223u;
		const float r1 = (seed >> 8) * (1.f / 16777216.f);
		seed = seed * 1664525u + 1013904223u;
		const float r2 = (seed >> 8) * (1.f / 16777216.f);
		d->osc = OSC_SINE;
		d->dph = (150.f + r1 * 1500.f) * t / SR;
		d->penv = clampf_((r2 - 0.5f) * s * 8.f, -0.9f, 8.f);
		d->kp = kdecay(0.01f);
		d->at = g * (0.7f - n * 0.4f);
		d->kt = kdecay(erange(dec, 0.004f, 5.f));
		d->an = g * (0.4f + n);
		d->kn = kdecay(erange(dec, 0.004f, 5.f));
		exc_filter(d, EF_BP, (300.f + r2 * 7000.f) * (0.5f + c), 4.f);
		d->srr0 = fexp2(-r1 * c * 5.f);
		break; }
	case DM_FOLD: // pitch-dropping sine through a wavefolder (colour), ring modulated (snap)
		d->osc = OSC_FOLD;
		d->dph = 55.f * t / SR;
		d->dph2 = d->dph * 1.5f;
		d->penv = 0.2f + s * 6.f;
		d->kp = kdecay(0.02f);
		d->rm = n;
		d->fold = 0.25f + c * 1.5f;
		d->at = g;
		d->kt = kdecay(erange(dec, 0.02f, 6.5f));
		break;
	case DM_CLANG: // metallic FM (inharmonic ratios, colour) with a metal layer
		d->osc = OSC_FM;
		d->dph = 320.f * t / SR;
		d->dph2 = d->dph * clang_ratios[(int)(c * 3.99f)];
		d->idx = 0.2f + n * (0.4f + n * 0.5f);
		d->ki = kdecay(erange(dec, 0.01f, 5.f) * 0.6f);
		d->penv = s * 1.5f;
		d->kp = kdecay(0.01f);
		d->at = g * 0.8f;
		d->kt = kdecay(erange(dec, 0.03f, 6.5f));
		d->exc = EXC_METAL;
		d->nmetal = 6;
		d->mtab = metal_cym;
		d->mf = t * 1.3f * PH32 / SR;
		d->nmix = 0.15f;
		d->an = g * 0.5f;
		d->kn = kdecay(erange(dec, 0.01f, 5.f));
		exc_filter(d, EF_HP, 3000.f, 0.7f);
		break;
	case DM_CHIRP: // laser sweeping up (colour = sweep time), FM buzz (snap)
		d->osc = OSC_FM;
		d->dph = 900.f * t / SR;
		d->dph2 = d->dph * 2.f;
		d->idx = n * 0.8f;
		d->ki = kdecay(0.08f);
		d->penv = -0.92f * (0.3f + 0.7f * s);
		d->kp = kdecay(0.004f + c * 0.12f);
		d->at = g;
		d->kt = kdecay(erange(dec, 0.02f, 6.f));
		break;
	case DM_CRUSH: // folded ring-mod tone and noise, bit crushed (colour) and rate reduced
		d->osc = OSC_FOLD;
		d->dph = 120.f * t / SR;
		d->dph2 = d->dph * 2.01f;
		d->penv = s * 4.f;
		d->kp = kdecay(0.01f);
		d->rm = 0.5f;
		d->fold = 1.2f;
		d->at = g * 0.8f;
		d->kt = kdecay(erange(dec, 0.01f, 5.5f));
		d->an = g * n;
		d->kn = kdecay(erange(dec, 0.01f, 5.5f));
		exc_filter(d, EF_BP, 2000.f, 1.f);
		d->crush = 2.f + (1.f - c) * 30.f;
		d->srr0 = fexp2(-(1.5f + c * 2.5f));
		break;
	case DM_BASS: // sustained note: wavefolded sine (colour = harmonics) held for the decay time, punch (sweep), click (snap)
		d->osc = OSC_FOLD;
		d->dph = note_hz(p->tune) / SR;
		d->dph2 = d->dph;
		d->fold = 0.25f + c * 1.25f;
		d->penv = s * 2.f;
		d->kp = kdecay(0.008f);
		d->a0 = g;                   // 3 ms attack up to g (soft start, no click) ...
		d->at = g * 0.02f;
		d->kt = 1.0425f;
		d->kt_rel = kdecay(0.02f);   // ... holds, then a 20 ms release
		d->hold = (uint16_t)(erange(dec, 0.03f, 5.5f) * (SR / DRUM_BLOCK)); // 30 ms .. 1.4 s
		d->an = g * n * 0.5f;
		d->kn = kdecay(0.002f);
		exc_filter(d, EF_LP, 2500.f, 0.7f); // Snap: a soft thump rather than a click
		break;
	case DM_ACID: // 303 style: DPW saw -> square (colour), 4-pole ladder with envelope (decay, depth = Filter Env),
		// accent (snap), glide between slid steps (sweep = slide time); held for the gate, 12 ms release
		d->osc = OSC_ACID;
		d->dph = d->dph_t = note_hz(p->tune) / SR;
		d->gk = 1.f / (0.005f * fexp2(s * 6.3f) * SR); // slide 5 .. 400 ms
		d->y3 = d->y4 = d->sq2 = 0.f;                  // (exc_filter above wrote these bytes as the noise filter)
		d->sq1 = 1.f;                                  // ramp starts at -1 (squared: 1), the shifted one at 0
		d->a0 = g * 1.5f;                              // the ladder takes the top off the saw: make up for it
		d->at = g * 0.05f;
		d->kt = 1.1f;                                  // 1 ms attack: grows until it reaches a0, then holds ...
		d->kt_rel = kdecay(0.012f);                    // ... and releases after the gate
		d->kn = dec >= 255 ? 1.f : kdecay(erange(dec, 0.06f, 5.5f)); // amp decay while held (Decay; max = no decay)
		d->hold = (uint16_t)gate;
		d->env = acid_env0(p, vel);
		d->kenv = 1.f - 1.f / (erange(dec, 0.03f, 5.5f) * (SR / DRUM_BLOCK)); // 30 ms .. 1.4 s, per block
		d->g = 1.f - fexp2(-9.0647f * acid_cutoff(p->filter) / SR);
		d->drv0 = 0.2f;
		break;
	case DM_LXR_DRUM: // LXR-02 drum voice: sine, FM by a second sine whose depth follows the pitch EG,
		// pitch EG (sweep = amount; deeper also lasts longer), slope amp EG (decay), snap pitch spike (snap),
		// FM amount (colour), the LXR's distortion curve (drive, in drums_render)
		d->osc = OSC_LXR;
		d->dph = d->dph2 = 55.f * t / SR; // mod oscillator at the same note, not swept (as on the LXR)
		d->ph = d->ph2 = 0.25f;           // the LXR starts its sines at the peak
		d->at = d->ga = 1.f;              // attack 0: full level at once
		d->vg = g;
		d->kt = lxr_step(dec * (100.f / 255.f / 127.f), 1998.f); // LXR decay 0..100: 1.5 ms .. 5.7 s
		d->penv = 1.f;
		d->kp = lxr_step((10.f + s * 30.f) * (1.f / 127.f), 198.f); // 15 .. 65 ms
		d->pamt = s * s * 32.f;           // the LXR's mod amount curve: up to x33 (5 octaves)
		d->sn = 1.f;
		d->snamt = n;
		d->fmamt = c;
		break;
	case DM_LXR_SNARE: // LXR-02 snare: sine (pitch EG = sweep, snap) mixed with sample-and-hold noise
		// (colour = tone -> noise) through the snare's own high-pass, both under one slope amp EG (decay);
		// the noise rate and filter follow Tune
		d->osc = OSC_LXR;
		d->dph = 180.f * t / SR;
		d->ph = 0.25f;
		d->at = d->ga = 1.f;
		d->vg = g;
		d->kt = lxr_step(dec * (100.f / 255.f / 127.f), 1998.f);
		d->penv = 1.f;
		d->kp = lxr_step((10.f + s * 30.f) * (1.f / 127.f), 198.f);
		d->pamt = s * s * 32.f;
		d->sn = 1.f;
		d->snamt = n;
		d->lnmix = c;
		{ float nf = 16000.f * t; d->ninc = nf >= SR ? 0xffffffffu : (uint32_t)(nf * (PH32 / SR)); }
		d->nhold = 0.f;
		d->ns1 = d->ns2 = 0.f;
		svf_coefs(1000.f * t, 0.7f, &d->na1, &d->na2, &d->na3, &d->nk);
		break;
	case DM_LXR_HAT:    // LXR-02 hihat / cymbal: square carrier (tune, snap) phase modulated by two sines at
	case DM_LXR_CYMBAL: // inharmonic ratios (sweep = ratio spread, colour = FM amount) through the voice's own
		// high-pass, slope amp EG (decay), the LXR distortion (drive)
		if (p->model == DM_LXR_HAT) // one hihat voice on the LXR: a new hit chokes the ringing one
			for (int i = 0; i < VOICES; ++i)
				if (dv[i].active && dv[i].model == DM_LXR_HAT && dv[i].kt < 0.25f)
					dv[i].kt = 0.25f;
		{
			const int hat = p->model == DM_LXR_HAT;
			d->osc = OSC_LXR;
			d->dph = (hat ? 1400.f : 700.f) * t / SR;
			d->dph2 = d->dph * (hat ? 1.17f + s * 2.6f : 1.33f + s * 2.2f);
			d->dph3 = d->dph * (hat ? 2.31f + s * 3.9f : 3.17f + s * 2.9f);
			d->ph = 0.25f;
			d->at = d->ga = 1.f;
			d->vg = g;
			d->kt = lxr_step(dec * (100.f / 255.f / 127.f), 1998.f);
			d->sn = 1.f;
			d->snamt = n * 0.25f; // a high carrier: the LXR's full x24 spike would only alias
			d->fmamt = c * 1.2f;
			d->fm2 = c * 0.8f;
			d->ns1 = d->ns2 = 0.f;
			svf_coefs((hat ? 6000.f : 3500.f) * t, 0.9f, &d->na1, &d->na2, &d->na3, &d->nk);
		}
		break;
	case DM_ZAP: // huge fast pitch sweep (colour = sweep time)
		d->osc = OSC_SINE;
		d->dph = 55.f * t / SR;
		d->penv = 3.f + s * 55.f;
		d->kp = kdecay(0.003f + c * 0.07f);
		d->at = g;
		d->kt = kdecay(erange(dec, 0.008f, 6.f));
		d->an = g * n * 0.3f;
		d->kn = kdecay(0.003f);
		exc_filter(d, EF_HP, 1000.f, 0.7f);
		break;
	}
	// filter envelope: decays twice as fast as the main amplitude (k^2 per sample, ^64 per block);
	// a held note (Bass) decays it over half of the hold
	float kb;
	if (d->hold && d->osc != OSC_ACID)
		kb = 1.f - 2.f / (float)d->hold;
	else {
		kb = (d->osc != OSC_NONE) ? d->kt : (d->kn_tail > 0.f ? d->kn_tail : d->kn);
		for (int i = 0; i < 7; ++i)
			kb *= kb;
	}
	if (d->osc == OSC_LXR) // linear amp EG: the filter envelope runs out in half its time
		kb = clampf_(1.f - 4.f * d->kt, 0.f, 1.f);
	d->fenv = 1.f;
	d->fenv_k = kb;
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

HOT static void pass_fold(DrumVoice* d, float* buf, int n) {
	float ph = d->ph, ph2 = d->ph2, penv = d->penv, at = d->at;
	const float dph = d->dph, dph2 = d->dph2, kp = d->kp, kt = d->kt, rm = d->rm, fg = d->fold;
	const float amax = d->a0 > 0.f ? d->a0 : 1e9f; // attack (Bass): grows until a0
	for (int i = 0; i < n; ++i) {
		float pm = 1.f + penv;
		ph += dph * pm;
		ph -= (float)(ph >= 1.f);
		ph2 += dph2 * pm;
		ph2 -= (float)(ph2 >= 1.f);
		float x = psin(ph);
		x *= 1.f - rm + rm * psin(ph2); // ring modulation
		float q = x * fg + 2.f;           // sine wavefolder: sin(2 pi * x * fold)
		q -= (float)(int)q;
		buf[i] = psin(q) * at;
		at *= kt;
		at = at > amax ? amax : at;
		penv *= kp;
	}
	d->ph = ph; d->ph2 = ph2; d->penv = penv; d->at = at;
}

// DM_ACID: two DPW (differentiated parabolic) ramps make an alias-reduced saw and
// square, mixed by wmix, into a 4-pole ladder (feedback kres, soft clipped) whose
// coefficient ramps from the last block's g to g1; the amplitude envelope follows
HOT static void pass_acid(DrumVoice* d, float* buf, int n, float g1, float kres, float wmix) {
	float ph = d->ph, dph = d->dph, at = d->at, sq1 = d->sq1, sq2 = d->sq2;
	float y1 = d->y1, y2 = d->y2, y3 = d->y3, y4 = d->y4, g = d->g;
	const float dph_t = d->dph_t, gk = d->gk, kt = d->kt, amax = d->a0, dg = (g1 - g) * (1.f / SUB);
	const float scale = 0.25f / dph; // DPW gain (the glide moves dph slowly: one value per sub-block is fine)
	for (int i = 0; i < n; ++i) {
		dph += (dph_t - dph) * gk;
		ph += dph;
		ph -= (float)(ph >= 1.f);
		float p1 = 2.f * ph - 1.f, q1 = p1 * p1;
		float ph2 = ph + 0.5f;
		ph2 -= (float)(ph2 >= 1.f);
		float p2 = 2.f * ph2 - 1.f, q2 = p2 * p2;
		float saw = (q1 - sq1) * scale, saw2 = (q2 - sq2) * scale;
		sq1 = q1; sq2 = q2;
		float x = saw - saw2 * wmix; // saw .. saw - saw2 = square
		float u = x - kres * y4;
		u = u > 1.5f ? 1.5f : u < -1.5f ? -1.5f : u;
		u -= 0.148148f * u * u * u;
		y1 += g * (u - y1);
		y2 += g * (y1 - y2);
		y3 += g * (y2 - y3);
		y4 += g * (y3 - y4);
		g += dg;
		buf[i] = y4 * at;
		at *= kt;
		at = at > amax ? amax : at;
	}
	d->ph = ph; d->dph = dph; d->at = at; d->sq1 = sq1; d->sq2 = sq2;
	d->y1 = y1; d->y2 = y2; d->y3 = y3; d->y4 = y4; d->g = g1;
}

// LXR models: envelopes once per sub-block (as the LXR per block), amp gain interpolated across it
HOT static void pass_lxr(DrumVoice* d, float* buf, int n) {
	float a = d->at - d->kt;
	d->at = a = a < 0.f ? 0.f : a;
	const float g1 = lxr_curve(a, d->model == DM_LXR_HAT ? LXR_KA_HAT : LXR_KA);
	float pe = d->penv - d->kp;
	d->penv = pe = pe < 0.f ? 0.f : pe;
	const float pes = lxr_curve(pe, LXR_KPE);
	float sn = d->sn;
	const float snv = sn * sn * 24.f * d->snamt;
	sn -= LXR_KSN;
	d->sn = sn < 0.f ? 0.f : sn;
	float dph = d->dph * (1.f + pes * d->pamt + snv);
	dph = dph > 0.45f ? 0.45f : dph;
	const float dph2 = d->dph2, idx = d->fmamt * d->fmamt * pes, vg = d->vg;
	float ph = d->ph, ph2 = d->ph2, gn = d->ga * vg;
	const float dg = (g1 * vg - gn) / (float)n;
	d->ga = g1;
	if (d->model == DM_LXR_SNARE) { // tone + filtered sample-and-hold noise
		uint32_t nph = d->nph, rng = d->rng;
		const uint32_t ninc = d->ninc;
		float hold = d->nhold, s1 = d->ns1, s2 = d->ns2;
		const float a1 = d->na1, a2 = d->na2, a3 = d->na3, k = d->nk, nm = d->lnmix, tm = 1.f - nm;
		for (int i = 0; i < n; ++i) {
			ph += dph;
			ph -= (float)(ph >= 1.f);
			rng = rng * 1664525u + 1013904223u;
			uint32_t np = nph + ninc;
			hold = np < nph ? (float)(int32_t)rng * (0.9f / 2147483648.f) : hold; // wrapped: a new value
			nph = np;
			float v3 = hold - s2;
			float v1 = a1 * s1 + a2 * v3;
			float v2 = s2 + a2 * s1 + a3 * v3;
			s1 = 2.f * v1 - s1;
			s2 = 2.f * v2 - s2;
			buf[i] = (psin(ph) * tm + (hold - k * v1 - v2) * nm) * gn;
			gn += dg;
		}
		d->ph = ph; d->nph = nph; d->rng = rng; d->nhold = hold; d->ns1 = s1; d->ns2 = s2;
		return;
	}
	if (d->model == DM_LXR_HAT || d->model == DM_LXR_CYMBAL) { // FM metal: square carrier, two sine modulators
		float ph3 = d->ph3, s1 = d->ns1, s2 = d->ns2;
		const float dph3 = d->dph3, i1 = d->fmamt, i2 = d->fm2;
		const float a1 = d->na1, a2 = d->na2, a3 = d->na3, k = d->nk;
		for (int i = 0; i < n; ++i) {
			ph2 += dph2;
			ph2 -= (float)(ph2 >= 1.f);
			ph3 += dph3;
			ph3 -= (float)(ph3 >= 1.f);
			float q = ph + i1 * psin(ph2) + i2 * psin(ph3) + 4.f;
			q -= (float)(int)q;
			float x = (float)(q < 0.5f) - 0.5f; // square, the LXR's carrier gain 0.5
			float v3 = x - s2;
			float v1 = a1 * s1 + a2 * v3;
			float v2 = s2 + a2 * s1 + a3 * v3;
			s1 = 2.f * v1 - s1;
			s2 = 2.f * v2 - s2;
			buf[i] = (x - k * v1 - v2) * gn;
			gn += dg;
			ph += dph;
			ph -= (float)(ph >= 1.f);
		}
		d->ph = ph; d->ph2 = ph2; d->ph3 = ph3; d->ns1 = s1; d->ns2 = s2;
		return;
	}
	for (int i = 0; i < n; ++i) {
		ph2 += dph2;
		ph2 -= (float)(ph2 >= 1.f);
		float q = ph + idx * psin(ph2) + 2.f;
		q -= (float)(int)q;
		buf[i] = psin(q) * gn;
		gn += dg;
		ph += dph;
		ph -= (float)(ph >= 1.f);
	}
	d->ph = ph; d->ph2 = ph2;
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
	const float k1 = d->k1, a1 = d->ea1, a2 = d->ea2, a3 = d->ea3, k = d->ek, kn = d->kn;
	// output mix of the filter responses (low-pass, normalised band-pass, high-pass)
	const float cl = d->emode == EF_LP ? 1.f : 0.f, cb = d->emode == EF_BP ? k : 0.f, ch = d->emode == EF_HP ? 1.f : 0.f;
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

// global drum filter (DJ style) on the bus: dry at full rate, sends at half rate
static float dj_s[3][2];
static void dj_filter_pass(int16_t* x, int n, float* st, float a1, float a2, float a3, float k, float cl) {
	float s1 = st[0], s2 = st[1];
	const float ch = 1.f - cl;
	for (int i = 0; i < n; ++i) {
		float in = x[i];
		float v3 = in - s2;
		float v1 = a1 * s1 + a2 * v3;
		float v2 = s2 + a2 * s1 + a3 * v3;
		s1 = 2.f * v1 - s1;
		s2 = 2.f * v2 - s2;
		x[i] = sat16((int32_t)(cl * v2 + ch * (in - k * v1 - v2)));
	}
	st[0] = s1; st[1] = s2;
}

// drum bus compressor: the block's peak sets a target gain (threshold, ratio and
// make-up from the amount); the gain follows it per sample with a 1 ms attack and
// an 80 ms release, so stacked hits are held down without steps or pumping.
// Kicks are mixed in after it (see drums_render).
static float comp_gain = 1.f;

// drum bus compressor (see comp_gain above); applied to dry and sends
static void bus_compress(int comp) {
	if (!comp) {
		comp_gain = 1.f;
		return;
	}
	const float a = comp * (1.f / 255.f); // amount: threshold 0 .. -36 dB, ratio 2:1 .. 10:1, make-up 0 .. +25 dB (a^2)
	int peak = 0;
	for (int i = 0; i < DRUM_BLOCK; ++i) {
		int v = drum_bus_dry[i] < 0 ? -drum_bus_dry[i] : drum_bus_dry[i];
		if (v > peak) peak = v;
	}
#ifndef COMP_THR
#define COMP_THR 6.f      // threshold range in octaves (6 = 36 dB)
#endif
#ifndef COMP_RATIO
#define COMP_RATIO 8.f    // ratio 2:1 .. (2 + COMP_RATIO):1
#endif
#ifndef COMP_MAKEUP
#define COMP_MAKEUP 4.2f  // make-up in octaves at full amount (scaled by amount^2): the level stays about the same
#endif
#ifndef COMP_KA
#define COMP_KA (1.f / 10.f) // attack per sample (1 / 10 = 0.3 ms)
#endif
	const float thr = 32767.f * BUS_SCALE * fexp2(a * -COMP_THR); // bus full scale = BUS_SCALE
	float target = 1.f;
	if (peak > thr)
		target = fexp2(flog2(thr / (float)peak) * (1.f - 1.f / (2.f + a * COMP_RATIO))); // (thr / peak) ^ (1 - 1 / ratio)
	target *= fexp2(a * a * COMP_MAKEUP);
#ifndef COMP_KR
#define COMP_KR (1.f / 1560.f) // release per sample (1 / 1560 = 50 ms; faster starts to distort a held bass)
#endif
	const float ka = COMP_KA, kr = COMP_KR;
	float g = comp_gain;
	for (int i = 0; i < DRUM_BLOCK; ++i) {
		g += (target - g) * (target < g ? ka : kr);
		drum_bus_dry[i] = sat16((int32_t)(drum_bus_dry[i] * g));
		if (i & 1) {
			drum_bus_dl[i >> 1] = sat16((int32_t)(drum_bus_dl[i >> 1] * g));
			drum_bus_rv[i >> 1] = sat16((int32_t)(drum_bus_rv[i >> 1] * g));
		}
	}
	comp_gain = g;
}

// global overdrive on the bus: gain, then a sine shaper that saturates softly and
// folds the loudest peaks back (wavefolder); the level is kept roughly constant
static void bus_drive_pass(int16_t* x, int n, float g, float post) {
	for (int i = 0; i < n; ++i) {
		float s = x[i] * g;
		s = s / (1.f + fabs_(s) * 0.3f); // never more than one fold
		float ph = s * 0.25f + 0.5f;
		ph += (float)(ph < 0.f);
		ph -= (float)(ph >= 1.f);
		x[i] = sat16((int32_t)(psin(ph) * post));
	}
}
static void bus_drive(int drive) {
	const float a = drive * (1.f / 255.f);
	const float g = (1.f + a * 9.f) / (32768.f * BUS_SCALE);
	const float post = 32768.f * BUS_SCALE * (1.f / 1.57f) * (1.f - a * 0.35f);
	bus_drive_pass(drum_bus_dry, DRUM_BLOCK, g, post);
	bus_drive_pass(drum_bus_dl, DRUM_BLOCK / 2, g, post);
	bus_drive_pass(drum_bus_rv, DRUM_BLOCK / 2, g, post);
}

// global sample rate reduction: sample & hold at a lower rate (dry at full rate, sends at half)
static void bus_srr_pass(int16_t* x, int n, float rate, float* acc, float* hold) {
	float a = *acc, h = *hold;
	for (int i = 0; i < n; ++i) {
		a += rate;
		if (a >= 1.f) { a -= 1.f; h = x[i]; }
		x[i] = (int16_t)h;
	}
	*acc = a; *hold = h;
}
static void bus_srr(int srr) {
	const float rate = fexp2(srr * (-6.f / 255.f));
	bus_srr_pass(drum_bus_dry, DRUM_BLOCK, rate, &bus_srr_acc[0], &bus_srr_hold[0]);
	const float r2 = rate * 2.f > 1.f ? 1.f : rate * 2.f;
	float a = bus_srr_acc[1];
	bus_srr_pass(drum_bus_dl, DRUM_BLOCK / 2, r2, &a, &bus_srr_hold[1]);
	bus_srr_pass(drum_bus_rv, DRUM_BLOCK / 2, r2, &bus_srr_acc[1], &bus_srr_hold[2]);
}

// voicing of the drum output: a high shelf, so hats and noise don't hide the kick
// and bass (a typical drum mix falls off up top)
#ifndef SHELF_K
#define SHELF_K 0.55f   // one-pole split at ~4 kHz (1 - e^(-2 pi fc / 31250))
#endif
#ifndef SHELF_CUT
#define SHELF_CUT 0.6f  // -4.4 dB above the split
#endif
static void bus_shelf(void) {
	const float k = SHELF_K, cut = SHELF_CUT;
	float lp = shelf_lp;
	for (int i = 0; i < DRUM_BLOCK; ++i) {
		float x = drum_bus_dry[i];
		lp += k * (x - lp);
		drum_bus_dry[i] = sat16((int32_t)(lp + (x - lp) * cut));
	}
	shelf_lp = lp;
}

HOT void drums_render(int master, int djfilter, int comp, int drive, int srr, int dlsend, int rvsend) {
	memset(drum_bus_dry, 0, sizeof(drum_bus_dry));
	memset(drum_bus_dl, 0, sizeof(drum_bus_dl));
	memset(drum_bus_rv, 0, sizeof(drum_bus_rv));
	const float mg = master * (OUT_GAIN / (255.f * 255.f));
	float buf[SUB];
	// pass 0: every voice but the kicks, then the compressor; pass 1: the kicks on
	// top, so they bypass it (the body of the loop keeps its old indentation)
	for (int pass = 0; pass < 2; ++pass) {
	if (pass == 1)
		bus_compress(comp);
	for (int v = 0; v < VOICES; ++v) {
		DrumVoice* d = &dv[v];
		if (!d->active || is_kick(d->model) != pass)
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

		if (d->hold && --d->hold == 0) // end of the hold: start the release
			d->kt = d->kt_rel;
		else if (d->osc == OSC_ACID && d->hold && d->kt > 1.f && d->at >= d->a0) // Acid: attack done, decay while held
			d->kt = d->kn;
		// per block: which parts run, and the live filter / drive / level
		const int use_tone = d->osc != OSC_NONE && d->at > 1e-5f;
		const int use_exc = d->an > 1e-5f;
		if (!use_tone) d->at = 0.f;
		if (!use_exc) d->an = 0.f;
		int fmode = 0; // resonant filter: 0 off, 1 low-pass, 2 high-pass
		float a1 = 0.f, a2 = 0.f, a3 = 0.f, k = 2.f;
		int fb = p->filter;
		float acid_g = 0.f, acid_k = 0.f, acid_w = 0.f;
		if (d->osc == OSC_ACID) { // the ladder replaces the lane filter: cutoff (live) * envelope (depth = fenv, live)
			float fc = acid_cutoff(p->filter) * fexp2((p->fenv - DRUM_FILTER_OFF) * (5.f / 127.f) * d->env);
			d->env *= d->kenv;
			if (fc > 5000.f) fc = 5000.f;
			acid_g = 1.f - fexp2(-9.0647f * fc / SR); // 1 - e^(-2 pi fc / SR)
			acid_k = p->reso * (3.9f / 255.f);
			acid_w = p->tone * (1.f / 255.f);
		}
		else if (p->fenv != DRUM_FILTER_OFF) {
			// filter envelope (1 -> 0) pushes the cutoff away from the knob by up to +-4 octaves
			// and returns to it: positive = starts open / thin and closes to the knob, negative =
			// starts closed and opens. With the filter off it works on an open low-pass (so only
			// a negative amount does something); the filter never switches between LP and HP.
			float oct = (p->fenv - DRUM_FILTER_OFF) * (4.f / 127.f) * d->fenv;
			d->fenv *= d->fenv_k;
			if (fb > DRUM_FILTER_OFF - 5 && fb < DRUM_FILTER_OFF + 5) fb = DRUM_FILTER_OFF - 5;
			if (fb < DRUM_FILTER_OFF) fb = clampi_(fb + (int)(oct * (128.f / 9.f)), 0, DRUM_FILTER_OFF - 5);
			else fb = clampi_(fb + (int)(oct * (127.f / 9.f)), DRUM_FILTER_OFF + 5, 255);
		}
		if (d->osc != OSC_ACID && (fb < DRUM_FILTER_OFF - 4 || fb > DRUM_FILTER_OFF + 4)) {
			fmode = fb < DRUM_FILTER_OFF ? 1 : 2;
			float fc = (fmode == 1) ? 18000.f * fexp2((fb - DRUM_FILTER_OFF) * (9.f / 128.f)) // 35 Hz .. open
			                        : 20.f * fexp2((fb - DRUM_FILTER_OFF) * (9.f / 127.f));   // 20 Hz .. 10 kHz
			if (fc > 13000.f) fc = 13000.f;
			float gc = tanpade(3.14159265f * fc / SR);
			k = 2.f - p->reso * (1.95f / 255.f);
			a1 = 1.f / (1.f + gc * (gc + k));
			a2 = gc * a1;
			a3 = gc * a2;
		}
		const float cl = (fmode == 1) ? 1.f : 0.f, ch = 1.f - cl;
		const float drv = d->osc == OSC_LXR ? 0.f : d->drv0 + p->drive * (4.f / 255.f), dg = 1.f + drv;
		const float lxd_s = p->drive * (0.99f / 255.f) - 0.0078125f, lxd = 2.f * lxd_s / (1.f - lxd_s); // LXR distortion
		float srr_rate = p->srr ? fexp2(p->srr * (-6.f / 255.f)) : 1.f;
		if (d->srr0 < srr_rate) srr_rate = d->srr0;
		const float gd = p->level * mg * BUS_SCALE;
		const float gdl = gd * p->dlsend * (0.5f / 255.f), grv = gd * p->rvsend * (0.5f / 255.f); // sends: full level (not BUS_SCALE)

		for (int h = 0; h < DRUM_BLOCK; h += SUB) {
			// 1) tone
			if (use_tone) {
				if (d->osc == OSC_SINE) pass_sine(d, buf, SUB);
				else if (d->osc == OSC_SINE2) pass_sine2(d, buf, SUB);
				else if (d->osc == OSC_FOLD) pass_fold(d, buf, SUB);
				else if (d->osc == OSC_ACID) pass_acid(d, buf, SUB, acid_g, acid_k, acid_w);
				else if (d->osc == OSC_LXR) pass_lxr(d, buf, SUB);
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
			// 4b) the LXR's distortion: (1+k)x / (1+k|x|)
			if (d->osc == OSC_LXR && p->drive)
				for (int i = 0; i < SUB; ++i)
					buf[i] = (1.f + lxd) * buf[i] / (1.f + lxd * fabs_(buf[i]));
			// 5a) bit crush (built into some models)
			if (d->crush > 0.f) {
				const float lv = d->crush, inv = 1.f / d->crush;
				for (int i = 0; i < SUB; ++i)
					buf[i] = (float)(int)(buf[i] * lv) * inv;
			}
			// 5b) sample rate reduction (live): sample & hold at a lower rate
			if (srr_rate < 1.f) {
				float acc = d->srr_acc, hold = d->srr_hold;
				for (int i = 0; i < SUB; ++i) {
					acc += srr_rate;
					if (acc >= 1.f) {
						acc -= 1.f;
						hold = buf[i];
					}
					buf[i] = hold;
				}
				d->srr_acc = acc; d->srr_hold = hold;
			}
			// 6) mix into the drum bus
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
			// 7) declick: the hit this voice replaced (retrigger or steal) fades out over ~2 ms
			if (d->dc != 0.f) {
				float dc = d->dc;
				for (int i = 0; i < SUB; ++i, dc *= 0.985f)
					dry[i] = sat16(dry[i] + (int32_t)dc);
				d->dc = fabs_(dc) > 1.f ? dc : 0.f;
			}
			d->last = buf[SUB - 1] * gd;
		}

		if (d->at < SILENT && d->an < SILENT && !d->clap_left)
			d->active = 0;
	}
	}
	if (djfilter < DRUM_FILTER_OFF - 4 || djfilter > DRUM_FILTER_OFF + 4) {
		int lp = djfilter < DRUM_FILTER_OFF;
		float fc = lp ? 18000.f * fexp2((djfilter - DRUM_FILTER_OFF) * (8.f / 128.f)) // 70 Hz .. open
		              : 20.f * fexp2((djfilter - DRUM_FILTER_OFF) * (9.f / 127.f));   // 20 Hz .. 10 kHz
		if (fc > 13000.f) fc = 13000.f;
		const float k = 1.2f; // a little resonance, like a DJ mixer
		float g = tanpade(3.14159265f * fc / SR);
		float a1 = 1.f / (1.f + g * (g + k)), a2 = g * a1, a3 = g * a2;
		dj_filter_pass(drum_bus_dry, DRUM_BLOCK, dj_s[0], a1, a2, a3, k, (float)lp);
		g = tanpade(3.14159265f * fc * 2.f / SR); // sends run at half rate
		if (fc * 2.f > 13000.f) g = tanpade(3.14159265f * 13000.f / SR);
		a1 = 1.f / (1.f + g * (g + k)); a2 = g * a1; a3 = g * a2;
		dj_filter_pass(drum_bus_dl, DRUM_BLOCK / 2, dj_s[1], a1, a2, a3, k, (float)lp);
		dj_filter_pass(drum_bus_rv, DRUM_BLOCK / 2, dj_s[2], a1, a2, a3, k, (float)lp);
	}
	else
		memset(dj_s, 0, sizeof(dj_s));

	if (drive)
		bus_drive(drive);
	if (dlsend || rvsend) { // whole drum mix -> delay / reverb (half rate), at the lanes' send scale
		const float gl = dlsend * (0.5f / 255.f), gr = rvsend * (0.5f / 255.f);
		for (int i = 0; i < DRUM_BLOCK / 2; ++i) {
			float x = (float)drum_bus_dry[i * 2] + (float)drum_bus_dry[i * 2 + 1];
			drum_bus_dl[i] = sat16(drum_bus_dl[i] + (int32_t)(x * gl));
			drum_bus_rv[i] = sat16(drum_bus_rv[i] + (int32_t)(x * gr));
		}
	}
	bus_shelf();

	{ // 2-pole Butterworth high-pass at 25 Hz: removes DC and sub rumble, a 50 Hz kick loses 0.3 dB
		const float g = 3.14159265f * 25.f / SR, k = 1.41421356f; // tan(x) ~ x this low
		const float a1 = 1.f / (1.f + g * (g + k)), a2 = g * a1, a3 = g * a2;
		dj_filter_pass(drum_bus_dry, DRUM_BLOCK, sub_s, a1, a2, a3, k, 0.f);
	}
	if (srr) // last before the output stage, so its steps stay crisp
		bus_srr(srr);

	// output: the bus holds BUS_SCALE of the signal and the output stage has gain 2,
	// so one full hit (-3.5 dBFS at full level and volume) and a compressed stack of
	// hits stay below the knee at 80 %, where the stage is linear. Above it a soft
	// knee y = t + (1 - t) z / (1 + z) bends the rest smoothly towards full scale
	// instead of clipping hard (slope 1 at the knee, so no kink).
#ifndef OUT_KNEE
#define OUT_KNEE 0.8f  // linear below this (fraction of full scale)
#endif
#ifndef OUT_CEIL
#define OUT_CEIL 1.0f  // ... and bends smoothly towards this ceiling above it
#endif
	const float t = OUT_KNEE, c = OUT_CEIL;
	for (int i = 0; i < DRUM_BLOCK; ++i) {
		float x = drum_bus_dry[i] * (1.f / (32768.f * BUS_SCALE));
		float a = fabs_(x);
		if (a > t) {
			float z = (a - t) * (1.f / (c - t));
			a = t + (c - t) * z / (1.f + z);
			x = x < 0.f ? -a : a;
		}
		drum_bus_dry[i] = (int16_t)(x * 32767.f);
	}
}
