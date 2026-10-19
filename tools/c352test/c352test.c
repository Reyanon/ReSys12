// ReSys12: C352 test. Synthetic ROM only (no game data). Checks the port against a literal transliteration of MAME's
// sample-major c352_device::sound_stream_update() (BSD-3-Clause, R. Belmont / superctr), plus WAV / zero-crossing /
// peak checks and a 32-voice timing run.
// usage: c352test [out_dir]
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "c352.h"

#define RATE_CLOCK 25401600u
#define RATE_DIV 288
#define ROM_SIZE 0x1000000u

enum {
	F_BUSY = 0x8000, F_KEYON = 0x4000, F_KEYOFF = 0x2000, F_LOOPHIST = 0x0800, F_PHASERL = 0x0200,
	F_PHASEFL = 0x0100, F_PHASEFR = 0x0080, F_LDIR = 0x0040, F_LINK = 0x0020, F_NOISE = 0x0010,
	F_MULAW = 0x0008, F_FILTER = 0x0004, F_LOOP = 0x0002, F_REVERSE = 0x0001
};

static uint8_t *rom;
static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

// ---------------- reference: MAME transliteration ----------------
typedef struct {
	uint32_t pos, counter;
	int16_t sample, last_sample;
	uint16_t vol_f, vol_r;
	uint8_t curr_vol[4];
	uint16_t freq, flags, wave_bank, wave_start, wave_end, wave_loop;
} RefVoice;

static RefVoice rv[32];
static uint16_t r_random, r_control;
static int16_t r_mulaw[256];

static uint16_t *ref_reg(uint32_t o)
{
	RefVoice *v = &rv[o / 8];
	uint16_t *m[8] = { &v->vol_f, &v->vol_r, &v->freq, &v->flags, &v->wave_bank, &v->wave_start, &v->wave_end,
		&v->wave_loop };
	return m[o % 8];
}

static void ref_init(void)
{
	memset(rv, 0, sizeof(rv));
	r_random = 0x1234;
	r_control = 0;
	int j = 0;
	for (int i = 0; i < 128; i++) {
		r_mulaw[i] = (int16_t)(j << 5);
		if (i < 16) j += 1; else if (i < 24) j += 2; else if (i < 48) j += 4; else if (i < 100) j += 8; else j += 16;
	}
	for (int i = 0; i < 128; i++)
		r_mulaw[i + 128] = (int16_t)((~r_mulaw[i]) & 0xffe0);
}

static uint16_t ref_read(uint32_t o)
{
	if (o < 0x100) return *ref_reg(o);
	if (o == 0x200) return r_control;
	return 0;
}

static void ref_write(uint32_t o, uint16_t d)
{
	if (o < 0x100) *ref_reg(o) = d;
	else if (o == 0x200) r_control = d;
	else if (o == 0x202) {
		for (int i = 0; i < 32; i++) {
			if (rv[i].flags & F_KEYON) {
				rv[i].pos = (rv[i].wave_bank << 16) | rv[i].wave_start;
				rv[i].sample = 0;
				rv[i].last_sample = 0;
				rv[i].counter = 0xffff;
				rv[i].flags |= F_BUSY;
				rv[i].flags &= ~(F_KEYON | F_LOOPHIST);
				rv[i].curr_vol[0] = rv[i].curr_vol[1] = 0;
				rv[i].curr_vol[2] = rv[i].curr_vol[3] = 0;
			}
			if (rv[i].flags & F_KEYOFF) {
				rv[i].flags &= ~(F_BUSY | F_KEYOFF);
				rv[i].counter = 0xffff;
			}
		}
	}
}

