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

// LOCK=1: every frame is run in Exact, then again in Balanced from the same state; the picture, the
// sound and the memories are compared, and the run goes on from Exact's state.
static bool locking, lockKeep;
static s16 lockSound[2][8192];
static u32 lockSamples[2];
static u32* lockPicture[2];

void audioSample(s16 left, s16 right) {
	if (locking) {
		if (lockSamples[lockKeep] < 4096) {
			lockSound[lockKeep][lockSamples[lockKeep] * 2] = left;
			lockSound[lockKeep][lockSamples[lockKeep] * 2 + 1] = right;
		}
		lockSamples[lockKeep]++;
		if (lockKeep) {
			return;
		}
	}
	s16 pair[2] = { left, right };
	audioHash = fnv(audioHash, pair, sizeof(pair));
	samples++;
	if (audioOut) {
		fwrite(pair, sizeof(pair), 1, audioOut);
	}
}

static u32 lastFrame;

void videoFrame(const u32* canvas) {
	if (locking) {
		memcpy(lockPicture[lockKeep], canvas, CANVAS_WIDTH * rows * sizeof(u32));
		if (lockKeep) {
			return;
		}
	}
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

// DEVICE2=n plugs a device into the second port (the numbers of snes.h's DEVICE_ list), MOUSE1=1 a
// mouse into the first. A mouse moves in a slow circle and clicks; a gun sweeps the picture and
// fires every second.
void inputPollMouse(int port, int* x, int* y, bool* left, bool* right) {
	(void) port;
	u32 phase = sys.frames / 30 % 4;
	*x = phase == 0 ? 2 : phase == 2 ? -2 : 0;
	*y = phase == 1 ? 2 : phase == 3 ? -2 : 0;
	*left = sys.frames % 120 >= 60 && sys.frames % 120 < 66;
	*right = false;
}

void inputPollGun(int gun, int* x, int* y, u8* buttons) {
	*x = (sys.frames * 2 + gun * 100) % 256;
	*y = (sys.frames + gun * 50) % 200 + 10;
	*buttons = sys.frames % 60 < 4 ? 1 << GUN_TRIGGER : 0;
	if (sys.frames % 240 >= 200 && sys.frames % 240 < 206) {
		*buttons |= 1 << GUN_PAUSE;
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
	if (getenv("DEVICE2")) {
		inputSetDevice(1, atoi(getenv("DEVICE2")));
	}
	if (getenv("MOUSE1")) {
		inputSetDevice(0, DEVICE_MOUSE);
	}
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
	// BALANCED=1: Balanced mode.
	systemBalanced = getenv("BALANCED") != NULL;
	// CHEAT=codes: cheat codes from the start.
	if (getenv("CHEAT") && !cheatAdd(getenv("CHEAT"))) {
		fprintf(stderr, "no cheat code in %s\n", getenv("CHEAT"));
		return 1;
	}
	locking = getenv("LOCK") != NULL;
	u8* before = locking ? malloc(stateBytes) : NULL;
	u8* memories = locking ? malloc(sizeof(cpu.wram) + sizeof(ppu.vram) + sizeof(ppu.oam) + sizeof(ppu.cgram) + sizeof(dsp.apuram)) : NULL;
	lockPicture[0] = malloc(CANVAS_WIDTH * CANVAS_HEIGHT * sizeof(u32));
	lockPicture[1] = malloc(CANVAS_WIDTH * CANVAS_HEIGHT * sizeof(u32));
	FILE* lockLog = getenv("LOCKLOG") ? fopen(getenv("LOCKLOG"), "w") : NULL;
	u64 lockFrames = 0, pictureFrames = 0, pixelsOff = 0, soundFrames = 0, countFrames = 0, memoryFrames[5] = { 0 };
	while (locking && sys.frames < frames) {
		stateSave(before, stateBytes);
		systemBalanced = false;
		lockKeep = false;
		lockSamples[0] = lockSamples[1] = 0;
		systemRunFrame();
		stateSave(state, stateBytes);
		struct { void* at; size_t size; const char* name; } parts[5] = {
			{ cpu.wram, sizeof(cpu.wram), "wram" }, { ppu.vram, sizeof(ppu.vram), "vram" }, { ppu.oam, sizeof(ppu.oam), "oam" },
			{ ppu.cgram, sizeof(ppu.cgram), "cgram" }, { dsp.apuram, sizeof(dsp.apuram), "apuram" },
		};
		size_t at = 0;
		for (int i = 0; i < 5; ++i) {
			memcpy(memories + at, parts[i].at, parts[i].size);
			at += parts[i].size;
		}
		u32 frame = sys.frames;
		stateLoad(before, stateBytes);
		systemBalanced = true;
		cpuFastReplan();
		lockKeep = true;
		systemRunFrame();
		// The picture: pixels that differ. The sound: the samples both made, compared in order.
		u32 pixels = 0, x0 = CANVAS_WIDTH, x1 = 0, y0 = rows, y1 = 0;
		// Only the picture's lines (1 to the last before the vertical blank) count: what is below them
		// isn't shown.
		u32 scale = ppu.state.interlace ? 2 : 1, top = ((ppu.state.overscan ? 1 : 9) + (sys.pal ? 20 : 0)) * scale;
		u32 bottom = top + (ppu.state.vdisp - 1) * scale;
		for (u32 i = top * CANVAS_WIDTH; i < bottom * CANVAS_WIDTH && i < CANVAS_WIDTH * rows; ++i) {
			if (lockPicture[0][i] != lockPicture[1][i]) {
				u32 x = i % CANVAS_WIDTH, y = i / CANVAS_WIDTH;
				pixels++;
				x0 = x < x0 ? x : x0, x1 = x > x1 ? x : x1, y0 = y < y0 ? y : y0, y1 = y > y1 ? y : y1;
			}
		}
		u32 common = lockSamples[0] < lockSamples[1] ? lockSamples[0] : lockSamples[1], soundOff = 0;
		common = common > 4096 ? 4096 : common;
		for (u32 i = 0; i < common * 2; ++i) {
			soundOff += lockSound[0][i] != lockSound[1][i];
		}
		u32 bytes[5];
		at = 0;
		for (int i = 0; i < 5; ++i) {
			bytes[i] = 0;
			const u8* was = memories + at;
			const u8* now = parts[i].at;
			for (size_t n = 0; n < parts[i].size; ++n) {
				bytes[i] += was[n] != now[n];
			}
			memoryFrames[i] += bytes[i] != 0;
			at += parts[i].size;
		}
		lockFrames++;
		pictureFrames += pixels != 0;
		pixelsOff += pixels;
		soundFrames += soundOff != 0;
		countFrames += lockSamples[0] != lockSamples[1];
		if (lockLog && (pixels || soundOff || lockSamples[0] != lockSamples[1] || bytes[0] || bytes[1] || bytes[2] || bytes[3] || bytes[4])) {
			if (pixels) {
				fprintf(lockLog, "%u box %u,%u-%u,%u\n", frame, x0, y0, x1, y1);
			}
			fprintf(lockLog, "%u pixels %u sound %u of %u (%u/%u) wram %u vram %u oam %u cgram %u apuram %u\n", frame, pixels, soundOff, common * 2,
			        lockSamples[0], lockSamples[1], bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);
		}
		stateLoad(state, stateBytes);
		systemBalanced = false;
	}
	if (locking) {
		printf("lock frames=%llu picture=%llu (pixels %.0f a frame) sound=%llu count=%llu wram=%llu vram=%llu oam=%llu cgram=%llu apuram=%llu\n",
		       (unsigned long long) lockFrames, (unsigned long long) pictureFrames, pictureFrames ? (double) pixelsOff / pictureFrames : 0,
		       (unsigned long long) soundFrames, (unsigned long long) countFrames, (unsigned long long) memoryFrames[0],
		       (unsigned long long) memoryFrames[1], (unsigned long long) memoryFrames[2], (unsigned long long) memoryFrames[3],
		       (unsigned long long) memoryFrames[4]);
		if (lockLog) {
			fclose(lockLog);
		}
	}
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
