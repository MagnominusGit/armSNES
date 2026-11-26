/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Licensed under the GNU General Public License, version 2 or (at your option) any later version: see LICENSE.
 *
 * The STAT logic and the line timing follow SameBoy (Copyright (c) Lior Halphon), under the Expat (MIT) license:
 * see LICENSES/SameBoy.txt. */
#include "gb.h"

#include <string.h>

// The PPU (Pan Docs, "Rendering" and "STAT"). A line is 456 dots: OAM scan (mode 2) for 80, drawing
// (mode 3) for 172 dots or more, then H-blank (mode 0); lines 144 to 153 are V-blank (mode 1). The
// PPU moves from step to step by an event, and draws a whole line where mode 3 ends.
//
// When within a line LY, STAT's mode and the LY=LYC comparison change, and when the STAT interrupt
// line sees them, follow SameBoy's PPU (Lior Halphon, MIT; Core/display.c), which measured them.

enum {
	// Lines 0 to 143.
	STEP_OAM_WRITE,
	STEP_LY,
	STEP_OAM,
	STEP_OAM_LATE,
	STEP_MODE3,
	STEP_DRAW,
	STEP_MODE0,
	STEP_HBLANK_INT,
	STEP_PRE_OAM,
	STEP_LINE_END,
	// The first line after the LCD is switched on.
	STEP_FIRST,
	STEP_FIRST_OAM_WRITE,
	STEP_FIRST_MODE3,
	STEP_FIRST_VRAM,
	// Lines 144 to 152.
	STEP_V_START,
	STEP_V_LY,
	STEP_V_COMPARE,
	STEP_V_BLANK,
	// Line 153.
	STEP_L153_LY,
	STEP_L153_ZERO,
	STEP_L153_LYC0,
	STEP_L153_COMPARE,
};

enum {
	LCDC_BG = 0x01,
	LCDC_OBJ = 0x02,
	LCDC_TALL = 0x04,
	LCDC_BG_MAP = 0x08,
	LCDC_TILES = 0x10,
	LCDC_WINDOW = 0x20,
	LCDC_WINDOW_MAP = 0x40,
	LCDC_ON = 0x80,
};

// The objects on the line, in OAM order: at most ten.
static void findObjects(struct GB* gb) {
	struct GBPpu* ppu = &gb->ppu;
	uint64_t found = gbFindObjects(gb->oam, ppu->line, ppu->lcdc & LCDC_TALL ? 16 : 8);
	int count = 0;
	for (; found && count < 10; found &= found - 1) {
		ppu->objects[count++] = __builtin_ctzll(found);
	}
	ppu->objectCount = count;
}

// The window shows from the line where WY matches LY with the window on, for the rest of the frame
// (SameBoy's wy_check): checked as each line's OAM scan starts, and when WY or LCDC is written.
static void checkWindow(struct GB* gb) {
	struct GBPpu* ppu = &gb->ppu;
	if (!(ppu->lcdc & LCDC_ON) || !(ppu->lcdc & LCDC_WINDOW)) {
		return;
	}
	int line = (!gbColor(gb) || gb->doubleSpeed) && ppu->lyCompare >= 0 ? ppu->lyCompare : ppu->line;
	if (ppu->wy == line) {
		ppu->windowSeen = true;
	}
}

static bool windowOnLine(const struct GB* gb) {
	const struct GBPpu* ppu = &gb->ppu;
	return (ppu->lcdc & LCDC_WINDOW) && (gb->cgbMode || (ppu->lcdc & LCDC_BG)) && ppu->windowSeen && ppu->wx <= 166;
}

// How long mode 3 lasts: 172 dots, the fine scroll's, 6 for the window, and each object's penalty
// (Pan Docs, "Mode 3 length").
static unsigned drawLength(struct GB* gb) {
	struct GBPpu* ppu = &gb->ppu;
	findObjects(gb);
	unsigned length = 172 + (ppu->scx & 7);
	// The window is fetched, and takes its 6 dots, even where it isn't shown (the original's background
	// off): up to WX 165 on the original, 166 on the Color; a dot more at WX 0 with a fine scroll (AGE's
	// stat-mode-window tests).
	bool window = (ppu->lcdc & LCDC_WINDOW) && ppu->windowSeen && ppu->wx <= (gbColor(gb) ? 166 : 165);
	if (window) {
		length += 6;
		if (!ppu->wx && (ppu->scx & 7)) {
			++length;
		}
	}
	if (!(ppu->lcdc & LCDC_OBJ) && !gb->cgbMode) {
		return length;
	}
	// The objects are reached from left to right: the first in a background tile waits for its fetch.
	uint8_t xs[10];
	int count = ppu->objectCount;
	for (int i = 0; i < count; ++i) {
		uint8_t x = gb->oam[ppu->objects[i] * 4 + 1];
		int j = i;
		for (; j > 0 && xs[j - 1] > x; --j) {
			xs[j] = xs[j - 1];
		}
		xs[j] = x;
	}
	// Background tiles 0-31, the window's from 32.
	bool tileSeen[64] = { false }, leftSeen = false;
	for (int i = 0; i < count; ++i) {
		int x = xs[i];
		if (x >= 168) {
			continue;
		}
		// Wholly off the left edge: the first waits 5 dots for the fetch, as if at its tile's start,
		// which then has no wait for the objects after it.
		if (x == 0) {
			length += leftSeen ? 6 : 11;
			leftSeen = true;
			tileSeen[((ppu->scx - 8) >> 3) & 31] = true;
			continue;
		}
		bool inWindow = window && x - 1 >= ppu->wx;
		int pixel = inWindow ? x - 1 - ppu->wx : x - 8 + ppu->scx;
		int tile = inWindow ? 32 + (pixel >> 3) : (pixel >> 3) & 31;
		if (!tileSeen[tile]) {
			tileSeen[tile] = true;
			int right = 7 - (pixel & 7) - 2;
			if (right > 0) {
				length += right;
			}
		}
		length += 6;
	}
	return length;
}

