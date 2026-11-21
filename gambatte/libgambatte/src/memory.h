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

#ifndef MEMORY_H
#define MEMORY_H

#include "gbint.h"
#include "mem/cartridge.h"
#include "mem/sgb.h"
#include "inputgetter.h"
#include "interrupter.h"
#include "pakinfo.h"
#include "sound.h"
#include "tima.h"
#include "video.h"
#include "gambatte.h"
#include "newstate.h"

#include <cstring>

namespace gambatte {

class FilterInfo;

class Memory {
public:
	explicit Memory(Interrupter const &interrupter);
	bool loaded() const { return cart_.loaded(); }
	unsigned getBank(unsigned type) { return cart_.getBank(type); }
	unsigned getAddrBank(unsigned short addr) { return cart_.getAddrBank(addr); }
	void setBank(unsigned type, unsigned bank) { cart_.setBank(type, bank); }
	void setAddrBank(unsigned short addr, unsigned bank) { cart_.setAddrBank(addr, bank); }
	char const * romTitle() const { return cart_.romTitle(); }
	unsigned char const * sachenLogo() const { return cart_.sachenLogo(); }
	PakInfo const pakInfo() const { return cart_.pakInfo(); }
	void setStatePtrs(SaveState &state);
	gb_ulong saveState(SaveState &state, gb_ulong cc);
	void loadState(SaveState const &state);
	void loadSavedata(gb_ulong const cc) { cart_.loadSavedata(cc); }
	void saveSavedata(gb_ulong const cc) { cart_.saveSavedata(cc); }
	unsigned getSavedataLength() { return cart_.getSavedataLength(); }
	void loadSavedata(char const *data, gb_ulong const cc) { cart_.loadSavedata(data, cc); }
	void saveSavedata(char *dest, gb_ulong const cc) { cart_.saveSavedata(dest, cc); }

	std::string const saveBasePath() const { return cart_.saveBasePath(); }

	void setOsdElement(transfer_ptr<OsdElement> osdElement) {
		lcd_.setOsdElement(osdElement);
	}

	bool getMemoryArea(int which, unsigned char **data, int *length);
	bool swapMegaDuckCartridge(unsigned char const *data, std::size_t size, unsigned kind) { return cart_.swapMegaDuckRom(data, size, kind); }
	bool attachCheatCart(unsigned char const *program, std::size_t size, bool hasGame) {
		bool const attached = cart_.attachCheatCart(program, size, hasGame);
		if (attached && cart_.cheatCart())
			cart_.cheatCart()->setHram(ioamhram_);
		return attached;
	}
	CheatCartGb *cheatCart() const { return cart_.cheatCart(); }
	void resetCartChip() { cart_.resetChip(); }

	gb_ulong stop(gb_ulong cycleCounter, bool &prefetched);
	void stall(gb_ulong cycleCounter, gb_ulong cycles);
	bool isCgb() const { return lcd_.isCgb(); }
	bool inBootRom() const { return biosMode_; }
	// armGBA: the CPU Clock, in quarters of the console's own (4). The CPU's machine cycles take
	// 16 / quarters of the counter's cycles, any part left over carried to the next; the rest of the
	// console keeps the counter's pace. In double speed a dot is two of the counter's cycles: there
	// the CPU moves a whole dot at a time and goes no faster than 200%, as the picture can't take
	// more than one access of the CPU's in a dot.
	void setCpuQuarters(unsigned quarters) {
		if (quarters != cpuQuarters_) {
			cpuQuarters_ = quarters;
			cpuCarry_ = 0;
		}
	}
	gb_ulong cpuCycles(unsigned mcycles) {
		if (cpuQuarters_ == 4)
			return 4 * mcycles;
		if (stockLeft_) {
			stockLeft_ = stockLeft_ > 4 * mcycles ? stockLeft_ - 4 * mcycles : 0;
			return 4 * mcycles;
		}
		unsigned const quarters = cpuQuartersNow(), unit = isDoubleSpeed() ? 2 : 1;
		cpuCarry_ += 16 * mcycles;
		gb_ulong const cycles = cpuCarry_ / (unit * quarters) * unit;
		cpuCarry_ -= cycles * quarters;
		return cycles;
	}
	unsigned cpuQuartersNow() const { return isDoubleSpeed() && cpuQuarters_ > 8 ? 8 : cpuQuarters_; }
	// armGBA: a game reading STAT or LY may be timing its code against the picture, counting the CPU's
	// cycles to land in a blanking period or between two reads of a mode change; for two lines after,
	// the CPU runs at the console's own pace, so such loops meet the picture as on the console.
	void cpuTimingPicture() {
		if (cpuQuarters_ != 4)
			stockLeft_ = 2 * 456 << isDoubleSpeed();
	}
	// armGBA: OAM DMA moves a byte per machine cycle of the CPU, so it keeps pace with code timed
	// to wait for it; at a clock between, it is a little ahead of the CPU, never behind.
	unsigned oamDmaStep() const {
		if (cpuQuarters_ == 4)
			return 4;
		return isDoubleSpeed() ? 2 * (8 / cpuQuartersNow()) : 16 / cpuQuarters_;
	}
	bool isCgbDmg() const { return lcd_.isCgbDmg(); }
	bool isSgb() const { return gbIsSgb_; }
	bool ime() const { return intreq_.ime(); }
	bool halted() const { return intreq_.halted(); }
	gb_ulong nextEventTime() const { return intreq_.minEventTime(); }
	void setLayers(unsigned mask) { lcd_.setLayers(mask); }
	bool isActive() const { return intreq_.eventTime(intevent_end) != disabled_time; }

