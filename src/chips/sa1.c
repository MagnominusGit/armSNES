/* Copyright (c) 2004-2025 ares team, Near et al
 * Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Ported from ares (ISC); see LICENSES/ISC-ares.txt. */
#include "../snes.h"

// The SA-1: a second 65816 at the S-CPU's clock with its own memory map, timers, DMA and arithmetic,
// sharing the cartridge's ROM, BW-RAM and I-RAM with the S-CPU.
//
// It runs when the S-CPU needs it (on touching its registers or memory, and once a scanline) and
// stops after any two-clock step that puts it ahead. So it is one state machine that can stop
// anywhere: its 65816 instructions call reads, writes and idle cycles as subroutines inside it, with
// a return stack, and what an instruction holds between its cycles is in sa1.r.

struct Sa1 sa1;

#define R sa1.r

static inline bool interruptPending(void) {
	return sa1.status.interruptPending;
}

static void lastCycle(void);

#include "../w65816r.inc"

enum {
	MAIN = 256,
	SUB_READ,
	SUB_WRITE,
	SUB_IDLE,
	SUB_IDLE_JUMP,
	SUB_DMA,
	FIRST_LABEL = 512,
};

#define LABEL_(n) (FIRST_LABEL + (n))
#define LABEL_X(n) LABEL_(n)

#define CALL_(sub, n) \
	do { \
		sa1.stack[sa1.depth++] = (n); \
		sa1.state = (sub); \
		goto dispatch; \
	case (n):; \
	} while (0)
#define CALL_X(sub, n) CALL_(sub, n)
#define CALL(sub) CALL_X(sub, LABEL_X(__COUNTER__))
#define RETURN() \
	do { \
		sa1.state = sa1.stack[--sa1.depth]; \
		goto dispatch; \
	} while (0)

// A clock step. The SA-1 leaves as soon as it is ahead of the S-CPU, and its timers move on when it
// comes back.
#define STEP_(n) \
	do { \
		sa1.th.clock += sa1.th.scalar * 2; \
		if (cpu.th.clock < sa1.th.clock) { \
			sa1.state = (n); \
			return; \
		case (n):; \
		} \
		counters(); \
	} while (0)
#define STEP_X(n) STEP_(n)
#define STEP() STEP_X(LABEL_X(__COUNTER__))

// A loop the program waits in: where an instruction last began after a jump back, with the
// registers and the clock as they were. Coming back there with the same registers, having written
// nothing and read no register of the chip's, it will go round the same way until the S-CPU runs
// again. This is only kept within one run, so it is no part of a state.
static struct {
	bool valid;
	u32 last;
	u32 pc;
	u64 clock;
	struct W65816 r;
} loop;

// Whether reading a register gives the same again and changes nothing: all but the one that latches
// the counters and the one that moves the variable-length data on.
static inline bool ioReadRepeats(u32 address) {
	u32 port = 0x2200 | (address & 0x1ff);
	return port != 0x2302 && port != 0x230d;
}

// An access whose clock steps all end with the SA-1 still behind the S-CPU can't stop part way: it
// is done on the spot, with the same steps. Otherwise it goes the way that can stop.
static bool fastRead(void);
static bool fastWrite(void);
static bool fastSteps(u32 steps);
static bool pcInRom(void);
static bool romConflict(void);

#define READ(target, address) \
	do { \
		R.ad = (address) & 0xffffff; \
		if (!fastRead()) { \
			CALL(SUB_READ); \
		} \
		target = R.dr; \
	} while (0)
#define WRITE(address, data) \
	do { \
		loop.valid = false; \
		R.ad = (address) & 0xffffff; \
		R.dw = (data); \
		if (!fastWrite()) { \
			CALL(SUB_WRITE); \
		} \
	} while (0)
#define FETCH(target) \
	do { \
		R.ad = PC.b << 16 | PC.w; \
		PC.w++; \
		if (!fastRead()) { \
			CALL(SUB_READ); \
		} \
		target = R.dr; \
	} while (0)
#define PULL(target) \
	do { \
		if (EF) { \
			S.l++; \
		} else { \
			S.w++; \
		} \
		READ(target, S.w); \
	} while (0)
#define PULL_N(target) \
	do { \
		++S.w; \
		READ(target, S.w); \
	} while (0)
#define PUSH(data) \
	do { \
		WRITE(S.w, data); \
		if (EF) { \
			S.l--; \
		} else { \
			S.w--; \
		} \
	} while (0)
#define PUSH_N(data) \
	do { \
		WRITE(S.w, data); \
		S.w--; \
	} while (0)
#define IDLE() \
	do { \
		if (!fastSteps(1)) { \
			CALL(SUB_IDLE); \
		} \
	} while (0)
#define IDLE_IRQ() \
	do { \
		if (interruptPending()) { \
			READ(R.dr, PC.d); \
		} else { \
			IDLE(); \
		} \
	} while (0)
#define IDLE_JUMP() \
	do { \
		if (pcInRom() && !fastSteps(1 + romConflict())) { \
			CALL(SUB_IDLE_JUMP); \
		} \
	} while (0)
#define IDLE_BRANCH() \
	do { \
		if (PC.d & 1) { \
			IDLE_JUMP(); \
		} \
	} while (0)

// Timers

static void triggerIRQ(void) {
	sa1.io.timer_irqfl = true;
	if (sa1.io.timer_irqen) {
		sa1.io.timer_irqcl = 0;
	}
}

// The counters are in clocks; the registers count dots of four.
static void counters(void) {
	if (sa1.io.hvselb == 0) {
		// H/V timer
		sa1.status.hcounter += 2;
		if (sa1.status.hcounter >= 1364) {
			sa1.status.hcounter = 0;
			if (++sa1.status.vcounter >= sa1.status.scanlines) {
				sa1.status.vcounter = 0;
			}
		}
	} else {
		// Linear timer
		sa1.status.hcounter += 2;
		sa1.status.vcounter += sa1.status.hcounter >> 11;
		sa1.status.hcounter &= 0x07ff;
		sa1.status.vcounter &= 0x01ff;
	}

	switch (sa1.io.hen << 0 | sa1.io.ven << 1) {
	case 1:
		if (sa1.status.hcounter == sa1.io.hcnt << 2) {
			triggerIRQ();
		}
		break;
	case 2:
		if (sa1.status.vcounter == sa1.io.vcnt && sa1.status.hcounter == 0) {
			triggerIRQ();
		}
		break;
	case 3:
		if (sa1.status.vcounter == sa1.io.vcnt && sa1.status.hcounter == sa1.io.hcnt << 2) {
			triggerIRQ();
		}
		break;
	}
}

