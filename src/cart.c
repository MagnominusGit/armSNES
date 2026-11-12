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
	// A 7 MiB SPC7110 dump has an expansion ROM after its data ROM.
	if (!strncmp(board, "SPC7110-", 8) && romSize == 0x700000 && used + 3 <= size) {
		memmove(board + 2, board, used + 1);
		memcpy(board, "EX", 2);
	}
#undef APPEND
}

#include "boards.inc"

static const struct BoardEntry* board;
// What came off the end of the game: firmware a dump carries after it.
static u32 firmwareSize;

// The parts of boards this core has.
static bool partSupported(u16 part) {
	switch (part) {
	case PART_MEMORY_ROM_PROGRAM:
	case PART_MEMORY_RAM_SAVE:
	case PART_PROCESSOR_UPD7725:
	case PART_PROCESSOR_UPD7725_MEMORY_ROM_PROGRAM_UPD7725:
	case PART_PROCESSOR_UPD7725_MEMORY_ROM_DATA_UPD7725:
	case PART_PROCESSOR_UPD7725_MEMORY_RAM_DATA_UPD7725:
	case PART_PROCESSOR_UPD7725_OSCILLATOR:
	case PART_PROCESSOR_UPD96050:
	case PART_PROCESSOR_UPD96050_MEMORY_ROM_PROGRAM_UPD96050:
	case PART_PROCESSOR_UPD96050_MEMORY_ROM_DATA_UPD96050:
	case PART_PROCESSOR_UPD96050_MEMORY_RAM_DATA_UPD96050:
	case PART_PROCESSOR_UPD96050_OSCILLATOR:
	case PART_PROCESSOR_W65C816S:
	case PART_PROCESSOR_W65C816S_MCU:
	case PART_PROCESSOR_W65C816S_MCU_MEMORY_ROM_PROGRAM:
	case PART_PROCESSOR_W65C816S_MEMORY_RAM_SAVE:
	case PART_PROCESSOR_W65C816S_MEMORY_RAM_INTERNAL:
	// A memory-pack slot left empty.
	case PART_PROCESSOR_W65C816S_MCU_SLOT_BSMEMORY:
	case PART_PROCESSOR_GSU:
	case PART_PROCESSOR_GSU_MEMORY_ROM_PROGRAM:
	case PART_PROCESSOR_GSU_MEMORY_RAM_SAVE:
	case PART_PROCESSOR_SDD1:
	case PART_PROCESSOR_SDD1_MCU:
	case PART_PROCESSOR_SDD1_MCU_MEMORY_ROM_PROGRAM:
	case PART_PROCESSOR_OBC1:
	case PART_PROCESSOR_OBC1_MEMORY_RAM_SAVE:
	case PART_PROCESSOR_HG51BS169:
	case PART_PROCESSOR_HG51BS169_MEMORY_ROM_PROGRAM:
	case PART_PROCESSOR_HG51BS169_MEMORY_RAM_SAVE:
	case PART_PROCESSOR_HG51BS169_MEMORY_ROM_DATA_HG51BS169:
	case PART_PROCESSOR_HG51BS169_MEMORY_RAM_DATA_HG51BS169:
	case PART_PROCESSOR_HG51BS169_OSCILLATOR:
	case PART_MEMORY_ROM_EXPANSION:
	case PART_PROCESSOR_SPC7110:
	case PART_PROCESSOR_SPC7110_MCU:
	case PART_PROCESSOR_SPC7110_MCU_MEMORY_ROM_PROGRAM:
	case PART_PROCESSOR_SPC7110_MCU_MEMORY_ROM_DATA:
	case PART_PROCESSOR_SPC7110_MEMORY_RAM_SAVE:
	case PART_RTC_EPSON:
	case PART_RTC_EPSON_MEMORY_RTC_TIME:
	case PART_RTC_SHARP:
	case PART_RTC_SHARP_MEMORY_RTC_TIME:
	case PART_PROCESSOR_ARM6:
	case PART_PROCESSOR_ARM6_MEMORY_ROM_PROGRAM_ARM6:
	case PART_PROCESSOR_ARM6_MEMORY_ROM_DATA_ARM6:
	case PART_PROCESSOR_ARM6_MEMORY_RAM_DATA_ARM6:
	case PART_PROCESSOR_ARM6_OSCILLATOR:
	case PART_PROCESSOR_MCC:
	case PART_PROCESSOR_MCC_MCU:
	case PART_PROCESSOR_MCC_MCU_MEMORY_ROM_PROGRAM:
	case PART_PROCESSOR_MCC_MCU_MEMORY_RAM_DOWNLOAD:
	case PART_PROCESSOR_MCC_MCU_SLOT_BSMEMORY:
	case PART_SLOT_BSMEMORY:
		return true;
	}
	return false;
}

