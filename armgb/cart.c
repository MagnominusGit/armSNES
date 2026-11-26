/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Licensed under the GNU General Public License, version 2 or (at your option) any later version: see LICENSE.
 *
 * MBC7, HuC3 and the Pocket Camera follow SameBoy (Copyright (c) Lior Halphon), under the Expat (MIT) license:
 * see LICENSES/SameBoy.txt. */
#include "gb.h"

#include <string.h>

// The cartridge header and the memory bank controllers (Pan Docs, "The Cartridge Header" and "MBCs").

static const uint32_t ramSizes[6] = { 0, 0x800, 0x2000, 0x8000, 0x20000, 0x10000 };

// Unlicensed MBC5 variants (bit orders and the second-logo CRCs as mGBA documents them, src/gb/mbc).
// Each order lists, for each bit of the result, the bit of the input it comes from.
static const uint8_t sintaxOrders[16][8] = {
	{ 2, 1, 4, 3, 6, 5, 0, 7 }, { 3, 2, 5, 4, 7, 6, 1, 0 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 },
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, { 4, 5, 2, 3, 0, 1, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 6, 7, 4, 5, 1, 3, 0, 2 },
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, { 7, 6, 1, 0, 3, 2, 5, 4 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 5, 4, 7, 6, 1, 0, 3, 2 },
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, { 2, 3, 4, 5, 6, 7, 0, 1 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 },
};
static const uint8_t bbdData[8][8] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 },
	{ 0, 5, 1, 3, 4, 2, 6, 7 }, { 0, 4, 2, 3, 1, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 5, 3, 4, 2, 6, 7 },
};
static const uint8_t bbdBanks[8][8] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 3, 4, 2, 0, 1, 5, 6, 7 },
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, { 1, 2, 3, 4, 0, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 },
};
static const uint8_t hitekData[8][8] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 6, 5, 3, 4, 1, 2, 7 }, { 0, 5, 6, 3, 4, 2, 1, 7 }, { 0, 6, 2, 3, 4, 5, 1, 7 },
	{ 0, 6, 1, 3, 4, 5, 2, 7 }, { 0, 1, 6, 3, 4, 5, 2, 7 }, { 0, 2, 6, 3, 4, 1, 5, 7 }, { 0, 6, 2, 3, 4, 1, 5, 7 },
};
static const uint8_t hitekBanks[8][8] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, { 3, 2, 1, 0, 4, 5, 6, 7 }, { 2, 1, 0, 3, 4, 5, 6, 7 }, { 1, 0, 3, 2, 4, 5, 6, 7 },
	{ 0, 3, 2, 1, 4, 5, 6, 7 }, { 2, 3, 0, 1, 4, 5, 6, 7 }, { 3, 0, 1, 2, 4, 5, 6, 7 }, { 2, 0, 3, 1, 4, 5, 6, 7 },
};
static const uint8_t ggb81Data[8][8] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 2, 1, 3, 4, 6, 5, 7 }, { 0, 6, 5, 3, 4, 2, 1, 7 }, { 0, 5, 1, 3, 4, 2, 6, 7 },
	{ 0, 5, 2, 3, 4, 1, 6, 7 }, { 0, 2, 6, 3, 4, 5, 1, 7 }, { 0, 1, 6, 3, 4, 2, 5, 7 }, { 0, 2, 5, 3, 4, 6, 1, 7 },
};

static uint8_t reorder(uint8_t value, const uint8_t order[8]) {
	uint8_t result = 0;
	for (int i = 0; i < 8; ++i) {
		result |= (value >> order[i] & 1) << i;
	}
	return result;
}

static uint32_t crc32(const uint8_t* data, size_t size) {
	uint32_t crc = 0xFFFFFFFF;
	for (size_t i = 0; i < size; ++i) {
		crc ^= data[i];
		for (int k = 0; k < 8; ++k) {
			crc = crc >> 1 ^ (0xEDB88320 & -(crc & 1));
		}
	}
	return ~crc;
}

static bool sintaxLocked(const struct GB* gb);
static uint32_t crc32(const uint8_t* data, size_t size);

static unsigned logoSum(const struct GB* gb) {
	unsigned sum = 0;
	for (int i = 0x184; i < 0x1B4; ++i) {
		sum += gb->rom[i];
	}
	return sum;
}

static bool vastFameMaybeGgb81(const struct GB* gb) {
	return logoSum(gb) == 2981;
}

// Which variant a cartridge is, by its second logo; a "fixed" dump (its last bank-0 byte 1) is plain.
static uint8_t variantOf(const struct GB* gb) {
	const uint8_t* rom = gb->rom;
	if (gb->romSize < 0x8000) {
		return GB_PLAIN;
	}
	// Vast Fame's logos ("V.fame", "SOUL", "DIGI" in italics) by the sum of their bytes (hhugboy).
	unsigned sum = 0;
	for (int i = 0x184; i < 0x1B4; ++i) {
		sum += rom[i];
	}
	// Vast Fame's logo, Soul Falchion's, an italic DIGI, two its games carry elsewhere, and one GGB-81's
	// games carry too (armGBA's list).
	if (sum == 4844 || sum == 6127 || sum == 4406 || sum == 3209 || sum == 2981) {
		return GB_VAST_FAME;
	}
	switch (crc32(&rom[0x184], 0x30)) {
	case 0x4FDAB691:
		return GB_HITEK;
	case 0xC7D8C1DF:
	case 0x6D1EA662:
		return rom[0x7FFF] != 0x01 ? GB_BBD : GB_PLAIN;
	case 0x79F34594:
	case 0x7E8C539B:
		return GB_GGB81;
	case 0x20D092E2:
	case 0xD2B57657:
		return rom[0x147] == 0x01 || (0x8000u << rom[0x148]) != gb->romSize ? GB_LI_CHENG : GB_PLAIN;
	case 0x6C1DCF2D:
	case 0x99E3449D:
		return sintaxLocked(gb) ? GB_SINTAX : GB_PLAIN;
	default:
		break;
	}
	// Other logos of the same makers, by the sum of their bytes as hhugboy tells them (armGBA's list).
	// Their games end each bank in its number; a dump where that holds is one patched to run without
	// the lock.
	bool locked = rom[0x7FFF] != 0x01 || (gb->romSize > 0xBFFF && rom[0xBFFF] != 0x02);
	switch (logoSum(gb)) {
	case 4639: // BBD
	case 3334: // "S-GBC"
	case 5092: // Fiver Firm
		return locked ? GB_BBD : GB_PLAIN;
	case 4125:
	case 4138:
		return sintaxLocked(gb) ? GB_SINTAX : GB_PLAIN;
	default:
		break;
	}
	// The Telefang pirate sold as Pokemon Jade, on a Li Cheng cartridge (armGBA's).
	return crc32(rom + 0x100, 0x50) == 0x3EF5AFB2 ? GB_LI_CHENG : GB_PLAIN;
}

