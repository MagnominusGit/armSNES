/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/snes.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// snesrun game frames: runs a game with no frontend and says what came out.
// env: FRAMELOG (a hash of every frame's canvas), AUDIOOUT (the sound, 16-bit stereo),
//      INPUT=1 (presses buttons on a fixed schedule), PPMOUT (the last frame as a picture),
//      RAWOUT and RAWFRAME (the canvas of that frame, counted from 1), HASHLOG (a hash of the saved
//      state after every frame), RUNAHEAD=n (after each frame, n more are run and undone by a state)

static FILE* frameLog;
static FILE* audioOut;
static u64 audioHash = 14695981039346656037ull;
static u64 samples;
static u64 lastHash;
static bool pressing;
static u32 rows;

static u64 fnv(u64 hash, const void* data, size_t size) {
	const u8* bytes = data;
	while (size--) {
		hash ^= *bytes++;
		hash *= 1099511628211ull;
	}
	return hash;
}

void audioSample(s16 left, s16 right) {
	s16 pair[2] = { left, right };
	audioHash = fnv(audioHash, pair, sizeof(pair));
	samples++;
	if (audioOut) {
		fwrite(pair, sizeof(pair), 1, audioOut);
	}
}

void videoFrame(const u32* canvas) {
	lastHash = fnv(14695981039346656037ull, canvas, CANVAS_WIDTH * rows * sizeof(u32));
	if (frameLog) {
		fprintf(frameLog, "%u %016llx\n", sys.frames, (unsigned long long) lastHash);
	}
	if (getenv("RAWOUT") && sys.frames + 1 == (u32) atoi(getenv("RAWFRAME"))) {
		FILE* out = fopen(getenv("RAWOUT"), "wb");
		fwrite(canvas, sizeof(u32), CANVAS_WIDTH * rows, out);
		fclose(out);
	}
	if (getenv("PPMOUT")) {
		FILE* out = fopen(getenv("PPMOUT"), "wb");
		fprintf(out, "P6\n%d %u\n255\n", CANVAS_WIDTH, rows);
		for (u32 n = 0; n < CANVAS_WIDTH * rows; ++n) {
			u32 pixel = canvas[n];
			u32 luma = pixel >> 15 & 15;
			u8 rgb[3] = { (pixel & 31) * 8 * (luma + 1) / 16, (pixel >> 5 & 31) * 8 * (luma + 1) / 16,
				      (pixel >> 10 & 31) * 8 * (luma + 1) / 16 };
			fwrite(rgb, 1, 3, out);
		}
		fclose(out);
	}
}

// The buttons a test run holds: Start, then A, then the D-pad, round and round. Both this and the
// reference runner go by the number of frames finished.
u16 inputPoll(int port) {
	if (!pressing || port != 0) {
		return 0;
	}
	u32 phase = sys.frames % 240;
	if (phase >= 60 && phase < 66) {
		return 1 << PAD_START;
	}
	if (phase >= 120 && phase < 126) {
		return 1 << PAD_A;
	}
	if (phase >= 150 && phase < 170) {
		return 1 << PAD_RIGHT;
	}
	if (phase >= 180 && phase < 186) {
		return 1 << PAD_B;
	}
	if (phase >= 200 && phase < 220) {
		return 1 << PAD_DOWN;
	}
	return 0;
}

int main(int argc, char** argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: snesrun game frames\n");
		return 2;
	}
	FILE* file = fopen(argv[1], "rb");
	if (!file) {
		fprintf(stderr, "can't open %s\n", argv[1]);
		return 1;
	}
	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	fseek(file, 0, SEEK_SET);
	u8* rom = malloc(size);
	if (fread(rom, 1, size, file) != (size_t) size) {
		return 1;
	}
	fclose(file);

	bool supported = cartLoad(rom, size);
	printf("board=%s region=%s\n", cart.board, cart.pal ? "PAL" : "NTSC");
	if (!supported) {
		fprintf(stderr, "this board isn't supported yet\n");
		return 3;
	}

	if (getenv("FRAMELOG")) {
		frameLog = fopen(getenv("FRAMELOG"), "w");
	}
	if (getenv("AUDIOOUT")) {
		audioOut = fopen(getenv("AUDIOOUT"), "wb");
	}
	pressing = getenv("INPUT") != NULL;

	bool pal = cart.pal;
	rows = (pal ? 288 : 242) * 2;
	systemLoad(pal);
	systemPower(false);

	FILE* hashLog = getenv("HASHLOG") ? fopen(getenv("HASHLOG"), "w") : NULL;
	int runAhead = getenv("RUNAHEAD") ? atoi(getenv("RUNAHEAD")) : 0;
	size_t stateBytes = stateSize();
	u8* state = malloc(stateBytes);
	u8* ahead = malloc(stateBytes);

	u32 frames = atoi(argv[2]);
	clock_t start = clock();
	while (sys.frames < frames) {
		systemRunFrame();
		if (runAhead) {
			// What a frontend's run-ahead does: the frames after this one are run, then taken back.
			FILE* log = frameLog;
			bool saveOut = audioOut != NULL;
			frameLog = NULL;
			FILE* audio = audioOut;
			audioOut = NULL;
			u64 hash = audioHash, count = samples, shown = lastHash;
			stateSave(ahead, stateBytes);
			for (int n = 0; n < runAhead; ++n) {
				systemRunFrame();
			}
			if (!stateLoad(ahead, stateBytes)) {
				fprintf(stderr, "the state didn't load\n");
				return 1;
			}
			frameLog = log;
			audioOut = saveOut ? audio : NULL;
			audioHash = hash;
			samples = count;
			lastHash = shown;
		}
		if (hashLog) {
			stateSave(state, stateBytes);
			fprintf(hashLog, "%u %016llx\n", sys.frames, (unsigned long long) fnv(14695981039346656037ull, state, stateBytes));
		}
	}
	double seconds = (double) (clock() - start) / CLOCKS_PER_SEC;

	printf("frames=%u seconds=%.3f fps=%.1f samples=%llu video=%016llx audio=%016llx\n", sys.frames, seconds,
	       sys.frames / seconds, (unsigned long long) samples, (unsigned long long) lastHash, (unsigned long long) audioHash);
	if (frameLog) {
		fclose(frameLog);
	}
	if (audioOut) {
		fclose(audioOut);
	}
	if (hashLog) {
		fclose(hashLog);
	}
	return 0;
}
