/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "composite.h"

#include <math.h>
#include <string.h>

// The picture as a TV decodes the console's composite signal, after armNES's filter. The console
// encodes each pixel's RGB as luma plus a color carrier, 12 samples to a cycle of the carrier: 8
// samples a pixel on NTSC, 10 on PAL, half that for a hires pixel. The TV takes chroma with a filter
// two carrier cycles long, and luma with a notch filter: the signal less its carrier band, which keeps
// broad detail sharp and leaves the fringes and dot crawl at edges (S-Video's luma has no carrier and
// is only smoothed). PAL flips the V part of the carrier every other line, and the TV averages each
// line's color with the line before, which blurs color vertically and cancels the fringes from luma.
//
// The decoding is linear, so each pixel's part of the output is the sum of a table for each of its
// red, green and blue, indexed by the value, the carrier's phase where the pixel starts and, on PAL,
// the line's flip: 11 output pixels around it, 3 to a column. Each output pixel adds up the 5
// columns around it.

#define TAPS 11
#define BEFORE 4
#define VALUES (TAPS * 3)
// Stored with 6 zeros before and more after, so each of the 5 columns gives a group of 3 output pixels
// 9 values in a row, read as 16 for SIMD
#define PADDED 56
#define PAD 6
// White is 4080; about four pixels add up at an output pixel, which stays within 16 bits
#define ONE 4080
// The triangle's response at the carrier: sinc(1/3) squared
#define NOTCH 0.68389f
#define PI 3.14159265f

// A column's pixel, or the left or right half of a hires pair
enum { WHOLE, LEFT, RIGHT, PARTS };
// The values of red (5 bits), green (6) and blue (5) in a row
enum { CHANNEL_VALUES = 32 + 64 + 32 };

static int16_t ntscKernel[3][PARTS][CHANNEL_VALUES][PADDED];
static int16_t palLuma[2][6][PARTS][CHANNEL_VALUES][PADDED];
static int16_t palChroma[2][6][PARTS][CHANNEL_VALUES][PADDED];
static int built = -1;

// How much of the luma triangle (8 samples wide, area 1) lies before x
static float triangleArea(float x) {
	if (x <= -4) {
		return 0;
	}
	if (x >= 4) {
		return 1;
	}
	return x < 0 ? (x + 4) * (x + 4) / 32 : 1 - (4 - x) * (4 - x) / 32;
}

// Full red, green or blue's part of the decoded picture, as luma and chroma RGB at its 11 output pixels
static void decode(int channel, int part, int phase, bool pal, bool flipped, bool svideo, float luma[VALUES], float chroma[VALUES]) {
	static const float lumaOf[3] = { 0.299f, 0.587f, 0.114f };
	int samples = pal ? 10 : 8;
	int first = part == RIGHT ? samples / 2 : 0, count = part == WHOLE ? samples : samples / 2;
	float y0 = lumaOf[channel], rgb[3] = { 0, 0, 0 };
	rgb[channel] = 1;
	// NTSC's I and Q, PAL's U and V
	float a = pal ? 0.492f * (rgb[2] - y0) : 0.5959f * rgb[0] - 0.2746f * rgb[1] - 0.3213f * rgb[2];
	float b = pal ? 0.877f * (rgb[0] - y0) : 0.2115f * rgb[0] - 0.5227f * rgb[1] + 0.3112f * rgb[2];
	float flip = flipped ? -1.0f : 1.0f;
	for (int j = 0; j < TAPS; j++) {
		float center = (j - BEFORE + 0.5f) * samples / 3;
		float y = 0, i = 0, q = 0;
		for (int k = first; k < first + count; k++) {
			float angle = PI * ((phase + k) % 12) / 6;
			float c = pal ? a * sinf(angle) + flip * b * cosf(angle) : a * cosf(angle) + b * sinf(angle);
			float s = y0 + c;
			float distance = fabsf(k + 0.5f - center);
			if (!svideo) {
				// Luma: the signal smoothed by a triangle 8 samples wide (to the TV's bandwidth), less
				// the carrier band rebuilt from two cycles around (the notch), scaled to what the
				// smoothing leaves of the carrier so a flat color cancels. Each sample holds its level
				// for its whole length, so the filter is summed across it.
				float from = fmaxf(k - center, -12), to = fminf(k + 1 - center, 12);
				float band = to > from ? NOTCH * (sinf(PI * to / 6) - sinf(PI * from / 6)) / (12 * PI / 6) : 0;
				y += s * (triangleArea(k + 1 - center) - triangleArea(k - center) - band);
			} else if (distance < 3) {
				y += y0 / 6;
			}
			if (distance < 12) {
				float carrier = svideo ? c : s;
				i += carrier * cosf(angle) / 12;
				q += carrier * sinf(angle) / 12;
			}
		}
		float out[3];
		if (pal) {
			float u = q, v = flip * i;
			out[0] = 1.140f * v;
			out[1] = -0.395f * u - 0.581f * v;
			out[2] = 2.032f * u;
		} else {
			out[0] = 0.956f * i + 0.621f * q;
			out[1] = -0.272f * i - 0.647f * q;
			out[2] = -1.106f * i + 1.703f * q;
		}
		for (int c = 0; c < 3; c++) {
			luma[j * 3 + c] = y;
			chroma[j * 3 + c] = out[c];
		}
	}
}

