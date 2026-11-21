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
#include "channel1.h"
#include "psgdef.h"
#include "../savestate.h"

#include <algorithm>

using namespace gambatte;

Channel1::SweepUnit::SweepUnit(MasterDisabler &disabler, DutyUnit &dutyUnit)
: disableMaster_(disabler)
, dutyUnit_(dutyUnit)
, shadow_(0)
, nr0_(0)
, neg_(false)
, cgb_(false)
{
}

unsigned Channel1::SweepUnit::calcFreq() {
	unsigned const freq = nr0_ & psg_nr10_neg
		? shadow_ - (shadow_ >> (nr0_ & psg_nr10_rsh))
		: shadow_ + (shadow_ >> (nr0_ & psg_nr10_rsh));
	if (nr0_ & psg_nr10_neg)
		neg_ = true;
	if (freq & 2048)
		disableMaster_();

	return freq;
}

void Channel1::SweepUnit::event() {
	gb_ulong const period = (nr0_ & psg_nr10_time) / (1u * psg_nr10_time & -psg_nr10_time);

	if (period) {
		unsigned const freq = calcFreq();

		if (!(freq & 2048) && nr0_ & psg_nr10_rsh) {
			shadow_ = freq;
			dutyUnit_.setFreq(freq, counter_);
			calcFreq();
		}

		counter_ += period << 14;
	} else
		counter_ += 8u << 14;
}

void Channel1::SweepUnit::nr0Change(unsigned newNr0) {
	if (neg_ && !(newNr0 & psg_nr10_neg))
		disableMaster_();

	nr0_ = newNr0;
}

void Channel1::SweepUnit::nr4Init(gb_ulong const cc) {
	neg_ = false;
	shadow_ = dutyUnit_.freq();

	unsigned const period = (nr0_ & psg_nr10_time) / (1u * psg_nr10_time & -psg_nr10_time);
	unsigned const rsh = nr0_ & psg_nr10_rsh;

	if (period | rsh)
		counter_ = ((((cc + 2 + cgb_ * 2) >> 14) + (period ? period : 8)) << 14) + 2;
	else
		counter_ = counter_disabled;

	if (rsh)
		calcFreq();
}

void Channel1::SweepUnit::reset() {
	counter_ = counter_disabled;
}

void Channel1::SweepUnit::saveState(SaveState &state) const {
	state.spu.ch1.sweep.counter = counter_;
	state.spu.ch1.sweep.shadow = shadow_;
	state.spu.ch1.sweep.nr0 = nr0_;
	state.spu.ch1.sweep.neg = neg_;
}

void Channel1::SweepUnit::loadState(SaveState const &state) {
	// armGBA: as saved, even when past, as the machine had it
	counter_ = state.spu.ch1.sweep.counter;
	shadow_ = state.spu.ch1.sweep.shadow;
	nr0_ = state.spu.ch1.sweep.nr0;
	neg_ = state.spu.ch1.sweep.neg;
}

template<bool isReader>
void Channel1::SweepUnit::SyncState(NewState *ns) {
	NSS(counter_);
	NSS(shadow_);
	NSS(nr0_);
	NSS(neg_);
	NSS(cgb_);
}

Channel1::Channel1()
: staticOutputTest_(*this, dutyUnit_)
, disableMaster_(master_, dutyUnit_)
, lengthCounter_(disableMaster_, 0x3F)
, envelopeUnit_(staticOutputTest_)
, sweepUnit_(disableMaster_, dutyUnit_)
, nextEventUnit_(0)
, soMask_(0)
, prevOut_(0)
, nr4_(0)
, vol_(0)
, master_(false)
{
	setEvent();
}

void Channel1::setEvent() {
	nextEventUnit_ = &sweepUnit_;
	if (envelopeUnit_.counter() < nextEventUnit_->counter())
		nextEventUnit_ = &envelopeUnit_;
	if (lengthCounter_.counter() < nextEventUnit_->counter())
		nextEventUnit_ = &lengthCounter_;
}

void Channel1::setNr0(unsigned data) {
	sweepUnit_.nr0Change(data);
	setEvent();
}

void Channel1::setNr1(unsigned data, gb_ulong cc) {
	lengthCounter_.nr1Change(data, nr4_, cc);
	dutyUnit_.nr1Change(data, cc);
	setEvent();
}

void Channel1::setNr2(unsigned data, gb_ulong cc) {
	envelopeUnit_.nr2Change(data, cc, master_);
	if (!(data & (psg_nr2_initvol | psg_nr2_inc)))
		disableMaster_();
	else
		staticOutputTest_(cc);

	setEvent();
}

