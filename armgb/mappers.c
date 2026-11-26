/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Licensed under the GNU General Public License, version 2 or (at your option) any later version: see LICENSE. */
#include "gb.h"

#include <string.h>

// The rarer mappers: armGBA's own (Sachen, PKJD, NT's, the Mega Duck's), and Wisdom Tree, MMM01 and MBC6 as mGBA
// documents them. Each sets the banks gbMapRom maps: bank 0's (bank0, with bank0Mapped), the switchable
// one (romBank), or two 8 KiB halves (half, with halves).

static unsigned banks(const struct GB* gb) {
	return gb->cart.romBanks;
}

static uint8_t reorder(uint8_t value, const uint8_t order[8]) {
	uint8_t result = 0;
	for (int i = 0; i < 8; ++i) {
		result |= (value >> order[i] & 1) << i;
	}
	return result;
}

// Sachen's multicarts once their logo check has let the game start: the fixed area's header page
// reads through scrambled address lines (gbCartSachenAddress).
static void sachenBanks(struct GB* gb) {
	struct GBCart* cart = &gb->cart;
	unsigned base = cart->sachenBase & cart->sachenMask;
	// A 512 KiB cartridge of two games (Rocman X Gold + 4 in 1) has the second game in its upper half
	// and picks it with the base's bit 6, as its own start-up code shows by comparing the two headers.
	if (banks(gb) == 32 && (base & 0x40)) {
		base = (base & 0x1F) | 0x10;
	}
	cart->bank0 = base & (banks(gb) - 1);
	cart->romBank = ((cart->sachenBank & ~cart->sachenMask) | base) & (banks(gb) - 1);
}

uint16_t gbCartSachenAddress(uint16_t address) {
	return (address & 0xFFAC) | (address & 0x40) >> 6 | (address & 0x10) >> 3 | (address & 0x02) << 3 | (address & 0x01) << 6;
}

// NT's older multicarts: MBC3's RAM switch and a bank number, with a menu at $5000-$5003 choosing a
// game (its first bank, its size) and an order for the bank number's bits.
static const uint8_t ntOld1[8] = { 0, 2, 1, 4, 3, 5, 6, 7 };
static const uint8_t ntOld2[8] = { 1, 2, 0, 3, 4, 5, 6, 7 };

static void ntOldBanks(struct GB* gb) {
	struct GBCart* cart = &gb->cart;
	unsigned bank = cart->ntBank ? cart->ntBank : 1;
	if (cart->ntSwapped) {
		bank = reorder(bank, cart->mapper == GB_NT_OLD1 ? ntOld1 : ntOld2);
	}
	if (cart->ntCount) {
		bank &= cart->ntCount - 1;
	}
	cart->bank0 = cart->ntBase & (banks(gb) - 1);
	cart->romBank = (bank + cart->ntBase) & (banks(gb) - 1);
}

// The Mega Duck's cartridges (as megaduck-info and Super Junior SameDuck have them):
//   MD1   a write to B000 picks a 32 KiB bank for the whole of 0000-7FFF
//   MD2   a write to 0001 picks the bank at 4000-7FFF; bank 0 there is bank 1, as on an MBC1 (as the
//         MiSTer core has it: with bank 0, levels come out as garbage)
//   MD0   (the laptops' system ROMs and cartridges) a write to 1000 picks a 32 KiB bank with its low
//         half and an 8 KiB bank of the memory card's 32 KiB with bits 5-4; that RAM is always on
//   MD20S MD2 with an MD0 memory card: bits 5-4 at 1000 pick its bank
//   MD25S MD2 with 128 KiB of RAM: 4000 picks its bank (0000 is its enable, which games write but
//         Super Junior SameDuck doesn't need)
static void megaDuckWrite(struct GB* gb, uint16_t address, uint8_t value) {
	struct GBCart* cart = &gb->cart;
	switch (cart->duckKind) {
	case GB_DUCK_MD0:
		if (address == 0x1000) {
			cart->bank0 = (value & 0xF) * 2;
			cart->romBank = (value & 0xF) * 2 + 1;
			cart->ramBank = value >> 4 & 3;
		}
		break;
	case GB_DUCK_AUTO:
	case GB_DUCK_MD2:
	case GB_DUCK_MD20S:
	case GB_DUCK_MD25S:
		if (address == 0x0001) {
			cart->romBank = value ? value : 1;
		} else if (address == 0x1000 && cart->duckKind == GB_DUCK_MD20S) {
			cart->ramBank = value >> 4 & 3;
		} else if (address == 0x4000 && cart->duckKind == GB_DUCK_MD25S) {
			cart->ramBank = value & 0xF;
		}
		break;
	default:
		break;
	}
}