	gb_long cyclesSinceBlit(gb_ulong cc) const {
		if (cc < intreq_.eventTime(intevent_blit))
			return -1;

		return (cc - intreq_.eventTime(intevent_blit)) >> isDoubleSpeed();
	}

	void freeze(gb_ulong cc);
	bool halt(gb_ulong cc);
	void ei(gb_ulong cycleCounter) { if (!ime()) { intreq_.ei(cycleCounter); } }
	void di() { intreq_.di(); }
	unsigned pendingIrqs(gb_ulong cc);
	void ackIrq(unsigned bit, gb_ulong cc);

	unsigned readBios(unsigned p) {
		return bios_.get()[p];
	}

	template <bool callbacksActive>
	unsigned ff_read(unsigned p, gb_ulong cc) {
		if (callbacksActive && readCallback_)
			readCallback_(p, callbackCycleOffset(cc));

		if (p >= 0x80)
			return ioamhram_[p + 0x100];

		return megaDuck_ ? megaDuckFfRead(p, cc) : nontrivial_ff_read(p, cc);
	}

	// armGBA: an I/O register by its Game Boy address, on a Mega Duck too (the machine's own reads)
	unsigned dmgFfRead(unsigned p, gb_ulong cc) {
		return p < 0x80 ? nontrivial_ff_read(p, cc) : ioamhram_[p + 0x100];
	}

	struct CDMapResult {
		eCDLog_AddrType type;
		unsigned addr;
	};

	CDMapResult CDMap(const unsigned p) const {
		if (p < mm_rom1_begin) {
			CDMapResult ret = { eCDLog_AddrType_ROM, p };
			return ret;
		} else if (p < mm_vram_begin) {
			unsigned bank = cart_.rmem(p >> 12) - cart_.rmem(0);
			unsigned addr = p + bank;
			CDMapResult ret = { eCDLog_AddrType_ROM, addr };
			return ret;
		} else if (p < mm_sram_begin) {
		} else if (p < mm_wram_begin) {
			if (cart_.wsrambankptr()) {
				//not bankable. but. we're not sure how much might be here
				unsigned char *data;
				int length;
				bool has = cart_.getMemoryArea(3, &data, &length);
				unsigned addr = p & (length - 1);
				if (has && length) {
					CDMapResult ret = { eCDLog_AddrType_CartRAM, addr };
					return ret;
				}
			}
		} else if (p < mm_wram_mirror_begin) {
			unsigned bank = cart_.wramdata(p >> 12 & 1) - cart_.wramdata(0);
			unsigned addr = (p & 0xFFF) + bank;
			CDMapResult ret = { eCDLog_AddrType_WRAM, addr };
			return ret;
		} else if (p < mm_hram_begin) {
		} else {
			////this is just for debugging, really, it's pretty useless
			//CDMapResult ret = { eCDLog_AddrType_HRAM, (p - mm_hram_begin) };
			//return ret;
		}
		CDMapResult ret = { eCDLog_AddrType_None, p };
		return ret;
	}

