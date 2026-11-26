/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Licensed under the GNU General Public License, version 2 or (at your option) any later version: see LICENSE. */
#include "gb.h"

#include <stdlib.h>
#include <string.h>

// Cheat cartridges in front of the game on top of them: the console sees one or the other, the cheat
// cartridge at power on. Each answers reads of the ROM area itself and passes the game's mapper what it
// lets through; from power on until its program first writes a register, the header (0104-014F) is the
// game's, for the boot ROM's check.
//
// Datel's Action Replay, as its programs use it (nothing about its hardware is published). Its switch,
// when off, leaves the game running alone: the game's accesses no longer reach its registers or set off
// its hook, except while the later cartridges' button is held, which still opens their menu.
//
// The later cartridges (Action Replay Online and Xtreme, GameShark Online, Action Replay Pro):
// - Showing the Action Replay: 0000-3FFF is the first 16 KiB of its flash chip, 4000-5FFF the 8 KiB
//   bank 7FE1 picks, 6000-6FFF the 4 KiB bank of its 32 KiB of RAM that 7FE0 picks, and 7000-7FFF
//   the RAM's first 4 KiB, where its registers are (7FE0-7FEF). 7FEE reads its button (bit 0, clear
//   while pressed) and switch (bit 4, set while off).
// - An access to 0040 swaps what shows, while 7FE2 bit 0 or 7FE5 bit 4 is set. A game runs with
//   7FE2 set: the VBlank interrupt's fetch from 0040 shows the Action Replay, whose 0040 jumps to its
//   handler in RAM; that applies the codes (compiled into RAM by the program) and jumps to 0040,
//   which shows the game again at the game's own vector.
// - While the Action Replay shows, 7FE5 bit 0 lets writes through to the game's own mapper too, and
//   while 7FE5 bit 4 is set the RAM's first 4 KiB shows at 7000-7FFF over the game, for a program
//   that looks at the game from there.
// - The flash chip takes the 8-bit commands at 5555 and 2AAA of its address space: ID (90), byte
//   program (A0), and erase (80, then 30 for a 4 KiB sector or 10 for all of it).
// - 7FF2 and 7FF5 arm the hook and show the RAM over the game too (bit 0, bit 4), apart from 7FE2 and
//   7FE5: the later programs read a byte of the game with them, turning 7FE2 and 7FE5 off while the
//   game shows, then swap back with them and return by their stack at 76xx. They're kept in the RAM
//   there, where the program sees what it wrote.
// - With 7FE2 bit 4 set, the game's byte at the address in 7FE7 (high) and 7FE6 (low) reads FF: the
//   program sets it on a game's check that VBlank still has time to copy tiles, which the hook's delay
//   to the game's VBlank handler would otherwise fail.
//
// The first cartridge (Pro Action Replay), a 32 KiB ROM with 4 KiB of RAM at B000-BFFF that shows over
// the game's RAM there too while the switch is on, its codes kept by a battery:
// - A write to A900 shows the game and sets the hook (the VBlank interrupt's fetch from 0040 shows
//   the Action Replay, which jumps to its handler at B080); a write to A800 shows the game again
//   from there, or before a game runs, shows the game's first 512 bytes (its vectors and header)
//   for the program to look at.
//
// Blaze's Xploder GB (Future Console Design), as its program uses it:
// - Showing the Xploder: 0000-3FFF is the first 16 KiB of its flash chip, or with 0000 bit 0 set, its
//   RAM's first 8 KiB at 0000-1FFF (the program builds its hook there); with 0001 clear, the game's
//   first 16 KiB, for the program to read its header. 4000-7FFF is the 16 KiB bank of the flash chip
//   0006 picks. A000-BFFF is the 8 KiB bank of its 128 KiB of RAM 0007 picks, or with 0003 clear the
//   game's own RAM. Its registers are written at 0000-0007, and with 0002 clear its other writes reach
//   the game's mapper too; 0000 reads its button in bit 1 (set while pressed).
// - With 0005 set, an access to 0040 shows the Xploder, and from the Xploder, one to 0040 or 0100 shows
//   the game. A game runs so: the VBlank interrupt's fetch from 0040 runs the Xploder's hook from its
//   RAM, which writes the codes to the game's RAM, opens its menu while the button is held, and jumps
//   to 0040, which shows the game again at the game's own vector. The program starts the game by
//   jumping to 0100.
// - The flash chip (an Atmel AT29C020, 256 KiB) takes the 8-bit commands at 5555 and 2AAA of its
//   address space: ID (90, F0 to leave) and the write of a 256-byte page (A0, then the page's bytes;
//   the page is erased first, so those not written read FF). It holds the program, then from 20000
//   the cheats: for each game its name, its number of cheats, and for each cheat its name, its number
//   of codes and the codes, 4 bytes each; an empty name ends the list.
//
// Galoob's Game Genie, as its program (8 KiB) uses it: its program at power on and the game's once it
// starts it, with up to three of the game's bytes replaced.
// - Registers at 4000-4013, written by the program: 4000 says what shows (bit 2 the game, else the
//   program; bit 1 the registers, read back at 4000-4013; bit 0 the game runs, which nothing undoes
//   until the power goes off). 4001 has a bit for each code that's on. The three codes are at 4003,
//   4008 and 400D: an address of the ROM's (low byte, then high), the byte it reads, the byte that
//   replaces it, and whether only that byte is replaced (a code of nine letters, rather than six).
// - The program decodes the codes the player types in (ABC-DEF-GHI) and starts the game at 0100 by
//   writing 6, then 7, to 4000.
// - The codes typed in are kept in work RAM at C140, nine digits each (0-15, 0x10 for none), with how
//   many digits each has in FFC1-FFC3. The program draws its code screen from them at 08D7: codes
//   chosen in the frontend's options go in the slots still empty there, as if typed in.

