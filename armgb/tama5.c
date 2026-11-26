/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Licensed under the GNU General Public License, version 2 or (at your option) any later version: see LICENSE. */
#include "gb.h"

#include <string.h>

// Bandai's TAMA5 with its TAMA6 clock, as mGBA documents them. All of it is kept in cartridge RAM past
// the chip's own 32 bytes, so save states and the save file carry it. The clock runs on the console's
// time, 2^22 dots to the second.
enum {
	RAM_SIZE = 0x20,
	REGS = 0x20,      // the eight nibble registers
	REG = 0x28,       // the one selected
	STOPPED = 0x29,
	MARKER = 0x2F,
	TIMER_PAGE = 0x30,
	ALARM_PAGE = 0x40,
	FREE_PAGE0 = 0x50,
	FREE_PAGE1 = 0x60,
	LAST = 0x70,      // the time the clock is counted up to, four bytes
	MARKED = 0x5A,
};

enum { BANK_LO, BANK_HI, WRITE_LO = 4, WRITE_HI, ADDR_HI, ADDR_LO, READ_LO = 0xC, READ_HI };
enum { SECOND_1, SECOND_10, MINUTE_1, MINUTE_10, HOUR_1, HOUR_10, WEEK, DAY_1, DAY_10, MONTH_1, MONTH_10, YEAR_1, YEAR_10, PAGE };
enum { IS_24_HOUR = 0xA, LEAP_YEAR = 0xB };

static const uint8_t rtcMask[32] = {
	0xF, 0x7, 0xF, 0x7, 0xF, 0x3, 0x7, 0xF, 0x3, 0xF, 0x1, 0xF, 0xF, 0x0, 0x0, 0x0,
	0x0, 0x0, 0xF, 0x7, 0xF, 0x3, 0x7, 0xF, 0x3, 0x0, 0x1, 0x3, 0x0, 0x0, 0x0, 0x0,
};

// The chip's memory, set up the first time (new RAM, or a save from elsewhere with only its 32 bytes).
static uint8_t* chip(struct GB* gb) {
	uint8_t* s = gb->sram;
	if (s[MARKER] != MARKED) {
		memset(s + RAM_SIZE, 0, 0x80 - RAM_SIZE);
		s[ALARM_PAGE + PAGE] = 1;
		s[FREE_PAGE0 + PAGE] = 2;
		s[FREE_PAGE1 + PAGE] = 3;
		s[MARKER] = MARKED;
		for (int i = 0; i < 4; ++i) {
			s[LAST + i] = gb->now >> i * 8;
		}
	}
	return s;
}

// Adds one to a two-digit count kept as two nibbles; true when it wraps to first.
static bool step(uint8_t* ones, unsigned first, unsigned count) {
	unsigned value = ones[0] + ones[1] * 10 + 1;
	bool wrapped = value >= first + count;
	ones[0] = (wrapped ? first : value) % 10;
	ones[1] = (wrapped ? first : value) / 10;
	return wrapped;
}

