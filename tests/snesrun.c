/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/snes.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// snesrun game frames: runs a game with no frontend and says what came out.
// env: FRAMELOG (a hash of every frame's canvas), AUDIOOUT (the sound, 16-bit stereo),
//      INPUT=1 (presses buttons on a fixed schedule; 2: A, Down, A every two seconds), INPUTSCRIPT (lines of
//      "frame button frames", six if the length is left out), PPMOUT (the last frame as a picture),
//      RAWOUT and RAWFRAME (the canvas of that frame, counted from 1), HASHLOG (a hash of the saved
//      state after every frame), RUNAHEAD=n (after each frame, n more are run and undone by a state),
//      RESETAT=n (the reset button is pressed after frame n), SYSDIR (where chip firmware is),
//      CLEARRAM (RAM starts at 0), PIRATE=n (a pirate chip to try: 1 latch, 2 Tekken 2's, 3 the 20-in-1's, 4 the latch
//      with its bank switch, 5 the four fixed bytes), BSPACK (a memory pack), SUFAMIA and SUFAMIB (Sufami Turbo cartridges), GAMEBOY (a Game Boy game for
//      the Super Game Boy, with sgb1.boot.rom from SYSDIR),
//      SAVEIN (the save RAM it starts with)

static FILE* frameLog;
static FILE* audioOut;
static u64 audioHash = 14695981039346656037ull;
static u64 samples;
static u64 lastHash;
static bool pressing;
// INPUT=2: A, Down, A every two seconds.
static bool pressingA;
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

static u32 lastFrame;

