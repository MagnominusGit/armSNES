/* Copyright (c) 2004-2025 ares team, Near et al
 * Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Ported from ares (ISC); see LICENSES/ISC-ares.txt. */
#include "snes.h"

#include <pthread.h>
#include <sched.h>

// The S-SMP: an SPC700 with three timers and four ports to the CPU.
//
// The SMP runs only when the CPU needs it, and stops as soon as its clock passes the CPU's, which can
// be in the middle of an instruction. So the interpreter is one function that can leave after any
// clock step and come back to the same place: smp.state says where, and what an instruction holds
// between its cycles is in smp too, never in a local variable.

ON_OWN_LINES(struct Smp, smp);

static const u8 iplrom[64] = {
	0xcd, 0xef, 0xbd, 0xe8, 0x00, 0xc6, 0x1d, 0xd0, 0xfc, 0x8f, 0xaa, 0xf4, 0x8f, 0xbb, 0xf5, 0x78,
	0xcc, 0xf4, 0xd0, 0xfb, 0x2f, 0x19, 0xeb, 0xf4, 0xd0, 0xfc, 0x7e, 0xf4, 0xd0, 0x0b, 0xe4, 0xf5,
	0xcb, 0xf4, 0xd7, 0x00, 0xfc, 0xd0, 0xf3, 0xab, 0x01, 0x10, 0xef, 0x7e, 0xf4, 0x10, 0xeb, 0xba,
	0xf6, 0xda, 0x00, 0xba, 0xf4, 0xc4, 0xf4, 0xdd, 0x5d, 0xd0, 0xdb, 0x1f, 0x00, 0x00, 0xc0, 0xff,
};

// The clock the SMP runs up to: the CPU's, when they were last brought together.
u64 smpLimit;
// smpRunToPortWrite: whether to stop, and what it found (in Fast mode).
static bool smpStopAtPortWrite;
static struct SmpPortWrite smpPortWrite;
// Balanced and Fast: each port to the S-CPU before its last write, and that write's clock. The SMP
// runs whole instructions, so an S-CPU read can find it past a write the S-CPU shouldn't see yet.
static u8 smpPortsBefore[4];
static u64 smpPortsAt[4];
// Balanced and Fast: a write to a port the S-CPU made, while the SMP runs up to its clock. An
// instruction under way reads it once its clock is past the write's, as Exact steps them.
static struct {
	bool on;
	u64 at;
	u8 port, data;
} smpWriting;

// The S-CPU's write, once the SMP's clock is past it.
static void smpTakeWrite(void) {
	if (smpWriting.on && smpWriting.at < smp.th.clock) {
		smp.fromCpu[smpWriting.port] = smpWriting.data;
		smpWriting.on = false;
	}
}

void smpPower(bool reset) {
	struct Thread th = smp.th;
	memset(&smp, 0, sizeof(smp));
	smp.th = th;
	threadFrequency(&smp.th, sys.apuFrequency / 12.0);
	smp.s = 0xef;
	smp.z = true;
	smp.pc = iplrom[62] | iplrom[63] << 8;
	smp.ramWritable = true;
	smp.timersEnable = true;
	smp.iplromEnable = true;
	smp.state = 256;
}

static u8 getP(void) {
	return smp.c << 0 | smp.z << 1 | smp.i << 2 | smp.h << 3 | smp.b << 4 | smp.p << 5 | smp.v << 6 | smp.n << 7;
}

static void setP(u8 data) {
	smp.c = data & 1;
	smp.z = data & 2;
	smp.i = data & 4;
	smp.h = data & 8;
	smp.b = data & 16;
	smp.p = data & 32;
	smp.v = data & 64;
	smp.n = data & 128;
}

// Timers

static void timerSynchronizeStage1(struct SmpTimer* timer) {
	bool level = timer->stage1;
	if (!smp.timersEnable) {
		level = false;
	}
	if (smp.timersDisable) {
		level = false;
	}
	// Only a 1 -> 0 transition pulses.
	if (!lowerTo(&timer->line, level)) {
		return;
	}
	if (!timer->enable) {
		return;
	}
	if (++timer->stage2 != timer->target) {
		return;
	}
	timer->stage2 = 0;
	timer->stage3 = (timer->stage3 + 1) & 15;
}

static void timerStep(struct SmpTimer* timer, u32 frequency, u32 clocks) {
	timer->stage0 += clocks;
	if (timer->stage0 < frequency) {
		return;
	}
	timer->stage0 -= frequency;
	timer->stage1 ^= 1;
	timerSynchronizeStage1(timer);
}

static void stepTimers(u32 clocks) {
	timerStep(&smp.timer[0], 128, clocks);
	timerStep(&smp.timer[1], 128, clocks);
	timerStep(&smp.timer[2], 16, clocks);
}

// Balanced mode: the timers' clocks since they were brought up to date. Only the I/O registers see
// them, so they are brought up to date before an I/O access and as the SMP stops; as no step is
// longer than a timer's period, the same pulses come out as from stepping them.
static u32 timersPending;

static void timersCatchUp(void) {
	static const u32 frequency[3] = { 128, 128, 16 };
	for (int n = 0; n < 3; ++n) {
		struct SmpTimer* timer = &smp.timer[n];
		u32 stage0 = timer->stage0 + timersPending;
		while (stage0 >= frequency[n]) {
			stage0 -= frequency[n];
			timer->stage1 ^= 1;
			timerSynchronizeStage1(timer);
		}
		timer->stage0 = stage0;
	}
	timersPending = 0;
}

// Fast mode: a sound driver waiting for a timer by reading its counter until it isn't 0 (MOV A/X/Y from
// $fd-$ff, then BEQ back to it). Its turns before the counter can change are skipped, their clocks and
// the timers' given at once (spcWaitSkip); a turn is measured first, and the turn that finds the count is
// run.
static struct {
	u16 pc;
	u8 timer, length;
	bool measured;
	u64 lastRead;
} spcWait = { .pc = 0xffff };

// A turn's clocks: the MOV's fetches and the BEQ's (external), the counter's read and the branch's two
// idle cycles (internal); beforeRead, the MOV's part up to its read.
static void spcWaitCost(u32 length, const u8* cycles, u32* turn, u32* beforeRead) {
	u32 external = cycles[smp.externalWaitStates], internal = cycles[smp.internalWaitStates];
	*turn = (length + 2) * external + 3 * internal;
	*beforeRead = length * external + internal;
}

