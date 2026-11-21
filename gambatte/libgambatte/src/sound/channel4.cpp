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
#include "channel4.h"
#include "psgdef.h"
#include "../savestate.h"

#include <algorithm>

using namespace gambatte;

namespace {

gb_ulong toPeriod(unsigned const nr3) {
	unsigned s = nr3 / (1u * psg_nr43_s & -psg_nr43_s) + 3;
	unsigned r = nr3 & psg_nr43_r;

	if (!r) {
		r = 1;
		--s;
	}

	return r << s;
}

}

Channel4::Lfsr::Lfsr()
: backupCounter_(counter_disabled)
, reg_(0x7FFF)
, nr3_(0)
, master_(false)
{
}

void Channel4::Lfsr::updateBackupCounter(gb_ulong const cc) {
	if (backupCounter_ <= cc) {
		gb_ulong const period = toPeriod(nr3_);
		gb_ulong periods = (cc - backupCounter_) / period + 1;
		backupCounter_ += periods * period;

		if (master_ && nr3_ < 0xE * (1u * psg_nr43_s & -psg_nr43_s)) {
			if (nr3_ & psg_nr43_7biten) {
				while (periods > 6) {
					unsigned const xored = (reg_ << 1 ^ reg_) & 0x7E;
					reg_ = (reg_ >> 6 & ~0x7Eu) | xored | xored << 8;
					periods -= 6;
				}

				unsigned const xored = ((reg_ ^ reg_ >> 1) << (7 - periods)) & 0x7F;
				reg_ = (reg_ >> periods & ~(0x80u - (0x80 >> periods))) | xored | xored << 8;
			} else {
				while (periods > 15) {
					reg_ = reg_ ^ reg_ >> 1;
					periods -= 15;
				}

				reg_ = reg_ >> periods | (((reg_ ^ reg_ >> 1) << (15 - periods)) & 0x7FFF);
			}
		}
	}
}

void Channel4::Lfsr::reviveCounter(gb_ulong cc) {
	updateBackupCounter(cc);
	counter_ = backupCounter_;
}

inline void Channel4::Lfsr::event() {
	if (nr3_ < 0xE * (1u * psg_nr43_s & -psg_nr43_s)) {
		unsigned const shifted = reg_ >> 1;
		unsigned const xored = (reg_ ^ shifted) & 1;
		reg_ = shifted | xored << 14;

		if (nr3_ & psg_nr43_7biten)
			reg_ = (reg_ & ~0x40u) | xored << 6;
	}

	counter_ += toPeriod(nr3_);
	backupCounter_ = counter_;
}

bool Channel4::Lfsr::isHighState(gb_ulong const cc) {
	updateBackupCounter(cc);
	return ~reg_ & 1;
}

void Channel4::Lfsr::nr3Change(unsigned newNr3, gb_ulong cc) {
	updateBackupCounter(cc);
	nr3_ = newNr3;
	
	// GSR NOTE: See "Mickey fix" pull request for info
	// https://github.com/pokemon-speedrunning/gambatte-speedrun/pull/27
	counter_ = cc;
}

void Channel4::Lfsr::nr4Init(gb_ulong cc) {
	disableMaster();
	updateBackupCounter(cc);
	master_ = true;
	backupCounter_ += 4;
	counter_ = backupCounter_;
}

void Channel4::Lfsr::reset(gb_ulong cc) {
	nr3_ = 0;
	disableMaster();
	backupCounter_ = cc + toPeriod(nr3_);
}

// armGBA: whether the LFSR counted when saved, once the rest of the sound has loaded and tested its
// output
void Channel4::Lfsr::restoreCounting(unsigned char const counting) {
	if (counting)
		counter_ = counting == 2 ? backupCounter_ : 1 * counter_disabled;
}

void Channel4::Lfsr::resetCc(gb_ulong cc, gb_ulong newCc) {
	updateBackupCounter(cc);
	backupCounter_ -= cc - newCc;
	if (counter_ != counter_disabled)
		counter_ -= cc - newCc;
}

void Channel4::Lfsr::resetCounters(gb_ulong cc) {
	resetCc(cc, cc - counter_max);
}

void Channel4::Lfsr::saveState(SaveState &state, gb_ulong cc) {
	updateBackupCounter(cc);
	state.spu.ch4.lfsr.counter = backupCounter_;
	state.spu.ch4.lfsr.counting = counter_ == counter_disabled ? 1 : 2;
	state.spu.ch4.lfsr.reg = reg_;
}

void Channel4::Lfsr::loadState(SaveState const &state) {
	// armGBA: as saved, even when past, as the machine had it
	counter_ = backupCounter_ = state.spu.ch4.lfsr.counter;
	reg_ = state.spu.ch4.lfsr.reg;
	master_ = state.spu.ch4.master;
	nr3_ = state.mem.ioamhram.get()[0x122];
}

template<bool isReader>
void Channel4::Lfsr::SyncState(NewState *ns) {
	NSS(counter_);
	NSS(backupCounter_);
	NSS(reg_);
	NSS(nr3_);
	NSS(master_);
}

