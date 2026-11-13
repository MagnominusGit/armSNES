/* Copyright (c) 2004-2025 ares team, Near et al
 * Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Ported from ares (ISC); see LICENSES/ISC-ares.txt. */
#include "../snes.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

// The BS-X Satellaview's cartridges: the base cartridge's memory controller (MCC), which maps its
// ROM, its 512 KiB of PSRAM and a memory pack, and the memory pack itself, mask ROM or a Sharp flash
// chip with its command set. A flash pack runs a 1 MHz clock that completes block erases.

struct Mcc mcc;
struct BsMemory bsmemory;

#define B bsmemory

// The pack's contents, which flash writes change, and the base cartridge's PSRAM: each kept in a
// saved state when there is one.
static u8* pack;
static u32 packSize;
static u8 psram[0x80000];

// --- The memory pack ---

enum { MODE_FLASH, MODE_CHIP, MODE_PAGE, MODE_COMPATIBLE_STATUS, MODE_EXTENDED_STATUS };
enum { RYBY_LEVEL, RYBY_PULSE_WRITE, RYBY_PULSE_ERASE, RYBY_DISABLE };

static u32 blockCount(void) {
	return packSize >> 16;
}

static struct BsBlock* block(u32 id) {
	return &B.blocks[id & (blockCount() - 1)];
}

static u32 blockId(struct BsBlock* b) {
	return (u32) (b - B.blocks);
}

static void failed(void) {
	B.compatibleWriteFailed = 1;
	B.compatibleEraseFailed = 1;
	B.globalFailed = 1;
}

static u8 pageRead(u8 address) {
	return B.page[B.globalPage][address];
}

static void pageWrite(u8 address, u8 data) {
	B.page[B.globalPage][address] = data;
}

static void pageSwap(void) {
	B.globalPage ^= 1;
}

static u8 blockRead(struct BsBlock* b, u32 address) {
	return pack[blockId(b) << 16 | (address & 0xffff)];
}

// Writes to flash can only clear bits.
static void blockWrite(struct BsBlock* b, u32 address, u8 data) {
	if (!B.writable && b->statusLocked) {
		b->statusFailed = 1;
		failed();
		return;
	}
	u32 at = blockId(b) << 16 | (address & 0xffff);
	pack[at] &= data;
}

// An erase the S-CPU asks for runs on the pack's own clock; it takes 300 ms.
static void blockEraseStart(struct BsBlock* b) {
	b->erasing = 1;
	b->statusReady = 0;
	B.compatibleReady = 0;
	B.globalReady = 0;
}

static void blockEraseFinish(struct BsBlock* b) {
	b->erasing = 0;
	// It sets no failure bits when it can't.
	if (!B.writable && b->statusLocked) {
		return;
	}
	memset(&pack[blockId(b) << 16], 0xff, 0x10000);
	b->erased++;
	b->locked = 0;
	b->statusLocked = 0;
}

static void blockLock(struct BsBlock* b) {
	if (!B.writable) {
		b->statusFailed = 1;
		failed();
		return;
	}
	b->locked = 1;
	b->statusLocked = 1;
}

static u8 blockStatus(const struct BsBlock* b) {
	return b->statusVppLow << 2 | b->statusQueueFull << 3 | b->statusAborted << 4 | b->statusFailed << 5 |
	       !b->statusLocked << 6 | b->statusReady << 7;
}

static u8 compatibleStatus(void) {
	return B.compatibleVppLow << 3 | B.compatibleWriteFailed << 4 | B.compatibleEraseFailed << 5 |
	       B.compatibleEraseSuspended << 6 | B.compatibleReady << 7;
}

static u8 globalStatus(void) {
	return B.globalPage | B.globalPageReady << 1 | B.globalPageAvailable << 2 | B.globalQueueFull << 3 |
	       B.globalSleeping << 4 | B.globalFailed << 5 | B.globalSuspended << 6 | B.globalReady << 7;
}

static void queueFlush(void) {
	memset(B.queue, 0, sizeof(B.queue));
}

static void queuePop(void) {
	for (int i = 3; i >= 0; --i) {
		if (B.queue[i].valid) {
			memset(&B.queue[i], 0, sizeof(B.queue[i]));
			return;
		}
	}
}