	template <bool peek, bool execute, bool opcode, bool callbacksActive>
	unsigned read(unsigned p, gb_ulong cc) {
		if (!peek && callbacksActive && !execute && readCallback_)
			readCallback_(p, callbackCycleOffset(cc));
		else if (!peek && callbacksActive && execute && opcode && execCallback_)
			execCallback_(p, callbackCycleOffset(cc));

		if (biosMode_ && p < bios_.size() && !(p >= 0x100 && p < 0x200))
			return readBios(p);
		else if (!peek && callbacksActive && cdCallback_) {
			CDMapResult map = CDMap(p);
			if (map.type != eCDLog_AddrType_None) {
				eCDLog_Flags flags;
				if (execute && opcode)
					flags = eCDLog_Flags_ExecOpcode;
				else if (execute)
					flags = eCDLog_Flags_ExecOperand;
				else
					flags = eCDLog_Flags_Data;

				cdCallback_(map.addr, map.type, flags);
			}
		}

		unsigned char const *const rmem = cart_.rmem(p >> 12);
		unsigned (Memory::*nontrivial_handling)(unsigned, gb_ulong) = peek ? &Memory::nontrivial_peek : &Memory::nontrivial_read;
		switch (p >> 13 & 7) {
			case 0:
			case 1:
			case 2:
			case 3:
				if (!peek) {
					cartBus_ = rmem ? rmem[p] : (this->*nontrivial_handling)(p, cc);
					lastCartBusUpdate_ = cc;
					return cartBus_;
				}
				// fallthrough
			case 4:
				return rmem ? rmem[p] : (this->*nontrivial_handling)(p, cc);
			case 5:
				if (!cart_.disabledRam() & !cart_.isMbc2() & !cart_.isPocketCamera())
					return rmem ? rmem[p] : (this->*nontrivial_handling)(p, cc);
				else if (cart_.disabledRam())
					return rmem ? cartBus(cc) : (this->*nontrivial_handling)(p, cc);
				else if (cart_.isMbc2())
					return rmem ? ((rmem[p & 0xA1FF] & 0x0F) | (cartBus(cc) & 0xF0)) : (this->*nontrivial_handling)(p & 0xA1FF, cc);
				else if (!peek && cart_.isPocketCamera() && cart_.cameraIsActive(cc))
					return rmem ? 0x00 : (this->*nontrivial_handling)(p, cc);
				else
					return rmem ? rmem[p] : (this->*nontrivial_handling)(p, cc);
			case 6:
				if (!peek && !isCgb()) {
					cartBus_ = rmem ? rmem[p] : (this->*nontrivial_handling)(p, cc);
					lastCartBusUpdate_ = cc;
					return cartBus_;
				}
				return rmem ? rmem[p] : (this->*nontrivial_handling)(p, cc);
			case 7:
				if (!peek && !isCgb() && p < mm_oam_begin) {
					cartBus_ = rmem ? rmem[p] : (this->*nontrivial_handling)(p, cc);
					lastCartBusUpdate_ = cc;
					return cartBus_;
				}
				// fallthrough
			default:
				return rmem ? rmem[p] : (this->*nontrivial_handling)(p, cc);
		}
	}

	template <bool poke, bool callbacksActive>
	void write(unsigned p, unsigned data, gb_ulong cc) {
		if (cart_.isMbc2() && (p >= mm_sram_begin && p < mm_wram_begin))
			p &= 0xA1FF;

		if (cart_.wmem(p >> 12))
			cart_.wmem(p >> 12)[p] = data;
		else
			nontrivial_write(p, data, cc);

		if (!poke && callbacksActive) {
			if (writeCallback_)
				writeCallback_(p, callbackCycleOffset(cc));

			if (cdCallback_ && !biosMode_) {
				CDMapResult map = CDMap(p);
				if (map.type != eCDLog_AddrType_None)
					cdCallback_(map.addr, map.type, eCDLog_Flags_Data);
			}
		}
	}

	template <bool callbacksActive>
	void ff_write(unsigned p, unsigned data, gb_ulong cc) {
		if (p - 0x80u < 0x7Fu)
			ioamhram_[p + 0x100] = data;
		else if (megaDuck_ && p < 0x80)
			megaDuckFfWrite(p, data, cc);
		else
			nontrivial_ff_write(p, data, cc);

		if (callbacksActive && writeCallback_)
			writeCallback_(mm_io_begin + p, callbackCycleOffset(cc));
		if (callbacksActive && cdCallback_ && !biosMode_) {
			CDMapResult map = CDMap(mm_io_begin + p);
			if (map.type != eCDLog_AddrType_None)
				cdCallback_(map.addr, map.type, eCDLog_Flags_Data);
		}
	}

	gb_ulong event(gb_ulong cycleCounter);
	gb_ulong resetCounters(gb_ulong cycleCounter);
	LoadRes loadROM(Array<unsigned char> &buffer, unsigned flags, std::string const &filepath);
	void setSaveDir(std::string const &dir) { cart_.setSaveDir(dir); }