Channel4::Channel4()
: staticOutputTest_(*this, lfsr_)
, disableMaster_(master_, lfsr_)
, lengthCounter_(disableMaster_, 0x3F)
, envelopeUnit_(staticOutputTest_)
, nextEventUnit_(0)
, soMask_(0)
, prevOut_(0)
, nr4_(0)
, vol_(0)
, master_(false)
{
	setEvent();
}

void Channel4::setEvent() {
	nextEventUnit_ = &envelopeUnit_;
	if (lengthCounter_.counter() < nextEventUnit_->counter())
		nextEventUnit_ = &lengthCounter_;
}

void Channel4::setNr1(unsigned data, gb_ulong cc) {
	lengthCounter_.nr1Change(data, nr4_, cc);
	setEvent();
}

void Channel4::setNr2(unsigned data, gb_ulong cc) {
	envelopeUnit_.nr2Change(data, cc, master_);
	if (!(data & (psg_nr2_initvol | psg_nr2_inc)))
		disableMaster_();
	else
		staticOutputTest_(cc);

	setEvent();
}

void Channel4::setNr4(unsigned const data, gb_ulong const cc) {
	lengthCounter_.nr4Change(nr4_, data, cc);
	nr4_ = data;

	if (nr4_ & psg_nr4_init) {
		nr4_ -= psg_nr4_init;
		master_ = !envelopeUnit_.nr4Init(cc);
		if (master_)
			lfsr_.nr4Init(cc);

		staticOutputTest_(cc);
	}

	setEvent();
}

void Channel4::setSo(gb_ulong soMask, gb_ulong cc) {
	soMask_ = soMask;
	staticOutputTest_(cc);
	setEvent();
}

void Channel4::reset(gb_ulong cc) {
	lfsr_.reset(cc);
	envelopeUnit_.reset();
	setEvent();
}

void Channel4::saveState(SaveState &state, gb_ulong cc) {
	state.spu.prevOut4 = prevOut_;
	lfsr_.saveState(state, cc);
	envelopeUnit_.saveState(state.spu.ch4.env);
	lengthCounter_.saveState(state.spu.ch4.lcounter);

	state.spu.ch4.nr4 = nr4_;
	state.spu.ch4.master = master_;
}

void Channel4::loadState(SaveState const &state) {
	prevOut_ = state.spu.prevOut4;
	lfsr_.loadState(state);
	envelopeUnit_.loadState(state.spu.ch4.env, state.mem.ioamhram.get()[0x121],
		state.spu.cycleCounter);
	lengthCounter_.loadState(state.spu.ch4.lcounter, state.spu.cycleCounter);

	nr4_ = state.spu.ch4.nr4;
	master_ = state.spu.ch4.master;
}

void Channel4::update(uint_least32_t *buf, gb_ulong const soBaseVol, gb_ulong cc, gb_ulong const end) {
	gb_ulong const outBase = envelopeUnit_.dacIsOn() ? soBaseVol & soMask_ : 0;
	gb_ulong const outLow = outBase * -15;

	while (cc < end) {
		gb_ulong const outHigh = outBase * (envelopeUnit_.getVolume() * 2 - 15);
		gb_ulong const nextMajorEvent = std::min(nextEventUnit_->counter(), end);
		gb_ulong out = lfsr_.isHighState() ? outHigh : outLow;
		if (lfsr_.counter() <= nextMajorEvent) {
			Lfsr lfsr = lfsr_;
			while (lfsr.counter() <= nextMajorEvent) {
				*buf += out - prevOut_;
				prevOut_ = out;
				buf += std::ptrdiff_t(lfsr.counter()) - cc;
				cc = lfsr.counter();
				lfsr.event();
				out = lfsr.isHighState() ? outHigh : outLow;
			}
			lfsr_ = lfsr;
		}
		if (cc < nextMajorEvent) {
			*buf += out - prevOut_;
			prevOut_ = out;
			buf += nextMajorEvent - cc;
			cc = nextMajorEvent;
		}
		if (nextEventUnit_->counter() == nextMajorEvent) {
			nextEventUnit_->event();
			setEvent();
		}
	}

	vol_ = lfsr_.isHighState(cc) ? envelopeUnit_.getVolume() : 0;

	if (cc >= SoundUnit::counter_max) {
		lengthCounter_.resetCounters(cc);
		lfsr_.resetCounters(cc);
		envelopeUnit_.resetCounters(cc);
	}
}

SYNCFUNC(Channel4) {
	SSS(lengthCounter_);
	SSS(envelopeUnit_);
	SSS(lfsr_);

	EBS(nextEventUnit_, 0);
	EVS(nextEventUnit_, &lfsr_, 1);
	EVS(nextEventUnit_, &envelopeUnit_, 2);
	EVS(nextEventUnit_, &lengthCounter_, 3);
	EES(nextEventUnit_, NULL);

	NSS(soMask_);
	NSS(prevOut_);

	NSS(nr4_);
	NSS(master_);
}
