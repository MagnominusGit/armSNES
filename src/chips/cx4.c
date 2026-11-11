/* Copyright (c) 2004-2025 ares team, Near et al
 * Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Ported from ares (ISC); see LICENSES/ISC-ares.txt. */
#include "../snes.h"

// The Cx4: a Hitachi HG51B with two pages of instruction cache it fills from the cartridge, a DMA
// unit, a data ROM (its firmware) and 3 KiB of data RAM the S-CPU sees too.
//
// It can stop after any clock step, so it is one state machine like the Super FX: a clock step, filling
// the cache, moving the program counter on and a DMA are subroutines with frames on a stack.

struct Cx4 cx4;

static u32 dataROM[1024];

// Whether the HG51B is running: its reads of the ROM always reach it, the S-CPU's don't while it has
// the bus.
static bool active;

enum {
	MAIN,
	SUB_STEP,
	SUB_CACHE,
	SUB_ADVANCE,
	SUB_DMA,
	FIRST_LABEL = 16,
};

#define FR cx4.stack[cx4.depth - 1]

// The arguments are worked out in the caller's frame, before the new one is pushed.
#define CALL_(sub, n, va) \
	do { \
		u32 argA = (va); \
		struct Cx4Frame* frame = &cx4.stack[cx4.depth++]; \
		frame->ret = (n); \
		frame->a = argA; \
		cx4.state = (sub); \
		goto dispatch; \
	case (n):; \
	} while (0)
#define CALL_X(sub, n, va) CALL_(sub, n, va)
#define CALL(sub, va) CALL_X(sub, FIRST_LABEL + __COUNTER__, va)
#define RETURN() \
	do { \
		cx4.state = cx4.stack[--cx4.depth].ret; \
		goto dispatch; \
	} while (0)
#define STEP(clocks) CALL(SUB_STEP, (clocks))

// --- The HG51B's view of the cartridge ---

static u8 readRom(u32 address) {
	address = busMirror(address, cart.romSize);
	return cart.rom[address];
}

static u8 readRam(u32 address) {
	if (!cart.ramSize) {
		return 0x00;
	}
	return cart.ram[busMirror(address, cart.ramSize)];
}

static void writeRam(u32 address, u8 data) {
	if (cart.ramSize) {
		cart.ram[busMirror(address, cart.ramSize)] = data;
	}
}

static u8 readDataRam(u32 address, u8 data) {
	address &= 0xfff;
	if (address >= 0xc00) {
		return data;
	}
	return cx4.dataRAM[address];
}

static void writeDataRam(u32 address, u8 data) {
	address &= 0xfff;
	if (address < 0xc00) {
		cx4.dataRAM[address] = data;
	}
}

// 00-3f,80-bf:8000-ffff and c0-ff:0000-ffff.
static bool isRom(u32 address) {
	return (address & 0x408000) == 0x008000 || (address & 0xc00000) == 0xc00000;
}

// 70-77:0000-7fff.
static bool isRam(u32 address) {
	return (address & 0xf88000) == 0x700000;
}

// 00-3f,80-bf:6000-6bff,7000-7bff.
static bool isDataRam(u32 address) {
	return (address & 0x40e000) == 0x006000 && (address & 0x0c00) != 0x0c00;
}

// 00-3f,80-bf:6c00-6fff,7c00-7fff.
static bool isIO(u32 address) {
	return (address & 0x40ec00) == 0x006c00;
}

static u8 read(u32 address) {
	if (isRom(address)) {
		return readRom(((address & 0x3f0000) >> 1 | (address & 0x7fff)) & 0x1fffff);
	}
	if (isRam(address)) {
		return readRam(((address & 0x070000) >> 1 | (address & 0x7fff)) & 0x03ffff);
	}
	if (isDataRam(address)) {
		return readDataRam(address, 0);
	}
	if (isIO(address)) {
		return cx4ReadIO(address, 0);
	}
	return 0x00;
}