void Channel1::setNr3(unsigned data, gb_ulong cc) {
	dutyUnit_.nr3Change(data, cc);
	setEvent();
}

void Channel1::setNr4(unsigned data, gb_ulong cc, gb_ulong ref) {
	lengthCounter_.nr4Change(nr4_, data, cc);
	dutyUnit_.nr4Change(data, cc, ref, master_);
	nr4_ = data;

	if (nr4_ & psg_nr4_init) {
		nr4_ -= psg_nr4_init;
		master_ = !envelopeUnit_.nr4Init(cc);
		sweepUnit_.nr4Init(cc);
		staticOutputTest_(cc);
	}

	setEvent();
}

void Channel1::setSo(gb_ulong soMask, gb_ulong cc) {
	soMask_ = soMask;
	staticOutputTest_(cc);
	setEvent();
}

void Channel1::reset() {
	dutyUnit_.reset();
	envelopeUnit_.reset();
	sweepUnit_.reset();
	setEvent();
}

void Channel1::init(bool cgb) {
	sweepUnit_.init(cgb);
}

void Channel1::saveState(SaveState &state, gb_ulong cc) {
	state.spu.prevOut1 = prevOut_;
	sweepUnit_.saveState(state);
	dutyUnit_.saveState(state.spu.ch1.duty, cc);
	envelopeUnit_.saveState(state.spu.ch1.env);
	lengthCounter_.saveState(state.spu.ch1.lcounter);

	state.spu.ch1.nr4 = nr4_;
	state.spu.ch1.master = master_;
}

void Channel1::loadState(SaveState const &state) {
	prevOut_ = state.spu.prevOut1;
	sweepUnit_.loadState(state);
	dutyUnit_.loadState(state.spu.ch1.duty, state.mem.ioamhram.get()[0x111],
		state.spu.ch1.nr4, state.spu.cycleCounter);
	envelopeUnit_.loadState(state.spu.ch1.env, state.mem.ioamhram.get()[0x112],
		state.spu.cycleCounter);
	lengthCounter_.loadState(state.spu.ch1.lcounter, state.spu.cycleCounter);

	nr4_ = state.spu.ch1.nr4;
	master_ = state.spu.ch1.master;
}

void Channel1::update(uint_least32_t *buf, gb_ulong const soBaseVol, gb_ulong cc, gb_ulong const end) {
	gb_ulong const outBase = envelopeUnit_.dacIsOn() ? soBaseVol & soMask_ : 0;
	gb_ulong const outLow = outBase * -15;

	while (cc < end) {
		gb_ulong const outHigh = master_
			? outBase * (envelopeUnit_.getVolume() * 2 - 15)
			: outLow;
		gb_ulong const nextMajorEvent = std::min(nextEventUnit_->counter(), end);
		gb_ulong out = dutyUnit_.isHighState() ? outHigh : outLow;

		while (dutyUnit_.counter() <= nextMajorEvent) {
			*buf = out - prevOut_;
			prevOut_ = out;
			buf += std::ptrdiff_t(dutyUnit_.counter()) - cc;
			cc = dutyUnit_.counter();
			dutyUnit_.event();
			out = dutyUnit_.isHighState() ? outHigh : outLow;
		}
		if (cc < nextMajorEvent) {
			*buf = out - prevOut_;
			prevOut_ = out;
			buf += nextMajorEvent - cc;
			cc = nextMajorEvent;
		}
		if (nextEventUnit_->counter() == nextMajorEvent) {
			nextEventUnit_->event();
			setEvent();
		}
	}

	vol_ = dutyUnit_.isHighState(cc) ? envelopeUnit_.getVolume() : 0;

	if (cc >= SoundUnit::counter_max) {
		dutyUnit_.resetCounters(cc);
		lengthCounter_.resetCounters(cc);
		envelopeUnit_.resetCounters(cc);
		sweepUnit_.resetCounters(cc);
	}
}

SYNCFUNC(Channel1) {
	SSS(lengthCounter_);
	SSS(dutyUnit_);
	SSS(envelopeUnit_);
	SSS(sweepUnit_);

	EBS(nextEventUnit_, 0);
	EVS(nextEventUnit_, &dutyUnit_, 1);
	EVS(nextEventUnit_, &sweepUnit_, 2);
	EVS(nextEventUnit_, &envelopeUnit_, 3);
	EVS(nextEventUnit_, &lengthCounter_, 4);
	EES(nextEventUnit_, NULL);

	NSS(soMask_);
	NSS(prevOut_);

	NSS(nr4_);
	NSS(master_);
}
