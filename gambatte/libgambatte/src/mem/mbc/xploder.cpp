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

#include "mbc.h"

#include <cstring>

// armGBA: Blaze's Xploder GB (Future Console Design), as its program uses it. The game plugs in on top;
// the console sees one or the other, the Xploder at power on.
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
// - From power on until the program first writes a register, the header (0104-014F) is the game's,
//   for the boot ROM's check: the Xploder's program has none.

namespace gambatte {

namespace {

enum {
	reg_overlay = 0x0,
	reg_own_low = 0x1,
	reg_game_writes = 0x2,
	reg_own_ram = 0x3,
	reg_game_bank = 0x4,
	reg_armed = 0x5,
	reg_flash_bank = 0x6,
	reg_ram_bank = 0x7,
	regs_count = 8,
	button_pressed = 0x02,
	vector = 0x0040,
	entry = 0x0100,
	overlay_size = 0x2000,
	ram_bank_size = 0x2000,
	flash_size = 0x40000,
	cheats_start = 0x20000,
	page_size = 0x100,
	maker_id = 0x1F,
	device_id = 0xDA,
};

}

bool XploderGb::is(unsigned char const *program, std::size_t size) {
	static char const name[] = "Future Console Design";
	return size >= 0x150 && size <= flash_size && !std::memcmp(program + 0x104, name, sizeof name - 1);
}

XploderGb::XploderGb(MemPtrs &memptrs, Mbc *game, RomHook *gameHook,
                     unsigned char const *program, std::size_t size, bool hasGame)
: memptrs_(memptrs)
, game_(game)
, gameHook_(gameHook)
, flash_(flash_size, 0xFF)
, hasGame_(hasGame)
, button_(false)
, flashChanged_(false)
{
	std::memcpy(&flash_[0], program, size);
	// The cheats start at 20000, where a dump of the program alone has none: an empty list there,
	// which a save of the chip replaces.
	if (size <= cheats_start)
		flash_[cheats_start] = 0;
	factory_ = flash_;
	std::memset(&s_, 0, sizeof s_);
	reset();
}

// Power on: the Xploder shows, its program at 0000.
void XploderGb::reset() {
	std::memset(&s_, 0, sizeof s_);
	s_.shown = 1;
	s_.boot = 1;
	s_.regs[reg_own_low] = 1;
	s_.regs[reg_game_writes] = 1;
	s_.regs[reg_own_ram] = 1;
	s_.regs[reg_flash_bank] = 1;
	memptrs_.setRomHooked(true);
	memptrs_.setSramHooked(true);
}

unsigned XploderGb::gameRead(unsigned p) {
	if (!hasGame_)
		return 0xFF;
	return gameHook_ ? gameHook_->romRead(p) : memptrs_.romdata(p >> 14)[p];
}

std::size_t XploderGb::flashAddress(unsigned p) const {
	std::size_t const at = p < 0x4000 ? p : s_.regs[reg_flash_bank] * std::size_t(0x4000) + (p - 0x4000);
	return at & (flash_.size() - 1);
}

unsigned XploderGb::flashRead(std::size_t const at) {
	if (s_.flashId && at < 2)
		return at ? device_id : maker_id;
	return flash_[at];
}

unsigned XploderGb::romRead(unsigned p) {
	if (s_.regs[reg_armed]) {
		if (!s_.shown && p == vector)
			s_.shown = 1;
		else if (s_.shown && (p == vector || p == entry))
			s_.shown = 0;
	}
	// A page is loaded once the program reads the chip again, to wait for it
	if (s_.shown && p >= 0x4000 && s_.flashLoad == 2)
		s_.flashLoad = 0;
	return romDmaRead(p);
}

unsigned XploderGb::romDmaRead(unsigned p) {
	if (!s_.shown || (s_.boot && p >= 0x104 && p < 0x150))
		return gameRead(p);
	if (p >= 0x4000)
		return flashRead(flashAddress(p));
	if (p == reg_overlay)
		return button_ ? button_pressed : 0;
	if (p == reg_game_bank)
		return s_.gameBank;
	if (p == reg_flash_bank)
		return s_.regs[reg_flash_bank];
	if (!s_.regs[reg_own_low])
		return gameRead(p);
	if ((s_.regs[reg_overlay] & 1) && p < overlay_size)
		return s_.ram[p];
	return flashRead(p);
}

void XploderGb::romWrite(unsigned const p, unsigned const data, gb_ulong const cc) {
	if (!s_.shown || s_.regs[reg_game_writes] == 0) {
		if (p >= 0x4000 && p < 0x6000)
			s_.gameBank = data;
		if (!s_.shown) {
			game_->romWrite(p, data, cc);
			return;
		}
	}
	if (p < regs_count) {
		s_.regs[p] = data;
		s_.boot = 0;
		return;
	}
	if (s_.regs[reg_game_writes] == 0)
		game_->romWrite(p, data, cc);
	if (p >= 0x4000)
		flashWrite(flashAddress(p), data);
}

// Its RAM, while it shows and 0003 is set; otherwise the game's.
bool XploderGb::sramRead(unsigned const p, unsigned &value) {
	if (!s_.shown || !s_.regs[reg_own_ram])
		return false;
	value = s_.ram[(s_.regs[reg_ram_bank] & 0xF) * ram_bank_size + (p & (ram_bank_size - 1))];
	return true;
}

bool XploderGb::sramWrite(unsigned const p, unsigned const data) {
	if (!s_.shown || !s_.regs[reg_own_ram])
		return false;
	s_.ram[(s_.regs[reg_ram_bank] & 0xF) * ram_bank_size + (p & (ram_bank_size - 1))] = data;
	return true;
}

void XploderGb::flashWrite(std::size_t const at, unsigned const data) {
	std::size_t const page = at / page_size;
	if (s_.flashLoad == 2 && page != (std::size_t(s_.pageHigh) << 8 | s_.pageLow))
		s_.flashLoad = 0;
	if (s_.flashLoad) {
		if (s_.flashLoad == 1) {
			s_.flashLoad = 2;
			s_.pageHigh = page >> 8;
			s_.pageLow = page & 0xFF;
			std::memset(&flash_[page * page_size], 0xFF, page_size);
		}
		flash_[at] = data;
		flashChanged_ = true;
		return;
	}
	unsigned const command = at & 0x7FFF;
	switch (s_.flashStep) {
	case 0:
		s_.flashStep = command == 0x5555 && data == 0xAA;
		break;
	case 1:
		s_.flashStep = command == 0x2AAA && data == 0x55 ? 2 : 0;
		break;
	case 2:
		s_.flashStep = 0;
		if (command != 0x5555)
			break;
		if (data == 0x90)
			s_.flashId = 1;
		else if (data == 0xF0)
			s_.flashId = 0;
		else if (data == 0xA0)
			s_.flashLoad = 1;
		break;
	}
}

bool XploderGb::disabledRam() const {
	return game_->disabledRam();
}

void XploderGb::saveState(SaveState::Mem &ss) const {
	game_->saveState(ss);
}

void XploderGb::loadState(SaveState::Mem const &ss) {
	game_->loadState(ss);
	memptrs_.setRomHooked(true);
	memptrs_.setSramHooked(true);
}

bool XploderGb::isAddressWithinAreaRombankCanBeMappedTo(unsigned addr, unsigned bank) const {
	return game_->isAddressWithinAreaRombankCanBeMappedTo(addr, bank);
}

void XploderGb::SyncState(NewState *ns, bool isReader) {
	game_->SyncState(ns, isReader);
	NSS(s_);
	// The flash chip too, so every device in netplay has the same cheats: as it differs from the
	// cartridge's image, which every player has (see swapFactory)
	if (!isReader)
		swapFactory();
	PSS(&flash_[0], flash_.size());
	swapFactory();
	if (isReader) {
		memptrs_.setRomHooked(true);
		memptrs_.setSramHooked(true);
	}
}

bool XploderGb::flashChanged() {
	bool const was = flashChanged_;
	flashChanged_ = false;
	return was;
}

}
