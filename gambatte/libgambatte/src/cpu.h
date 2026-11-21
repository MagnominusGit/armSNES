//
//   Copyright (C) 2007 by sinamas <sinamas at users.sourceforge.net>
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

#ifndef CPU_H
#define CPU_H

#include "gbint.h"
#include "memory.h"
#include "newstate.h"

namespace gambatte {

class CPU {
public:
	CPU();
	gb_long runFor(gb_ulong cycles);
	void setStatePtrs(SaveState &state);
	void saveState(SaveState &state);
	void loadState(SaveState const &state);
	void setLayers(unsigned mask) { mem_.setLayers(mask); }
	unsigned getSavedataLength() { return mem_.getSavedataLength(); }
	void loadSavedata(char const *data) { mem_.loadSavedata(data, cycleCounter_); }
	void saveSavedata(char *dest) { mem_.saveSavedata(dest, cycleCounter_); }
	void loadSavedata() { mem_.loadSavedata(cycleCounter_); }
	void saveSavedata() { mem_.saveSavedata(cycleCounter_); }

	bool getMemoryArea(int which, unsigned char **data, int *length) { return mem_.getMemoryArea(which, data, length); }
	bool swapMegaDuckCartridge(unsigned char const *data, std::size_t size, unsigned kind) { return mem_.swapMegaDuckCartridge(data, size, kind); }
	bool attachCheatCart(unsigned char const *program, std::size_t size, bool hasGame) { return mem_.attachCheatCart(program, size, hasGame); }
	CheatCartGb *cheatCart() const { return mem_.cheatCart(); }
	void resetCartChip() { mem_.resetCartChip(); }

	void setVideoBuffer(uint_least32_t *videoBuf, std::ptrdiff_t pitch) {
		mem_.setVideoBuffer(videoBuf, pitch);
	}

	unsigned updateScreenBorder(uint_least32_t *videoBuf, std::ptrdiff_t pitch) {
		return mem_.updateScreenBorder(videoBuf, pitch);
	}

	unsigned generateSgbSamples(short *soundBuf, std::size_t &samples) {
		return mem_.generateSgbSamples(soundBuf, samples);
	}

	unsigned generateMbcSamples(short *soundBuf) {
		return mem_.generateMbcSamples(soundBuf);
	}

	void setInputGetter(InputGetter *getInput, void *p) {
		mem_.setInputGetter(getInput, p);
	}

	// armSNES
	void setIcd(unsigned (*joyp)(void *, unsigned, unsigned), void (*line)(void *, unsigned, uint_least32_t const *), void *p) { mem_.setIcd(joyp, line, p); }
	void updateVideo() { mem_.updateVideo(cycleCounter_); }

	unsigned getJoypadIndex() {
		return mem_.getJoypadIndex();
	}

	void setReadCallback(MemoryCallback callback) {
		mem_.setReadCallback(callback);
	}

	void setWriteCallback(MemoryCallback callback) {
		mem_.setWriteCallback(callback);
	}

	void setExecCallback(MemoryCallback callback) {
		mem_.setExecCallback(callback);
	}

	void setCDCallback(CDCallback cdc) {
		mem_.setCDCallback(cdc);
	}

	void setTraceCallback(void (*callback)(void *)) {
		traceCallback_ = callback;
	}

	void setScanlineCallback(void (*callback)(), int sl) {
		mem_.setScanlineCallback(callback, sl);
	}

	void setLinkCallback(void (*callback)()) {
		mem_.setLinkCallback(callback);
	}

	void setCameraCallback(void (*callback)(int *cameraBuf)) {
		mem_.setCameraCallback(callback);
	}

	void setRemoteCallback(unsigned char (*callback)()) {
		mem_.setRemoteCallback(callback);
	}

	void setSaveDir(std::string const &sdir) {
		mem_.setSaveDir(sdir);
	}

	std::string const saveBasePath() const {
		return mem_.saveBasePath();
	}

	void setOsdElement(transfer_ptr<OsdElement> osdElement) {
		mem_.setOsdElement(osdElement);
	}

	LoadRes load(Array<unsigned char> &buffer, unsigned flags, std::string const &filepath) {
		return mem_.loadROM(buffer, flags, filepath);
	}

