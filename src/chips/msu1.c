/* Copyright (c) 2004-2025 ares team, Near et al
 * Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Ported from ares (ISC); see LICENSES/ISC-ares.txt. */
#include "../snes.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The MSU-1: a data file the game reads a byte at a time, and 44.1 kHz audio tracks it plays, from
// files beside the game (game.msu, game-1.pcm and on), or in the zip the game came from.

struct Msu1 msu1;

#define M msu1

struct Msu1Archive msu1Archive;

// A file the MSU-1 reads: beside the game, or in the zip.
struct Source {
	FILE* file;
	void* zipped;
	size_t size, at;
};

static char base[4096];
// The zip and the name in it without the extension, when the game came from one.
static char archive[4096], inside[4096];
static struct Source dataSource, audioSource;
// The track audioSource holds from the zip: one opened is kept (as far as it was inflated) until
// another is played.
static u32 audioHeld = ~0u;
static u32 audioSize;

enum { REVISION = 0x02 };

static void sourceClose(struct Source* s) {
	if (s->file) {
		fclose(s->file);
	}
	if (s->zipped) {
		msu1Archive.close(s->zipped);
	}
	*s = (struct Source) { 0 };
}

static bool sourceOpen(struct Source* s, const char* suffix) {
	sourceClose(s);
	char name[4200];
	if (archive[0]) {
		snprintf(name, sizeof(name), "%s%s", inside, suffix);
		s->zipped = msu1Archive.open(archive, name, &s->size);
		return s->zipped != NULL;
	}
	snprintf(name, sizeof(name), "%s%s", base, suffix);
	s->file = fopen(name, "rb");
	if (s->file) {
		fseek(s->file, 0, SEEK_END);
		s->size = ftell(s->file);
		fseek(s->file, 0, SEEK_SET);
	}
	return s->file != NULL;
}

static bool sourceOpened(const struct Source* s) {
	return s->file || s->zipped;
}

static void sourceSeek(struct Source* s, u32 offset) {
	if (s->file) {
		fseek(s->file, offset, SEEK_SET);
	}
	s->at = offset;
}

static size_t sourceRead(struct Source* s, u8* to, size_t bytes) {
	if (s->file) {
		size_t done = fread(to, 1, bytes, s->file);
		s->at += done;
		return done;
	}
	size_t done = msu1Archive.read(s->zipped, s->at, to, bytes);
	s->at += done;
	return done;
}

// game-N.pcm, a track of the game's.
static int isTrack(const char* name, size_t length, const void* with) {
	const char* game = with;
	size_t gameLength = strlen(game);
	if (length < gameLength + 6 || memcmp(name, game, gameLength) || name[gameLength] != '-' || memcmp(name + length - 4, ".pcm", 4)) {
		return 0;
	}
	for (size_t at = gameLength + 1; at < length - 4; ++at) {
		if (name[at] < '0' || name[at] > '9') {
			return 0;
		}
	}
	return 1;
}

// Tracks beside the game, for one without its data file. ares looks only for the data file, but a few
// games come without it, and the MSU-1 reads as if it were empty then.
static bool tracksBeside(void) {
	const char* slash = strrchr(base, '/');
	char folder[4096];
	snprintf(folder, sizeof(folder), "%.*s", slash ? (int) (slash - base) : 1, slash ? base : ".");
	const char* game = slash ? slash + 1 : base;
	DIR* dir = opendir(folder);
	bool found = false;
	for (struct dirent* e; dir && !found && (e = readdir(dir));) {
		found = isTrack(e->d_name, strlen(e->d_name), game);
	}
	if (dir) {
		closedir(dir);
	}
	return found;
}

bool msu1Load(const char* path) {
	msu1Unload();
	snprintf(base, sizeof(base), "%s", path);
	// "archive.zip#name": the game was in the zip, and the MSU-1's files are beside it there.
	const char* mark = msu1Archive.open ? strchr(path, '#') : NULL;
	if (mark) {
		snprintf(archive, sizeof(archive), "%.*s", (int) (mark - path), path);
		snprintf(inside, sizeof(inside), "%s", mark + 1);
		// The data file is opened once, and kept.
		return sourceOpen(&dataSource, ".msu") || (msu1Archive.find && msu1Archive.find(archive, isTrack, inside));
	}
	char name[4200];
	snprintf(name, sizeof(name), "%s.msu", base);
	FILE* file = fopen(name, "rb");
	if (!file) {
		return tracksBeside();
	}
	fclose(file);
	return true;
}

void msu1Unload(void) {
	sourceClose(&dataSource);
	sourceClose(&audioSource);
	archive[0] = inside[0] = 0;
	audioHeld = ~0u;
}

static void dataOpen(void) {
	if (!archive[0] || !sourceOpened(&dataSource)) {
		sourceOpen(&dataSource, ".msu");
	}
	if (sourceOpened(&dataSource)) {
		sourceSeek(&dataSource, M.dataReadOffset);
	}
}

static u32 readLittle(struct Source* s, int bytes) {
	u8 data[4] = { 0 };
	if (sourceRead(s, data, bytes) != (size_t) bytes) {
		return 0;
	}
	return data[0] | data[1] << 8 | data[2] << 16 | (u32) data[3] << 24;
}

static void audioOpen(void) {
	if (!archive[0] || audioHeld != M.audioTrack) {
		char suffix[16];
		snprintf(suffix, sizeof(suffix), "-%u.pcm", M.audioTrack);
		audioHeld = sourceOpen(&audioSource, suffix) && archive[0] ? M.audioTrack : ~0u;
	}
	if (sourceOpened(&audioSource)) {
		audioSize = audioSource.size;
		sourceSeek(&audioSource, 0);
		u8 header[4];
		if (audioSize >= 8 && sourceRead(&audioSource, header, 4) == 4 && !memcmp(header, "MSU1", 4)) {
			M.audioLoopOffset = 8 + readLittle(&audioSource, 4) * 4;
			if (M.audioLoopOffset > audioSize) {
				M.audioLoopOffset = 8;
			}
			M.audioError = false;
			sourceSeek(&audioSource, M.audioPlayOffset);
			return;
		}
		sourceClose(&audioSource);
		audioHeld = ~0u;
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
		if (sourceOpened(&audioSource)) {
			if (M.audioPlayOffset >= audioSize) {
				if (!M.audioRepeat) {
					M.audioPlay = false;
					M.audioPlayOffset = 8;
				} else {
					M.audioPlayOffset = M.audioLoopOffset;
				}
				sourceSeek(&audioSource, M.audioPlayOffset);
			} else {
				M.audioPlayOffset += 4;
				left = (s16) readLittle(&audioSource, 2) * M.audioVolume / 255;
				right = (s16) readLittle(&audioSource, 2) * M.audioVolume / 255;
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
		u8 byte;
		if (M.dataBusy || !sourceOpened(&dataSource) || sourceRead(&dataSource, &byte, 1) != 1) {
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
		if (sourceOpened(&dataSource)) {
			sourceSeek(&dataSource, M.dataReadOffset);
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