static void queuePush(u32 address, u8 data) {
	for (int i = 0; i < 4; ++i) {
		if (!B.queue[i].valid) {
			B.queue[i].valid = 1;
			B.queue[i].address = address & 0xffffff;
			B.queue[i].data = data;
			return;
		}
	}
}

static u32 queueSize(void) {
	for (int i = 3; i >= 0; --i) {
		if (B.queue[i].valid) {
			return i + 1;
		}
	}
	return 0;
}

static u32 queueAddress(u32 i) {
	return i < 4 && B.queue[i].valid ? B.queue[i].address : 0;
}

static u8 queueData(u32 i) {
	return i < 4 && B.queue[i].valid ? B.queue[i].data : 0;
}

bool bsPresent(void) {
	return packSize != 0;
}

u8 bsRead(u32 address, u8 data) {
	if (!packSize) {
		return data;
	}
	if (B.rom) {
		return pack[busMirror(address, packSize)];
	}
	switch (B.mode) {
	case MODE_CHIP:
		if ((address & 0xffffff) == 0) return B.vendor;
		if ((address & 0xffffff) == 1) return B.device;
		if ((address & 7) == 2) return 0x63;
		return 0x20;
	case MODE_PAGE:
		return pageRead(address);
	case MODE_COMPATIBLE_STATUS:
		return compatibleStatus();
	case MODE_EXTENDED_STATUS:
		if ((address & 0xffff) == 0x0002) return blockStatus(block((address & 0xffffff) >> 16));
		if ((address & 0xffff) == 0x0004) return globalStatus();
		return 0x00;
	}
	return blockRead(block((address & 0xffffff) >> 16), address);
}