static void write(u32 address, u8 data) {
	if (isRom(address)) {
		return;
	}
	if (isRam(address)) {
		writeRam(((address & 0x070000) >> 1 | (address & 0x7fff)) & 0x03ffff, data);
		return;
	}
	if (isDataRam(address)) {
		writeDataRam(address, data);
		return;
	}
	if (isIO(address)) {
		cx4WriteIO(address, data);
	}
}

// The clocks an access waits.
static u32 waitFor(u32 address) {
	if (isRom(address)) {
		return 1 + cx4.io.waitRom;
	}
	if (isRam(address)) {
		return 1 + cx4.io.waitRam;
	}
	return 0;
}

static bool running(void) {
	return cx4.io.cache.enable || cx4.io.dma.enable || cx4.io.bus.pending || !cx4.io.halt;
}

static bool busy(void) {
	return cx4.io.cache.enable || cx4.io.dma.enable || cx4.io.bus.pending;
}

static void halt(void) {
	cx4.io.halt = true;
	if (cx4.io.irq == 0) {
		cx4.r.i = 1;
		cpu.r.irq = 1;
	}
}

// --- Registers ---

static void setByte(u32* word, u32 byte, u8 data) {
	*word = (*word & ~(0xffu << (byte * 8))) | (u32) data << (byte * 8);
}

static u32 readRegister(u32 address) {
	switch (address & 0x7f) {
	case 0x01: return cx4.r.mul >> 24 & 0xffffff;
	case 0x02: return cx4.r.mul & 0xffffff;
	case 0x03: return cx4.r.mdr;
	case 0x08: return cx4.r.rom;
	case 0x0c: return cx4.r.ram;
	case 0x13: return cx4.r.mar;
	case 0x1c: return cx4.r.dpr;
	case 0x20: return cx4.r.pc;
	case 0x28: return cx4.r.p;
	case 0x2e:
		cx4.io.bus.enable = 1;
		cx4.io.bus.reading = 1;
		cx4.io.bus.pending = cx4.io.waitRom;
		cx4.io.bus.address = cx4.r.mar;
		return 0x000000;
	case 0x2f:
		cx4.io.bus.enable = 1;
		cx4.io.bus.reading = 1;
		cx4.io.bus.pending = cx4.io.waitRam;
		cx4.io.bus.address = cx4.r.mar;
		return 0x000000;

	// Constants.
	case 0x50: return 0x000000;
	case 0x51: return 0xffffff;
	case 0x52: return 0x00ff00;
	case 0x53: return 0xff0000;
	case 0x54: return 0x00ffff;
	case 0x55: return 0xffff00;
	case 0x56: return 0x800000;
	case 0x57: return 0x7fffff;
	case 0x58: return 0x008000;
	case 0x59: return 0x007fff;
	case 0x5a: return 0xff7fff;
	case 0x5b: return 0xffff7f;
	case 0x5c: return 0x010000;
	case 0x5d: return 0xfeffff;
	case 0x5e: return 0x000100;
	case 0x5f: return 0x00feff;
	}
	if ((address & 0x60) == 0x60) {
		return cx4.r.gpr[address & 15];
	}
	return 0x000000;
}

static void writeRegister(u32 address, u32 data) {
	data &= 0xffffff;
	switch (address & 0x7f) {
	case 0x01: cx4.r.mul = (cx4.r.mul & 0xffffff) | (u64) data << 24; return;
	case 0x02: cx4.r.mul = (cx4.r.mul & 0xffffff000000ull) | data; return;
	case 0x03: cx4.r.mdr = data; return;
	case 0x08: cx4.r.rom = data; return;
	case 0x0c: cx4.r.ram = data; return;
	case 0x13: cx4.r.mar = data; return;
	case 0x1c: cx4.r.dpr = data; return;
	case 0x20: cx4.r.pc = data; return;
	case 0x28: cx4.r.p = data & 0x7fff; return;
	case 0x2e:
		cx4.io.bus.enable = 1;
		cx4.io.bus.writing = 1;
		cx4.io.bus.pending = cx4.io.waitRom;
		cx4.io.bus.address = cx4.r.mar;
		return;
	case 0x2f:
		cx4.io.bus.enable = 1;
		cx4.io.bus.writing = 1;
		cx4.io.bus.pending = cx4.io.waitRam;
		cx4.io.bus.address = cx4.r.mar;
		return;
	}
	if ((address & 0x60) == 0x60) {
		cx4.r.gpr[address & 15] = data;
	}
}

