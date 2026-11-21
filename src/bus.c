/* Copyright (c) 2004-2025 ares team, Near et al
 * Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Ported from ares (ISC); see LICENSES/ISC-ares.txt. */
#include "snes.h"

#include <stdlib.h>

// The 24-bit address bus. Each 256-byte page goes to one reader and writer with offsets that run on
// from the page's first; the few pages that hold several things keep them per address.

struct Page {
	u8 id;
	bool mixed;
	// The offset of the page's first address, or which mixed page this is.
	u32 base;
};
struct MixedPage {
	u8 id[256];
	u32 target[256];
};

// The last handler is the cheats' (busCheat).
enum { CHEAT_HANDLER = 255 };

static struct Page pages[0x10000];
// Pages whose reads are plain memory: where their first byte is (null for the others).
const u8* busDirectPages[0x10000];
static struct {
	BusReader reader;
	const u8* memory;
} directReaders[4];
static u32 directCount;
// Fast mode's: as busDirectPages, with the pages of readers plain memory only nearly always.
const u8* busFastPages[0x10000];
static struct {
	BusReader reader;
	const u8* memory;
	u32 size;
} fastReaders[2];
static u32 fastCount;
static struct MixedPage* mixedPages;
static u32 mixedCount, mixedSpace;
static BusReader readers[256];

// Pages with a cheat code read through the last handler, which goes to the page's own and then the
// cheats; other pages read as they are.
enum { CHEAT_PAGES = 512 };
static struct {
	u32 page;
	struct Page was;
} hooked[CHEAT_PAGES];
static u32 hookedCount;

static BusWriter writers[256];
static u32 handlers;

// While a machine is put together, a page some map covers only part of, or whose addresses don't run
// on, is mixed; those that end up running on again are made whole by busFinish.
static bool building;

u32 busMirrorOver(u32 address, u32 size) {
	if (size == 0) {
		return 0;
	}
	u32 base = 0;
	u32 mask = 1 << 23;
	while (address >= size) {
		while (!(address & mask)) {
			mask >>= 1;
		}
		address -= mask;
		if (size > mask) {
			size -= mask;
			base += mask;
		}
		mask >>= 1;
	}
	return base + address;
}

u32 busReduce(u32 address, u32 mask) {
	while (mask) {
		u32 bits = (mask & -mask) - 1;
		address = ((address >> 1) & ~bits) | (address & bits);
		mask = (mask & (mask - 1)) >> 1;
	}
	return address;
}

static u8 readOpen(u32 address, u8 data) {
	(void) address;
	return data;
}

static void writeOpen(u32 address, u8 data) {
	(void) address;
	(void) data;
}

void busReset(void) {
	free(mixedPages);
	mixedPages = NULL;
	mixedCount = mixedSpace = 0;
	memset(pages, 0, sizeof(pages));
	building = true;
	memset(readers, 0, sizeof(readers));
	memset(writers, 0, sizeof(writers));
	readers[0] = readOpen;
	writers[0] = writeOpen;
	handlers = 1;
	hookedCount = 0;
	directCount = 0;
	fastCount = 0;
}

void busDirect(BusReader reader, const u8* memory) {
	if (directCount < sizeof(directReaders) / sizeof(directReaders[0])) {
		directReaders[directCount].reader = reader;
		directReaders[directCount].memory = memory;
		++directCount;
	}
}

void busDirectFast(BusReader reader, const u8* memory, u32 size) {
	if (fastCount < sizeof(fastReaders) / sizeof(fastReaders[0])) {
		fastReaders[fastCount].reader = reader;
		fastReaders[fastCount].memory = memory;
		fastReaders[fastCount].size = size;
		++fastCount;
	}
}

const u8* busPageMemory(u32 page, BusReader reader, const u8* memory) {
	const struct Page* p = &pages[page];
	return !p->mixed && readers[p->id] == reader ? memory + p->base : NULL;
}

static const u8* directFor(const struct Page* page);

// The interrupt vectors' page, in either half of the banks, is always read through its reader.
static const u8* fastFor(u32 at, const struct Page* page) {
	const u8* direct = directFor(page);
	if (direct || page->mixed || (at & 0x7fff) == 0x00ff) {
		return direct;
	}
	for (u32 i = 0; i < fastCount; ++i) {
		if (readers[page->id] == fastReaders[i].reader && page->base + 0x100 <= fastReaders[i].size) {
			return fastReaders[i].memory + page->base;
		}
	}
	return NULL;
}

