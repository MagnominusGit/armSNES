/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Licensed under the GNU General Public License, version 2 or (at your option) any later version: see LICENSE. */
#include "gb.h"

// The serial port (Pan Docs, "Serial Data Transfer"). The internal clock runs off a bit of the
// divider's counter, bit 7, or bit 2 with the Color's fast clock: each falling edge of it turns the
// serial clock over, and a bit shifts as that goes low, so eight bits take sixteen edges. Writing SC
// sets the serial clock low (SameBoy). With no cable, 1s shift in; a transfer on the external clock
// never ends.

static unsigned clockBit(const struct GB* gb) {
	return gb->cgbMode && (gb->serial.sc & 2) ? 2 : 7;
}

// The transfer's end, at the counter's rate from now (or from where a hold ends).
static void schedule(struct GB* gb) {
	uint32_t per = gb->doubleSpeed ? 2 : 1;
	uint32_t from = gbSince(gb->timer.lastSync, gb->now) > 0 ? gb->timer.lastSync : gb->now;
	uint16_t left = gb->serial.end - gb->timer.counter;
	gbSchedule(gb, GB_EVENT_SERIAL, from + (left + per - 1) / per);
}

void gbSerialWrite(struct GB* gb, uint8_t reg, uint8_t value) {
	struct GBSerial* serial = &gb->serial;
	if (reg == 0x01) {
		serial->sb = value;
		return;
	}
	serial->sc = value & (gb->cgbMode ? 0x83 : 0x81);
	if ((value & 0x81) != 0x81) {
		gbUnschedule(gb, GB_EVENT_SERIAL);
		return;
	}
	if (gb->serialOut) {
		gb->serialOut(gb->user, serial->sb);
	}
	// The sixteenth falling edge of the clock bit from now.
	gbTimerSync(gb);
	unsigned shift = clockBit(gb) + 1;
	serial->end = ((gb->timer.counter >> shift) + 16) << shift;
	serial->bits = 8;
	schedule(gb);
}

// A speed switch changes the counter's rate, and a hold where it starts.
void gbSerialReplan(struct GB* gb) {
	if (gb->eventsOn & 1u << GB_EVENT_SERIAL) {
		schedule(gb);
	}
}

// DIV reset from old: the edges left count from 0, less one if the clock bit falls with it.
void gbSerialDivReset(struct GB* gb, uint16_t old) {
	struct GBSerial* serial = &gb->serial;
	if (!(gb->eventsOn & 1u << GB_EVENT_SERIAL)) {
		return;
	}
	unsigned bit = clockBit(gb), period = 2u << bit;
	unsigned edges = ((uint16_t) (serial->end - old) + period - 1) / period;
	if (old >> bit & 1) {
		--edges;
	}
	serial->end = edges * period;
	schedule(gb);
}

void gbSerialEvent(struct GB* gb) {
	struct GBSerial* serial = &gb->serial;
	serial->bits = 0;
	serial->sc &= 0x7F;
	gbRequest(gb, GB_IRQ_SERIAL);
	if (!serial->linked) {
		serial->sb = 0xFF;
		return;
	}
	serial->clocked = true;
	if (gb->linkClocked) {
		gb->linkClocked(gb->user);
	}
}

int gbLinkStatus(struct GB* gb, int request) {
	struct GBSerial* serial = &gb->serial;
	switch (request) {
	case GB_LINK_CLOCKED:
		return serial->clocked;
	case GB_LINK_ACK:
		serial->clocked = false;
		return 0;
	case GB_LINK_OUT:
		return serial->sb;
	case GB_LINK_ON:
	case GB_LINK_OFF:
		serial->linked = request == GB_LINK_ON;
		return 0;
	case GB_LINK_STOP:
		if (serial->sc & 0x80) {
			serial->sc &= 0x7F;
			gbUnschedule(gb, GB_EVENT_SERIAL);
		}
		return 0;
	case GB_LINK_CONTROL:
		return serial->sc;
	case GB_LINK_LEFT: {
		if (!(gb->eventsOn >> GB_EVENT_SERIAL & 1)) {
			return -1;
		}
		// A transfer whose end is due but not yet run (a run stops at an instruction's end) has none left.
		int32_t left = gbSince(gb->events[GB_EVENT_SERIAL], gb->now);
		return left > 0 ? left : 0;
	}
	default:
		// The other end's byte: the transfer under way ends with it.
		serial->sb = request;
		if (serial->sc & 0x80) {
			serial->sc &= 0x7F;
			gbUnschedule(gb, GB_EVENT_SERIAL);
			gbRequest(gb, GB_IRQ_SERIAL);
		}
		return 0;
	}
}
