/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Licensed under the GNU General Public License, version 2 or (at your option) any later version: see LICENSE. */
#include "gb.h"

#include <string.h>

// The Game Boy Color's registers (Pan Docs, "CGB Registers"): the VRAM and work RAM banks, the speed
// switch, the palettes, VRAM DMA, object priority and the infrared port. On the original Game Boy,
// and on a Color running an original Game Boy game, most of them read as $FF.

enum {
	LCDC_ON = 0x80,
	// Dots a VRAM DMA takes for each 16 bytes, 2 a byte at either speed (SameBoy).
	BLOCK_DOTS = 32,
};

static bool colorHardware(const struct GB* gb) {
	return gbColor(gb);
}

// The palettes can't be reached while the PPU draws.
static bool palettesLocked(struct GB* gb) {
	gbPpuSync(gb);
	return (gb->ppu.lcdc & LCDC_ON) && gb->ppu.mode == 3;
}

// Moves 16-byte blocks from the source to VRAM, the CPU waiting: 2 dots before the first byte, and
// at single speed 2 after the last (SameBoy). The destination is kept whole, VRAM taking its low 13
// bits; past FFFF the transfer stops, the blocks left as they were.
static void copyBlocks(struct GB* gb, unsigned blocks) {
	struct GBCgb* cgb = &gb->cgb;
	gbPpuSync(gb);
	gb->now += 2;
	for (unsigned b = 0; b < blocks && cgb->hdmaBlocks; ++b) {
		for (int i = 0; i < 16; ++i) {
			uint16_t from = cgb->hdmaSource + i;
			uint8_t value = from >= 0xE000 || (from >= 0x8000 && from < 0xA000) ? 0xFF : from < 0x8000 && gb->cheatCart ? gbCheatCartRead(gb, from, true) : gbRead(gb, from);
			gb->vram[(gb->vramBank & 1) * 0x2000 + ((cgb->hdmaDest + i) & 0x1FFF)] = value;
		}
		cgb->hdmaSource += 16;
		cgb->hdmaDest += 16;
		gb->now += BLOCK_DOTS;
		--cgb->hdmaBlocks;
		if (!cgb->hdmaDest) {
			cgb->hdmaHblank = false;
			break;
		}
	}
	if (!gb->doubleSpeed) {
		gb->now += 2;
	}
	if (!cgb->hdmaBlocks) {
		cgb->hdmaHblank = false;
	}
}

// An H-blank DMA's block comes due at time at: 3 dots after mode 3 ends, or as the LCD goes off
// (SameBoy).
void gbHdmaHblank(struct GB* gb, uint32_t at) {
	gbSchedule(gb, GB_EVENT_HDMA, at);
}

void gbHdmaEvent(struct GB* gb) {
	struct GBCgb* cgb = &gb->cgb;
	if (cgb->hdmaHblank && cgb->hdmaBlocks && !gb->cpu.halted) {
		cgb->hdmaDue = true;
		gb->attention = true;
	}
}

void gbHdmaRun(struct GB* gb) {
	struct GBCgb* cgb = &gb->cgb;
	cgb->hdmaDue = false;
	if (cgb->hdmaHblank && cgb->hdmaBlocks) {
		copyBlocks(gb, 1);
	}
}

static void writePalette(struct GB* gb, uint8_t* palette, uint8_t* index, uint8_t value) {
	if (!palettesLocked(gb)) {
		palette[*index & 0x3F] = value;
		gb->colorsBuilt = false;
	}
	if (*index & 0x80) {
		*index = 0x80 | ((*index + 1) & 0x3F);
	}
}

