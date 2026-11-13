/* Copyright (c) 2004-2025 ares team, Near et al
 * Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Ported from ares (ISC); see LICENSES/ISC-ares.txt. */
#include "../snes.h"

// The NEC uPD7725 (DSP-1 to DSP-4) and uPD96050 (ST010, ST011): a 16-bit DSP running its own program
// from ROM. The CPU talks to it through a data register and a status register. It runs one
// instruction a clock, and like ares's, only catches up to the CPU when the CPU reads or writes it and
// once a scanline.

struct NecDsp necdsp;
struct NecDspFirmware necdspFirmware;

static inline u16 getSR(void) {
	const struct NecDspStatus* sr = &necdsp.sr;
	// With DRC set, DRS reads as 0.
	bool drs = sr->drs & !sr->drc;
	return sr->p0 << 0 | sr->p1 << 1 | sr->ei << 7 | sr->sic << 8 | sr->soc << 9 | sr->drc << 10 | sr->dma << 11 | drs << 12 |
		sr->usf0 << 13 | sr->usf1 << 14 | sr->rqm << 15;
}

static inline void setSR(u16 data) {
	struct NecDspStatus* sr = &necdsp.sr;
	sr->p0 = data >> 0 & 1;
	sr->p1 = data >> 1 & 1;
	sr->ei = data >> 7 & 1;
	sr->sic = data >> 8 & 1;
	sr->soc = data >> 9 & 1;
	sr->drc = data >> 10 & 1;
	sr->dma = data >> 11 & 1;
	sr->drs = data >> 12 & 1;
	sr->usf0 = data >> 13 & 1;
	sr->usf1 = data >> 14 & 1;
	sr->rqm = data >> 15 & 1;
}

static inline void setFlag(struct NecDspFlag* flag, u16 data) {
	flag->ov0 = data >> 0 & 1;
	flag->ov1 = data >> 1 & 1;
	flag->z = data >> 2 & 1;
	flag->c = data >> 3 & 1;
	flag->s0 = data >> 4 & 1;
	flag->s1 = data >> 5 & 1;
}

static inline u16 getFlag(const struct NecDspFlag* flag) {
	return flag->ov0 << 0 | flag->ov1 << 1 | flag->z << 2 | flag->c << 3 | flag->s0 << 4 | flag->s1 << 5;
}

// The registers' widths differ between the two chips.
#define PC_MASK (necdsp.uPD96050 ? 0x3fff : 0x7ff)
#define RP_MASK (necdsp.uPD96050 ? 0x7ff : 0x3ff)
#define DP_MASK (necdsp.uPD96050 ? 0x7ff : 0xff)

static void execLD(u32 opcode) {
	u16 id = opcode >> 6;
	u8 dst = opcode & 15;

	switch (dst) {
	case 0:
		break;
	case 1:
		necdsp.a = id;
		break;
	case 2:
		necdsp.b = id;
		break;
	case 3:
		necdsp.tr = id;
		break;
	case 4:
		necdsp.dp = id & DP_MASK;
		break;
	case 5:
		necdsp.rp = id & RP_MASK;
		break;
	case 6:
		necdsp.dr = id;
		necdsp.sr.rqm = 1;
		break;
	case 7:
		setSR((getSR() & 0x907c) | (id & ~0x907c));
		break;
	case 8:
	case 9:
		necdsp.so = id;
		break;
	case 10:
		necdsp.k = id;
		break;
	case 11:
		necdsp.k = id;
		necdsp.l = necdspFirmware.dataROM[necdsp.rp];
		break;
	case 12:
		necdsp.l = id;
		necdsp.k = necdsp.dataRAM[necdsp.dp | 0x40];
		break;
	case 13:
		necdsp.l = id;
		break;
	case 14:
		necdsp.trb = id;
		break;
	case 15:
		necdsp.dataRAM[necdsp.dp] = id;
		break;
	}
}