// A command needs as many bytes as it takes; until then they wait in the queue.
void bsWrite(u32 address, u8 data) {
	if (!packSize || B.rom) {
		return;
	}
	queuePush(address, data);
	u8 command = queueData(0);
	u32 size = queueSize();

	switch (command) {
	// The page buffer to flash.
	case 0x0c: {
		if (size < 3) return;
		bool swapped = queueAddress(1) & 1;
		u16 count = queueData(swapped ? 2 : 1) | queueData(swapped ? 1 : 2) << 8;
		u32 at = queueAddress(2);
		do {
			blockWrite(block(at >> 16), at, pageRead(at));
			at = (at + 1) & 0xffffff;
		} while (count--);
		pageSwap();
		B.mode = MODE_COMPATIBLE_STATUS;
		queueFlush();
		return;
	}
	// A byte.
	case 0x10:
	case 0x40:
		if (size < 2) return;
		blockWrite(block(queueAddress(1) >> 16), queueAddress(1), queueData(1));
		B.mode = MODE_COMPATIBLE_STATUS;
		queueFlush();
		return;
	// A block erased.
	case 0x20:
		if (size < 2) return;
		if (queueData(1) != 0xd0) {
			failed();
			queueFlush();
			return;
		}
		blockEraseStart(block(queueAddress(1) >> 16));
		B.mode = MODE_COMPATIBLE_STATUS;
		queueFlush();
		return;
	// The signature, size and serial number into the page buffer, which is then swapped.
	case 0x38: {
		if (size < 2) return;
		if (queueData(1) != 0xd0) {
			failed();
			queueFlush();
			return;
		}
		u32 log2 = 0;
		while ((packSize >> 10) >> (log2 + 1)) {
			log2++;
		}
		pageWrite(0x00, 0x4d);
		pageWrite(0x02, 0x50);
		pageWrite(0x04, 0x04);
		pageWrite(0x06, 0x10 | (log2 & 15));
		pageWrite(0x08, B.serial >> 40);
		pageWrite(0x0a, B.serial >> 32);
		pageWrite(0x0c, B.serial >> 24);
		pageWrite(0x0e, B.serial >> 16);
		pageWrite(0x10, B.serial >> 8);
		pageWrite(0x12, B.serial >> 0);
		pageSwap();
		queueFlush();
		return;
	}
	// The status registers cleared.
	case 0x50:
		for (u32 id = 0; id < blockCount(); ++id) {
			block(id)->statusVppLow = 0;
			block(id)->statusFailed = 0;
		}
		B.compatibleVppLow = 0;
		B.compatibleWriteFailed = 0;
		B.compatibleEraseFailed = 0;
		B.globalFailed = 0;
		queueFlush();
		return;
	case 0x70:
		B.mode = MODE_COMPATIBLE_STATUS;
		queueFlush();
		return;
	case 0x71:
		B.mode = MODE_EXTENDED_STATUS;
		queueFlush();
		return;
	case 0x72:
		pageSwap();
		queueFlush();
		return;
	// One byte into the page buffer.
	case 0x74:
		if (size < 2) return;
		pageWrite(queueAddress(1), queueData(1));
		queueFlush();
		return;
	case 0x75:
		B.mode = MODE_PAGE;
		queueFlush();
		return;
	case 0x77:
		if (size < 2) return;
		if (queueData(1) != 0xd0) {
			failed();
			queueFlush();
			return;
		}
		blockLock(block(queueAddress(1) >> 16));
		queueFlush();
		return;
	// Abort puts the chip to sleep.
	case 0x80:
		B.globalSleeping = 1;
		queueFlush();
		return;
	case 0x90:
		B.mode = MODE_CHIP;
		queueFlush();
		return;
	case 0x96:
		if (size < 2) return;
		if (queueData(1) == 0x01) B.readyBusyMode = RYBY_LEVEL;
		if (queueData(1) == 0x02) B.readyBusyMode = RYBY_PULSE_WRITE;
		if (queueData(1) == 0x03) B.readyBusyMode = RYBY_PULSE_ERASE;
		if (queueData(1) == 0x04) B.readyBusyMode = RYBY_DISABLE;
		queueFlush();
		return;
	// The lock bits read back from the device.
	case 0x97:
		if (size < 2) return;
		if (queueData(1) != 0xd0) {
			failed();
			queueFlush();
			return;
		}
		for (u32 id = 0; id < blockCount(); ++id) {
			block(id)->statusLocked = block(id)->locked;
		}
		queueFlush();
		return;
	// Each block's erase count into the page buffer.
	case 0x99:
		if (size < 2) return;
		if (queueData(1) != 0xd0) {
			failed();
			queueFlush();
			return;
		}
		pageWrite(0x06, 0x06);
		pageWrite(0x07, 0x00);
		for (u32 id = 0; id < blockCount(); ++id) {
			u8 at = (id & 3) * 0x08 + (id >> 2 & 3) * 0x40 + (id >> 4 & 1) * 0x20 + (id >> 5 & 1) * 0x04;
			u32 erased = 1u << 31 | block(id)->erased;
			for (u32 byte = 0; byte < 4; ++byte) {
				pageWrite((u8) (at + byte), erased >> (byte * 8));
			}
		}
		pageSwap();
		queueFlush();
		return;
	// Every block erased.
	case 0xa7:
		if (size < 2) return;
		if (queueData(1) != 0xd0) {
			failed();
			queueFlush();
			return;
		}
		for (u32 id = 0; id < blockCount(); ++id) {
			blockEraseStart(block(id));
		}
		B.mode = MODE_COMPATIBLE_STATUS;
		queueFlush();
		return;
	// Erase suspend and resume.
	case 0xb0:
		if (size < 2) return;
		if (queueData(1) != 0xd0) {
			failed();
			queueFlush();
			return;
		}
		B.mode = MODE_COMPATIBLE_STATUS;
		queueFlush();
		return;
	// Bytes into the page buffer one after another; the count stays in the queue meanwhile.
	case 0xe0: {
		if (size < 4) return;
		u16 count = queueData(1) | queueData(2) << 8;
		pageWrite(queueAddress(3), queueData(3));
		if (count--) {
			B.queue[1].data = count;
			B.queue[2].data = count >> 8;
			queuePop();
		} else {
			queueFlush();
		}
		return;
	}
	case 0xf0:
		B.globalSleeping = 1;
		queueFlush();
		return;
	// A word, its address's A0 toggling.
	case 0xfb: {
		if (size < 3) return;
		bool swapped = queueAddress(1) & 1;
		u16 value = queueData(swapped ? 2 : 1) | queueData(swapped ? 1 : 2) << 8;
		blockWrite(block(queueAddress(2) >> 16), queueAddress(2) ^ 0, value);
		blockWrite(block(queueAddress(2) >> 16), queueAddress(2) ^ 1, value >> 8);
		B.mode = MODE_COMPATIBLE_STATUS;
		queueFlush();
		return;
	}
	case 0xff:
		B.mode = MODE_FLASH;
		queueFlush();
		return;
	}
	queueFlush();
}

