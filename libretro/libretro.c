/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "libretro.h"
#include "snes.h"
#include "fastppu.h"
#include "composite.h"
#include "options.h"
#include "chips/gb.h"

#include <stdarg.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <strings.h>
#include <time.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__x86_64__)
#include <emmintrin.h>
#endif

// The libretro core: a game, the pad on each port, the picture as the PPU drew it and the sound at the
// DSP's own rate.

static retro_environment_t environment;
static retro_video_refresh_t videoRefresh;
static retro_audio_sample_batch_t audioBatch;
static retro_input_poll_t inputPollFrontend;
static retro_input_state_t inputState;
static retro_log_printf_t logPrintf;

// Why a game didn't load, in the log and on screen.
static void loadError(const char* format, ...) {
	char text[512];
	va_list list;
	va_start(list, format);
	vsnprintf(text, sizeof(text), format, list);
	va_end(list);
	if (logPrintf) {
		logPrintf(RETRO_LOG_ERROR, "armSNES: %s\n", text);
	}
	struct retro_message message = { text, 360 };
	environment(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
}

static u8* game;
static bool loaded;
static bool bitmasks;

// The picture, in RGB565: up to 512 pixels by 239 lines, or twice the lines when interlaced.
enum { OUTPUT_WIDTH = 512, OUTPUT_HEIGHT = 478 };
// Two pictures: the last frame's, which the frontend is given, and the one being drawn, whose lines a
// render thread (or the emulation running on past the frame's end) converts as it goes.
static u16 outputs[2][OUTPUT_WIDTH * OUTPUT_HEIGHT];
static u16* output = outputs[0];
static unsigned outputWidth = 256, outputHeight = 224;
static bool crosshair = true;
static bool outputReady;
// The picture last given to the frontend, and its rows' length.
static const u16* shown;
static u32 shownPitch;

// A pixel of the canvas, four bits of brightness over 15 bits of color, as RGB565. Brightness 0 isn't
// black but much darker than a linear scale would make it, as ares shows it.
static u16 channel[16][32];
// ares's Deep Black Boost: dark colors darker, as a TV's gamma showed them. Every pixel then goes
// through the table.
static bool deepBlackBoost;
static u32 tableAlways;

static void makeColors(void) {
	static const u8 gammaRamp[32] = {
		0x00, 0x01, 0x03, 0x06, 0x0a, 0x0f, 0x15, 0x1c,
		0x24, 0x2d, 0x37, 0x42, 0x4e, 0x5b, 0x69, 0x78,
		0x88, 0x90, 0x98, 0xa0, 0xa8, 0xb0, 0xb8, 0xc0,
		0xc8, 0xd0, 0xd8, 0xe0, 0xe8, 0xf0, 0xf8, 0xff,
	};
	for (int l = 0; l < 16; ++l) {
		double scale = (1.0 + l) / 16.0 * (l ? 1.0 : 0.25);
		for (int c = 0; c < 32; ++c) {
			channel[l][c] = (u16) (scale * (deepBlackBoost ? gammaRamp[c] : c << 3 | c >> 2) + 0.5);
		}
	}
	tableAlways = deepBlackBoost ? 0x78000 : 0;
}

static inline u16 rgb565(u32 pixel) {
	u32 l = pixel >> 15 & 15;
	u32 r = channel[l][pixel & 31];
	u32 g = channel[l][pixel >> 5 & 31];
	u32 b = channel[l][pixel >> 10 & 31];
	return (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3);
}

// The same at full brightness, where the table only moves bits.
static inline u16 rgb565Bright(u32 pixel) {
	return (pixel << 11 & 0xf800) | (pixel << 1 & 0x07c0) | (pixel >> 4 & 0x0020) | (pixel >> 10 & 0x001f);
}

static void convertWide(const u32* from, u16* to) {
	u32 dim = tableAlways, mixed = 0;
	for (u32 n = 0; n < OUTPUT_WIDTH; ++n) {
		dim |= ~from[n] & 0x78000;
		mixed |= from[n] ^ from[0];
		to[n] = rgb565Bright(from[n]);
	}
	if (dim && !mixed) {
		u16 color = rgb565(from[0]);
		for (u32 n = 0; n < OUTPUT_WIDTH; ++n) {
			to[n] = color;
		}
	} else if (dim) {
		for (u32 n = 0; n < OUTPUT_WIDTH; ++n) {
			to[n] = rgb565(from[n]);
		}
	}
}

// A row with no pixel of its own (no hi-res) is written at half the width; it is widened only if
// another row of the frame needs the full width.
static bool rowWide[OUTPUT_HEIGHT];

static void convertRow(const u32* from, u16* to, bool* wide) {
	u32 dim = 0, mixed = 0, pairs = 0;
#if defined(__aarch64__)
	// Four pixel pairs at a time: the left pixel of each converted, the right one compared with it.
	uint32x4_t dims = vdupq_n_u32(0), mixes = dims, pairings = dims;
	const uint32x4_t first = vdupq_n_u32(from[0]);
	for (u32 n = 0; n < OUTPUT_WIDTH / 2; n += 4) {
		uint32x4x2_t two = vld2q_u32(&from[2 * n]);
		uint32x4_t pixel = two.val[0];
		dims = vorrq_u32(dims, vbicq_u32(vdupq_n_u32(0x78000), pixel));
		mixes = vorrq_u32(mixes, veorq_u32(pixel, first));
		pairings = vorrq_u32(pairings, veorq_u32(pixel, two.val[1]));
		uint32x4_t color = vandq_u32(vshlq_n_u32(pixel, 11), vdupq_n_u32(0xf800));
		color = vorrq_u32(color, vandq_u32(vshlq_n_u32(pixel, 1), vdupq_n_u32(0x07c0)));
		color = vorrq_u32(color, vandq_u32(vshrq_n_u32(pixel, 4), vdupq_n_u32(0x0020)));
		color = vorrq_u32(color, vandq_u32(vshrq_n_u32(pixel, 10), vdupq_n_u32(0x001f)));
		vst1_u16(&to[n], vmovn_u32(color));
	}
	dim = vmaxvq_u32(dims);
	mixed = vmaxvq_u32(mixes);
	pairs = vmaxvq_u32(pairings);
#elif defined(__x86_64__)
	__m128i dims = _mm_setzero_si128(), mixes = dims, pairings = dims;
	const __m128i first = _mm_set1_epi32((int) from[0]);
	for (u32 n = 0; n < OUTPUT_WIDTH / 2; n += 4) {
		__m128i a = _mm_loadu_si128((const __m128i*) &from[2 * n]);
		__m128i b = _mm_loadu_si128((const __m128i*) &from[2 * n + 4]);
		__m128i pixel = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(a), _mm_castsi128_ps(b), _MM_SHUFFLE(2, 0, 2, 0)));
		__m128i right = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(a), _mm_castsi128_ps(b), _MM_SHUFFLE(3, 1, 3, 1)));
		dims = _mm_or_si128(dims, _mm_andnot_si128(pixel, _mm_set1_epi32(0x78000)));
		mixes = _mm_or_si128(mixes, _mm_xor_si128(pixel, first));
		pairings = _mm_or_si128(pairings, _mm_xor_si128(pixel, right));
		__m128i color = _mm_and_si128(_mm_slli_epi32(pixel, 11), _mm_set1_epi32(0xf800));
		color = _mm_or_si128(color, _mm_and_si128(_mm_slli_epi32(pixel, 1), _mm_set1_epi32(0x07c0)));
		color = _mm_or_si128(color, _mm_and_si128(_mm_srli_epi32(pixel, 4), _mm_set1_epi32(0x0020)));
		color = _mm_or_si128(color, _mm_and_si128(_mm_srli_epi32(pixel, 10), _mm_set1_epi32(0x001f)));
		// 16 bits from each lane, kept as they are by sign-extending first.
		color = _mm_srai_epi32(_mm_slli_epi32(color, 16), 16);
		_mm_storel_epi64((__m128i*) &to[n], _mm_packs_epi32(color, color));
	}
	dim = (u32) _mm_movemask_epi8(_mm_cmpeq_epi32(dims, _mm_setzero_si128())) != 0xffff;
	mixed = (u32) _mm_movemask_epi8(_mm_cmpeq_epi32(mixes, _mm_setzero_si128())) != 0xffff;
	pairs = (u32) _mm_movemask_epi8(_mm_cmpeq_epi32(pairings, _mm_setzero_si128())) != 0xffff;
#else
	for (u32 n = 0; n < OUTPUT_WIDTH / 2; ++n) {
		u32 pixel = from[2 * n];
		dim |= ~pixel & 0x78000;
		mixed |= pixel ^ from[0];
		pairs |= pixel ^ from[2 * n + 1];
		to[n] = rgb565Bright(pixel);
	}
#endif
	dim |= tableAlways;
	*wide = pairs != 0;
	if (pairs) {
		convertWide(from, to);
	} else if (dim && !mixed) {
		u16 color = rgb565(from[0]);
		for (u32 n = 0; n < OUTPUT_WIDTH / 2; ++n) {
			to[n] = color;
		}
	} else if (dim) {
		for (u32 n = 0; n < OUTPUT_WIDTH / 2; ++n) {
			to[n] = rgb565(from[2 * n]);
		}
	}
}

// Fast mode's renderer converts each line it draws as it goes, for the last frame's crop, and marks
// its row with the frame's stamp; videoFrame converts the others from the canvas. A line outside that
// crop is put in the canvas.
static u32 cropX = 26, cropY = 9, cropHeight = 224;
static u32 rowStamp[CANVAS_HEIGHT];
static u32 stamp = 1;

static void lineDrawn(u32* at, const u32* pixels) {
	u32 row = (u32) (at - ppuCanvas) / CANVAS_WIDTH;
	if (row - cropY < cropHeight && at == &ppuCanvas[row * CANVAS_WIDTH + cropX]) {
		convertRow(pixels, &outputs[stamp & 1][(row - cropY) * OUTPUT_WIDTH], &rowWide[row - cropY]);
		rowStamp[row] = stamp;
	} else {
		memcpy(at, pixels, 512 * sizeof(u32));
		ppuCanvasDirty(at);
	}
}

// Hi-res pixel pairs averaged, as a TV's picture blended them; Merge makes the picture 256 wide.
enum { BLEND_OFF, BLEND_ON, BLEND_MERGE };
static int hiresBlend;

// HD mode 7 and widescreen (Fast and Balanced modes): a frame with mode 7 lines is given hdScale times larger each
// way, those lines drawn finer (fastPpuSetHdSink) and the others' pixels made larger; a widescreen
// frame is given with its columns more each side. Two pictures, as with outputs.
enum { WIDE_COLUMNS = 256 + 2 * FAST_PPU_WIDE_MOST, HD_WIDTH = WIDE_COLUMNS * 8, HD_HEIGHT = 239 * 8 };
static u16 hdOutputs[2][HD_WIDTH * HD_HEIGHT];
static u32 hdRowStamp[CANVAS_HEIGHT];
static u8 hdRowScale[CANVAS_HEIGHT];
static u8 hdScale;
// Widescreen lines drawn at the console's scale, as hires pairs, and whether each has hires pixels.
static u16 wideRows[2][OUTPUT_HEIGHT * WIDE_COLUMNS * 2];
static bool wideRowHires[OUTPUT_HEIGHT];
static u32 wideColumns = 256;

static inline u16 hdColor(u32 pixel) {
	return (pixel & 0x78000) == 0x78000 && !tableAlways ? rgb565Bright(pixel) : rgb565(pixel);
}

// A row of an HD line. At full brightness only bits move, in a loop the compiler makes SIMD.
static void hdRow(const u32* from, u16* to, u32 count) {
	u32 dim = tableAlways;
	for (u32 n = 0; n < count; ++n) {
		dim |= ~from[n] & 0x78000;
	}
	if (!dim) {
		for (u32 n = 0; n < count; ++n) {
			to[n] = rgb565Bright(from[n]);
		}
		return;
	}
	for (u32 n = 0; n < count; ++n) {
		to[n] = rgb565(from[n]);
	}
}

static void hdLineDrawn(u32* at, const u32* block, unsigned scale, unsigned columns) {
	u32 row = (u32) (at - ppuCanvas) / CANVAS_WIDTH;
	if ((scale > 1 && scale != hdScale) || columns != wideColumns || row - cropY >= cropHeight ||
	    at != &ppuCanvas[row * CANVAS_WIDTH + cropX]) {
		return;
	}
	if (scale == 1) {
		u16* to = &wideRows[stamp & 1][(row - cropY) * WIDE_COLUMNS * 2];
		bool hires = false;
		for (u32 n = 0; n < columns; ++n) {
			u32 a = block[2 * n], b = block[2 * n + 1];
			hires |= a != b;
			to[2 * n] = hdColor(a);
			to[2 * n + 1] = hdColor(b);
		}
		wideRowHires[row - cropY] = hires;
	} else {
		u16* to = &hdOutputs[stamp & 1][(row - cropY) * scale * HD_WIDTH];
		for (u32 j = 0; j < scale; ++j) {
			hdRow(block + j * columns * scale, &to[j * HD_WIDTH], columns * scale);
		}
	}
	hdRowScale[row] = (u8) scale;
	hdRowStamp[row] = stamp;
}

// The picture given: the output, or the HD picture; its rows' length in pixels, and how many of its
// rows and columns a console pixel takes.
static const u16* framePicture;
static u32 framePitch = OUTPUT_WIDTH, frameScaleY = 1;
// The console columns across the picture given, and how many of them are widescreen's each side.
static u32 frameColumns = 256, frameExtension;

