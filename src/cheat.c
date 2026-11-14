/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "snes.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

// Cheat codes, as the cartridges that held them worked: a Game Genie code gives a value for a read
// of its address (a cartridge address, in either half of the banks), and a Pro Action Replay code
// writes its value to the work RAM as each frame begins (anywhere else, it gives the value for reads
// as a Game Genie code does). "address:value" and "address=value" are taken as Pro Action Replay
// codes, and several codes can be joined by '+'.

enum { CHEATS_MAX = 256 };

struct Cheat {
	u32 address;
	u8 value;
	bool everyFrame;
};

static struct Cheat cheats[CHEATS_MAX];
static int cheatCount;

void cheatReset(void) {
	cheatCount = 0;
	busCheatsOff();
}

static int hexDigit(char c) {
	const char* digits = "0123456789ABCDEF";
	const char* at = strchr(digits, toupper((unsigned char) c));
	return c && at ? (int) (at - digits) : -1;
}

// The Game Genie's letters stand for the digits in this order, and its address bits are shuffled.
static bool gameGenie(const char* code, u32* address, u8* value) {
	static const char letters[] = "DF4709156BC8A23E";
	u32 data = 0;
	for (int i = 0; i < 8; ++i) {
		const char* at = strchr(letters, toupper((unsigned char) code[i]));
		if (!code[i] || !at) {
			return false;
		}
		data = data << 4 | (u32) (at - letters);
	}
	u32 a = data & 0xffffff;
	*value = data >> 24;
	*address = (a & 0x003c00) << 10 | (a & 0x00003c) << 14 | (a & 0xf00000) >> 8 | (a & 0x000003) << 10 | (a & 0x00c000) >> 6 |
	           (a & 0x0f0000) >> 12 | (a & 0x0003c0) >> 6;
	return true;
}

static bool hexNumber(const char* text, int digits, u32* number) {
	*number = 0;
	for (int i = 0; i < digits; ++i) {
		int d = hexDigit(text[i]);
		if (d < 0) {
			return false;
		}
		*number = *number << 4 | (u32) d;
	}
	return true;
}

static bool workRam(u32 address) {
	return (address & 0xfe0000) == 0x7e0000 || !(address & 0x40e000);
}

static void add(u32 address, u8 value, bool actionReplay) {
	if (cheatCount == CHEATS_MAX) {
		return;
	}
	address &= 0xffffff;
	bool everyFrame = actionReplay && workRam(address);
	cheats[cheatCount++] = (struct Cheat) { address, value, everyFrame };
	if (!everyFrame) {
		// A cartridge address reads the same in either half of the banks.
		busCheat(address >> 8);
		busCheat((address ^ 0x800000) >> 8);
	}
}

void cheatHook(void) {
	for (int i = 0; i < cheatCount; ++i) {
		if (!cheats[i].everyFrame) {
			busCheat(cheats[i].address >> 8);
			busCheat((cheats[i].address ^ 0x800000) >> 8);
		}
	}
}

// One code, spaces taken out; false if it isn't one. A Game Genie code has a dash after its fourth
// letter, a Pro Action Replay code is eight digits.
static bool addCode(const char* code) {
	u32 address, value;
	u8 byte;
	size_t length = strlen(code);
	if (length == 9 && code[4] == '-') {
		char letters[9];
		memcpy(letters, code, 4);
		memcpy(letters + 4, code + 5, 5);
		if (gameGenie(letters, &address, &byte)) {
			add(address, byte, false);
			return true;
		}
		return false;
	}
	if (length == 8 && hexNumber(code, 8, &value)) {
		add(value >> 8, value, true);
		return true;
	}
	const char* split = strpbrk(code, ":=");
	if (split && split - code >= 1 && split - code <= 6 && strlen(split + 1) >= 1 && strlen(split + 1) <= 2 &&
	    hexNumber(code, (int) (split - code), &address) && hexNumber(split + 1, (int) strlen(split + 1), &value)) {
		add(address, value, true);
		return true;
	}
	return false;
}

bool cheatAdd(const char* text) {
	bool any = false;
	char code[32];
	size_t n = 0;
	for (const char* p = text;; ++p) {
		if (*p == '+' || *p == 0) {
			code[n] = 0;
			any |= n && addCode(code);
			n = 0;
			if (!*p) {
				break;
			}
		} else if (*p != ' ' && *p != '\t' && n < sizeof(code) - 1) {
			code[n++] = *p;
		}
	}
	return any;
}

u8 cheatRead(u32 address, u8 data) {
	for (int i = 0; i < cheatCount; ++i) {
		const struct Cheat* c = &cheats[i];
		if (!c->everyFrame && (c->address == address || (c->address ^ 0x800000) == address)) {
			return c->value;
		}
	}
	return data;
}

void cheatFrame(void) {
	for (int i = 0; i < cheatCount; ++i) {
		const struct Cheat* c = &cheats[i];
		if (c->everyFrame) {
			u32 a = c->address;
			cpu.wram[(a & 0xfe0000) == 0x7e0000 ? a & 0x1ffff : a & 0x1fff] = c->value;
		}
	}
}
