//
//   Copyright (c) 2026 Magnominus
//
//   This program is free software; you can redistribute it and/or modify
//   it under the terms of the GNU General Public License version 2 as
//   published by the Free Software Foundation.
//
//   This program is distributed in the hope that it will be useful,
//   but WITHOUT ANY WARRANTY; without even the implied warranty of
//   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//   GNU General Public License version 2 for more details.
//

#include "gbint.h"
#include "mbc.h"

// armGBA: Datel's Action Replay for the Game Boy and Game Boy Color, as its programs use it (nothing
// about its hardware is published). The game plugs in on top; the console sees one or the other, the
// Action Replay at power on. Its switch, when off, leaves the game running alone: the game's accesses
// no longer reach the Action Replay's registers or set off its hook, except while the later
// cartridges' button is held, which still opens their menu.
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
// - With 7FE2 bit 4 set, the game's byte at the address in 7FE7 (high) and 7FE6 (low) reads FF. The
//   program sets it for Pokemon Crystal, at the operand of the game's check that VBlank still has
//   time to copy tiles (cp $92 after reading LY), which the hook's delay to its VBlank handler would
//   otherwise fail, freezing it at its next change of map.
//
// The first cartridge (Pro Action Replay), a 32 KiB ROM with 4 KiB of RAM at B000-BFFF that shows over
// the game's RAM there too while the switch is on, its codes kept by a battery:
// - A write to A900 shows the game and sets the hook (the VBlank interrupt's fetch from 0040 shows
//   the Action Replay, which jumps to its handler at B080); a write to A800 shows the game again
//   from there, or before a game runs, shows the game's first 512 bytes (its vectors and header)
//   for the program to look at.
//
// For both, from power on until the program first writes a register, the header (0104-014F) is the
// game's, for the boot ROM's check.

namespace gambatte {

namespace {

enum {
	reg_ram_bank = 0x0,
	reg_rom8 = 0x1,
	reg_arm = 0x2,
	reg_bus = 0x5,
	reg_patch_low = 0x6,
	reg_patch_high = 0x7,
	reg_status = 0xE,
	arm_patch = 0x10,
	bus_game_writes = 0x01,
	bus_look = 0x10,
	status_switch_off = 0x10,
	status_button = 0x01,
	vector = 0x0040,
	ram_start = 0x6000,
	ram_fixed = 0x7000,
	ram_bank_size = 0x1000,
	regs_start = 0x7FE0,
	sector_size = 0x1000,