// Sintax's games end each bank in its number, with the bits in the lock's order. Where banks 2, 4 and 8
// end in just those the dump runs without the lock (armGBA's rule: Emo Cheng DX, Luke Yingxiong Chuan 3).
static bool sintaxLocked(const struct GB* gb) {
	const uint8_t* rom = gb->rom;
	if (rom[0x7FFF] <= 0x01) {
		return false;
	}
	return gb->romSize < 9 * 0x4000u || rom[3 * 0x4000 - 1] != 2 || rom[5 * 0x4000 - 1] != 4 || rom[9 * 0x4000 - 1] != 8;
}

// NT's cartridges have no mark of their own; their games are told by their headers (armGBA's list, from
// mGBA's), and Makon's by the licensee code it made up: the first few on the older kind, with a header
// that says 256 KiB, the rest on the newer. 0 for none.
static uint8_t ntMapper(const struct GB* gb) {
	const uint8_t* rom = gb->rom;
	if (gb->romSize < 0x8000) {
		return 0;
	}
	uint32_t header = crc32(rom + 0x100, 0x50);
	switch (header) {
	case 0xE1147E75: // Rockman 8
	case 0xEFF88FAA: // True Color 25 in 1
		return GB_NT_OLD1;
	case 0x811925D9: // 23 in 1
	case 0x62A8016A: // 29 in 1
	case 0x5758D6D9: // Caise Gedou 24 in 1
	case 0x80265A64: // Rockman X4
	case 0x805459DE: // Sonic Adventure 8
	case 0x0B1B808A: // Super Donkey Kong 5
	case 0x4650EB9A: // Super Mario Special 3
		return GB_NT_OLD2;
	case 0xB289D95A: // Capcom vs SNK - Millennium Fight 2001
	case 0x688D6713: // Digimon 02 4
	case 0x8931A272: // Digimon 2
	case 0x79083C6B: // Digimon Pocket
	case 0x0C5047EE: // Harry Potter 3
	case 0x8AC634B7: // Pokemon Diamond (Special Pikachu Edition)
	case 0x8628A287: // Pokemon Jade (Special Pikachu Edition)
	case 0xBC75D7B8: // Pokemon - Mewtwo Strikes Back
	case 0xFF0B60CC: // Shuma Baolong 02 4
	case 0x14A992A6: // Street Fighter Zero 4
		return GB_NT_NEW;
	}
	if ((rom[0x144] == 'M' && rom[0x145] == 'K') || (rom[0x144] == 'G' && rom[0x145] == 'C')) {
		static const char* const early[] = { "SONIC 7", "SUPER MARIO 3", "DONKEY\x09KONG 5", "ROCKMAN 99" };
		for (unsigned i = 0; i < sizeof(early) / sizeof(early[0]); ++i) {
			if (!strcmp((const char*) rom + 0x134, early[i])) {
				return rom[0x148] == 3 ? GB_NT_OLD2 : 0;
			}
		}
		// Two dumps of such games are patched to run on the cartridge their headers name.
		return header == 0x72EDBF3E || header == 0x212ECA2F ? 0 : GB_NT_NEW;
	}
	return 0;
}

// mGBA's test for Wisdom Tree's cartridges: no title or header around it, a header checksum of E7, and
// the company's name in the ROM.
static bool isWisdomTree(const struct GB* gb) {
	const uint8_t* rom = gb->rom;
	for (int i = 0x134; i < 0x14C; ++i) {
		if (rom[i]) {
			return false;
		}
	}
	for (int i = 0xF0; i < 0x100; ++i) {
		if (rom[i]) {
			return false;
		}
	}
	if (rom[0x14D] != 0xE7) {
		return false;
	}
	for (size_t i = 0x300; i + 11 < gb->romSize; ++i) {
		if (!memcmp(rom + i, "WISDOM", 6) && !memcmp(rom + i + 7, "TREE", 4)) {
			return true;
		}
	}
	return false;
}

// Sachen's multicarts keep the logo where their scrambled address lines put it (armGBA's).
static bool isSachen(const struct GB* gb) {
	const uint8_t* rom = gb->rom;
	return gb->romSize >= 0x200 && ((rom[0x104] == 0xCE && rom[0x144] == 0xED && rom[0x114] == 0x66)
	                                || (rom[0x184] == 0xCE && rom[0x1C4] == 0xED && rom[0x194] == 0x66));
}

// mGBA's test for MMM01: its own header, a valid one with a type of 0B-0D, in the last 32 KiB.
static const uint8_t* mmm01Header(const struct GB* gb) {
	static const uint8_t logo[0x30] = {
		0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
		0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E, 0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
		0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E,
	};
	if (gb->romSize < 0x10000) {
		return NULL;
	}
	const uint8_t* footer = gb->rom + gb->romSize - 0x8000;
	return !memcmp(footer + 0x104, logo, sizeof(logo)) && footer[0x147] >= 0x0B && footer[0x147] <= 0x0D ? footer : NULL;
}

// What the switchable bank's bytes read as: in a data bit order, or XORed with Sintax's key for the
// bank written.
static void romScramble(struct GBCart* cart) {
	const uint8_t* order = NULL;
	uint8_t key = 0;
	switch (cart->variant) {
	case GB_SINTAX:
		key = cart->xorKeys[cart->bankWritten & 3];
		break;
	case GB_BBD:
		order = bbdData[cart->dataOrder];
		break;
	case GB_HITEK:
		order = hitekData[cart->dataOrder];
		break;
	case GB_GGB81:
		order = ggb81Data[cart->dataOrder];
		break;
	case GB_VAST_FAME:
		order = ggb81Data[cart->vf.dataMode];
		break;
	default:
		break;
	}
	cart->romTransformed = key || (order && memcmp(order, bbdData[0], 8));
	for (int i = 0; i < 256; ++i) {
		cart->romLut[i] = (order ? reorder(i, order) : i) ^ key;
	}
}

// Vast Fame (armGBA's, after hhugboy): how many reads the armed sequence answers.
static unsigned vastFameLength(const struct GBVastFame* vf) {
	return (vf->at7000[0] & 7) >= 4 ? (vf->at7000[0] & 7) - 3 : 0;
}

// Where the difference between the two kinds' starting values is after so many turns.
static uint8_t vastFameTurned(unsigned turns) {
	return (0x10 >> (turns & 7) | 0x10 << (8 - (turns & 7))) & 0xFF;
}

// The registers as the other kind of cartridge would have them.
static void vastFameOtherKind(struct GBVastFame* vf) {
	vf->start ^= 0x10;
	vf->running ^= vastFameTurned(vf->turns);
	vf->at6000 ^= vastFameTurned(vf->turnOf[11]);
	for (int i = 0; i < 11; ++i) {
		vf->at7000[i] ^= vastFameTurned(vf->turnOf[i]);
	}
	vf->decided = true;
}

