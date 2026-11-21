//
//   Copyright (C) 2007-2010 by sinamas <sinamas at users.sourceforge.net>
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
//   You should have received a copy of the GNU General Public License
//   version 2 along with this program; if not, write to the
//   Free Software Foundation, Inc.,
//   51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA.
//

#ifndef MBC_H
#define MBC_H

#include "gbint.h"
#include <cstring>
#include "../memptrs.h"
#include "../time.h"
#include "../rtc.h"
#include "../infrared.h"
#include "../huc3_chip.h"
#include "../camera.h"
#include "savestate.h"
#include "newstate.h"
#include "scoped_ptr.h"

#include <string>
#include <vector>

namespace gambatte {

inline unsigned rambanks(MemPtrs const &memptrs) {
	return (memptrs.rambankdataend() - memptrs.rambankdata()) / rambank_size();
}

inline unsigned rombanks(MemPtrs const &memptrs) {
	return (memptrs.romdataend() - memptrs.romdata()) / rombank_size();
}

class Mbc {
public:
	virtual ~Mbc() {}
	virtual bool disabledRam() const = 0;
	virtual void romWrite(unsigned p, unsigned data, gb_ulong cycleCounter) = 0;
	virtual void saveState(SaveState::Mem &ss) const = 0;
	virtual void loadState(SaveState::Mem const &ss) = 0;
	virtual bool isAddressWithinAreaRombankCanBeMappedTo(unsigned address, unsigned rombank) const = 0;
	template<bool isReader>void SyncState(NewState *ns) {
		// can't have virtual templates, so..
		SyncState(ns, isReader);
	}
	virtual void SyncState(NewState *ns, bool isReader) = 0;
};

class DefaultMbc : public Mbc {
public:
	virtual bool isAddressWithinAreaRombankCanBeMappedTo(unsigned addr, unsigned bank) const {
		return (addr < rombank_size()) == (bank == 0);
	}

	virtual void SyncState(NewState */*ns*/, bool /*isReader*/)
	{
	}
};

class Mbc0 : public DefaultMbc {
public:
	explicit Mbc0(MemPtrs &memptrs);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned const p, unsigned const data, gb_ulong const /*cc*/);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual void SyncState(NewState *ns, bool isReader);

private:
	MemPtrs &memptrs_;
	bool enableRam_;
};

class Mbc1 : public Mbc {
public:
	explicit Mbc1(MemPtrs &memptrs, unsigned char rombankMask = 0x1Fu, unsigned char rombankShift = 0x05u);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned const p, unsigned const data, gb_ulong const /*cc*/);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual bool isAddressWithinAreaRombankCanBeMappedTo(unsigned addr, unsigned bank) const;
	virtual void SyncState(NewState *ns, bool isReader);

private:
	MemPtrs &memptrs_;
	bool enableRam_;
	bool rambankMode_;
	unsigned char bankReg1_;
	unsigned char bankReg2_;
	unsigned char rombank0_;
	unsigned char rombank_;
	unsigned char rambank_;
	unsigned char rombankMask_;
	unsigned char rombankShift_;

	void updateBanking();
	void setRambank() const;
	void setRombank() const;
};

class Mbc1Multi64 : public Mbc1 {
public:
	explicit Mbc1Multi64(MemPtrs &memptrs)
	: Mbc1(memptrs, 0x0Fu, 0x04u)
	{
	}
};

class Mbc2 : public DefaultMbc {
public:
	explicit Mbc2(MemPtrs &memptrs);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned const p, unsigned const data, gb_ulong const /*cc*/);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual void SyncState(NewState *ns, bool isReader);

private:
	MemPtrs &memptrs_;
	unsigned char rombank_;
	bool enableRam_;
};

class Mbc3 : public DefaultMbc {
public:
	Mbc3(MemPtrs &memptrs, Rtc *const rtc, unsigned char rombankMask = 0x7Fu, unsigned char rambankMask = 0x03u);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned const p, unsigned const data, gb_ulong const cc);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual void SyncState(NewState *ns, bool isReader);

private:
	MemPtrs &memptrs_;
	Rtc *const rtc_;
	unsigned char rombank_;
	unsigned char rambank_;
	bool enableRam_;
	unsigned char rombankMask_;
	unsigned char rambankMask_;

	void setRambank(unsigned flags = MemPtrs::read_en | MemPtrs::write_en) const;
	void setRombank() const;
};

