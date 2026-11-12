/* Copyright (c) 2004-2025 ares team, Near et al
 * Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Ported from ares (ISC); see LICENSES/ISC-ares.txt. */
#include "../snes.h"

// The event cartridges' NEC uPD78214, whose firmware was never dumped: as ares has it, a timer a
// second at a time and the switch between the program ROM and the three levels' ROMs.

struct Competition competition;

#define C competition

static int board;
// The program ROM, then the three levels'.
static const u8* rom[4];
static u32 romSize[4];

void competitionLoad(int which, const u8* data, const u32 sizes[4]) {
	board = which;
	u32 offset = 0;
	for (int n = 0; n < 4; n++) {
		rom[n] = data + offset;
		romSize[n] = sizes[n];
		offset += sizes[n];
	}
}

void competitionPower(void) {
	C.th.clock = 0;
	threadFrequency(&C.th, 1);
	// DIP switches 0-3 give the time: 3 minutes and 0-15 more. ares sets them to 3, the 6 minutes the
	// competitions used.
	C.timer = (3 + 3) * 60;
	C.status = 0x00;
	C.select = 0x00;
	C.timerActive = false;
	C.scoreActive = false;
	C.timerSecondsRemaining = 0;
	C.scoreSecondsRemaining = 0;
}

void competitionRun(void) {
	do {
		if (C.scoreActive && C.scoreSecondsRemaining) {
			if (--C.scoreSecondsRemaining == 0) {
				C.scoreActive = false;
			}
		}
		if (C.timerActive && C.timerSecondsRemaining) {
			if (--C.timerSecondsRemaining == 0) {
				C.timerActive = false;
				// Time over.
				C.status |= 0x02;
				C.scoreActive = true;
				C.scoreSecondsRemaining = 5;
			}
		}
		threadStep(&C.th, 1);
	} while (!(cpu.th.clock < C.th.clock));
}

static u8 romRead(u32 id, u32 address, u8 data) {
	if (!romSize[id]) {
		return data;
	}
	return rom[id][busMirror(address, romSize[id])];
}

u8 competitionReadMCU(u32 address, u8 data) {
	if (board == COMPETITION_CC92) {
		u32 id = 0;
		if (C.select == 0x09) {
			id = 1;
		}
		if (C.select == 0x05) {
			id = 2;
		}
		if (C.select == 0x03) {
			id = 3;
		}
		if ((address & 0x808000) == 0x808000) {
			id = 0;
		}
		if (address & 0x008000) {
			return romRead(id, (address & 0x7f0000) >> 1 | (address & 0x7fff), data);
		}
	}
	if (board == COMPETITION_PF94) {
		u32 id = 0;
		if (C.select == 0x09) {
			id = 1;
		}
		if (C.select == 0x0c) {
			id = 2;
		}
		if (C.select == 0x0a) {
			id = 3;
		}
		if ((address & 0x208000) == 0x208000) {
			id = 0;
		}
		if (address & 0x400000) {
			return romRead(id, address & 0x3fffff, data);
		}
		if (address & 0x008000) {
			address &= 0x1fffff;
			if (id != 2) {
				address = (address & 0x1f0000) >> 1 | (address & 0x7fff);
			}
			return romRead(id, address, data);
		}
	}
	return data;
}

void competitionWriteMCU(u32 address, u8 data) {
	(void) address;
	(void) data;
}

u8 competitionRead(u32 address, u8 data) {
	if (address == 0x106000 || address == 0xc00000) {
		return C.status;
	}
	return data;
}

void competitionWrite(u32 address, u8 data) {
	if (address == 0x206000 || address == 0xe00000) {
		C.select = data;
		if (C.timer && data == 0x09) {
			C.timerActive = true;
			C.timerSecondsRemaining = C.timer;
		}
	}
}