static uint16_t tileAddress(const struct GBPpu* ppu, uint8_t tile) {
	if (ppu->lcdc & LCDC_TILES) {
		return tile * 16;
	}
	return 0x1000 + (int8_t) tile * 16;
}

// A tile row's two bytes to eight colour indices, a byte each, leftmost first.
static uint64_t spread[256];

static void initSpread(void) {
	if (spread[1]) {
		return;
	}
	for (int b = 0; b < 256; ++b) {
		uint64_t v = 0;
		for (int i = 0; i < 8; ++i) {
			v |= (uint64_t) (b >> (7 - i) & 1) << (i * 8);
		}
		spread[b] = v;
	}
}

static inline uint64_t tileRow(const struct GB* gb, uint16_t address) {
	return spread[gb->vram[address]] | spread[gb->vram[address + 1]] << 1;
}

// Draws the line whole: background and window a tile at a time, then the objects.
static void drawLineDmg(struct GB* gb) {
	struct GBPpu* ppu = &gb->ppu;
	int y = ppu->line;
	// A Super Game Boy takes the lines of the first frame after the LCD is switched on, which the LCD
	// doesn't show.
	if (y >= GB_HEIGHT || (ppu->hiddenFrame && !gb->sgbLine)) {
		return;
	}
	initSpread();
	uint8_t index[GB_WIDTH + 16];
	uint16_t* out = gb->frame[y];
	// Taken first: the byte stores below could otherwise be rereading it.
	bool const hideWindow = gb->layersHidden & GB_LAYER_WINDOW;
	if (ppu->lcdc & LCDC_BG) {
		uint8_t row[21 * 8];
		uint16_t map = (ppu->lcdc & LCDC_BG_MAP ? 0x1C00 : 0x1800) + ((ppu->scy + y) & 0xFF) / 8 * 32;
		unsigned fineY = (ppu->scy + y) & 7;
		unsigned column = ppu->scx >> 3;
		for (int t = 0; t < 21; ++t) {
			uint8_t tile = gb->vram[map + ((column + t) & 31)];
			uint64_t pixels = tileRow(gb, tileAddress(ppu, tile) + fineY * 2);
			memcpy(&row[t * 8], &pixels, 8);
		}
		memcpy(index, &row[ppu->scx & 7], GB_WIDTH);
		// Layer Selection: a hidden background is colour 0; a hidden window isn't drawn, though its
		// line still counts.
		if (gb->layersHidden & GB_LAYER_BG) {
			memset(index, 0, GB_WIDTH);
		}
		if (windowOnLine(gb)) {
			uint16_t wmap = (ppu->lcdc & LCDC_WINDOW_MAP ? 0x1C00 : 0x1800) + ppu->windowLine / 8 * 32;
			unsigned wfine = ppu->windowLine & 7;
			int start = ppu->wx - 7;
			int first = start < 0 ? 0 : start;
			for (int t = (first - start) / 8; !hideWindow && t * 8 + start < GB_WIDTH; ++t) {
				uint64_t pixels = tileRow(gb, tileAddress(ppu, gb->vram[wmap + t]) + wfine * 2);
				uint8_t tilePixels[8];
				memcpy(tilePixels, &pixels, 8);
				for (int i = 0; i < 8; ++i) {
					int x = t * 8 + i + start;
					if (x >= first && x < GB_WIDTH) {
						index[x] = tilePixels[i];
					}
				}
			}
			++ppu->windowLine;
		}
	} else {
		memset(index, 0, GB_WIDTH);
	}
	// With a frontend's picture, its colors go there; otherwise the shades go to the frame.
	uint32_t* video = gb->video ? gb->video + y * GB_WIDTH : NULL;
	if (video) {
		uint32_t lut[4];
		for (int i = 0; i < 4; ++i) {
			lut[i] = gb->videoColors[ppu->bgp >> (i * 2) & 3];
		}
		gbShadeLine(video, index, lut, GB_WIDTH);
	} else {
		uint16_t lut[4] = { ppu->bgp & 3, ppu->bgp >> 2 & 3, ppu->bgp >> 4 & 3, ppu->bgp >> 6 & 3 };
		for (int x = 0; x < GB_WIDTH; ++x) {
			out[x] = lut[index[x]];
		}
	}
	if (!(ppu->lcdc & LCDC_OBJ) || (gb->layersHidden & GB_LAYER_OBJ)) {
		return;
	}
	int height = ppu->lcdc & LCDC_TALL ? 16 : 8;
	// Which object owns each pixel: the one with the smallest X, then the first in OAM.
	uint8_t ownerX[GB_WIDTH];
	memset(ownerX, 0xFF, sizeof(ownerX));
	for (int i = 0; i < ppu->objectCount; ++i) {
		const uint8_t* object = &gb->oam[ppu->objects[i] * 4];
		int left = object[1] - 8;
		if (left <= -8 || left >= GB_WIDTH) {
			continue;
		}
		uint8_t flags = object[3];
		int line = y - (object[0] - 16);
		if (flags & 0x40) {
			line = height - 1 - line;
		}
		uint8_t tile = height == 16 ? object[2] & 0xFE : object[2];
		uint64_t pixels = tileRow(gb, tile * 16 + line * 2);
		uint8_t colors[8];
		memcpy(colors, &pixels, 8);
		uint8_t palette = flags & 0x10 ? ppu->obp1 : ppu->obp0;
		for (int px = 0; px < 8; ++px) {
			int x = left + px;
			if (x < 0 || x >= GB_WIDTH || ownerX[x] <= object[1]) {
				continue;
			}
			int color = colors[flags & 0x20 ? 7 - px : px];
			if (!color) {
				continue;
			}
			ownerX[x] = object[1];
			if ((flags & 0x80) && index[x]) {
				continue;
			}
			if (video) {
				video[x] = gb->videoColors[palette >> (color * 2) & 3];
			} else {
				out[x] = palette >> (color * 2) & 3;
			}
		}
	}
}

