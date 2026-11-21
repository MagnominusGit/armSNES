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

#include "gbint.h"
#include "memory.h"
#include "gambatte.h"
#include "inputgetter.h"
#include "savestate.h"
#include "sound.h"
#include "video.h"
#include "file/crc32.h"

#include <algorithm>
#include <cstring>

using namespace gambatte;

namespace {

int const oam_size = 4 * lcd_num_oam_entries;

void decCycles(gb_ulong &counter, gb_ulong dec) {
	if (counter != disabled_time)
		counter -= dec;
}

int serialCntFrom(gb_ulong cyclesUntilDone, bool cgbFast) {
	return cgbFast ? (cyclesUntilDone + 0xF) >> 4 : (cyclesUntilDone + 0x1FF) >> 9;
}

} // unnamed namespace.

Memory::Memory(Interrupter const &interrupter)
: cpuQuarters_(4)
, cpuCarry_(0)
, stockLeft_(0)
, getInput_(0)
, icdJoyp_(0) // armSNES
, icdP_(0)
, icdInput_(0xF)
, divLastUpdate_(0)
, lastOamDmaUpdate_(disabled_time)
, lastCartBusUpdate_(0)
, cartBusPullUpTime_(8)
, lcd_(ioamhram_, 0, VideoInterruptRequester(intreq_))
, interrupter_(interrupter)
, dmaSource_(0)
, dmaDestination_(0)
, oamDmaPos_(-2u & 0xFF)
, oamDmaStartPos_(0)
, serialCnt_(0)
, cartBus_(0xFF)
, blanklcd_(false)
, haltHdmaState_(hdma_low)
, readCallback_(0)
, writeCallback_(0)
, execCallback_(0)
, cdCallback_(0)
, linkCallback_(0)
, linked_(false)
, linkClockTrigger_(false)
, megaDuck_(false)
{
	intreq_.setEventTime<intevent_blit>(1 * lcd_vres * lcd_cycles_per_line);
	intreq_.setEventTime<intevent_end>(0);
}

void Memory::setStatePtrs(SaveState &state) {
	state.mem.ioamhram.set(ioamhram_, sizeof ioamhram_);

	cart_.setStatePtrs(state);
	sgb_.setStatePtrs(state, isSgb());
	lcd_.setStatePtrs(state);
	psg_.setStatePtrs(state);
}

gb_ulong Memory::saveState(SaveState &state, gb_ulong cc) {
	cc = resetCounters(cc);
	ioamhram_[0x104] = 0;
	nontrivial_ff_read(0x05, cc);
	nontrivial_ff_read(0x0F, cc);
	nontrivial_ff_read(0x26, cc);

	state.mem.divLastUpdate = divLastUpdate_;
	state.mem.nextSerialtime = intreq_.eventTime(intevent_serial);
	state.mem.unhaltTime = intreq_.eventTime(intevent_unhalt);
	state.mem.nextBlitTime = intreq_.eventTime(intevent_blit);
	state.mem.blankLcd = blanklcd_;
	state.mem.cpuCarry = cpuCarry_;
	state.mem.cpuStockLeft = stockLeft_;
	state.mem.lastOamDmaUpdate = oamDmaStartPos_
		? lastOamDmaUpdate_ + ((oamDmaStartPos_ - oamDmaPos_) & 0xFF) * oamDmaStep()
		: lastOamDmaUpdate_;
	state.mem.dmaSource = dmaSource_;
	state.mem.dmaDestination = dmaDestination_;
	state.mem.oamDmaPos = oamDmaPos_;
	state.mem.haltHdmaState = haltHdmaState_;
	state.mem.biosMode = biosMode_;
	state.mem.stopped = stopped_;
	state.mem.lastCartBusUpdate = lastCartBusUpdate_;
	state.mem.totalSamplesEmittedHigh = totalSamplesEmitted_ >> 32;
	state.mem.totalSamplesEmittedLow = totalSamplesEmitted_ & 0xFFFFFFFF;

	intreq_.saveState(state);
	cart_.saveState(state, cc);
	sgb_.saveState(state);
	tima_.saveState(state);
	lcd_.saveState(state);
	psg_.saveState(state);

	return cc;
}

void Memory::loadState(SaveState const &state) {
	biosMode_ = state.mem.biosMode;
	cpuCarry_ = state.mem.cpuCarry;
	stockLeft_ = state.mem.cpuStockLeft;
	stopped_ = state.mem.stopped;
	lastCartBusUpdate_ = state.mem.lastCartBusUpdate;
	totalSamplesEmitted_ = (unsigned long long)state.mem.totalSamplesEmittedHigh << 32;
	totalSamplesEmitted_ |= state.mem.totalSamplesEmittedLow;

	psg_.loadState(state);
	lcd_.loadState(state, state.mem.oamDmaPos < oam_size ? cart_.rdisabledRam() : ioamhram_);
	tima_.loadState(state, TimaInterruptRequester(intreq_));
	sgb_.loadState(state);
	cart_.loadState(state);
	intreq_.loadState(state);

	intreq_.setEventTime<intevent_serial>(state.mem.nextSerialtime > state.cpu.cycleCounter
		? state.mem.nextSerialtime
		: state.cpu.cycleCounter);
	intreq_.setEventTime<intevent_unhalt>(state.mem.unhaltTime);
	lastOamDmaUpdate_ = state.mem.lastOamDmaUpdate;
	dmaSource_ = state.mem.dmaSource;
	dmaDestination_ = state.mem.dmaDestination;
	oamDmaPos_ = state.mem.oamDmaPos;
	oamDmaStartPos_ = 0;
	haltHdmaState_ = static_cast<HdmaState>(std::min(1u * state.mem.haltHdmaState, 1u * hdma_requested));
	serialCnt_ = intreq_.eventTime(intevent_serial) != disabled_time
		? serialCntFrom(intreq_.eventTime(intevent_serial) - state.cpu.cycleCounter,
			ioamhram_[0x102] & (isCgb() & !isCgbDmg()) * 2)
		: 8;

	cart_.setVrambank(ioamhram_[0x14F] & (isCgb() & !isCgbDmg()));
	cart_.setOamDmaSrc(oam_dma_src_off);
	cart_.setWrambank((isCgb() && !isCgbDmg()) && (ioamhram_[0x170] & 0x07) ? ioamhram_[0x170] & 0x07 : 1);

	if (lastOamDmaUpdate_ != disabled_time) {
		if (lastOamDmaUpdate_ > state.cpu.cycleCounter) {
			oamDmaStartPos_ = (oamDmaPos_ + (lastOamDmaUpdate_ - state.cpu.cycleCounter) / oamDmaStep()) & 0xFF;
			lastOamDmaUpdate_ = state.cpu.cycleCounter;
		}
		oamDmaInitSetup();

		unsigned oamEventPos = oamDmaPos_ < oam_size ? oam_size : oamDmaStartPos_;
		intreq_.setEventTime<intevent_oam>(
			lastOamDmaUpdate_ + ((oamEventPos - oamDmaPos_) & 0xFF) * oamDmaStep());
	}

	intreq_.setEventTime<intevent_blit>(ioamhram_[0x140] & lcdc_en
		? lcd_.nextMode1IrqTime()
		: state.cpu.cycleCounter);
	blanklcd_ = false;
	// armGBA: the frames go on being handed over when they were going to be
	if (state.mem.nextBlitTime) {
		intreq_.setEventTime<intevent_blit>(state.mem.nextBlitTime);
		blanklcd_ = state.mem.blankLcd;
	}

	if (!isCgb() || isCgbDmg())
		std::fill_n(cart_.vramdata() + vrambank_size(), vrambank_size(), 0);
}

void Memory::setEndtime(gb_ulong cc, gb_ulong inc) {
	if (intreq_.eventTime(intevent_blit) <= cc) {
		intreq_.setEventTime<intevent_blit>(intreq_.eventTime(intevent_blit)
			+ (lcd_cycles_per_frame << isDoubleSpeed()));
	}

	intreq_.setEventTime<intevent_end>(cc + (inc << isDoubleSpeed()));
}

