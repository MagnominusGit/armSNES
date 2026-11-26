/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Licensed under the GNU General Public License, version 2 or (at your option) any later version: see LICENSE. */
#include "gb.h"

#include <string.h>

// The bus: the memory map (Pan Docs, "Memory Map") and the I/O registers. Reads of cartridge ROM and
// work RAM go through readMap without a call; everything else comes here.

// Bits of each I/O register that read as 1 whatever is written (unused, or write-only).
static const uint8_t ioUnused[0x80] = {
	[0x00] = 0xC0, [0x02] = 0x7E, [0x03] = 0xFF, [0x07] = 0xF8, [0x08] = 0xFF, [0x09] = 0xFF, [0x0A] = 0xFF,
	[0x0B] = 0xFF, [0x0C] = 0xFF, [0x0D] = 0xFF, [0x0E] = 0xFF, [0x0F] = 0xE0,
	[0x10] = 0x80, [0x11] = 0x3F, [0x13] = 0xFF, [0x14] = 0xBF, [0x15] = 0xFF, [0x16] = 0x3F, [0x18] = 0xFF,
	[0x19] = 0xBF, [0x1A] = 0x7F, [0x1B] = 0xFF, [0x1C] = 0x9F, [0x1D] = 0xFF, [0x1E] = 0xBF, [0x1F] = 0xFF,
	[0x20] = 0xFF, [0x23] = 0xBF, [0x26] = 0x70, [0x27] = 0xFF, [0x28] = 0xFF, [0x29] = 0xFF, [0x2A] = 0xFF,
	[0x2B] = 0xFF, [0x2C] = 0xFF, [0x2D] = 0xFF, [0x2E] = 0xFF, [0x2F] = 0xFF,
	[0x41] = 0x80,
	[0x4C] = 0xFF, [0x4D] = 0xFF, [0x4E] = 0xFF, [0x4F] = 0xFF, [0x50] = 0xFF, [0x51] = 0xFF, [0x52] = 0xFF,
	[0x53] = 0xFF, [0x54] = 0xFF, [0x55] = 0xFF, [0x56] = 0xFF, [0x57] = 0xFF, [0x58] = 0xFF, [0x59] = 0xFF,
	[0x5A] = 0xFF, [0x5B] = 0xFF, [0x5C] = 0xFF, [0x5D] = 0xFF, [0x5E] = 0xFF, [0x5F] = 0xFF, [0x60] = 0xFF,
	[0x61] = 0xFF, [0x62] = 0xFF, [0x63] = 0xFF, [0x64] = 0xFF, [0x65] = 0xFF, [0x66] = 0xFF, [0x67] = 0xFF,
	[0x68] = 0xFF, [0x69] = 0xFF, [0x6A] = 0xFF, [0x6B] = 0xFF, [0x6C] = 0xFF, [0x6D] = 0xFF, [0x6E] = 0xFF,
	[0x6F] = 0xFF, [0x70] = 0xFF, [0x71] = 0xFF, [0x72] = 0xFF, [0x73] = 0xFF, [0x74] = 0xFF, [0x75] = 0xFF,
	[0x76] = 0xFF, [0x77] = 0xFF, [0x78] = 0xFF, [0x79] = 0xFF, [0x7A] = 0xFF, [0x7B] = 0xFF, [0x7C] = 0xFF,
	[0x7D] = 0xFF, [0x7E] = 0xFF, [0x7F] = 0xFF,
};