enum {
	VECTOR = 0x0040,
	ENTRY = 0x0100,

	AR_RAM_BANK = 0x0,
	AR_ROM8 = 0x1,
	AR_ARM = 0x2,
	AR_BUS = 0x5,
	AR_PATCH_LOW = 0x6,
	AR_PATCH_HIGH = 0x7,
	AR_STATUS = 0xE,
	AR_ARM_PATCH = 0x10,
	AR_BUS_GAME_WRITES = 0x01,
	AR_BUS_LOOK = 0x10,
	AR_RAM_START = 0x6000,
	AR_RAM_FIXED = 0x7000,
	AR_RAM_BANK_SIZE = 0x1000,
	AR_REGS = 0x7FE0,
	AR_REGS2 = 0x7FF0,
	AR_SECTOR = 0x1000,
	AR_FIRST_RAM = 0xB000,
	AR_FIRST_SHOW_GAME = 0xA800,
	AR_FIRST_START = 0xA900,
	AR_FIRST_LOW_PAGE = 0x200,

	XP_OVERLAY = 0x0,
	XP_OWN_LOW = 0x1,
	XP_GAME_WRITES = 0x2,
	XP_OWN_RAM = 0x3,
	XP_GAME_BANK = 0x4,
	XP_ARMED = 0x5,
	XP_FLASH_BANK = 0x6,
	XP_RAM_BANK = 0x7,
	XP_REGS = 8,
	XP_BUTTON = 0x02,
	XP_OVERLAY_SIZE = 0x2000,
	XP_RAM_BANK_SIZE = 0x2000,
	XP_FLASH_SIZE = 0x40000,
	XP_CHEATS = 0x20000,
	XP_PAGE = 0x100,

	GG_CONTROL = 0x0,
	GG_CODES_ON = 0x1,
	GG_REGS = 0x4000,
	GG_REGS_COUNT = 0x14,
	GG_RUNNING = 0x01,
	GG_REGISTERS = 0x02,
	GG_GAME = 0x04,
	GG_TYPED = 0x140,
	GG_TYPED_COUNTS = 0x41,
	GG_DIGITS = 9,
	GG_DIGIT_NONE = 0x10,
	GG_DRAW_CODES = 0x08D7,
};

