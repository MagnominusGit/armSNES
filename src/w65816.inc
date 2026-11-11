/* Copyright (c) 2004-2025 ares team, Near et al
 * Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Ported from ares (ISC); see LICENSES/ISC-ares.txt. */

// The WDC 65C816, for the file that includes this to make a processor of: it defines R (the
// registers, a struct W65816), and read, write, idle, lastCycle and interruptPending.
//
// lastCycle() comes before an instruction's last cycle, where the processor looks for interrupts.
// A stopped or waiting processor is the includer's to run: see instructionWait.

#define PC R.pc
#define A R.a
#define X R.x
#define Y R.y
#define Z R.z
#define S R.s
#define D R.d
#define B R.b
#define CF R.c
#define ZF R.zf
#define IF R.i
#define DF R.df
#define XF R.xf
#define MF R.m
#define VF R.vf
#define NF R.n
#define EF R.e
#define U R.u
#define V R.v
#define W R.w
// Only the SA-1 does anything on these.
#define idleBranch()
#define idleJump()

typedef u8 (*Alu8)(u8);
typedef u16 (*Alu16)(u16);

static inline union Reg16 reg16(u32 value) {
	union Reg16 reg;
	reg.w = value;
	return reg;
}

static u8 getP(void) {
	return CF << 0 | ZF << 1 | IF << 2 | DF << 3 | XF << 4 | MF << 5 | VF << 6 | NF << 7;
}

static void setP(u8 data) {
	CF = data & 1;
	ZF = data & 2;
	IF = data & 4;
	DF = data & 8;
	XF = data & 16;
	MF = data & 32;
	VF = data & 64;
	NF = data & 128;
}

static void w65816Power(void) {
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
}

// An immediate two-cycle opcode's idle cycle becomes a bus read when an interrupt is about to be
// taken.
static inline void idleIRQ(void) {
	if (interruptPending()) {
		read(PC.d);
	} else {
		idle();
	}
}

static inline void idle2(void) {
	if (D.l) {
		idle();
	}
}

static inline void idle4(u16 x, u16 y) {
	if (!XF || x >> 8 != y >> 8) {
		idle();
	}
}

static inline void idle6(u16 address) {
	if (EF && PC.h != address >> 8) {
		idle();
	}
}

static inline u8 fetch(void) {
	u32 address = PC.b << 16 | PC.w;
	PC.w++;
	return read(address);
}

static inline u8 pull(void) {
	if (EF) {
		S.l++;
	} else {
		S.w++;
	}
	return read(S.w);
}

static void push(u8 data) {
	write(S.w, data);
	if (EF) {
		S.l--;
	} else {
		S.w--;
	}
}

static inline u8 pullN(void) {
	return read(++S.w);
}

static inline void pushN(u8 data) {
	write(S.w--, data);
}

static inline u8 readDirect(u32 address) {
	if (EF && !D.l) {
		return read(D.w | (u8) address);
	}
	return read((u16) (D.w + address));
}

static inline void writeDirect(u32 address, u8 data) {
	if (EF && !D.l) {
		write(D.w | (u8) address, data);
		return;
	}
	write((u16) (D.w + address), data);
}

// (direct,X) wraps the high byte within the page in emulation mode when D's low byte isn't zero.
static inline u8 readDirectX(u32 address, u32 offset) {
	if (EF && D.l) {
		return read(((D.w + address) & 0xffff00) | (u8) (D.w + address + offset));
	}
	return readDirect(address + offset);
}

static inline u8 readDirectN(u32 address) {
	return read((u16) (D.w + address));
}

static inline u8 readBank(u32 address) {
	return read((B << 16) + address);
}

static inline void writeBank(u32 address, u8 data) {
	write((B << 16) + address, data);
}

static inline u8 readStack(u32 address) {
	return read((u16) (S.w + address));
}

static inline void writeStack(u32 address, u8 data) {
	write((u16) (S.w + address), data);
}

static void interrupt(void) {
	read(PC.d);
	idle();
	if (!EF) {
		push(PC.b);
	}
	push(PC.h);
	push(PC.l);
	push(EF ? getP() & ~0x10 : getP());
	IF = 1;
	DF = 0;
	PC.l = read(R.vector + 0);
	lastCycle();
	PC.h = read(R.vector + 1);
	PC.b = 0x00;
	idleJump();
}

static u8 algorithmADC8(u8 data) {
	s32 result;

	if (!DF) {
		result = A.l + data + CF;
	} else {
		result = (A.l & 0x0f) + (data & 0x0f) + (CF << 0);
		if (result > 0x09) result += 0x06;
		CF = result > 0x0f;
		result = (A.l & 0xf0) + (data & 0xf0) + (CF << 4) + (result & 0x0f);
	}

	VF = ~(A.l ^ data) & (A.l ^ result) & 0x80;
	if (DF && result > 0x9f) result += 0x60;
	CF = result > 0xff;
	ZF = (u8) result == 0;
	NF = result & 0x80;

	return A.l = result;
}

static u16 algorithmADC16(u16 data) {
	s32 result;

	if (!DF) {
		result = A.w + data + CF;
	} else {
		result = (A.w & 0x000f) + (data & 0x000f) + (CF <<  0);
		if (result > 0x0009) result += 0x0006;
		CF = result > 0x000f;
		result = (A.w & 0x00f0) + (data & 0x00f0) + (CF <<  4) + (result & 0x000f);
		if (result > 0x009f) result += 0x0060;
		CF = result > 0x00ff;
		result = (A.w & 0x0f00) + (data & 0x0f00) + (CF <<  8) + (result & 0x00ff);
		if (result > 0x09ff) result += 0x0600;
		CF = result > 0x0fff;
		result = (A.w & 0xf000) + (data & 0xf000) + (CF << 12) + (result & 0x0fff);
	}

	VF = ~(A.w ^ data) & (A.w ^ result) & 0x8000;
	if (DF && result > 0x9fff) result += 0x6000;
	CF = result > 0xffff;
	ZF = (u16) result == 0;
	NF = result & 0x8000;

	return A.w = result;
}

static u8 algorithmAND8(u8 data) {
	A.l &= data;
	ZF = A.l == 0;
	NF = A.l & 0x80;
	return A.l;
}

static u16 algorithmAND16(u16 data) {
	A.w &= data;
	ZF = A.w == 0;
	NF = A.w & 0x8000;
	return A.w;
}

static u8 algorithmASL8(u8 data) {
	CF = data & 0x80;
	data <<= 1;
	ZF = data == 0;
	NF = data & 0x80;
	return data;
}

static u16 algorithmASL16(u16 data) {
	CF = data & 0x8000;
	data <<= 1;
	ZF = data == 0;
	NF = data & 0x8000;
	return data;
}

static u8 algorithmBIT8(u8 data) {
	ZF = (data & A.l) == 0;
	VF = data & 0x40;
	NF = data & 0x80;
	return data;
}

static u16 algorithmBIT16(u16 data) {
	ZF = (data & A.w) == 0;
	VF = data & 0x4000;
	NF = data & 0x8000;
	return data;
}

static u8 algorithmCMP8(u8 data) {
	s32 result = A.l - data;
	CF = result >= 0;
	ZF = (u8) result == 0;
	NF = result & 0x80;
	return result;
}

static u16 algorithmCMP16(u16 data) {
	s32 result = A.w - data;
	CF = result >= 0;
	ZF = (u16) result == 0;
	NF = result & 0x8000;
	return result;
}

static u8 algorithmCPX8(u8 data) {
	s32 result = X.l - data;
	CF = result >= 0;
	ZF = (u8) result == 0;
	NF = result & 0x80;
	return result;
}

static u16 algorithmCPX16(u16 data) {
	s32 result = X.w - data;
	CF = result >= 0;
	ZF = (u16) result == 0;
	NF = result & 0x8000;
	return result;
}

static u8 algorithmCPY8(u8 data) {
	s32 result = Y.l - data;
	CF = result >= 0;
	ZF = (u8) result == 0;
	NF = result & 0x80;
	return result;
}

static u16 algorithmCPY16(u16 data) {
	s32 result = Y.w - data;
	CF = result >= 0;
	ZF = (u16) result == 0;
	NF = result & 0x8000;
	return result;
}

static u8 algorithmDEC8(u8 data) {
	data--;
	ZF = data == 0;
	NF = data & 0x80;
	return data;
}

static u16 algorithmDEC16(u16 data) {
	data--;
	ZF = data == 0;
	NF = data & 0x8000;
	return data;
}

static u8 algorithmEOR8(u8 data) {
	A.l ^= data;
	ZF = A.l == 0;
	NF = A.l & 0x80;
	return A.l;
}

