//
//   Copyright (C) 2007-2010 by sinamas <sinamas at users.sourceforge.net>
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

#include "gbint.h"
#include "remote.h"

namespace gambatte {

Remote::Remote()
: isActive_(false)
, lastUpdate_(0)
, cyclesElapsed_(0)
, command_(0)
, ds_(false)
, remoteCallback_(0)
{
}

void Remote::update(gb_ulong const cc) {
	// armGBA: the count stops at its top, long after the signal has ended, and doesn't wrap round
	// to send the signal again.
	gb_ulong const passed = (cc - lastUpdate_) >> ds_;
	cyclesElapsed_ = cyclesElapsed_ + passed < cyclesElapsed_ ? 0xFFFFFFFFu : cyclesElapsed_ + passed;
	lastUpdate_ = cc;
}

bool Remote::getRemoteSignal(gb_ulong const cc) {
	if (lastUpdate_ == disabled_time) {
		cyclesElapsed_ = 0;
		lastUpdate_ = cc;
	}

	update(cc);
	unsigned char modulation = (cyclesElapsed_ / 105) & 1; // 4194304 Hz CPU, 40000 Hz remote signal
	// armGBA: in 64 bits; in 32 the product overflowed after 28 seconds and the signal came again.
	unsigned long long timeUs = cyclesElapsed_ * 36ull / 151; // actually *1000000/4194304
	// sony protocol
	if (timeUs < 10000) // initialization allowance
		return 0;
	else if (timeUs < 10000 + 2400) // initial mark
		return modulation;
	else if (timeUs < 10000 + 2400 + 600) // initial space
		return 0;
	else { // send data
		timeUs -= 13000;
		unsigned data = (command_ << 13) | 0x1FFF; // write 20 bits (upper 7 bits are the command)
		for (gb_ulong mask = 1u << (20 - 1); mask; mask >>= 1) {
			unsigned markTime = (data & mask) ? 1200 : 600;
			if (timeUs < markTime)
				return modulation;

			timeUs -= markTime;
			if (timeUs < 600)
				return 0;

			timeUs -= 600;
		}

		return 0;
	}
}

void Remote::resetCc(gb_ulong const oldCc, gb_ulong const newCc) {
	if (lastUpdate_ != disabled_time) {
		update(oldCc);
		lastUpdate_ -= oldCc - newCc;		
	}
}

void Remote::speedChange(gb_ulong const cc) {
	if (lastUpdate_ != disabled_time) {
		update(cc);
	}

	ds_ = !ds_;
}

void Remote::saveState(SaveState::Infrared &ss) const {
	ss.remote.isActive = isActive_;
	ss.remote.lastUpdate = lastUpdate_;
	ss.remote.cyclesElapsed = cyclesElapsed_;
	ss.remote.command = command_;
}

void Remote::loadState(SaveState::Infrared const &ss, bool const ds) {
	isActive_ = ss.remote.isActive;
	lastUpdate_ = ss.remote.lastUpdate;
	cyclesElapsed_ = ss.remote.cyclesElapsed;
	command_ = ss.remote.command;
	ds_ = ds;
}

SYNCFUNC(Remote) {
	NSS(isActive_);
	NSS(lastUpdate_);
	NSS(cyclesElapsed_);
	NSS(command_);
	NSS(ds_);
}

}
