/* Copyright (c) 2004-2025 ares team, Near et al
 * Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Ported from ares (ISC); see LICENSES/ISC-ares.txt. The decompressor is neviksti's, as talarubi
 * optimized it. */
#include "../snes.h"

// The SPC7110: a decompression unit, a data port and an ALU reading the data ROM, and a memory
// controller banking it in. The program ROM is the first megabyte of the dump and the data ROM the rest.

struct Spc7110 spc7110;

static u32 promSize;
static const u8* drom;
static u32 dromSize;

// --- Clock steps ---

// A step of some clocks; whether it left the chip ahead of the S-CPU.
static bool step(u32 clocks) {
	spc7110.th.clock += spc7110.th.scalar * clocks;
	return cpu.th.clock < spc7110.th.clock;
}

// --- The data ROM ---

static u8 dataromRead(u32 address) {
	address &= 0xffffff;
	u32 mask = 0x100000 * (1 << (spc7110.r4834 & 3)) - 1;
	u32 offset = address & mask;
	if ((spc7110.r4834 & 3) != 3 && (address & 0x400000)) {
		return 0x00;
	}
	if (!dromSize) {
		return 0x00;
	}
	return drom[busMirror(offset, dromSize)];
}

// --- The decompressor ---

enum { MPS = 0, LPS = 1 };
enum { HALF = 0x55, MAX = 0xff };

static const struct {
	u8 probability;  // of the more probable symbol
	u8 next[2];      // the state after an MPS and after an LPS
} evolution[53] = {
	{0x5a, { 1, 1}}, {0x25, { 2, 6}}, {0x11, { 3, 8}},
	{0x08, { 4,10}}, {0x03, { 5,12}}, {0x01, { 5,15}},

	{0x5a, { 7, 7}}, {0x3f, { 8,19}}, {0x2c, { 9,21}},
	{0x20, {10,22}}, {0x17, {11,23}}, {0x11, {12,25}},
	{0x0c, {13,26}}, {0x09, {14,28}}, {0x07, {15,29}},
	{0x05, {16,31}}, {0x04, {17,32}}, {0x03, {18,34}},
	{0x02, { 5,35}},

	{0x5a, {20,20}}, {0x48, {21,39}}, {0x3a, {22,40}},
	{0x2e, {23,42}}, {0x26, {24,44}}, {0x1f, {25,45}},
	{0x19, {26,46}}, {0x15, {27,25}}, {0x11, {28,26}},
	{0x0e, {29,26}}, {0x0b, {30,27}}, {0x09, {31,28}},
	{0x08, {32,29}}, {0x07, {33,30}}, {0x05, {34,31}},
	{0x04, {35,33}}, {0x04, {36,33}}, {0x03, {37,34}},
	{0x02, {38,35}}, {0x02, { 5,36}},

	{0x58, {40,39}}, {0x4d, {41,47}}, {0x43, {42,48}},
	{0x3b, {43,49}}, {0x34, {44,50}}, {0x2e, {45,51}},
	{0x29, {46,44}}, {0x25, {24,45}},

	{0x56, {48,47}}, {0x4f, {49,47}}, {0x47, {50,48}},
	{0x41, {51,49}}, {0x3c, {52,50}}, {0x37, {43,51}},
};

#define D spc7110.dcu

static u8 decompressorRead(void) {
	return dataromRead(D.offset++);
}

// The inverse Morton transform: big-endian packed pixels unpacked, odd bits in the low half and even
// bits in the high half.
static u32 deinterleave(u64 data, u32 bits) {
	data = data & ((1ull << bits) - 1);
	data = 0x5555555555555555ull & (data << bits | data >> 1);
	data = 0x3333333333333333ull & (data | data >> 1);
	data = 0x0f0f0f0f0f0f0f0full & (data | data >> 2);
	data = 0x00ff00ff00ff00ffull & (data | data >> 4);
	data = 0x0000ffff0000ffffull & (data | data >> 8);
	return (u32) (data | data >> 16);
}