static void lastCycle(void) {
	if (sa1.io.sa1_nmi && !sa1.io.sa1_nmicl) {
		sa1.status.interruptPending = true;
		R.vector = sa1.io.cnv;
		sa1.io.sa1_nmifl = true;
		sa1.io.sa1_nmicl = 1;
		R.wai = false;
	} else if (!R.i) {
		if (sa1.io.timer_irqen && !sa1.io.timer_irqcl) {
			sa1.status.interruptPending = true;
			R.vector = sa1.io.civ;
			sa1.io.timer_irqfl = true;
			R.wai = false;
		} else if (sa1.io.dma_irqen && !sa1.io.dma_irqcl) {
			sa1.status.interruptPending = true;
			R.vector = sa1.io.civ;
			sa1.io.dma_irqfl = true;
			R.wai = false;
		} else if (sa1.io.sa1_irq && !sa1.io.sa1_irqcl) {
			sa1.status.interruptPending = true;
			R.vector = sa1.io.civ;
			sa1.io.sa1_irqfl = true;
			R.wai = false;
		}
	}
}

// Memory: ROM

static bool romConflict(void) {
	if ((cpu.r.mar & 0x408000) == 0x008000) {
		return true;
	}
	if ((cpu.r.mar & 0xc00000) == 0xc00000) {
		return true;
	}
	return false;
}

// A bank of 4 or more reaches the memory pack when there is one.
static inline u8 romRead(u32 address) {
	if ((address & 0x400000) && bsPresent()) {
		return bsRead(address, 0x00);
	}
	return cart.rom[busMirror(address, cart.romSize)];
}

// 00-3f,80-bf:8000-ffff comes here as 00-3f:0000-ffff; c0-ff:0000-ffff as it is.
u8 sa1ReadRomCPU(u32 address, u8 data) {
	// The S-CPU's NMI and IRQ vectors can be replaced.
	if ((address & 0xffffe0) == 0x007fe0) {
		if (address == 0x7fea && sa1.io.cpu_nvsw) {
			return sa1.io.snv >> 0;
		}
		if (address == 0x7feb && sa1.io.cpu_nvsw) {
			return sa1.io.snv >> 8;
		}
		if (address == 0x7fee && sa1.io.cpu_ivsw) {
			return sa1.io.siv >> 0;
		}
		if (address == 0x7fef && sa1.io.cpu_ivsw) {
			return sa1.io.siv >> 8;
		}
	}

	// A bank's mode 0 applies only to 00-3f,80-bf:8000-ffff.
	bool lo = address < 0x400000;
	address &= 0x3fffff;
	if (address < 0x100000) {
		if (lo && sa1.io.cbmode == 0) {
			return romRead(address);
		}
		return romRead(sa1.io.cb << 20 | (address & 0x0fffff));
	}
	if (address < 0x200000) {
		if (lo && sa1.io.dbmode == 0) {
			return romRead(address);
		}
		return romRead(sa1.io.db << 20 | (address & 0x0fffff));
	}
	if (address < 0x300000) {
		if (lo && sa1.io.ebmode == 0) {
			return romRead(address);
		}
		return romRead(sa1.io.eb << 20 | (address & 0x0fffff));
	}
	if (address < 0x400000) {
		if (lo && sa1.io.fbmode == 0) {
			return romRead(address);
		}
		return romRead(sa1.io.fb << 20 | (address & 0x0fffff));
	}
	return data;
}

void sa1WriteRomCPU(u32 address, u8 data) {
	(void) address;
	(void) data;
}

static u8 romReadSA1(u32 address, u8 data) {
	if ((address & 0x408000) == 0x008000) {
		address = (address & 0x800000) >> 2 | (address & 0x3f0000) >> 1 | (address & 0x007fff);
	}
	return sa1ReadRomCPU(address, data);
}

// Memory: I-RAM

static inline bool iramConflict(void) {
	if ((cpu.r.mar & 0x40f800) == 0x003000) {
		return cpu.status.dramRefresh != 1;
	}
	return false;
}

static inline u8 iramRead(u32 address) {
	return sa1.iram[busMirror(address, sizeof(sa1.iram))];
}

static inline void iramWrite(u32 address, u8 data) {
	sa1.iram[busMirror(address, sizeof(sa1.iram))] = data;
}

static void syncFromCpu(void) {
	if (sa1.th.clock < cpu.th.clock) {
		sa1Run();
	}
}

u8 sa1ReadIramCPU(u32 address, u8 data) {
	(void) data;
	syncFromCpu();
	return iramRead(address);
}

void sa1WriteIramCPU(u32 address, u8 data) {
	syncFromCpu();
	if (!(sa1.io.siwp >> (address >> 8 & 7) & 1)) {
		return;
	}
	iramWrite(address, data);
}

static inline void iramWriteSA1(u32 address, u8 data) {
	if (!(sa1.io.ciwp >> (address >> 8 & 7) & 1)) {
		return;
	}
	iramWrite(address, data);
}

// Memory: BW-RAM, the cartridge's save RAM

static inline bool bwramConflict(void) {
	if ((cpu.r.mar & 0x40e000) == 0x006000) {
		return true;
	}
	if ((cpu.r.mar & 0xf00000) == 0x400000) {
		return true;
	}
	return false;
}

static inline u8 bwramRead(u32 address, u8 data) {
	if (!cart.ramSize) {
		return data;
	}
	return cart.ram[busMirror(address & 0xffffff, cart.ramSize)];
}

static inline void bwramWrite(u32 address, u8 data) {
	if (!cart.ramSize) {
		return;
	}
	cart.ram[busMirror(address & 0xffffff, cart.ramSize)] = data;
}

static u8 dmaCC1Read(u32 address);

// 00-3f,80-bf:6000-7fff comes here as 0000-1fff; 40-4f:0000-ffff as it is.
u8 sa1ReadBwramCPU(u32 address, u8 data) {
	syncFromCpu();
	if (address < 0x2000) {
		address = sa1.io.sbm * 0x2000 + (address & 0x1fff);
	}
	if (sa1.bwramDma) {
		return dmaCC1Read(address);
	}
	return bwramRead(address, data);
}

// BW-RAM protection works only when both SWEN and CWEN are off.
static inline bool bwramProtected(u32 address) {
	return !sa1.io.swen && !sa1.io.cwen && (address & 0x3ffff) < (0x100u << sa1.io.bwp);
}

void sa1WriteBwramCPU(u32 address, u8 data) {
	syncFromCpu();
	if (address < 0x2000) {
		address = sa1.io.sbm * 0x2000 + (address & 0x1fff);
	}
	if (bwramProtected(address)) {
		return;
	}
	bwramWrite(address, data);
}

