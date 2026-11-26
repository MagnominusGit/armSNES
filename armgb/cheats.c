/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Licensed under the GNU General Public License, version 2 or (at your option) any later version: see LICENSE. */
#include "gb.h"

#include <ctype.h>
#include <string.h>

// Cheat codes typed in, as the cheat devices took them (Pan Docs, "Game Genie/GameShark"):
// - Game Genie, ABC-DEF or ABC-DEF-GHI: AB the byte read instead, FCDE the address (F xored with F),
//   and with nine letters, GI the byte that must be there (rotated right by 2, xored with BA); H is a
//   check. The ROM's byte is replaced as the CPU reads it, whichever bank is in.
// - GameShark, ttvvllhh: the byte vv written to hhll each frame, as the device wrote it at V-blank: tt
//   01 as the CPU would, 8n to bank n of cartridge RAM, 9n to bank n of the Color's work RAM.

static int hexDigit(char c) {
	return isdigit((unsigned char) c) ? c - '0' : isxdigit((unsigned char) c) ? toupper((unsigned char) c) - 'A' + 10 : -1;
}

static bool hexDigits(const char* text, size_t count, unsigned* value) {
	*value = 0;
	for (size_t i = 0; i < count; ++i) {
		int d = hexDigit(text[i]);
		if (d < 0) {
			return false;
		}
		*value = *value << 4 | d;
	}
	return true;
}

void gbClearCheats(struct GB* gb) {
	gb->genieCount = 0;
	gb->sharkCount = 0;
	gbMapRom(gb);
}

bool gbAddCheat(struct GB* gb, const char* code) {
	size_t length = strlen(code);
	unsigned a, b;
	if ((length == 7 || length == 11) && code[3] == '-' && (length == 7 || code[7] == '-')) {
		char digits[9];
		for (size_t i = 0, n = 0; i < length; ++i) {
			if (code[i] != '-') {
				digits[n++] = code[i];
			}
		}
		if (gb->genieCount >= GB_MAX_CHEATS || !hexDigits(digits, length == 7 ? 6 : 9, &a)) {
			return false;
		}
		struct GBGenieCode* g = &gb->genie[gb->genieCount];
		hexDigits(digits, 2, &b);
		g->value = b;
		unsigned c, d, e, f;
		hexDigits(digits + 2, 1, &c);
		hexDigits(digits + 3, 1, &d);
		hexDigits(digits + 4, 1, &e);
		hexDigits(digits + 5, 1, &f);
		g->address = (f ^ 0xF) << 12 | c << 8 | d << 4 | e;
		g->compares = length == 11;
		if (g->compares) {
			unsigned gi, i;
			hexDigits(digits + 6, 1, &gi);
			hexDigits(digits + 8, 1, &i);
			uint8_t both = gi << 4 | i;
			g->compare = (uint8_t) ((both >> 2 | both << 6) ^ 0xBA);
		}
		if (g->address >= 0x8000) {
			return false;
		}
		++gb->genieCount;
		gbMapRom(gb);
		return true;
	}
	if (length == 8 && hexDigits(code, 8, &a)) {
		if (gb->sharkCount >= GB_MAX_CHEATS) {
			return false;
		}
		struct GBSharkCode* s = &gb->shark[gb->sharkCount++];
		s->type = a >> 24;
		s->value = a >> 16;
		s->address = (a & 0xFF) << 8 | (a >> 8 & 0xFF);
		return true;
	}
	return false;
}

// Whether a 4 KiB page of the ROM area has a Game Genie code, so reads there come to gbCheatRomByte.
bool gbCheatsOnPage(const struct GB* gb, unsigned page) {
	for (unsigned i = 0; i < gb->genieCount; ++i) {
		if (gb->genie[i].address >> 12 == page) {
			return true;
		}
	}
	return false;
}

uint8_t gbCheatRomByte(const struct GB* gb, uint16_t address, uint8_t value) {
	for (unsigned i = 0; i < gb->genieCount; ++i) {
		const struct GBGenieCode* g = &gb->genie[i];
		if (g->address == address && (!g->compares || g->compare == value)) {
			return g->value;
		}
	}
	return value;
}

void gbApplySharkCodes(struct GB* gb) {
	for (unsigned i = 0; i < gb->sharkCount; ++i) {
		const struct GBSharkCode* s = &gb->shark[i];
		uint16_t address = s->address;
		if ((s->type & 0xF0) == 0x80 && address >= 0xA000 && address < 0xC000 && gb->cart.ramSize) {
			gb->sram[((s->type & 0xF) * 0x2000 + (address & 0x1FFF)) & (gb->cart.ramSize - 1)] = s->value;
		} else if ((s->type & 0xF0) == 0x90 && address >= 0xC000 && address < 0xE000) {
			unsigned bank = address < 0xD000 ? 0 : gbColor(gb) ? (s->type & 7 ? s->type & 7 : 1) : 1;
			gb->wram[bank * 0x1000 + (address & 0xFFF)] = s->value;
		} else if (address >= 0x8000) {
			// Not the ROM area: a write there would change the cartridge's banks.
			gbWrite(gb, address, s->value);
		}
	}
}