static void execOP(u32 opcode) {
	u8 pselect = opcode >> 20 & 3;
	u8 alu = opcode >> 16 & 15;
	u8 asl = opcode >> 15 & 1;
	u8 dpl = opcode >> 13 & 3;
	u8 dphm = opcode >> 9 & 15;
	u8 rpdcr = opcode >> 8 & 1;
	u8 src = opcode >> 4 & 15;
	u8 dst = opcode >> 0 & 15;

	u16 idb = 0;
	switch (src) {
	case 0: idb = necdsp.trb; break;
	case 1: idb = necdsp.a; break;
	case 2: idb = necdsp.b; break;
	case 3: idb = necdsp.tr; break;
	case 4: idb = necdsp.dp; break;
	case 5: idb = necdsp.rp; break;
	case 6: idb = necdspFirmware.dataROM[necdsp.rp]; break;
	// ASL is ignored: always SA1.
	case 7: idb = 0x8000 - necdsp.flagA.s1; break;
	case 8:
		idb = necdsp.dr;
		necdsp.sr.rqm = 1;
		break;
	case 9: idb = necdsp.dr; break;
	case 10: idb = getSR(); break;
	case 11: idb = necdsp.si; break;
	case 12: idb = necdsp.si; break;
	case 13: idb = necdsp.k; break;
	case 14: idb = necdsp.l; break;
	case 15: idb = necdsp.dataRAM[necdsp.dp]; break;
	}

	if (alu) {
		u16 p = 0, q = 0, r = 0;
		struct NecDspFlag flag;
		bool c = 0;

		switch (pselect) {
		case 0: p = necdsp.dataRAM[necdsp.dp]; break;
		case 1: p = idb; break;
		case 2: p = necdsp.m; break;
		case 3: p = necdsp.n; break;
		}

		if (!asl) {
			q = necdsp.a;
			flag = necdsp.flagA;
			c = necdsp.flagB.c;
		} else {
			q = necdsp.b;
			flag = necdsp.flagB;
			c = necdsp.flagA.c;
		}

		switch (alu) {
		case 1: r = q | p; break;
		case 2: r = q & p; break;
		case 3: r = q ^ p; break;
		case 4: r = q - p; break;
		case 5: r = q + p; break;
		case 6: r = q - p - c; break;
		case 7: r = q + p + c; break;
		case 8:
			r = q - 1;
			p = 1;
			break;
		case 9:
			r = q + 1;
			p = 1;
			break;
		case 10: r = ~q; break;
		case 11: r = q >> 1 | (q & 0x8000); break;
		case 12: r = q << 1 | c; break;
		case 13: r = q << 2 | 3; break;
		case 14: r = q << 4 | 15; break;
		case 15: r = q << 8 | q >> 8; break;
		}

		flag.z = r == 0;
		flag.s0 = r & 0x8000;
		if (!flag.ov1) {
			flag.s1 = flag.s0;
		}

		switch (alu) {
		case 1:
		case 2:
		case 3:
		case 10:
		case 13:
		case 14:
		case 15:
			flag.ov0 = 0;
			flag.ov1 = 0;
			flag.c = 0;
			break;
		case 4:
		case 5:
		case 6:
		case 7:
		case 8:
		case 9: {
			u16 carries = q ^ p ^ r;
			u16 overflow = (q ^ r) & (p ^ (alu & 1 ? r : q));
			flag.ov0 = overflow & 0x8000;
			flag.ov1 = flag.ov0 & flag.ov1 ? flag.s0 == flag.s1 : flag.ov0 | flag.ov1;
			flag.c = (carries ^ overflow) & 0x8000;
			break;
		}
		case 11:
			flag.ov0 = 0;
			flag.ov1 = 0;
			flag.c = q & 1;
			break;
		case 12:
			flag.ov0 = 0;
			flag.ov1 = 0;
			flag.c = q >> 15;
			break;
		}

		if (!asl) {
			necdsp.a = r;
			necdsp.flagA = flag;
		} else {
			necdsp.b = r;
			necdsp.flagB = flag;
		}
	}

	execLD(idb << 6 | dst);

	// Unless the load set DP.
	if (dst != 4) {
		switch (dpl) {
		case 1: necdsp.dp = (necdsp.dp & 0xf0) + ((necdsp.dp + 1) & 0x0f); break;
		case 2: necdsp.dp = (necdsp.dp & 0xf0) + ((necdsp.dp - 1) & 0x0f); break;
		case 3: necdsp.dp = necdsp.dp & 0xf0; break;
		}
		necdsp.dp = (necdsp.dp ^ dphm << 4) & DP_MASK;
	}

	// Unless the load set RP.
	if (dst != 5) {
		if (rpdcr) {
			necdsp.rp = (necdsp.rp - 1) & RP_MASK;
		}
	}
}

