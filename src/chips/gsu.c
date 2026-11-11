/* Copyright (c) 2004-2025 ares team, Near et al
 * Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Ported from ares (ISC); see LICENSES/ISC-ares.txt. */
#include "../snes.h"

// The Super FX: a GSU drawing into the cartridge's RAM, with an instruction cache, ROM and RAM
// buffers and a pixel cache.
//
// Like the SA-1 it can stop after any clock step and carry on later, so it is one state machine. Its
// helpers (a clock step, which can finish a buffered ROM read or RAM write; a read, which can wait in
// steps for the bus; fetching an opcode through the cache; plotting) are subroutines in it, each
// with a frame on a stack for its return state, its arguments and its locals.

struct Gsu gsu;

enum { SFR_Z = 1, SFR_CY = 2, SFR_S = 3, SFR_OV = 4, SFR_G = 5, SFR_R = 6, SFR_ALT1 = 8, SFR_ALT2 = 9, SFR_B = 12, SFR_IRQ = 15 };

#define FLAG(bit) (gsu.sfr >> (bit) & 1)
#define SET_FLAG(bit, value) (gsu.sfr = (gsu.sfr & ~(1 << (bit))) | ((value) ? 1 << (bit) : 0))

static inline void setR(u32 n, u32 value) {
	gsu.r[n] = value;
	gsu.modified[n] = true;
}

#define SR() gsu.r[gsu.sreg]
#define SET_DR(value) setR(gsu.dreg, (value))
#define DR() gsu.r[gsu.dreg]

static void regsReset(void) {
	SET_FLAG(SFR_B, 0);
	SET_FLAG(SFR_ALT1, 0);
	SET_FLAG(SFR_ALT2, 0);
	gsu.sreg = 0;
	gsu.dreg = 0;
}

static void setSCMR(u8 data) {
	gsu.scmr.ht = (bool) (data & 0x20) << 1;
	gsu.scmr.ht |= (bool) (data & 0x04) << 0;
	gsu.scmr.ron = data & 0x10;
	gsu.scmr.ran = data & 0x08;
	gsu.scmr.md = data & 0x03;
}

static void setPOR(u8 data) {
	gsu.por.obj = data & 0x10;
	gsu.por.freezehigh = data & 0x08;
	gsu.por.highnibble = data & 0x04;
	gsu.por.dither = data & 0x02;
	gsu.por.transparent = data & 0x01;
}

static void setCFGR(u8 data) {
	gsu.cfgr.irq = data & 0x80;
	gsu.cfgr.ms0 = data & 0x20;
}

// The ROM's size, rounded up to a power of two, less one; and the RAM's.
static u32 romMask;
static u32 ramMask;

static inline u8 romAt(u32 address) {
	address &= romMask;
	return address < cart.romSize ? cart.rom[address] : 0;
}

static void flushCache(void) {
	memset(gsu.cacheValid, 0, sizeof(gsu.cacheValid));
}

static u8 readCache(u16 address) {
	return gsu.cache[(address + gsu.cbr) & 511];
}

static void writeCache(u16 address, u8 data) {
	address = (address + gsu.cbr) & 511;
	gsu.cache[address] = data;
	if ((address & 15) == 15) {
		gsu.cacheValid[address >> 4] = true;
	}
}

static void updateROMBuffer(void) {
	SET_FLAG(SFR_R, 1);
	gsu.romcl = gsu.clsr ? 5 : 6;
}

static u8 color(u8 source) {
	if (gsu.por.highnibble) {
		return (gsu.colr & 0xf0) | (source >> 4);
	}
	if (gsu.por.freezehigh) {
		return (gsu.colr & 0xf0) | (source & 0x0f);
	}
	return source;
}

// The character a pixel is in, and where its row of that character starts in RAM.
static u32 pixelAddress(u8 x, u8 y, u32* bpp) {
	u32 cn = 0;
	switch (gsu.por.obj ? 3 : gsu.scmr.ht) {
	case 0: cn = ((x & 0xf8) << 1) + ((y & 0xf8) >> 3); break;
	case 1: cn = ((x & 0xf8) << 1) + ((x & 0xf8) >> 1) + ((y & 0xf8) >> 3); break;
	case 2: cn = ((x & 0xf8) << 1) + ((x & 0xf8) << 0) + ((y & 0xf8) >> 3); break;
	case 3: cn = ((y & 0x80) << 2) + ((x & 0x80) << 1) + ((y & 0x78) << 1) + ((x & 0x78) >> 3); break;
	}
	*bpp = 2 << (gsu.scmr.md - (gsu.scmr.md >> 1));
	return 0x700000 + (cn * (*bpp << 3)) + (gsu.scbr << 10) + ((y & 0x07) * 2);
}

// The state machine

enum {
	MAIN = 256,
	SUB_STEP,
	SUB_READ,
	SUB_WRITE,
	SUB_READ_OPCODE,
	SUB_FLUSH,
	SUB_PLOT,
	SUB_RPIX,
	FIRST_LABEL = 512,
};

