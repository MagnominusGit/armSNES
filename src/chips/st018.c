/* Copyright (c) 2004-2025 ares team, Near et al
 * Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Ported from ares (ISC); see LICENSES/ISC-ares.txt. */
#include "../snes.h"

// The ST018: an ARM6 with its own program and data ROM, 16 KiB of RAM, a byte port each way to the
// S-CPU and a timer. As in ares, it runs the ARM7TDMI's ARM instructions (the Thumb state is turned
// off before each one), and every memory access and idle cycle is a clock step. The S-CPU can see
// the port between any two of them, so the processor is one function that can stop at each step and
// carry on later; what an instruction keeps across a step is in struct St018.

struct St018 st018;

#define S st018

static u8 programROM[128 * 1024];
static u8 dataROM[32 * 1024];

enum { USR = 0x10, FIQ = 0x11, IRQ = 0x12, SVC = 0x13, ABT = 0x17, UND = 0x1b, SYS = 0x1f };
enum { LOAD = 1 << 0, STORE = 1 << 1, PREFETCH = 1 << 2, BYTE = 1 << 3, HALF = 1 << 4, WORD = 1 << 5, SIGNED = 1 << 6 };

// --- Registers ---

static u32* reg(u32 i) {
	if (i < 8 || i == 15) {
		return &S.r[i];
	}
	if (i < 13) {
		return S.cpsr.m == FIQ ? &S.fiq[i - 8] : &S.r[i];
	}
	switch (S.cpsr.m) {
	case USR:
	case SYS: return &S.r[i];
	case FIQ: return &S.fiq[i - 8];
	case IRQ: return &S.irqBank[i - 13];
	case SVC: return &S.svcBank[i - 13];
	case ABT: return &S.abtBank[i - 13];
	case UND: return &S.undBank[i - 13];
	}
	return &S.rNull;
}

static inline u32 getR(u32 i) {
	return *reg(i);
}

// Writing the program counter refills the pipeline; the register of a mode with none stays zero.
static void setR(u32 i, u32 value) {
	u32* p = reg(i);
	*p = value;
	if (p == &S.r[15]) {
		S.r[15] &= ~1u;
		S.pipeline.reload = true;
	} else if (p == &S.rNull) {
		S.rNull = 0;
	}
}

static struct ArmPsr* spsr(void) {
	switch (S.cpsr.m) {
	case FIQ: return &S.spsrFiq;
	case IRQ: return &S.spsrIrq;
	case SVC: return &S.spsrSvc;
	case ABT: return &S.spsrAbt;
	case UND: return &S.spsrUnd;
	}
	return &S.spsrNull;
}

static u32 psrValue(const struct ArmPsr* p) {
	return (p->m & 0x1f) | 1 << 4 | p->t << 5 | p->f << 6 | p->i << 7 | (u32) p->v << 28 | (u32) p->c << 29 |
	       (u32) p->z << 30 | (u32) p->n << 31;
}

static void psrSet(struct ArmPsr* p, u32 field, u32 data) {
	if (p->readonly) {
		return;
	}
	if (field & 1) {
		p->m = (data & 0x1f) | 0x10;
		p->t = data >> 5 & 1;
		p->f = data >> 6 & 1;
		p->i = data >> 7 & 1;
	}
	if (field & 8) {
		p->v = data >> 28 & 1;
		p->c = data >> 29 & 1;
		p->z = data >> 30 & 1;
		p->n = data >> 31 & 1;
	}
}

static bool privileged(void) {
	return S.cpsr.m != USR;
}

static bool exceptionMode(void) {
	return privileged() && S.cpsr.m != SYS;
}

static void exception(u32 mode, u32 address) {
	struct ArmPsr psr = S.cpsr;
	S.cpsr.m = mode;
	*spsr() = psr;
	S.cpsr.t = 0;
	if (S.cpsr.m == FIQ) {
		S.cpsr.f = 1;
	}
	S.cpsr.i = 1;
	setR(14, S.pipeline.decode.address);
	setR(15, address);
}

// --- The ALU ---

#define OP S.opcode
#define FIELD(lo, bits) (OP >> (lo) & ((1u << (bits)) - 1))
#define FLAG(b) (OP >> (b) & 1)
#define SAVE_FLAGS (S.cpsr.t || FLAG(20))

static u32 add(u32 source, u32 modify, bool carry) {
	u32 result = source + modify + carry;
	if (SAVE_FLAGS) {
		u32 overflow = ~(source ^ modify) & (source ^ result);
		S.cpsr.v = overflow >> 31;
		S.cpsr.c = (overflow ^ source ^ modify ^ result) >> 31;
		S.cpsr.z = result == 0;
		S.cpsr.n = result >> 31;
	}
	return result;
}

static u32 sub(u32 source, u32 modify, bool carry) {
	return add(source, ~modify, carry);
}

static u32 bit(u32 result) {
	if (SAVE_FLAGS) {
		S.cpsr.c = S.carry;
		S.cpsr.z = result == 0;
		S.cpsr.n = result >> 31;
	}
	return result;
}

