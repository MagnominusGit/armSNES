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

#ifndef SOUND_CHANNEL1_H
#define SOUND_CHANNEL1_H

#include "duty_unit.h"
#include "envelope_unit.h"
#include "gbint.h"
#include "length_counter.h"
#include "master_disabler.h"
#include "static_output_tester.h"
#include "newstate.h"

namespace gambatte {

struct SaveState;

class Channel1 {
public:
	Channel1();
	void setNr0(unsigned data);
	void setNr1(unsigned data, gb_ulong cc);
	void setNr2(unsigned data, gb_ulong cc);
	void setNr3(unsigned data, gb_ulong cc);
	void setNr4(unsigned data, gb_ulong cc, gb_ulong ref);
	void setSo(gb_ulong soMask, gb_ulong cc);
	unsigned char getVolume() const { return vol_; }
	bool isActive() const { return master_; }
	void update(uint_least32_t *buf, gb_ulong soBaseVol, gb_ulong cc, gb_ulong end);
	void reset();
	void resetCc(gb_ulong cc, gb_ulong ncc) { dutyUnit_.resetCc(cc, ncc); }
	void init(bool cgb);
	void saveState(SaveState &state, gb_ulong cc);
	void loadState(SaveState const &state);
	void restoreEvents(SaveState const &state) { dutyUnit_.restoreEvents(state.spu.ch1.duty.events); }
	template<bool isReader>void SyncState(NewState *ns);

private:
	class SweepUnit : public SoundUnit {
	public:
		SweepUnit(MasterDisabler &disabler, DutyUnit &dutyUnit);
		virtual void event();
		void nr0Change(unsigned newNr0);
		void nr4Init(gb_ulong cycleCounter);
		void reset();
		void init(bool cgb) { cgb_ = cgb; }
		void saveState(SaveState &state) const;
		void loadState(SaveState const &state);
		template<bool isReader>void SyncState(NewState *ns);

	private:
		MasterDisabler &disableMaster_;
		DutyUnit &dutyUnit_;
		unsigned short shadow_;
		unsigned char nr0_;
		bool neg_;
		bool cgb_;

		unsigned calcFreq();
	};

	friend class StaticOutputTester<Channel1, DutyUnit>;

	StaticOutputTester<Channel1, DutyUnit> staticOutputTest_;
	DutyMasterDisabler disableMaster_;
	LengthCounter lengthCounter_;
	DutyUnit dutyUnit_;
	EnvelopeUnit envelopeUnit_;
	SweepUnit sweepUnit_;
	SoundUnit *nextEventUnit_;
	gb_ulong soMask_;
	gb_ulong prevOut_;
	unsigned char nr4_;
	unsigned char vol_;
	bool master_;

	void setEvent();
};

}

#endif
