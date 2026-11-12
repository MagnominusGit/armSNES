/* Copyright (c) 2004-2025 ares team, Near et al
 * Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Ported from ares (ISC); see LICENSES/ISC-ares.txt. */
#include "../snes.h"
#include <stdlib.h>
#include <string.h>

// The Sufami Turbo's two slots. Each takes a cartridge of ROM and maybe save RAM.

static struct {
	u8* rom;
	u32 romSize;
	u8* ram;
	u32 ramSize;
} slots[2];

bool sufamiInsert(int slot, const u8* data, size_t size) {
	free(slots[slot].rom);
	free(slots[slot].ram);
	memset(&slots[slot], 0, sizeof(slots[slot]));
	if (!data || size < 0x20000) {
		return false;
	}
	slots[slot].rom = malloc(size);
	memcpy(slots[slot].rom, data, size);
	slots[slot].romSize = size;
	// The header gives the save RAM in 2 KiB units.
	slots[slot].ramSize = data[0x37] * 0x800;
	if (slots[slot].ramSize) {
		slots[slot].ram = malloc(slots[slot].ramSize);
		memset(slots[slot].ram, 0xff, slots[slot].ramSize);
	}
	return true;
}

u8* sufamiRam(int slot, u32* size) {
	*size = slots[slot].ramSize;
	return slots[slot].ram;
}

static u8 readRom(int slot, u32 address, u8 data) {
	if (!slots[slot].rom) {
		return data;
	}
	return slots[slot].rom[address & (slots[slot].romSize - 1)];
}

static u8 readRam(int slot, u32 address, u8 data) {
	if (!slots[slot].ram) {
		return data;
	}
	return slots[slot].ram[address & (slots[slot].ramSize - 1)];
}

static void writeRam(int slot, u32 address, u8 data) {
	if (slots[slot].ram) {
		slots[slot].ram[address & (slots[slot].ramSize - 1)] = data;
	}
}

u8 sufamiReadRomA(u32 address, u8 data) {
	return readRom(0, address, data);
}

u8 sufamiReadRomB(u32 address, u8 data) {
	return readRom(1, address, data);
}

void sufamiWriteRom(u32 address, u8 data) {
	(void) address;
	(void) data;
}

u8 sufamiReadRamA(u32 address, u8 data) {
	return readRam(0, address, data);
}

u8 sufamiReadRamB(u32 address, u8 data) {
	return readRam(1, address, data);
}

void sufamiWriteRamA(u32 address, u8 data) {
	writeRam(0, address, data);
}

void sufamiWriteRamB(u32 address, u8 data) {
	writeRam(1, address, data);
}