class Mbc30 : public Mbc3 {
public:
	Mbc30(MemPtrs &memptrs, Rtc *const rtc)
	: Mbc3(memptrs, rtc, 0xFFu, 0x07u)
	{
	}
};

class Mbc5 : public DefaultMbc {
public:
	explicit Mbc5(MemPtrs &memptrs);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned const p, unsigned const data, gb_ulong const /*cc*/);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual void SyncState(NewState *ns, bool isReader);

private:
	MemPtrs &memptrs_;
	unsigned short rombank_;
	unsigned char rambank_;
	bool enableRam_;

	void setRambank() const;
	void setRombank() const;
};

class M161 : public Mbc {
public:
	explicit M161(MemPtrs &memptrs);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned const /*p*/, unsigned const data, gb_ulong const /*cc*/);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual bool isAddressWithinAreaRombankCanBeMappedTo(unsigned addr, unsigned bank) const;
	virtual void SyncState(NewState* ns, bool isReader);

private:
	MemPtrs &memptrs_;
	unsigned char rombank_;
	bool mapped_;

	void setRombank() const;
};

class Mmm01 : public Mbc {
public:
	explicit Mmm01(MemPtrs &memptrs);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned const p, unsigned const data, gb_ulong const /*cc*/);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual bool isAddressWithinAreaRombankCanBeMappedTo(unsigned addr, unsigned bank) const;
	virtual void SyncState(NewState *ns, bool isReader);

private:
	MemPtrs &memptrs_;
	unsigned char bankReg0_;
	unsigned char bankReg1_;
	unsigned char bankReg2_;
	unsigned char bankReg3_;

	bool isMapped() const;
	bool enableRam() const;
	unsigned char rombankLow(bool upper) const;
	unsigned char rombankMid() const;
	unsigned char rombankHigh() const;
	unsigned char rombankMask() const;
	unsigned char rambankLow(bool noMode) const;
	unsigned char rambankHigh() const;
	unsigned char rambankMask() const;
	bool isMuxed() const;
	void updateBanking() const;
};

class HuC1 : public DefaultMbc {
public:
	explicit HuC1(MemPtrs &memptrs, Infrared *const ir_);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned const p, unsigned const data, gb_ulong const /*cc*/);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual void SyncState(NewState *ns, bool isReader);

private:
	MemPtrs &memptrs_;
	Infrared *const ir_;
	unsigned char rombank_;
	unsigned char rambank_;
	unsigned char ramflag_;

	void setRambank(bool loadingState = false) const;
	void setRombank() const;
};

class HuC3 : public DefaultMbc {
public:
	explicit HuC3(MemPtrs &memptrs, HuC3Chip *const huc3);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned const p, unsigned const data, gb_ulong const /*cc*/);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual void SyncState(NewState *ns, bool isReader);

private:
	MemPtrs &memptrs_;
	HuC3Chip *const huc3_;
	unsigned char rombank_;
	unsigned char rambank_;
	unsigned char ramflag_;

	void setRambank(bool loadingState = false) const;
	void setRombank() const;
};

class PocketCamera : public DefaultMbc {
public:
	explicit PocketCamera(MemPtrs &memptrs, Camera *const camera);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned const p, unsigned const data, gb_ulong const /*cc*/);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual void SyncState(NewState *ns, bool isReader);

private:
	MemPtrs &memptrs_;
	Camera *const camera_;
	unsigned char rombank_;
	unsigned char rambank_;
	bool enableRam_;

	void setRambank() const;
	void setRombank() const;
};

class WisdomTree : public Mbc {
public:
	explicit WisdomTree(MemPtrs &memptrs);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned const p, unsigned const /*data*/, gb_ulong const /*cc*/);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual bool isAddressWithinAreaRombankCanBeMappedTo(unsigned addr, unsigned bank) const;
	virtual void SyncState(NewState *ns, bool isReader);

private:
	MemPtrs &memptrs_;
	unsigned char rombank_;

	void setRombank() const;
};

// armGBA: unlicensed cartridges that are MBC5 behind a lock (see unl.cpp)
class UnlMbc5 : public DefaultMbc {
public:
	enum Kind { bbd, hitek, ggb81, licheng, sintax, ntold1, ntold2, ntnew };
	// banks: the ROM's own; one more follows them in memory for a bank unpacked through the lock
	UnlMbc5(MemPtrs &memptrs, Kind kind, unsigned banks);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned p, unsigned data, gb_ulong cc);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual void SyncState(NewState *ns, bool isReader);