static u8 bwramReadLinear(u32 address, u8 data) {
	return bwramRead(address, data);
}

static void bwramWriteLinear(u32 address, u8 data) {
	if (bwramProtected(address)) {
		return;
	}
	bwramWrite(address, data);
}

static u8 bwramReadBitmap(u32 address, u8 data) {
	(void) data;
	address &= 0xfffff;
	if (sa1.io.bbf == 0) {
		// 4bpp
		u32 shift = address & 1;
		address >>= 1;
		return bwramRead(address, 0) >> (shift * 4) & 15;
	}
	// 2bpp
	u32 shift = address & 3;
	address >>= 2;
	return bwramRead(address, 0) >> (shift * 2) & 3;
}

static void bwramWriteBitmap(u32 address, u8 data) {
	address &= 0xfffff;
	if (sa1.io.bbf == 0) {
		u32 shift = (address & 1) * 4;
		address >>= 1;
		data = (bwramRead(address, 0) & ~(15 << shift)) | (data & 15) << shift;
	} else {
		u32 shift = (address & 3) * 2;
		address >>= 2;
		data = (bwramRead(address, 0) & ~(3 << shift)) | (data & 3) << shift;
	}
	bwramWrite(address, data);
}

static u8 bwramReadSA1(u32 address, u8 data) {
	if (sa1.io.sw46 == 0) {
		// 40-43:0000-ffff, projected 32 times
		address = (sa1.io.cbm & 0x1f) * 0x2000 + (address & 0x1fff);
		return bwramReadLinear(address, data);
	}
	// 60-6f:0000-ffff, projected 128 times
	address = sa1.io.cbm * 0x2000 + (address & 0x1fff);
	return bwramReadBitmap(address, data);
}

static void bwramWriteSA1(u32 address, u8 data) {
	if (sa1.io.sw46 == 0) {
		address = (sa1.io.cbm & 0x1f) * 0x2000 + (address & 0x1fff);
		bwramWriteLinear(address, data);
		return;
	}
	address = sa1.io.cbm * 0x2000 + (address & 0x1fff);
	bwramWriteBitmap(address, data);
}

// The variable-length data ports read through this bus, which reaches no registers.
static u8 readVBR(u32 address) {
	address &= 0xffffff;
	if ((address & 0x408000) == 0x008000 || (address & 0xc00000) == 0xc00000) {
		return romReadSA1(address, 0);
	}
	if ((address & 0x40e000) == 0x006000 || (address & 0xf00000) == 0x400000) {
		return bwramRead(address, 0);
	}
	if ((address & 0x40f800) == 0x000000 || (address & 0x40f800) == 0x003000) {
		return iramRead(address);
	}
	return 0xff;
}

// DMA

// Type-1 character conversion: the S-CPU reads BW-RAM as tiles, which go through I-RAM.
static void dmaCC1(void) {
	sa1.bwramDma = true;
	sa1.io.chdma_irqfl = true;
	if (sa1.io.chdma_irqen) {
		sa1.io.chdma_irqcl = 0;
		cpu.r.irq = 1;
	}
}

static u8 dmaCC1Read(u32 address) {
	// 16 bytes a tile at 2bpp, 32 at 4bpp, 64 at 8bpp
	u32 charmask = (1 << (6 - sa1.io.dmacb)) - 1;

	if ((address & charmask) == 0) {
		// The next tile into I-RAM.
		u32 bpp = 2 << (2 - sa1.io.dmacb);
		u32 bpl = (8 << sa1.io.dmasize) >> sa1.io.dmacb;
		u32 bwmask = cart.ramSize - 1;
		u32 tile = ((address - sa1.io.dsa) & bwmask) >> (6 - sa1.io.dmacb);
		u32 ty = tile >> sa1.io.dmasize;
		u32 tx = tile & ((1 << sa1.io.dmasize) - 1);
		u32 bwaddr = sa1.io.dsa + ty * 8 * bpl + tx * bpp;

		for (u32 y = 0; y < 8; ++y) {
			u64 data = 0;
			for (u32 byte = 0; byte < bpp; ++byte) {
				data |= (u64) bwramRead((bwaddr + byte) & bwmask, 0) << (byte << 3);
			}
			bwaddr += bpl;

			u8 out[8] = { 0 };
			for (u32 x = 0; x < 8; ++x) {
				out[0] |= (data & 1) << (7 - x);
				data >>= 1;
				out[1] |= (data & 1) << (7 - x);
				data >>= 1;
				if (sa1.io.dmacb == 2) {
					continue;
				}
				out[2] |= (data & 1) << (7 - x);
				data >>= 1;
				out[3] |= (data & 1) << (7 - x);
				data >>= 1;
				if (sa1.io.dmacb == 1) {
					continue;
				}
				out[4] |= (data & 1) << (7 - x);
				data >>= 1;
				out[5] |= (data & 1) << (7 - x);
				data >>= 1;
				out[6] |= (data & 1) << (7 - x);
				data >>= 1;
				out[7] |= (data & 1) << (7 - x);
				data >>= 1;
			}

			for (u32 byte = 0; byte < bpp; ++byte) {
				u32 p = sa1.io.dda + (y << 1) + ((byte & 6) << 3) + (byte & 1);
				iramWrite(p & 0x07ff, out[byte]);
			}
		}
	}

	return iramRead((sa1.io.dda + (address & charmask)) & 0x07ff);
}

// Type-2 character conversion: the bitmap register files into I-RAM.
static void dmaCC2(void) {
	const u8* brf = &sa1.io.brf[(sa1.dmaLine & 1) << 3];
	u32 bpp = 2 << (2 - sa1.io.dmacb);
	u32 address = sa1.io.dda & 0x07ff;
	address &= ~((1 << (7 - sa1.io.dmacb)) - 1);
	address += (sa1.dmaLine & 8) * bpp;
	address += (sa1.dmaLine & 7) * 2;

	for (u32 byte = 0; byte < bpp; ++byte) {
		u8 output = 0;
		for (u32 bit = 0; bit < 8; ++bit) {
			output |= ((brf[bit] >> byte) & 1) << (7 - bit);
		}
		iramWrite(address + ((byte & 6) << 3) + (byte & 1), output);
	}

	sa1.dmaLine = (sa1.dmaLine + 1) & 15;
}

enum { SOURCE_ROM = 0, SOURCE_BWRAM = 1, SOURCE_IRAM = 2 };
enum { DEST_IRAM = 0, DEST_BWRAM = 1 };

// Registers