static const u8 cycleWaitStates[4];
static const u8 timerWaitStates[4];

// At a read of a counter that found 0: whether it is such a loop's, and whether its turn takes the clocks
// expected.
static void spcWaitSeen(u16 address) {
	u16 pc = smp.pc;
	const u8* ram = dsp.apuram;
	u32 length = 0;
	if ((ram[(u16) (pc - 3)] == 0xe5 || ram[(u16) (pc - 3)] == 0xe9 || ram[(u16) (pc - 3)] == 0xec) &&
	    ram[(u16) (pc - 2)] == address && ram[(u16) (pc - 1)] == 0x00 && ram[pc] == 0xf0 && ram[(u16) (pc + 1)] == 0xfb) {
		length = 3;
	} else if ((ram[(u16) (pc - 2)] == 0xe4 || ram[(u16) (pc - 2)] == 0xf8 || ram[(u16) (pc - 2)] == 0xeb) && !smp.p &&
	           ram[(u16) (pc - 1)] == address && ram[pc] == 0xf0 && ram[(u16) (pc + 1)] == 0xfc) {
		length = 2;
	}
	u16 start = pc - length;
	if (!length || start < 0x0100 || (u16) (pc + 2) >= 0xffc0 || smp.ramDisable) {
		spcWait.pc = 0xffff;
		return;
	}
	if (spcWait.pc != start || spcWait.timer != address - 0xfd) {
		spcWait.pc = start;
		spcWait.timer = address - 0xfd;
		spcWait.length = length;
		spcWait.measured = false;
	} else {
		u32 turn, beforeRead;
		spcWaitCost(length, cycleWaitStates, &turn, &beforeRead);
		spcWait.measured = smp.th.clock - spcWait.lastRead == smp.th.scalar * turn;
	}
	spcWait.lastRead = smp.th.clock;
}

// Memory

static u8 readRAM(u16 address) {
	if (address >= 0xffc0 && smp.iplromEnable) {
		return iplrom[address & 0x3f];
	}
	if (smp.ramDisable) {
		return 0x5a;
	}
	return dsp.apuram[address];
}

static void writeRAM(u16 address, u8 data) {
	// Writes to $ffc0-$ffff reach the RAM even with the IPL ROM enabled.
	if (smp.ramWritable && !smp.ramDisable) {
		dsp.apuram[address] = data;
	}
}

static u8 readIO(u16 address) {
	u8 data = 0;
	switch (address) {
	case 0xf0:
	case 0xf1:
		return 0x00;
	case 0xf2:
		return smp.dspAddress;
	case 0xf3:
		if (systemBalanced && dsp.th.clock < smp.th.clock) {
			dspRun();
		}
		return dspRead(smp.dspAddress & 0x7f);
	case 0xf4:
	case 0xf5:
	case 0xf6:
	case 0xf7:
		smpTakeWrite();
		return smp.fromCpu[address & 3];
	case 0xf8:
		return smp.aux4;
	case 0xf9:
		return smp.aux5;
	case 0xfa:
	case 0xfb:
	case 0xfc:
		return 0x00;
	case 0xfd:
	case 0xfe:
	case 0xff:
		data = smp.timer[address - 0xfd].stage3;
		smp.timer[address - 0xfd].stage3 = 0;
		if (systemFast && !data) {
			spcWaitSeen(address);
		}
		return data;
	}
	return data;
}

static void writeIO(u16 address, u8 data) {
	switch (address) {
	case 0xf0:
		if (smp.p) {
			break;
		}
		smp.timersDisable = data & 1;
		smp.ramWritable = data & 2;
		smp.ramDisable = data & 4;
		smp.timersEnable = data & 8;
		smp.externalWaitStates = data >> 4 & 3;
		smp.internalWaitStates = data >> 6 & 3;
		timerSynchronizeStage1(&smp.timer[0]);
		timerSynchronizeStage1(&smp.timer[1]);
		timerSynchronizeStage1(&smp.timer[2]);
		break;
	case 0xf1:
		if (raiseTo(&smp.timer[0].enable, data & 1)) {
			smp.timer[0].stage2 = 0;
			smp.timer[0].stage3 = 0;
		}
		if (raiseTo(&smp.timer[1].enable, data & 2)) {
			smp.timer[1].stage2 = 0;
			smp.timer[1].stage3 = 0;
		}
		// As in ares: timer 2 is cleared unless it was just enabled.
		if (!raiseTo(&smp.timer[2].enable, data & 4)) {
			smp.timer[2].stage2 = 0;
			smp.timer[2].stage3 = 0;
		}
		// A write of the S-CPU's from before is cleared with the rest.
		smpTakeWrite();
		if (data & 0x10) {
			smp.fromCpu[0] = 0x00;
			smp.fromCpu[1] = 0x00;
		}
		if (data & 0x20) {
			smp.fromCpu[2] = 0x00;
			smp.fromCpu[3] = 0x00;
		}
		smp.iplromEnable = data & 0x80;
		break;
	case 0xf2:
		smp.dspAddress = data;
		break;
	case 0xf3:
		if (smp.dspAddress & 0x80) {
			break;
		}
		if (systemBalanced && dsp.th.clock < smp.th.clock) {
			dspRun();
		}
		dspWrite(smp.dspAddress, data);
		break;
	case 0xf4:
	case 0xf5:
	case 0xf6:
	case 0xf7:
		smpPortsBefore[address & 3] = smp.toCpu[address & 3];
		smpPortsAt[address & 3] = smp.th.clock;
		smp.toCpu[address & 3] = data;
		if (smpStopAtPortWrite && smpPortWrite.at == ~0ull) {
			smpPortWrite.at = smp.th.clock;
			smpLimit = 0;
		}
		break;
	case 0xf8:
		smp.aux4 = data;
		break;
	case 0xf9:
		smp.aux5 = data;
		break;
	case 0xfa:
	case 0xfb:
	case 0xfc:
		smp.timer[address - 0xfa].target = data;
		break;
	}
}

static u8 readMemory(u16 address) {
	u8 data = readRAM(address);
	if ((address & 0xfff0) == 0x00f0) {
		data = readIO(address);
	}
	return data;
}

static void writeMemory(u16 address, u8 data) {
	// Even I/O writes reach the RAM under them.
	writeRAM(address, data);
	if ((address & 0xfff0) == 0x00f0) {
		writeIO(address, data);
	}
}

