/* Copyright (c) 2004-2025 ares team, Near et al
 * Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Ported from ares (ISC); see LICENSES/ISC-ares.txt. */
#include "snes.h"

// The S-CPU: a 65C816 with DMA, the multiplier and divider, the H/V timer interrupts and automatic
// joypad polling. It drives the machine: its clock steps two master clocks at a time, and the other
// processors are run up to it whenever it touches them and at the start of each scanline.

struct Cpu cpu;

#define R cpu.r
#define interruptPending() cpu.status.interruptPending

static u8 read(u32 address);
static void write(u32 address, u8 data);
static void idle(void);
static void lastCycle(void);
static inline void step(u32 clocks);
static inline void dmaEdge(void);

#include "w65816.inc"

// The other processors, brought up to the CPU's clock.

static void syncSmp(void) {
	if (smp.th.clock < cpu.th.clock) {
		smpRun();
	}
}

static void syncPpu(void) {
	if (ppu.th.clock < cpu.th.clock) {
		ppuRun();
	}
}

// Counter

void counterReset(struct Counter* counter) {
	counter->interlace = 0;
	counter->field = 0;
	counter->vperiod = sys.pal ? 312 : 262;
	counter->hperiod = 1364;
	counter->vcounter = 0;
	counter->hcounter = 0;
	counter->lastVperiod = 0;
	counter->lastHperiod = 0;
}

void counterTickScanline(struct Counter* counter) {
	if (++counter->vcounter == 128) {
		// When interlace is taken doesn't matter, as long as it is before line 240 or 311, and not at
		// 0,0 where the CPU and PPU may be out of step.
		counter->interlace = ppu.state.interlace;
		counter->vperiod += counter->interlace && !counter->field;
	}

	if (counter->vcounter == counter->vperiod) {
		counter->lastVperiod = counter->vperiod;
		counter->vperiod = sys.pal ? 312 : 262;
		counter->vcounter = 0;
		counter->field ^= 1;
	}

	counter->hperiod = 1364;
	// Every line being 1364 clocks wouldn't match the color clock: NTSC has one short line, PAL one
	// long one.
	if (!sys.pal && counter->interlace == 0 && counter->field == 1 && counter->vcounter == 240) {
		counter->hperiod -= 4;
	}
	if (sys.pal && counter->interlace == 1 && counter->field == 1 && counter->vcounter == 311) {
		counter->hperiod += 4;
	}
}

// Timing

static inline u32 dmaCounter(void) {
	return cpu.counterCpu & 7;
}

static inline u32 joypadCounter(void) {
	return cpu.counterCpu & 127;
}

static void scanline(void) {
	// Even if nothing made them talk, the processors are brought together once a line.
	syncSmp();
	syncPpu();
	cartSync();

	if (cpu.counter.vcounter == 0) {
		// HDMA is set up once a frame.
		cpu.status.hdmaSetupPosition = (cpu.io.version == 1 ? 12 + 8 - dmaCounter() : 12 + dmaCounter());
		cpu.status.hdmaSetupTriggered = 0;

		cpu.status.autoJoypadCounter = 33;
	}

	// DRAM is refreshed once a line.
	if (cpu.io.version == 2) {
		cpu.status.dramRefreshPosition = 530 + 8 - dmaCounter();
	}
	cpu.status.dramRefresh = 0;

	// HDMA runs once on every visible line.
	if (cpu.counter.vcounter < ppu.state.vdisp) {
		cpu.status.hdmaPosition = 1104;
		cpu.status.hdmaTriggered = 0;
	}
}

// NMI and IRQ are polled every four clocks. The counters are read as they were some clocks back, for
// the delay between the units on hardware.
static inline void nmiPoll(void) {
	bool held = cpu.status.nmiHold;
	cpu.status.nmiHold = 0;
	if (held && cpu.io.nmiEnable) {
		cpu.status.nmiTransition = 1;
	}

	if (flipTo(&cpu.status.nmiValid, counterVcounterAt(&cpu.counter, 2) >= ppu.state.vdisp)) {
		// /NMI is held for four cycles.
		if ((cpu.status.nmiLine = cpu.status.nmiValid)) {
			cpu.status.nmiHold = 1;
		}
	}
}

static inline void irqPoll(void) {
	cpu.status.irqHold = 0;
	if (cpu.status.irqLine && cpu.io.irqEnable) {
		cpu.status.irqTransition = 1;
	}

	// IRQs can't trigger on the last dot of a field.
	bool valid = cpu.io.irqEnable && (!cpu.io.virqEnable || counterVcounterAt(&cpu.counter, 10) == cpu.io.vtime) &&
		(!cpu.io.hirqEnable || counterHcounterAt(&cpu.counter, 10) == cpu.io.htime) &&
		(counterVcounterAt(&cpu.counter, 6) || counterHcounterAt(&cpu.counter, 6));
	if (raiseTo(&cpu.status.irqValid, valid)) {
		cpu.status.irqLine = cpu.status.irqHold = 1;
	}
}

