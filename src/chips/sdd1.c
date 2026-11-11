/* Copyright (c) 2004-2025 ares team, Near et al
 * Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Ported from ares (ISC); see LICENSES/ISC-ares.txt. */
#include "../snes.h"

// The S-DD1: a ROM mapper that decompresses graphics as the S-CPU's DMA reads them. It watches the DMA
// registers for each channel's address and size.

struct Sdd1 sdd1;

static inline u8 romRead(u32 address) {
	return cart.rom[busMirror(address & 0xffffff, cart.romSize)];
}

static u8 mmcRead(u32 address) {
	u32 low = address & 0xfffff;
	switch (address >> 20 & 3) {
	case 0: return romRead((sdd1.r4804 & 15) << 20 | low);
	case 1: return romRead((sdd1.r4805 & 15) << 20 | low);
	case 2: return romRead((sdd1.r4806 & 15) << 20 | low);
	default: return romRead((sdd1.r4807 & 15) << 20 | low);
	}
}

// Input manager

static u8 getCodeWord(u8 codeLength) {
	u8 codeWord = mmcRead(sdd1.imOffset) << sdd1.imBitCount;
	sdd1.imBitCount++;

	if (codeWord & 0x80) {
		codeWord |= mmcRead(sdd1.imOffset + 1) >> (9 - sdd1.imBitCount);
		sdd1.imBitCount += codeLength;
	}

	if (sdd1.imBitCount & 0x08) {
		sdd1.imOffset++;
		sdd1.imBitCount &= 0x07;
	}

	return codeWord;
}

// Golomb-code decoder

static const u8 runCount[256] = {
	0x00, 0x00, 0x01, 0x00, 0x03, 0x01, 0x02, 0x00, 0x07, 0x03, 0x05, 0x01, 0x06, 0x02, 0x04, 0x00,
	0x0f, 0x07, 0x0b, 0x03, 0x0d, 0x05, 0x09, 0x01, 0x0e, 0x06, 0x0a, 0x02, 0x0c, 0x04, 0x08, 0x00,
	0x1f, 0x0f, 0x17, 0x07, 0x1b, 0x0b, 0x13, 0x03, 0x1d, 0x0d, 0x15, 0x05, 0x19, 0x09, 0x11, 0x01,
	0x1e, 0x0e, 0x16, 0x06, 0x1a, 0x0a, 0x12, 0x02, 0x1c, 0x0c, 0x14, 0x04, 0x18, 0x08, 0x10, 0x00,
	0x3f, 0x1f, 0x2f, 0x0f, 0x37, 0x17, 0x27, 0x07, 0x3b, 0x1b, 0x2b, 0x0b, 0x33, 0x13, 0x23, 0x03,
	0x3d, 0x1d, 0x2d, 0x0d, 0x35, 0x15, 0x25, 0x05, 0x39, 0x19, 0x29, 0x09, 0x31, 0x11, 0x21, 0x01,
	0x3e, 0x1e, 0x2e, 0x0e, 0x36, 0x16, 0x26, 0x06, 0x3a, 0x1a, 0x2a, 0x0a, 0x32, 0x12, 0x22, 0x02,
	0x3c, 0x1c, 0x2c, 0x0c, 0x34, 0x14, 0x24, 0x04, 0x38, 0x18, 0x28, 0x08, 0x30, 0x10, 0x20, 0x00,
	0x7f, 0x3f, 0x5f, 0x1f, 0x6f, 0x2f, 0x4f, 0x0f, 0x77, 0x37, 0x57, 0x17, 0x67, 0x27, 0x47, 0x07,
	0x7b, 0x3b, 0x5b, 0x1b, 0x6b, 0x2b, 0x4b, 0x0b, 0x73, 0x33, 0x53, 0x13, 0x63, 0x23, 0x43, 0x03,
	0x7d, 0x3d, 0x5d, 0x1d, 0x6d, 0x2d, 0x4d, 0x0d, 0x75, 0x35, 0x55, 0x15, 0x65, 0x25, 0x45, 0x05,
	0x79, 0x39, 0x59, 0x19, 0x69, 0x29, 0x49, 0x09, 0x71, 0x31, 0x51, 0x11, 0x61, 0x21, 0x41, 0x01,
	0x7e, 0x3e, 0x5e, 0x1e, 0x6e, 0x2e, 0x4e, 0x0e, 0x76, 0x36, 0x56, 0x16, 0x66, 0x26, 0x46, 0x06,
	0x7a, 0x3a, 0x5a, 0x1a, 0x6a, 0x2a, 0x4a, 0x0a, 0x72, 0x32, 0x52, 0x12, 0x62, 0x22, 0x42, 0x02,
	0x7c, 0x3c, 0x5c, 0x1c, 0x6c, 0x2c, 0x4c, 0x0c, 0x74, 0x34, 0x54, 0x14, 0x64, 0x24, 0x44, 0x04,
	0x78, 0x38, 0x58, 0x18, 0x68, 0x28, 0x48, 0x08, 0x70, 0x30, 0x50, 0x10, 0x60, 0x20, 0x40, 0x00,
};

