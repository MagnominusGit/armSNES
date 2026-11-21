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

#include <algorithm>
#include <cstring>

// armGBA: Galoob's Game Genie for the Game Boy, as its program (8 KiB) uses it. The game plugs in on
// top; the Game Genie answers reads of the cartridge's ROM, its program at power on and the game's once
// it starts it, with up to three of the game's bytes replaced.
// - Registers at 4000-4013, written by the program: 4000 says what shows (bit 2 the game, else the
//   program; bit 1 the registers, read back at 4000-4013; bit 0 the game runs, which nothing undoes
//   until the power goes off). 4001 has a bit for each code that's on. The three codes are at 4003,
//   4008 and 400D: an address of the ROM's (low byte, then high), the byte it reads, the byte that
//   replaces it, and whether only that byte is replaced (a code of nine letters, rather than six).
//   4012 and 4013 are written as it starts, with no use found for them.
// - The program decodes the codes the player types in (ABC-DEF-GHI) and starts the game at 0100 by
//   writing 6, then 7, to 4000.
// - From power on until the program first writes a register, the header (0104-014F) is the game's,
//   for the boot ROM's check: the Game Genie's program has none.
// - The codes typed in are kept in work RAM at C140, nine digits each (0-15, 0x10 for none), with how
//   many digits each has in FF C1-C3. The program draws its code screen from them at 08D7.

namespace gambatte {

namespace {

enum {
	reg_control = 0x0,
	reg_codes_on = 0x1,
	regs_start = 0x4000,
	regs_count = 0x14,
	control_running = 0x01,
	control_registers = 0x02,
	control_game = 0x04,
	codes = 3,
	code_size = 5,
	code_low = 0,
	code_high = 1,
	code_compare = 2,
	code_value = 3,
	code_compares = 4,
};

unsigned const codeAt[codes] = { 0x3, 0x8, 0xD };

enum {
	typed_at = 0x140,
	typed_counts = 0x1C1,
	digits = 9,
	digit_none = 0x10,
	draw_codes = 0x08D7,
};

}

bool GameGenieGb::is(unsigned char const *program, std::size_t size) {
	static char const name[] = "GAME GENIE";
	if (size < 0x2000 || size > 0x8000)
		return false;
	unsigned char const *const end = program + size;
	return std::search(program, end, name, name + sizeof name - 1) != end;
}

GameGenieGb::GameGenieGb(MemPtrs &memptrs, Mbc *game, RomHook *gameHook,
                         unsigned char const *program, std::size_t size, bool hasGame)
: memptrs_(memptrs)
, game_(game)
, gameHook_(gameHook)
, hasGame_(hasGame)
, hram_(0)
{
	std::size_t room = 0x2000;
	while (room < size)
		room <<= 1;
	program_.assign(room, 0xFF);
	std::memcpy(&program_[0], program, size);
	std::memset(&s_, 0, sizeof s_);
	std::memset(s_.fill, digit_none, sizeof s_.fill);
	reset();
}

// Power on: the program shows, with every code off.
void GameGenieGb::reset() {
	// The codes chosen stay through a restart, as the options do
	State const kept = s_;
	std::memset(&s_, 0, sizeof s_);
	std::memcpy(s_.fill, kept.fill, sizeof s_.fill);
	std::memcpy(s_.fillCount, kept.fillCount, sizeof s_.fillCount);
	s_.boot = 1;
	memptrs_.setRomHooked(true);
}

bool GameGenieGb::running() const {
	return s_.regs[reg_control] & control_running;
}

unsigned GameGenieGb::gameRead(unsigned p) {
	if (!hasGame_)
		return 0xFF;
	return gameHook_ ? gameHook_->romRead(p) : memptrs_.romdata(p >> 14)[p];
}

// The game's byte at p, or the one a code that's on puts in its place.
unsigned GameGenieGb::patched(unsigned p, unsigned value) const {
	for (unsigned i = 0; i < codes; ++i) {
		unsigned char const *const code = &s_.regs[codeAt[i]];
		if (!(s_.regs[reg_codes_on] & 1 << i) || p != (unsigned(code[code_high]) << 8 | code[code_low]))
			continue;
		if (!code[code_compares] || value == code[code_compare])
			return code[code_value];
	}
	return value;
}

// armGBA: codes chosen in the core options go in the slots that are empty as the program draws its
// code screen, as if typed in: they can still be changed there, and a code typed in stays.
void GameGenieGb::setCodes(unsigned char const *digitsIn, unsigned char const *counts) {
	std::memcpy(s_.fill, digitsIn, sizeof s_.fill);
	std::memcpy(s_.fillCount, counts, sizeof s_.fillCount);
}

void GameGenieGb::fillCodes() {
	static unsigned char const drawing[] = { 0x21, typed_at & 0xFF, 0xC1 };
	if (!hram_ || program_.size() < draw_codes + sizeof drawing
			|| std::memcmp(&program_[draw_codes], drawing, sizeof drawing))
		return;
	unsigned char *const typed = memptrs_.wramdata(0) + typed_at;
	for (unsigned i = 0; i < codes; ++i) {
		unsigned char *const row = typed + i * digits;
		if (!s_.fillCount[i] || hram_[typed_counts + i]
				|| std::count(row, row + digits, digit_none) != digits)
			continue;
		std::memcpy(row, &s_.fill[i * digits], digits);
		hram_[typed_counts + i] = s_.fillCount[i];
	}
}

unsigned GameGenieGb::romRead(unsigned p) {
	if (p == draw_codes && !running())
		fillCodes();
	return romDmaRead(p);
}

unsigned GameGenieGb::romDmaRead(unsigned p) {
	if (running())
		return patched(p, gameRead(p));
	if (s_.boot && p >= 0x104 && p < 0x150)
		return gameRead(p);
	if ((s_.regs[reg_control] & control_registers) && p >= regs_start && p < regs_start + regs_count)
		return s_.regs[p - regs_start];
	if (s_.regs[reg_control] & control_game)
		return gameRead(p);
	return program_[p & (program_.size() - 1)];
}

void GameGenieGb::romWrite(unsigned const p, unsigned const data, gb_ulong const cc) {
	if (running()) {
		game_->romWrite(p, data, cc);
		return;
	}
	if (p >= regs_start && p < regs_start + regs_count) {
		s_.regs[p - regs_start] = data;
		s_.boot = 0;
	}
}

bool GameGenieGb::disabledRam() const {
	return game_->disabledRam();
}

void GameGenieGb::saveState(SaveState::Mem &ss) const {
	game_->saveState(ss);
}

void GameGenieGb::loadState(SaveState::Mem const &ss) {
	game_->loadState(ss);
	memptrs_.setRomHooked(true);
}

bool GameGenieGb::isAddressWithinAreaRombankCanBeMappedTo(unsigned addr, unsigned bank) const {
	return game_->isAddressWithinAreaRombankCanBeMappedTo(addr, bank);
}

void GameGenieGb::SyncState(NewState *ns, bool isReader) {
	game_->SyncState(ns, isReader);
	NSS(s_);
	if (isReader)
		memptrs_.setRomHooked(true);
}

}
