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
static struct MixedPage* mixedPages;
static u32 mixedCount;
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

// While a machine is put together, every address has its own entry, as in ares.
static u8* lookup;
static u32* target;

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
	free(lookup);
	free(target);
	free(mixedPages);
	mixedPages = NULL;
	mixedCount = 0;
	lookup = calloc(1 << 24, sizeof(*lookup));
	target = calloc(1 << 24, sizeof(*target));
	memset(readers, 0, sizeof(readers));
	memset(writers, 0, sizeof(writers));
	readers[0] = readOpen;
	writers[0] = writeOpen;
	handlers = 1;
	hookedCount = 0;
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

void busMap(BusReader reader, BusWriter writer, const char* address, u32 size, u32 base, u32 mask) {
	if (handlers >= CHEAT_HANDLER || !lookup) {
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
				for (u32 addr = addrLo; addr <= addrHi; ++addr) {
					u32 offset = busReduce(bank << 16 | addr, mask);
					if (size) {
						base = busMirror(base, size);
						offset = base + busMirror(offset, size - base);
					}
					lookup[bank << 16 | addr] = id;
					target[bank << 16 | addr] = offset;
				}
			}
		}
	}
}

void busFinish(void) {
	u32 mixed = 0;
	for (int pass = 0; pass < 2; ++pass) {
		for (u32 page = 0; page < 0x10000; ++page) {
			const u8* ids = &lookup[page << 8];
			const u32* targets = &target[page << 8];
			bool uniform = true;
			for (u32 n = 1; n < 256 && uniform; ++n) {
				uniform = ids[n] == ids[0] && targets[n] == targets[0] + n;
			}
			if (uniform) {
				pages[page] = (struct Page) { .id = ids[0], .mixed = false, .base = targets[0] };
			} else if (pass == 0) {
				++mixed;
			} else {
				pages[page] = (struct Page) { .id = 0, .mixed = true, .base = mixedCount };
				memcpy(mixedPages[mixedCount].id, ids, 256);
				memcpy(mixedPages[mixedCount].target, targets, 256 * sizeof(u32));
				++mixedCount;
			}
		}
		if (pass == 0) {
			mixedPages = calloc(mixed ? mixed : 1, sizeof(*mixedPages));
		}
	}
	free(lookup);
	free(target);
	lookup = NULL;
	target = NULL;
	cheatHook();
}

u8 busRead(u32 address, u8 data) {
	// The low RAM's mirrors.
	if (!(address & 0x40e000)) {
		address = 0x7e0000 | (address & 0x1fff);
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
	++hookedCount;
	readers[CHEAT_HANDLER] = readCheat;
	pages[page] = (struct Page) { .id = CHEAT_HANDLER, .mixed = false, .base = page << 8 };
}

void busCheatsOff(void) {
	while (hookedCount) {
		--hookedCount;
		pages[hooked[hookedCount].page] = hooked[hookedCount].was;
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