static u16 algorithmEOR16(u16 data) {
	A.w ^= data;
	ZF = A.w == 0;
	NF = A.w & 0x8000;
	return A.w;
}

static u8 algorithmINC8(u8 data) {
	data++;
	ZF = data == 0;
	NF = data & 0x80;
	return data;
}

static u16 algorithmINC16(u16 data) {
	data++;
	ZF = data == 0;
	NF = data & 0x8000;
	return data;
}

static u8 algorithmLDA8(u8 data) {
	A.l = data;
	ZF = A.l == 0;
	NF = A.l & 0x80;
	return data;
}

static u16 algorithmLDA16(u16 data) {
	A.w = data;
	ZF = A.w == 0;
	NF = A.w & 0x8000;
	return data;
}

static u8 algorithmLDX8(u8 data) {
	X.l = data;
	ZF = X.l == 0;
	NF = X.l & 0x80;
	return data;
}

static u16 algorithmLDX16(u16 data) {
	X.w = data;
	ZF = X.w == 0;
	NF = X.w & 0x8000;
	return data;
}

static u8 algorithmLDY8(u8 data) {
	Y.l = data;
	ZF = Y.l == 0;
	NF = Y.l & 0x80;
	return data;
}

static u16 algorithmLDY16(u16 data) {
	Y.w = data;
	ZF = Y.w == 0;
	NF = Y.w & 0x8000;
	return data;
}

static u8 algorithmLSR8(u8 data) {
	CF = data & 1;
	data >>= 1;
	ZF = data == 0;
	NF = data & 0x80;
	return data;
}

static u16 algorithmLSR16(u16 data) {
	CF = data & 1;
	data >>= 1;
	ZF = data == 0;
	NF = data & 0x8000;
	return data;
}

static u8 algorithmORA8(u8 data) {
	A.l |= data;
	ZF = A.l == 0;
	NF = A.l & 0x80;
	return A.l;
}

static u16 algorithmORA16(u16 data) {
	A.w |= data;
	ZF = A.w == 0;
	NF = A.w & 0x8000;
	return A.w;
}

static u8 algorithmROL8(u8 data) {
	bool carry = CF;
	CF = data & 0x80;
	data = data << 1 | carry;
	ZF = data == 0;
	NF = data & 0x80;
	return data;
}

static u16 algorithmROL16(u16 data) {
	bool carry = CF;
	CF = data & 0x8000;
	data = data << 1 | carry;
	ZF = data == 0;
	NF = data & 0x8000;
	return data;
}

static u8 algorithmROR8(u8 data) {
	bool carry = CF;
	CF = data & 1;
	data = carry << 7 | data >> 1;
	ZF = data == 0;
	NF = data & 0x80;
	return data;
}

static u16 algorithmROR16(u16 data) {
	bool carry = CF;
	CF = data & 1;
	data = carry << 15 | data >> 1;
	ZF = data == 0;
	NF = data & 0x8000;
	return data;
}

static u8 algorithmSBC8(u8 data) {
	s32 result;
	data = ~data;

	if (!DF) {
		result = A.l + data + CF;
	} else {
		result = (A.l & 0x0f) + (data & 0x0f) + (CF << 0);
		if (result <= 0x0f) result -= 0x06;
		CF = result > 0x0f;
		result = (A.l & 0xf0) + (data & 0xf0) + (CF << 4) + (result & 0x0f);
	}

	VF = ~(A.l ^ data) & (A.l ^ result) & 0x80;
	if (DF && result <= 0xff) result -= 0x60;
	CF = result > 0xff;
	ZF = (u8) result == 0;
	NF = result & 0x80;

	return A.l = result;
}

static u16 algorithmSBC16(u16 data) {
	s32 result;
	data = ~data;

	if (!DF) {
		result = A.w + data + CF;
	} else {
		result = (A.w & 0x000f) + (data & 0x000f) + (CF <<  0);
		if (result <= 0x000f) result -= 0x0006;
		CF = result > 0x000f;
		result = (A.w & 0x00f0) + (data & 0x00f0) + (CF <<  4) + (result & 0x000f);
		if (result <= 0x00ff) result -= 0x0060;
		CF = result > 0x00ff;
		result = (A.w & 0x0f00) + (data & 0x0f00) + (CF <<  8) + (result & 0x00ff);
		if (result <= 0x0fff) result -= 0x0600;
		CF = result > 0x0fff;
		result = (A.w & 0xf000) + (data & 0xf000) + (CF << 12) + (result & 0x0fff);
	}

	VF = ~(A.w ^ data) & (A.w ^ result) & 0x8000;
	if (DF && result <= 0xffff) result -= 0x6000;
	CF = result > 0xffff;
	ZF = (u16) result == 0;
	NF = result & 0x8000;

	return A.w = result;
}

static u8 algorithmTRB8(u8 data) {
	ZF = (data & A.l) == 0;
	data &= ~A.l;
	return data;
}

static u16 algorithmTRB16(u16 data) {
	ZF = (data & A.w) == 0;
	data &= ~A.w;
	return data;
}

static u8 algorithmTSB8(u8 data) {
	ZF = (data & A.l) == 0;
	data |= A.l;
	return data;
}

static u16 algorithmTSB16(u16 data) {
	ZF = (data & A.w) == 0;
	data |= A.w;
	return data;
}


static void instructionImmediateRead8(Alu8 op) {
	lastCycle(); W.l = fetch();
	op(W.l);
}

static void instructionImmediateRead16(Alu16 op) {
	W.l = fetch();
	lastCycle(); W.h = fetch();
	op(W.w);
}

static void instructionBankRead8(Alu8 op) {
	V.l = fetch();
	V.h = fetch();
	lastCycle(); W.l = readBank(V.w + 0);
	op(W.l);
}

static void instructionBankRead16(Alu16 op) {
	V.l = fetch();
	V.h = fetch();
	W.l = readBank(V.w + 0);
	lastCycle(); W.h = readBank(V.w + 1);
	op(W.w);
}

static void instructionBankReadI8(Alu8 op, union Reg16 I) {
	V.l = fetch();
	V.h = fetch();
	idle4(V.w, V.w + I.w);
	lastCycle(); W.l = readBank(V.w + I.w + 0);
	op(W.l);
}

static void instructionBankReadI16(Alu16 op, union Reg16 I) {
	V.l = fetch();
	V.h = fetch();
	idle4(V.w, V.w + I.w);
	W.l = readBank(V.w + I.w + 0);
	lastCycle(); W.h = readBank(V.w + I.w + 1);
	op(W.w);
}

static void instructionLongRead8(Alu8 op, union Reg16 I) {
	V.l = fetch();
	V.h = fetch();
	V.b = fetch();
	lastCycle(); W.l = read(V.d + I.w + 0);
	op(W.l);
}

static void instructionLongRead16(Alu16 op, union Reg16 I) {
	V.l = fetch();
	V.h = fetch();
	V.b = fetch();
	W.l = read(V.d + I.w + 0);
	lastCycle(); W.h = read(V.d + I.w + 1);
	op(W.w);
}

static void instructionDirectRead8(Alu8 op) {
	U.l = fetch();
	idle2();
	lastCycle(); W.l = readDirect(U.l + 0);
	op(W.l);
}

static void instructionDirectRead16(Alu16 op) {
	U.l = fetch();
	idle2();
	W.l = readDirect(U.l + 0);
	lastCycle(); W.h = readDirect(U.l + 1);
	op(W.w);
}

static void instructionDirectReadI8(Alu8 op, union Reg16 I) {
	U.l = fetch();
	idle2();
	idle();
	lastCycle(); W.l = readDirect(U.l + I.w + 0);
	op(W.l);
}

static void instructionDirectReadI16(Alu16 op, union Reg16 I) {
	U.l = fetch();
	idle2();
	idle();
	W.l = readDirect(U.l + I.w + 0);
	lastCycle(); W.h = readDirect(U.l + I.w + 1);
	op(W.w);
}

static void instructionIndirectRead8(Alu8 op) {
	U.l = fetch();
	idle2();
	V.l = readDirect(U.l + 0);
	V.h = readDirect(U.l + 1);
	lastCycle(); W.l = readBank(V.w + 0);
	op(W.l);
}

static void instructionIndirectRead16(Alu16 op) {
	U.l = fetch();
	idle2();
	V.l = readDirect(U.l + 0);
	V.h = readDirect(U.l + 1);
	W.l = readBank(V.w + 0);
	lastCycle(); W.h = readBank(V.w + 1);
	op(W.w);
}

static void instructionIndexedIndirectRead8(Alu8 op) {
	U.l = fetch();
	idle2();
	idle();
	V.l = readDirectX(U.l + X.w, 0);
	V.h = readDirectX(U.l + X.w, 1);
	lastCycle(); W.l = readBank(V.w + 0);
	op(W.l);
}