static void nmitimenUpdate(u8 data) {
	cpu.io.hirqEnable = data & 0x10;
	cpu.io.virqEnable = data & 0x20;
	cpu.io.irqEnable = cpu.io.hirqEnable || cpu.io.virqEnable;

	if (cpu.io.virqEnable && !cpu.io.hirqEnable && cpu.status.irqLine) {
		cpu.status.irqTransition = 1;
	} else if (!cpu.io.irqEnable) {
		cpu.status.irqLine = 0;
		cpu.status.irqTransition = 0;
	}

	if (raiseTo(&cpu.io.nmiEnable, data & 0x80) && cpu.status.nmiLine) {
		cpu.status.nmiTransition = 1;
	}

	cpu.status.irqLock = 1;
}

static bool rdnmi(void) {
	bool result = cpu.status.nmiLine;
	if (!cpu.status.nmiHold) {
		cpu.status.nmiLine = 0;
	}
	return result;
}

static bool timeup(void) {
	bool result = cpu.status.irqLine;
	if (!cpu.status.irqHold) {
		cpu.status.irqLine = 0;
		cpu.status.irqTransition = 0;
	}
	return result;
}

static inline bool nmiTest(void) {
	if (!cpu.status.nmiTransition) {
		return 0;
	}
	cpu.status.nmiTransition = 0;
	R.wai = 0;
	return 1;
}

static inline bool irqTest(void) {
	if (!cpu.status.irqTransition && !R.irq) {
		return 0;
	}
	cpu.status.irqTransition = 0;
	R.wai = 0;
	return !R.i;
}

// Interrupts are looked for a cycle before an instruction ends, as the 65816's pipeline does. After
// DMA or a write to $4200 they have to wait: that is irqLock.
static void lastCycle(void) {
	if (!cpu.status.irqLock) {
		if (nmiTest()) {
			cpu.status.nmiPending = 1;
			cpu.status.interruptPending = 1;
		}
		if (irqTest()) {
			cpu.status.irqPending = 1;
			cpu.status.interruptPending = 1;
		}
	}
}

static inline void aluEdge(void) {
	if (cpu.alu.mpyctr) {
		cpu.alu.mpyctr--;
		if (cpu.io.rddiv & 1) {
			cpu.io.rdmpy += cpu.alu.shift;
		}
		cpu.io.rddiv >>= 1;
		cpu.alu.shift <<= 1;
	}

	if (cpu.alu.divctr) {
		cpu.alu.divctr--;
		cpu.io.rddiv <<= 1;
		cpu.alu.shift >>= 1;
		if (cpu.io.rdmpy >= cpu.alu.shift) {
			cpu.io.rdmpy -= cpu.alu.shift;
			cpu.io.rddiv |= 1;
		}
	}
}

static void joypadCycle(void);

// Every 128 clocks.
static void joypadEdge(void) {
	if (cpu.counter.vcounter == ppu.state.vdisp && (cpu.counterCpu & 255) == 0 && cpu.counter.hcounter >= 130 &&
	    cpu.counter.hcounter <= 384) {
		// A new polling sequence.
		cpu.status.autoJoypadCounter = 0;
	} else {
		// Polling is over for this frame.
		if (cpu.status.autoJoypadCounter >= 33) {
			return;
		}
		cpu.status.autoJoypadCounter++;
	}
	joypadCycle();
}

// One 128-clock cycle of the automatic polling, autoJoypadCounter saying which.
static void joypadCycle(void) {
	if (cpu.status.autoJoypadCounter == 0) {
		// The controllers are latched on the first cycle.
		cpu.status.autoJoypadLatch = cpu.io.autoJoypadPoll;
		controllerLatch(0, cpu.status.autoJoypadLatch | cpu.status.cpuLatch);
		controllerLatch(1, cpu.status.autoJoypadLatch | cpu.status.cpuLatch);
		if (cpu.io.autoJoypadPoll) {
			// The shift registers are cleared as polling starts.
			cpu.io.joy1 = 0;
			cpu.io.joy2 = 0;
			cpu.io.joy3 = 0;
			cpu.io.joy4 = 0;
		}
	}

	if (cpu.status.autoJoypadCounter == 1) {
		// The latch is released on the second, and reading begins.
		cpu.status.autoJoypadLatch = 0;
		controllerLatch(0, cpu.status.autoJoypadLatch | cpu.status.cpuLatch);
		controllerLatch(1, cpu.status.autoJoypadLatch | cpu.status.cpuLatch);
	}

	if (cpu.status.autoJoypadCounter != 1 && !cpu.io.autoJoypadPoll) {
		// With polling switched off by now, the rest is skipped.
		cpu.status.autoJoypadCounter = 33;
		return;
	}

	if (cpu.status.autoJoypadCounter >= 2) {
		// Sixteen bits go into joy1-4, one every 256 clocks: read on one 128-clock cycle, written on
		// the next.
		if ((cpu.status.autoJoypadCounter & 1) == 0) {
			cpu.status.autoJoypadPort1 = controllerData(0);
			cpu.status.autoJoypadPort2 = controllerData(1);
		} else {
			cpu.io.joy1 = cpu.io.joy1 << 1 | (cpu.status.autoJoypadPort1 & 1);
			cpu.io.joy2 = cpu.io.joy2 << 1 | (cpu.status.autoJoypadPort2 & 1);
			cpu.io.joy3 = cpu.io.joy3 << 1 | (cpu.status.autoJoypadPort1 >> 1 & 1);
			cpu.io.joy4 = cpu.io.joy4 << 1 | (cpu.status.autoJoypadPort2 >> 1 & 1);
		}
	}
}