private:
	MemPtrs &memptrs_;
	Kind kind_;
	unsigned banks_;
	unsigned short rombank_;
	unsigned char rambank_;
	bool enableRam_;
	unsigned char dataMode_;
	unsigned char bankMode_;
	unsigned char sintaxMode_;
	unsigned char sintaxBank_;
	unsigned char xor_[4];
	// NT's multicarts: where the game picked starts and how many banks it has, and whether bank
	// numbers come with their bits out of order
	unsigned char base_;
	unsigned char count_;
	bool swapped_;
	// NT's later cartridges: the switchable area as two halves of 8 KiB each
	bool split_;
	unsigned short half_[2];
	// Whether the halves are mapped: a split takes effect at the next bank write
	bool splitMapped_;

	void setRambank() const;
	void setRombank();
	void mapRombank();
};

// armGBA: a cartridge chip that answers in the RAM area itself
class CartIo {
public:
	virtual ~CartIo() {}
	virtual unsigned read(unsigned p, gb_ulong cc) const = 0;
	virtual void write(unsigned p, unsigned data, gb_ulong cc) = 0;
	virtual void resetCc(gb_ulong, gb_ulong) {}
};

// armGBA: Bandai's TAMA5 (Game de Hakken!! Tamagotchi - Osutchi to Mesutchi)
class Tama5 : public Mbc, public CartIo {
public:
	explicit Tama5(MemPtrs &memptrs);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned p, unsigned data, gb_ulong cc);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual bool isAddressWithinAreaRombankCanBeMappedTo(unsigned addr, unsigned bank) const;
	virtual void SyncState(NewState *ns, bool isReader);
	virtual unsigned read(unsigned p, gb_ulong cc) const;
	virtual void write(unsigned p, unsigned data, gb_ulong cc);
	virtual void resetCc(gb_ulong oldCc, gb_ulong newCc);

private:
	MemPtrs &memptrs_;

	unsigned char * chip() const;
	void tick(gb_ulong cc) const;
	void setRombank() const;
};

// armGBA: a cartridge that answers some reads of its ROM itself
class RomHook {
public:
	virtual ~RomHook() {}
	virtual unsigned romRead(unsigned p) = 0;
	// What OAM DMA copies from the ROM area: what the CPU would read, without the side effects a CPU
	// read has on the cartridge
	virtual unsigned romDmaRead(unsigned p) { return romRead(p); }
};

// armGBA: a cartridge that answers some reads and writes of A000-BFFF itself, the rest going to
// the RAM its mapper shows there
class SramHook {
public:
	virtual ~SramHook() {}
	// Return whether the cartridge took the access
	virtual bool sramRead(unsigned p, unsigned &value) = 0;
	virtual bool sramWrite(unsigned p, unsigned data) = 0;
};

// armGBA: Vast Fame's protected cartridge (see vf001.cpp)
class Vf001 : public DefaultMbc, public RomHook {
public:
	// banks: the ROM's own, with two spare ones after them; maybeGgb81: the logo is one a GGB81
	// cartridge's game carries too
	Vf001(MemPtrs &memptrs, unsigned banks, bool maybeGgb81);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned p, unsigned data, gb_ulong cc);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual void SyncState(NewState *ns, bool isReader);
	virtual unsigned romRead(unsigned p);

private:
	MemPtrs &memptrs_;
	unsigned banks_;
	unsigned char start_;
	unsigned short rombank_;
	unsigned char rambank_;
	bool enableRam_;
	bool config_;
	unsigned char running_;
	unsigned char reg6000_;
	unsigned char reg700x_[11];
	unsigned char left_;
	// How many times the running value has turned since the lock was opened, and at which turn
	// each register was written: for telling the two kinds of cartridge apart (not kept in states)
	unsigned char turns_;
	unsigned char turnOf_[12];
	bool decided_;
	// Mojie Chuanshuo has the logo of Vast Fame's Fellowship of the Rings on a GGB81 cartridge:
	// until the lock is first opened, a write to 2001 picks GGB81's order of data bits
	bool opened_;
	bool maybeGgb81_;
	unsigned char dataMode_;

	unsigned length() const;
	void otherKind();
	void setBanks() const;
};