// Sets which ROM and work RAM banks the CPU reads, and which of them it reads directly.
void gbMapRom(struct GB* gb) {
	const struct GBCart* cart = &gb->cart;
	size_t mask = gb->romSize - 1;
	size_t low = (size_t) (cart->mapper == GB_MBC1 && cart->mbc1Mode ? cart->mbc1Upper << (cart->mbc1Multicart ? 4 : 5) : 0) * 0x4000 & mask;
	if (cart->bank0Mapped) {
		low = (size_t) cart->bank0 * 0x4000 & mask;
	}
	size_t high = (size_t) cart->romBank * 0x4000 & mask;
	for (int i = 0; i < 4; ++i) {
		gb->memMap[i] = gb->rom + low + i * 0x1000;
		gb->memMap[4 + i] = gb->rom + high + i * 0x1000;
	}
	if (cart->halves) {
		for (int i = 0; i < 4; ++i) {
			gb->memMap[4 + i] = gb->rom + ((size_t) cart->half[i >> 1] * 0x2000 & mask) + (i & 1) * 0x1000;
		}
	}
	unsigned bank = gb->cgbMode ? (gb->wramBank & 7 ? gb->wramBank & 7 : 1) : 1;
	gb->memMap[0xC] = gb->wram;
	gb->memMap[0xD] = gb->wram + bank * 0x1000;
	gb->memMap[0xE] = gb->wram;
	for (int i = 0; i < 16; ++i) {
		gb->readMap[i] = gb->dma.active ? NULL : gb->memMap[i];
		gb->writeMap[i] = NULL;
	}
	gb->writeMap[0xC] = gb->writeMap[0xE] = gb->wram;
	gb->writeMap[0xD] = (uint8_t*) gb->memMap[0xD];
	// Sachen's header page reads through its scrambled address lines.
	if (gb->bootMapped || cart->mapper == GB_SACHEN) {
		gb->readMap[0] = NULL;
	}
	for (int i = 0; i < 8; ++i) {
		if (gb->cheatCart || (gb->genieCount && gbCheatsOnPage(gb, i))) {
			gb->readMap[i] = NULL;
		}
	}
	if (cart->romTransformed) {
		for (int i = 4; i < 8; ++i) {
			gb->readMap[i] = NULL;
		}
	}
	// Vast Fame: every ROM page while a read is armed, and bank 0 from where another bank covers it.
	if (cart->variant == GB_VAST_FAME) {
		const struct GBVastFame* vf = &cart->vf;
		bool armed = (vf->at7000[0] & 7) >= 4;
		unsigned from = vf->at7000[10] << 8 | vf->at7000[9];
		bool covered = (vf->at7000[8] & 0xF) == 0xF && from < 0x4000;
		for (int i = 0; i < 8; ++i) {
			if (armed || (covered && i < 4 && i >= (int) (from >> 12))) {
				gb->readMap[i] = NULL;
			}
		}
	}

}

// The bus a DMA source or a CPU access is on: the external bus (the cartridge, and on the original
// work RAM too), VRAM's, or on the Color work RAM's own (SameBoy's bus_for_addr).
enum { BUS_EXTERNAL, BUS_VRAM, BUS_WRAM };

static int busOf(const struct GB* gb, uint16_t address) {
	if (address >= 0x8000 && address < 0xA000) {
		return BUS_VRAM;
	}
	return address >= 0xC000 && gbColor(gb) ? BUS_WRAM : BUS_EXTERNAL;
}

// A byte for OAM DMA, read without the PPU's locks.
static uint8_t dmaByte(struct GB* gb, uint16_t address) {
	switch (address >> 12) {
	case 0x8:
	case 0x9:
		return gb->vram[(gb->vramBank & 1) * 0x2000 + (address & 0x1FFF)];
	case 0xA:
	case 0xB:
		return gbCartReadRam(gb, address);
	case 0xE:
	case 0xF:
		return gb->memMap[0xC + (address >> 12 & 1)][address & 0xFFF];
	case 0x4:
	case 0x5:
	case 0x6:
	case 0x7:
		if (gb->cheatCart) {
			return gbCheatCartRead(gb, address, true);
		}
		return gbCartRomByte(gb, gb->memMap[address >> 12][address & 0xFFF]);
	default:
		if (address < 0x4000 && gb->cheatCart) {
			return gbCheatCartRead(gb, address, true);
		}
		return gb->memMap[address >> 12][address & 0xFFF];
	}
}

// Which byte the DMA moves in the M-cycle ending now: -1 while it waits, 160 when it's done.
static int dmaIndex(const struct GB* gb) {
	int32_t cycles = gbSince(gb->now, gb->dma.start) / gb->dma.step;
	if (cycles < 2) {
		return -1;
	}
	return cycles - 2 < 160 ? cycles - 2 : 160;
}

// Copies the bytes moved by now.
void gbDmaSync(struct GB* gb) {
	struct GBDma* dma = &gb->dma;
	if (!dma->active) {
		return;
	}
	int index = dmaIndex(gb);
	// Lines still to be drawn see OAM as it was.
	gbPpuSync(gb);
	// The byte of the M-cycle ending now is moved by its end too.
	int upTo = index < 0 ? 0 : index < 160 ? index + 1 : 160;
	for (; dma->done < upTo; ++dma->done) {
		gb->oam[dma->done] = dmaByte(gb, dma->source + dma->done);
	}
	if (dma->done == 160 && index == 160) {
		dma->active = false;
		gbUnschedule(gb, GB_EVENT_DMA);
		gbMapRom(gb);
	}
}