static u32 lsl(u32 source, u8 shift) {
	S.carry = S.cpsr.c;
	if (shift == 0) {
		return source;
	}
	S.carry = shift > 32 ? 0 : source >> (32 - shift) & 1;
	return shift > 31 ? 0 : source << shift;
}

static u32 lsr(u32 source, u8 shift) {
	S.carry = S.cpsr.c;
	if (shift == 0) {
		return source;
	}
	S.carry = shift > 32 ? 0 : source >> (shift - 1) & 1;
	return shift > 31 ? 0 : source >> shift;
}

static u32 asr(u32 source, u8 shift) {
	S.carry = S.cpsr.c;
	if (shift == 0) {
		return source;
	}
	S.carry = shift > 32 ? source >> 31 : source >> (shift - 1) & 1;
	return shift > 31 ? (u32) ((s32) source >> 31) : (u32) ((s32) source >> shift);
}

static u32 ror(u32 source, u8 shift) {
	S.carry = S.cpsr.c;
	if (shift == 0) {
		return source;
	}
	if (shift &= 31) {
		source = source << (32 - shift) | source >> shift;
	}
	S.carry = source >> 31;
	return source;
}

static u32 rrx(u32 source) {
	S.carry = source & 1;
	return (u32) S.cpsr.c << 31 | source >> 1;
}

static bool test(u32 cond) {
	switch (cond & 15) {
	case 0: return S.cpsr.z;
	case 1: return !S.cpsr.z;
	case 2: return S.cpsr.c;
	case 3: return !S.cpsr.c;
	case 4: return S.cpsr.n;
	case 5: return !S.cpsr.n;
	case 6: return S.cpsr.v;
	case 7: return !S.cpsr.v;
	case 8: return S.cpsr.c && !S.cpsr.z;
	case 9: return !S.cpsr.c || S.cpsr.z;
	case 10: return S.cpsr.n == S.cpsr.v;
	case 11: return S.cpsr.n != S.cpsr.v;
	case 12: return !S.cpsr.z && S.cpsr.n == S.cpsr.v;
	case 13: return S.cpsr.z || S.cpsr.n != S.cpsr.v;
	case 14: return true;
	}
	return false;
}

// A register shifted by an immediate amount; 0 means 32 for the right shifts and RRX for the rotate.
static u32 shiftImmediate(u32 rm, u32 type, u32 shift) {
	switch (type) {
	case 0: return lsl(rm, shift);
	case 1: return lsr(rm, shift ? shift : 32);
	case 2: return asr(rm, shift ? shift : 32);
	}
	return shift ? ror(rm, shift) : rrx(rm);
}

static void alu(u32 mode, u32 d, u32 rn, u32 rm) {
	switch (mode) {
	case 0: setR(d, bit(rn & rm)); break;
	case 1: setR(d, bit(rn ^ rm)); break;
	case 2: setR(d, sub(rn, rm, 1)); break;
	case 3: setR(d, sub(rm, rn, 1)); break;
	case 4: setR(d, add(rn, rm, 0)); break;
	case 5: setR(d, add(rn, rm, S.cpsr.c)); break;
	case 6: setR(d, sub(rn, rm, S.cpsr.c)); break;
	case 7: setR(d, sub(rm, rn, S.cpsr.c)); break;
	case 8: bit(rn & rm); break;
	case 9: bit(rn ^ rm); break;
	case 10: sub(rn, rm, 1); break;
	case 11: add(rn, rm, 0); break;
	case 12: setR(d, bit(rn | rm)); break;
	case 13: setR(d, bit(rm)); break;
	case 14: setR(d, bit(rn & ~rm)); break;
	case 15: setR(d, bit(~rm)); break;
	}
	if (exceptionMode() && d == 15 && FLAG(20)) {
		S.cpsr = *spsr();
	}
}

static void moveToStatus(u32 field, u32 mode, u32 data) {
	if (mode) {
		psrSet(spsr(), field, data);
	} else {
		if (!privileged()) {
			field &= ~1u;
		}
		psrSet(&S.cpsr, field, data);
		if (S.cpsr.t) {
			S.r[15] += 2;
		}
	}
}

// --- Memory ---

static u32 memoryRead(const u8* memory, u32 mode, u32 address) {
	if (mode & WORD) {
		memory += address & ~3u;
		return memory[0] | memory[1] << 8 | memory[2] << 16 | (u32) memory[3] << 24;
	}
	if (mode & BYTE) {
		return memory[address];
	}
	return 0;
}

static u8 status(void) {
	return S.bridge.armtocpuReady | S.bridge.signal << 2 | S.bridge.cputoarmReady << 3 | S.bridge.ready << 7;
}

// The access itself, after its clock step: ROM, RAM, the port, or what the bus last held.
static u32 armGet(u32 mode, u32 address) {
	switch (address & 0xe0000000) {
	case 0x00000000: return memoryRead(programROM, mode, address & 0x1ffff);
	case 0x20000000: return S.pipeline.fetch.instruction;
	case 0x40000000: break;
	case 0x60000000: return 0x40404001;
	case 0x80000000: return S.pipeline.fetch.instruction;
	case 0xa0000000: return memoryRead(dataROM, mode, address & 0x7fff);
	case 0xc0000000: return S.pipeline.fetch.instruction;
	case 0xe0000000: return memoryRead(S.programRAM, mode, address & 0x3fff);
	}
	address &= 0xe000003f;
	if (address == 0x40000010 && S.bridge.cputoarmReady) {
		S.bridge.cputoarmReady = false;
		return S.bridge.cputoarmData;
	}
	if (address == 0x40000020) {
		return status();
	}
	return 0;
}