static void getRunCount(u8 codeNumber, u8* mpsCount, bool* lpsIndex) {
	u8 codeWord = getCodeWord(codeNumber);

	if (codeWord & 0x80) {
		*lpsIndex = 1;
		*mpsCount = runCount[codeWord >> (codeNumber ^ 0x07)];
	} else {
		*mpsCount = 1 << codeNumber;
	}
}

// Bits generators, one for each code number

static u8 bgGetBit(u8 codeNumber, bool* endOfRun) {
	__typeof__(sdd1.bg[0])* bg = &sdd1.bg[codeNumber];
	if (!(bg->mpsCount || bg->lpsIndex)) {
		getRunCount(codeNumber, &bg->mpsCount, &bg->lpsIndex);
	}

	u8 bit;
	if (bg->mpsCount) {
		bit = 0;
		bg->mpsCount--;
	} else {
		bit = 1;
		bg->lpsIndex = 0;
	}

	*endOfRun = !(bg->mpsCount || bg->lpsIndex);
	return bit;
}

// Probability estimation module

static const struct {
	u8 codeNumber, nextIfMps, nextIfLps;
} evolutionTable[33] = {
	{ 0, 25, 25 }, { 0, 2, 1 }, { 0, 3, 1 }, { 0, 4, 2 }, { 0, 5, 3 }, { 1, 6, 4 }, { 1, 7, 5 },
	{ 1, 8, 6 }, { 1, 9, 7 }, { 2, 10, 8 }, { 2, 11, 9 }, { 2, 12, 10 }, { 2, 13, 11 }, { 3, 14, 12 },
	{ 3, 15, 13 }, { 3, 16, 14 }, { 3, 17, 15 }, { 4, 18, 16 }, { 4, 19, 17 }, { 5, 20, 18 },
	{ 5, 21, 19 }, { 6, 22, 20 }, { 6, 23, 21 }, { 7, 24, 22 }, { 7, 24, 23 }, { 0, 26, 1 },
	{ 1, 27, 2 }, { 2, 28, 4 }, { 3, 29, 8 }, { 4, 30, 12 }, { 5, 31, 16 }, { 6, 32, 18 }, { 7, 24, 22 },
};

static u8 pemGetBit(u8 context) {
	__typeof__(sdd1.contextInfo[0])* info = &sdd1.contextInfo[context];
	u8 currentStatus = info->status;
	u8 currentMps = info->mps;
	bool endOfRun = false;
	u8 bit = bgGetBit(evolutionTable[currentStatus].codeNumber, &endOfRun);

	if (endOfRun) {
		if (bit) {
			if (!(currentStatus & 0xfe)) {
				info->mps ^= 0x01;
			}
			info->status = evolutionTable[currentStatus].nextIfLps;
		} else {
			info->status = evolutionTable[currentStatus].nextIfMps;
		}
	}

	return bit ^ currentMps;
}