// --- The ALU ---

static u32 add(u32 x, u32 y) {
	s32 z = (s32) (x + y);
	cx4.r.n = z & 0x800000;
	cx4.r.z = (z & 0xffffff) == 0;
	cx4.r.c = z > 0xffffff;
	cx4.r.v = ~(x ^ y) & (x ^ z) & 0x800000;
	return z & 0xffffff;
}

// The overflow flag is worked out as for an addition, as ares has it.
static u32 sub(u32 x, u32 y) {
	s32 z = (s32) (x - y);
	cx4.r.n = z & 0x800000;
	cx4.r.z = (z & 0xffffff) == 0;
	cx4.r.c = z >= 0;
	cx4.r.v = ~(x ^ y) & (x ^ z) & 0x800000;
	return z & 0xffffff;
}

static u32 logic(u32 x) {
	x &= 0xffffff;
	cx4.r.n = x & 0x800000;
	cx4.r.z = x == 0;
	return x;
}

static inline s32 signed24(u32 x) {
	return (s32) (x << 8) >> 8;
}

static u32 shiftRight(u32 a, u32 s) {
	s &= 31;
	return logic(a >> (s > 24 ? 0 : s));
}

static u32 shiftRightArithmetic(u32 a, u32 s) {
	s &= 31;
	return logic((u32) (signed24(a) >> (s > 24 ? 0 : s)));
}

static u32 rotateRight(u32 a, u32 s) {
	s &= 31;
	if (s > 24) {
		s = 0;
	}
	return logic(a >> s | a << (24 - s));
}

static u32 shiftLeft(u32 a, u32 s) {
	s &= 31;
	return logic(a << (s > 24 ? 0 : s));
}

static void push(void) {
	for (int n = 7; n > 0; --n) {
		cx4.callStack[n] = cx4.callStack[n - 1];
	}
	cx4.callStack[0] = cx4.r.pb << 8 | cx4.r.pc;
}

static void pull(void) {
	u32 pc = cx4.callStack[0];
	for (int n = 0; n < 7; ++n) {
		cx4.callStack[n] = cx4.callStack[n + 1];
	}
	cx4.callStack[7] = 0;
	cx4.r.pb = pc >> 8 & 0x7fff;
	cx4.r.pc = pc;
}

static u32 dataRamAddress(u32 address) {
	address &= 0xfff;
	return address >= 0xc00 ? address - 0x400 : address;
}

// What an instruction leaves to do: nothing, a step of some clocks, or moving past the next
// instruction and a step.
enum { TAIL_NONE = -1, TAIL_SKIP = -2 };