static void bsWritable(bool writable) {
	B.writable = !B.rom && writable;
}

// The pack's clock: an erase waiting runs (300 ms); otherwise the blocks are ready and 10 ms pass.
// It stops after the step that takes it past the S-CPU.
enum { BS_TOP, BS_ERASING, BS_IDLE };

static bool bsStep(u32 clocks) {
	B.th.clock += B.th.scalar * clocks;
	return cpu.th.clock < B.th.clock;
}

void bsRun(void) {
	for (;;) {
		switch (B.state) {
		case BS_TOP: {
			u32 id;
			for (id = 0; id < blockCount(); ++id) {
				if (block(id)->erasing) {
					break;
				}
				block(id)->statusReady = 1;
			}
			if (id < blockCount()) {
				B.erasingBlock = id;
				B.state = BS_ERASING;
				if (bsStep(300000)) {
					return;
				}
				continue;
			}
			B.compatibleReady = 1;
			B.globalReady = 1;
			B.state = BS_IDLE;
			if (bsStep(10000)) {
				return;
			}
			continue;
		}
		case BS_ERASING:
			blockEraseFinish(block(B.erasingBlock));
			B.state = BS_TOP;
			continue;
		case BS_IDLE:
			B.state = BS_TOP;
			continue;
		}
	}
}

// A pack in the slot: mask ROM, or flash of 1, 2 or 4 MiB (a smaller flash dump fills 1 MiB, the
// rest erased).
void bsLoad(const u8* data, size_t size) {
	memset(&B, 0, sizeof(B));
	free(pack);
	pack = NULL;
	packSize = 0;
	if (!data || !size) {
		return;
	}
	B.rom = false;
	packSize = size < 0x100000 ? 0x100000 : (u32) size;
	pack = malloc(packSize);
	memset(pack, 0xff, packSize);
	memcpy(pack, data, size);
	if (packSize != 0x100000 && packSize != 0x200000 && packSize != 0x400000) {
		free(pack);
		pack = NULL;
		packSize = 0;
		return;
	}
	B.vendor = 0xb0;
	B.device = packSize == 0x100000 ? 0x66a8 : 0x6688;
	B.serial = 0x000123456789ull;
	// ares fills the page buffers from its generator before power on, when it gives zeros.
	memset(B.page, 0, sizeof(B.page));
	for (u32 id = 0; id < 64; ++id) {
		B.blocks[id].erased = 1;
		B.blocks[id].locked = 1;
	}
}

u8* bsData(u32* size) {
	*size = B.rom ? 0 : packSize;
	return pack;
}

void bsPower(void) {
	if (!packSize || B.rom) {
		return;
	}
	threadFrequency(&B.th, 1000000);
	B.state = BS_TOP;
	for (u32 id = 0; id < 64; ++id) {
		B.blocks[id].erasing = 0;
		B.blocks[id].statusVppLow = 0;
		B.blocks[id].statusQueueFull = 0;
		B.blocks[id].statusAborted = 0;
		B.blocks[id].statusFailed = 0;
		B.blocks[id].statusLocked = 1;
		B.blocks[id].statusReady = 1;
	}
	B.compatibleVppLow = 0;
	B.compatibleWriteFailed = 0;
	B.compatibleEraseFailed = 0;
	B.compatibleEraseSuspended = 0;
	B.compatibleReady = 1;
	B.globalPage = 0;
	B.globalPageReady = 1;
	B.globalPageAvailable = 1;
	B.globalQueueFull = 0;
	B.globalSleeping = 0;
	B.globalFailed = 0;
	B.globalSuspended = 0;
	B.globalReady = 1;
	B.mode = MODE_FLASH;
	B.readyBusyMode = RYBY_DISABLE;
	queueFlush();
}