// The tables for each value of a channel, the channel at full scaled down
static void store(int16_t out[CHANNEL_VALUES][PADDED], int channel, const float* a, const float* b) {
	static const int base[3] = { 0, 32, 96 }, most[3] = { 31, 63, 31 };
	for (int n = 0; n <= most[channel]; n++) {
		float scale = (float) n / most[channel] * ONE;
		for (int v = 0; v < VALUES; v++) {
			out[base[channel] + n][PAD + v] = (int16_t) lrintf((a[v] + (b ? b[v] : 0)) * scale);
		}
	}
}

static void build(int mode, bool pal) {
	bool svideo = mode == COMPOSITE_SVIDEO;
	float luma[VALUES], chroma[VALUES];
	for (int part = 0; part < PARTS; part++) {
		for (int channel = 0; channel < 3; channel++) {
			for (int p = 0; !pal && p < 3; p++) {
				decode(channel, part, p * 4, false, false, svideo, luma, chroma);
				store(ntscKernel[p][part], channel, luma, chroma);
			}
			for (int p = 0; pal && p < 6; p++) {
				for (int m = 0; m < 2; m++) {
					decode(channel, part, p * 2, true, m, svideo, luma, chroma);
					store(palLuma[m][p][part], channel, luma, NULL);
					store(palChroma[m][p][part], channel, chroma, NULL);
				}
			}
		}
	}
	built = mode * 2 + pal;
}

// A pixel's table: the sum of its red's, green's and blue's, or a hires pair's two halves
static void makeKernel(int16_t* restrict to, int16_t (*table)[CHANNEL_VALUES][PADDED], uint16_t left, uint16_t right) {
	const int16_t* r = table[WHOLE][left >> 11];
	const int16_t* g = table[WHOLE][32 + (left >> 5 & 63)];
	const int16_t* b = table[WHOLE][96 + (left & 31)];
	if (left == right) {
		for (int v = 0; v < PADDED; v++) {
			to[v] = (int16_t) (r[v] + g[v] + b[v]);
		}
		return;
	}
	r = table[LEFT][left >> 11];
	g = table[LEFT][32 + (left >> 5 & 63)];
	b = table[LEFT][96 + (left & 31)];
	const int16_t* r2 = table[RIGHT][right >> 11];
	const int16_t* g2 = table[RIGHT][32 + (right >> 5 & 63)];
	const int16_t* b2 = table[RIGHT][96 + (right & 31)];
	for (int v = 0; v < PADDED; v++) {
		to[v] = (int16_t) (r[v] + g[v] + b[v] + r2[v] + g2[v] + b2[v]);
	}
}

// Each column's table, with 2 blank columns at either end. A column with the pixels and phase of the
// last one made for that phase shares its table.
static const int16_t blank[PADDED];
static const int16_t* lumaAt[COMPOSITE_MOST + 4];
static const int16_t* chromaAt[COMPOSITE_MOST + 4];
static int16_t lumaMade[COMPOSITE_MOST][PADDED], chromaMade[COMPOSITE_MOST][PADDED];

