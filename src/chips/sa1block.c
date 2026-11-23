/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "../snes.h"

// Fast mode's SA-1 blocks (sa1.c): the plain 65816 again, for an instruction whose bytes a block has
// kept. sa1.c steps the clock for its fetches first; the bytes then come from the block, leaving the
// bus as a fetch from the ROM would. Its other accesses are sa1fast.c's.

#define R sa1.r
#define interruptPending() sa1.status.interruptPending
#define idleJump() sa1FastIdleJump()
#define idleBranch() \
	do { \
		if (R.pc.d & 1) { \
			sa1FastIdleJump(); \
		} \
	} while (0)

static const u8* bytes;

static inline u8 read(u32 address) {
	return sa1FastRead(address);
}

#define fetchRead(address) (R.ad = R.mar = (address), R.dr = R.mdr = *bytes++)

static inline void write(u32 address, u8 data) {
	sa1FastWrite(address, data);
}

static inline void idle(void) {
	sa1FastIdle();
}

static inline void lastCycle(void) {
	sa1FastLastCycle();
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../w65816.inc"
#pragma GCC diagnostic pop

void sa1BlockInstruction(const u8* from) {
	bytes = from;
	instruction();
}