static void writeIOShared(u32 address, u8 data) {
	switch (0x2200 | (address & 0x1ff)) {
	// CDMA
	case 0x2231:
		sa1.io.dmacb = data & 3;
		sa1.io.dmasize = data >> 2 & 7;
		sa1.io.chdend = data & 0x80;
		if (sa1.io.dmacb > 2) {
			sa1.io.dmacb = 2;
		}
		if (sa1.io.dmasize > 5) {
			sa1.io.dmasize = 5;
		}
		if (sa1.io.chdend) {
			sa1.bwramDma = 0;
		}
		return;
	// SDA
	case 0x2232:
		sa1.io.dsa = (sa1.io.dsa & 0xffff00) | data;
		return;
	case 0x2233:
		sa1.io.dsa = (sa1.io.dsa & 0xff00ff) | data << 8;
		return;
	case 0x2234:
		sa1.io.dsa = (sa1.io.dsa & 0x00ffff) | data << 16;
		return;
	// DDA
	case 0x2235:
		sa1.io.dda = (sa1.io.dda & 0xffff00) | data;
		return;
	case 0x2236:
		sa1.io.dda = (sa1.io.dda & 0xff00ff) | data << 8;
		if (sa1.io.dmaen) {
			if (sa1.io.cden == 0 && sa1.io.dd == DEST_IRAM) {
				sa1.dmaStart = true;
			} else if (sa1.io.cden == 1 && sa1.io.cdsel == 1) {
				dmaCC1();
			}
		}
		return;
	case 0x2237:
		sa1.io.dda = (sa1.io.dda & 0x00ffff) | data << 16;
		if (sa1.io.dmaen) {
			if (sa1.io.cden == 0 && sa1.io.dd == DEST_BWRAM) {
				sa1.dmaStart = true;
			}
		}
		return;
	}
}

static void dmaNormalAtOnce(void);

u8 sa1ReadIOCPU(u32 address, u8 data) {
	syncFromCpu();
	switch (0x2200 | (address & 0x1ff)) {
	// SFR
	case 0x2300:
		return sa1.io.cmeg | sa1.io.cpu_nvsw << 4 | sa1.io.chdma_irqfl << 5 | sa1.io.cpu_ivsw << 6 | sa1.io.cpu_irqfl << 7;
	// Doesn't exist on hardware: open bus.
	case 0x230e:
		return data;
	}
	return data;
}

void sa1WriteIOCPU(u32 address, u8 data) {
	syncFromCpu();
	switch (0x2200 | (address & 0x1ff)) {
	// CCNT
	case 0x2200:
		if (sa1.io.sa1_resb && !(data & 0x20)) {
			// The SA-1 is let out of reset: PC and bank from CRV, STP cleared.
			R.pc.d = sa1.io.crv;
			R.b = 0x00;
			R.stp = false;
			sa1.io.ciwp = 0x00;
		}
		sa1.io.smeg = data & 15;
		sa1.io.sa1_nmi = data >> 4 & 1;
		sa1.io.sa1_resb = data >> 5 & 1;
		sa1.io.sa1_rdyb = data >> 6 & 1;
		sa1.io.sa1_irq = data >> 7 & 1;
		if (sa1.io.sa1_irq) {
			sa1.io.sa1_irqfl = true;
			if (sa1.io.sa1_irqen) {
				sa1.io.sa1_irqcl = 0;
			}
		}
		if (sa1.io.sa1_nmi) {
			sa1.io.sa1_nmifl = true;
			if (sa1.io.sa1_nmien) {
				sa1.io.sa1_nmicl = 0;
			}
		}
		return;
	// SIE
	case 0x2201:
		if (!sa1.io.chdma_irqen && (data & 0x20)) {
			if (sa1.io.chdma_irqfl) {
				sa1.io.chdma_irqcl = 0;
				cpu.r.irq = 1;
			}
		}
		if (!sa1.io.cpu_irqen && (data & 0x80)) {
			if (sa1.io.cpu_irqfl) {
				sa1.io.cpu_irqcl = 0;
				cpu.r.irq = 1;
			}
		}
		sa1.io.chdma_irqen = data >> 5 & 1;
		sa1.io.cpu_irqen = data >> 7 & 1;
		return;
	// SIC
	case 0x2202:
		sa1.io.chdma_irqcl = data >> 5 & 1;
		sa1.io.cpu_irqcl = data >> 7 & 1;
		if (sa1.io.chdma_irqcl) {
			sa1.io.chdma_irqfl = 0;
		}
		if (sa1.io.cpu_irqcl) {
			sa1.io.cpu_irqfl = 0;
		}
		if (!sa1.io.chdma_irqfl && !sa1.io.cpu_irqfl) {
			cpu.r.irq = 0;
		}
		return;
	// CRV, CNV, CIV
	case 0x2203:
		sa1.io.crv = (sa1.io.crv & 0xff00) | data;
		return;
	case 0x2204:
		sa1.io.crv = (sa1.io.crv & 0x00ff) | data << 8;
		return;
	case 0x2205:
		sa1.io.cnv = (sa1.io.cnv & 0xff00) | data;
		return;
	case 0x2206:
		sa1.io.cnv = (sa1.io.cnv & 0x00ff) | data << 8;
		return;
	case 0x2207:
		sa1.io.civ = (sa1.io.civ & 0xff00) | data;
		return;
	case 0x2208:
		sa1.io.civ = (sa1.io.civ & 0x00ff) | data << 8;
		return;
	// CXB, DXB, EXB, FXB
	case 0x2220:
		sa1.io.cb = data & 7;
		sa1.io.cbmode = data >> 7;
		return;
	case 0x2221:
		sa1.io.db = data & 7;
		sa1.io.dbmode = data >> 7;
		return;
	case 0x2222:
		sa1.io.eb = data & 7;
		sa1.io.ebmode = data >> 7;
		return;
	case 0x2223:
		sa1.io.fb = data & 7;
		sa1.io.fbmode = data >> 7;
		return;
	// BMAPS
	case 0x2224:
		sa1.io.sbm = data & 0x1f;
		return;
	// SWBE
	case 0x2226:
		sa1.io.swen = data >> 7;
		return;
	// BWPA
	case 0x2228:
		sa1.io.bwp = data & 15;
		return;
	// SIWP
	case 0x2229:
		sa1.io.siwp = data;
		return;
	case 0x2231:
	case 0x2232:
	case 0x2233:
	case 0x2234:
	case 0x2235:
	case 0x2236:
	case 0x2237:
		writeIOShared(address, data);
		// A normal DMA the S-CPU starts runs at once.
		if (sa1.dmaStart) {
			sa1.dmaStart = false;
			dmaNormalAtOnce();
		}
		return;
	}
}

