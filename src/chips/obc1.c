/* Copyright (c) 2004-2025 ares team, Near et al
 * Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Ported from ares (ISC); see LICENSES/ISC-ares.txt. */
#include "../snes.h"

// The OBC1: sprite attribute helper over the cartridge's 8 KiB of save RAM.

struct Obc1 obc1;

static u8 ramRead(u32 address) {
	return cart.ramSize ? cart.ram[busMirror(address & 0x1fff, cart.ramSize)] : 0xff;
}

static void ramWrite(u32 address, u8 data) {
	if (cart.ramSize) {
		cart.ram[busMirror(address & 0x1fff, cart.ramSize)] = data;
	}
}

void obc1Power(void) {
	obc1.baseptr = (ramRead(0x1ff5) & 1) ? 0x1800 : 0x1c00;
	obc1.address = ramRead(0x1ff6) & 0x7f;
	obc1.shift = (ramRead(0x1ff6) & 3) << 1;
}

u8 obc1Read(u32 address, u8 data) {
	(void) data;
	address &= 0x1fff;
	switch (address) {
	case 0x1ff0:
	case 0x1ff1:
	case 0x1ff2:
	case 0x1ff3:
		return ramRead(obc1.baseptr + (obc1.address << 2) + (address & 3));
	case 0x1ff4:
		return ramRead(obc1.baseptr + (obc1.address >> 2) + 0x200);
	}
	return ramRead(address);
}

void obc1Write(u32 address, u8 data) {
	address &= 0x1fff;
	switch (address) {
	case 0x1ff0:
	case 0x1ff1:
	case 0x1ff2:
	case 0x1ff3:
		ramWrite(obc1.baseptr + (obc1.address << 2) + (address & 3), data);
		return;
	case 0x1ff4: {
		u8 temp = ramRead(obc1.baseptr + (obc1.address >> 2) + 0x200);
		temp = (temp & ~(3 << obc1.shift)) | ((data & 3) << obc1.shift);
		ramWrite(obc1.baseptr + (obc1.address >> 2) + 0x200, temp);
		return;
	}
	case 0x1ff5:
		obc1.baseptr = (data & 1) ? 0x1800 : 0x1c00;
		ramWrite(address, data);
		return;
	case 0x1ff6:
		obc1.address = data & 0x7f;
		obc1.shift = (data & 3) << 1;
		ramWrite(address, data);
		return;
	}
	ramWrite(address, data);
}