static void hdmaReset(void);
static bool hdmaEnable(void);
static bool hdmaActive(void);

// --- Balanced mode: the clock moves a bus access at a time, and what the counters bring about (the line's
// end, the interrupts, HDMA, the DRAM refresh, the automatic joypad polling) is looked at only when
// the access passed the place on the line where the next of them happens. ---

// Where on this line the next of them is (0: to be worked out).
static u32 fastNext;

// The IRQ's level where the counters are. With an H position the level stands for "taken on this
// line"; the line's end clears it.
static void fastIrq(void) {
	bool valid = false;
	if (cpu.io.irqEnable) {
		u32 v = cpu.counter.vcounter, h = cpu.counter.hcounter;
		if (!cpu.io.hirqEnable) {
			valid = v == cpu.io.vtime;
		} else if ((u32) cpu.io.htime + 10 < cpu.counter.hperiod) {
			valid = (!cpu.io.virqEnable || v == cpu.io.vtime) && h >= (u32) cpu.io.htime + 10;
		} else if (cpu.io.htime < cpu.counter.lastHperiod) {
			// Its place is in the next line's first clocks.
			valid = !cpu.io.virqEnable || (v ? v - 1 : cpu.counter.lastVperiod - 1) == cpu.io.vtime;
		}
	}
	if (raiseTo(&cpu.status.irqValid, valid)) {
		cpu.status.irqLine = 1;
		cpu.status.irqTransition = 1;
	}
}

static void fastPlan(void) {
	u32 h = cpu.counter.hcounter, next = cpu.counter.hperiod;
	if (!cpu.status.dramRefresh && (u32) cpu.status.dramRefreshPosition < next) {
		next = cpu.status.dramRefreshPosition;
	}
	if (!cpu.status.hdmaSetupTriggered && (u32) cpu.status.hdmaSetupPosition < next) {
		next = cpu.status.hdmaSetupPosition;
	}
	if (!cpu.status.hdmaTriggered && (u32) cpu.status.hdmaPosition < next) {
		next = cpu.status.hdmaPosition;
	}
	if (cpu.io.hirqEnable && !cpu.status.irqValid && (u32) cpu.io.htime + 10 > h && (u32) cpu.io.htime + 10 < next) {
		next = cpu.io.htime + 10;
	}
	if (cpu.counter.vcounter == ppu.state.vdisp && cpu.status.autoJoypadCounter == 33 && h < 130) {
		next = next < 130 ? next : 130;
	}
	fastNext = next;
}

static void fastCatch(void) {
	for (;;) {
		u32 h = cpu.counter.hcounter;
		fastIrq();
		if (!cpu.status.dramRefresh && h >= (u32) cpu.status.dramRefreshPosition) {
			// Five refresh cycles of eight clocks.
			cpu.status.dramRefresh = 2;
			cpu.counterCpu += 40;
			cpu.counter.hcounter += 40;
			threadStep(&cpu.th, 40);
			for (int n = 0; n < 5; ++n) {
				aluEdge();
			}
			continue;
		}
		if (!cpu.status.hdmaSetupTriggered && h >= (u32) cpu.status.hdmaSetupPosition) {
			cpu.status.hdmaSetupTriggered = 1;
			hdmaReset();
			if (hdmaEnable()) {
				cpu.status.hdmaPending = 1;
				cpu.status.hdmaMode = 0;
			}
		}
		if (!cpu.status.hdmaTriggered && h >= (u32) cpu.status.hdmaPosition) {
			cpu.status.hdmaTriggered = 1;
			if (hdmaActive()) {
				cpu.status.hdmaPending = 1;
				cpu.status.hdmaMode = 1;
			}
		}
		// The automatic polling's 34 cycles at once, a frame's marked done by the count past its end.
		if (cpu.counter.vcounter == ppu.state.vdisp && cpu.status.autoJoypadCounter == 33 && h >= 130) {
			cpu.status.autoJoypadCounter = 0;
			joypadCycle();
			while (cpu.status.autoJoypadCounter < 33) {
				cpu.status.autoJoypadCounter++;
				joypadCycle();
			}
			cpu.status.autoJoypadCounter = 34;
		}
		if (h < cpu.counter.hperiod) {
			break;
		}
		cpu.counter.lastHperiod = cpu.counter.hperiod;
		cpu.counter.hcounter -= cpu.counter.hperiod;
		counterTickScanline(&cpu.counter);
		// The others are brought up to the line's start, not to where the access ended, so the PPU's
		// lines stay whole.
		u64 const over = cpu.th.scalar * cpu.counter.hcounter;
		cpu.th.clock -= over;
		scanline();
		cpu.th.clock += over;
		if (cpu.io.hirqEnable) {
			cpu.status.irqValid = 0;
		}
		if (flipTo(&cpu.status.nmiValid, cpu.counter.vcounter >= ppu.state.vdisp)) {
			cpu.status.nmiLine = cpu.status.nmiValid;
			if (cpu.status.nmiLine && cpu.io.nmiEnable) {
				cpu.status.nmiTransition = 1;
			}
		}
	}
	fastPlan();
}