// A tile row's eight colour indices, flipped left to right if asked.
static inline uint64_t flipRow(uint64_t pixels, bool flip) {
	return flip ? __builtin_bswap64(pixels) : pixels;
}

// The Game Boy Color's line. A Color game: tiles with attributes (palette, VRAM bank, flips, priority
// over objects) and colour palettes; LCDC bit 0 only takes the background's priority away. An
// original Game Boy game on a Color: drawn as on the original, its shades through the colour
// palettes the boot ROM left.
// A palette entry as the frame stores it: 15-bit RGB, or the frontend's colour.
static inline uint32_t paletteColor(const struct GB* gb, const uint8_t* palettes, unsigned entry) {
	uint16_t rgb = palettes[entry * 2] | (palettes[entry * 2 + 1] & 0x7F) << 8;
	return gb->video && gb->videoTable ? gb->videoTable[rgb] : rgb;
}

// Eight palettes' colours for a line: a Color game's own, or for an original Game Boy game palette 0
// (or 0 and 1 for objects) through its BGP, OBP0 and OBP1 shades.
static void lineColors(const struct GB* gb, const uint8_t* palettes, int count, const uint8_t* shades, uint32_t colors[8][4]) {
	// With no boot ROM to choose colors, the frontend's shades.
	bool const own = shades && gb->cgb.compatShades && gb->video;
	for (int p = 0; p < count; ++p) {
		for (int i = 0; i < 4; ++i) {
			unsigned shade = shades ? shades[p] >> (i * 2) & 3 : (unsigned) i;
			colors[p][i] = own ? gb->videoColors[shade] : paletteColor(gb, palettes, p * 4 + shade);
		}
	}
}