static void armSet(u32 mode, u32 address, u32 word) {
	switch (address & 0xe0000000) {
	case 0x40000000:
		break;
	case 0xe0000000: {
		u8* memory = S.programRAM;
		address &= 0x3fff;
		if (mode & WORD) {
			memory += address & ~3u;
			memory[0] = word;
			memory[1] = word >> 8;
			memory[2] = word >> 16;
			memory[3] = word >> 24;
		} else if (mode & BYTE) {
			memory[address] = word;
		}
		return;
	}
	default:
		return;
	}
	address &= 0xe000003f;
	word &= 0xff;
	if (address == 0x40000000) {
		S.bridge.armtocpuReady = true;
		S.bridge.armtocpuData = word;
	}
	if (address == 0x40000010) S.bridge.signal = true;
	if (address == 0x40000020) S.bridge.timerlatch = (S.bridge.timerlatch & ~0x0000ffu) | word;
	if (address == 0x40000024) S.bridge.timerlatch = (S.bridge.timerlatch & ~0x00ff00u) | word << 8;
	if (address == 0x40000028) S.bridge.timerlatch = (S.bridge.timerlatch & ~0xff0000u) | word << 16;
	if (address == 0x4000002c) S.bridge.timer = S.bridge.timerlatch;
}

// A loaded word turned to the bytes asked for: a halfword or byte zero- or sign-extended, and the
// word rotated by its address's low bits.
static u32 loadFinish(u32 mode, u32 address, u32 word) {
	if (mode & HALF) {
		address &= 1;
		word = mode & SIGNED ? (u32) (s32) (s16) word : (u16) word;
	}
	if (mode & BYTE) {
		address = 0;
		word = mode & SIGNED ? (u32) (s32) (s8) word : (u8) word;
	}
	return mode & SIGNED ? asr(word, (address & 3) << 3) : ror(word, (address & 3) << 3);
}

static u32 storeWord(u32 mode, u32 word) {
	if (mode & HALF) {
		word &= 0xffff;
		word |= word << 16;
	}
	if (mode & BYTE) {
		word &= 0xff;
		word |= word << 8;
		word |= word << 16;
	}
	return word;
}

// --- The decoder ---

enum {
	H_BRANCH,
	H_BX,
	H_CDP,
	H_DATA_IMM,
	H_DATA_IMM_SHIFT,
	H_DATA_REG_SHIFT,
	H_SWAP,
	H_HALF_IMM,
	H_HALF_REG,
	H_MOVE_IMM,
	H_MULTIPLE,
	H_MOVE_REG,
	H_SIGNED_IMM,
	H_SIGNED_REG,
	H_MCR,
	H_MRC,
	H_MOV_REG_REG,
	H_MRS,
	H_MSR_IMM,
	H_MSR_REG,
	H_MUL,
	H_MULL,
	H_SWI,
	H_UNDEFINED,
	H_NONE = 0xff,
};

static u8 armTable[4096];

static u32 pattern(const char* s) {
	u32 value = 0;
	for (; *s; ++s) {
		if (*s != ' ') {
			value = value << 1 | (*s == '1');
		}
	}
	return value;
}

static void bind(u32 opcode, u8 handler) {
	armTable[(opcode & 0x0ff00000) >> 16 | (opcode & 0x000000f0) >> 4] = handler;
}