// armGBA: MBC7 (Kirby Tilt 'n' Tumble, Command Master) and MBC6 (Net de Get); see mbc7.cpp
class Mbc7 : public Mbc, public CartIo {
public:
	explicit Mbc7(MemPtrs &memptrs);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned p, unsigned data, gb_ulong cc);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual bool isAddressWithinAreaRombankCanBeMappedTo(unsigned addr, unsigned bank) const;
	virtual void SyncState(NewState *ns, bool isReader);
	virtual unsigned read(unsigned p, gb_ulong cc) const;
	virtual void write(unsigned p, unsigned data, gb_ulong cc);

private:
	MemPtrs &memptrs_;
	unsigned char rombank_;
	bool enable1_;
	bool enable2_;
	unsigned short x_;
	unsigned short y_;

	void eeprom(unsigned data);
	void setBanks() const;
};

class Mbc6 : public DefaultMbc {
public:
	Mbc6(MemPtrs &memptrs, unsigned banks);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned p, unsigned data, gb_ulong cc);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual void SyncState(NewState *ns, bool isReader);

private:
	MemPtrs &memptrs_;
	unsigned banks_;
	bool enableRam_;
	unsigned char rambank_;
	unsigned char half_[2];
	bool flash_[2];

	void setBanks() const;
};

// armGBA: the cartridge of the Telefang pirates sold as Pokemon Jade and Diamond: MBC3's banking,
// with two counters where the clock would be that the games use as a lock
class Pkjd : public Mbc, public CartIo {
public:
	explicit Pkjd(MemPtrs &memptrs);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned p, unsigned data, gb_ulong cc);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual bool isAddressWithinAreaRombankCanBeMappedTo(unsigned addr, unsigned bank) const;
	virtual void SyncState(NewState *ns, bool isReader);
	virtual unsigned read(unsigned p, gb_ulong cc) const;
	virtual void write(unsigned p, unsigned data, gb_ulong cc);

private:
	MemPtrs &memptrs_;
	unsigned char rombank_;
	unsigned char select_;
	unsigned char counters_[2];
	bool enableRam_;

	void setBanks() const;
};

// armGBA: the Mega Duck's cartridges, with their two ways to change banks
class MegaDuck : public Mbc, public CartIo {
public:
	// The cartridge kinds, as GB::LoadFlag's MEGA_DUCK_MAPPER bits number them
	enum Kind { automatic, none, md0, md1, md2, md20s, md25s };

	MegaDuck(MemPtrs &memptrs, Kind kind);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned p, unsigned data, gb_ulong cc);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual bool isAddressWithinAreaRombankCanBeMappedTo(unsigned addr, unsigned bank) const;
	virtual void SyncState(NewState *ns, bool isReader);
	virtual unsigned read(unsigned p, gb_ulong cc) const;
	virtual void write(unsigned p, unsigned data, gb_ulong cc);

private:
	MemPtrs &memptrs_;
	Kind kind_;
	unsigned short rombank0_;
	unsigned short rombank_;
	unsigned char rambank_;

	bool hasRam() const;
	void setBanks() const;
};

// armGBA: a cheat cartridge in front of the game on top of it, which takes the place of the game's
// mapper while that stays behind it: Datel's Action Replay (ActionReplayGb) or Galoob's Game Genie
// (GameGenieGb)
class CheatCartGb : public Mbc, public RomHook, public SramHook {
public:
	// Power on
	virtual void reset() = 0;
	// The cartridge's button and switch, where it has them
	virtual void setButton(bool) {}
	virtual void setSwitch(bool) {}
	// What it keeps with the power off, and whether that was written since this was last asked (none
	// by default)
	virtual unsigned char *kept(std::size_t *size) { *size = 0; return 0; }
	virtual bool flashChanged() { return false; }
	// What a state keeps of it, and of its flash chip
	virtual unsigned char *stateData() = 0;
	virtual std::size_t stateSize() const = 0;
	virtual unsigned char *flashData() { return 0; }
	virtual std::size_t flashSize() const { return 0; }
	// armGBA: a state holds the flash chip as it differs from the cartridge's own image, so that every
	// player has their own copy of the cartridge and none is sent in netplay. This turns the chip into
	// that and back (each call undoes the last).
	virtual void swapFactory() {}
	// The console's I/O and high RAM (FF00-FFFF at 0x100), for a cartridge that fills in its program's
	// variables
	virtual void setHram(unsigned char *) {}
	// Codes to put in the Game Genie's empty slots as its code screen shows: nine digits (0-15, 0x10
	// past the end) and a count of 6 or 9 (0 for none) for each of the three
	virtual void setCodes(unsigned char const *, unsigned char const *) {}
};

