/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Licensed under the GNU General Public License, version 2 or (at your option) any later version: see LICENSE. */
#include "gb.h"

// DIV, TIMA, TMA and TAC (Pan Docs, "Timer and Divider Registers" and "Timer obscure behaviour").
// A 16-bit counter runs at the CPU's clock and DIV is its top byte. TIMA counts the falling edges of
// one counter bit, chosen by TAC, while TAC enables it; so writing DIV or TAC can make an edge. After
// TIMA overflows it reads 0 for an M-cycle, then takes TMA and requests the interrupt.

static const uint8_t tacBit[4] = { 9, 3, 5, 7 };

// CPU T-cycles per dot.
static uint32_t perDot(const struct GB* gb) {
	return gb->doubleSpeed ? 2 : 1;
}

static bool enabled(const struct GBTimer* t) {
	return t->tac & 4;
}

// When the counter, from c at time from, reaches value (a later one).
static uint32_t timeOf(const struct GB* gb, uint32_t from, uint32_t c, uint32_t value) {
	uint32_t per = perDot(gb);
	return from + (value - c + per - 1) / per;
}

// TIMA overflowed at time at.
static void overflow(struct GB* gb, uint32_t at) {
	struct GBTimer* t = &gb->timer;
	t->tima = 0;
	t->reloading = true;
	t->reloadAt = at + 4 / perDot(gb);
}

static void reload(struct GB* gb) {
	struct GBTimer* t = &gb->timer;
	t->reloading = false;
	t->tima = t->tma;
	t->reloadedAt = t->reloadAt;
	gbRequest(gb, GB_IRQ_TIMER);
}

static void increment(struct GB* gb, uint32_t at) {
	struct GBTimer* t = &gb->timer;
	if (++t->tima == 0) {
		overflow(gb, at);
	}
}

// Schedules the next time the timer does something by itself: a reload or an overflow.
static void plan(struct GB* gb) {
	struct GBTimer* t = &gb->timer;
	if (t->reloading) {
		gbSchedule(gb, GB_EVENT_TIMER, t->reloadAt);
		return;
	}
	if (!enabled(t)) {
		gbUnschedule(gb, GB_EVENT_TIMER);
		return;
	}
	unsigned shift = tacBit[t->tac & 3] + 1;
	uint32_t edges = 0x100 - t->tima;
	uint32_t value = ((uint32_t) (t->counter >> shift) + edges) << shift;
	gbSchedule(gb, GB_EVENT_TIMER, timeOf(gb, t->lastSync, t->counter, value));
}

void gbTimerSync(struct GB* gb) {
	struct GBTimer* t = &gb->timer;
	int32_t dots = gbSince(gb->now, t->lastSync);
	if (dots <= 0) {
		if (t->reloading && gbSince(gb->now, t->reloadAt) >= 0) {
			reload(gb);
		}
		return;
	}
	uint32_t c = t->counter;
	uint32_t from = t->lastSync;
	uint32_t end = c + (uint32_t) dots * perDot(gb);
	for (;;) {
		if (t->reloading) {
			if (gbSince(gb->now, t->reloadAt) < 0) {
				break;
			}
			reload(gb);
		}
		if (!enabled(t)) {
			break;
		}
		unsigned shift = tacBit[t->tac & 3] + 1;
		uint32_t edges = (end >> shift) - (c >> shift);
		if (t->tima + edges <= 0xFF) {
			t->tima += edges;
			break;
		}
		// Up to the edge that overflows, then on from there.
		uint32_t value = ((c >> shift) + (0x100 - t->tima)) << shift;
		uint32_t at = timeOf(gb, from, c, value);
		from = at;
		c = value;
		overflow(gb, at);
	}
	t->counter = end;
	t->lastSync = gb->now;
}

// DIV, just reset, stands still from now until dots on.
void gbTimerHold(struct GB* gb, uint32_t dots) {
	struct GBTimer* t = &gb->timer;
	t->counter = 0;
	t->lastSync = gb->now + dots;
	t->held = true;
	t->holdEnd = t->lastSync;
	plan(gb);
	gbSerialReplan(gb);
}

void gbTimerEvent(struct GB* gb) {
	gbTimerSync(gb);
	plan(gb);
}

uint8_t gbTimerRead(struct GB* gb, uint8_t reg) {
	struct GBTimer* t = &gb->timer;
	gbTimerSync(gb);
	switch (reg) {
	case 0x04:
		return t->counter >> 8;
	case 0x05:
		return t->tima;
	case 0x06:
		return t->tma;
	default:
		return t->tac | 0xF8;
	}
}

// Whether the bit TIMA counts is high, as TAC would gate it.
static bool timerBit(const struct GBTimer* t, uint8_t tac) {
	return (tac & 4) && (t->counter >> tacBit[tac & 3] & 1);
}

static void divReset(struct GB* gb) {
	struct GBTimer* t = &gb->timer;
	gbApuDivReset(gb);
	uint16_t old = t->counter;
	t->counter = 0;
	gbSerialDivReset(gb, old);
}

// A speed switch's DIV reset, as TIMA sees it (AGE's spsw-tima): going back to single speed an M-cycle
// later than the reset, and at 4 KHz (and on the Color E at 65 and 16 KHz) an M-cycle earlier. Later,
// the edges up to then count; earlier, the bit counts only if it was high then and still is (had it
// fallen since, that edge has counted).
void gbTimerSwitchReset(struct GB* gb, bool toSingle) {
	struct GBTimer* t = &gb->timer;
	gbTimerSync(gb);
	if (t->tac & 4) {
		unsigned rate = t->tac & 3, bit = tacBit[rate];
		int shift = (toSingle ? 4 : 0) - (rate == 0 || (gbColor(gb) && rate >= 2) ? 4 : 0);
		uint32_t c = t->counter;
		bool high = c >> bit & 1;
		if (shift > 0) {
			for (uint32_t edges = ((c + shift) >> (bit + 1)) - (c >> (bit + 1)); edges; --edges) {
				increment(gb, gb->now);
			}
			high = (c + shift) >> bit & 1;
		} else if (shift < 0) {
			high = high && ((c + shift) >> bit & 1);
		}
		if (high) {
			increment(gb, gb->now);
		}
	}
	divReset(gb);
}

void gbTimerWrite(struct GB* gb, uint8_t reg, uint8_t value) {
	struct GBTimer* t = &gb->timer;
	gbTimerSync(gb);
	switch (reg) {
	case 0x04:
		if (timerBit(t, t->tac)) {
			increment(gb, gb->now);
		}
		divReset(gb);
		break;
	case 0x05:
		// Written in the M-cycle TIMA reads 0, the reload is called off; in the one it reloads, the
		// write is lost.
		if (t->reloading) {
			t->reloading = false;
		} else if (t->reloadedAt == gb->now) {
			break;
		}
		t->tima = value;
		break;
	case 0x06:
		t->tma = value;
		if (t->reloadedAt == gb->now) {
			t->tima = value;
		}
		break;
	default:
		if (timerBit(t, t->tac) && !timerBit(t, value)) {
			increment(gb, gb->now);
		}
		t->tac = value & 7;
		break;
	}
	plan(gb);
}