static void drawLineCgb(struct GB* gb) {
	struct GBPpu* ppu = &gb->ppu;
	struct GBCgb* cgb = &gb->cgb;
	int y = ppu->line;
	if (y >= GB_HEIGHT || ppu->hiddenFrame) {
		return;
	}
	initSpread();
	bool const color = gb->cgbMode;
	// The line from the first tile's left edge: each pixel's colour, and its colour index with bit 2
	// set where the tile is in front of objects.
	uint32_t line[21 * 8];
	uint8_t info[21 * 8];
	unsigned const o = ppu->scx & 7;
	// A Color game's palettes change rarely: built once until they do. An original game's go through
	// BGP, OBP0 and OBP1, which change often, so they're built for each line.
	uint32_t compatBg[8][4];
	uint8_t const bgShades[1] = { ppu->bgp };
	if (color && !gb->colorsBuilt) {
		lineColors(gb, cgb->bgPalette, 8, NULL, gb->bgColors);
		lineColors(gb, cgb->objPalette, 8, NULL, gb->objColors);
		gb->colorsBuilt = true;
	} else if (!color) {
		lineColors(gb, cgb->bgPalette, 1, bgShades, compatBg);
	}
	uint32_t (*const bg)[4] = color ? gb->bgColors : compatBg;
	bool const hideBg = gb->layersHidden & GB_LAYER_BG, hideWindow = gb->layersHidden & GB_LAYER_WINDOW;
	if (color || (ppu->lcdc & LCDC_BG)) {
		uint16_t map = (ppu->lcdc & LCDC_BG_MAP ? 0x1C00 : 0x1800) + ((ppu->scy + y) & 0xFF) / 8 * 32;
		unsigned fineY = (ppu->scy + y) & 7;
		unsigned column = ppu->scx >> 3;
		uint64_t pixels[21];
		const uint32_t* colors[21];
		uint8_t front[21];
		for (int t = 0; t < 21; ++t) {
			uint16_t at = map + ((column + t) & 31);
			uint8_t tile = gb->vram[at];
			uint8_t a = color ? gb->vram[0x2000 + at] : 0;
			unsigned row = a & 0x40 ? 7 - fineY : fineY;
			pixels[t] = flipRow(tileRow(gb, (a & 0x08 ? 0x2000 : 0) + tileAddress(ppu, tile) + row * 2), a & 0x20);
			colors[t] = bg[a & 7];
			front[t] = a & 0x80 ? 4 : 0;
		}
		gbExpandTiles(line, info, pixels, colors, front, 21);
		// Layer Selection: a hidden background is colour 0 of each tile's palette, drawn over
		// afterwards so the loop above stays as it is.
		for (int t = 0; hideBg && t < 21; ++t) {
			uint8_t a = color ? gb->vram[0x2000 + map + ((column + t) & 31)] : 0;
			for (int i = 0; i < 8; ++i) {
				line[t * 8 + i] = bg[a & 7][0];
				info[t * 8 + i] = a & 0x80 ? 4 : 0;
			}
		}
		if (windowOnLine(gb)) {
			uint16_t wmap = (ppu->lcdc & LCDC_WINDOW_MAP ? 0x1C00 : 0x1800) + ppu->windowLine / 8 * 32;
			unsigned wfine = ppu->windowLine & 7;
			int start = ppu->wx - 7;
			int first = start < 0 ? 0 : start;
			// The window's tiles from its first one on screen, drawn apart and copied in from where
			// it starts.
			int firstTile = (first - start) / 8;
			int tiles = 0;
			uint64_t pixels[21];
			const uint32_t* colors[21];
			uint8_t front[21];
			for (int t = firstTile; !hideWindow && t * 8 + start < GB_WIDTH; ++t, ++tiles) {
				uint8_t tile = gb->vram[wmap + t];
				uint8_t a = color ? gb->vram[0x2000 + wmap + t] : 0;
				unsigned row = a & 0x40 ? 7 - wfine : wfine;
				pixels[tiles] = flipRow(tileRow(gb, (a & 0x08 ? 0x2000 : 0) + tileAddress(ppu, tile) + row * 2), a & 0x20);
				colors[tiles] = bg[a & 7];
				front[tiles] = a & 0x80 ? 4 : 0;
			}
			if (tiles) {
				uint32_t windowLine[21 * 8];
				uint8_t windowInfo[21 * 8];
				gbExpandTiles(windowLine, windowInfo, pixels, colors, front, tiles);
				int from = first - (firstTile * 8 + start);
				memcpy(&line[o + first], &windowLine[from], (GB_WIDTH - first) * sizeof(uint32_t));
				memcpy(&info[o + first], &windowInfo[from], GB_WIDTH - first);
			}
			++ppu->windowLine;
		}
	} else {
		for (int x = 0; x < GB_WIDTH; ++x) {
			line[o + x] = bg[0][0];
			info[o + x] = 0;
		}
	}
	if ((ppu->lcdc & LCDC_OBJ) && ppu->objectCount && !(gb->layersHidden & GB_LAYER_OBJ)) {
		uint32_t compatObj[8][4];
		uint8_t const objShades[2] = { ppu->obp0, ppu->obp1 };
		if (!color) {
			lineColors(gb, cgb->objPalette, 2, objShades, compatObj);
		}
		uint32_t (*const obj)[4] = color ? gb->objColors : compatObj;
		int height = ppu->lcdc & LCDC_TALL ? 16 : 8;
		// Which object has each pixel: by OAM position in a Color game (unless OPRI says by X), else
		// the smallest X, then the first in OAM.
		bool byPosition = color && !(cgb->opri & 1);
		uint8_t ownerX[GB_WIDTH];
		bool taken[GB_WIDTH];
		memset(ownerX, 0xFF, sizeof(ownerX));
		memset(taken, 0, sizeof(taken));
		for (int i = 0; i < ppu->objectCount; ++i) {
			const uint8_t* object = &gb->oam[ppu->objects[i] * 4];
			int left = object[1] - 8;
			if (left <= -8 || left >= GB_WIDTH) {
				continue;
			}
			uint8_t flags = object[3];
			int row = y - (object[0] - 16);
			if (flags & 0x40) {
				row = height - 1 - row;
			}
			uint8_t tile = height == 16 ? object[2] & 0xFE : object[2];
			uint16_t bank = color && (flags & 0x08) ? 0x2000 : 0;
			uint64_t pixels = flipRow(tileRow(gb, bank + tile * 16 + row * 2), flags & 0x20);
			uint8_t indices[8];
			memcpy(indices, &pixels, 8);
			const uint32_t* colors = obj[color ? flags & 7 : flags >> 4 & 1];
			for (int px = 0; px < 8; ++px) {
				int x = left + px;
				if (x < 0 || x >= GB_WIDTH || (byPosition ? taken[x] : ownerX[x] <= object[1])) {
					continue;
				}
				int c = indices[px];
				if (!c) {
					continue;
				}
				taken[x] = true;
				ownerX[x] = object[1];
				// Behind the background's colours 1 to 3: by the object's flag, or the tile's in a
				// Color game; LCDC bit 0 off in a Color game puts objects in front regardless.
				uint8_t under = info[o + x];
				bool behind = (under & 3) && ((flags & 0x80) || (under & 4));
				if (behind && !(color && !(ppu->lcdc & LCDC_BG))) {
					continue;
				}
				line[o + x] = colors[c];
			}
		}
	}
	if (gb->video && gb->videoTable) {
		memcpy(gb->video + y * GB_WIDTH, &line[o], GB_WIDTH * sizeof(uint32_t));
	} else {
		for (int x = 0; x < GB_WIDTH; ++x) {
			gb->frame[y][x] = line[o + x];
		}
	}
}