// The clock divider the cycle for this address uses. Dividers of 8 and 16 are glitchy on hardware: the
// SMP takes 10 and 20 clocks, while the timers advance by the expected amount.
static const u8 cycleWaitStates[4] = { 2, 4, 10, 20 };
static const u8 timerWaitStates[4] = { 2, 4, 8, 16 };

static u8 waitStatesFor(u16 address) {
	if ((address & 0xfff0) == 0x00f0) {
		return smp.internalWaitStates;
	}
	if (address >= 0xffc0 && smp.iplromEnable) {
		return smp.internalWaitStates;
	}
	return smp.externalWaitStates;
}

// Algorithms

static u8 algADC(u8 x, u8 y) {
	s32 z = x + y + smp.c;
	smp.c = z > 0xff;
	smp.z = (u8) z == 0;
	smp.h = (x ^ y ^ z) & 0x10;
	smp.v = ~(x ^ y) & (x ^ z) & 0x80;
	smp.n = z & 0x80;
	return z;
}
static u8 algAND(u8 x, u8 y) {
	x &= y;
	smp.z = x == 0;
	smp.n = x & 0x80;
	return x;
}
static u8 algASL(u8 x) {
	smp.c = x & 0x80;
	x <<= 1;
	smp.z = x == 0;
	smp.n = x & 0x80;
	return x;
}
static u8 algCMP(u8 x, u8 y) {
	s32 z = x - y;
	smp.c = z >= 0;
	smp.z = (u8) z == 0;
	smp.n = z & 0x80;
	return x;
}
static u8 algDEC(u8 x) {
	x--;
	smp.z = x == 0;
	smp.n = x & 0x80;
	return x;
}
static u8 algEOR(u8 x, u8 y) {
	x ^= y;
	smp.z = x == 0;
	smp.n = x & 0x80;
	return x;
}
static u8 algINC(u8 x) {
	x++;
	smp.z = x == 0;
	smp.n = x & 0x80;
	return x;
}
static u8 algLD(u8 x, u8 y) {
	(void) x;
	smp.z = y == 0;
	smp.n = y & 0x80;
	return y;
}
static u8 algLSR(u8 x) {
	smp.c = x & 0x01;
	x >>= 1;
	smp.z = x == 0;
	smp.n = x & 0x80;
	return x;
}
static u8 algOR(u8 x, u8 y) {
	x |= y;
	smp.z = x == 0;
	smp.n = x & 0x80;
	return x;
}
static u8 algROL(u8 x) {
	bool carry = smp.c;
	smp.c = x & 0x80;
	x = x << 1 | carry;
	smp.z = x == 0;
	smp.n = x & 0x80;
	return x;
}
static u8 algROR(u8 x) {
	bool carry = smp.c;
	smp.c = x & 0x01;
	x = carry << 7 | x >> 1;
	smp.z = x == 0;
	smp.n = x & 0x80;
	return x;
}
static u8 algSBC(u8 x, u8 y) {
	return algADC(x, ~y);
}
static u16 algADW(u16 x, u16 y) {
	u16 z;
	smp.c = 0;
	z = algADC(x, y);
	z |= algADC(x >> 8, y >> 8) << 8;
	smp.z = z == 0;
	return z;
}
static u16 algCPW(u16 x, u16 y) {
	s32 z = x - y;
	smp.c = z >= 0;
	smp.z = (u16) z == 0;
	smp.n = z & 0x8000;
	return x;
}
static u16 algLDW(u16 x, u16 y) {
	(void) x;
	smp.z = y == 0;
	smp.n = y & 0x8000;
	return y;
}
static u16 algSBW(u16 x, u16 y) {
	u16 z;
	smp.c = 1;
	z = algSBC(x, y);
	z |= algSBC(x >> 8, y >> 8) << 8;
	smp.z = z == 0;
	return z;
}

// The interpreter. States 0-255 start that opcode, MAIN fetches one, and each place the SMP can stop
// has a state of its own above that.

enum { MAIN = 256, FIRST_STOP = 257 };

#define STOP_(n) \
	do { \
		smp.state = FIRST_STOP + (n); \
		return; \
	case FIRST_STOP + (n):; \
	} while (0)
#define STOP_X(n) STOP_(n)
#define STOP() STOP_X(__COUNTER__)

// A clock step: the SMP leaves if that put it ahead of the CPU, and brings the DSP up to itself.
#define STEP_EXACT(clocks) \
	do { \
		threadStep(&smp.th, (clocks)); \
		if (cpu.th.clock < smp.th.clock) { \
			STOP(); \
		} \
		if (dsp.th.clock < smp.th.clock) { \
			dspRun(); \
		} \
	} while (0)
#define WAIT(halve, states) \
	do { \
		smp.pend = (states); \
		STEP(cycleWaitStates[smp.pend] >> (halve)); \
		TIMERS(timerWaitStates[smp.pend] >> (halve)); \
	} while (0)

#define IDLE() WAIT(0, smp.internalWaitStates)
// Reads of $f4-$f7 take their time in two halves, around the read.
#define READ(target, address) \
	do { \
		smp.ad = (address); \
		if ((smp.ad & 0xfffc) == 0x00f4) { \
			WAIT(1, waitStatesFor(smp.ad)); \
			smp.dr = readMemory(smp.ad); \
			WAIT(1, waitStatesFor(smp.ad)); \
		} else { \
			WAIT(0, waitStatesFor(smp.ad)); \
			SMP_BEFORE_READ(smp.ad); \
			smp.dr = readMemory(smp.ad); \
		} \
		target = smp.dr; \
	} while (0)
#define WRITE(address, data) \
	do { \
		smp.ad = (address); \
		smp.dw = (data); \
		WAIT(0, waitStatesFor(smp.ad)); \
		SMP_BEFORE_WRITE(); \
		writeMemory(smp.ad, smp.dw); \
	} while (0)
#define SKIP(address) READ(smp.dr, address)
#define FETCH(target) READ(target, smp.pc++)
#define LOAD(target, address) READ(target, smp.p << 8 | (u8) (address))
#define STORE(address, data) WRITE(smp.p << 8 | (u8) (address), data)
#define PULL(target) READ(target, 0x100 | ++smp.s)
#define PUSH(data) WRITE(0x100 | smp.s--, data)

#define PC smp.pc
#define A smp.a
#define X smp.x
#define Y smp.y
#define S smp.s
#define P getP()
#define YA (smp.y << 8 | smp.a)
#define SET_YA(value) \
	do { \
		u16 ya = (value); \
		smp.a = ya; \
		smp.y = ya >> 8; \
	} while (0)
