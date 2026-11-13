/* Copyright (c) Fabio Priuli, byuu (MAME's bus/snes/rom.cpp)
 * Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Ported from MAME; the Tekken 2 chip as d4s and nocash worked it out. */
#include "../snes.h"

// The protection chips and bank switches of pirate cartridges, as MAME has them.

struct Pirate pirate;

#define P pirate

void pirateLoad(int board) {
	P.board = board;
}

void piratePower(void) {
	P.latch = 0;
	P.prot = 0;
	// The 20-in-1 starts at its menu.
	P.baseBank = 4;
}

// A byte written comes back with its bits moved round: banks 80, 88, 90 and on to f8.
u8 pirateLatchRead(u32 address, u8 data) {
	(void) address;
	(void) data;
	u8 l = P.latch;
	return (l & 1) << 7 | (l >> 6 & 1) << 6 | (l >> 7 & 1) << 5 | (l >> 1 & 1) << 4 | (l >> 2 & 1) << 3 | (l >> 3 & 1) << 2 |
	       (l >> 4 & 1) << 1 | (l >> 5 & 1);
}

void pirateLatchWrite(u32 address, u8 data) {
	(void) address;
	P.latch = data;
}

// Tekken 2's: reads and writes alike of 80-bf:8000-87ff build a value by their address lines A8-A10,
// and a read at x1xx gives it back shifted or stepped.
static void tekkenUpdate(u32 address) {
	switch (address & 0x700) {
	case 0x000: P.prot = 0; break;
	case 0x100: break;
	case 0x200: P.prot |= 1; break;
	case 0x300: P.prot |= 2; break;
	case 0x400: P.prot |= 4; break;
	case 0x500: P.prot |= 8; break;
	// The direction, then the function.
	case 0x600: P.prot |= 0x10; break;
	case 0x700: P.prot |= 0x20; break;
	}
}

u8 pirateTekkenRead(u32 address, u8 data) {
	tekkenUpdate(address);
	if ((address & 0x700) != 0x100) {
		return data;
	}
	u8 value = P.prot & 15;
	if (P.prot & 0x20) {
		return P.prot & 0x10 ? value >> 1 : value << 1;
	}
	return P.prot & 0x10 ? value - 1 : value + 1;
}

void pirateTekkenWrite(u32 address, u8 data) {
	(void) data;
	tekkenUpdate(address);
}

// The Korean 20-in-1: a write to 00:8000-8fff picks the game's first 32 KiB bank in bits 0-4; bit 7
// without bit 6 is a game of one bank, seen in every bank.
u8 pirate20Read(u32 address, u8 data) {
	(void) data;
	bool single = !(P.baseBank & 0x40) && (P.baseBank & 0x80);
	u32 bank = single ? 0 : address >> 16 & 0x7f;
	u32 offset = ((P.baseBank & 0x1f) + bank) * 0x8000 + (address & 0x7fff);
	return cart.rom[busMirror(offset, cart.romSize)];
}

void pirate20Write(u32 address, u8 data) {
	if ((address & 0xfff000) == 0x008000) {
		P.baseBank = data & 0xdf;
	}
}