static const uint16_t ggCodeAt[3] = { 0x3, 0x8, 0xD };

static size_t roundUp(size_t size, size_t least) {
	size_t room = least;
	while (room < size) {
		room <<= 1;
	}
	return room;
}

// The first Action Replay's program stores to A800 and A900, where the later ones' store to 7FE0-7FE7
// all through.
static bool arFirst(const uint8_t* flash, size_t size) {
	unsigned found = 0, later = 0;
	for (size_t i = 0; i + 2 < size; ++i) {
		later += flash[i] == 0xEA && flash[i + 2] == 0x7F && flash[i + 1] >= 0xE0 && flash[i + 1] <= 0xE7;
		if (flash[i] == 0xEA && flash[i + 1] == 0x00) {
			found |= flash[i + 2] == 0xA8 ? 1 : flash[i + 2] == 0xA9 ? 2 : 0;
		}
	}
	return found == 3 && later < 16;
}

static bool isXploder(const uint8_t* program, size_t size) {
	static const char name[] = "Future Console Design";
	return size >= 0x150 && size <= XP_FLASH_SIZE && !memcmp(program + 0x104, name, sizeof(name) - 1);
}

static bool isGameGenie(const uint8_t* program, size_t size) {
	static const char name[] = "GAME GENIE";
	if (size < 0x2000 || size > 0x8000) {
		return false;
	}
	for (size_t i = 0; i + sizeof(name) - 1 <= size; ++i) {
		if (!memcmp(program + i, name, sizeof(name) - 1)) {
			return true;
		}
	}
	return false;
}

bool gbCheatCartInit(struct GBCheatCart* cc, const uint8_t* program, size_t size, bool hasGame) {
	memset(cc, 0, sizeof(*cc));
	if (!size || size > 0x80000) {
		return false;
	}
	cc->kind = isGameGenie(program, size) ? GB_CHEAT_GAME_GENIE : isXploder(program, size) ? GB_CHEAT_XPLODER : GB_CHEAT_ACTION_REPLAY;
	cc->first = cc->kind == GB_CHEAT_ACTION_REPLAY && arFirst(program, size);
	cc->flashSize = cc->kind == GB_CHEAT_GAME_GENIE ? roundUp(size, 0x2000) : cc->kind == GB_CHEAT_XPLODER ? XP_FLASH_SIZE : roundUp(size, 0x8000);
	cc->flash = malloc(cc->flashSize);
	cc->factory = malloc(cc->flashSize);
	if (!cc->flash || !cc->factory) {
		gbCheatCartFree(cc);
		return false;
	}
	memset(cc->flash, 0xFF, cc->flashSize);
	memcpy(cc->flash, program, size);
	// The Xploder's cheats start at 20000, where a dump of the program alone has none: an empty list
	// there, which a save of the chip replaces.
	if (cc->kind == GB_CHEAT_XPLODER && size <= XP_CHEATS) {
		cc->flash[XP_CHEATS] = 0;
	}
	memcpy(cc->factory, cc->flash, cc->flashSize);
	cc->hasGame = hasGame;
	cc->switchOn = true;
	memset(cc->s.fill, GG_DIGIT_NONE, sizeof(cc->s.fill));
	return true;
}

void gbCheatCartFree(struct GBCheatCart* cc) {
	free(cc->flash);
	free(cc->factory);
	cc->flash = cc->factory = NULL;
}