#define CF smp.c
#define ZF smp.z
#define IF smp.i
#define HF smp.h
#define BF smp.b
#define PF smp.p
#define VF smp.v
#define NF smp.n
// An instruction's own values.
#define AD smp.ta
#define W smp.tb
#define D smp.tc
#define E smp.td
#define FETCH16() \
	do { \
		FETCH(AD); \
		FETCH(E); \
		AD |= E << 8; \
	} while (0)
#define BRANCH(displacement) \
	do { \
		IDLE(); \
		IDLE(); \
		PC += (s8) (displacement); \
	} while (0)

#define I_AbsoluteBitModify(mode) \
	FETCH16(); \
	W = AD >> 13; \
	AD &= 0x1fff; \
	READ(D, AD); \
	if (mode == 0) { \
		IDLE(); \
		CF |= D >> W & 1; \
	} else if (mode == 1) { \
		IDLE(); \
		CF |= !(D >> W & 1); \
	} else if (mode == 2) { \
		CF &= D >> W & 1; \
	} else if (mode == 3) { \
		CF &= !(D >> W & 1); \
	} else if (mode == 4) { \
		IDLE(); \
		CF ^= D >> W & 1; \
	} else if (mode == 5) { \
		CF = D >> W & 1; \
	} else if (mode == 6) { \
		IDLE(); \
		D = (D & ~(1 << W)) | CF << W; \
		WRITE(AD, D); \
	} else { \
		D ^= 1 << W; \
		WRITE(AD, D); \
	}
#define I_AbsoluteBitSet(bit, value) \
	FETCH(AD); \
	LOAD(D, AD); \
	D = (D & ~(1 << bit)) | value << bit; \
	STORE(AD, D);
#define I_AbsoluteRead(alu, target) \
	FETCH16(); \
	READ(D, AD); \
	target = alu(target, D);
#define I_AbsoluteModify(alu) \
	FETCH16(); \
	READ(D, AD); \
	WRITE(AD, alu(D));
#define I_AbsoluteWrite(data) \
	FETCH16(); \
	SKIP(AD); \
	WRITE(AD, data);
#define I_AbsoluteIndexedRead(alu, index) \
	FETCH16(); \
	IDLE(); \
	READ(D, AD + index); \
	A = alu(A, D);
#define I_AbsoluteIndexedWrite(index) \
	FETCH16(); \
	IDLE(); \
	SKIP(AD + index); \
	WRITE(AD + index, A);
#define I_Branch(take) \
	FETCH(D); \
	if (!(take)) { \
		goto done; \
	} \
	BRANCH(D);
#define I_BranchBit(bit, match) \
	FETCH(AD); \
	LOAD(D, AD); \
	IDLE(); \
	FETCH(E); \
	if ((D >> bit & 1) != match) { \
		goto done; \
	} \
	BRANCH(E);
#define I_BranchNotDirect() \
	FETCH(AD); \
	LOAD(D, AD); \
	IDLE(); \
	FETCH(E); \
	if (A == D) { \
		goto done; \
	} \
	BRANCH(E);
#define I_BranchNotDirectDecrement() \
	FETCH(AD); \
	LOAD(D, AD); \
	STORE(AD, --D); \
	FETCH(E); \
	if (D == 0) { \
		goto done; \
	} \
	BRANCH(E);
#define I_BranchNotDirectIndexed(index) \
	FETCH(AD); \
	IDLE(); \
	LOAD(D, AD + index); \
	IDLE(); \
	FETCH(E); \
	if (A == D) { \
		goto done; \
	} \
	BRANCH(E);
#define I_BranchNotYDecrement() \
	SKIP(PC); \
	IDLE(); \
	FETCH(E); \
	if (--Y == 0) { \
		goto done; \
	} \
	BRANCH(E);
#define I_Break() \
	SKIP(PC); \
	PUSH(PC >> 8); \
	PUSH(PC >> 0); \
	PUSH(P); \
	IDLE(); \
	READ(AD, 0xffde + 0); \
	READ(E, 0xffde + 1); \
	PC = AD | E << 8; \
	IF = 0; \
	BF = 1;
#define I_CallAbsolute() \
	FETCH16(); \
	IDLE(); \
	PUSH(PC >> 8); \
	PUSH(PC >> 0); \
	IDLE(); \
	IDLE(); \
	PC = AD;
#define I_CallPage() \
	FETCH(AD); \
	IDLE(); \
	PUSH(PC >> 8); \
	PUSH(PC >> 0); \
	IDLE(); \
	PC = 0xff00 | AD;
#define I_CallTable(vector) \
	SKIP(PC); \
	IDLE(); \
	PUSH(PC >> 8); \
	PUSH(PC >> 0); \
	IDLE(); \
	W = 0xffde - (vector << 1); \
	READ(AD, W + 0); \
	READ(E, W + 1); \
	PC = AD | E << 8;
#define I_ComplementCarry() \
	SKIP(PC); \
	IDLE(); \
	CF = !CF;
#define I_DecimalAdjustAdd() \
	SKIP(PC); \
	IDLE(); \
	if (CF || A > 0x99) { \
		A += 0x60; \
		CF = 1; \
	} \
	if (HF || (A & 15) > 0x09) { \
		A += 0x06; \
	} \
	ZF = A == 0; \
	NF = A & 0x80;
#define I_DecimalAdjustSub() \
	SKIP(PC); \
	IDLE(); \
	if (!CF || A > 0x99) { \
		A -= 0x60; \
		CF = 0; \
	} \
	if (!HF || (A & 15) > 0x09) { \
		A -= 0x06; \
	} \
	ZF = A == 0; \
	NF = A & 0x80;
#define I_DirectRead(alu, target) \
	FETCH(AD); \
	LOAD(D, AD); \
	target = alu(target, D);
#define I_DirectModify(alu) \
	FETCH(AD); \
	LOAD(D, AD); \
	STORE(AD, alu(D));
#define I_DirectWrite(data) \
	FETCH(AD); \
	LOAD(D, AD); \
	STORE(AD, data);
#define I_DirectDirectCompare(alu) \
	FETCH(AD); \
	LOAD(E, AD); \
	FETCH(AD); \
	LOAD(D, AD); \
	D = alu(D, E); \
	IDLE();
#define I_DirectDirectModify(alu) \
	FETCH(AD); \
	LOAD(E, AD); \
	FETCH(AD); \
	LOAD(D, AD); \
	D = alu(D, E); \
	STORE(AD, D);