void videoFrame(const u32* canvas) {
	// Only the frames that are logged, and the last, are hashed, so timings are the core's.
	if (frameLog || sys.frames + 1 >= lastFrame) {
		lastHash = fnv(14695981039346656037ull, canvas, CANVAS_WIDTH * rows * sizeof(u32));
	}
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

bool cartFirmware(const char* name, u8* data, size_t size) {
	char path[4096];
	snprintf(path, sizeof(path), "%s/%s", getenv("SYSDIR") ? getenv("SYSDIR") : ".", name);
	FILE* file = fopen(path, "rb");
	if (!file) {
		return false;
	}
	bool read = fread(data, 1, size, file) == size;
	fclose(file);
	return read;
}

// The buttons a test run holds: Start, then A, then the D-pad, round and round. Both this and the
// reference runner go by the number of frames finished.
static u32 scriptFrame[4096];
static u16 scriptButtons[4096];
static u32 scriptHold[4096];
static int scriptLength;

// INPUTSCRIPT: lines of "frame button frames", each held that long (six frames if left out).
static void loadScript(const char* path) {
	static const char* names[] = { "B", "Y", "Select", "Start", "Up", "Down", "Left", "Right", "A", "X", "L", "R" };
	static const int pads[] = { PAD_B, PAD_Y, PAD_SELECT, PAD_START, PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_A, PAD_X, PAD_L, PAD_R };
	FILE* file = fopen(path, "r");
	char line[64], name[16];
	u32 frame, hold;
	while (file && scriptLength < 4096 && fgets(line, sizeof(line), file)) {
		int fields = sscanf(line, "%u %15s %u", &frame, name, &hold);
		if (fields < 2) {
			continue;
		}
		for (int n = 0; n < 12; n++) {
			if (!strcmp(name, names[n])) {
				scriptFrame[scriptLength] = frame;
				scriptHold[scriptLength] = fields == 3 ? hold : 6;
				scriptButtons[scriptLength++] = 1 << pads[n];
			}
		}
	}
	if (file) {
		fclose(file);
	}
}

u16 inputPoll(int port) {
	if (scriptLength) {
		u16 buttons = 0;
		for (int n = 0; port == 0 && n < scriptLength; n++) {
			if (sys.frames >= scriptFrame[n] && sys.frames < scriptFrame[n] + scriptHold[n]) {
				buttons |= scriptButtons[n];
			}
		}
		return buttons;
	}
	if (!pressing || port != 0) {
		return 0;
	}
	if (pressingA) {
		u32 step = sys.frames % 120;
		if (step >= 30 && step < 36) return 1 << PAD_A;
		if (step >= 60 && step < 66) return 1 << PAD_DOWN;
		if (step >= 90 && step < 96) return 1 << PAD_A;
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

	systemClearPowerOn = getenv("CLEARRAM") != NULL;
	cartTryPirate = getenv("PIRATE") ? atoi(getenv("PIRATE")) : 0;
	bool supported = cartLoad(rom, size);
	printf("board=%s region=%s\n", cart.board, cart.pal ? "PAL" : "NTSC");
	if (!supported) {
		fprintf(stderr, "can't load it without %s\n", cart.missing);
		return 3;
	}

	for (int slot = 0; slot < 2; slot++) {
		const char* path = getenv(slot ? "SUFAMIB" : "SUFAMIA");
		FILE* file = path ? fopen(path, "rb") : NULL;
		if (!file) {
			continue;
		}
		fseek(file, 0, SEEK_END);
		long length = ftell(file);
		fseek(file, 0, SEEK_SET);
		u8* data = malloc(length);
		if (fread(data, 1, length, file) != (size_t) length || !cartInsertSufami(slot, data, length)) {
			fprintf(stderr, "can't insert the Sufami Turbo cartridge\n");
			return 1;
		}
		fclose(file);
		free(data);
	}
	if (getenv("GAMEBOY")) {
		FILE* file = fopen(getenv("GAMEBOY"), "rb");
		u8 boot[256];
		char path[4096];
		snprintf(path, sizeof(path), "%s/sgb1.boot.rom", getenv("SYSDIR") ? getenv("SYSDIR") : ".");
		FILE* bootFile = fopen(path, "rb");
		if (!file || !bootFile || fread(boot, 1, sizeof(boot), bootFile) != sizeof(boot)) {
			fprintf(stderr, "can't read the Game Boy game or sgb1.boot.rom\n");
			return 1;
		}
		fclose(bootFile);
		fseek(file, 0, SEEK_END);
		long length = ftell(file);
		fseek(file, 0, SEEK_SET);
		u8* data = malloc(length);
		if (fread(data, 1, length, file) != (size_t) length || !cartInsertGameBoy(data, length, boot, sizeof(boot))) {
			fprintf(stderr, "can't insert the Game Boy game\n");
			return 1;
		}
		fclose(file);
		free(data);
	}
	// The MSU-1's files sit beside the game: game.msu, game-1.pcm and on.
	{
		char path[4096];
		snprintf(path, sizeof(path), "%s", argv[1]);
		char* dot = strrchr(path, '.');
		if (dot) {
			*dot = 0;
		}
		cartLoadMsu1(path);
	}
	if (getenv("BSPACK")) {
		FILE* pack = fopen(getenv("BSPACK"), "rb");
		if (!pack) {
			return 1;
		}
		fseek(pack, 0, SEEK_END);
		long packSize = ftell(pack);
		fseek(pack, 0, SEEK_SET);
		u8* data = malloc(packSize);
		if (fread(data, 1, packSize, pack) != (size_t) packSize || !cartInsertPack(data, packSize)) {
			fprintf(stderr, "can't insert the pack\n");
			return 1;
		}
		fclose(pack);
		free(data);
	}
	if (getenv("SAVEIN")) {
		FILE* save = fopen(getenv("SAVEIN"), "rb");
		if (save) {
			if (fread(cart.saveRam, 1, cart.saveRamSize, save) == 0) {
				fprintf(stderr, "the save is empty\n");
			}
			fclose(save);
		}
	}
	if (getenv("FRAMELOG")) {
		frameLog = fopen(getenv("FRAMELOG"), "w");
	}
	if (getenv("AUDIOOUT")) {
		audioOut = fopen(getenv("AUDIOOUT"), "wb");
	}
	if (getenv("INPUTSCRIPT")) {
		loadScript(getenv("INPUTSCRIPT"));
	}
	pressing = getenv("INPUT") != NULL;
	pressingA = pressing && !strcmp(getenv("INPUT"), "2");

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
	lastFrame = frames;
	clock_t start = clock();
	int resetAt = getenv("RESETAT") ? atoi(getenv("RESETAT")) : -1;
	while (sys.frames < frames) {
		systemRunFrame();
		if ((int) sys.frames == resetAt) {
			systemPower(true);
		}
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