// The frame of the subroutine running.
#define FR gsu.stack[gsu.depth - 1]

// The arguments are worked out in the caller's frame, before the new one is pushed.
#define CALL_(sub, n, va, vb, vc) \
	do { \
		u32 argA = (va), argB = (vb), argC = (vc); \
		struct GsuFrame* frame = &gsu.stack[gsu.depth++]; \
		frame->ret = (n); \
		frame->a = argA; \
		frame->b = argB; \
		frame->c = argC; \
		gsu.state = (sub); \
		goto dispatch; \
	case (n):; \
	} while (0)
#define CALL_X(sub, n, va, vb, vc) CALL_(sub, n, va, vb, vc)
#define CALL(sub, va, vb, vc) CALL_X(sub, FIRST_LABEL + __COUNTER__, va, vb, vc)
#define RETURN() \
	do { \
		gsu.state = gsu.stack[--gsu.depth].ret; \
		goto dispatch; \
	} while (0)

#define STEP(clocks) CALL(SUB_STEP, (clocks), 0, 0)
#define READ(target, address) \
	do { \
		CALL(SUB_READ, (address), 0, 0); \
		target = gsu.result; \
	} while (0)
#define WRITE(address, data) CALL(SUB_WRITE, (address), (data), 0)
#define SYNC_ROM_BUFFER() \
	do { \
		if (gsu.romcl) { \
			STEP(gsu.romcl); \
		} \
	} while (0)
#define SYNC_RAM_BUFFER() \
	do { \
		if (gsu.ramcl) { \
			STEP(gsu.ramcl); \
		} \
	} while (0)
#define READ_ROM_BUFFER(target) \
	do { \
		SYNC_ROM_BUFFER(); \
		target = gsu.romdr; \
	} while (0)
#define READ_RAM_BUFFER(target, address) \
	do { \
		SYNC_RAM_BUFFER(); \
		READ(target, 0x700000 + (gsu.rambr << 16) + (u16) (address)); \
	} while (0)
#define WRITE_RAM_BUFFER(address, data) \
	do { \
		SYNC_RAM_BUFFER(); \
		gsu.ramcl = gsu.clsr ? 5 : 6; \
		gsu.ramar = (address); \
		gsu.ramdr = (data); \
	} while (0)
// The next byte of the program: the pipeline's, and the one after it fetched into the pipeline.
#define PIPE(target) \
	do { \
		gsu.pipeOut = gsu.pipeline; \
		setR(15, gsu.r[15] + 1); \
		CALL(SUB_READ_OPCODE, gsu.r[15], 0, 0); \
		target = gsu.pipeOut; \
	} while (0)