// MD1's register, where RAM would be.
void gbMegaDuckWrite(struct GB* gb, uint16_t address, uint8_t value) {
	struct GBCart* cart = &gb->cart;
	if (address == 0xB000 && (cart->duckKind == GB_DUCK_AUTO || cart->duckKind == GB_DUCK_MD1)) {
		cart->bank0 = value * 2;
		cart->romBank = value * 2 + 1;
		gbMapRom(gb);
	}
}

void gbSwapMegaDuckRom(struct GB* gb, const uint8_t* rom, size_t size, enum GBDuckKind kind) {
	struct GBCart* cart = &gb->cart;
	gb->rom = rom;
	gb->romSize = size;
	cart->romBanks = size / 0x4000 ? size / 0x4000 : 1;
	cart->duckKind = kind;
	cart->hasRam = cart->ramSize && (kind == GB_DUCK_MD0 || kind == GB_DUCK_MD20S || kind == GB_DUCK_MD25S);
	cart->ramEnabled = cart->hasRam;
	cart->bank0 = 0;
	cart->romBank = 1;
	cart->ramBank = 0;
	gbMapRom(gb);
}

void gbMapperInit(struct GB* gb) {
	struct GBCart* cart = &gb->cart;
	switch (cart->mapper) {
	case GB_SACHEN:
	case GB_NT_OLD1:
	case GB_NT_OLD2:
	case GB_WISDOM_TREE:
	case GB_MMM01:
	case GB_MEGADUCK:
		cart->bank0Mapped = true;
		break;
	default:
		break;
	}
	switch (cart->mapper) {
	case GB_SACHEN:
		cart->sachenBank = 1;
		sachenBanks(gb);
		break;
	case GB_NT_OLD1:
	case GB_NT_OLD2:
		cart->ntBank = 1;
		ntOldBanks(gb);
		break;
	case GB_NT_NEW:
		cart->half[0] = cart->half[1] = 2;
		break;
	case GB_WISDOM_TREE:
		cart->bank0 = 0;
		cart->romBank = 1;
		break;
	case GB_MMM01:
		// Until locked, the last 32 KiB, which hold the menu.
		cart->bank0 = banks(gb) - 2;
		cart->romBank = banks(gb) - 1;
		break;
	case GB_MBC6:
		cart->halves = true;
		cart->half[0] = 2;
		cart->half[1] = 3;
		cart->ramHalf[0] = 0;
		cart->ramHalf[1] = 1;
		break;
	default:
		break;
	}
}

