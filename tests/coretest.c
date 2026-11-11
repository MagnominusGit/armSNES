/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "libretro.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// coretest core game frames [picture.ppm]: loads the libretro core as a frontend would, runs it with
// nothing pressed, and writes the last picture it gave.

static uint16_t last[512 * 478];
static unsigned lastWidth, lastHeight;
static size_t samples;

static bool environment(unsigned cmd, void* data) {
	switch (cmd) {
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		return *(enum retro_pixel_format*) data == RETRO_PIXEL_FORMAT_RGB565;
	case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
		return true;
	default:
		return false;
	}
}

static void video(const void* data, unsigned width, unsigned height, size_t pitch) {
	if (!data) {
		return;
	}
	lastWidth = width;
	lastHeight = height;
	for (unsigned y = 0; y < height; ++y) {
		memcpy(&last[y * width], (const uint8_t*) data + y * pitch, width * 2);
	}
}

static size_t audio(const int16_t* data, size_t frames) {
	(void) data;
	samples += frames;
	return frames;
}

static void inputPoll(void) {
}

static int16_t inputState(unsigned port, unsigned device, unsigned index, unsigned id) {
	return 0;
}

#define LOAD(name) \
	__typeof__(&name) p_##name = (__typeof__(&name)) dlsym(core, #name); \
	if (!p_##name) { \
		fprintf(stderr, "no %s\n", #name); \
		return 1; \
	}

int main(int argc, char** argv) {
	if (argc < 4) {
		fprintf(stderr, "usage: coretest core game frames [picture.ppm]\n");
		return 2;
	}
	void* core = dlopen(argv[1], RTLD_NOW);
	if (!core) {
		fprintf(stderr, "%s\n", dlerror());
		return 1;
	}
	LOAD(retro_set_environment);
	LOAD(retro_set_video_refresh);
	LOAD(retro_set_audio_sample_batch);
	LOAD(retro_set_input_poll);
	LOAD(retro_set_input_state);
	LOAD(retro_init);
	LOAD(retro_load_game);
	LOAD(retro_get_system_av_info);
	LOAD(retro_run);
	LOAD(retro_unload_game);
	LOAD(retro_deinit);

	p_retro_set_environment(environment);
	p_retro_set_video_refresh(video);
	p_retro_set_audio_sample_batch(audio);
	p_retro_set_input_poll(inputPoll);
	p_retro_set_input_state(inputState);
	p_retro_init();

	FILE* file = fopen(argv[2], "rb");
	if (!file) {
		return 1;
	}
	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	fseek(file, 0, SEEK_SET);
	void* data = malloc(size);
	if (fread(data, 1, size, file) != (size_t) size) {
		return 1;
	}
	fclose(file);

	struct retro_game_info game = { argv[2], data, size, NULL };
	if (!p_retro_load_game(&game)) {
		fprintf(stderr, "the core didn't load the game\n");
		return 1;
	}
	struct retro_system_av_info av;
	p_retro_get_system_av_info(&av);
	printf("%ux%u aspect %.4f, %.4f fps, %.1f Hz\n", av.geometry.base_width, av.geometry.base_height, av.geometry.aspect_ratio,
	       av.timing.fps, av.timing.sample_rate);

	int frames = atoi(argv[3]);
	for (int n = 0; n < frames; ++n) {
		p_retro_run();
	}
	printf("%d frames, %zu samples (%.1f a frame), last picture %ux%u\n", frames, samples, (double) samples / frames, lastWidth, lastHeight);

	if (argc > 4) {
		FILE* out = fopen(argv[4], "wb");
		fprintf(out, "P6\n%u %u\n255\n", lastWidth, lastHeight);
		for (unsigned n = 0; n < lastWidth * lastHeight; ++n) {
			uint16_t p = last[n];
			uint8_t rgb[3] = { (p >> 11) << 3 | (p >> 13), (p >> 5 & 63) << 2 | (p >> 9 & 3), (p & 31) << 3 | (p >> 2 & 7) };
			fwrite(rgb, 1, 3, out);
		}
		fclose(out);
	}

	p_retro_unload_game();
	p_retro_deinit();
	return 0;
}