#define I_DirectDirectWrite() \
	FETCH(AD); \
	LOAD(D, AD); \
	FETCH(AD); \
	STORE(AD, D);
#define I_DirectImmediateCompare(alu) \
	FETCH(E); \
	FETCH(AD); \
	LOAD(D, AD); \
	D = alu(D, E); \
	IDLE();
#define I_DirectImmediateModify(alu) \
	FETCH(E); \
	FETCH(AD); \
	LOAD(D, AD); \
	D = alu(D, E); \
	STORE(AD, D);
#define I_DirectImmediateWrite() \
	FETCH(E); \
	FETCH(AD); \
	LOAD(D, AD); \
	STORE(AD, E);
#define I_DirectCompareWord(alu) \
	FETCH(AD); \
	LOAD(W, AD + 0); \
	LOAD(E, AD + 1); \
	W |= E << 8; \
	SET_YA(alu(YA, W));
#define I_DirectReadWord(alu) \
	FETCH(AD); \
	LOAD(W, AD + 0); \
	IDLE(); \
	LOAD(E, AD + 1); \
	W |= E << 8; \
	SET_YA(alu(YA, W));
#define I_DirectModifyWord(adjust) \
	FETCH(AD); \
	LOAD(W, AD + 0); \
	W += adjust; \
	STORE(AD + 0, W >> 0); \
	LOAD(E, AD + 1); \
	W += E << 8; \
	STORE(AD + 1, W >> 8); \
	ZF = W == 0; \
	NF = W & 0x8000;
#define I_DirectWriteWord() \
	FETCH(AD); \
	LOAD(D, AD + 0); \
	STORE(AD + 0, A); \
	STORE(AD + 1, Y);
#define I_DirectIndexedRead(alu, target, index) \
	FETCH(AD); \
	IDLE(); \
	LOAD(D, AD + index); \
	target = alu(target, D);
#define I_DirectIndexedModify(alu, index) \
	FETCH(AD); \
	IDLE(); \
	LOAD(D, AD + index); \
	STORE(AD + index, alu(D));
#define I_DirectIndexedWrite(data, index) \
	FETCH(AD); \
	IDLE(); \
	LOAD(D, AD + index); \
	STORE(AD + index, data);
#define I_Divide() \
	SKIP(PC); \
	IDLE(); \
	IDLE(); \
	IDLE(); \
	IDLE(); \
	IDLE(); \
	IDLE(); \
	IDLE(); \
	IDLE(); \
	IDLE(); \
	IDLE(); \
	divide();
#define I_ExchangeNibble() \
	SKIP(PC); \
	IDLE(); \
	IDLE(); \
	IDLE(); \
	A = A >> 4 | A << 4; \
	ZF = A == 0; \
	NF = A & 0x80;
#define I_FlagSet(flag, value) \
	SKIP(PC); \
	if (&flag == &IF) { \
		IDLE(); \
	} \
	flag = value;
#define I_ImmediateRead(alu, target) \
	FETCH(D); \
	target = alu(target, D);
#define I_ImpliedModify(alu, target) \
	SKIP(PC); \
	target = alu(target);
#define I_IndexedIndirectRead(alu, index) \
	FETCH(W); \
	IDLE(); \
	LOAD(AD, W + index + 0); \
	LOAD(E, W + index + 1); \
	AD |= E << 8; \
	READ(D, AD); \
	A = alu(A, D);
#define I_IndexedIndirectWrite(data, index) \
	FETCH(W); \
	IDLE(); \
	LOAD(AD, W + index + 0); \
	LOAD(E, W + index + 1); \
	AD |= E << 8; \
	SKIP(AD); \
	WRITE(AD, data);
#define I_IndirectIndexedRead(alu, index) \
	FETCH(W); \
	IDLE(); \
	LOAD(AD, W + 0); \
	LOAD(E, W + 1); \
	AD |= E << 8; \
	READ(D, AD + index); \
	A = alu(A, D);
#define I_IndirectIndexedWrite(data, index) \
	FETCH(W); \
	LOAD(AD, W + 0); \
	LOAD(E, W + 1); \
	AD |= E << 8; \
	IDLE(); \
	SKIP(AD + index); \
	WRITE(AD + index, data);
#define I_IndirectXRead(alu) \
	SKIP(PC); \
	LOAD(D, X); \
	A = alu(A, D);
#define I_IndirectXWrite(data) \
	SKIP(PC); \
	LOAD(D, X); \
	STORE(X, data);
#define I_IndirectXIncrementRead(data) \
	SKIP(PC); \
	LOAD(data, X++); \
	IDLE(); \
	ZF = data == 0; \
	NF = data & 0x80;
#define I_IndirectXIncrementWrite(data) \
	SKIP(PC); \
	IDLE(); \
	STORE(X++, data);
#define I_IndirectXCompareIndirectY(alu) \
	SKIP(PC); \
	LOAD(E, Y); \
	LOAD(D, X); \
	D = alu(D, E); \
	IDLE();
#define I_IndirectXWriteIndirectY(alu) \
	SKIP(PC); \
	LOAD(E, Y); \
	LOAD(D, X); \
	D = alu(D, E); \
	STORE(X, D);
#define I_JumpAbsolute() \
	FETCH16(); \
	PC = AD;
#define I_JumpIndirectX() \
	FETCH16(); \
	IDLE(); \
	READ(W, AD + X + 0); \
	READ(E, AD + X + 1); \
	PC = W | E << 8;
#define I_Multiply() \
	SKIP(PC); \
	IDLE(); \
	IDLE(); \
	IDLE(); \
	IDLE(); \
	IDLE(); \
	IDLE(); \
	IDLE(); \
	SET_YA(Y * A); \
	ZF = Y == 0; \
	NF = Y & 0x80;
#define I_NoOperation() SKIP(PC);
#define I_OverflowClear() \
	SKIP(PC); \
	HF = 0; \
	VF = 0;
#define I_Pull(data) \
	SKIP(PC); \
	IDLE(); \
	PULL(data);
#define I_PullP() \
	SKIP(PC); \
	IDLE(); \
	PULL(D); \
	setP(D);
#define I_Push(data) \
	SKIP(PC); \
	PUSH(data); \
	IDLE();
#define I_ReturnInterrupt() \
	SKIP(PC); \
	IDLE(); \
	PULL(D); \
	setP(D); \
	PULL(AD); \
	PULL(E); \
	PC = AD | E << 8;
