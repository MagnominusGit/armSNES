/* Copyright (c) 2004-2025 ares team, Near et al
 * Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Ported from ares (ISC); see LICENSES/ISC-ares.txt. */
#include "../snes.h"
#include "gb.h"

#include <string.h>

// The Super Game Boy's ICD2: it runs the Game Boy, takes its picture a line at a time into four
// banks of eight lines the SNES reads as tiles, passes the SNES's joypads to it and takes the command
// packets it sends through JOYP.

struct Icd icd;

#define I icd

// The Game Boy's clock: the CPU's oscillator, or the SGB2's own, over the control port's divider.
static u32 oscillator(void) {
	return I.frequency ? I.frequency : (u32) sys.cpuFrequency;
}

static void setDivider(u8 select) {
	static const u8 dividers[4] = { 4, 5, 7, 9 };
	I.clockFrequency = oscillator() / dividers[select & 3];
	threadFrequency(&I.th, I.clockFrequency);
}

void icdPower(bool reset) {
	(void) reset;
	I.th.clock = 0;
	setDivider(1);
	I.joypId = 3;
	I.pulseLock = true;
	I.strobeLock = false;
	I.packetLock = false;
	I.packetOffset = 0;
	I.packetReady = false;
	I.bitData = 0;
	I.bitOffset = 0;
	I.previousP15 = false;
	memset(I.output, 0xff, sizeof(I.output));
	I.readBank = 0;
	I.readAddress = 0;
	I.writeBank = 0;
	I.r6003 = 0x00;
	memset(I.joypads, 0xff, sizeof(I.joypads));
	memset(I.r7000, 0, sizeof(I.r7000));
	I.mltReq = 0;
	I.hcounter = 0;
	I.vcounter = 0;
	if (gbLoaded()) {
		gbReset();
	}
}

// --- The Game Boy's side ---

static void ppuWrite(u8 color) {
	u8 x = I.hcounter++;
	u8 y = I.vcounter & 7;
	if (x >= 160) {
		return;
	}
	u16 address = (I.writeBank & 3) * 512 + y * 2 + x / 8 * 16;
	I.output[address + 0] = I.output[address + 0] << 1 | (color & 1);
	I.output[address + 1] = I.output[address + 1] << 1 | (color >> 1 & 1);
}

// Each line comes as its pixels are done; line 0 starts a frame.
void icdLine(unsigned ly, const uint32_t* pixels) {
	if (ly >= 144) {
		return;
	}
	if (ly == 0) {
		I.hcounter = 0;
		I.vcounter = 0;
	}
	for (int x = 0; x < 160; x++) {
		ppuWrite(3 - (pixels[x] >> 4 & 3));
	}
	I.hcounter = 0;
	I.vcounter++;
	if ((I.vcounter & 7) == 0) {
		I.writeBank = (I.writeBank + 1) & 3;
	}
}

unsigned icdJoypWrite(unsigned p14, unsigned p15) {
	if (p15 && !I.previousP15) {
		I.joypId++;
		if (I.mltReq == 0) {
			I.joypId &= 0;
		}
		if (I.mltReq == 1) {
			I.joypId &= 1;
		}
		if (I.mltReq >= 2) {
			I.joypId &= 3;
		}
	}
	I.previousP15 = p15;

	u8 joypad = I.joypads[I.joypId & 3];
	unsigned input = 0xf;
	if (p14 && p15) {
		input -= I.joypId & 3;
	}
	if (!p14) {
		input &= joypad & 15;
	}
	if (!p15) {
		input &= joypad >> 4;
	}

	// The packets.
	if (!p14 && !p15) {
		I.pulseLock = false;
		I.packetOffset = 0;
		I.bitOffset = 0;
		I.strobeLock = true;
		I.packetLock = false;
		return input;
	}
	if (I.pulseLock) {
		return input;
	}
	if (p14 && p15) {
		I.strobeLock = false;
		return input;
	}
	if (I.strobeLock) {
		if (p14 || p15) {
			// A malformed packet.
			I.packetLock = false;
			I.pulseLock = true;
			I.bitOffset = 0;
			I.packetOffset = 0;
		} else {
			return input;
		}
	}

	// P14 low is a 0, P15 low a 1.
	bool bit = !p15;
	I.strobeLock = true;
	if (I.packetLock) {
		if (!p14 && p15) {
			I.packetReady = true;
			I.packetLock = false;
			I.pulseLock = true;
		}
		return input;
	}
	I.bitData = bit << 7 | I.bitData >> 1;
	I.bitOffset = (I.bitOffset + 1) & 7;
	if (I.bitOffset) {
		return input;
	}
	I.r7000[I.packetOffset] = I.bitData;
	I.packetOffset = (I.packetOffset + 1) & 15;
	if (I.packetOffset) {
		return input;
	}
	I.packetLock = true;
	return input;
}

// Its sound comes a sample every two clocks.
static void soundSample(s16 left, s16 right) {
	mixerInput(&I.sound, left, right, 32040 * 2, I.clockFrequency);
}

// --- Running ---

void icdRun(void) {
	do {
		if (I.r6003 & 0x80) {
			unsigned samples;
			const unsigned* audio;
			u32 dots = gbRun(ICD_SLICE, &samples, &audio);
			for (unsigned n = 0; n < samples; n++) {
				soundSample((s16) (audio[n] & 0xffff), (s16) (audio[n] >> 16));
			}
			threadStep(&I.th, dots);
		} else {
			// Halted: silence, two clocks to a sample.
			for (int n = 0; n < ICD_SLICE / 2; n++) {
				soundSample(0, 0);
			}
			threadStep(&I.th, ICD_SLICE);
		}
	} while (!(cpu.th.clock < I.th.clock));
}

// --- The SNES's side ---

u8 icdRead(u32 address, u8 data) {
	(void) data;
	address &= 0x40f80f;
	// The LY counter.
	if (address == 0x6000) {
		return (I.vcounter & ~7) | (I.writeBank & 3);
	}
	// The command ready port.
	if (address == 0x6002) {
		return I.packetReady;
	}
	// The ICD2's revision.
	if (address == 0x600f) {
		return 0x21;
	}
	// The command port.
	if (address >= 0x7000 && address <= 0x700f) {
		if ((address & 15) == 0) {
			I.packetReady = false;
		}
		return I.r7000[address & 15];
	}
	// The VRAM port.
	if (address >= 0x7800 && address <= 0x780f) {
		u8 value = I.output[(I.readBank & 3) * 512 + I.readAddress];
		I.readAddress = (I.readAddress + 1) & 511;
		return value;
	}
	return 0x00;
}

void icdWrite(u32 address, u8 data) {
	address &= 0xffff;
	if (address == 0x6001) {
		I.readBank = data & 3;
		I.readAddress = 0;
		return;
	}
	// The control port: d7 runs (1) or halts the Game Boy, d5-d4 the players, d1-d0 the divider.
	if (address == 0x6003) {
		if (!(I.r6003 & 0x80) && (data & 0x80)) {
			icdPower(true);
		}
		I.mltReq = data >> 4 & 3;
		if (I.mltReq == 1) {
			I.joypId &= ~1;
		}
		if (I.mltReq >= 2) {
			I.joypId &= ~3;
		}
		setDivider(data);
		I.r6003 = data;
		return;
	}
	if (address >= 0x6004 && address <= 0x6007) {
		I.joypads[address - 0x6004] = data;
	}
}