void Memory::updateSerial(gb_ulong const cc) {
	if (!linked_) {
		if (intreq_.eventTime(intevent_serial) != disabled_time) {
			if (intreq_.eventTime(intevent_serial) <= cc) {
				ioamhram_[0x101] = (((ioamhram_[0x101] + 1) << serialCnt_) - 1) & 0xFF;
				ioamhram_[0x102] &= 0x7F;
				intreq_.flagIrq(8, intreq_.eventTime(intevent_serial));
				intreq_.setEventTime<intevent_serial>(disabled_time);
			} else {
				int const targetCnt = serialCntFrom(intreq_.eventTime(intevent_serial) - cc,
					ioamhram_[0x102] & (isCgb() & !isCgbDmg()) * 2);
				ioamhram_[0x101] = (((ioamhram_[0x101] + 1) << (serialCnt_ - targetCnt)) - 1) & 0xFF;
				serialCnt_ = targetCnt;
			}
		}
	} else {
		if (intreq_.eventTime(intevent_serial) != disabled_time) {
			if (intreq_.eventTime(intevent_serial) <= cc) {
				linkClockTrigger_ = true;
				intreq_.setEventTime<intevent_serial>(disabled_time);
				if (linkCallback_)
					linkCallback_();
			}
		}
	}
}

void Memory::updateTimaIrq(gb_ulong cc) {
	while (intreq_.eventTime(intevent_tima) <= cc)
		tima_.doIrqEvent(TimaInterruptRequester(intreq_));
}

void Memory::updateIrqs(gb_ulong cc) {
	updateSerial(cc);
	updateTimaIrq(cc);
	lcd_.update(cc);
}

gb_ulong Memory::event(gb_ulong cc) {
	if (lastOamDmaUpdate_ != disabled_time)
		updateOamDma(cc);

	switch (intreq_.minEventId()) {
	case intevent_unhalt:
		if ((lcd_.hdmaIsEnabled() && lcd_.isHdmaPeriod(cc) && haltHdmaState_ == hdma_low)
				|| haltHdmaState_ == hdma_requested) {
			flagHdmaReq(intreq_);
		}

		intreq_.unhalt();
		intreq_.setEventTime<intevent_unhalt>(disabled_time);
		break;
	case intevent_end:
		intreq_.setEventTime<intevent_end>(disabled_time - 1);

		while (cc >= intreq_.minEventTime()
				&& intreq_.eventTime(intevent_end) != disabled_time) {
			cc = event(cc);
		}

		intreq_.setEventTime<intevent_end>(disabled_time);

		break;
	case intevent_blit:
		{
			bool const lcden = ioamhram_[0x140] & lcdc_en;
			gb_ulong blitTime = intreq_.eventTime(intevent_blit);

			if (lcden | blanklcd_) {
				lcd_.updateScreen(blanklcd_, cc, 0);
				if (isSgb() && !icdJoyp_) // armSNES
					sgb_.updateScreen(blanklcd_);
				lcd_.updateScreen(blanklcd_, cc, 1);

				intreq_.setEventTime<intevent_blit>(disabled_time);
				intreq_.setEventTime<intevent_end>(disabled_time);

				while (cc >= intreq_.minEventTime())
					cc = event(cc);
			} else
				blitTime += lcd_cycles_per_frame << isDoubleSpeed();

			blanklcd_ = lcden ^ 1;
			intreq_.setEventTime<intevent_blit>(blitTime);
		}
		break;
	case intevent_serial:
		updateSerial(cc);
		break;
	case intevent_oam:
		if (lastOamDmaUpdate_ != disabled_time) {
			unsigned const oamEventPos = oamDmaPos_ < oam_size ? oam_size : oamDmaStartPos_;
			intreq_.setEventTime<intevent_oam>(
				lastOamDmaUpdate_ + ((oamEventPos - oamDmaPos_) & 0xFF) * oamDmaStep());
		} else
			intreq_.setEventTime<intevent_oam>(disabled_time);

		break;
	case intevent_dma:
		interrupter_.prefetch(cc, *this);
		cc = dma(cc);
		if (haltHdmaState_ == hdma_requested) {
			haltHdmaState_ = hdma_low;
			intreq_.setMinIntTime(cc);
			cc -= 4;
		}
		break;
	case intevent_tima:
		tima_.doIrqEvent(TimaInterruptRequester(intreq_));
		break;
	case intevent_video:
		lcd_.update(cc);
		break;
	case intevent_interrupts:
		// GSR NOTE: "make stop really stop" (commit 9a79253, since r649)
		if (stopped_) {
			intreq_.setEventTime<intevent_interrupts>(disabled_time);
			break;
		}

		if (halted()) {
			cc += isCgb() || cc - intreq_.eventTime(intevent_interrupts) < 2 ? cpuCycles(1) : 0;
			if (cc > lastOamDmaUpdate_)
				updateOamDma(cc);
			if ((lcd_.hdmaIsEnabled() && lcd_.isHdmaPeriod(cc) && haltHdmaState_ == hdma_low)
					|| haltHdmaState_ == hdma_requested) {
				flagHdmaReq(intreq_);
			}

			intreq_.unhalt();
			intreq_.setEventTime<intevent_unhalt>(disabled_time);
		}
		if (cc >= intreq_.eventTime(intevent_video))
			lcd_.update(cc);
		if (cc >= intreq_.eventTime(intevent_dma))
			break;

		if (ime()) {
			di();
			cc = interrupter_.interrupt(cc, *this);
		}

		break;
	}

	return cc;
}

gb_ulong Memory::dma(gb_ulong cc) {
	bool const doubleSpeed = isDoubleSpeed();
	unsigned dmaSrc = dmaSource_;
	unsigned dmaDest = dmaDestination_;
	unsigned dmaLength = ((ioamhram_[0x155] & 0x7F) + 1) * 0x10;
	unsigned length = hdmaReqFlagged(intreq_) ? 0x10 : dmaLength;

	if (1u * dmaDest + length >= 0x10000) {
		length = 0x10000 - dmaDest;
		ioamhram_[0x155] |= 0x80;
	}

	dmaLength -= length;

	if (!(ioamhram_[0x140] & lcdc_en) && gdmaReqFlagged(intreq_)) // FIXME: This is wrong for HDMA, but is it right for GDMA?
		dmaLength = 0;

	gb_ulong lOamDmaUpdate = lastOamDmaUpdate_;
	lastOamDmaUpdate_ = disabled_time;

	while (length--) {
		unsigned const src = dmaSrc++ & 0xFFFF;
		unsigned const data = (src & -vrambank_size()) == mm_vram_begin || src >= mm_wram_mirror_begin
			? cartBus(cc + 4) // cart bus seems to pull up sooner here? might be prefetch related
			// armGBA: the Action Replay answers DMA's reads of the ROM area without a CPU read's effects
			: src < mm_vram_begin && cart_.cheatCart() ? cart_.romHookDmaRead(src)
			: read<false, false, false, false>(src, cc);

		cc += 2 + 2 * doubleSpeed;

		if (cc - (oamDmaStep() - 1) > lOamDmaUpdate && !halted()) {
			oamDmaPos_ = (oamDmaPos_ + 1) & 0xFF;
			lOamDmaUpdate += oamDmaStep();
			if (oamDmaPos_ == oamDmaStartPos_)
				startOamDma(lOamDmaUpdate);

			if (oamDmaPos_ < oam_size) {
				unsigned const p = src & 0xFF;
				if (p < oam_size)
					ioamhram_[p] = data;
				else if (!agbFlag_)
					ioamhram_[p & 0xE7] = data;
			} else if (oamDmaPos_ == oam_size) {
				endOamDma(lOamDmaUpdate);
				if (oamDmaStartPos_ == 0)
					lOamDmaUpdate = disabled_time;
			}
		}

		nontrivial_write(mm_vram_begin | dmaDest++ % vrambank_size(), data, cc);
	}

	lastOamDmaUpdate_ = lOamDmaUpdate;
	ackDmaReq(intreq_);
	cc += 4;

	dmaSource_ = dmaSrc;
	dmaDestination_ = dmaDest;
	ioamhram_[0x155] = halted()
		? ioamhram_[0x155] | 0x80
		: ((dmaLength / 0x10 - 1) & 0xFF) | (ioamhram_[0x155] & 0x80);

	if ((ioamhram_[0x155] & 0x80) && lcd_.hdmaIsEnabled()) {
		if (lastOamDmaUpdate_ != disabled_time)
			updateOamDma(cc);

		lcd_.disableHdma(cc);
	}

	return cc;
}