static int instruction(u32 op) {
	static const u8 shifts[4] = { 0, 1, 8, 16 };
	u32 shift = shifts[op >> 8 & 3];
	u32 reg = op & 0x7f;
	u32 imm = op & 0xff;
	u32 a = cx4.r.a << shift & 0xffffff;
	bool take;

	switch (op >> 10) {
	case 0x02: take = true; goto jump;
	case 0x03: take = cx4.r.z; goto jump;
	case 0x04: take = cx4.r.c; goto jump;
	case 0x05: take = cx4.r.n; goto jump;
	case 0x06: take = cx4.r.v; goto jump;
	jump:
		if (!take) {
			return TAIL_NONE;
		}
		if (op & 0x200) {
			cx4.r.pb = cx4.r.p;
		}
		cx4.r.pc = imm;
		return 2;
	case 0x07:
		if (!cx4.io.bus.enable) {
			return TAIL_NONE;
		}
		return cx4.io.bus.pending;
	case 0x09: {
		const bool flags[4] = { cx4.r.v, cx4.r.c, cx4.r.z, cx4.r.n };
		return flags[op >> 8 & 3] != (op & 1) ? TAIL_NONE : TAIL_SKIP;
	}
	case 0x0a: take = true; goto call;
	case 0x0b: take = cx4.r.z; goto call;
	case 0x0c: take = cx4.r.c; goto call;
	case 0x0d: take = cx4.r.n; goto call;
	case 0x0e: take = cx4.r.v; goto call;
	call:
		if (!take) {
			return TAIL_NONE;
		}
		push();
		if (op & 0x200) {
			cx4.r.pb = cx4.r.p;
		}
		cx4.r.pc = imm;
		return 2;
	case 0x0f:
		pull();
		return 2;
	case 0x10:
		cx4.r.mar = (cx4.r.mar + 1) & 0xffffff;
		return TAIL_NONE;
	case 0x12: sub(readRegister(reg), a); return TAIL_NONE;
	case 0x13: sub(imm, a); return TAIL_NONE;
	case 0x14: sub(a, readRegister(reg)); return TAIL_NONE;
	case 0x15: sub(a, imm); return TAIL_NONE;
	case 0x16:
		if ((op >> 8) == 0x59) {
			cx4.r.a = logic((u32) (s8) cx4.r.a);
		} else if ((op >> 8) == 0x5a) {
			cx4.r.a = logic((u32) (s16) cx4.r.a);
		}
		return TAIL_NONE;
	case 0x18:
		switch (op >> 8) {
		case 0x60: cx4.r.a = readRegister(reg); break;
		case 0x61: cx4.r.mdr = readRegister(reg); break;
		case 0x62: cx4.r.mar = readRegister(reg); break;
		case 0x63: cx4.r.p = cx4.r.gpr[op & 15] & 0x7fff; break;
		}
		return TAIL_NONE;
	case 0x19:
		switch (op >> 8) {
		case 0x64: cx4.r.a = imm; break;
		case 0x65: cx4.r.mdr = imm; break;
		case 0x66: cx4.r.mar = imm; break;
		case 0x67: cx4.r.p = imm; break;
		}
		return TAIL_NONE;
	case 0x1a:
	case 0x1b: {
		u32 byte = op >> 8 & 3;
		if (byte == 3) {
			return TAIL_NONE;
		}
		u32 address = dataRamAddress(op & 0x400 ? cx4.r.dpr + imm : cx4.r.a);
		setByte(&cx4.r.ram, byte, cx4.dataRAM[address]);
		return TAIL_NONE;
	}
	case 0x1c: cx4.r.rom = dataROM[cx4.r.a & 0x3ff]; return TAIL_NONE;
	case 0x1d: cx4.r.rom = dataROM[op & 0x3ff]; return TAIL_NONE;
	case 0x1f:
		if ((op >> 8) == 0x7c) {
			cx4.r.p = (cx4.r.p & 0x7f00) | imm;
		} else if ((op >> 8) == 0x7d) {
			cx4.r.p = (cx4.r.p & 0x00ff) | (op & 0x7f) << 8;
		}
		return TAIL_NONE;
	case 0x20: cx4.r.a = add(a, readRegister(reg)); return TAIL_NONE;
	case 0x21: cx4.r.a = add(a, imm); return TAIL_NONE;
	case 0x22: cx4.r.a = sub(readRegister(reg), a); return TAIL_NONE;
	case 0x23: cx4.r.a = sub(imm, a); return TAIL_NONE;
	case 0x24: cx4.r.a = sub(a, readRegister(reg)); return TAIL_NONE;
	case 0x25: cx4.r.a = sub(a, imm); return TAIL_NONE;
	case 0x26:
		cx4.r.mul = (u64) ((s64) signed24(cx4.r.a) * signed24(readRegister(reg))) & 0xffffffffffffull;
		return TAIL_NONE;
	case 0x27:
		cx4.r.mul = (u64) ((s64) signed24(cx4.r.a) * (s64) imm) & 0xffffffffffffull;
		return TAIL_NONE;
	case 0x28: cx4.r.a = logic(~a ^ readRegister(reg)); return TAIL_NONE;
	case 0x29: cx4.r.a = logic(~a ^ imm); return TAIL_NONE;
	case 0x2a: cx4.r.a = logic(a ^ readRegister(reg)); return TAIL_NONE;
	case 0x2b: cx4.r.a = logic(a ^ imm); return TAIL_NONE;
	case 0x2c: cx4.r.a = logic(a & readRegister(reg)); return TAIL_NONE;
	case 0x2d: cx4.r.a = logic(a & imm); return TAIL_NONE;
	case 0x2e: cx4.r.a = logic(a | readRegister(reg)); return TAIL_NONE;
	case 0x2f: cx4.r.a = logic(a | imm); return TAIL_NONE;
	case 0x30: cx4.r.a = shiftRight(cx4.r.a, readRegister(reg)); return TAIL_NONE;
	case 0x31: cx4.r.a = shiftRight(cx4.r.a, op & 0x1f); return TAIL_NONE;
	case 0x32: cx4.r.a = shiftRightArithmetic(cx4.r.a, readRegister(reg)); return TAIL_NONE;
	case 0x33: cx4.r.a = shiftRightArithmetic(cx4.r.a, op & 0x1f); return TAIL_NONE;
	case 0x34: cx4.r.a = rotateRight(cx4.r.a, readRegister(reg)); return TAIL_NONE;
	case 0x35: cx4.r.a = rotateRight(cx4.r.a, op & 0x1f); return TAIL_NONE;
	case 0x36: cx4.r.a = shiftLeft(cx4.r.a, readRegister(reg)); return TAIL_NONE;
	case 0x37: cx4.r.a = shiftLeft(cx4.r.a, op & 0x1f); return TAIL_NONE;
	case 0x38:
		if ((op >> 8) == 0xe0) {
			writeRegister(reg, cx4.r.a);
		} else if ((op >> 8) == 0xe1) {
			writeRegister(reg, cx4.r.mdr);
		}
		return TAIL_NONE;
	case 0x3a:
	case 0x3b: {
		u32 byte = op >> 8 & 3;
		if (byte == 3) {
			return TAIL_NONE;
		}
		u32 address = dataRamAddress(op & 0x400 ? cx4.r.dpr + imm : cx4.r.a);
		cx4.dataRAM[address] = cx4.r.ram >> (byte * 8);
		return TAIL_NONE;
	}
	case 0x3c: {
		u32 swap = cx4.r.a;
		cx4.r.a = cx4.r.gpr[op & 15];
		cx4.r.gpr[op & 15] = swap;
		return TAIL_NONE;
	}
	case 0x3e:
		cx4.r.a = 0;
		cx4.r.p = 0;
		cx4.r.ram = 0;
		cx4.r.dpr = 0;
		return TAIL_NONE;
	case 0x3f:
		halt();
		return TAIL_NONE;
	}
	return TAIL_NONE;
}