// Which of the NEC DSPs' programs a game's cartridge has, which its header doesn't say: by the title
// in its header. The rest have the DSP-1B.
static const struct {
	const char* title;
	const char* firmware;
} necdspFirmwareTable[] = {
	{ "PILOTWINGS", "dsp1" },
	{ "DUNGEON MASTER", "dsp2" },
	// SD Gundam GX, in half-width katakana.
	{ "SD\xb6\xde\xdd\xc0\xde\xd1GX", "dsp3" },
	{ "PLANETS CHAMP TG3000", "dsp4" },
	{ "TOP GEAR 3000", "dsp4" },
};

// Whether the header's title is this, padded with spaces or empty bytes.
static bool titleIs(const char* title) {
	size_t length = strlen(title);
	if (length > 21 || memcmp(&rom[headerAddress + 0x10], title, length)) {
		return false;
	}
	for (size_t n = length; n < 21; ++n) {
		u8 c = rom[headerAddress + 0x10 + n];
		if (c != 0x20 && c != 0x00 && c != 0xff) {
			return false;
		}
	}
	return true;
}

static const char* necdspFirmwareName(void) {
	for (size_t n = 0; n < sizeof(necdspFirmwareTable) / sizeof(*necdspFirmwareTable); ++n) {
		if (titleIs(necdspFirmwareTable[n].title)) {
			return necdspFirmwareTable[n].firmware;
		}
	}
	return "dsp1b";
}

// The same for the uPD96050: the ST011 by title, the rest the ST010.
static const char* stFirmwareName(void) {
	if (titleIs("2DAN MORITA SHOUGI")) {
		return "st011";
	}
	return "st010";
}

// The NEC DSP's firmware: 24-bit program words then 16-bit data words (2048 and 1024 of them for the
// uPD7725, 16384 and 2048 for the uPD96050), from the end of the dump if it has them there, or from
// the frontend's "<name>.rom", or "<name>.program.rom" with "<name>.data.rom".
static bool loadNecdspFirmware(const char* name, u32 programWords, u32 dataWords) {
	static u8 buffer[0xd000];
	u32 programSize = programWords * 3;
	u32 total = programSize + dataWords * 2;
	bool found = false;
	if (firmwareSize == total) {
		memcpy(buffer, rom + romSize, total);
		found = true;
	} else {
		char file[64];
		snprintf(file, sizeof(file), "%s.rom", name);
		if (cartFirmware(file, buffer, total)) {
			found = true;
		} else {
			snprintf(file, sizeof(file), "%s.program.rom", name);
			bool program = cartFirmware(file, buffer, programSize);
			snprintf(file, sizeof(file), "%s.data.rom", name);
			found = program && cartFirmware(file, buffer + programSize, dataWords * 2);
		}
	}
	if (!found) {
		snprintf(cart.missing, sizeof(cart.missing), "%s.rom", name);
		return false;
	}

	memset(&necdspFirmware, 0, sizeof(necdspFirmware));
	for (u32 n = 0; n < programWords; ++n) {
		necdspFirmware.programROM[n] = buffer[n * 3] | buffer[n * 3 + 1] << 8 | buffer[n * 3 + 2] << 16;
	}
	for (u32 n = 0; n < dataWords; ++n) {
		necdspFirmware.dataROM[n] = buffer[programSize + n * 2] | buffer[programSize + n * 2 + 1] << 8;
	}
	return true;
}