// armGBA: Datel's Action Replay for the Game Boy and Game Boy Color (see actionreplay.cpp)
class ActionReplayGb : public CheatCartGb {
public:
	// What a state keeps of it besides its flash chip, which it keeps too
	struct State {
		unsigned char regs[16];
		unsigned char ram[0x8000];
		unsigned char shown;
		unsigned char boot;
		unsigned char flashStep;
		unsigned char flashId;
		unsigned char flashProgram;
		// The first cartridge: the game's first 512 bytes show for the program to look at
		unsigned char lowPage;
	};

	// game: the game's mapper, which this takes; hasGame: whether a game is on top, else the cartridge
	// space behind it is empty
	ActionReplayGb(MemPtrs &memptrs, Mbc *game, RomHook *gameHook, unsigned char const *flash, std::size_t size, bool hasGame);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned p, unsigned data, gb_ulong cc);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual bool isAddressWithinAreaRombankCanBeMappedTo(unsigned addr, unsigned bank) const;
	virtual void SyncState(NewState *ns, bool isReader);
	virtual unsigned romRead(unsigned p);
	virtual unsigned romDmaRead(unsigned p);
	virtual bool sramRead(unsigned p, unsigned &value);
	virtual bool sramWrite(unsigned p, unsigned data);
	virtual void reset();
	virtual void setButton(bool pressed) { button_ = pressed; }
	virtual void setSwitch(bool on) { switchOn_ = on; }
	// What it keeps with the power off: its flash chip, or the first cartridge's RAM
	virtual unsigned char *kept(std::size_t *size);
	virtual bool flashChanged();
	virtual unsigned char *stateData() { return reinterpret_cast<unsigned char *>(&s_); }
	virtual std::size_t stateSize() const { return sizeof s_; }
	virtual unsigned char *flashData() { return &flash_[0]; }
	virtual void swapFactory() {
		for (std::size_t i = 0; i < flash_.size(); ++i)
			flash_[i] ^= factory_[i];
	}
	virtual std::size_t flashSize() const { return flash_.size(); }
	bool first() const { return first_; }

private:
	MemPtrs &memptrs_;
	scoped_ptr<Mbc> game_;
	RomHook *gameHook_;
	std::vector<unsigned char> flash_;
	// The flash chip as built, before the cartridge's save went over it
	std::vector<unsigned char> factory_;
	bool hasGame_;
	bool first_;
	bool button_;
	bool switchOn_;
	bool flashChanged_;
	State s_;

	bool armed() const;
	bool listening() const;
	std::size_t flashAddress(unsigned p) const;
	unsigned gameRead(unsigned p);
	unsigned char &ram(unsigned p);
	unsigned status() const;
	void flashWrite(std::size_t at, unsigned data);
};

// armGBA: Blaze's Xploder GB (see xploder.cpp)
class XploderGb : public CheatCartGb {
public:
	struct State {
		// 0000-0007
		unsigned char regs[8];
		// Its RAM: 16 banks of 8 KiB
		unsigned char ram[0x20000];
		// Whether the Xploder shows, rather than the game
		unsigned char shown;
		// From power on until the program first writes a register, the header is the game's
		unsigned char boot;
		// The last byte the game wrote to 4000-5FFF, which 0004 reads
		unsigned char gameBank;
		unsigned char flashStep;
		unsigned char flashId;
		// Loading a page of the flash chip: 1 after the command, 2 once its first byte is written,
		// for the page pageHigh:pageLow
		unsigned char flashLoad;
		unsigned char pageHigh;
		unsigned char pageLow;
	};