// --- The thread ---

void cx4Run(void) {
	int tail;
	active = true;

dispatch:
	switch (cx4.state) {
	case MAIN:
		if (cx4.io.lock) {
			STEP(1);
			goto done;
		}
		if (cx4.io.suspendEnable) {
			if (!cx4.io.suspendDuration) {
				STEP(1);
				goto done;
			}
			STEP(cx4.io.suspendDuration);
			cx4.io.suspendDuration = 0;
			cx4.io.suspendEnable = 0;
			goto done;
		}
		if (cx4.io.cache.enable) {
			CALL(SUB_CACHE, 0);
			goto done;
		}
		if (cx4.io.dma.enable) {
			CALL(SUB_DMA, 0);
			goto done;
		}
		if (cx4.io.halt) {
			STEP(1);
			goto done;
		}

		CALL(SUB_CACHE, 0);
		if (!cx4.result) {
			halt();
			goto done;
		}
		cx4.opcode = cx4.programRAM[cx4.io.cache.page][cx4.r.pc];
		CALL(SUB_ADVANCE, 0);
		STEP(1);
		tail = instruction(cx4.opcode);
		if (tail == TAIL_SKIP) {
			CALL(SUB_ADVANCE, 0);
			STEP(1);
		} else if (tail != TAIL_NONE) {
			STEP(tail);
		}
		goto done;

	// A clock step, a: the clocks. A pending bus access finishes when its time is up.
	case SUB_STEP:
		if (cx4.io.bus.enable) {
			if (cx4.io.bus.pending > FR.a) {
				cx4.io.bus.pending -= FR.a;
			} else {
				cx4.io.bus.enable = 0;
				cx4.io.bus.pending = 0;
				if (cx4.io.bus.reading) {
					cx4.io.bus.reading = 0;
					cx4.r.mdr = read(cx4.io.bus.address);
				}
				if (cx4.io.bus.writing) {
					cx4.io.bus.writing = 0;
					write(cx4.io.bus.address, cx4.r.mdr);
				}
			}
		}
		cx4.th.clock += cx4.th.scalar * FR.a;
		if (cpu.th.clock < cx4.th.clock) {
			cx4.state = FIRST_LABEL - 1;
			active = false;
			return;
		case FIRST_LABEL - 1:;
		}
		RETURN();

	// The page of program the program bank is in, loaded into the cache when no page has it. The
	// result is whether it could be. a: its address, b: the word being loaded.
	case SUB_CACHE:
		FR.a = (cx4.io.cache.base + cx4.r.pb * 512) & 0xffffff;
		if (cx4.io.cache.preload == 0) {
			if (cx4.io.cache.address[cx4.io.cache.page] == FR.a) {
				cx4.io.cache.enable = 0;
				cx4.result = true;
				RETURN();
			}
			cx4.io.cache.page ^= 1;
			if (cx4.io.cache.address[cx4.io.cache.page] == FR.a) {
				cx4.io.cache.enable = 0;
				cx4.result = true;
				RETURN();
			}
			if (cx4.io.cache.lock[cx4.io.cache.page]) {
				cx4.io.cache.page ^= 1;
			}
			if (cx4.io.cache.lock[cx4.io.cache.page]) {
				cx4.io.cache.enable = 0;
				cx4.result = false;
				RETURN();
			}
		}
		cx4.io.cache.preload = 0;
		cx4.io.cache.address[cx4.io.cache.page] = FR.a;
		for (FR.b = 0; FR.b < 256; ++FR.b) {
			STEP(waitFor(FR.a));
			cx4.programRAM[cx4.io.cache.page][FR.b] = (cx4.programRAM[cx4.io.cache.page][FR.b] & 0xff00) | read(FR.a);
			FR.a = (FR.a + 1) & 0xffffff;
			STEP(waitFor(FR.a));
			cx4.programRAM[cx4.io.cache.page][FR.b] = (cx4.programRAM[cx4.io.cache.page][FR.b] & 0x00ff) | read(FR.a) << 8;
			FR.a = (FR.a + 1) & 0xffffff;
		}
		cx4.io.cache.enable = 0;
		cx4.result = true;
		RETURN();

	// The program counter moved on, into the second page of the cache at the end of the first.
	case SUB_ADVANCE:
		cx4.r.pc++;
		if (cx4.r.pc == 0) {
			if (cx4.io.cache.page == 1) {
				halt();
				RETURN();
			}
			cx4.io.cache.page = 1;
			if (cx4.io.cache.lock[cx4.io.cache.page]) {
				halt();
				RETURN();
			}
			cx4.r.pb = cx4.r.p;
			CALL(SUB_CACHE, 0);
			if (!cx4.result) {
				halt();
			}
		}
		RETURN();

	// A DMA. a: the byte, b: the offset, c: the length, d: the source, e: the target. Between two ROM
	// or two RAM addresses it locks up.
	case SUB_DMA:
		FR.c = cx4.io.dma.length;
		for (FR.b = 0; FR.b < FR.c; ++FR.b) {
			FR.d = (cx4.io.dma.source + FR.b) & 0xffffff;
			FR.e = (cx4.io.dma.target + FR.b) & 0xffffff;
			if ((isRom(FR.d) && isRom(FR.e)) || (isRam(FR.d) && isRam(FR.e))) {
				cx4.io.lock = true;
				RETURN();
			}
			STEP(waitFor(FR.d));
			FR.a = read(FR.d);
			STEP(waitFor(FR.e));
			write(FR.e, FR.a);
		}
		cx4.io.dma.enable = 0;
		RETURN();
	}

done:
	cx4.state = MAIN;
	goto dispatch;
}

