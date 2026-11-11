/* Copyright (c) 2004-2025 ares team, Near et al
 * Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Ported from ares (ISC); see LICENSES/ISC-ares.txt. */
#include "snes.h"

#include <stdio.h>
#include <stdlib.h>

// The cartridge: what board a game's header describes, and that board's memory map.

struct Cart cart;

static const u8* rom;
static u32 romSize;
static u32 headerAddress;

// How likely a header is to be at this address: the reset vector has to point at plausible code.
static u32 scoreHeader(u32 address) {
	s32 score = 0;
	if (romSize < address + 0x50) {
		return score;
	}

	u8 mapMode = rom[address + 0x25] & ~0x10;
	u16 complement = rom[address + 0x2c] | rom[address + 0x2d] << 8;
	u16 checksum = rom[address + 0x2e] | rom[address + 0x2f] << 8;
	u16 resetVector = rom[address + 0x4c] | rom[address + 0x4d] << 8;
	if (resetVector < 0x8000) {
		return score;
	}

	u8 opcode = rom[(address & ~0x7fff) | (resetVector & 0x7fff)];

	// sei, clc, sec, stz $nnnn, jmp $nnnn, jml $nnnnnn
	if (opcode == 0x78 || opcode == 0x18 || opcode == 0x38 || opcode == 0x9c || opcode == 0x4c || opcode == 0x5c) {
		score += 8;
	}
	// rep, sep, lda/ldx/ldy $nnnn, lda $nnnnnn, lda/ldx/ldy #, jsr, jsl
	if (opcode == 0xc2 || opcode == 0xe2 || opcode == 0xad || opcode == 0xae || opcode == 0xac || opcode == 0xaf ||
	    opcode == 0xa9 || opcode == 0xa2 || opcode == 0xa0 || opcode == 0x20 || opcode == 0x22) {
		score += 4;
	}
	// rti, rts, rtl, cmp/cpx/cpy $nnnn
	if (opcode == 0x40 || opcode == 0x60 || opcode == 0x6b || opcode == 0xcd || opcode == 0xec || opcode == 0xcc) {
		score -= 4;
	}
	// brk, cop, stp, wdm, sbc $nnnnnn,x
	if (opcode == 0x00 || opcode == 0x02 || opcode == 0xdb || opcode == 0x42 || opcode == 0xff) {
		score -= 8;
	}

	if (checksum + complement == 0xffff) {
		score += 4;
	}
	if (address == 0x7fb0 && mapMode == 0x20) {
		score += 2;
	}
	if (address == 0xffb0 && mapMode == 0x21) {
		score += 2;
	}

	return score > 0 ? score : 0;
}

static bool serialValid(char n) {
	return (n >= '0' && n <= '9') || (n >= 'A' && n <= 'Z');
}

// The four-character game code of an extended header, or an empty string.
static void serial(char out[5]) {
	const char* code = (const char*) &rom[headerAddress + 0x02];
	out[0] = 0;
	if (rom[headerAddress + 0x2a] == 0x33 && serialValid(code[0]) && serialValid(code[1]) && serialValid(code[2]) &&
	    serialValid(code[3])) {
		memcpy(out, code, 4);
		out[4] = 0;
	}
}

static u32 headerRamSize(void) {
	u8 size = rom[headerAddress + 0x28];
	if (size) {
		return (1 << (((size - 1) & 7) + 1)) << 10;
	}
	return 0;
}

static u32 expansionRamSize(void) {
	if (rom[headerAddress + 0x2a] == 0x33) {
		u8 size = rom[headerAddress + 0x0d] & 7;
		if (size) {
			return 1024 << size;
		}
	}
	if ((rom[headerAddress + 0x26] >> 4) == 1) {
		return 0x8000;
	}
	return 0;
}

// Hardware the header gets wrong, by the title it carries: a title a character too long runs over
// the map mode.
static const struct {
	const char* title;
	const char* mode;
} titleModes[] = {
	{ "YUYU NO QUIZ DE GO!GO", "LOROM-" },
};