static u8 readIOSA1(u32 address, u8 data) {
	switch (0x2200 | (address & 0x1ff)) {
	// CFR
	case 0x2301:
		return sa1.io.smeg | sa1.io.sa1_nmifl << 4 | sa1.io.dma_irqfl << 5 | sa1.io.timer_irqfl << 6 | sa1.io.sa1_irqfl << 7;
	// HCR: latches the counters
	case 0x2302:
		sa1.io.hcr = sa1.status.hcounter >> 2;
		sa1.io.vcr = sa1.status.vcounter;
		return sa1.io.hcr;
	case 0x2303:
		return sa1.io.hcr >> 8;
	// VCR
	case 0x2304:
		return sa1.io.vcr;
	case 0x2305:
		return sa1.io.vcr >> 8;
	// MR
	case 0x2306:
	case 0x2307:
	case 0x2308:
	case 0x2309:
	case 0x230a:
		return sa1.io.mr >> (((address & 0x1ff) - 0x106) * 8);
	// OF
	case 0x230b:
		return sa1.io.overflow << 7;
	// VDPL
	case 0x230c: {
		u32 value = readVBR(sa1.io.va + 0) | readVBR(sa1.io.va + 1) << 8 | readVBR(sa1.io.va + 2) << 16;
		value >>= sa1.io.vbit;
		return value;
	}
	// VDPH
	case 0x230d: {
		u32 value = readVBR(sa1.io.va + 0) | readVBR(sa1.io.va + 1) << 8 | readVBR(sa1.io.va + 2) << 16;
		value >>= sa1.io.vbit;
		if (sa1.io.hl == 1) {
			// Auto-increment
			sa1.io.vbit += sa1.io.vb;
			sa1.io.va = (sa1.io.va + (sa1.io.vbit >> 3)) & 0xffffff;
			sa1.io.vbit &= 7;
		}
		return value >> 8;
	}
	}
	(void) data;
	return 0xff;
}

static void writeIOSA1(u32 address, u8 data) {
	switch (0x2200 | (address & 0x1ff)) {
	// SCNT
	case 0x2209:
		sa1.io.cmeg = data & 15;
		sa1.io.cpu_nvsw = data >> 4 & 1;
		sa1.io.cpu_ivsw = data >> 6 & 1;
		sa1.io.cpu_irq = data >> 7 & 1;
		if (sa1.io.cpu_irq) {
			sa1.io.cpu_irqfl = 1;
			if (sa1.io.cpu_irqen) {
				sa1.io.cpu_irqcl = 0;
				cpu.r.irq = 1;
			}
		}
		return;
	// CIE
	case 0x220a:
		if (!sa1.io.sa1_nmien && (data & 0x10) && sa1.io.sa1_nmifl) {
			sa1.io.sa1_nmicl = 0;
		}
		if (!sa1.io.dma_irqen && (data & 0x20) && sa1.io.dma_irqfl) {
			sa1.io.dma_irqcl = 0;
		}
		if (!sa1.io.timer_irqen && (data & 0x40) && sa1.io.timer_irqfl) {
			sa1.io.timer_irqcl = 0;
		}
		if (!sa1.io.sa1_irqen && (data & 0x80) && sa1.io.sa1_irqfl) {
			sa1.io.sa1_irqcl = 0;
		}
		sa1.io.sa1_nmien = data >> 4 & 1;
		sa1.io.dma_irqen = data >> 5 & 1;
		sa1.io.timer_irqen = data >> 6 & 1;
		sa1.io.sa1_irqen = data >> 7 & 1;
		return;
	// CIC
	case 0x220b:
		sa1.io.sa1_nmicl = data >> 4 & 1;
		sa1.io.dma_irqcl = data >> 5 & 1;
		sa1.io.timer_irqcl = data >> 6 & 1;
		sa1.io.sa1_irqcl = data >> 7 & 1;
		if (sa1.io.sa1_nmicl) {
			sa1.io.sa1_nmifl = 0;
		}
		if (sa1.io.sa1_irqcl) {
			sa1.io.sa1_irqfl = 0;
		}
		if (sa1.io.timer_irqcl) {
			sa1.io.timer_irqfl = 0;
		}
		if (sa1.io.dma_irqcl) {
			sa1.io.dma_irqfl = 0;
		}
		return;
	// SNV, SIV
	case 0x220c:
		sa1.io.snv = (sa1.io.snv & 0xff00) | data;
		return;
	case 0x220d:
		sa1.io.snv = (sa1.io.snv & 0x00ff) | data << 8;
		return;
	case 0x220e:
		sa1.io.siv = (sa1.io.siv & 0xff00) | data;
		return;
	case 0x220f:
		sa1.io.siv = (sa1.io.siv & 0x00ff) | data << 8;
		return;
	// TMC
	case 0x2210:
		sa1.io.hen = data & 1;
		sa1.io.ven = data >> 1 & 1;
		sa1.io.hvselb = data >> 7 & 1;
		return;
	// CTR: the timer restarts
	case 0x2211:
		sa1.status.vcounter = 0;
		sa1.status.hcounter = 0;
		return;
	// HCNT, VCNT
	case 0x2212:
		sa1.io.hcnt = (sa1.io.hcnt & 0xff00) | data;
		return;
	case 0x2213:
		sa1.io.hcnt = (sa1.io.hcnt & 0x00ff) | data << 8;
		return;
	case 0x2214:
		sa1.io.vcnt = (sa1.io.vcnt & 0xff00) | data;
		return;
	case 0x2215:
		sa1.io.vcnt = (sa1.io.vcnt & 0x00ff) | data << 8;
		return;
	// BMAP
	case 0x2225:
		sa1.io.cbm = data & 0x7f;
		sa1.io.sw46 = data >> 7;
		return;
	// CWBE
	case 0x2227:
		sa1.io.cwen = data >> 7;
		return;
	// CIWP
	case 0x222a:
		sa1.io.ciwp = data;
		return;
	// DCNT
	case 0x2230:
		sa1.io.sd = data & 3;
		sa1.io.dd = data >> 2 & 1;
		sa1.io.cdsel = data >> 4 & 1;
		sa1.io.cden = data >> 5 & 1;
		sa1.io.dprio = data >> 6 & 1;
		sa1.io.dmaen = data >> 7 & 1;
		if (sa1.io.dmaen == 0) {
			sa1.dmaLine = 0;
		}
		return;
	case 0x2231:
	case 0x2232:
	case 0x2233:
	case 0x2234:
	case 0x2235:
	case 0x2236:
	case 0x2237:
		writeIOShared(address, data);
		return;
	// DTC
	case 0x2238:
		sa1.io.dtc = (sa1.io.dtc & 0xff00) | data;
		return;
	case 0x2239:
		sa1.io.dtc = (sa1.io.dtc & 0x00ff) | data << 8;
		return;
	// BBF
	case 0x223f:
		sa1.io.bbf = data >> 7;
		return;
	// BRF
	case 0x2240: case 0x2241: case 0x2242: case 0x2243: case 0x2244: case 0x2245: case 0x2246:
	case 0x2248: case 0x2249: case 0x224a: case 0x224b: case 0x224c: case 0x224d: case 0x224e:
		sa1.io.brf[address & 15] = data;
		return;
	case 0x2247:
	case 0x224f:
		sa1.io.brf[address & 15] = data;
		if (sa1.io.dmaen) {
			if (sa1.io.cden == 1 && sa1.io.cdsel == 0) {
				dmaCC2();
			}
		}
		return;
	// MCNT
	case 0x2250:
		sa1.io.md = data & 1;
		sa1.io.acm = data >> 1 & 1;
		if (sa1.io.acm) {
			sa1.io.mr = 0;
		}
		return;
	// MA
	case 0x2251:
		sa1.io.ma = (sa1.io.ma & 0xff00) | data;
		return;
	case 0x2252:
		sa1.io.ma = (sa1.io.ma & 0x00ff) | data << 8;
		return;
	// MB: multiplication and the cumulative sum reset only MB; division resets both.
	case 0x2253:
		sa1.io.mb = (sa1.io.mb & 0xff00) | data;
		return;
	case 0x2254:
		sa1.io.mb = (sa1.io.mb & 0x00ff) | data << 8;
		if (sa1.io.acm == 0) {
			if (sa1.io.md == 0) {
				// Signed multiplication
				sa1.io.mr = (u32) ((s16) sa1.io.ma * (s16) sa1.io.mb);
				sa1.io.mb = 0;
			} else {
				// Unsigned division
				if (sa1.io.mb == 0) {
					sa1.io.mr = 0;
				} else {
					s16 dividend = sa1.io.ma;
					u16 divisor = sa1.io.mb;
					u16 remainder = dividend >= 0 ? (u16) (dividend % divisor) : (u16) ((dividend % divisor + divisor) % divisor);
					u16 quotient = (dividend - remainder) / divisor;
					sa1.io.mr = (u32) remainder << 16 | quotient;
				}
				sa1.io.ma = 0;
				sa1.io.mb = 0;
			}
		} else {
			// Cumulative sum of products
			sa1.io.mr += (s64) ((s16) sa1.io.ma * (s16) sa1.io.mb);
			sa1.io.overflow = sa1.io.mr >> 40 & 1;
			sa1.io.mr &= 0xffffffffffull;
			sa1.io.mb = 0;
		}
		return;
	// VBD
	case 0x2258:
		sa1.io.vb = data & 15;
		sa1.io.hl = data >> 7;
		if (sa1.io.vb == 0) {
			sa1.io.vb = 16;
		}
		if (sa1.io.hl == 0) {
			// Fixed mode
			sa1.io.vbit += sa1.io.vb;
			sa1.io.va = (sa1.io.va + (sa1.io.vbit >> 3)) & 0xffffff;
			sa1.io.vbit &= 7;
		}
		return;
	// VDA
	case 0x2259:
		sa1.io.va = (sa1.io.va & 0xffff00) | data;
		return;
	case 0x225a:
		sa1.io.va = (sa1.io.va & 0xff00ff) | data << 8;
		return;
	case 0x225b:
		sa1.io.va = (sa1.io.va & 0x00ffff) | data << 16;
		sa1.io.vbit = 0;
		return;
	}
}