// --- The S-CPU's view ---

u8 cx4ReadIO(u32 address, u8 data) {
	(void) data;
	address = 0x7c00 | (address & 0x3ff);
	switch (address) {
	case 0x7f40: return cx4.io.dma.source;
	case 0x7f41: return cx4.io.dma.source >> 8;
	case 0x7f42: return cx4.io.dma.source >> 16;
	case 0x7f43: return cx4.io.dma.length;
	case 0x7f44: return cx4.io.dma.length >> 8;
	case 0x7f45: return cx4.io.dma.target;
	case 0x7f46: return cx4.io.dma.target >> 8;
	case 0x7f47: return cx4.io.dma.target >> 16;
	case 0x7f48: return cx4.io.cache.page;
	case 0x7f49: return cx4.io.cache.base;
	case 0x7f4a: return cx4.io.cache.base >> 8;
	case 0x7f4b: return cx4.io.cache.base >> 16;
	case 0x7f4c: return cx4.io.cache.lock[0] << 0 | cx4.io.cache.lock[1] << 1;
	case 0x7f4d: return cx4.io.cache.pb;
	case 0x7f4e: return cx4.io.cache.pb >> 8;
	case 0x7f4f: return cx4.io.cache.pc;
	case 0x7f50: return cx4.io.waitRam << 0 | cx4.io.waitRom << 4;
	case 0x7f51: return cx4.io.irq;
	case 0x7f52: return cx4.io.rom;
	case 0x7f53: case 0x7f54: case 0x7f55: case 0x7f56: case 0x7f57:
	case 0x7f59: case 0x7f5b: case 0x7f5c: case 0x7f5d: case 0x7f5e:
	case 0x7f5f:
		return cx4.io.suspendEnable << 0 | cx4.r.i << 1 | running() << 6 | busy() << 7;
	}
	if (address >= 0x7f60 && address <= 0x7f7f) {
		return cx4.io.vector[address & 0x1f];
	}
	if ((address >= 0x7f80 && address <= 0x7faf) || (address >= 0x7fc0 && address <= 0x7fef)) {
		address &= 0x3f;
		return cx4.r.gpr[address / 3] >> (address % 3 * 8);
	}
	return 0x00;
}