// The HD or widescreen picture of the frame when it has lines drawn so: the others made larger from
// the output, and set in the middle of a widescreen picture.
static bool hdFrame(u32 y, u32 h) {
	u32 lines = 0, scaled = 0;
	// Hires in a row drawn wide, or in the output a row's middle is taken from.
	bool hires = false;
	for (u32 row = 0; row < h; ++row) {
		bool drawn = hdRowStamp[y + row] == stamp;
		lines += drawn;
		scaled += drawn && hdRowScale[y + row] > 1;
		hires |= (drawn && hdRowScale[y + row] == 1 && wideRowHires[row]) ||
		         ((!drawn || ppuWideSides[y + row]) && outputWidth == OUTPUT_WIDTH);
	}
	u32 n = scaled ? hdScale : 1, columns = wideColumns, extension = (columns - 256) / 2;
	// A widescreen frame is wide whatever it has drawn, so its shape stays.
	if (columns == 256 && (!lines || n < 2)) {
		return false;
	}
	// Hires is kept at the console's scale, unless merged.
	u32 across = n > 1 ? n : hires && hiresBlend != BLEND_MERGE ? 2 : 1;
	u32 width = columns * across;
	u16* hd = hdOutputs[stamp & 1];
	const u16* wide = wideRows[stamp & 1];
	for (u32 row = 0; row < h; ++row) {
		u16* to = &hd[row * n * HD_WIDTH];
		if (hdRowStamp[y + row] == stamp && hdRowScale[y + row] > 1) {
			continue;
		}
		u32 left = extension * across, middle = 256 * across;
		if (hdRowStamp[y + row] == stamp) {
			const u16* from = &wide[row * WIDE_COLUMNS * 2];
			bool blend = wideRowHires[row] && hiresBlend != BLEND_OFF;
			for (u32 x = 0; x < width; ++x) {
				u32 at = x * 2 / across;
				u16 a = from[at & ~1u], b = from[at | 1];
				to[x] = blend ? (u16) ((a & b) + (((a ^ b) & 0xf7de) >> 1)) : from[across == 2 ? at : at & ~1u];
			}
			// A line the S-PPU drew: its middle from the output.
			if (ppuWideSides[y + row]) {
				const u16* out = &output[row * OUTPUT_WIDTH];
				for (u32 x = 0; x < middle; ++x) {
					to[left + x] = out[x * outputWidth / middle];
				}
			}
		} else {
			// A line not drawn wide (the picture's edge or a line left out): the output's, in the middle.
			const u16* from = &output[row * OUTPUT_WIDTH];
			for (u32 x = 0; x < width; ++x) {
				to[x] = x >= left && x < left + middle ? from[(x - left) * outputWidth / middle] : 0;
			}
		}
		for (u32 j = 1; j < n; ++j) {
			memcpy(&to[j * HD_WIDTH], to, width * sizeof(u16));
		}
	}
	framePicture = hd;
	framePitch = HD_WIDTH;
	frameScaleY = n;
	frameColumns = columns;
	frameExtension = extension;
	outputWidth = width;
	outputHeight = h * n;
	return true;
}

// The rows converted as they were drawn, where the last crop put them in the output.
static u16 moved[OUTPUT_WIDTH * OUTPUT_HEIGHT];
static bool movedWide[OUTPUT_HEIGHT];

static bool overscanShown;

// The NTSC/PAL filter, and how much of a line's brightness the scanline below it keeps, in 256ths.
static int filter;
static u32 scanlines = 256;
static bool frameField;
static u16 filtered[COMPOSITE_SCALE * WIDE_COLUMNS * OUTPUT_HEIGHT];

// The frame's picture as a TV decoded it, three pixels to a column: each line twice, the second dimmed
// as the scanline, or an interlaced picture's lines once, each field on its own.
static void filterFrame(void) {
	u32 width = frameColumns * COMPOSITE_SCALE;
	bool hires = outputWidth == 2 * frameColumns, interlace = frameScaleY == 2;
	u32 fields = interlace ? 2 : 1, lines = outputHeight / fields;
	for (u32 field = 0; field < fields; ++field) {
		for (u32 n = 0; n < lines; ++n) {
			u32 row = n * fields + field;
			u16* to = &filtered[(interlace ? row : 2 * n) * width];
			u32 shift = interlace || sys.pal ? field : frameField;
			compositeLine(filter, sys.pal, n, shift, &framePicture[row * framePitch], frameColumns, hires, to);
			if (interlace) {
				continue;
			}
			for (u32 x = 0; x < width; ++x) {
				u32 p = to[x];
				to[width + x] = (u16) (((p >> 11) * scanlines >> 8) << 11 | (((p >> 5) & 63) * scanlines >> 8) << 5 | (p & 31) * scanlines >> 8);
			}
		}
	}
	framePicture = filtered;
	framePitch = width;
	frameScaleY = 2;
	outputWidth = width;
	outputHeight = lines * 2;
}

static void blendRow(u16* row) {
	for (u32 n = 0; n < OUTPUT_WIDTH / 2; ++n) {
		u16 a = row[2 * n], b = row[2 * n + 1];
		u16 mixed = (u16) ((a & b) + (((a ^ b) & 0xf7de) >> 1));
		if (hiresBlend == BLEND_MERGE) {
			row[n] = mixed;
		} else {
			row[2 * n] = row[2 * n + 1] = mixed;
		}
	}
}

// The frontend won't show this frame (run-ahead, fast-forward): nothing is converted from the canvas.
static bool pictureUnseen;

// The part of the canvas a television shows, as ares crops it without overscan.
void videoFrame(const u32* canvas) {
	if (pictureUnseen) {
		stamp++;
		return;
	}
	output = outputs[stamp & 1];
	bool interlace = ppu.state.interlace;
	u32 yScale = interlace ? 2 : 1;
	u32 height = sys.pal ? 288 : 242;
	u32 x = 26;
	u32 y = 9 * yScale;
	u32 h = height - 18;
	// NTSC's overscan lines as PAL shows them: all 239 the game draws.
	if (overscanShown && !sys.pal && ppu.state.overscan) {
		y = yScale;
		h = 239;
	}
	if (sys.pal) {
		x -= 4;
		y += 12 * yScale;
		h -= 31;
		if (!ppu.io.overscan) {
			y += 8 * yScale;
			h -= 15;
		}
	}
	h *= yScale;
	if (h > OUTPUT_HEIGHT) {
		h = OUTPUT_HEIGHT;
	}

	bool sameCrop = canvas == ppuCanvas && x == cropX && y == cropY && h == cropHeight;
	// A new crop: the rows converted as they were drawn are moved to where it puts them (they aren't in
	// the canvas).
	bool move = !sameCrop && canvas == ppuCanvas && x == cropX;
	if (move) {
		memcpy(moved, output, cropHeight * OUTPUT_WIDTH * sizeof(u16));
		memcpy(movedWide, rowWide, cropHeight * sizeof(bool));
	}
	for (u32 row = 0; row < h; ++row) {
		u32 from = y + row - cropY;
		if (move && rowStamp[y + row] == stamp && from < cropHeight) {
			memcpy(&output[row * OUTPUT_WIDTH], &moved[from * OUTPUT_WIDTH], OUTPUT_WIDTH * sizeof(u16));
			rowWide[row] = movedWide[from];
		} else if (!sameCrop || rowStamp[y + row] != stamp) {
			convertRow(&canvas[(y + row) * CANVAS_WIDTH + x], &output[row * OUTPUT_WIDTH], &rowWide[row]);
		}
	}
	bool wide = false;
	for (u32 row = 0; row < h; ++row) {
		if (rowWide[row] && hiresBlend != BLEND_OFF) {
			blendRow(&output[row * OUTPUT_WIDTH]);
		}
		wide |= rowWide[row] && hiresBlend != BLEND_MERGE;
	}
	if (wide) {
		for (u32 row = 0; row < h; ++row) {
			u16* to = &output[row * OUTPUT_WIDTH];
			for (u32 n = OUTPUT_WIDTH / 2; !rowWide[row] && n--;) {
				to[2 * n] = to[2 * n + 1] = to[n];
			}
		}
	}
	outputWidth = wide ? OUTPUT_WIDTH : OUTPUT_WIDTH / 2;
	outputHeight = h;
	framePicture = output;
	framePitch = OUTPUT_WIDTH;
	frameScaleY = yScale;
	frameColumns = 256;
	frameExtension = 0;
	frameField = ppu.counter.field;
	if (sameCrop && !interlace) {
		hdFrame(y, h);
	}
	cropX = x;
	cropY = y;
	cropHeight = h;
	stamp++;
	outputReady = true;
}

// The sound of a frame, at 32040 Hz: about 534 samples, more if a frame runs long.
static s16 sound[2 * 4096];
static size_t soundCount;

void audioSample(s16 left, s16 right) {
	if (soundCount < sizeof(sound) / sizeof(*sound) / 2) {
		sound[soundCount * 2 + 0] = left;
		sound[soundCount * 2 + 1] = right;
		soundCount++;
	}
}

bool cartFirmware(const char* name, u8* data, size_t size) {
	const char* folder = NULL;
	if (!environment(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &folder) || !folder) {
		return false;
	}
	char path[4096];
	snprintf(path, sizeof(path), "%s/%s", folder, name);
	FILE* file = fopen(path, "rb");
	if (!file) {
		return false;
	}
	bool read = fread(data, 1, size, file) == size;
	fclose(file);
	return read;
}

// A whole file from the frontend's system folder, or NULL.
static u8* readFile(const char* path, size_t* size) {
	FILE* file = fopen(path, "rb");
	if (!file) {
		return NULL;
	}
	fseek(file, 0, SEEK_END);
	long length = ftell(file);
	fseek(file, 0, SEEK_SET);
	u8* data = length > 0 ? malloc(length) : NULL;
	if (data && fread(data, 1, length, file) != (size_t) length) {
		free(data);
		data = NULL;
	}
	fclose(file);
	*size = length;
	return data;
}

static u8* systemFile(const char* name, size_t* size) {
	const char* folder = NULL;
	if (!environment(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &folder) || !folder) {
		return NULL;
	}
	char path[4096];
	snprintf(path, sizeof(path), "%s/%s", folder, name);
	return readFile(path, size);
}

// The file beside the game with its name and another extension.
static u8* siblingFile(const char* game, const char* extension, size_t* size) {
	if (!game) {
		return NULL;
	}
	char path[4096];
	snprintf(path, sizeof(path) - 8, "%s", game);
	char* dot = strrchr(path, '.');
	if (!dot) {
		return NULL;
	}
	strcpy(dot + 1, extension);
	return readFile(path, size);
}

static bool hasExtension(const char* path, const char* extension) {
	const char* dot = path ? strrchr(path, '.') : NULL;
	return dot && !strcasecmp(dot + 1, extension);
}

// The cartridge's clock as the frontend keeps it. What the frontend gives back after loading the game
// is applied before the first frame; until then it holds the clock as the game started.
static u8 rtcMemory[16];
static u8 rtcStart[16];
static bool rtcApplied;

// The joypad's buttons are numbered as the pad's shift register sends them. Players are numbered
// across the ports: the first port's pad, or its Super Multitap's four, then the second's.
enum { PLAYERS = 8 };
static u16 held[PLAYERS];
static u8 playerOf[8];
static int mouseX[2], mouseY[2];
static bool mouseLeft[2], mouseRight[2];
// Where each gun points, in the picture's 256 columns and its rows, or below 0 when off it.
static int gunX[2], gunY[2];
static u8 gunButtons[2];
static struct retro_rumble_interface rumble;

enum {
	DEVICE_LR_MULTITAP = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0),
	DEVICE_LR_KEYPAD = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 2),
	DEVICE_LR_TWIN_TAP = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 3),
	DEVICE_LR_SUPER_SCOPE = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_LIGHTGUN, 0),
	DEVICE_LR_JUSTIFIER = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_LIGHTGUN, 1),
	DEVICE_LR_JUSTIFIERS = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_LIGHTGUN, 2),
};

// What each port takes, by the names the frontend's menu and the Port Device options give them; the
// first port takes the first six.
static const struct retro_controller_description portDevices[] = {
	{ "SNES Controller", RETRO_DEVICE_JOYPAD },
	{ "SNES Mouse", RETRO_DEVICE_MOUSE },
	{ "Super Multitap", DEVICE_LR_MULTITAP },
	{ "NTT Data Keypad", DEVICE_LR_KEYPAD },
	{ "Twin Tap", DEVICE_LR_TWIN_TAP },
	{ "None", RETRO_DEVICE_NONE },
	{ "Super Scope", DEVICE_LR_SUPER_SCOPE },
	{ "Justifier", DEVICE_LR_JUSTIFIER },
	{ "Two Justifiers", DEVICE_LR_JUSTIFIERS },
};
// Those of the Port 1 and 2 Device options, while the frontend chooses none itself.
static bool portsChosen;
static int portOptionDevice[2] = { -1, -1 };

static void numberPlayers(void) {
	static const u8 firstTap[4] = { 0, 5, 6, 7 };
	u8 player = 0;
	for (int n = 0; n < (inputDevice[0] == DEVICE_MULTITAP ? 4 : 1); ++n) {
		playerOf[firstTap[n]] = player++;
	}
	for (int n = 0; n < (inputDevice[1] == DEVICE_MULTITAP ? 4 : 1); ++n) {
		playerOf[1 + n] = player++;
	}
}

u16 inputPoll(int pad) {
	return held[playerOf[pad]];
}

// The keypad's keys from the keyboard: the digits, * # and . from either part of it, Backspace or
// Delete for C, Enter for End.
u16 inputPollKeys(int port) {
	static const struct {
		u16 key, pad;
		u8 bit;
	} keys[] = {
		{ RETROK_0, RETROK_KP0, 0 }, { RETROK_1, RETROK_KP1, 1 }, { RETROK_2, RETROK_KP2, 2 }, { RETROK_3, RETROK_KP3, 3 },
		{ RETROK_4, RETROK_KP4, 4 }, { RETROK_5, RETROK_KP5, 5 }, { RETROK_6, RETROK_KP6, 6 }, { RETROK_7, RETROK_KP7, 7 },
		{ RETROK_8, RETROK_KP8, 8 }, { RETROK_9, RETROK_KP9, 9 }, { RETROK_ASTERISK, RETROK_KP_MULTIPLY, KEY_STAR },
		{ RETROK_HASH, RETROK_HASH, KEY_POUND }, { RETROK_PERIOD, RETROK_KP_PERIOD, KEY_POINT },
		{ RETROK_BACKSPACE, RETROK_DELETE, KEY_CLEAR }, { RETROK_RETURN, RETROK_KP_ENTER, KEY_END },
	};
	u16 held = 0;
	for (size_t n = 0; n < sizeof(keys) / sizeof(*keys); ++n) {
		if (inputState(playerOf[port], RETRO_DEVICE_KEYBOARD, 0, keys[n].key) ||
		    inputState(playerOf[port], RETRO_DEVICE_KEYBOARD, 0, keys[n].pad)) {
			held |= 1 << keys[n].bit;
		}
	}
	return held;
}