void gsuRun(void) {
	u32 n;

dispatch:
	switch (gsu.state) {
	case MAIN:
		if (FLAG(SFR_G) == 0) {
			STEP(6);
			goto done;
		}
		// The opcode from the pipeline, and the next byte fetched into it.
		gsu.opcode = gsu.pipeline;
		CALL(SUB_READ_OPCODE, gsu.r[15], 0, 0);
		gsu.state = gsu.opcode;
		goto dispatch;

	// $00 stop
	case 0x00:
		if (gsu.cfgr.irq == 0) {
			SET_FLAG(SFR_IRQ, 1);
			cpu.r.irq = 1;
		}
		SET_FLAG(SFR_G, 0);
		gsu.pipeline = 0x01;
		regsReset();
		goto next;

	// $01 nop
	case 0x01:
		regsReset();
		goto next;

	// $02 cache
	case 0x02:
		if (gsu.cbr != (gsu.r[15] & 0xfff0)) {
			gsu.cbr = gsu.r[15] & 0xfff0;
			flushCache();
		}
		regsReset();
		goto next;

	// $03 lsr
	case 0x03:
		SET_FLAG(SFR_CY, SR() & 1);
		SET_DR(SR() >> 1);
		SET_FLAG(SFR_S, DR() & 0x8000);
		SET_FLAG(SFR_Z, DR() == 0);
		regsReset();
		goto next;

	// $04 rol
	case 0x04: {
		bool carry = SR() & 0x8000;
		SET_DR((SR() << 1) | FLAG(SFR_CY));
		SET_FLAG(SFR_S, DR() & 0x8000);
		SET_FLAG(SFR_CY, carry);
		SET_FLAG(SFR_Z, DR() == 0);
		regsReset();
		goto next;
	}

	// $05-$0f branches
	case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0a:
	case 0x0b: case 0x0c: case 0x0d: case 0x0e: case 0x0f:
		PIPE(gsu.result);
		{
			bool s = FLAG(SFR_S), ov = FLAG(SFR_OV), z = FLAG(SFR_Z), cy = FLAG(SFR_CY);
			bool take = false;
			switch (gsu.opcode) {
			case 0x05: take = 1; break;
			case 0x06: take = (s ^ ov) == 0; break;
			case 0x07: take = (s ^ ov) == 1; break;
			case 0x08: take = z == 0; break;
			case 0x09: take = z == 1; break;
			case 0x0a: take = s == 0; break;
			case 0x0b: take = s == 1; break;
			case 0x0c: take = cy == 0; break;
			case 0x0d: take = cy == 1; break;
			case 0x0e: take = ov == 0; break;
			case 0x0f: take = ov == 1; break;
			}
			if (take) {
				setR(15, gsu.r[15] + (s8) gsu.result);
			}
		}
		goto next;

	// $10-$1f to rN / move rN
	case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
	case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d: case 0x1e: case 0x1f:
		n = gsu.opcode & 15;
		if (!FLAG(SFR_B)) {
			gsu.dreg = n;
		} else {
			setR(n, SR());
			regsReset();
		}
		goto next;

	// $20-$2f with rN
	case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
	case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x2c: case 0x2d: case 0x2e: case 0x2f:
		gsu.sreg = gsu.opcode & 15;
		gsu.dreg = gsu.opcode & 15;
		SET_FLAG(SFR_B, 1);
		goto next;

	// $30-$3b stw / stb (rN)
	case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35:
	case 0x36: case 0x37: case 0x38: case 0x39: case 0x3a: case 0x3b:
		gsu.ramaddr = gsu.r[gsu.opcode & 15];
		WRITE_RAM_BUFFER(gsu.ramaddr, SR());
		if (!FLAG(SFR_ALT1)) {
			WRITE_RAM_BUFFER(gsu.ramaddr ^ 1, SR() >> 8);
		}
		regsReset();
		goto next;

	// $3c loop
	case 0x3c:
		setR(12, gsu.r[12] - 1);
		SET_FLAG(SFR_S, gsu.r[12] & 0x8000);
		SET_FLAG(SFR_Z, gsu.r[12] == 0);
		if (!FLAG(SFR_Z)) {
			setR(15, gsu.r[13]);
		}
		regsReset();
		goto next;

	// $3d-$3f alt1, alt2, alt3
	case 0x3d:
		SET_FLAG(SFR_B, 0);
		SET_FLAG(SFR_ALT1, 1);
		goto next;
	case 0x3e:
		SET_FLAG(SFR_B, 0);
		SET_FLAG(SFR_ALT2, 1);
		goto next;
	case 0x3f:
		SET_FLAG(SFR_B, 0);
		SET_FLAG(SFR_ALT1, 1);
		SET_FLAG(SFR_ALT2, 1);
		goto next;

	// $40-$4b ldw / ldb (rN)
	case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45:
	case 0x46: case 0x47: case 0x48: case 0x49: case 0x4a: case 0x4b:
		gsu.ramaddr = gsu.r[gsu.opcode & 15];
		READ_RAM_BUFFER(gsu.result, gsu.ramaddr);
		SET_DR((u8) gsu.result);
		if (!FLAG(SFR_ALT1)) {
			READ_RAM_BUFFER(gsu.result, gsu.ramaddr ^ 1);
			SET_DR(DR() | (u8) gsu.result << 8);
		}
		regsReset();
		goto next;

	// $4c plot / rpix
	case 0x4c:
		if (!FLAG(SFR_ALT1)) {
			CALL(SUB_PLOT, gsu.r[1], gsu.r[2], 0);
			setR(1, gsu.r[1] + 1);
		} else {
			CALL(SUB_RPIX, gsu.r[1], gsu.r[2], 0);
			SET_DR(gsu.result);
			SET_FLAG(SFR_S, DR() & 0x8000);
			SET_FLAG(SFR_Z, DR() == 0);
		}
		regsReset();
		goto next;

	// $4d swap
	case 0x4d:
		SET_DR(SR() >> 8 | SR() << 8);
		SET_FLAG(SFR_S, DR() & 0x8000);
		SET_FLAG(SFR_Z, DR() == 0);
		regsReset();
		goto next;

	// $4e color / cmode
	case 0x4e:
		if (!FLAG(SFR_ALT1)) {
			gsu.colr = color(SR());
		} else {
			setPOR(SR());
		}
		regsReset();
		goto next;

	// $4f not
	case 0x4f:
		SET_DR(~SR());
		SET_FLAG(SFR_S, DR() & 0x8000);
		SET_FLAG(SFR_Z, DR() == 0);
		regsReset();
		goto next;

	// $50-$5f add / adc / add # / adc #
	case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
	case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f: {
		n = gsu.opcode & 15;
		if (!FLAG(SFR_ALT2)) {
			n = gsu.r[n];
		}
		s32 r = SR() + n + (FLAG(SFR_ALT1) ? FLAG(SFR_CY) : 0);
		SET_FLAG(SFR_OV, ~(SR() ^ n) & (n ^ r) & 0x8000);
		SET_FLAG(SFR_S, r & 0x8000);
		SET_FLAG(SFR_CY, r >= 0x10000);
		SET_FLAG(SFR_Z, (u16) r == 0);
		SET_DR(r);
		regsReset();
		goto next;
	}

	// $60-$6f sub / sbc / sub # / cmp
	case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:
	case 0x68: case 0x69: case 0x6a: case 0x6b: case 0x6c: case 0x6d: case 0x6e: case 0x6f: {
		n = gsu.opcode & 15;
		if (!FLAG(SFR_ALT2) || FLAG(SFR_ALT1)) {
			n = gsu.r[n];
		}
		s32 r = SR() - n - (!FLAG(SFR_ALT2) && FLAG(SFR_ALT1) ? !FLAG(SFR_CY) : 0);
		SET_FLAG(SFR_OV, (SR() ^ n) & (SR() ^ r) & 0x8000);
		SET_FLAG(SFR_S, r & 0x8000);
		SET_FLAG(SFR_CY, r >= 0);
		SET_FLAG(SFR_Z, (u16) r == 0);
		if (!FLAG(SFR_ALT2) || !FLAG(SFR_ALT1)) {
			SET_DR(r);
		}
		regsReset();
		goto next;
	}

	// $70 merge
	case 0x70:
		SET_DR((gsu.r[7] & 0xff00) | (gsu.r[8] >> 8));
		SET_FLAG(SFR_OV, DR() & 0xc0c0);
		SET_FLAG(SFR_S, DR() & 0x8080);
		SET_FLAG(SFR_CY, DR() & 0xe0e0);
		SET_FLAG(SFR_Z, DR() & 0xf0f0);
		regsReset();
		goto next;

	// $71-$7f and / bic / and # / bic #
	case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78:
	case 0x79: case 0x7a: case 0x7b: case 0x7c: case 0x7d: case 0x7e: case 0x7f:
		n = gsu.opcode & 15;
		if (!FLAG(SFR_ALT2)) {
			n = gsu.r[n];
		}
		SET_DR(SR() & (FLAG(SFR_ALT1) ? ~n : n));
		SET_FLAG(SFR_S, DR() & 0x8000);
		SET_FLAG(SFR_Z, DR() == 0);
		regsReset();
		goto next;

	// $80-$8f mult / umult / mult # / umult #
	case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
	case 0x88: case 0x89: case 0x8a: case 0x8b: case 0x8c: case 0x8d: case 0x8e: case 0x8f:
		n = gsu.opcode & 15;
		if (!FLAG(SFR_ALT2)) {
			n = gsu.r[n];
		}
		SET_DR(!FLAG(SFR_ALT1) ? (u16) ((s8) SR() * (s8) n) : (u16) ((u8) SR() * (u8) n));
		SET_FLAG(SFR_S, DR() & 0x8000);
		SET_FLAG(SFR_Z, DR() == 0);
		regsReset();
		if (!gsu.cfgr.ms0) {
			STEP(gsu.clsr ? 1 : 2);
		}
		goto next;

	// $90 sbk
	case 0x90:
		WRITE_RAM_BUFFER(gsu.ramaddr ^ 0, SR() >> 0);
		WRITE_RAM_BUFFER(gsu.ramaddr ^ 1, SR() >> 8);
		regsReset();
		goto next;

	// $91-$94 link #N
	case 0x91: case 0x92: case 0x93: case 0x94:
		setR(11, gsu.r[15] + (gsu.opcode & 15));
		regsReset();
		goto next;

	// $95 sex
	case 0x95:
		SET_DR((s8) SR());
		SET_FLAG(SFR_S, DR() & 0x8000);
		SET_FLAG(SFR_Z, DR() == 0);
		regsReset();
		goto next;

	// $96 asr / div2
	case 0x96:
		SET_FLAG(SFR_CY, SR() & 1);
		SET_DR(((s16) SR() >> 1) + (FLAG(SFR_ALT1) ? ((SR() + 1) >> 16) : 0));
		SET_FLAG(SFR_S, DR() & 0x8000);
		SET_FLAG(SFR_Z, DR() == 0);
		regsReset();
		goto next;

	// $97 ror
	case 0x97: {
		bool carry = SR() & 1;
		SET_DR((FLAG(SFR_CY) << 15) | (SR() >> 1));
		SET_FLAG(SFR_S, DR() & 0x8000);
		SET_FLAG(SFR_CY, carry);
		SET_FLAG(SFR_Z, DR() == 0);
		regsReset();
		goto next;
	}

	// $98-$9d jmp / ljmp rN
	case 0x98: case 0x99: case 0x9a: case 0x9b: case 0x9c: case 0x9d:
		n = gsu.opcode & 15;
		if (!FLAG(SFR_ALT1)) {
			setR(15, gsu.r[n]);
		} else {
			gsu.pbr = gsu.r[n] & 0x7f;
			setR(15, SR());
			gsu.cbr = gsu.r[15] & 0xfff0;
			flushCache();
		}
		regsReset();
		goto next;

	// $9e lob
	case 0x9e:
		SET_DR(SR() & 0xff);
		SET_FLAG(SFR_S, DR() & 0x80);
		SET_FLAG(SFR_Z, DR() == 0);
		regsReset();
		goto next;

	// $9f fmult / lmult
	case 0x9f: {
		u32 result = (s16) SR() * (s16) gsu.r[6];
		if (FLAG(SFR_ALT1)) {
			setR(4, result);
		}
		SET_DR(result >> 16);
		SET_FLAG(SFR_S, DR() & 0x8000);
		SET_FLAG(SFR_CY, result & 0x8000);
		SET_FLAG(SFR_Z, DR() == 0);
		regsReset();
		STEP((gsu.cfgr.ms0 ? 3 : 7) * (gsu.clsr ? 1 : 2));
		goto next;
	}

	// $a0-$af ibt rN,#pp / lms rN,(yy) / sms (yy),rN
	case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xa4: case 0xa5: case 0xa6: case 0xa7:
	case 0xa8: case 0xa9: case 0xaa: case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf:
		if (FLAG(SFR_ALT1)) {
			PIPE(gsu.result);
			gsu.ramaddr = gsu.result << 1;
			READ_RAM_BUFFER(gsu.result, gsu.ramaddr ^ 0);
			gsu.lo = gsu.result;
			READ_RAM_BUFFER(gsu.result, gsu.ramaddr ^ 1);
			setR(gsu.opcode & 15, (u8) gsu.result << 8 | gsu.lo);
		} else if (FLAG(SFR_ALT2)) {
			PIPE(gsu.result);
			gsu.ramaddr = gsu.result << 1;
			WRITE_RAM_BUFFER(gsu.ramaddr ^ 0, gsu.r[gsu.opcode & 15] >> 0);
			WRITE_RAM_BUFFER(gsu.ramaddr ^ 1, gsu.r[gsu.opcode & 15] >> 8);
		} else {
			PIPE(gsu.result);
			setR(gsu.opcode & 15, (s8) gsu.result);
		}
		regsReset();
		goto next;

	// $b0-$bf from rN / moves rN
	case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7:
	case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf:
		n = gsu.opcode & 15;
		if (!FLAG(SFR_B)) {
			gsu.sreg = n;
		} else {
			SET_DR(gsu.r[n]);
			SET_FLAG(SFR_OV, DR() & 0x80);
			SET_FLAG(SFR_S, DR() & 0x8000);
			SET_FLAG(SFR_Z, DR() == 0);
			regsReset();
		}
		goto next;

	// $c0 hib
	case 0xc0:
		SET_DR(SR() >> 8);
		SET_FLAG(SFR_S, DR() & 0x80);
		SET_FLAG(SFR_Z, DR() == 0);
		regsReset();
		goto next;

	// $c1-$cf or / xor / or # / xor #
	case 0xc1: case 0xc2: case 0xc3: case 0xc4: case 0xc5: case 0xc6: case 0xc7: case 0xc8:
	case 0xc9: case 0xca: case 0xcb: case 0xcc: case 0xcd: case 0xce: case 0xcf:
		n = gsu.opcode & 15;
		if (!FLAG(SFR_ALT2)) {
			n = gsu.r[n];
		}
		SET_DR(!FLAG(SFR_ALT1) ? (SR() | n) : (SR() ^ n));
		SET_FLAG(SFR_S, DR() & 0x8000);
		SET_FLAG(SFR_Z, DR() == 0);
		regsReset();
		goto next;

	// $d0-$de inc rN
	case 0xd0: case 0xd1: case 0xd2: case 0xd3: case 0xd4: case 0xd5: case 0xd6: case 0xd7:
	case 0xd8: case 0xd9: case 0xda: case 0xdb: case 0xdc: case 0xdd: case 0xde:
		n = gsu.opcode & 15;
		setR(n, gsu.r[n] + 1);
		SET_FLAG(SFR_S, gsu.r[n] & 0x8000);
		SET_FLAG(SFR_Z, gsu.r[n] == 0);
		regsReset();
		goto next;

	// $df getc / ramb / romb
	case 0xdf:
		if (!FLAG(SFR_ALT2)) {
			READ_ROM_BUFFER(gsu.result);
			gsu.colr = color(gsu.result);
		} else if (!FLAG(SFR_ALT1)) {
			SYNC_RAM_BUFFER();
			gsu.rambr = SR() & 0x01;
		} else {
			SYNC_ROM_BUFFER();
			gsu.rombr = SR() & 0x7f;
		}
		regsReset();
		goto next;

	// $e0-$ee dec rN
	case 0xe0: case 0xe1: case 0xe2: case 0xe3: case 0xe4: case 0xe5: case 0xe6: case 0xe7:
	case 0xe8: case 0xe9: case 0xea: case 0xeb: case 0xec: case 0xed: case 0xee:
		n = gsu.opcode & 15;
		setR(n, gsu.r[n] - 1);
		SET_FLAG(SFR_S, gsu.r[n] & 0x8000);
		SET_FLAG(SFR_Z, gsu.r[n] == 0);
		regsReset();
		goto next;

	// $ef getb / getbh / getbl / getbs
	case 0xef:
		READ_ROM_BUFFER(gsu.result);
		switch (FLAG(SFR_ALT2) << 1 | FLAG(SFR_ALT1)) {
		case 0: SET_DR(gsu.result); break;
		case 1: SET_DR(gsu.result << 8 | (u8) SR()); break;
		case 2: SET_DR((SR() & 0xff00) | gsu.result); break;
		case 3: SET_DR((s8) gsu.result); break;
		}
		regsReset();
		goto next;

	// $f0-$ff iwt rN,#xx / lm rN,(xx) / sm (xx),rN
	case 0xf0: case 0xf1: case 0xf2: case 0xf3: case 0xf4: case 0xf5: case 0xf6: case 0xf7:
	case 0xf8: case 0xf9: case 0xfa: case 0xfb: case 0xfc: case 0xfd: case 0xfe: case 0xff:
		if (FLAG(SFR_ALT1)) {
			PIPE(gsu.result);
			gsu.ramaddr = gsu.result;
			PIPE(gsu.result);
			gsu.ramaddr |= gsu.result << 8;
			READ_RAM_BUFFER(gsu.result, gsu.ramaddr ^ 0);
			gsu.lo = gsu.result;
			READ_RAM_BUFFER(gsu.result, gsu.ramaddr ^ 1);
			setR(gsu.opcode & 15, (u8) gsu.result << 8 | gsu.lo);
		} else if (FLAG(SFR_ALT2)) {
			PIPE(gsu.result);
			gsu.ramaddr = gsu.result;
			PIPE(gsu.result);
			gsu.ramaddr |= gsu.result << 8;
			WRITE_RAM_BUFFER(gsu.ramaddr ^ 0, gsu.r[gsu.opcode & 15] >> 0);
			WRITE_RAM_BUFFER(gsu.ramaddr ^ 1, gsu.r[gsu.opcode & 15] >> 8);
		} else {
			PIPE(gsu.result);
			gsu.lo = gsu.result;
			PIPE(gsu.result);
			setR(gsu.opcode & 15, (u8) gsu.result << 8 | gsu.lo);
		}
		regsReset();
		goto next;

	// A clock step, a: the clocks. A pending ROM read or RAM write finishes when its time is up.
	case SUB_STEP:
		if (gsu.romcl) {
			gsu.romcl -= FR.a < gsu.romcl ? FR.a : gsu.romcl;
			if (gsu.romcl == 0) {
				SET_FLAG(SFR_R, 0);
				READ(gsu.romdr, (gsu.rombr << 16) + gsu.r[14]);
			}
		}
		if (gsu.ramcl) {
			gsu.ramcl -= FR.a < gsu.ramcl ? FR.a : gsu.ramcl;
			if (gsu.ramcl == 0) {
				WRITE(0x700000 + (gsu.rambr << 16) + gsu.ramar, gsu.ramdr);
			}
		}
		gsu.th.clock += gsu.th.scalar * FR.a;
		if (cpu.th.clock < gsu.th.clock) {
			gsu.state = FIRST_LABEL - 1;
			return;
		case FIRST_LABEL - 1:;
		}
		RETURN();

	// A read, a: the address. The result is in gsu.result. While the S-CPU has the bus, the GSU
	// waits for it in steps of 6.
	case SUB_READ:
		FR.a &= 0xffffff;
		if ((FR.a & 0xc00000) == 0x000000) {
			while (!gsu.scmr.ron) {
				STEP(6);
			}
			gsu.result = romAt(((FR.a & 0x3f0000) >> 1) | (FR.a & 0x7fff));
			RETURN();
		}
		if ((FR.a & 0xe00000) == 0x400000) {
			while (!gsu.scmr.ron) {
				STEP(6);
			}
			gsu.result = romAt(FR.a);
			RETURN();
		}
		if ((FR.a & 0xfe0000) == 0x700000) {
			while (!gsu.scmr.ran) {
				STEP(6);
			}
			gsu.result = cart.ramSize ? cart.ram[FR.a & ramMask] : 0;
			RETURN();
		}
		gsu.result = 0x00;
		RETURN();

	// A write, a: the address, b: the data.
	case SUB_WRITE:
		FR.a &= 0xffffff;
		if ((FR.a & 0xfe0000) == 0x700000) {
			while (!gsu.scmr.ran) {
				STEP(6);
			}
			if (cart.ramSize) {
				cart.ram[FR.a & ramMask] = FR.b;
			}
		}
		RETURN();

	// The next opcode byte into the pipeline, a: its address. Through the cache when the address is
	// in it, filling a line of 16 when it isn't valid yet.
	case SUB_READ_OPCODE:
		FR.c = (u16) (FR.a - gsu.cbr);
		if (FR.c < 512) {
			if (gsu.cacheValid[FR.c >> 4] == false) {
				FR.d = FR.c & 0xfff0;
				FR.e = (gsu.pbr << 16) + ((gsu.cbr + FR.d) & 0xfff0);
				for (FR.b = 0; FR.b < 16; ++FR.b) {
					STEP(gsu.clsr ? 5 : 6);
					READ(gsu.cache[FR.d], FR.e);
					FR.d++;
					FR.e++;
				}
				gsu.cacheValid[FR.c >> 4] = true;
			} else {
				STEP(gsu.clsr ? 1 : 2);
			}
			gsu.pipeline = gsu.cache[FR.c];
		} else if (gsu.pbr <= 0x5f) {
			// ROM
			SYNC_ROM_BUFFER();
			STEP(gsu.clsr ? 5 : 6);
			READ(gsu.pipeline, gsu.pbr << 16 | (u16) FR.a);
		} else {
			// RAM
			SYNC_RAM_BUFFER();
			STEP(gsu.clsr ? 5 : 6);
			READ(gsu.pipeline, gsu.pbr << 16 | (u16) FR.a);
		}
		gsu.modified[15] = false;
		RETURN();

	// A pixel cache to RAM, a: which.
	case SUB_FLUSH:
		if (gsu.pixelcache[FR.a].bitpend == 0x00) {
			RETURN();
		}
		{
			u8 x = gsu.pixelcache[FR.a].offset << 3;
			u8 y = gsu.pixelcache[FR.a].offset >> 5;
			u32 bpp;
			FR.c = pixelAddress(x, y, &bpp);
			FR.d = bpp;
		}
		for (FR.b = 0; FR.b < FR.d; ++FR.b) {
			{
				u32 byte = ((FR.b >> 1) << 4) + (FR.b & 1);
				u8 data = 0x00;
				for (u32 x = 0; x < 8; ++x) {
					data |= ((gsu.pixelcache[FR.a].data[x] >> FR.b) & 1) << x;
				}
				FR.e = byte << 8 | data;
			}
			if (gsu.pixelcache[FR.a].bitpend != 0xff) {
				STEP(gsu.clsr ? 5 : 6);
				READ(gsu.result, FR.c + (FR.e >> 8));
				FR.e = (FR.e & 0xff00) | ((FR.e & gsu.pixelcache[FR.a].bitpend) | (gsu.result & ~gsu.pixelcache[FR.a].bitpend));
			}
			STEP(gsu.clsr ? 5 : 6);
			WRITE(FR.c + (FR.e >> 8), FR.e & 0xff);
		}
		gsu.pixelcache[FR.a].bitpend = 0x00;
		RETURN();

	// plot, a: x, b: y, eight bits each.
	case SUB_PLOT:
		FR.a &= 0xff;
		FR.b &= 0xff;
		if (!gsu.por.transparent) {
			if (gsu.scmr.md == 3) {
				if (gsu.por.freezehigh) {
					if ((gsu.colr & 0x0f) == 0) {
						RETURN();
					}
				} else if (gsu.colr == 0) {
					RETURN();
				}
			} else if ((gsu.colr & 0x0f) == 0) {
				RETURN();
			}
		}
		FR.c = gsu.colr;
		if (gsu.por.dither && gsu.scmr.md != 3) {
			if ((FR.a ^ FR.b) & 1) {
				FR.c >>= 4;
			}
			FR.c &= 0x0f;
		}
		FR.d = (u16) ((FR.b << 5) + (FR.a >> 3));
		if (FR.d != gsu.pixelcache[0].offset) {
			CALL(SUB_FLUSH, 1, 0, 0);
			gsu.pixelcache[1] = gsu.pixelcache[0];
			gsu.pixelcache[0].bitpend = 0x00;
			gsu.pixelcache[0].offset = FR.d;
		}
		{
			u32 x = (FR.a & 7) ^ 7;
			gsu.pixelcache[0].data[x] = FR.c;
			gsu.pixelcache[0].bitpend |= 1 << x;
		}
		if (gsu.pixelcache[0].bitpend == 0xff) {
			CALL(SUB_FLUSH, 1, 0, 0);
			gsu.pixelcache[1] = gsu.pixelcache[0];
			gsu.pixelcache[0].bitpend = 0x00;
		}
		RETURN();

	// rpix, a: x, b: y, eight bits each. The result is in gsu.result.
	case SUB_RPIX:
		FR.a &= 0xff;
		FR.b &= 0xff;
		CALL(SUB_FLUSH, 1, 0, 0);
		CALL(SUB_FLUSH, 0, 0, 0);
		{
			u32 bpp;
			FR.c = pixelAddress(FR.a, FR.b, &bpp);
			FR.d = bpp << 8;
		}
		FR.a = (FR.a & 7) ^ 7;
		for (FR.e = 0; FR.e < (FR.d >> 8); ++FR.e) {
			STEP(gsu.clsr ? 5 : 6);
			READ(gsu.result, FR.c + ((FR.e >> 1) << 4) + (FR.e & 1));
			FR.d |= ((gsu.result >> FR.a) & 1) << FR.e;
		}
		gsu.result = FR.d & 0xff;
		RETURN();
	}

next:
	if (gsu.modified[14]) {
		gsu.modified[14] = false;
		updateROMBuffer();
	}
	if (gsu.modified[15]) {
		gsu.modified[15] = false;
	} else {
		gsu.r[15]++;
	}
done:
	gsu.state = MAIN;
	goto dispatch;
}