// The Cx4's data ROM, 3 KiB, from the end of the dump or the frontend's "cx4.rom" or "cx4.data.rom".
static bool loadCx4Firmware(void) {
	static u8 buffer[0xc00];
	if (firmwareSize == sizeof(buffer)) {
		memcpy(buffer, rom + romSize, sizeof(buffer));
	} else if (!cartFirmware("cx4.rom", buffer, sizeof(buffer)) && !cartFirmware("cx4.data.rom", buffer, sizeof(buffer))) {
		snprintf(cart.missing, sizeof(cart.missing), "cx4.rom");
		return false;
	}
	cx4Load(buffer);
	return true;
}

// The ST018's firmware, 160 KiB of program then data ROM, from the end of the dump or the
// frontend's "st018.rom", or "st018.program.rom" with "st018.data.rom".
static bool loadSt018Firmware(void) {
	static u8 buffer[0x28000];
	if (firmwareSize == sizeof(buffer)) {
		memcpy(buffer, rom + romSize, sizeof(buffer));
	} else if (!cartFirmware("st018.rom", buffer, sizeof(buffer)) &&
	           !(cartFirmware("st018.program.rom", buffer, 0x20000) && cartFirmware("st018.data.rom", buffer + 0x20000, 0x8000))) {
		snprintf(cart.missing, sizeof(cart.missing), "st018.rom");
		return false;
	}
	st018Load(buffer);
	return true;
}