static void drawLine(struct GB* gb) {
	// A frame no one will see (Frame Skip, a frame run again): only the window's line count moves.
	if (gb->noDraw) {
		if (gb->ppu.line < GB_HEIGHT && !gb->ppu.hiddenFrame && windowOnLine(gb)) {
			++gb->ppu.windowLine;
		}
		return;
	}
	if (gbColor(gb)) {
		drawLineCgb(gb);
	} else {
		drawLineDmg(gb);
	}
}

// SameBoy's GB_STAT_update: the coincidence bit and the STAT interrupt line, from the comparison
// LY and the mode the interrupt goes by.
static void updateStat(struct GB* gb) {
	struct GBPpu* ppu = &gb->ppu;
	if (!(ppu->lcdc & LCDC_ON)) {
		return;
	}
	if (gb->dma.active && ppu->mode == 2) {
		ppu->mode = 0;
	}
	bool single = !gbColor(gb) || !gb->doubleSpeed;
	if (ppu->lyCompare >= 0 || single) {
		if (ppu->lyCompare == ppu->lyc) {
			ppu->lycLine = true;
			ppu->coincidence = true;
		} else {
			if (ppu->lyCompare >= 0) {
				ppu->lycLine = false;
			}
			ppu->coincidence = false;
		}
	}
	bool line;
	switch (ppu->modeForInterrupt) {
	case 0:
		line = ppu->stat & 0x08;
		break;
	case 1:
		line = ppu->stat & 0x10;
		break;
	case 2:
		line = ppu->stat & 0x20;
		break;
	default:
		line = false;
		break;
	}
	if ((ppu->stat & 0x40) && ppu->lycLine) {
		line = true;
	}
	if (line && !ppu->statLine) {
		gbRequest(gb, GB_IRQ_STAT);
	}
	ppu->statLine = line;
}

static void next(struct GB* gb, int step, uint32_t at) {
	struct GBPpu* ppu = &gb->ppu;
	ppu->step = step;
	ppu->nextAt = at;
}

// A new line starts, at time start.
static void beginLine(struct GB* gb, uint32_t start) {
	struct GBPpu* ppu = &gb->ppu;
	ppu->lineStart = start;
	ppu->lineEnd = start + GB_LINE_DOTS;
	if (ppu->line < GB_HEIGHT) {
		bool cgb = gbColor(gb);
		ppu->oamWriteBlocked = cgb && !gb->doubleSpeed;
		next(gb, STEP_OAM_WRITE, start + 2);
	} else {
		ppu->lyCompare = -1;
		updateStat(gb);
		next(gb, ppu->line == GB_LINES - 1 ? STEP_L153_LY : STEP_V_LY, start + 2);
	}
}

// Mode 3 has begun to fetch: the line is drawn as a whole where it ends.
static void startDrawing(struct GB* gb, uint32_t at) {
	struct GBPpu* ppu = &gb->ppu;
	ppu->drawStart = at;
	ppu->mode3Length = drawLength(gb);
	next(gb, STEP_MODE0, at + ppu->mode3Length - 5);
}