void Memory::freeze(gb_ulong cc) {
	// permanently halt CPU.
	// simply halt and clear IE to avoid unhalt from occuring,
	// which avoids additional state to represent a "frozen" state.
	nontrivial_ff_write(0xFF, 0, cc);
	ackDmaReq(intreq_);
	intreq_.halt();
}

bool Memory::halt(gb_ulong cc) {
	if (lastOamDmaUpdate_ != disabled_time)
		updateOamDma(cc);

	haltHdmaState_ = lcd_.hdmaIsEnabled() && lcd_.isHdmaPeriod(cc)
		? hdma_high : hdma_low;
	bool const hdmaReq = hdmaReqFlagged(intreq_);
	if (hdmaReq)
		haltHdmaState_ = hdma_requested;
	if (lastOamDmaUpdate_ != disabled_time)
		updateOamDma(cc + 4);

	ackDmaReq(intreq_);
	intreq_.halt();
	return hdmaReq;
}

unsigned Memory::pendingIrqs(gb_ulong cc) {
	if (lastOamDmaUpdate_ != disabled_time)
		updateOamDma(cc);

	updateIrqs(cc);
	return intreq_.pendingIrqs();
}

void Memory::ackIrq(unsigned bit, gb_ulong cc) {
	if (lastOamDmaUpdate_ != disabled_time)
		updateOamDma(cc);

	// TODO: adjust/extend IRQ assertion time rather than use the odd cc offsets?
	// NOTE: a minimum offset of 2 is required for the LCD due to implementation assumptions w.r.t. cc headroom.
	updateSerial(cc + 3 + isCgb());
	updateTimaIrq(cc + 2 + isCgb());
	lcd_.update(cc + 2);
	intreq_.ackIrq(bit);
}

gb_ulong Memory::stop(gb_ulong cc, bool &prefetched) {
	// FIXME: this is incomplete.
	intreq_.setEventTime<intevent_unhalt>(cc + 0x20000 + 4);

	// speed change.
	if (ioamhram_[0x14D] & (isCgb() & !isCgbDmg())) {
		tima_.speedChange(TimaInterruptRequester(intreq_));
		// DIV reset.
		nontrivial_ff_write(0x04, 0, cc);
		haltHdmaState_ = lcd_.hdmaIsEnabled() && lcd_.isHdmaPeriod(cc)
			? hdma_high : hdma_low;
		prefetched = hdmaReqFlagged(intreq_);
		if (prefetched && isDoubleSpeed())
			haltHdmaState_ = hdma_requested;
		gb_ulong const cc_ = cc + 8 * !isDoubleSpeed();
		if (cc_ >= cc + 4) {
			if (lastOamDmaUpdate_ != disabled_time)
				updateOamDma(cc + 4);
			if (!prefetched || isDoubleSpeed())
				ackDmaReq(intreq_);
			intreq_.halt();
		}
		psg_.speedChange(cc_, isDoubleSpeed());
		lcd_.speedChange(cc_);
		cart_.speedChange(cc_);
		ioamhram_[0x14D] ^= 0x81;
		// TODO: perhaps make this a bit nicer?
		intreq_.setEventTime<intevent_blit>(ioamhram_[0x140] & lcdc_en
			? lcd_.nextMode1IrqTime()
			: cc + (lcd_cycles_per_frame << isDoubleSpeed()));
		if (intreq_.eventTime(intevent_end) > cc_) {
			intreq_.setEventTime<intevent_end>(cc_
				+ (isDoubleSpeed()
				   ? (intreq_.eventTime(intevent_end) - cc_) * 2
				   : (intreq_.eventTime(intevent_end) - cc_) / 2));
		}
		if (cc_ < cc + 4) {
			if (lastOamDmaUpdate_ != disabled_time)
				updateOamDma(cc + 4);
			if (!prefetched || !isDoubleSpeed())
				ackDmaReq(intreq_);
			intreq_.halt();
		}
		// ensure that no updates with a previous cc occur.
		cc += 8;
		// armGBA: overclocked, the CPU may stand between two dots of double speed; it starts on one.
		if (isDoubleSpeed() && cpuQuarters_ != 4)
			cc += cc & 1;
	} else {
		// FIXME: test and implement stop correctly.
		prefetched = halt(cc);
		cc += 4;

		// GSR NOTE: "make stop really stop" (commit 9a79253, since r649)
		stopped_ = true;
		intreq_.setEventTime<intevent_unhalt>(disabled_time);
	}

	return cc;
}

void Memory::stall(gb_ulong cc, gb_ulong cycles) {
	intreq_.halt();
	intreq_.setEventTime<intevent_unhalt>(cc + cycles);
}

void Memory::decEventCycles(IntEventId eventId, gb_ulong dec) {
	if (intreq_.eventTime(eventId) != disabled_time)
		intreq_.setEventTime(eventId, intreq_.eventTime(eventId) - dec);
}

gb_ulong Memory::resetCounters(gb_ulong cc) {
	if (lastOamDmaUpdate_ != disabled_time)
		updateOamDma(cc);

	updateIrqs(cc);

	gb_ulong const dec = cc < 0x20000
		? 0
		: (cc & -0x10000) - 0x10000;
	decCycles(lastOamDmaUpdate_, dec);
	decCycles(lastCartBusUpdate_, dec);
	decEventCycles(intevent_serial, dec);
	decEventCycles(intevent_oam, dec);
	decEventCycles(intevent_blit, dec);
	decEventCycles(intevent_end, dec);
	decEventCycles(intevent_unhalt, dec);

	gb_ulong const oldCC = cc;
	cc -= dec;
	intreq_.resetCc(oldCC, cc);
	cart_.resetCc(oldCC, cc);
	tima_.resetCc(oldCC, cc, TimaInterruptRequester(intreq_));
	lcd_.resetCc(oldCC, cc);
	psg_.resetCounter(cc, oldCC, isDoubleSpeed());
	return cc;
}

void Memory::updateInput() {
	unsigned state = 0xF;

	if (icdJoyp_) { // armSNES
		state = icdInput_;
		if (state != 0xF && (ioamhram_[0x100] & 0xF) == 0xF)
			intreq_.flagIrq(0x10);

		ioamhram_[0x100] = (ioamhram_[0x100] & -0x10u) | state;
		return;
	}

	if ((ioamhram_[0x100] & 0x30) != 0x30) {
		if (getInput_) {
			unsigned input = (*getInput_)(getInputP_);
			unsigned dpad_state = ~input >> 4;
			unsigned button_state = ~input;
			if (!(ioamhram_[0x100] & 0x10))
				state &= dpad_state;
			if (!(ioamhram_[0x100] & 0x20))
				state &= button_state;

			if (state != 0xF && (ioamhram_[0x100] & 0xF) == 0xF)
				intreq_.flagIrq(0x10);
		}
	} else if (isSgb())
		state -= getJoypadIndex();

	ioamhram_[0x100] = (ioamhram_[0x100] & -0x10u) | state;
}

