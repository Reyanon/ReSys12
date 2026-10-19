#include "frontend/resample.h"

#include <math.h>
#include <string.h>

#define TAPS   16
#define HALF   (TAPS / 2)
#define PHASES 1024
#define HIST   (TAPS - 1) // input samples carried over to the next call

static float s_coef[PHASES][TAPS];
static float s_buf[(RS12_RESAMPLE_MAX_IN + HIST) * 2]; // first HIST pairs are the tail of the previous input
static uint32_t s_pos; // next output position (16.16, in s_buf sample pairs)

static double sinc(double x)
{
	if (fabs(x) < 1e-9)
		return 1.0;
	return sin(M_PI * x) / (M_PI * x);
}

void rs12_resample_init(void)
{
	// with the output position at fraction f between x[i] and x[i+1], tap j multiplies x[i + j - (HALF-1)]
	for (int p = 0; p < PHASES; p++) {
		double f = (double)p / PHASES, w[TAPS], sum = 0;
		for (int j = 0; j < TAPS; j++) {
			double u = f - (j - (HALF - 1));
			w[j] = fabs(u) < HALF ? sinc(u) * sinc(u / HALF) : 0.0;
			sum += w[j];
		}
		for (int j = 0; j < TAPS; j++)
			s_coef[p][j] = (float)(w[j] / sum); // DC gain 1
	}
	memset(s_buf, 0, sizeof(s_buf));
	s_pos = (uint32_t)(HALF - 1) << 16;
}

static int16_t clip16(float v)
{
	if (v > 32767.0f) return 32767;
	if (v < -32768.0f) return -32768;
	return (int16_t)lrintf(v);
}

int rs12_resample(const int16_t *in, int frames, uint32_t step, int16_t *out)
{
	if (frames > RS12_RESAMPLE_MAX_IN)
		frames = RS12_RESAMPLE_MAX_IN;
	float *dst = s_buf + HIST * 2;
	for (int i = 0; i < frames * 2; i++)
		dst[i] = in[i];
	const uint32_t total = HIST + (uint32_t)frames;

	int n = 0;
	uint32_t pos = s_pos;
	for (;;) {
		uint32_t i = pos >> 16;
		if (i + HALF >= total)
			break;
		const float *c = s_coef[(pos & 0xffff) >> 6];
		const float *x = s_buf + (i - (HALF - 1)) * 2;
		float l = 0, r = 0;
		for (int j = 0; j < TAPS; j++) {
			l += x[j * 2] * c[j];
			r += x[j * 2 + 1] * c[j];
		}
		out[n * 2] = clip16(l);
		out[n * 2 + 1] = clip16(r);
		n++;
		pos += step;
	}

	// move the last HIST pairs to the front and adjust the position
	memmove(s_buf, s_buf + (total - HIST) * 2, HIST * 2 * sizeof(float));
	s_pos = pos - ((total - HIST) << 16);
	return n;
}