static void ref_fetch(RefVoice *v)
{
	v->last_sample = v->sample;
	if (v->flags & F_NOISE) {
		r_random = (r_random >> 1) ^ ((-(r_random & 1)) & 0xfff6);
		v->sample = r_random;
	} else {
		int8_t s = (int8_t)rom[v->pos & 0xffffff];
		if (v->flags & F_MULAW) v->sample = r_mulaw[s & 0xff];
		else v->sample = s * 256;
		uint16_t pos = v->pos & 0xffff;
		if ((v->flags & F_LOOP) && v->flags & F_REVERSE) {
			if ((v->flags & F_LDIR) && pos == v->wave_loop) v->flags &= ~F_LDIR;
			else if (!(v->flags & F_LDIR) && pos == v->wave_end) v->flags |= F_LDIR;
			v->pos += (v->flags & F_LDIR) ? -1 : 1;
		} else if (pos == v->wave_end) {
			if ((v->flags & F_LINK) && (v->flags & F_LOOP)) {
				v->pos = (v->wave_start << 16) | v->wave_loop;
				v->flags |= F_LOOPHIST;
			} else if (v->flags & F_LOOP) {
				v->pos = (v->pos & 0xff0000) | v->wave_loop;
				v->flags |= F_LOOPHIST;
			} else {
				v->flags |= F_KEYOFF;
				v->flags &= ~F_BUSY;
				v->sample = 0;
			}
		} else
			v->pos += (v->flags & F_REVERSE) ? -1 : 1;
	}
}

static void ref_ramp(RefVoice *v, int ch, uint8_t val)
{
	int16_t d = v->curr_vol[ch] - val;
	if (d != 0) v->curr_vol[ch] += (d > 0) ? -1 : 1;
}

static int16_t sat(int x)
{
	return (int16_t)(x > 32767 ? 32767 : (x < -32768 ? -32768 : x));
}

static void ref_render(int16_t *outp, int n)
{
	for (int i = 0; i < n; i++) {
		int out[4] = { 0, 0, 0, 0 };
		for (int j = 0; j < 32; j++) {
			RefVoice *v = &rv[j];
			int16_t s = 0;
			if (v->flags & F_BUSY) {
				int32_t next_counter = v->counter + v->freq;
				if (next_counter & 0x10000) ref_fetch(v);
				if ((next_counter ^ v->counter) & 0x18000) {
					ref_ramp(v, 0, v->vol_f >> 8);
					ref_ramp(v, 1, v->vol_f & 0xff);
					ref_ramp(v, 2, v->vol_r >> 8);
					ref_ramp(v, 3, v->vol_r & 0xff);
				}
				v->counter = next_counter & 0xffff;
				s = v->sample;
				if ((v->flags & F_FILTER) == 0)
					s = v->last_sample + (v->counter * (v->sample - v->last_sample) >> 16);
			}
			out[0] += (((v->flags & F_PHASEFL) ? -s : s) * v->curr_vol[0]) >> 8;
			out[2] += (((v->flags & F_PHASERL) ? -s : s) * v->curr_vol[2]) >> 8;
			out[1] += (((v->flags & F_PHASEFR) ? -s : s) * v->curr_vol[1]) >> 8;
			out[3] += (((v->flags & F_PHASEFR) ? -s : s) * v->curr_vol[3]) >> 8;
		}
		outp[2 * i] = sat(out[0] >> 3);
		outp[2 * i + 1] = sat(out[1] >> 3);
	}
}

// ---------------- helpers ----------------
static void both_write(uint32_t o, uint16_t d)
{
	c352_write(o, d);
	ref_write(o, d);
}

static void voice_setup(int ch, uint16_t bank, uint16_t start, uint16_t end, uint16_t loop, uint16_t freq,
	uint16_t vol_f, uint16_t vol_r, uint16_t flags)
{
	const uint32_t b = (uint32_t)ch * 8;
	both_write(b + 0, vol_f);
	both_write(b + 1, vol_r);
	both_write(b + 2, freq);
	both_write(b + 3, flags);
	both_write(b + 4, bank);
	both_write(b + 5, start);
	both_write(b + 6, end);
	both_write(b + 7, loop);
}

static uint8_t mulaw_encode(int v)
{
	// nearest entry of the chip's mu-law table
	int best = 0, bd = 1 << 30;
	for (int i = 0; i < 256; i++) {
		int d = abs(r_mulaw[i] - v);
		if (d < bd) { bd = d; best = i; }
	}
	return (uint8_t)best;
}