uint8_t gbCgbRead(struct GB* gb, uint8_t reg) {
	struct GBCgb* cgb = &gb->cgb;
	if (!colorHardware(gb)) {
		return 0xFF;
	}
	switch (reg) {
	case 0x4C:
		return gb->bootMapped ? cgb->key0 : 0xFF;
	case 0x4D:
		return gb->cgbMode ? (gb->doubleSpeed ? 0x80 : 0) | (gb->io[0x4D] & 1) | 0x7E : 0xFF;
	case 0x4F:
		return 0xFE | (gb->cgbMode ? gb->vramBank : 0);
	case 0x55:
		return gb->cgbMode ? (cgb->hdmaBlocks ? (cgb->hdmaHblank ? 0 : 0x80) | ((cgb->hdmaBlocks - 1) & 0x7F) : 0xFF) : 0xFF;
	case 0x56:
		return gb->cgbMode ? cgb->rp | 0x3C | 0x02 : 0xFF;
	case 0x68:
		return cgb->bcps | 0x40;
	case 0x69:
		return gb->cgbMode && !palettesLocked(gb) ? cgb->bgPalette[cgb->bcps & 0x3F] : 0xFF;
	case 0x6A:
		return cgb->ocps | 0x40;
	case 0x6B:
		return gb->cgbMode && !palettesLocked(gb) ? cgb->objPalette[cgb->ocps & 0x3F] : 0xFF;
	case 0x6C:
		return gb->cgbMode ? cgb->opri | 0xFE : 0xFF;
	case 0x70:
		return gb->cgbMode ? 0xF8 | gb->wramBank : 0xFF;
	case 0x72:
	case 0x73:
		return cgb->undocumented[reg - 0x72];
	case 0x74:
		return gb->cgbMode ? cgb->undocumented[2] : 0xFF;
	case 0x75:
		return cgb->undocumented[3] | 0x8F;
	case 0x76:
	case 0x77:
		return gbApuPcm(gb, reg == 0x77);
	default:
		return 0xFF;
	}
}

void gbCgbWrite(struct GB* gb, uint8_t reg, uint8_t value) {
	struct GBCgb* cgb = &gb->cgb;
	if (!colorHardware(gb)) {
		return;
	}
	switch (reg) {
	case 0x4C:
		// The boot ROM's: bit 2 runs an original Game Boy game.
		if (gb->bootMapped) {
			cgb->key0 = value;
			gb->cgbMode = !(value & 0x04);
		}
		return;
	case 0x72:
	case 0x73:
		cgb->undocumented[reg - 0x72] = value;
		return;
	case 0x75:
		cgb->undocumented[3] = value & 0x70;
		return;
	default:
		break;
	}
	// In an original Game Boy game, the boot ROM still sets the palettes and object priority.
	if (!gb->cgbMode && !gb->bootMapped) {
		return;
	}
	switch (reg) {
	case 0x4D:
		gb->io[0x4D] = value & 1;
		break;
	case 0x4F:
		gbPpuSync(gb);
		gb->vramBank = value & 1;
		break;
	case 0x51:
		cgb->hdmaSource = (cgb->hdmaSource & 0x00F0) | value << 8;
		// E000 and up read as F000 and up (SameBoy).
		if (cgb->hdmaSource >= 0xE000) {
			cgb->hdmaSource |= 0xF000;
		}
		break;
	case 0x52:
		cgb->hdmaSource = (cgb->hdmaSource & 0xFF00) | (value & 0xF0);
		break;
	case 0x53:
		cgb->hdmaDest = value << 8 | (cgb->hdmaDest & 0xF0);
		break;
	case 0x54:
		cgb->hdmaDest = (cgb->hdmaDest & 0xFF00) | (value & 0xF0);
		break;
	case 0x55:
		// Writing bit 7 clear during an H-blank DMA stops it, its count taken from the write (read
		// back with bit 7 set, as SameBoy has it).
		if (cgb->hdmaHblank && !(value & 0x80)) {
			cgb->hdmaHblank = false;
			cgb->hdmaBlocks = (value & 0x7F) + 1;
			break;
		}
		cgb->hdmaBlocks = (value & 0x7F) + 1;
		if (value & 0x80) {
			// The PPU's steps up to now ran without it.
			gbPpuSync(gb);
			cgb->hdmaHblank = true;
			gbPpuReplan(gb);
			// Started in an H-blank, or with the LCD off, a block moves at once.
			if (!(gb->ppu.lcdc & LCDC_ON) || gbPpuInHblank(gb)) {
				copyBlocks(gb, 1);
			}
		} else {
			cgb->hdmaHblank = false;
			copyBlocks(gb, cgb->hdmaBlocks);
		}
		break;
	case 0x56:
		cgb->rp = value & 0xC1;
		break;
	case 0x68:
		cgb->bcps = value & 0xBF;
		break;
	case 0x69:
		writePalette(gb, cgb->bgPalette, &cgb->bcps, value);
		break;
	case 0x6A:
		cgb->ocps = value & 0xBF;
		break;
	case 0x6B:
		writePalette(gb, cgb->objPalette, &cgb->ocps, value);
		break;
	case 0x6C:
		cgb->opri = value & 1;
		break;
	case 0x70:
		gb->wramBank = value & 7;
		gbMapRom(gb);
		break;
	case 0x74:
		cgb->undocumented[2] = value;
		break;
	default:
		break;
	}
}
