/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ARMSNES_FASTPPU_H
#define ARMSNES_FASTPPU_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Fast mode's line renderer (fastppu.cpp, from Mednafen's snes_faust): one whole line from the
// registers as the S-PPU's ports would have them written, VRAM and CGRAM, and the line's sprite tiles.
struct FastPpuSprite {
	int16_t x;
	uint8_t priority;  // 0-3
	uint8_t palette;   // the first CGRAM entry
	bool hflip;
	uint32_t data;     // the four planes, eight bits each
};

struct FastPpuLine {
	const uint16_t* vram;   // 32768 words
	const uint16_t* cgram;  // 256 colors
	uint32_t* out;          // 512 canvas pixels, or NULL
	uint32_t brightness;    // in the canvas's place for it
	bool blank;
	uint16_t y;
	uint8_t field;          // the interlace field

	uint8_t bgMode;         // $2105
	uint8_t mosaic;         // $2106
	uint8_t mosaicYOffset;
	uint8_t bgsc[4];        // $2107-$210a
	uint8_t bgnba[2];       // $210b-$210c
	uint16_t bghofs[4], bgvofs[4];
	uint8_t m7sel;          // $211a
	int16_t m7Matrix[4], m7Center[2], m7hofs, m7vofs;
	uint8_t wmSettings[3];  // $2123-$2125
	uint8_t windowPos[2][2];
	uint16_t wmLogic;       // $212a-$212b
	uint8_t msEnable, ssEnable, wmMainEnable, wmSubEnable;  // $212c-$212f
	uint8_t cgwsel, cgadsub;
	uint16_t fixedColor;
	uint8_t screenMode;     // $2133
	bool hiresFirstBlack;   // the first column's sub screen pixel is black

	uint32_t spriteCount;
	struct FastPpuSprite sprites[34];
};

void fastPpuLine(const struct FastPpuLine* line);

// The same, on a thread of its own when threaded is set: the line is drawn later, from the thread's
// copies of VRAM and CGRAM, which see every write in order. Fence waits for every line given to be
// drawn; Resync gives the copies all of VRAM and CGRAM again. The picture is the same either way.
// Next gives the place to fill the line in, Submit draws it or gives it to the thread.
struct FastPpuLine* fastPpuNext(bool threaded);
void fastPpuSubmit(struct FastPpuLine* line);
void fastPpuVram(uint32_t address, uint16_t data);
void fastPpuCgram(uint32_t address, uint16_t data);
void fastPpuFence(void);
void fastPpuResync(const uint16_t* vram, const uint16_t* cgram);
// The thread drawn out and ended; the next line given starts it again.
void fastPpuStop(void);
// How many render threads to use: 0 for as many as the cores allow.
void fastPpuThreads(unsigned count);
// Called with each line drawn, on the thread that drew it: the canvas place it is for, and its pixels.
// With a sink, the canvas isn't written: the pixels are in a line of the renderer's own until it
// returns.
void fastPpuSetSink(void (*sink)(uint32_t* at, const uint32_t* pixels));
bool fastPpuDirect(void);
// The cores this process may run on (its affinity), else those online.
unsigned fastPpuCores(void);

#ifdef __cplusplus
}
#endif

#endif