void cx4WriteIO(u32 address, u8 data) {
	address = 0x7c00 | (address & 0x3ff);
	switch (address) {
	case 0x7f40: setByte(&cx4.io.dma.source, 0, data); return;
	case 0x7f41: setByte(&cx4.io.dma.source, 1, data); return;
	case 0x7f42: setByte(&cx4.io.dma.source, 2, data); return;
	case 0x7f43: cx4.io.dma.length = (cx4.io.dma.length & 0xff00) | data; return;
	case 0x7f44: cx4.io.dma.length = (cx4.io.dma.length & 0x00ff) | data << 8; return;
	case 0x7f45: setByte(&cx4.io.dma.target, 0, data); return;
	case 0x7f46: setByte(&cx4.io.dma.target, 1, data); return;
	case 0x7f47:
		setByte(&cx4.io.dma.target, 2, data);
		if (cx4.io.halt) {
			cx4.io.dma.enable = 1;
		}
		return;
	case 0x7f48:
		cx4.io.cache.page = data & 1;
		if (cx4.io.halt) {
			cx4.r.pb = cx4.io.cache.pb;
			cx4.io.cache.preload = 1;
			cx4.io.cache.enable = 1;
		}
		return;
	case 0x7f49: setByte(&cx4.io.cache.base, 0, data); return;
	case 0x7f4a: setByte(&cx4.io.cache.base, 1, data); return;
	case 0x7f4b: setByte(&cx4.io.cache.base, 2, data); return;
	case 0x7f4c:
		cx4.io.cache.lock[0] = data & 1;
		cx4.io.cache.lock[1] = data >> 1 & 1;
		return;
	case 0x7f4d: cx4.io.cache.pb = (cx4.io.cache.pb & 0x7f00) | data; return;
	case 0x7f4e: cx4.io.cache.pb = (cx4.io.cache.pb & 0x00ff) | (data & 0x7f) << 8; return;
	case 0x7f4f:
		cx4.io.cache.pc = data;
		if (cx4.io.halt) {
			cx4.io.halt = 0;
			cx4.r.pb = cx4.io.cache.pb;
			cx4.r.pc = cx4.io.cache.pc;
		}
		return;
	case 0x7f50:
		cx4.io.waitRam = data & 7;
		cx4.io.waitRom = data >> 4 & 7;
		return;
	case 0x7f51:
		cx4.io.irq = data & 1;
		if (cx4.io.irq == 1) {
			cx4.r.i = 0;
			cpu.r.irq = 0;
		}
		return;
	case 0x7f52:
		cx4.io.rom = data & 1;
		return;
	case 0x7f53:
		cx4.io.lock = 0;
		cx4.io.halt = 1;
		return;
	case 0x7f55: cx4.io.suspendEnable = 1; cx4.io.suspendDuration = 0; return;
	case 0x7f56: cx4.io.suspendEnable = 1; cx4.io.suspendDuration = 32; return;
	case 0x7f57: cx4.io.suspendEnable = 1; cx4.io.suspendDuration = 64; return;
	case 0x7f58: cx4.io.suspendEnable = 1; cx4.io.suspendDuration = 96; return;
	case 0x7f59: cx4.io.suspendEnable = 1; cx4.io.suspendDuration = 128; return;
	case 0x7f5a: cx4.io.suspendEnable = 1; cx4.io.suspendDuration = 160; return;
	case 0x7f5b: cx4.io.suspendEnable = 1; cx4.io.suspendDuration = 192; return;
	case 0x7f5c: cx4.io.suspendEnable = 1; cx4.io.suspendDuration = 224; return;
	case 0x7f5d: cx4.io.suspendEnable = 0; return;
	// The S-CPU's IRQ line stays as it is.
	case 0x7f5e: cx4.r.i = 0; return;
	}
	if (address >= 0x7f60 && address <= 0x7f7f) {
		cx4.io.vector[address & 0x1f] = data;
		return;
	}
	if ((address >= 0x7f80 && address <= 0x7faf) || (address >= 0x7fc0 && address <= 0x7fef)) {
		address &= 0x3f;
		setByte(&cx4.r.gpr[address / 3], address % 3, data);
	}
}