// A nibble taken out of the list and put at its front.
static u64 moveToFront(u64 list, u32 nibble) {
	u64 mask = ~(u64) 15;
	for (u32 n = 0; n < 64; n += 4, mask <<= 4) {
		if ((list >> n & 15) != nibble) {
			continue;
		}
		return (list & mask) + (list << 4 & ~mask) + nibble;
	}
	return list;
}

static void decompressorInitialize(u32 mode, u32 origin) {
	memset(D.context, 0, sizeof(D.context));
	D.bpp = 1 << mode;
	D.offset = origin;
	D.bits = 8;
	D.range = MAX + 1;
	D.input = decompressorRead();
	D.input = D.input << 8 | decompressorRead();
	D.output = 0;
	D.pixels = 0;
	D.colormap = 0xfedcba9876543210ull;
}

static void decode(void) {
	for (u32 pixel = 0; pixel < 8; pixel++) {
		u64 map = D.colormap;
		u32 diff = 0;

		if (D.bpp > 1) {
			u32 pa = D.bpp == 2 ? D.pixels >> 2 & 3 : D.pixels >> 0 & 15;
			u32 pb = D.bpp == 2 ? D.pixels >> 14 & 3 : D.pixels >> 28 & 15;
			u32 pc = D.bpp == 2 ? D.pixels >> 16 & 3 : D.pixels >> 32 & 15;

			if (pa != pb || pb != pc) {
				u32 match = pa ^ pb ^ pc;
				diff = 4;                         // all three differ
				if ((match ^ pc) == 0) diff = 3;  // a == b, c differs
				if ((match ^ pb) == 0) diff = 2;  // c == a, b differs
				if ((match ^ pa) == 0) diff = 1;  // b == c, a differs
			}

			D.colormap = moveToFront(D.colormap, pa);

			map = moveToFront(map, pc);
			map = moveToFront(map, pb);
			map = moveToFront(map, pa);
		}

		for (u32 plane = 0; plane < D.bpp; plane++) {
			u32 bit = D.bpp > 1 ? 1 << plane : 1 << (pixel & 3);
			u32 history = (bit - 1) & D.output;
			u32 set = 0;

			if (D.bpp == 1) set = pixel >= 4;
			if (D.bpp == 2) set = diff;
			if (plane >= 2 && history <= 1) set = diff;

			u8* prediction = &D.context[set][bit + history - 1].prediction;
			u8* swap = &D.context[set][bit + history - 1].swap;
			u32 model = *prediction;
			u8 lpsOffset = D.range - evolution[model].probability;
			bool symbol = D.input >= (lpsOffset << 8);

			D.output = D.output << 1 | (symbol ^ *swap);

			if (symbol == MPS) {
				D.range = lpsOffset;
			} else {
				D.range -= lpsOffset;
				D.input -= lpsOffset << 8;
			}

			while (D.range <= MAX / 2) {
				*prediction = evolution[model].next[symbol];

				D.range <<= 1;
				D.input <<= 1;

				if (--D.bits == 0) {
					D.bits = 8;
					D.input += decompressorRead();
				}
			}

			if (symbol == LPS && evolution[model].probability > HALF) {
				*swap ^= 1;
			}
		}

		u32 index = D.output & ((1 << D.bpp) - 1);
		if (D.bpp == 1) {
			index ^= D.pixels >> 15 & 1;
		}

		D.pixels = D.pixels << D.bpp | (map >> 4 * index & 15);
	}

	if (D.bpp == 1) D.result = (u32) D.pixels;
	if (D.bpp == 2) D.result = deinterleave(D.pixels, 16);
	if (D.bpp == 4) D.result = deinterleave(deinterleave(D.pixels, 32), 32);
}

#undef D

// --- The decompression unit ---

static void dcuLoadAddress(void) {
	u32 table = (spc7110.r4801 | spc7110.r4802 << 8 | spc7110.r4803 << 16) & 0xffffff;
	u32 address = (table + (spc7110.r4804 << 2)) & 0xffffff;
	spc7110.dcuMode = dataromRead(address + 0) & 3;
	spc7110.dcuAddress = (dataromRead(address + 1) << 16 | dataromRead(address + 2) << 8 | dataromRead(address + 3)) & 0x7fffff;
}

