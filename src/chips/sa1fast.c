/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "../snes.h"

// Fast mode's SA-1: the plain 65816 the S-CPU uses, run an instruction at a time between sa1Run's own
// steps. sa1Run sees to waiting, stopping, sleep, reset and interrupts; the accesses are sa1.c's.

#define R sa1.r
#define interruptPending() sa1.status.interruptPending
#define idleJump() sa1FastIdleJump()
#define idleBranch() \
	do { \
		if (R.pc.d & 1) { \
			sa1FastIdleJump(); \
		} \
	} while (0)

static inline u8 read(u32 address) {
	return sa1FastRead(address);
}

#define fetchRead(address) sa1FastFetch(address)

static inline void write(u32 address, u8 data) {
	sa1FastWrite(address, data);
}

static inline void idle(void) {
	sa1FastIdle();
}

static inline void lastCycle(void) {
	sa1FastLastCycle();
}

// Its power-on and interrupt entry are sa1.c's.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../w65816.inc"
#pragma GCC diagnostic pop

void sa1FastInstruction(void) {
	instruction();
}