// The same encodings as ares binds, in the same loops.
static void armInitialize(void) {
	memset(armTable, H_NONE, sizeof(armTable));
	for (u32 lo = 0; lo < 16; ++lo)
		for (u32 hi = 0; hi < 16; ++hi)
			for (u32 link = 0; link < 2; ++link)
				bind(pattern(".... 101? ???? ???? ???? ???? ???? ????") | lo << 4 | hi << 20 | link << 24, H_BRANCH);
	for (u32 x = 0; x < 8; ++x)
		for (u32 mode = 0; mode < 2; ++mode)
			bind(pattern(".... 0001 0??0 ???? ???? ---- 0??1 ????") | (x & 3) << 5 | (x >> 2 & 1) << 21 | mode << 22, H_BX);
	for (u32 op2 = 0; op2 < 8; ++op2)
		for (u32 op1 = 0; op1 < 16; ++op1)
			bind(pattern(".... 1110 ???? ???? ???? ???? ???0 ????") | op2 << 5 | op1 << 20, H_CDP);
	for (u32 shiftHi = 0; shiftHi < 16; ++shiftHi)
		for (u32 save = 0; save < 2; ++save)
			for (u32 mode = 0; mode < 16; ++mode) {
				if (mode >= 8 && mode <= 11 && !save) continue;
				bind(pattern(".... 001? ???? ???? ???? ???? ???? ????") | shiftHi << 4 | save << 20 | mode << 21, H_DATA_IMM);
			}
	for (u32 type = 0; type < 4; ++type)
		for (u32 shiftLo = 0; shiftLo < 2; ++shiftLo)
			for (u32 save = 0; save < 2; ++save)
				for (u32 mode = 0; mode < 16; ++mode) {
					if (mode >= 8 && mode <= 11 && !save) continue;
					bind(pattern(".... 000? ???? ???? ???? ???? ???0 ????") | type << 5 | shiftLo << 7 | save << 20 | mode << 21,
					     H_DATA_IMM_SHIFT);
				}
	for (u32 type = 0; type < 4; ++type)
		for (u32 save = 0; save < 2; ++save)
			for (u32 mode = 0; mode < 16; ++mode) {
				if (mode >= 8 && mode <= 11 && !save) continue;
				bind(pattern(".... 000? ???? ???? ???? ???? 0??1 ????") | type << 5 | save << 20 | mode << 21, H_DATA_REG_SHIFT);
			}
	for (u32 x = 0; x < 8; ++x)
		for (u32 byte = 0; byte < 2; ++byte)
			bind(pattern(".... 0001 ???? ???? ???? ---- 1001 ????") | (x & 3) << 20 | byte << 22 | (x >> 2 & 1) << 23, H_SWAP);
	for (u32 mode = 0; mode < 2; ++mode)
		for (u32 wb = 0; wb < 2; ++wb)
			for (u32 up = 0; up < 2; ++up)
				for (u32 pre = 0; pre < 2; ++pre) {
					u32 bits = mode << 20 | wb << 21 | up << 23 | pre << 24;
					bind(pattern(".... 000? ?1?? ???? ???? ???? 1011 ????") | bits, H_HALF_IMM);
					bind(pattern(".... 000? ?0?? ???? ???? ---- 1011 ????") | bits, H_HALF_REG);
				}
	for (u32 part = 0; part < 16; ++part)
		for (u32 mode = 0; mode < 2; ++mode)
			for (u32 wb = 0; wb < 2; ++wb)
				for (u32 byte = 0; byte < 2; ++byte)
					for (u32 up = 0; up < 2; ++up)
						for (u32 pre = 0; pre < 2; ++pre) {
							u32 bits = mode << 20 | wb << 21 | byte << 22 | up << 23 | pre << 24;
							bind(pattern(".... 010? ???? ???? ???? ???? ???? ????") | part << 4 | bits, H_MOVE_IMM);
							bind(pattern(".... 100? ???? ???? ???? ???? ???? ????") | part << 4 | bits, H_MULTIPLE);
						}
	for (u32 type = 0; type < 4; ++type)
		for (u32 shiftLo = 0; shiftLo < 2; ++shiftLo)
			for (u32 mode = 0; mode < 2; ++mode)
				for (u32 wb = 0; wb < 2; ++wb)
					for (u32 byte = 0; byte < 2; ++byte)
						for (u32 up = 0; up < 2; ++up)
							for (u32 pre = 0; pre < 2; ++pre)
								bind(pattern(".... 011? ???? ???? ???? ???? ???0 ????") | type << 5 | shiftLo << 7 | mode << 20 |
								     wb << 21 | byte << 22 | up << 23 | pre << 24, H_MOVE_REG);
	for (u32 half = 0; half < 2; ++half)
		for (u32 mode = 0; mode < 2; ++mode)
			for (u32 wb = 0; wb < 2; ++wb)
				for (u32 up = 0; up < 2; ++up)
					for (u32 pre = 0; pre < 2; ++pre) {
						u32 bits = half << 5 | mode << 20 | wb << 21 | up << 23 | pre << 24;
						bind(pattern(".... 000? ?1?? ???? ???? ???? 11?1 ????") | bits, H_SIGNED_IMM);
						bind(pattern(".... 000? ?0?? ???? ???? ---- 11?1 ????") | bits, H_SIGNED_REG);
					}
	for (u32 op2 = 0; op2 < 8; ++op2)
		for (u32 op1 = 0; op1 < 8; ++op1) {
			bind(pattern(".... 1110 ???0 ???? ???? ???? ???1 ????") | op2 << 5 | op1 << 21, H_MCR);
			bind(pattern(".... 1110 ???1 ???? ???? ???? ???1 ????") | op2 << 5 | op1 << 21, H_MRC);
		}
	for (u32 x = 0; x < 32; ++x)
		bind(pattern(".... 0011 0?00 ???? ???? ---- ???? ----") | (x & 15) << 4 | (x >> 4 & 1) << 22, H_MOV_REG_REG);
	for (u32 x = 0; x < 8; ++x)
		for (u32 mode = 0; mode < 2; ++mode)
			bind(pattern(".... 0001 0?00 ---- ???? ---- ???0 ----") | x << 5 | mode << 22, H_MRS);
	for (u32 hi = 0; hi < 16; ++hi)
		for (u32 mode = 0; mode < 2; ++mode)
			bind(pattern(".... 0011 0?10 ???? ---- ???? ???? ????") | hi << 4 | mode << 22, H_MSR_IMM);
	for (u32 type = 0; type < 4; ++type)
		for (u32 shiftLo = 0; shiftLo < 2; ++shiftLo)
			for (u32 mode = 0; mode < 2; ++mode)
				bind(pattern(".... 0001 0?10 ???? ---- ???? ???0 ????") | type << 5 | shiftLo << 7 | mode << 22, H_MSR_REG);
	for (u32 save = 0; save < 2; ++save)
		for (u32 acc = 0; acc < 2; ++acc)
			for (u32 x = 0; x < 2; ++x) {
				bind(pattern(".... 0000 0??? ???? ???? ???? 1001 ????") | save << 20 | acc << 21 | x << 22, H_MUL);
				bind(pattern(".... 0000 1??? ???? ???? ???? 1001 ????") | save << 20 | acc << 21 | x << 22, H_MULL);
			}
	for (u32 lo = 0; lo < 16; ++lo)
		for (u32 hi = 0; hi < 16; ++hi)
			bind(pattern(".... 1111 ???? ???? ???? ???? ???? ????") | lo << 4 | hi << 20, H_SWI);
	for (u32 x = 0; x < 256; ++x) {
		bind(pattern(".... 011? ???? ---- ---- ---- ???1 ----") | (x & 7) << 5 | (x >> 3 & 31) << 20, H_UNDEFINED);
		bind(pattern(".... 110? ???1 ---- ---- ---- ???? ----") | (x & 15) << 4 | (x >> 4 & 15) << 21, H_UNDEFINED);
		bind(pattern(".... 110? ???0 ---- ---- ---- ???? ----") | (x & 15) << 4 | (x >> 4 & 15) << 21, H_UNDEFINED);
	}
}