	void setInputGetter(InputGetter *getInput, void *p) {
		getInput_ = getInput;
		getInputP_ = p;
	}

	// armSNES
	void setIcd(unsigned (*joyp)(void *, unsigned, unsigned), void (*line)(void *, unsigned, uint_least32_t const *), void *p) {
		icdJoyp_ = joyp;
		icdP_ = p;
		icdInput_ = 0xF;
		lcd_.setIcdLine(line, p);
	}
	void updateVideo(gb_ulong cc) { lcd_.update(cc); }

	unsigned getJoypadIndex() {
		return sgb_.getJoypadIndex();
	}

	void setReadCallback(MemoryCallback callback) {
		readCallback_ = callback;
	}

	void setWriteCallback(MemoryCallback callback) {
		writeCallback_ = callback;
	}

	void setExecCallback(MemoryCallback callback) {
		execCallback_ = callback;
	}

	void setCDCallback(CDCallback cdc) {
		cdCallback_ = cdc;
	}

	void setScanlineCallback(void (*callback)(), int sl) {
		lcd_.setScanlineCallback(callback, sl);
	}

	void setLinkCallback(void (*callback)()) {
		linkCallback_ = callback;
	}

	void setCameraCallback(void (*callback)(int *cameraBuf)) {
		if (cart_.isPocketCamera())
			cart_.setCameraCallback(callback);
	}

	void setRemoteCallback(unsigned char (*callback)()) {
		cart_.setRemoteCallback(callback);
	}

	inline bool slowCallbacksActive() { return readCallback_ || writeCallback_ || execCallback_ || cdCallback_; }

	void setEndtime(gb_ulong cc, gb_ulong inc);

	std::size_t callbackCycleOffset(gb_ulong const cc) { return psg_.callbackCycleOffset(cc, isDoubleSpeed()); }

	void setSoundBuffer(uint_least32_t *buf) { psg_.setBuffer(buf); }
	std::size_t fillSoundBuffer(gb_ulong cc);

	void setVideoBuffer(uint_least32_t *videoBuf, std::ptrdiff_t pitch) {
		lcd_.setVideoBuffer(videoBuf, pitch);
		sgb_.setVideoBuffer(videoBuf, pitch);
	}

	unsigned updateScreenBorder(uint_least32_t *videoBuf, std::ptrdiff_t pitch) {
		return isSgb()
			? sgb_.updateScreenBorder(videoBuf, pitch)
			: -1;
	}

	unsigned generateSgbSamples(short *soundBuf, std::size_t &samples) {
		return isSgb()
			? sgb_.generateSamples(soundBuf, samples)
			: -1;
	}

	unsigned generateMbcSamples(short *soundBuf) {
		return cart_.generateSamples(soundBuf);
	}

	void setDmgPaletteColor(int palNum, int colorNum, gb_ulong rgb32) {
		if (!isSgb())
			lcd_.setDmgPaletteColor(palNum, colorNum, rgb32);
	}

    
	void blackScreen() {
		lcd_.blackScreen();
	}

	void setCgbPalette(unsigned *lut) {
		if (isSgb())
			sgb_.setCgbPalette(lut);
		else
			lcd_.setCgbPalette(lut);
	}

	void setTimeMode(bool useCycles, gb_ulong const cc) { cart_.setTimeMode(useCycles, cc); }
	void setRtcDivisorOffset(gb_long const rtcDivisorOffset) { cart_.setRtcDivisorOffset(rtcDivisorOffset); }
	void setRtcSpeed(unsigned percent, gb_ulong const cc) { cart_.setRtcSpeed(percent, cc); }
	bool deviceTime() const { return cart_.deviceTime(); }
	void setDeviceTime(bool device, gb_ulong const cc) { cart_.setDeviceTime(device, cc); }
	bool hasMbc3Rtc() const { return cart_.hasMbc3Rtc(); }
	void getRtcRegs(gb_ulong *dest, gb_ulong const cc) { cart_.getRtcRegs(dest, cc); }
	void setRtcRegs(gb_ulong *src, gb_ulong const cc) { cart_.setRtcRegs(src, cc); }
	bool isHuC3() const { return cart_.isHuC3(); }
	void getHuC3Regs(unsigned char *dest, gb_ulong const cc) { cart_.getHuC3Regs(dest, cc); }
	void setHuC3Regs(unsigned char *src, gb_ulong const cc) { cart_.setHuC3Regs(src, cc); }

	void setCartBusPullUpTime(gb_ulong const cartBusPullUpTime) { cartBusPullUpTime_ = cartBusPullUpTime; }