static void instructionIndexedIndirectRead16(Alu16 op) {
	U.l = fetch();
	idle2();
	idle();
	V.l = readDirectX(U.l + X.w, 0);
	V.h = readDirectX(U.l + X.w, 1);
	W.l = readBank(V.w + 0);
	lastCycle(); W.h = readBank(V.w + 1);
	op(W.w);
}

static void instructionIndirectIndexedRead8(Alu8 op) {
	U.l = fetch();
	idle2();
	V.l = readDirect(U.l + 0);
	V.h = readDirect(U.l + 1);
	idle4(V.w, V.w + Y.w);
	lastCycle(); W.l = readBank(V.w + Y.w + 0);
	op(W.l);
}

static void instructionIndirectIndexedRead16(Alu16 op) {
	U.l = fetch();
	idle2();
	V.l = readDirect(U.l + 0);
	V.h = readDirect(U.l + 1);
	idle4(V.w, V.w + Y.w);
	W.l = readBank(V.w + Y.w + 0);
	lastCycle(); W.h = readBank(V.w + Y.w + 1);
	op(W.w);
}

static void instructionIndirectLongRead8(Alu8 op, union Reg16 I) {
	U.l = fetch();
	idle2();
	V.l = readDirectN(U.l + 0);
	V.h = readDirectN(U.l + 1);
	V.b = readDirectN(U.l + 2);
	lastCycle(); W.l = read(V.d + I.w + 0);
	op(W.l);
}

static void instructionIndirectLongRead16(Alu16 op, union Reg16 I) {
	U.l = fetch();
	idle2();
	V.l = readDirectN(U.l + 0);
	V.h = readDirectN(U.l + 1);
	V.b = readDirectN(U.l + 2);
	W.l = read(V.d + I.w + 0);
	lastCycle(); W.h = read(V.d + I.w + 1);
	op(W.w);
}

static void instructionStackRead8(Alu8 op) {
	U.l = fetch();
	idle();
	lastCycle(); W.l = readStack(U.l + 0);
	op(W.l);
}

static void instructionStackRead16(Alu16 op) {
	U.l = fetch();
	idle();
	W.l = readStack(U.l + 0);
	lastCycle(); W.h = readStack(U.l + 1);
	op(W.w);
}

static void instructionIndirectStackRead8(Alu8 op) {
	U.l = fetch();
	idle();
	V.l = readStack(U.l + 0);
	V.h = readStack(U.l + 1);
	idle();
	lastCycle(); W.l = readBank(V.w + Y.w + 0);
	op(W.l);
}

static void instructionIndirectStackRead16(Alu16 op) {
	U.l = fetch();
	idle();
	V.l = readStack(U.l + 0);
	V.h = readStack(U.l + 1);
	idle();
	W.l = readBank(V.w + Y.w + 0);
	lastCycle(); W.h = readBank(V.w + Y.w + 1);
	op(W.w);
}


static void instructionBankWrite8(union Reg16 F) {
	V.l = fetch();
	V.h = fetch();
	lastCycle(); writeBank(V.w + 0, F.l);
}

static void instructionBankWrite16(union Reg16 F) {
	V.l = fetch();
	V.h = fetch();
	writeBank(V.w + 0, F.l);
	lastCycle(); writeBank(V.w + 1, F.h);
}

static void instructionBankWriteI8(union Reg16 F, union Reg16 I) {
	V.l = fetch();
	V.h = fetch();
	idle();
	lastCycle(); writeBank(V.w + I.w + 0, F.l);
}

static void instructionBankWriteI16(union Reg16 F, union Reg16 I) {
	V.l = fetch();
	V.h = fetch();
	idle();
	writeBank(V.w + I.w + 0, F.l);
	lastCycle(); writeBank(V.w + I.w + 1, F.h);
}

static void instructionLongWrite8(union Reg16 I) {
	V.l = fetch();
	V.h = fetch();
	V.b = fetch();
	lastCycle(); write(V.d + I.w + 0, A.l);
}

static void instructionLongWrite16(union Reg16 I) {
	V.l = fetch();
	V.h = fetch();
	V.b = fetch();
	write(V.d + I.w + 0, A.l);
	lastCycle(); write(V.d + I.w + 1, A.h);
}

static void instructionDirectWrite8(union Reg16 F) {
	U.l = fetch();
	idle2();
	lastCycle(); writeDirect(U.l + 0, F.l);
}

static void instructionDirectWrite16(union Reg16 F) {
	U.l = fetch();
	idle2();
	writeDirect(U.l + 0, F.l);
	lastCycle(); writeDirect(U.l + 1, F.h);
}

static void instructionDirectWriteI8(union Reg16 F, union Reg16 I) {
	U.l = fetch();
	idle2();
	idle();
	lastCycle(); writeDirect(U.l + I.w + 0, F.l);
}

static void instructionDirectWriteI16(union Reg16 F, union Reg16 I) {
	U.l = fetch();
	idle2();
	idle();
	writeDirect(U.l + I.w + 0, F.l);
	lastCycle(); writeDirect(U.l + I.w + 1, F.h);
}

static void instructionIndirectWrite8(void) {
	U.l = fetch();
	idle2();
	V.l = readDirect(U.l + 0);
	V.h = readDirect(U.l + 1);
	lastCycle(); writeBank(V.w + 0, A.l);
}

static void instructionIndirectWrite16(void) {
	U.l = fetch();
	idle2();
	V.l = readDirect(U.l + 0);
	V.h = readDirect(U.l + 1);
	writeBank(V.w + 0, A.l);
	lastCycle(); writeBank(V.w + 1, A.h);
}

static void instructionIndexedIndirectWrite8(void) {
	U.l = fetch();
	idle2();
	idle();
	V.l = readDirectX(U.l + X.w, 0);
	V.h = readDirectX(U.l + X.w, 1);
	lastCycle(); writeBank(V.w + 0, A.l);
}

static void instructionIndexedIndirectWrite16(void) {
	U.l = fetch();
	idle2();
	idle();
	V.l = readDirectX(U.l + X.w, 0);
	V.h = readDirectX(U.l + X.w, 1);
	writeBank(V.w + 0, A.l);
	lastCycle(); writeBank(V.w + 1, A.h);
}

static void instructionIndirectIndexedWrite8(void) {
	U.l = fetch();
	idle2();
	V.l = readDirect(U.l + 0);
	V.h = readDirect(U.l + 1);
	idle();
	lastCycle(); writeBank(V.w + Y.w + 0, A.l);
}

static void instructionIndirectIndexedWrite16(void) {
	U.l = fetch();
	idle2();
	V.l = readDirect(U.l + 0);
	V.h = readDirect(U.l + 1);
	idle();
	writeBank(V.w + Y.w + 0, A.l);
	lastCycle(); writeBank(V.w + Y.w + 1, A.h);
}

static void instructionIndirectLongWrite8(union Reg16 I) {
	U.l = fetch();
	idle2();
	V.l = readDirectN(U.l + 0);
	V.h = readDirectN(U.l + 1);
	V.b = readDirectN(U.l + 2);
	lastCycle(); write(V.d + I.w + 0, A.l);
}

static void instructionIndirectLongWrite16(union Reg16 I) {
	U.l = fetch();
	idle2();
	V.l = readDirectN(U.l + 0);
	V.h = readDirectN(U.l + 1);
	V.b = readDirectN(U.l + 2);
	write(V.d + I.w + 0, A.l);
	lastCycle(); write(V.d + I.w + 1, A.h);
}

static void instructionStackWrite8(void) {
	U.l = fetch();
	idle();
	lastCycle(); writeStack(U.l + 0, A.l);
}

static void instructionStackWrite16(void) {
	U.l = fetch();
	idle();
	writeStack(U.l + 0, A.l);
	lastCycle(); writeStack(U.l + 1, A.h);
}

static void instructionIndirectStackWrite8(void) {
	U.l = fetch();
	idle();
	V.l = readStack(U.l + 0);
	V.h = readStack(U.l + 1);
	idle();
	lastCycle(); writeBank(V.w + Y.w + 0, A.l);
}

static void instructionIndirectStackWrite16(void) {
	U.l = fetch();
	idle();
	V.l = readStack(U.l + 0);
	V.h = readStack(U.l + 1);
	idle();
	writeBank(V.w + Y.w + 0, A.l);
	lastCycle(); writeBank(V.w + Y.w + 1, A.h);
}


static void instructionImpliedModify8(Alu8 op, union Reg16* M) {
	lastCycle(); idleIRQ();
	M->l = op(M->l);
}