static const u8* directFor(const struct Page* page) {
	if (page->mixed) {
		return NULL;
	}
	for (u32 i = 0; i < directCount; ++i) {
		if (readers[page->id] == directReaders[i].reader) {
			return directReaders[i].memory + page->base;
		}
	}
	return NULL;
}

static u32 hexAt(const char** text) {
	u32 value = 0;
	for (;; ++*text) {
		char c = **text;
		if (c >= '0' && c <= '9') {
			value = value << 4 | (c - '0');
		} else if (c >= 'a' && c <= 'f') {
			value = value << 4 | (c - 'a' + 10);
		} else {
			return value;
		}
	}
}

// One "lo-hi" or "value" of a comma-separated list; false at its end.
static bool rangeAt(const char** text, char end, u32* lo, u32* hi) {
	if (**text == end || !**text) {
		return false;
	}
	*lo = *hi = hexAt(text);
	if (**text == '-') {
		++*text;
		*hi = hexAt(text);
	}
	if (**text == ',') {
		++*text;
	}
	return true;
}

static void setAddress(u32 address, u8 id, u32 offset) {
	struct Page* page = &pages[address >> 8];
	u32 n = address & 0xff;
	if (!page->mixed) {
		if (page->id == id && page->base + n == offset) {
			return;
		}
		if (mixedCount == mixedSpace) {
			mixedSpace = mixedSpace ? mixedSpace * 2 : 64;
			mixedPages = realloc(mixedPages, mixedSpace * sizeof(*mixedPages));
		}
		struct MixedPage* mixed = &mixedPages[mixedCount];
		for (u32 k = 0; k < 256; ++k) {
			mixed->id[k] = page->id;
			mixed->target[k] = page->base + k;
		}
		*page = (struct Page) { .id = 0, .mixed = true, .base = mixedCount++ };
	}
	mixedPages[page->base].id[n] = id;
	mixedPages[page->base].target[n] = offset;
}

void busMap(BusReader reader, BusWriter writer, const char* address, u32 size, u32 base, u32 mask) {
	if (handlers >= CHEAT_HANDLER || !building) {
		return;
	}
	u32 id = handlers++;
	readers[id] = reader;
	writers[id] = writer;

	const char* addrs = strchr(address, ':');
	if (!addrs) {
		return;
	}
	++addrs;

	const char* bankText = address;
	u32 bankLo, bankHi;
	while (rangeAt(&bankText, ':', &bankLo, &bankHi)) {
		const char* addrText = addrs;
		u32 addrLo, addrHi;
		while (rangeAt(&addrText, 0, &addrLo, &addrHi)) {
			for (u32 bank = bankLo; bank <= bankHi; ++bank) {
				for (u32 addr = addrLo; addr <= addrHi;) {
					// A whole page at a time, so a page whose addresses run on is never made mixed.
					u32 count = !(addr & 0xff) && addrHi - addr >= 0xff ? 256 : 1;
					u32 offsets[256];
					bool runsOn = true;
					for (u32 k = 0; k < count; ++k) {
						u32 offset = busReduce(bank << 16 | (addr + k), mask);
						if (size) {
							base = busMirror(base, size);
							offset = base + busMirror(offset, size - base);
						}
						offsets[k] = offset;
						runsOn = runsOn && offset == offsets[0] + k;
					}
					if (count == 256 && runsOn) {
						pages[bank << 8 | addr >> 8] = (struct Page) { .id = (u8) id, .mixed = false, .base = offsets[0] };
					} else {
						for (u32 k = 0; k < count; ++k) {
							setAddress(bank << 16 | (addr + k), (u8) id, offsets[k]);
						}
					}
					if (addrHi - addr < count) {
						break;
					}
					addr += count;
				}
			}
		}
	}
}

void busFinish(void) {
	building = false;
	u32 mixed = 0;
	for (u32 page = 0; page < 0x10000; ++page) {
		if (!pages[page].mixed) {
			continue;
		}
		const u8* ids = mixedPages[pages[page].base].id;
		const u32* targets = mixedPages[pages[page].base].target;
		bool uniform = true;
		for (u32 n = 1; n < 256 && uniform; ++n) {
			uniform = ids[n] == ids[0] && targets[n] == targets[0] + n;
		}
		if (uniform) {
			pages[page] = (struct Page) { .id = ids[0], .mixed = false, .base = targets[0] };
		} else {
			++mixed;
		}
	}
	// Only the pages still mixed are kept, in the pages' order.
	struct MixedPage* kept = calloc(mixed ? mixed : 1, sizeof(*mixedPages));
	u32 keptCount = 0;
	for (u32 page = 0; page < 0x10000; ++page) {
		if (pages[page].mixed) {
			kept[keptCount] = mixedPages[pages[page].base];
			pages[page].base = keptCount++;
		}
	}
	free(mixedPages);
	mixedPages = kept;
	mixedCount = mixedSpace = keptCount;
	for (u32 page = 0; page < 0x10000; ++page) {
		busDirectPages[page] = directFor(&pages[page]);
		busFastPages[page] = fastFor(page, &pages[page]);
	}
	cheatHook();
}

