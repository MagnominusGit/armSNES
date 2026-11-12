/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "libretro.h"
#include "snes.h"

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <time.h>

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
static unsigned outputHeight = 224;
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

	for (u32 row = 0; row < h; ++row) {
		const u32* from = &canvas[(y + row) * CANVAS_WIDTH + x];
		u16* to = &output[row * OUTPUT_WIDTH];
		for (u32 n = 0; n < OUTPUT_WIDTH; ++n) {
			to[n] = rgb565(from[n]);
		}
	}
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
static u8* systemFile(const char* name, size_t* size) {
	const char* folder = NULL;
	if (!environment(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &folder) || !folder) {
		return NULL;
	}
	char path[4096];
	snprintf(path, sizeof(path), "%s/%s", folder, name);
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

static bool isPack(const char* path) {
	const char* dot = path ? strrchr(path, '.') : NULL;
	return dot && !strcasecmp(dot, ".bs");
}

// The cartridge's clock as the frontend keeps it. What the frontend gives back after loading the game
// is applied before the first frame; until then it holds the clock as the game started.
static u8 rtcMemory[16];
static u8 rtcStart[16];
static bool rtcApplied;

// The joypad's buttons are numbered as the pad's shift register sends them.
static u16 held[2];

u16 inputPoll(int port) {
	return held[port];
}

static void readPads(void) {
	inputPollFrontend();
	for (unsigned port = 0; port < 2; ++port) {
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
}

void retro_set_environment(retro_environment_t callback) {
	environment = callback;
	bool noGame = false;
	callback(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &noGame);
	struct retro_log_callback log;
	if (callback(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log)) {
		logPrintf = log.log;
	}

	static const struct retro_controller_description pads[] = {
		{ "SNES Controller", RETRO_DEVICE_JOYPAD },
	};
	static const struct retro_controller_info ports[] = {
		{ pads, 1 },
		{ pads, 1 },
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
	bitmasks = environment(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL);
}

void retro_deinit(void) {
}

void retro_get_system_info(struct retro_system_info* info) {
	memset(info, 0, sizeof(*info));
	info->library_name = "armSNES";
	info->library_version = "0.1";
	info->valid_extensions = "sfc|smc|bs";
	info->need_fullpath = false;
	info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info* info) {
	memset(info, 0, sizeof(*info));
	info->geometry.base_width = OUTPUT_WIDTH;
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
	(void) port;
	(void) device;
}

void retro_reset(void) {
	if (loaded) {
		systemPower(true);
	}
}

void retro_run(void) {
	if (!rtcApplied) {
		rtcApplied = true;
		if (rtcPresent() && memcmp(rtcMemory, rtcStart, sizeof(rtcMemory))) {
			rtcLoad(rtcMemory, (s64) time(NULL));
		}
	}
	readPads();
	outputReady = false;
	soundCount = 0;

	systemRunFrame();

	if (outputReady) {
		videoRefresh(output, OUTPUT_WIDTH, outputHeight, OUTPUT_WIDTH * sizeof(u16));
	} else {
		videoRefresh(NULL, OUTPUT_WIDTH, outputHeight, OUTPUT_WIDTH * sizeof(u16));
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
	return loaded && stateSave(data, size);
}

bool retro_unserialize(const void* data, size_t size) {
	return loaded && stateLoad(data, size);
}

void retro_cheat_reset(void) {
}

void retro_cheat_set(unsigned index, bool enabled, const char* code) {
	(void) index;
	(void) enabled;
	(void) code;
}

bool retro_load_game(const struct retro_game_info* info) {
	if (!info || !info->data) {
		return false;
	}

	enum retro_pixel_format format = RETRO_PIXEL_FORMAT_RGB565;
	if (!environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format)) {
		return false;
	}

	free(game);
	size_t size = info->size;
	// A memory pack goes in the BS-X cartridge's slot.
	bool pack = isPack(info->path);
	if (pack) {
		game = systemFile("BS-X.bin", &size);
		if (!game) {
			if (logPrintf) {
				logPrintf(RETRO_LOG_ERROR, "armSNES: a memory pack needs BS-X.bin in the system folder\n");
			}
			return false;
		}
	} else {
		game = malloc(size);
		memcpy(game, info->data, size);
	}
	if (!cartLoad(game, size) || (pack && !cartInsertPack(info->data, info->size))) {
		if (logPrintf) {
			logPrintf(RETRO_LOG_ERROR, "armSNES: the %s board can't be loaded without %s\n", cart.board, cart.missing);
		}
		free(game);
		game = NULL;
		return false;
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
		PAD(1),
#undef PAD
		{ 0, 0, 0, 0, NULL },
	};
	environment(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void*) descriptors);

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
	loaded = false;
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
