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
static inline void idle(void);
static inline void lastCycle(void);
static inline void step(u32 clocks);
static inline void dmaEdge(void);

// Fast mode's accesses, unless a gun is plugged in: worked out once an instruction.
static bool fastNow;
// Balanced and Fast: where on this line the next of the counters' events is (0: to be worked out).
static u32 fastNext;
static void fastCatch(void);
static void fastWaitChoose(void);

// Fast mode: the clocks the accesses to plain memory took, which the counters and the clock get when
// something looks at them (fastSettle). What the counters bring about is seen to between
// instructions, or before an access that isn't to plain memory (fastCatchUp).
static u32 fastPending;

static inline void fastSettle(void) {
	u32 clocks = fastPending;
	if (clocks) {
		fastPending = 0;
		cpu.counterCpu += clocks;
		cpu.counter.hcounter += clocks;
		cpu.th.clock += cpu.th.scalar * clocks;
	}
}

static inline void fastCatchUp(void) {
	if (!fastNow) {
		return;
	}
	fastSettle();
	if (cpu.counter.hcounter >= fastNext) {
		fastCatch();
	}
}
// Where the instruction being run started.
static u32 waitFrom;
// Fast mode looks at taken branches back a little way for loops the CPU waits in.
static void fastBranch(void);
static bool fastBranchLong(void);
// A BRL (0x82) back further can close a loop too: it is rare in loops that do work.
#define idleBranch() \
	do { \
		if (fastNow && R.pc.d <= waitFrom && (waitFrom - R.pc.d <= 12 || (waitFrom - R.pc.d <= 0x400 && fastBranchLong()))) { \
			fastBranch(); \
		} \
	} while (0)
#include "w65816.inc"

// The other processors, brought up to the CPU's clock.

