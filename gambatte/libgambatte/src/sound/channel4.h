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

#ifndef SOUND_CHANNEL4_H
#define SOUND_CHANNEL4_H

#include "envelope_unit.h"
#include "gbint.h"
#include "length_counter.h"
#include "master_disabler.h"
#include "static_output_tester.h"
#include "newstate.h"

namespace gambatte {

struct SaveState;

class Channel4 {
public:
	Channel4();
	void setNr1(unsigned data, gb_ulong cc);
	void setNr2(unsigned data, gb_ulong cc);
	void setNr3(unsigned data, gb_ulong cc) { lfsr_.nr3Change(data, cc); }
	void setNr4(unsigned data, gb_ulong cc);
	void setSo(gb_ulong soMask, gb_ulong cc);
	unsigned char getVolume() const { return vol_; }
	bool isActive() const { return master_; }
	void update(uint_least32_t *buf, gb_ulong soBaseVol, gb_ulong cc, gb_ulong end);
	void reset(gb_ulong cc);
	void resetCc(gb_ulong cc, gb_ulong newCc) { lfsr_.resetCc(cc, newCc); }
	void saveState(SaveState &state, gb_ulong cc);
	void loadState(SaveState const &state);
	void restoreCounting(SaveState const &state) { lfsr_.restoreCounting(state.spu.ch4.lfsr.counting); }
	template<bool isReader>void SyncState(NewState *ns);

private:
	class Lfsr : public SoundUnit {
	public:
		Lfsr();
		virtual void event();
		virtual void resetCounters(gb_ulong oldCc);
		bool isHighState() const { return ~reg_ & 1; }
		bool isHighState(gb_ulong cc);
		void nr3Change(unsigned newNr3, gb_ulong cc);
		void nr4Init(gb_ulong cc);
		void reset(gb_ulong cc);
		void resetCc(gb_ulong cc, gb_ulong newCc);
		void saveState(SaveState &state, gb_ulong cc);
		void loadState(SaveState const &state);
		void disableMaster() { killCounter(); master_ = false; reg_ = 0x7FFF; }
		void killCounter() { counter_ = counter_disabled; }
		void restoreCounting(unsigned char counting);
		void reviveCounter(gb_ulong cc);
		template<bool isReader>void SyncState(NewState *ns);

	private:
		gb_ulong backupCounter_;
		unsigned short reg_;
		unsigned char nr3_;
		bool master_;

		void updateBackupCounter(gb_ulong cc);
	};

	class Ch4MasterDisabler : public MasterDisabler {
	public:
		Ch4MasterDisabler(bool &m, Lfsr &lfsr) : MasterDisabler(m), lfsr_(lfsr) {}
		virtual void operator()() { MasterDisabler::operator()(); lfsr_.disableMaster(); }

	private:
		Lfsr &lfsr_;
	};

	friend class StaticOutputTester<Channel4, Lfsr>;

	StaticOutputTester<Channel4, Lfsr> staticOutputTest_;
	Ch4MasterDisabler disableMaster_;
	LengthCounter lengthCounter_;
	EnvelopeUnit envelopeUnit_;
	Lfsr lfsr_;
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