void Memory::updateOamDma(gb_ulong const cc) {
	unsigned char const *const oamDmaSrc = oamDmaSrcPtr();
	unsigned const step = oamDmaStep();
	unsigned cycles = (cc - lastOamDmaUpdate_) / step;

	if (halted()) {
		lastOamDmaUpdate_ += step * cycles;
	} else while (cycles--) {
		oamDmaPos_ = (oamDmaPos_ + 1) & 0xFF;
		lastOamDmaUpdate_ += step;
		if (oamDmaPos_ == oamDmaStartPos_)
			startOamDma(lastOamDmaUpdate_);

		if (oamDmaPos_ < oam_size) {
			// armGBA: the Action Replay answers OAM DMA's reads of the ROM area too
			if (cart_.oamDmaSrc() == oam_dma_src_rom && cart_.cheatCart())
				ioamhram_[oamDmaPos_] = cart_.romHookDmaRead(ioamhram_[0x146] * 0x100 + oamDmaPos_);
			else if (oamDmaSrc) ioamhram_[oamDmaPos_] = oamDmaSrc[oamDmaPos_];
			else if (cart_.isHuC3()) ioamhram_[oamDmaPos_] = cart_.HuC3Read(oamDmaPos_, cc);
			else ioamhram_[oamDmaPos_] = cart_.rtcRead();
		} else if (oamDmaPos_ == oam_size) {
			endOamDma(lastOamDmaUpdate_);
			if (oamDmaStartPos_ == 0) {
				lastOamDmaUpdate_ = disabled_time;
				break;
			}
		}
	}
}

void Memory::oamDmaInitSetup() {
	if (ioamhram_[0x146] < mm_sram_begin / 0x100) {
		cart_.setOamDmaSrc(ioamhram_[0x146] < mm_vram_begin / 0x100 ? oam_dma_src_rom : oam_dma_src_vram);
	} else if (ioamhram_[0x146] < 0x100 - isCgb() * 0x20) {
		cart_.setOamDmaSrc(ioamhram_[0x146] < mm_wram_begin / 0x100 ? oam_dma_src_sram : oam_dma_src_wram);
	} else
		cart_.setOamDmaSrc(oam_dma_src_invalid);
}

unsigned char const * Memory::oamDmaSrcPtr() const {
	switch (cart_.oamDmaSrc()) {
	case oam_dma_src_rom:
		return cart_.romdata(ioamhram_[0x146] >> 6) + ioamhram_[0x146] * 0x100;
	case oam_dma_src_sram:
		return cart_.rsrambankptr() ? cart_.rsrambankptr() + ioamhram_[0x146] * 0x100 : 0;
	case oam_dma_src_vram:
		return cart_.vrambankptr() + ioamhram_[0x146] * 0x100;
	case oam_dma_src_wram:
		return cart_.wramdata(ioamhram_[0x146] >> 4 & 1) + (ioamhram_[0x146] * 0x100 & 0xFFF);
	case oam_dma_src_invalid:
	case oam_dma_src_off:
		break;
	}

	return cart_.rdisabledRam();
}

void Memory::startOamDma(gb_ulong cc) {
	oamDmaPos_ = 0;
	oamDmaStartPos_ = 0;
	lcd_.oamChange(cart_.rdisabledRam(), cc);
}

void Memory::endOamDma(gb_ulong cc) {
	if (oamDmaStartPos_ == 0) {
		oamDmaPos_ = -2u & 0xFF;
		cart_.setOamDmaSrc(oam_dma_src_off);
	}
	lcd_.oamChange(ioamhram_, cc);
}

// armGBA: the Mega Duck is a Game Boy with its I/O registers moved and some of their bits rearranged
// (as SameDuck has them). Each Mega Duck register is the Game Boy register here, or none (-1).
namespace {

signed char const megaDuckIo[0x80] = {
	// 00-0F: joypad, serial, timer and IF, where the Game Boy has them
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
	// 10-1F: LCDC STAT SCY SCX OBP0 OBP1 WY WX LY LYC DMA BGP
	0x40, 0x41, 0x42, 0x43, 0x48, 0x49, 0x4A, 0x4B, 0x44, 0x45, 0x46, 0x47,   -1,   -1,   -1,   -1,
	// 20-2F: NR10 NR12 NR11 NR13 NR14 NR21 (NR20) NR22 NR23 NR24 NR30 NR31 NR32 NR34 NR33 (NR40)
	0x10, 0x12, 0x11, 0x13, 0x14, 0x16, 0x15, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1E, 0x1D, 0x1F,
	// 30-3F: wave RAM
	0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
	// 40-4F: NR41 NR43 NR42 NR44 NR50 NR52 NR51, then nothing up to the Game Boy Color's registers
	0x20, 0x22, 0x21, 0x23, 0x24, 0x26, 0x25,   -1,   -1,   -1,   -1,   -1, 0x4C, 0x4D, 0x4E, 0x4F,
	0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F,
	0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F,
	0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F,
};

// LCDC bit by bit: the Mega Duck's bit 6 is the Game Boy's bit 0 (background on), 3 is 6 (window
// map), 2 is 3 (background map), 1 is 2 (sprite size) and 0 is 1 (sprites on); 7, 5 and 4 match.
unsigned megaDuckLcdcToDmg(unsigned v) {
	return (v & 0xB0) | (v >> 6 & 0x01) | (v << 3 & 0x40) | (v << 1 & 0x08) | (v << 1 & 0x04) | (v << 1 & 0x02);
}

unsigned dmgLcdcToMegaDuck(unsigned v) {
	return (v & 0xB0) | (v << 6 & 0x40) | (v >> 3 & 0x08) | (v >> 1 & 0x04) | (v >> 1 & 0x02) | (v >> 1 & 0x01);
}

// The envelopes (NR12, NR22, NR42) and the noise's frequency (NR43) have their two halves swapped.
// The wave's volume (NR32 bits 6-5) has full and a quarter the other way round: 11 is full, 01 a
// quarter (as GBDK-2020 has it; SameDuck doesn't).
unsigned megaDuckValue(unsigned dmgReg, unsigned v, bool toDmg) {
	switch (dmgReg) {
	case 0x40: return toDmg ? megaDuckLcdcToDmg(v) : dmgLcdcToMegaDuck(v);
	case 0x12:
	case 0x17:
	case 0x21:
	case 0x22: return (v << 4 & 0xF0) | (v >> 4 & 0x0F);
	case 0x1C: return (v & ~0x60u) | (0u - (v & 0x60)) & 0x60;
	}

	return v;
}

} // unnamed namespace

unsigned Memory::megaDuckFfRead(unsigned const p, gb_ulong const cc) {
	int const reg = megaDuckIo[p];
	return reg < 0 ? 0xFF : megaDuckValue(reg, nontrivial_ff_read(reg, cc), false);
}

unsigned Memory::megaDuckFfPeek(unsigned const p, gb_ulong const cc) {
	int const reg = megaDuckIo[p];
	return reg < 0 ? 0xFF : megaDuckValue(reg, nontrivial_ff_peek(reg, cc), false);
}

void Memory::megaDuckFfWrite(unsigned const p, unsigned const data, gb_ulong const cc) {
	int const reg = megaDuckIo[p];
	if (reg >= 0)
		nontrivial_ff_write(reg, megaDuckValue(reg, data, true), cc);
}