static void vastFameWrite(struct GB* gb, uint16_t address, uint8_t value) {
	struct GBVastFame* vf = &gb->cart.vf;
	uint16_t at = address & 0xF00F;
	if (at == 0x7000 && value == 0x96) {
		vf->opened = true;
		vf->dataMode = 0;
		vf->config = true;
		vf->running = vf->start;
		vf->turns = 0;
		romScramble(&gb->cart);
	} else if (at == 0x700F && value == 0x96) {
		vf->config = false;
	} else if (vf->config && (at == 0x6000 || (at >= 0x7000 && at <= 0x700A))) {
		vf->running = (uint8_t) ((vf->running & 1) << 7 | vf->running >> 1) ^ value;
		vf->turns = (vf->turns + 1) & 7;
		unsigned index = at == 0x6000 ? 11 : at & 0xF;
		vf->turnOf[index] = vf->turns;
		if (at == 0x6000) {
			vf->at6000 = vf->running;
		} else {
			vf->at7000[at & 0xF] = vf->running;
		}
		uint8_t other = vf->running ^ vastFameTurned(vf->turns);
		if (!vf->decided && ((at == 0x7000 && (vf->running & 7) < 4 && (other & 7) >= 4)
		                     || (at == 0x7008 && (vf->running & 0xF) != 0xF && (other & 0xF) == 0xF))) {
			vastFameOtherKind(vf);
		}
		if (at == 0x7000) {
			vf->left = 0;
		}
	}
	gbMapRom(gb);
}

// Whether the address is the armed one: bank 0's, or the switchable bank's (hhugboy's bounds, its
// decimal 4000 included).
static bool vastFameWaited(const struct GB* gb, unsigned bank, uint16_t address) {
	return (bank == 0 && address < 0x3FFF) || (bank == gb->cart.romBank && address >= 4000);
}

uint8_t gbCartReadVastFame(struct GB* gb, uint16_t address) {
	struct GBVastFame* vf = &gb->cart.vf;
	if (!vf->decided && !vf->left) {
		// Whichever kind's trigger address the game reads first is the kind this cartridge is.
		unsigned otherBank = vf->at7000[3] ^ vastFameTurned(vf->turnOf[3]);
		unsigned other = (vf->at7000[2] ^ vastFameTurned(vf->turnOf[2])) << 8 | (vf->at7000[1] ^ vastFameTurned(vf->turnOf[1]));
		unsigned mine = vf->at7000[2] << 8 | vf->at7000[1];
		if (address == mine) {
			vf->decided = true;
		} else if (address == other && vastFameWaited(gb, otherBank, address)) {
			vastFameOtherKind(vf);
		}
	}
	unsigned length = vastFameLength(vf);
	if (vastFameWaited(gb, vf->at7000[3], address) && address == (vf->at7000[2] << 8 | vf->at7000[1]) && !vf->left) {
		vf->left = length;
	}
	if (vf->left) {
		--vf->left;
		return vf->at7000[4 + length - vf->left - 1];
	}
	// Bank 0 with its upper part from another bank, while $7008's value ends in F.
	uint16_t from = vf->at7000[10] << 8 | vf->at7000[9];
	if ((vf->at7000[8] & 0xF) == 0xF && address < 0x4000 && address >= from) {
		return gb->rom[(((size_t) vf->at6000 << 14) & (gb->romSize - 1)) + address];
	}
	return address < 0x4000 ? gb->memMap[address >> 12][address & 0xFFF] : gbCartRomByte(gb, gb->memMap[address >> 12][address & 0xFFF]);
}

// An unlicensed variant's writes, before MBC5 sees them: false when MBC5 doesn't.
static bool variantWrite(struct GB* gb, uint16_t address, uint8_t* value) {
	struct GBCart* cart = &gb->cart;
	switch (cart->variant) {
	case GB_SINTAX:
		if (address >= 0x2000 && address < 0x3000) {
			cart->bankWritten = *value;
			*value = reorder(*value, sintaxOrders[cart->bankOrder]);
		} else if ((address & 0xF0F0) == 0x5010) {
			// The bank order, then the bank written again in it.
			cart->bankOrder = *value & 0xF;
			gbCartWrite(gb, 0x2000, cart->bankWritten);
			return false;
		} else if (address >= 0x7000 && (address & 0xF0) >= 0x20 && (address & 0xF0) <= 0x50) {
			cart->xorKeys[((address & 0xF0) >> 4) - 2] = *value;
		}
		romScramble(cart);
		return true;
	case GB_BBD:
	case GB_HITEK:
		switch (address & 0xF0FF) {
		case 0x2000:
			*value = reorder(*value, (cart->variant == GB_BBD ? bbdBanks : hitekBanks)[cart->bankOrder]);
			break;
		case 0x2001:
			cart->dataOrder = *value & 7;
			break;
		case 0x2080:
			cart->bankOrder = *value & 7;
			break;
		}
		romScramble(cart);
		return true;
	case GB_GGB81:
		if ((address & 0xF0FF) == 0x2001) {
			cart->dataOrder = *value & 7;
		}
		romScramble(cart);
		return true;
	case GB_LI_CHENG:
		return !(address > 0x2100 && address < 0x3000);
	case GB_VAST_FAME:
		if (address >= 0x6000) {
			vastFameWrite(gb, address, *value);
			return false;
		}
		if (!cart->vf.opened && (address & 0xF0FF) == 0x2001) {
			cart->vf.dataMode = *value & 7;
			romScramble(cart);
		}
		return true;
	default:
		return true;
	}
}