bool bsRunning(void) {
	return packSize && !B.rom;
}

// --- The MCC ---

static void mccCommit(void) {
	mcc.r = mcc.w;
	bsWritable(mcc.r.externallyWritable);
}

// $00-0f:5000-5fff: a bit 7 per register, by A16-A19.
u8 mccRead(u32 address, u8 data) {
	if ((address & 0xf0f000) != 0x005000) {
		return data;
	}
	switch (address >> 16 & 15) {
	case 0: return mcc.irqFlag << 7;
	case 1: return mcc.irqEnable << 7;
	case 2: return mcc.r.mapping << 7;
	case 3: return mcc.r.psramEnableLo << 7;
	case 4: return mcc.r.psramEnableHi << 7;
	case 5: return (mcc.r.psramMapping & 1) << 7;
	case 6: return (mcc.r.psramMapping >> 1 & 1) << 7;
	case 7: return mcc.r.romEnableLo << 7;
	case 8: return mcc.r.romEnableHi << 7;
	case 9: return mcc.r.exEnableLo << 7;
	case 10: return mcc.r.exEnableHi << 7;
	case 11: return mcc.r.exMapping << 7;
	case 12: return mcc.r.internallyWritable << 7;
	case 13: return mcc.r.externallyWritable << 7;
	}
	return 0;
}

void mccWrite(u32 address, u8 data) {
	if ((address & 0xf0f000) != 0x005000) {
		return;
	}
	bool bit = data >> 7;
	switch (address >> 16 & 15) {
	case 1: mcc.irqEnable = bit; break;
	case 2: mcc.w.mapping = bit; break;
	case 3: mcc.w.psramEnableLo = bit; break;
	case 4: mcc.w.psramEnableHi = bit; break;
	case 5: mcc.w.psramMapping = (mcc.w.psramMapping & 2) | bit; break;
	case 6: mcc.w.psramMapping = (mcc.w.psramMapping & 1) | bit << 1; break;
	case 7: mcc.w.romEnableLo = bit; break;
	case 8: mcc.w.romEnableHi = bit; break;
	case 9: mcc.w.exEnableLo = bit; break;
	case 10: mcc.w.exEnableHi = bit; break;
	case 11: mcc.w.exMapping = bit; break;
	case 12: mcc.w.internallyWritable = bit; break;
	case 13: mcc.w.externallyWritable = bit; break;
	case 14:
		if (bit) {
			mccCommit();
		}
		break;
	}
}

static u8 romAccess(bool write, u32 address, u8 data) {
	if (write) {
		return data;
	}
	return cart.rom[busMirror(address, cart.romSize)];
}

static u8 psramAccess(bool write, u32 address, u8 data) {
	address = busMirror(address, sizeof(psram));
	if (!write) {
		return psram[address];
	}
	psram[address] = data;
	return data;
}

static u8 bsAccess(bool write, u32 address, u8 data) {
	address = busMirror(address, packSize);
	if (!write) {
		return bsRead(address, data);
	}
	if (!mcc.r.internallyWritable) {
		return data;
	}
	bsWrite(address, data);
	return data;
}