static void dcuBeginTransfer(void) {
	decompressorInitialize(spc7110.dcuMode, spc7110.dcuAddress);
	decode();
	u16 seek = spc7110.r480b & 2 ? spc7110.r4805 | spc7110.r4806 << 8 : 0;
	while (seek--) {
		decode();
	}
	spc7110.r480c |= 0x80;
	spc7110.dcuOffset = 0;
}

static u8 dcuRead(void) {
	if ((spc7110.r480c & 0x80) == 0) {
		return 0x00;
	}
	if (spc7110.dcuOffset == 0) {
		for (u32 row = 0; row < 8; ++row) {
			u32 result = spc7110.dcu.result;
			switch (spc7110.dcu.bpp) {
			case 1:
				spc7110.dcuTile[row] = result;
				break;
			case 2:
				spc7110.dcuTile[row * 2 + 0] = result >> 0;
				spc7110.dcuTile[row * 2 + 1] = result >> 8;
				break;
			case 4:
				spc7110.dcuTile[row * 2 + 0] = result >> 0;
				spc7110.dcuTile[row * 2 + 1] = result >> 8;
				spc7110.dcuTile[row * 2 + 16] = result >> 16;
				spc7110.dcuTile[row * 2 + 17] = result >> 24;
				break;
			}
			u8 seek = spc7110.r480b & 1 ? spc7110.r4807 : 1;
			while (seek--) {
				decode();
			}
		}
	}
	u8 data = spc7110.dcuTile[spc7110.dcuOffset++];
	spc7110.dcuOffset &= 8 * spc7110.dcu.bpp - 1;
	return data;
}

// --- The data port ---

static u32 dataOffset(void) {
	return spc7110.r4811 | spc7110.r4812 << 8 | spc7110.r4813 << 16;
}

static u16 dataAdjust(void) {
	return spc7110.r4814 | spc7110.r4815 << 8;
}

static u16 dataStride(void) {
	return spc7110.r4816 | spc7110.r4817 << 8;
}

static void setDataOffset(u32 address) {
	spc7110.r4811 = address;
	spc7110.r4812 = address >> 8;
	spc7110.r4813 = address >> 16 & 0x7f;
}

static void setDataAdjust(u16 address) {
	spc7110.r4814 = address;
	spc7110.r4815 = address >> 8;
}

static void dataPortRead(void) {
	u32 adjust = spc7110.r4818 & 2 ? dataAdjust() : 0;
	if (spc7110.r4818 & 8) {
		adjust = (u32) (s16) adjust;
	}
	spc7110.r4810 = dataromRead(dataOffset() + adjust);
}

static void dataPortIncrement4810(void) {
	u32 offset = dataOffset();
	u32 stride = spc7110.r4818 & 1 ? dataStride() : 1;
	u32 adjust = dataAdjust();
	if (spc7110.r4818 & 4) {
		stride = (u32) (s16) stride;
	}
	if (spc7110.r4818 & 8) {
		adjust = (u32) (s16) adjust;
	}
	if ((spc7110.r4818 & 16) == 0) {
		setDataOffset(offset + stride);
	} else {
		setDataAdjust(adjust + stride);
	}
	dataPortRead();
}

// A write to $4814, $4815 or a read of $481a adds the adjust to the offset, when the settings choose
// that register for it (1, 2 or 3).
static void dataPortIncrement(u32 which) {
	if (spc7110.r4818 >> 5 != which) {
		return;
	}
	u32 adjust = dataAdjust();
	if (spc7110.r4818 & 8) {
		adjust = (u32) (s16) adjust;
	}
	setDataOffset(dataOffset() + adjust);
	dataPortRead();
}

// --- The ALU ---

static void aluMultiply(void) {
	u32 result;
	if (spc7110.r482e & 1) {
		s16 r0 = (s16) (spc7110.r4824 | spc7110.r4825 << 8);
		s16 r1 = (s16) (spc7110.r4820 | spc7110.r4821 << 8);
		result = (u32) ((s32) r0 * r1);
	} else {
		u32 r0 = spc7110.r4824 | spc7110.r4825 << 8;
		u32 r1 = spc7110.r4820 | spc7110.r4821 << 8;
		result = r0 * r1;
	}
	spc7110.r4828 = result;
	spc7110.r4829 = result >> 8;
	spc7110.r482a = result >> 16;
	spc7110.r482b = result >> 24;
	spc7110.r482f &= 0x7f;
}

