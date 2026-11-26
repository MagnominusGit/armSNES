/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "sgb.h"
#include "../../armgb/gb.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

// The Super Game Boy's Game Boy: armGB, armGBA's, with the ICD2 taking its JOYP and its lines.

static struct GB* gb;
// The game, padded to a power of two, at least 32 KiB, as armGB banks it; and the boot ROM.
static uint8_t* rom;
static uint8_t* boot;

static uint8_t joypad(void* user, uint8_t select, bool write) {
	(void) user;
	return write ? icdJoypWrite(select >> 4 & 1, select >> 5 & 1) : icdJoypRead();
}

static void line(void* user, int ly, const uint16_t* shades) {
	(void) user;
	icdLine(ly, shades);
}

// The .rtc file, as armGBA lays it out (libretro/gb-armgb.c there):
// - 8 bytes: the Unix time the clock would have started from, gambatte-libretro's whole file;
// - "aGBm", and the Unix time it was saved;
// - an MBC3's registers at that time, 4 bytes each, seconds first, then the time again: what mGBA
//   reads of its own file;
// - the cycles into the second and the latched registers;
// - armGBA's Clock Offset and what of it the clock couldn't take, kept as they were.
// A HuC3 chip's file has "aGBh" and the chip's whole state after the saved time.
enum {
	RTC_BASE = 0,
	RTC_MAGIC = 8,
	RTC_SAVED = 12,
	RTC_REGS = 20,
	MGBA_REGS = 20,
	MGBA_SAVED = 40,
	RTC_CYCLES = 48,
	RTC_LATCH = 52,
	RTC_MOVED = 64,
	RTC_SHORT = 72,
	RTC_SIZE = 80,
	OLD_OFFSET = RTC_REGS + 14,
	HUC3_STATE = GB_HUC3_STATE,
	HUC3_SIZE = RTC_REGS + HUC3_STATE,
	HUC3_MINUTES = 4 + 0x10,
	HUC3_DAYS = 4 + 0x13,
	// A host clock before 2024 was never set, so it says nothing about the time the game was off.
	SANE_TIME = 1704067200,
};
// The registers in the order armGB gives them (gbRtcGet), and the length of a second in its ticks.
enum { RTC_DH, RTC_DL, RTC_H, RTC_M, RTC_S, RTC_C, RTC_LATCHED };
static unsigned long const RTC_SECOND = GB_RTC_SECOND;

static unsigned char rtcFile[HUC3_SIZE];

static void put32(unsigned char* to, unsigned long value) {
	for (int i = 0; i < 4; ++i) {
		to[i] = value >> i * 8;
	}
}

static unsigned long get32(const unsigned char* from) {
	return from[0] | from[1] << 8 | from[2] << 16 | (unsigned long) from[3] << 24;
}

static void put64(unsigned char* to, unsigned long long value) {
	for (int i = 0; i < 8; ++i) {
		to[i] = value >> i * 8;
	}
}

static unsigned long long get64(const unsigned char* from) {
	unsigned long long value = 0;
	for (int i = 7; i >= 0; --i) {
		value = value << 8 | from[i];
	}
	return value;
}

static unsigned long getNibbles(const unsigned char* at) {
	return (at[0] & 0xF) | (at[1] & 0xF) << 4 | (at[2] & 0xF) << 8;
}

static void putNibbles(unsigned char* at, unsigned long value) {
	for (int i = 0; i < 3; ++i) {
		at[i] = value >> i * 4 & 0xF;
	}
}

// A HuC3 clock moved on by some seconds the way the chip counts: minutes up to 1440, then days up to
// 4096.
static void runHuC3(unsigned char* state, unsigned long long seconds) {
	unsigned long long const minute = RTC_SECOND * 60;
	unsigned long long cycles = (unsigned long long) state[0] << 24 | state[1] << 16 | state[2] << 8 | state[3];
	cycles = cycles % minute + seconds % 60 * RTC_SECOND;
	unsigned long long minutes = getNibbles(state + HUC3_MINUTES) + seconds / 60 + cycles / minute;
	cycles %= minute;
	unsigned long long const days = getNibbles(state + HUC3_DAYS) + minutes / 1440;
	putNibbles(state + HUC3_MINUTES, minutes % 1440);
	putNibbles(state + HUC3_DAYS, days & 0xFFF);
	for (int i = 0; i < 4; ++i) {
		state[i] = cycles >> (24 - i * 8);
	}
}

