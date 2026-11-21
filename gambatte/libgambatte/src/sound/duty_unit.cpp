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
#include "duty_unit.h"
#include "psgdef.h"

#include <algorithm>

namespace {

int const duty_pattern_len = 8;

bool toOutState(unsigned duty, unsigned pos) {
	return 0x7EE18180 >> (duty * duty_pattern_len + pos) & 1;
}

unsigned toPeriod(unsigned freq) {
	return (2048 - freq) * 2;
}

}

using namespace gambatte;

DutyUnit::DutyUnit()
: nextPosUpdate_(counter_disabled)
, period_(4096)
, pos_(0)
, duty_(0)
, inc_(0)
, high_(false)
, enableEvents_(true)
{
}

void DutyUnit::updatePos(gb_ulong const cc) {
	if (cc >= nextPosUpdate_) {
		gb_ulong const inc = (cc - nextPosUpdate_) / period_ + 1;
		nextPosUpdate_ += period_ * inc;
		pos_ = (pos_ + inc) % duty_pattern_len;
		high_ = toOutState(duty_, pos_);
	}
}

void DutyUnit::setCounter() {
	static unsigned char const nextStateDistance[][duty_pattern_len] = {
		{ 7, 6, 5, 4, 3, 2, 1, 1 },
		{ 1, 6, 5, 4, 3, 2, 1, 2 },
		{ 1, 4, 3, 2, 1, 4, 3, 2 },
		{ 1, 6, 5, 4, 3, 2, 1, 2 }
	};

	if (enableEvents_ && nextPosUpdate_ != counter_disabled) {
		unsigned const npos = (pos_ + 1) % duty_pattern_len;
		counter_ = nextPosUpdate_;
		inc_ = nextStateDistance[duty_][npos];
		if (toOutState(duty_, npos) == high_) {
			counter_ += period_ * inc_;
			inc_ = nextStateDistance[duty_][(npos + inc_) % duty_pattern_len];
		}
	} else
		counter_ = counter_disabled;
}

void DutyUnit::setFreq(unsigned newFreq, gb_ulong cc) {
	updatePos(cc);
	period_ = toPeriod(newFreq);
	setCounter();
}

void DutyUnit::event() {
	static unsigned char const inc[][2] = {
		{ 1, 7 },
		{ 2, 6 },
		{ 4, 4 },
		{ 6, 2 }
	};

	high_ ^= true;
	counter_ += inc_ * period_;
	inc_ = inc[duty_][high_];
}

bool DutyUnit::isHighState(gb_ulong const cc) const {
	bool high = high_;
	if (cc >= nextPosUpdate_) {
		gb_ulong const inc = (cc - nextPosUpdate_) / period_ + 1;
		unsigned char const pos = (pos_ + inc) % duty_pattern_len;
		high = toOutState(duty_, pos);
	}
	return high;
}

void DutyUnit::nr1Change(unsigned newNr1, gb_ulong cc) {
	updatePos(cc);
	duty_ = newNr1 >> 6;
	setCounter();
}

void DutyUnit::nr3Change(unsigned newNr3, gb_ulong cc) {
	setFreq((freq() & 0x700) | newNr3, cc);
}

// GSR NOTE: `master` is required to get 4 sound/ch1_init_pos* hwtests to pass; upstream passes them by happenstance
void DutyUnit::nr4Change(unsigned const newNr4, gb_ulong const cc, gb_ulong const ref, bool const master) {
	setFreq((newNr4 << 8 & 0x700) | (freq() & 0xFF), cc);

	if (newNr4 & psg_nr4_init) {
		nextPosUpdate_ = cc - (cc - ref) % 2 + period_ + 4 - (master << 1);
		setCounter();
	}
}

void DutyUnit::reset() {
	pos_ = 0;
	high_ = false;
	nextPosUpdate_ = counter_disabled;
	setCounter();
}

void DutyUnit::resetCc(gb_ulong cc, gb_ulong newCc) {
	if (nextPosUpdate_ == counter_disabled)
		return;

	updatePos(cc);
	nextPosUpdate_ -= cc - newCc;
	setCounter();
}

void DutyUnit::saveState(SaveState::SPU::Duty &dstate, gb_ulong const cc) {
	updatePos(cc);
	setCounter();
	dstate.nextPosUpdate = nextPosUpdate_;
	dstate.nr3 = freq() & 0xFF;
	dstate.freqHigh = 0x08 | (freq() >> 8 & 7);
	dstate.events = enableEvents_ ? 2 : 1;
	dstate.pos = pos_;
	dstate.high = high_;
}

void DutyUnit::loadState(SaveState::SPU::Duty const &dstate,
		unsigned const nr1, unsigned const nr4, gb_ulong const cc) {
	// armGBA: as saved, even when past, as the machine had it
	nextPosUpdate_ = dstate.nextPosUpdate;
	pos_ = dstate.pos % duty_pattern_len;
	high_ = dstate.high;
	duty_ = nr1 >> 6;
	unsigned const high = dstate.freqHigh & 0x08 ? dstate.freqHigh & 7 : nr4 & 7;
	period_ = toPeriod(high << 8 | dstate.nr3);
	enableEvents_ = true;
	setCounter();
}

void DutyUnit::resetCounters(gb_ulong cc) {
	resetCc(cc, cc - counter_max);
}

// armGBA: whether the unit's events ran when saved, once the rest of the sound has loaded and tested
// its output
void DutyUnit::restoreEvents(unsigned char const events) {
	if (events) {
		enableEvents_ = events == 2;
		setCounter();
	}
}

void DutyUnit::killCounter() {
	enableEvents_ = false;
	setCounter();
}

void DutyUnit::reviveCounter(gb_ulong const cc) {
	updatePos(cc);
	enableEvents_ = true;
	setCounter();
}

SYNCFUNC(DutyUnit) {
	NSS(counter_);
	NSS(nextPosUpdate_);
	NSS(period_);
	NSS(pos_);
	NSS(duty_);
	NSS(inc_);
	NSS(high_);
	NSS(enableEvents_);
}
