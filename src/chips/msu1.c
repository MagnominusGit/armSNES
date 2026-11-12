/* Copyright (c) 2004-2025 ares team, Near et al
 * Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Ported from ares (ISC); see LICENSES/ISC-ares.txt. */
#include "../snes.h"

#include <stdio.h>
#include <string.h>

// The MSU-1: a data file the game reads a byte at a time, and 44.1 kHz audio tracks it plays, from
// files beside the game (game.msu, game-1.pcm and on).

struct Msu1 msu1;

#define M msu1

static char base[4096];
static FILE* dataFile;
static FILE* audioFile;
static u32 audioSize;

enum { REVISION = 0x02 };

bool msu1Load(const char* path) {
	msu1Unload();
	snprintf(base, sizeof(base), "%s", path);
	char name[4200];
	snprintf(name, sizeof(name), "%s.msu", base);
	FILE* file = fopen(name, "rb");
	if (!file) {
		return false;
	}
	fclose(file);
	return true;
}

void msu1Unload(void) {
	if (dataFile) {
		fclose(dataFile);
	}
	if (audioFile) {
		fclose(audioFile);
	}
	dataFile = audioFile = NULL;
}

static void dataOpen(void) {
	if (dataFile) {
		fclose(dataFile);
	}
	char name[4200];
	snprintf(name, sizeof(name), "%s.msu", base);
	dataFile = fopen(name, "rb");
	if (dataFile) {
		fseek(dataFile, M.dataReadOffset, SEEK_SET);
	}
}

static u32 readLittle(FILE* file, int bytes) {
	u8 data[4] = { 0 };
	if (fread(data, 1, bytes, file) != (size_t) bytes) {
		return 0;
	}
	return data[0] | data[1] << 8 | data[2] << 16 | (u32) data[3] << 24;
}

static void audioOpen(void) {
	if (audioFile) {
		fclose(audioFile);
	}
	char name[4200];
	snprintf(name, sizeof(name), "%s-%u.pcm", base, M.audioTrack);
	audioFile = fopen(name, "rb");
	if (audioFile) {
		fseek(audioFile, 0, SEEK_END);
		audioSize = ftell(audioFile);
		fseek(audioFile, 0, SEEK_SET);
		u8 header[4];
		if (audioSize >= 8 && fread(header, 1, 4, audioFile) == 4 && !memcmp(header, "MSU1", 4)) {
			M.audioLoopOffset = 8 + readLittle(audioFile, 4) * 4;
			if (M.audioLoopOffset > audioSize) {
				M.audioLoopOffset = 8;
			}
			M.audioError = false;
			fseek(audioFile, M.audioPlayOffset, SEEK_SET);
			return;
		}
		fclose(audioFile);
		audioFile = NULL;
	}
	M.audioError = true;
}

void msu1Power(void) {
	M.th.clock = 0;
	threadFrequency(&M.th, 44100);
	M.dataSeekOffset = 0;
	M.dataReadOffset = 0;
	M.audioPlayOffset = 0;
	M.audioLoopOffset = 0;
	M.audioTrack = 0;
	M.audioVolume = 0;
	M.audioResumeTrack = ~0u;
	M.audioResumeOffset = 0;
	M.audioError = false;
	M.audioPlay = false;
	M.audioRepeat = false;
	M.audioBusy = false;
	M.dataBusy = false;
	memset(&M.sound, 0, sizeof(M.sound));
	dataOpen();
	audioOpen();
}

// A state's offsets, with the files opened again where they were.
void msu1Reopen(void) {
	dataOpen();
	audioOpen();
}

static void sample(void) {
	s16 left = 0, right = 0;
	if (M.audioPlay) {
		if (audioFile) {
			if (M.audioPlayOffset >= audioSize) {
				if (!M.audioRepeat) {
					M.audioPlay = false;
					M.audioPlayOffset = 8;
				} else {
					M.audioPlayOffset = M.audioLoopOffset;
				}
				fseek(audioFile, M.audioPlayOffset, SEEK_SET);
			} else {
				M.audioPlayOffset += 4;
				left = (s16) readLittle(audioFile, 2) * M.audioVolume / 255;
				right = (s16) readLittle(audioFile, 2) * M.audioVolume / 255;
				if (dsp.mainvol.mute) {
					left = right = 0;
				}
			}
		} else {
			M.audioPlay = false;
		}
	}
	mixerInput(&M.sound, left, right, 32040, 44100);
}

void msu1Run(void) {
	do {
		sample();
		threadStep(&M.th, 1);
	} while (!(cpu.th.clock < M.th.clock));
}

static void sync(void) {
	if (M.th.clock < cpu.th.clock) {
		msu1Run();
	}
}

u8 msu1Read(u32 address, u8 data) {
	sync();
	switch (address & 7) {
	case 0:
		return REVISION | M.audioError << 3 | M.audioPlay << 4 | M.audioRepeat << 5 | M.audioBusy << 6 |
		       M.dataBusy << 7;
	case 1: {
		if (M.dataBusy || !dataFile) {
			return 0x00;
		}
		int byte = fgetc(dataFile);
		if (byte == EOF) {
			return 0x00;
		}
		M.dataReadOffset++;
		return byte;
	}
	case 2: return 'S';
	case 3: return '-';
	case 4: return 'M';
	case 5: return 'S';
	case 6: return 'U';
	case 7: return '1';
	}
	return data;
}

void msu1Write(u32 address, u8 data) {
	sync();
	switch (address & 7) {
	case 0: M.dataSeekOffset = (M.dataSeekOffset & 0xffffff00) | data; break;
	case 1: M.dataSeekOffset = (M.dataSeekOffset & 0xffff00ff) | data << 8; break;
	case 2: M.dataSeekOffset = (M.dataSeekOffset & 0xff00ffff) | data << 16; break;
	case 3:
		M.dataSeekOffset = (M.dataSeekOffset & 0x00ffffff) | (u32) data << 24;
		M.dataReadOffset = M.dataSeekOffset;
		if (dataFile) {
			fseek(dataFile, M.dataReadOffset, SEEK_SET);
		}
		break;
	case 4: M.audioTrack = (M.audioTrack & 0xff00) | data; break;
	case 5:
		M.audioTrack = (M.audioTrack & 0x00ff) | data << 8;
		M.audioPlay = false;
		M.audioRepeat = false;
		M.audioPlayOffset = 8;
		if (M.audioTrack == M.audioResumeTrack) {
			M.audioPlayOffset = M.audioResumeOffset;
			M.audioResumeTrack = ~0u;
			M.audioResumeOffset = 0;
		}
		audioOpen();
		break;
	case 6:
		M.audioVolume = data;
		break;
	case 7:
		if (M.audioBusy || M.audioError) {
			break;
		}
		M.audioPlay = data & 1;
		M.audioRepeat = data >> 1 & 1;
		if (!M.audioPlay && (data & 4)) {
			M.audioResumeTrack = M.audioTrack;
			M.audioResumeOffset = M.audioPlayOffset;
		}
		break;
	}
}