unsigned Memory::nontrivial_ff_read(unsigned const p, gb_ulong const cc) {
	if (lastOamDmaUpdate_ != disabled_time)
		updateOamDma(cc);

	switch (p) {
	case 0x00:
		updateInput();
		break;
	case 0x01:
	case 0x02:
		updateSerial(cc);
		break;
	case 0x04:
		return (cc - tima_.divLastUpdate()) >> 8 & 0xFF;
	case 0x05:
		ioamhram_[0x105] = tima_.tima(cc);
		break;
	case 0x0F:
		updateIrqs(cc);
		ioamhram_[0x10F] = intreq_.ifreg();
		break;
	case 0x26:
		if (ioamhram_[0x126] & 0x80) {
			psg_.generateSamples(cc, isDoubleSpeed());
			ioamhram_[0x126] = 0xF0 | psg_.getStatus();
		} else
			ioamhram_[0x126] = 0x70;

		break;
	case 0x30:
	case 0x31:
	case 0x32:
	case 0x33:
	case 0x34:
	case 0x35:
	case 0x36:
	case 0x37:
	case 0x38:
	case 0x39:
	case 0x3A:
	case 0x3B:
	case 0x3C:
	case 0x3D:
	case 0x3E:
	case 0x3F:
		psg_.generateSamples(cc, isDoubleSpeed());
		return psg_.waveRamRead(p & 0xF);
	case 0x41:
		cpuTimingPicture();
		return ioamhram_[0x141] | lcd_.getStat(ioamhram_[0x145], cc);
	case 0x44:
		cpuTimingPicture();
		return lcd_.getLyReg(cc);
	case 0x4C:
		if (!biosMode_)
			return 0xFF;

		break;
	case 0x50:
		return 0xFE | !biosMode_;
	case 0x56:
		if (isCgb() && !isCgbDmg()) {
			if (!agbFlag_ && ((ioamhram_[0x156] & 0xC0) == 0xC0) && cart_.getIrSignal(linked_ ? Infrared::linked_gb : Infrared::remote, cc))
				return ioamhram_[0x156] & ~0x02;
		}

		return ioamhram_[0x156] | 0x02;
	case 0x69:
		if (isCgb() && !isCgbDmg())
			return lcd_.cgbBgColorRead(ioamhram_[0x168] & 0x3F, cc);

		break;
	case 0x6B:
		if (isCgb() && !isCgbDmg())
			return lcd_.cgbSpColorRead(ioamhram_[0x16A] & 0x3F, cc);

		break;
	case 0x76:
		if (isCgb())
			return psg_.isEnabled() ? psg_.pcm12Read(cc, isDoubleSpeed()) : 0;

		break;
	case 0x77:
		if (isCgb())
			return psg_.isEnabled() ? psg_.pcm34Read(cc, isDoubleSpeed()) : 0;

		break;
	default:
		break;
	}

	return ioamhram_[p + 0x100];
}

unsigned Memory::nontrivial_read(unsigned const p, gb_ulong const cc) {
	if (p < mm_hram_begin) {
		if (lastOamDmaUpdate_ != disabled_time) {
			updateOamDma(cc);

			if (cart_.isInOamDmaConflictArea(p) && oamDmaPos_ < oam_size) {
				int const r = isCgb() && cart_.oamDmaSrc() != oam_dma_src_wram && p >= mm_wram_begin
					? cart_.wramdata(ioamhram_[0x146] >> 4 & 1)[p & 0xFFF]
					: ioamhram_[oamDmaPos_];
				if (isCgb() && cart_.oamDmaSrc() == oam_dma_src_vram)
					ioamhram_[oamDmaPos_] = 0;

				return r;
			}
		}

		if (p < mm_wram_begin) {
			if (p < mm_vram_begin)
				return cart_.hasRomHook() ? cart_.romHookRead(p) : cart_.romdata(p >> 14)[p];

			if (p < mm_sram_begin) {
				if (!lcd_.vramReadable(cc))
					return 0xFF;

				if (p < 0x9000) {
					if (lcd_.vramExactlyReadable(cc)) {
						return 0x00;
					}
				}

				return cart_.vrambankptr()[p];
			}

			{
				unsigned value;
				if (cart_.sramHookRead(p, value))
					return value;
			}

			if (cart_.rsrambankptr())
				return cart_.rsrambankptr()[p];

			if (cart_.disabledRam())
				return cartBus_;

			if (cart_.isHuC1())
				return 0xC0 | cart_.getIrSignal(linked_ ? Infrared::linked_gb : Infrared::remote, cc);

			if (cart_.hasIo())
				return cart_.ioRead(p, cc);

			if (cart_.isHuC3())
				return cart_.HuC3Read(p, cc);

			if (cart_.isPocketCamera())
				return cart_.cameraRead(p, cc);

			return cart_.rtcRead();
		}

		if (p < mm_oam_begin)
			return cart_.wramdata(p >> 12 & 1)[p & 0xFFF];

		gb_long const ffp = static_cast<gb_long>(p) - mm_io_begin;
		if (ffp >= 0)
			return megaDuck_ && ffp < 0x80 ? megaDuckFfRead(ffp, cc) : nontrivial_ff_read(ffp, cc);

		if (!lcd_.oamReadable(cc) || oamDmaPos_ < oam_size)
			return 0xFF;

		if (p >= (mm_oam_begin + oam_size) && isCgb() && !agbFlag_)
			return ioamhram_[(p - mm_oam_begin) & 0xE7];
	}

	return ioamhram_[p - mm_oam_begin];
}

unsigned Memory::nontrivial_ff_peek(unsigned const p, gb_ulong const cc) {
	// some regs might be wrong
	switch (p) {
	case 0x04:
		return (cc - tima_.divLastUpdate()) >> 8 & 0xFF;
	case 0x30:
	case 0x31:
	case 0x32:
	case 0x33:
	case 0x34:
	case 0x35:
	case 0x36:
	case 0x37:
	case 0x38:
	case 0x39:
	case 0x3A:
	case 0x3B:
	case 0x3C:
	case 0x3D:
	case 0x3E:
	case 0x3F:
		return psg_.waveRamRead(p & 0xF);
	case 0x44:
		return lcd_.peekLy();
	case 0x4C:
		if (!biosMode_)
			return 0xFF;

		break;
	case 0x50:
		return 0xFE | !biosMode_;
	case 0x56:
		if (isCgb() && !isCgbDmg()) {
			if (linked_ && !agbFlag_ && ((ioamhram_[0x156] & 0xC0) == 0xC0) && cart_.getIrSignal(Infrared::linked_gb, cc))
				return ioamhram_[0x156] & ~0x02;
		}

		return ioamhram_[0x156] | 0x02;
	default:
		break;
	}

	return ioamhram_[p + 0x100];
}

unsigned Memory::nontrivial_peek(unsigned const p, gb_ulong const cc) {
	if (p < mm_hram_begin) {
		if (lastOamDmaUpdate_ != disabled_time) {
			if (cart_.isInOamDmaConflictArea(p) && oamDmaPos_ < oam_size) {
				int const r = isCgb() && cart_.oamDmaSrc() != oam_dma_src_wram && p >= mm_wram_begin
					? cart_.wramdata(ioamhram_[0x146] >> 4 & 1)[p & 0xFFF]
					: ioamhram_[oamDmaPos_];

				return r;
			}
		}

		if (p < mm_wram_begin) {
			if (p < mm_vram_begin)
				return cart_.romdata(p >> 14)[p];

			if (p < mm_sram_begin)
				return cart_.vrambankptr()[p];

			if (cart_.rsrambankptr())
				return cart_.rsrambankptr()[p];

			if (cart_.disabledRam())
				return cartBus_;

			if (cart_.isHuC1())
				return 0xC0 | (linked_ && cart_.getIrSignal(Infrared::linked_gb, cc));

			if (cart_.isHuC3() || cart_.isPocketCamera() || cart_.hasIo())
				return 0xFF; // unsafe to peek

			return cart_.rtcRead(); // safe to peek
		}

		if (p < mm_oam_begin)
			return cart_.wramdata(p >> 12 & 1)[p & 0xFFF];

		gb_long const ffp = static_cast<gb_long>(p) - mm_io_begin;
		if (ffp >= 0)
			return megaDuck_ && ffp < 0x80 ? megaDuckFfPeek(ffp, cc) : nontrivial_ff_peek(ffp, cc);

		if (oamDmaPos_ < oam_size)
			return 0xFF;

		if (p >= (mm_oam_begin + oam_size) && isCgb() && !agbFlag_)
			return ioamhram_[(p - mm_oam_begin) & 0xE7];
	}

	return ioamhram_[p - mm_oam_begin];
}

