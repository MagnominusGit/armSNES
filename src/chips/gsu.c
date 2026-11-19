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

// Fast mode's recompiled blocks are dropped when the code cache or the settings they were made for
// change (gsu_jit.inc).
static void gsuJitInvalidate(void);
static void gsuJitLine(u32 line);
static void gsuJitDrop(void);

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
	gsuJitInvalidate();
}

// The ROM's size, rounded up to a power of two, less one; and the RAM's.
static u32 romMask;
static u32 ramMask;

static inline u8 romAt(u32 address) {
	address &= romMask;
	return address < cart.romSize ? cart.rom[address] : 0;
}

// Where gsuRunTo's run stopped.
static u64 stoppedAt = ~0ull;

static void flushCache(void) {
	memset(gsu.cacheValid, 0, sizeof(gsu.cacheValid));
	gsuJitInvalidate();
}

static u8 readCache(u16 address) {
	return gsu.cache[(address + gsu.cbr) & 511];
}

static void writeCache(u16 address, u8 data) {
	address = (address + gsu.cbr) & 511;
	gsu.cache[address] = data;
	gsuJitLine(address >> 4);
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

// Exact: the GSU can stop at any clock step. In Balanced mode it leaves between instructions once it
// has the ROM and the RAM, for the Balanced loop.
#define WAIT_STEPS() \
	do { \
		if (!gsu.romcl && !gsu.ramcl && gsu.th.clock <= cpu.th.clock) { \
			gsu.th.clock += (cpu.th.clock - gsu.th.clock) / (gsu.th.scalar * 6) * (gsu.th.scalar * 6); \
		} \
	} while (0)

static void gsuRunExact(void) {
	u32 n;

dispatch:
	switch (gsu.state) {
	case MAIN:
		if (systemBalanced && gsu.scmr.ron && gsu.scmr.ran) {
			return;
		}
		if (FLAG(SFR_G) == 0) {
			// Stopped, with no ROM read or RAM write under way, each step only moves the clock on:
			// all but the last of them, up to the CPU, are taken in one.
			WAIT_STEPS();
			STEP(6);
			goto done;
		}
		// The opcode from the pipeline, and the next byte fetched into it.
		gsu.opcode = gsu.pipeline;
		CALL(SUB_READ_OPCODE, gsu.r[15], 0, 0);
		gsu.state = gsu.opcode;
		goto dispatch;

#define GSU_CLOCK gsu.th.clock
#define GSU_PIPELINE gsu.pipeline
#include "gsu_ops.inc"
#undef GSU_CLOCK
#undef GSU_PIPELINE

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
	// waits for it in steps of 6: with no ROM read or RAM write under way, all but the last up to the
	// CPU (which alone gives the bus back) are taken in one.
	case SUB_READ:
		FR.a &= 0xffffff;
		if ((FR.a & 0xc00000) == 0x000000) {
			while (!gsu.scmr.ron) {
				WAIT_STEPS();
				STEP(6);
			}
			gsu.result = romAt(((FR.a & 0x3f0000) >> 1) | (FR.a & 0x7fff));
			RETURN();
		}
		if ((FR.a & 0xe00000) == 0x400000) {
			while (!gsu.scmr.ron) {
				WAIT_STEPS();
				STEP(6);
			}
			gsu.result = romAt(FR.a);
			RETURN();
		}
		if ((FR.a & 0xfe0000) == 0x700000) {
			while (!gsu.scmr.ran) {
				WAIT_STEPS();
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
				WAIT_STEPS();
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
				gsuJitLine(FR.c >> 4);
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

// Balanced mode: the subroutines as functions, and the GSU stopping only between instructions once
// it is ahead of the S-CPU. It only runs this way while it has the ROM and the RAM: waiting for
// either, which only the S-CPU can end, is Exact's.
static inline void callBalanced(u32 sub, u32 a, u32 b);

// A pending ROM read or RAM write is seen to out of line; the clock moves on in place.
static __attribute__((noinline)) void stepBuffers(u32 clocks) {
	if (gsu.romcl) {
		gsu.romcl -= clocks < gsu.romcl ? clocks : gsu.romcl;
		if (gsu.romcl == 0) {
			SET_FLAG(SFR_R, 0);
			callBalanced(SUB_READ, (gsu.rombr << 16) + gsu.r[14], 0);
			gsu.romdr = gsu.result;
		}
	}
	if (gsu.ramcl) {
		gsu.ramcl -= clocks < gsu.ramcl ? clocks : gsu.ramcl;
		if (gsu.ramcl == 0) {
			callBalanced(SUB_WRITE, 0x700000 + (gsu.rambr << 16) + gsu.ramar, gsu.ramdr);
		}
	}
}

static inline __attribute__((always_inline)) void stepBalanced(u32 clocks) {
	if (gsu.romcl | gsu.ramcl) {
		stepBuffers(clocks);
	}
	gsu.th.clock += gsu.th.scalar * clocks;
}

static void readBalanced(u32 address) {
	address &= 0xffffff;
	if ((address & 0xc00000) == 0x000000) {
		gsu.result = romAt(((address & 0x3f0000) >> 1) | (address & 0x7fff));
	} else if ((address & 0xe00000) == 0x400000) {
		gsu.result = romAt(address);
	} else if ((address & 0xfe0000) == 0x700000) {
		gsu.result = cart.ramSize ? cart.ram[address & ramMask] : 0;
	} else {
		gsu.result = 0x00;
	}
}

static void writeBalanced(u32 address, u32 data) {
	address &= 0xffffff;
	if ((address & 0xfe0000) == 0x700000 && cart.ramSize) {
		cart.ram[address & ramMask] = data;
	}
}

static __attribute__((noinline)) void readOpcodeBalanced(u32 address) {
	u32 offset = (u16) (address - gsu.cbr);
	if (offset < 512) {
		if (gsu.cacheValid[offset >> 4] == false) {
			u32 to = offset & 0xfff0;
			u32 from = (gsu.pbr << 16) + ((gsu.cbr + to) & 0xfff0);
			for (u32 n = 0; n < 16; ++n) {
				stepBalanced(gsu.clsr ? 5 : 6);
				readBalanced(from);
				gsu.cache[to] = gsu.result;
				to++;
				from++;
			}
			gsu.cacheValid[offset >> 4] = true;
			gsuJitLine(offset >> 4);
		} else {
			stepBalanced(gsu.clsr ? 1 : 2);
		}
		gsu.pipeline = gsu.cache[offset];
	} else {
		// ROM or RAM
		u32 buffer = gsu.pbr <= 0x5f ? gsu.romcl : gsu.ramcl;
		if (buffer) {
			stepBalanced(buffer);
		}
		stepBalanced(gsu.clsr ? 5 : 6);
		readBalanced(gsu.pbr << 16 | (u16) address);
		gsu.pipeline = gsu.result;
	}
	gsu.modified[15] = false;
}

// An opcode in the cache is fetched in place.
static inline __attribute__((always_inline)) void readOpcodeCached(u32 address) {
	u32 offset = (u16) (address - gsu.cbr);
	if (offset < 512 && gsu.cacheValid[offset >> 4]) {
		stepBalanced(gsu.clsr ? 1 : 2);
		gsu.pipeline = gsu.cache[offset];
		gsu.modified[15] = false;
	} else {
		readOpcodeBalanced(address);
	}
}

static void flushBalanced(u32 which) {
	struct GsuPixelCache* cache = &gsu.pixelcache[which];
	if (cache->bitpend == 0x00) {
		return;
	}
	u32 bpp;
	u32 address = pixelAddress(cache->offset << 3, cache->offset >> 5, &bpp);
	u32 clocks = gsu.clsr ? 5 : 6;
	if (gsu.romcl > clocks || gsu.ramcl > clocks) {
		for (u32 n = 0; n < bpp; ++n) {
			u32 byte = ((n >> 1) << 4) + (n & 1);
			u32 data = 0x00;
			for (u32 x = 0; x < 8; ++x) {
				data |= ((cache->data[x] >> n) & 1) << x;
			}
			if (cache->bitpend != 0xff) {
				stepBalanced(clocks);
				readBalanced(address + byte);
				data = (data & cache->bitpend) | (gsu.result & ~cache->bitpend);
			}
			stepBalanced(clocks);
			writeBalanced(address + byte, data & 0xff);
		}
		cache->bitpend = 0x00;
		return;
	}
	// A buffered ROM read or RAM write ends within the first access's clocks: the rest only take theirs.
	stepBalanced(clocks);
	gsu.th.clock += gsu.th.scalar * clocks * ((cache->bitpend != 0xff ? 2 * bpp : bpp) - 1);
	// The eight pixels' bits, a plane to a byte.
	u64 planes = 0;
	for (u32 x = 0; x < 8; ++x) {
		planes |= (u64) cache->data[x] << (x * 8);
	}
	u64 t = (planes ^ (planes >> 7)) & 0x00aa00aa00aa00aaull;
	planes ^= t ^ (t << 7);
	t = (planes ^ (planes >> 14)) & 0x0000cccc0000ccccull;
	planes ^= t ^ (t << 14);
	t = (planes ^ (planes >> 28)) & 0x00000000f0f0f0f0ull;
	planes ^= t ^ (t << 28);
	for (u32 n = 0; n < bpp; ++n) {
		u32 byte = ((n >> 1) << 4) + (n & 1);
		u32 data = planes >> (n * 8) & 0xff;
		if (cache->bitpend != 0xff) {
			readBalanced(address + byte);
			data = (data & cache->bitpend) | (gsu.result & ~cache->bitpend);
		}
		writeBalanced(address + byte, data);
	}
	cache->bitpend = 0x00;
}

static void plotBalanced(u32 x, u32 y) {
	x &= 0xff;
	y &= 0xff;
	if (!gsu.por.transparent) {
		if (gsu.scmr.md == 3) {
			if (gsu.por.freezehigh) {
				if ((gsu.colr & 0x0f) == 0) {
					return;
				}
			} else if (gsu.colr == 0) {
				return;
			}
		} else if ((gsu.colr & 0x0f) == 0) {
			return;
		}
	}
	u32 c = gsu.colr;
	if (gsu.por.dither && gsu.scmr.md != 3) {
		if ((x ^ y) & 1) {
			c >>= 4;
		}
		c &= 0x0f;
	}
	u32 offset = (u16) ((y << 5) + (x >> 3));
	if (offset != gsu.pixelcache[0].offset) {
		flushBalanced(1);
		gsu.pixelcache[1] = gsu.pixelcache[0];
		gsu.pixelcache[0].bitpend = 0x00;
		gsu.pixelcache[0].offset = offset;
	}
	u32 bit = (x & 7) ^ 7;
	gsu.pixelcache[0].data[bit] = c;
	gsu.pixelcache[0].bitpend |= 1 << bit;
	if (gsu.pixelcache[0].bitpend == 0xff) {
		flushBalanced(1);
		gsu.pixelcache[1] = gsu.pixelcache[0];
		gsu.pixelcache[0].bitpend = 0x00;
	}
}

static void rpixBalanced(u32 x, u32 y) {
	x &= 0xff;
	y &= 0xff;
	flushBalanced(1);
	flushBalanced(0);
	u32 bpp;
	u32 address = pixelAddress(x, y, &bpp);
	u32 bit = (x & 7) ^ 7;
	u32 data = bpp << 8;
	for (u32 n = 0; n < bpp; ++n) {
		stepBalanced(gsu.clsr ? 5 : 6);
		readBalanced(address + ((n >> 1) << 4) + (n & 1));
		data |= ((gsu.result >> bit) & 1) << n;
	}
	gsu.result = data & 0xff;
}

static inline __attribute__((always_inline)) void callBalanced(u32 sub, u32 a, u32 b) {
	switch (sub) {
	case SUB_STEP: stepBalanced(a); return;
	case SUB_READ: readBalanced(a); return;
	case SUB_WRITE: writeBalanced(a, b); return;
	case SUB_READ_OPCODE: readOpcodeCached(a); return;
	case SUB_FLUSH: flushBalanced(a); return;
	case SUB_PLOT: plotBalanced(a, b); return;
	case SUB_RPIX: rpixBalanced(a, b); return;
	}
}

#undef CALL
#define CALL(sub, va, vb, vc) callBalanced((sub), (va), (vb))

#include "gsu_jit.inc"

// The subroutines that move the clock on find it in gsu.th.clock; the loop keeps it in clock.
static inline __attribute__((always_inline)) void callLean(u32 sub, u32 a, u32 b, u64* clock) {
	switch (sub) {
	case SUB_STEP:
		if (gsu.romcl | gsu.ramcl) {
			stepBuffers(a);
		}
		*clock += gsu.th.scalar * a;
		return;
	case SUB_READ: readBalanced(a); return;
	case SUB_WRITE: writeBalanced(a, b); return;
	default:
		gsu.th.clock = *clock;
		callBalanced(sub, a, b);
		*clock = gsu.th.clock;
		return;
	}
}

static void gsuRunBalanced(void) {
	u32 n;
	// Only the S-CPU gives and takes the ROM and the RAM, and sets the clock speed; its clock stands
	// still while the GSU runs.
	if (!gsu.scmr.ron || !gsu.scmr.ran) {
		gsuRunExact();
		return;
	}
	const u64 until = cpu.th.clock;
	const u64 scalar = gsu.th.scalar;
	const u32 fetchClocks = gsu.clsr ? 1 : 2, romFetchClocks = gsu.clsr ? 5 : 6;
	// The clock, R15 (also kept in gsu.r[15]), the pipeline, and the registers written by the
	// instruction under way that matter at its end (R14 and R15).
	u64 clock = gsu.th.clock;
	u32 pc = gsu.r[15];
	u8 pipe = gsu.pipeline;
	// An R14 the S-CPU wrote is taken at the end of the first instruction; an R15 by its fetch.
	u32 written = gsu.modified[14] ? 1u << 14 : 0;
	gsu.modified[15] = false;
#undef CALL
#define CALL(sub, va, vb, vc) callLean((sub), (va), (vb), &clock)
#define setR(n, value) (gsu.r[n] = (value), gsu.modified[n] = true, written |= 1u << (n))
#define FETCH() \
	do { \
		u32 offset = (u16) (pc - gsu.cbr); \
		if (offset < 512 && gsu.cacheValid[offset >> 4]) { \
			if (gsu.romcl | gsu.ramcl) { \
				stepBuffers(fetchClocks); \
			} \
			clock += scalar * fetchClocks; \
			pipe = gsu.cache[offset]; \
		} else if (offset >= 512 && gsu.pbr <= 0x5f && !(gsu.romcl | gsu.ramcl)) { \
			/* From the ROM, with no read or write pending, as readOpcodeBalanced does it. */ \
			clock += scalar * romFetchClocks; \
			u32 rom = gsu.pbr << 16 | (u16) pc; \
			pipe = gsu.result = romAt(gsu.pbr < 0x40 ? ((rom & 0x3f0000) >> 1) | (rom & 0x7fff) : rom); \
			gsu.modified[15] = false; \
		} else { \
			gsu.th.clock = clock; \
			readOpcodeBalanced(pc); \
			clock = gsu.th.clock; \
			pipe = gsu.pipeline; \
		} \
	} while (0)
#undef PIPE
#define PIPE(target) \
	do { \
		gsu.pipeOut = pipe; \
		pc = (u16) (pc + 1); \
		gsu.r[15] = pc; \
		FETCH(); \
		target = gsu.pipeOut; \
	} while (0)
	for (;;) {
		if (until < clock) {
			break;
		}
		if (FLAG(SFR_G) == 0) {
			if (!gsu.romcl && !gsu.ramcl && clock <= until) {
				clock += (until - clock) / (scalar * 6) * (scalar * 6);
			}
			CALL(SUB_STEP, 6, 0, 0);
			continue;
		}
		// Fast mode: a recompiled block, when no prefix or written R14 is pending and R15 is in the cache or
		// the ROM.
		if ((u16) (pc - gsu.cbr) >= 512 && gsu.pbr > 0x5f) {
			gsuJitPass();
		} else if (GSU_JIT && systemFast && gsuJitEnabled && !written && !gsu.sreg && !gsu.dreg &&
		           !(gsu.sfr & (1 << SFR_B | 1 << SFR_ALT1 | 1 << SFR_ALT2))) {
			gsu.th.clock = clock;
			gsu.pipeline = pipe;
			if (gsuJitRun(pc, pipe, until)) {
				clock = gsu.th.clock;
				pc = gsu.r[15];
				pipe = gsu.pipeline;
				continue;
			}
		}
		gsu.opcode = pipe;
		FETCH();
		switch (gsu.opcode) {
#define GSU_CLOCK clock
#define GSU_PIPELINE pipe
#include "gsu_ops.inc"
#undef GSU_CLOCK
#undef GSU_PIPELINE
		}
	next:
		if (written & 0xc000) {
			if (written & 0x4000) {
				gsu.modified[14] = false;
				updateROMBuffer();
			}
			if (written & 0x8000) {
				gsu.modified[15] = false;
				pc = gsu.r[15];
			} else {
				pc = (u16) (pc + 1);
				gsu.r[15] = pc;
			}
		} else {
			pc = (u16) (pc + 1);
			gsu.r[15] = pc;
		}
		written = 0;
	}
	gsu.th.clock = clock;
	gsu.pipeline = pipe;
	gsu.modified[15] = false;
#undef setR
#undef FETCH
}

void gsuRun(void) {
	if (!systemBalanced || gsu.state != MAIN) {
		gsuRunExact();
	}
	if (systemBalanced && gsu.state == MAIN) {
		gsuRunBalanced();
	}
}

u64 gsuRunTo(u64 clock) {
	if (!FLAG(SFR_G)) {
		return ~0ull;
	}
	u64 now = cpu.th.clock;
	cpu.th.clock = clock;
	stoppedAt = ~0ull;
	if (gsu.th.clock < clock) {
		gsuRun();
	}
	cpu.th.clock = now;
	// Stopped, the GSU only waited after: its clock goes back to where it stopped, so a start the
	// S-CPU makes before the end of the run isn't late.
	if (stoppedAt != ~0ull && !FLAG(SFR_G)) {
		gsu.th.clock = stoppedAt > now ? stoppedAt : now;
	}
	return stoppedAt;
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
		gsuJitInvalidate();
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

const u8* gsuRamCPU(void) {
	return FLAG(SFR_G) && gsu.scmr.ran ? NULL : cart.ram;
}

void gsuWriteRamCPU(u32 address, u8 data) {
	cart.ram[address] = data;
}

void gsuLoaded(void) {
	gsuJitDrop();
}

void gsuPower(void) {
	gsuJitDrop();
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