#define I_ReturnSubroutine() \
	SKIP(PC); \
	IDLE(); \
	PULL(AD); \
	PULL(E); \
	PC = AD | E << 8;
#define I_Stop() smp.stop = true;
#define I_TestSetBitsAbsolute(set) \
	FETCH16(); \
	READ(D, AD); \
	ZF = (u8) (A - D) == 0; \
	NF = (A - D) & 0x80; \
	SKIP(AD); \
	WRITE(AD, set ? D | A : D & ~A);
#define I_Transfer(from, to) \
	SKIP(PC); \
	to = from; \
	if (&to != &S) { \
		ZF = to == 0; \
		NF = to & 0x80; \
	}
#define I_Wait() smp.wait = true;

static void divide(void) {
	u16 ya = YA;
	// The overflow flag is set if the quotient is 256 or more.
	HF = (Y & 15) >= (X & 15);
	VF = Y >= X;
	if (Y < (X << 1)) {
		// The quotient fits in nine bits.
		A = ya / X;
		Y = ya % X;
	} else {
		// It doesn't: what the S-SMP does then.
		A = 255 - (ya - (X << 9)) / (256 - X);
		Y = X + (ya - (X << 9)) % (256 - X);
	}
	ZF = A == 0;
	NF = A & 0x80;
}

#define op(id, name, ...) \
	case id: \
		I_##name(__VA_ARGS__) \
		goto done;
#define fp(name) alg##name

// Exact: the SMP can stop at any clock step, and brings the DSP up to itself at each. In Balanced
// mode it finishes the instruction under way and leaves (to the Balanced run loop).
#define STEP(clocks) STEP_EXACT(clocks)
#define TIMERS(clocks) stepTimers(clocks)
#define SMP_AT_INSTRUCTION() \
	if (systemBalanced) { \
		return; \
	}
#define SMP_BEFORE_READ(address)
#define SMP_BEFORE_WRITE()
#define SMP_RUN smpRunExact
#include "smp_run.inc"
#undef STEP
#undef TIMERS
#undef SMP_AT_INSTRUCTION
#undef SMP_BEFORE_READ
#undef SMP_BEFORE_WRITE
#undef SMP_RUN

// Whether the DSP's echo writes reach this address now or once its echo page and size take effect.
static bool echoWritten(u16 address) {
	if (dsp.echo.readonly && dsp.echo._readonly) {
		return false;
	}
	u16 now = address - (u16) (dsp.echo._page << 8), next = address - (u16) (dsp.echo.page << 8);
	return now < (dsp.echo._length > 4 ? dsp.echo._length : 4) || next < (dsp.echo.delay ? dsp.echo.delay * 2048 : 4);
}

// The loop spcWaitSeen found, at its first instruction: the turns whose reads still find 0 are skipped,
// up to the clock the SMP runs to, which no skipped turn passes. The DSP isn't brought up to the SMP in
// those turns unless one of their reads is of its echo's RAM, which isn't skipped.
static void spcWaitSkip(void) {
	const u8* ram = dsp.apuram;
	u16 pc = spcWait.pc;
	u32 length = spcWait.length;
	u8 op = ram[pc], counter = 0xfd + spcWait.timer;
	bool abs = op == 0xe5 || op == 0xe9 || op == 0xec, dp = op == 0xe4 || op == 0xf8 || op == 0xeb;
	if (!spcWait.measured || !(length == 3 ? abs && ram[(u16) (pc + 2)] == 0x00 : dp && !smp.p) || ram[(u16) (pc + 1)] != counter ||
	    ram[(u16) (pc + length)] != 0xf0 || ram[(u16) (pc + length + 1)] != (u8) -(length + 2) || smp.timer[spcWait.timer].stage3) {
		return;
	}
	u32 turn, beforeRead, timerTurn, timerBeforeRead;
	spcWaitCost(length, cycleWaitStates, &turn, &beforeRead);
	spcWaitCost(length, timerWaitStates, &timerTurn, &timerBeforeRead);
	// Only straight after one of the loop's turns, which leaves the registers as the skipped ones would.
	u64 clocks = smp.th.scalar * turn;
	if (smp.th.clock - spcWait.lastRead != smp.th.scalar * (turn - beforeRead) || smpLimit < smp.th.clock) {
		return;
	}
	for (u32 n = 0; n < length + 2; ++n) {
		if (dspEchoPage((u16) (pc + n)) && echoWritten((u16) (pc + n))) {
			return;
		}
	}
	if (dspEchoPage(counter) && echoWritten(counter)) {
		return;
	}
	u64 most = (smpLimit - smp.th.clock) / clocks;
	if (most > 4096) {
		most = 4096;
	}
	// The counter's timer, a copy, caught up read by read as the turns would.
	static const u32 frequency[3] = { 128, 128, 16 };
	struct SmpTimer timer = smp.timer[spcWait.timer];
	u32 pending = timersPending + timerBeforeRead;
	u64 skip = 0;
	for (;;) {
		timer.stage0 += pending;
		while (timer.stage0 >= frequency[spcWait.timer]) {
			timer.stage0 -= frequency[spcWait.timer];
			timer.stage1 ^= 1;
			timerSynchronizeStage1(&timer);
		}
		if (timer.stage3 || skip == most) {
			break;
		}
		skip++;
		pending = timerTurn;
	}
	smp.th.clock += skip * clocks;
	timersPending += (u32) skip * timerTurn;
	spcWait.lastRead += skip * clocks;
}

// Balanced mode: the SMP stops only between instructions once it is ahead of the CPU, and the DSP is
// brought up to it only before a read or write of its registers, a write to RAM it may soon read or
// its echo writes to, a read of the RAM its echo writes to, and as the SMP stops.
#define STEP(clocks) threadStep(&smp.th, (clocks))
#define TIMERS(clocks) (timersPending += (clocks))
#define SMP_BEFORE_READ(address) \
	if ((address & 0xfff0) == 0x00f0) { \
		timersCatchUp(); \
	} \
	if (dsp.th.clock < smp.th.clock && dspEchoPage(address) && echoWritten(address)) { \
		dspRun(); \
	}
#define SMP_BEFORE_WRITE() \
	if ((smp.ad & 0xfff0) == 0x00f0) { \
		timersCatchUp(); \
	} \
	if (dsp.th.clock < smp.th.clock && (dspWillRead(smp.ad) || (dspEchoPage(smp.ad) && echoWritten(smp.ad)))) { \
		dspRun(); \
	}