void gbDmaStart(struct GB* gb, uint8_t page) {
	struct GBDma* dma = &gb->dma;
	gbDmaSync(gb);
	dma->overlap = dma->active && dmaIndex(gb) >= 0;
	dma->active = true;
	dma->source = page << 8;
	dma->start = gb->now;
	dma->done = 0;
	// A byte per M-cycle of the CPU's, at the CPU Clock (armGBA's): 16 / quarters T-cycles, whole ones.
	unsigned quarters = gb->doubleSpeed && gb->cpuQuarters > 8 ? 8 : gb->cpuQuarters;
	dma->step = gb->cpuQuarters == 4 ? gb->mcycle : gb->doubleSpeed ? 8 / quarters : 16 / quarters;
	gbSchedule(gb, GB_EVENT_DMA, gb->now + 162 * dma->step);
	gbMapRom(gb);
}

// During the transfer, OAM reads as $FF and a read on the source's bus gets the byte being moved.
static bool dmaConflict(struct GB* gb, uint16_t address, uint8_t* value) {
	gbDmaSync(gb);
	struct GBDma* dma = &gb->dma;
	if (!dma->active) {
		return false;
	}
	int index = dmaIndex(gb);
	if (index >= 0) {
		dma->overlap = false;
	}
	bool oam = address >= 0xFE00 && address < 0xFF00;
	if (index < 0 && dma->overlap && oam) {
		*value = 0xFF;
		return true;
	}
	if (index < 0 || index >= 160) {
		return false;
	}
	if (oam) {
		*value = 0xFF;
		return true;
	}
	if (address >= 0xFE00 || busOf(gb, address) != busOf(gb, dma->source)) {
		return false;
	}
	*value = dmaByte(gb, dma->source + index);
	return true;
}

static uint8_t joypad(const struct GB* gb) {
	if (gb->sgbJoypad) {
		return 0xC0 | gb->joypadSelect | (gb->sgbJoypad(gb->user, gb->joypadSelect, false) & 0xF);
	}
	uint8_t value = 0xCF | gb->joypadSelect;
	if (!(gb->joypadSelect & 0x10)) {
		value &= ~(gb->keys >> 4 & 0xF);
	}
	if (!(gb->joypadSelect & 0x20)) {
		value &= ~(gb->keys & 0xF);
	}
	return value;
}

uint8_t gbIoRead(struct GB* gb, uint8_t reg) {
	if (gbSince(gb->now, gb->nextEvent) >= 0) {
		gbRunEvents(gb);
	}
	switch (reg) {
	case 0x00:
		return joypad(gb);
	case 0x01:
		return gb->serial.sb;
	case 0x02:
		// The Color's fast clock bit is there in Color mode.
		return gb->serial.sc | (gb->cgbMode ? 0x7C : ioUnused[reg]);
	case 0x04:
	case 0x05:
	case 0x06:
	case 0x07:
		return gbTimerRead(gb, reg);
	case 0x0F:
		return gb->ifl | 0xE0;
	case 0x10 ... 0x3F:
		return gbApuRead(gb, reg) | ioUnused[reg];
	case 0x4C:
	case 0x4D:
	case 0x4F:
	case 0x51 ... 0x56:
	case 0x68 ... 0x6C:
	case 0x70:
	case 0x72 ... 0x77:
		return gbCgbRead(gb, reg);
	case 0x40:
	case 0x41:
	case 0x42:
	case 0x43:
	case 0x44:
	case 0x45:
	case 0x46:
	case 0x47:
	case 0x48:
	case 0x49:
	case 0x4A:
	case 0x4B:
		return gbPpuRead(gb, reg);
	default:
		return gb->io[reg] | ioUnused[reg];
	}
}