// Runs the step due now.
static void advance(struct GB* gb) {
	struct GBPpu* ppu = &gb->ppu;
	bool single = !gbColor(gb) || !gb->doubleSpeed;
	switch (ppu->step) {
	case STEP_OAM_WRITE:
		ppu->oamWriteBlocked = gbColor(gb);
		next(gb, STEP_LY, ppu->lineStart + 3);
		break;
	case STEP_LY:
		ppu->oamReadBlocked = true;
		ppu->ly = ppu->line;
		ppu->lyCompare = ppu->line ? -1 : 0;
		if (ppu->line) {
			ppu->modeForInterrupt = 2;
			ppu->mode = 0;
		} else if (!gbColor(gb)) {
			ppu->mode = 0;
		}
		updateStat(gb);
		next(gb, STEP_OAM, ppu->lineStart + 4);
		break;
	case STEP_OAM:
		ppu->oamReadBlocked = true;
		ppu->oamWriteBlocked = true;
		ppu->mode = 2;
		ppu->modeForInterrupt = 2;
		ppu->lyCompare = ppu->line;
		checkWindow(gb);
		updateStat(gb);
		ppu->modeForInterrupt = -1;
		updateStat(gb);
		next(gb, STEP_OAM_LATE, ppu->lineStart + 80);
		break;
	case STEP_OAM_LATE:
		// Near the scan's end, VRAM writes open for a moment, and reads close on the original.
		ppu->vramReadBlocked = !gbColor(gb);
		ppu->vramWriteBlocked = false;
		ppu->oamWriteBlocked = gbColor(gb);
		next(gb, STEP_MODE3, ppu->lineStart + 84);
		break;
	case STEP_MODE3:
		ppu->mode = 3;
		ppu->modeForInterrupt = 3;
		ppu->vramReadBlocked = true;
		ppu->vramWriteBlocked = true;
		ppu->oamReadBlocked = true;
		ppu->oamWriteBlocked = true;
		updateStat(gb);
		next(gb, STEP_DRAW, ppu->lineStart + 89);
		break;
	case STEP_DRAW:
		startDrawing(gb, ppu->nextAt);
		break;
	case STEP_MODE0:
		// Mode 3 ends: STAT shows mode 0 now, and its interrupt comes a dot later.
		drawLine(gb);
		if (gb->sgbLine && ppu->line < GB_HEIGHT) {
			gb->sgbLine(gb->user, ppu->line, gb->frame[ppu->line]);
		}
		if (gb->cgb.hdmaHblank) {
			gbHdmaHblank(gb, ppu->nextAt + 3);
		}
		if (single) {
			ppu->mode = 0;
			ppu->modeForInterrupt = 0;
			// The Color (E) keeps OAM from the CPU's reads a dot longer (SameBoy).
			ppu->oamReadBlocked = gbColor(gb);
			ppu->vramReadBlocked = false;
			ppu->oamWriteBlocked = false;
			ppu->vramWriteBlocked = false;
		}
		next(gb, STEP_HBLANK_INT, ppu->nextAt + 1);
		break;
	case STEP_HBLANK_INT:
		ppu->oamReadBlocked = false;
		ppu->vramReadBlocked = false;
		ppu->oamWriteBlocked = false;
		ppu->vramWriteBlocked = false;
		ppu->mode = 0;
		ppu->modeForInterrupt = 0;
		updateStat(gb);
		next(gb, STEP_PRE_OAM, ppu->lineEnd - 2);
		break;
	case STEP_PRE_OAM:
		if (ppu->line != GB_HEIGHT - 1) {
			ppu->modeForInterrupt = 2;
		}
		next(gb, STEP_LINE_END, ppu->lineEnd);
		break;
	case STEP_LINE_END:
		if (++ppu->line == GB_LINES) {
			ppu->line = 0;
			ppu->hiddenFrame = false;
			ppu->windowLine = 0;
			ppu->windowSeen = false;
		}
		ppu->firstLine = false;
		beginLine(gb, ppu->lineEnd);
		break;
	case STEP_FIRST:
		// SameBoy's line 0 after the LCD is switched on: mode 0 instead of the OAM scan.
		ppu->mode = 0;
		ppu->lyCompare = 0;
		ppu->modeForInterrupt = -1;
		ppu->oamReadBlocked = false;
		ppu->vramReadBlocked = false;
		ppu->oamWriteBlocked = false;
		ppu->vramWriteBlocked = false;
		updateStat(gb);
		next(gb, STEP_FIRST_OAM_WRITE, ppu->nextAt + 76);
		break;
	case STEP_FIRST_OAM_WRITE:
		ppu->oamWriteBlocked = true;
		next(gb, STEP_FIRST_MODE3, ppu->nextAt + 2);
		break;
	case STEP_FIRST_MODE3:
		ppu->mode = 3;
		ppu->modeForInterrupt = 3;
		ppu->oamWriteBlocked = true;
		ppu->oamReadBlocked = true;
		ppu->vramReadBlocked = !gbColor(gb) || gb->doubleSpeed;
		ppu->vramWriteBlocked = ppu->vramReadBlocked;
		next(gb, STEP_FIRST_VRAM, ppu->nextAt + 5);
		break;
	case STEP_FIRST_VRAM:
		ppu->vramReadBlocked = true;
		ppu->vramWriteBlocked = true;
		startDrawing(gb, ppu->nextAt);
		break;
	case STEP_V_LY:
		ppu->ly = ppu->line;
		if (ppu->line == GB_HEIGHT && !ppu->statLine && (ppu->stat & 0x20)) {
			gbRequest(gb, GB_IRQ_STAT);
		}
		next(gb, STEP_V_COMPARE, ppu->lineStart + 4);
		break;
	case STEP_V_COMPARE:
		ppu->lyCompare = ppu->line;
		updateStat(gb);
		if (ppu->line == GB_HEIGHT) {
			next(gb, STEP_V_BLANK, ppu->lineStart + 5);
		} else {
			next(gb, STEP_LINE_END, ppu->lineEnd);
		}
		break;
	case STEP_V_BLANK:
		ppu->mode = 1;
		gbRequest(gb, GB_IRQ_VBLANK);
		if (!ppu->statLine && (ppu->stat & 0x20)) {
			gbRequest(gb, GB_IRQ_STAT);
		}
		ppu->modeForInterrupt = 1;
		updateStat(gb);
		gb->frameEnded = true;
		gb->attention = true;
		++gb->frameCount;
		next(gb, STEP_LINE_END, ppu->lineEnd);
		break;
	case STEP_L153_LY:
		ppu->ly = 153;
		next(gb, STEP_L153_ZERO, ppu->lineStart + (gbColor(gb) ? 4 : 6));
		break;
	case STEP_L153_ZERO:
		if (single && !gbColor(gb)) {
			ppu->ly = 0;
		}
		ppu->lyCompare = 153;
		updateStat(gb);
		next(gb, STEP_L153_LYC0, ppu->lineStart + 8);
		break;
	case STEP_L153_LYC0:
		ppu->ly = 0;
		ppu->lyCompare = gbColor(gb) && gb->doubleSpeed ? 153 : -1;
		updateStat(gb);
		next(gb, STEP_L153_COMPARE, ppu->lineStart + 12);
		break;
	case STEP_L153_COMPARE:
		ppu->lyCompare = 0;
		updateStat(gb);
		next(gb, STEP_LINE_END, ppu->lineEnd);
		break;
	}
}