// The last megabyte of a 7 MiB SPC7110 dump.
static u8 readExpansion(u32 address, u8 data) {
	(void) data;
	return cart.rom[0x600000 + address];
}

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
	boardName(cart.board, sizeof(cart.board));

	// Firmware some dumps carry after the game.
	u8 typeLo = rom[headerAddress + 0x26] & 15;
	u8 typeHi = rom[headerAddress + 0x26] >> 4;
	firmwareSize = 0;
	u8 subType = rom[headerAddress + 0x0f];
	if (typeLo >= 0x3 && typeHi == 0x0 && (size & 0x7fff) == 0x2000) {
		firmwareSize = 0x2000;
	}
	if (typeLo >= 0x3 && typeHi == 0xf && subType == 0x01 && (size & 0xffff) == 0xd000) {
		firmwareSize = 0xd000;
	}
	if (typeLo >= 0x3 && typeHi == 0xf && subType == 0x10 && (size & 0x7fff) == 0xc00) {
		firmwareSize = 0xc00;
	}
	if (typeLo >= 0x3 && typeHi == 0xf && subType == 0x02 && (size & 0x3ffff) == 0x28000) {
		firmwareSize = 0x28000;
	}
	romSize = size - firmwareSize;
	cart.rom = rom;
	cart.romSize = romSize;

	u8 region = rom[headerAddress + 0x29];
	cart.pal = !(region == 0x00 || region == 0x01 || region == 0x0b || region == 0x0d || region == 0x0f || region == 0x10);

	board = NULL;
	for (size_t n = 0; n < sizeof(boardEntries) / sizeof(*boardEntries); ++n) {
		if (!strcmp(boardEntries[n].name, cart.board)) {
			board = &boardEntries[n];
		}
	}
	if (!board) {
		return false;
	}
	for (u8 n = 0; n < board->partCount; ++n) {
		if (!partSupported(board->parts[n])) {
			snprintf(cart.missing, sizeof(cart.missing), "%s", partNames[board->parts[n]]);
			return false;
		}
		if (board->parts[n] == PART_PROCESSOR_W65C816S) {
			cart.sa1 = true;
		}
		if (board->parts[n] == PART_PROCESSOR_GSU) {
			cart.gsu = true;
		}
		if (board->parts[n] == PART_PROCESSOR_SDD1) {
			cart.sdd1 = true;
		}
		if (board->parts[n] == PART_PROCESSOR_OBC1) {
			cart.obc1 = true;
		}
		if (board->parts[n] == PART_PROCESSOR_HG51BS169) {
			cart.cx4 = true;
		}
		if (board->parts[n] == PART_PROCESSOR_SPC7110) {
			cart.spc7110 = true;
		}
		if (board->parts[n] == PART_RTC_EPSON) {
			cart.epsonRtc = true;
		}
		if (board->parts[n] == PART_RTC_SHARP) {
			cart.sharpRtc = true;
		}
		if (board->parts[n] == PART_PROCESSOR_ARM6) {
			cart.st018 = true;
		}
		if (board->parts[n] == PART_PROCESSOR_MCC) {
			cart.mcc = true;
		}
		if (board->parts[n] == PART_SLOT_BSMEMORY || board->parts[n] == PART_PROCESSOR_MCC_MCU_SLOT_BSMEMORY ||
		    board->parts[n] == PART_PROCESSOR_W65C816S_MCU_SLOT_BSMEMORY) {
			cart.bsSlot = true;
		}
		if (board->parts[n] == PART_PROCESSOR_UPD7725 || board->parts[n] == PART_PROCESSOR_UPD96050) {
			cart.necdsp = true;
			necdsp.uPD96050 = board->parts[n] == PART_PROCESSOR_UPD96050;
		}
	}

	if (cart.necdsp) {
		bool uPD96050 = necdsp.uPD96050;
		memset(&necdsp, 0, sizeof(necdsp));
		necdsp.uPD96050 = uPD96050;
		if (!uPD96050) {
			if (!loadNecdspFirmware(necdspFirmwareName(), 2048, 1024)) {
				return false;
			}
			necdsp.frequency = 7600000;
			// Its 256 words of RAM start out as 0xff, as ares's loader leaves them.
			memset(necdsp.dataRAM, 0xff, 256 * sizeof(u16));
		} else {
			const char* name = stFirmwareName();
			if (!loadNecdspFirmware(name, 16384, 2048)) {
				return false;
			}
			necdsp.frequency = strcmp(name, "st011") ? 11000000 : 15000000;
		}
	}

	if (cart.cx4 && !loadCx4Firmware()) {
		return false;
	}
	if (cart.spc7110) {
		spc7110Load();
	}
	if (cart.st018 && !loadSt018Firmware()) {
		return false;
	}
	if (cart.mcc) {
		mccLoad();
	}
	bsLoad(NULL, 0);
	if (cart.epsonRtc) {
		epsonRtcLoad();
	}
	if (cart.sharpRtc) {
		sharpRtcLoad();
	}

	// Save RAM starts out as 0xff. The uPD96050 keeps its saves in its own data RAM.
	cart.ramSize = headerRamSize() ? headerRamSize() : expansionRamSize();
	if (cart.ramSize) {
		cart.ram = malloc(cart.ramSize);
		memset(cart.ram, 0xff, cart.ramSize);
		// A new SPC7110 cartridge left the factory with its check passed, noted at 00:7ff0; without it
		// the game runs the check program instead. A save the frontend loads replaces it.
		if (cart.spc7110 && cart.ramSize >= 16) {
			memcpy(cart.ram + busMirror(0x1ff0, cart.ramSize), "SPC7110 CHECK OK", 16);
		}
	}
	if (cart.necdsp && necdsp.uPD96050) {
		// A save not there yet is 0xff, as other save RAM.
		memset(necdsp.dataRAM, 0xff, sizeof(necdsp.dataRAM));
		cart.saveRam = (u8*) necdsp.dataRAM;
		cart.saveRamSize = sizeof(necdsp.dataRAM);
	} else {
		cart.saveRam = cart.ram;
		cart.saveRamSize = cart.ramSize;
	}
	return true;
}