static void instructionImpliedModify16(Alu16 op, union Reg16* M) {
	lastCycle(); idleIRQ();
	M->w = op(M->w);
}

static void instructionBankModify8(Alu8 op) {
	V.l = fetch();
	V.h = fetch();
	W.l = readBank(V.w + 0);
	idle();
	W.l = op(W.l);
	lastCycle(); writeBank(V.w + 0, W.l);
}

static void instructionBankModify16(Alu16 op) {
	V.l = fetch();
	V.h = fetch();
	W.l = readBank(V.w + 0);
	W.h = readBank(V.w + 1);
	idle();
	W.w = op(W.w);
	writeBank(V.w + 1, W.h);
	lastCycle(); writeBank(V.w + 0, W.l);
}

static void instructionBankIndexedModify8(Alu8 op) {
	V.l = fetch();
	V.h = fetch();
	idle();
	W.l = readBank(V.w + X.w + 0);
	idle();
	W.l = op(W.l);
	lastCycle(); writeBank(V.w + X.w + 0, W.l);
}

static void instructionBankIndexedModify16(Alu16 op) {
	V.l = fetch();
	V.h = fetch();
	idle();
	W.l = readBank(V.w + X.w + 0);
	W.h = readBank(V.w + X.w + 1);
	idle();
	W.w = op(W.w);
	writeBank(V.w + X.w + 1, W.h);
	lastCycle(); writeBank(V.w + X.w + 0, W.l);
}

static void instructionDirectModify8(Alu8 op) {
	U.l = fetch();
	idle2();
	W.l = readDirect(U.l + 0);
	idle();
	W.l = op(W.l);
	lastCycle(); writeDirect(U.l + 0, W.l);
}

static void instructionDirectModify16(Alu16 op) {
	U.l = fetch();
	idle2();
	W.l = readDirect(U.l + 0);
	W.h = readDirect(U.l + 1);
	idle();
	W.w = op(W.w);
	writeDirect(U.l + 1, W.h);
	lastCycle(); writeDirect(U.l + 0, W.l);
}

static void instructionDirectIndexedModify8(Alu8 op) {
	U.l = fetch();
	idle2();
	idle();
	W.l = readDirect(U.l + X.w + 0);
	idle();
	W.l = op(W.l);
	lastCycle(); writeDirect(U.l + X.w + 0, W.l);
}

static void instructionDirectIndexedModify16(Alu16 op) {
	U.l = fetch();
	idle2();
	idle();
	W.l = readDirect(U.l + X.w + 0);
	W.h = readDirect(U.l + X.w + 1);
	idle();
	W.w = op(W.w);
	writeDirect(U.l + X.w + 1, W.h);
	lastCycle(); writeDirect(U.l + X.w + 0, W.l);
}


static void instructionBranch(bool take) {
	if (!take) {
		lastCycle(); fetch();
	} else {
		U.l = fetch();
		V.w = PC.d + (s8) U.l;
		idle6(V.w);
		lastCycle(); idle();
		PC.w = V.w;
		idleBranch();
	}
}

static void instructionBranchLong(void) {
	U.l = fetch();
	U.h = fetch();
	V.w = PC.d + (s16) U.w;
	lastCycle(); idle();
	PC.w = V.w;
	idleBranch();
}

static void instructionJumpShort(void) {
	W.l = fetch();
	lastCycle(); W.h = fetch();
	PC.w = W.w;
	idleJump();
}

static void instructionJumpLong(void) {
	V.l = fetch();
	V.h = fetch();
	lastCycle(); V.b = fetch();
	PC.d = V.d;
	idleJump();
}

static void instructionJumpIndirect(void) {
	V.l = fetch();
	V.h = fetch();
	W.l = read((u16) (V.w + 0));
	lastCycle(); W.h = read((u16) (V.w + 1));
	PC.w = W.w;
	idleJump();
}

static void instructionJumpIndexedIndirect(void) {
	V.l = fetch();
	V.h = fetch();
	idle();
	W.l = read(PC.b << 16 | (u16) (V.w + X.w + 0));
	lastCycle(); W.h = read(PC.b << 16 | (u16) (V.w + X.w + 1));
	PC.w = W.w;
	idleJump();
}

static void instructionJumpIndirectLong(void) {
	U.l = fetch();
	U.h = fetch();
	V.l = read((u16) (U.w + 0));
	V.h = read((u16) (U.w + 1));
	lastCycle(); V.b = read((u16) (U.w + 2));
	PC.d = V.d;
	idleJump();
}

static void instructionCallShort(void) {
	W.l = fetch();
	W.h = fetch();
	idle();
	PC.w--;
	push(PC.h);
	lastCycle(); push(PC.l);
	PC.w = W.w;
	idleJump();
}

static void instructionCallLong(void) {
	V.l = fetch();
	V.h = fetch();
	pushN(PC.b);
	idle();
	V.b = fetch();
	PC.w--;
	pushN(PC.h);
	lastCycle(); pushN(PC.l);
	PC.d = V.d;
	if (EF) S.h = 0x01;
	idleJump();
}

static void instructionCallIndexedIndirect(void) {
	V.l = fetch();
	pushN(PC.h);
	pushN(PC.l);
	V.h = fetch();
	idle();
	W.l = read(PC.b << 16 | (u16) (V.w + X.w + 0));
	lastCycle(); W.h = read(PC.b << 16 | (u16) (V.w + X.w + 1));
	PC.w = W.w;
	if (EF) S.h = 0x01;
	idleJump();
}

static void instructionReturnInterrupt(void) {
	idle();
	idle();
	setP(pull());
	if (EF) XF = 1, MF = 1;
	if (XF) X.h = 0x00, Y.h = 0x00;
	PC.l = pull();
	if (EF) {
		lastCycle(); PC.h = pull();
	} else {
		PC.h = pull();
		lastCycle(); PC.b = pull();
	}
	idleJump();
}

static void instructionReturnShort(void) {
	idle();
	idle();
	W.l = pull();
	W.h = pull();
	lastCycle(); idle();
	PC.w = W.w;
	PC.w++;
	idleJump();
}

static void instructionReturnLong(void) {
	idle();
	idle();
	V.l = pullN();
	V.h = pullN();
	lastCycle(); V.b = pullN();
	PC.d = V.d;
	PC.w++;
	if (EF) S.h = 0x01;
	idleJump();
}


static void instructionBitImmediate8(void) {
	lastCycle(); U.l = fetch();
	ZF = (U.l & A.l) == 0;
}

static void instructionBitImmediate16(void) {
	U.l = fetch();
	lastCycle(); U.h = fetch();
	ZF = (U.w & A.w) == 0;
}

static void instructionNoOperation(void) {
	lastCycle(); idleIRQ();
}

static void instructionPrefix(void) {
	lastCycle(); fetch();
}

static void instructionExchangeBA(void) {
	idle();
	lastCycle(); idle();
	A.w = A.w >> 8 | A.w << 8;
	ZF = A.l == 0;
	NF = A.l & 0x80;
}

static void instructionBlockMove8(s32 adjust) {
	U.b = fetch();
	V.b = fetch();
	B = U.b;
	W.l = read(V.b << 16 | X.w);
	write(U.b << 16 | Y.w, W.l);
	idle();
	X.l += adjust;
	Y.l += adjust;
	lastCycle(); idle();
	if (A.w--) PC.w -= 3;
}

static void instructionBlockMove16(s32 adjust) {
	U.b = fetch();
	V.b = fetch();
	B = U.b;
	W.l = read(V.b << 16 | X.w);
	write(U.b << 16 | Y.w, W.l);
	idle();
	X.w += adjust;
	Y.w += adjust;
	lastCycle(); idle();
	if (A.w--) PC.w -= 3;
}

static void instructionInterrupt(union Reg16 vector) {
	fetch();
	if (!EF) push(PC.b);
	push(PC.h);
	push(PC.l);
	push(getP());
	IF = 1;
	DF = 0;
	PC.l = read(vector.w + 0);
	lastCycle(); PC.h = read(vector.w + 1);
	PC.b = 0x00;
}

static void instructionStop(void) {
	R.stp = true;
}

static void instructionWait(void) {
	R.wai = true;
}

static void instructionExchangeCE(void) {
	lastCycle(); idleIRQ();
	{ bool carry = CF; CF = EF; EF = carry; }
	if (EF) {
		XF = 1;
		MF = 1;
		X.h = 0x00;
		Y.h = 0x00;
		S.h = 0x01;
	}
}

static void instructionSetFlag(bool* flag) {
	lastCycle(); idleIRQ();
	*flag = 1;
}

static void instructionClearFlag(bool* flag) {
	lastCycle(); idleIRQ();
	*flag = 0;
}

