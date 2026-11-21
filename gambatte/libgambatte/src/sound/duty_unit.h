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

#ifndef DUTY_UNIT_H
#define DUTY_UNIT_H

#include "gbint.h"
#include "sound_unit.h"
#include "master_disabler.h"
#include "../savestate.h"
#include "newstate.h"

namespace gambatte {

class DutyUnit : public SoundUnit {
public:
	DutyUnit();
	virtual void event();
	virtual void resetCounters(gb_ulong oldCc);
	bool isHighState() const { return high_; }
	bool isHighState(gb_ulong cc) const;
	void nr1Change(unsigned newNr1, gb_ulong cc);
	void nr3Change(unsigned newNr3, gb_ulong cc);
	void nr4Change(unsigned newNr4, gb_ulong cc, gb_ulong ref, bool master);
	void reset();
	void resetCc(gb_ulong cc, gb_ulong newCc);
	void saveState(SaveState::SPU::Duty &dstate, gb_ulong cc);
	void loadState(SaveState::SPU::Duty const &dstate, unsigned nr1, unsigned nr4, gb_ulong cc);
	void killCounter();
	void restoreEvents(unsigned char events);
	void reviveCounter(gb_ulong cc);

	//intended for use by SweepUnit only.
	unsigned freq() const { return 2048 - (period_ >> 1); }
	void setFreq(unsigned newFreq, gb_ulong cc);

	template<bool isReader>void SyncState(NewState *ns);

private:
	gb_ulong nextPosUpdate_;
	unsigned short period_;
	unsigned char pos_;
	unsigned char duty_;
	unsigned char inc_;
	bool high_;
	bool enableEvents_;

	void setCounter();
	void setDuty(unsigned nr1);
	void updatePos(gb_ulong cc);
};

class DutyMasterDisabler : public MasterDisabler {
public:
	DutyMasterDisabler(bool &m, DutyUnit &dutyUnit) : MasterDisabler(m), dutyUnit_(dutyUnit) {}
	virtual void operator()() { MasterDisabler::operator()(); dutyUnit_.killCounter(); }

private:
	DutyUnit &dutyUnit_;
};

}

#endif