void cartMap(void) {
	if (!board) {
		return;
	}
	for (u8 n = 0; n < board->mapCount; ++n) {
		const struct BoardMap* map = &board->maps[n];
		switch (map->part) {
		case PART_MEMORY_ROM_PROGRAM:
			busMap(readRom, writeRom, map->address, map->size ? map->size : cart.romSize, map->base, map->mask);
			break;
		case PART_MEMORY_RAM_SAVE:
			if (cart.ramSize) {
				busMap(readRam, writeRam, map->address, map->size ? map->size : cart.ramSize, map->base, map->mask);
			}
			break;
		case PART_PROCESSOR_UPD7725:
		case PART_PROCESSOR_UPD96050:
			busMap(necdspRead, necdspWrite, map->address, map->size, map->base, map->mask);
			break;
		case PART_PROCESSOR_UPD96050_MEMORY_RAM_DATA_UPD96050:
			busMap(necdspReadRAM, necdspWriteRAM, map->address, map->size, map->base, map->mask);
			break;
		case PART_PROCESSOR_W65C816S:
			busMap(sa1ReadIOCPU, sa1WriteIOCPU, map->address, map->size, map->base, map->mask);
			break;
		case PART_PROCESSOR_W65C816S_MCU:
			busMap(sa1ReadRomCPU, sa1WriteRomCPU, map->address, map->size, map->base, map->mask);
			break;
		case PART_PROCESSOR_W65C816S_MEMORY_RAM_SAVE:
			busMap(sa1ReadBwramCPU, sa1WriteBwramCPU, map->address, map->size, map->base, map->mask);
			break;
		case PART_PROCESSOR_W65C816S_MEMORY_RAM_INTERNAL:
			busMap(sa1ReadIramCPU, sa1WriteIramCPU, map->address, map->size, map->base, map->mask);
			break;
		case PART_PROCESSOR_GSU:
			busMap(gsuReadIO, gsuWriteIO, map->address, map->size, map->base, map->mask);
			break;
		case PART_PROCESSOR_GSU_MEMORY_ROM_PROGRAM:
			busMap(gsuReadRomCPU, gsuWriteRomCPU, map->address, map->size ? map->size : cart.romSize, map->base, map->mask);
			break;
		case PART_PROCESSOR_GSU_MEMORY_RAM_SAVE:
			if (cart.ramSize) {
				busMap(gsuReadRamCPU, gsuWriteRamCPU, map->address, map->size ? map->size : cart.ramSize, map->base, map->mask);
			}
			break;
		case PART_PROCESSOR_SDD1:
			busMap(sdd1ReadIO, sdd1WriteIO, map->address, map->size, map->base, map->mask);
			break;
		case PART_PROCESSOR_SDD1_MCU:
			busMap(sdd1ReadMCU, sdd1WriteMCU, map->address, map->size, map->base, map->mask);
			break;
		case PART_PROCESSOR_OBC1:
			busMap(obc1Read, obc1Write, map->address, map->size, map->base, map->mask);
			break;
		case PART_MEMORY_ROM_EXPANSION:
			busMap(readExpansion, writeRom, map->address, 0x100000, map->base, map->mask);
			break;
		case PART_PROCESSOR_SPC7110:
			busMap(spc7110Read, spc7110Write, map->address, map->size, map->base, map->mask);
			break;
		case PART_PROCESSOR_SPC7110_MCU:
			busMap(spc7110ReadMCU, spc7110WriteMCU, map->address, map->size, map->base, map->mask);
			break;
		case PART_PROCESSOR_SPC7110_MEMORY_RAM_SAVE:
			busMap(spc7110ReadRam, spc7110WriteRam, map->address, map->size, map->base, map->mask);
			break;
		case PART_RTC_EPSON:
			busMap(epsonRtcRead, epsonRtcWrite, map->address, map->size, map->base, map->mask);
			break;
		case PART_PROCESSOR_MCC:
			busMap(mccRead, mccWrite, map->address, map->size, map->base, map->mask);
			break;
		case PART_PROCESSOR_MCC_MCU:
			busMap(mccReadMCU, mccWriteMCU, map->address, map->size, map->base, map->mask);
			break;
		case PART_SLOT_BSMEMORY:
			busMap(bsRead, bsWrite, map->address, map->size, map->base, map->mask);
			break;
		case PART_PROCESSOR_ARM6:
			busMap(st018Read, st018Write, map->address, map->size, map->base, map->mask);
			break;
		case PART_RTC_SHARP:
			busMap(sharpRtcRead, sharpRtcWrite, map->address, map->size, map->base, map->mask);
			break;
		case PART_PROCESSOR_HG51BS169:
			busMap(cx4ReadIO, cx4WriteIO, map->address, map->size, map->base, map->mask);
			break;
		case PART_PROCESSOR_HG51BS169_MEMORY_ROM_PROGRAM:
			busMap(cx4ReadRomCPU, cx4WriteRomCPU, map->address, map->size, map->base, map->mask);
			break;
		case PART_PROCESSOR_HG51BS169_MEMORY_RAM_SAVE:
			busMap(cx4ReadRamCPU, cx4WriteRamCPU, map->address, map->size, map->base, map->mask);
			break;
		case PART_PROCESSOR_HG51BS169_MEMORY_RAM_DATA_HG51BS169:
			busMap(cx4ReadDataRamCPU, cx4WriteDataRamCPU, map->address, map->size, map->base, map->mask);
			break;
		}
	}
	if (cart.sdd1) {
		sdd1Map();
	}
}