// Fast mode: the pages of a reader whose memory depends on the cartridge's own registers, from map
// (null for a page that isn't plain memory), but for the interrupt vectors' page and cheats'; count
// pages from first.
void busFastMapPages(BusReader reader, const u8* (*map)(u32 target), u32 first, u32 count) {
	for (u32 page = first; page < first + count && page < 0x10000; ++page) {
		if (!pages[page].mixed && readers[pages[page].id] == reader && (page & 0x7fff) != 0x00ff) {
			busFastPages[page] = map(pages[page].base);
		}
	}
}

void busFastMap(BusReader reader, const u8* (*map)(u32 target)) {
	busFastMapPages(reader, map, 0, 0x10000);
}

u8 busRead(u32 address, u8 data) {
	// The low RAM's mirrors.
	if (!(address & 0x40e000)) {
		address = 0x7e0000 | (address & 0x1fff);
	}
	const u8* memory = busDirectPages[address >> 8];
	if (memory) {
		return memory[address & 0xff];
	}
	const struct Page* page = &pages[address >> 8];
	if (!page->mixed) {
		return readers[page->id](page->base + (address & 0xff), data);
	}
	const struct MixedPage* mixed = &mixedPages[page->base];
	return readers[mixed->id[address & 0xff]](mixed->target[address & 0xff], data);
}

static u8 readCheat(u32 address, u8 data) {
	for (u32 i = 0; i < hookedCount; ++i) {
		if (hooked[i].page == address >> 8) {
			const struct Page* page = &hooked[i].was;
			if (!page->mixed) {
				data = readers[page->id](page->base + (address & 0xff), data);
			} else {
				const struct MixedPage* mixed = &mixedPages[page->base];
				data = readers[mixed->id[address & 0xff]](mixed->target[address & 0xff], data);
			}
			break;
		}
	}
	return cheatRead(address, data);
}

void busCheat(u32 page) {
	page &= 0xffff;
	if (pages[page].id == CHEAT_HANDLER && !pages[page].mixed) {
		return;
	}
	if (hookedCount == CHEAT_PAGES) {
		return;
	}
	hooked[hookedCount].page = page;
	hooked[hookedCount].was = pages[page];
	busDirectPages[page] = NULL;
	busFastPages[page] = NULL;
	// Fast mode reads the low RAM's mirrors through their own pages: they go to the cheats too.
	if ((page & 0xffe0) == 0x7e00) {
		for (u32 bank = 0; bank < 0x100; ++bank) {
			if (!(bank & 0x40)) {
				busFastPages[bank << 8 | (page & 0x1f)] = NULL;
			}
		}
	}
	++hookedCount;
	readers[CHEAT_HANDLER] = readCheat;
	pages[page] = (struct Page) { .id = CHEAT_HANDLER, .mixed = false, .base = page << 8 };
}

void busCheatsOff(void) {
	while (hookedCount) {
		--hookedCount;
		pages[hooked[hookedCount].page] = hooked[hookedCount].was;
		busDirectPages[hooked[hookedCount].page] = directFor(&hooked[hookedCount].was);
		busFastPages[hooked[hookedCount].page] = fastFor(hooked[hookedCount].page, &hooked[hookedCount].was);
		if ((hooked[hookedCount].page & 0xffe0) == 0x7e00) {
			for (u32 bank = 0; bank < 0x100; ++bank) {
				if (!(bank & 0x40)) {
					u32 mirror = bank << 8 | (hooked[hookedCount].page & 0x1f);
					busFastPages[mirror] = fastFor(mirror, &pages[mirror]);
				}
			}
		}
	}
}

void busWrite(u32 address, u8 data) {
	const struct Page* page = &pages[address >> 8];
	if (!page->mixed) {
		writers[page->id](page->base + (address & 0xff), data);
		return;
	}
	const struct MixedPage* mixed = &mixedPages[page->base];
	writers[mixed->id[address & 0xff]](mixed->target[address & 0xff], data);
}