// Context model

static u8 cmGetBit(void) {
	switch (sdd1.cmBitplanesInfo) {
	case 0x00:
		sdd1.cmCurrentBitplane ^= 0x01;
		break;
	case 0x40:
		sdd1.cmCurrentBitplane ^= 0x01;
		if (!(sdd1.cmBitNumber & 0x7f)) {
			sdd1.cmCurrentBitplane = (sdd1.cmCurrentBitplane + 2) & 0x07;
		}
		break;
	case 0x80:
		sdd1.cmCurrentBitplane ^= 0x01;
		if (!(sdd1.cmBitNumber & 0x7f)) {
			sdd1.cmCurrentBitplane ^= 0x02;
		}
		break;
	case 0xc0:
		sdd1.cmCurrentBitplane = sdd1.cmBitNumber & 0x07;
		break;
	}

	u16* contextBits = &sdd1.cmPreviousBitplaneBits[sdd1.cmCurrentBitplane];
	u8 currentContext = (sdd1.cmCurrentBitplane & 0x01) << 4;
	switch (sdd1.cmContextBitsInfo) {
	case 0x00: currentContext |= ((*contextBits & 0x01c0) >> 5) | (*contextBits & 0x0001); break;
	case 0x10: currentContext |= ((*contextBits & 0x0180) >> 5) | (*contextBits & 0x0001); break;
	case 0x20: currentContext |= ((*contextBits & 0x00c0) >> 5) | (*contextBits & 0x0001); break;
	case 0x30: currentContext |= ((*contextBits & 0x0180) >> 5) | (*contextBits & 0x0003); break;
	}

	u8 bit = pemGetBit(currentContext);
	*contextBits = *contextBits << 1 | bit;
	sdd1.cmBitNumber++;
	return bit;
}

// Output logic

static u8 olDecompress(void) {
	switch (sdd1.olBitplanesInfo) {
	case 0x00:
	case 0x40:
	case 0x80:
		if (sdd1.olR0 == 0) {
			sdd1.olR0 = ~sdd1.olR0;
			return sdd1.olR2;
		}
		for (sdd1.olR0 = 0x80, sdd1.olR1 = 0, sdd1.olR2 = 0; sdd1.olR0; sdd1.olR0 >>= 1) {
			if (cmGetBit()) {
				sdd1.olR1 |= sdd1.olR0;
			}
			if (cmGetBit()) {
				sdd1.olR2 |= sdd1.olR0;
			}
		}
		return sdd1.olR1;
	case 0xc0:
		for (sdd1.olR0 = 0x01, sdd1.olR1 = 0; sdd1.olR0; sdd1.olR0 <<= 1) {
			if (cmGetBit()) {
				sdd1.olR1 |= sdd1.olR0;
			}
		}
		return sdd1.olR1;
	}
	return 0;
}

static void decompressorInit(u32 offset) {
	sdd1.imOffset = offset;
	sdd1.imBitCount = 4;
	memset(sdd1.bg, 0, sizeof(sdd1.bg));
	memset(sdd1.contextInfo, 0, sizeof(sdd1.contextInfo));

	sdd1.cmBitplanesInfo = mmcRead(offset) & 0xc0;
	sdd1.cmContextBitsInfo = mmcRead(offset) & 0x30;
	sdd1.cmBitNumber = 0;
	memset(sdd1.cmPreviousBitplaneBits, 0, sizeof(sdd1.cmPreviousBitplaneBits));
	switch (sdd1.cmBitplanesInfo) {
	case 0x00: sdd1.cmCurrentBitplane = 1; break;
	case 0x40: sdd1.cmCurrentBitplane = 7; break;
	case 0x80: sdd1.cmCurrentBitplane = 3; break;
	}

	sdd1.olBitplanesInfo = mmcRead(offset) & 0xc0;
	sdd1.olR0 = 0x01;
}