bool gbCartInit(struct GB* gb) {
	struct GBCart* cart = &gb->cart;
	const uint8_t* rom = gb->rom;
	memset(cart, 0, sizeof(*cart));
	cart->rtc.lastSync = gb->now;
	cart->rtc.lastHost = gb->hostMicros;
	uint8_t type = rom[0x147];
	cart->variant = gb->megaDuck ? GB_PLAIN : variantOf(gb);
	if (cart->variant == GB_HITEK) {
		cart->bankOrder = cart->dataOrder = 7;
	} else if (cart->variant == GB_SINTAX) {
		cart->bankOrder = 0xF;
	}
	romScramble(cart);
	cart->vf.opened = cart->variant == GB_VAST_FAME && !vastFameMaybeGgb81(gb);
	// Cartridges told by more than their header (armGBA's order): Wisdom Tree's, the variants above,
	// NT's, Sachen's, MMM01's.
	cart->romBanks = (uint32_t) (gb->romSize / 0x4000) ? (uint32_t) (gb->romSize / 0x4000) : 1;
	uint8_t special = gb->megaDuck ? GB_MEGADUCK : cart->variant != GB_PLAIN ? GB_MBC5 : isWisdomTree(gb) ? GB_WISDOM_TREE : ntMapper(gb) ? ntMapper(gb)
	                : isSachen(gb) ? GB_SACHEN : mmm01Header(gb) ? GB_MMM01 : 0;
	const uint8_t* header = special == GB_MMM01 ? mmm01Header(gb) : rom;
	if (special == GB_MMM01) {
		type = header[0x147];
	}
	switch (special ? 0x100 : type) {
	case 0x100:
		cart->mapper = special;
		break;
	case 0x00:
	case 0x08:
	case 0x09:
		cart->mapper = GB_MBC_NONE;
		break;
	case 0x01:
	case 0x02:
	case 0x03:
		cart->mapper = GB_MBC1;
		break;
	case 0x05:
	case 0x06:
		cart->mapper = GB_MBC2;
		break;
	case 0x0F:
	case 0x10:
	case 0x11:
	case 0x12:
	case 0x13:
		cart->mapper = GB_MBC3;
		cart->hasRtc = type <= 0x10;
		break;
	case 0x19:
	case 0x1A:
	case 0x1B:
	case 0x1C:
	case 0x1D:
	case 0x1E:
		cart->mapper = GB_MBC5;
		break;
	case 0xFF:
		cart->mapper = GB_HUC1;
		break;
	case 0xFC:
		cart->mapper = GB_CAMERA;
		break;
	case 0xFD:
		cart->mapper = GB_TAMA5;
		break;
	case 0xFE:
		cart->mapper = GB_HUC3;
		cart->hasRtc = true;
		break;
	case 0x22:
		cart->mapper = GB_MBC7;
		cart->mbc7.x = cart->mbc7.y = 0x8000;
		cart->mbc7.readBits = 0xFFFF;
		break;
	default:
		// Prototypes and unlicensed games with nothing sensible here are guessed at below.
		cart->mapper = GB_MBC_NONE;
		break;
	}
	cart->hasBattery = type == 0x03 || type == 0x06 || type == 0x09 || type == 0x0D || type == 0x0F || type == 0x10 || type == 0x13 ||
	                   type == 0x1B || type == 0x1E || type == 0xFC || type == 0xFE || type == 0xFF;
	cart->ramSize = header[0x149] < 6 ? ramSizes[header[0x149]] : 0;
	// armGBA's guesses for headers that don't hold: no mapper over more than 32 KiB, or HuC1 with a ROM
	// size that is none (unlicensed multicarts), is the commonest mapper, though not the Doctor GB flash
	// cartridge's menu, whose hardware is its own.
	unsigned banks = cart->romBanks < 2 ? 2 : cart->romBanks;
	if ((cart->mapper == GB_MBC_NONE && !special && banks > 2 && type != 0x08 && type != 0x09 && memcmp(rom + 0x134, "GB16M", 6))
	    || (cart->mapper == GB_HUC1 && rom[0x148] > 0x08)) {
		cart->mapper = banks > 2 ? GB_MBC1 : GB_MBC_NONE;
	}
	// The Telefang pirates sold as Pokemon Jade and Diamond: Telefang's title on an MBC3 header that
	// gives a tenth of the ROM's size.
	if (cart->mapper == GB_MBC3 && !memcmp(rom + 0x134, "TELEFANG", 8) && rom[0x148] < 5) {
		cart->mapper = GB_PKJD;
	}
	// Prototypes of Zelda DX were built for MBC5, as the game shipped, under a header that still says
	// MBC1: they write whole bank numbers past 31 to one register.
	if (cart->mapper == GB_MBC1 && banks == 64 && (rom[0x143] & 0x80) && !memcmp(rom + 0x134, "ZELDA", 6)) {
		cart->mapper = GB_MBC5;
	}
	// MBC1 can't address more than 512 KiB of ROM along with more than 8 KiB of RAM; games that claim
	// both, or 2 MiB under a header giving a small ROM's size, are on something like MBC3.
	if (cart->mapper == GB_MBC1 && banks > 32 && (cart->ramSize > 0x2000 || (rom[0x148] < 8 && (2u << rom[0x148]) < banks))) {
		cart->mapper = GB_MBC3;
	}
	cart->mbc30 = cart->mapper == GB_MBC3 && (banks > 0x80 || cart->ramSize > 0x8000);
	if (cart->mapper == GB_MBC2) {
		cart->ramSize = 0x200;
	}
	// TAMA5: its chip and clock in one 8 KiB bank, always reachable.
	if (cart->mapper == GB_TAMA5) {
		cart->ramSize = 0x2000;
		cart->hasRam = cart->hasBattery = true;
	}
	// MBC7's EEPROM: 128 16-bit words, kept as the save.
	if (cart->mapper == GB_MBC7) {
		cart->ramSize = 0x100;
		cart->hasBattery = true;
	}
	// A Mega Duck cartridge has no header. A memory card's RAM (32 KiB, 128 KiB on MD25S) is kept as a
	// battery-backed cartridge's is.
	if (cart->mapper == GB_MEGADUCK) {
		cart->duckKind = gb->megaDuck - 1;
		bool ram = cart->duckKind == GB_DUCK_MD0 || cart->duckKind == GB_DUCK_MD20S || cart->duckKind == GB_DUCK_MD25S;
		cart->ramSize = !ram ? 0 : cart->duckKind == GB_DUCK_MD25S ? 0x20000 : 0x8000;
		cart->hasBattery = ram;
	}
	cart->hasRam = cart->ramSize != 0;

	// Sintax's cartridge RAM needs no enabling (mGBA), nor HuC1's (SameBoy), nor a Mega Duck's.
	if (cart->variant == GB_SINTAX || cart->mapper == GB_HUC1 || cart->mapper == GB_MEGADUCK) {
		cart->ramEnabled = cart->hasRam;
	}
	cart->romBank = 1;
	if (cart->mapper == GB_TAMA5) {
		cart->romBank = gbTama5Bank(gb);
	}
	gbMapperInit(gb);
	// A 1 MiB MBC1 cartridge with a second header at bank $10 is a multicart.
	if (cart->mapper == GB_MBC1 && gb->romSize == 0x100000 && !memcmp(rom + 0x104, rom + 0x40104, 0x30)) {
		cart->mbc1Multicart = true;
	}
	return true;
}

static void mbc1Banks(struct GB* gb) {
	struct GBCart* cart = &gb->cart;
	uint8_t low = cart->mbc1Low;
	low = low ? low : 1;
	cart->romBank = cart->mbc1Multicart ? (cart->mbc1Upper << 4) | (low & 0xF) : (cart->mbc1Upper << 5) | low;
	cart->ramBank = cart->mbc1Mode ? cart->mbc1Upper : 0;
}