	first_ram = 0xB000,
	first_show_game = 0xA800,
	first_start = 0xA900,
	first_low_page = 0x200,
};

std::size_t flashSizeFor(std::size_t size) {
	std::size_t p = 0x8000;
	while (p < size)
		p <<= 1;
	return p;
}

}

// The first cartridge's program stores to A800 and A900, where the later ones' store to 7FE0-7FE7
// all through.
static bool isFirst(unsigned char const *flash, std::size_t size) {
	unsigned found = 0;
	unsigned later = 0;
	for (std::size_t i = 0; i + 2 < size; ++i) {
		later += flash[i] == 0xEA && flash[i + 2] == 0x7F && flash[i + 1] >= 0xE0 && flash[i + 1] <= 0xE7;
		if (flash[i] == 0xEA && flash[i + 1] == 0x00)
			found |= flash[i + 2] == 0xA8 ? 1 : flash[i + 2] == 0xA9 ? 2 : 0;
	}
	return found == 3 && later < 16;
}

ActionReplayGb::ActionReplayGb(MemPtrs &memptrs, Mbc *game, RomHook *gameHook,
                               unsigned char const *flash, std::size_t size, bool hasGame)
: memptrs_(memptrs)
, game_(game)
, gameHook_(gameHook)
, flash_(flashSizeFor(size), 0xFF)
, hasGame_(hasGame)
, first_(isFirst(flash, size))
, button_(false)
, switchOn_(true)
, flashChanged_(false)
{
	std::memcpy(&flash_[0], flash, size);
	factory_ = flash_;
	std::memset(&s_, 0, sizeof s_);
	reset();
}

// Power on. The first cartridge's RAM keeps what it held.
void ActionReplayGb::reset() {
	State const was = s_;
	std::memset(&s_, 0, sizeof s_);
	if (first_)
		std::memcpy(s_.ram, was.ram, ram_bank_size);
	s_.shown = 1;
	s_.boot = 1;
	s_.regs[reg_ram_bank] = 1;
	s_.regs[reg_rom8] = 2;
	memptrs_.setRomHooked(true);
	memptrs_.setSramHooked(first_);
}

// From the game, the hook works only with the switch on (or, on the later cartridges, while the button
// is held); the program's own look at the game always.
bool ActionReplayGb::armed() const {
	if (first_)
		return s_.shown ? false : s_.regs[reg_arm] && switchOn_;
	if (s_.regs[reg_bus] & bus_look)
		return true;
	return (s_.regs[reg_arm] & 1) && (s_.shown || switchOn_ || button_);
}

std::size_t ActionReplayGb::flashAddress(unsigned p) const {
	std::size_t const at = p < 0x4000 || first_ ? p : s_.regs[reg_rom8] * std::size_t(0x2000) + (p - 0x4000);
	return at & (flash_.size() - 1);
}

unsigned ActionReplayGb::gameRead(unsigned p) {
	if (!hasGame_)
		return 0xFF;
	return gameHook_ ? gameHook_->romRead(p) : memptrs_.romdata(p >> 14)[p];
}

// 7000-7FFF is the RAM's first 4 KiB; 6000-6FFF the 4 KiB bank 7FE0 picks.
unsigned char &ActionReplayGb::ram(unsigned const p) {
	unsigned const bank = p < ram_fixed ? s_.regs[reg_ram_bank] & 7 : 0;
	return s_.ram[bank * ram_bank_size + (p & (ram_bank_size - 1))];
}

unsigned ActionReplayGb::status() const {
	unsigned value = 0xEE;
	if (!switchOn_)
		value |= status_switch_off;
	if (!button_)
		value |= status_button;
	return value;
}

unsigned ActionReplayGb::romRead(unsigned p) {
	if (p == vector && armed())
		s_.shown ^= 1;
	return romDmaRead(p);
}

unsigned ActionReplayGb::romDmaRead(unsigned p) {
	if (first_) {
		bool const game = !s_.shown || (s_.lowPage && p < first_low_page) || (s_.boot && p >= 0x104 && p < 0x150);
		return game ? gameRead(p) : flash_[flashAddress(p)];
	}
	if (!s_.shown && p >= ram_fixed && (s_.regs[reg_bus] & bus_look))
		return ram(p);
	if (!s_.shown && (s_.regs[reg_arm] & arm_patch)
			&& p == (s_.regs[reg_patch_high] << 8 | s_.regs[reg_patch_low]))
		return 0xFF;
	if (!s_.shown || (s_.boot && p >= 0x104 && p < 0x150))
		return gameRead(p);
	if (p < ram_start)
		return s_.flashId && p < 2 ? (p ? 0xB5 : 0xBF) : flash_[flashAddress(p)];
	if (p >= regs_start && p < regs_start + 0x10) {
		unsigned const reg = p - regs_start;
		return reg == reg_status ? status() : s_.regs[reg];
	}
	return ram(p);
}

// Whether the registers take a write: from the game, only with the switch on or while the program
// looks at the game itself.
bool ActionReplayGb::listening() const {
	return s_.shown || switchOn_ || (s_.regs[reg_bus] & bus_look);
}

void ActionReplayGb::romWrite(unsigned const p, unsigned const data, gb_ulong const cc) {
	if (first_) {
		if (!s_.shown)
			game_->romWrite(p, data, cc);
		return;
	}
	if (p >= regs_start && p < regs_start + 0x10 && listening()) {
		s_.regs[p - regs_start] = data;
		s_.boot = 0;
		return;
	}
	if (p == vector && armed()) {
		s_.shown ^= 1;
		return;
	}
	bool const lookRam = p >= ram_fixed && (s_.regs[reg_bus] & bus_look);
	if (!s_.shown && !lookRam) {
		game_->romWrite(p, data, cc);
		return;
	}
	if (s_.regs[reg_bus] & bus_game_writes)
		game_->romWrite(p, data, cc);
	if (p >= ram_start) {
		ram(p) = data;
		return;
	}
	flashWrite(flashAddress(p), data);
}

// The first cartridge's RAM and registers. With the switch off, the game running has its own RAM there.
bool ActionReplayGb::sramRead(unsigned const p, unsigned &value) {
	if (!first_ || p < first_ram || (!s_.shown && !switchOn_))
		return false;
	value = s_.ram[p & (ram_bank_size - 1)];
	return true;
}

bool ActionReplayGb::sramWrite(unsigned const p, unsigned const data) {
	if (!first_ || (!s_.shown && !switchOn_))
		return false;
	if (p >= first_ram) {
		s_.ram[p & (ram_bank_size - 1)] = data;
		flashChanged_ = true;
		return true;
	}
	if (p != first_show_game && p != first_start)
		return false;
	s_.boot = 0;
	if (p == first_start || s_.regs[reg_arm]) {
		s_.regs[reg_arm] = 1;
		s_.lowPage = 0;
		s_.shown = 0;
	} else {
		s_.lowPage = 1;
	}
	return true;
}

void ActionReplayGb::flashWrite(std::size_t const at, unsigned const data) {
	if (s_.flashProgram) {
		s_.flashProgram = 0;
		unsigned char const now = flash_[at] & data;
		if (now != flash_[at]) {
			flash_[at] = now;
			flashChanged_ = true;
		}
		return;
	}
	if (data == 0xF0) {
		s_.flashStep = 0;
		s_.flashId = 0;
		return;
	}
	unsigned const command = at & 0x7FFF;
	switch (s_.flashStep) {
	case 0:
	case 3:
		s_.flashStep = command == 0x5555 && data == 0xAA ? s_.flashStep + 1 : 0;
		break;
	case 1:
	case 4:
		s_.flashStep = command == 0x2AAA && data == 0x55 ? s_.flashStep + 1 : 0;
		break;
	case 2:
		s_.flashStep = 0;
		if (command != 0x5555)
			break;
		if (data == 0x90)
			s_.flashId = 1;
		else if (data == 0xA0)
			s_.flashProgram = 1;
		else if (data == 0x80)
			s_.flashStep = 3;
		break;
	case 5:
		s_.flashStep = 0;
		if (data == 0x30) {
			std::memset(&flash_[at & ~std::size_t(sector_size - 1)], 0xFF, sector_size);
			flashChanged_ = true;
		} else if (data == 0x10 && command == 0x5555) {
			std::memset(&flash_[0], 0xFF, flash_.size());
			flashChanged_ = true;
		}
		break;
	}
}

bool ActionReplayGb::disabledRam() const {
	return game_->disabledRam();
}

void ActionReplayGb::saveState(SaveState::Mem &ss) const {
	game_->saveState(ss);
}

void ActionReplayGb::loadState(SaveState::Mem const &ss) {
	game_->loadState(ss);
	memptrs_.setRomHooked(true);
	memptrs_.setSramHooked(first_);
}

bool ActionReplayGb::isAddressWithinAreaRombankCanBeMappedTo(unsigned addr, unsigned bank) const {
	return game_->isAddressWithinAreaRombankCanBeMappedTo(addr, bank);
}

void ActionReplayGb::SyncState(NewState *ns, bool isReader) {
	game_->SyncState(ns, isReader);
	NSS(s_);
	// The flash chip too, so every device in netplay has the same code lists and menus: as it differs
	// from the cartridge's image, which every player has (see swapFactory)
	if (!isReader)
		swapFactory();
	PSS(&flash_[0], flash_.size());
	swapFactory();
	if (isReader) {
		memptrs_.setRomHooked(true);
		memptrs_.setSramHooked(first_);
	}
}

// What the cartridge keeps with the power off: the later ones' flash chip, the first one's RAM.
unsigned char *ActionReplayGb::kept(std::size_t *size) {
	*size = first_ ? std::size_t(ram_bank_size) : flash_.size();
	return first_ ? s_.ram : &flash_[0];
}

bool ActionReplayGb::flashChanged() {
	bool const was = flashChanged_;
	flashChanged_ = false;
	return was;
}

}
