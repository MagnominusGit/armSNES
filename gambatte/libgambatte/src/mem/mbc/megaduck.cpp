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

// armGBA: the Mega Duck's cartridges (as megaduck-info and Super Junior SameDuck have them). A Mega Duck
// game has no header to say which one it is on; a file extension can, else (a .bin) both handheld kinds
// are there at once, as their registers don't overlap:
//   MD1   a write to B000 picks a 32 KiB bank for the whole of 0000-7FFF
//   MD2   a write to 0001 picks the bank at 4000-7FFF
//   MD0   (the laptops' system ROMs and cartridges) a write to 1000 picks a 32 KiB bank with its low
//         half and an 8 KiB bank of the memory card's 32 KiB with bits 5-4; that RAM is always on
//   MD20S MD2 with an MD0 memory card: bits 5-4 at 1000 pick its bank
//   MD25S MD2 with 128 KiB of RAM: 4000 picks its bank (0000 is its enable, which games write but
//         Super Junior SameDuck doesn't need)
// Bank 0 written to 0001 is bank 1, as on an MBC1 (as the MiSTer core has it, and megaduck-info gives
// that register's range as 1-3 or 1-7): with bank 0 there, Duck Adventures' levels come out as garbage.

namespace gambatte {

MegaDuck::MegaDuck(MemPtrs &memptrs, Kind const kind)
: memptrs_(memptrs)
, kind_(kind)
, rombank0_(0)
, rombank_(1)
, rambank_(0)
{
	setBanks();
}

bool MegaDuck::disabledRam() const {
	return !hasRam();
}

bool MegaDuck::hasRam() const {
	return kind_ == md0 || kind_ == md20s || kind_ == md25s;
}

void MegaDuck::romWrite(unsigned const p, unsigned const data, gb_ulong const /*cc*/) {
	switch (kind_) {
	case none:
	case md1:
		return;
	case md0:
		if (p != 0x1000)
			return;
		rombank0_ = (data & 0x0F) * 2;
		rombank_ = (data & 0x0F) * 2 + 1;
		rambank_ = data >> 4 & 3;
		break;
	case automatic:
	case md2:
	case md20s:
	case md25s:
		if (p == 0x0001)
			rombank_ = data ? data : 1;
		else if (p == 0x1000 && kind_ == md20s)
			rambank_ = data >> 4 & 3;
		else if (p == 0x4000 && kind_ == md25s)
			rambank_ = data & 0x0F;
		else
			return;
		break;
	}
	setBanks();
}

unsigned MegaDuck::read(unsigned, gb_ulong) const {
	return 0xFF;
}

void MegaDuck::write(unsigned const p, unsigned const data, gb_ulong) {
	if (p == 0xB000 && (kind_ == automatic || kind_ == md1)) {
		rombank0_ = data * 2;
		rombank_ = data * 2 + 1;
		setBanks();
	}
}

void MegaDuck::saveState(SaveState::Mem &ss) const {
	ss.rombank = rombank_ | rombank0_ << 8;
	ss.rambank = rambank_;
	ss.enableRam = hasRam();
}

void MegaDuck::loadState(SaveState::Mem const &ss) {
	rombank_ = ss.rombank & 0xFF;
	rombank0_ = ss.rombank >> 8;
	rambank_ = ss.rambank;
	setBanks();
}

bool MegaDuck::isAddressWithinAreaRombankCanBeMappedTo(unsigned addr, unsigned bank) const {
	return addr < rombank_size() ? bank == rombank0_ : true;
}

void MegaDuck::SyncState(NewState *ns, bool isReader) {
	NSS(rombank0_);
	NSS(rombank_);
	NSS(rambank_);
}

void MegaDuck::setBanks() const {
	unsigned const mask = rombanks(memptrs_) - 1;
	if (hasRam()) {
		memptrs_.setRambank(MemPtrs::read_en | MemPtrs::write_en, rambank_ & (rambanks(memptrs_) - 1));
	} else {
		// A000-BFFF goes to read() and write(), as the B000 register is there
		memptrs_.setRambank(MemPtrs::read_en | MemPtrs::write_en | MemPtrs::rtc_en, 0);
	}
	memptrs_.setRombank0(rombank0_ & mask);
	memptrs_.setRombank(rombank_ & mask);
}

}