void gbCartWrite(struct GB* gb, uint16_t address, uint8_t value) {
	struct GBCart* cart = &gb->cart;
	uint16_t romBank = cart->romBank;
	uint8_t mode = cart->mbc1Mode, upper = cart->mbc1Upper;
	if (cart->mapper >= GB_SACHEN) {
		gbMapperWrite(gb, address, value);
		gbMapRom(gb);
		return;
	}
	if (cart->variant != GB_PLAIN) {
		bool mbc5 = variantWrite(gb, address, &value);
		// Its bytes may read differently now.
		gbMapRom(gb);
		if (!mbc5) {
			return;
		}
	}
	switch (cart->mapper) {
	case GB_MBC1:
		switch (address >> 13) {
		case 0:
			cart->ramEnabled = (value & 0xF) == 0xA;
			break;
		case 1:
			cart->mbc1Low = value & 0x1F;
			mbc1Banks(gb);
			break;
		case 2:
			cart->mbc1Upper = value & 3;
			mbc1Banks(gb);
			break;
		default:
			cart->mbc1Mode = value & 1;
			mbc1Banks(gb);
			break;
		}
		break;
	case GB_MBC2:
		if (address < 0x4000) {
			if (address & 0x100) {
				cart->romBank = value & 0xF ? value & 0xF : 1;
			} else {
				cart->ramEnabled = (value & 0xF) == 0xA;
			}
		}
		break;
	case GB_MBC3:
		switch (address >> 13) {
		case 0:
			cart->ramEnabled = (value & 0xF) == 0xA;
			break;
		case 1:
			cart->romBank = value & (cart->mbc30 ? 0xFF : 0x7F) ? value & (cart->mbc30 ? 0xFF : 0x7F) : 1;
			break;
		case 2:
			cart->ramBank = value;
			break;
		default:
			// Any write latches the clock (SameBoy's MBC3).
			if (cart->hasRtc) {
				gbRtcSync(gb);
				memcpy(cart->rtc.latched, cart->rtc.regs, sizeof(cart->rtc.latched));
			}
			break;
		}
		break;
	case GB_MBC7:
		switch (address >> 12) {
		case 0:
		case 1:
			cart->ramEnabled = value == 0x0A;
			break;
		case 2:
		case 3:
			cart->romBank = value;
			break;
		case 4:
		case 5:
			cart->mbc7.secondary = value == 0x40;
			break;
		default:
			break;
		}
		break;
	case GB_CAMERA:
		switch (address >> 13) {
		case 0:
			cart->ramEnabled = (value & 0xF) == 0xA;
			break;
		case 1:
			cart->romBank = value;
			break;
		case 2:
			cart->ramBank = value & 0xF;
			cart->camera.mapped = value & 0x10;
			break;
		default:
			break;
		}
		break;
	case GB_HUC3:
		switch (address >> 13) {
		case 0:
			cart->huc3.mode = value;
			cart->ramEnabled = value == 0x0A;
			break;
		case 1:
			cart->romBank = value;
			break;
		case 2:
			cart->ramBank = value;
			break;
		default:
			break;
		}
		break;
	case GB_HUC1:
		// SameBoy's HuC1: $0E at $0000-$1FFF puts the infrared port where RAM was; 8-bit ROM bank, 0 too.
		switch (address >> 13) {
		case 0:
			cart->irMode = (value & 0xF) == 0xE;
			break;
		case 1:
			cart->romBank = value;
			break;
		case 2:
			cart->ramBank = value;
			break;
		default:
			break;
		}
		break;
	case GB_MBC5:
		switch (address >> 12) {
		case 0:
		case 1:
			cart->ramEnabled = value == 0xA;
			break;
		case 2:
			cart->romBank = (cart->romBank & 0x100) | value;
			break;
		case 3:
			cart->romBank = (cart->romBank & 0xFF) | (value & 1) << 8;
			break;
		case 4:
		case 5:
			cart->ramBank = value & 0xF;
			break;
		default:
			break;
		}
		break;
	default:
		return;
	}
	// Games write the bank they already have all the time.
	if (cart->romBank != romBank || cart->mbc1Mode != mode || cart->mbc1Upper != upper) {
		gbMapRom(gb);
	}
}

static int32_t ramOffset(const struct GB* gb, uint16_t address) {
	const struct GBCart* cart = &gb->cart;
	if (!cart->hasRam || !cart->ramEnabled) {
		return -1;
	}
	if (cart->mapper == GB_MBC2) {
		return address & 0x1FF;
	}
	if (cart->mapper == GB_MBC3 && cart->ramBank > (cart->mbc30 ? 7 : 3)) {
		return -1;
	}
	// MBC6: two 4 KiB halves.
	if (cart->mapper == GB_MBC6) {
		return (cart->ramHalf[address >> 12 & 1] * 0x1000 + (address & 0xFFF)) & (cart->ramSize - 1);
	}
	return (cart->ramBank * 0x2000 + (address & 0x1FFF)) & (cart->ramSize - 1);
}

// MBC3's clock (Pan Docs, "MBC3"; SameBoy): registers 6, 6, 5, 8 and 3 bits wide. A field past its
// range counts up to its last value and wraps to 0 without a carry; past 511 days the day counter wraps
// and sets the carry bit, which stays until written. Writing the seconds restarts the second.
static const uint8_t rtcMasks[5] = { 0x3F, 0x3F, 0x1F, 0xFF, 0xC1 };

static bool rtcSelected(const struct GB* gb) {
	const struct GBCart* cart = &gb->cart;
	return cart->mapper == GB_MBC3 && cart->hasRtc && cart->ramBank >= 0x08 && cart->ramBank <= 0x0C;
}

static void rtcAdvance(struct GBRtc* rtc, uint64_t seconds) {
	static const uint8_t limits[3] = { 60, 60, 24 };
	static const uint8_t ends[3] = { 64, 64, 32 };
	uint64_t carry = seconds;
	for (int i = 0; i < 3 && carry; ++i) {
		uint8_t* value = &rtc->regs[i];
		if (*value >= limits[i]) {
			if (carry < (uint64_t) (ends[i] - *value)) {
				*value += carry;
				return;
			}
			carry -= ends[i] - *value;
			*value = 0;
		}
		uint64_t total = *value + carry;
		*value = total % limits[i];
		carry = total / limits[i];
	}
	if (!carry) {
		return;
	}
	uint64_t days = ((rtc->regs[4] & 1) << 8 | rtc->regs[3]) + carry;
	if (days > 511) {
		rtc->regs[4] |= 0x80;
	}
	rtc->regs[3] = days & 0xFF;
	rtc->regs[4] = (rtc->regs[4] & 0xFE) | (days >> 8 & 1);
}