// A write to $0000-$7FFF for one of these mappers.
void gbMapperWrite(struct GB* gb, uint16_t address, uint8_t value) {
	struct GBCart* cart = &gb->cart;
	switch (cart->mapper) {
	case GB_SACHEN:
		switch (address >> 13) {
		case 0:
			if ((cart->sachenBank & 0x30) == 0x30) {
				cart->sachenBase = value;
			}
			break;
		case 1:
			cart->sachenBank = value ? value : 1;
			break;
		case 2:
			if ((cart->sachenBank & 0x30) == 0x30) {
				cart->sachenMask = value;
			}
			break;
		}
		sachenBanks(gb);
		break;
	case GB_PKJD:
		// MBC3 with, in RAM's place, two counters (D, E) and their command register (F).
		switch (address >> 13) {
		case 0:
			cart->ramEnabled = (value & 0xF) == 0xA;
			break;
		case 1:
			cart->romBank = value & 0x7F ? value & 0x7F : 1;
			break;
		case 2:
			if (value < 8 || (value >= 0xD && value <= 0xF)) {
				cart->ramBank = value;
			}
			break;
		}
		break;
	case GB_NT_OLD1:
	case GB_NT_OLD2:
		if (address < 0x2000) {
			cart->ramEnabled = (value & 0xF) == 0xA;
		} else if (address < 0x4000) {
			cart->ntBank = cart->mapper == GB_NT_OLD1 ? value & 0x1F : value;
		} else if ((address & 0xF000) == 0x5000) {
			switch (address & 3) {
			case 1:
				cart->ntBase = (value & 0x3F) * 2;
				break;
			case 2:
				switch (value & 0xF) {
				case 0x8:
					cart->ntCount = 16;
					break;
				case 0xC:
					cart->ntCount = 8;
					break;
				case 0xE:
					cart->ntCount = 4;
					break;
				case 0xF:
					cart->ntCount = 2;
					break;
				default:
					cart->ntCount = 32;
					break;
				}
				break;
			case 3:
				cart->ntSwapped = value & 0x10;
				break;
			}
		}
		ntOldBanks(gb);
		break;
	case GB_NT_NEW:
		// MBC5; $55 at $14xx splits the switchable area, whose halves are then picked at $20xx and
		// $24xx (hhugboy's conditions). The split shows from the next bank chosen.
		if (address >> 8 == 0x14 && value == 0x55) {
			cart->ntSplit = true;
			return;
		}
		if (cart->ntSplit && (address >> 8 == 0x20 || address >> 8 == 0x24)) {
			cart->half[address >> 10 & 1] = value < 2 ? 2 : value;
		} else {
			switch (address >> 13) {
			case 0:
				cart->ramEnabled = (value & 0xF) == 0xA;
				break;
			case 1:
				cart->romBank = address < 0x3000 ? (cart->romBank & 0x100) | value : (value << 8 & 0x100) | (cart->romBank & 0xFF);
				cart->half[0] = cart->romBank * 2;
				cart->half[1] = cart->romBank * 2 + 1;
				break;
			case 2:
				cart->ramBank = value & 0xF;
				break;
			}
		}
		cart->halves = cart->ntSplit;
		break;
	case GB_WISDOM_TREE:
		// Writes to $0000-$3FFF pick a 32 KiB bank by their address.
		if (address < 0x4000) {
			cart->bank0 = (address & 0x3F) * 2;
			cart->romBank = (address & 0x3F) * 2 + 1;
		}
		break;
	case GB_MMM01:
		if (!cart->mmm01Locked) {
			switch (address >> 13) {
			case 0:
				cart->mmm01Locked = true;
				cart->bank0 = cart->mmm01Base;
				cart->romBank = cart->mmm01Base + 1;
				break;
			case 1:
				cart->mmm01Base = (cart->mmm01Base & ~0x7F) | (value & 0x7F);
				break;
			case 2:
				cart->mmm01Base = (cart->mmm01Base & ~0x180) | (value & 0x30) << 3;
				break;
			}
			break;
		}
		switch (address >> 13) {
		case 0:
			cart->ramEnabled = value == 0xA;
			break;
		case 1:
			cart->romBank = value + cart->mmm01Base;
			break;
		case 2:
			cart->ramBank = value;
			break;
		}
		break;
	case GB_MEGADUCK:
		megaDuckWrite(gb, address, value);
		break;
	case GB_MBC6:
		// 8 KiB ROM halves at $2000-$27FF and $3000-$37FF; 4 KiB RAM halves at $0400 and $0800. The flash
		// chip isn't mapped: its halves read as ROM.
		switch (address >> 10) {
		case 0:
			cart->ramEnabled = value == 0xA;
			break;
		case 1:
			cart->ramHalf[0] = value;
			break;
		case 2:
			cart->ramHalf[1] = value;
			break;
		case 0x8:
		case 0x9:
			cart->half[0] = value;
			break;
		case 0xC:
		case 0xD:
			cart->half[1] = value;
			break;
		}
		break;
	default:
		break;
	}
}

// PKJD's counters in RAM's place: false when RAM is there.
bool gbPkjdRead(struct GB* gb, uint8_t* value) {
	struct GBCart* cart = &gb->cart;
	if (cart->ramBank < 8) {
		return false;
	}
	*value = cart->ramBank == 0xD || cart->ramBank == 0xE ? cart->pkjdCounters[cart->ramBank - 0xD] : 0;
	return true;
}

bool gbPkjdWrite(struct GB* gb, uint8_t value) {
	struct GBCart* cart = &gb->cart;
	if (cart->ramBank < 8) {
		return false;
	}
	uint8_t* c = cart->pkjdCounters;
	if (cart->ramBank == 0xD || cart->ramBank == 0xE) {
		c[cart->ramBank - 0xD] = value;
	} else if (cart->ramBank == 0xF) {
		switch (value) {
		case 0x11:
			--c[0];
			break;
		case 0x12:
			--c[1];
			break;
		case 0x41:
			c[0] += c[1];
			break;
		case 0x42:
			c[1] += c[0];
			break;
		case 0x51:
			++c[0];
			break;
		case 0x52:
			--c[1];
			break;
		}
	}
	return true;
}