// Power on. The first Action Replay's RAM keeps what it held, and the Game Genie the codes chosen.
void gbCheatCartReset(struct GBCheatCart* cc) {
	struct GBCheatState* s = &cc->s;
	uint8_t fill[27], fillCount[3];
	memcpy(fill, s->fill, sizeof(fill));
	memcpy(fillCount, s->fillCount, sizeof(fillCount));
	memset(s->regs, 0, offsetof(struct GBCheatState, ram));
	if (!cc->first) {
		memset(s->ram, 0, sizeof(s->ram));
	}
	s->boot = 1;
	switch (cc->kind) {
	case GB_CHEAT_ACTION_REPLAY:
		s->shown = 1;
		s->regs[AR_RAM_BANK] = 1;
		s->regs[AR_ROM8] = 2;
		break;
	case GB_CHEAT_XPLODER:
		s->shown = 1;
		s->regs[XP_OWN_LOW] = 1;
		s->regs[XP_GAME_WRITES] = 1;
		s->regs[XP_OWN_RAM] = 1;
		s->regs[XP_FLASH_BANK] = 1;
		break;
	case GB_CHEAT_GAME_GENIE:
		memcpy(s->fill, fill, sizeof(fill));
		memcpy(s->fillCount, fillCount, sizeof(fillCount));
		break;
	}
}

static uint8_t gameRead(struct GB* gb, uint16_t address) {
	return gb->cheatCart->hasGame ? gbCartRomRead(gb, address) : 0xFF;
}

static bool header(const struct GBCheatCart* cc, uint16_t address) {
	return cc->s.boot && address >= 0x104 && address < 0x150;
}

// The Action Replay.

static bool arArmBit(const struct GBCheatCart* cc) {
	return (cc->s.regs[AR_ARM] | cc->s.ram[AR_REGS2 - AR_RAM_FIXED + AR_ARM]) & 1;
}

static bool arLooking(const struct GBCheatCart* cc) {
	return (cc->s.regs[AR_BUS] | cc->s.ram[AR_REGS2 - AR_RAM_FIXED + AR_BUS]) & AR_BUS_LOOK;
}

// From the game, the hook works only with the switch on (or, on the later cartridges, while the button
// is held); the program's own look at the game always.
static bool arArmed(const struct GBCheatCart* cc) {
	if (cc->first) {
		return !cc->s.shown && cc->s.regs[AR_ARM] && cc->switchOn;
	}
	return arLooking(cc) || (arArmBit(cc) && (cc->s.shown || cc->switchOn || cc->button));
}

static size_t arFlashAddress(const struct GBCheatCart* cc, uint16_t address) {
	size_t at = address < 0x4000 || cc->first ? address : cc->s.regs[AR_ROM8] * (size_t) 0x2000 + (address - 0x4000);
	return at & (cc->flashSize - 1);
}

// 7000-7FFF is the RAM's first 4 KiB; 6000-6FFF the 4 KiB bank 7FE0 picks.
static uint8_t* arRam(struct GBCheatCart* cc, uint16_t address) {
	unsigned bank = address < AR_RAM_FIXED ? cc->s.regs[AR_RAM_BANK] & 7 : 0;
	return &cc->s.ram[bank * AR_RAM_BANK_SIZE + (address & (AR_RAM_BANK_SIZE - 1))];
}

static uint8_t arRead(struct GB* gb, uint16_t address) {
	struct GBCheatCart* cc = gb->cheatCart;
	struct GBCheatState* s = &cc->s;
	if (cc->first) {
		bool game = !s->shown || (s->lowPage && address < AR_FIRST_LOW_PAGE) || header(cc, address);
		return game ? gameRead(gb, address) : cc->flash[arFlashAddress(cc, address)];
	}
	if (!s->shown && address >= AR_RAM_FIXED && arLooking(cc)) {
		return *arRam(cc, address);
	}
	if (!s->shown && (s->regs[AR_ARM] & AR_ARM_PATCH) && address == (s->regs[AR_PATCH_HIGH] << 8 | s->regs[AR_PATCH_LOW])) {
		return 0xFF;
	}
	if (!s->shown || header(cc, address)) {
		return gameRead(gb, address);
	}
	if (address < AR_RAM_START) {
		return s->flashId && address < 2 ? (address ? 0xB5 : 0xBF) : cc->flash[arFlashAddress(cc, address)];
	}
	if (address >= AR_REGS && address < AR_REGS + 0x10) {
		unsigned reg = address - AR_REGS;
		if (reg == AR_STATUS) {
			return 0xEE | (cc->switchOn ? 0 : 0x10) | (cc->button ? 0 : 0x01);
		}
		return s->regs[reg];
	}
	return *arRam(cc, address);
}