static void write_wav(const char *path, const int16_t *s, int frames, int rate)
{
	FILE *f = fopen(path, "wb");
	if (!f) { printf("cannot write %s\n", path); return; }
	const uint32_t data = (uint32_t)frames * 4;
	uint8_t h[44];
	memcpy(h, "RIFF", 4);
	uint32_t v = 36 + data; memcpy(h + 4, &v, 4);
	memcpy(h + 8, "WAVEfmt ", 8);
	v = 16; memcpy(h + 16, &v, 4);
	uint16_t w = 1; memcpy(h + 20, &w, 2);
	w = 2; memcpy(h + 22, &w, 2);
	v = (uint32_t)rate; memcpy(h + 24, &v, 4);
	v = (uint32_t)rate * 4; memcpy(h + 28, &v, 4);
	w = 4; memcpy(h + 32, &w, 2);
	w = 16; memcpy(h + 34, &w, 2);
	memcpy(h + 36, "data", 4);
	memcpy(h + 40, &data, 4);
	fwrite(h, 1, 44, f);
	fwrite(s, 4, (size_t)frames, f);
	fclose(f);
}

static void analyze(const int16_t *s, int frames, int ch, int *zc, int *peak)
{
	*zc = 0;
	*peak = 0;
	int prev = 0;
	for (int i = 0; i < frames; i++) {
		int x = s[2 * i + ch];
		if (abs(x) > *peak) *peak = abs(x);
		if (x != 0) {
			if (prev != 0 && ((x > 0) != (prev > 0))) (*zc)++;
			prev = x;
		}
	}
}

static int compare(const int16_t *a, const int16_t *b, int n, const char *what)
{
	for (int i = 0; i < 2 * n; i++)
		if (a[i] != b[i]) {
			printf("mismatch %s at frame %d ch %d: port %d ref %d\n", what, i / 2, i & 1, a[i], b[i]);
			return 0;
		}
	return 1;
}

static int compare_regs(const char *what)
{
	for (uint32_t o = 0; o < 0x204; o++)
		if (c352_read(o) != ref_read(o)) {
			printf("reg mismatch %s at %03x: port %04x ref %04x\n", what, o, c352_read(o), ref_read(o));
			return 0;
		}
	return 1;
}

static uint32_t rng = 12345;
static uint32_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static double now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

// ---------------- tests ----------------
#define SINE_PERIOD 64
#define SINE_LEN 4096 // 64 periods

static void build_rom(void)
{
	rom = malloc(ROM_SIZE);
	for (uint32_t i = 0; i < ROM_SIZE; i++)
		rom[i] = (uint8_t)rnd(); // fuzz data everywhere else
	for (int i = 0; i < SINE_LEN; i++) {
		double x = sin(2.0 * M_PI * i / SINE_PERIOD);
		rom[0x001000 + i] = (uint8_t)(int8_t)lrint(x * 127.0);          // bank 0: linear 8-bit
		rom[0x011000 + i] = mulaw_encode((int)lrint(x * 30000.0));     // bank 1: mu-law
	}
}