static void syncSmp(void) {
	if (smpThreaded) {
		smpPost(cpu.th.clock, false, 0, 0);
		return;
	}
	smpLimit = cpu.th.clock;
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

static void fastIrqSettle(void);

static void irqTimesWritten(void) {
	if (systemBalanced) {
		fastIrqSettle();
	} else {
		irqPoll();
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
	if (systemBalanced) {
		fastIrqSettle();
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
static void lastCycleLook(void) {
	fastCatchUp();
	// The IRQ is a level: Exact's polls find it again every few clocks while it stays up, so one
	// looked for while I was set isn't lost. Balanced and Fast poll only here.
	if (systemBalanced && cpu.status.irqLine && cpu.io.irqEnable) {
		cpu.status.irqTransition = 1;
	}
	// Balanced and Fast raise the NMI as the first line below the picture starts; Exact's polls see it
	// from H 6 on.
	bool nmiYet = !systemBalanced || cpu.counter.vcounter != ppu.state.vdisp || cpu.counter.hcounter >= 6;
	if (!cpu.status.irqLock) {
		if (nmiYet && nmiTest()) {
			cpu.status.nmiPending = 1;
			cpu.status.interruptPending = 1;
		}
		if (irqTest()) {
			cpu.status.irqPending = 1;
			cpu.status.interruptPending = 1;
		}
	}
}

// With no interrupt line or transition up, there is nothing to look at.
static inline void lastCycle(void) {
	if (cpu.status.irqLine || cpu.status.nmiTransition || cpu.status.irqTransition || R.irq) {
		lastCycleLook();
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

// Fast and Balanced poll the joypads at once (autoJoypadCounter 34), but $4212 shows the polling busy
// as long as Exact takes: 33 cycles of 128 clocks from the first 256-clock edge at or after H 130 of the
// first line below the picture (lines taken as 1364 clocks).
static bool joypadBusyLines(void) {
	return cpu.counter.vcounter >= ppu.state.vdisp && cpu.counter.vcounter <= ppu.state.vdisp + 4u;
}

static bool joypadBusyFast(void) {
	if (!joypadBusyLines()) {
		return false;
	}
	u32 since = (cpu.counter.vcounter - ppu.state.vdisp) * 1364 + cpu.counter.hcounter;
	if (since < 130) {
		return false;
	}
	u32 start = 130 + ((256 - ((cpu.counterCpu - (since - 130)) & 255)) & 255);
	return since >= start && since < start + 33 * 128;
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

// The IRQ's level where the counters are. With an H position the level stands for "taken on this
// line"; the line's end clears it.
static bool fastIrqLevel(void) {
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
	return valid;
}

static void fastIrq(void) {
	if (raiseTo(&cpu.status.irqValid, fastIrqLevel())) {
		cpu.status.irqLine = 1;
		cpu.status.irqTransition = 1;
	}
}

// Balanced and Fast: new IRQ times or enables take the level where the counters are without raising it,
// as a place this line has passed doesn't come again; one still ahead is planned for.
static void fastIrqSettle(void) {
	cpu.status.irqValid = fastIrqLevel();
	cpuFastReplan();
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

// fastCatch calls, so that a turn of a waiting loop an event fell in isn't taken for its length.
static u32 fastCatches;

static void fastCatch(void) {
	fastCatches++;
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

static inline u32 waitForSpeed(u32 address, u32 romSpeed) {
	// 00-3f,80-bf:8000-ffff; 40-7f,c0-ff:0000-ffff
	if (address & 0x408000) {
		return address & 0x800000 ? romSpeed : 8;
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

static inline u32 waitFor(u32 address) {
	return waitForSpeed(address, cpu.io.romSpeed);
}

// Fast mode: each page's access time, for either ROM speed, and the one there is now.
static u8 fastWaits[2][0x10000];
static const u8* fastWait = fastWaits[1];

static void fastWaitsInit(void) {
	for (u32 page = 0; page < 0x10000; ++page) {
		fastWaits[0][page] = (u8) waitForSpeed(page << 8, 6);
		fastWaits[1][page] = (u8) waitForSpeed(page << 8, 8);
	}
}

static void fastWaitChoose(void) {
	fastWait = fastWaits[cpu.io.romSpeed == 6 ? 0 : 1];
}

// Fast mode: the S-CPU waiting in a loop. A turn of it that writes nothing and reads only memory
// reads the same again until the next event: as many whole turns as end before it are taken in one.
// The registers from A to the E flag, in the order the struct has them.
#define WAIT_FROM offsetof(struct W65816, a)
#define WAIT_SIZE (offsetof(struct W65816, e) + sizeof(bool) - WAIT_FROM)

static struct {
	bool valid;
	u32 pc;
	u8 regs[WAIT_SIZE];
	u8 mdr;
	u64 clock;
	u32 catches;
	bool unsafe;
	bool hvbjoy;  // the turn read $4212
	bool apu;     // and an APU port
	u8 apuRead;   // the ports it read (a bit each), what it read last from each, and when
	u8 apuSeen[4];
	u64 apuLast;
	bool quiet;   // no event in the turn before the one kept, whose DMA or interrupt it would have had
	u8 sa1Count;  // the SA-1's I-RAM and BW-RAM bytes the turn read (sa1WatchCPU)
	u32 sa1Read[2];
} wait;
// A write to the APU's ports the SMP was run past in a skip: reads before it get the ports as they were,
// and ones before the next instruction started (which no SMP instruction comes before) as they are
// then, each port's write seen from its clock on.
static struct {
	bool on;
	struct SmpPortWrite write;
} apuHeld;
// Fast mode: reads of the APU's ports that come close together have the SMP run on this thread, as
// waiting for the APU's thread takes longer than the little it has to do between them.
static struct {
	u64 last;
	u32 close;
} apuPace;

void cpuFrameStart(void) {
	wait.valid = false;
	wait.catches = fastCatches;
	fastPlan();
	fastWaitChoose();
	wait.unsafe = false;
	wait.hvbjoy = false;
	wait.apu = false;
	wait.apuRead = 0;
	wait.sa1Count = 0;
	apuHeld.on = false;
	apuPace.close = 0;
	smpPortsSettle();
}

// Fast mode: an access to plain memory only moves the clock on. The IRQ lock, the DMA and the ALU are
// seen to between instructions (fastBetween), as snes_faust does.
static inline __attribute__((always_inline)) void stepFast(u32 clocks) {
	fastPending += clocks;
}

// One that isn't brings everything up to where it ends.
static inline void stepFastNow(u32 clocks) {
	fastCatchUp();
	cpu.counterCpu += clocks;
	cpu.counter.hcounter += clocks;
	threadStep(&cpu.th, clocks);
	if (cpu.counter.hcounter >= fastNext) {
		fastCatch();
	}
}

static inline bool fastAccess(void) {
	return fastNow;
}

static inline __attribute__((always_inline)) void idle(void) {
	if (fastAccess()) {
		stepFast(6);
		return;
	}
	cpu.status.clockCount = 6;
	dmaEdge();
	step(6);
	aluEdge();
}

static u8 readSlow(u32 address);

// Fast mode's read of plain memory is made in place; the rest are a call.
static inline __attribute__((always_inline)) u8 read(u32 address) {
	address &= 0xffffff;
	if (fastAccess()) {
		const u8* memory = busFastPages[address >> 8];
		if (memory) {
			stepFast(fastWait[address >> 8]);
			R.mar = address;
			return R.mdr = memory[address & 0xff];
		}
	}
	return readSlow(address);
}

static __attribute__((noinline)) u8 readSlow(u32 address) {
	fastCatchUp();
	if (fastAccess()) {
		// The GSU's status changes only as the GSU stops, which a skip stops at; $4212 and the APU's
		// ports only where fastSkipTurns keeps them from.
		if ((address & 0x40ffff) == 0x4212) {
			wait.hvbjoy = true;
		} else if ((address & 0x40ffc0) == 0x2140) {
			wait.apu = true;
		} else if (cart.sa1 && wait.sa1Count < 2 && sa1WatchCPU(address) != ~0u) {
			// The SA-1's I-RAM and BW-RAM change only as the SA-1 writes them, which a skip stops at.
			wait.sa1Read[wait.sa1Count++] = sa1WatchCPU(address);
		} else if (!cart.gsu || (address & 0x40fffe) != 0x3030) {
			wait.unsafe = true;
		}
	}
	cpu.status.clockCount = waitFor(address);
	dmaEdge();
	R.mar = address;
	if (systemBalanced && !inputGuns) {
		// Balanced mode: a read of plain memory takes its time in one step.
		u32 at = address & 0x40e000 ? address : 0x7e0000 | (address & 0x1fff);
		const u8* memory = busDirectPages[at >> 8];
		if (memory) {
			step(cpu.status.clockCount);
			u8 data = memory[at & 0xff];
			aluEdge();
			R.mdr = data;
			return data;
		}
	}
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
	if (fastAccess()) {
		wait.unsafe = true;
		// WRAM is written in place: it takes 8 clocks, and starts no multiplication or division.
		u32 at = address & 0x40e000 ? address : 0x7e0000 | (address & 0x1fff);
		if ((at & 0xfe0000) == 0x7e0000) {
			stepFast(8);
			R.mar = address;
			cpu.wram[at & 0x1ffff] = R.mdr = data;
			return;
		}
		stepFastNow(waitFor(address));
		R.mar = address;
		busWrite(address, R.mdr = data);
		// A multiplication or division has its result at once.
		while (cpu.alu.mpyctr || cpu.alu.divctr) {
			aluEdge();
		}
		// A DMA starts at the access edges that follow, as in Balanced mode.
		if (cpu.status.dmaPending) {
			fastNow = false;
		}
		return;
	}
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

static inline u8 transferAddressB(const struct DmaChannel* channel, u8 index) {
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
	return addressB;
}

static void transfer(struct DmaChannel* channel, u32 addressA, u8 index) {
	u8 addressB = transferAddressB(channel, index);

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

// Fast mode's one-pass DMA reads plain memory and the Super FX's RAM a page at a time, from where the
// page's reader would read it.
struct DmaSource {
	u32 page;
	const u8* memory;
	const u8* gsuRam;
};

static inline void dmaSourceStart(struct DmaSource* from) {
	from->page = ~0u;
	from->gsuRam = cart.gsu ? gsuRamCPU() : NULL;
}

static inline u8 dmaSourceRead(struct DmaSource* from, u32 address, u8 mdr) {
	u32 at = address & 0x40e000 ? address : 0x7e0000 | (address & 0x1fff);
	if (at >> 8 != from->page) {
		from->page = at >> 8;
		from->memory = busDirectPages[at >> 8];
		if (!from->memory && from->gsuRam && (at & 0xfe0000) == 0x700000) {
			from->memory = busPageMemory(at >> 8, gsuReadRamCPU, from->gsuRam);
		}
	}
	return from->memory ? from->memory[at & 0xff] : busRead(address, mdr);
}

// The bytes a one-pass DMA can move before the CPU's next event.
static inline u32 dmaRoom(u32 remaining) {
	if (!fastNext) {
		fastPlan();
	}
	u32 h = cpu.counter.hcounter;
	u32 room = fastNext <= h ? 0 : (fastNext - h - 1) / 8;
	return room < remaining ? room : remaining;
}

// Fast mode (whose DMA runs on the plain path, fastNow off): bytes to $2118/$2119 from plain memory or
// the Super FX's RAM, while neither the CPU nor the PPU has anything to do before the last of them is
// written, go in one pass. Each takes its eight clocks as transfer() would; with nothing pending, the
// dmaEdge after each does nothing. Whether the channel is done.
static bool dmaToVram(struct DmaChannel* channel, u8* index) {
	enum { MOST = 512 };
	u8 data[MOST], high[MOST];
	u8 first = transferAddressB(channel, *index & 3);
	if (first != 0x18 && first != 0x19) {
		return false;
	}
	u32 remaining = channel->transferSize ? channel->transferSize : 0x10000;
	u32 most = dmaRoom(remaining < MOST ? remaining : MOST);
	// And by the PPU's clock.
	u64 until = ppuVramDmaUntil();
	u64 clocks = cpu.th.scalar * 8;
	if (!until || until < cpu.th.clock) {
		most = 0;
	} else if ((until - cpu.th.clock) / clocks < most) {
		most = (until - cpu.th.clock) / clocks;
	}
	u8 ports[4];
	for (u32 n = 0; n < 4; ++n) {
		ports[n] = transferAddressB(channel, (*index + n) & 3);
	}
	struct DmaSource from;
	dmaSourceStart(&from);
	u16 source = channel->sourceAddress;
	u32 count = 0;
	u8 mdr = R.mdr;
	u32 address = 0;
	while (count < most) {
		u8 port = ports[count & 3];
		address = channel->sourceBank << 16 | source;
		u32 at = address & 0x40e000 ? address : 0x7e0000 | (address & 0x1fff);
		if ((port != 0x18 && port != 0x19) || (!busFastPages[at >> 8] && !(cart.gsu && (address & 0xfe0000) == 0x700000))) {
			break;
		}
		data[count] = mdr = dmaSourceRead(&from, address, mdr);
		high[count] = port & 1;
		if (!channel->fixedTransfer) {
			source += channel->reverseTransfer ? -1 : 1;
		}
		count++;
	}
	if (!count) {
		return false;
	}
	R.mar = address;
	R.mdr = mdr;
	cpu.status.irqLock = 0;
	cpu.counterDma += 8 * count;
	cpu.counterCpu += 8 * count;
	cpu.counter.hcounter += 8 * count;
	cpu.th.clock += clocks * count;
	ppuVramDma(data, high, count);
	channel->sourceAddress = source;
	*index += count;
	channel->transferSize -= count;
	return count == remaining;
}

// The same for bytes to $2180, the WRAM port, from memory that isn't WRAM: they go straight into WRAM.
static bool dmaToWram(struct DmaChannel* channel, u8* index) {
	if (transferAddressB(channel, *index & 3) != 0x80) {
		return false;
	}
	u32 remaining = channel->transferSize ? channel->transferSize : 0x10000;
	u32 most = dmaRoom(remaining);
	u8 ports[4];
	for (u32 n = 0; n < 4; ++n) {
		ports[n] = transferAddressB(channel, (*index + n) & 3);
	}
	struct DmaSource from;
	dmaSourceStart(&from);
	u16 source = channel->sourceAddress;
	u32 count = 0;
	u8 mdr = R.mdr;
	u32 address = 0;
	u32 wram = cpu.io.wramAddress;
	while (count < most) {
		address = channel->sourceBank << 16 | source;
		u32 at = address & 0x40e000 ? address : 0x7e0000 | (address & 0x1fff);
		if (ports[count & 3] != 0x80 || (at & 0xfe0000) == 0x7e0000 ||
		    (!busFastPages[at >> 8] && !(cart.gsu && (address & 0xfe0000) == 0x700000))) {
			break;
		}
		mdr = dmaSourceRead(&from, address, mdr);
		cpu.wram[wram] = mdr;
		wram = (wram + 1) & 0x1ffff;
		if (!channel->fixedTransfer) {
			source += channel->reverseTransfer ? -1 : 1;
		}
		count++;
	}
	cpu.io.wramAddress = wram;
	if (!count) {
		return false;
	}
	R.mar = address;
	R.mdr = mdr;
	cpu.status.irqLock = 0;
	cpu.counterDma += 8 * count;
	cpu.counterCpu += 8 * count;
	cpu.counter.hcounter += 8 * count;
	cpu.th.clock += cpu.th.scalar * 8 * count;
	channel->sourceAddress = source;
	*index += count;
	channel->transferSize -= count;
	return count == remaining;
}

// And bytes the other way, from $2180 to the Super FX's RAM.
static bool dmaFromWram(struct DmaChannel* channel, u8* index) {
	if (!cart.gsu || transferAddressB(channel, *index & 3) != 0x80) {
		return false;
	}
	u32 remaining = channel->transferSize ? channel->transferSize : 0x10000;
	u32 most = dmaRoom(remaining);
	u8 ports[4];
	for (u32 n = 0; n < 4; ++n) {
		ports[n] = transferAddressB(channel, (*index + n) & 3);
	}
	struct DmaSource from;
	dmaSourceStart(&from);
	u16 source = channel->sourceAddress;
	u32 count = 0;
	u8 mdr = R.mdr;
	u32 address = 0;
	u32 wram = cpu.io.wramAddress;
	while (count < most) {
		address = channel->sourceBank << 16 | source;
		if (ports[count & 3] != 0x80 || (address & 0xfe0000) != 0x700000) {
			break;
		}
		mdr = dmaSourceRead(&from, 0x7e0000 | wram, mdr);
		wram = (wram + 1) & 0x1ffff;
		busWrite(address, mdr);
		if (!channel->fixedTransfer) {
			source += channel->reverseTransfer ? -1 : 1;
		}
		count++;
	}
	cpu.io.wramAddress = wram;
	if (!count) {
		return false;
	}
	R.mar = address;
	R.mdr = mdr;
	cpu.status.irqLock = 0;
	cpu.counterDma += 8 * count;
	cpu.counterCpu += 8 * count;
	cpu.counter.hcounter += 8 * count;
	cpu.th.clock += cpu.th.scalar * 8 * count;
	channel->sourceAddress = source;
	*index += count;
	channel->transferSize -= count;
	return count == remaining;
}

static void channelDmaRun(struct DmaChannel* channel) {
	if (!channel->dmaEnable) {
		return;
	}

	channelStep(8);
	dmaEdge();

	u8 index = 0;
	do {
		if (systemFast && !inputGuns &&
		    (channel->direction == 0 ? dmaToVram(channel, &index) || dmaToWram(channel, &index) : dmaFromWram(channel, &index))) {
			break;
		}
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

// What a turn of a waiting loop read from a port, for fastSkipTurns.
static u8 apuSaw(u32 address, u8 data) {
	wait.apuRead |= 1 << (address & 3);
	wait.apuSeen[address & 3] = data;
	wait.apuLast = cpu.th.clock;
	return data;
}

static u8 readAPU(u32 address, u8 data) {
	(void) data;
	if (apuHeld.on) {
		if (cpu.th.clock < apuHeld.write.at) {
			return apuSaw(address, apuHeld.write.before[address & 3]);
		}
		if (cpu.th.clock <= apuHeld.write.done) {
			return apuSaw(address, smpPortRead(address & 3, cpu.th.clock));
		}
		apuHeld.on = false;
	}
	if (smpThreadable) {
		u64 since = cpu.th.clock - apuPace.last;
		apuPace.last = cpu.th.clock;
		if (since >= (u64) 1364 * cpu.th.scalar) {
			apuPace.close = 0;
			smpThreaded = true;
		} else if (++apuPace.close == 4) {
			smpDrain();
			smpThreaded = false;
		}
	}
	syncSmp();
	smpDrain();
	return apuSaw(address, systemBalanced ? smpPortRead(address & 3, cpu.th.clock) : smp.toCpu[address & 3]);
}

static void writeAPU(u32 address, u8 data) {
	if (smpThreaded) {
		smpPost(cpu.th.clock, true, address & 3, data);
		return;
	}
	if (systemBalanced) {
		smpRunToWrite(cpu.th.clock, true, address & 3, data);
		return;
	}
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
		data |= cpu.io.autoJoypadPoll && (cpu.status.autoJoypadCounter < 33 || (cpu.status.autoJoypadCounter == 34 && joypadBusyFast()));
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
		irqTimesWritten();
		return;
	case 0x4208:
		cpu.io.htime = ((cpu.io.htime >> 2) - 1) & 0xfff;
		cpu.io.htime = (cpu.io.htime & 0xeff) | (data & 1) << 8;
		cpu.io.htime = ((cpu.io.htime + 1) << 2) & 0xfff;
		irqTimesWritten();
		return;
	case 0x4209:
		cpu.io.vtime = (cpu.io.vtime & 0x100) | data;
		irqTimesWritten();
		return;
	case 0x420a:
		cpu.io.vtime = (cpu.io.vtime & 0x0ff) | (data & 1) << 8;
		irqTimesWritten();
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
		fastWaitChoose();
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
	busDirect(readRAM, cpu.wram);
	busMap(readRAM, writeRAM, "00-3f,80-bf:0000-1fff", 0x2000, 0, 0);
	busMap(readRAM, writeRAM, "7e-7f:0000-ffff", 0x20000, 0, 0);
	busMap(readAPU, writeAPU, "00-3f,80-bf:2140-217f", 0, 0, 0);
	busMap(readCPU, writeCPU, "00-3f,80-bf:2180-2183,4016-4017,4200-421f", 0, 0, 0);
	busMap(readDMA, writeDMA, "00-3f,80-bf:4300-437f", 0, 0, 0);
}

static void fastTakeTurns(u64 turns, u64 turn) {
	u32 clocks = (u32) (turns * turn / cpu.th.scalar);
	cpu.counterCpu += clocks;
	cpu.counter.hcounter += clocks;
	cpu.th.clock += turns * turn;
}

// The turns of a loop found waiting, taken in one. True when they end at the counters' next event.
static bool fastSkipOnce(u64 turn, bool hvbjoy, bool apu, u64 apuBack) {
	u32 h = cpu.counter.hcounter, limitH = fastNext;
	if (!turn || limitH <= h) {
		return false;
	}
	bool toEvent = true;
	// $4212 read the same in the turn kept and the ones taken: its H blank bit changes at 3 and 1096,
	// the V blank bit at the line's end, and the joypad bit as a read ends.
	if (hvbjoy) {
		u32 from = h - (u32) (turn / cpu.th.scalar);
		if (from > h || from < 3 || (cpu.io.autoJoypadPoll && (cpu.status.autoJoypadCounter < 33 || joypadBusyLines()))) {
			return false;
		}
		if (from < 1096) {
			if (h >= 1096) {
				return false;
			}
			limitH = limitH < 1096 ? limitH : 1096;
		}
	}
	u64 room = (u64) (limitH - h) * cpu.th.scalar;
	// The GSU stopping can interrupt the S-CPU or end what it waits for: no further than that.
	if (cart.gsu) {
		u64 stopped = gsuRunTo(cpu.th.clock + room);
		if (stopped != ~0ull) {
			room = stopped > cpu.th.clock ? stopped - cpu.th.clock : 0;
			toEvent = false;
		}
	}
	// So can the SA-1 writing what the loop reads, or its registers.
	if (cart.sa1) {
		u64 stopped = sa1RunTo(cpu.th.clock + room, wait.sa1Read, wait.sa1Count);
		if (stopped != ~0ull) {
			room = stopped > cpu.th.clock ? stopped - cpu.th.clock : 0;
			toEvent = false;
		}
	}
	// Not worth it for less than two turns.
	if (room < 2 * turn) {
		return false;
	}
	// Whole turns that end before the limit: the turn the limit falls in is run, so it sees its event.
	u64 turns = (room - 1) / turn;
	// The ports read the same up to the SMP's first write to them: the SMP is run to it (apuHeld), but
	// no further than the last turn taken reads them, so that it is where it would be after that read.
	if (apu) {
		// A write run to before is held to: past it, the next read sees it.
		if (!apuHeld.on) {
			// A port the SMP wrote after the turn read it (as it ran for a later read) reads otherwise
			// now, or will once the S-CPU's clock reaches the write. With no write in the turn, the SMP
			// has nothing to do since its last read.
			if (smpPortsPending(cpu.th.clock)) {
				return false;
			}
			for (u32 n = 0; n < 4; ++n) {
				if (wait.apuRead >> n & 1 && wait.apuSeen[n] != smp.toCpu[n]) {
					return false;
				}
			}
			apuHeld.on = smpRunToPortWrite(cpu.th.clock + turns * turn - apuBack, &apuHeld.write);
		}
		if (apuHeld.on) {
			u64 at = apuHeld.write.at;
			if (at < cpu.th.clock + room) {
				room = at > cpu.th.clock ? at - cpu.th.clock : 0;
			}
			if (room < 2 * turn) {
				return false;
			}
			turns = (room - 1) / turn;
		}
	}
	fastTakeTurns(turns, turn);
	return toEvent;
}

// HDMA that writes neither WRAM nor A-bus memory leaves what the loop reads as it was.
static bool hdmaKeepsMemory(void) {
	for (int n = 0; n < 8; ++n) {
		struct DmaChannel* channel = &cpu.channels[n];
		if (channel->hdmaEnable && (channel->direction || (u8) (channel->targetAddress - 0x7d) <= 6)) {
			return false;
		}
	}
	return true;
}

// A loop that reads neither $4212 nor the APU's ports reads the same past the counters' events: the turns
// to the next one are taken with the one it falls in, the event seen to, and so on until one can end the
// wait or the GSU stops. So does one only $4212's V blank bit decides (vblank), up to where that changes.
static void fastSkipTurns(u64 turn, bool hvbjoy, bool apu, u64 apuBack, bool vblank) {
	fastCatchUp();
	if (!fastSkipOnce(turn, hvbjoy, apu, apuBack) || hvbjoy || apu) {
		return;
	}
	u32 clocks = (u32) (turn / cpu.th.scalar);
	for (;;) {
		u32 h = cpu.counter.hcounter;
		u64 turns = fastNext > h ? (fastNext - h + clocks - 1) / clocks : 1;
		if (cart.gsu) {
			u64 stopped = gsuRunTo(cpu.th.clock + turns * turn);
			if (stopped != ~0ull) {
				if (stopped > cpu.th.clock) {
					fastTakeTurns((stopped - cpu.th.clock - 1) / turn, turn);
				}
				return;
			}
		}
		if (cart.sa1) {
			u64 stopped = sa1RunTo(cpu.th.clock + turns * turn, wait.sa1Read, wait.sa1Count);
			if (stopped != ~0ull) {
				if (stopped > cpu.th.clock) {
					fastTakeTurns((stopped - cpu.th.clock - 1) / turn, turn);
				}
				return;
			}
		}
		fastTakeTurns(turns, turn);
		u32 line = cpu.counter.vcounter;
		fastCatch();
		if (vblank && cpu.counter.vcounter != line && (cpu.counter.vcounter == ppu.state.vdisp || cpu.counter.vcounter == 0)) {
			return;
		}
		if (cpu.status.hdmaPending && !cpu.status.dmaPending && hdmaKeepsMemory()) {
			cpu.status.clockCount = 8;
			dmaEdgeRun();
			dmaEdge();
		}
		if (sys.frameEvent || cpu.status.irqLine || cpu.status.nmiTransition || cpu.status.irqTransition || R.irq ||
		    cpu.status.dmaPending || cpu.status.hdmaPending || cpu.status.dmaActive) {
			return;
		}
	}
}

// Fast mode: a byte of plain memory, read with no clock or side effect (-1 where there is none).
static int fastPeek(u32 address) {
	address &= 0xffffff;
	u32 at = address & 0x40e000 ? address : 0x7e0000 | (address & 0x1fff);
	const u8* memory = busFastPages[at >> 8];
	return memory ? memory[at & 0xff] : -1;
}

// A loop of BIT or LDA $4212, then BPL or BMI back to it: only the V blank bit decides it.
static bool fastVblankLoop(u32 branch) {
	int op = fastPeek(branch), read = fastPeek(branch - 3);
	return (op == 0x10 || op == 0x30) && fastPeek(branch + 1) == 0xfb && (read == 0x2c || read == 0xad) && fastPeek(branch - 2) == 0x12 &&
	       fastPeek(branch - 1) == 0x42;
}

static bool fastBranchLong(void) {
	return fastPeek(waitFrom) == 0x82;
}

static void fastBranch(void) {
	fastCatchUp();
	u32 pc = R.pc.d;
	// A short jump back. A turn that wrote, or read what can change, starts over; after a clean one
	// the registers are kept, and the same turn again with the same registers and no event inside
	// it is waiting.
	bool hvbjoy = wait.hvbjoy, apu = wait.apu;
	wait.hvbjoy = wait.apu = false;
	if (wait.unsafe) {
		wait.valid = false;
		wait.unsafe = false;
		wait.apuRead = 0;
		wait.sa1Count = 0;
		return;
	}
	const u8* regs = (const u8*) &R + WAIT_FROM;
	bool quiet = wait.catches == fastCatches;
	if (wait.valid && wait.quiet && quiet && pc == wait.pc && R.mdr == wait.mdr && !memcmp(regs, wait.regs, WAIT_SIZE) && !cpu.status.interruptPending &&
	    !cpu.status.dmaActive && !cpu.status.dmaPending && !cpu.status.hdmaPending) {
		bool vblank = hvbjoy && !apu && fastVblankLoop(waitFrom);
		fastSkipTurns(cpu.th.clock - wait.clock, hvbjoy && !vblank, apu, cpu.th.clock - wait.apuLast, vblank);
	}
	wait.valid = true;
	wait.pc = pc;
	memcpy(wait.regs, regs, WAIT_SIZE);
	wait.mdr = R.mdr;
	wait.clock = cpu.th.clock;
	wait.catches = fastCatches;
	wait.quiet = quiet;
	wait.apuRead = 0;
	wait.sa1Count = 0;
}

// Fast mode: what an access would see to, once an instruction. A DMA starts at the second access edge.
static inline __attribute__((always_inline)) void fastBetween(void) {
	cpu.status.irqLock = 0;
	if (cpu.status.dmaActive || cpu.status.dmaPending || cpu.status.hdmaPending) {
		fastSettle();
		cpu.status.clockCount = 8;
		dmaEdgeRun();
		dmaEdge();
	}
	waitFrom = R.pc.d;
}

// Fast mode: instructions one after another as cpuMain runs them, looking only at what can change how
// the next one starts (the counters' next event, an interrupt, DMA or HDMA, WAI or STP), which is
// cpuMain's to see to.
_Static_assert(offsetof(struct Cpu, status.hdmaPending) - offsetof(struct Cpu, status.interruptPending) == 3, "flags apart");
_Static_assert(offsetof(struct W65816, stp) - offsetof(struct W65816, wai) == 1, "flags apart");

void cpuRunFast(void) {
	while (!sys.frameEvent) {
		u32 starting;
		u16 halted;
		memcpy(&starting, &cpu.status.interruptPending, sizeof(starting));
		memcpy(&halted, &R.wai, sizeof(halted));
		if (starting || halted || cpu.counter.hcounter + fastPending >= fastNext) {
			cpuMain();
			continue;
		}
		fastNow = true;
		cpu.status.irqLock = 0;
		waitFrom = R.pc.d;
		instruction();
	}
	fastSettle();
}

void cpuMain(void) {
	fastNow = systemFast && !inputGuns && !cpu.status.dmaPending;
	if (fastAccess()) {
		if (cpu.counter.hcounter + fastPending >= fastNext) {
			fastCatchUp();
		}
		fastBetween();
	}
	// A waiting CPU idles until an interrupt is looked for and found, then idles once more. A stopped
	// one idles for good.
	if (R.wai) {
		lastCycle();
		idle();
		if (!R.wai) {
			idle();
		} else if (fastAccess() && !cpu.status.interruptPending && !cpu.status.dmaActive && !cpu.status.dmaPending &&
		           !cpu.status.hdmaPending) {
			// Fast mode: it idles as a loop of one 6-clock turn would, to what can end the wait.
			wait.sa1Count = 0;
			fastSkipTurns(cpu.th.scalar * 6, false, false, 0, false);
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
		fastSettle();
		step(132);
		R.vector = 0xfffc;
		interrupt();
		return;
	}

	cpu.status.interruptPending = 0;
}

void cpuPower(bool reset) {
	fastWaitsInit();
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
	fastWaitChoose();
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