void Memory::nontrivial_ff_write(unsigned const p, unsigned data, gb_ulong const cc) {
	if (lastOamDmaUpdate_ != disabled_time)
		updateOamDma(cc);

	switch (p & 0xFF) {
	case 0x00:
		if (icdJoyp_) { // armSNES
			ioamhram_[0x100] = (ioamhram_[0x100] & ~0x30u) | (data & 0x30);
			icdInput_ = icdJoyp_(icdP_, data >> 4 & 1, data >> 5 & 1) & 0xF;
			updateInput();
			return;
		}
		if ((data ^ ioamhram_[0x100]) & 0x30) {
			if (isSgb())
				sgb_.onJoypad(ioamhram_[0x100], data);

			ioamhram_[0x100] = (ioamhram_[0x100] & ~0x30u) | (data & 0x30);
			updateInput();
		}

		return;
	case 0x01:
		updateSerial(cc);
		break;
	case 0x02:
		updateSerial(cc);
		serialCnt_ = 8;
		if ((data & 0x81) == 0x81) {
			intreq_.setEventTime<intevent_serial>(data & (isCgb() & !isCgbDmg()) * 2
				? cc - (cc - tima_.divLastUpdate()) % 8 + 0x10 * serialCnt_
				: cc - (cc - tima_.divLastUpdate()) % 0x100 + 0x200 * serialCnt_);
		} else
			intreq_.setEventTime<intevent_serial>(disabled_time);

		data |= 0x7E - (isCgb() & !isCgbDmg()) * 2;
		break;
	case 0x04:
		if (intreq_.eventTime(intevent_serial) != disabled_time
				&& intreq_.eventTime(intevent_serial) > cc) {
			gb_ulong const t = intreq_.eventTime(intevent_serial);
			gb_ulong const n = ioamhram_[0x102] & isCgb() * 2
				? t + (cc - t) % 8 - 2 * ((cc - t) & 4)
				: t + (cc - t) % 0x100 - 2 * ((cc - t) & 0x80);
			intreq_.setEventTime<intevent_serial>(std::max(cc, n));
		}
		psg_.generateSamples(cc, isDoubleSpeed());
		psg_.divReset(isDoubleSpeed());
		tima_.divReset(cc, TimaInterruptRequester(intreq_));
		return;
	case 0x05:
		tima_.setTima(data, cc, TimaInterruptRequester(intreq_));
		break;
	case 0x06:
		tima_.setTma(data, cc, TimaInterruptRequester(intreq_));
		break;
	case 0x07:
		data |= 0xF8;
		tima_.setTac(data, cc, TimaInterruptRequester(intreq_), agbFlag_);
		break;
	case 0x0F:
		updateIrqs(cc + 1 + isDoubleSpeed());
		intreq_.setIfreg(0xE0 | data);
		return;
	case 0x10:
		if (!psg_.isEnabled())
			return;

		psg_.generateSamples(cc, isDoubleSpeed());
		psg_.setNr10(data);
		data |= 0x80;
		break;
	case 0x11:
		if (!psg_.isEnabled()) {
			if (isCgb())
				return;

			data &= 0x3F;
		}

		psg_.generateSamples(cc, isDoubleSpeed());
		psg_.setNr11(data);
		data |= 0x3F;
		break;
	case 0x12:
		if (!psg_.isEnabled())
			return;

		psg_.generateSamples(cc, isDoubleSpeed());
		psg_.setNr12(data);
		break;
	case 0x13:
		if (!psg_.isEnabled())
			return;

		psg_.generateSamples(cc, isDoubleSpeed());
		psg_.setNr13(data);
		return;
	case 0x14:
		if (!psg_.isEnabled())
			return;

		psg_.generateSamples(cc, isDoubleSpeed());
		psg_.setNr14(data, isDoubleSpeed());
		data |= 0xBF;
		break;
	case 0x16:
		if (!psg_.isEnabled()) {
			if (isCgb())
				return;

			data &= 0x3F;
		}

		psg_.generateSamples(cc, isDoubleSpeed());
		psg_.setNr21(data);
		data |= 0x3F;
		break;
	case 0x17:
		if (!psg_.isEnabled())
			return;

		psg_.generateSamples(cc, isDoubleSpeed());
		psg_.setNr22(data);
		break;
	case 0x18:
		if (!psg_.isEnabled())
			return;

		psg_.generateSamples(cc, isDoubleSpeed());
		psg_.setNr23(data);
		return;
	case 0x19:
		if (!psg_.isEnabled())
			return;

		psg_.generateSamples(cc, isDoubleSpeed());
		psg_.setNr24(data, isDoubleSpeed());
		data |= 0xBF;
		break;
	case 0x1A:
		if (!psg_.isEnabled())
			return;

		psg_.generateSamples(cc, isDoubleSpeed());
		psg_.setNr30(data, interrupter_.getPc());
		data |= 0x7F;
		break;
	case 0x1B:
		if (!psg_.isEnabled() && isCgb())
			return;

		psg_.generateSamples(cc, isDoubleSpeed());
		psg_.setNr31(data);
		return;
	case 0x1C:
		if (!psg_.isEnabled())
			return;

		psg_.generateSamples(cc, isDoubleSpeed());
		psg_.setNr32(data);
		data |= 0x9F;
		break;
	case 0x1D:
		if (!psg_.isEnabled())
			return;

		psg_.generateSamples(cc, isDoubleSpeed());
		psg_.setNr33(data);
		return;
	case 0x1E:
		if (!psg_.isEnabled())
			return;

		psg_.generateSamples(cc, isDoubleSpeed());
		psg_.setNr34(data);
		data |= 0xBF;
		break;
	case 0x20:
		if (!psg_.isEnabled() && isCgb())
			return;

		psg_.generateSamples(cc, isDoubleSpeed());
		psg_.setNr41(data);
		return;
	case 0x21:
		if (!psg_.isEnabled())
			return;

		psg_.generateSamples(cc, isDoubleSpeed());
		psg_.setNr42(data);
		break;
	case 0x22:
		if (!psg_.isEnabled())
			return;

		psg_.generateSamples(cc, isDoubleSpeed());
		psg_.setNr43(data);
		break;
	case 0x23:
		if (!psg_.isEnabled())
			return;

		psg_.generateSamples(cc, isDoubleSpeed());
		psg_.setNr44(data);
		data |= 0xBF;
		break;
	case 0x24:
		if (!psg_.isEnabled())
			return;

		psg_.generateSamples(cc, isDoubleSpeed());
		psg_.setSoVolume(data);
		break;
	case 0x25:
		if (!psg_.isEnabled())
			return;

		psg_.generateSamples(cc, isDoubleSpeed());
		psg_.mapSo(data);
		break;
	case 0x26:
		if ((ioamhram_[0x126] ^ data) & 0x80) {
			psg_.generateSamples(cc, isDoubleSpeed());

			if (!(data & 0x80)) {
				for (unsigned i = 0x10; i < 0x26; ++i)
					nontrivial_ff_write(i, 0, cc);

				psg_.setEnabled(false);
			} else {
				psg_.reset(isDoubleSpeed());
				psg_.setEnabled(true);
			}
		}

		data = (data & 0x80) | (ioamhram_[0x126] & 0x7F);
		break;
	case 0x30:
	case 0x31:
	case 0x32:
	case 0x33:
	case 0x34:
	case 0x35:
	case 0x36:
	case 0x37:
	case 0x38:
	case 0x39:
	case 0x3A:
	case 0x3B:
	case 0x3C:
	case 0x3D:
	case 0x3E:
	case 0x3F:
		psg_.generateSamples(cc, isDoubleSpeed());
		psg_.waveRamWrite(p & 0xF, data);
		break;
	case 0x40:
		if (ioamhram_[0x140] != data) {
			if ((ioamhram_[0x140] ^ data) & lcdc_en) {
				unsigned const stat = data & lcdc_en ? ioamhram_[0x141] : lcd_.getStat(ioamhram_[0x145], cc);
				bool const hdmaEnabled = lcd_.hdmaIsEnabled();

				lcd_.lcdcChange(data, cc);
				ioamhram_[0x144] = 0;
				ioamhram_[0x141] &= 0xF8;

				if (data & lcdc_en) {
					if (ioamhram_[0x141] & lcdstat_lycirqen && ioamhram_[0x145] == 0 && !(stat & lcdstat_lycflag))
						intreq_.flagIrq(2);

					intreq_.setEventTime<intevent_blit>(blanklcd_
						? lcd_.nextMode1IrqTime()
						: lcd_.nextMode1IrqTime() + (lcd_cycles_per_frame << isDoubleSpeed()));
				} else {
					ioamhram_[0x141] |= stat & lcdstat_lycflag;
					intreq_.setEventTime<intevent_blit>(
						cc + (lcd_cycles_per_line * 4 << isDoubleSpeed()));

					if (hdmaEnabled)
						flagHdmaReq(intreq_);
				}
			} else
				lcd_.lcdcChange(data, cc);

			ioamhram_[0x140] = data;
		}

		return;
	case 0x41:
		lcd_.lcdstatChange(data, cc);
		if (!(ioamhram_[0x140] & lcdc_en) && (ioamhram_[0x141] & lcdstat_lycflag)
				&& (~ioamhram_[0x141] & lcdstat_lycirqen & (isCgb() ? data : -1))) {
			intreq_.flagIrq(2);
		}
		data = (ioamhram_[0x141] & 0x87) | (data & 0x78);
		break;
	case 0x42:
		lcd_.scyChange(data, cc);
		break;
	case 0x43:
		lcd_.scxChange(data, cc);
		break;
	case 0x45:
		lcd_.lycRegChange(data, cc);
		break;
	case 0x46:
		lastOamDmaUpdate_ = cc;
		oamDmaStartPos_ = (oamDmaPos_ + 2) & 0xFF;
		intreq_.setEventTime<intevent_oam>(std::min(intreq_.eventTime(intevent_oam), cc + 2 * oamDmaStep()));
		ioamhram_[0x146] = data;
		oamDmaInitSetup();
		return;
	case 0x47:
		if (!isCgb() || isCgbDmg())
			lcd_.dmgBgPaletteChange(data, cc);

		break;
	case 0x48:
		if (!isCgb() || isCgbDmg())
			lcd_.dmgSpPalette1Change(data, cc);

		break;
	case 0x49:
		if (!isCgb() || isCgbDmg())
			lcd_.dmgSpPalette2Change(data, cc);

		break;
	case 0x4A:
		lcd_.wyChange(data, cc);
		break;
	case 0x4B:
		lcd_.wxChange(data, cc);
		break;
	case 0x4C:
		if (!biosMode_)
			return;

		break;
	case 0x4D:
		if (isCgb() && !isCgbDmg())
			ioamhram_[0x14D] = (ioamhram_[0x14D] & ~1u) | (data & 1);

		return;
	case 0x4F:
		if (isCgb() && !isCgbDmg()) {
			cart_.setVrambank(data & 1);
			ioamhram_[0x14F] = 0xFE | data;
		}

		return;
	case 0x50:
		if (!biosMode_)
			return;

		if (data & 1) {
			if (isCgb() && (ioamhram_[0x14C] & 0x04)) {
				lcd_.copyCgbPalettesToDmg();
				lcd_.dmgBgPaletteChange(ioamhram_[0x147], cc);
				lcd_.dmgSpPalette1Change(ioamhram_[0x148], cc);
				lcd_.dmgSpPalette2Change(ioamhram_[0x149], cc);
				lcd_.setCgbDmg(true);
				// armGBA: VRAM's second bank, which the game can't reach, is cleared as a state load
				// clears it. Nintendo's boot ROM leaves it clear; SameBoy's leaves attributes there that
				// would flip and hide the game's tiles.
				std::fill_n(cart_.vramdata() + vrambank_size(), vrambank_size(), 0);
				ioamhram_[0x102] |= 0x02;
				ioamhram_[0x14D] |= 0x81;
				ioamhram_[0x156] |= 0xC1;
				ioamhram_[0x16B] |= 0xFF;
				ioamhram_[0x170] |= 0x07;
				ioamhram_[0x174] |= 0xFF;
			}

			biosMode_ = false;
		}

		return;
	case 0x51:
		dmaSource_ = data << 8 | (dmaSource_ & 0xFF);
		return;
	case 0x52:
		dmaSource_ = (dmaSource_ & 0xFF00) | (data & 0xF0);
		return;
	case 0x53:
		dmaDestination_ = data << 8 | (dmaDestination_ & 0xFF);
		return;
	case 0x54:
		dmaDestination_ = (dmaDestination_ & 0xFF00) | (data & 0xF0);
		return;
	case 0x55:
		if (isCgb() && !isCgbDmg()) {
			ioamhram_[0x155] = data & 0x7F;

			if (lcd_.hdmaIsEnabled()) {
				if (!(data & 0x80)) {
					ioamhram_[0x155] |= 0x80;
					lcd_.disableHdma(cc);
				}
			} else {
				if (data & 0x80) {
					lcd_.enableHdma(cc);
				} else
					flagGdmaReq(intreq_);
			}
		}

		return;
	case 0x56:
		if (isCgb() && !isCgbDmg()) {
			if (!agbFlag_) {
				cart_.setIrSignal(Infrared::this_gb, data & 1);
				cart_.setRemoteActive((data & 0xC0) == 0xC0);
			}

			ioamhram_[0x156] = (data & 0xC1) | (ioamhram_[0x156] & 0x3E);
		}

		return;
	case 0x68:
		if (isCgb() && !isCgbDmg())
			ioamhram_[0x168] = data | 0x40;

		return;
	case 0x69:
		if (isCgb() && !isCgbDmg()) {
			unsigned index = ioamhram_[0x168] & 0x3F;
			lcd_.cgbBgColorChange(index, data, cc);
			ioamhram_[0x168] = (ioamhram_[0x168] & ~0x3Fu)
				| ((index + (ioamhram_[0x168] >> 7)) & 0x3F);
		}

		return;
	case 0x6A:
		if (isCgb() && !isCgbDmg())
			ioamhram_[0x16A] = data | 0x40;

		return;
	case 0x6B:
		if (isCgb() && !isCgbDmg()) {
			unsigned index = ioamhram_[0x16A] & 0x3F;
			lcd_.cgbSpColorChange(index, data, cc);
			ioamhram_[0x16A] = (ioamhram_[0x16A] & ~0x3Fu)
				| ((index + (ioamhram_[0x16A] >> 7)) & 0x3F);
		}

		return;
	case 0x6C:
		if (isCgb() && !isCgbDmg()) {
			if (biosMode_)
				lcd_.setSpPriority(data & 1, cc);

			ioamhram_[0x16C] = data | 0xFE;
		}

		return;
	case 0x70:
		if (isCgb() && !isCgbDmg()) {
			cart_.setWrambank(data & 0x07 ? data & 0x07 : 1);
			ioamhram_[0x170] = data | 0xF8;
		}

		return;
	case 0x72:
	case 0x73:
		if (isCgb())
			break;

		return;
	case 0x74:
		if (isCgb() && !isCgbDmg())
			break;

		return;
	case 0x75:
		if (isCgb())
			ioamhram_[0x175] = data | 0x8F;

		return;
	case 0xFF:
		intreq_.setIereg(data);
		break;
	default:
		return;
	}

	ioamhram_[p + 0x100] = data;
}