void cpuFastReplan(void) {
	fastNext = 0;
}

static void stepExact(u32 clocks);

static inline __attribute__((always_inline)) void step(u32 clocks) {
	cpu.status.irqLock = 0;
	if (systemBalanced && !inputGuns) {
		cpu.counterCpu += clocks;
		cpu.counter.hcounter += clocks;
		threadStep(&cpu.th, clocks);
		if (cpu.counter.hcounter >= fastNext) {
			fastCatch();
		}
		return;
	}
	stepExact(clocks);
}

static void stepExact(u32 clocks) {
	u32 ticks = clocks >> 1;
	while (ticks--) {
		cpu.counterCpu += 2;
		cpu.counter.hcounter += 2;
		if (cpu.counter.hcounter == cpu.counter.hperiod) {
			cpu.counter.lastHperiod = cpu.counter.hperiod;
			cpu.counter.hcounter = 0;
			counterTickScanline(&cpu.counter);
			scanline();
		}
		if (inputGuns) {
			inputBeam(cpu.counter.vcounter, cpu.counter.hcounter);
		}
		if (cpu.counter.hcounter & 2) {
			nmiPoll();
			irqPoll();
		}
		if (joypadCounter() == 0) {
			joypadEdge();
		}
	}

	if (!cpu.status.dramRefresh && cpu.counter.hcounter >= cpu.status.dramRefreshPosition) {
		// Five refresh cycles of 6 + 2 clocks.
		for (int n = 0; n < 5; ++n) {
			cpu.status.dramRefresh = 1;
			step(6);
			cpu.status.dramRefresh = 2;
			step(2);
			aluEdge();
		}
	}

	if (!cpu.status.hdmaSetupTriggered && cpu.counter.hcounter >= cpu.status.hdmaSetupPosition) {
		cpu.status.hdmaSetupTriggered = 1;
		hdmaReset();
		if (hdmaEnable()) {
			cpu.status.hdmaPending = 1;
			cpu.status.hdmaMode = 0;
		}
	}

	if (!cpu.status.hdmaTriggered && cpu.counter.hcounter >= cpu.status.hdmaPosition) {
		cpu.status.hdmaTriggered = 1;
		if (hdmaActive()) {
			cpu.status.hdmaPending = 1;
			cpu.status.hdmaMode = 1;
		}
	}

	threadStep(&cpu.th, clocks);
}

// Memory

static inline u32 waitFor(u32 address) {
	// 00-3f,80-bf:8000-ffff; 40-7f,c0-ff:0000-ffff
	if (address & 0x408000) {
		return address & 0x800000 ? cpu.io.romSpeed : 8;
	}
	// 00-3f,80-bf:0000-1fff,6000-7fff
	if ((address + 0x6000) & 0x4000) {
		return 8;
	}
	// 00-3f,80-bf:2000-3fff,4200-5fff
	if ((address - 0x4000) & 0x7e00) {
		return 6;
	}
	// 00-3f,80-bf:4000-41ff
	return 12;
}

static void idle(void) {
	cpu.status.clockCount = 6;
	dmaEdge();
	step(6);
	aluEdge();
}

static u8 read(u32 address) {
	address &= 0xffffff;
	cpu.status.clockCount = waitFor(address);
	dmaEdge();
	R.mar = address;
	step(cpu.status.clockCount - 4);
	u8 data = busRead(address, R.mdr);
	step(4);
	aluEdge();
	// Reads of 00-3f,80-bf:4000-43ff are inside the CPU and leave the data bus as it was.
	if ((address & 0x40fc00) != 0x4000) {
		R.mdr = data;
	}
	return data;
}

static void write(u32 address, u8 data) {
	address &= 0xffffff;
	aluEdge();
	cpu.status.clockCount = waitFor(address);
	dmaEdge();
	R.mar = address;
	step(cpu.status.clockCount);
	busWrite(address, R.mdr = data);
}

// DMA

static bool dmaEnable(void) {
	for (int n = 0; n < 8; ++n) {
		if (cpu.channels[n].dmaEnable) {
			return true;
		}
	}
	return false;
}

static bool hdmaEnable(void) {
	for (int n = 0; n < 8; ++n) {
		if (cpu.channels[n].hdmaEnable) {
			return true;
		}
	}
	return false;
}

static inline bool channelHdmaActive(const struct DmaChannel* channel) {
	return channel->hdmaEnable && !channel->hdmaCompleted;
}

static bool hdmaActive(void) {
	for (int n = 0; n < 8; ++n) {
		if (channelHdmaActive(&cpu.channels[n])) {
			return true;
		}
	}
	return false;
}

static inline void channelStep(u32 clocks) {
	cpu.counterDma += clocks;
	step(clocks);
}

// The A bus can't reach the B bus or the CPU's own registers.
static inline bool validA(u32 address) {
	if ((address & 0x40ff00) == 0x2100) {
		return false;
	}
	if ((address & 0x40fe00) == 0x4000) {
		return false;
	}
	if ((address & 0x40ffe0) == 0x4200) {
		return false;
	}
	if ((address & 0x40ff80) == 0x4300) {
		return false;
	}
	return true;
}