// An MBC3 clock moved on by some seconds the way the chip counts: a field past its range counts up to
// its last value and wraps to 0 without a carry; past 511 days the day counter wraps and sets its
// carry bit.
static void runClock(uint32_t r[11], unsigned long long seconds) {
	if (r[RTC_DH] & 0x40) {
		return;
	}
	static const unsigned limits[3] = { 60, 60, 24 };
	static const unsigned ends[3] = { 64, 64, 32 };
	static const int fields[3] = { RTC_S, RTC_M, RTC_H };
	unsigned long long carry = seconds;
	for (int i = 0; i < 3 && carry; ++i) {
		uint32_t* value = &r[fields[i]];
		if (*value >= limits[i]) {
			if (carry < ends[i] - *value) {
				*value += carry;
				return;
			}
			carry -= ends[i] - *value;
			*value = 0;
		}
		unsigned long long const total = *value + carry;
		*value = total % limits[i];
		carry = total / limits[i];
	}
	unsigned long long days = ((r[RTC_DH] & 1) << 8 | r[RTC_DL]) + carry;
	if (days > 511) {
		r[RTC_DH] |= 0x80;
	}
	r[RTC_DL] = days & 0xFF;
	r[RTC_DH] = (r[RTC_DH] & 0xFE) | (days >> 8 & 1);
}

// Whether a file holds mGBA's registers and the time they were saved, sane ones.
static bool mgbaFile(const unsigned char* file, unsigned long long now) {
	static const unsigned ends[5] = { 64, 64, 32, 256, 256 };
	unsigned long long const saved = get64(file + MGBA_SAVED);
	for (int i = 0; i < 5; ++i) {
		if (get32(file + MGBA_REGS + i * 4) >= ends[i]) {
			return false;
		}
	}
	return saved >= SANE_TIME / 2 && saved <= now + 366 * 86400ull;
}

static bool hasMbc3Clock(void) {
	return gb && gb->cart.mapper == GB_MBC3 && gb->cart.hasRtc;
}

static bool hasHuC3(void) {
	return gb && gb->cart.mapper == GB_HUC3;
}

bool sgbLoad(const unsigned char* data, size_t size, const unsigned char* bootData, size_t bootSize) {
	sgbUnload();
	size_t room = 0x8000;
	while (room < size) {
		room <<= 1;
	}
	gb = calloc(1, sizeof(*gb));
	rom = calloc(room, 1);
	boot = malloc(bootSize ? bootSize : 1);
	if (!gb || !rom || !boot) {
		sgbUnload();
		return false;
	}
	memcpy(rom, data, size);
	memcpy(boot, bootData, bootSize);
	gb->boot = boot;
	gb->bootSize = bootSize;
	if (!gbLoad(gb, rom, room, GB_MODEL_DMG)) {
		sgbUnload();
		return false;
	}
	gb->sgbJoypad = joypad;
	gb->sgbLine = line;
	memset(rtcFile, 0, sizeof(rtcFile));
	return true;
}

void sgbUnload(void) {
	free(gb);
	free(rom);
	free(boot);
	gb = NULL;
	rom = boot = NULL;
}

bool sgbLoaded(void) {
	return gb != NULL;
}

void sgbReset(void) {
	gbReset(gb);
}

unsigned sgbRun(unsigned dots, unsigned* samples, const int16_t** audio) {
	uint32_t ran;
	gb->audioCount = 0;
	// A run can stop at the end of a frame before any time passed.
	do {
		ran = dots;
		gbRunFor(gb, &ran);
	} while (!ran);
	*samples = gb->audioCount / 2;
	*audio = gb->audio;
	return ran;
}

unsigned char* sgbSaveRam(unsigned* size) {
	*size = gb ? gb->cart.ramSize : 0;
	return *size ? gb->sram : NULL;
}

unsigned char* sgbRtc(size_t* size) {
	*size = hasMbc3Clock() ? RTC_SIZE : hasHuC3() ? HUC3_SIZE : 0;
	return *size ? rtcFile : NULL;
}