// --- The thread ---

enum { BOOT, MAIN, FIRST_HANDLER = 16, FIRST_LABEL = 64 };

// A clock step: the timer counts down once per step, whatever its length.
static void stepClocks(u32 clocks) {
	if (S.bridge.timer) {
		--S.bridge.timer;
	}
	S.th.clock += S.th.scalar * clocks;
}

#define STOP_(n) \
	do { \
		S.state = (n); \
		return; \
	case (n):; \
	} while (0)
#define STOP_X(n) STOP_(n)
#define STEP(clocks) \
	do { \
		stepClocks(clocks); \
		if (cpu.th.clock < S.th.clock) { \
			STOP_X(FIRST_LABEL + __COUNTER__); \
		} \
	} while (0)
// A read for the pipeline or a block transfer, after its step.
#define READ(target_, mode_, address_) \
	do { \
		S.mode = (mode_); \
		S.address = (address_); \
		STEP(1); \
		target_ = armGet(S.mode, S.address); \
		S.nonsequential = false; \
	} while (0)
#define WRITE(mode_, address_, word_) \
	do { \
		S.mode = (mode_); \
		S.address = (address_); \
		S.word = (word_); \
		STEP(1); \
		armSet(S.mode, S.address, S.word); \
		S.nonsequential = false; \
	} while (0)
#define LOAD(target_, mode_, address_) \
	do { \
		S.nonsequential = true; \
		S.mode = (mode_); \
		S.address = (address_); \
		STEP(1); \
		target_ = loadFinish(S.mode, S.address, armGet(LOAD | S.mode, S.address)); \
	} while (0)
#define STORE(mode_, address_, word_) \
	do { \
		S.nonsequential = true; \
		S.mode = (mode_); \
		S.address = (address_); \
		S.word = storeWord(S.mode, (word_)); \
		STEP(1); \
		armSet(STORE | S.mode, S.address, S.word); \
	} while (0)
#define IDLE() \
	do { \
		S.nonsequential = true; \
		STEP(1); \
	} while (0)
// The pipeline moves on: the decoded instruction to execute, the fetched one to decode, and a new
// fetch after the program counter.
#define FETCH() \
	do { \
		S.pipeline.execute = S.pipeline.decode; \
		S.pipeline.execute.irq = S.pipeline.execute.irq & S.irq; \
		S.pipeline.decode = S.pipeline.fetch; \
		S.pipeline.decode.thumb = S.cpsr.t; \
		S.pipeline.decode.irq = !S.cpsr.i; \
		S.r[15] += S.cpsr.t ? 2 : 4; \
		S.pipeline.fetch.address = S.r[15]; \
		READ(S.pipeline.fetch.instruction, PREFETCH | (S.cpsr.t ? HALF : WORD), S.pipeline.fetch.address); \
	} while (0)

