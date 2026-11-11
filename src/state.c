/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "snes.h"

// A saved state is the machine and nothing else: the processors' structs as they are, the cartridge's
// RAM, the part of the next frame already drawn, and a header that says which build's layout they are
// in. Every struct starts out zeroed and
// only its fields are ever written, so its padding is always zero and the same machine makes the same
// state on any CPU.
//
// A state is taken between frames. The canvas is usually clear then, but not after a DMA that ran on
// into the next frame.

enum { STATE_MAGIC = 0x534e5341, STATE_VERSION = 6 };

struct StateHeader {
	u32 magic;
	u32 version;
	u32 sizes[12];
	u32 ramSize;
	u32 reserved;
};

static void layout(u32 sizes[12]) {
	sizes[0] = sizeof(sys);
	sizes[1] = sizeof(cpu);
	sizes[2] = sizeof(smp);
	sizes[3] = sizeof(dsp);
	sizes[4] = sizeof(ppu);
	sizes[5] = sizeof(input);
	sizes[6] = sizeof(necdsp);
	sizes[7] = sizeof(sa1);
	sizes[8] = sizeof(gsu);
	sizes[9] = sizeof(sdd1);
	sizes[10] = sizeof(obc1);
	sizes[11] = sizeof(cx4);
}

size_t stateSize(void) {
	return sizeof(struct StateHeader) + sizeof(sys) + sizeof(cpu) + sizeof(smp) + sizeof(dsp) + sizeof(ppu) + sizeof(input) +
		sizeof(necdsp) + sizeof(sa1) + sizeof(gsu) + sizeof(sdd1) + sizeof(obc1) + sizeof(cx4) + cart.ramSize + sizeof(ppuCanvas);
}

#define PUT(object) \
	do { \
		memcpy(out, &(object), sizeof(object)); \
		out += sizeof(object); \
	} while (0)
#define GET(object) \
	do { \
		memcpy(&(object), in, sizeof(object)); \
		in += sizeof(object); \
	} while (0)

bool stateSave(void* data, size_t size) {
	if (size < stateSize()) {
		return false;
	}
	struct StateHeader header = { .magic = STATE_MAGIC, .version = STATE_VERSION, .ramSize = cart.ramSize };
	layout(header.sizes);

	u8* out = data;
	PUT(header);
	PUT(sys);
	PUT(cpu);
	PUT(smp);
	PUT(dsp);
	PUT(ppu);
	PUT(input);
	PUT(necdsp);
	PUT(sa1);
	PUT(gsu);
	PUT(sdd1);
	PUT(obc1);
	PUT(cx4);
	memcpy(out, cart.ram, cart.ramSize);
	out += cart.ramSize;
	memcpy(out, ppuCanvas, ppu.drawnTo * sizeof(u32));
	out += ppu.drawnTo * sizeof(u32);
	memset(out, 0, size - (out - (u8*) data));
	return true;
}

bool stateLoad(const void* data, size_t size) {
	struct StateHeader header;
	if (size < sizeof(header)) {
		return false;
	}
	memcpy(&header, data, sizeof(header));

	u32 sizes[12];
	layout(sizes);
	if (header.magic != STATE_MAGIC || header.version != STATE_VERSION || memcmp(header.sizes, sizes, sizeof(sizes)) ||
	    header.ramSize != cart.ramSize || size < stateSize()) {
		return false;
	}

	// Whether the machine is PAL comes from the cartridge, which a state can't change.
	bool pal = sys.pal;

	const u8* in = (const u8*) data + sizeof(header);
	GET(sys);
	GET(cpu);
	GET(smp);
	GET(dsp);
	GET(ppu);
	GET(input);
	GET(necdsp);
	GET(sa1);
	GET(gsu);
	GET(sdd1);
	GET(obc1);
	GET(cx4);
	memcpy(cart.ram, in, cart.ramSize);
	in += cart.ramSize;

	sys.pal = pal;
	if (ppu.drawnTo > CANVAS_WIDTH * CANVAS_HEIGHT) {
		ppu.drawnTo = 0;
	}
	memset(ppuCanvas, 0, sizeof(ppuCanvas));
	memcpy(ppuCanvas, in, ppu.drawnTo * sizeof(u32));
	return true;
}
