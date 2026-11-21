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

#ifndef LY_COUNTER_H
#define LY_COUNTER_H

#include "gbint.h"
#include "lcddef.h"
#include "newstate.h"

namespace gambatte {

struct SaveState;

class LyCounter {
public:
	LyCounter();
	void doEvent();
	bool isDoubleSpeed() const { return ds_; }

	gb_ulong frameCycles(gb_ulong cc) const {
		return 1 * ly_ * lcd_cycles_per_line + lineCycles(cc);
	}

	unsigned lineCycles(gb_ulong cc) const {
		return lcd_cycles_per_line - ((time_ - cc) >> isDoubleSpeed());
	}

	unsigned lineTime() const { return lineTime_; }
	unsigned ly() const { return ly_; }
	gb_ulong nextLineCycle(unsigned lineCycle, gb_ulong cycleCounter) const;
	gb_ulong nextFrameCycle(gb_ulong frameCycle, gb_ulong cycleCounter) const;
	void reset(gb_ulong videoCycles, gb_ulong lastUpdate);
	void setDoubleSpeed(bool ds);
	gb_ulong time() const { return time_; }
	template<bool isReader>void SyncState(NewState *ns);

private:
	gb_ulong time_;
	unsigned short lineTime_;
	unsigned char ly_;
	bool ds_;
};

}

#endif