static void columnKernels(const uint16_t* in, unsigned columns, bool hires, unsigned phase, bool pal,
                          int16_t (*luma)[PARTS][CHANNEL_VALUES][PADDED], int16_t (*chroma)[PARTS][CHANNEL_VALUES][PADDED]) {
	uint32_t lastKey[6];
	int lastMade[6] = { -1, -1, -1, -1, -1, -1 };
	int made = 0;
	// A column is 8 samples on NTSC, 10 on PAL, so its phase is one of 3 or 6
	unsigned advance = pal ? 10 : 8, step = pal ? 2 : 4;
	lumaAt[0] = lumaAt[1] = lumaAt[columns + 2] = lumaAt[columns + 3] = blank;
	chromaAt[0] = chromaAt[1] = chromaAt[columns + 2] = chromaAt[columns + 3] = blank;
	for (unsigned x = 0; x < columns; x++) {
		uint16_t left = hires ? in[2 * x] : in[x], right = hires ? in[2 * x + 1] : left;
		uint32_t key = (uint32_t) left | (uint32_t) right << 16;
		unsigned p = phase / step;
		if (lastMade[p] < 0 || lastKey[p] != key) {
			lastKey[p] = key;
			lastMade[p] = made;
			makeKernel(lumaMade[made], luma[p], left, right);
			if (chroma) {
				makeKernel(chromaMade[made], chroma[p], left, right);
			}
			made++;
		}
		lumaAt[x + 2] = lumaMade[lastMade[p]];
		chromaAt[x + 2] = chromaMade[lastMade[p]];
		phase = (phase + advance) % 12;
	}
}

// A group of 3 output pixels: the 9 values from the columns 2 after it to 2 before it, and 7 more that
// the next group overwrites
static inline void gather(const int16_t* const* k, int16_t* out) {
	for (int v = 0; v < 16; v++) {
		out[v] = (int16_t) (k[4][v] + k[3][9 + v] + k[2][18 + v] + k[1][27 + v] + k[0][36 + v]);
	}
}

// A line's values clamped to black and white, then packed into pixels
static void pack(int16_t* values, unsigned count, uint16_t* row) {
	for (unsigned v = 0; v < count * 3; v++) {
		int16_t c = values[v];
		values[v] = c < 0 ? 0 : c > 4095 ? 4095 : c;
	}
	for (unsigned x = 0; x < count; x++) {
		const int16_t* a = &values[x * 3];
		row[x] = (uint16_t) ((a[0] >> 7) << 11 | (a[1] >> 6) << 5 | a[2] >> 7);
	}
}

void compositeLine(int mode, bool pal, unsigned line, unsigned shift, const uint16_t* in, unsigned columns, bool hires,
                   uint16_t* out) {
	if (built != mode * 2 + pal) {
		build(mode, pal);
	}
	static int16_t chromaLines[2][COMPOSITE_MOST * 9 + 16], values[COMPOSITE_MOST * 9 + 16];
	if (columns > COMPOSITE_MOST) {
		columns = COMPOSITE_MOST;
	}
	unsigned at = line + shift;
	if (!pal) {
		// 1364 clocks a line, 6 to a carrier cycle, move the carrier 4 samples a line; the short line
		// every other frame moves it back
		columnKernels(in, columns, hires, (at % 3) * 4, false, ntscKernel, NULL);
		for (unsigned x = 0; x < columns; x++) {
			gather(&lumaAt[x], &values[x * 9]);
		}
		pack(values, columns * 3, out);
		return;
	}
	// 4.8 clocks to a cycle: 2 samples a line
	unsigned flipped = at & 1;
	columnKernels(in, columns, hires, (at * 2) % 12, true, palLuma[flipped], palChroma[flipped]);
	int16_t* current = chromaLines[line & 1];
	const int16_t* previous = line ? chromaLines[(line & 1) ^ 1] : current;
	for (unsigned x = 0; x < columns; x++) {
		gather(&chromaAt[x], &current[x * 9]);
		gather(&lumaAt[x], &values[x * 9]);
	}
	for (unsigned v = 0; v < columns * 9; v++) {
		values[v] = (int16_t) (values[v] + ((current[v] + previous[v]) >> 1));
	}
	pack(values, columns * 3, out);
}
