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

#ifndef CARTRIDGE_H
#define CARTRIDGE_H

#include "gbint.h"
#include "mbc/mbc.h"
#include "loadres.h"
#include "savestate.h"
#include "scoped_ptr.h"
#include "array.h"
#include "newstate.h"

#include <string>
#include <vector>

namespace gambatte {

class Cartridge {
public:
	Cartridge();
	void setStatePtrs(SaveState &);
	void saveState(SaveState &, gb_ulong cycleCounter);
	void loadState(SaveState const &);
	bool loaded() const { return mbc_.get(); }
	unsigned char const * rmem(unsigned area) const { return memptrs_.rmem(area); }
	unsigned char * wmem(unsigned area) const { return memptrs_.wmem(area); }
	unsigned char * vramdata() const { return memptrs_.vramdata(); }
	unsigned char * romdata(unsigned area) const { return memptrs_.romdata(area); }
	unsigned char * wramdata(unsigned area) const { return memptrs_.wramdata(area); }
	unsigned char const * rdisabledRam() const { return memptrs_.rdisabledRam(); }
	unsigned char const * rsrambankptr() const { return memptrs_.rsrambankptr(); }
	unsigned char * wsrambankptr() const { return memptrs_.wsrambankptr(); }
	unsigned char * vrambankptr() const { return memptrs_.vrambankptr(); }
	OamDmaSrc oamDmaSrc() const { return memptrs_.oamDmaSrc(); }
	bool isInOamDmaConflictArea(unsigned p) const { return memptrs_.isInOamDmaConflictArea(p); }
	unsigned getBank(unsigned type) { return memptrs_.getBank(type); }
	unsigned getAddrBank(unsigned short addr) { return memptrs_.getAddrBank(addr); }
	void setBank(unsigned type, unsigned bank) { memptrs_.setBank(type, bank); }
	void setAddrBank(unsigned short addr, unsigned bank) { memptrs_.setAddrBank(addr, bank); }
	void setVrambank(unsigned bank) { memptrs_.setVrambank(bank); }
	void setWrambank(unsigned bank) { memptrs_.setWrambank(bank); }
	void setOamDmaSrc(OamDmaSrc oamDmaSrc) { memptrs_.setOamDmaSrc(oamDmaSrc); }
	bool disabledRam() const { return mbc_->disabledRam(); }
	void mbcWrite(unsigned addr, unsigned data, gb_ulong const cc) { mbc_->romWrite(addr, data, cc); }
	bool isCgb() const { return gambatte::isCgb(memptrs_); }
	void resetCc(gb_ulong const oldCc, gb_ulong const newCc) {
		time_.resetCc(oldCc, newCc);
		huc3_.resetCc(oldCc, newCc);
		ir_.resetCc(oldCc, newCc);
		camera_.resetCc(oldCc, newCc);
		if (io_)
			io_->resetCc(oldCc, newCc);
	}
	void speedChange(gb_ulong const cc) {
		time_.speedChange(cc);
		huc3_.speedChange(cc);
		ir_.speedChange(cc);
		camera_.speedChange(cc);
	}
	void setTimeMode(bool useCycles, gb_ulong const cc) { time_.setTimeMode(useCycles, cc); }
	void setRtcDivisorOffset(gb_long const rtcDivisorOffset) { time_.setRtcDivisorOffset(rtcDivisorOffset); }
	void setRtcSpeed(unsigned percent, gb_ulong const cc) { time_.setSpeed(percent, cc); }
	bool deviceTime() const { return time_.deviceTime(); }
	void setDeviceTime(bool device, gb_ulong const cc) { time_.setDeviceTime(device, cc); }
	void setTime(unsigned long long dividers) { time_.setTime(dividers); }
	void rtcWrite(unsigned data, gb_ulong const cc) { rtc_.write(data, cc); }
	unsigned char rtcRead() const { return rtc_.activeLatch() ? *rtc_.activeLatch() : 0xFF; }
	void loadSavedata(gb_ulong cycleCounter);
	void saveSavedata(gb_ulong cycleCounter);
	unsigned getSavedataLength();
	void loadSavedata(char const *data, gb_ulong cycleCounter);
	void saveSavedata(char *dest, gb_ulong cycleCounter);
	bool getMemoryArea(int which, unsigned char **data, int *length) const;
	// armGBA: the Mega Duck laptop's slot (see GB::swapMegaDuckCartridge)
	bool swapMegaDuckRom(unsigned char const *data, std::size_t size, unsigned kind);
	// armGBA: puts a cheat cartridge in front of the cartridge loaded, by its program: Galoob's Game Genie
	// (GameGenieGb) or Datel's Action Replay (ActionReplayGb); hasGame: whether that is a game, else
	// nothing is on top
	bool attachCheatCart(unsigned char const *program, std::size_t size, bool hasGame);
	CheatCartGb *cheatCart() const { return cheatCart_; }
	// armGBA: power on for a chip of the cartridge's own that a state's reset doesn't reach
	void resetChip() { if (cheatCart_) cheatCart_->reset(); }
	std::string const saveBasePath() const;
	void setSaveDir(std::string const &dir);
	// armGBA: megaDuck is 0 for a Game Boy cartridge, else 1 + its MegaDuck::Kind
	LoadRes loadROM(Array<unsigned char> &buffer, bool cgbMode, unsigned megaDuck, std::string const &filepath);
	char const * romTitle() const { return reinterpret_cast<char const *>(romHeader + 0x134); }
	// armGBA: the logo a Sachen multicart shows the boot ROM while its lock is closed, or none
	unsigned char const * sachenLogo() const { return sachen_ ? sachenLogo_ : 0; }
	class PakInfo const pakInfo() const;
	void setGameGenie(std::string const &codes);
	bool isMbc2() const { return mbc2_; }
	bool isHuC1() const { return huc1_; }
	// armGBA: a cartridge that answers some reads of its ROM itself (Vast Fame's)
	bool hasRomHook() const { return romHook_; }
	unsigned romHookRead(unsigned p) { return romHook_->romRead(p); }
	unsigned romHookDmaRead(unsigned p) { return romHook_->romDmaRead(p); }
	bool sramHookRead(unsigned p, unsigned &value) { return sramHook_ && sramHook_->sramRead(p, value); }
	bool sramHookWrite(unsigned p, unsigned data) { return sramHook_ && sramHook_->sramWrite(p, data); }
	// armGBA: a chip of the cartridge's own in the RAM area (TAMA5, the Pokemon Jade pirates')
	bool hasIo() const { return io_; }
	unsigned ioRead(unsigned p, gb_ulong cc) const { return io_->read(p, cc); }
	void ioWrite(unsigned p, unsigned data, gb_ulong cc) { io_->write(p, data, cc); }
	bool getIrTrigger() const { return ir_.getIrTrigger(); }
	void ackIrTrigger() { ir_.ackIrTrigger(); }
	bool getIrSignal(Infrared::WhichIrGb which, gb_ulong const cc) { return ir_.getIrSignal(which, cc); }
	void setIrSignal(Infrared::WhichIrGb which, bool signal) { ir_.setIrSignal(which, signal); }
	void setRemoteCallback(unsigned char (*callback)()) { ir_.setRemoteCallback(callback); }
	void setRemoteActive(bool active) { ir_.setRemoteActive(active); }
	bool isHuC3() const { return huc3_.isHuC3(); }
	// armGBA: an MBC3 clock's registers, in the order of its .rtc file (rtc.h), for the frontend to keep
	bool hasMbc3Rtc() const;
	void getRtcRegs(gb_ulong *dest, gb_ulong const cc) { rtc_.getRtcRegs(dest, cc); }
	void setRtcRegs(gb_ulong *src, gb_ulong const cc) { rtc_.updateClock(cc); rtc_.setRtcRegs(src); }
	// armGBA: the HuC3 chip whole, its clock with it (huc3_chip.h), likewise
	void getHuC3Regs(unsigned char *dest, gb_ulong const cc) { huc3_.getHuC3Regs(dest, cc); }
	void setHuC3Regs(unsigned char *src, gb_ulong const cc) { huc3_.updateClock(cc); huc3_.setHuC3Regs(src); }
	void accumulateSamples(gb_ulong const cc) { huc3_.accumulateSamples(cc); }
	unsigned generateSamples(short *soundbuf) { return huc3_.generateSamples(soundbuf); }
	unsigned char HuC3Read(unsigned p, gb_ulong const cc) { return huc3_.read(p, cc); }
	void HuC3Write(unsigned p, unsigned data, gb_ulong const cc) { huc3_.write(p, data, cc); }
	bool isPocketCamera() const { return pocketCamera_; }
	bool cameraIsActive(gb_ulong cc) { return camera_.cameraIsActive(cc); }
	unsigned char cameraRead(unsigned p, gb_ulong const cc) { return camera_.read(p, cc); }
	void cameraWrite(unsigned p, unsigned data, gb_ulong const cc) { camera_.write(p, data, cc); }
	void setCameraCallback(void (*callback)(int *cameraBuf)) { camera_.setCameraCallback(callback); }
	template<bool isReader>void SyncState(NewState *ns);

private:
	unsigned char romHeader[0x150];
	bool sachen_ = false;
	unsigned char sachenLogo_[0x30];
	bool mbc2_;
	bool huc1_;
	CartIo *io_ = 0;
	RomHook *romHook_ = 0;
	CheatCartGb *cheatCart_ = 0;
	SramHook *sramHook_ = 0;
	bool pocketCamera_;
	struct AddrData {
		gb_ulong addr;
		unsigned char data;
		AddrData(gb_ulong addr, unsigned data) : addr(addr), data(data) {}
	};

	MemPtrs memptrs_;
	Time time_;
	Rtc rtc_;
	Infrared ir_;
	HuC3Chip huc3_;
	Camera camera_;
	scoped_ptr<Mbc> mbc_;
	std::string defaultSaveBasePath_;
	std::string saveDir_;
	std::vector<AddrData> ggUndoList_;

	void applyGameGenie(std::string const &code);
};

}

#endif
