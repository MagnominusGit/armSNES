/* Copyright (c) 2004-2025 ares team, Near et al
 * Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Ported from ares (ISC); see LICENSES/ISC-ares.txt. */
#include "snes.h"

struct System sys;

// ares's generator (PCG), on its low-entropy setting.

static u32 randomStep(void) {
	u64 state = sys.randomState;
	sys.randomState = state * 6364136223846793005ull + sys.randomIncrement;
	u32 xorshift = (state >> 18 ^ state) >> 27;
	u32 rotate = state >> 59;
	return xorshift >> rotate | xorshift << (-rotate & 31);
}

static void randomSeed(u64 seed) {
	sys.randomState = 0;
	sys.randomIncrement = 1;
	randomStep();
	sys.randomState += seed;
	randomStep();
}

u64 randomNext(void) {
	u64 hi = randomStep();
	return hi << 32 | randomStep();
}

u64 randomBias(u64 bias) {
	(void) bias;
	return randomNext();
}

// What RAM holds at power on: stripes of two values, with a stray bit here and there.
void randomArray(u8* buffer, u32 size) {
	u32 lobit = randomNext() & 3;
	u32 hibit = (lobit + 8 + (randomNext() & 3)) & 15;
	u32 lovalue = randomNext() & 255;
	u32 hivalue = randomNext() & 255;
	if ((randomNext() & 3) == 0) {
		lovalue = 0;
	}
	if ((randomNext() & 1) == 0) {
		hivalue = ~lovalue;
	}

	for (u32 address = 0; address < size; ++address) {
		u8 value = address >> lobit & 1 ? lovalue : hivalue;
		if (address >> hibit & 1) {
			value = ~value;
		}
		if ((randomNext() & 511) == 0) {
			value ^= 1 << (randomNext() & 7);
		}
		if ((randomNext() & 2047) == 0) {
			value ^= 1 << (randomNext() & 7);
		}
		buffer[address] = value;
	}
}

void systemLoad(bool pal) {
	memset(&sys, 0, sizeof(sys));
	sys.pal = pal;
	// The color subcarrier times 6 (NTSC) or 4.8 (PAL).
	sys.cpuFrequency = pal ? (283.75 * 15625.0 + 25.0) * 4.8 : 315.0 / 88.0 * 1000000.0 * 6.0;
	sys.apuFrequency = 32040.0 * 768.0;

	busReset();
	cpuMap();
	ppuMap();
	cartMap();
	busFinish();
}

void systemPower(bool reset) {
	randomSeed(0);

	cpuPower(reset);
	smpPower(reset);
	dspPower(reset);
	ppuPower(reset);
	cartPower(reset);
	inputPower();

	// Equal clocks would leave it open who runs first, so each starts at its own number.
	cpu.th.clock = 0;
	smp.th.clock = 1;
	dsp.th.clock = 2;
	ppu.th.clock = 3;
	// A cartridge has one coprocessor at most, and it comes next.
	necdsp.th.clock = 4;
	sa1.th.clock = 4;
	gsu.th.clock = 4;
}

// The PPU finished a frame. The clocks all drop by the same amount, so they never overflow.
void systemFrameEvent(void) {
	u64 minimum = cpu.th.clock - 0;
	if (smp.th.clock - 1 < minimum) {
		minimum = smp.th.clock - 1;
	}
	if (dsp.th.clock - 2 < minimum) {
		minimum = dsp.th.clock - 2;
	}
	if (ppu.th.clock - 3 < minimum) {
		minimum = ppu.th.clock - 3;
	}
	if (cart.necdsp && necdsp.th.clock - 4 < minimum) {
		minimum = necdsp.th.clock - 4;
	}
	if (cart.sa1 && sa1.th.clock - 4 < minimum) {
		minimum = sa1.th.clock - 4;
	}
	if (cart.gsu && gsu.th.clock - 4 < minimum) {
		minimum = gsu.th.clock - 4;
	}
	cpu.th.clock -= minimum;
	smp.th.clock -= minimum;
	dsp.th.clock -= minimum;
	ppu.th.clock -= minimum;
	if (cart.necdsp) {
		necdsp.th.clock -= minimum;
	}
	if (cart.sa1) {
		sa1.th.clock -= minimum;
	}
	if (cart.gsu) {
		gsu.th.clock -= minimum;
	}

	sys.frames++;
	sys.frameEvent = true;
}

void systemRunFrame(void) {
	sys.frameEvent = false;
	while (!sys.frameEvent) {
		cpuMain();
	}
}