void gbRtcSync(struct GB* gb) {
	struct GBRtc* rtc = &gb->cart.rtc;
	uint64_t ticks;
	if (gb->rtcRealTime) {
		int64_t micros = gb->hostMicros - rtc->lastHost;
		ticks = micros > 0 ? (uint64_t) micros * GB_RTC_SECOND / 1000000 : 0;
	} else {
		uint64_t parts = (uint64_t) (uint32_t) (gb->now - rtc->lastSync) * gb->rtcPercent + rtc->fraction;
		ticks = parts / 100;
		rtc->fraction = parts % 100;
	}
	rtc->lastSync = gb->now;
	rtc->lastHost = gb->hostMicros;
	// HuC3: minutes up to a day's, then days.
	if (gb->cart.mapper == GB_HUC3) {
		struct GBHuc3* huc3 = &gb->cart.huc3;
		ticks += rtc->ticks;
		rtc->ticks = ticks % (GB_RTC_SECOND * 60ull);
		uint64_t minutes = huc3->minutes + ticks / (GB_RTC_SECOND * 60ull);
		huc3->days += minutes / 1440;
		huc3->minutes = minutes % 1440;
		return;
	}
	if (rtc->regs[4] & 0x40) {
		return;
	}
	ticks += rtc->ticks;
	rtc->ticks = ticks % GB_RTC_SECOND;
	rtcAdvance(rtc, ticks / GB_RTC_SECOND);
}

void gbRtcGet(struct GB* gb, uint32_t r[11]) {
	struct GBRtc* rtc = &gb->cart.rtc;
	gbRtcSync(gb);
	for (int i = 0; i < 5; ++i) {
		r[4 - i] = rtc->regs[i];
		r[10 - i] = rtc->latched[i];
	}
	r[5] = rtc->ticks;
}

void gbRtcSet(struct GB* gb, const uint32_t r[11]) {
	struct GBRtc* rtc = &gb->cart.rtc;
	for (int i = 0; i < 5; ++i) {
		rtc->regs[i] = r[4 - i] & rtcMasks[i];
		rtc->latched[i] = r[10 - i] & rtcMasks[i];
	}
	rtc->ticks = r[5] % GB_RTC_SECOND;
	rtc->fraction = 0;
	rtc->lastSync = gb->now;
	rtc->lastHost = gb->hostMicros;
}

void gbRtcSetSpeed(struct GB* gb, unsigned percent) {
	gbRtcSync(gb);
	gb->rtcPercent = percent;
	gb->rtcSpeedSet = true;
}

void gbRtcSetRealTime(struct GB* gb, bool on) {
	gbRtcSync(gb);
	gb->rtcRealTime = on;
	gb->cart.rtc.lastHost = gb->hostMicros;
}

// MBC7 (SameBoy's): $Ax0x then $Ax1x latch the accelerometer, read at $Ax2x-$Ax5x; $Ax8x is the EEPROM's
// pins (DO bit 0, DI bit 1, CLK bit 6, CS bit 7), a command clocked in on each rising CLK with CS high.
static uint8_t mbc7Read(struct GB* gb, uint16_t address) {
	const struct GBMbc7* m = &gb->cart.mbc7;
	if (!gb->cart.ramEnabled || !m->secondary || address >= 0xB000) {
		return 0xFF;
	}
	switch (address >> 4 & 0xF) {
	case 2:
		return m->x;
	case 3:
		return m->x >> 8;
	case 4:
		return m->y;
	case 5:
		return m->y >> 8;
	case 6:
		return 0;
	case 8:
		return m->dout | m->di << 1 | m->clk << 6 | m->cs << 7;
	default:
		return 0xFF;
	}
}

static uint16_t eepromWord(const struct GB* gb, unsigned word) {
	return gb->sram[word * 2] | gb->sram[word * 2 + 1] << 8;
}

static void setEepromWord(struct GB* gb, unsigned word, uint16_t value) {
	gb->sram[word * 2] = value;
	gb->sram[word * 2 + 1] = value >> 8;
}

static void eepromCommand(struct GB* gb) {
	struct GBMbc7* m = &gb->cart.mbc7;
	unsigned word = m->command & 0x7F;
	switch (m->command >> 6 & 0xF) {
	case 0x8:
	case 0x9:
	case 0xA:
	case 0xB: // READ
		m->readBits = eepromWord(gb, word);
		m->command = 0;
		break;
	case 0x3: // EWEN
		m->writeEnabled = true;
		m->command = 0;
		break;
	case 0x0: // EWDS
		m->writeEnabled = false;
		m->command = 0;
		break;
	case 0x4:
	case 0x5:
	case 0x6:
	case 0x7: // WRITE: the word cleared, its 16 bits to come
		if (m->writeEnabled) {
			setEepromWord(gb, word, 0);
		}
		m->argumentBits = 16;
		break;
	case 0xC:
	case 0xD:
	case 0xE:
	case 0xF: // ERASE
		if (m->writeEnabled) {
			setEepromWord(gb, word, 0xFFFF);
			m->readBits = 0x3FFF;
		}
		m->command = 0;
		break;
	case 0x2: // ERAL
		if (m->writeEnabled) {
			memset(gb->sram, 0xFF, 0x100);
			m->readBits = 0xFF;
		}
		m->command = 0;
		break;
	case 0x1: // WRAL: everything cleared, 16 bits to come
		if (m->writeEnabled) {
			memset(gb->sram, 0, 0x100);
		}
		m->argumentBits = 16;
		break;
	}
}

static void mbc7Write(struct GB* gb, uint16_t address, uint8_t value) {
	struct GBMbc7* m = &gb->cart.mbc7;
	if (!gb->cart.ramEnabled || !m->secondary || address >= 0xB000) {
		return;
	}
	switch (address >> 4 & 0xF) {
	case 0:
		if (value == 0x55) {
			m->x = m->y = 0x8000;
		}
		break;
	case 1:
		if (value == 0xAA) {
			m->x = 0x81D0 + gb->tiltX;
			m->y = 0x81D0 + gb->tiltY - (gb->flick ? 0xE0 : 0);
			gb->flick = false;
		}
		break;
	case 8:
		m->cs = value & 0x80;
		m->di = value & 2;
		if (m->cs && !m->clk && (value & 0x40)) {
			m->dout = m->readBits >> 15;
			m->readBits = m->readBits << 1 | 1;
			if (!m->argumentBits) {
				m->command = m->command << 1 | m->di;
				if (m->command & 0x400) {
					eepromCommand(gb);
				}
			} else {
				--m->argumentBits;
				m->dout = true;
				if (m->di) {
					uint16_t bit = 1 << m->argumentBits;
					if (m->command & 0x100) {
						setEepromWord(gb, m->command & 0x7F, eepromWord(gb, m->command & 0x7F) | bit);
					} else {
						for (unsigned i = 0; i < 0x7F; ++i) {
							setEepromWord(gb, i, eepromWord(gb, i) | bit);
						}
					}
				}
				if (!m->argumentBits) {
					m->command = 0;
					m->readBits = 0x3FFF;
				}
			}
		}
		m->clk = value & 0x40;
		break;
	default:
		break;
	}
}