static void arFlashWrite(struct GBCheatCart* cc, size_t at, uint8_t value) {
	struct GBCheatState* s = &cc->s;
	if (s->flashLoad) {
		s->flashLoad = 0;
		uint8_t now = cc->flash[at] & value;
		if (now != cc->flash[at]) {
			cc->flash[at] = now;
			cc->flashChanged = true;
		}
		return;
	}
	if (value == 0xF0) {
		s->flashStep = 0;
		s->flashId = 0;
		return;
	}
	unsigned command = at & 0x7FFF;
	switch (s->flashStep) {
	case 0:
	case 3:
		s->flashStep = command == 0x5555 && value == 0xAA ? s->flashStep + 1 : 0;
		break;
	case 1:
	case 4:
		s->flashStep = command == 0x2AAA && value == 0x55 ? s->flashStep + 1 : 0;
		break;
	case 2:
		s->flashStep = 0;
		if (command != 0x5555) {
			break;
		}
		if (value == 0x90) {
			s->flashId = 1;
		} else if (value == 0xA0) {
			s->flashLoad = 1;
		} else if (value == 0x80) {
			s->flashStep = 3;
		}
		break;
	case 5:
		s->flashStep = 0;
		if (value == 0x30) {
			memset(&cc->flash[at & ~(size_t) (AR_SECTOR - 1)], 0xFF, AR_SECTOR);
			cc->flashChanged = true;
		} else if (value == 0x10 && command == 0x5555) {
			memset(cc->flash, 0xFF, cc->flashSize);
			cc->flashChanged = true;
		}
		break;
	}
}

static void arWrite(struct GB* gb, uint16_t address, uint8_t value) {
	struct GBCheatCart* cc = gb->cheatCart;
	struct GBCheatState* s = &cc->s;
	if (cc->first) {
		if (!s->shown) {
			gbCartWrite(gb, address, value);
		}
		return;
	}
	// The registers take a write from the game only with the switch on, or while the program looks at
	// the game itself.
	bool listening = s->shown || cc->switchOn || arLooking(cc);
	if (address >= AR_REGS && address < AR_REGS + 0x10 && listening) {
		s->regs[address - AR_REGS] = value;
		s->boot = 0;
		return;
	}
	if (address == VECTOR && arArmed(cc)) {
		s->shown ^= 1;
		return;
	}
	if (address >= AR_REGS2 && !s->shown && listening) {
		*arRam(cc, address) = value;
		return;
	}
	bool lookRam = address >= AR_RAM_FIXED && arLooking(cc);
	if (!s->shown && !lookRam) {
		gbCartWrite(gb, address, value);
		return;
	}
	if (s->regs[AR_BUS] & AR_BUS_GAME_WRITES) {
		gbCartWrite(gb, address, value);
	}
	if (address >= AR_RAM_START) {
		*arRam(cc, address) = value;
		return;
	}
	arFlashWrite(cc, arFlashAddress(cc, address), value);
}

// The first cartridge's RAM and registers. With the switch off, the game running has its own RAM there.
static bool arReadRam(struct GBCheatCart* cc, uint16_t address, uint8_t* value) {
	if (!cc->first || address < AR_FIRST_RAM || (!cc->s.shown && !cc->switchOn)) {
		return false;
	}
	*value = cc->s.ram[address & (AR_RAM_BANK_SIZE - 1)];
	return true;
}