void st018Run(void) {
dispatch:
	switch (S.state) {
	// The reset hold, then the reset sequence's delay.
	case BOOT:
		while (S.bridge.reset) {
			STEP(1);
		}
		if (!S.bridge.ready) {
			STEP(65536);
			S.bridge.ready = true;
		}
		goto done;

	case MAIN:
		S.cpsr.t = 0;
		if (S.pipeline.reload) {
			S.pipeline.reload = false;
			S.nonsequential = true;
			S.pipeline.fetch.address = S.r[15];
			READ(S.pipeline.fetch.instruction, PREFETCH | (S.cpsr.t ? HALF : WORD), S.pipeline.fetch.address);
			FETCH();
		}
		FETCH();
		if (S.pipeline.execute.irq) {
			exception(IRQ, 0x18);
			if (S.pipeline.execute.thumb) {
				setR(14, getR(14) + 2);
			}
			goto done;
		}
		S.opcode = S.pipeline.execute.instruction;
		if (!test(OP >> 28)) {
			goto done;
		}
		S.state = FIRST_HANDLER + armTable[(OP & 0x0ff00000) >> 16 | (OP & 0x000000f0) >> 4];
		goto dispatch;

	case FIRST_HANDLER + H_BRANCH: {
		s32 displacement = (s32) (OP << 8) >> 8;
		if (FLAG(24)) {
			setR(14, getR(15) - 4);
		}
		setR(15, getR(15) + (u32) displacement * 4);
		goto done;
	}

	case FIRST_HANDLER + H_BX: {
		u32 address = getR(FIELD(0, 4));
		u32 field = FIELD(16, 4);
		if ((field & 9) == 9) {
			struct ArmPsr* psr = FLAG(22) ? spsr() : &S.cpsr;
			psr->t = address & 1;
		} else {
			moveToStatus(field, FLAG(22), address);
		}
		setR(FIELD(12, 4), address);
		goto done;
	}

	// No coprocessors: their instructions are undefined.
	case FIRST_HANDLER + H_CDP:
	case FIRST_HANDLER + H_MCR:
	case FIRST_HANDLER + H_MRC:
	case FIRST_HANDLER + H_UNDEFINED:
		exception(UND, 0x04);
		goto done;

	case FIRST_HANDLER + H_DATA_IMM: {
		u32 rn = getR(FIELD(16, 4));
		u32 data = FIELD(0, 8);
		S.carry = S.cpsr.c;
		if (FIELD(8, 4)) {
			data = ror(data, FIELD(8, 4) << 1);
		}
		alu(FIELD(21, 4), FIELD(12, 4), rn, data);
		goto done;
	}

	case FIRST_HANDLER + H_DATA_IMM_SHIFT: {
		u32 rn = getR(FIELD(16, 4));
		u32 rm = getR(FIELD(0, 4));
		S.carry = S.cpsr.c;
		rm = shiftImmediate(rm, FIELD(5, 2), FIELD(7, 5));
		alu(FIELD(21, 4), FIELD(12, 4), rn, rm);
		goto done;
	}

	case FIRST_HANDLER + H_DATA_REG_SHIFT:
		// The registers are read before the idle cycle; nothing changes them during it.
		IDLE();
		{
			u8 rs = getR(FIELD(8, 4));
			u32 n = FIELD(16, 4), m = FIELD(0, 4);
			u32 rn = getR(n) + (n == 15 ? 4 : 0);
			u32 rm = getR(m) + (m == 15 ? 4 : 0);
			S.carry = S.cpsr.c;
			switch (FIELD(5, 2)) {
			case 0: rm = lsl(rm, rs); break;
			case 1: rm = lsr(rm, rs); break;
			case 2: rm = asr(rm, rs); break;
			case 3: rm = ror(rm, rs); break;
			}
			alu(FIELD(21, 4), FIELD(12, 4), rn, rm);
		}
		goto done;

	case FIRST_HANDLER + H_SWAP:
		LOAD(S.value, FLAG(22) ? BYTE : WORD, getR(FIELD(16, 4)));
		STORE(FLAG(22) ? BYTE : WORD, getR(FIELD(16, 4)), getR(FIELD(0, 4)) + (FIELD(0, 4) == 15 ? 4 : 0));
		IDLE();
		setR(FIELD(12, 4), S.value);
		goto done;

	// Halfword and signed transfers, by an immediate or a register offset.
	case FIRST_HANDLER + H_HALF_IMM:
	case FIRST_HANDLER + H_HALF_REG:
	case FIRST_HANDLER + H_SIGNED_IMM:
	case FIRST_HANDLER + H_SIGNED_REG: {
		u32 handler = S.state - FIRST_HANDLER;
		bool immediate = handler == H_HALF_IMM || handler == H_SIGNED_IMM;
		u32 d = FIELD(12, 4);
		S.offset = immediate ? (FIELD(0, 4) | FIELD(8, 4) << 4) : getR(FIELD(0, 4));
		S.access = handler == H_HALF_IMM || handler == H_HALF_REG ? HALF : (FLAG(5) ? HALF : BYTE) | SIGNED;
		S.rn = getR(FIELD(16, 4));
		S.value = getR(d) + (d == 15 ? 4 : 0);
		if (FLAG(24)) {
			S.rn = FLAG(23) ? S.rn + S.offset : S.rn - S.offset;
		}
		if (FLAG(20)) {
			LOAD(S.value, S.access, S.rn);
		} else {
			STORE(S.access, S.rn, S.value);
		}
		if (!FLAG(24)) {
			S.rn = FLAG(23) ? S.rn + S.offset : S.rn - S.offset;
		}
		if (!FLAG(24) || FLAG(21)) {
			setR(FIELD(16, 4), S.rn + (FIELD(16, 4) == 15 ? 4 : 0));
		}
		if (FLAG(20)) {
			IDLE();
			setR(FIELD(12, 4), S.value);
		}
		goto done;
	}

	// Word and byte transfers, by an immediate or a shifted register offset.
	case FIRST_HANDLER + H_MOVE_IMM:
	case FIRST_HANDLER + H_MOVE_REG: {
		u32 d = FIELD(12, 4);
		if (S.state - FIRST_HANDLER == H_MOVE_IMM) {
			S.offset = FIELD(0, 12);
		} else {
			u32 rm = getR(FIELD(0, 4));
			S.carry = S.cpsr.c;
			S.offset = shiftImmediate(rm, FIELD(5, 2), FIELD(7, 5));
		}
		S.access = FLAG(22) ? BYTE : WORD;
		S.value = getR(d) + (d == 15 ? 4 : 0);
		S.rn = getR(FIELD(16, 4));
		if (FLAG(24)) {
			S.rn = FLAG(23) ? S.rn + S.offset : S.rn - S.offset;
		}
		if (FLAG(20)) {
			LOAD(S.value, S.access, S.rn);
		} else {
			STORE(S.access, S.rn, S.value);
		}
		if (!FLAG(24)) {
			S.rn = FLAG(23) ? S.rn + S.offset : S.rn - S.offset;
		}
		if (!FLAG(24) || FLAG(21)) {
			setR(FIELD(16, 4), S.rn);
		}
		if (FLAG(20)) {
			IDLE();
			setR(FIELD(12, 4), S.value);
		}
		goto done;
	}

	case FIRST_HANDLER + H_MULTIPLE: {
		u32 n = FIELD(16, 4);
		S.list = FIELD(0, 16);
		S.rn = getR(n);
		u32 bitCount = S.list ? (u32) __builtin_popcount(S.list) : 16;
		S.rnEnd = FLAG(23) ? getR(n) + bitCount * 4 : getR(n) - bitCount * 4;
		if (FLAG(24) && FLAG(23)) S.rn = S.rn + 4;
		if (FLAG(24) && !FLAG(23)) S.rn = S.rn - bitCount * 4;
		if (!FLAG(24) && !FLAG(23)) S.rn = S.rn - bitCount * 4 + 4;
		S.cpsrMode = S.cpsr.m;
		S.usr = false;
		if (FLAG(22) && FLAG(20) && !(S.list & 0x8000)) S.usr = true;
		if (FLAG(22) && !FLAG(20)) S.usr = true;
		if (S.usr) {
			S.cpsr.m = USR;
		}
		if (FLAG(21) && FLAG(20) && !(S.list >> n & 1)) {
			setR(n, S.rnEnd);
		}
		S.nonsequential = true;
		if (!S.list) {
			S.list = 0x8000;
		}
		for (S.index = 0; S.index < 16; ++S.index) {
			if (!(S.list >> S.index & 1)) {
				continue;
			}
			if (FLAG(20)) {
				READ(S.value, WORD, S.rn);
				setR(S.index, S.value);
			} else {
				WRITE(WORD, S.rn, getR(S.index) + (S.index == 15 ? 4 : 0));
				// The writeback comes after the first access; a write to the program counter wins.
				if (FLAG(21) && FIELD(16, 4) != 15) {
					setR(FIELD(16, 4), S.rnEnd);
				}
			}
			S.rn += 4;
		}
		if (S.usr) {
			S.cpsr.m = S.cpsrMode;
		}
		if (FLAG(20)) {
			IDLE();
			if (FLAG(22) && (S.list & 0x8000) && S.cpsr.m != USR && S.cpsr.m != SYS) {
				S.cpsr = *spsr();
			}
		} else {
			S.nonsequential = true;
		}
		goto done;
	}

	case FIRST_HANDLER + H_MOV_REG_REG:
		setR(FIELD(12, 4), getR(FIELD(16, 4)));
		goto done;

	case FIRST_HANDLER + H_MRS:
		setR(FIELD(12, 4), FLAG(22) && S.cpsr.m != USR && S.cpsr.m != SYS ? psrValue(spsr()) : psrValue(&S.cpsr));
		goto done;

	case FIRST_HANDLER + H_MSR_IMM: {
		u32 data = FIELD(0, 8);
		if (FIELD(8, 4)) {
			data = ror(data, FIELD(8, 4) << 1);
		}
		moveToStatus(FIELD(16, 4), FLAG(22), data);
		goto done;
	}

	case FIRST_HANDLER + H_MSR_REG: {
		u32 rm = getR(FIELD(0, 4));
		S.carry = S.cpsr.c;
		rm = shiftImmediate(rm, FIELD(5, 2), FIELD(7, 5));
		moveToStatus(FIELD(16, 4), FLAG(22), rm);
		goto done;
	}

	// The multiplier's idle cycles: one, and one more for each byte above the lowest that isn't all
	// copies of the sign.
	case FIRST_HANDLER + H_MUL: {
		u32 multiplier = getR(FIELD(8, 4));
		S.idles = FLAG(21) + 1;
		if (multiplier >> 8 && multiplier >> 8 != 0xffffff) S.idles++;
		if (multiplier >> 16 && multiplier >> 16 != 0xffff) S.idles++;
		if (multiplier >> 24 && multiplier >> 24 != 0xff) S.idles++;
		while (S.idles) {
			S.idles--;
			IDLE();
		}
		u32 product = (FLAG(21) ? getR(FIELD(12, 4)) : 0) + getR(FIELD(0, 4)) * getR(FIELD(8, 4));
		if (SAVE_FLAGS) {
			S.cpsr.z = product == 0;
			S.cpsr.n = product >> 31;
		}
		if (FIELD(16, 4) != 15) {
			setR(FIELD(16, 4), product);
		}
		goto done;
	}

	case FIRST_HANDLER + H_MULL: {
		u64 rs = getR(FIELD(8, 4));
		S.idles = 2 + FLAG(21);
		if (FLAG(22)) {
			if (rs >> 8 && rs >> 8 != 0xffffff) S.idles++;
			if (rs >> 16 && rs >> 16 != 0xffff) S.idles++;
			if (rs >> 24 && rs >> 24 != 0xff) S.idles++;
		} else {
			if (rs >> 8) S.idles++;
			if (rs >> 16) S.idles++;
			if (rs >> 24) S.idles++;
		}
		while (S.idles) {
			S.idles--;
			IDLE();
		}
		u64 rm = getR(FIELD(0, 4));
		rs = getR(FIELD(8, 4));
		if (FLAG(22)) {
			rm = (u64) (s64) (s32) rm;
			rs = (u64) (s64) (s32) rs;
		}
		u64 rd = rm * rs;
		u32 l = FIELD(12, 4), h = FIELD(16, 4);
		if (FLAG(21)) {
			rd += (u64) getR(h) << 32 | getR(l);
		}
		if (l != 15) setR(l, (u32) rd);
		if (h != 15) setR(h, (u32) (rd >> 32));
		if (FLAG(20)) {
			S.cpsr.z = rd == 0;
			S.cpsr.n = rd >> 63;
		}
		goto done;
	}

	case FIRST_HANDLER + H_SWI:
		exception(SVC, 0x08);
		goto done;
	}

done:
	S.state = MAIN;
	goto dispatch;
}