// The bus

u8 sdd1ReadIO(u32 address, u8 data) {
	(void) data;
	address = 0x4800 | (address & 15);
	switch (address) {
	case 0x4800: return sdd1.r4800;
	case 0x4801: return sdd1.r4801;
	case 0x4804: return sdd1.r4804;
	case 0x4805: return sdd1.r4805;
	case 0x4806: return sdd1.r4806;
	case 0x4807: return sdd1.r4807;
	}
	// 4802-4803 and 4808-480f fall through to ROM.
	return romRead(address);
}

void sdd1WriteIO(u32 address, u8 data) {
	address = 0x4800 | (address & 15);
	switch (address) {
	case 0x4800: sdd1.r4800 = data; break;
	case 0x4801: sdd1.r4801 = data; break;
	case 0x4804: sdd1.r4804 = data & 0x8f; break;
	case 0x4805: sdd1.r4805 = data & 0x8f; break;
	case 0x4806: sdd1.r4806 = data & 0x8f; break;
	case 0x4807: sdd1.r4807 = data & 0x8f; break;
	}
}

static u8 dmaRead(u32 address, u8 data) {
	return cpuReadDMA(address, data);
}

static void dmaWrite(u32 address, u8 data) {
	u32 channel = address >> 4 & 7;
	switch (address & 15) {
	case 2: sdd1.dma[channel].address = (sdd1.dma[channel].address & 0xffff00) | data; break;
	case 3: sdd1.dma[channel].address = (sdd1.dma[channel].address & 0xff00ff) | data << 8; break;
	case 4: sdd1.dma[channel].address = (sdd1.dma[channel].address & 0x00ffff) | data << 16; break;
	case 5: sdd1.dma[channel].size = (sdd1.dma[channel].size & 0xff00) | data; break;
	case 6: sdd1.dma[channel].size = (sdd1.dma[channel].size & 0x00ff) | data << 8; break;
	}
	cpuWriteDMA(address, data);
}

// The S-CPU's DMA registers go through the S-DD1, which notes what it needs from them.
void sdd1Map(void) {
	busMap(dmaRead, dmaWrite, "00-3f,80-bf:4300-437f", 0, 0, 0);
}

u8 sdd1ReadMCU(u32 address, u8 data) {
	address &= 0xffffff;
	// 00-3f,80-bf:8000-ffff
	if (!(address & 0x400000)) {
		if (!(address & 0x800000) && (address & 0x200000) && (sdd1.r4805 & 0x80)) {
			address &= ~0x200000;
		}
		if ((address & 0x800000) && (address & 0x200000) && (sdd1.r4807 & 0x80)) {
			address &= ~0x200000;
		}
		address = (address >> 16 & 0x3f) << 15 | (address & 0x7fff);
		return romRead(address);
	}

	// c0-ff:0000-ffff
	if (sdd1.r4800 & sdd1.r4801) {
		for (u32 n = 0; n < 8; ++n) {
			if ((sdd1.r4800 >> n & 1) && (sdd1.r4801 >> n & 1)) {
				// The S-DD1 always uses fixed transfers, so the address stays the same.
				if (address == sdd1.dma[n].address) {
					if (!sdd1.dmaReady) {
						decompressorInit(address);
						sdd1.dmaReady = true;
					}
					data = olDecompress();
					if (--sdd1.dma[n].size == 0) {
						sdd1.dmaReady = false;
						sdd1.r4801 &= ~(1 << n);
					}
					return data;
				}
			}
		}
	}

	return mmcRead(address);
}

void sdd1WriteMCU(u32 address, u8 data) {
	(void) address;
	(void) data;
}

void sdd1Power(void) {
	memset(&sdd1, 0, sizeof(sdd1));
	sdd1.r4805 = 0x01;
	sdd1.r4806 = 0x02;
	sdd1.r4807 = 0x03;
}