#define SMP_AT_INSTRUCTION() \
	if (smpLimit < smp.th.clock) { \
		timersCatchUp(); \
		if (dsp.th.clock < smp.th.clock) { \
			dspRun(); \
		} \
		return; \
	} \
	if (smp.pc == spcWait.pc) { \
		spcWaitSkip(); \
	}
// Plain RAM (not the I/O registers or the IPL ROM's place) is read and written straight.
#define PLAIN(address) ((u16) ((address) - 0x00f0) >= 0x10 && (address) < 0xffc0)
#undef READ
#define READ(target, address) \
	do { \
		smp.ad = (address); \
		if (PLAIN(smp.ad)) { \
			WAIT(0, smp.externalWaitStates); \
			SMP_BEFORE_READ(smp.ad); \
			smp.dr = smp.ramDisable ? 0x5a : dsp.apuram[smp.ad]; \
		} else if ((smp.ad & 0xfffc) == 0x00f4) { \
			WAIT(1, waitStatesFor(smp.ad)); \
			smp.dr = readMemory(smp.ad); \
			WAIT(1, waitStatesFor(smp.ad)); \
		} else { \
			WAIT(0, waitStatesFor(smp.ad)); \
			SMP_BEFORE_READ(smp.ad); \
			smp.dr = readMemory(smp.ad); \
		} \
		target = smp.dr; \
	} while (0)
#undef WRITE
#define WRITE(address, data) \
	do { \
		smp.ad = (address); \
		smp.dw = (data); \
		if (PLAIN(smp.ad)) { \
			WAIT(0, smp.externalWaitStates); \
			SMP_BEFORE_WRITE(); \
			if (smp.ramWritable && !smp.ramDisable) { \
				dsp.apuram[smp.ad] = smp.dw; \
			} \
		} else { \
			WAIT(0, waitStatesFor(smp.ad)); \
			SMP_BEFORE_WRITE(); \
			writeMemory(smp.ad, smp.dw); \
		} \
	} while (0)
#define SMP_RUN smpRunBalanced
#define SMP_WHOLE_INSTRUCTIONS
#include "smp_run.inc"
#undef SMP_WHOLE_INSTRUCTIONS
#undef STEP
#undef TIMERS
#undef SMP_AT_INSTRUCTION
#undef SMP_BEFORE_READ
#undef SMP_BEFORE_WRITE
#undef SMP_RUN

void smpRun(void) {
	if (!systemBalanced) {
		smpRunExact();
		return;
	}
	// An instruction Exact stopped in is finished as Exact does it.
	if (smp.state != MAIN) {
		smpRunExact();
		if (smp.state != MAIN) {
			return;
		}
	}
	smpRunBalanced();
}

bool smpCanThread(void) {
	return smp.state == MAIN;
}

// Fast mode: the SMP and the DSP on a thread of their own, fed in order through a ring of commands from
// the CPU's thread: run up to a clock, then perhaps take a write to a port. It stops at the same clocks
// as on the CPU's thread, so it does the same.
enum { APU_RING = 1 << 12 };
static struct ApuCommand {
	u64 clock;
	bool write, toPortWrite;
	u8 port, data;
	u32 gameBoyWrite;
	bool settle;  // smpPortsSettle, in its place among the commands
	bool kept;    // smpClocksKept
} apuRing[APU_RING];
_Thread_local bool apuOnThread;
u32 apuGameBoyWrite;
// On cache lines of their own, as each is written by one thread and read by the other.
static _Alignas(64) u32 apuHead;  // written by the CPU's thread
static _Alignas(64) u32 apuTail;  // by the APU's
static u64 apuReached;  // the clock of the last command the APU's thread has run
static pthread_t apuThread;
static pthread_mutex_t apuLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t apuWake = PTHREAD_COND_INITIALIZER;
static bool apuStarted, apuStopping;
static _Alignas(64) bool apuSleeping;
bool smpThreaded, smpThreadable;

u8 smpPortRead(u32 port, u64 clock) {
	return smpPortsAt[port] > clock ? smpPortsBefore[port] : smp.toCpu[port];
}

bool smpPortsPending(u64 clock) {
	return smpPortsAt[0] > clock || smpPortsAt[1] > clock || smpPortsAt[2] > clock || smpPortsAt[3] > clock;
}

static void apuPost(struct ApuCommand command);

void smpPortsSettle(void) {
	if (apuStarted && apuHead != __atomic_load_n(&apuTail, __ATOMIC_ACQUIRE)) {
		apuPost((struct ApuCommand) { .settle = true });
		return;
	}
	memset(smpPortsAt, 0, sizeof(smpPortsAt));
}

// The same as sa1ClocksKept, for a sound driver's wait.
void smpClocksKept(void) {
	if (apuStarted && apuHead != __atomic_load_n(&apuTail, __ATOMIC_ACQUIRE)) {
		apuPost((struct ApuCommand) { .kept = true });
		return;
	}
	spcWait.lastRead = ~0ull;
}

void smpRunToWrite(u64 clock, bool write, u32 port, u8 data) {
	smpWriting.on = write;
	smpWriting.at = clock;
	smpWriting.port = port;
	smpWriting.data = data;
	smpLimit = clock;
	if (smp.th.clock < smpLimit) {
		smpRun();
	}
	if (smpWriting.on) {
		smp.fromCpu[port] = data;
		smpWriting.on = false;
	}
}

static void smpRunToPortWriteHere(u64 clock) {
	memcpy(smpPortWrite.before, smp.toCpu, 4);
	smpStopAtPortWrite = true;
	smpPortWrite.at = ~0ull;
	smpLimit = clock;
	if (smp.th.clock < smpLimit) {
		smpRun();
	}
	smpStopAtPortWrite = false;
	smpPortWrite.done = smp.th.clock;
}

// A wait in a spin loop, without leaving the core.
static inline void apuRelax(void) {
#if defined(__aarch64__) || defined(__arm__)
	__asm__ volatile("yield");
#elif defined(__x86_64__) || defined(__i386__)
	__builtin_ia32_pause();
#endif
}