// A division by zero gives a quotient of 0 and the dividend as the remainder.
static void aluDivide(void) {
	u32 dividend = spc7110.r4820 | spc7110.r4821 << 8 | spc7110.r4822 << 16 | (u32) spc7110.r4823 << 24;
	u16 divisorBits = spc7110.r4826 | spc7110.r4827 << 8;
	u32 quotient;
	u16 remainder;
	if (spc7110.r482e & 1) {
		s32 a = (s32) dividend;
		s16 b = (s16) divisorBits;
		if (b == 0) {
			quotient = 0;
			remainder = (u16) a;
		} else if (a == INT32_MIN && b == -1) {
			quotient = (u32) a;
			remainder = 0;
		} else {
			quotient = (u32) (a / b);
			remainder = (u16) (a % b);
		}
	} else {
		if (divisorBits == 0) {
			quotient = 0;
			remainder = (u16) dividend;
		} else {
			quotient = dividend / divisorBits;
			remainder = (u16) (dividend % divisorBits);
		}
	}
	spc7110.r4828 = quotient;
	spc7110.r4829 = quotient >> 8;
	spc7110.r482a = quotient >> 16;
	spc7110.r482b = quotient >> 24;
	spc7110.r482c = remainder;
	spc7110.r482d = remainder >> 8;
	spc7110.r482f &= 0x7f;
}

// --- The thread ---

// Each turn of its loop starts what the S-CPU asked for, each after its wait, then takes a clock.
void spc7110Run(void) {
	switch (spc7110.state) {
	case 1: goto decompress;
	case 2: goto multiply;
	case 3: goto divide;
	}

top:
	if (spc7110.dcuPending) {
		spc7110.dcuPending = 0;
		if (spc7110.dcuMode != 3) {
			if (step(20)) {
				spc7110.state = 1;
				return;
			}
		decompress:
			dcuBeginTransfer();
		}
	}
	if (spc7110.mulPending) {
		spc7110.mulPending = 0;
		if (step(30)) {
			spc7110.state = 2;
			return;
		}
	multiply:
		aluMultiply();
	}
	if (spc7110.divPending) {
		spc7110.divPending = 0;
		if (step(40)) {
			spc7110.state = 3;
			return;
		}
	divide:
		aluDivide();
	}

	// A request made during a wait starts on the next turn. With none, nothing can start until the
	// S-CPU writes again, so the rest is clocks one at a time until the chip is ahead of it.
	spc7110.state = 0;
	if (spc7110.dcuPending || spc7110.mulPending || spc7110.divPending) {
		if (step(1)) {
			return;
		}
		goto top;
	}
	if (spc7110.th.clock <= cpu.th.clock) {
		spc7110.th.clock += spc7110.th.scalar * ((cpu.th.clock - spc7110.th.clock) / spc7110.th.scalar + 1);
	}
}

static void syncFromCpu(void) {
	if (spc7110.th.clock < cpu.th.clock) {
		spc7110Run();
	}
}

// --- The S-CPU's view ---

// $50:0000-ffff is $4800 and $58:0000-ffff is $4808.
static u32 registerOf(u32 address) {
	if ((address & 0xff0000) == 0x500000) {
		address = 0x4800;
	}
	if ((address & 0xff0000) == 0x580000) {
		address = 0x4808;
	}
	return 0x4800 | (address & 0x3f);
}