void Memory::nontrivial_write(unsigned const p, unsigned const data, gb_ulong const cc) {
	if (lastOamDmaUpdate_ != disabled_time) {
		updateOamDma(cc);

		if (cart_.isInOamDmaConflictArea(p) && oamDmaPos_ < oam_size) {
			if (isCgb()) {
				if (p < mm_wram_begin)
					ioamhram_[oamDmaPos_] = cart_.oamDmaSrc() != oam_dma_src_vram ? data : 0;
				else if (cart_.oamDmaSrc() != oam_dma_src_wram)
					cart_.wramdata(ioamhram_[0x146] >> 4 & 1)[p & 0xFFF] = data;
			} else {
				ioamhram_[oamDmaPos_] = cart_.oamDmaSrc() == oam_dma_src_wram
					? ioamhram_[oamDmaPos_] & data
					: data;
			}

			return;
		}
	}

	if (p < mm_oam_begin) {
		if (p < mm_sram_begin) {
			if (p < mm_vram_begin) {
				cart_.mbcWrite(p, data, cc);
			} else if (lcd_.vramWritable(cc)) {
				lcd_.vramChange(cc);
				cart_.vrambankptr()[p] = data;
			}
		} else if (p < mm_wram_begin) {
			if (cart_.sramHookWrite(p, data))
				;
			else if (cart_.wsrambankptr())
				cart_.wsrambankptr()[p] = data;
			else if (cart_.isHuC1()) {
				cart_.setIrSignal(Infrared::this_gb, data & 1);
			} else if (cart_.hasIo())
				cart_.ioWrite(p, data, cc);
			else if (cart_.isHuC3())
				cart_.HuC3Write(p, data, cc);
			else if (cart_.isPocketCamera())
				cart_.cameraWrite(p, data, cc);
			else
				cart_.rtcWrite(data, cc);
		} else
			cart_.wramdata(p >> 12 & 1)[p & 0xFFF] = data;
	} else if (p - mm_hram_begin >= 0x7Fu) {
		gb_long const ffp = static_cast<gb_long>(p) - mm_io_begin;
		if (ffp < 0) {
			bool const validOam = p < (mm_oam_begin + oam_size);
			if (lcd_.oamWritable(cc) && oamDmaPos_ >= oam_size
					&& (validOam || (isCgb() && !agbFlag_))) {
				lcd_.oamChange(cc);
				ioamhram_[(p - mm_oam_begin) & (validOam ? 0xFF : 0xE7)] = data;
			}
		} else if (megaDuck_ && ffp < 0x80)
			megaDuckFfWrite(ffp, data, cc);
		else
			nontrivial_ff_write(ffp, data, cc);
	} else
		ioamhram_[p - mm_oam_begin] = data;
}