// Whether a step can request an interrupt, as STAT and LYC stand: only such steps are events. The
// others run when something reads or writes the PPU, or on the way to one that is.
static bool interrupting(const struct GB* gb, int step, int line) {
	const struct GBPpu* ppu = &gb->ppu;
	bool lyc = ppu->stat & 0x40;
	switch (step) {
	case STEP_LY:
		return (line && (ppu->stat & 0x20)) || (!line && lyc && ppu->lyc == 0);
	case STEP_OAM:
		return (ppu->stat & 0x20) || (lyc && ppu->lyc == line);
	case STEP_DRAW:
	case STEP_FIRST_VRAM:
		// Where mode 3 ends is worked out here.
		return (ppu->stat & 0x08) || gb->cgb.hdmaHblank || gb->sgbLine;
	case STEP_MODE0:
		// An H-blank DMA moves a block then, and a Super Game Boy takes the line.
		return gb->cgb.hdmaHblank || gb->sgbLine;
	case STEP_HBLANK_INT:
		return ppu->stat & 0x08;
	case STEP_FIRST:
		return lyc && ppu->lyc == 0;
	case STEP_V_LY:
		return line == GB_HEIGHT && (ppu->stat & 0x20);
	case STEP_V_COMPARE:
		return lyc && ppu->lyc == line;
	case STEP_V_BLANK:
		return true;
	case STEP_L153_ZERO:
		return lyc && ppu->lyc == 153;
	case STEP_L153_COMPARE:
		return lyc && ppu->lyc == 0;
	default:
		return false;
	}
}

// Schedules the PPU's event at the first step from here that can request an interrupt, following the
// steps' times as advance() takes them without running them. Past the start of mode 3, where its
// length is not yet known, it goes on from the line's end.
static void plan(struct GB* gb) {
	const struct GBPpu* ppu = &gb->ppu;
	if (!(ppu->lcdc & LCDC_ON)) {
		gbUnschedule(gb, GB_EVENT_PPU);
		return;
	}
	int step = ppu->step;
	int line = ppu->line;
	uint32_t at = ppu->nextAt;
	uint32_t lineStart = ppu->lineStart;
	uint32_t lineEnd = ppu->lineEnd;
	unsigned late153 = gbColor(gb) ? 4 : 6;
	for (int guard = 0; guard < 2400; ++guard) {
		if (interrupting(gb, step, line)) {
			break;
		}
		switch (step) {
		case STEP_OAM_WRITE:
			step = STEP_LY;
			at = lineStart + 3;
			break;
		case STEP_LY:
			step = STEP_OAM;
			at = lineStart + 4;
			break;
		case STEP_OAM:
			step = STEP_OAM_LATE;
			at = lineStart + 80;
			break;
		case STEP_OAM_LATE:
			step = STEP_MODE3;
			at = lineStart + 84;
			break;
		case STEP_MODE3:
			step = STEP_DRAW;
			at = lineStart + 89;
			break;
		case STEP_FIRST:
			step = STEP_FIRST_OAM_WRITE;
			at += 76;
			break;
		case STEP_FIRST_OAM_WRITE:
			step = STEP_FIRST_MODE3;
			at += 2;
			break;
		case STEP_FIRST_MODE3:
			step = STEP_FIRST_VRAM;
			at += 5;
			break;
		case STEP_MODE0:
			// Mode 3's end is known once it has started.
			step = STEP_HBLANK_INT;
			at += 1;
			break;
		case STEP_DRAW:
		case STEP_FIRST_VRAM:
		case STEP_HBLANK_INT:
			step = STEP_PRE_OAM;
			at = lineEnd - 2;
			break;
		case STEP_PRE_OAM:
			step = STEP_LINE_END;
			at = lineEnd;
			break;
		case STEP_LINE_END:
			line = line + 1 == GB_LINES ? 0 : line + 1;
			lineStart = lineEnd;
			lineEnd = lineStart + GB_LINE_DOTS;
			step = line < GB_HEIGHT ? STEP_OAM_WRITE : line == GB_LINES - 1 ? STEP_L153_LY : STEP_V_LY;
			at = lineStart + 2;
			break;
		case STEP_V_LY:
			step = STEP_V_COMPARE;
			at = lineStart + 4;
			break;
		case STEP_V_COMPARE:
			step = line == GB_HEIGHT ? STEP_V_BLANK : STEP_LINE_END;
			at = line == GB_HEIGHT ? lineStart + 5 : lineEnd;
			break;
		case STEP_V_BLANK:
		case STEP_L153_COMPARE:
			step = STEP_LINE_END;
			at = lineEnd;
			break;
		case STEP_L153_LY:
			step = STEP_L153_ZERO;
			at = lineStart + late153;
			break;
		case STEP_L153_ZERO:
			step = STEP_L153_LYC0;
			at = lineStart + 8;
			break;
		case STEP_L153_LYC0:
			step = STEP_L153_COMPARE;
			at = lineStart + 12;
			break;
		}
	}
	gbSchedule(gb, GB_EVENT_PPU, at);
}

static void catchUp(struct GB* gb) {
	while (gb->ppu.lcdc & LCDC_ON && gbSince(gb->now, gb->ppu.nextAt) >= 0) {
		advance(gb);
	}
}

void gbPpuEvent(struct GB* gb) {
	catchUp(gb);
	plan(gb);
}

void gbPpuSync(struct GB* gb) {
	catchUp(gb);
}