static bool arWriteRam(struct GBCheatCart* cc, uint16_t address, uint8_t value) {
	struct GBCheatState* s = &cc->s;
	if (!cc->first || (!s->shown && !cc->switchOn)) {
		return false;
	}
	if (address >= AR_FIRST_RAM) {
		s->ram[address & (AR_RAM_BANK_SIZE - 1)] = value;
		cc->flashChanged = true;
		return true;
	}
	if (address != AR_FIRST_SHOW_GAME && address != AR_FIRST_START) {
		return false;
	}
	s->boot = 0;
	if (address == AR_FIRST_START || s->regs[AR_ARM]) {
		s->regs[AR_ARM] = 1;
		s->lowPage = 0;
		s->shown = 0;
	} else {
		s->lowPage = 1;
	}
	return true;
}

// The Xploder.

static size_t xpFlashAddress(const struct GBCheatCart* cc, uint16_t address) {
	size_t at = address < 0x4000 ? address : cc->s.regs[XP_FLASH_BANK] * (size_t) 0x4000 + (address - 0x4000);
	return at & (cc->flashSize - 1);
}

static uint8_t xpFlashRead(const struct GBCheatCart* cc, size_t at) {
	if (cc->s.flashId && at < 2) {
		return at ? 0xDA : 0x1F;
	}
	return cc->flash[at];
}

static uint8_t xpRead(struct GB* gb, uint16_t address) {
	struct GBCheatCart* cc = gb->cheatCart;
	struct GBCheatState* s = &cc->s;
	if (!s->shown || header(cc, address)) {
		return gameRead(gb, address);
	}
	if (address >= 0x4000) {
		return xpFlashRead(cc, xpFlashAddress(cc, address));
	}
	if (address == XP_OVERLAY) {
		return cc->button ? XP_BUTTON : 0;
	}
	if (address == XP_GAME_BANK) {
		return s->gameBank;
	}
	if (address == XP_FLASH_BANK) {
		return s->regs[XP_FLASH_BANK];
	}
	if (!s->regs[XP_OWN_LOW]) {
		return gameRead(gb, address);
	}
	if ((s->regs[XP_OVERLAY] & 1) && address < XP_OVERLAY_SIZE) {
		return s->ram[address];
	}
	return xpFlashRead(cc, address);
}

static void xpFlashWrite(struct GBCheatCart* cc, size_t at, uint8_t value) {
	struct GBCheatState* s = &cc->s;
	size_t page = at / XP_PAGE;
	if (s->flashLoad == 2 && page != ((size_t) s->pageHigh << 8 | s->pageLow)) {
		s->flashLoad = 0;
	}
	if (s->flashLoad) {
		if (s->flashLoad == 1) {
			s->flashLoad = 2;
			s->pageHigh = page >> 8;
			s->pageLow = page & 0xFF;
			memset(&cc->flash[page * XP_PAGE], 0xFF, XP_PAGE);
		}
		cc->flash[at] = value;
		cc->flashChanged = true;
		return;
	}
	unsigned command = at & 0x7FFF;
	switch (s->flashStep) {
	case 0:
		s->flashStep = command == 0x5555 && value == 0xAA;
		break;
	case 1:
		s->flashStep = command == 0x2AAA && value == 0x55 ? 2 : 0;
		break;
	case 2:
		s->flashStep = 0;
		if (command != 0x5555) {
			break;
		}
		if (value == 0x90) {
			s->flashId = 1;
		} else if (value == 0xF0) {
			s->flashId = 0;
		} else if (value == 0xA0) {
			s->flashLoad = 1;
		}
		break;
	}
}

static void xpWrite(struct GB* gb, uint16_t address, uint8_t value) {
	struct GBCheatCart* cc = gb->cheatCart;
	struct GBCheatState* s = &cc->s;
	if (!s->shown || s->regs[XP_GAME_WRITES] == 0) {
		if (address >= 0x4000 && address < 0x6000) {
			s->gameBank = value;
		}
		if (!s->shown) {
			gbCartWrite(gb, address, value);
			return;
		}
	}
	if (address < XP_REGS) {
		s->regs[address] = value;
		s->boot = 0;
		return;
	}
	if (s->regs[XP_GAME_WRITES] == 0) {
		gbCartWrite(gb, address, value);
	}
	if (address >= 0x4000) {
		xpFlashWrite(cc, xpFlashAddress(cc, address), value);
	}
}