static u8 readA(u32 address) {
	channelStep(4);
	R.mdr = validA(address) ? busRead(address, R.mdr) : 0x00;
	channelStep(4);
	return R.mdr;
}

static u8 readB(u8 address, bool valid) {
	channelStep(4);
	R.mdr = valid ? busRead(0x2100 | address, R.mdr) : 0x00;
	channelStep(4);
	return R.mdr;
}

static void writeA(u32 address, u8 data) {
	if (validA(address)) {
		busWrite(address, data);
	}
}

static void writeB(u8 address, u8 data, bool valid) {
	if (valid) {
		busWrite(0x2100 | address, data);
	}
}

static void transfer(struct DmaChannel* channel, u32 addressA, u8 index) {
	u8 addressB = channel->targetAddress;
	switch (channel->transferMode) {
	case 1:
	case 5:
		addressB += index & 1;
		break;
	case 3:
	case 7:
		addressB += index >> 1 & 1;
		break;
	case 4:
		addressB += index;
		break;
	}

	// WRAM to WRAM does nothing.
	bool valid = addressB != 0x80 || ((addressA & 0xfe0000) != 0x7e0000 && (addressA & 0x40e000) != 0x0000);

	R.mar = addressA;
	if (channel->direction == 0) {
		u8 data = readA(addressA);
		writeB(addressB, data, valid);
	} else {
		u8 data = readB(addressB, valid);
		writeA(addressA, data);
	}
}

static void channelDmaRun(struct DmaChannel* channel) {
	if (!channel->dmaEnable) {
		return;
	}

	channelStep(8);
	dmaEdge();

	u8 index = 0;
	do {
		transfer(channel, channel->sourceBank << 16 | channel->sourceAddress, index++ & 3);
		if (!channel->fixedTransfer) {
			if (!channel->reverseTransfer) {
				channel->sourceAddress++;
			} else {
				channel->sourceAddress--;
			}
		}
		dmaEdge();
	} while (channel->dmaEnable && --channel->transferSize);

	channel->dmaEnable = false;
}

// Whether no later channel still has HDMA to do.
static bool hdmaFinished(const struct DmaChannel* channel) {
	for (const struct DmaChannel* next = channel + 1; next < cpu.channels + 8; ++next) {
		if (channelHdmaActive(next)) {
			return false;
		}
	}
	return true;
}

static void hdmaReload(struct DmaChannel* channel) {
	u8 data = readA(R.mar = channel->sourceBank << 16 | channel->hdmaAddress);

	if ((channel->lineCounter & 0x7f) == 0) {
		channel->lineCounter = data;
		channel->hdmaAddress++;

		channel->hdmaCompleted = channel->lineCounter == 0;
		channel->hdmaDoTransfer = !channel->hdmaCompleted;

		if (channel->indirect) {
			data = readA(R.mar = channel->sourceBank << 16 | channel->hdmaAddress++);
			channel->transferSize = data << 8 | 0x00;
			if (channel->hdmaCompleted && hdmaFinished(channel)) {
				return;
			}

			data = readA(R.mar = channel->sourceBank << 16 | channel->hdmaAddress++);
			channel->transferSize = data << 8 | channel->transferSize >> 8;
		}
	}
}

static void channelHdmaSetup(struct DmaChannel* channel) {
	channel->hdmaDoTransfer = true;
	if (!channel->hdmaEnable) {
		return;
	}

	// HDMA stops a DMA under way.
	channel->dmaEnable = false;
	channel->hdmaAddress = channel->sourceAddress;
	channel->lineCounter = 0;
	hdmaReload(channel);
}

static void channelHdmaTransfer(struct DmaChannel* channel) {
	if (!channelHdmaActive(channel)) {
		return;
	}
	channel->dmaEnable = false;
	if (!channel->hdmaDoTransfer) {
		return;
	}

	static const u8 lengths[8] = { 1, 2, 2, 4, 4, 4, 2, 4 };
	for (u8 index = 0; index < lengths[channel->transferMode]; ++index) {
		u32 address = !channel->indirect ? channel->sourceBank << 16 | channel->hdmaAddress++
		                                 : channel->indirectBank << 16 | channel->transferSize++;
		transfer(channel, address, index);
	}
}

static void channelHdmaAdvance(struct DmaChannel* channel) {
	if (!channelHdmaActive(channel)) {
		return;
	}
	channel->lineCounter--;
	channel->hdmaDoTransfer = channel->lineCounter & 0x80;
	hdmaReload(channel);
}

static void dmaRun(void) {
	cpu.counterDma += 8;
	step(8);
	dmaEdge();
	for (int n = 0; n < 8; ++n) {
		channelDmaRun(&cpu.channels[n]);
	}
	cpu.status.irqLock = true;
}

static void hdmaReset(void) {
	for (int n = 0; n < 8; ++n) {
		cpu.channels[n].hdmaCompleted = false;
		cpu.channels[n].hdmaDoTransfer = false;
	}
}

