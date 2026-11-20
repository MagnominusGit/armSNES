/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "libretro.h"
#include "snes.h"
#include "fastppu.h"
#include "options.h"
#include "chips/gb.h"

#include <stdarg.h>
#include <stdio.h>
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

// HD mode 7 (Fast mode): a frame with mode 7 lines is given hdScale times larger each way, those lines
// drawn finer (fastPpuSetHdSink) and the others' pixels made larger. Two pictures, as with outputs.
enum { HD_WIDTH = 256 * 4, HD_HEIGHT = 239 * 4 };
static u16 hdOutputs[2][HD_WIDTH * HD_HEIGHT];
static u32 hdRowStamp[CANVAS_HEIGHT];
static u8 hdScale;

static void hdLineDrawn(u32* at, const u32* block, unsigned scale) {
	u32 row = (u32) (at - ppuCanvas) / CANVAS_WIDTH;
	if (scale != hdScale || row - cropY >= cropHeight || at != &ppuCanvas[row * CANVAS_WIDTH + cropX]) {
		return;
	}
	u16* to = &hdOutputs[stamp & 1][(row - cropY) * scale * HD_WIDTH];
	for (u32 j = 0; j < scale; ++j) {
		const u32* from = block + j * 256 * scale;
		for (u32 n = 0; n < 256 * scale; ++n) {
			u32 pixel = from[n];
			to[j * HD_WIDTH + n] = (pixel & 0x78000) == 0x78000 && !tableAlways ? rgb565Bright(pixel) : rgb565(pixel);
		}
	}
	hdRowStamp[row] = stamp;
}

// The picture given: the output, or the HD picture; its rows' length in pixels, and how many of its
// rows and columns a console pixel takes.
static const u16* framePicture;
static u32 framePitch = OUTPUT_WIDTH, frameScaleY = 1;

// The HD picture of the frame when it has HD lines: the others made larger from the output.
static bool hdFrame(u32 y, u32 h) {
	u32 n = hdScale, lines = 0;
	for (u32 row = 0; row < h; ++row) {
		lines += hdRowStamp[y + row] == stamp;
	}
	if (n < 2 || !lines) {
		return false;
	}
	u16* hd = hdOutputs[stamp & 1];
	u32 width = 256 * n;
	for (u32 row = 0; row < h; ++row) {
		if (hdRowStamp[y + row] == stamp) {
			continue;
		}
		const u16* from = &output[row * OUTPUT_WIDTH];
		u16* to = &hd[row * n * HD_WIDTH];
		for (u32 x = 0; x < width; ++x) {
			to[x] = from[x * outputWidth / width];
		}
		for (u32 j = 1; j < n; ++j) {
			memcpy(&to[j * HD_WIDTH], to, width * sizeof(u16));
		}
	}
	framePicture = hd;
	framePitch = HD_WIDTH;
	frameScaleY = n;
	outputWidth = width;
	outputHeight = h * n;
	return true;
}

// The rows converted as they were drawn, where the last crop put them in the output.
static u16 moved[OUTPUT_WIDTH * OUTPUT_HEIGHT];
static bool movedWide[OUTPUT_HEIGHT];

static bool overscanShown;

// Hi-res pixel pairs averaged, as a TV's picture blended them; Merge makes the picture 256 wide.
enum { BLEND_OFF, BLEND_ON, BLEND_MERGE };
static int hiresBlend;

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
	bool wide = inputGuns && crosshair && hiresBlend != BLEND_MERGE;
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

// The joypad's buttons are numbered as the pad's shift register sends them. Pad 0 is the first
// port's; the frontend's ports 2 to 5 are pads 1 to 4, the last three a Super Multitap's.
static u16 held[5];
static int mouseX[2], mouseY[2];
static bool mouseLeft[2], mouseRight[2];
// Where each gun points, in the picture's 256 columns and its rows, or below 0 when off it.
static int gunX[2], gunY[2];
static u8 gunButtons[2];

enum {
	DEVICE_LR_MULTITAP = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0),
	DEVICE_LR_SUPER_SCOPE = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_LIGHTGUN, 0),
	DEVICE_LR_JUSTIFIER = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_LIGHTGUN, 1),
	DEVICE_LR_JUSTIFIERS = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_LIGHTGUN, 2),
};