// Its RAM, while it shows and 0003 is set; otherwise the game's.
static uint8_t* xpRam(struct GBCheatCart* cc, uint16_t address) {
	if (!cc->s.shown || !cc->s.regs[XP_OWN_RAM]) {
		return NULL;
	}
	return &cc->s.ram[(cc->s.regs[XP_RAM_BANK] & 0xF) * XP_RAM_BANK_SIZE + (address & (XP_RAM_BANK_SIZE - 1))];
}

// The Game Genie.

static bool ggRunning(const struct GBCheatCart* cc) {
	return cc->s.regs[GG_CONTROL] & GG_RUNNING;
}

// The game's byte, or the one a code that's on puts in its place.
static uint8_t ggPatched(const struct GBCheatCart* cc, uint16_t address, uint8_t value) {
	for (int i = 0; i < 3; ++i) {
		const uint8_t* code = &cc->s.regs[ggCodeAt[i]];
		if (!(cc->s.regs[GG_CODES_ON] & 1 << i) || address != (code[1] << 8 | code[0])) {
			continue;
		}
		if (!code[4] || value == code[2]) {
			return code[3];
		}
	}
	return value;
}

static void ggFillCodes(struct GB* gb) {
	struct GBCheatCart* cc = gb->cheatCart;
	static const uint8_t drawing[] = { 0x21, GG_TYPED & 0xFF, 0xC1 };
	if (cc->flashSize < GG_DRAW_CODES + sizeof(drawing) || memcmp(&cc->flash[GG_DRAW_CODES], drawing, sizeof(drawing))) {
		return;
	}
	for (int i = 0; i < 3; ++i) {
		uint8_t* row = gb->wram + GG_TYPED + i * GG_DIGITS;
		int empty = 0;
		for (int d = 0; d < GG_DIGITS; ++d) {
			empty += row[d] == GG_DIGIT_NONE;
		}
		if (!cc->s.fillCount[i] || gb->hram[GG_TYPED_COUNTS + i] || empty != GG_DIGITS) {
			continue;
		}
		memcpy(row, &cc->s.fill[i * GG_DIGITS], GG_DIGITS);
		gb->hram[GG_TYPED_COUNTS + i] = cc->s.fillCount[i];
	}
}

static uint8_t ggRead(struct GB* gb, uint16_t address) {
	struct GBCheatCart* cc = gb->cheatCart;
	if (ggRunning(cc)) {
		return ggPatched(cc, address, gameRead(gb, address));
	}
	if (header(cc, address)) {
		return gameRead(gb, address);
	}
	if ((cc->s.regs[GG_CONTROL] & GG_REGISTERS) && address >= GG_REGS && address < GG_REGS + GG_REGS_COUNT) {
		return cc->s.regs[address - GG_REGS];
	}
	if (cc->s.regs[GG_CONTROL] & GG_GAME) {
		return gameRead(gb, address);
	}
	return cc->flash[address & (cc->flashSize - 1)];
}

static void ggWrite(struct GB* gb, uint16_t address, uint8_t value) {
	struct GBCheatCart* cc = gb->cheatCart;
	if (ggRunning(cc)) {
		gbCartWrite(gb, address, value);
		return;
	}
	if (address >= GG_REGS && address < GG_REGS + GG_REGS_COUNT) {
		cc->s.regs[address - GG_REGS] = value;
		cc->s.boot = 0;
	}
}