u8 spc7110Read(u32 address, u8 data) {
	syncFromCpu();
	switch (registerOf(address)) {
	case 0x4800: {
		u16 counter = spc7110.r4809 | spc7110.r480a << 8;
		counter--;
		spc7110.r4809 = counter;
		spc7110.r480a = counter >> 8;
		return dcuRead();
	}
	case 0x4801: return spc7110.r4801;
	case 0x4802: return spc7110.r4802;
	case 0x4803: return spc7110.r4803;
	case 0x4804: return spc7110.r4804;
	case 0x4805: return spc7110.r4805;
	case 0x4806: return spc7110.r4806;
	case 0x4807: return spc7110.r4807;
	case 0x4808: return 0x00;
	case 0x4809: return spc7110.r4809;
	case 0x480a: return spc7110.r480a;
	case 0x480b: return spc7110.r480b;
	case 0x480c: return spc7110.r480c;

	case 0x4810: {
		u8 value = spc7110.r4810;
		dataPortIncrement4810();
		return value;
	}
	case 0x4811: return spc7110.r4811;
	case 0x4812: return spc7110.r4812;
	case 0x4813: return spc7110.r4813;
	case 0x4814: return spc7110.r4814;
	case 0x4815: return spc7110.r4815;
	case 0x4816: return spc7110.r4816;
	case 0x4817: return spc7110.r4817;
	case 0x4818: return spc7110.r4818;
	case 0x481a:
		dataPortIncrement(3);
		return 0x00;

	case 0x4820: return spc7110.r4820;
	case 0x4821: return spc7110.r4821;
	case 0x4822: return spc7110.r4822;
	case 0x4823: return spc7110.r4823;
	case 0x4824: return spc7110.r4824;
	case 0x4825: return spc7110.r4825;
	case 0x4826: return spc7110.r4826;
	case 0x4827: return spc7110.r4827;
	case 0x4828: return spc7110.r4828;
	case 0x4829: return spc7110.r4829;
	case 0x482a: return spc7110.r482a;
	case 0x482b: return spc7110.r482b;
	case 0x482c: return spc7110.r482c;
	case 0x482d: return spc7110.r482d;
	case 0x482e: return spc7110.r482e;
	case 0x482f: return spc7110.r482f;

	case 0x4830: return spc7110.r4830;
	case 0x4831: return spc7110.r4831;
	case 0x4832: return spc7110.r4832;
	case 0x4833: return spc7110.r4833;
	case 0x4834: return spc7110.r4834;
	}
	return data;
}

void spc7110Write(u32 address, u8 data) {
	syncFromCpu();
	switch (registerOf(address)) {
	case 0x4801: spc7110.r4801 = data; break;
	case 0x4802: spc7110.r4802 = data; break;
	case 0x4803: spc7110.r4803 = data & 0x7f; break;
	case 0x4804: spc7110.r4804 = data; dcuLoadAddress(); break;
	case 0x4805: spc7110.r4805 = data; break;
	case 0x4806: spc7110.r4806 = data; spc7110.r480c &= 0x7f; spc7110.dcuPending = 1; break;
	case 0x4807: spc7110.r4807 = data; break;
	case 0x4809: spc7110.r4809 = data; break;
	case 0x480a: spc7110.r480a = data; break;
	case 0x480b: spc7110.r480b = data & 0x03; break;

	case 0x4811: spc7110.r4811 = data; break;
	case 0x4812: spc7110.r4812 = data; break;
	case 0x4813: spc7110.r4813 = data & 0x7f; dataPortRead(); break;
	case 0x4814: spc7110.r4814 = data; dataPortIncrement(1); break;
	case 0x4815:
		spc7110.r4815 = data;
		if (spc7110.r4818 & 2) {
			dataPortRead();
		}
		dataPortIncrement(2);
		break;
	case 0x4816: spc7110.r4816 = data; break;
	case 0x4817: spc7110.r4817 = data; break;
	case 0x4818: spc7110.r4818 = data & 0x7f; dataPortRead(); break;

	case 0x4820: spc7110.r4820 = data; break;
	case 0x4821: spc7110.r4821 = data; break;
	case 0x4822: spc7110.r4822 = data; break;
	case 0x4823: spc7110.r4823 = data; break;
	case 0x4824: spc7110.r4824 = data; break;
	case 0x4825: spc7110.r4825 = data; spc7110.r482f |= 0x81; spc7110.mulPending = 1; break;
	case 0x4826: spc7110.r4826 = data; break;
	case 0x4827: spc7110.r4827 = data; spc7110.r482f |= 0x80; spc7110.divPending = 1; break;
	case 0x482e: spc7110.r482e = data & 0x01; break;

	case 0x4830: spc7110.r4830 = data & 0x87; break;
	case 0x4831: spc7110.r4831 = data & 0x07; break;
	case 0x4832: spc7110.r4832 = data & 0x07; break;
	case 0x4833: spc7110.r4833 = data & 0x07; break;
	case 0x4834: spc7110.r4834 = data & 0x07; break;
	}
}