// What the S-CPU sees through the MCC: the ROM, the PSRAM and the memory pack, each where the
// registers map it. The expansion memory isn't on the cartridge.
static u8 mcuAccess(bool write, u32 address, u8 data) {
	const struct MccRegisters* r = &mcc.r;
	if (r->romEnableLo && (address & 0xc08000) == 0x008000) {
		return romAccess(write, (address & 0x3f0000) >> 1 | (address & 0x7fff), data);
	}
	if (r->romEnableHi && (address & 0xc08000) == 0x808000) {
		return romAccess(write, (address & 0x3f0000) >> 1 | (address & 0x7fff), data);
	}

	if (r->psramEnableLo && r->mapping == 0) {
		if (((address & 0xf08000) == 0x008000 && r->psramMapping == 0) || ((address & 0xf08000) == 0x208000 && r->psramMapping == 1) ||
		    ((address & 0xf00000) == 0x400000 && r->psramMapping == 2) || ((address & 0xf00000) == 0x600000 && r->psramMapping == 3)) {
			return psramAccess(write, (address & 0x0f0000) >> 1 | (address & 0x7fff), data);
		}
		if ((address & 0xf08000) == 0x700000) {
			return psramAccess(write, (address & 0x0f0000) >> 1 | (address & 0x7fff), data);
		}
	}
	if (r->psramEnableHi && r->mapping == 0) {
		if (((address & 0xf08000) == 0x808000 && r->psramMapping == 0) || ((address & 0xf08000) == 0xa08000 && r->psramMapping == 1) ||
		    ((address & 0xf00000) == 0xc00000 && r->psramMapping == 2) || ((address & 0xf00000) == 0xe00000 && r->psramMapping == 3)) {
			return psramAccess(write, (address & 0x0f0000) >> 1 | (address & 0x7fff), data);
		}
		if ((address & 0xf08000) == 0xf00000) {
			return psramAccess(write, (address & 0x0f0000) >> 1 | (address & 0x7fff), data);
		}
	}
	if (r->psramEnableLo && r->mapping == 1) {
		if (((address & 0xf88000) == 0x008000 && r->psramMapping == 0) || ((address & 0xf88000) == 0x108000 && r->psramMapping == 1) ||
		    ((address & 0xf88000) == 0x208000 && r->psramMapping == 2) || ((address & 0xf88000) == 0x308000 && r->psramMapping == 3) ||
		    ((address & 0xf80000) == 0x400000 && r->psramMapping == 0) || ((address & 0xf80000) == 0x500000 && r->psramMapping == 1) ||
		    ((address & 0xf80000) == 0x600000 && r->psramMapping == 2) || ((address & 0xf80000) == 0x700000 && r->psramMapping == 3)) {
			return psramAccess(write, address & 0x07ffff, data);
		}
		if ((address & 0xe0e000) == 0x206000) {
			return psramAccess(write, (address & 0x3f0000) >> 3 | (address & 0x1fff), data);
		}
	}
	if (r->psramEnableHi && r->mapping == 1) {
		if (((address & 0xf88000) == 0x808000 && r->psramMapping == 0) || ((address & 0xf88000) == 0x908000 && r->psramMapping == 1) ||
		    ((address & 0xf88000) == 0xa08000 && r->psramMapping == 2) || ((address & 0xf88000) == 0xb08000 && r->psramMapping == 3) ||
		    ((address & 0xf80000) == 0xc00000 && r->psramMapping == 0) || ((address & 0xf80000) == 0xd00000 && r->psramMapping == 1) ||
		    ((address & 0xf80000) == 0xe00000 && r->psramMapping == 2) || ((address & 0xf80000) == 0xf00000 && r->psramMapping == 3)) {
			return psramAccess(write, address & 0x07ffff, data);
		}
		if ((address & 0xe0e000) == 0xa06000) {
			return psramAccess(write, (address & 0x3f0000) >> 3 | (address & 0x1fff), data);
		}
	}

	if (r->exEnableLo && r->mapping == 0) {
		if (((address & 0xe08000) == 0x008000 && r->exMapping == 0) || ((address & 0xe00000) == 0x400000 && r->exMapping == 1)) {
			return data;
		}
	}
	if (r->exEnableLo && r->mapping == 1) {
		if (((address & 0xf08000) == 0x008000 && r->exMapping == 0) || ((address & 0xf08000) == 0x208000 && r->exMapping == 1) ||
		    ((address & 0xf00000) == 0x400000 && r->exMapping == 0) || ((address & 0xf00000) == 0x600000 && r->exMapping == 1)) {
			return data;
		}
	}
	if (r->exEnableHi && r->mapping == 0) {
		if (((address & 0xe08000) == 0x808000 && r->exMapping == 0) || ((address & 0xe00000) == 0xc00000 && r->exMapping == 1)) {
			return data;
		}
	}
	if (r->exEnableHi && r->mapping == 1) {
		if (((address & 0xf08000) == 0x808000 && r->exMapping == 0) || ((address & 0xf08000) == 0xa08000 && r->exMapping == 1) ||
		    ((address & 0xf00000) == 0xc00000 && r->exMapping == 0) || ((address & 0xf00000) == 0xe00000 && r->exMapping == 1)) {
			return data;
		}
	}

	if (packSize && ((address & 0x408000) == 0x008000 || (address & 0x400000) == 0x400000)) {
		return bsAccess(write, r->mapping == 0 ? (address & 0x3f0000) >> 1 | (address & 0x7fff) : address & 0x3fffff, data);
	}
	return data;
}