void inputRumble(int port, u8 strong, u8 weak) {
	if (rumble.set_rumble_state) {
		rumble.set_rumble_state(playerOf[port], RETRO_RUMBLE_STRONG, (u16) (strong * 0xffff / 15));
		rumble.set_rumble_state(playerOf[port], RETRO_RUMBLE_WEAK, (u16) (weak * 0xffff / 15));
	}
}

void inputPollMouse(int port, int* x, int* y, bool* left, bool* right) {
	*x = mouseX[port];
	*y = mouseY[port];
	*left = mouseLeft[port];
	*right = mouseRight[port];
	// A frame's movement is given once.
	mouseX[port] = mouseY[port] = 0;
}

void inputPollGun(int gun, int* x, int* y, u8* buttons) {
	*x = gunX[gun];
	// The rows given are the picture's from the line after the PPU's first, which the shown picture
	// starts below: past the 8 lines without overscan and PAL's border.
	int first = (int) (cropY / frameScaleY) - (ppu.state.overscan ? 0 : 8) - (sys.pal ? 20 : 0) - 1;
	*y = gunY[gun] < 0 ? -1 : gunY[gun] + first;
	*buttons = gunButtons[gun];
}

// Netplay, once it has saved or loaded a state of this game: then L2 and R2 don't fast-forward, as the
// other players would have to wait.
static bool netplaySeen;

static void checkNetplay(void) {
	int context = RETRO_SAVESTATE_CONTEXT_NORMAL;
	netplaySeen |= environment(RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT, &context) && context == RETRO_SAVESTATE_CONTEXT_ROLLBACK_NETPLAY;
}

// R2 held on the first pad fast-forwards at the frontend's top speed, outside netplay, and L2 too
// unless it aims.
static bool fastForwardHeld;

static void setFastForward(bool on) {
	fastForwardHeld = on;
	struct retro_fastforwarding_override override = { .ratio = -1.0f, .fastforward = on, .notification = true, .inhibit_toggle = on };
	environment(RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE, &override);
}

static u16 turboButtons;
static u32 turboPeriod = 4, turboPeriodLR = 4;
static bool allowOpposing;
static bool leftStickDpad = true;

// How the guns aim: the right stick moving the crosshair or pointing it, a light gun, or the mouse.
// The device's motion can add to the stick's.
enum { AIM_STICK_PAN, AIM_STICK, AIM_LIGHT_GUN, AIM_MOUSE };
enum { SENSOR_OFF = -1, SENSOR_ACCELEROMETER, SENSOR_GYROSCOPE };
static int gunAim, aimSpeed = 100, stickInvert;
static int motionSensor = SENSOR_OFF, motionSpeed = 100, motionRotation, motionInvert;
static bool motionWhole = true;

static bool padHeld(unsigned player, unsigned id) {
	return inputState(player, RETRO_DEVICE_JOYPAD, 0, id) != 0;
}

static int rightStick(unsigned player, unsigned axis) {
	return inputState(player, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, axis);
}

// A move this frame in 256ths of a pixel: the push past the dead zone, squared, so small pushes aim
// finely and a full one crosses the screen in about a third of a second at 100%.
static int panStep(int push, int speed) {
	const int dead = 0x1000, size = abs(push);
	if (size <= dead) {
		return 0;
	}
	float amount = (float) (size - dead) / (0x7fff - dead);
	int step = (int) (amount * amount * 12 * 256 * speed / 100);
	return push < 0 ? -step : step;
}

static struct retro_sensor_interface sensors;
static int sensorOn = -1;
static bool motionCentered;
static float motionX, motionY, tiltMiddleRight, tiltMiddleDown;
static int motionLastX, motionLastY;

// Turns the frontend's accelerometer or gyroscope on while a gun aims by the device's motion.
static void useSensor(int sensor) {
	if (sensor >= 0 && !sensors.set_sensor_state && !environment(RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE, &sensors)) {
		memset(&sensors, 0, sizeof(sensors));
	}
	if (!sensors.set_sensor_state || sensor == sensorOn) {
		return;
	}
	if (sensorOn >= 0) {
		sensors.set_sensor_state(0, sensorOn == SENSOR_GYROSCOPE ? RETRO_SENSOR_GYROSCOPE_DISABLE : RETRO_SENSOR_ACCELEROMETER_DISABLE, 60);
	}
	sensorOn = sensor >= 0 && sensors.set_sensor_state(0, sensor == SENSOR_GYROSCOPE ? RETRO_SENSOR_GYROSCOPE_ENABLE : RETRO_SENSOR_ACCELEROMETER_ENABLE, 60) ? sensor : -1;
	motionCentered = false;
}

// The device's motion, in right stick units to add to the stick: how far it's tilted from where it
// was held (smoothed over a few frames, as the accelerometer feels the hands too), or how far it's
// turned, since R3 was last pressed or aiming started. About 20 degrees moves the crosshair a quarter
// of the screen, or with Whole Screen from the middle to the edge, at 100%.
static void deviceMotion(int* x, int* y) {
	*x = *y = 0;
	if (sensorOn < 0) {
		return;
	}
	bool gyro = sensorOn == SENSOR_GYROSCOPE;
	unsigned first = gyro ? RETRO_SENSOR_GYROSCOPE_X : RETRO_SENSOR_ACCELEROMETER_X;
	float sensorX = sensors.get_sensor_input(0, first);
	float sensorY = sensors.get_sensor_input(0, first + 1);
	float sensorZ = sensors.get_sensor_input(0, first + 2);
	if (!gyro && sensorX == 0 && sensorY == 0 && sensorZ == 0) {
		return;
	}
	// The sensor's axes turned to the screen's, for a frontend that passes them on as the device has them.
	static const float turns[4][4] = { { 1, 0, 0, 1 }, { 0, -1, 1, 0 }, { -1, 0, 0, -1 }, { 0, 1, -1, 0 } };
	const float* turn = turns[motionRotation];
	float alongX = turn[0] * sensorX + turn[1] * sensorY, alongY = turn[2] * sensorX + turn[3] * sensorY;
	float range = motionWhole ? 0x7fff : 0x7fff / 2;
	bool center = !motionCentered || padHeld(0, RETRO_DEVICE_ID_JOYPAD_R3);
	motionCentered = true;
	if (gyro) {
		// Turning the device right moves the crosshair right, tipping its top back moves it up.
		float scale = range / (20 * 3.14159265f / 180) * motionSpeed / 100 / 60;
		if (center) {
			motionX = motionY = 0;
		}
		motionX = fmaxf(-range, fminf(range, motionX + (fabsf(alongY) > 0.003f ? alongY : 0) * scale));
		motionY = fmaxf(-range, fminf(range, motionY - (fabsf(alongX) > 0.003f ? alongX : 0) * scale));
	} else {
		// The sensor reads the push that holds the device up, so a side tipped down reads away from it.
		float right = -alongX, down = alongY;
		if (center) {
			tiltMiddleRight = right;
			tiltMiddleDown = down;
		}
		float scale = range * 3 / 9.80665f * motionSpeed / 100;
		motionX += (fmaxf(-range, fminf(range, (right - tiltMiddleRight) * scale)) - motionX) / 3;
		motionY += (fmaxf(-range, fminf(range, (down - tiltMiddleDown) * scale)) - motionY) / 3;
	}
	*x = (int) motionX;
	*y = (int) motionY;
}

// The part of the picture shown, in the console's pixels and lines.
struct Area {
	int left, top, width, height;
};
static struct Area shownArea(void);

// Each gun's crosshair, in 256ths of a pixel, where the stick last pointed it, and a touch's place,
// which the first gun aims at for a moment after, while the game looks for the light.
static int crossX[2] = { 128 << 8, 128 << 8 }, crossY[2] = { 112 << 8, 112 << 8 };
static int stickX[2] = { -1, -1 }, stickY[2];
static int touchX, touchY, touchFrames;

// Fine aim: the crosshair's speed in 256ths, from all of it down to a quarter as L2 is pulled.
static int fineAim(unsigned player) {
	int pull = inputState(player, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_BUTTON, RETRO_DEVICE_ID_JOYPAD_L2);
	if (!pull && padHeld(player, RETRO_DEVICE_ID_JOYPAD_L2)) {
		pull = 0x7fff;
	}
	return 256 - pull * 192 / 0x7fff;
}