	XploderGb(MemPtrs &memptrs, Mbc *game, RomHook *gameHook, unsigned char const *program, std::size_t size, bool hasGame);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned p, unsigned data, gb_ulong cc);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual bool isAddressWithinAreaRombankCanBeMappedTo(unsigned addr, unsigned bank) const;
	virtual void SyncState(NewState *ns, bool isReader);
	virtual unsigned romRead(unsigned p);
	virtual unsigned romDmaRead(unsigned p);
	virtual bool sramRead(unsigned p, unsigned &value);
	virtual bool sramWrite(unsigned p, unsigned data);
	virtual void reset();
	virtual void setButton(bool pressed) { button_ = pressed; }
	// What it keeps with the power off: its flash chip
	virtual unsigned char *kept(std::size_t *size) { *size = flash_.size(); return &flash_[0]; }
	virtual bool flashChanged();
	virtual unsigned char *stateData() { return reinterpret_cast<unsigned char *>(&s_); }
	virtual std::size_t stateSize() const { return sizeof s_; }
	virtual unsigned char *flashData() { return &flash_[0]; }
	virtual void swapFactory() {
		for (std::size_t i = 0; i < flash_.size(); ++i)
			flash_[i] ^= factory_[i];
	}
	virtual std::size_t flashSize() const { return flash_.size(); }

	// Whether a cartridge program is the Xploder's
	static bool is(unsigned char const *program, std::size_t size);

private:
	MemPtrs &memptrs_;
	scoped_ptr<Mbc> game_;
	RomHook *gameHook_;
	std::vector<unsigned char> flash_;
	// The flash chip as built, before the cartridge's save went over it
	std::vector<unsigned char> factory_;
	bool hasGame_;
	bool button_;
	bool flashChanged_;
	State s_;

	unsigned gameRead(unsigned p);
	std::size_t flashAddress(unsigned p) const;
	unsigned flashRead(std::size_t at);
	void flashWrite(std::size_t at, unsigned data);
};
class GameGenieGb : public CheatCartGb {
public:
	struct State {
		// 4000: what shows, 4001: the codes on, 4003-4011: the three codes, 4012 and 4013
		unsigned char regs[0x14];
		// From power on until the program first writes a register, the header is the game's
		unsigned char boot;
		// armGBA: the codes chosen in the core options (setCodes), in the state so that every device in
		// netplay fills the code screen with the host's: nine digits a slot, and each slot's length
		unsigned char fill[27];
		unsigned char fillCount[3];
	};

	// program: the Game Genie's own ROM; game: the game's mapper, which this takes; hasGame: whether a
	// game is on top
	GameGenieGb(MemPtrs &memptrs, Mbc *game, RomHook *gameHook, unsigned char const *program, std::size_t size, bool hasGame);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned p, unsigned data, gb_ulong cc);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual bool isAddressWithinAreaRombankCanBeMappedTo(unsigned addr, unsigned bank) const;
	virtual void SyncState(NewState *ns, bool isReader);
	virtual unsigned romRead(unsigned p);
	virtual unsigned romDmaRead(unsigned p);
	virtual bool sramRead(unsigned, unsigned &) { return false; }
	virtual bool sramWrite(unsigned, unsigned) { return false; }
	virtual void reset();
	virtual unsigned char *stateData() { return reinterpret_cast<unsigned char *>(&s_); }
	virtual std::size_t stateSize() const { return sizeof s_; }
	virtual void setHram(unsigned char *ioamhram) { hram_ = ioamhram; }
	virtual void setCodes(unsigned char const *digits, unsigned char const *counts);

	// Whether a cartridge program is the Game Genie's
	static bool is(unsigned char const *program, std::size_t size);

private:
	MemPtrs &memptrs_;
	scoped_ptr<Mbc> game_;
	RomHook *gameHook_;
	std::vector<unsigned char> program_;
	bool hasGame_;
	unsigned char *hram_;
	State s_;

	void fillCodes();

	bool running() const;
	unsigned gameRead(unsigned p);
	unsigned patched(unsigned p, unsigned value) const;
};

class Sachen : public Mbc {
public:
	explicit Sachen(MemPtrs &memptrs);
	virtual bool disabledRam() const;
	virtual void romWrite(unsigned const p, unsigned const data, gb_ulong const /*cc*/);
	virtual void saveState(SaveState::Mem &ss) const;
	virtual void loadState(SaveState::Mem const &ss);
	virtual bool isAddressWithinAreaRombankCanBeMappedTo(unsigned addr, unsigned bank) const;
	virtual void SyncState(NewState *ns, bool isReader);
	// Where the cartridge's scrambled address lines take a read of the header page
	static unsigned unscramble(unsigned p);

private:
	MemPtrs &memptrs_;
	unsigned char bank_;
	unsigned char base_;
	unsigned char mask_;

	void setBanks() const;
};

}

#endif
