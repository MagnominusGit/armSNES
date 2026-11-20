/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ARMSNES_GB_H
#define ARMSNES_GB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The Super Game Boy's Game Boy (gb.cpp, on Gambatte).

#define GB_MAX_DOTS 4096

bool gbLoad(const unsigned char* rom, size_t size, const unsigned char* boot, size_t bootSize);
void gbUnload(void);
bool gbLoaded(void);
void gbReset(void);
// Runs about that many dots (4 MiHz clocks); says how many it ran. Its sound comes out as samples,
// one for every two dots: left in the low 16 bits, right in the high.
unsigned gbRun(unsigned dots, unsigned* samples, const unsigned** audio);
unsigned char* gbSaveRam(unsigned* size);
// The cartridge's clock (MBC3 or HuC3) as armGBA's .rtc file, whose parts gambatte-libretro and mGBA
// read too: the frontend fills it after the game is loaded, gbRtcRead sets the clock from it, moved on
// by the time the game was off, and gbRtcWrite brings it up to date. NULL without a clock.
unsigned char* gbRtc(size_t* size);
void gbRtcRead(void);
void gbRtcWrite(void);
size_t gbStateSize(void);
void gbStateSave(void* data);
bool gbStateLoad(const void* data, size_t size);

// The ICD2's, which the Game Boy calls: a JOYP write, which gives back the four input bits, and a
// line's pixels as they are done.
unsigned icdJoypWrite(unsigned p14, unsigned p15);
void icdLine(unsigned ly, const uint32_t* pixels);

#endif