// Where a gun aims and what it presses. The first gun takes the first two players' pads, mice and
// light guns; the second Justifier the third's.
static void aimGun(int gun) {
	struct Area area = shownArea();
	// The crosshair stays on the console's 256 columns; a light gun or a touch on widescreen's shoots off
	// the screen.
	int pictureLeft = area.left << 8;
	int left = area.left > 0 ? area.left << 8 : 0, right = (area.left + area.width - 1 < 255 ? area.left + area.width - 1 : 255) << 8;
	int top = area.top << 8, bottom = (area.top + area.height - 1) << 8;
	unsigned players[2] = { gun ? 2u : 0u, gun ? 2u : 1u };
	bool trigger = false, other = false, turbo = false, pause = false, away = false;
	if (gunAim == AIM_LIGHT_GUN) {
		unsigned port = gun + 1;
		int x = inputState(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X);
		int y = inputState(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y);
		bool reload = inputState(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_RELOAD);
		away = reload || inputState(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN);
		trigger = reload || inputState(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER);
		other = inputState(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_AUX_A);
		turbo = inputState(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_AUX_B);
		pause = inputState(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_START);
		crossX[gun] = pictureLeft + (x + 0x8000) * area.width / 0x10000 * 256;
		crossY[gun] = top + (y + 0x8000) * area.height / 0x10000 * 256;
		away |= crossX[gun] < left || crossX[gun] > right + 255;
	} else {
		int fine = fineAim(players[0]), speed = aimSpeed * fine / 256;
		int pointX = -1, pointY = 0;
		for (int n = 0; n < 2; ++n) {
			unsigned player = players[n];
			if (n && player == players[0]) {
				break;
			}
			trigger |= padHeld(player, RETRO_DEVICE_ID_JOYPAD_R);
			other |= padHeld(player, RETRO_DEVICE_ID_JOYPAD_L);
			turbo |= padHeld(player, RETRO_DEVICE_ID_JOYPAD_SELECT);
			pause |= padHeld(player, RETRO_DEVICE_ID_JOYPAD_START);
			if (gunAim == AIM_MOUSE) {
				// Half a pixel a count at 100%.
				crossX[gun] += inputState(player, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X) * 128 * speed / 100;
				crossY[gun] += inputState(player, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y) * 128 * speed / 100;
				trigger |= inputState(player, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT) != 0;
				other |= inputState(player, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT) != 0;
				pause |= inputState(player, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_MIDDLE) != 0;
				continue;
			}
			int x = rightStick(player, RETRO_DEVICE_ID_ANALOG_X);
			int y = rightStick(player, RETRO_DEVICE_ID_ANALOG_Y);
			int mx = 0, my = 0;
			if (!gun && !n) {
				deviceMotion(&mx, &my);
			}
			x = stickInvert & 1 ? -x : x;
			y = stickInvert & 2 ? -y : y;
			mx = motionInvert & 1 ? -mx : mx;
			my = motionInvert & 2 ? -my : my;
			if (gunAim == AIM_STICK) {
				// The stick points the crosshair, from the first pad whose stick is off the middle.
				x = x + mx < -0x7fff ? -0x7fff : x + mx > 0x7fff ? 0x7fff : x + mx;
				y = y + my < -0x7fff ? -0x7fff : y + my > 0x7fff ? 0x7fff : y + my;
				if (!n || x || y) {
					pointX = left + (x + 0x8000) * ((right - left) / 256 + 1) / 0x10000 * 256;
					pointY = top + (y + 0x8000) * area.height / 0x10000 * 256;
				}
			} else {
				// Up to 12 pixels a frame at 100%, and the motion's change since the last frame.
				crossX[gun] += panStep(x, speed);
				crossY[gun] += panStep(y, speed);
				if (!gun && !n) {
					crossX[gun] += (int) ((s64) (mx - motionLastX) * (right - left) / 0xffff * fine / 256);
					crossY[gun] += (int) ((s64) (my - motionLastY) * (bottom - top) / 0xffff * fine / 256);
				}
			}
			if (!gun && !n) {
				motionLastX = mx;
				motionLastY = my;
			}
		}
		// With fine aim, the stick's moves count for less from where the crosshair is.
		if (pointX >= 0) {
			if (fine < 256 && stickX[gun] >= 0) {
				crossX[gun] += (pointX - stickX[gun]) * fine / 256;
				crossY[gun] += (pointY - stickY[gun]) * fine / 256;
			} else {
				crossX[gun] = pointX;
				crossY[gun] = pointY;
			}
			stickX[gun] = pointX;
			stickY[gun] = pointY;
		}
		if (gunAim != AIM_MOUSE && padHeld(players[0], RETRO_DEVICE_ID_JOYPAD_R3)) {
			crossX[gun] = (left + right) / 2;
			crossY[gun] = (top + bottom) / 2;
		}
	}
	// A Justifier's other button is a shot off the screen, its reload.
	if (inputDevice[1] != DEVICE_SUPER_SCOPE && other) {
		trigger = away = true;
		other = false;
	}
	// A touch aims there and fires.
	if (!gun && inputState(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED)) {
		touchX = inputState(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
		touchY = inputState(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
		touchFrames = 30;
		trigger = true;
		away = false;
	}
	if (!gun && touchFrames) {
		touchFrames--;
		crossX[gun] = pictureLeft + (touchX + 0x8000) * area.width / 0x10000 * 256;
		crossY[gun] = top + (touchY + 0x8000) * area.height / 0x10000 * 256;
		away = crossX[gun] < left || crossX[gun] > right + 255;
	}
	crossX[gun] = crossX[gun] < left ? left : crossX[gun] > right ? right : crossX[gun];
	crossY[gun] = crossY[gun] < top ? top : crossY[gun] > bottom ? bottom : crossY[gun];
	gunX[gun] = away ? -1 : crossX[gun] >> 8;
	gunY[gun] = away ? -1 : crossY[gun] >> 8;
	gunButtons[gun] = (u8) (trigger << GUN_TRIGGER | other << GUN_CURSOR | turbo << GUN_TURBO | pause << GUN_PAUSE);
}

// A mouse moved by the mouse, by the right stick (with R and L for its buttons), or by dragging on the
// touchscreen like a touchpad, where a tap clicks.
static int mouseSub[2][2];
static bool touchHeld;
static int touchLastX, touchLastY, touchHeldFrames, touchMoved, tapFrames;

static void moveMouse(int port) {
	unsigned player = playerOf[port];
	int x = inputState(player, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
	int y = inputState(player, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
	bool left = inputState(player, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT) || padHeld(player, RETRO_DEVICE_ID_JOYPAD_R);
	bool right = inputState(player, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT) || padHeld(player, RETRO_DEVICE_ID_JOYPAD_L);
	// The stick and touch drags move it in 256ths of a count, kept between frames.
	int sx = rightStick(player, RETRO_DEVICE_ID_ANALOG_X), sy = rightStick(player, RETRO_DEVICE_ID_ANALOG_Y);
	sx = stickInvert & 1 ? -sx : sx;
	sy = stickInvert & 2 ? -sy : sy;
	int speed = aimSpeed * fineAim(player) / 256;
	mouseSub[port][0] += panStep(sx, speed);
	mouseSub[port][1] += panStep(sy, speed);
	if (port == 0 || inputDevice[0] != DEVICE_MOUSE) {
		struct Area area = shownArea();
		bool pressed = inputState(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED);
		int tx = inputState(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
		int ty = inputState(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
		if (pressed && touchHeld) {
			int dx = (tx - touchLastX) * area.width / 0x100 * speed / 100;
			int dy = (ty - touchLastY) * area.height / 0x100 * speed / 100;
			mouseSub[port][0] += dx;
			mouseSub[port][1] += dy;
			touchMoved += abs(dx) + abs(dy);
			touchHeldFrames++;
		} else if (pressed) {
			touchHeldFrames = touchMoved = 0;
		} else if (touchHeld && touchHeldFrames < 15 && touchMoved < 4 * 256) {
			tapFrames = 4;
		}
		touchHeld = pressed;
		touchLastX = tx;
		touchLastY = ty;
		if (tapFrames) {
			tapFrames--;
			left = true;
		}
	}
	x += mouseSub[port][0] / 256;
	y += mouseSub[port][1] / 256;
	mouseSub[port][0] %= 256;
	mouseSub[port][1] %= 256;
	mouseX[port] += x;
	mouseY[port] += y;
	mouseLeft[port] = left;
	mouseRight[port] = right;
}

// A gun or a mouse is plugged in: then L2 aims finer, and only R2 fast-forwards.
static bool aiming(void) {
	return inputGuns || inputDevice[0] == DEVICE_MOUSE || inputDevice[1] == DEVICE_MOUSE;
}

// L3 and R3 are L and R with turbo, and the right stick A, B, X and Y with turbo, each pushed toward
// where it sits on the pad, unless the stick aims a gun or the mouse.
static u16 turboHeld(unsigned player) {
	if (aiming()) {
		return 0;
	}
	int x = rightStick(player, RETRO_DEVICE_ID_ANALOG_X), y = rightStick(player, RETRO_DEVICE_ID_ANALOG_Y);
	return (u16) (padHeld(player, RETRO_DEVICE_ID_JOYPAD_L3) << RETRO_DEVICE_ID_JOYPAD_L |
	              padHeld(player, RETRO_DEVICE_ID_JOYPAD_R3) << RETRO_DEVICE_ID_JOYPAD_R |
	              (y < -0x4000) << RETRO_DEVICE_ID_JOYPAD_X | (x > 0x4000) << RETRO_DEVICE_ID_JOYPAD_A |
	              (y > 0x4000) << RETRO_DEVICE_ID_JOYPAD_B | (x < -0x4000) << RETRO_DEVICE_ID_JOYPAD_Y);
}

static void readPads(void) {
	inputPollFrontend();
	numberPlayers();
	bool fastForward = !netplaySeen && (padHeld(0, RETRO_DEVICE_ID_JOYPAD_R2) || (!aiming() && padHeld(0, RETRO_DEVICE_ID_JOYPAD_L2)));
	if (fastForward != fastForwardHeld) {
		setFastForward(fastForward);
	}
	for (unsigned player = 0; player < PLAYERS; ++player) {
		u16 buttons = 0;
		if (bitmasks) {
			buttons = inputState(player, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
		} else {
			for (unsigned id = RETRO_DEVICE_ID_JOYPAD_B; id <= RETRO_DEVICE_ID_JOYPAD_R; ++id) {
				buttons |= (inputState(player, RETRO_DEVICE_JOYPAD, 0, id) ? 1 : 0) << id;
			}
		}
		if (leftStickDpad) {
			int x = inputState(player, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
			int y = inputState(player, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
			buttons |= (y < -0x4000) << RETRO_DEVICE_ID_JOYPAD_UP | (y > 0x4000) << RETRO_DEVICE_ID_JOYPAD_DOWN |
			           (x < -0x4000) << RETRO_DEVICE_ID_JOYPAD_LEFT | (x > 0x4000) << RETRO_DEVICE_ID_JOYPAD_RIGHT;
		}
		// Turbo buttons are held for the first half of each period, counted in the console's frames so
		// run-ahead and netplay see the same presses.
		const u16 lr = 1 << RETRO_DEVICE_ID_JOYPAD_L | 1 << RETRO_DEVICE_ID_JOYPAD_R;
		u16 turbo = turboHeld(player);
		if (sys.frames % turboPeriod >= (turboPeriod + 1) / 2) {
			buttons &= ~turboButtons;
			turbo &= lr;
		}
		if (sys.frames % turboPeriodLR >= (turboPeriodLR + 1) / 2) {
			turbo &= ~lr;
		}
		buttons |= turbo;
		if (!allowOpposing) {
			const u16 upDown = 1 << RETRO_DEVICE_ID_JOYPAD_UP | 1 << RETRO_DEVICE_ID_JOYPAD_DOWN;
			const u16 leftRight = 1 << RETRO_DEVICE_ID_JOYPAD_LEFT | 1 << RETRO_DEVICE_ID_JOYPAD_RIGHT;
			buttons &= (buttons & upDown) == upDown ? ~upDown : 0xffff;
			buttons &= (buttons & leftRight) == leftRight ? ~leftRight : 0xffff;
		}
		held[player] = buttons & 0xfff;
	}
	for (int port = 0; port < 2; ++port) {
		if (inputDevice[port] == DEVICE_MOUSE) {
			moveMouse(port);
		}
	}
	useSensor(inputGuns && (gunAim == AIM_STICK || gunAim == AIM_STICK_PAN) ? motionSensor : SENSOR_OFF);
	if (!inputGuns) {
		return;
	}
	for (int gun = 0; gun < 2; ++gun) {
		gunX[gun] = gunY[gun] = -1;
		gunButtons[gun] = 0;
		if (!gun || inputDevice[1] == DEVICE_JUSTIFIERS) {
			aimGun(gun);
		}
	}
}

// A small cross where each gun points, its pixels inverted to show on any picture, as armNES draws it.
static void drawCrosshairs(void) {
	u16* picture = (u16*) framePicture;
	u32 xScale = outputWidth / frameColumns, yScale = frameScaleY;
	int lines = (int) (outputHeight / yScale);
	for (int gun = 0; gun < 2; ++gun) {
		if (gunX[gun] < 0 || gunX[gun] > 255 || gunY[gun] < 0) {
			continue;
		}
		int cx = gunX[gun] + (int) frameExtension, cy = gunY[gun];
		for (int d = -4; d <= 4; ++d) {
			int points[2][2] = { { cx + d, cy }, { cx, cy + d } };
			for (int i = d ? 0 : 1; i < 2; ++i) {
				int x = points[i][0], y = points[i][1];
				if (x < 0 || x >= (int) frameColumns || y < 0 || y >= lines) {
					continue;
				}
				for (u32 row = 0; row < yScale; ++row) {
					for (u32 s = 0; s < xScale; ++s) {
						picture[(y * yScale + row) * framePitch + x * xScale + s] ^= 0xffff;
					}
				}
			}
		}
	}
}

#define CLOCK_VALUES \
	{ { "25%", NULL }, { "50%", NULL }, { "75%", NULL }, { "100%", NULL }, { "125%", NULL }, { "150%", NULL }, \
	  { "175%", NULL }, { "200%", NULL }, { "225%", NULL }, { "250%", NULL }, { "275%", NULL }, { "300%", NULL }, \
	  { "325%", NULL }, { "350%", NULL }, { "375%", NULL }, { "400%", NULL }, { NULL, NULL } }

#define VOLUME_VALUES \
	{ { "0%", NULL }, { "10%", NULL }, { "20%", NULL }, { "30%", NULL }, { "40%", NULL }, { "50%", NULL }, { "60%", NULL }, \
	  { "70%", NULL }, { "80%", NULL }, { "90%", NULL }, { "100%", NULL }, { NULL, NULL } }
#define VOICE_OPTION(n) \
	{ "armsnes_volume_voice" #n, "Voice " #n " Volume", NULL, \
	  (n) == 1 ? "Each of the sound chip's eight voices. A voice's echo stays as loud." : NULL, NULL, "audio", VOLUME_VALUES, "100%" }

// A layer's switch under Layer Selection, and a voice's or sound source's under Channel Selection
#define LAYER_OPTION(key, name, info) \
	{ "armsnes_layer_" key, name, NULL, info, NULL, "video", { { "Shown", NULL }, { "Hidden", NULL }, { NULL, NULL } }, "Shown" }
#define CHANNEL_OPTION(key, name) \
	{ "armsnes_channel_" key, name, NULL, NULL, NULL, "audio", { { "On", NULL }, { "Off", NULL }, { NULL, NULL } }, "On" }

// What's plugged into a port, for frontends without a controller menu of their own
#define PORT_OPTION(n, a, b, c, d, e, f, g, h, i) \
	{ "armsnes_port" n, "Port " n " Device", NULL, \
	  "What's plugged into the port. Shown where the frontend has no controller menu of its own.", NULL, "input", \
	  { { a, NULL }, { b, NULL }, { c, NULL }, { d, NULL }, { e, NULL }, { f, NULL }, { g, NULL }, { h, NULL }, { i, NULL }, \
	    { NULL, NULL } }, "SNES Controller" }

static struct retro_core_option_v2_definition optionDefinitions[] = {
	{ "armsnes_mode", "Mode", NULL,
		"Exact runs as ares does, clock for clock. Balanced gives the same results with coarser timing, "
		"faster. Fast draws the picture with its own renderer, fastest; its timing differences are too small "
		"to see. Saved states load in any mode.",
		NULL, "system", { { "Fast", NULL }, { "Balanced", NULL }, { "Exact", NULL }, { NULL, NULL } }, "Fast" },
	{ "armsnes_threads", "Fast Mode Threads", NULL,
		"Fast mode's sound and picture on threads of their own, as the device's cores allow. Picture Only "
		"keeps the sound on the main thread, Off keeps both there, for devices whose cores are busy.",
		NULL, "system", { { "All", NULL }, { "Picture Only", NULL }, { "Off", NULL }, { NULL, NULL } }, "All" },
	{ "armsnes_frame_skip", "Frame Skip", NULL,
		"Skips drawing frames to keep up: Auto when the sound is about to run short, or this many after each "
		"frame shown. Fast mode only, not in netplay.",
		NULL, "system", { { "Off", NULL }, { "Auto", NULL }, { "1", NULL }, { "2", NULL }, { "3", NULL }, { NULL, NULL } }, "Off" },
	{ "armsnes_region", "Region", NULL,
		"The console's region: NTSC at 60 frames a second, PAL at 50. Auto follows the game's header. Changing "
		"it restarts the game.",
		NULL, "system", { { "Auto", NULL }, { "NTSC", NULL }, { "PAL", NULL }, { NULL, NULL } }, "Auto" },
	{ "armsnes_power_on_ram", "Power-On RAM", NULL,
		"The work RAM as the console is switched on: all zero, which some unlicensed games and prototypes "
		"need, or ares's stripes of two values.",
		NULL, "system", { { "Cleared", NULL }, { "Patterned", NULL }, { NULL, NULL } }, "Cleared" },
	{ "armsnes_cpu_clock", "CPU Clock", NULL,
		"The console's CPU speed. Above 100% less slowdown; above 200% some games don't start.",
		NULL, "system", CLOCK_VALUES, "100%" },
	{ "armsnes_superfx_clock", "Super FX Clock", NULL,
		"The Super FX chip's speed, for its games: above 100% a smoother frame rate.",
		NULL, "system", CLOCK_VALUES, "100%" },
	{ "armsnes_sa1_clock", "SA-1 Clock", NULL,
		"The SA-1 chip's speed, for its games: above 100% less slowdown.",
		NULL, "system", CLOCK_VALUES, "100%" },
	{ "armsnes_aspect", "Aspect Ratio", NULL,
		"The console's own pixel shape (8:7, about 11:8 on PAL), 4:3 as a TV showed it, or square pixels.",
		NULL, "video", { { "8:7 PAR", "Console pixels (8:7)" }, { "4:3", NULL }, { "1:1", "Square pixels" }, { NULL, NULL } }, "8:7 PAR" },
	{ "armsnes_overscan", "Overscan Lines", NULL,
		"The 15 lines more a game in overscan mode draws, which an NTSC TV hid. PAL shows them always.",
		NULL, "video", { { "Hidden", NULL }, { "Shown", NULL }, { NULL, NULL } }, "Hidden" },
	{ "armsnes_crop_vertical", "Crop Top and Bottom", NULL,
		"Pixels hidden at the top and bottom.",
		NULL, "video", { { "0", NULL }, { "4", NULL }, { "8", NULL }, { "12", NULL }, { "16", NULL }, { NULL, NULL } }, "0" },
	{ "armsnes_crop_horizontal", "Crop Sides", NULL,
		"Pixels hidden at the left and right edges.",
		NULL, "video", { { "0", NULL }, { "4", NULL }, { "8", NULL }, { "12", NULL }, { "16", NULL }, { NULL, NULL } }, "0" },
	{ "armsnes_sgb_border", "Super Game Boy Border", NULL,
		"The border round a Game Boy game, or the Game Boy's screen alone.",
		NULL, "video", { { "Shown", NULL }, { "Hidden", NULL }, { NULL, NULL } }, "Shown" },
	{ "armsnes_deep_black_boost", "Deep Black Boost", NULL,
		"Dark colors darker, as a TV's gamma showed them (ares's option of the name).",
		NULL, "video", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armsnes_hires_blend", "Hi-Res Blending", NULL,
		"The two halves of each hi-res pixel averaged, as a TV blended them: some games draw see-through "
		"effects that way. Merge also makes the picture 256 pixels wide, for small screens.",
		NULL, "video", { { "Off", NULL }, { "On", NULL }, { "Merge", NULL }, { NULL, NULL } }, "Off" },
	{ "armsnes_filter", "NTSC/PAL Filter", NULL,
		"The picture as a TV decoded the console's video signal: Composite, with its color fringes and blended "
		"dithering, or the sharper S-Video. Takes more CPU time; HD Mode 7 is off with it.",
		NULL, "video", { { "Off", NULL }, { "Composite", NULL }, { "S-Video", NULL }, { NULL, NULL } }, "Off" },
	{ "armsnes_scanlines", "Scanlines", NULL,
		"Dark rows between the filtered picture's lines, as a CRT's beam left them. Best with integer scaling "
		"or a smooth filter in the frontend.",
		NULL, "video", { { "Off", NULL }, { "Light", NULL }, { "Medium", NULL }, { "Dark", NULL }, { NULL, NULL } }, "Off" },
	{ "armsnes_hd_mode7", "HD Mode 7", NULL,
		"Mode 7's rotated and scaled layer drawn 2 to 8 times finer each way, the rest of the picture as it "
		"is. Each step takes much more time. Fast and Balanced modes (Balanced draws mode 7 lines with Fast "
		"mode's renderer, from each line's registers as it starts).",
		NULL, "video", { { "Off", NULL }, { "2x", NULL }, { "3x", NULL }, { "4x", NULL }, { "5x", NULL }, { "6x", NULL }, { "7x", NULL },
		  { "8x", NULL }, { NULL, NULL } }, "Off" },
	{ "armsnes_widescreen", "Widescreen", NULL,
		"Columns added each side to fill a wider screen, of the ratio picked with the Aspect Ratio option's "
		"pixel shape. The console draws 256: past them, a layer shows what the game left in video memory, "
		"which looks right on mode 7's plane, often wrong elsewhere unless the game is patched for it. "
		"Fast and Balanced modes (Balanced draws the sides with Fast mode's renderer, from each line's "
		"registers as it starts); not with the Super Game Boy or interlaced pictures.",
		NULL, "video", { { "Off", NULL }, { "16:10", NULL }, { "16:9", NULL }, { "2:1", NULL }, { "21:9", NULL }, { NULL, NULL } }, "Off" },
	{ "armsnes_widescreen_layers", "Widescreen Layers", NULL,
		"Mode 7 only keeps other lines (and status bars) at the console's width, with black at the sides. All "
		"widens every line, for games patched for widescreen.",
		NULL, "video", { { "Mode 7 only", NULL }, { "All", NULL }, { NULL, NULL } }, "Mode 7 only" },
	{ "armsnes_widescreen_sprites", "Widescreen Sprites", NULL,
		"Sprites past the console's 256 columns: hidden, as the console has them, or shown. Games often leave "
		"sprites there they don't expect to be seen.",
		NULL, "video", { { "Hidden", NULL }, { "Shown", NULL }, { NULL, NULL } }, "Hidden" },
	{ "armsnes_sprite_limit", "Sprite Limit", NULL,
		"The console shows 32 sprites and 34 sprite tiles on a line, which games turn into flicker or gaps. "
		"No limit shows them all; games still see the console's limit. Fast mode only.",
		NULL, "video", { { "32 per line", NULL }, { "No limit", NULL }, { NULL, NULL } }, "32 per line" },
	{ "armsnes_layer_selection", "Layer Selection", NULL,
		"Shows switches below that hide the backgrounds, the sprites or color math, to see one on its own: for "
		"ripping graphics or a closer look. Only the picture changes; the game plays on the same, so run-ahead "
		"and states aren't affected. Off shows everything.",
		NULL, "video", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	LAYER_OPTION("bg1", "Background 1", "Which background is the playfield, the scenery or the status bar differs from game to game."),
	LAYER_OPTION("bg2", "Background 2", NULL),
	LAYER_OPTION("bg3", "Background 3", NULL),
	LAYER_OPTION("bg4", "Background 4", NULL),
	LAYER_OPTION("sprites", "Sprites", "Characters, enemies and shots."),
	LAYER_OPTION("color_math", "Color Math", "Transparency and the other blends of the two screens. Hidden shows the main screen alone."),
	{ "armsnes_interpolation", "Interpolation", NULL,
		"How the sound chip smooths its samples: Gaussian as the console does, or sharper. Only what is heard "
		"changes; the echo keeps the console's.",
		NULL, "audio", { { "Gaussian", NULL }, { "Cubic", NULL }, { "Linear", NULL }, { "None", NULL }, { NULL, NULL } }, "Gaussian" },
	{ "armsnes_volume_gameboy", "Game Boy Volume", NULL, "The Super Game Boy's Game Boy sound.", NULL, "audio", VOLUME_VALUES, "100%" },
	{ "armsnes_volume_msu1", "MSU-1 Volume", NULL, "An MSU-1 game's music and sound tracks.", NULL, "audio", VOLUME_VALUES, "100%" },
	VOICE_OPTION(1), VOICE_OPTION(2), VOICE_OPTION(3), VOICE_OPTION(4),
	VOICE_OPTION(5), VOICE_OPTION(6), VOICE_OPTION(7), VOICE_OPTION(8),
	{ "armsnes_channel_selection", "Channel Selection", NULL,
		"Shows a switch below for each voice and sound source, to hear one or a few on their own: for ripping "
		"music or a closer listen. A channel switched off keeps its volume for when it's back on, and a voice's "
		"echo still plays, as the game's memory holds it. Only the sound changes; the game plays on the same, "
		"so netplay, run-ahead and states aren't affected. Off plays everything.",
		NULL, "audio", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	CHANNEL_OPTION("gameboy", "Game Boy"), CHANNEL_OPTION("msu1", "MSU-1"),
	CHANNEL_OPTION("voice1", "Voice 1"), CHANNEL_OPTION("voice2", "Voice 2"), CHANNEL_OPTION("voice3", "Voice 3"),
	CHANNEL_OPTION("voice4", "Voice 4"), CHANNEL_OPTION("voice5", "Voice 5"), CHANNEL_OPTION("voice6", "Voice 6"),
	CHANNEL_OPTION("voice7", "Voice 7"), CHANNEL_OPTION("voice8", "Voice 8"),
	{ "armsnes_turbo_buttons", "Turbo Buttons", NULL,
		"Buttons that press and release over and over while held. L3, R3 and the right stick are turbo L, R "
		"and A, B, X and Y as well.",
		NULL, "input", { { "Off", NULL }, { "A", NULL }, { "B", NULL }, { "X", NULL }, { "Y", NULL }, { "A and B", NULL },
		{ "X and Y", NULL }, { "A, B, X and Y", NULL }, { NULL, NULL } }, "Off" },
	{ "armsnes_turbo_period", "Turbo Button Period", NULL,
		"Frames per press while a turbo A, B, X or Y is held.",
		NULL, "input", { { NULL, NULL } }, "4" },
	{ "armsnes_turbo_period_lr", "Turbo L and R Period", NULL,
		"Frames per press while L3 or R3 is held.",
		NULL, "input", { { NULL, NULL } }, "4" },
	{ "armsnes_opposing", "Allow Opposing Directions", NULL,
		"Left and right (or up and down) held together, which a real pad can't do. Some games glitch.",
		NULL, "input", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armsnes_left_stick_dpad", "Left Stick as D-Pad", NULL, "The left stick presses the D-pad too.",
		NULL, "input", { { "On", NULL }, { "Off", NULL }, { NULL, NULL } }, "On" },
	PORT_OPTION("1", "SNES Controller", "SNES Mouse", "Super Multitap", "NTT Data Keypad", "Twin Tap", "None", NULL, NULL, NULL),
	PORT_OPTION("2", "SNES Controller", "SNES Mouse", "Super Multitap", "NTT Data Keypad", "Twin Tap", "None", "Super Scope",
		"Justifier", "Two Justifiers"),
	{ "armsnes_rumble", "Rumble", NULL,
		"The pads' rumble, for homebrew that sends it: no Super Nintendo pad had any.",
		NULL, "input", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armsnes_gun_aim", "Light Gun Aim", "Aim",
		"Right stick: the stick moves the crosshair, or points it where the stick points; R fires, L is the "
		"Super Scope's Cursor button or the Justifier's shot off the screen, Select the Super Scope's Turbo "
		"switch, Start pauses, R3 centers. Light gun: a light gun or the mouse pointer aims. Mouse: the mouse "
		"moves the crosshair, its buttons fire, press the other button and pause. Touching the screen always "
		"aims and fires.",
		NULL, "aim", { { "Right stick (pan)", "Right stick (moves)" }, { "Right stick (absolute)", "Right stick (points)" },
		{ "Light gun", NULL }, { "Mouse", NULL }, { NULL, NULL } }, "Right stick (pan)" },
	{ "armsnes_crosshair", "Light Gun Crosshair", "Crosshair",
		"A cross where the Super Scope or Justifier points.",
		NULL, "aim", { { "On", NULL }, { "Off", NULL }, { NULL, NULL } }, "On" },
	{ "armsnes_aim_sensitivity", "Aim Sensitivity", "Sensitivity",
		"How fast the stick, the mouse and touch drags move the crosshair or the SNES Mouse. Holding L2 aims "
		"finer the further it's pulled.",
		NULL, "aim", { { "25%", NULL }, { "50%", NULL }, { "75%", NULL }, { "100%", NULL }, { "150%", NULL },
		{ "200%", NULL }, { "300%", NULL }, { NULL, NULL } }, "100%" },
	{ "armsnes_stick_invert", "Aim Stick Invert", "Stick Invert",
		"Turns the right stick's aim around, side to side, up and down, or both.",
		NULL, "aim", { { "Off", NULL }, { "Horizontal", NULL }, { "Vertical", NULL }, { "Both", NULL }, { NULL, NULL } }, "Off" },
	{ "armsnes_motion_sensor", "Light Gun Motion", "Motion",
		"The device's motion moves the crosshair from where the right stick aims it: turning it with the "
		"gyroscope, or tilting it with the accelerometer. R3 takes how the device is held as the middle.",
		NULL, "aim", { { "Off", NULL }, { "Gyroscope", NULL }, { "Accelerometer", NULL }, { NULL, NULL } }, "Off" },
	{ "armsnes_motion_range", "Light Gun Motion Range", "Motion Range",
		"How far the motion moves the crosshair: the whole screen, to aim by motion alone, or a quarter of it "
		"either way, for fine aim on top of the stick.",
		NULL, "aim", { { "Whole screen", NULL }, { "Fine", NULL }, { NULL, NULL } }, "Whole screen" },
	{ "armsnes_motion_sensitivity", "Light Gun Motion Sensitivity", "Motion Sensitivity",
		"How far the device turns or tilts to move the crosshair all the way: about 20 degrees at 100%, less "
		"the higher it is.",
		NULL, "aim", { { "25%", NULL }, { "50%", NULL }, { "75%", NULL }, { "100%", NULL }, { "150%", NULL },
		{ "200%", NULL }, { "300%", NULL }, { NULL, NULL } }, "100%" },
	{ "armsnes_motion_invert", "Light Gun Motion Invert", "Motion Invert",
		"Turns the motion's aim around, side to side, up and down, or both.",
		NULL, "aim", { { "Off", NULL }, { "Horizontal", NULL }, { "Vertical", NULL }, { "Both", NULL }, { NULL, NULL } }, "Off" },
	{ "armsnes_motion_rotation", "Light Gun Motion Sensor Rotation", "Motion Sensor Rotation",
		"Turns the motion sensor to match the screen if aiming goes the wrong way. Usually 0 degrees in the "
		"standalone app, often 90 in RetroArch.",
		NULL, "aim", { { "0", "0 degrees" }, { "90", "90 degrees" }, { "180", "180 degrees" }, { "270", "270 degrees" },
		{ NULL, NULL } }, "0" },
	{ NULL, NULL, NULL, NULL, NULL, NULL, { { NULL, NULL } }, NULL },
};

static struct retro_core_option_v2_category categories[] = {
	{ "system", "System", "The core's mode, threads and frame skip, the console's region, its memory at power-on and its chips' speeds." },
	{ "video", "Video", "The picture's shape, edges and colors, the TV filter, and which layers it shows." },
	{ "audio", "Audio", "The sound chip's interpolation, each voice's volume and the cartridge's own sound, and which channels play." },
	{ "input", "Input", "Turbo, the D-pad and rumble." },
	{ "aim", "Light Guns and Mouse",
		"How the Super Scope and Justifiers aim, and the SNES Mouse's speed. The mouse moves with the mouse, "
		"the right stick (R and L for its buttons) or a drag on the touchscreen, where a tap clicks. Shown "
		"when one is plugged in." },
	{ NULL, NULL, NULL },
};

static struct retro_core_options_v2 options = { categories, optionDefinitions };

// Options for chips and controllers the game doesn't have are hidden.
static bool showOptions(void) {
	bool guns = !loaded || inputGuns, mouse = !loaded || inputDevice[0] == DEVICE_MOUSE || inputDevice[1] == DEVICE_MOUSE;
	bool stick = guns && (optionIs(environment, "armsnes_gun_aim", "Right stick (pan)") ||
	                      optionIs(environment, "armsnes_gun_aim", "Right stick (absolute)"));
	bool motion = stick && !optionIs(environment, "armsnes_motion_sensor", "Off");
	bool wide = !optionIs(environment, "armsnes_mode", "Exact") && !optionIs(environment, "armsnes_widescreen", "Off") &&
	            !(loaded && cart.icd);
	bool layers = optionIs(environment, "armsnes_layer_selection", "On");
	bool channels = optionIs(environment, "armsnes_channel_selection", "On");
	const struct {
		const char* key;
		bool show;
	} shown[] = {
		{ "armsnes_threads", !optionIs(environment, "armsnes_mode", "Balanced") && !optionIs(environment, "armsnes_mode", "Exact") },
		{ "armsnes_frame_skip", !optionIs(environment, "armsnes_mode", "Balanced") && !optionIs(environment, "armsnes_mode", "Exact") },
		{ "armsnes_hd_mode7", !optionIs(environment, "armsnes_mode", "Exact") && optionIs(environment, "armsnes_filter", "Off") },
		{ "armsnes_scanlines", !optionIs(environment, "armsnes_filter", "Off") },
		{ "armsnes_sprite_limit", !optionIs(environment, "armsnes_mode", "Balanced") && !optionIs(environment, "armsnes_mode", "Exact") },
		{ "armsnes_widescreen", !optionIs(environment, "armsnes_mode", "Exact") },
		{ "armsnes_widescreen_layers", wide },
		{ "armsnes_widescreen_sprites", wide },
		{ "armsnes_superfx_clock", !loaded || cart.gsu },
		{ "armsnes_sa1_clock", !loaded || cart.sa1 },
		{ "armsnes_gun_aim", guns },
		{ "armsnes_crosshair", guns },
		{ "armsnes_aim_sensitivity", mouse || (guns && !optionIs(environment, "armsnes_gun_aim", "Light gun")) },
		{ "armsnes_stick_invert", mouse || stick },
		{ "armsnes_motion_sensor", stick },
		{ "armsnes_motion_range", motion },
		{ "armsnes_motion_sensitivity", motion },
		{ "armsnes_motion_invert", motion },
		{ "armsnes_motion_rotation", motion },
		{ "armsnes_volume_gameboy", !loaded || cart.icd },
		{ "armsnes_volume_msu1", !loaded || cart.msu1 },
		{ "armsnes_sgb_border", !loaded || cart.icd },
		{ "armsnes_port1", !portsChosen },
		{ "armsnes_port2", !portsChosen },
		{ "armsnes_layer_bg1", layers },
		{ "armsnes_layer_bg2", layers },
		{ "armsnes_layer_bg3", layers },
		{ "armsnes_layer_bg4", layers },
		{ "armsnes_layer_sprites", layers },
		{ "armsnes_layer_color_math", layers },
		{ "armsnes_channel_voice1", channels },
		{ "armsnes_channel_voice2", channels },
		{ "armsnes_channel_voice3", channels },
		{ "armsnes_channel_voice4", channels },
		{ "armsnes_channel_voice5", channels },
		{ "armsnes_channel_voice6", channels },
		{ "armsnes_channel_voice7", channels },
		{ "armsnes_channel_voice8", channels },
		{ "armsnes_channel_gameboy", channels && (!loaded || cart.icd) },
		{ "armsnes_channel_msu1", channels && (!loaded || cart.msu1) },
	};
	for (size_t i = 0; i < sizeof(shown) / sizeof(*shown); ++i) {
		struct retro_core_option_display display = { shown[i].key, shown[i].show };
		environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &display);
	}
	return true;
}

// Loads of two files: a Sufami Turbo's two cartridges, a cartridge with a memory pack in its slot,
// and a Super Game Boy cartridge with a Game Boy game. Each file's save memory is kept beside it.
enum { SUBSYSTEM_SUFAMI = 0x101, SUBSYSTEM_PACK, SUBSYSTEM_GAME_BOY };
enum { MEMORY_SUFAMI_A = 0x101, MEMORY_SUFAMI_B, MEMORY_CARTRIDGE, MEMORY_GAME_BOY, MEMORY_GAME_BOY_CLOCK, MEMORY_PACK };

static const struct retro_subsystem_memory_info sufamiAMemory[] = { { "srm", MEMORY_SUFAMI_A } };
static const struct retro_subsystem_memory_info sufamiBMemory[] = { { "srm", MEMORY_SUFAMI_B } };
static const struct retro_subsystem_memory_info cartridgeMemory[] = { { "srm", MEMORY_CARTRIDGE } };
static const struct retro_subsystem_memory_info gameBoyMemory[] = { { "srm", MEMORY_GAME_BOY }, { "rtc", MEMORY_GAME_BOY_CLOCK } };
static const struct retro_subsystem_memory_info packMemory[] = { { "flash", MEMORY_PACK } };

static const struct retro_subsystem_rom_info sufamiRoms[] = {
	{ "Slot A", "st", false, false, true, sufamiAMemory, 1 },
	{ "Slot B", "st", false, false, false, sufamiBMemory, 1 },
};
static const struct retro_subsystem_rom_info packRoms[] = {
	{ "Cartridge", "sfc|smc", false, false, true, cartridgeMemory, 1 },
	{ "Memory Pack", "bs", false, false, true, packMemory, 1 },
};
static const struct retro_subsystem_rom_info gameBoyRoms[] = {
	{ "Super Game Boy", "sfc|smc", false, false, true, NULL, 0 },
	{ "Game Boy", "gb|gbc", false, false, true, gameBoyMemory, 2 },
};
static const struct retro_subsystem_info subsystems[] = {
	{ "Sufami Turbo", "sufami", sufamiRoms, 2, SUBSYSTEM_SUFAMI },
	{ "Cartridge with Memory Pack", "pack", packRoms, 2, SUBSYSTEM_PACK },
	{ "Super Game Boy", "sgb", gameBoyRoms, 2, SUBSYSTEM_GAME_BOY },
	{ NULL, NULL, NULL, 0, 0 },
};

enum { TURBO_FEWEST = 2, TURBO_MOST = 60 };

void retro_set_environment(retro_environment_t callback) {
	environment = callback;
	for (struct retro_core_option_v2_definition* d = optionDefinitions; d->key; ++d) {
		if (!strcmp(d->key, "armsnes_turbo_period") || !strcmp(d->key, "armsnes_turbo_period_lr")) {
			static char periods[TURBO_MOST - TURBO_FEWEST + 1][4];
			static char labels[TURBO_MOST - TURBO_FEWEST + 1][12];
			for (int i = 0; i <= TURBO_MOST - TURBO_FEWEST; ++i) {
				snprintf(periods[i], sizeof(periods[i]), "%d", TURBO_FEWEST + i);
				snprintf(labels[i], sizeof(labels[i]), "%d frames", TURBO_FEWEST + i);
				d->values[i] = (struct retro_core_option_value) { periods[i], labels[i] };
			}
		}
	}
	bool noGame = false;
	callback(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &noGame);
	struct retro_log_callback log;
	if (callback(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log)) {
		logPrintf = log.log;
	}


	// A Super Multitap's other pads, or the second Justifier.
	static const struct retro_controller_description more[] = {
		{ "SNES Controller", RETRO_DEVICE_JOYPAD },
		{ "Justifier", RETRO_DEVICE_LIGHTGUN },
	};
	// The first port takes all but the guns.
	static const struct retro_controller_info ports[] = {
		{ portDevices, 6 },
		{ portDevices, 9 },
		{ more, 2 },
		{ more, 1 },
		{ more, 1 },
		{ more, 1 },
		{ more, 1 },
		{ more, 1 },
		{ NULL, 0 },
	};
	callback(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*) ports);
	callback(RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO, (void*) subsystems);
	optionsGive(callback, &options);
	static struct retro_core_options_update_display_callback update = { showOptions };
	callback(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK, &update);
}

void retro_set_video_refresh(retro_video_refresh_t callback) {
	videoRefresh = callback;
}

void retro_set_audio_sample(retro_audio_sample_t callback) {
	(void) callback;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t callback) {
	audioBatch = callback;
}

void retro_set_input_poll(retro_input_poll_t callback) {
	inputPollFrontend = callback;
}

void retro_set_input_state(retro_input_state_t callback) {
	inputState = callback;
}

unsigned retro_api_version(void) {
	return RETRO_API_VERSION;
}

void retro_init(void) {
	makeColors();
	fastPpuSetSink(lineDrawn);
	fastPpuSetHdSink(hdLineDrawn);
	bitmasks = environment(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL);
}

static void stopThreads(void) {
	fastPpuStop();
	smpStop();
	gsuStop();
	st018Stop();
}

void retro_deinit(void) {
	stopThreads();
}

void retro_get_system_info(struct retro_system_info* info) {
	memset(info, 0, sizeof(*info));
	info->library_name = "armSNES";
	info->library_version = "1.0";
	info->valid_extensions = "sfc|smc|bs|st|gb|gbc";
	info->need_fullpath = false;
	info->block_extract = false;
}

enum { ASPECT_PAR, ASPECT_4_3, ASPECT_SQUARE };
static int aspect;
static u32 cropTopBottom, cropSides;
static bool sgbBorderHidden;

// The ratio of a picture of so many of the console's pixels across and lines down.
static float aspectOf(u32 width, u32 lines) {
	if (aspect == ASPECT_4_3) {
		return 4.0f / 3.0f * (float) width / (float) (width - 2 * frameExtension);
	}
	double pixel = aspect == ASPECT_SQUARE ? 1.0 : sys.pal ? 1.3889 : 8.0 / 7.0;
	return (float) (width * pixel / lines);
}

static unsigned maxWidth(void) {
	unsigned hd = wideColumns * (filter ? COMPOSITE_SCALE : hdScale > 2 ? hdScale : 2);
	return hd > OUTPUT_WIDTH ? hd : OUTPUT_WIDTH;
}

static unsigned maxHeight(void) {
	return hdScale > 2 ? 239u * hdScale : OUTPUT_HEIGHT;
}

// The part of the output shown: the crops, or the Game Boy's screen without the Super Game Boy's border.
struct Shown {
	u32 x, y, width, height;
};

static struct Shown shownPart(void) {
	u32 xScale = outputWidth / frameColumns, yScale = frameScaleY;
	u32 lines = outputHeight / yScale;
	struct Shown part = { cropSides, cropTopBottom, frameColumns - 2 * cropSides, lines > 2 * cropTopBottom ? lines - 2 * cropTopBottom : lines };
	if (sgbBorderHidden && cart.icd && lines >= 144) {
		part = (struct Shown) { 48, (lines - 144) / 2, 160, 144 };
	}
	part.x *= xScale;
	part.width *= xScale;
	part.y *= yScale;
	part.height *= yScale;
	return part;
}

static struct Area shownArea(void) {
	struct Shown part = shownPart();
	u32 xScale = outputWidth / frameColumns, yScale = frameScaleY;
	return (struct Area) { (int) (part.x / xScale) - (int) frameExtension, (int) (part.y / yScale), (int) (part.width / xScale),
	                       (int) (part.height / yScale) };
}

static struct retro_game_geometry given;

// The frontend is told when the picture's shape changes, in the console's pixels and lines.
static void giveGeometry(const struct Shown* part) {
	u32 xScale = outputWidth / frameColumns, yScale = frameScaleY;
	u32 width = part->width / xScale, lines = part->height / yScale;
	struct retro_game_geometry geometry = { width, lines, maxWidth(), maxHeight(), aspectOf(width, lines) };
	if (geometry.base_width != given.base_width || geometry.base_height != given.base_height || geometry.aspect_ratio != given.aspect_ratio) {
		given = geometry;
		environment(RETRO_ENVIRONMENT_SET_GEOMETRY, &given);
	}
}

void retro_get_system_av_info(struct retro_system_av_info* info) {
	memset(info, 0, sizeof(*info));
	info->geometry.base_width = OUTPUT_WIDTH / 2;
	info->geometry.base_height = sys.pal ? 239 : 224;
	info->geometry.max_width = maxWidth();
	info->geometry.max_height = maxHeight();
	info->geometry.aspect_ratio = aspectOf(256, sys.pal ? 239 : 224);
	given = info->geometry;
	// A frame is 262 lines of 1364 clocks, less 4 clocks every other frame; PAL's is 312 lines.
	info->timing.fps = sys.pal ? sys.cpuFrequency / (1364.0 * 312.0) : sys.cpuFrequency / (1364.0 * 262.0 - 2.0);
	info->timing.sample_rate = sys.apuFrequency / 768.0;
}

// The pads' buttons, and L2 as fast-forward or, with a gun or a mouse plugged in, fine aim.
static void giveDescriptors(void) {
#define PAD(port) \
	{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left" }, \
	{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up" }, \
	{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down" }, \
	{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" }, \
	{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "B" }, \
	{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "A" }, \
	{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "X" }, \
	{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Y" }, \
	{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L" }, \
	{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R" }, \
	{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" }, \
	{ port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" }
#define LIST(l2) \
	{ PAD(0), { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, l2 }, \
	  { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "Fast-Forward" }, \
	  PAD(1), PAD(2), PAD(3), PAD(4), PAD(5), PAD(6), PAD(7), { 0, 0, 0, 0, NULL } }
	static const struct retro_input_descriptor plain[] = LIST("Fast-Forward"), aimed[] = LIST("Fine Aim");
#undef LIST
#undef PAD
	environment(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void*) (aiming() ? aimed : plain));
}


static void setPortDevice(unsigned port, unsigned device) {
	switch (device) {
	case RETRO_DEVICE_NONE: inputSetDevice(port, DEVICE_NONE); break;
	case RETRO_DEVICE_MOUSE: inputSetDevice(port, DEVICE_MOUSE); break;
	case DEVICE_LR_MULTITAP: inputSetDevice(port, DEVICE_MULTITAP); break;
	case DEVICE_LR_KEYPAD: inputSetDevice(port, DEVICE_KEYPAD); break;
	case DEVICE_LR_TWIN_TAP: inputSetDevice(port, DEVICE_TWIN_TAP); break;
	case DEVICE_LR_SUPER_SCOPE: inputSetDevice(port, DEVICE_SUPER_SCOPE); break;
	case DEVICE_LR_JUSTIFIER: inputSetDevice(port, DEVICE_JUSTIFIER); break;
	case DEVICE_LR_JUSTIFIERS: inputSetDevice(port, DEVICE_JUSTIFIERS); break;
	default: inputSetDevice(port, DEVICE_PAD); break;
	}
	if (loaded) {
		giveDescriptors();
	}
	showOptions();
}

void retro_set_controller_port_device(unsigned port, unsigned device) {
	if (port > 1) {
		return;
	}
	portsChosen = true;
	setPortDevice(port, device);
}

static void readPortOptions(void) {
	for (unsigned port = 0; port < 2 && !portsChosen; ++port) {
		const char* name = optionValue(environment, port ? "armsnes_port2" : "armsnes_port1");
		for (int n = 0; name && n < (port ? 9 : 6); ++n) {
			if (!strcmp(name, portDevices[n].desc) && portOptionDevice[port] != (int) portDevices[n].id) {
				portOptionDevice[port] = (int) portDevices[n].id;
				setPortDevice(port, portDevices[n].id);
			}
		}
	}
}

void retro_reset(void) {
	if (loaded) {
		systemPower(true);
	}
}

// A memory pack loaded on its own keeps what is written to it in the save folder, as <pack>.flash (the
// name the frontend gives it for the Cartridge with Memory Pack subsystem), written when it changes.
static char packPath[4096];
static u8* packKept;

static void packKeep(void) {
	u32 size = 0;
	u8* data = bsData(&size);
	if (!packKept || !size || !memcmp(packKept, data, size)) {
		return;
	}
	FILE* file = fopen(packPath, "wb");
	if (file) {
		if (fwrite(data, 1, size, file) == size) {
			memcpy(packKept, data, size);
		}
		fclose(file);
	}
}

static void packOpen(const char* path) {
	free(packKept);
	packKept = NULL;
	const char* folder = NULL;
	u32 size = 0;
	u8* data = bsData(&size);
	if (!size || !path || !environment(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &folder) || !folder) {
		return;
	}
	const char* name = strrchr(path, '/');
	name = name ? name + 1 : path;
	snprintf(packPath, sizeof(packPath) - 8, "%s/%s", folder, name);
	char* dot = strrchr(packPath, '.');
	strcpy(dot && dot > strrchr(packPath, '/') ? dot : packPath + strlen(packPath), ".flash");
	size_t kept = 0;
	u8* saved = readFile(packPath, &kept);
	if (saved && kept == size) {
		memcpy(data, saved, size);
	}
	free(saved);
	packKept = malloc(size);
	if (packKept) {
		memcpy(packKept, data, size);
	}
}

static void readOptions(void);

// Frame Skip (Fast mode): Auto skips while the frontend's sound is about to run short, up to three in a
// row; a number skips that many after each frame shown. Not in netplay, where the others would wait.
static int frameSkip;
static bool soundActive, soundShort;
static unsigned soundFull = 100;

static void soundBufferStatus(bool active, unsigned occupancy, bool underrunLikely) {
	soundActive = active;
	soundFull = occupancy;
	soundShort = underrunLikely;
}

static bool skipThisFrame(void) {
	static int run;
	bool skip = false;
	if (frameSkip && systemFast && !netplaySeen) {
		skip = frameSkip < 0 ? soundActive && (soundShort || soundFull <= 33) && run < 3 : run < frameSkip;
	}
	run = skip ? run + 1 : 0;
	return skip;
}

void retro_run(void) {
	bool updated = false;
	if (environment(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
		readOptions();
	}
	if (!rtcApplied) {
		rtcApplied = true;
		if (rtcPresent() && memcmp(rtcMemory, rtcStart, sizeof(rtcMemory))) {
			rtcLoad(rtcMemory, (s64) time(NULL));
		}
		if (cart.icd) {
			gbRtcRead();
		}
	}
	satellaviewNow = (s64) time(NULL);
	readPads();
	outputReady = false;
	soundCount = 0;
	int av = RETRO_AV_ENABLE_VIDEO | RETRO_AV_ENABLE_AUDIO;
	if (!environment(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &av)) {
		av = RETRO_AV_ENABLE_VIDEO | RETRO_AV_ENABLE_AUDIO;
	}
	bool dupe = false;
	environment(RETRO_ENVIRONMENT_GET_CAN_DUPE, &dupe);
	pictureUnseen = dupe && (!(av & RETRO_AV_ENABLE_VIDEO) || skipThisFrame());
	ppuSkipPicture = pictureUnseen && systemFast;

	systemRunFrame();

	if (outputReady) {
		if (filter) {
			filterFrame();
		}
		if (inputGuns && crosshair) {
			drawCrosshairs();
		}
		shown = framePicture;
		shownPitch = framePitch;
	}
	// Without a new picture the last one is given again, unless the frontend repeats it itself.
	const u16* picture = outputReady || !dupe ? (shown ? shown : output) : NULL;
	struct Shown part = shownPart();
	giveGeometry(&part);
	u32 pitch = shown ? shownPitch : OUTPUT_WIDTH;
	videoRefresh(picture ? picture + part.y * pitch + part.x : NULL, part.width, part.height, pitch * sizeof(u16));
	if (soundCount && (av & RETRO_AV_ENABLE_AUDIO)) {
		audioBatch(sound, soundCount);
	}
	if (rtcPresent()) {
		rtcSave(rtcMemory, (s64) time(NULL));
	}
	if (cart.icd) {
		gbRtcWrite();
	}
	if (sys.frames % 300 == 0) {
		packKeep();
	}
}

size_t retro_serialize_size(void) {
	return loaded ? stateSize() : 0;
}

bool retro_serialize(void* data, size_t size) {
	checkNetplay();
	return loaded && stateSave(data, size);
}

bool retro_unserialize(const void* data, size_t size) {
	checkNetplay();
	// A state loaded before the first frame has the clocks the files would have set.
	rtcApplied = true;
	return loaded && stateLoad(data, size);
}

void retro_cheat_reset(void) {
	cheatReset();
}

void retro_cheat_set(unsigned index, bool enabled, const char* code) {
	(void) index;
	if (enabled && code) {
		cheatAdd(code);
	}
}

// A Clock option, in quarters (25% each).
static u8 clockOption(const char* key) {
	struct retro_variable variable = { key, NULL };
	int percent = environment(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) && variable.value ? atoi(variable.value) : 100;
	return percent >= 25 && percent <= 400 ? (u8) (percent / 25) : 4;
}

// Patterned is ares's: stripes of two values. Cleared is all 0, which some unlicensed games and
// betas need.
// Exact is ares's timing, Balanced coarser, Fast Balanced's with snes_faust's renderer on a thread of
// its own (docs/ROADMAP.md). A change takes effect between frames.
// A volume, 0 while its channel is switched off under Channel Selection.
static u8 volumeOption(const char* key, const char* channel) {
	char channelKey[40];
	snprintf(channelKey, sizeof(channelKey), "armsnes_channel_%s", channel);
	if (optionIs(environment, "armsnes_channel_selection", "On") && optionIs(environment, channelKey, "Off")) {
		return 0;
	}
	const char* value = optionValue(environment, key);
	int percent = value ? atoi(value) : 100;
	return percent >= 0 && percent <= 100 ? (u8) percent : 100;
}

static bool regionPal(void) {
	return optionIs(environment, "armsnes_region", "PAL") || (cart.pal && !optionIs(environment, "armsnes_region", "NTSC"));
}

static u32 periodOption(const char* key) {
	const char* value = optionValue(environment, key);
	u32 period = value ? (u32) atoi(value) : 4;
	return period < TURBO_FEWEST || period > TURBO_MOST ? 4 : period;
}

static void readOptions(void) {
	struct retro_variable mode = { "armsnes_mode", NULL };
	const char* value = environment(RETRO_ENVIRONMENT_GET_VARIABLE, &mode) && mode.value ? mode.value : "Fast";
	bool exact = !strcmp(value, "Exact"), balanced = !exact, fast = !exact && strcmp(value, "Balanced");
	if (balanced != systemBalanced || fast != systemFast) {
		systemBalanced = balanced;
		systemFast = fast;
		systemFrameAtTop = fast;
		cpuFastReplan();
	}
	// Fast mode's threads: the sound's and the picture's, the picture's only, or none, for devices whose
	// cores are busy.
	struct retro_variable threads = { "armsnes_threads", NULL };
	const char* threading = environment(RETRO_ENVIRONMENT_GET_VARIABLE, &threads) && threads.value ? threads.value : "All";
	// All: as many as the cores the frontend gives the core allow, the sound's with three or more.
	unsigned cores = fastPpuCores();
	systemSoundThread = !strcmp(threading, "All") && cores >= 3;
	systemPictureThread = strcmp(threading, "Off") != 0 && cores >= 2;
	struct retro_variable variable = { "armsnes_power_on_ram", NULL };
	systemClearPowerOn = !environment(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) || !variable.value ||
	                     strcmp(variable.value, "Patterned");
	systemCpuClock = clockOption("armsnes_cpu_clock");
	systemGsuClock = clockOption("armsnes_superfx_clock");
	systemSa1Clock = clockOption("armsnes_sa1_clock");
	static const struct OptionChoice turbo[] = {
		{ "A", 1 << RETRO_DEVICE_ID_JOYPAD_A },
		{ "B", 1 << RETRO_DEVICE_ID_JOYPAD_B },
		{ "X", 1 << RETRO_DEVICE_ID_JOYPAD_X },
		{ "Y", 1 << RETRO_DEVICE_ID_JOYPAD_Y },
		{ "A and B", 1 << RETRO_DEVICE_ID_JOYPAD_A | 1 << RETRO_DEVICE_ID_JOYPAD_B },
		{ "X and Y", 1 << RETRO_DEVICE_ID_JOYPAD_X | 1 << RETRO_DEVICE_ID_JOYPAD_Y },
		{ "A, B, X and Y", 1 << RETRO_DEVICE_ID_JOYPAD_A | 1 << RETRO_DEVICE_ID_JOYPAD_B | 1 << RETRO_DEVICE_ID_JOYPAD_X | 1 << RETRO_DEVICE_ID_JOYPAD_Y },
	};
	turboButtons = (u16) optionChoice(environment, "armsnes_turbo_buttons", turbo, sizeof(turbo) / sizeof(*turbo), 0);
	turboPeriod = periodOption("armsnes_turbo_period");
	turboPeriodLR = periodOption("armsnes_turbo_period_lr");
	if (loaded && regionPal() != sys.pal) {
		stopThreads();
		systemLoad(regionPal());
		systemPower(false);
		struct retro_system_av_info av;
		retro_get_system_av_info(&av);
		environment(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av);
	}
	static const struct OptionChoice aspects[] = { { "8:7 PAR", ASPECT_PAR }, { "4:3", ASPECT_4_3 }, { "1:1", ASPECT_SQUARE } };
	aspect = optionChoice(environment, "armsnes_aspect", aspects, 3, ASPECT_PAR);
	overscanShown = optionIs(environment, "armsnes_overscan", "Shown");
	const char* crop = optionValue(environment, "armsnes_crop_vertical");
	cropTopBottom = crop ? (u32) atoi(crop) % 17 : 0;
	crop = optionValue(environment, "armsnes_crop_horizontal");
	cropSides = crop ? (u32) atoi(crop) % 17 : 0;
	sgbBorderHidden = optionIs(environment, "armsnes_sgb_border", "Hidden");
	ppuNoSpriteLimit = optionIs(environment, "armsnes_sprite_limit", "No limit");
	static const char* const layerKeys[6] = { "bg1", "bg2", "bg3", "bg4", "sprites", "color_math" };
	ppuHidden = 0;
	for (u32 n = 0; n < 6 && optionIs(environment, "armsnes_layer_selection", "On"); ++n) {
		char layerKey[40];
		snprintf(layerKey, sizeof(layerKey), "armsnes_layer_%s", layerKeys[n]);
		ppuHidden |= optionIs(environment, layerKey, "Hidden") << n;
	}
	static const struct OptionChoice hdScales[] = { { "2x", 2 }, { "3x", 3 }, { "4x", 4 }, { "5x", 5 }, { "6x", 6 }, { "7x", 7 }, { "8x", 8 } };
	unsigned largest = maxWidth() * maxHeight();
	static const struct OptionChoice filters[] = { { "Composite", COMPOSITE }, { "S-Video", COMPOSITE_SVIDEO } };
	filter = optionChoice(environment, "armsnes_filter", filters, 2, COMPOSITE_OFF);
	static const struct OptionChoice darkness[] = { { "Light", 192 }, { "Medium", 128 }, { "Dark", 64 } };
	scanlines = (u32) optionChoice(environment, "armsnes_scanlines", darkness, 3, 256);
	hdScale = ppuHdScale = filter ? 0 : (u8) optionChoice(environment, "armsnes_hd_mode7", hdScales, 7, 0);
	// Widescreen: the columns each side that make the picture that wide, by the pixels' shape, rounded to
	// 16.
	static const struct OptionChoice ratios[] = { { "16:10", 1600 }, { "16:9", 1778 }, { "2:1", 2000 }, { "21:9", 2333 } };
	int ratio = optionChoice(environment, "armsnes_widescreen", ratios, 4, 0);
	u32 columns = 256;
	if (ratio && systemBalanced && !cart.icd) {
		double lines = regionPal() ? 239.0 : 224.0;
		double pixel = aspect == ASPECT_SQUARE ? 1.0 : aspect == ASPECT_4_3 ? 4.0 / 3.0 * lines / 256.0 : regionPal() ? 1.3889 : 8.0 / 7.0;
		int side = (int) ((ratio / 1000.0 * lines / pixel - 256.0) / 2.0 / 16.0 + 0.5) * 16;
		side = side < 0 ? 0 : side > FAST_PPU_WIDE_MOST ? FAST_PPU_WIDE_MOST : side;
		columns = 256 + 2 * (u32) side;
	}
	ppuWideAll = optionIs(environment, "armsnes_widescreen_layers", "All");
	ppuWideSprites = optionIs(environment, "armsnes_widescreen_sprites", "Shown");
	wideColumns = columns;
	ppuWide = (u8) ((columns - 256) / 2);
	// A larger picture than the frontend was told of needs a new AV info.
	if (loaded && maxWidth() * maxHeight() > largest) {
		struct retro_system_av_info av;
		retro_get_system_av_info(&av);
		environment(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av);
	}
	static const struct OptionChoice skips[] = { { "Auto", -1 }, { "1", 1 }, { "2", 2 }, { "3", 3 } };
	frameSkip = optionChoice(environment, "armsnes_frame_skip", skips, 4, 0);
	struct retro_audio_buffer_status_callback status = { frameSkip < 0 ? soundBufferStatus : NULL };
	if (!environment(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK, &status) && frameSkip < 0) {
		frameSkip = 0;
	}
	static const struct OptionChoice blends[] = { { "On", BLEND_ON }, { "Merge", BLEND_MERGE } };
	hiresBlend = optionChoice(environment, "armsnes_hires_blend", blends, 2, BLEND_OFF);
	bool boost = optionIs(environment, "armsnes_deep_black_boost", "On");
	if (boost != deepBlackBoost) {
		deepBlackBoost = boost;
		makeColors();
	}
	static const struct OptionChoice interpolations[] = {
		{ "Cubic", INTERPOLATE_CUBIC }, { "Linear", INTERPOLATE_LINEAR }, { "None", INTERPOLATE_NONE } };
	dspInterpolation = (u8) optionChoice(environment, "armsnes_interpolation", interpolations, 3, INTERPOLATE_GAUSSIAN);
	for (int n = 0; n < 8; ++n) {
		char key[32], channel[8];
		snprintf(key, sizeof(key), "armsnes_volume_voice%d", n + 1);
		snprintf(channel, sizeof(channel), "voice%d", n + 1);
		dspVoiceVolume[n] = volumeOption(key, channel);
	}
	dspGameBoyVolume = volumeOption("armsnes_volume_gameboy", "gameboy");
	dspMsu1Volume = volumeOption("armsnes_volume_msu1", "msu1");
	dspListenUpdate();
	allowOpposing = optionIs(environment, "armsnes_opposing", "On");
	leftStickDpad = !optionIs(environment, "armsnes_left_stick_dpad", "Off");
	readPortOptions();
	bool rumbleOn = optionIs(environment, "armsnes_rumble", "On");
	if (inputRumbleOn && !rumbleOn) {
		inputRumble(0, 0, 0);
		inputRumble(1, 0, 0);
	}
	inputRumbleOn = rumbleOn;
	static const struct OptionChoice aims[] = {
		{ "Right stick (absolute)", AIM_STICK }, { "Light gun", AIM_LIGHT_GUN }, { "Mouse", AIM_MOUSE } };
	gunAim = optionChoice(environment, "armsnes_gun_aim", aims, 3, AIM_STICK_PAN);
	static const struct OptionChoice speeds[] = {
		{ "25%", 25 }, { "50%", 50 }, { "75%", 75 }, { "150%", 150 }, { "200%", 200 }, { "300%", 300 } };
	aimSpeed = optionChoice(environment, "armsnes_aim_sensitivity", speeds, 6, 100);
	motionSpeed = optionChoice(environment, "armsnes_motion_sensitivity", speeds, 6, 100);
	static const struct OptionChoice inverts[] = { { "Horizontal", 1 }, { "Vertical", 2 }, { "Both", 3 } };
	stickInvert = optionChoice(environment, "armsnes_stick_invert", inverts, 3, 0);
	motionInvert = optionChoice(environment, "armsnes_motion_invert", inverts, 3, 0);
	static const struct OptionChoice sensors[] = { { "Gyroscope", SENSOR_GYROSCOPE }, { "Accelerometer", SENSOR_ACCELEROMETER } };
	motionSensor = optionChoice(environment, "armsnes_motion_sensor", sensors, 2, SENSOR_OFF);
	motionWhole = !optionIs(environment, "armsnes_motion_range", "Fine");
	static const struct OptionChoice rotations[] = { { "90", 1 }, { "180", 2 }, { "270", 3 } };
	motionRotation = optionChoice(environment, "armsnes_motion_rotation", rotations, 3, 0);
	struct retro_variable cross = { "armsnes_crosshair", NULL };
	crosshair = !environment(RETRO_ENVIRONMENT_GET_VARIABLE, &cross) || !cross.value || strcmp(cross.value, "Off");
}

// Where the work RAM, the cartridge's RAM and the SA-1's I-RAM sit, for achievements and cheat
// searches: the cartridge's RAM and the I-RAM above the 24-bit bus, where rcheevos looks for them.
static void setMemoryMaps(void) {
	static struct retro_memory_descriptor descriptors[3];
	static struct retro_memory_map map;
	unsigned count = 0;
	descriptors[count++] = (struct retro_memory_descriptor) { RETRO_MEMDESC_SYSTEM_RAM, cpu.wram, 0, 0x7e0000, 0, 0, sizeof(cpu.wram), "WRAM" };
	u8* ram = cart.ramSize ? cart.ram : cart.saveRam;
	size_t ramSize = cart.ramSize ? cart.ramSize : cart.saveRamSize;
	if (ram && ramSize) {
		descriptors[count++] = (struct retro_memory_descriptor) { RETRO_MEMDESC_SAVE_RAM, ram, 0, 0x1000000, 0, 0, ramSize, "SRAM" };
	}
	if (cart.sa1) {
		descriptors[count++] = (struct retro_memory_descriptor) { RETRO_MEMDESC_SYSTEM_RAM, sa1.iram, 0, 0x1080000, 0, 0, sizeof(sa1.iram), "IRAM" };
	}
	map.descriptors = descriptors;
	map.num_descriptors = count;
	environment(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &map);
	bool achievements = true;
	environment(RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS, &achievements);
}

// What every load ends with, once the cartridge and what is in its slots are in.
static bool loadFinish(const char* gamePath) {
	// The MSU-1's files sit beside the game: game.msu, game-1.pcm and on.
	if (gamePath) {
		char path[4096];
		snprintf(path, sizeof(path), "%s", gamePath);
		char* dot = strrchr(path, '.');
		if (dot) {
			*dot = 0;
		}
		cartLoadMsu1(path);
	}

	giveDescriptors();
	if (!environment(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble)) {
		rumble.set_rumble_state = NULL;
	}

	readOptions();
	systemLoad(regionPal());
	systemPower(false);
	setMemoryMaps();
	if (rtcPresent()) {
		rtcSave(rtcMemory, (s64) time(NULL));
		memcpy(rtcStart, rtcMemory, sizeof(rtcStart));
	}
	rtcApplied = false;
	shown = NULL;
	loaded = true;
	showOptions();
	return true;
}

bool retro_load_game(const struct retro_game_info* info) {
	netplaySeen = false;
	if (!info || !info->data) {
		return false;
	}

	enum retro_pixel_format format = RETRO_PIXEL_FORMAT_RGB565;
	if (!environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format)) {
		return false;
	}

	free(game);
	size_t size = info->size;
	// A memory pack goes in the BS-X cartridge's slot, a Sufami Turbo cartridge in the base's slot A.
	bool pack = hasExtension(info->path, "bs");
	bool sufami = hasExtension(info->path, "st");
	bool gameBoy = hasExtension(info->path, "gb") || hasExtension(info->path, "gbc");
	size_t bootSize = 0;
	u8* boot = gameBoy ? systemFile("sgb1.boot.rom", &bootSize) : NULL;
	if (gameBoy && (!boot || bootSize != 256)) {
		free(boot);
		loadError("A Game Boy game needs sgb1.boot.rom in the system folder");
		return false;
	}
	if (pack || sufami || gameBoy) {
		const char* base = pack ? "BS-X.bin" : sufami ? "STBIOS.bin" : "SGB1.sfc";
		// A Game Boy game with a Super Game Boy cartridge of its name beside it goes in that one:
		// an MSU-1 pack's.
		game = gameBoy ? siblingFile(info->path, "sfc", &size) : NULL;
		if (!game) {
			game = systemFile(base, &size);
		}
		if (!game) {
			free(boot);
			loadError("This game needs %s in the system folder", base);
			return false;
		}
	} else {
		game = malloc(size);
		memcpy(game, info->data, size);
	}
	if (!cartLoad(game, size) || (pack && !cartInsertPack(info->data, info->size)) ||
	    (sufami && !cartInsertSufami(0, info->data, info->size)) ||
	    (gameBoy && !cartInsertGameBoy(info->data, info->size, boot, bootSize))) {
		free(boot);
		if (cart.missing[0]) {
			loadError("The %s board needs %s in the system folder", cart.board, cart.missing);
		} else {
			loadError("This game couldn't be loaded");
		}
		free(game);
		game = NULL;
		return false;
	}
	free(boot);
	// A Super Game Boy cartridge takes the Game Boy game of its name beside it.
	if (!gameBoy && cart.icd) {
		size_t gbSize = 0;
		u8* gb = siblingFile(info->path, "gb", &gbSize);
		if (!gb) {
			gb = siblingFile(info->path, "gbc", &gbSize);
		}
		boot = gb ? systemFile("sgb1.boot.rom", &bootSize) : NULL;
		if (boot && bootSize == 256) {
			cartInsertGameBoy(gb, gbSize, boot, bootSize);
		}
		free(boot);
		free(gb);
	}
	if (pack) {
		packOpen(info->path);
	}
	return loadFinish(info->path);
}

// A whole file the frontend gave, copied.
static u8* copyOf(const struct retro_game_info* info, size_t* size) {
	*size = info->size;
	u8* data = malloc(info->size ? info->size : 1);
	if (data) {
		memcpy(data, info->data, info->size);
	}
	return data;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info* info, size_t count) {
	netplaySeen = false;
	if (!info || !count || !info[0].data) {
		return false;
	}
	enum retro_pixel_format format = RETRO_PIXEL_FORMAT_RGB565;
	if (!environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format)) {
		return false;
	}
	free(game);
	game = NULL;
	size_t size = 0;
	bool second = count > 1 && info[1].data && info[1].size;
	switch (type) {
	case SUBSYSTEM_SUFAMI:
		game = systemFile("STBIOS.bin", &size);
		if (!game) {
			loadError("This game needs STBIOS.bin in the system folder");
			return false;
		}
		if (!cartLoad(game, size) || !cartInsertSufami(0, info[0].data, info[0].size) ||
		    (second && !cartInsertSufami(1, info[1].data, info[1].size))) {
			loadError("These Sufami Turbo cartridges couldn't be loaded");
			free(game);
			game = NULL;
			return false;
		}
		break;
	case SUBSYSTEM_PACK:
		game = copyOf(&info[0], &size);
		if (!game || !cartLoad(game, size) || !cart.bsSlot) {
			loadError("This cartridge has no memory pack slot");
			free(game);
			game = NULL;
			return false;
		}
		if (second && !cartInsertPack(info[1].data, info[1].size)) {
			loadError("This memory pack couldn't be loaded");
			free(game);
			game = NULL;
			return false;
		}
		break;
	case SUBSYSTEM_GAME_BOY: {
		game = copyOf(&info[0], &size);
		if (!game || !cartLoad(game, size) || !cart.icd) {
			loadError("This isn't a Super Game Boy cartridge");
			free(game);
			game = NULL;
			return false;
		}
		size_t bootSize = 0;
		u8* boot = systemFile("sgb1.boot.rom", &bootSize);
		bool inserted = boot && bootSize == 256 && second && cartInsertGameBoy(info[1].data, info[1].size, boot, bootSize);
		free(boot);
		if (!inserted) {
			loadError(second ? "A Game Boy game needs sgb1.boot.rom in the system folder" : "Choose a Game Boy game too");
			free(game);
			game = NULL;
			return false;
		}
		break;
	}
	default:
		return false;
	}
	return loadFinish(info[0].path);
}

void retro_unload_game(void) {
	if (fastForwardHeld) {
		setFastForward(false);
	}
	useSensor(SENSOR_OFF);
	if (inputRumbleOn) {
		inputRumble(0, 0, 0);
		inputRumble(1, 0, 0);
	}
	stopThreads();
	packKeep();
	free(packKept);
	packKept = NULL;
	loaded = false;
	cheatReset();
	free(game);
	game = NULL;
}

unsigned retro_get_region(void) {
	return sys.pal ? RETRO_REGION_PAL : RETRO_REGION_NTSC;
}

void* retro_get_memory_data(unsigned id) {
	switch (id) {
	case RETRO_MEMORY_SAVE_RAM:
		return cart.saveRam;
	case RETRO_MEMORY_SYSTEM_RAM:
		return cpu.wram;
	case RETRO_MEMORY_RTC:
		if (rtcPresent()) {
			return rtcMemory;
		}
		// Fall through.
	case MEMORY_GAME_BOY_CLOCK: {
		// A Super Game Boy's Game Boy cartridge's clock.
		size_t size = 0;
		return cart.icd ? gbRtc(&size) : NULL;
	}
	case MEMORY_PACK: {
		u32 size = 0;
		u8* data = bsData(&size);
		return size ? data : NULL;
	}
	case MEMORY_SUFAMI_A:
	case MEMORY_SUFAMI_B: {
		u32 size = 0;
		return cart.sufami ? sufamiRam(id - MEMORY_SUFAMI_A, &size) : NULL;
	}
	case MEMORY_CARTRIDGE:
	case MEMORY_GAME_BOY:
		return cart.saveRam;
	}
	return NULL;
}

size_t retro_get_memory_size(unsigned id) {
	switch (id) {
	case RETRO_MEMORY_SAVE_RAM:
		return cart.saveRamSize;
	case RETRO_MEMORY_SYSTEM_RAM:
		return sizeof(cpu.wram);
	case RETRO_MEMORY_RTC:
		if (rtcPresent()) {
			return sizeof(rtcMemory);
		}
		// Fall through.
	case MEMORY_GAME_BOY_CLOCK: {
		size_t size = 0;
		return cart.icd && gbRtc(&size) ? size : 0;
	}
	case MEMORY_PACK: {
		u32 size = 0;
		bsData(&size);
		return size;
	}
	case MEMORY_SUFAMI_A:
	case MEMORY_SUFAMI_B: {
		u32 size = 0;
		return cart.sufami && sufamiRam(id - MEMORY_SUFAMI_A, &size) ? size : 0;
	}
	case MEMORY_CARTRIDGE:
	case MEMORY_GAME_BOY:
		return cart.saveRamSize;
	}
	return 0;
}
