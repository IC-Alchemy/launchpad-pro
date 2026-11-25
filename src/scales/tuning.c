#include "scales/tuning.h"
#include "scales/scales.h"

/* Pico2Seq breaks a nearest-degree tie by 0.01 cent: about 3 fine units. */
#define TIE_FINE 3

const struct tuning_def *tuning_find(uint8_t id)
{
	uint8_t i;

	for (i = 0; i < TUNING_COUNT; i++)
		if (TUNINGS[i].id == id)
			return &TUNINGS[i];
	return 0;
}

const struct tuning_def *tuning_get(uint8_t id)
{
	const struct tuning_def *t = tuning_find(id);

	return t ? t : &TUNINGS[0];
}

int8_t tuning_index(uint8_t id)
{
	const struct tuning_def *t = tuning_find(id);

	return t ? (int8_t)(t - TUNINGS) : -1;
}

int32_t tuning_floor_div(int32_t v, int32_t d)
{
	int32_t q = v / d;

	if (v % d != 0 && v < 0)
		q--;
	return q;
}

/* the period in 1/256 fine units */
static int32_t period256(const struct tuning_def *t)
{
	return (int32_t)(t->period * 256u + t->period_frac);
}

/* fine units of degree i (0..N-1) within the period */
static int32_t in_period(const struct tuning_def *t, int32_t i)
{
	if (t->pitch)
		return (int32_t)t->pitch[i];
	/* period256 * i / N without a 64-bit divide: i < N <= 53 */
	int32_t p = period256(t), n = t->degrees;

	return ((p / n) * i + (p % n) * i / n + 128) >> 8;
}

int32_t tuning_degree_fine(const struct tuning_def *t, int32_t degree)
{
	int32_t p = tuning_floor_div(degree, t->degrees);

	return in_period(t, degree - p * t->degrees) + (int32_t)(((int64_t)period256(t) * p + 128) >> 8);
}

static int32_t dist(int32_t a, int32_t b)
{
	return a > b ? a - b : b - a;
}

static int32_t nearest_degree(const struct tuning_def *t, int32_t slot);

int32_t tuning_slot_to_degree(const struct tuning_def *t, int32_t slot)
{
	int32_t n = t->degrees;

	if (n == 12 && t->period == 12 * TUNING_FINE_PER_SEMI)
		return slot;
	if (t->chroma) {
		int32_t oct = tuning_floor_div(slot, 12);
		return oct * n + t->chroma[slot - oct * 12];
	}
	/* the search is up to 53 steps and the grid asks the same few slots
	 * (the root, a fourth, the guitar rows) for every pad: remember them */
	{
		static struct { const struct tuning_def *t; int32_t slot, degree; } cache[16];
		uint8_t h = (uint8_t)(slot & 15);

		if (cache[h].t == t && cache[h].slot == slot)
			return cache[h].degree;
		cache[h].t = t;
		cache[h].slot = slot;
		cache[h].degree = nearest_degree(t, slot);
		return cache[h].degree;
	}
}

/* Pico2Seq's nearest degree, ties to the lower one */
static int32_t nearest_degree(const struct tuning_def *t, int32_t slot)
{
	int32_t n = t->degrees, target, k, base, best, best_d, i;

	target = slot * TUNING_FINE_PER_SEMI;
	/* the period holding the target, by the exact (fractional) period */
	k = tuning_floor_div(target, (int32_t)t->period);
	while (tuning_degree_fine(t, k * n) > target)
		k--;
	while (tuning_degree_fine(t, (k + 1) * n) <= target)
		k++;
	base = tuning_degree_fine(t, k * n);
	best = k * n;
	best_d = dist(target, base);
	for (i = 1; i < n; i++) {
		int32_t d = dist(target, base + in_period(t, i));
		if (d < best_d - TIE_FINE) {
			best_d = d;
			best = k * n + i;
		}
	}
	if (dist(target, tuning_degree_fine(t, (k + 1) * n)) < best_d - TIE_FINE)
		best = (k + 1) * n;
	return best;
}

uint8_t tuning_scale_slots(const struct tuning_def *t)
{
	return t->nscales ? t->nscales : SCALE_TWELVE_COUNT;
}

uint8_t tuning_scale_at(const struct tuning_def *t, uint8_t i)
{
	if (i >= tuning_scale_slots(t))
		return 0xFF;
	return t->nscales ? t->scales[i] : i;
}

uint8_t tuning_offers(const struct tuning_def *t, uint8_t scale)
{
	uint8_t i;

	if (!t->nscales)
		return scale < SCALE_TWELVE_COUNT;
	for (i = 0; i < t->nscales; i++)
		if (t->scales[i] == scale)
			return 1;
	return 0;
}

static int32_t mul_q30(int32_t a, int32_t b)
{
	return (int32_t)(((int64_t)a * b) >> 30);
}

int32_t tuning_a4_fine(uint16_t a4_tenths)
{
	/* 12 log2(x) = (24 / ln 2) atanh(y), y = (x - 1) / (x + 1), |y| < 0.03:
	 * three terms of the series are exact to well under a fine unit.
	 * Integer only (the firmware links no libgcc): 32 x 32 -> 64 products,
	 * 32-bit divides. */
	int32_t a = a4_tenths, num = a - A4_TENTHS_DEFAULT, den = a + A4_TENTHS_DEFAULT;
	int32_t y, y2, y3, y5, at;

	/* y in Q30: Q22 quotient rounded to nearest, then << 8 */
	y = (num * (1 << 22) + (num < 0 ? -den / 2 : den / 2)) / den;
	y *= 256;
	y2 = mul_q30(y, y);
	y3 = mul_q30(y2, y);
	y5 = mul_q30(y3, y2);
	at = y + y3 / 3 + y5 / 5;
	/* 32768 * 24 / ln 2 = 1134576.27 */
	return (int32_t)(((int64_t)at * 1134576 + (1 << 29)) >> 30);
}

uint8_t tuning_is_standard(uint8_t id, uint16_t a4_tenths)
{
	return id == TUNING_DEFAULT_ID && a4_tenths == A4_TENTHS_DEFAULT;
}

int16_t tuning_tonic_note(const struct tuning_def *t, uint8_t root)
{
	return (int16_t)(60 + tuning_slot_to_degree(t, root % 12));
}

uint16_t tuning_pitch79(uint8_t id, uint8_t root, uint16_t a4_tenths, uint8_t note)
{
	const struct tuning_def *t = tuning_get(id);
	int32_t d = (int32_t)note - tuning_tonic_note(t, root);
	int32_t f = (60 + (int32_t)(root % 12)) * TUNING_FINE_PER_SEMI + tuning_degree_fine(t, d) +
	            tuning_a4_fine(a4_tenths);
	int32_t v = (f + 32) >> 6;

	return (uint16_t)(v < 0 ? 0 : v > 0xFFFF ? 0xFFFF : v);
}