// Brings the clock up to now.
static void tick(struct GB* gb) {
	uint8_t* s = chip(gb);
	uint32_t last = s[LAST] | s[LAST + 1] << 8 | s[LAST + 2] << 16 | (uint32_t) s[LAST + 3] << 24;
	if (gbSince(gb->now, last) < 0) {
		last = gb->now;
	}
	uint8_t* t = s + TIMER_PAGE;
	for (; (uint32_t) (gb->now - last) >= 1u << 22; last += 1u << 22) {
		if (s[STOPPED] || !step(t + SECOND_1, 0, 60) || !step(t + MINUTE_1, 0, 60)) {
			continue;
		}
		unsigned hour = t[HOUR_1];
		if (s[ALARM_PAGE + IS_24_HOUR]) {
			hour += t[HOUR_10] * 10;
		} else {
			hour += (t[HOUR_10] & 1) * 10 + (t[HOUR_10] & 2) * 6;
		}
		hour = (hour + 1) % 24;
		if (s[ALARM_PAGE + IS_24_HOUR]) {
			t[HOUR_1] = hour % 10;
			t[HOUR_10] = hour / 10;
		} else {
			t[HOUR_1] = hour % 12 % 10;
			t[HOUR_10] = hour % 12 / 10 + hour / 12 * 2;
		}
		if (hour) {
			continue;
		}
		t[WEEK] = (t[WEEK] + 1) % 7;
		static const uint8_t days[13] = { 31, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
		unsigned month = t[MONTH_1] + t[MONTH_10] * 10;
		unsigned length = month == 2 && !(s[ALARM_PAGE + LEAP_YEAR] & 3) ? 29 : days[month <= 12 ? month : 0];
		if (step(t + DAY_1, 1, length) && step(t + MONTH_1, 1, 12)) {
			step(t + YEAR_1, 0, 100);
			s[ALARM_PAGE + LEAP_YEAR] = (s[ALARM_PAGE + LEAP_YEAR] + 1) & 3;
		}
	}
	for (int i = 0; i < 4; ++i) {
		s[LAST + i] = last >> i * 8;
	}
}

uint16_t gbTama5Bank(struct GB* gb) {
	const uint8_t* r = chip(gb) + REGS;
	return r[BANK_LO] | r[BANK_HI] << 4;
}

void gbTama5Write(struct GB* gb, uint16_t address, uint8_t data) {
	uint8_t* s = chip(gb);
	if (address & 1) {
		s[REG] = data;
		return;
	}
	if (s[REG] >= 8) {
		return;
	}
	tick(gb);
	uint8_t* r = s + REGS;
	r[s[REG]] = data & 0xF;
	unsigned at = (r[ADDR_HI] << 4 & 0x10) | r[ADDR_LO];
	unsigned out = r[WRITE_HI] << 4 | r[WRITE_LO];
	if (s[REG] == BANK_LO || s[REG] == BANK_HI) {
		gb->cart.romBank = gbTama5Bank(gb);
		gbMapRom(gb);
	} else if (s[REG] == ADDR_LO) {
		switch (r[ADDR_HI] >> 1) {
		case 0:
			s[at] = out;
			break;
		case 2:
			switch (at) {
			case 0x00:
			case 0x01:
				s[STOPPED] = !at;
				if (at) {
					s[TIMER_PAGE + SECOND_1] = s[TIMER_PAGE + SECOND_10] = 0;
				}
				for (unsigned i = TIMER_PAGE; i <= FREE_PAGE1; i += 0x10) {
					s[i + PAGE] = (s[i + PAGE] & 0x7) | at << 3;
				}
				break;
			case 0x04:
				s[TIMER_PAGE + MINUTE_1] = out & 0xF;
				s[TIMER_PAGE + MINUTE_10] = out >> 4;
				break;
			case 0x05:
				s[TIMER_PAGE + HOUR_1] = out & 0xF;
				s[TIMER_PAGE + HOUR_10] = out >> 4;
				break;
			case 0x10:
			case 0x11:
				for (unsigned i = TIMER_PAGE; i <= FREE_PAGE1; i += 0x10) {
					s[i + PAGE] = (s[i + PAGE] & 0xB) | (at & 1) << 2;
				}
				break;
			}
			break;
		case 4:
			if (r[WRITE_LO] >= PAGE) {
				break;
			}
			switch (r[ADDR_LO]) {
			case 0:
				s[TIMER_PAGE + r[WRITE_LO]] = r[WRITE_HI] & rtcMask[r[WRITE_LO]];
				break;
			case 2:
				s[ALARM_PAGE + r[WRITE_LO]] = r[WRITE_HI] & rtcMask[r[WRITE_LO] | 0x10];
				break;
			case 4:
				s[FREE_PAGE0 + r[WRITE_LO]] = r[WRITE_HI];
				break;
			case 6:
				s[FREE_PAGE1 + r[WRITE_LO]] = r[WRITE_HI];
				break;
			}
			break;
		}
	}
}

uint8_t gbTama5Read(struct GB* gb, uint16_t address) {
	if (address & 1) {
		return 0xFF;
	}
	uint8_t* s = chip(gb);
	const uint8_t* r = s + REGS;
	if (s[REG] != READ_LO && s[REG] != READ_HI) {
		return 0xF1;
	}
	unsigned at = (r[ADDR_HI] << 4 & 0x10) | r[ADDR_LO];
	unsigned value = 0xF0;
	switch (r[ADDR_HI] >> 1) {
	case 1:
		value = s[at];
		break;
	case 2:
		tick(gb);
		value = at == 0x06 ? (unsigned) (s[TIMER_PAGE + MINUTE_10] << 4 | s[TIMER_PAGE + MINUTE_1])
		      : at == 0x07 ? (unsigned) (s[TIMER_PAGE + HOUR_10] << 4 | s[TIMER_PAGE + HOUR_1]) : at;
		break;
	case 4:
		if (s[REG] == READ_HI) {
			break;
		}
		tick(gb);
		if (r[WRITE_LO] > PAGE) {
			value = 0;
			break;
		}
		switch (r[ADDR_LO]) {
		case 1:
			value = s[TIMER_PAGE + r[WRITE_LO]];
			break;
		case 3:
			value = s[ALARM_PAGE + r[WRITE_LO]];
			break;
		case 5:
			value = s[FREE_PAGE0 + r[WRITE_LO]];
			break;
		case 7:
			value = s[FREE_PAGE1 + r[WRITE_LO]];
			break;
		}
		break;
	}
	if (s[REG] == READ_HI) {
		value >>= 4;
	}
	return value | 0xF0;
}