static void syncFromCpu(void) {
	if (gsu.th.clock < cpu.th.clock) {
		gsuRun();
	}
}

u8 gsuReadIO(u32 address, u8 data) {
	(void) data;
	syncFromCpu();
	address = 0x3000 | (address & 0x3ff);

	if (address >= 0x3100 && address <= 0x32ff) {
		return readCache(address - 0x3100);
	}
	if (address >= 0x3000 && address <= 0x301f) {
		return gsu.r[address >> 1 & 15] >> ((address & 1) << 3);
	}

	switch (address) {
	case 0x3030:
		return (gsu.sfr & 0x9f7e) >> 0;
	case 0x3031: {
		u8 r = (gsu.sfr & 0x9f7e) >> 8;
		SET_FLAG(SFR_IRQ, 0);
		cpu.r.irq = 0;
		return r;
	}
	case 0x3034:
		return gsu.pbr;
	case 0x3036:
		return gsu.rombr;
	case 0x303b:
		return gsu.vcr;
	case 0x303c:
		return gsu.rambr;
	case 0x303e:
		return gsu.cbr >> 0;
	case 0x303f:
		return gsu.cbr >> 8;
	}
	return 0x00;
}

void gsuWriteIO(u32 address, u8 data) {
	syncFromCpu();
	address = 0x3000 | (address & 0x3ff);

	if (address >= 0x3100 && address <= 0x32ff) {
		writeCache(address - 0x3100, data);
		return;
	}

	if (address >= 0x3000 && address <= 0x301f) {
		u32 n = address >> 1 & 15;
		if (!(address & 1)) {
			setR(n, (gsu.r[n] & 0xff00) | data);
		} else {
			setR(n, (data << 8) | (gsu.r[n] & 0xff));
		}
		if (n == 14) {
			updateROMBuffer();
		}
		if (address == 0x301f) {
			SET_FLAG(SFR_G, 1);
		}
		return;
	}

	switch (address) {
	case 0x3030: {
		bool g = FLAG(SFR_G);
		gsu.sfr = (gsu.sfr & 0x9f7e & 0xff00) | data;
		if (g == 1 && FLAG(SFR_G) == 0) {
			gsu.cbr = 0x0000;
			flushCache();
		}
		break;
	}
	case 0x3031:
		gsu.sfr = (data << 8) | (gsu.sfr & 0x9f7e & 0x00ff);
		break;
	case 0x3033:
		gsu.bramr = data & 0x01;
		break;
	case 0x3034:
		gsu.pbr = data & 0x7f;
		flushCache();
		break;
	case 0x3037:
		setCFGR(data);
		break;
	case 0x3038:
		gsu.scbr = data;
		break;
	case 0x3039:
		gsu.clsr = data & 0x01;
		break;
	case 0x303a:
		setSCMR(data);
		break;
	}
}