// The board's name, in ares's form: [slot]-[coprocessor]-[mapper]-[ram]-[rtc].
static void boardName(char* board, size_t size) {
	u8 mapMode = rom[headerAddress + 0x25];
	u8 cartridgeTypeLo = rom[headerAddress + 0x26] & 15;
	u8 cartridgeTypeHi = rom[headerAddress + 0x26] >> 4;
	u8 cartridgeSubType = rom[headerAddress + 0x0f];

	const char* mode = "";
	if (mapMode == 0x20 || mapMode == 0x30) {
		mode = "LOROM-";
	}
	if (mapMode == 0x21 || mapMode == 0x31) {
		mode = "HIROM-";
	}
	if (mapMode == 0x22 || mapMode == 0x32) {
		mode = "SDD1-";
	}
	if (mapMode == 0x23 || mapMode == 0x33) {
		mode = "SA1-";
	}
	if (mapMode == 0x25 || mapMode == 0x35) {
		mode = "EXHIROM-";
	}
	if (mapMode == 0x2a || mapMode == 0x3a) {
		mode = "SPC7110-";
	}
	// Many games have a title a character too long, over the map mode; ExLoROM has no map mode value.
	if (!*mode) {
		if (headerAddress == 0x7fb0) {
			mode = "LOROM-";
		}
		if (headerAddress == 0xffb0) {
			mode = "HIROM-";
		}
		if (headerAddress == 0x407fb0) {
			mode = "EXLOROM-";
		}
		if (headerAddress == 0x40ffb0) {
			mode = "EXHIROM-";
		}
	}

	board[0] = 0;
	if (romSize >= 14 && !memcmp(rom, "BANDAI SFC-ADX", 14)) {
		snprintf(board, size, "ST-%s", mode);
	}

	for (size_t n = 0; n < sizeof(titleModes) / sizeof(*titleModes); ++n) {
		size_t length = strlen(titleModes[n].title);
		if (!memcmp(&rom[headerAddress + 0x10], titleModes[n].title, length) && length == 21) {
			mode = titleModes[n].mode;
		}
	}
	if (!strcmp(mode, "LOROM-") && headerAddress == 0x407fb0) {
		mode = "EXLOROM-";
	}

	bool epsonRTC = false;
	bool sharpRTC = false;
	char code[5];
	serial(code);
	size_t used = strlen(board);
#define APPEND(...) used += snprintf(board + used, size - used, __VA_ARGS__)

	if (!strcmp(code, "ZBSJ")) {
		APPEND("BS-MCC-");
	} else if (!strcmp(code, "042J")) {
		APPEND("GB-%s", mode);
	} else if (code[0] == 'Z' && code[3] == 'J') {
		APPEND("BS-%s", mode);
	} else if (cartridgeTypeLo >= 0x3) {
		if (cartridgeTypeHi == 0x0) {
			APPEND("NEC-%s", mode);
		}
		if (cartridgeTypeHi == 0x1) {
			APPEND("GSU-");
		}
		if (cartridgeTypeHi == 0x2) {
			APPEND("OBC1-%s", mode);
		}
		if (cartridgeTypeHi == 0x3) {
			APPEND("SA1-");
		}
		if (cartridgeTypeHi == 0x4) {
			APPEND("SDD1-");
		}
		if (cartridgeTypeHi == 0x5) {
			APPEND("%s", mode);
			sharpRTC = true;
		}
		if (cartridgeTypeHi == 0xe && cartridgeTypeLo == 0x3) {
			APPEND("GB-%s", mode);
		}
		if (cartridgeTypeHi == 0xf && cartridgeTypeLo == 0x5 && cartridgeSubType == 0x00) {
			APPEND("SPC7110-");
		}
		if (cartridgeTypeHi == 0xf && cartridgeTypeLo == 0x9 && cartridgeSubType == 0x00) {
			APPEND("SPC7110-");
			epsonRTC = true;
		}
		if (cartridgeTypeHi == 0xf && cartridgeSubType == 0x01) {
			APPEND("EXNEC-%s", mode);
		}
		if (cartridgeTypeHi == 0xf && cartridgeSubType == 0x02) {
			APPEND("ARM-%s", mode);
		}
		if (cartridgeTypeHi == 0xf && cartridgeSubType == 0x10) {
			APPEND("HITACHI-%s", mode);
		}
	}
	if (!board[0]) {
		APPEND("%s", mode);
	}

	if (headerRamSize() || expansionRamSize()) {
		APPEND("RAM-");
	}
	if (epsonRTC) {
		APPEND("EPSONRTC-");
	}
	if (sharpRTC) {
		APPEND("SHARPRTC-");
	}
	if (used && board[used - 1] == '-') {
		board[--used] = 0;
	}

	if (!strncmp(board, "LOROM-RAM", 9) && romSize <= 0x200000) {
		APPEND("#A");
	}
	if (!strncmp(board, "NEC-LOROM-RAM", 13) && romSize <= 0x100000) {
		APPEND("#A");
	}
#undef APPEND
}