// HuC3 (SameBoy's): mode $B takes commands at $Axxx (high nibble 1: read the nibble at the index and
// step on, 2: write it, 3: write and step on, 4 and 5: the index's low and high nibbles, 6: flags);
// $C reads the nibble read, $D the status, $E the infrared port. Nibbles 0-2 are the minutes, 3-6 the
// days, $58-$5F the alarm.
static void huc3SetNibble(uint16_t* field, unsigned nibble, uint8_t value) {
	*field = (*field & ~(0xF << nibble * 4)) | (value & 0xF) << nibble * 4;
}

static bool huc3Write(struct GB* gb, uint8_t value) {
	struct GBHuc3* h = &gb->cart.huc3;
	switch (h->mode) {
	case 0xB:
		gbRtcSync(gb);
		switch (value >> 4) {
		case 1:
			if (h->index < 3) {
				h->nibble = h->minutes >> h->index * 4 & 0xF;
			} else if (h->index < 7) {
				h->nibble = h->days >> (h->index - 3) * 4 & 0xF;
			}
			++h->index;
			break;
		case 2:
		case 3:
			if (h->index < 3) {
				huc3SetNibble(&h->minutes, h->index, value);
			} else if (h->index < 7) {
				huc3SetNibble(&h->days, h->index - 3, value);
			} else if (h->index >= 0x58 && h->index <= 0x5A) {
				huc3SetNibble(&h->alarmMinutes, h->index - 0x58, value);
			} else if (h->index >= 0x5B && h->index <= 0x5E) {
				huc3SetNibble(&h->alarmDays, h->index - 0x5B, value);
			} else if (h->index == 0x5F) {
				h->alarmOn = value & 1;
			}
			if (value >> 4 == 3) {
				++h->index;
			}
			break;
		case 4:
			h->index = (h->index & 0xF0) | (value & 0xF);
			break;
		case 5:
			h->index = (h->index & 0x0F) | (value & 0xF) << 4;
			break;
		case 6:
			h->flags = value & 0xF;
			break;
		default:
			break;
		}
		return true;
	case 0xC:
	case 0xD:
	case 0xE:
		return true;
	default:
		return false;
	}
}

static bool huc3Read(const struct GB* gb, uint8_t* value) {
	const struct GBHuc3* h = &gb->cart.huc3;
	switch (h->mode) {
	case 0x0:
	case 0xA:
		return false;
	case 0xC:
		*value = h->flags == 2 ? 1 : h->nibble;
		return true;
	case 0xE:
		// The infrared port: no light seen.
		*value = 0;
		return true;
	default:
		*value = 1;
		return true;
	}
}

static uint32_t huc3Nibbles(const uint8_t* at) {
	return (at[0] & 0xF) | (at[1] & 0xF) << 4 | (at[2] & 0xF) << 8;
}

void gbHuc3Get(struct GB* gb, uint8_t state[GB_HUC3_STATE]) {
	const struct GBHuc3* h = &gb->cart.huc3;
	gbRtcSync(gb);
	memset(state, 0, GB_HUC3_STATE);
	uint32_t ticks = gb->cart.rtc.ticks;
	for (int i = 0; i < 4; ++i) {
		state[i] = ticks >> (24 - i * 8);
	}
	for (int i = 0; i < 3; ++i) {
		state[4 + 0x10 + i] = h->minutes >> i * 4 & 0xF;
		state[4 + 0x13 + i] = h->days >> i * 4 & 0xF;
		state[4 + 0x58 + i] = h->alarmMinutes >> i * 4 & 0xF;
	}
	for (int i = 0; i < 4; ++i) {
		state[4 + 0x5B + i] = h->alarmDays >> i * 4 & 0xF;
	}
	state[4 + 0x5F] = h->alarmOn;
}

void gbHuc3Set(struct GB* gb, const uint8_t state[GB_HUC3_STATE]) {
	struct GBHuc3* h = &gb->cart.huc3;
	struct GBRtc* rtc = &gb->cart.rtc;
	rtc->ticks = ((uint32_t) state[0] << 24 | state[1] << 16 | state[2] << 8 | state[3]) % (GB_RTC_SECOND * 60u);
	h->minutes = huc3Nibbles(&state[4 + 0x10]) % 1440;
	h->days = huc3Nibbles(&state[4 + 0x13]);
	h->alarmMinutes = huc3Nibbles(&state[4 + 0x58]);
	h->alarmDays = huc3Nibbles(&state[4 + 0x5B]) | (state[4 + 0x5E] & 0xF) << 12;
	h->alarmOn = state[4 + 0x5F] & 1;
	rtc->fraction = 0;
	rtc->lastSync = gb->now;
	rtc->lastHost = gb->hostMicros;
}

// The Pocket Camera (SameBoy's camera.c): register 0 starts a shot (bit 0, busy until it's done), the
// others set gain, exposure, edge enhancement and the 4x4 dithering thresholds; RAM bank 0 at
// $A100-$AEFF reads the picture as 16x14 tiles. RAM can't be reached during a shot.
static bool cameraBusy(struct GB* gb) {
	struct GBCamera* camera = &gb->cart.camera;
	if ((camera->regs[0] & 1) && gbSince(gb->now, camera->busyUntil) >= 0) {
		camera->regs[0] &= ~1;
	}
	return camera->regs[0] & 1;
}

static uint8_t cameraNoise(const struct GBCamera* camera, uint8_t x, uint8_t y) {
	uint32_t value = (x * 151 + y * 149) ^ camera->seed;
	uint32_t hash = 0;
	while (value) {
		hash <<= 1;
		if (hash & 0x100) {
			hash ^= 0x101;
		}
		if (value & 0x80000000) {
			hash ^= 0xA1;
		}
		value <<= 1;
	}
	return hash;
}

static long cameraPixel(const struct GBCamera* camera, int x, int y) {
	static const double gains[32] = {
		0.8809390, 0.9149149, 0.9457498, 0.9739758, 1.0000000, 1.0241412, 1.0466537, 1.0677433,
		1.0875793, 1.1240310, 1.1568911, 1.1868043, 1.2142561, 1.2396208, 1.2743837, 1.3157323,
		1.3525190, 1.3856512, 1.4157897, 1.4434309, 1.4689574, 1.4926697, 1.5148087, 1.5355703,
		1.5551159, 1.5735801, 1.5910762, 1.6077008, 1.6235366, 1.6386550, 1.6531183, 1.6669808,
	};
	x = x == 128 ? 127 : x > 128 || x < 0 ? 0 : x;
	y = y == 112 ? 111 : y > 112 || y < 0 ? 0 : y;
	long color = cameraNoise(camera, x, y);
	color = (long) (color * gains[camera->regs[1] & 0x1F]);
	return color * ((camera->regs[2] << 8) + camera->regs[3]) / 0x1000;
}

