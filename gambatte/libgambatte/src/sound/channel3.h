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

#ifndef SOUND_CHANNEL3_H
#define SOUND_CHANNEL3_H

#include "gbint.h"
#include "length_counter.h"
#include "master_disabler.h"
#include "newstate.h"

namespace gambatte {

struct SaveState;

class Channel3 {
public:
	Channel3();
	unsigned char getVolume() const { return vol_; }
	bool isActive() const { return master_; }
	bool isCgb() const { return cgb_; }
	void reset();
	void resetCc(gb_ulong cc, gb_ulong newCc);
	void init(bool cgb, bool agb);
	void setStatePtrs(SaveState &state);
	void saveState(SaveState &state) const;
	void loadState(SaveState const &state);
	void setNr0(unsigned data, gb_ulong cc, unsigned short pc);
	void setNr1(unsigned data, gb_ulong cc) { lengthCounter_.nr1Change(data, nr4_, cc); }
	void setNr2(unsigned data);
	void setNr3(unsigned data) { nr3_ = data; }
	void setNr4(unsigned data, gb_ulong cc);
	void setSo(gb_ulong soMask);
	void update(uint_least32_t *buf, gb_ulong soBaseVol, gb_ulong cc, gb_ulong end);

	unsigned waveRamRead(unsigned index, gb_ulong cc) const {
		if (master_) {
			if (agb_ || (!cgb_ && cc != lastReadTime_))
				return 0xFF;

			index = wavePos_ / 2;
		}

		return waveRam_[index];
	}

	void waveRamWrite(unsigned index, unsigned data, gb_ulong cc) {
		if (master_) {
			if (agb_ || (!cgb_ && cc != lastReadTime_))
				return;

			index = wavePos_ / 2;
		}

		waveRam_[index] = data;
	}

	template<bool isReader>void SyncState(NewState *ns);

private:
	class Ch3MasterDisabler : public MasterDisabler {
	public:
		Ch3MasterDisabler(bool &m, gb_ulong &wC) : MasterDisabler(m), waveCounter_(wC) {}

		virtual void operator()() {
			MasterDisabler::operator()();
			waveCounter_ = SoundUnit::counter_disabled;
		}

	private:
		gb_ulong &waveCounter_;
	};

	unsigned char waveRam_[0x10];
	Ch3MasterDisabler disableMaster_;
	LengthCounter lengthCounter_;
	gb_ulong soMask_;
	gb_ulong prevOut_;
	gb_ulong waveCounter_;
	gb_ulong lastReadTime_;
	unsigned char nr0_;
	unsigned char nr3_;
	unsigned char nr4_;
	unsigned char wavePos_;
	unsigned char rshift_;
	unsigned char sampleBuf_;
	unsigned char vol_;
	bool master_;
	bool cgb_;
	bool agb_;

	void updateWaveCounter(gb_ulong cc);

	inline unsigned char waveSample(unsigned pos, unsigned char s, unsigned char rsh) const {
		return (pos % 2 ? s & 0xF : s >> 4) >> rsh;
	}
};

}

#endif