// The boards without a coprocessor, as in ares's board database.
static const struct {
	const char* name;
	const char* romMap[4];
	u32 romBase[4];
	u32 romMask[4];
	const char* ramMap;
	u32 ramMask;
} boards[] = {
	{ "LOROM", { "00-7d,80-ff:8000-ffff" }, { 0 }, { 0x8000 }, NULL, 0 },
	{ "LOROM-RAM", { "00-7d,80-ff:8000-ffff" }, { 0 }, { 0x8000 }, "70-7d,f0-ff:0000-7fff", 0x8000 },
	{ "LOROM-RAM#A", { "00-6f,80-ef:8000-ffff" }, { 0 }, { 0x8000 }, "70-7d,f0-ff:0000-ffff", 0x8000 },
	{ "HIROM", { "00-3f,80-bf:8000-ffff", "40-7d,c0-ff:0000-ffff" }, { 0 }, { 0 }, NULL, 0 },
	{ "HIROM-RAM", { "00-3f,80-bf:8000-ffff", "40-7d,c0-ff:0000-ffff" }, { 0 }, { 0 }, "20-3f,a0-bf:6000-7fff", 0xe000 },
	{ "EXHIROM",
	  { "00-3f:8000-ffff", "40-7d:0000-ffff", "80-bf:8000-ffff", "c0-ff:0000-ffff" },
	  { 0x400000, 0x400000, 0, 0 },
	  { 0, 0, 0xc00000, 0xc00000 },
	  NULL,
	  0 },
	{ "EXHIROM-RAM",
	  { "00-3f:8000-ffff", "40-7d:0000-ffff", "80-bf:8000-ffff", "c0-ff:0000-ffff" },
	  { 0x400000, 0x400000, 0, 0 },
	  { 0, 0, 0xc00000, 0xc00000 },
	  "20-3f,a0-bf:6000-7fff",
	  0xe000 },
	{ "EXLOROM", { "00-7d:8000-ffff", "80-ff:8000-ffff" }, { 0x400000, 0 }, { 0x808000, 0x808000 }, NULL, 0 },
	{ "EXLOROM-RAM",
	  { "00-7d:8000-ffff", "80-ff:8000-ffff" },
	  { 0x400000, 0 },
	  { 0x808000, 0x808000 },
	  "70-7d,f0-ff:0000-7fff",
	  0x8000 },
};
static int boardIndex = -1;

static u8 readRom(u32 address, u8 data) {
	(void) data;
	return cart.rom[address];
}

static void writeRom(u32 address, u8 data) {
	(void) address;
	(void) data;
}

static u8 readRam(u32 address, u8 data) {
	(void) data;
	return cart.ram[address];
}

static void writeRam(u32 address, u8 data) {
	cart.ram[address] = data;
}

bool cartLoad(const u8* data, u32 size) {
	// A copier's 512-byte header.
	if ((size & 0x7fff) == 512) {
		data += 512;
		size -= 512;
	}
	if (size < 0x8000) {
		return false;
	}
	rom = data;
	romSize = size;

	u32 lo = scoreHeader(0x7fb0);
	u32 hi = scoreHeader(0xffb0);
	u32 exLo = scoreHeader(0x407fb0);
	u32 exHi = scoreHeader(0x40ffb0);
	if (exLo) {
		exLo += 4;
	}
	if (exHi) {
		exHi += 4;
	}
	if (lo >= hi && lo >= exLo && lo >= exHi) {
		headerAddress = 0x7fb0;
	} else if (hi >= exLo && hi >= exHi) {
		headerAddress = 0xffb0;
	} else if (exLo >= exHi) {
		headerAddress = 0x407fb0;
	} else {
		headerAddress = 0x40ffb0;
	}

	free(cart.ram);
	memset(&cart, 0, sizeof(cart));
	cart.rom = rom;
	cart.romSize = romSize;
	boardName(cart.board, sizeof(cart.board));

	u8 region = rom[headerAddress + 0x29];
	cart.pal = !(region == 0x00 || region == 0x01 || region == 0x0b || region == 0x0d || region == 0x0f || region == 0x10);

	boardIndex = -1;
	for (size_t n = 0; n < sizeof(boards) / sizeof(*boards); ++n) {
		if (!strcmp(boards[n].name, cart.board)) {
			boardIndex = n;
		}
	}
	if (boardIndex < 0) {
		return false;
	}

	// Save RAM starts out as 0xff.
	cart.ramSize = headerRamSize() ? headerRamSize() : expansionRamSize();
	if (cart.ramSize) {
		cart.ram = malloc(cart.ramSize);
		memset(cart.ram, 0xff, cart.ramSize);
	}
	return true;
}

void cartMap(void) {
	if (boardIndex < 0) {
		return;
	}
	for (int n = 0; n < 4 && boards[boardIndex].romMap[n]; ++n) {
		busMap(readRom, writeRom, boards[boardIndex].romMap[n], cart.romSize, boards[boardIndex].romBase[n],
		       boards[boardIndex].romMask[n]);
	}
	if (boards[boardIndex].ramMap && cart.ramSize) {
		busMap(readRam, writeRam, boards[boardIndex].ramMap, cart.ramSize, 0, boards[boardIndex].ramMask);
	}
}

void cartPower(bool reset) {
	(void) reset;
}