static void instructionResetP(void) {
	W.l = fetch();
	lastCycle(); idle();
	setP(getP() & ~W.l);
	if (EF) XF = 1, MF = 1;
	if (XF) X.h = 0x00, Y.h = 0x00;
}

static void instructionSetP(void) {
	W.l = fetch();
	lastCycle(); idle();
	setP(getP() | W.l);
	if (EF) XF = 1, MF = 1;
	if (XF) X.h = 0x00, Y.h = 0x00;
}

static void instructionTransfer8(union Reg16 F, union Reg16* T) {
	lastCycle(); idleIRQ();
	T->l = F.l;
	ZF = T->l == 0;
	NF = T->l & 0x80;
}

static void instructionTransfer16(union Reg16 F, union Reg16* T) {
	lastCycle(); idleIRQ();
	T->w = F.w;
	ZF = T->w == 0;
	NF = T->w & 0x8000;
}

static void instructionTransferCS(void) {
	lastCycle(); idleIRQ();
	S.w = A.w;
	if (EF) S.h = 0x01;
}

static void instructionTransferSX8(void) {
	lastCycle(); idleIRQ();
	X.l = S.l;
	ZF = X.l == 0;
	NF = X.l & 0x80;
}

static void instructionTransferSX16(void) {
	lastCycle(); idleIRQ();
	X.w = S.w;
	ZF = X.w == 0;
	NF = X.w & 0x8000;
}

static void instructionTransferXS(void) {
	lastCycle(); idleIRQ();
	if (EF) S.l = X.l;
	if (!EF) S.w = X.w;
}

static void instructionPush8(union Reg16 F) {
	idle();
	lastCycle(); push(F.l);
}

static void instructionPush16(union Reg16 F) {
	idle();
	push(F.h);
	lastCycle(); push(F.l);
}

static void instructionPushD(void) {
	idle();
	pushN(D.h);
	lastCycle(); pushN(D.l);
	if (EF) S.h = 0x01;
}

static void instructionPull8(union Reg16* T) {
	idle();
	idle();
	lastCycle(); T->l = pull();
	ZF = T->l == 0;
	NF = T->l & 0x80;
}

static void instructionPull16(union Reg16* T) {
	idle();
	idle();
	T->l = pull();
	lastCycle(); T->h = pull();
	ZF = T->w == 0;
	NF = T->w & 0x8000;
}

static void instructionPullD(void) {
	idle();
	idle();
	D.l = pullN();
	lastCycle(); D.h = pullN();
	ZF = D.w == 0;
	NF = D.w & 0x8000;
	if (EF) S.h = 0x01;
}

static void instructionPullB(void) {
	idle();
	idle();
	lastCycle(); B = pullN();
	ZF = B == 0;
	NF = B & 0x80;
	if (EF) S.h = 0x01;
}

static void instructionPullP(void) {
	idle();
	idle();
	lastCycle(); setP(pull());
	if (EF) XF = 1, MF = 1;
	if (XF) X.h = 0x00, Y.h = 0x00;
}

static void instructionPushEffectiveAddress(void) {
	W.l = fetch();
	W.h = fetch();
	pushN(W.h);
	lastCycle(); pushN(W.l);
	if (EF) S.h = 0x01;
}

static void instructionPushEffectiveIndirectAddress(void) {
	U.l = fetch();
	idle2();
	W.l = readDirectN(U.l + 0);
	W.h = readDirectN(U.l + 1);
	pushN(W.h);
	lastCycle(); pushN(W.l);
	if (EF) S.h = 0x01;
}

static void instructionPushEffectiveRelativeAddress(void) {
	V.l = fetch();
	V.h = fetch();
	idle();
	W.w = PC.d + (s16) V.w;
	pushN(W.h);
	lastCycle(); pushN(W.l);
	if (EF) S.h = 0x01;
}