static void hdmaSetup(void) {
	cpu.counterDma += 8;
	step(8);
	for (int n = 0; n < 8; ++n) {
		channelHdmaSetup(&cpu.channels[n]);
	}
	cpu.status.irqLock = true;
}

static void hdmaRun(void) {
	cpu.counterDma += 8;
	step(8);
	for (int n = 0; n < 8; ++n) {
		channelHdmaTransfer(&cpu.channels[n]);
	}
	for (int n = 0; n < 8; ++n) {
		channelHdmaAdvance(&cpu.channels[n]);
	}
	cpu.status.irqLock = true;
}

// With DMA or HDMA pending and none under way, one whole CPU cycle runs first. Then, after waiting for
// the DMA clock: HDMA if pending and enabled, DMA if pending and enabled (HDMA may cut in), and the
// CPU's clock is rejoined.
static void dmaEdgeRun(void);

static inline __attribute__((always_inline)) void dmaEdge(void) {
	if (cpu.status.dmaActive || cpu.status.dmaPending || cpu.status.hdmaPending) {
		dmaEdgeRun();
	}
}

static void dmaEdgeRun(void) {
	if (cpu.status.dmaActive) {
		if (cpu.status.hdmaPending) {
			cpu.status.hdmaPending = 0;
			if (hdmaEnable()) {
				if (!dmaEnable()) {
					step(cpu.counterDma = 8 - dmaCounter());
				}
				if (cpu.status.hdmaMode == 0) {
					hdmaSetup();
				} else {
					hdmaRun();
				}
				if (!dmaEnable()) {
					step(cpu.status.clockCount - cpu.counterDma % cpu.status.clockCount);
					cpu.status.dmaActive = 0;
				}
			}
		}

		if (cpu.status.dmaPending) {
			cpu.status.dmaPending = 0;
			if (dmaEnable()) {
				step(cpu.counterDma = 8 - dmaCounter());
				dmaRun();
				step(cpu.status.clockCount - cpu.counterDma % cpu.status.clockCount);
				cpu.status.dmaActive = 0;
			}
		}
	}

	if (!cpu.status.dmaActive) {
		if (cpu.status.dmaPending || cpu.status.hdmaPending) {
			cpu.status.dmaActive = 1;
		}
	}
}

// Registers

static u8 readRAM(u32 address, u8 data) {
	(void) data;
	return cpu.wram[address];
}

static void writeRAM(u32 address, u8 data) {
	cpu.wram[address] = data;
}

static u8 readAPU(u32 address, u8 data) {
	(void) data;
	syncSmp();
	return smp.toCpu[address & 3];
}

static void writeAPU(u32 address, u8 data) {
	syncSmp();
	smp.fromCpu[address & 3] = data;
}

static u8 readCPU(u32 address, u8 data) {
	switch (address & 0xffff) {
	case 0x2180: {
		u32 wramAddress = cpu.io.wramAddress;
		cpu.io.wramAddress = (cpu.io.wramAddress + 1) & 0x1ffff;
		return busRead(0x7e0000 | wramAddress, data);
	}
	case 0x4016:
		return (data & ~3) | (controllerData(0) & 3);
	case 0x4017:
		// Bits 2-4 are wired to ground.
		return (data & ~0x1f) | (controllerData(1) & 3) | 0x1c;
	case 0x4210:
		data = (data & 0x70) | (cpu.io.version & 15);
		return data | rdnmi() << 7;
	case 0x4211:
		return (data & 0x7f) | timeup() << 7;
	case 0x4212:
		data &= ~0xc1;
		data |= cpu.io.autoJoypadPoll && cpu.status.autoJoypadCounter < 33;
		data |= (cpu.counter.hcounter <= 2 || cpu.counter.hcounter >= 1096) << 6;
		data |= (cpu.counter.vcounter >= ppu.state.vdisp) << 7;
		return data;
	case 0x4213:
		return cpu.io.pio;
	case 0x4214:
		return cpu.io.rddiv;
	case 0x4215:
		return cpu.io.rddiv >> 8;
	case 0x4216:
		return cpu.io.rdmpy;
	case 0x4217:
		return cpu.io.rdmpy >> 8;
	case 0x4218:
		return cpu.io.joy1;
	case 0x4219:
		return cpu.io.joy1 >> 8;
	case 0x421a:
		return cpu.io.joy2;
	case 0x421b:
		return cpu.io.joy2 >> 8;
	case 0x421c:
		return cpu.io.joy3;
	case 0x421d:
		return cpu.io.joy3 >> 8;
	case 0x421e:
		return cpu.io.joy4;
	case 0x421f:
		return cpu.io.joy4 >> 8;
	}
	return data;
}

static u8 readDMA(u32 address, u8 data) {
	const struct DmaChannel* channel = &cpu.channels[address >> 4 & 7];
	switch (address & 0xff8f) {
	case 0x4300:
		return channel->transferMode | channel->fixedTransfer << 3 | channel->reverseTransfer << 4 | channel->unused << 5 |
			channel->indirect << 6 | channel->direction << 7;
	case 0x4301:
		return channel->targetAddress;
	case 0x4302:
		return channel->sourceAddress;
	case 0x4303:
		return channel->sourceAddress >> 8;
	case 0x4304:
		return channel->sourceBank;
	case 0x4305:
		return channel->transferSize;
	case 0x4306:
		return channel->transferSize >> 8;
	case 0x4307:
		return channel->indirectBank;
	case 0x4308:
		return channel->hdmaAddress;
	case 0x4309:
		return channel->hdmaAddress >> 8;
	case 0x430a:
		return channel->lineCounter;
	case 0x430b:
	case 0x430f:
		return channel->unknown;
	}
	return data;
}