u16 inputPoll(int pad) {
	return held[pad];
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
	// NTSC's picture with overscan loses its first eight lines.
	*y = gunY[gun] < 0 ? -1 : gunY[gun] + (!sys.pal && ppu.io.overscan ? 8 : 0);
	*buttons = gunButtons[gun];
}

// Netplay, once it has saved or loaded a state of this game: then L2 and R2 don't fast-forward, as the
// other players would have to wait.
static bool netplaySeen;

static void checkNetplay(void) {
	int context = RETRO_SAVESTATE_CONTEXT_NORMAL;
	netplaySeen |= environment(RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT, &context) && context == RETRO_SAVESTATE_CONTEXT_ROLLBACK_NETPLAY;
}

// L2 or R2 held on the first pad fast-forwards at the frontend's top speed, outside netplay.
static bool fastForwardHeld;

static void setFastForward(bool on) {
	fastForwardHeld = on;
	struct retro_fastforwarding_override override = { .ratio = -1.0f, .fastforward = on, .notification = true, .inhibit_toggle = on };
	environment(RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE, &override);
}

static u16 turboButtons;
static u32 turboPeriod = 4;
static bool allowOpposing;
static bool leftStickDpad = true;

static void readPads(void) {
	inputPollFrontend();
	bool fastForward = !netplaySeen && (inputState(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2) ||
	                                    inputState(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2));
	if (fastForward != fastForwardHeld) {
		setFastForward(fastForward);
	}
	for (unsigned port = 0; port < 5; ++port) {
		u16 buttons = 0;
		if (bitmasks) {
			buttons = inputState(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
		} else {
			for (unsigned id = RETRO_DEVICE_ID_JOYPAD_B; id <= RETRO_DEVICE_ID_JOYPAD_R; ++id) {
				buttons |= (inputState(port, RETRO_DEVICE_JOYPAD, 0, id) ? 1 : 0) << id;
			}
		}
		if (leftStickDpad) {
			int x = inputState(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
			int y = inputState(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
			buttons |= (y < -0x4000) << RETRO_DEVICE_ID_JOYPAD_UP | (y > 0x4000) << RETRO_DEVICE_ID_JOYPAD_DOWN |
			           (x < -0x4000) << RETRO_DEVICE_ID_JOYPAD_LEFT | (x > 0x4000) << RETRO_DEVICE_ID_JOYPAD_RIGHT;
		}
		// Turbo buttons are held for the first half of each period, counted in the console's frames so
		// run-ahead and netplay see the same presses.
		if (turboButtons && sys.frames % turboPeriod >= (turboPeriod + 1) / 2) {
			buttons &= ~turboButtons;
		}
		if (!allowOpposing) {
			const u16 upDown = 1 << RETRO_DEVICE_ID_JOYPAD_UP | 1 << RETRO_DEVICE_ID_JOYPAD_DOWN;
			const u16 leftRight = 1 << RETRO_DEVICE_ID_JOYPAD_LEFT | 1 << RETRO_DEVICE_ID_JOYPAD_RIGHT;
			buttons &= (buttons & upDown) == upDown ? ~upDown : 0xffff;
			buttons &= (buttons & leftRight) == leftRight ? ~leftRight : 0xffff;
		}
		held[port] = buttons & 0xfff;
	}
	for (unsigned port = 0; port < 2; ++port) {
		if (inputDevice[port] != DEVICE_MOUSE) {
			continue;
		}
		mouseX[port] += inputState(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
		mouseY[port] += inputState(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
		mouseLeft[port] = inputState(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT);
		mouseRight[port] = inputState(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT);
	}
	if (!inputGuns) {
		return;
	}
	// The first gun is the second port's; a second Justifier is the frontend's third port.
	int rows = sys.pal ? 239 : 224;
	for (unsigned gun = 0; gun < 2; ++gun) {
		unsigned port = gun + 1;
		gunX[gun] = gunY[gun] = -1;
		gunButtons[gun] = 0;
		if (gun && inputDevice[1] != DEVICE_JUSTIFIERS) {
			continue;
		}
		int x = inputState(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X);
		int y = inputState(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y);
		if (!inputState(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN)) {
			gunX[gun] = (x + 0x8000) * 256 / 0x10000;
			gunY[gun] = (y + 0x8000) * rows / 0x10000;
		}
		u8 buttons = 0;
		buttons |= (inputState(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER) ? 1 : 0) << GUN_TRIGGER;
		buttons |= (inputState(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_AUX_A) ? 1 : 0) << GUN_CURSOR;
		buttons |= (inputState(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_AUX_B) ? 1 : 0) << GUN_TURBO;
		buttons |= (inputState(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_START) ? 1 : 0) << GUN_PAUSE;
		gunButtons[gun] = buttons;
	}
}

// A cross where each gun points: white, with a dark edge to show on any picture.
static void drawCrosshairs(void) {
	u16* picture = (u16*) framePicture;
	u32 yScale = frameScaleY;
	int xScale = (int) (outputWidth / (OUTPUT_WIDTH / 2));
	for (int gun = 0; gun < 2; ++gun) {
		if (gunX[gun] < 0 || gunY[gun] < 0) {
			continue;
		}
		int cx = gunX[gun] * xScale;
		int cy = gunY[gun] * yScale;
		for (int d = -7 * 2; d <= 7 * 2; ++d) {
			for (int w = -1; w <= 1; ++w) {
				u16 color = w ? 0x0000 : (gun ? 0xf800 : 0xffff);
				int x = cx + d * xScale / 2, y = cy + w * (int) yScale;
				if (x >= 0 && x < (int) outputWidth && y >= 0 && y < (int) outputHeight && (d < -3 || d > 3)) {
					picture[y * framePitch + x] = color;
				}
				x = cx + w * xScale;
				y = cy + d / 2 * (int) yScale;
				if (x >= 0 && x < (int) outputWidth && y >= 0 && y < (int) outputHeight && (d < -3 || d > 3)) {
					picture[y * framePitch + x] = color;
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
	{ "armsnes_frame_skip", "Frame Skip", NULL,
		"Skips drawing frames to keep up: Auto when the sound is about to run short, or this many after each "
		"frame shown. Fast mode only, not in netplay.",
		NULL, "video", { { "Off", NULL }, { "Auto", NULL }, { "1", NULL }, { "2", NULL }, { "3", NULL }, { NULL, NULL } }, "Off" },
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
	{ "armsnes_hd_mode7", "HD Mode 7", NULL,
		"Mode 7's rotated and scaled layer drawn 2 to 4 times finer each way, the rest of the picture as it "
		"is. Each step takes much more time. Fast mode only.",
		NULL, "video", { { "Off", NULL }, { "2x", NULL }, { "3x", NULL }, { "4x", NULL }, { NULL, NULL } }, "Off" },
	{ "armsnes_sprite_limit", "Sprite Limit", NULL,
		"The console shows 32 sprites and 34 sprite tiles on a line, which games turn into flicker or gaps. "
		"No limit shows them all; games still see the console's limit. Fast mode only.",
		NULL, "video", { { "32 per line", NULL }, { "No limit", NULL }, { NULL, NULL } }, "32 per line" },
	{ "armsnes_interpolation", "Interpolation", NULL,
		"How the sound chip smooths its samples: Gaussian as the console does, or sharper. Only what is heard "
		"changes; the echo keeps the console's.",
		NULL, "audio", { { "Gaussian", NULL }, { "Cubic", NULL }, { "Linear", NULL }, { "None", NULL }, { NULL, NULL } }, "Gaussian" },
	VOICE_OPTION(1), VOICE_OPTION(2), VOICE_OPTION(3), VOICE_OPTION(4),
	VOICE_OPTION(5), VOICE_OPTION(6), VOICE_OPTION(7), VOICE_OPTION(8),
	{ "armsnes_volume_gameboy", "Game Boy Volume", NULL, "The Super Game Boy's Game Boy sound.", NULL, "audio", VOLUME_VALUES, "100%" },
	{ "armsnes_volume_msu1", "MSU-1 Volume", NULL, "An MSU-1 game's music and sound tracks.", NULL, "audio", VOLUME_VALUES, "100%" },
	{ "armsnes_turbo_buttons", "Turbo Buttons", NULL,
		"Buttons that press and release over and over while held.",
		NULL, "input", { { "Off", NULL }, { "A", NULL }, { "B", NULL }, { "X", NULL }, { "Y", NULL }, { "A and B", NULL },
		{ "X and Y", NULL }, { "A, B, X and Y", NULL }, { NULL, NULL } }, "Off" },
	{ "armsnes_turbo_period", "Turbo Period", NULL,
		"Frames per press and release of a turbo button.",
		NULL, "input", { { NULL, NULL } }, "4" },
	{ "armsnes_opposing", "Allow Opposing Directions", NULL,
		"Left and right (or up and down) held together, which a real pad can't do. Some games glitch.",
		NULL, "input", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armsnes_left_stick_dpad", "Left Stick as D-Pad", NULL, "The left stick presses the D-pad too.",
		NULL, "input", { { "On", NULL }, { "Off", NULL }, { NULL, NULL } }, "On" },
	{ "armsnes_crosshair", "Light Gun Crosshair", NULL,
		"A cross where the Super Scope or Justifier points.",
		NULL, "input", { { "On", NULL }, { "Off", NULL }, { NULL, NULL } }, "On" },
	{ NULL, NULL, NULL, NULL, NULL, NULL, { { NULL, NULL } }, NULL },
};

static struct retro_core_option_v2_category categories[] = {
	{ "system", "System", "The core's mode and threads, the console's region, its memory at power-on and its chips' speeds." },
	{ "video", "Video", "The picture's shape, edges and colors." },
	{ "audio", "Audio", "The sound chip's interpolation and each voice's volume, and the cartridge's own sound." },
	{ "input", "Input", "Turbo, the D-pad and the light guns." },
	{ NULL, NULL, NULL },
};

static struct retro_core_options_v2 options = { categories, optionDefinitions };

// Options for chips and controllers the game doesn't have are hidden.
static bool showOptions(void) {
	const struct {
		const char* key;
		bool show;
	} shown[] = {
		{ "armsnes_threads", !optionIs(environment, "armsnes_mode", "Balanced") && !optionIs(environment, "armsnes_mode", "Exact") },
		{ "armsnes_frame_skip", !optionIs(environment, "armsnes_mode", "Balanced") && !optionIs(environment, "armsnes_mode", "Exact") },
		{ "armsnes_hd_mode7", !optionIs(environment, "armsnes_mode", "Balanced") && !optionIs(environment, "armsnes_mode", "Exact") },
		{ "armsnes_sprite_limit", !optionIs(environment, "armsnes_mode", "Balanced") && !optionIs(environment, "armsnes_mode", "Exact") },
		{ "armsnes_superfx_clock", !loaded || cart.gsu },
		{ "armsnes_sa1_clock", !loaded || cart.sa1 },
		{ "armsnes_crosshair", !loaded || inputGuns },
		{ "armsnes_volume_gameboy", !loaded || cart.icd },
		{ "armsnes_volume_msu1", !loaded || cart.msu1 },
		{ "armsnes_sgb_border", !loaded || cart.icd },
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
		if (!strcmp(d->key, "armsnes_turbo_period")) {
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


	static const struct retro_controller_description first[] = {
		{ "SNES Controller", RETRO_DEVICE_JOYPAD },
		{ "SNES Mouse", RETRO_DEVICE_MOUSE },
		{ "None", RETRO_DEVICE_NONE },
	};
	static const struct retro_controller_description second[] = {
		{ "SNES Controller", RETRO_DEVICE_JOYPAD },
		{ "SNES Mouse", RETRO_DEVICE_MOUSE },
		{ "Super Multitap", DEVICE_LR_MULTITAP },
		{ "Super Scope", DEVICE_LR_SUPER_SCOPE },
		{ "Justifier", DEVICE_LR_JUSTIFIER },
		{ "Two Justifiers", DEVICE_LR_JUSTIFIERS },
		{ "None", RETRO_DEVICE_NONE },
	};
	// A Super Multitap's other pads, or the second Justifier.
	static const struct retro_controller_description more[] = {
		{ "SNES Controller", RETRO_DEVICE_JOYPAD },
		{ "Justifier", RETRO_DEVICE_LIGHTGUN },
	};
	static const struct retro_controller_info ports[] = {
		{ first, 3 },
		{ second, 7 },
		{ more, 2 },
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
	info->library_version = "0.1a";
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
		return 4.0f / 3.0f;
	}
	double pixel = aspect == ASPECT_SQUARE ? 1.0 : sys.pal ? 1.3889 : 8.0 / 7.0;
	return (float) (width * pixel / lines);
}

static unsigned maxWidth(void) {
	return hdScale > 2 ? 256u * hdScale : OUTPUT_WIDTH;
}

static unsigned maxHeight(void) {
	return hdScale > 2 ? 239u * hdScale : OUTPUT_HEIGHT;
}

// The part of the output shown: the crops, or the Game Boy's screen without the Super Game Boy's border.
struct Shown {
	u32 x, y, width, height;
};

static struct Shown shownPart(void) {
	u32 xScale = outputWidth / (OUTPUT_WIDTH / 2), yScale = frameScaleY;
	u32 lines = outputHeight / yScale;
	struct Shown part = { cropSides, cropTopBottom, 256 - 2 * cropSides, lines > 2 * cropTopBottom ? lines - 2 * cropTopBottom : lines };
	if (sgbBorderHidden && cart.icd && lines >= 144) {
		part = (struct Shown) { 48, (lines - 144) / 2, 160, 144 };
	}
	part.x *= xScale;
	part.width *= xScale;
	part.y *= yScale;
	part.height *= yScale;
	return part;
}

static struct retro_game_geometry given;

// The frontend is told when the picture's shape changes, in the console's pixels and lines.
static void giveGeometry(const struct Shown* part) {
	u32 xScale = outputWidth / (OUTPUT_WIDTH / 2), yScale = frameScaleY;
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

void retro_set_controller_port_device(unsigned port, unsigned device) {
	if (port > 1) {
		return;
	}
	switch (device) {
	case RETRO_DEVICE_NONE: inputSetDevice(port, DEVICE_NONE); break;
	case RETRO_DEVICE_MOUSE: inputSetDevice(port, DEVICE_MOUSE); break;
	case DEVICE_LR_MULTITAP: inputSetDevice(port, DEVICE_MULTITAP); break;
	case DEVICE_LR_SUPER_SCOPE: inputSetDevice(port, DEVICE_SUPER_SCOPE); break;
	case DEVICE_LR_JUSTIFIER: inputSetDevice(port, DEVICE_JUSTIFIER); break;
	case DEVICE_LR_JUSTIFIERS: inputSetDevice(port, DEVICE_JUSTIFIERS); break;
	default: inputSetDevice(port, DEVICE_PAD); break;
	}
	showOptions();
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
static u8 volumeOption(const char* key) {
	const char* value = optionValue(environment, key);
	int percent = value ? atoi(value) : 100;
	return percent >= 0 && percent <= 100 ? (u8) percent : 100;
}

static bool regionPal(void) {
	return optionIs(environment, "armsnes_region", "PAL") || (cart.pal && !optionIs(environment, "armsnes_region", "NTSC"));
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
	const char* period = optionValue(environment, "armsnes_turbo_period");
	turboPeriod = period ? (u32) atoi(period) : 4;
	if (turboPeriod < TURBO_FEWEST || turboPeriod > TURBO_MOST) {
		turboPeriod = 4;
	}
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
	static const struct OptionChoice hdScales[] = { { "2x", 2 }, { "3x", 3 }, { "4x", 4 } };
	u8 scale = (u8) optionChoice(environment, "armsnes_hd_mode7", hdScales, 3, 0);
	if (scale != hdScale) {
		bool larger = maxWidth() < (scale > 2 ? 256u * scale : OUTPUT_WIDTH);
		hdScale = ppuHdScale = scale;
		// A larger picture than the frontend was told of needs a new AV info.
		if (loaded && larger) {
			struct retro_system_av_info av;
			retro_get_system_av_info(&av);
			environment(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av);
		}
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
		char key[32];
		snprintf(key, sizeof(key), "armsnes_volume_voice%d", n + 1);
		dspVoiceVolume[n] = volumeOption(key);
	}
	dspGameBoyVolume = volumeOption("armsnes_volume_gameboy");
	dspMsu1Volume = volumeOption("armsnes_volume_msu1");
	dspListenUpdate();
	allowOpposing = optionIs(environment, "armsnes_opposing", "On");
	leftStickDpad = !optionIs(environment, "armsnes_left_stick_dpad", "Off");
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

	static const struct retro_input_descriptor descriptors[] = {
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
		PAD(0),
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "Fast-Forward" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "Fast-Forward" },
		PAD(1),
		PAD(2),
		PAD(3),
		PAD(4),
#undef PAD
		{ 0, 0, 0, 0, NULL },
	};
	environment(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void*) descriptors);

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