static void instruction(void) {
	switch (fetch()) {
	case 0x00: instructionInterrupt(reg16(EF ? 0xfffe : 0xffe6)); return;
	case 0x01: if (MF) instructionIndexedIndirectRead8(algorithmORA8); else instructionIndexedIndirectRead16(algorithmORA16); return;
	case 0x02: instructionInterrupt(reg16(EF ? 0xfff4 : 0xffe4)); return;
	case 0x03: if (MF) instructionStackRead8(algorithmORA8); else instructionStackRead16(algorithmORA16); return;
	case 0x04: if (MF) instructionDirectModify8(algorithmTSB8); else instructionDirectModify16(algorithmTSB16); return;
	case 0x05: if (MF) instructionDirectRead8(algorithmORA8); else instructionDirectRead16(algorithmORA16); return;
	case 0x06: if (MF) instructionDirectModify8(algorithmASL8); else instructionDirectModify16(algorithmASL16); return;
	case 0x07: if (MF) instructionIndirectLongRead8(algorithmORA8, Z); else instructionIndirectLongRead16(algorithmORA16, Z); return;
	case 0x08: instructionPush8(reg16(getP())); return;
	case 0x09: if (MF) instructionImmediateRead8(algorithmORA8); else instructionImmediateRead16(algorithmORA16); return;
	case 0x0a: if (MF) instructionImpliedModify8(algorithmASL8, &A); else instructionImpliedModify16(algorithmASL16, &A); return;
	case 0x0b: instructionPushD(); return;
	case 0x0c: if (MF) instructionBankModify8(algorithmTSB8); else instructionBankModify16(algorithmTSB16); return;
	case 0x0d: if (MF) instructionBankRead8(algorithmORA8); else instructionBankRead16(algorithmORA16); return;
	case 0x0e: if (MF) instructionBankModify8(algorithmASL8); else instructionBankModify16(algorithmASL16); return;
	case 0x0f: if (MF) instructionLongRead8(algorithmORA8, Z); else instructionLongRead16(algorithmORA16, Z); return;
	case 0x10: instructionBranch(NF == 0); return;
	case 0x11: if (MF) instructionIndirectIndexedRead8(algorithmORA8); else instructionIndirectIndexedRead16(algorithmORA16); return;
	case 0x12: if (MF) instructionIndirectRead8(algorithmORA8); else instructionIndirectRead16(algorithmORA16); return;
	case 0x13: if (MF) instructionIndirectStackRead8(algorithmORA8); else instructionIndirectStackRead16(algorithmORA16); return;
	case 0x14: if (MF) instructionDirectModify8(algorithmTRB8); else instructionDirectModify16(algorithmTRB16); return;
	case 0x15: if (MF) instructionDirectReadI8(algorithmORA8, X); else instructionDirectReadI16(algorithmORA16, X); return;
	case 0x16: if (MF) instructionDirectIndexedModify8(algorithmASL8); else instructionDirectIndexedModify16(algorithmASL16); return;
	case 0x17: if (MF) instructionIndirectLongRead8(algorithmORA8, Y); else instructionIndirectLongRead16(algorithmORA16, Y); return;
	case 0x18: instructionClearFlag(&CF); return;
	case 0x19: if (MF) instructionBankReadI8(algorithmORA8, Y); else instructionBankReadI16(algorithmORA16, Y); return;
	case 0x1a: if (MF) instructionImpliedModify8(algorithmINC8, &A); else instructionImpliedModify16(algorithmINC16, &A); return;
	case 0x1b: instructionTransferCS(); return;
	case 0x1c: if (MF) instructionBankModify8(algorithmTRB8); else instructionBankModify16(algorithmTRB16); return;
	case 0x1d: if (MF) instructionBankReadI8(algorithmORA8, X); else instructionBankReadI16(algorithmORA16, X); return;
	case 0x1e: if (MF) instructionBankIndexedModify8(algorithmASL8); else instructionBankIndexedModify16(algorithmASL16); return;
	case 0x1f: if (MF) instructionLongRead8(algorithmORA8, X); else instructionLongRead16(algorithmORA16, X); return;
	case 0x20: instructionCallShort(); return;
	case 0x21: if (MF) instructionIndexedIndirectRead8(algorithmAND8); else instructionIndexedIndirectRead16(algorithmAND16); return;
	case 0x22: instructionCallLong(); return;
	case 0x23: if (MF) instructionStackRead8(algorithmAND8); else instructionStackRead16(algorithmAND16); return;
	case 0x24: if (MF) instructionDirectRead8(algorithmBIT8); else instructionDirectRead16(algorithmBIT16); return;
	case 0x25: if (MF) instructionDirectRead8(algorithmAND8); else instructionDirectRead16(algorithmAND16); return;
	case 0x26: if (MF) instructionDirectModify8(algorithmROL8); else instructionDirectModify16(algorithmROL16); return;
	case 0x27: if (MF) instructionIndirectLongRead8(algorithmAND8, Z); else instructionIndirectLongRead16(algorithmAND16, Z); return;
	case 0x28: instructionPullP(); return;
	case 0x29: if (MF) instructionImmediateRead8(algorithmAND8); else instructionImmediateRead16(algorithmAND16); return;
	case 0x2a: if (MF) instructionImpliedModify8(algorithmROL8, &A); else instructionImpliedModify16(algorithmROL16, &A); return;
	case 0x2b: instructionPullD(); return;
	case 0x2c: if (MF) instructionBankRead8(algorithmBIT8); else instructionBankRead16(algorithmBIT16); return;
	case 0x2d: if (MF) instructionBankRead8(algorithmAND8); else instructionBankRead16(algorithmAND16); return;
	case 0x2e: if (MF) instructionBankModify8(algorithmROL8); else instructionBankModify16(algorithmROL16); return;
	case 0x2f: if (MF) instructionLongRead8(algorithmAND8, Z); else instructionLongRead16(algorithmAND16, Z); return;
	case 0x30: instructionBranch(NF == 1); return;
	case 0x31: if (MF) instructionIndirectIndexedRead8(algorithmAND8); else instructionIndirectIndexedRead16(algorithmAND16); return;
	case 0x32: if (MF) instructionIndirectRead8(algorithmAND8); else instructionIndirectRead16(algorithmAND16); return;
	case 0x33: if (MF) instructionIndirectStackRead8(algorithmAND8); else instructionIndirectStackRead16(algorithmAND16); return;
	case 0x34: if (MF) instructionDirectReadI8(algorithmBIT8, X); else instructionDirectReadI16(algorithmBIT16, X); return;
	case 0x35: if (MF) instructionDirectReadI8(algorithmAND8, X); else instructionDirectReadI16(algorithmAND16, X); return;
	case 0x36: if (MF) instructionDirectIndexedModify8(algorithmROL8); else instructionDirectIndexedModify16(algorithmROL16); return;
	case 0x37: if (MF) instructionIndirectLongRead8(algorithmAND8, Y); else instructionIndirectLongRead16(algorithmAND16, Y); return;
	case 0x38: instructionSetFlag(&CF); return;
	case 0x39: if (MF) instructionBankReadI8(algorithmAND8, Y); else instructionBankReadI16(algorithmAND16, Y); return;
	case 0x3a: if (MF) instructionImpliedModify8(algorithmDEC8, &A); else instructionImpliedModify16(algorithmDEC16, &A); return;
	case 0x3b: instructionTransfer16(S, &A); return;
	case 0x3c: if (MF) instructionBankReadI8(algorithmBIT8, X); else instructionBankReadI16(algorithmBIT16, X); return;
	case 0x3d: if (MF) instructionBankReadI8(algorithmAND8, X); else instructionBankReadI16(algorithmAND16, X); return;
	case 0x3e: if (MF) instructionBankIndexedModify8(algorithmROL8); else instructionBankIndexedModify16(algorithmROL16); return;
	case 0x3f: if (MF) instructionLongRead8(algorithmAND8, X); else instructionLongRead16(algorithmAND16, X); return;
	case 0x40: instructionReturnInterrupt(); return;
	case 0x41: if (MF) instructionIndexedIndirectRead8(algorithmEOR8); else instructionIndexedIndirectRead16(algorithmEOR16); return;
	case 0x42: instructionPrefix(); return;
	case 0x43: if (MF) instructionStackRead8(algorithmEOR8); else instructionStackRead16(algorithmEOR16); return;
	case 0x44: if (XF) instructionBlockMove8(-1); else instructionBlockMove16(-1); return;
	case 0x45: if (MF) instructionDirectRead8(algorithmEOR8); else instructionDirectRead16(algorithmEOR16); return;
	case 0x46: if (MF) instructionDirectModify8(algorithmLSR8); else instructionDirectModify16(algorithmLSR16); return;
	case 0x47: if (MF) instructionIndirectLongRead8(algorithmEOR8, Z); else instructionIndirectLongRead16(algorithmEOR16, Z); return;
	case 0x48: if (MF) instructionPush8(A); else instructionPush16(A); return;
	case 0x49: if (MF) instructionImmediateRead8(algorithmEOR8); else instructionImmediateRead16(algorithmEOR16); return;
	case 0x4a: if (MF) instructionImpliedModify8(algorithmLSR8, &A); else instructionImpliedModify16(algorithmLSR16, &A); return;
	case 0x4b: instructionPush8(reg16(PC.b)); return;
	case 0x4c: instructionJumpShort(); return;
	case 0x4d: if (MF) instructionBankRead8(algorithmEOR8); else instructionBankRead16(algorithmEOR16); return;
	case 0x4e: if (MF) instructionBankModify8(algorithmLSR8); else instructionBankModify16(algorithmLSR16); return;
	case 0x4f: if (MF) instructionLongRead8(algorithmEOR8, Z); else instructionLongRead16(algorithmEOR16, Z); return;
	case 0x50: instructionBranch(VF == 0); return;
	case 0x51: if (MF) instructionIndirectIndexedRead8(algorithmEOR8); else instructionIndirectIndexedRead16(algorithmEOR16); return;
	case 0x52: if (MF) instructionIndirectRead8(algorithmEOR8); else instructionIndirectRead16(algorithmEOR16); return;
	case 0x53: if (MF) instructionIndirectStackRead8(algorithmEOR8); else instructionIndirectStackRead16(algorithmEOR16); return;
	case 0x54: if (XF) instructionBlockMove8(+1); else instructionBlockMove16(+1); return;
	case 0x55: if (MF) instructionDirectReadI8(algorithmEOR8, X); else instructionDirectReadI16(algorithmEOR16, X); return;
	case 0x56: if (MF) instructionDirectIndexedModify8(algorithmLSR8); else instructionDirectIndexedModify16(algorithmLSR16); return;
	case 0x57: if (MF) instructionIndirectLongRead8(algorithmEOR8, Y); else instructionIndirectLongRead16(algorithmEOR16, Y); return;
	case 0x58: instructionClearFlag(&IF); return;
	case 0x59: if (MF) instructionBankReadI8(algorithmEOR8, Y); else instructionBankReadI16(algorithmEOR16, Y); return;
	case 0x5a: if (XF) instructionPush8(Y); else instructionPush16(Y); return;
	case 0x5b: instructionTransfer16(A, &D); return;
	case 0x5c: instructionJumpLong(); return;
	case 0x5d: if (MF) instructionBankReadI8(algorithmEOR8, X); else instructionBankReadI16(algorithmEOR16, X); return;
	case 0x5e: if (MF) instructionBankIndexedModify8(algorithmLSR8); else instructionBankIndexedModify16(algorithmLSR16); return;
	case 0x5f: if (MF) instructionLongRead8(algorithmEOR8, X); else instructionLongRead16(algorithmEOR16, X); return;
	case 0x60: instructionReturnShort(); return;
	case 0x61: if (MF) instructionIndexedIndirectRead8(algorithmADC8); else instructionIndexedIndirectRead16(algorithmADC16); return;
	case 0x62: instructionPushEffectiveRelativeAddress(); return;
	case 0x63: if (MF) instructionStackRead8(algorithmADC8); else instructionStackRead16(algorithmADC16); return;
	case 0x64: if (MF) instructionDirectWrite8(Z); else instructionDirectWrite16(Z); return;
	case 0x65: if (MF) instructionDirectRead8(algorithmADC8); else instructionDirectRead16(algorithmADC16); return;
	case 0x66: if (MF) instructionDirectModify8(algorithmROR8); else instructionDirectModify16(algorithmROR16); return;
	case 0x67: if (MF) instructionIndirectLongRead8(algorithmADC8, Z); else instructionIndirectLongRead16(algorithmADC16, Z); return;
	case 0x68: if (MF) instructionPull8(&A); else instructionPull16(&A); return;
	case 0x69: if (MF) instructionImmediateRead8(algorithmADC8); else instructionImmediateRead16(algorithmADC16); return;
	case 0x6a: if (MF) instructionImpliedModify8(algorithmROR8, &A); else instructionImpliedModify16(algorithmROR16, &A); return;
	case 0x6b: instructionReturnLong(); return;
	case 0x6c: instructionJumpIndirect(); return;
	case 0x6d: if (MF) instructionBankRead8(algorithmADC8); else instructionBankRead16(algorithmADC16); return;
	case 0x6e: if (MF) instructionBankModify8(algorithmROR8); else instructionBankModify16(algorithmROR16); return;
	case 0x6f: if (MF) instructionLongRead8(algorithmADC8, Z); else instructionLongRead16(algorithmADC16, Z); return;
	case 0x70: instructionBranch(VF == 1); return;
	case 0x71: if (MF) instructionIndirectIndexedRead8(algorithmADC8); else instructionIndirectIndexedRead16(algorithmADC16); return;
	case 0x72: if (MF) instructionIndirectRead8(algorithmADC8); else instructionIndirectRead16(algorithmADC16); return;
	case 0x73: if (MF) instructionIndirectStackRead8(algorithmADC8); else instructionIndirectStackRead16(algorithmADC16); return;
	case 0x74: if (MF) instructionDirectWriteI8(Z, X); else instructionDirectWriteI16(Z, X); return;
	case 0x75: if (MF) instructionDirectReadI8(algorithmADC8, X); else instructionDirectReadI16(algorithmADC16, X); return;
	case 0x76: if (MF) instructionDirectIndexedModify8(algorithmROR8); else instructionDirectIndexedModify16(algorithmROR16); return;
	case 0x77: if (MF) instructionIndirectLongRead8(algorithmADC8, Y); else instructionIndirectLongRead16(algorithmADC16, Y); return;
	case 0x78: instructionSetFlag(&IF); return;
	case 0x79: if (MF) instructionBankReadI8(algorithmADC8, Y); else instructionBankReadI16(algorithmADC16, Y); return;
	case 0x7a: if (XF) instructionPull8(&Y); else instructionPull16(&Y); return;
	case 0x7b: instructionTransfer16(D, &A); return;
	case 0x7c: instructionJumpIndexedIndirect(); return;
	case 0x7d: if (MF) instructionBankReadI8(algorithmADC8, X); else instructionBankReadI16(algorithmADC16, X); return;
	case 0x7e: if (MF) instructionBankIndexedModify8(algorithmROR8); else instructionBankIndexedModify16(algorithmROR16); return;
	case 0x7f: if (MF) instructionLongRead8(algorithmADC8, X); else instructionLongRead16(algorithmADC16, X); return;
	case 0x80: instructionBranch(1); return;
	case 0x81: if (MF) instructionIndexedIndirectWrite8(); else instructionIndexedIndirectWrite16(); return;
	case 0x82: instructionBranchLong(); return;
	case 0x83: if (MF) instructionStackWrite8(); else instructionStackWrite16(); return;
	case 0x84: if (XF) instructionDirectWrite8(Y); else instructionDirectWrite16(Y); return;
	case 0x85: if (MF) instructionDirectWrite8(A); else instructionDirectWrite16(A); return;
	case 0x86: if (XF) instructionDirectWrite8(X); else instructionDirectWrite16(X); return;
	case 0x87: if (MF) instructionIndirectLongWrite8(Z); else instructionIndirectLongWrite16(Z); return;
	case 0x88: if (XF) instructionImpliedModify8(algorithmDEC8, &Y); else instructionImpliedModify16(algorithmDEC16, &Y); return;
	case 0x89: if (MF) instructionBitImmediate8(); else instructionBitImmediate16(); return;
	case 0x8a: if (MF) instructionTransfer8(X, &A); else instructionTransfer16(X, &A); return;
	case 0x8b: instructionPush8(reg16(B)); return;
	case 0x8c: if (XF) instructionBankWrite8(Y); else instructionBankWrite16(Y); return;
	case 0x8d: if (MF) instructionBankWrite8(A); else instructionBankWrite16(A); return;
	case 0x8e: if (XF) instructionBankWrite8(X); else instructionBankWrite16(X); return;
	case 0x8f: if (MF) instructionLongWrite8(Z); else instructionLongWrite16(Z); return;
	case 0x90: instructionBranch(CF == 0); return;
	case 0x91: if (MF) instructionIndirectIndexedWrite8(); else instructionIndirectIndexedWrite16(); return;
	case 0x92: if (MF) instructionIndirectWrite8(); else instructionIndirectWrite16(); return;
	case 0x93: if (MF) instructionIndirectStackWrite8(); else instructionIndirectStackWrite16(); return;
	case 0x94: if (XF) instructionDirectWriteI8(Y, X); else instructionDirectWriteI16(Y, X); return;
	case 0x95: if (MF) instructionDirectWriteI8(A, X); else instructionDirectWriteI16(A, X); return;
	case 0x96: if (XF) instructionDirectWriteI8(X, Y); else instructionDirectWriteI16(X, Y); return;
	case 0x97: if (MF) instructionIndirectLongWrite8(Y); else instructionIndirectLongWrite16(Y); return;
	case 0x98: if (MF) instructionTransfer8(Y, &A); else instructionTransfer16(Y, &A); return;
	case 0x99: if (MF) instructionBankWriteI8(A, Y); else instructionBankWriteI16(A, Y); return;
	case 0x9a: instructionTransferXS(); return;
	case 0x9b: if (XF) instructionTransfer8(X, &Y); else instructionTransfer16(X, &Y); return;
	case 0x9c: if (MF) instructionBankWrite8(Z); else instructionBankWrite16(Z); return;
	case 0x9d: if (MF) instructionBankWriteI8(A, X); else instructionBankWriteI16(A, X); return;
	case 0x9e: if (MF) instructionBankWriteI8(Z, X); else instructionBankWriteI16(Z, X); return;
	case 0x9f: if (MF) instructionLongWrite8(X); else instructionLongWrite16(X); return;
	case 0xa0: if (XF) instructionImmediateRead8(algorithmLDY8); else instructionImmediateRead16(algorithmLDY16); return;
	case 0xa1: if (MF) instructionIndexedIndirectRead8(algorithmLDA8); else instructionIndexedIndirectRead16(algorithmLDA16); return;
	case 0xa2: if (XF) instructionImmediateRead8(algorithmLDX8); else instructionImmediateRead16(algorithmLDX16); return;
	case 0xa3: if (MF) instructionStackRead8(algorithmLDA8); else instructionStackRead16(algorithmLDA16); return;
	case 0xa4: if (XF) instructionDirectRead8(algorithmLDY8); else instructionDirectRead16(algorithmLDY16); return;
	case 0xa5: if (MF) instructionDirectRead8(algorithmLDA8); else instructionDirectRead16(algorithmLDA16); return;
	case 0xa6: if (XF) instructionDirectRead8(algorithmLDX8); else instructionDirectRead16(algorithmLDX16); return;
	case 0xa7: if (MF) instructionIndirectLongRead8(algorithmLDA8, Z); else instructionIndirectLongRead16(algorithmLDA16, Z); return;
	case 0xa8: if (XF) instructionTransfer8(A, &Y); else instructionTransfer16(A, &Y); return;
	case 0xa9: if (MF) instructionImmediateRead8(algorithmLDA8); else instructionImmediateRead16(algorithmLDA16); return;
	case 0xaa: if (XF) instructionTransfer8(A, &X); else instructionTransfer16(A, &X); return;
	case 0xab: instructionPullB(); return;
	case 0xac: if (XF) instructionBankRead8(algorithmLDY8); else instructionBankRead16(algorithmLDY16); return;
	case 0xad: if (MF) instructionBankRead8(algorithmLDA8); else instructionBankRead16(algorithmLDA16); return;
	case 0xae: if (XF) instructionBankRead8(algorithmLDX8); else instructionBankRead16(algorithmLDX16); return;
	case 0xaf: if (MF) instructionLongRead8(algorithmLDA8, Z); else instructionLongRead16(algorithmLDA16, Z); return;
	case 0xb0: instructionBranch(CF == 1); return;
	case 0xb1: if (MF) instructionIndirectIndexedRead8(algorithmLDA8); else instructionIndirectIndexedRead16(algorithmLDA16); return;
	case 0xb2: if (MF) instructionIndirectRead8(algorithmLDA8); else instructionIndirectRead16(algorithmLDA16); return;
	case 0xb3: if (MF) instructionIndirectStackRead8(algorithmLDA8); else instructionIndirectStackRead16(algorithmLDA16); return;
	case 0xb4: if (XF) instructionDirectReadI8(algorithmLDY8, X); else instructionDirectReadI16(algorithmLDY16, X); return;
	case 0xb5: if (MF) instructionDirectReadI8(algorithmLDA8, X); else instructionDirectReadI16(algorithmLDA16, X); return;
	case 0xb6: if (XF) instructionDirectReadI8(algorithmLDX8, Y); else instructionDirectReadI16(algorithmLDX16, Y); return;
	case 0xb7: if (MF) instructionIndirectLongRead8(algorithmLDA8, Y); else instructionIndirectLongRead16(algorithmLDA16, Y); return;
	case 0xb8: instructionClearFlag(&VF); return;
	case 0xb9: if (MF) instructionBankReadI8(algorithmLDA8, Y); else instructionBankReadI16(algorithmLDA16, Y); return;
	case 0xba: if (XF) instructionTransferSX8(); else instructionTransferSX16(); return;
	case 0xbb: if (XF) instructionTransfer8(Y, &X); else instructionTransfer16(Y, &X); return;
	case 0xbc: if (XF) instructionBankReadI8(algorithmLDY8, X); else instructionBankReadI16(algorithmLDY16, X); return;
	case 0xbd: if (MF) instructionBankReadI8(algorithmLDA8, X); else instructionBankReadI16(algorithmLDA16, X); return;
	case 0xbe: if (XF) instructionBankReadI8(algorithmLDX8, Y); else instructionBankReadI16(algorithmLDX16, Y); return;
	case 0xbf: if (MF) instructionLongRead8(algorithmLDA8, X); else instructionLongRead16(algorithmLDA16, X); return;
	case 0xc0: if (XF) instructionImmediateRead8(algorithmCPY8); else instructionImmediateRead16(algorithmCPY16); return;
	case 0xc1: if (MF) instructionIndexedIndirectRead8(algorithmCMP8); else instructionIndexedIndirectRead16(algorithmCMP16); return;
	case 0xc2: instructionResetP(); return;
	case 0xc3: if (MF) instructionStackRead8(algorithmCMP8); else instructionStackRead16(algorithmCMP16); return;
	case 0xc4: if (XF) instructionDirectRead8(algorithmCPY8); else instructionDirectRead16(algorithmCPY16); return;
	case 0xc5: if (MF) instructionDirectRead8(algorithmCMP8); else instructionDirectRead16(algorithmCMP16); return;
	case 0xc6: if (MF) instructionDirectModify8(algorithmDEC8); else instructionDirectModify16(algorithmDEC16); return;
	case 0xc7: if (MF) instructionIndirectLongRead8(algorithmCMP8, Z); else instructionIndirectLongRead16(algorithmCMP16, Z); return;
	case 0xc8: if (XF) instructionImpliedModify8(algorithmINC8, &Y); else instructionImpliedModify16(algorithmINC16, &Y); return;
	case 0xc9: if (MF) instructionImmediateRead8(algorithmCMP8); else instructionImmediateRead16(algorithmCMP16); return;
	case 0xca: if (XF) instructionImpliedModify8(algorithmDEC8, &X); else instructionImpliedModify16(algorithmDEC16, &X); return;
	case 0xcb: instructionWait(); return;
	case 0xcc: if (XF) instructionBankRead8(algorithmCPY8); else instructionBankRead16(algorithmCPY16); return;
	case 0xcd: if (MF) instructionBankRead8(algorithmCMP8); else instructionBankRead16(algorithmCMP16); return;
	case 0xce: if (MF) instructionBankModify8(algorithmDEC8); else instructionBankModify16(algorithmDEC16); return;
	case 0xcf: if (MF) instructionLongRead8(algorithmCMP8, Z); else instructionLongRead16(algorithmCMP16, Z); return;
	case 0xd0: instructionBranch(ZF == 0); return;
	case 0xd1: if (MF) instructionIndirectIndexedRead8(algorithmCMP8); else instructionIndirectIndexedRead16(algorithmCMP16); return;
	case 0xd2: if (MF) instructionIndirectRead8(algorithmCMP8); else instructionIndirectRead16(algorithmCMP16); return;
	case 0xd3: if (MF) instructionIndirectStackRead8(algorithmCMP8); else instructionIndirectStackRead16(algorithmCMP16); return;
	case 0xd4: instructionPushEffectiveIndirectAddress(); return;
	case 0xd5: if (MF) instructionDirectReadI8(algorithmCMP8, X); else instructionDirectReadI16(algorithmCMP16, X); return;
	case 0xd6: if (MF) instructionDirectIndexedModify8(algorithmDEC8); else instructionDirectIndexedModify16(algorithmDEC16); return;
	case 0xd7: if (MF) instructionIndirectLongRead8(algorithmCMP8, Y); else instructionIndirectLongRead16(algorithmCMP16, Y); return;
	case 0xd8: instructionClearFlag(&DF); return;
	case 0xd9: if (MF) instructionBankReadI8(algorithmCMP8, Y); else instructionBankReadI16(algorithmCMP16, Y); return;
	case 0xda: if (XF) instructionPush8(X); else instructionPush16(X); return;
	case 0xdb: instructionStop(); return;
	case 0xdc: instructionJumpIndirectLong(); return;
	case 0xdd: if (MF) instructionBankReadI8(algorithmCMP8, X); else instructionBankReadI16(algorithmCMP16, X); return;
	case 0xde: if (MF) instructionBankIndexedModify8(algorithmDEC8); else instructionBankIndexedModify16(algorithmDEC16); return;
	case 0xdf: if (MF) instructionLongRead8(algorithmCMP8, X); else instructionLongRead16(algorithmCMP16, X); return;
	case 0xe0: if (XF) instructionImmediateRead8(algorithmCPX8); else instructionImmediateRead16(algorithmCPX16); return;
	case 0xe1: if (MF) instructionIndexedIndirectRead8(algorithmSBC8); else instructionIndexedIndirectRead16(algorithmSBC16); return;
	case 0xe2: instructionSetP(); return;
	case 0xe3: if (MF) instructionStackRead8(algorithmSBC8); else instructionStackRead16(algorithmSBC16); return;
	case 0xe4: if (XF) instructionDirectRead8(algorithmCPX8); else instructionDirectRead16(algorithmCPX16); return;
	case 0xe5: if (MF) instructionDirectRead8(algorithmSBC8); else instructionDirectRead16(algorithmSBC16); return;
	case 0xe6: if (MF) instructionDirectModify8(algorithmINC8); else instructionDirectModify16(algorithmINC16); return;
	case 0xe7: if (MF) instructionIndirectLongRead8(algorithmSBC8, Z); else instructionIndirectLongRead16(algorithmSBC16, Z); return;
	case 0xe8: if (XF) instructionImpliedModify8(algorithmINC8, &X); else instructionImpliedModify16(algorithmINC16, &X); return;
	case 0xe9: if (MF) instructionImmediateRead8(algorithmSBC8); else instructionImmediateRead16(algorithmSBC16); return;
	case 0xea: instructionNoOperation(); return;
	case 0xeb: instructionExchangeBA(); return;
	case 0xec: if (XF) instructionBankRead8(algorithmCPX8); else instructionBankRead16(algorithmCPX16); return;
	case 0xed: if (MF) instructionBankRead8(algorithmSBC8); else instructionBankRead16(algorithmSBC16); return;
	case 0xee: if (MF) instructionBankModify8(algorithmINC8); else instructionBankModify16(algorithmINC16); return;
	case 0xef: if (MF) instructionLongRead8(algorithmSBC8, Z); else instructionLongRead16(algorithmSBC16, Z); return;
	case 0xf0: instructionBranch(ZF == 1); return;
	case 0xf1: if (MF) instructionIndirectIndexedRead8(algorithmSBC8); else instructionIndirectIndexedRead16(algorithmSBC16); return;
	case 0xf2: if (MF) instructionIndirectRead8(algorithmSBC8); else instructionIndirectRead16(algorithmSBC16); return;
	case 0xf3: if (MF) instructionIndirectStackRead8(algorithmSBC8); else instructionIndirectStackRead16(algorithmSBC16); return;
	case 0xf4: instructionPushEffectiveAddress(); return;
	case 0xf5: if (MF) instructionDirectReadI8(algorithmSBC8, X); else instructionDirectReadI16(algorithmSBC16, X); return;
	case 0xf6: if (MF) instructionDirectIndexedModify8(algorithmINC8); else instructionDirectIndexedModify16(algorithmINC16); return;
	case 0xf7: if (MF) instructionIndirectLongRead8(algorithmSBC8, Y); else instructionIndirectLongRead16(algorithmSBC16, Y); return;
	case 0xf8: instructionSetFlag(&DF); return;
	case 0xf9: if (MF) instructionBankReadI8(algorithmSBC8, Y); else instructionBankReadI16(algorithmSBC16, Y); return;
	case 0xfa: if (XF) instructionPull8(&X); else instructionPull16(&X); return;
	case 0xfb: instructionExchangeCE(); return;
	case 0xfc: instructionCallIndexedIndirect(); return;
	case 0xfd: if (MF) instructionBankReadI8(algorithmSBC8, X); else instructionBankReadI16(algorithmSBC16, X); return;
	case 0xfe: if (MF) instructionBankIndexedModify8(algorithmINC8); else instructionBankIndexedModify16(algorithmINC16); return;
	case 0xff: if (MF) instructionLongRead8(algorithmSBC8, X); else instructionLongRead16(algorithmSBC16, X); return;
	}
}