static void syncFromCpu(void) {
	if (S.th.clock < cpu.th.clock) {
		st018Run();
	}
}

// The ARM held in reset: the processor at its reset state, its thread back at the start, the bridge
// cleared. The thread's clock starts again from 0, as ares creates it anew.
static void armReset(void) {
	memset(S.r, 0, sizeof(S.r));
	memset(S.fiq, 0, sizeof(S.fiq));
	memset(S.irqBank, 0, sizeof(S.irqBank));
	memset(S.svcBank, 0, sizeof(S.svcBank));
	memset(S.abtBank, 0, sizeof(S.abtBank));
	memset(S.undBank, 0, sizeof(S.undBank));
	S.rNull = 0;
	memset(&S.cpsr, 0, sizeof(S.cpsr));
	memset(&S.spsrNull, 0, sizeof(S.spsrNull));
	memset(&S.spsrFiq, 0, sizeof(S.spsrFiq));
	memset(&S.spsrIrq, 0, sizeof(S.spsrIrq));
	memset(&S.spsrSvc, 0, sizeof(S.spsrSvc));
	memset(&S.spsrAbt, 0, sizeof(S.spsrAbt));
	memset(&S.spsrUnd, 0, sizeof(S.spsrUnd));
	S.spsrNull.readonly = true;
	memset(&S.pipeline, 0, sizeof(S.pipeline));
	S.pipeline.reload = true;
	S.carry = 0;
	S.irq = 0;
	S.cpsr.f = 1;
	exception(SVC, 0x00);

	S.th.clock = 0;
	S.state = BOOT;
	S.bridge.ready = false;
	S.bridge.signal = false;
	S.bridge.timer = 0;
	S.bridge.timerlatch = 0;
	S.bridge.cputoarmReady = false;
	S.bridge.armtocpuReady = false;
}

