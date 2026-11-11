/* Copyright (c) 2004-2025 ares team, Near et al
 * Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Ported from ares (ISC); see LICENSES/ISC-ares.txt. */
#include "snes.h"

// The S-SMP: an SPC700 with three timers and four ports to the CPU.
//
// The SMP runs only when the CPU needs it, and stops as soon as its clock passes the CPU's, which can
// be in the middle of an instruction. So the interpreter is one function that can leave after any
// clock step and come back to the same place: smp.state says where, and what an instruction holds
// between its cycles is in smp too, never in a local variable.

struct Smp smp;

static const u8 iplrom[64] = {
	0xcd, 0xef, 0xbd, 0xe8, 0x00, 0xc6, 0x1d, 0xd0, 0xfc, 0x8f, 0xaa, 0xf4, 0x8f, 0xbb, 0xf5, 0x78,
	0xcc, 0xf4, 0xd0, 0xfb, 0x2f, 0x19, 0xeb, 0xf4, 0xd0, 0xfc, 0x7e, 0xf4, 0xd0, 0x0b, 0xe4, 0xf5,
	0xcb, 0xf4, 0xd7, 0x00, 0xfc, 0xd0, 0xf3, 0xab, 0x01, 0x10, 0xef, 0x7e, 0xf4, 0x10, 0xeb, 0xba,
	0xf6, 0xda, 0x00, 0xba, 0xf4, 0xc4, 0xf4, 0xdd, 0x5d, 0xd0, 0xdb, 0x1f, 0x00, 0x00, 0xc0, 0xff,
};

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
		return dspRead(smp.dspAddress & 0x7f);
	case 0xf4:
	case 0xf5:
	case 0xf6:
	case 0xf7:
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
		dspWrite(smp.dspAddress, data);
		break;
	case 0xf4:
	case 0xf5:
	case 0xf6:
	case 0xf7:
		smp.toCpu[address & 3] = data;
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
#define STEP(clocks) \
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
		stepTimers(timerWaitStates[smp.pend] >> (halve)); \
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
			smp.dr = readMemory(smp.ad); \
		} \
		target = smp.dr; \
	} while (0)
