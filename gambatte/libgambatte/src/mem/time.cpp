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

#include "time.h"
#include "../savestate.h"

#include <chrono>

namespace gambatte {

static long long nowNanos() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

Time::Time()
: lastNanos_(nowNanos())
, nanoRest_(0)
, lastCycles_(0)
, useCycles_(true)
, rtcDivisor_(0x400000)
, ds_(0)
, percent_(100)
, clock_(0)
{
}

void Time::saveState(SaveState &state, unsigned long const cc) {
	update(cc);
	state.time.seconds = 0; // old total seconds field, 0 indicates new time system
	// other fields used to be saved, but they no longer need to be saved
}

void Time::loadState(SaveState const &state, bool const ds) {
	lastCycles_ = state.cpu.cycleCounter;
	ds_ = ds;

	// state backwards compat with old time system
	if (state.time.seconds)
		setTime(state.time.seconds * rtcDivisor_ / 2);
}

void Time::resetCc(unsigned long const oldCc, unsigned long const newCc) {
	update(oldCc);
	lastCycles_ -= oldCc - newCc;
}

void Time::speedChange(unsigned long const cc) {
	update(cc);
	ds_ = !ds_;
}

void Time::setTimeMode(bool useCycles, unsigned long const cc) {
	update(cc);
	useCycles_ = useCycles;
	lastCycles_ = cc;
	lastNanos_ = nowNanos();
	nanoRest_ = 0;
}

// armGBA: on the device's time, the clock starts again from now. Staying on the console's cycles, it
// keeps those it hasn't counted yet.
void Time::setDeviceTime(bool const device, unsigned long const cc) {
	if (!device && useCycles_)
		return;
	useCycles_ = !device;
	lastCycles_ = cc;
	lastNanos_ = nowNanos();
	nanoRest_ = 0;
}

void Time::setSpeed(unsigned const percent, unsigned long const cc) {
	update(cc);
	percent_ = percent;
}

unsigned long long Time::diff(unsigned long const cc) {
	unsigned long long diff;
	if (useCycles_) {
		diff = ((cc - lastCycles_) >> ds_) * (unsigned long long) percent_ / 100;
		lastCycles_ = cc;
	} else {
		long long const now = nowNanos();
		unsigned long long const nanos = now > lastNanos_ ? now - lastNanos_ : 0;
		unsigned long long const part = nanos % 1000000000 * rtcDivisor_ + nanoRest_;
		diff = nanos / 1000000000 * rtcDivisor_ + part / 1000000000;
		nanoRest_ = part % 1000000000;
		lastNanos_ = now;
	}
	return diff;
}

SYNCFUNC(Time) {
	NSS(lastCycles_);
	NSS(useCycles_);
	NSS(ds_);
}

}