	bool loaded() const { return mem_.loaded(); }
	char const * romTitle() const { return mem_.romTitle(); }
	unsigned char const * sachenLogo() const { return mem_.sachenLogo(); }
	PakInfo const pakInfo() const { return mem_.pakInfo(); }
	void setSoundBuffer(uint_least32_t *buf) { mem_.setSoundBuffer(buf); }
	std::size_t fillSoundBuffer() { return mem_.fillSoundBuffer(cycleCounter_); }
	void stall(gb_ulong cycles) { mem_.stall(cycleCounter_, cycles); }
	bool isCgb() const { return mem_.isCgb(); }
	bool inBootRom() const { return mem_.inBootRom(); }
	bool isCgbDmg() const { return mem_.isCgbDmg(); }
	bool isSgb() const { return mem_.isSgb(); }

	void setDmgPaletteColor(int palNum, int colorNum, gb_ulong rgb32) {
		mem_.setDmgPaletteColor(palNum, colorNum, rgb32);
	}

	void setCgbPalette(unsigned *lut) { mem_.setCgbPalette(lut); }
	void setTimeMode(bool useCycles) { mem_.setTimeMode(useCycles, cycleCounter_); }
	void setRtcDivisorOffset(gb_long const rtcDivisorOffset) { mem_.setRtcDivisorOffset(rtcDivisorOffset); }
	void setRtcSpeed(unsigned percent) { mem_.setRtcSpeed(percent, cycleCounter_); }
	bool deviceTime() const { return mem_.deviceTime(); }
	void setDeviceTime(bool device) { mem_.setDeviceTime(device, cycleCounter_); }
	bool hasMbc3Rtc() const { return mem_.hasMbc3Rtc(); }
	void getRtcRegs(gb_ulong *dest) { mem_.getRtcRegs(dest, cycleCounter_); }
	void setRtcRegs(gb_ulong *src) { mem_.setRtcRegs(src, cycleCounter_); }
	bool isHuC3() const { return mem_.isHuC3(); }
	void getHuC3Regs(unsigned char *dest) { mem_.getHuC3Regs(dest, cycleCounter_); }
	void setHuC3Regs(unsigned char *src) { mem_.setHuC3Regs(src, cycleCounter_); }
	
	void setCartBusPullUpTime(gb_ulong const cartBusPullUpTime) { mem_.setCartBusPullUpTime(cartBusPullUpTime); }

	void setGameGenie(std::string const &codes) { mem_.setGameGenie(codes); }
	void setGameShark(std::string const &codes) { mem_.setGameShark(codes); }
	void setCpuQuarters(unsigned quarters) { mem_.setCpuQuarters(quarters); }
	void setBios(Array<unsigned char> &buffer) { mem_.setBios(buffer); }

	unsigned char externalRead(unsigned short addr) {
		return mem_.read<true, false, false, false>(addr, cycleCounter_);
	}

	void externalWrite(unsigned short addr, unsigned char val) {
		mem_.write<true, false>(addr, val, cycleCounter_);
	}

	// 268: cycles until the serial transfer under way ends, or -1.
	int linkStatus(int which) { return which == 268 ? mem_.serialLeft(cycleCounter_) : mem_.linkStatus(which); }

	unsigned getBank(unsigned type) { return mem_.getBank(type); }
	unsigned getAddrBank(unsigned short addr) { return mem_.getAddrBank(addr); }

	void setBank(unsigned type, unsigned bank) { mem_.setBank(type, bank); }
	void setAddrBank(unsigned short addr, unsigned bank) { mem_.setAddrBank(addr, bank); }

	void getRegs(int *dest);
	void setRegs(int *src);
	void setInterruptAddresses(int *addrs, int numAddrs);
	int getHitInterruptAddress();

	unsigned long long timeNow() const { return mem_.timeNow(); }
	void setTime(unsigned long long dividers) { mem_.setTime(dividers); }

	gb_ulong getCycleCounter() { return cycleCounter_; }
	gb_ulong getDivLastUpdate() { return mem_.getDivLastUpdate(); }
	unsigned char getRawIOAMHRAM(int offset) { return mem_.getRawIOAMHRAM(offset); }

	void setSpeedupFlags(unsigned flags) { mem_.setSpeedupFlags(flags); }

	template<bool isReader>void SyncState(NewState *ns);

private:
	Memory mem_;
	gb_ulong cycleCounter_;
	unsigned short pc;
	unsigned short sp;
	unsigned hf1, hf2, zf, cf;
	unsigned char a, b, c, d, e, /*f,*/ h, l;
	unsigned char opcode_;
	bool prefetched_;

	int *interruptAddresses;
	int numInterruptAddresses;
	int hitInterruptAddress;

	template <bool callbacksActive> void process(gb_ulong cycles);

	void (*traceCallback_)(void *);
};

}

#endif