void sgbRtcRead(void) {
	size_t size;
	if (!sgbRtc(&size)) {
		return;
	}
	unsigned long long const now = time(NULL);
	const unsigned char* const file = rtcFile;
	unsigned long long saved = get64(file + RTC_SAVED);
	if (hasHuC3()) {
		unsigned char state[HUC3_STATE];
		if (!memcmp(file + RTC_MAGIC, "aGBh", 4)) {
			memcpy(state, file + RTC_REGS, sizeof(state));
		} else {
			// gambatte-libretro's: the minutes and days since the clock's start.
			unsigned long long const base = get64(file + RTC_BASE);
			if (!base || base < SANE_TIME / 2 || now < SANE_TIME || base > now) {
				return;
			}
			gbHuc3Get(gb, state);
			memset(state, 0, 4);
			putNibbles(state + HUC3_MINUTES, 0);
			putNibbles(state + HUC3_DAYS, 0);
			saved = base;
		}
		if (saved >= SANE_TIME / 2 && now >= SANE_TIME && now >= saved) {
			runHuC3(state, now - saved);
		}
		gbHuc3Set(gb, state);
		return;
	}
	uint32_t r[11] = { 0 };
	bool const ours = !memcmp(file + RTC_MAGIC, "aGBm", 4), older = !memcmp(file + RTC_MAGIC, "aGBc", 4);
	if (older) {
		const unsigned char* const regs = file + RTC_REGS;
		for (int i = 0; i < 5; ++i) {
			r[i] = regs[i];
			r[i + RTC_LATCHED] = regs[9 + i];
		}
		r[RTC_C] = get32(regs + 5);
		// armGBA keeps the offset where the newer layout has it.
		unsigned long long const moved = get64(file + OLD_OFFSET);
		memset(rtcFile + RTC_MOVED, 0, RTC_SIZE - RTC_MOVED);
		put64(rtcFile + RTC_MOVED, moved);
	} else if (ours || mgbaFile(file, now)) {
		static const int fields[5] = { RTC_S, RTC_M, RTC_H, RTC_DL, RTC_DH };
		for (int i = 0; i < 5; ++i) {
			r[fields[i]] = get32(file + MGBA_REGS + i * 4);
			r[fields[i] + RTC_LATCHED] = ours ? file[RTC_LATCH + fields[i]] : r[fields[i]];
		}
		saved = get64(file + MGBA_SAVED);
		if (ours) {
			r[RTC_C] = get32(file + RTC_CYCLES);
		} else {
			memset(rtcFile + RTC_MOVED, 0, RTC_SIZE - RTC_MOVED);
		}
	} else {
		// gambatte-libretro's: the clock shows the time since its start, unless that start is in the
		// future.
		unsigned long long const base = get64(file + RTC_BASE);
		if (base && base >= SANE_TIME / 2 && now >= SANE_TIME && base <= now) {
			runClock(r, now - base);
			for (int i = 0; i < 5; ++i) {
				r[i + RTC_LATCHED] = r[i];
			}
		}
		saved = 0;
		memset(rtcFile + RTC_MOVED, 0, RTC_SIZE - RTC_MOVED);
	}
	r[RTC_DH] &= 0xC1;
	r[RTC_DH + RTC_LATCHED] &= 0xC1;
	for (int i = RTC_H; i <= RTC_S; ++i) {
		r[i] &= i == RTC_H ? 0x1F : 0x3F;
		r[i + RTC_LATCHED] &= i == RTC_H ? 0x1F : 0x3F;
	}
	r[RTC_C] %= RTC_SECOND;
	if (saved >= SANE_TIME && now >= saved) {
		runClock(r, now - saved);
	}
	gbRtcSet(gb, r);
}

void sgbRtcWrite(void) {
	size_t size;
	if (!sgbRtc(&size)) {
		return;
	}
	unsigned char* const file = rtcFile;
	unsigned long long const now = time(NULL);
	put64(file + RTC_SAVED, now);
	if (hasHuC3()) {
		memcpy(file + RTC_MAGIC, "aGBh", 4);
		unsigned char* const state = file + RTC_REGS;
		gbHuc3Get(gb, state);
		unsigned long long const cycles = (unsigned long long) state[0] << 24 | state[1] << 16 | state[2] << 8 | state[3];
		unsigned long long const shown = getNibbles(state + HUC3_DAYS) * 86400ull + getNibbles(state + HUC3_MINUTES) * 60 + cycles / RTC_SECOND;
		put64(file + RTC_BASE, now - shown);
		return;
	}
	uint32_t r[11];
	gbRtcGet(gb, r);
	unsigned long long const shown = (((((r[RTC_DH] & 1) << 8 | r[RTC_DL]) * 24 + r[RTC_H]) * 60 + r[RTC_M]) * 60 + r[RTC_S]);
	put64(file + RTC_BASE, now - shown);
	memcpy(file + RTC_MAGIC, "aGBm", 4);
	static const int fields[5] = { RTC_S, RTC_M, RTC_H, RTC_DL, RTC_DH };
	for (int i = 0; i < 5; ++i) {
		put32(file + MGBA_REGS + i * 4, r[fields[i]]);
		file[RTC_LATCH + fields[i]] = r[fields[i] + RTC_LATCHED];
	}
	put64(file + MGBA_SAVED, now);
	put32(file + RTC_CYCLES, r[RTC_C]);
}

size_t sgbStateSize(void) {
	return gb ? gbStateSize() : 0;
}

void sgbStateSave(void* data) {
	gbSaveState(gb, data);
}

bool sgbStateLoad(const void* data, size_t size) {
	return size == gbStateSize() && gbLoadState(gb, data, size);
}