// A normal DMA the S-CPU started: its steps all taken now.
static void dmaNormalAtOnce(void) {
	for (;;) {
		u16 count = sa1.io.dtc--;
		if (!count) {
			break;
		}
		u8 data = R.mdr;
		u32 source = sa1.io.dsa;
		u32 target = sa1.io.dda;
		sa1.io.dsa = (sa1.io.dsa + 1) & 0xffffff;
		sa1.io.dda = (sa1.io.dda + 1) & 0xffffff;
		u32 steps = 0;
		if (sa1.io.sd == SOURCE_ROM && sa1.io.dd == DEST_BWRAM) {
			steps = 2 + bwramConflict() * 2;
			data = romReadSA1(source, data);
			bwramWrite(target, data);
		}
		if (sa1.io.sd == SOURCE_ROM && sa1.io.dd == DEST_IRAM) {
			steps = 1 + (iramConflict() || romConflict()) + iramConflict();
			data = romReadSA1(source, data);
			iramWrite(target, data);
		}
		if (sa1.io.sd == SOURCE_BWRAM && sa1.io.dd == DEST_IRAM) {
			steps = 2 + (bwramConflict() || iramConflict()) + bwramConflict();
			data = bwramRead(source, data);
			iramWrite(target, data);
		}
		if (sa1.io.sd == SOURCE_IRAM && sa1.io.dd == DEST_BWRAM) {
			steps = 2 + (bwramConflict() || iramConflict()) + bwramConflict();
			data = iramRead(source);
			bwramWrite(target, data);
		}
		while (steps--) {
			sa1.th.clock += sa1.th.scalar * 2;
			counters();
		}
	}
	sa1.io.dma_irqfl = true;
	if (sa1.io.dma_irqen) {
		sa1.io.dma_irqcl = 0;
	}
}

// The state machine

// --- Accesses that can't stop ---

static bool fastSteps(u32 steps) {
	// Balanced mode stops only between instructions: every access is done on the spot.
	if (!systemBalanced && cpu.th.clock < sa1.th.clock + sa1.th.scalar * 2 * steps) {
		return false;
	}
	while (steps--) {
		sa1.th.clock += sa1.th.scalar * 2;
		counters();
	}
	return true;
}

static bool pcInRom(void) {
	return (PC.d & 0x408000) == 0x008000 || (PC.d & 0xc00000) == 0xc00000;
}