void gbIoWrite(struct GB* gb, uint8_t reg, uint8_t value) {
	if (gbSince(gb->now, gb->nextEvent) >= 0) {
		gbRunEvents(gb);
	}
	switch (reg) {
	case 0x00:
		gb->joypadSelect = value & 0x30;
		if (gb->sgbJoypad) {
			gb->sgbJoypad(gb->user, gb->joypadSelect, true);
		}
		break;
	case 0x01:
	case 0x02:
		gbSerialWrite(gb, reg, value);
		break;
	case 0x04:
	case 0x05:
	case 0x06:
	case 0x07:
		gbTimerWrite(gb, reg, value);
		break;
	case 0x0F:
		gb->ifl = value & 0x1F;
		gb->attention = true;
		break;
	case 0x10 ... 0x3F:
		gbApuWrite(gb, reg, value);
		break;
	case 0x4C:
	case 0x4D:
	case 0x4F:
	case 0x51 ... 0x56:
	case 0x68 ... 0x6C:
	case 0x70:
	case 0x72 ... 0x75:
		gbCgbWrite(gb, reg, value);
		break;
	case 0x50:
		if (value & 1 && gb->bootMapped) {
			gb->bootMapped = false;
			gbMapRom(gb);
		}
		break;
	case 0x40:
	case 0x41:
	case 0x42:
	case 0x43:
	case 0x44:
	case 0x45:
	case 0x46:
	case 0x47:
	case 0x48:
	case 0x49:
	case 0x4A:
	case 0x4B:
		gbPpuWrite(gb, reg, value);
		break;
	default:
		gb->io[reg] = value;
		break;
	}
}

// The Mega Duck is a Game Boy with its I/O registers moved and some of their bits rearranged (as
// SameDuck has them): each of its registers is the Game Boy register here, or none (-1).
static const int8_t duckIo[0x80] = {
	// 00-0F: joypad, serial, timer and IF, where the Game Boy has them
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
	// 10-1F: LCDC STAT SCY SCX OBP0 OBP1 WY WX LY LYC DMA BGP
	0x40, 0x41, 0x42, 0x43, 0x48, 0x49, 0x4A, 0x4B, 0x44, 0x45, 0x46, 0x47, -1, -1, -1, -1,
	// 20-2F: NR10 NR12 NR11 NR13 NR14 NR21 (NR20) NR22 NR23 NR24 NR30 NR31 NR32 NR34 NR33 (NR40)
	0x10, 0x12, 0x11, 0x13, 0x14, 0x16, 0x15, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1E, 0x1D, 0x1F,
	// 30-3F: wave RAM
	0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
	// 40-4F: NR41 NR43 NR42 NR44 NR50 NR52 NR51, then nothing up to the Color's registers
	0x20, 0x22, 0x21, 0x23, 0x24, 0x26, 0x25, -1, -1, -1, -1, -1, 0x4C, 0x4D, 0x4E, 0x4F,
	0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F,
	0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F,
	0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F,
};

// LCDC bit by bit: the Mega Duck's bit 6 is the Game Boy's 0 (background on), 3 is 6 (window map), 2 is
// 3 (background map), 1 is 2 (sprite size) and 0 is 1 (sprites on); 7, 5 and 4 match. The envelopes
// (NR12, NR22, NR42) and the noise's frequency (NR43) have their halves swapped. The wave's volume (NR32
// bits 6-5) has full and a quarter the other way round: 11 is full, 01 a quarter (as GBDK-2020 has it).
static uint8_t duckValue(uint8_t reg, uint8_t v, bool toGb) {
	switch (reg) {
	case 0x40:
		return toGb ? (v & 0xB0) | (v >> 6 & 0x01) | (v << 3 & 0x40) | (v << 1 & 0x0E)
		            : (v & 0xB0) | (v << 6 & 0x40) | (v >> 3 & 0x08) | (v >> 1 & 0x07);
	case 0x12:
	case 0x17:
	case 0x21:
	case 0x22:
		return (uint8_t) (v << 4 | v >> 4);
	case 0x1C:
		return (v & ~0x60) | ((0u - (v & 0x60)) & 0x60);
	default:
		return v;
	}
}

static __attribute__((noinline)) uint8_t duckIoRead(struct GB* gb, uint8_t reg) {
	int8_t gbReg = duckIo[reg & 0x7F];
	return gbReg < 0 ? 0xFF : duckValue(gbReg, gbIoRead(gb, gbReg), false);
}

static __attribute__((noinline)) void duckIoWrite(struct GB* gb, uint8_t reg, uint8_t value) {
	int8_t gbReg = duckIo[reg & 0x7F];
	if (gbReg >= 0) {
		gbIoWrite(gb, gbReg, duckValue(gbReg, value, true));
	}
}

static inline uint8_t ioRead(struct GB* gb, uint8_t reg) {
	return gb->cart.mapper == GB_MEGADUCK ? duckIoRead(gb, reg) : gbIoRead(gb, reg);
}