static void* apuMain(void* unused) {
	(void) unused;
	apuOnThread = true;
	for (;;) {
		u32 tail = apuTail;
		while (tail != __atomic_load_n(&apuHead, __ATOMIC_ACQUIRE)) {
			const struct ApuCommand* c = &apuRing[tail & (APU_RING - 1)];
			apuGameBoyWrite = c->gameBoyWrite;
			if (c->settle) {
				memset(smpPortsAt, 0, sizeof(smpPortsAt));
			} else if (c->kept) {
				spcWait.lastRead = ~0ull;
			} else if (c->toPortWrite) {
				smpRunToPortWriteHere(c->clock);
			} else {
				smpRunToWrite(c->clock, c->write, c->port, c->data);
			}
			__atomic_store_n(&apuReached, c->clock, __ATOMIC_RELAXED);
			tail++;
			__atomic_store_n(&apuTail, tail, __ATOMIC_RELEASE);
		}
		// The CPU waits on the thread when it reads a port: a new command is looked for at once for a
		// while before the thread gives way.
		for (u32 spin = 0; spin < 5000 && tail == __atomic_load_n(&apuHead, __ATOMIC_ACQUIRE); spin++) {
			apuRelax();
		}
		for (u32 spin = 0; spin < 20 && tail == __atomic_load_n(&apuHead, __ATOMIC_ACQUIRE); spin++) {
			sched_yield();
		}
		// Sleeping is set before the ring is looked at again, and smpPost looks at it after it adds to
		// the ring, both in one order for both threads: one of them sees the other.
		pthread_mutex_lock(&apuLock);
		__atomic_store_n(&apuSleeping, true, __ATOMIC_SEQ_CST);
		while (tail == __atomic_load_n(&apuHead, __ATOMIC_SEQ_CST) && !apuStopping) {
			pthread_cond_wait(&apuWake, &apuLock);
		}
		__atomic_store_n(&apuSleeping, false, __ATOMIC_SEQ_CST);
		bool stop = apuStopping && tail == __atomic_load_n(&apuHead, __ATOMIC_SEQ_CST);
		pthread_mutex_unlock(&apuLock);
		if (stop) {
			return NULL;
		}
	}
}

static void apuWakeIfSleeping(void) {
	if (__atomic_load_n(&apuSleeping, __ATOMIC_SEQ_CST)) {
		pthread_mutex_lock(&apuLock);
		pthread_cond_signal(&apuWake);
		pthread_mutex_unlock(&apuLock);
	}
}

void smpPost(u64 clock, bool write, u32 port, u8 data) {
	apuPost((struct ApuCommand) { clock, write, false, (u8) port, data });
}

bool smpRunToPortWrite(u64 clock, struct SmpPortWrite* write) {
	if (smpThreaded) {
		apuPost((struct ApuCommand) { clock, false, true, 0, 0 });
		smpDrain();
	} else {
		smpRunToPortWriteHere(clock);
	}
	*write = smpPortWrite;
	return smpPortWrite.at != ~0ull;
}

static void apuPost(struct ApuCommand command) {
	// The SMP is as far as the last run when the thread comes to a settle or kept (smpDrainTo).
	static u64 lastRun;
	if (command.settle || command.kept) {
		command.clock = lastRun;
	} else {
		lastRun = command.clock;
	}
	command.gameBoyWrite = cart.icd ? icd.sound.write : 0;
	u64 clock = command.clock;
	bool write = command.write;
	u32 port = command.port;
	u8 data = command.data;
	if (!apuStarted) {
		apuStarted = pthread_create(&apuThread, NULL, apuMain, NULL) == 0;
		if (!apuStarted) {
			if (command.toPortWrite) {
				smpRunToPortWriteHere(clock);
				return;
			}
			smpRunToWrite(clock, write, port, data);
			return;
		}
	}
	while (apuHead - __atomic_load_n(&apuTail, __ATOMIC_ACQUIRE) >= APU_RING) {
		apuWakeIfSleeping();
		sched_yield();
	}
	apuRing[apuHead & (APU_RING - 1)] = command;
	__atomic_store_n(&apuHead, apuHead + 1, __ATOMIC_SEQ_CST);
	apuWakeIfSleeping();
}

void smpDrain(void) {
	if (!apuStarted) {
		return;
	}
	while (__atomic_load_n(&apuTail, __ATOMIC_ACQUIRE) != apuHead) {
		apuWakeIfSleeping();
		apuRelax();
	}
}

void smpClocksMoved(void) {
	smpDrain();
	__atomic_store_n(&apuReached, smp.th.clock, __ATOMIC_RELAXED);
}

void smpDrainTo(u64 clock) {
	if (!apuStarted) {
		return;
	}
	for (;;) {
		u32 tail = __atomic_load_n(&apuTail, __ATOMIC_ACQUIRE);
		if (tail == apuHead || __atomic_load_n(&apuReached, __ATOMIC_RELAXED) >= clock) {
			return;
		}
		apuWakeIfSleeping();
		apuRelax();
	}
}

// The DSP's samples, which the frontend takes on the CPU's thread: those the APU's thread makes wait in a
// ring until smpSamplesOut hands them on, and so do any the CPU's thread makes while some are waiting.
enum { SOUND_RING = 1 << 14 };
static s16 soundRing[SOUND_RING][2];
static _Alignas(64) u32 soundHead;  // written by whichever thread runs the DSP
static _Alignas(64) u32 soundTail;  // by the CPU's

void smpSamplesOut(void) {
	u32 head = __atomic_load_n(&soundHead, __ATOMIC_ACQUIRE);
	for (u32 tail = soundTail; tail != head; tail++) {
		audioSample(soundRing[tail & (SOUND_RING - 1)][0], soundRing[tail & (SOUND_RING - 1)][1]);
	}
	__atomic_store_n(&soundTail, head, __ATOMIC_RELEASE);
}

void smpSample(s16 left, s16 right) {
	if (!apuOnThread && soundHead == soundTail) {
		audioSample(left, right);
		return;
	}
	while (soundHead - __atomic_load_n(&soundTail, __ATOMIC_ACQUIRE) >= SOUND_RING) {
		if (!apuOnThread) {
			smpSamplesOut();
		} else {
			sched_yield();
		}
	}
	soundRing[soundHead & (SOUND_RING - 1)][0] = left;
	soundRing[soundHead & (SOUND_RING - 1)][1] = right;
	__atomic_store_n(&soundHead, soundHead + 1, __ATOMIC_RELEASE);
}

void smpStop(void) {
	if (!apuStarted) {
		return;
	}
	smpDrain();
	pthread_mutex_lock(&apuLock);
	apuStopping = true;
	pthread_cond_signal(&apuWake);
	pthread_mutex_unlock(&apuLock);
	pthread_join(apuThread, NULL);
	apuStopping = false;
	apuStarted = false;
}