u8 mccReadMCU(u32 address, u8 data) {
	return mcuAccess(false, address, data);
}

void mccWriteMCU(u32 address, u8 data) {
	mcuAccess(true, address, data);
}

// The PSRAM starts out as 0xff, as ares's loader leaves new RAM.
void mccLoad(void) {
	memset(&mcc, 0, sizeof(mcc));
	memset(psram, 0xff, sizeof(psram));
}

u8* mccPsram(u32* size) {
	*size = sizeof(psram);
	return psram;
}

void mccPower(void) {
	mcc.irqFlag = 0;
	mcc.irqEnable = 0;
	mcc.w.mapping = 1;
	mcc.w.psramEnableLo = 1;
	mcc.w.psramEnableHi = 0;
	mcc.w.psramMapping = 3;
	mcc.w.romEnableLo = 1;
	mcc.w.romEnableHi = 1;
	mcc.w.exEnableLo = 1;
	mcc.w.exEnableHi = 0;
	mcc.w.exMapping = 1;
	mcc.w.internallyWritable = 0;
	mcc.w.externallyWritable = 0;
	mccCommit();
}

// --- The Satellaview base unit under the console: its receiver's registers, as ares has them ---
// Nothing is broadcast: the registers hold what is written, and the clock gives the frontend's time.

struct Satellaview satellaview;
s64 satellaviewNow;

#define V satellaview

void satellaviewPower(void) {
	memset(&V, 0, sizeof(V));
}

u8 satellaviewRead(u32 address, u8 data) {
	switch (address & 0xffff) {
	case 0x2188: return V.r2188;
	case 0x2189: return V.r2189;
	case 0x218a: return V.r218a;
	case 0x218c: return V.r218c;
	case 0x218e: return V.r218e;
	case 0x218f: return V.r218f;
	case 0x2190: return V.r2190;
	case 0x2192: {
		u32 counter = V.rtcCounter++;
		if (V.rtcCounter >= 18) {
			V.rtcCounter = 0;
		}
		if (counter == 0) {
			time_t host = (time_t) satellaviewNow;
			struct tm* t = localtime(&host);
			if (t) {
				V.rtcHour = t->tm_hour;
				V.rtcMinute = t->tm_min;
				V.rtcSecond = t->tm_sec;
			}
		}
		switch (counter) {
		case 5:
		case 6:
			return 0x01;
		case 10: return V.rtcSecond;
		case 11: return V.rtcMinute;
		case 12: return V.rtcHour;
		}
		return 0x00;
	}
	case 0x2193: return V.r2193 & ~0x0c;
	case 0x2194: return V.r2194;
	case 0x2196: return V.r2196;
	case 0x2197: return V.r2197;
	case 0x2199: return V.r2199;
	}
	return data;
}

void satellaviewWrite(u32 address, u8 data) {
	switch (address & 0xffff) {
	case 0x2188: V.r2188 = data; break;
	case 0x2189: V.r2189 = data; break;
	case 0x218a: V.r218a = data; break;
	case 0x218b: V.r218b = data; break;
	case 0x218c: V.r218c = data; break;
	case 0x218e: V.r218e = data; break;
	case 0x218f:
		V.r218e >>= 1;
		V.r218e = V.r218f - V.r218e;
		V.r218f >>= 1;
		break;
	case 0x2191:
		V.r2191 = data;
		V.rtcCounter = 0;
		break;
	case 0x2192: V.r2190 = 0x80; break;
	case 0x2193: V.r2193 = data; break;
	case 0x2194: V.r2194 = data; break;
	case 0x2197: V.r2197 = data; break;
	case 0x2199: V.r2199 = data; break;
	}
}