uint8_t gbCheatCartRead(struct GB* gb, uint16_t address, bool dma) {
	struct GBCheatCart* cc = gb->cheatCart;
	struct GBCheatState* s = &cc->s;
	switch (cc->kind) {
	case GB_CHEAT_ACTION_REPLAY:
		if (!dma && address == VECTOR && arArmed(cc)) {
			s->shown ^= 1;
		}
		return arRead(gb, address);
	case GB_CHEAT_XPLODER:
		if (!dma && s->regs[XP_ARMED]) {
			if (!s->shown && address == VECTOR) {
				s->shown = 1;
			} else if (s->shown && (address == VECTOR || address == ENTRY)) {
				s->shown = 0;
			}
		}
		// A page is loaded once the program reads the chip again, to wait for it.
		if (!dma && s->shown && address >= 0x4000 && s->flashLoad == 2) {
			s->flashLoad = 0;
		}
		return xpRead(gb, address);
	default:
		if (!dma && address == GG_DRAW_CODES && !ggRunning(cc)) {
			ggFillCodes(gb);
		}
		return ggRead(gb, address);
	}
}

void gbCheatCartWrite(struct GB* gb, uint16_t address, uint8_t value) {
	switch (gb->cheatCart->kind) {
	case GB_CHEAT_ACTION_REPLAY:
		arWrite(gb, address, value);
		break;
	case GB_CHEAT_XPLODER:
		xpWrite(gb, address, value);
		break;
	default:
		ggWrite(gb, address, value);
		break;
	}
}

bool gbCheatCartReadRam(struct GB* gb, uint16_t address, uint8_t* value) {
	struct GBCheatCart* cc = gb->cheatCart;
	if (cc->kind == GB_CHEAT_ACTION_REPLAY) {
		return arReadRam(cc, address, value);
	}
	uint8_t* ram = cc->kind == GB_CHEAT_XPLODER ? xpRam(cc, address) : NULL;
	if (ram) {
		*value = *ram;
	}
	return ram;
}

bool gbCheatCartWriteRam(struct GB* gb, uint16_t address, uint8_t value) {
	struct GBCheatCart* cc = gb->cheatCart;
	if (cc->kind == GB_CHEAT_ACTION_REPLAY) {
		return arWriteRam(cc, address, value);
	}
	uint8_t* ram = cc->kind == GB_CHEAT_XPLODER ? xpRam(cc, address) : NULL;
	if (ram) {
		*ram = value;
	}
	return ram;
}

// What the cartridge keeps with the power off: the first Action Replay's RAM, the others' flash chip
// (none for the Game Genie).
uint8_t* gbCheatCartKept(struct GBCheatCart* cc, size_t* size) {
	if (cc->kind == GB_CHEAT_GAME_GENIE) {
		*size = 0;
		return NULL;
	}
	*size = cc->first ? AR_RAM_BANK_SIZE : cc->flashSize;
	return cc->first ? cc->s.ram : cc->flash;
}

// A state holds the cartridge's registers and RAM, and its flash chip as it differs from the image it
// was built from, so that each player keeps their own copy of the cartridge and none goes over netplay.
static size_t ramSize(const struct GBCheatCart* cc) {
	return cc->kind == GB_CHEAT_XPLODER ? 0x20000 : cc->kind == GB_CHEAT_ACTION_REPLAY ? 0x8000 : 0;
}

static size_t flashInState(const struct GBCheatCart* cc) {
	return cc->kind == GB_CHEAT_GAME_GENIE ? 0 : cc->flashSize;
}

size_t gbCheatCartStateSize(const struct GBCheatCart* cc) {
	return offsetof(struct GBCheatState, ram) + ramSize(cc) + flashInState(cc);
}

void gbCheatCartSaveState(const struct GBCheatCart* cc, uint8_t* data) {
	size_t head = offsetof(struct GBCheatState, ram) + ramSize(cc);
	memcpy(data, &cc->s, head);
	for (size_t i = 0; i < flashInState(cc); ++i) {
		data[head + i] = cc->flash[i] ^ cc->factory[i];
	}
}

void gbCheatCartLoadState(struct GBCheatCart* cc, const uint8_t* data) {
	size_t head = offsetof(struct GBCheatState, ram) + ramSize(cc);
	memcpy(&cc->s, data, head);
	for (size_t i = 0; i < flashInState(cc); ++i) {
		cc->flash[i] = data[head + i] ^ cc->factory[i];
	}
}