static uint8_t cameraImage(const struct GBCamera* camera, uint16_t offset) {
	static const double edges[8] = { 0.5, 0.75, 1, 1.25, 2, 3, 4, 5 };
	int tileX = offset / 0x10 % 0x10;
	int y = (offset >> 1 & 7) + offset / 0x100 * 8;
	int bit = offset & 1;
	uint8_t result = 0;
	for (int x = tileX * 8; x < tileX * 8 + 8; ++x) {
		long color = cameraPixel(camera, x, y);
		if ((camera->regs[1] & 0xE0) == 0xE0) {
			double edge = edges[camera->regs[4] >> 4 & 7];
			color += (long) (color * 4 * edge);
			color -= (long) (cameraPixel(camera, x - 1, y) * edge);
			color -= (long) (cameraPixel(camera, x + 1, y) * edge);
			color -= (long) (cameraPixel(camera, x, y - 1) * edge);
			color -= (long) (cameraPixel(camera, x, y + 1) * edge);
		}
		const uint8_t* thresholds = &camera->regs[6 + ((x & 3) + (y & 3) * 4) * 3];
		int shade = color < thresholds[0] ? 3 : color < thresholds[1] ? 2 : color < thresholds[2] ? 1 : 0;
		result = result << 1 | (shade >> bit & 1);
	}
	return result;
}

static void cameraWrite(struct GB* gb, uint16_t address, uint8_t value) {
	struct GBCamera* camera = &gb->cart.camera;
	unsigned reg = address & 0x7F;
	if (reg != 0) {
		if (reg < 0x36) {
			camera->regs[reg] = value;
		}
		return;
	}
	value &= 7;
	bool busy = cameraBusy(gb);
	// A new sensor reading each time, its noise from a counter (SameBoy draws a random one).
	camera->seed = camera->seed * 1103515245 + 12345;
	if ((value & 1) && !busy) {
		// SameBoy's time for a shot, in CPU cycles: as dots, halved at double speed.
		uint32_t exposure = camera->regs[2] << 8 | camera->regs[3];
		uint32_t cycles = 129792 + (camera->regs[1] & 0x80 ? 0 : 2048) + exposure * 64;
		camera->busyUntil = gb->now + (gb->doubleSpeed ? cycles / 2 : cycles);
	}
	// A shot can't be stopped.
	if (busy) {
		value |= 1;
	}
	camera->regs[0] = value;
}

uint8_t gbCartReadRam(struct GB* gb, uint16_t address) {
	uint8_t cheat;
	if (gb->cheatCart && gbCheatCartReadRam(gb, address, &cheat)) {
		return cheat;
	}
	if (gb->cart.mapper == GB_TAMA5) {
		return gbTama5Read(gb, address);
	}
	if (gb->cart.mapper == GB_CAMERA) {
		if (gb->cart.camera.mapped) {
			return (address & 0x7F) == 0 ? (cameraBusy(gb), gb->cart.camera.regs[0]) : 0;
		}
		if (!gb->cart.ramEnabled) {
			return 0xFF;
		}
		if (cameraBusy(gb)) {
			return 0;
		}
		if (gb->cart.ramBank == 0 && address >= 0xA100 && address < 0xAF00) {
			return cameraImage(&gb->cart.camera, address - 0xA100);
		}
	}
	if (gb->cart.mapper == GB_MBC7) {
		return mbc7Read(gb, address);
	}
	uint8_t value;
	if (gb->cart.mapper == GB_PKJD && gb->cart.ramEnabled && gbPkjdRead(gb, &value)) {
		return value;
	}
	if (gb->cart.mapper == GB_HUC3) {
		if (huc3Read(gb, &value)) {
			return value;
		}
		// Mode 0 still reads RAM.
		if (gb->cart.huc3.mode == 0 && gb->cart.hasRam) {
			return gb->sram[(gb->cart.ramBank * 0x2000 + (address & 0x1FFF)) & (gb->cart.ramSize - 1)];
		}
	}
	// HuC1's infrared port: no light seen.
	if (gb->cart.mapper == GB_HUC1 && gb->cart.irMode) {
		return 0xC0;
	}
	if (rtcSelected(gb)) {
		return gb->cart.ramEnabled ? gb->cart.rtc.latched[gb->cart.ramBank - 0x08] : 0xFF;
	}
	int32_t offset = ramOffset(gb, address);
	if (offset < 0) {
		return 0xFF;
	}
	if (gb->cart.mapper == GB_MBC2) {
		return gb->sram[offset] | 0xF0;
	}
	return gb->sram[offset];
}

void gbCartWriteRam(struct GB* gb, uint16_t address, uint8_t value) {
	if (gb->cheatCart && gbCheatCartWriteRam(gb, address, value)) {
		return;
	}
	if (gb->cart.mapper == GB_MEGADUCK && !gb->cart.hasRam) {
		gbMegaDuckWrite(gb, address, value);
		return;
	}
	if (gb->cart.mapper == GB_TAMA5) {
		gbTama5Write(gb, address, value);
		return;
	}
	if (gb->cart.mapper == GB_CAMERA) {
		if (gb->cart.camera.mapped) {
			cameraWrite(gb, address, value);
			return;
		}
		if (cameraBusy(gb)) {
			return;
		}
	}
	if (gb->cart.mapper == GB_MBC7) {
		mbc7Write(gb, address, value);
		return;
	}
	if (gb->cart.mapper == GB_HUC3 && huc3Write(gb, value)) {
		return;
	}
	if (gb->cart.mapper == GB_PKJD && gb->cart.ramEnabled && gbPkjdWrite(gb, value)) {
		return;
	}
	// HuC1's infrared LED: nothing to see it.
	if (gb->cart.mapper == GB_HUC1 && gb->cart.irMode) {
		return;
	}
	if (rtcSelected(gb)) {
		if (gb->cart.ramEnabled) {
			struct GBRtc* rtc = &gb->cart.rtc;
			unsigned reg = gb->cart.ramBank - 0x08;
			gbRtcSync(gb);
			rtc->regs[reg] = value & rtcMasks[reg];
			if (reg == 0) {
				rtc->ticks = 0;
				rtc->fraction = 0;
			}
		}
		return;
	}
	int32_t offset = ramOffset(gb, address);
	if (offset >= 0) {
		gb->sram[offset] = gb->cart.mapper == GB_MBC2 ? value & 0xF : value;
	}
}