// --- The S-CPU's view: 00-3f,80-bf:3800-38ff, 3800-3807 mirrored, A0 ignored ---

u8 st018Read(u32 address, u8 data) {
	(void) data;
	syncFromCpu();
	u8 value = 0x00;
	address &= 0xff06;
	if (address == 0x3800 && S.bridge.armtocpuReady) {
		S.bridge.armtocpuReady = false;
		value = S.bridge.armtocpuData;
	}
	if (address == 0x3802) {
		S.bridge.signal = false;
	}
	if (address == 0x3804) {
		value = status();
	}
	return value;
}

void st018Write(u32 address, u8 data) {
	syncFromCpu();
	address &= 0xff06;
	if (address == 0x3802) {
		S.bridge.cputoarmReady = true;
		S.bridge.cputoarmData = data;
	}
	if (address == 0x3804) {
		data &= 1;
		if (!S.bridge.reset && data) {
			armReset();
		}
		S.bridge.reset = data;
	}
}

// The firmware: 128 KiB of program ROM, then 32 KiB of data ROM.
void st018Load(const u8* firmware) {
	memset(&S, 0, sizeof(S));
	memcpy(programROM, firmware, sizeof(programROM));
	memcpy(dataROM, firmware + sizeof(programROM), sizeof(dataROM));
	armInitialize();
}

// Power and reset both fill the RAM afresh and hold nothing in reset.
void st018Power(void) {
	threadFrequency(&S.th, 21440000);
	randomArray(S.programRAM, sizeof(S.programRAM));
	S.bridge.reset = false;
	armReset();
}
