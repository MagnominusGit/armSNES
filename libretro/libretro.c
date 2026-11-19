/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "libretro.h"
#include "snes.h"
#include "fastppu.h"

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

static u8* game;
static bool loaded;
static bool bitmasks;

// The picture, in RGB565: up to 512 pixels by 239 lines, or twice the lines when interlaced.
enum { OUTPUT_WIDTH = 512, OUTPUT_HEIGHT = 478 };
static u16 output[OUTPUT_WIDTH * OUTPUT_HEIGHT];
static unsigned outputWidth = 256, outputHeight = 224;
static bool crosshair = true;
static bool outputReady;

// A pixel of the canvas, four bits of brightness over 15 bits of color, as RGB565. Brightness 0 isn't
// black but much darker than a linear scale would make it, as ares shows it.
static u16 channel[16][32];

static void makeColors(void) {
	for (int l = 0; l < 16; ++l) {
		double scale = (1.0 + l) / 16.0 * (l ? 1.0 : 0.25);
		for (int c = 0; c < 32; ++c) {
			channel[l][c] = (u16) (scale * (c << 3 | c >> 2) + 0.5);
		}
	}
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
	u32 dim = 0, mixed = 0;
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

// Fast mode's render thread converts each line it draws as it goes, for the last frame's crop, and
// marks its row with the frame's stamp; videoFrame converts the others.
static u32 cropX = 26, cropY = 9, cropHeight = 224;
static u32 rowStamp[CANVAS_HEIGHT];
static u32 stamp = 1;

static void lineDrawn(const u32* line) {
	u32 row = (u32) (line - ppuCanvas) / CANVAS_WIDTH;
	if (row - cropY < cropHeight && line == &ppuCanvas[row * CANVAS_WIDTH + cropX]) {
		convertRow(line, &output[(row - cropY) * OUTPUT_WIDTH], &rowWide[row - cropY]);
		rowStamp[row] = stamp;
	}
}

// The part of the canvas a television shows, as ares crops it without overscan.
void videoFrame(const u32* canvas) {
	bool interlace = ppu.state.interlace;
	u32 yScale = interlace ? 2 : 1;
	u32 height = sys.pal ? 288 : 242;
	u32 x = 26;
	u32 y = 9 * yScale;
	u32 h = height - 18;
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
	for (u32 row = 0; row < h; ++row) {
		if (!sameCrop || rowStamp[y + row] != stamp) {
			convertRow(&canvas[(y + row) * CANVAS_WIDTH + x], &output[row * OUTPUT_WIDTH], &rowWide[row]);
		}
	}
	bool wide = inputGuns && crosshair;
	for (u32 row = 0; row < h; ++row) {
		wide |= rowWide[row];
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
	cropX = x;
	cropY = y;
	cropHeight = h;
	stamp++;
	outputHeight = h;
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
	u32 yScale = outputHeight > 240 ? 2 : 1;
	for (int gun = 0; gun < 2; ++gun) {
		if (gunX[gun] < 0 || gunY[gun] < 0) {
			continue;
		}
		int cx = gunX[gun] * 2;
		int cy = gunY[gun] * yScale;
		for (int d = -7 * 2; d <= 7 * 2; ++d) {
			for (int w = -1; w <= 1; ++w) {
				u16 color = w ? 0x0000 : (gun ? 0xf800 : 0xffff);
				int x = cx + d, y = cy + w * (int) yScale;
				if (x >= 0 && x < OUTPUT_WIDTH && y >= 0 && y < (int) outputHeight && (d < -3 || d > 3)) {
					output[y * OUTPUT_WIDTH + x] = color;
				}
				x = cx + w * 2;
				y = cy + d / 2 * (int) yScale;
				if (x >= 0 && x < OUTPUT_WIDTH && y >= 0 && y < (int) outputHeight && (d < -3 || d > 3)) {
					output[y * OUTPUT_WIDTH + x] = color;
				}
			}
		}
	}
}

void retro_set_environment(retro_environment_t callback) {
	environment = callback;
	bool noGame = false;
	callback(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &noGame);
	struct retro_log_callback log;
	if (callback(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log)) {
		logPrintf = log.log;
	}

	// The first value is the default.
	static const struct retro_variable variables[] = {
		{ "armsnes_mode", "Mode; Fast|Balanced|Exact" },
		{ "armsnes_power_on_ram", "Power-On RAM; Cleared|Patterned" },
		{ "armsnes_crosshair", "Light Gun Crosshair; On|Off" },
		{ "armsnes_threads", "Fast Mode Threads; All|Picture Only|Off" },
		{ "armsnes_cpu_clock", "CPU Clock; 100%|125%|150%|175%|200%|225%|250%|275%|300%|325%|350%|375%|400%|25%|50%|75%" },
		{ "armsnes_superfx_clock", "Super FX Clock; 100%|125%|150%|175%|200%|225%|250%|275%|300%|325%|350%|375%|400%|25%|50%|75%" },
		{ "armsnes_sa1_clock", "SA-1 Clock; 100%|125%|150%|175%|200%|225%|250%|275%|300%|325%|350%|375%|400%|25%|50%|75%" },
		{ NULL, NULL },
	};
	callback(RETRO_ENVIRONMENT_SET_VARIABLES, (void*) variables);

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
	bitmasks = environment(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL);
}

void retro_deinit(void) {
	fastPpuStop();
	smpStop();
	gsuStop();
}

void retro_get_system_info(struct retro_system_info* info) {
	memset(info, 0, sizeof(*info));
	info->library_name = "armSNES";
	info->library_version = "0.1a";
	info->valid_extensions = "sfc|smc|bs|st|gb|gbc";
	info->need_fullpath = false;
	info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info* info) {
	memset(info, 0, sizeof(*info));
	info->geometry.base_width = OUTPUT_WIDTH / 2;
	info->geometry.base_height = sys.pal ? 239 : 224;
	info->geometry.max_width = OUTPUT_WIDTH;
	info->geometry.max_height = OUTPUT_HEIGHT;
	// SNES pixels are 8:7 on an NTSC television.
	info->geometry.aspect_ratio = sys.pal ? 256.0 * 1.3889 / 239.0 : 256.0 * 8.0 / 7.0 / 224.0;
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
}

void retro_reset(void) {
	if (loaded) {
		systemPower(true);
	}
}

static void readOptions(void);

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
	}
	satellaviewNow = (s64) time(NULL);
	readPads();
	outputReady = false;
	soundCount = 0;

	systemRunFrame();

	if (outputReady) {
		if (inputGuns && crosshair) {
			drawCrosshairs();
		}
		videoRefresh(output, outputWidth, outputHeight, OUTPUT_WIDTH * sizeof(u16));
	} else {
		videoRefresh(NULL, outputWidth, outputHeight, OUTPUT_WIDTH * sizeof(u16));
	}
	if (soundCount) {
		audioBatch(sound, soundCount);
	}
	if (rtcPresent()) {
		rtcSave(rtcMemory, (s64) time(NULL));
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
	systemSoundThread = !strcmp(threading, "All");
	systemPictureThread = strcmp(threading, "Off") != 0;
	struct retro_variable variable = { "armsnes_power_on_ram", NULL };
	systemClearPowerOn = !environment(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) || !variable.value ||
	                     strcmp(variable.value, "Patterned");
	systemCpuClock = clockOption("armsnes_cpu_clock");
	systemGsuClock = clockOption("armsnes_superfx_clock");
	systemSa1Clock = clockOption("armsnes_sa1_clock");
	struct retro_variable cross = { "armsnes_crosshair", NULL };
	crosshair = !environment(RETRO_ENVIRONMENT_GET_VARIABLE, &cross) || !cross.value || strcmp(cross.value, "Off");
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
		if (logPrintf) {
			logPrintf(RETRO_LOG_ERROR, "armSNES: a Game Boy game needs sgb1.boot.rom in the system folder\n");
		}
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
			if (logPrintf) {
				logPrintf(RETRO_LOG_ERROR, "armSNES: this needs %s in the system folder\n", base);
			}
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
		if (logPrintf) {
			logPrintf(RETRO_LOG_ERROR, "armSNES: the %s board can't be loaded without %s\n", cart.board, cart.missing);
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
	// The MSU-1's files sit beside the game: game.msu, game-1.pcm and on.
	if (info->path) {
		char path[4096];
		snprintf(path, sizeof(path), "%s", info->path);
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
	systemLoad(cart.pal);
	systemPower(false);
	if (rtcPresent()) {
		rtcSave(rtcMemory, (s64) time(NULL));
		memcpy(rtcStart, rtcMemory, sizeof(rtcStart));
	}
	rtcApplied = false;
	loaded = true;
	return true;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info* info, size_t count) {
	(void) type;
	(void) info;
	(void) count;
	return false;
}

void retro_unload_game(void) {
	if (fastForwardHeld) {
		setFastForward(false);
	}
	fastPpuStop();
	smpStop();
	gsuStop();
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
		return rtcPresent() ? rtcMemory : NULL;
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
		return rtcPresent() ? sizeof(rtcMemory) : 0;
	}
	return 0;
}