// While the HG51B has the bus, the S-CPU reads its registers at 00:ffc0-ffff, vectors included.
u8 cx4ReadRomCPU(u32 address, u8 data) {
	if (active || !busy()) {
		return readRom(address);
	}
	if ((address & 0xbfffc0) == 0x007fc0) {
		return cx4ReadIO(0x7f40 | (address & 0x3f), 0);
	}
	return data;
}

void cx4WriteRomCPU(u32 address, u8 data) {
	(void) address;
	(void) data;
}

u8 cx4ReadRamCPU(u32 address, u8 data) {
	(void) data;
	return readRam(address);
}

void cx4WriteRamCPU(u32 address, u8 data) {
	writeRam(address, data);
}

u8 cx4ReadDataRamCPU(u32 address, u8 data) {
	return readDataRam(address, data);
}

void cx4WriteDataRamCPU(u32 address, u8 data) {
	writeDataRam(address, data);
}

// The data ROM: 1024 words of three bytes, low byte first.
void cx4Load(const u8* firmware) {
	memset(&cx4, 0, sizeof(cx4));
	for (u32 n = 0; n < 1024; ++n) {
		dataROM[n] = firmware[n * 3] | firmware[n * 3 + 1] << 8 | firmware[n * 3 + 2] << 16;
	}
}

// A reset leaves the cache, the call stack and the data RAM as they were.
void cx4Power(void) {
	threadFrequency(&cx4.th, 20000000);
	cx4.state = MAIN;
	cx4.depth = 0;
	memset(&cx4.r, 0, sizeof(cx4.r));
	memset(&cx4.io, 0, sizeof(cx4.io));
	cx4.io.halt = 1;
	cx4.io.rom = 1;
	cx4.io.waitRom = 3;
	cx4.io.waitRam = 3;
}