	int linkStatus(int which);
	int serialLeft(gb_ulong cc) const;

	void setGameGenie(std::string const &codes) { cart_.setGameGenie(codes); }
	void setGameShark(std::string const &codes) { interrupter_.setGameShark(codes); }
	void updateInput();

	void setBios(Array<unsigned char> &buffer) {
		bios_.reset(buffer.size());
		std::memcpy(bios_.get(), buffer.get(), buffer.size());
	}

	unsigned long long timeNow() const { return totalSamplesEmitted_; }

	void setTime(unsigned long long dividers) { cart_.setTime(dividers); }

	gb_ulong getDivLastUpdate() { return divLastUpdate_; }
	unsigned char getRawIOAMHRAM(int offset) { return ioamhram_[offset]; }

	void setSpeedupFlags(unsigned flags) {
		lcd_.setSpeedupFlags(flags);
		psg_.setSpeedupFlags(flags);
	}

	template<bool isReader>void SyncState(NewState *ns);

private:
	unsigned cpuQuarters_;
	unsigned cpuCarry_;
	gb_ulong stockLeft_;
	Cartridge cart_;
	Sgb sgb_;
	unsigned char ioamhram_[0x200];
	Array<unsigned char> bios_;
	InputGetter *getInput_;
	// armSNES: the Super Game Boy's ICD2 takes JOYP writes and gives the input bits back.
	unsigned (*icdJoyp_)(void *p, unsigned p14, unsigned p15);
	void *icdP_;
	unsigned char icdInput_;
	void *getInputP_;
	gb_ulong divLastUpdate_;
	gb_ulong lastOamDmaUpdate_;
	gb_ulong lastCartBusUpdate_;
	gb_ulong cartBusPullUpTime_; 
	InterruptRequester intreq_;
	Tima tima_;
	LCD lcd_;
	PSG psg_;
	Interrupter interrupter_;
	unsigned short dmaSource_;
	unsigned short dmaDestination_;
	unsigned char oamDmaPos_;
	unsigned char oamDmaStartPos_;
	unsigned char serialCnt_;
	unsigned char cartBus_;
	bool blanklcd_;
	bool biosMode_;
	bool agbFlag_;
	bool gbIsSgb_;
	bool megaDuck_;
	bool stopped_;
	enum HdmaState { hdma_low, hdma_high, hdma_requested } haltHdmaState_;

	MemoryCallback readCallback_;
	MemoryCallback writeCallback_;
	MemoryCallback execCallback_;
	CDCallback cdCallback_;
	void (*linkCallback_)();
	bool linked_;
	bool linkClockTrigger_;

	unsigned long long totalSamplesEmitted_;

	void decEventCycles(IntEventId eventId, gb_ulong dec);
	void oamDmaInitSetup();
	void updateOamDma(gb_ulong cycleCounter);
	void startOamDma(gb_ulong cycleCounter);
	void endOamDma(gb_ulong cycleCounter);
	unsigned char const * oamDmaSrcPtr() const;
	gb_ulong dma(gb_ulong cc);
	unsigned nontrivial_ff_read(unsigned p, gb_ulong cycleCounter);
	unsigned nontrivial_read(unsigned p, gb_ulong cycleCounter);
	unsigned nontrivial_ff_peek(unsigned p, gb_ulong cycleCounter);
	// armGBA: the Mega Duck's I/O registers, at its own addresses and with its own bit layouts, on
	// the Game Boy's
	unsigned megaDuckFfRead(unsigned p, gb_ulong cycleCounter);
	unsigned megaDuckFfPeek(unsigned p, gb_ulong cycleCounter);
	void megaDuckFfWrite(unsigned p, unsigned data, gb_ulong cycleCounter);
	unsigned nontrivial_peek(unsigned p, gb_ulong cycleCounter);
	void nontrivial_ff_write(unsigned p, unsigned data, gb_ulong cycleCounter);
	void nontrivial_write(unsigned p, unsigned data, gb_ulong cycleCounter);
	void updateSerial(gb_ulong cc);
	void updateTimaIrq(gb_ulong cc);
	void updateIrqs(gb_ulong cc);
	bool isDoubleSpeed() const { return lcd_.isDoubleSpeed(); }
	unsigned char cartBus(gb_ulong const cc) const {
		gb_ulong const diff = cc - lastCartBusUpdate_;
		return diff >= (cartBusPullUpTime_ << isDoubleSpeed()) ? 0xFF : cartBus_;
	}
};

}

#endif