static void test_basic(const char *dir)
{
	const int n = 88200;
	int16_t *a = malloc(sizeof(int16_t) * 2 * n), *b = malloc(sizeof(int16_t) * 2 * n);
	c352_reset();
	ref_init();
	CHECK(c352_sample_rate() == 88200, "sample rate %u", c352_sample_rate());

	// voice 0: linear sine, looped, front left only. freq 0x4000 -> 22050 samples/s -> 344.53 Hz
	voice_setup(0, 0x00, 0x1000, 0x1000 + SINE_LEN - 1, 0x1000, 0x4000, 0xff00, 0x0000, F_KEYON | F_LOOP);
	// voice 1: mu-law sine, looped, front right only. freq 0x8000 -> 689.06 Hz
	voice_setup(1, 0x01, 0x1000, 0x1000 + SINE_LEN - 1, 0x1000, 0x8000, 0x00ff, 0x0000, F_KEYON | F_LOOP | F_MULAW);
	both_write(0x202, 0);
	CHECK(c352_read(3) == (F_BUSY | F_LOOP), "flags after keyon %04x", c352_read(3));
	CHECK(c352_read(0) == 0xff00 && c352_read(2) == 0x4000 && c352_read(13) == 0x1000, "register readback");

	c352_render(a, n);
	ref_render(b, n);
	CHECK(compare(a, b, n, "basic"), "basic output differs from reference");
	CHECK(compare_regs("basic"), "basic registers differ");
	CHECK(c352_read(3) & F_LOOPHIST, "loop history not set after looping");

	int zc, pk;
	analyze(a, n, 0, &zc, &pk);
	printf("linear sine L: zero crossings %d (expect ~689), peak %d (expect ~%d)\n", zc, pk, 127 * 256 * 255 / 256 / 8);
	CHECK(zc >= 685 && zc <= 693, "linear zero crossings %d", zc);
	CHECK(pk >= 3950 && pk <= 4070, "linear peak %d", pk);
	analyze(a, n, 1, &zc, &pk);
	printf("mu-law sine R: zero crossings %d (expect ~1378), peak %d (expect ~%d)\n", zc, pk, 30000 * 255 / 256 / 8);
	CHECK(zc >= 1372 && zc <= 1384, "mulaw zero crossings %d", zc);
	CHECK(pk >= 3600 && pk <= 3800, "mulaw peak %d", pk);

	char path[1024];
	snprintf(path, sizeof(path), "%s/c352_sine.wav", dir);
	write_wav(path, a, n, 88200);
	printf("wrote %s\n", path);

	// key-off voice 0 through the flags register
	both_write(3, (uint16_t)(c352_read(3) | F_KEYOFF));
	both_write(0x202, 0);
	CHECK(!(c352_read(3) & (F_BUSY | F_KEYOFF)), "voice 0 still busy after keyoff: %04x", c352_read(3));
	c352_render(a, 1000);
	ref_render(b, 1000);
	CHECK(compare(a, b, 1000, "keyoff"), "keyoff output differs");
	analyze(a, 1000, 0, &zc, &pk);
	CHECK(pk == 0, "left channel not silent after keyoff (peak %d)", pk);
	analyze(a, 1000, 1, &zc, &pk);
	CHECK(pk > 3000, "right channel stopped (peak %d)", pk);

	// voice 2: one-shot (no loop), 256 samples at freq 0x10000-1 -> ends after ~256 frames
	voice_setup(2, 0x00, 0x1000, 0x10ff, 0x1000, 0xffff, 0xffff, 0xffff, F_KEYON);
	both_write(1 * 8 + 3, (uint16_t)(c352_read(1 * 8 + 3) | F_KEYOFF));
	both_write(0x202, 0);
	c352_render(a, 400);
	ref_render(b, 400);
	CHECK(compare(a, b, 400, "oneshot"), "one-shot output differs");
	CHECK((c352_read(2 * 8 + 3) & (F_BUSY | F_KEYOFF)) == F_KEYOFF, "one-shot end flags %04x", c352_read(2 * 8 + 3));
	analyze(a + 2 * 300, 100, 0, &zc, &pk);
	CHECK(pk == 0, "one-shot not silent after end (peak %d)", pk);
	CHECK(compare_regs("oneshot"), "one-shot registers differ");
	both_write(0x200, 0xbeef);
	CHECK(c352_read(0x200) == 0xbeef && c352_read(0x201) == 0 && c352_read(0x150) == 0, "control register");
	free(a);
	free(b);
}

static void random_voice(int ch)
{
	static const uint16_t flagset[] = { 0, F_LOOP, F_REVERSE, F_LOOP | F_REVERSE, F_LOOP | F_LINK, F_NOISE,
		F_MULAW, F_MULAW | F_LOOP, F_FILTER, F_FILTER | F_LOOP, F_LOOP | F_REVERSE | F_LDIR, F_FILTER | F_NOISE };
	uint16_t f = flagset[rnd() % (sizeof(flagset) / sizeof(flagset[0]))];
	f |= (uint16_t)(rnd() & (F_PHASEFL | F_PHASEFR | F_PHASERL));
	if (rnd() & 1) f |= F_FILTER;
	if (rnd() & 1) f |= F_MULAW;
	const uint16_t start = (uint16_t)rnd();
	const uint16_t len = (uint16_t)(rnd() % 3000);
	const int rev = (f & F_REVERSE) && !(f & F_LOOP);
	const uint16_t end = rev ? (uint16_t)(start - len) : (uint16_t)(start + len);
	const uint16_t loop = rev ? (uint16_t)(start - rnd() % (len + 1u)) : (uint16_t)(start + rnd() % (len + 1u));
	voice_setup(ch, (uint16_t)rnd(), start, end, loop, (uint16_t)rnd(), (uint16_t)rnd(), (uint16_t)rnd(),
		(uint16_t)(f | F_KEYON));
}