static void execRT(u32 opcode) {
	execOP(opcode);
	necdsp.sp = (necdsp.sp - 1) & 15;
	necdsp.pc = necdsp.stack[necdsp.sp] & PC_MASK;
}

static void execJP(u32 opcode) {
	u16 brch = opcode >> 13 & 0x1ff;
	u16 na = opcode >> 2 & 0x7ff;
	u8 bank = opcode & 3;

	u16 jp = ((necdsp.pc & 0x2000) | bank << 11 | na) & PC_MASK;
	const struct NecDspFlag* fa = &necdsp.flagA;
	const struct NecDspFlag* fb = &necdsp.flagB;
	bool take;

	switch (brch) {
	case 0x000:
		necdsp.pc = necdsp.so & PC_MASK;
		return;
	case 0x080: take = fa->c == 0; break;
	case 0x082: take = fa->c == 1; break;
	case 0x084: take = fb->c == 0; break;
	case 0x086: take = fb->c == 1; break;
	case 0x088: take = fa->z == 0; break;
	case 0x08a: take = fa->z == 1; break;
	case 0x08c: take = fb->z == 0; break;
	case 0x08e: take = fb->z == 1; break;
	case 0x090: take = fa->ov0 == 0; break;
	case 0x092: take = fa->ov0 == 1; break;
	case 0x094: take = fb->ov0 == 0; break;
	case 0x096: take = fb->ov0 == 1; break;
	case 0x098: take = fa->ov1 == 0; break;
	case 0x09a: take = fa->ov1 == 1; break;
	case 0x09c: take = fb->ov1 == 0; break;
	case 0x09e: take = fb->ov1 == 1; break;
	case 0x0a0: take = fa->s0 == 0; break;
	case 0x0a2: take = fa->s0 == 1; break;
	case 0x0a4: take = fb->s0 == 0; break;
	case 0x0a6: take = fb->s0 == 1; break;
	case 0x0a8: take = fa->s1 == 0; break;
	case 0x0aa: take = fa->s1 == 1; break;
	case 0x0ac: take = fb->s1 == 0; break;
	case 0x0ae: take = fb->s1 == 1; break;
	case 0x0b0: take = (necdsp.dp & 0x0f) == 0x00; break;
	case 0x0b1: take = (necdsp.dp & 0x0f) != 0x00; break;
	case 0x0b2: take = (necdsp.dp & 0x0f) == 0x0f; break;
	case 0x0b3: take = (necdsp.dp & 0x0f) != 0x0f; break;
	// Serial input and output acknowledge aren't emulated.
	case 0x0b4: take = necdsp.siack == 0; break;
	case 0x0b6: take = necdsp.siack == 1; break;
	case 0x0b8: take = necdsp.soack == 0; break;
	case 0x0ba: take = necdsp.soack == 1; break;
	case 0x0bc: take = necdsp.sr.rqm == 0; break;
	case 0x0be: take = necdsp.sr.rqm == 1; break;
	case 0x100:
		necdsp.pc = jp & ~0x2000;
		return;
	case 0x101:
		necdsp.pc = (jp | 0x2000) & PC_MASK;
		return;
	case 0x140:
		necdsp.stack[necdsp.sp] = necdsp.pc;
		necdsp.sp = (necdsp.sp + 1) & 15;
		necdsp.pc = jp & ~0x2000;
		return;
	case 0x141:
		necdsp.stack[necdsp.sp] = necdsp.pc;
		necdsp.sp = (necdsp.sp + 1) & 15;
		necdsp.pc = (jp | 0x2000) & PC_MASK;
		return;
	default:
		return;
	}
	if (take) {
		necdsp.pc = jp;
	}
}