static void writeCPU(u32 address, u8 data) {
	fastNext = 0;
	switch (address & 0xffff) {
	case 0x2180: {
		u32 wramAddress = cpu.io.wramAddress;
		cpu.io.wramAddress = (cpu.io.wramAddress + 1) & 0x1ffff;
		busWrite(0x7e0000 | wramAddress, data);
		return;
	}
	case 0x2181:
		cpu.io.wramAddress = (cpu.io.wramAddress & 0x1ff00) | data;
		return;
	case 0x2182:
		cpu.io.wramAddress = (cpu.io.wramAddress & 0x100ff) | data << 8;
		return;
	case 0x2183:
		cpu.io.wramAddress = (cpu.io.wramAddress & 0x0ffff) | (data & 1) << 16;
		return;
	case 0x4016:
		// Bit 0 strobes both controller ports; $4017's is ignored.
		cpu.status.cpuLatch = data & 1;
		controllerLatch(0, cpu.status.autoJoypadLatch | cpu.status.cpuLatch);
		controllerLatch(1, cpu.status.autoJoypadLatch | cpu.status.cpuLatch);
		return;
	case 0x4200:
		cpu.io.autoJoypadPoll = data & 1;
		if (cpu.status.autoJoypadCounter == 0) {
			// The controllers can still be latched now.
			cpu.status.autoJoypadLatch = cpu.io.autoJoypadPoll;
			controllerLatch(0, cpu.status.autoJoypadLatch | cpu.status.cpuLatch);
			controllerLatch(1, cpu.status.autoJoypadLatch | cpu.status.cpuLatch);
		} else if (!cpu.io.autoJoypadPoll && cpu.status.autoJoypadCounter >= 2) {
			cpu.status.autoJoypadCounter = 33;
		}
		nmitimenUpdate(data);
		return;
	case 0x4201:
		if ((cpu.io.pio & 0x80) && !(data & 0x80)) {
			ppuLatchCounters();
		}
		cpu.io.pio = data;
		return;
	case 0x4202:
		cpu.io.wrmpya = data;
		return;
	case 0x4203:
		cpu.io.rdmpy = 0;
		if (cpu.alu.mpyctr || cpu.alu.divctr) {
			return;
		}
		cpu.io.wrmpyb = data;
		cpu.io.rddiv = cpu.io.wrmpyb << 8 | cpu.io.wrmpya;
		// The multiplication takes the next eight cycles.
		cpu.alu.mpyctr = 8;
		cpu.alu.shift = cpu.io.wrmpyb;
		return;
	case 0x4204:
		cpu.io.wrdiva = (cpu.io.wrdiva & 0xff00) | data;
		return;
	case 0x4205:
		cpu.io.wrdiva = (cpu.io.wrdiva & 0x00ff) | data << 8;
		return;
	case 0x4206:
		cpu.io.rdmpy = cpu.io.wrdiva;
		if (cpu.alu.mpyctr || cpu.alu.divctr) {
			return;
		}
		cpu.io.wrdivb = data;
		// The division takes the next sixteen cycles.
		cpu.alu.divctr = 16;
		cpu.alu.shift = cpu.io.wrdivb << 16;
		return;
	case 0x4207:
		cpu.io.htime = ((cpu.io.htime >> 2) - 1) & 0xfff;
		cpu.io.htime = (cpu.io.htime & 0xf00) | data;
		cpu.io.htime = ((cpu.io.htime + 1) << 2) & 0xfff;
		irqPoll();
		return;
	case 0x4208:
		cpu.io.htime = ((cpu.io.htime >> 2) - 1) & 0xfff;
		cpu.io.htime = (cpu.io.htime & 0xeff) | (data & 1) << 8;
		cpu.io.htime = ((cpu.io.htime + 1) << 2) & 0xfff;
		irqPoll();
		return;
	case 0x4209:
		cpu.io.vtime = (cpu.io.vtime & 0x100) | data;
		irqPoll();
		return;
	case 0x420a:
		cpu.io.vtime = (cpu.io.vtime & 0x0ff) | (data & 1) << 8;
		irqPoll();
		return;
	case 0x420b:
		for (int n = 0; n < 8; ++n) {
			cpu.channels[n].dmaEnable = data >> n & 1;
		}
		if (data) {
			cpu.status.dmaPending = true;
		}
		return;
	case 0x420c:
		for (int n = 0; n < 8; ++n) {
			cpu.channels[n].hdmaEnable = data >> n & 1;
		}
		return;
	case 0x420d:
		cpu.io.romSpeed = data & 1 ? 6 : 8;
		return;
	}
}

