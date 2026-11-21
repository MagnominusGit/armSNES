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

#ifndef SOUND_CHANNEL2_H
#define SOUND_CHANNEL2_H

#include "duty_unit.h"
#include "envelope_unit.h"
#include "gbint.h"
#include "length_counter.h"
#include "static_output_tester.h"
#include "newstate.h"

namespace gambatte {

struct SaveState;

class Channel2 {
public:
	Channel2();
	void setNr1(unsigned data, gb_ulong cc);
	void setNr2(unsigned data, gb_ulong cc);
	void setNr3(unsigned data, gb_ulong cc);
	void setNr4(unsigned data, gb_ulong cc, gb_ulong ref);
	void setSo(gb_ulong soMask, gb_ulong cc);
	bool isActive() const { return master_; }
	unsigned char getVolume() const { return vol_; }
	void update(uint_least32_t *buf, gb_ulong soBaseVol, gb_ulong cc, gb_ulong end);
	void reset();
	void resetCc(gb_ulong cc, gb_ulong ncc) { dutyUnit_.resetCc(cc, ncc); }
	void saveState(SaveState &state, gb_ulong cc);
	void loadState(SaveState const &state);
	void restoreEvents(SaveState const &state) { dutyUnit_.restoreEvents(state.spu.ch2.duty.events); }
	template<bool isReader>void SyncState(NewState *ns);

private:
	friend class StaticOutputTester<Channel2, DutyUnit>;

	StaticOutputTester<Channel2, DutyUnit> staticOutputTest_;
	DutyMasterDisabler disableMaster_;
	LengthCounter lengthCounter_;
	DutyUnit dutyUnit_;
	EnvelopeUnit envelopeUnit_;
	SoundUnit *nextEventUnit;
	gb_ulong soMask_;
	gb_ulong prevOut_;
	unsigned char nr4_;
	unsigned char vol_;
	bool master_;

	void setEvent();
};

}

#endif