// The PPU stands still for some dots (a speed switch freezes it): its timeline moves on by that much.
void gbPpuFreeze(struct GB* gb, uint32_t dots) {
	struct GBPpu* ppu = &gb->ppu;
	catchUp(gb);
	if (!(ppu->lcdc & LCDC_ON)) {
		return;
	}
	ppu->lineStart += dots;
	ppu->lineEnd += dots;
	ppu->drawStart += dots;
	ppu->nextAt += dots;
	plan(gb);
}

// Whether the PPU is in an H-blank for a DMA started now: mode 0 on a visible line, though not in the
// dot before mode 2 shows (SameBoy's display state 7).
bool gbPpuInHblank(struct GB* gb) {
	const struct GBPpu* ppu = &gb->ppu;
	return ppu->mode == 0 && ppu->line < GB_HEIGHT && ppu->step != STEP_OAM;
}

void gbPpuReplan(struct GB* gb) {
	catchUp(gb);
	plan(gb);
}

static void lcdOn(struct GB* gb) {
	struct GBPpu* ppu = &gb->ppu;
	ppu->line = 0;
	ppu->ly = 0;
	ppu->firstLine = true;
	ppu->hiddenFrame = true;
	ppu->windowLine = 0;
	ppu->windowSeen = false;
	// The first line is 8 dots short, and starts a dot late: on the Color at double speed, LCDC's enable
	// bit lands 2 T-cycles into the write (SameBoy); at single speed, half a dot late reads the same
	// (AGE's stat-mode and ly tests).
	unsigned late = 1;
	ppu->halfDotLate = gbColor(gb) && !gb->doubleSpeed;
	ppu->lineStart = gb->now + late;
	ppu->lineEnd = gb->now + late + GB_LINE_DOTS - 8;
	next(gb, STEP_FIRST, gb->now + late);
	plan(gb);
}

static void lcdOff(struct GB* gb) {
	struct GBPpu* ppu = &gb->ppu;
	// An H-blank DMA waiting outside an H-blank moves a block as the LCD goes off (SameBoy).
	if (gb->cgb.hdmaHblank && gb->cgb.hdmaBlocks && ppu->mode != 0) {
		gbHdmaHblank(gb, gb->now);
	}
	// The screen shows nothing while the LCD is off.
	gbBlankFrame(gb);
	ppu->oamReadBlocked = false;
	ppu->vramReadBlocked = false;
	ppu->oamWriteBlocked = false;
	ppu->vramWriteBlocked = false;
	ppu->ly = 0;
	ppu->line = 0;
	ppu->mode = 0;
	ppu->windowLine = 0;
	ppu->windowSeen = false;
	gbUnschedule(gb, GB_EVENT_PPU);
}

void gbPpuReset(struct GB* gb) {
	struct GBPpu* ppu = &gb->ppu;
	memset(ppu, 0, sizeof(*ppu));
	ppu->lcdc = 0x91;
	ppu->bgp = 0xFC;
	ppu->modeForInterrupt = -1;
	lcdOn(gb);
}

uint8_t gbPpuRead(struct GB* gb, uint8_t reg) {
	struct GBPpu* ppu = &gb->ppu;
	gbPpuSync(gb);
	switch (reg) {
	case 0x40:
		return ppu->lcdc;
	case 0x41:
		if (gb->cpuQuarters != 4) {
			gb->cpuPaced = 2 * GB_LINE_DOTS;
		}
		return 0x80 | (ppu->stat & 0x78) | (ppu->coincidence ? 4 : 0) | (ppu->lcdc & LCDC_ON ? ppu->mode : 0);
	case 0x42:
		return ppu->scy;
	case 0x43:
		return ppu->scx;
	case 0x44:
		if (gb->cpuQuarters != 4) {
			gb->cpuPaced = 2 * GB_LINE_DOTS;
		}
		return ppu->ly;
	case 0x45:
		return ppu->lyc;
	case 0x46:
		return ppu->dma;
	case 0x47:
		return ppu->bgp;
	case 0x48:
		return ppu->obp0;
	case 0x49:
		return ppu->obp1;
	case 0x4A:
		return ppu->wy;
	default:
		return ppu->wx;
	}
}

void gbPpuWrite(struct GB* gb, uint8_t reg, uint8_t value) {
	struct GBPpu* ppu = &gb->ppu;
	gbPpuSync(gb);
	switch (reg) {
	case 0x40: {
		uint8_t old = ppu->lcdc;
		ppu->lcdc = value;
		if ((old & LCDC_ON) && !(value & LCDC_ON)) {
			lcdOff(gb);
		} else if (!(old & LCDC_ON) && (value & LCDC_ON)) {
			lcdOn(gb);
		} else {
			checkWindow(gb);
		}
		break;
	}
	case 0x41:
		ppu->stat = value & 0x78;
		updateStat(gb);
		plan(gb);
		break;
	case 0x42:
		ppu->scy = value;
		break;
	case 0x43:
		ppu->scx = value;
		break;
	case 0x44:
		break;
	case 0x45:
		ppu->lyc = value;
		updateStat(gb);
		plan(gb);
		break;
	case 0x46:
		ppu->dma = value;
		gbDmaStart(gb, value);
		break;
	case 0x47:
		ppu->bgp = value;
		break;
	case 0x48:
		ppu->obp0 = value;
		break;
	case 0x49:
		ppu->obp1 = value;
		break;
	case 0x4A:
		ppu->wy = value;
		checkWindow(gb);
		break;
	default:
		ppu->wx = value;
		break;
	}
}