static void writeDMA(u32 address, u8 data) {
	struct DmaChannel* channel = &cpu.channels[address >> 4 & 7];
	switch (address & 0xff8f) {
	case 0x4300:
		channel->transferMode = data & 7;
		channel->fixedTransfer = data & 0x08;
		channel->reverseTransfer = data & 0x10;
		channel->unused = data & 0x20;
		channel->indirect = data & 0x40;
		channel->direction = data & 0x80;
		return;
	case 0x4301:
		channel->targetAddress = data;
		return;
	case 0x4302:
		channel->sourceAddress = (channel->sourceAddress & 0xff00) | data;
		return;
	case 0x4303:
		channel->sourceAddress = (channel->sourceAddress & 0x00ff) | data << 8;
		return;
	case 0x4304:
		channel->sourceBank = data;
		return;
	case 0x4305:
		channel->transferSize = (channel->transferSize & 0xff00) | data;
		return;
	case 0x4306:
		channel->transferSize = (channel->transferSize & 0x00ff) | data << 8;
		return;
	case 0x4307:
		channel->indirectBank = data;
		return;
	case 0x4308:
		channel->hdmaAddress = (channel->hdmaAddress & 0xff00) | data;
		return;
	case 0x4309:
		channel->hdmaAddress = (channel->hdmaAddress & 0x00ff) | data << 8;
		return;
	case 0x430a:
		channel->lineCounter = data;
		return;
	case 0x430b:
	case 0x430f:
		channel->unknown = data;
		return;
	}
}

u8 cpuReadDMA(u32 address, u8 data) {
	return readDMA(address, data);
}

void cpuWriteDMA(u32 address, u8 data) {
	writeDMA(address, data);
}

void cpuMap(void) {
	busMap(readRAM, writeRAM, "00-3f,80-bf:0000-1fff", 0x2000, 0, 0);
	busMap(readRAM, writeRAM, "7e-7f:0000-ffff", 0x20000, 0, 0);
	busMap(readAPU, writeAPU, "00-3f,80-bf:2140-217f", 0, 0, 0);
	busMap(readCPU, writeCPU, "00-3f,80-bf:2180-2183,4016-4017,4200-421f", 0, 0, 0);
	busMap(readDMA, writeDMA, "00-3f,80-bf:4300-437f", 0, 0, 0);
}

void cpuMain(void) {
	// A waiting CPU idles until an interrupt is looked for and found, then idles once more. A stopped
	// one idles for good.
	if (R.wai) {
		lastCycle();
		idle();
		if (!R.wai) {
			idle();
		}
		return;
	}
	if (R.stp) {
		lastCycle();
		idle();
		return;
	}

	if (!cpu.status.interruptPending) {
		instruction();
		return;
	}

	if (cpu.status.nmiPending) {
		cpu.status.nmiPending = 0;
		R.vector = R.e ? 0xfffa : 0xffea;
		interrupt();
		return;
	}

	if (cpu.status.irqPending) {
		cpu.status.irqPending = 0;
		R.vector = R.e ? 0xfffe : 0xffee;
		interrupt();
		return;
	}

	if (cpu.status.resetPending) {
		cpu.status.resetPending = 0;
		step(132);
		R.vector = 0xfffc;
		interrupt();
		return;
	}

	cpu.status.interruptPending = 0;
}

void cpuPower(bool reset) {
	w65816Power();
	threadFrequency(&cpu.th, sys.cpuFrequency);
	counterReset(&cpu.counter);

	if (!reset) {
		randomArray(cpu.wram, sizeof(cpu.wram));
	}

	for (int n = 0; n < 8; ++n) {
		struct DmaChannel* channel = &cpu.channels[n];
		memset(channel, 0, sizeof(*channel));
		channel->transferMode = 7;
		channel->fixedTransfer = 1;
		channel->reverseTransfer = 1;
		channel->unused = 1;
		channel->indirect = 1;
		channel->direction = 1;
		channel->targetAddress = 0xff;
		channel->sourceAddress = 0xffff;
		channel->sourceBank = 0xff;
		channel->transferSize = 0xffff;
		channel->indirectBank = 0xff;
		channel->hdmaAddress = 0xffff;
		channel->lineCounter = 0xff;
		channel->unknown = 0xff;
	}

	cpu.counterCpu = 0;
	cpu.counterDma = 0;

	memset(&cpu.io, 0, sizeof(cpu.io));
	cpu.io.pio = 0xff;
	cpu.io.wrmpya = 0xff;
	cpu.io.wrmpyb = 0xff;
	cpu.io.wrdiva = 0xffff;
	cpu.io.wrdivb = 0xff;
	cpu.io.htime = (0x1ff + 1) << 2;
	cpu.io.vtime = 0x1ff;
	cpu.io.romSpeed = 8;
	cpu.io.version = 2;

	memset(&cpu.alu, 0, sizeof(cpu.alu));

	memset(&cpu.status, 0, sizeof(cpu.status));
	cpu.status.autoJoypadCounter = 33;
	cpu.status.dramRefreshPosition = (cpu.io.version == 1 ? 530 : 538);
	cpu.status.hdmaSetupPosition = (cpu.io.version == 1 ? 12 + 8 - dmaCounter() : 12 + dmaCounter());
	cpu.status.hdmaPosition = 1104;
	cpu.status.resetPending = 1;
	cpu.status.interruptPending = 1;
}