static void test_fuzz(void)
{
	const int MAXN = 3000;
	int16_t *a = malloc(sizeof(int16_t) * 2 * MAXN), *b = malloc(sizeof(int16_t) * 2 * MAXN);
	c352_reset();
	ref_init();
	long frames = 0;
	int ok = 1;
	for (int it = 0; it < 4000 && ok; it++) {
		const int nw = (int)(rnd() % 6);
		for (int k = 0; k < nw; k++)
			random_voice((int)(rnd() % 32));
		if (rnd() % 3 == 0) // random keyoffs
			for (int k = (int)(rnd() % 4); k > 0; k--) {
				const uint32_t o = (rnd() % 32) * 8 + 3;
				both_write(o, (uint16_t)(c352_read(o) | F_KEYOFF));
			}
		if (rnd() % 5 == 0) // random raw register poke (volume / freq change while playing)
			both_write((rnd() % 32) * 8 + (rnd() % 3), (uint16_t)rnd());
		both_write(0x202, 0);
		const int n = (int)(rnd() % MAXN) + 1;
		c352_render(a, n);
		ref_render(b, n);
		frames += n;
		ok = compare(a, b, n, "fuzz") && compare_regs("fuzz");
		if (ok && it % 500 == 499) { // save / load round trip
			uint8_t *st = malloc(c352_state_size());
			c352_state_save(st);
			c352_render(a, 500);
			c352_state_load(st);
			int16_t *c = malloc(sizeof(int16_t) * 2 * 500);
			c352_render(c, 500);
			ok = compare(a, c, 500, "state reload");
			ref_render(b, 500);
			ok = ok && compare(c, b, 500, "state vs ref");
			free(c);
			free(st);
		}
	}
	CHECK(ok, "fuzz vs MAME reference failed");
	printf("fuzz: %ld frames over 4000 iterations bit-exact with reference: %s\n", frames, ok ? "yes" : "NO");
	free(a);
	free(b);
}

static void test_timing(void)
{
	const int n = 88200;
	int16_t *a = malloc(sizeof(int16_t) * 2 * n);
	double best[3] = { 1e9, 1e9, 1e9 };
	const char *name[3] = { "linear+interp", "mu-law+interp", "linear no-interp (FILTER)" };
	const uint16_t fl[3] = { F_LOOP, F_LOOP | F_MULAW, F_LOOP | F_FILTER };
	for (int t = 0; t < 3; t++)
		for (int rep = 0; rep < 5; rep++) {
			c352_reset();
			for (int ch = 0; ch < 32; ch++) {
				const uint32_t b = (uint32_t)ch * 8;
				c352_write(b + 0, 0x8080);
				c352_write(b + 1, 0x8080);
				c352_write(b + 2, (uint16_t)(0x3000 + ch * 0x400)); // 0.19 .. 0.67 source samples per frame
				c352_write(b + 3, (uint16_t)(F_KEYON | fl[t]));
				c352_write(b + 4, (uint16_t)ch);
				c352_write(b + 5, 0x0000);
				c352_write(b + 6, 0x7fff);
				c352_write(b + 7, 0x0100);
			}
			c352_write(0x202, 0);
			const double t0 = now_ms();
			for (int i = 0; i < n; i += 1470) // 60 Hz frame-sized calls
				c352_render(a + 2 * i, (n - i) < 1470 ? n - i : 1470);
			const double dt = now_ms() - t0;
			if (dt < best[t]) best[t] = dt;
		}
	for (int t = 0; t < 3; t++)
		printf("timing: 1 s (88200 frames) of 32 voices %s: %.2f ms (best of 5)\n", name[t], best[t]);
	free(a);
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : ".";
	ref_init(); // builds the mu-law table used by build_rom()
	build_rom();
	c352_init(rom, ROM_SIZE, RATE_CLOCK, RATE_DIV);
	test_basic(dir);
	test_fuzz();
	test_timing();
	printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	free(rom);
	return failures ? 1 : 0;
}