#define WRITE(address, data) \
	do { \
		smp.ad = (address); \
		smp.dw = (data); \
		WAIT(0, waitStatesFor(smp.ad)); \
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

void smpRun(void) {
	for (;;) {
		switch (smp.state) {
		case MAIN:
			// A waiting or stopped SMP never wakes: nothing interrupts it.
			if (smp.wait || smp.stop) {
				SKIP(PC);
				IDLE();
				goto done;
			}
			FETCH(D);
			smp.state = D;
			continue;

		op(0x00, NoOperation)
		op(0x01, CallTable, 0)
		op(0x02, AbsoluteBitSet, 0, true)
		op(0x03, BranchBit, 0, true)
		op(0x04, DirectRead, fp(OR), A)
		op(0x05, AbsoluteRead, fp(OR), A)
		op(0x06, IndirectXRead, fp(OR))
		op(0x07, IndexedIndirectRead, fp(OR), X)
		op(0x08, ImmediateRead, fp(OR), A)
		op(0x09, DirectDirectModify, fp(OR))
		op(0x0a, AbsoluteBitModify, 0)
		op(0x0b, DirectModify, fp(ASL))
		op(0x0c, AbsoluteModify, fp(ASL))
		op(0x0d, Push, P)
		op(0x0e, TestSetBitsAbsolute, true)
		op(0x0f, Break)
		op(0x10, Branch, NF == 0)
		op(0x11, CallTable, 1)
		op(0x12, AbsoluteBitSet, 0, false)
		op(0x13, BranchBit, 0, false)
		op(0x14, DirectIndexedRead, fp(OR), A, X)
		op(0x15, AbsoluteIndexedRead, fp(OR), X)
		op(0x16, AbsoluteIndexedRead, fp(OR), Y)
		op(0x17, IndirectIndexedRead, fp(OR), Y)
		op(0x18, DirectImmediateModify, fp(OR))
		op(0x19, IndirectXWriteIndirectY, fp(OR))
		op(0x1a, DirectModifyWord, -1)
		op(0x1b, DirectIndexedModify, fp(ASL), X)
		op(0x1c, ImpliedModify, fp(ASL), A)
		op(0x1d, ImpliedModify, fp(DEC), X)
		op(0x1e, AbsoluteRead, fp(CMP), X)
		op(0x1f, JumpIndirectX)
		op(0x20, FlagSet, PF, false)
		op(0x21, CallTable, 2)
		op(0x22, AbsoluteBitSet, 1, true)
		op(0x23, BranchBit, 1, true)
		op(0x24, DirectRead, fp(AND), A)
		op(0x25, AbsoluteRead, fp(AND), A)
		op(0x26, IndirectXRead, fp(AND))
		op(0x27, IndexedIndirectRead, fp(AND), X)
		op(0x28, ImmediateRead, fp(AND), A)
		op(0x29, DirectDirectModify, fp(AND))
		op(0x2a, AbsoluteBitModify, 1)
		op(0x2b, DirectModify, fp(ROL))
		op(0x2c, AbsoluteModify, fp(ROL))
		op(0x2d, Push, A)
		op(0x2e, BranchNotDirect)
		op(0x2f, Branch, true)
		op(0x30, Branch, NF == 1)
		op(0x31, CallTable, 3)
		op(0x32, AbsoluteBitSet, 1, false)
		op(0x33, BranchBit, 1, false)
		op(0x34, DirectIndexedRead, fp(AND), A, X)
		op(0x35, AbsoluteIndexedRead, fp(AND), X)
		op(0x36, AbsoluteIndexedRead, fp(AND), Y)
		op(0x37, IndirectIndexedRead, fp(AND), Y)
		op(0x38, DirectImmediateModify, fp(AND))
		op(0x39, IndirectXWriteIndirectY, fp(AND))
		op(0x3a, DirectModifyWord, +1)
		op(0x3b, DirectIndexedModify, fp(ROL), X)
		op(0x3c, ImpliedModify, fp(ROL), A)
		op(0x3d, ImpliedModify, fp(INC), X)
		op(0x3e, DirectRead, fp(CMP), X)
		op(0x3f, CallAbsolute)
		op(0x40, FlagSet, PF, true)
		op(0x41, CallTable, 4)
		op(0x42, AbsoluteBitSet, 2, true)
		op(0x43, BranchBit, 2, true)
		op(0x44, DirectRead, fp(EOR), A)
		op(0x45, AbsoluteRead, fp(EOR), A)
		op(0x46, IndirectXRead, fp(EOR))
		op(0x47, IndexedIndirectRead, fp(EOR), X)
		op(0x48, ImmediateRead, fp(EOR), A)
		op(0x49, DirectDirectModify, fp(EOR))
		op(0x4a, AbsoluteBitModify, 2)
		op(0x4b, DirectModify, fp(LSR))
		op(0x4c, AbsoluteModify, fp(LSR))
		op(0x4d, Push, X)
		op(0x4e, TestSetBitsAbsolute, false)
		op(0x4f, CallPage)
		op(0x50, Branch, VF == 0)
		op(0x51, CallTable, 5)
		op(0x52, AbsoluteBitSet, 2, false)
		op(0x53, BranchBit, 2, false)
		op(0x54, DirectIndexedRead, fp(EOR), A, X)
		op(0x55, AbsoluteIndexedRead, fp(EOR), X)
		op(0x56, AbsoluteIndexedRead, fp(EOR), Y)
		op(0x57, IndirectIndexedRead, fp(EOR), Y)
		op(0x58, DirectImmediateModify, fp(EOR))
		op(0x59, IndirectXWriteIndirectY, fp(EOR))
		op(0x5a, DirectCompareWord, fp(CPW))
		op(0x5b, DirectIndexedModify, fp(LSR), X)
		op(0x5c, ImpliedModify, fp(LSR), A)
		op(0x5d, Transfer, A, X)
		op(0x5e, AbsoluteRead, fp(CMP), Y)
		op(0x5f, JumpAbsolute)
		op(0x60, FlagSet, CF, false)
		op(0x61, CallTable, 6)
		op(0x62, AbsoluteBitSet, 3, true)
		op(0x63, BranchBit, 3, true)
		op(0x64, DirectRead, fp(CMP), A)
		op(0x65, AbsoluteRead, fp(CMP), A)
		op(0x66, IndirectXRead, fp(CMP))
		op(0x67, IndexedIndirectRead, fp(CMP), X)
		op(0x68, ImmediateRead, fp(CMP), A)
		op(0x69, DirectDirectCompare, fp(CMP))
		op(0x6a, AbsoluteBitModify, 3)
		op(0x6b, DirectModify, fp(ROR))
		op(0x6c, AbsoluteModify, fp(ROR))
		op(0x6d, Push, Y)
		op(0x6e, BranchNotDirectDecrement)
		op(0x6f, ReturnSubroutine)
		op(0x70, Branch, VF == 1)
		op(0x71, CallTable, 7)
		op(0x72, AbsoluteBitSet, 3, false)
		op(0x73, BranchBit, 3, false)
		op(0x74, DirectIndexedRead, fp(CMP), A, X)
		op(0x75, AbsoluteIndexedRead, fp(CMP), X)
		op(0x76, AbsoluteIndexedRead, fp(CMP), Y)
		op(0x77, IndirectIndexedRead, fp(CMP), Y)
		op(0x78, DirectImmediateCompare, fp(CMP))
		op(0x79, IndirectXCompareIndirectY, fp(CMP))
		op(0x7a, DirectReadWord, fp(ADW))
		op(0x7b, DirectIndexedModify, fp(ROR), X)
		op(0x7c, ImpliedModify, fp(ROR), A)
		op(0x7d, Transfer, X, A)
		op(0x7e, DirectRead, fp(CMP), Y)
		op(0x7f, ReturnInterrupt)
		op(0x80, FlagSet, CF, true)
		op(0x81, CallTable, 8)
		op(0x82, AbsoluteBitSet, 4, true)
		op(0x83, BranchBit, 4, true)
		op(0x84, DirectRead, fp(ADC), A)
		op(0x85, AbsoluteRead, fp(ADC), A)
		op(0x86, IndirectXRead, fp(ADC))
		op(0x87, IndexedIndirectRead, fp(ADC), X)
		op(0x88, ImmediateRead, fp(ADC), A)
		op(0x89, DirectDirectModify, fp(ADC))
		op(0x8a, AbsoluteBitModify, 4)
		op(0x8b, DirectModify, fp(DEC))
		op(0x8c, AbsoluteModify, fp(DEC))
		op(0x8d, ImmediateRead, fp(LD), Y)
		op(0x8e, PullP)
		op(0x8f, DirectImmediateWrite)
		op(0x90, Branch, CF == 0)
		op(0x91, CallTable, 9)
		op(0x92, AbsoluteBitSet, 4, false)
		op(0x93, BranchBit, 4, false)
		op(0x94, DirectIndexedRead, fp(ADC), A, X)
		op(0x95, AbsoluteIndexedRead, fp(ADC), X)
		op(0x96, AbsoluteIndexedRead, fp(ADC), Y)
		op(0x97, IndirectIndexedRead, fp(ADC), Y)
		op(0x98, DirectImmediateModify, fp(ADC))
		op(0x99, IndirectXWriteIndirectY, fp(ADC))
		op(0x9a, DirectReadWord, fp(SBW))
		op(0x9b, DirectIndexedModify, fp(DEC), X)
		op(0x9c, ImpliedModify, fp(DEC), A)
		op(0x9d, Transfer, S, X)
		op(0x9e, Divide)
		op(0x9f, ExchangeNibble)
		op(0xa0, FlagSet, IF, true)
		op(0xa1, CallTable, 10)
		op(0xa2, AbsoluteBitSet, 5, true)
		op(0xa3, BranchBit, 5, true)
		op(0xa4, DirectRead, fp(SBC), A)
		op(0xa5, AbsoluteRead, fp(SBC), A)
		op(0xa6, IndirectXRead, fp(SBC))
		op(0xa7, IndexedIndirectRead, fp(SBC), X)
		op(0xa8, ImmediateRead, fp(SBC), A)
		op(0xa9, DirectDirectModify, fp(SBC))
		op(0xaa, AbsoluteBitModify, 5)
		op(0xab, DirectModify, fp(INC))
		op(0xac, AbsoluteModify, fp(INC))
		op(0xad, ImmediateRead, fp(CMP), Y)
		op(0xae, Pull, A)
		op(0xaf, IndirectXIncrementWrite, A)
		op(0xb0, Branch, CF == 1)
		op(0xb1, CallTable, 11)
		op(0xb2, AbsoluteBitSet, 5, false)
		op(0xb3, BranchBit, 5, false)
		op(0xb4, DirectIndexedRead, fp(SBC), A, X)
		op(0xb5, AbsoluteIndexedRead, fp(SBC), X)
		op(0xb6, AbsoluteIndexedRead, fp(SBC), Y)
		op(0xb7, IndirectIndexedRead, fp(SBC), Y)
		op(0xb8, DirectImmediateModify, fp(SBC))
		op(0xb9, IndirectXWriteIndirectY, fp(SBC))
		op(0xba, DirectReadWord, fp(LDW))
		op(0xbb, DirectIndexedModify, fp(INC), X)
		op(0xbc, ImpliedModify, fp(INC), A)
		op(0xbd, Transfer, X, S)
		op(0xbe, DecimalAdjustSub)
		op(0xbf, IndirectXIncrementRead, A)
		op(0xc0, FlagSet, IF, false)
		op(0xc1, CallTable, 12)
		op(0xc2, AbsoluteBitSet, 6, true)
		op(0xc3, BranchBit, 6, true)
		op(0xc4, DirectWrite, A)
		op(0xc5, AbsoluteWrite, A)
		op(0xc6, IndirectXWrite, A)
		op(0xc7, IndexedIndirectWrite, A, X)
		op(0xc8, ImmediateRead, fp(CMP), X)
		op(0xc9, AbsoluteWrite, X)
		op(0xca, AbsoluteBitModify, 6)
		op(0xcb, DirectWrite, Y)
		op(0xcc, AbsoluteWrite, Y)
		op(0xcd, ImmediateRead, fp(LD), X)
		op(0xce, Pull, X)
		op(0xcf, Multiply)
		op(0xd0, Branch, ZF == 0)
		op(0xd1, CallTable, 13)
		op(0xd2, AbsoluteBitSet, 6, false)
		op(0xd3, BranchBit, 6, false)
		op(0xd4, DirectIndexedWrite, A, X)
		op(0xd5, AbsoluteIndexedWrite, X)
		op(0xd6, AbsoluteIndexedWrite, Y)
		op(0xd7, IndirectIndexedWrite, A, Y)
		op(0xd8, DirectWrite, X)
		op(0xd9, DirectIndexedWrite, X, Y)
		op(0xda, DirectWriteWord)
		op(0xdb, DirectIndexedWrite, Y, X)
		op(0xdc, ImpliedModify, fp(DEC), Y)
		op(0xdd, Transfer, Y, A)
		op(0xde, BranchNotDirectIndexed, X)
		op(0xdf, DecimalAdjustAdd)
		op(0xe0, OverflowClear)
		op(0xe1, CallTable, 14)
		op(0xe2, AbsoluteBitSet, 7, true)
		op(0xe3, BranchBit, 7, true)
		op(0xe4, DirectRead, fp(LD), A)
		op(0xe5, AbsoluteRead, fp(LD), A)
		op(0xe6, IndirectXRead, fp(LD))
		op(0xe7, IndexedIndirectRead, fp(LD), X)
		op(0xe8, ImmediateRead, fp(LD), A)
		op(0xe9, AbsoluteRead, fp(LD), X)
		op(0xea, AbsoluteBitModify, 7)
		op(0xeb, DirectRead, fp(LD), Y)
		op(0xec, AbsoluteRead, fp(LD), Y)
		op(0xed, ComplementCarry)
		op(0xee, Pull, Y)
		op(0xef, Wait)
		op(0xf0, Branch, ZF == 1)
		op(0xf1, CallTable, 15)
		op(0xf2, AbsoluteBitSet, 7, false)
		op(0xf3, BranchBit, 7, false)
		op(0xf4, DirectIndexedRead, fp(LD), A, X)
		op(0xf5, AbsoluteIndexedRead, fp(LD), X)
		op(0xf6, AbsoluteIndexedRead, fp(LD), Y)
		op(0xf7, IndirectIndexedRead, fp(LD), Y)
		op(0xf8, DirectRead, fp(LD), X)
		op(0xf9, DirectIndexedRead, fp(LD), X, Y)
		op(0xfa, DirectDirectWrite)
		op(0xfb, DirectIndexedRead, fp(LD), Y, X)
		op(0xfc, ImpliedModify, fp(INC), Y)
		op(0xfd, Transfer, A, Y)
		op(0xfe, BranchNotYDecrement)
		op(0xff, Stop)
		}
	done:
		smp.state = MAIN;
	}
}
