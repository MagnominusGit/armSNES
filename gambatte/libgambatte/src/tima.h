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

#ifndef TIMA_H
#define TIMA_H

#include "gbint.h"
#include "interruptrequester.h"

namespace gambatte {

class TimaInterruptRequester {
public:
	explicit TimaInterruptRequester(InterruptRequester &intreq) : intreq_(intreq) {}
	void flagIrq() const { intreq_.flagIrq(4); }
	void flagIrq(gb_ulong cc) const { intreq_.flagIrq(4, cc); }
	gb_ulong nextIrqEventTime() const { return intreq_.eventTime(intevent_tima); }
	void setNextIrqEventTime(gb_ulong time) const { intreq_.setEventTime<intevent_tima>(time); }

private:
	InterruptRequester &intreq_;
};

class Tima {
public:
	Tima();
	void saveState(SaveState &) const;
	void loadState(const SaveState &, TimaInterruptRequester timaIrq);
	void resetCc(gb_ulong oldCc, gb_ulong newCc, TimaInterruptRequester timaIrq);
	void setTima(unsigned tima, gb_ulong cc, TimaInterruptRequester timaIrq);
	void setTma(unsigned tma, gb_ulong cc, TimaInterruptRequester timaIrq);
	void setTac(unsigned tac, gb_ulong cc, TimaInterruptRequester timaIrq, bool agbFlag);
	void divReset(gb_ulong cc, TimaInterruptRequester);
	void speedChange(TimaInterruptRequester);
	gb_ulong divLastUpdate() const { return divLastUpdate_; }
	unsigned tima(gb_ulong cc);
	void doIrqEvent(TimaInterruptRequester timaIrq);
	template<bool isReader>void SyncState(NewState *ns);

private:
	gb_ulong divLastUpdate_;
	gb_ulong lastUpdate_;
	gb_ulong tmatime_;
	unsigned char tima_;
	unsigned char tma_;
	unsigned char tac_;

	void updateIrq(gb_ulong const cc, TimaInterruptRequester timaIrq) {
		while (cc >= timaIrq.nextIrqEventTime())
			doIrqEvent(timaIrq);
	}

	void updateTima(gb_ulong cc);
};

}

#endif