static void exec(void) {
	u32 opcode = necdspFirmware.programROM[necdsp.pc] & 0xffffff;
	necdsp.pc = (necdsp.pc + 1) & PC_MASK;
	switch (opcode >> 22) {
	case 0: execOP(opcode); break;
	case 1: execRT(opcode); break;
	case 2: execJP(opcode); break;
	case 3: execLD(opcode); break;
	}

	// Sign and a 30-bit result.
	s32 result = (s32) necdsp.k * necdsp.l;
	necdsp.m = result >> 15;
	necdsp.n = result << 1;
}

void necdspRun(void) {
	do {
		u16 pc = necdsp.pc;
		exec();
		threadStep(&necdsp.th, 1);
		// A jump to itself, as the program waits for the S-CPU, changes nothing until the S-CPU
		// does: all but the last of its turns up to the CPU are taken in one.
		if (necdsp.pc == pc && necdsp.th.clock <= cpu.th.clock) {
			u32 opcode = necdspFirmware.programROM[pc] & 0xffffff;
			u32 brch = opcode >> 13 & 0x1ff;
			if (opcode >> 22 == 2 && brch != 0x000 && brch != 0x140 && brch != 0x141) {
				necdsp.th.clock += (cpu.th.clock - necdsp.th.clock) / necdsp.th.scalar * necdsp.th.scalar;
			}
		}
	} while (!(cpu.th.clock < necdsp.th.clock));
}

static inline void syncFromCpu(void) {
	if (necdsp.th.clock < cpu.th.clock) {
		necdspRun();
	}
}

static u8 readDR(void) {
	if (necdsp.sr.drc == 0) {
		// 16 bits
		if (necdsp.sr.drs == 0) {
			necdsp.sr.drs = 1;
			return necdsp.dr;
		}
		necdsp.sr.rqm = 0;
		necdsp.sr.drs = 0;
		return necdsp.dr >> 8;
	}
	// 8 bits
	necdsp.sr.rqm = 0;
	return necdsp.dr;
}

static void writeDR(u8 data) {
	if (necdsp.sr.drc == 0) {
		if (necdsp.sr.drs == 0) {
			necdsp.sr.drs = 1;
			necdsp.dr = (necdsp.dr & 0xff00) | data;
		} else {
			necdsp.sr.rqm = 0;
			necdsp.sr.drs = 0;
			necdsp.dr = data << 8 | (necdsp.dr & 0x00ff);
		}
	} else {
		necdsp.sr.rqm = 0;
		necdsp.dr = (necdsp.dr & 0xff00) | data;
	}
}

// Bit 0 of the reduced address picks the status register (1) or the data register (0).
u8 necdspRead(u32 address, u8 data) {
	(void) data;
	syncFromCpu();
	if (address & 1) {
		return getSR() >> 8;
	}
	return readDR();
}

void necdspWrite(u32 address, u8 data) {
	syncFromCpu();
	if (address & 1) {
		// The status register can't be written from outside.
		return;
	}
	writeDR(data);
}

u8 necdspReadRAM(u32 address, u8 data) {
	(void) data;
	syncFromCpu();
	bool hi = address & 1;
	u16 word = necdsp.dataRAM[(address >> 1) & 2047];
	return hi ? word >> 8 : word;
}

void necdspWriteRAM(u32 address, u8 data) {
	syncFromCpu();
	bool hi = address & 1;
	u16* word = &necdsp.dataRAM[(address >> 1) & 2047];
	*word = hi ? (*word & 0x00ff) | data << 8 : (*word & 0xff00) | data;
}

void necdspPower(void) {
	struct Thread th = necdsp.th;
	bool uPD96050 = necdsp.uPD96050;
	u32 frequency = necdsp.frequency;
	u16 dataRAM[2048];
	memcpy(dataRAM, necdsp.dataRAM, sizeof(dataRAM));
	memset(&necdsp, 0, sizeof(necdsp));
	necdsp.th = th;
	necdsp.uPD96050 = uPD96050;
	necdsp.frequency = frequency;
	memcpy(necdsp.dataRAM, dataRAM, sizeof(dataRAM));
	threadFrequency(&necdsp.th, frequency);
}