// While the GSU runs with the ROM, the S-CPU reads its interrupt vectors from a fixed table.
u8 gsuReadRomCPU(u32 address, u8 data) {
	(void) data;
	if (FLAG(SFR_G) && gsu.scmr.ron) {
		static const u8 vector[16] = {
			0x00, 0x01, 0x00, 0x01, 0x04, 0x01, 0x00, 0x01, 0x00, 0x01, 0x08, 0x01, 0x00, 0x01, 0x0c, 0x01,
		};
		return vector[address & 15];
	}
	return cart.rom[address];
}

void gsuWriteRomCPU(u32 address, u8 data) {
	(void) address;
	(void) data;
}

u8 gsuReadRamCPU(u32 address, u8 data) {
	if (FLAG(SFR_G) && gsu.scmr.ran) {
		return data;
	}
	return cart.ram[address];
}

void gsuWriteRamCPU(u32 address, u8 data) {
	cart.ram[address] = data;
}

void gsuPower(void) {
	struct Thread th = gsu.th;
	memset(&gsu, 0, sizeof(gsu));
	gsu.th = th;
	threadFrequency(&gsu.th, 21440000);
	gsu.state = MAIN;

	gsu.vcr = 0x04;
	gsu.pipeline = 0x01;

	u32 size = 1;
	while (size < cart.romSize) {
		size <<= 1;
	}
	romMask = size - 1;
	ramMask = cart.ramSize ? cart.ramSize - 1 : 0;

	for (int n = 0; n < 2; ++n) {
		gsu.pixelcache[n].offset = ~0;
		gsu.pixelcache[n].bitpend = 0x00;
	}
}