static inline void ioWrite(struct GB* gb, uint8_t reg, uint8_t value) {
	if (gb->cart.mapper == GB_MEGADUCK) {
		duckIoWrite(gb, reg, value);
	} else {
		gbIoWrite(gb, reg, value);
	}
}

uint8_t gbRead(struct GB* gb, uint16_t address) {
	uint8_t conflict;
	if (gb->dma.active && dmaConflict(gb, address, &conflict)) {
		return conflict;
	}
	if (address < 0x8000) {
		if (gb->bootMapped && (address < 0x100 || (gb->bootSize > 0x100 && address >= 0x200 && address < 0x900))) {
			return gb->boot[address];
		}
		uint8_t value = gb->cheatCart ? gbCheatCartRead(gb, address, false) : gbCartRomRead(gb, address);
		return gb->genieCount ? gbCheatRomByte(gb, address, value) : value;
	}
	switch (address >> 12) {
	case 0x8:
	case 0x9:
		gbPpuSync(gb);
		if (gb->ppu.vramReadBlocked) {
			return 0xFF;
		}
		return gb->vram[(gb->vramBank & 1) * 0x2000 + (address & 0x1FFF)];
	case 0xA:
	case 0xB:
		return gbCartReadRam(gb, address);
	case 0xF:
		if (address < 0xFE00) {
			return gb->memMap[0xD][address & 0xFFF];
		}
		if (address < 0xFEA0) {
			gbPpuSync(gb);
			if (gb->ppu.oamReadBlocked) {
				return 0xFF;
			}
			return gb->oam[address & 0xFF];
		}
		if (address < 0xFF00) {
			return 0;
		}
		if (address < 0xFF80) {
			return ioRead(gb, address);
		}
		if (address < 0xFFFF) {
			return gb->hram[address & 0x7F];
		}
		return gb->ie;
	default:
		return gb->memMap[address >> 12][address & 0xFFF];
	}
}

// The game's ROM as the CPU reads it through its mapper.
uint8_t gbCartRomRead(struct GB* gb, uint16_t address) {
	if (gb->cart.variant == GB_VAST_FAME && !gb->bootMapped) {
		return gbCartReadVastFame(gb, address);
	}
	if (address >= 0x4000) {
		return gbCartRomByte(gb, gb->memMap[address >> 12][address & 0xFFF]);
	}
	if (gb->cart.mapper == GB_SACHEN && address >= 0x100 && address < 0x200) {
		return gb->memMap[0][gbCartSachenAddress(address)];
	}
	return gb->memMap[address >> 12][address & 0xFFF];
}

void gbWrite(struct GB* gb, uint16_t address, uint8_t value) {
	switch (address >> 12) {
	case 0x0:
	case 0x1:
	case 0x2:
	case 0x3:
	case 0x4:
	case 0x5:
	case 0x6:
	case 0x7:
		if (gb->cheatCart) {
			gbCheatCartWrite(gb, address, value);
		} else {
			gbCartWrite(gb, address, value);
		}
		return;
	case 0x8:
	case 0x9:
		gbPpuSync(gb);
		if (!gb->ppu.vramWriteBlocked) {
			gb->vram[(gb->vramBank & 1) * 0x2000 + (address & 0x1FFF)] = value;
		}
		return;
	case 0xA:
	case 0xB:
		gbCartWriteRam(gb, address, value);
		return;
	case 0xC:
	case 0xE:
		gb->wram[address & 0xFFF] = value;
		return;
	case 0xD:
		((uint8_t*) gb->memMap[0xD])[address & 0xFFF] = value;
		return;
	default:
		if (address < 0xFE00) {
			((uint8_t*) gb->memMap[0xD])[address & 0xFFF] = value;
		} else if (address < 0xFEA0) {
			gbPpuSync(gb);
			gbDmaSync(gb);
			if (!gb->ppu.oamWriteBlocked && !(gb->dma.active && (dmaIndex(gb) >= 0 || gb->dma.overlap))) {
				gb->oam[address & 0xFF] = value;
			}
		} else if (address < 0xFF00) {
		} else if (address < 0xFF80) {
			ioWrite(gb, address, value);
		} else if (address < 0xFFFF) {
			gb->hram[address & 0x7F] = value;
		} else {
			gb->ie = value;
			gb->attention = true;
		}
		return;
	}
}
