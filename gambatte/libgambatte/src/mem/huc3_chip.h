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

#ifndef HuC3Chip_H
#define HuC3Chip_H

#include "gbint.h"
#include "time.h"
#include "infrared.h"

namespace gambatte {

struct SaveState;

class HuC3Chip : public Clock {
public:
	HuC3Chip(Time &time, Infrared &ir);

	void setStatePtrs(SaveState &state);
	void saveState(SaveState &state) const;
	void loadState(SaveState const &state, bool const ds);

	void setRamflag(unsigned char ramflag) {
		ramflag_ = ramflag;
		committing_ = ramflag == 0xD;
		ir_.setRemoteActive(ramflag_ == 0xE);
	}

	bool isHuC3() const { return enabled_; }
	void set(bool enabled) { enabled_ = enabled; }

	void getHuC3Regs(unsigned char *dest, gb_ulong cycleCounter);
	void setHuC3Regs(unsigned char *src);

	void resetCc(gb_ulong oldCc, gb_ulong newCc);
	void speedChange(gb_ulong cycleCounter);

	void accumulateSamples(gb_ulong cycleCounter);
	unsigned generateSamples(short *soundBuf);

	unsigned char read(unsigned p, gb_ulong const cc);
	void write(unsigned p, unsigned data, gb_ulong cycleCounter);
	template<bool isReader>void SyncState(NewState *ns);

	virtual void updateClock(gb_ulong const cc);
	virtual void setTime(unsigned long long const dividers);
	virtual void setBaseTime(unsigned long long baseTime, gb_ulong const cc);

private:
	enum { max_samples = 35112 + 2064 };

	Time &time_;
	Infrared &ir_;
	unsigned char io_[0x100];
	unsigned char ioIndex_;
	unsigned char transferValue_;
	unsigned char ramflag_;
	gb_ulong rtcCycles_;
	short toneBuf_[max_samples * 2];
	short currentSample_;
	gb_ulong toneBufPos_;
	gb_ulong toneLastUpdate_;
	gb_ulong nextPhaseChangeTime_;
	gb_long remainingToneSamples_;
	bool enabled_;
	bool committing_;
	bool highIoReadOnly_;
	bool ds_;
};

}

#endif