bool cartInsertPack(const u8* data, size_t size) {
	if (!cart.bsSlot) {
		return false;
	}
	bsLoad(data, size);
	return bsPresent();
}

void cartPower(bool reset) {
	(void) reset;
	if (cart.mcc) {
		mccPower();
	}
	if (cart.necdsp) {
		necdspPower();
	}
	if (cart.sa1) {
		sa1Power();
	}
	if (cart.gsu) {
		gsuPower();
	}
	if (cart.sdd1) {
		sdd1Power();
	}
	if (cart.obc1) {
		obc1Power();
	}
	if (cart.cx4) {
		cx4Power();
	}
	if (cart.epsonRtc) {
		epsonRtcPower();
	}
	if (cart.sharpRtc) {
		sharpRtcPower();
	}
	if (cart.spc7110) {
		spc7110Power();
	}
	if (cart.st018) {
		st018Power();
	}
	if (cart.bsSlot) {
		bsPower();
	}
}

// The coprocessors, brought up to the CPU's clock.
void cartSync(void) {
	if (cart.necdsp && necdsp.th.clock < cpu.th.clock) {
		necdspRun();
	}
	if (cart.sa1 && sa1.th.clock < cpu.th.clock) {
		sa1Run();
	}
	if (cart.gsu && gsu.th.clock < cpu.th.clock) {
		gsuRun();
	}
	if (cart.cx4 && cx4.th.clock < cpu.th.clock) {
		cx4Run();
	}
	if (cart.epsonRtc && epsonRtc.th.clock < cpu.th.clock) {
		epsonRtcRun();
	}
	if (cart.sharpRtc && sharpRtc.th.clock < cpu.th.clock) {
		sharpRtcRun();
	}
	if (cart.spc7110 && spc7110.th.clock < cpu.th.clock) {
		spc7110Run();
	}
	if (cart.st018 && st018.th.clock < cpu.th.clock) {
		st018Run();
	}
	if (bsRunning() && bsmemory.th.clock < cpu.th.clock) {
		bsRun();
	}
}
