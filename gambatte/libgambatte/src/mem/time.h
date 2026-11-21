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

#ifndef TIME_H
#define TIME_H

#include "gbint.h"
#include "newstate.h"

#include <ctime>

namespace gambatte {

struct SaveState;

class Clock {
public:
	virtual void updateClock(gb_ulong const cc) = 0;
	virtual void setTime(unsigned long long const dividers) = 0;
	virtual void setBaseTime(unsigned long long baseTime, gb_ulong const cc) = 0;
};

class Time {
public:
	Time();
	void saveState(SaveState &state, gb_ulong cycleCounter);
	void loadState(SaveState const &state, bool const ds);

	void resetCc(gb_ulong oldCc, gb_ulong newCc);
	void speedChange(gb_ulong cycleCounter);

	void setTimeMode(bool useCycles, gb_ulong cycleCounter);
	void setSpeed(unsigned percent, gb_ulong cycleCounter);
	bool deviceTime() const { return !useCycles_; }
	void setDeviceTime(bool device, gb_ulong cycleCounter);

	void set(Clock *clock) { clock_ = clock; }
	void setTime(unsigned long long const dividers) { if (clock_) clock_->setTime(dividers); }
	void setBaseTime(unsigned long long baseTime, gb_ulong const cc) { if (clock_) clock_->setBaseTime(baseTime, cc); }

	gb_ulong getRtcDivisor() { return rtcDivisor_; }
	void setRtcDivisorOffset(gb_long const rtcDivisorOffset) { rtcDivisor_ = 0x400000 + rtcDivisorOffset; }	

	unsigned long long diff(gb_ulong cycleCounter);

	template<bool isReader>void SyncState(NewState *ns);

private:
	long long lastNanos_;
	unsigned long long nanoRest_;
	gb_ulong lastCycles_;
	bool useCycles_;
	gb_ulong rtcDivisor_;
	bool ds_;
	unsigned percent_;

	Clock *clock_;
	void update(gb_ulong const cc) { if (clock_) clock_->updateClock(cc); }
};

}

#endif
