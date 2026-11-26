/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ARMSNES_SGB_H
#define ARMSNES_SGB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The Super Game Boy's Game Boy (sgb.c, on armGB).

bool sgbLoad(const unsigned char* rom, size_t size, const unsigned char* boot, size_t bootSize);
void sgbUnload(void);
bool sgbLoaded(void);
void sgbReset(void);
// Runs about that many dots (4 MiHz clocks); says how many it ran. Its sound comes out as sample
// pairs, left then right, one pair for every 128 dots.
unsigned sgbRun(unsigned dots, unsigned* samples, const int16_t** audio);
unsigned char* sgbSaveRam(unsigned* size);
// The cartridge's clock (MBC3 or HuC3) as armGBA's .rtc file, whose parts gambatte-libretro and mGBA
// read too: the frontend fills it after the game is loaded, sgbRtcRead sets the clock from it, moved
// on by the time the game was off, and sgbRtcWrite brings it up to date. NULL without a clock.
unsigned char* sgbRtc(size_t* size);
void sgbRtcRead(void);
void sgbRtcWrite(void);
size_t sgbStateSize(void);
void sgbStateSave(void* data);
bool sgbStateLoad(const void* data, size_t size);

// The ICD2's, which the Game Boy calls: a JOYP write, which gives back the four input bits, the same
// bits for a read, and a line's shades (0 to 3) as they are done.
unsigned icdJoypWrite(unsigned p14, unsigned p15);
unsigned icdJoypRead(void);
void icdLine(unsigned ly, const uint16_t* shades);

#endif