// As SUB_READ.
static bool fastRead(void) {
	R.mar = R.ad;
	R.dr = R.mdr;
	if ((R.ad & 0x40fe00) == 0x002200) {
		if (!ioReadRepeats(R.ad)) {
			loop.valid = false;
		}
		if (!fastSteps(1)) {
			return false;
		}
		R.dr = R.mdr = readIOSA1(R.ad, R.dr);
		return true;
	}
	if ((R.ad & 0x408000) == 0x008000 || (R.ad & 0xc00000) == 0xc00000) {
		if (!fastSteps(1 + romConflict())) {
			return false;
		}
		R.dr = R.mdr = romReadSA1(R.ad, R.dr);
		return true;
	}
	if ((R.ad & 0x40e000) == 0x006000 || (R.ad & 0xe00000) == 0x400000 || (R.ad & 0xf00000) == 0x600000) {
		if (!fastSteps(2 + 2 * bwramConflict())) {
			return false;
		}
		if ((R.ad >> 22 & 1) && (R.ad >> 21 & 1)) {
			R.dr = R.mdr = bwramReadBitmap(R.ad, R.dr);
		} else if (R.ad >> 22 & 1) {
			R.dr = R.mdr = bwramReadLinear(R.ad, R.dr);
		} else {
			R.dr = R.mdr = bwramReadSA1(R.ad, R.dr);
		}
		return true;
	}
	if ((R.ad & 0x40f800) == 0x000000 || (R.ad & 0x40f800) == 0x003000) {
		if (!fastSteps(1 + 2 * iramConflict())) {
			return false;
		}
		R.dr = R.mdr = iramRead(R.ad);
		return true;
	}
	return fastSteps(1);
}

// As SUB_WRITE. A write to a register can start a DMA, which can stop.
static bool fastWrite(void) {
	if ((R.ad & 0x40fe00) == 0x002200) {
		return false;
	}
	if ((R.ad & 0x408000) == 0x008000 || (R.ad & 0xc00000) == 0xc00000) {
		if (!fastSteps(1 + romConflict())) {
			return false;
		}
		R.mar = R.ad;
		R.mdr = R.dw;
		return true;
	}
	if ((R.ad & 0x40e000) == 0x006000 || (R.ad & 0xe00000) == 0x400000 || (R.ad & 0xf00000) == 0x600000) {
		if (!fastSteps(2 + 2 * bwramConflict())) {
			return false;
		}
		R.mar = R.ad;
		R.mdr = R.dw;
		if ((R.ad >> 22 & 1) && (R.ad >> 21 & 1)) {
			bwramWriteBitmap(R.ad, R.dw);
		} else if (R.ad >> 22 & 1) {
			bwramWriteLinear(R.ad, R.dw);
		} else {
			bwramWriteSA1(R.ad, R.dw);
		}
		return true;
	}
	if ((R.ad & 0x40f800) == 0x000000 || (R.ad & 0x40f800) == 0x003000) {
		if (!fastSteps(1 + 2 * iramConflict())) {
			return false;
		}
		R.mar = R.ad;
		R.mdr = R.dw;
		iramWriteSA1(R.ad, R.dw);
		return true;
	}
	if (!fastSteps(1)) {
		return false;
	}
	R.mar = R.ad;
	R.mdr = R.dw;
	return true;
}

// The counters moved on by steps clock steps at once, when the timer raises nothing. False if they
// can't be.
static bool countersSkip(u64 steps) {
	if (sa1.io.hen || sa1.io.ven) {
		return false;
	}
	if (sa1.io.hvselb == 0) {
		if ((sa1.status.hcounter & 1) || sa1.status.hcounter >= 1364 || !sa1.status.scanlines ||
		    sa1.status.vcounter >= sa1.status.scanlines) {
			return false;
		}
		u64 clocks = sa1.status.hcounter + steps * 2;
		sa1.status.vcounter = (sa1.status.vcounter + clocks / 1364) % sa1.status.scanlines;
		sa1.status.hcounter = clocks % 1364;
	} else {
		if (sa1.status.hcounter >= 0x800 || sa1.status.vcounter >= 0x200) {
			return false;
		}
		u64 count = ((u64) sa1.status.vcounter << 11 | sa1.status.hcounter) + steps * 2;
		sa1.status.hcounter = count & 0x07ff;
		sa1.status.vcounter = count >> 11 & 0x01ff;
	}
	return true;
}