LoadRes Memory::loadROM(Array<unsigned char> &buffer, unsigned flags, std::string const &filepath) {
	bool const cgbMode = flags & GB::LoadFlag::CGB_MODE;

	megaDuck_ = flags & GB::LoadFlag::MEGA_DUCK;

	if (LoadRes const fail = cart_.loadROM(buffer, cgbMode, megaDuck_ ? 1 + (flags / GB::LoadFlag::MEGA_DUCK_MAPPER & 7) : 0, filepath))
		return fail;

	agbFlag_ = flags & GB::LoadFlag::GBA_FLAG;
	gbIsSgb_ = flags & GB::LoadFlag::SGB_MODE;

	psg_.init(cart_.isCgb(), agbFlag_);
	lcd_.reset(ioamhram_, cart_.vramdata(), cart_.isCgb(), agbFlag_);
	interrupter_.setGameShark(std::string());

	if (!filepath.empty() && agbFlag_ && (crc32(0, bios_.get(), bios_.size()) == 0x41884E46)) { // patch cgb bios to re'd agb bios equal
		bios_.get()[0xF3] ^= 0x03;
		for (unsigned i = 0xF5; i < 0xFB; i++)
			bios_.get()[i] = bios_.get()[i + 1];

		bios_.get()[0xFB] ^= 0x74;
	}

	return LOADRES_OK;
}

std::size_t Memory::fillSoundBuffer(gb_ulong cc) {
	psg_.generateSamples(cc, isDoubleSpeed());
	std::size_t samples = psg_.fillBuffer();
	if (isSgb() && !icdJoyp_) // armSNES
		sgb_.accumulateSamples(samples);

	if (cart_.isHuC3())
		cart_.accumulateSamples(cc);

	totalSamplesEmitted_ += samples;
	return samples;
}

bool Memory::getMemoryArea(int which, unsigned char **data, int *length) {
	if (!data || !length)
		return false;

	switch (which) {
	case 4: // oam
		*data = &ioamhram_[0];
		*length = 160;
		return true;
	case 5: // hram
		*data = &ioamhram_[384];
		*length = 127;
		return true;
	case 6: // bgpal
		*data = (unsigned char *)lcd_.bgPalette();
		*length = 32;
		return true;
	case 7: // sppal
		*data = (unsigned char *)lcd_.spPalette();
		*length = 32;
		return true;
	default: // pass to cartridge
		return cart_.getMemoryArea(which, data, length);
	}
}

int Memory::serialLeft(gb_ulong const cc) const {
	gb_ulong const end = intreq_.eventTime(intevent_serial);
	return end == disabled_time ? -1 : end > cc ? static_cast<int>(end - cc) : 0;
}

int Memory::linkStatus(int which) {
	switch (which) {
	case 256: // ClockSignaled
		return linkClockTrigger_;
	case 257: // AckClockSignal
		linkClockTrigger_ = false;
		return 0;
	case 258: // GetOut
		return ioamhram_[0x101] & 0xFF;
	case 259: // InfraredSignaled
		return cart_.getIrTrigger();
	case 260: // AckInfraredSignal
		cart_.ackIrTrigger();
		return 0;
	case 261: // GetInfraredOut
		return cart_.getIrSignal(Infrared::this_gb, 0);
	case 262: // ShiftInOn
		cart_.setIrSignal(Infrared::linked_gb, true);
		return 0;
	case 263: // ShiftInOff
		cart_.setIrSignal(Infrared::linked_gb, false);
		return 0;
	case 264: // enable link connection
		linked_ = true;
		return 0;
	case 265: // disable link connection
		linked_ = false;
		return 0;
	case 266: // the other Game Boy's clock ended this one's transfer, so its own clock stops
		intreq_.setEventTime<intevent_serial>(disabled_time);
		return 0;
	case 267: // GetControl
		return ioamhram_[0x102] & 0xFF;
	default: // ShiftIn
		if (!(ioamhram_[0x102] & 0x01) || (ioamhram_[0x102] & 0x80)) { // was enabled
			ioamhram_[0x101] = which;
			ioamhram_[0x102] &= 0x7F;
			intreq_.flagIrq(8);
		}
		return 0;
	}

	return -1;
}

SYNCFUNC(Memory) {
	SSS(cart_);
	SSS(sgb_);
	NSS(ioamhram_);
	NSS(divLastUpdate_);
	NSS(lastOamDmaUpdate_);
	NSS(lastCartBusUpdate_);
	SSS(intreq_);
	SSS(tima_);
	SSS(lcd_);
	SSS(psg_);
	NSS(dmaSource_);
	NSS(dmaDestination_);
	NSS(oamDmaPos_);
	NSS(oamDmaStartPos_);
	NSS(serialCnt_);
	NSS(cartBus_);
	NSS(blanklcd_);
	NSS(biosMode_);
	NSS(stopped_);
	NSS(linked_);
	NSS(linkClockTrigger_);
	NSS(totalSamplesEmitted_);
	NSS(icdInput_); // armSNES
	// armGBA: the PPU reads the sprites from OAM, or from nothing while an OAM DMA has the bus.
	if (isReader)
		lcd_.setOam(lastOamDmaUpdate_ != disabled_time && oamDmaPos_ < oam_size ? cart_.rdisabledRam() : ioamhram_);
}