// The program ROM and the data ROM's banks, as the memory controller maps them: 00-3f,80-bf:8000-ffff
// and c0-ff:0000-ffff, a megabyte to each of four ranges of banks.
u8 spc7110ReadMCU(u32 address, u8 data) {
	if (address < 0x100000) {
		address &= 0x0fffff;
		if (promSize) {
			return cart.rom[busMirror(address, promSize)];
		}
		return dataromRead(address | 0x100000 * (spc7110.r4830 & 7));
	}
	if (address < 0x200000) {
		address &= 0x0fffff;
		if (spc7110.r4834 & 4) {
			return cart.rom[busMirror(0x100000 + address, promSize)];
		}
		return dataromRead(address | 0x100000 * (spc7110.r4831 & 7));
	}
	if (address < 0x300000) {
		return dataromRead((address & 0x0fffff) | 0x100000 * (spc7110.r4832 & 7));
	}
	if (address < 0x400000) {
		return dataromRead((address & 0x0fffff) | 0x100000 * (spc7110.r4833 & 7));
	}
	return data;
}

void spc7110WriteMCU(u32 address, u8 data) {
	(void) address;
	(void) data;
}

// The save RAM, at 00-3f,80-bf:6000-7fff when it is enabled.
u8 spc7110ReadRam(u32 address, u8 data) {
	(void) data;
	if ((spc7110.r4830 & 0x80) && cart.ramSize) {
		return cart.ram[busMirror(address, cart.ramSize)];
	}
	return 0x00;
}

void spc7110WriteRam(u32 address, u8 data) {
	if ((spc7110.r4830 & 0x80) && cart.ramSize) {
		cart.ram[busMirror(address, cart.ramSize)] = data;
	}
}

// The dump is the program ROM's megabyte, then the data ROM: five megabytes of it when an expansion
// ROM follows.
void spc7110Load(void) {
	memset(&spc7110, 0, sizeof(spc7110));
	if (cart.romSize <= 0x100000) {
		promSize = cart.romSize;
		drom = NULL;
		dromSize = 0;
		return;
	}
	promSize = 0x100000;
	drom = cart.rom + 0x100000;
	dromSize = cart.romSize == 0x700000 ? 0x500000 : cart.romSize - 0x100000;
}

// A reset leaves the decompressor as it was.
void spc7110Power(void) {
	threadFrequency(&spc7110.th, 21477272);
	spc7110.state = 0;
	spc7110.r4801 = spc7110.r4802 = spc7110.r4803 = spc7110.r4804 = 0;
	spc7110.r4805 = spc7110.r4806 = spc7110.r4807 = 0;
	spc7110.r4809 = spc7110.r480a = spc7110.r480b = spc7110.r480c = 0;
	spc7110.dcuPending = 0;
	spc7110.dcuMode = 0;
	spc7110.dcuAddress = 0;
	spc7110.r4810 = spc7110.r4811 = spc7110.r4812 = spc7110.r4813 = spc7110.r4814 = 0;
	spc7110.r4815 = spc7110.r4816 = spc7110.r4817 = spc7110.r4818 = spc7110.r481a = 0;
	spc7110.r4820 = spc7110.r4821 = spc7110.r4822 = spc7110.r4823 = 0;
	spc7110.r4824 = spc7110.r4825 = spc7110.r4826 = spc7110.r4827 = 0;
	spc7110.r4828 = spc7110.r4829 = spc7110.r482a = spc7110.r482b = 0;
	spc7110.r482c = spc7110.r482d = spc7110.r482e = spc7110.r482f = 0;
	spc7110.mulPending = 0;
	spc7110.divPending = 0;
	spc7110.r4830 = 0x00;
	spc7110.r4831 = 0x00;
	spc7110.r4832 = 0x01;
	spc7110.r4833 = 0x02;
	spc7110.r4834 = 0x00;
}