void sa1Run(void) {
	// The S-CPU has run since: whatever was known of a loop is gone.
	loop.valid = false;
dispatch:
	switch (sa1.state) {
	case MAIN:
		if (systemBalanced && cpu.th.clock < sa1.th.clock) {
			return;
		}
		if (R.wai || R.stp || sa1.io.sa1_rdyb || sa1.io.sa1_resb || sa1.status.interruptPending) {
			loop.valid = false;
		}
		if (R.wai) {
			while (R.wai) {
				lastCycle();
				IDLE();
			}
			IDLE();
			goto done;
		}
		if (R.stp) {
			lastCycle();
			IDLE();
			goto done;
		}
		if (sa1.io.sa1_rdyb || sa1.io.sa1_resb) {
			// Asleep
			STEP();
			goto done;
		}
		if (sa1.status.interruptPending) {
			sa1.status.interruptPending = false;
			// The SA-1's vectors are in its registers: PC is set from them, bank 0.
			READ(R.dr, PC.d);
			IDLE();
			if (!EF) {
				PUSH(PC.b);
			}
			PUSH(PC.h);
			PUSH(PC.l);
			PUSH(EF ? getP() & ~0x10 : getP());
			IF = 1;
			DF = 0;
			PC.d = R.vector;
			goto done;
		}
		if (PC.d <= loop.last) {
			// A jump back.
			if (loop.valid && PC.d == loop.pc && !memcmp(&R, &loop.r, sizeof(R)) && sa1.th.clock > loop.clock &&
			    sa1.th.clock <= cpu.th.clock) {
				// The same again: as many whole turns of the loop as end before the S-CPU's clock
				// are taken in one.
				u64 turn = sa1.th.clock - loop.clock;
				u64 turns = (cpu.th.clock - sa1.th.clock) / turn;
				if (turns && countersSkip(turns * (turn / (sa1.th.scalar * 2)))) {
					sa1.th.clock += turns * turn;
				}
			}
			loop.valid = true;
			loop.pc = PC.d;
			loop.clock = sa1.th.clock;
			loop.r = R;
		}
		loop.last = PC.d;
		FETCH(R.op);
		sa1.state = R.op;
		goto dispatch;

	W65816_DISPATCH

	case SUB_READ:
		R.mar = R.ad;
		R.dr = R.mdr;
		if ((R.ad & 0x40fe00) == 0x002200) {
			if (!ioReadRepeats(R.ad)) {
				loop.valid = false;
			}
			STEP();
			R.dr = R.mdr = readIOSA1(R.ad, R.dr);
			RETURN();
		}
		if ((R.ad & 0x408000) == 0x008000 || (R.ad & 0xc00000) == 0xc00000) {
			STEP();
			if (romConflict()) {
				STEP();
			}
			R.dr = R.mdr = romReadSA1(R.ad, R.dr);
			RETURN();
		}
		if ((R.ad & 0x40e000) == 0x006000 || (R.ad & 0xe00000) == 0x400000 || (R.ad & 0xf00000) == 0x600000) {
			STEP();
			STEP();
			if (bwramConflict()) {
				STEP();
			}
			if (bwramConflict()) {
				STEP();
			}
			if ((R.ad >> 22 & 1) && (R.ad >> 21 & 1)) {
				R.dr = R.mdr = bwramReadBitmap(R.ad, R.dr);
			} else if (R.ad >> 22 & 1) {
				R.dr = R.mdr = bwramReadLinear(R.ad, R.dr);
			} else {
				R.dr = R.mdr = bwramReadSA1(R.ad, R.dr);
			}
			RETURN();
		}
		if ((R.ad & 0x40f800) == 0x000000 || (R.ad & 0x40f800) == 0x003000) {
			STEP();
			if (iramConflict()) {
				STEP();
			}
			if (iramConflict()) {
				STEP();
			}
			R.dr = R.mdr = iramRead(R.ad);
			RETURN();
		}
		STEP();
		RETURN();

	case SUB_WRITE:
		R.mar = R.ad;
		R.mdr = R.dw;
		if ((R.ad & 0x40fe00) == 0x002200) {
			STEP();
			writeIOSA1(R.ad, R.dw);
			if (sa1.dmaStart) {
				sa1.dmaStart = false;
				CALL(SUB_DMA);
			}
			RETURN();
		}
		if ((R.ad & 0x408000) == 0x008000 || (R.ad & 0xc00000) == 0xc00000) {
			STEP();
			if (romConflict()) {
				STEP();
			}
			RETURN();
		}
		if ((R.ad & 0x40e000) == 0x006000 || (R.ad & 0xe00000) == 0x400000 || (R.ad & 0xf00000) == 0x600000) {
			STEP();
			STEP();
			if (bwramConflict()) {
				STEP();
			}
			if (bwramConflict()) {
				STEP();
			}
			if ((R.ad >> 22 & 1) && (R.ad >> 21 & 1)) {
				bwramWriteBitmap(R.ad, R.dw);
			} else if (R.ad >> 22 & 1) {
				bwramWriteLinear(R.ad, R.dw);
			} else {
				bwramWriteSA1(R.ad, R.dw);
			}
			RETURN();
		}
		if ((R.ad & 0x40f800) == 0x000000 || (R.ad & 0x40f800) == 0x003000) {
			STEP();
			if (iramConflict()) {
				STEP();
			}
			if (iramConflict()) {
				STEP();
			}
			iramWriteSA1(R.ad, R.dw);
			RETURN();
		}
		STEP();
		RETURN();

	case SUB_IDLE:
		STEP();
		RETURN();

	// Jumps and returns: a penalty cycle when PC is in ROM.
	case SUB_IDLE_JUMP:
		if ((PC.d & 0x408000) == 0x008000 || (PC.d & 0xc00000) == 0xc00000) {
			STEP();
			if (romConflict()) {
				STEP();
			}
		}
		RETURN();

	// A normal DMA one of the SA-1's writes started.
	case SUB_DMA:
		for (;;) {
			{
				u16 count = sa1.io.dtc--;
				if (!count) {
					break;
				}
			}
			sa1.dmaData = R.mdr;
			sa1.dmaSource = sa1.io.dsa;
			sa1.dmaTarget = sa1.io.dda;
			sa1.io.dsa = (sa1.io.dsa + 1) & 0xffffff;
			sa1.io.dda = (sa1.io.dda + 1) & 0xffffff;

			if (sa1.io.sd == SOURCE_ROM && sa1.io.dd == DEST_BWRAM) {
				STEP();
				STEP();
				if (bwramConflict()) {
					STEP();
				}
				if (bwramConflict()) {
					STEP();
				}
				sa1.dmaData = romReadSA1(sa1.dmaSource, sa1.dmaData);
				bwramWrite(sa1.dmaTarget, sa1.dmaData);
			}
			if (sa1.io.sd == SOURCE_ROM && sa1.io.dd == DEST_IRAM) {
				STEP();
				if (iramConflict() || romConflict()) {
					STEP();
				}
				if (iramConflict()) {
					STEP();
				}
				sa1.dmaData = romReadSA1(sa1.dmaSource, sa1.dmaData);
				iramWrite(sa1.dmaTarget, sa1.dmaData);
			}
			if (sa1.io.sd == SOURCE_BWRAM && sa1.io.dd == DEST_IRAM) {
				STEP();
				STEP();
				if (bwramConflict() || iramConflict()) {
					STEP();
				}
				if (bwramConflict()) {
					STEP();
				}
				sa1.dmaData = bwramRead(sa1.dmaSource, sa1.dmaData);
				iramWrite(sa1.dmaTarget, sa1.dmaData);
			}
			if (sa1.io.sd == SOURCE_IRAM && sa1.io.dd == DEST_BWRAM) {
				STEP();
				STEP();
				if (bwramConflict() || iramConflict()) {
					STEP();
				}
				if (bwramConflict()) {
					STEP();
				}
				sa1.dmaData = iramRead(sa1.dmaSource);
				bwramWrite(sa1.dmaTarget, sa1.dmaData);
			}
		}
		sa1.io.dma_irqfl = true;
		if (sa1.io.dma_irqen) {
			sa1.io.dma_irqcl = 0;
		}
		RETURN();
	}

done:
	sa1.state = MAIN;
	goto dispatch;
}

void sa1Power(void) {
	struct Thread th = sa1.th;
	memset(&sa1, 0, sizeof(sa1));
	sa1.th = th;
	threadFrequency(&sa1.th, sys.cpuFrequency);
	sa1.state = MAIN;

	PC.d = 0x000000;
	A.w = 0x0000;
	X.w = 0x0000;
	Y.w = 0x0000;
	S.w = 0x01ff;
	D.w = 0x0000;
	B = 0x00;
	setP(0x34);
	EF = 1;
	R.irq = 0;
	R.wai = 0;
	R.stp = 0;
	R.mar = 0x000000;
	R.mdr = 0x00;
	R.vector = 0xfffc;

	sa1.status.scanlines = sys.pal ? 312 : 262;

	sa1.io.sa1_resb = true;
	sa1.io.db = 0x01;
	sa1.io.eb = 0x02;
	sa1.io.fb = 0x03;
	sa1.io.bwp = 0x0f;
	sa1.io.vb = 16;
}
