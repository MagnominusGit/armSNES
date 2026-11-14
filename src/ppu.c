/* Copyright (c) 2004-2025 ares team, Near et al
 * Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Ported from ares (ISC); see LICENSES/ISC-ares.txt. */
#include "snes.h"

// The S-PPU, drawn dot by dot: every two master clocks it does what the hardware does in that slot
// (evaluates a sprite, fetches a tile, puts out a pixel) and then stops if its clock has passed the
// CPU's. It is the picture the faster renderers have to match.

struct Ppu ppu;
u32 ppuCanvas[CANVAS_WIDTH * CANVAS_HEIGHT];

#define VCOUNTER ppu.counter.vcounter
#define HCOUNTER ppu.counter.hcounter

static inline u16* vramAt(u32 address) {
	return &ppu.vram[address & ppu.vramMask];
}

static inline bool hiresMode(void) {
	return ppu.io.bgMode == 5 || ppu.io.bgMode == 6;
}

// Mosaic

static bool mosaicEnable(void) {
	return ppu.bg[0].mosaic.enable || ppu.bg[1].mosaic.enable || ppu.bg[2].mosaic.enable || ppu.bg[3].mosaic.enable;
}

static inline u32 mosaicVoffset(void) {
	return ppu.mosaic.size - ppu.mosaic.vcounter;
}

static void mosaicScanline(void) {
	if (VCOUNTER == 1) {
		ppu.mosaic.vcounter = (mosaicEnable() ? ppu.mosaic.size + 1 : 0) & 31;
	}
	if (ppu.mosaic.vcounter && !--ppu.mosaic.vcounter) {
		ppu.mosaic.vcounter = mosaicEnable() ? ppu.mosaic.size : 0;
	}
}

// Backgrounds

static void bgScanline(struct Background* bg) {
	bg->mosaic.hcounter = ppu.mosaic.size;
	bg->mosaic.hoffset = 0;

	bg->renderingIndex = 0;
	bg->pixelCounter = ((bg->io.hoffset & 7) << hiresMode()) & 7;

	bg->opt.hoffset = 0;
	bg->opt.voffset = 0;
}

// At H = 56: the part of the first tile scrolled off the screen goes.
static void bgBegin(struct Background* bg) {
	for (int n = 0; n < 4; ++n) {
		bg->tiles[0].data[n] >>= bg->pixelCounter << 1;
	}
}

// optH and optV are BG3's offset-per-tile values as they are when this fetch happens.
static void bgFetchNameTable(struct Background* bg, u16 optH, u16 optV) {
	if (VCOUNTER == 0) {
		return;
	}

	bool hires = hiresMode();
	u32 nameTableIndex = HCOUNTER >> 5 << hires;
	s32 x = (HCOUNTER & ~31) >> 2;

	u32 hpixel = x << hires;
	u32 vpixel = VCOUNTER;
	u32 hscroll = bg->io.hoffset;
	u32 vscroll = bg->io.voffset;

	if (hires) {
		hscroll <<= 1;
		if (ppu.io.interlace) {
			vpixel = vpixel << 1 | (ppu.counter.field && !bg->mosaic.enable);
		}
	}
	if (bg->mosaic.enable) {
		vpixel -= mosaicVoffset() << (hires && ppu.io.interlace);
	}

	bool repeated = false;
	for (;;) {
		u32 hoffset = hpixel + hscroll;
		u32 voffset = vpixel + vscroll;

		if (ppu.io.bgMode == 2 || ppu.io.bgMode == 4 || ppu.io.bgMode == 6) {
			u32 hlookup = optH;
			u32 vlookup = optV;
			u32 valid = 13 + bg->id;

			if (ppu.io.bgMode == 4) {
				if (hlookup >> valid & 1) {
					if (!(hlookup & 0x8000)) {
						hoffset = hpixel + (hlookup & ~7) + (hscroll & 7);
					} else {
						voffset = vpixel + vlookup;
					}
				}
			} else {
				if (hlookup >> valid & 1) {
					hoffset = hpixel + (hlookup & ~7) + (hscroll & 7);
				}
				if (vlookup >> valid & 1) {
					voffset = vpixel + vlookup;
				}
			}
		}

		u32 width = 256 << hires;
		u32 hsize = width << bg->io.tileSize << (bg->io.screenSize & 1);
		u32 vsize = width << bg->io.tileSize << (bg->io.screenSize >> 1 & 1);

		hoffset &= hsize - 1;
		voffset &= vsize - 1;

		u32 vtiles = 3 + bg->io.tileSize;
		u32 htiles = !hires ? vtiles : 4;

		u32 htile = hoffset >> htiles;
		u32 vtile = voffset >> vtiles;

		u32 hscreen = bg->io.screenSize & 1 ? 32 << 5 : 0;
		u32 vscreen = bg->io.screenSize & 2 ? 32 << (5 + (bg->io.screenSize & 1)) : 0;

		u16 offset = (htile & 31) | (vtile & 31) << 5;
		if (htile & 0x20) {
			offset += hscreen;
		}
		if (vtile & 0x20) {
			offset += vscreen;
		}

		u16 address = bg->io.screenAddress + offset;
		u16 attributes = *vramAt(address);

		struct BgTile* tile = &bg->tiles[nameTableIndex];
		tile->character = attributes & 0x3ff;
		tile->paletteGroup = attributes >> 10 & 7;
		tile->priority = bg->io.priority[attributes >> 13 & 1];
		tile->hmirror = attributes >> 14 & 1;
		tile->vmirror = attributes >> 15 & 1;

		if (htiles == 4 && (bool) (hoffset & 8) != tile->hmirror) {
			tile->character = (tile->character + 1) & 0x3ff;
		}
		if (vtiles == 4 && (bool) (voffset & 8) != tile->vmirror) {
			tile->character = (tile->character + 16) & 0x3ff;
		}

		u32 characterMask = ppu.vramMask >> (3 + bg->io.mode);
		u32 characterIndex = bg->io.tiledataAddress >> (3 + bg->io.mode);
		u16 origin = (tile->character + characterIndex) & characterMask;

		if (tile->vmirror) {
			voffset ^= 7;
		}
		tile->address = (origin << (3 + bg->io.mode)) + (voffset & 7);

		u32 paletteOffset = ppu.io.bgMode == 0 ? bg->id << 5 : 0;
		u32 paletteSize = 2 << bg->io.mode;
		tile->palette = paletteOffset + (tile->paletteGroup << paletteSize);

		nameTableIndex++;
		if (hires && !repeated) {
			repeated = true;
			hpixel += 8;
			continue;
		}
		break;
	}
}

static void bgFetchOffset(struct Background* bg, u32 y) {
	if (VCOUNTER == 0) {
		return;
	}

	u32 characterIndex = HCOUNTER >> 5;
	u32 x = characterIndex << 3;

	u32 hoffset = x + (bg->io.hoffset & ~7);
	u32 voffset = y + bg->io.voffset;

	u32 vtiles = 3 + bg->io.tileSize;
	u32 htiles = !hiresMode() ? vtiles : 3;

	u32 htile = hoffset >> htiles;
	u32 vtile = voffset >> vtiles;

	u32 hscreen = bg->io.screenSize & 1 ? 32 << 5 : 0;
	u32 vscreen = bg->io.screenSize & 2 ? 32 << (5 + (bg->io.screenSize & 1)) : 0;

	u16 offset = (htile & 31) | (vtile & 31) << 5;
	if (htile & 0x20) {
		offset += hscreen;
	}
	if (vtile & 0x20) {
		offset += vscreen;
	}

	u16 address = bg->io.screenAddress + offset;
	if (y == 0) {
		bg->opt.hoffset = *vramAt(address);
	}
	if (y == 8) {
		bg->opt.voffset = *vramAt(address);
	}
}

static void bgFetchCharacter(struct Background* bg, u32 index, bool half) {
	if (VCOUNTER == 0) {
		return;
	}

	u32 characterIndex = (HCOUNTER >> 5 << hiresMode()) + half;
	struct BgTile* tile = &bg->tiles[characterIndex];

	u16 data = *vramAt(tile->address + (index << 3));

	// The bits are reversed, so the lowest is the leftmost pixel.
	if (!tile->hmirror) {
		data = (data >> 4 & 0x0f0f) | (data << 4 & 0xf0f0);
		data = (data >> 2 & 0x3333) | (data << 2 & 0xcccc);
		data = (data >> 1 & 0x5555) | (data << 1 & 0xaaaa);
	}

	// The two bitplanes are interleaved.
	tile->data[index] = ((((u64) (u8) (data >> 0) * 0x0101010101010101ull & 0x8040201008040201ull) * 0x0102040810204081ull >> 49) & 0x5555) |
		((((u64) (u8) (data >> 8) * 0x0101010101010101ull & 0x8040201008040201ull) * 0x0102040810204081ull >> 48) & 0xaaaa);
}

static void bgRunMode7(struct Background* bg) {
	s32 a = (s16) ppu.io.m7a;
	s32 b = (s16) ppu.io.m7b;
	s32 c = (s16) ppu.io.m7c;
	s32 d = (s16) ppu.io.m7d;

	s32 hcenter = signedBits(ppu.io.m7x, 13);
	s32 vcenter = signedBits(ppu.io.m7y, 13);
	s32 hoffset = signedBits(ppu.io.hoffsetMode7, 13);
	s32 voffset = signedBits(ppu.io.voffsetMode7, 13);

	u32 x = bg->mosaic.hoffset;
	u32 y = VCOUNTER;
	// BG2's vertical mosaic goes by BG1's enable.
	if (ppu.bg[0].mosaic.enable) {
		y -= mosaicVoffset();
	}

	if (!bg->mosaic.enable) {
		bg->mosaic.hoffset += 1;
	} else if (--bg->mosaic.hcounter == 0) {
		bg->mosaic.hcounter = ppu.mosaic.size;
		bg->mosaic.hoffset += ppu.mosaic.size;
	}

	if (ppu.io.hflipMode7) {
		x = 255 - x;
	}
	if (ppu.io.vflipMode7) {
		y = 255 - y;
	}

	// 13-bit sign extension: --s---nnnnnnnnnn -> ssssssnnnnnnnnnn
#define CLIP(n) ((n) & 0x2000 ? ((n) | ~1023) : ((n) & 1023))
	s32 hclip = CLIP(hoffset - hcenter);
	s32 vclip = CLIP(voffset - vcenter);
#undef CLIP

	// As in ares, the sums with the unsigned x and y are unsigned, and so is the shift after them.
	u32 originX = ((u32) (a * hclip) & ~63) + ((u32) (b * vclip) & ~63) + (((u32) b * y) & ~63) + ((u32) hcenter << 8);
	u32 originY = ((u32) (c * hclip) & ~63) + ((u32) (d * vclip) & ~63) + (((u32) d * y) & ~63) + ((u32) vcenter << 8);

	s32 pixelX = (originX + (u32) a * x) >> 8;
	s32 pixelY = (originY + (u32) c * x) >> 8;
	u16 paletteAddress = (pixelY & 7) << 3 | (pixelX & 7);

	u8 tileX = pixelX >> 3 & 127;
	u8 tileY = pixelY >> 3 & 127;
	u16 tileAddress = tileY << 7 | tileX;

	bool outOfBounds = (pixelX | pixelY) & ~1023;

	u8 tile = ppu.io.repeatMode7 == 3 && outOfBounds ? 0 : *vramAt(tileAddress) >> 0;
	u8 palette = ppu.io.repeatMode7 == 2 && outOfBounds ? 0 : *vramAt(tile << 6 | paletteAddress) >> 8;

	u32 priority = 0;
	if (bg->id == 0) {
		priority = bg->io.priority[0];
	} else if (bg->id == 1) {
		priority = bg->io.priority[palette >> 7];
		palette &= 0x7f;
	}

	if (palette == 0) {
		return;
	}

	if (bg->io.aboveEnable) {
		bg->output.above.priority = priority;
		bg->output.above.palette = palette;
		bg->output.above.paletteGroup = 0;
	}
	if (bg->io.belowEnable) {
		bg->output.below.priority = priority;
		bg->output.below.palette = palette;
		bg->output.below.paletteGroup = 0;
	}
}

// screen: 0 = above (main), 1 = below (sub). A line's first dot is at H = 56, and line 0 draws
// nothing: that is the caller's to see to.
static inline __attribute__((always_inline)) void bgRunAt(struct Background* bg, bool screen, bool hires, u32 at) {
	if (screen == 1) {
		bg->output.above.priority = 0;
		bg->output.below.priority = 0;
		if (!hires) {
			return;
		}
	}

	if (bg->io.mode == BG_MODE7) {
		bgRunMode7(bg);
		return;
	}

	struct BgTile* tile = &bg->tiles[bg->renderingIndex];
	u8 color = 0;
	color |= tile->data[0] & 3;
	tile->data[0] >>= 2;
	if (bg->io.mode >= BG_BPP4) {
		color |= (tile->data[1] & 3) << 2;
	}
	tile->data[1] >>= 2;
	if (bg->io.mode >= BG_BPP8) {
		color |= (tile->data[2] & 3) << 4;
	}
	tile->data[2] >>= 2;
	if (bg->io.mode >= BG_BPP8) {
		color |= (tile->data[3] & 3) << 6;
	}
	tile->data[3] >>= 2;

	struct BgPixel pixel;
	pixel.priority = tile->priority;
	pixel.palette = color ? tile->palette + color : 0;
	pixel.paletteGroup = tile->paletteGroup;
	bg->pixelCounter = (bg->pixelCounter + 1) & 7;
	if (bg->pixelCounter == 0) {
		bg->renderingIndex = (bg->renderingIndex + 1) & 127;
	}

	u32 x = (at - 56) >> 2;

	if (x == 0 && (!hires || screen == 1)) {
		bg->mosaic.hcounter = ppu.mosaic.size;
		bg->mosaic.pixel = pixel;
	} else if ((!hires || screen == 1) && --bg->mosaic.hcounter == 0) {
		bg->mosaic.hcounter = ppu.mosaic.size;
		bg->mosaic.pixel = pixel;
	} else if (bg->mosaic.enable) {
		pixel = bg->mosaic.pixel;
	}

	if (pixel.palette == 0) {
		return;
	}

	if (!hires || screen == 0) {
		if (bg->io.aboveEnable) {
			bg->output.above = pixel;
		}
	}
	if (!hires || screen == 1) {
		if (bg->io.belowEnable) {
			bg->output.below = pixel;
		}
	}
}

#if defined(ARMSNES_DOT)
static void bgRun(struct Background* bg, bool screen) {
	if (VCOUNTER == 0) {
		return;
	}
	bgRunAt(bg, screen, hiresMode(), HCOUNTER);
}
#endif

static void bgPower(struct Background* bg) {
	memset(&bg->io, 0, sizeof(bg->io));
	bg->io.tiledataAddress = (randomNext() & 0x0f) << 12;
	bg->io.screenAddress = (randomNext() & 0xfc) << 8;
	bg->io.screenSize = randomNext() & 3;
	bg->io.tileSize = randomNext() & 1;
	bg->io.aboveEnable = randomNext() & 1;
	bg->io.belowEnable = randomNext() & 1;
	bg->io.hoffset = randomNext();
	bg->io.voffset = randomNext();

	memset(&bg->output, 0, sizeof(bg->output));
	memset(&bg->mosaic, 0, sizeof(bg->mosaic));
	bg->mosaic.enable = randomNext() & 1;
}

// OAM

static u8 oamRead(u16 address) {
	address &= 0x3ff;
	u8 data = 0;

	if (!(address & 0x200)) {
		const struct OamObject* object = &ppu.oam[address >> 2 & 127];
		address &= 3;
		if (address == 0) {
			return object->x;
		}
		if (address == 1) {
			return object->y;
		}
		if (address == 2) {
			return object->character;
		}
		data |= object->nameselect << 0;
		data |= object->palette << 1;
		data |= object->priority << 4;
		data |= object->hflip << 6;
		data |= object->vflip << 7;
		return data;
	}

	u32 n = (address & 31) << 2;
	for (int i = 0; i < 4; ++i) {
		data |= (ppu.oam[n + i].x >> 8 & 1) << (i * 2);
		data |= ppu.oam[n + i].size << (i * 2 + 1);
	}
	return data;
}

static void oamWrite(u16 address, u8 data) {
	address &= 0x3ff;

	if (!(address & 0x200)) {
		struct OamObject* object = &ppu.oam[address >> 2 & 127];
		address &= 3;
		if (address == 0) {
			object->x = (object->x & 0x100) | data;
			return;
		}
		if (address == 1) {
			object->y = data;
			return;
		}
		if (address == 2) {
			object->character = data;
			return;
		}
		object->nameselect = data & 1;
		object->palette = data >> 1 & 7;
		object->priority = data >> 4 & 3;
		object->hflip = data >> 6 & 1;
		object->vflip = data >> 7 & 1;
	} else {
		u32 n = (address & 31) << 2;
		for (int i = 0; i < 4; ++i) {
			ppu.oam[n + i].x = (ppu.oam[n + i].x & 0xff) | (data >> (i * 2) & 1) << 8;
			ppu.oam[n + i].size = data >> (i * 2 + 1) & 1;
		}
	}
}

static u32 objectWidth(const struct OamObject* object) {
	static const u8 small[] = { 8, 8, 8, 16, 16, 32, 16, 16 };
	static const u8 large[] = { 16, 32, 64, 32, 64, 64, 32, 32 };
	return object->size == 0 ? small[ppu.obj.io.baseSize] : large[ppu.obj.io.baseSize];
}

static u32 objectHeight(const struct OamObject* object) {
	static const u8 small[] = { 8, 8, 8, 16, 16, 32, 32, 32 };
	static const u8 large[] = { 16, 32, 64, 32, 64, 64, 64, 32 };
	if (object->size == 0) {
		// A hardware quirk.
		if (ppu.obj.io.interlace && ppu.obj.io.baseSize >= 6) {
			return 16;
		}
		return small[ppu.obj.io.baseSize];
	}
	return large[ppu.obj.io.baseSize];
}

// Sprites

static void objSetFirstSprite(void) {
	ppu.obj.io.firstSprite = 0;
	if (ppu.io.oamPriority) {
		ppu.obj.io.firstSprite = ppu.io.oamAddress >> 2 & 127;
	}
}

static void objAddressReset(void) {
	ppu.io.oamAddress = ppu.io.oamBaseAddress;
	objSetFirstSprite();
}

static void objScanline(void) {
	ppu.obj.latchFirstSprite = ppu.obj.io.firstSprite;

	ppu.obj.x = 0;
	ppu.obj.y = VCOUNTER;
	ppu.obj.itemCount = 0;
	ppu.obj.tileCount = 0;

	ppu.obj.active = !ppu.obj.active;
	for (int n = 0; n < 32; ++n) {
		ppu.obj.item[ppu.obj.active][n].valid = false;
	}
	for (int n = 0; n < 34; ++n) {
		ppu.obj.tile[ppu.obj.active][n].valid = false;
	}

	if (ppu.obj.y == ppu.state.vdisp && !ppu.io.displayDisable) {
		objAddressReset();
	}
}

static bool objOnScanline(const struct OamObject* sprite) {
	if (sprite->x > 256 && sprite->x + objectWidth(sprite) - 1 < 512) {
		return false;
	}
	u32 height = objectHeight(sprite) >> ppu.obj.io.interlace;
	s32 index = (s32) (ppu.obj.y % 256) - sprite->y;
	if (index < 0) {
		index += 256;
	}
	return (u32) index < height;
}

static void objEvaluate(u8 index) {
	if (ppu.io.displayDisable) {
		return;
	}
	if (ppu.obj.itemCount > 32) {
		return;
	}

	u8 sprite = (ppu.obj.latchFirstSprite + index) & 127;
	if (!objOnScanline(&ppu.oam[sprite])) {
		return;
	}
	ppu.latch.oamAddress = sprite;

	if (ppu.obj.itemCount++ < 32) {
		ppu.obj.item[ppu.obj.active][ppu.obj.itemCount - 1].valid = true;
		ppu.obj.item[ppu.obj.active][ppu.obj.itemCount - 1].index = sprite;
	}
}

static void objRun(void) {
	ppu.obj.above.priority = 0;
	ppu.obj.below.priority = 0;

	u32 x = ppu.obj.x++;

	for (int n = 0; n < 34; ++n) {
		const __typeof__(ppu.obj.tile[0][0])* tile = &ppu.obj.tile[!ppu.obj.active][n];
		if (!tile->valid) {
			break;
		}

		s32 px = x - signedBits(tile->x, 9);
		if (px & ~7) {
			continue;
		}

		u32 color = 0;
		u32 shift = tile->hflip ? px : 7 - px;
		color += tile->data >> (shift + 0) & 1;
		color += tile->data >> (shift + 7) & 2;
		color += tile->data >> (shift + 14) & 4;
		color += tile->data >> (shift + 21) & 8;

		if (color) {
			if (ppu.obj.io.aboveEnable) {
				ppu.obj.above.palette = tile->palette + color;
				ppu.obj.above.priority = ppu.obj.io.priority[tile->priority];
			}
			if (ppu.obj.io.belowEnable) {
				ppu.obj.below.palette = tile->palette + color;
				ppu.obj.below.priority = ppu.obj.io.priority[tile->priority];
			}
		}
	}
}

static void objPower(void) {
	ppu.obj.x = 0;
	ppu.obj.y = 0;
	ppu.obj.itemCount = 0;
	ppu.obj.tileCount = 0;
	ppu.obj.active = 0;
	memset(ppu.obj.item, 0, sizeof(ppu.obj.item));
	memset(ppu.obj.tile, 0, sizeof(ppu.obj.tile));

	ppu.obj.io.aboveEnable = randomNext() & 1;
	ppu.obj.io.belowEnable = randomNext() & 1;
	ppu.obj.io.interlace = randomNext() & 1;
	ppu.obj.io.baseSize = randomNext() & 7;
	ppu.obj.io.nameselect = randomNext() & 3;
	ppu.obj.io.tiledataAddress = (randomNext() & 7) << 13;
	ppu.obj.io.firstSprite = 0;
	memset(ppu.obj.io.priority, 0, sizeof(ppu.obj.io.priority));
	ppu.obj.io.timeOver = false;
	ppu.obj.io.rangeOver = false;

	ppu.obj.latchFirstSprite = 0;

	ppu.obj.above.palette = 0;
	ppu.obj.above.priority = 0;
	ppu.obj.below.palette = 0;
	ppu.obj.below.priority = 0;
}

// Windows

static void windowScanline(void) {
	ppu.window.x = 0;
	ppu.window.oneActive = ppu.window.oneStopped = false;
	ppu.window.twoActive = ppu.window.twoStopped = false;
}

static bool windowTest(bool oneEnable, bool one, bool twoEnable, bool two, u32 mask) {
	if (!oneEnable) {
		return two && twoEnable;
	}
	if (!twoEnable) {
		return one;
	}
	if (mask == 0) {
		return one | two;
	}
	if (mask == 1) {
		return one & two;
	}
	return (u32) (one ^ two) == 3 - mask;
}

static inline bool windowLayerTest(const struct WindowLayer* layer, bool one, bool two) {
	return windowTest(layer->oneEnable, one ^ layer->oneInvert, layer->twoEnable, two ^ layer->twoInvert, layer->mask);
}

#if !defined(ARMSNES_DOT)
// What the windows do for each of the four ways the two can be (one inside or not, two inside or
// not): which layers' main and sub screen pixels they hide (BG1-4, then sprites, a bit each), and
// whether color math is on for each screen.
struct WindowEffect {
	u8 hideAbove;
	u8 hideBelow;
	bool aboveColorEnable;
	bool belowColorEnable;
};

static void windowEffects(struct WindowEffect effects[4]) {
	for (int c = 0; c < 4; ++c) {
		bool one = c & 1;
		bool two = c >> 1;
		struct WindowEffect* effect = &effects[c];
		effect->hideAbove = 0;
		effect->hideBelow = 0;
		for (int n = 0; n < 5; ++n) {
			const struct WindowLayer* layer = n < 4 ? &ppu.window.bg[n] : &ppu.window.obj;
			if (windowLayerTest(layer, one, two)) {
				effect->hideAbove |= layer->aboveEnable << n;
				effect->hideBelow |= layer->belowEnable << n;
			}
		}
		bool value = windowTest(ppu.window.col.oneEnable, one ^ ppu.window.col.oneInvert, ppu.window.col.twoEnable,
		                        two ^ ppu.window.col.twoInvert, ppu.window.col.mask);
		bool array[] = { true, value, !value, false };
		effect->aboveColorEnable = array[ppu.window.col.aboveMask];
		effect->belowColorEnable = array[ppu.window.col.belowMask];
	}
}

// The windows at the next pixel, with their effects worked out beforehand.
static inline void windowRunWith(const struct WindowEffect effects[4]) {
	if (ppu.window.x == ppu.window.oneLeft) {
		ppu.window.oneActive = true;
	}
	if (ppu.window.x == ppu.window.twoLeft) {
		ppu.window.twoActive = true;
	}

	bool one = ppu.window.oneActive && !ppu.window.oneStopped;
	bool two = ppu.window.twoActive && !ppu.window.twoStopped;

	if (ppu.window.x == ppu.window.oneRight) {
		ppu.window.oneStopped = true;
	}
	if (ppu.window.x == ppu.window.twoRight) {
		ppu.window.twoStopped = true;
	}

	ppu.window.x++;

	const struct WindowEffect* effect = &effects[one | two << 1];
	if (effect->hideAbove | effect->hideBelow) {
		for (int n = 0; n < 4; ++n) {
			if (effect->hideAbove >> n & 1) {
				ppu.bg[n].output.above.priority = 0;
			}
			if (effect->hideBelow >> n & 1) {
				ppu.bg[n].output.below.priority = 0;
			}
		}
		if (effect->hideAbove >> 4 & 1) {
			ppu.obj.above.priority = 0;
		}
		if (effect->hideBelow >> 4 & 1) {
			ppu.obj.below.priority = 0;
		}
	}
	ppu.window.aboveColorEnable = effect->aboveColorEnable;
	ppu.window.belowColorEnable = effect->belowColorEnable;
}

#else
static void windowRun(void) {
	if (ppu.window.x == ppu.window.oneLeft) {
		ppu.window.oneActive = true;
	}
	if (ppu.window.x == ppu.window.twoLeft) {
		ppu.window.twoActive = true;
	}

	bool one = ppu.window.oneActive && !ppu.window.oneStopped;
	bool two = ppu.window.twoActive && !ppu.window.twoStopped;

	if (ppu.window.x == ppu.window.oneRight) {
		ppu.window.oneStopped = true;
	}
	if (ppu.window.x == ppu.window.twoRight) {
		ppu.window.twoStopped = true;
	}

	ppu.window.x++;

	for (int n = 0; n < 4; ++n) {
		if (windowLayerTest(&ppu.window.bg[n], one, two)) {
			if (ppu.window.bg[n].aboveEnable) {
				ppu.bg[n].output.above.priority = 0;
			}
			if (ppu.window.bg[n].belowEnable) {
				ppu.bg[n].output.below.priority = 0;
			}
		}
	}

	if (windowLayerTest(&ppu.window.obj, one, two)) {
		if (ppu.window.obj.aboveEnable) {
			ppu.obj.above.priority = 0;
		}
		if (ppu.window.obj.belowEnable) {
			ppu.obj.below.priority = 0;
		}
	}

	bool value = windowTest(ppu.window.col.oneEnable, one ^ ppu.window.col.oneInvert, ppu.window.col.twoEnable,
	                        two ^ ppu.window.col.twoInvert, ppu.window.col.mask);
	bool array[] = { true, value, !value, false };
	ppu.window.aboveColorEnable = array[ppu.window.col.aboveMask];
	ppu.window.belowColorEnable = array[ppu.window.col.belowMask];
}

#endif

static void windowLayerPower(struct WindowLayer* layer) {
	layer->oneEnable = randomNext() & 1;
	layer->oneInvert = randomNext() & 1;
	layer->twoEnable = randomNext() & 1;
	layer->twoInvert = randomNext() & 1;
	layer->mask = randomNext() & 3;
	layer->aboveEnable = randomNext() & 1;
	layer->belowEnable = randomNext() & 1;
}

static void windowPower(void) {
	for (int n = 0; n < 4; ++n) {
		windowLayerPower(&ppu.window.bg[n]);
	}
	windowLayerPower(&ppu.window.obj);

	ppu.window.col.oneEnable = randomNext() & 1;
	ppu.window.col.oneInvert = randomNext() & 1;
	ppu.window.col.twoEnable = randomNext() & 1;
	ppu.window.col.twoInvert = randomNext() & 1;
	ppu.window.col.mask = randomNext() & 3;
	ppu.window.col.aboveMask = randomNext() & 3;
	ppu.window.col.belowMask = randomNext() & 3;

	ppu.window.oneLeft = randomNext();
	ppu.window.oneRight = randomNext();
	ppu.window.twoLeft = randomNext();
	ppu.window.twoRight = randomNext();

	ppu.window.aboveColorEnable = 0;
	ppu.window.belowColorEnable = 0;

	windowScanline();
}

// DAC

static inline u16 paletteColor(u8 palette) {
	ppu.latch.cgramAddress = palette;
	return ppu.cgram[palette];
}

// palette = BBGGGRRR, group = bgr: 0BBb00GG Gg0RRRr0
static inline u16 directColor(u8 palette, u8 paletteGroup) {
	return (palette << 7 & 0x6000) + (paletteGroup << 10 & 0x1000) + (palette << 4 & 0x0380) + (paletteGroup << 5 & 0x0040) +
		(palette << 2 & 0x001c) + (paletteGroup << 1 & 0x0002);
}

static inline u16 fixedColor(void) {
	return ppu.dac.colorRed << 0 | ppu.dac.colorGreen << 5 | ppu.dac.colorBlue << 10;
}

static u16 blend(u32 x, u32 y) {
	if (!ppu.dac.colorMode) {
		if (!ppu.dac.math.colorHalve) {
			u32 sum = x + y;
			u32 carry = (sum - ((x ^ y) & 0x0421)) & 0x8420;
			return ((sum - carry) | (carry - (carry >> 5))) & 0x7fff;
		}
		return ((x + y - ((x ^ y) & 0x0421)) >> 1) & 0x7fff;
	}
	u32 diff = x - y + 0x8420;
	u32 borrow = (diff - ((x ^ y) & 0x8420)) & 0x8420;
	if (!ppu.dac.math.colorHalve) {
		return ((diff - borrow) & (borrow - (borrow >> 5))) & 0x7fff;
	}
	return ((((diff - borrow) & (borrow - (borrow >> 5))) & 0x7bde) >> 1) & 0x7fff;
}

static void dacScanline(void) {
	ppu.dac.lineValid = false;

	u32 vcounter = VCOUNTER;
	if (!ppu.state.overscan) {
		vcounter += 8;
	}
	if (vcounter < 240) {
		u32 yScale = ppu.state.interlace ? 2 : 1;
		ppu.dac.line = vcounter * yScale * CANVAS_WIDTH;
		// PAL has more border above.
		if (sys.pal) {
			ppu.dac.line += (20 * yScale) * CANVAS_WIDTH;
		}
		if (ppu.state.interlace && ppu.counter.field) {
			ppu.dac.line += CANVAS_WIDTH;
		}
		// The border on the left.
		ppu.dac.line += sys.pal ? 22 : 26;
		ppu.dac.lineValid = true;
	}

	// The first hires pixel of a line is transparent.
	ppu.dac.math.aboveColor = paletteColor(0);
	ppu.dac.math.belowColor = ppu.dac.math.aboveColor;
	ppu.dac.math.aboveColorEnable = false;
	ppu.dac.math.belowColorEnable = false;
	ppu.dac.math.transparent = true;
	ppu.dac.math.blendMode = false;
	ppu.dac.math.colorHalve = ppu.dac.colorHalve && !ppu.dac.blendMode && ppu.dac.math.aboveColorEnable;
}

static inline bool directColorMode(void) {
	return ppu.dac.directColor && (ppu.io.bgMode == 3 || ppu.io.bgMode == 4 || ppu.io.bgMode == 7);
}

static u16 dacBelow(bool hires) {
	if (ppu.io.displayDisable || (!ppu.io.overscan && VCOUNTER >= 225)) {
		return 0;
	}

	u32 priority = 0;
	if (ppu.bg[0].output.below.priority) {
		priority = ppu.bg[0].output.below.priority;
		if (directColorMode()) {
			ppu.dac.math.belowColor = directColor(ppu.bg[0].output.below.palette, ppu.bg[0].output.below.paletteGroup);
		} else {
			ppu.dac.math.belowColor = paletteColor(ppu.bg[0].output.below.palette);
		}
	}
	for (int n = 1; n < 4; ++n) {
		if (ppu.bg[n].output.below.priority > priority) {
			priority = ppu.bg[n].output.below.priority;
			ppu.dac.math.belowColor = paletteColor(ppu.bg[n].output.below.palette);
		}
	}
	if (ppu.obj.below.priority > priority) {
		priority = ppu.obj.below.priority;
		ppu.dac.math.belowColor = paletteColor(ppu.obj.below.palette);
	}
	if ((ppu.dac.math.transparent = (priority == 0))) {
		ppu.dac.math.belowColor = paletteColor(0);
	}

	if (!hires) {
		return 0;
	}
	if (!ppu.dac.math.belowColorEnable) {
		return ppu.dac.math.aboveColorEnable ? ppu.dac.math.belowColor : 0;
	}

	return blend(ppu.dac.math.aboveColorEnable ? ppu.dac.math.belowColor : 0,
	             ppu.dac.math.blendMode ? ppu.dac.math.aboveColor : fixedColor());
}

static u16 dacAbove(void) {
	if (ppu.io.displayDisable || (!ppu.io.overscan && VCOUNTER >= 225)) {
		return 0;
	}

	u32 priority = 0;
	if (ppu.bg[0].output.above.priority) {
		priority = ppu.bg[0].output.above.priority;
		if (directColorMode()) {
			ppu.dac.math.aboveColor = directColor(ppu.bg[0].output.above.palette, ppu.bg[0].output.above.paletteGroup);
		} else {
			ppu.dac.math.aboveColor = paletteColor(ppu.bg[0].output.above.palette);
		}
		ppu.dac.math.belowColorEnable = ppu.dac.bgColorEnable[0];
	}
	for (int n = 1; n < 4; ++n) {
		if (ppu.bg[n].output.above.priority > priority) {
			priority = ppu.bg[n].output.above.priority;
			ppu.dac.math.aboveColor = paletteColor(ppu.bg[n].output.above.palette);
			ppu.dac.math.belowColorEnable = ppu.dac.bgColorEnable[n];
		}
	}
	if (ppu.obj.above.priority > priority) {
		priority = ppu.obj.above.priority;
		ppu.dac.math.aboveColor = paletteColor(ppu.obj.above.palette);
		ppu.dac.math.belowColorEnable = ppu.dac.objColorEnable && ppu.obj.above.palette >= 192;
	}
	if (priority == 0) {
		ppu.dac.math.aboveColor = paletteColor(0);
		ppu.dac.math.belowColorEnable = ppu.dac.backColorEnable;
	}

	if (!ppu.window.belowColorEnable) {
		ppu.dac.math.belowColorEnable = false;
	}
	ppu.dac.math.aboveColorEnable = ppu.window.aboveColorEnable;
	if (!ppu.dac.math.belowColorEnable) {
		return ppu.dac.math.aboveColorEnable ? ppu.dac.math.aboveColor : 0;
	}

	if (ppu.dac.blendMode && ppu.dac.math.transparent) {
		ppu.dac.math.blendMode = false;
		ppu.dac.math.colorHalve = false;
	} else {
		ppu.dac.math.blendMode = ppu.dac.blendMode;
		ppu.dac.math.colorHalve = ppu.dac.colorHalve && ppu.dac.math.aboveColorEnable;
	}

	return blend(ppu.dac.math.aboveColorEnable ? ppu.dac.math.aboveColor : 0,
	             ppu.dac.math.blendMode ? ppu.dac.math.belowColor : fixedColor());
}

static void dacRun(void) {
	if (VCOUNTER == 0) {
		return;
	}

	bool hires = ppu.io.pseudoHires || ppu.io.bgMode == 5 || ppu.io.bgMode == 6;
	u16 belowColor = dacBelow(hires);
	u16 aboveColor = dacAbove();

	if (!ppu.dac.lineValid) {
		return;
	}
	u32* line = &ppuCanvas[ppu.dac.line];
	line[0] = ppu.io.displayBrightness << 15 | (hires ? belowColor : aboveColor);
	line[1] = ppu.io.displayBrightness << 15 | aboveColor;
	ppu.dac.line += 2;
	if (ppu.drawnTo < ppu.dac.line) {
		ppu.drawnTo = ppu.dac.line;
	}
}

static void dacPower(void) {
	ppu.dac.blendMode = randomNext() & 1;
	ppu.dac.directColor = randomNext() & 1;
	ppu.dac.colorMode = randomNext() & 1;
	ppu.dac.colorHalve = randomNext() & 1;
	ppu.dac.bgColorEnable[0] = randomNext() & 1;
	ppu.dac.bgColorEnable[1] = randomNext() & 1;
	ppu.dac.bgColorEnable[2] = randomNext() & 1;
	ppu.dac.bgColorEnable[3] = randomNext() & 1;
	ppu.dac.objColorEnable = randomNext() & 1;
	ppu.dac.backColorEnable = randomNext() & 1;
	ppu.dac.colorBlue = randomNext() & 31;
	ppu.dac.colorGreen = randomNext() & 31;
	ppu.dac.colorRed = randomNext() & 31;
}

// Registers

static void syncFromCpu(void) {
	if (ppu.th.clock < cpu.th.clock) {
		ppuRun();
	}
}

void ppuLatchCounters(void) {
	// The counters as they will be once the PPU is past the CPU. While that is still on this line
	// they can be told without drawing up to there, which leaves the line whole.
	if (ppu.th.clock < cpu.th.clock) {
		u64 steps = (cpu.th.clock - ppu.th.clock) / (ppu.th.scalar * 2) + 1;
		if (ppu.counter.hcounter + steps * 2 < ppu.counter.hperiod) {
			struct Counter then = ppu.counter;
			then.hcounter += steps * 2;
			ppu.io.hcounter = counterHdot(&then);
			ppu.io.vcounter = VCOUNTER;
			ppu.latch.counters = 1;
			return;
		}
	}
	syncFromCpu();
	ppu.io.hcounter = counterHdot(&ppu.counter);
	ppu.io.vcounter = VCOUNTER;
	ppu.latch.counters = 1;
}

static u16 addressVRAM(void) {
	u16 address = ppu.io.vramAddress;
	switch (ppu.io.vramMapping) {
	case 0:
		return address;
	case 1:
		return (address & 0xff00) | (address & 0x1f) << 3 | (address >> 5 & 7);
	case 2:
		return (address & 0xfe00) | (address & 0x3f) << 3 | (address >> 6 & 7);
	default:
		return (address & 0xfc00) | (address & 0x7f) << 3 | (address >> 7 & 7);
	}
}

static inline bool rendering(void) {
	return !ppu.io.displayDisable && VCOUNTER < ppu.state.vdisp;
}

static u16 readVRAM(void) {
	if (rendering()) {
		return 0x0000;
	}
	return *vramAt(addressVRAM());
}

static void writeVRAM(bool byte, u8 data) {
	if (rendering()) {
		return;
	}
	u16* word = vramAt(addressVRAM());
	*word = byte ? (*word & 0x00ff) | data << 8 : (*word & 0xff00) | data;
}

static u8 readOAM(u16 address) {
	address &= 0x3ff;
	if (rendering()) {
		if (!(address & 0x200)) {
			return oamRead(0x000 | ppu.latch.oamAddress << 2 | (address & 1));
		}
		return oamRead(0x200 | ppu.latch.oamAddress >> 2);
	}
	return oamRead(address);
}

static void writeOAM(u16 address, u8 data) {
	address &= 0x3ff;
	if (rendering()) {
		if (!(address & 0x200)) {
			oamWrite(0x000 | ppu.latch.oamAddress << 2 | (address & 1), data);
			return;
		}
		oamWrite(0x200 | ppu.latch.oamAddress >> 2, data);
		return;
	}
	oamWrite(address, data);
}

static inline bool cgramLatched(void) {
	return !ppu.io.displayDisable && VCOUNTER > 0 && VCOUNTER < ppu.state.vdisp && HCOUNTER >= 88 && HCOUNTER < 1096;
}

static u8 readCGRAM(bool byte, u8 address) {
	if (cgramLatched()) {
		address = ppu.latch.cgramAddress;
	}
	return ppu.cgram[address] >> (byte * 8);
}

static void writeCGRAM(u8 address, u16 data) {
	if (cgramLatched()) {
		address = ppu.latch.cgramAddress;
	}
	ppu.cgram[address] = data & 0x7fff;
}

static void updateVideoMode(void) {
	ppu.state.vdisp = !ppu.io.overscan ? 225 : 240;

	struct Background* bg = ppu.bg;
	u8* obj = ppu.obj.io.priority;
#define MODES(a, b, c, d) bg[0].io.mode = a, bg[1].io.mode = b, bg[2].io.mode = c, bg[3].io.mode = d
#define BG(n, lo, hi) bg[n].io.priority[0] = lo, bg[n].io.priority[1] = hi
#define OBJ(a, b, c, d) obj[0] = a, obj[1] = b, obj[2] = c, obj[3] = d
	switch (ppu.io.bgMode) {
	case 0:
		MODES(BG_BPP2, BG_BPP2, BG_BPP2, BG_BPP2);
		BG(0, 8, 11);
		BG(1, 7, 10);
		BG(2, 2, 5);
		BG(3, 1, 4);
		OBJ(3, 6, 9, 12);
		break;
	case 1:
		MODES(BG_BPP4, BG_BPP4, BG_BPP2, BG_INACTIVE);
		if (ppu.io.bgPriority) {
			BG(0, 5, 8);
			BG(1, 4, 7);
			BG(2, 1, 10);
			OBJ(2, 3, 6, 9);
		} else {
			BG(0, 6, 9);
			BG(1, 5, 8);
			BG(2, 1, 3);
			OBJ(2, 4, 7, 10);
		}
		break;
	case 2:
		MODES(BG_BPP4, BG_BPP4, BG_INACTIVE, BG_INACTIVE);
		BG(0, 3, 7);
		BG(1, 1, 5);
		OBJ(2, 4, 6, 8);
		break;
	case 3:
		MODES(BG_BPP8, BG_BPP4, BG_INACTIVE, BG_INACTIVE);
		BG(0, 3, 7);
		BG(1, 1, 5);
		OBJ(2, 4, 6, 8);
		break;
	case 4:
		MODES(BG_BPP8, BG_BPP2, BG_INACTIVE, BG_INACTIVE);
		BG(0, 3, 7);
		BG(1, 1, 5);
		OBJ(2, 4, 6, 8);
		break;
	case 5:
		MODES(BG_BPP4, BG_BPP2, BG_INACTIVE, BG_INACTIVE);
		BG(0, 3, 7);
		BG(1, 1, 5);
		OBJ(2, 4, 6, 8);
		break;
	case 6:
		MODES(BG_BPP4, BG_INACTIVE, BG_INACTIVE, BG_INACTIVE);
		BG(0, 2, 5);
		OBJ(1, 3, 4, 6);
		break;
	case 7:
		if (!ppu.io.extbg) {
			MODES(BG_MODE7, BG_INACTIVE, BG_INACTIVE, BG_INACTIVE);
			bg[0].io.priority[0] = 2;
			OBJ(1, 3, 4, 5);
		} else {
			MODES(BG_MODE7, BG_MODE7, BG_INACTIVE, BG_INACTIVE);
			bg[0].io.priority[0] = 3;
			BG(1, 1, 5);
			OBJ(2, 4, 6, 7);
		}
		break;
	}
#undef MODES
#undef BG
#undef OBJ
}

static bool sameLinePending(void);

static u8 readIO(u32 address, u8 data) {
	// The latch and the counters it latched are the same wherever the PPU has drawn to on its line.
	if (((address & 0xffff) != 0x2137 && (address & 0xffff) != 0x213c && (address & 0xffff) != 0x213d) || !sameLinePending()) {
		syncFromCpu();
	}

	switch (address & 0xffff) {
	case 0x2104: case 0x2105: case 0x2106: case 0x2108:
	case 0x2109: case 0x210a: case 0x2114: case 0x2115:
	case 0x2116: case 0x2118: case 0x2119: case 0x211a:
	case 0x2124: case 0x2125: case 0x2126: case 0x2128:
	case 0x2129: case 0x212a:
		return ppu.ppu1.mdr;

	// MPYL, MPYM, MPYH
	case 0x2134:
	case 0x2135:
	case 0x2136: {
		u32 result = (s16) ppu.io.m7a * (s8) (ppu.io.m7b >> 8);
		return ppu.ppu1.mdr = result >> ((address & 3) * 8);
	}

	// SLHV
	case 0x2137:
		if (cpu.io.pio & 0x80) {
			ppuLatchCounters();
		}
		return data;

	// OAMDATAREAD
	case 0x2138: {
		u16 oamAddress = ppu.io.oamAddress;
		ppu.io.oamAddress = (ppu.io.oamAddress + 1) & 0x3ff;
		ppu.ppu1.mdr = readOAM(oamAddress);
		objSetFirstSprite();
		return ppu.ppu1.mdr;
	}

	// VMDATALREAD
	case 0x2139:
		ppu.ppu1.mdr = ppu.latch.vram;
		if (ppu.io.vramIncrementMode == 0) {
			ppu.latch.vram = readVRAM();
			ppu.io.vramAddress += ppu.io.vramIncrementSize;
		}
		return ppu.ppu1.mdr;

	// VMDATAHREAD
	case 0x213a:
		ppu.ppu1.mdr = ppu.latch.vram >> 8;
		if (ppu.io.vramIncrementMode == 1) {
			ppu.latch.vram = readVRAM();
			ppu.io.vramAddress += ppu.io.vramIncrementSize;
		}
		return ppu.ppu1.mdr;

	// CGDATAREAD
	case 0x213b: {
		bool high = ppu.io.cgramAddressLatch;
		ppu.io.cgramAddressLatch = !high;
		if (!high) {
			ppu.ppu2.mdr = readCGRAM(0, ppu.io.cgramAddress);
		} else {
			ppu.ppu2.mdr = (ppu.ppu2.mdr & 0x80) | (readCGRAM(1, ppu.io.cgramAddress++) & 0x7f);
		}
		return ppu.ppu2.mdr;
	}

	// OPHCT
	case 0x213c: {
		bool high = ppu.latch.hcounter;
		ppu.latch.hcounter = !high;
		if (!high) {
			ppu.ppu2.mdr = ppu.io.hcounter;
		} else {
			ppu.ppu2.mdr = (ppu.ppu2.mdr & 0xfe) | (ppu.io.hcounter >> 8 & 1);
		}
		return ppu.ppu2.mdr;
	}

	// OPVCT
	case 0x213d: {
		bool high = ppu.latch.vcounter;
		ppu.latch.vcounter = !high;
		if (!high) {
			ppu.ppu2.mdr = ppu.io.vcounter;
		} else {
			ppu.ppu2.mdr = (ppu.ppu2.mdr & 0xfe) | (ppu.io.vcounter >> 8 & 1);
		}
		return ppu.ppu2.mdr;
	}

	// STAT77
	case 0x213e:
		ppu.ppu1.mdr = (ppu.ppu1.mdr & 0x10) | ppu.ppu1.version | ppu.obj.io.rangeOver << 6 | ppu.obj.io.timeOver << 7;
		return ppu.ppu1.mdr;

	// STAT78
	case 0x213f:
		ppu.latch.hcounter = 0;
		ppu.latch.vcounter = 0;
		ppu.ppu2.mdr = (ppu.ppu2.mdr & 0x20) | ppu.ppu2.version | sys.pal << 4;
		if (!(cpu.io.pio & 0x80)) {
			ppu.ppu2.mdr |= 0x40;
		} else {
			ppu.ppu2.mdr |= ppu.latch.counters << 6;
			ppu.latch.counters = 0;
		}
		ppu.ppu2.mdr |= ppu.counter.field << 7;
		return ppu.ppu2.mdr;
	}

	return data;
}

// Whether a write to VRAM can go in without the PPU drawing up to it first: while the display is
// off, or the PPU is below the picture and will still be on the same line when it is past the CPU,
// it reads nothing from VRAM, and the write isn't held back.
// Whether the PPU, brought past the CPU now, would still be on the line it is on: then nothing
// that happens at a line's start (a frame's end among them) is waiting on it.
static bool sameLinePending(void) {
	if (!(ppu.th.clock < cpu.th.clock)) {
		return true;
	}
	u64 steps = (cpu.th.clock - ppu.th.clock) / (ppu.th.scalar * 2) + 1;
	return ppu.counter.hcounter + steps * 2 < ppu.counter.hperiod;
}

static bool vramWriteNow(void) {
	return (ppu.io.displayDisable || VCOUNTER >= ppu.state.vdisp) && sameLinePending();
}

static void writeIO(u32 address, u8 data) {
	if (((address & 0xffff) != 0x2118 && (address & 0xffff) != 0x2119) || !vramWriteNow()) {
		syncFromCpu();
	}

	switch (address & 0xffff) {
	// INIDISP
	case 0x2100:
		if (ppu.io.displayDisable && VCOUNTER == ppu.state.vdisp) {
			objAddressReset();
		}
		ppu.io.displayBrightness = data & 15;
		ppu.io.displayDisable = data & 0x80;
		return;

	// OBSEL
	case 0x2101:
		ppu.obj.io.tiledataAddress = (data & 7) << 13;
		ppu.obj.io.nameselect = data >> 3 & 3;
		ppu.obj.io.baseSize = data >> 5 & 7;
		return;

	// OAMADDL
	case 0x2102:
		ppu.io.oamBaseAddress = (ppu.io.oamBaseAddress & 0x201) | data << 1;
		objAddressReset();
		return;

	// OAMADDH
	case 0x2103:
		ppu.io.oamBaseAddress = (ppu.io.oamBaseAddress & 0x1ff) | (data & 1) << 9;
		ppu.io.oamPriority = data & 0x80;
		objAddressReset();
		return;

	// OAMDATA
	case 0x2104: {
		bool latchBit = ppu.io.oamAddress & 1;
		u16 oamAddress = ppu.io.oamAddress;
		ppu.io.oamAddress = (ppu.io.oamAddress + 1) & 0x3ff;
		if (latchBit == 0) {
			ppu.latch.oam = data;
		}
		if (oamAddress & 0x200) {
			writeOAM(oamAddress, data);
		} else if (latchBit == 1) {
			writeOAM((oamAddress & ~1) + 0, ppu.latch.oam);
			writeOAM((oamAddress & ~1) + 1, data);
		}
		objSetFirstSprite();
		return;
	}

	// BGMODE
	case 0x2105:
		ppu.io.bgMode = data & 7;
		ppu.io.bgPriority = data & 0x08;
		ppu.bg[0].io.tileSize = data & 0x10;
		ppu.bg[1].io.tileSize = data & 0x20;
		ppu.bg[2].io.tileSize = data & 0x40;
		ppu.bg[3].io.tileSize = data & 0x80;
		updateVideoMode();
		return;

	// MOSAIC
	case 0x2106: {
		bool wasEnabled = mosaicEnable();
		ppu.bg[0].mosaic.enable = data & 1;
		ppu.bg[1].mosaic.enable = data & 2;
		ppu.bg[2].mosaic.enable = data & 4;
		ppu.bg[3].mosaic.enable = data & 8;
		ppu.mosaic.size = (data >> 4) + 1;
		if (!wasEnabled && mosaicEnable()) {
			// The mosaic's line counter is reloaded when it is switched on.
			ppu.mosaic.vcounter = (ppu.mosaic.size + 1) & 31;
		}
		return;
	}

	// BG1SC-BG4SC
	case 0x2107:
	case 0x2108:
	case 0x2109:
	case 0x210a: {
		struct Background* bg = &ppu.bg[(address & 0xffff) - 0x2107];
		bg->io.screenSize = data & 3;
		bg->io.screenAddress = (data >> 2) << 10;
		return;
	}

	// BG12NBA
	case 0x210b:
		ppu.bg[0].io.tiledataAddress = (data & 15) << 12;
		ppu.bg[1].io.tiledataAddress = (data >> 4) << 12;
		return;

	// BG34NBA
	case 0x210c:
		ppu.bg[2].io.tiledataAddress = (data & 15) << 12;
		ppu.bg[3].io.tiledataAddress = (data >> 4) << 12;
		return;

	// BG1HOFS
	case 0x210d:
		ppu.io.hoffsetMode7 = data << 8 | ppu.latch.mode7;
		ppu.latch.mode7 = data;
		// fall through
	// BG2HOFS, BG3HOFS, BG4HOFS
	case 0x210f:
	case 0x2111:
	case 0x2113:
		ppu.bg[((address & 0xffff) - 0x210d) >> 1].io.hoffset = data << 8 | (ppu.latch.bgofsPPU1 & ~7) | (ppu.latch.bgofsPPU2 & 7);
		ppu.latch.bgofsPPU1 = data;
		ppu.latch.bgofsPPU2 = data & 7;
		return;

	// BG1VOFS
	case 0x210e:
		ppu.io.voffsetMode7 = data << 8 | ppu.latch.mode7;
		ppu.latch.mode7 = data;
		// fall through
	// BG2VOFS, BG3VOFS, BG4VOFS
	case 0x2110:
	case 0x2112:
	case 0x2114:
		ppu.bg[((address & 0xffff) - 0x210e) >> 1].io.voffset = data << 8 | ppu.latch.bgofsPPU1;
		ppu.latch.bgofsPPU1 = data;
		return;

	// VMAIN
	case 0x2115: {
		static const u8 size[4] = { 1, 32, 128, 128 };
		ppu.io.vramIncrementSize = size[data & 3];
		ppu.io.vramMapping = data >> 2 & 3;
		ppu.io.vramIncrementMode = data & 0x80;
		return;
	}

	// VMADDL
	case 0x2116:
		ppu.io.vramAddress = (ppu.io.vramAddress & 0xff00) | data;
		ppu.latch.vram = readVRAM();
		return;

	// VMADDH
	case 0x2117:
		ppu.io.vramAddress = (ppu.io.vramAddress & 0x00ff) | data << 8;
		ppu.latch.vram = readVRAM();
		return;

	// VMDATAL
	case 0x2118:
		writeVRAM(0, data);
		if (ppu.io.vramIncrementMode == 0) {
			ppu.io.vramAddress += ppu.io.vramIncrementSize;
		}
		return;

	// VMDATAH
	case 0x2119:
		writeVRAM(1, data);
		if (ppu.io.vramIncrementMode == 1) {
			ppu.io.vramAddress += ppu.io.vramIncrementSize;
		}
		return;

	// M7SEL
	case 0x211a:
		ppu.io.hflipMode7 = data & 1;
		ppu.io.vflipMode7 = data & 2;
		ppu.io.repeatMode7 = data >> 6;
		return;

	// M7A, M7B, M7C, M7D, M7X, M7Y
	case 0x211b:
		ppu.io.m7a = data << 8 | ppu.latch.mode7;
		ppu.latch.mode7 = data;
		return;
	case 0x211c:
		ppu.io.m7b = data << 8 | ppu.latch.mode7;
		ppu.latch.mode7 = data;
		return;
	case 0x211d:
		ppu.io.m7c = data << 8 | ppu.latch.mode7;
		ppu.latch.mode7 = data;
		return;
	case 0x211e:
		ppu.io.m7d = data << 8 | ppu.latch.mode7;
		ppu.latch.mode7 = data;
		return;
	case 0x211f:
		ppu.io.m7x = data << 8 | ppu.latch.mode7;
		ppu.latch.mode7 = data;
		return;
	case 0x2120:
		ppu.io.m7y = data << 8 | ppu.latch.mode7;
		ppu.latch.mode7 = data;
		return;

	// CGADD
	case 0x2121:
		ppu.io.cgramAddress = data;
		ppu.io.cgramAddressLatch = 0;
		return;

	// CGDATA
	case 0x2122: {
		bool high = ppu.io.cgramAddressLatch;
		ppu.io.cgramAddressLatch = !high;
		if (!high) {
			ppu.latch.cgram = data;
		} else {
			writeCGRAM(ppu.io.cgramAddress++, (data & 0x7f) << 8 | ppu.latch.cgram);
		}
		return;
	}

	// W12SEL, W34SEL
	case 0x2123:
	case 0x2124: {
		struct WindowLayer* layer = &ppu.window.bg[((address & 0xffff) - 0x2123) * 2];
		layer[0].oneInvert = data & 0x01;
		layer[0].oneEnable = data & 0x02;
		layer[0].twoInvert = data & 0x04;
		layer[0].twoEnable = data & 0x08;
		layer[1].oneInvert = data & 0x10;
		layer[1].oneEnable = data & 0x20;
		layer[1].twoInvert = data & 0x40;
		layer[1].twoEnable = data & 0x80;
		return;
	}

	// WOBJSEL
	case 0x2125:
		ppu.window.obj.oneInvert = data & 0x01;
		ppu.window.obj.oneEnable = data & 0x02;
		ppu.window.obj.twoInvert = data & 0x04;
		ppu.window.obj.twoEnable = data & 0x08;
		ppu.window.col.oneInvert = data & 0x10;
		ppu.window.col.oneEnable = data & 0x20;
		ppu.window.col.twoInvert = data & 0x40;
		ppu.window.col.twoEnable = data & 0x80;
		return;

	// WH0-WH3
	case 0x2126:
		ppu.window.oneLeft = data;
		return;
	case 0x2127:
		ppu.window.oneRight = data;
		return;
	case 0x2128:
		ppu.window.twoLeft = data;
		return;
	case 0x2129:
		ppu.window.twoRight = data;
		return;

	// WBGLOG
	case 0x212a:
		ppu.window.bg[0].mask = data >> 0 & 3;
		ppu.window.bg[1].mask = data >> 2 & 3;
		ppu.window.bg[2].mask = data >> 4 & 3;
		ppu.window.bg[3].mask = data >> 6 & 3;
		return;

	// WOBJLOG
	case 0x212b:
		ppu.window.obj.mask = data >> 0 & 3;
		ppu.window.col.mask = data >> 2 & 3;
		return;

	// TM
	case 0x212c:
		for (int n = 0; n < 4; ++n) {
			ppu.bg[n].io.aboveEnable = data >> n & 1;
		}
		ppu.obj.io.aboveEnable = data & 0x10;
		return;

	// TS
	case 0x212d:
		for (int n = 0; n < 4; ++n) {
			ppu.bg[n].io.belowEnable = data >> n & 1;
		}
		ppu.obj.io.belowEnable = data & 0x10;
		return;

	// TMW
	case 0x212e:
		for (int n = 0; n < 4; ++n) {
			ppu.window.bg[n].aboveEnable = data >> n & 1;
		}
		ppu.window.obj.aboveEnable = data & 0x10;
		return;

	// TSW
	case 0x212f:
		for (int n = 0; n < 4; ++n) {
			ppu.window.bg[n].belowEnable = data >> n & 1;
		}
		ppu.window.obj.belowEnable = data & 0x10;
		return;

	// CGWSEL
	case 0x2130:
		ppu.dac.directColor = data & 1;
		ppu.dac.blendMode = data & 2;
		ppu.window.col.belowMask = data >> 4 & 3;
		ppu.window.col.aboveMask = data >> 6 & 3;
		return;

	// CGADDSUB
	case 0x2131:
		for (int n = 0; n < 4; ++n) {
			ppu.dac.bgColorEnable[n] = data >> n & 1;
		}
		ppu.dac.objColorEnable = data & 0x10;
		ppu.dac.backColorEnable = data & 0x20;
		ppu.dac.colorHalve = data & 0x40;
		ppu.dac.colorMode = data & 0x80;
		return;

	// COLDATA
	case 0x2132:
		if (data & 0x20) {
			ppu.dac.colorRed = data & 31;
		}
		if (data & 0x40) {
			ppu.dac.colorGreen = data & 31;
		}
		if (data & 0x80) {
			ppu.dac.colorBlue = data & 31;
		}
		return;

	// SETINI
	case 0x2133:
		ppu.io.interlace = data & 0x01;
		ppu.obj.io.interlace = data & 0x02;
		ppu.io.overscan = data & 0x04;
		ppu.io.pseudoHires = data & 0x08;
		ppu.io.extbg = data & 0x40;
		updateVideoMode();
		return;
	}
}

void ppuMap(void) {
	busMap(readIO, writeIO, "00-3f,80-bf:2100-213f", 0, 0, 0);
}

// The line

// What a line starts with, at H = 0.
static void lineStart(void) {
	ppu.blankFrom = BLANK_NONE;
	if (VCOUNTER == 0) {
		ppu.state.interlace = ppu.io.interlace;
		ppu.state.overscan = ppu.io.overscan;
		ppu.obj.io.timeOver = false;
		ppu.obj.io.rangeOver = false;
	}

	mosaicScanline();
	for (int n = 0; n < 4; ++n) {
		bgScanline(&ppu.bg[n]);
	}
	objScanline();
	windowScanline();
	dacScanline();

	if (VCOUNTER == 240) {
		// The frame is done. The next one starts on a black canvas, so lines it doesn't draw stay black.
		videoFrame(ppuCanvas);
		memset(ppuCanvas, 0, ppu.drawnTo * sizeof(u32));
		ppu.drawnTo = 0;
		systemFrameEvent();
	}
}

// What each background mode fetches in the eight slots of a tile's 32 clocks: what, for which
// background, and the fetch's arguments.
enum { FETCH_NONE, FETCH_NAME, FETCH_OFFSET, FETCH_CHARACTER };
struct Fetch {
	u8 kind;
	u8 bg;
	u8 a;
	u8 b;
};
static const struct Fetch fetches[8][8] = {
	{ { FETCH_NAME, 3, 0, 0 }, { FETCH_NAME, 2, 0, 0 }, { FETCH_NAME, 1, 0, 0 }, { FETCH_NAME, 0, 0, 0 }, { FETCH_CHARACTER, 3, 0, 0 }, { FETCH_CHARACTER, 2, 0, 0 }, { FETCH_CHARACTER, 1, 0, 0 }, { FETCH_CHARACTER, 0, 0, 0 } },
	{ { FETCH_NAME, 2, 0, 0 }, { FETCH_NAME, 1, 0, 0 }, { FETCH_NAME, 0, 0, 0 }, { FETCH_CHARACTER, 2, 0, 0 }, { FETCH_CHARACTER, 1, 0, 0 }, { FETCH_CHARACTER, 1, 1, 0 }, { FETCH_CHARACTER, 0, 0, 0 }, { FETCH_CHARACTER, 0, 1, 0 } },
	{ { FETCH_NAME, 1, 0, 0 }, { FETCH_NAME, 0, 0, 0 }, { FETCH_OFFSET, 2, 0, 0 }, { FETCH_OFFSET, 2, 8, 0 }, { FETCH_CHARACTER, 1, 0, 0 }, { FETCH_CHARACTER, 1, 1, 0 }, { FETCH_CHARACTER, 0, 0, 0 }, { FETCH_CHARACTER, 0, 1, 0 } },
	{ { FETCH_NAME, 1, 0, 0 }, { FETCH_NAME, 0, 0, 0 }, { FETCH_CHARACTER, 1, 0, 0 }, { FETCH_CHARACTER, 1, 1, 0 }, { FETCH_CHARACTER, 0, 0, 0 }, { FETCH_CHARACTER, 0, 1, 0 }, { FETCH_CHARACTER, 0, 2, 0 }, { FETCH_CHARACTER, 0, 3, 0 } },
	{ { FETCH_NAME, 1, 0, 0 }, { FETCH_NAME, 0, 0, 0 }, { FETCH_OFFSET, 2, 0, 0 }, { FETCH_CHARACTER, 1, 0, 0 }, { FETCH_CHARACTER, 0, 0, 0 }, { FETCH_CHARACTER, 0, 1, 0 }, { FETCH_CHARACTER, 0, 2, 0 }, { FETCH_CHARACTER, 0, 3, 0 } },
	{ { FETCH_NAME, 1, 0, 0 }, { FETCH_NAME, 0, 0, 0 }, { FETCH_CHARACTER, 1, 0, 0 }, { FETCH_CHARACTER, 1, 0, 1 }, { FETCH_CHARACTER, 0, 0, 0 }, { FETCH_CHARACTER, 0, 1, 0 }, { FETCH_CHARACTER, 0, 0, 1 }, { FETCH_CHARACTER, 0, 1, 1 } },
	{ { FETCH_NAME, 1, 0, 0 }, { FETCH_NAME, 0, 0, 0 }, { FETCH_OFFSET, 2, 0, 0 }, { FETCH_OFFSET, 2, 8, 0 }, { FETCH_CHARACTER, 0, 0, 0 }, { FETCH_CHARACTER, 0, 1, 0 }, { FETCH_CHARACTER, 0, 0, 1 }, { FETCH_CHARACTER, 0, 1, 1 } },
	// Mode 7 fetches as it draws.
	{ { 0 } },
};

static void doFetch(const struct Fetch* fetch, u16 optH, u16 optV) {
	struct Background* bg = &ppu.bg[fetch->bg];
	switch (fetch->kind) {
	case FETCH_NAME:
		bgFetchNameTable(bg, optH, optV);
		break;
	case FETCH_OFFSET:
		bgFetchOffset(bg, fetch->a);
		break;
	case FETCH_CHARACTER:
		bgFetchCharacter(bg, fetch->a, fetch->b);
		break;
	}
}

// The dots from H = start to H = end of a line that draws, with no write from outside between them.
//
// In the hardware each two-clock slot does a little of everything: a sprite is evaluated, a tile
// fetched, the backgrounds shift out a pixel, and the window and color math put one together. Here
// each of those is done for the whole span on its own, which gives the same results: the
// backgrounds share nothing but BG3's offset-per-tile values, which are noted as the other two fetch
// with them, and what the window does to a background's pixel is gone by its next one.
//
// A slot at H = 56 + 4x shifts out the sub screen's pixel x, the one at 58 + 4x the main screen's,
// and puts pixel x together.
#if defined(ARMSNES_DOT)
// What the backgrounds do in the two-clock slot at H = at: a fetch, the first tile's start, a pixel
// shifted out for one screen or the other.
static void bgSlot(u32 at) {
	if (at <= 1054 && at % 4 == 0) {
		const struct Fetch* fetch = &fetches[ppu.io.bgMode][at / 4 & 7];
		if (fetch->kind != FETCH_NONE) {
			doFetch(fetch, ppu.bg[2].opt.hoffset, ppu.bg[2].opt.voffset);
		}
	}
	if (at == 56) {
		for (int n = 0; n < 4; ++n) {
			bgBegin(&ppu.bg[n]);
		}
	}
	if (at >= 56 && at <= 1078) {
		for (int n = 0; n < 4; ++n) {
			bgRun(&ppu.bg[n], (at - 56) % 4 == 0);
		}
	}
}

// The dots one at a time, as the hardware does them: the reference the spans are checked against.
static void renderSpan(u32 start, u32 end) {
	for (u32 at = start; at < end; at += 2) {
		ppu.counter.hcounter = at;
		if (at <= 1016 && at % 8 == 0) {
			objEvaluate(at >> 3);
		}
		// Below the picture nothing is fetched or shifted: the backgrounds stay as its last line
		// left them. (ares goes on fetching to line 240.) Nor while the display is off; if it comes
		// back on before the line ends, they are brought up to here first, from VRAM as it is now.
		bool tiled = VCOUNTER < ppu.state.vdisp;
		if (tiled && ppu.io.displayDisable) {
			if (ppu.blankFrom == BLANK_NONE) {
				ppu.blankFrom = at;
			}
			tiled = false;
		} else if (tiled && ppu.blankFrom != BLANK_NONE) {
			for (u32 missed = ppu.blankFrom; missed < at; missed += 2) {
				ppu.counter.hcounter = missed;
				bgSlot(missed);
			}
			ppu.counter.hcounter = at;
			ppu.blankFrom = BLANK_NONE;
		}
		if (tiled) {
			bgSlot(at);
		}
		if (at >= 56 && at <= 1078) {
			if ((at - 56) % 4 != 0) {
				objRun();
				windowRun();
				dacRun();
			}
		}
	}
	ppu.counter.hcounter = start;
}
#else
static struct BgPixel spanAbove[4][256];
static struct BgPixel spanBelow[4][256];
// A whole line of a background, a value to a pixel: the palette entry, the palette group above it and
// the priority above that, or 0 where the background is clear.
static u32 linePixels[4][256];
// In the double-width mode a background's main and sub screen pixels are different ones: these are
// the sub screen's, and linePixels the main screen's, for a background whose lineWide is set.
static u32 linePixelsSub[4][256];
static bool lineWide[4];

static inline const u32* lineSub(u32 n) {
	return lineWide[n] ? linePixelsSub[n] : linePixels[n];
}

// What a background's main and sub screen pixels are left as after such a line: the priority of the
// last pixel, and the palette of the last one that wasn't clear.
static void lineOutput(struct Background* bg, u32 n) {
	bg->output.above.priority = 0;
	bg->output.below.priority = 0;
	const u32* pixels = linePixels[n];
	s32 x = 255;
	while (x >= 0 && !pixels[x]) {
		--x;
	}
	if (x >= 0 && bg->io.aboveEnable) {
		bg->output.above.palette = pixels[x];
		bg->output.above.paletteGroup = pixels[x] >> 8;
		bg->output.above.priority = pixels[255] >> 16;
	}
	pixels = lineSub(n);
	x = 255;
	while (x >= 0 && !pixels[x]) {
		--x;
	}
	if (x >= 0 && bg->io.belowEnable) {
		bg->output.below.palette = pixels[x];
		bg->output.below.paletteGroup = pixels[x] >> 8;
		bg->output.below.priority = pixels[255] >> 16;
	}
}

// The same line as the slots leave it, a main and a sub screen pixel at a time, for a line that is
// put together slot by slot.
static void lineToSlots(u32 n) {
	struct Background* bg = &ppu.bg[n];
	struct BgPixel above = bg->output.above;
	struct BgPixel below = bg->output.below;
	const u32* sub = lineSub(n);
	for (u32 x = 0; x < 256; ++x) {
		u32 value = linePixels[n][x];
		u32 subValue = sub[x];
		above.priority = 0;
		below.priority = 0;
		if (value && bg->io.aboveEnable) {
			above = (struct BgPixel) { value >> 16, value, value >> 8 };
		}
		if (subValue && bg->io.belowEnable) {
			below = (struct BgPixel) { subValue >> 16, subValue, subValue >> 8 };
		}
		spanAbove[n][x] = above;
		spanBelow[n][x] = below;
	}
}
static u16 spanOptH[540];
static u16 spanOptV[540];

// One background over a span, slot by slot as the dots have it.
static void bgSpanSlots(u32 n, u32 start, u32 end, bool hires, bool draws) {
	const struct Fetch* modeFetches = fetches[ppu.io.bgMode];
	struct Background* bg = &ppu.bg[n];
	for (u32 at = start; at < end; at += 2) {
		if (!(at & 2)) {
			if (at <= 1054) {
				const struct Fetch* fetch = &modeFetches[at / 4 & 7];
				if (n == 2 && fetch->kind == FETCH_NAME) {
					spanOptH[at >> 1] = bg->opt.hoffset;
					spanOptV[at >> 1] = bg->opt.voffset;
				}
				if (fetch->kind != FETCH_NONE && fetch->bg == n) {
					ppu.counter.hcounter = at;
					doFetch(fetch, spanOptH[at >> 1], spanOptV[at >> 1]);
				}
			}
			if (at == 56) {
				bgBegin(bg);
			}
			if (at >= 56 && at <= 1078 && draws) {
				bgRunAt(bg, 1, hires, at);
			}
		} else if (at >= 58 && at <= 1078) {
			if (draws) {
				bgRunAt(bg, 0, hires, at);
			}
			spanAbove[n][(at - 58) >> 2] = bg->output.above;
			spanBelow[n][(at - 58) >> 2] = bg->output.below;
		}
	}
}

// A whole line of a tiled background in a mode without offset-per-tile or the double width, with
// nothing written while it is drawn: its 33 tiles are fetched and shifted out a tile at a time. It
// leaves the background as the slots would, tile for tile.
// A bitplane's eight bits as a tile's shift register has them: two bits to a pixel, the leftmost
// pixel lowest. [0] is a tile as it is, [1] one mirrored.
static u16 planeSpread[2][256];

// A bitplane's eight bits a byte each, the leftmost pixel lowest, for a tile's eight colors at once.
static u64 planeBytes[2][256];
// A line's 33 tiles' colors, a byte to a pixel, the leftmost lowest.
static u64 tileColors[33];

static void planeSpreadInit(void) {
	for (u32 byte = 0; byte < 256; ++byte) {
		u16 plain = 0, mirrored = 0;
		for (u32 bit = 0; bit < 8; ++bit) {
			if (byte >> bit & 1) {
				plain |= 1 << ((7 - bit) * 2);
				mirrored |= 1 << (bit * 2);
			}
		}
		planeSpread[0][byte] = plain;
		planeSpread[1][byte] = mirrored;
		u64 plainBytes = 0, mirroredBytes = 0;
		for (u32 bit = 0; bit < 8; ++bit) {
			if (byte >> bit & 1) {
				plainBytes |= 1ull << ((7 - bit) * 8);
				mirroredBytes |= 1ull << (bit * 8);
			}
		}
		planeBytes[0][byte] = plainBytes;
		planeBytes[1][byte] = mirroredBytes;
	}
}

// BG3's offset-per-tile values as each of a line's 33 tiles is fetched: a tile gets the ones read
// while the tile before it was fetched, and the first what the line started with.
static u16 lineOptH[34];
static u16 lineOptV[34];

// A whole line's offset fetches in modes 2 and 4, as bgFetchOffset makes them.
static void bgLineOffsets(void) {
	struct Background* bg = &ppu.bg[2];
	u32 tiles = 3 + bg->io.tileSize;
	u32 hscreen = bg->io.screenSize & 1 ? 32 << 5 : 0;
	u32 vscreen = bg->io.screenSize & 2 ? 32 << (5 + (bg->io.screenSize & 1)) : 0;
	for (u32 k = 0; k < 33; ++k) {
		lineOptH[k] = bg->opt.hoffset;
		lineOptV[k] = bg->opt.voffset;
		for (u32 y = 0; y <= (ppu.io.bgMode == 2 ? 8u : 0u); y += 8) {
			u32 hoffset = (k << 3) + (bg->io.hoffset & ~7);
			u32 voffset = y + bg->io.voffset;
			u32 htile = hoffset >> tiles;
			u32 vtile = voffset >> tiles;
			u16 offset = (htile & 31) | (vtile & 31) << 5;
			if (htile & 0x20) {
				offset += hscreen;
			}
			if (vtile & 0x20) {
				offset += vscreen;
			}
			u16 value = *vramAt((u16) (bg->io.screenAddress + offset));
			if (y == 0) {
				bg->opt.hoffset = value;
			} else {
				bg->opt.voffset = value;
			}
		}
	}
}

static void bgLineTiles(u32 n, bool fetch, bool opt) {
	struct Background* bg = &ppu.bg[n];
	lineWide[n] = false;
	u32 mode = bg->io.mode;
	u32 planes = 1u << mode;

	u32 vpixel = VCOUNTER;
	if (bg->mosaic.enable) {
		vpixel -= mosaicVoffset();
	}
	u32 hscroll = bg->io.hoffset;
	u32 vscroll = bg->io.voffset;
	u32 hsize = 256u << bg->io.tileSize << (bg->io.screenSize & 1);
	u32 vsize = 256u << bg->io.tileSize << (bg->io.screenSize >> 1 & 1);
	u32 tiles = 3 + bg->io.tileSize;
	u32 hscreen = bg->io.screenSize & 1 ? 32 << 5 : 0;
	u32 vscreen = bg->io.screenSize & 2 ? 32 << (5 + (bg->io.screenSize & 1)) : 0;
	u32 characterMask = ppu.vramMask >> (3 + mode);
	u32 characterIndex = bg->io.tiledataAddress >> (3 + mode);
	u32 paletteOffset = ppu.io.bgMode == 0 ? bg->id << 5 : 0;
	u32 paletteSize = 2 << mode;

	// The fetches, as bgFetchNameTable and bgFetchCharacter make them. A background the mode doesn't
	// use fetches nothing: it shifts out what its tiles were left with.
	for (u32 k = 0; fetch && k < 33; ++k) {
		u32 hpixel = k << 3;
		u32 hoffset = hpixel + hscroll;
		u32 voffset = vpixel + vscroll;
		if (opt) {
			u32 hlookup = lineOptH[k];
			u32 vlookup = lineOptV[k];
			u32 valid = 13 + bg->id;
			if (ppu.io.bgMode == 4) {
				if (hlookup >> valid & 1) {
					if (!(hlookup & 0x8000)) {
						hoffset = hpixel + (hlookup & ~7) + (hscroll & 7);
					} else {
						voffset = vpixel + vlookup;
					}
				}
			} else {
				if (hlookup >> valid & 1) {
					hoffset = hpixel + (hlookup & ~7) + (hscroll & 7);
				}
				if (vlookup >> valid & 1) {
					voffset = vpixel + vlookup;
				}
			}
		}
		hoffset &= hsize - 1;
		voffset &= vsize - 1;
		u32 htile = hoffset >> tiles;
		u32 vtile = voffset >> tiles;
		u16 offset = (htile & 31) | (vtile & 31) << 5;
		if (htile & 0x20) {
			offset += hscreen;
		}
		if (vtile & 0x20) {
			offset += vscreen;
		}
		u16 attributes = *vramAt((u16) (bg->io.screenAddress + offset));

		struct BgTile* tile = &bg->tiles[k];
		tile->character = attributes & 0x3ff;
		tile->paletteGroup = attributes >> 10 & 7;
		tile->priority = bg->io.priority[attributes >> 13 & 1];
		tile->hmirror = attributes >> 14 & 1;
		tile->vmirror = attributes >> 15 & 1;
		if (tiles == 4 && (bool) (hoffset & 8) != tile->hmirror) {
			tile->character = (tile->character + 1) & 0x3ff;
		}
		if (tiles == 4 && (bool) (voffset & 8) != tile->vmirror) {
			tile->character = (tile->character + 16) & 0x3ff;
		}
		u16 origin = (tile->character + characterIndex) & characterMask;
		if (tile->vmirror) {
			voffset ^= 7;
		}
		tile->address = (origin << (3 + mode)) + (voffset & 7);
		tile->palette = paletteOffset + (tile->paletteGroup << paletteSize);

		const u16* spread = planeSpread[tile->hmirror];
		const u64* bytes = planeBytes[tile->hmirror];
		u64 colors = 0;
		for (u32 index = 0; index < planes; ++index) {
			u16 data = *vramAt(tile->address + (index << 3));
			tile->data[index] = spread[data & 0xff] | spread[data >> 8] << 1;
			colors |= (bytes[data & 0xff] | bytes[data >> 8] << 1) << (index * 2);
		}
		tileColors[k] = colors;
	}

	bgBegin(bg);

	bool aboveEnable = bg->io.aboveEnable;
	bool belowEnable = bg->io.belowEnable;
	bool mosaic = bg->mosaic.enable;
	u32 mosaicSize = ppu.mosaic.size;

	if (!mosaic && fetch) {
		// Without the mosaic a pixel is its tile's color at that place: the first tile's after what
		// is scrolled off, then eight to a tile. The mosaic's counter still runs: it takes a pixel
		// at 0 and every mosaicSize after, and the last one it took is kept.
		u32 lastTaken = mosaicSize ? 255 - 255 % mosaicSize : 0;
		u32 scrolled = bg->pixelCounter;
		bool shown = aboveEnable | belowEnable;
		u32* out = linePixels[n];
		for (u32 x = 0, k = 0; x < 256; ++k) {
			const struct BgTile* tile = &bg->tiles[k];
			u32 first = k ? 0 : scrolled;
			u32 count = 8 - first;
			if (count > 256 - x) {
				count = 256 - x;
			}
			u64 colors = tileColors[k] >> (first * 8);
			u32 base = tile->paletteGroup << 8 | tile->priority << 16;
			u8 palette = tile->palette;
			if (lastTaken >= x && lastTaken < x + count) {
				u8 color = colors >> ((lastTaken - x) * 8);
				bg->mosaic.pixel.priority = tile->priority;
				bg->mosaic.pixel.palette = color ? palette + color : 0;
				bg->mosaic.pixel.paletteGroup = tile->paletteGroup;
			}
			if (!colors || !shown) {
				for (u32 i = 0; i < count; ++i) {
					out[x + i] = 0;
				}
			} else {
				for (u32 i = 0; i < count; ++i) {
					u8 color = colors >> (i * 8);
					u8 entry = palette + color;
					out[x + i] = color && entry ? base | entry : 0;
				}
			}
			x += count;
		}
		// The tiles as shifting them out leaves them: empty, but for the last one's pixels past the
		// screen's edge.
		for (u32 k = 0; k < 32; ++k) {
			memset(bg->tiles[k].data, 0, sizeof(bg->tiles[k].data));
		}
		for (u32 i = 0; i < 4; ++i) {
			bg->tiles[32].data[i] >>= scrolled * 2;
		}
		bg->renderingIndex = (bg->renderingIndex + ((scrolled + 256) >> 3)) & 127;
		bg->mosaic.hcounter = mosaicSize ? mosaicSize - 255 % mosaicSize : (u16) (0 - 255);
		lineOutput(bg, n);
		return;
	}

	if (!mosaic) {
		// A background the mode doesn't use: a pixel is what its tile shifts out. The mosaic's counter still runs: it
		// takes a pixel at 0 and every mosaicSize after, and the last one it took is kept.
		u32 lastTaken = mosaicSize ? 255 - 255 % mosaicSize : 0;
		u32 index = bg->renderingIndex;
		u32 x = 0;
		u32 pixels = 8 - bg->pixelCounter;
		while (x < 256) {
			struct BgTile* tile = &bg->tiles[index];
			u32 d0 = tile->data[0], d1 = tile->data[1], d2 = tile->data[2], d3 = tile->data[3];
			u32 count = 256 - x < pixels ? 256 - x : pixels;
			u8 priority = tile->priority, palette = tile->palette, group = tile->paletteGroup;
			if ((d0 | d1 | d2 | d3) == 0 || !(aboveEnable | belowEnable)) {
				// Nothing to see: clear, or on neither screen.
				for (u32 i = 0; i < count; ++i) {
					linePixels[n][x + i] = 0;
				}
				if (lastTaken >= x && lastTaken < x + count) {
					u32 shift = (lastTaken - x) * 2;
					u32 color = (d0 >> shift & 3) | (mode >= BG_BPP4 ? (d1 >> shift & 3) << 2 : 0) |
					            (mode >= BG_BPP8 ? (d2 >> shift & 3) << 4 | (d3 >> shift & 3) << 6 : 0);
					bg->mosaic.pixel.priority = priority;
					bg->mosaic.pixel.palette = color ? palette + color : 0;
					bg->mosaic.pixel.paletteGroup = group;
				}
				d0 >>= count * 2;
				d1 >>= count * 2;
				d2 >>= count * 2;
				d3 >>= count * 2;
			} else {
				for (u32 i = 0; i < count; ++i) {
					u32 color = d0 & 3;
					if (mode >= BG_BPP4) {
						color |= (d1 & 3) << 2;
					}
					if (mode >= BG_BPP8) {
						color |= (d2 & 3) << 4 | (d3 & 3) << 6;
					}
					d0 >>= 2;
					d1 >>= 2;
					d2 >>= 2;
					d3 >>= 2;
					u8 out = color ? palette + color : 0;
					if (x + i == lastTaken) {
						bg->mosaic.pixel.priority = priority;
						bg->mosaic.pixel.palette = out;
						bg->mosaic.pixel.paletteGroup = group;
					}
					linePixels[n][x + i] = out ? out | group << 8 | priority << 16 : 0;
				}
			}
			tile->data[0] = d0;
			tile->data[1] = d1;
			tile->data[2] = d2;
			tile->data[3] = d3;
			x += count;
			if (count == pixels) {
				index = (index + 1) & 127;
			}
			pixels = 8;
		}
		bg->renderingIndex = index;
		bg->pixelCounter = (bg->pixelCounter + 256) & 7;
		bg->mosaic.hcounter = mosaicSize ? mosaicSize - 255 % mosaicSize : (u16) (0 - 255);
		lineOutput(bg, n);
		return;
	}
	u32 mosaicCounter = bg->mosaic.hcounter;
	struct BgPixel mosaicPixel = bg->mosaic.pixel;
	u32 index = bg->renderingIndex;
	u32 counter = bg->pixelCounter;
	struct BgTile* tile = &bg->tiles[index];
	u32 d0 = tile->data[0], d1 = tile->data[1], d2 = tile->data[2], d3 = tile->data[3];

	for (u32 x = 0; x < 256; ++x) {
		u32 color = d0 & 3;
		if (mode >= BG_BPP4) {
			color |= (d1 & 3) << 2;
		}
		if (mode >= BG_BPP8) {
			color |= (d2 & 3) << 4 | (d3 & 3) << 6;
		}
		d0 >>= 2;
		d1 >>= 2;
		d2 >>= 2;
		d3 >>= 2;

		struct BgPixel pixel;
		pixel.priority = tile->priority;
		pixel.palette = color ? tile->palette + color : 0;
		pixel.paletteGroup = tile->paletteGroup;

		counter = (counter + 1) & 7;
		if (counter == 0) {
			tile->data[0] = d0;
			tile->data[1] = d1;
			tile->data[2] = d2;
			tile->data[3] = d3;
			index = (index + 1) & 127;
			tile = &bg->tiles[index];
			d0 = tile->data[0];
			d1 = tile->data[1];
			d2 = tile->data[2];
			d3 = tile->data[3];
		}

		if (x == 0 || --mosaicCounter == 0) {
			mosaicCounter = mosaicSize;
			mosaicPixel = pixel;
		} else if (mosaic) {
			pixel = mosaicPixel;
		}

		linePixels[n][x] = pixel.palette && (aboveEnable | belowEnable) ? pixel.palette | pixel.paletteGroup << 8 | pixel.priority << 16 : 0;
	}

	tile->data[0] = d0;
	tile->data[1] = d1;
	tile->data[2] = d2;
	tile->data[3] = d3;
	bg->renderingIndex = index;
	bg->pixelCounter = counter;
	bg->mosaic.hcounter = mosaicCounter;
	bg->mosaic.pixel = mosaicPixel;
	lineOutput(bg, n);
}

// A whole line of a background in mode 5, where the picture is twice as wide: 66 tiles sixteen
// pixels wide are fetched, two in the time of one, and each column of the screen takes two pixels,
// the first for the sub screen and the second for the main. A background the mode doesn't use
// fetches nothing and shifts out what its tiles were left with.
static void bgLineTilesWide(u32 n, bool fetch) {
	struct Background* bg = &ppu.bg[n];
	lineWide[n] = true;
	u32 mode = bg->io.mode;
	u32 planes = 1u << mode;

	if (fetch) {
		u32 vpixel = VCOUNTER;
		if (ppu.io.interlace) {
			vpixel = vpixel << 1 | (ppu.counter.field && !bg->mosaic.enable);
		}
		if (bg->mosaic.enable) {
			vpixel -= mosaicVoffset() << ppu.io.interlace;
		}
		u32 hscroll = bg->io.hoffset << 1;
		u32 vscroll = bg->io.voffset;
		u32 hsize = 512u << bg->io.tileSize << (bg->io.screenSize & 1);
		u32 vsize = 512u << bg->io.tileSize << (bg->io.screenSize >> 1 & 1);
		u32 vtiles = 3 + bg->io.tileSize;
		u32 hscreen = bg->io.screenSize & 1 ? 32 << 5 : 0;
		u32 vscreen = bg->io.screenSize & 2 ? 32 << (5 + (bg->io.screenSize & 1)) : 0;
		u32 characterMask = ppu.vramMask >> (3 + mode);
		u32 characterIndex = bg->io.tiledataAddress >> (3 + mode);
		u32 paletteSize = 2 << mode;
		for (u32 k = 0; k < 66; ++k) {
			u32 hoffset = ((k << 3) + hscroll) & (hsize - 1);
			u32 voffset = (vpixel + vscroll) & (vsize - 1);
			u32 htile = hoffset >> 4;
			u32 vtile = voffset >> vtiles;
			u16 offset = (htile & 31) | (vtile & 31) << 5;
			if (htile & 0x20) {
				offset += hscreen;
			}
			if (vtile & 0x20) {
				offset += vscreen;
			}
			u16 attributes = *vramAt((u16) (bg->io.screenAddress + offset));

			struct BgTile* tile = &bg->tiles[k];
			tile->character = attributes & 0x3ff;
			tile->paletteGroup = attributes >> 10 & 7;
			tile->priority = bg->io.priority[attributes >> 13 & 1];
			tile->hmirror = attributes >> 14 & 1;
			tile->vmirror = attributes >> 15 & 1;
			if ((bool) (hoffset & 8) != tile->hmirror) {
				tile->character = (tile->character + 1) & 0x3ff;
			}
			if (vtiles == 4 && (bool) (voffset & 8) != tile->vmirror) {
				tile->character = (tile->character + 16) & 0x3ff;
			}
			u16 origin = (tile->character + characterIndex) & characterMask;
			if (tile->vmirror) {
				voffset ^= 7;
			}
			tile->address = (origin << (3 + mode)) + (voffset & 7);
			tile->palette = tile->paletteGroup << paletteSize;

			const u16* spread = planeSpread[tile->hmirror];
			for (u32 index = 0; index < planes; ++index) {
				u16 data = *vramAt(tile->address + (index << 3));
				tile->data[index] = spread[data & 0xff] | spread[data >> 8] << 1;
			}
		}
	}

	bgBegin(bg);

	bool mosaic = bg->mosaic.enable;
	u32 mosaicSize = ppu.mosaic.size;
	u32 mosaicCounter = bg->mosaic.hcounter;
	struct BgPixel mosaicPixel = bg->mosaic.pixel;
	u32 index = bg->renderingIndex;
	u32 counter = bg->pixelCounter;
	struct BgTile* tile = &bg->tiles[index];
	u32 d0 = tile->data[0], d1 = tile->data[1], d2 = tile->data[2], d3 = tile->data[3];

	for (u32 at = 0; at < 512; ++at) {
		u32 color = d0 & 3;
		if (mode >= BG_BPP4) {
			color |= (d1 & 3) << 2;
		}
		if (mode >= BG_BPP8) {
			color |= (d2 & 3) << 4 | (d3 & 3) << 6;
		}
		d0 >>= 2;
		d1 >>= 2;
		d2 >>= 2;
		d3 >>= 2;

		struct BgPixel pixel;
		pixel.priority = tile->priority;
		pixel.palette = color ? tile->palette + color : 0;
		pixel.paletteGroup = tile->paletteGroup;

		counter = (counter + 1) & 7;
		if (counter == 0) {
			tile->data[0] = d0;
			tile->data[1] = d1;
			tile->data[2] = d2;
			tile->data[3] = d3;
			index = (index + 1) & 127;
			tile = &bg->tiles[index];
			d0 = tile->data[0];
			d1 = tile->data[1];
			d2 = tile->data[2];
			d3 = tile->data[3];
		}

		// The mosaic counts columns, on the sub screen's pixel.
		u32 x = at >> 1;
		bool sub = !(at & 1);
		if (sub && (x == 0 || (mosaicCounter = (u16) (mosaicCounter - 1)) == 0)) {
			mosaicCounter = mosaicSize;
			mosaicPixel = pixel;
		} else if (mosaic) {
			pixel = mosaicPixel;
		}

		u32 value = pixel.palette ? pixel.palette | pixel.paletteGroup << 8 | pixel.priority << 16 : 0;
		if (sub) {
			linePixelsSub[n][x] = value;
		} else {
			linePixels[n][x] = value;
		}
	}

	tile->data[0] = d0;
	tile->data[1] = d1;
	tile->data[2] = d2;
	tile->data[3] = d3;
	bg->renderingIndex = index;
	bg->pixelCounter = counter;
	bg->mosaic.hcounter = mosaicCounter;
	bg->mosaic.pixel = mosaicPixel;
	lineOutput(bg, n);
}

// A whole line of a mode 7 background, as bgRunMode7 makes each pixel of it: what doesn't change
// along the line is worked out once.
static void bgLineMode7(u32 n) {
	struct Background* bg = &ppu.bg[n];
	lineWide[n] = false;
	bgBegin(bg);

	s32 a = (s16) ppu.io.m7a;
	s32 b = (s16) ppu.io.m7b;
	s32 c = (s16) ppu.io.m7c;
	s32 d = (s16) ppu.io.m7d;
	s32 hcenter = signedBits(ppu.io.m7x, 13);
	s32 vcenter = signedBits(ppu.io.m7y, 13);
	s32 hoffset = signedBits(ppu.io.hoffsetMode7, 13);
	s32 voffset = signedBits(ppu.io.voffsetMode7, 13);

	u32 y = VCOUNTER;
	if (ppu.bg[0].mosaic.enable) {
		y -= mosaicVoffset();
	}
	if (ppu.io.vflipMode7) {
		y = 255 - y;
	}
#define CLIP(n) ((n) & 0x2000 ? ((n) | ~1023) : ((n) & 1023))
	s32 hclip = CLIP(hoffset - hcenter);
	s32 vclip = CLIP(voffset - vcenter);
#undef CLIP
	u32 originX = ((u32) (a * hclip) & ~63) + ((u32) (b * vclip) & ~63) + (((u32) b * y) & ~63) + ((u32) hcenter << 8);
	u32 originY = ((u32) (c * hclip) & ~63) + ((u32) (d * vclip) & ~63) + (((u32) d * y) & ~63) + ((u32) vcenter << 8);

	bool mosaic = bg->mosaic.enable;
	bool hflip = ppu.io.hflipMode7;
	u32 repeat = ppu.io.repeatMode7;
	bool second = bg->id == 1;
	u32 mosaicOffset = bg->mosaic.hoffset;
	u32 mosaicCounter = bg->mosaic.hcounter;
	u32 mosaicSize = ppu.mosaic.size;

	for (u32 at = 0; at < 256; ++at) {
		u32 x = mosaicOffset;
		if (!mosaic) {
			mosaicOffset = (u16) (mosaicOffset + 1);
		} else {
			mosaicCounter = (u16) (mosaicCounter - 1);
			if (mosaicCounter == 0) {
				mosaicCounter = mosaicSize;
				mosaicOffset = (u16) (mosaicOffset + mosaicSize);
			}
		}
		if (hflip) {
			x = 255 - x;
		}

		s32 pixelX = (originX + (u32) a * x) >> 8;
		s32 pixelY = (originY + (u32) c * x) >> 8;
		u16 paletteAddress = (pixelY & 7) << 3 | (pixelX & 7);
		u8 tileX = pixelX >> 3 & 127;
		u8 tileY = pixelY >> 3 & 127;
		u16 tileAddress = tileY << 7 | tileX;
		bool outOfBounds = (pixelX | pixelY) & ~1023;
		u8 tile = repeat == 3 && outOfBounds ? 0 : *vramAt(tileAddress) >> 0;
		u8 palette = repeat == 2 && outOfBounds ? 0 : *vramAt(tile << 6 | paletteAddress) >> 8;

		u32 priority = bg->io.priority[0];
		if (second) {
			priority = bg->io.priority[palette >> 7];
			palette &= 0x7f;
		}

		linePixels[n][at] = palette ? palette | priority << 16 : 0;
	}

	bg->mosaic.hoffset = mosaicOffset;
	bg->mosaic.hcounter = mosaicCounter;
	lineOutput(bg, n);
}

static inline u16 blendWith(u32 x, u32 y, bool subtract, bool halve) {
	if (!subtract) {
		if (!halve) {
			u32 sum = x + y;
			u32 carry = (sum - ((x ^ y) & 0x0421)) & 0x8420;
			return ((sum - carry) | (carry - (carry >> 5))) & 0x7fff;
		}
		return ((x + y - ((x ^ y) & 0x0421)) >> 1) & 0x7fff;
	}
	u32 diff = x - y + 0x8420;
	u32 borrow = (diff - ((x ^ y) & 0x8420)) & 0x8420;
	if (!halve) {
		return ((diff - borrow) & (borrow - (borrow >> 5))) & 0x7fff;
	}
	return ((((diff - borrow) & (borrow - (borrow >> 5))) & 0x7bde) >> 1) & 0x7fff;
}

// A whole line put together, 256 pixels wide, as objRun, windowRunWith, dacBelow and dacAbove would
// pixel by pixel. The windows cut the line into stretches; in each, only the layers that are on a
// screen and not hidden by a window are looked at. What those keep between pixels is held here and
// put back at the end.
static void composeLine(const struct WindowEffect effects[4], const u8* objPalette, const u8* objPriority, bool refreshed, bool wide) {
	bool blank = ppu.io.displayDisable || (!ppu.io.overscan && VCOUNTER >= 225);
	bool direct = directColorMode();
	bool objAboveEnable = ppu.obj.io.aboveEnable;
	bool objBelowEnable = ppu.obj.io.belowEnable;

	// Which of the four window effects each pixel has.
	u8 select[256];
	{
		u32 oneLeft = ppu.window.oneLeft, oneRight = ppu.window.oneRight;
		u32 twoLeft = ppu.window.twoLeft, twoRight = ppu.window.twoRight;
		bool oneActive = ppu.window.oneActive, oneStopped = ppu.window.oneStopped;
		bool twoActive = ppu.window.twoActive, twoStopped = ppu.window.twoStopped;
		for (u32 x = 0; x < 256; ++x) {
			if (x == oneLeft) {
				oneActive = true;
			}
			if (x == twoLeft) {
				twoActive = true;
			}
			select[x] = (oneActive && !oneStopped) | (twoActive && !twoStopped) << 1;
			if (x == oneRight) {
				oneStopped = true;
			}
			if (x == twoRight) {
				twoStopped = true;
			}
		}
		ppu.window.oneActive = oneActive;
		ppu.window.oneStopped = oneStopped;
		ppu.window.twoActive = twoActive;
		ppu.window.twoStopped = twoStopped;
		ppu.window.x = 256;
	}

	// Each pixel's winner on the main and the sub screen: its value, and which layer it is (the
	// backgrounds 0 to 3, the sprites 4, none 5); and whether the pixel had color math.
	static u32 aboveBest[256], belowBest[256];
	static u8 aboveId[256], belowId[256];
	static bool mathOn[256];
	bool dacBlend = ppu.dac.blendMode, dacHalve = ppu.dac.colorHalve, subtract = ppu.dac.colorMode;
	bool objColorEnable = ppu.dac.objColorEnable, backColorEnable = ppu.dac.backColorEnable;
	u32 fixed = fixedColor();
	u32 brightness = ppu.io.displayBrightness << 15;
	bool lineValid = ppu.dac.lineValid;
	u32* line = &ppuCanvas[ppu.dac.line];
	// What the color math keeps from column to column, for the double width.
	u32 wideAbove = ppu.dac.math.aboveColor, wideBelow = ppu.dac.math.belowColor;
	bool wideAboveOn = ppu.dac.math.aboveColorEnable, wideMath = ppu.dac.math.belowColorEnable;
	bool wideClear = ppu.dac.math.transparent, wideBlend = ppu.dac.math.blendMode, wideHalve = ppu.dac.math.colorHalve;
	u32 wideLatch = ppu.latch.cgramAddress;

	u32 hiddenAbove = 0, hiddenBelow = 0;
	for (u32 x = 0; x < 256;) {
		const struct WindowEffect* effect = &effects[select[x]];
		hiddenAbove |= effect->hideAbove;
		hiddenBelow |= effect->hideBelow;
		u32 end = x + 1;
		while (end < 256 && select[end] == select[x]) {
			++end;
		}

		// The layers to look at on each screen, lowest background first as the pixels are.
		const u32* abovePixels[4];
		const u32* belowPixels[4];
		u8 aboveIndex[4], belowIndex[4];
		u32 aboveCount = 0, belowCount = 0;
		for (u32 n = 0; n < 4; ++n) {
			if (ppu.bg[n].io.aboveEnable && !(effect->hideAbove >> n & 1)) {
				abovePixels[aboveCount] = linePixels[n];
				aboveIndex[aboveCount++] = n;
			}
			if (ppu.bg[n].io.belowEnable && !(effect->hideBelow >> n & 1)) {
				belowPixels[belowCount] = lineSub(n);
				belowIndex[belowCount++] = n;
			}
		}
		bool objAbove = objAboveEnable && !(effect->hideAbove >> 4 & 1);
		bool objBelow = objBelowEnable && !(effect->hideBelow >> 4 & 1);
		bool windowAbove = effect->aboveColorEnable, windowBelow = effect->belowColorEnable;

		if (blank) {
			if (lineValid) {
				for (u32 at = x; at < end; ++at) {
					line[0] = brightness;
					line[1] = brightness;
					line += 2;
				}
			}
			x = end;
			continue;
		}

		if (wide) {
			// Twice as wide: each column shows the sub screen's pixel, then the main screen's. The
			// sub screen's is blended by what the column before left of the main screen's color
			// math, so the columns are done in turn, as dacBelow and dacAbove do them.
			for (; x < end; ++x) {
				u32 priority = 0;
				for (u32 i = 0; i < belowCount; ++i) {
					u32 value = belowPixels[i][x];
					if (value >> 16 > priority) {
						priority = value >> 16;
						if (direct && belowIndex[i] == 0) {
							wideBelow = directColor(value, value >> 8);
						} else {
							wideLatch = value & 0xff;
							wideBelow = ppu.cgram[wideLatch];
						}
					}
				}
				if (objBelow && objPriority[x] > priority) {
					priority = objPriority[x];
					wideLatch = objPalette[x];
					wideBelow = ppu.cgram[wideLatch];
				}
				wideClear = priority == 0;
				if (wideClear) {
					wideLatch = 0;
					wideBelow = ppu.cgram[0];
				}
				u32 subColor;
				if (!wideMath) {
					subColor = wideAboveOn ? wideBelow : 0;
				} else {
					subColor = blendWith(wideAboveOn ? wideBelow : 0, wideBlend ? wideAbove : fixed, subtract, wideHalve);
				}

				priority = 0;
				for (u32 i = 0; i < aboveCount; ++i) {
					u32 value = abovePixels[i][x];
					if (value >> 16 > priority) {
						priority = value >> 16;
						if (direct && aboveIndex[i] == 0) {
							wideAbove = directColor(value, value >> 8);
						} else {
							wideLatch = value & 0xff;
							wideAbove = ppu.cgram[wideLatch];
						}
						wideMath = ppu.dac.bgColorEnable[aboveIndex[i]];
					}
				}
				if (objAbove && objPriority[x] > priority) {
					priority = objPriority[x];
					wideLatch = objPalette[x];
					wideAbove = ppu.cgram[wideLatch];
					wideMath = objColorEnable && objPalette[x] >= 192;
				}
				if (priority == 0) {
					wideLatch = 0;
					wideAbove = ppu.cgram[0];
					wideMath = backColorEnable;
				}
				if (!windowBelow) {
					wideMath = false;
				}
				wideAboveOn = windowAbove;
				u32 mainColor;
				if (!wideMath) {
					mainColor = wideAboveOn ? wideAbove : 0;
				} else {
					if (dacBlend && wideClear) {
						wideBlend = false;
						wideHalve = false;
					} else {
						wideBlend = dacBlend;
						wideHalve = dacHalve && wideAboveOn;
					}
					mainColor = blendWith(wideAboveOn ? wideAbove : 0, wideBlend ? wideBelow : fixed, subtract, wideHalve);
				}

				if (lineValid) {
					line[0] = brightness | subColor;
					line[1] = brightness | mainColor;
					line += 2;
				}
			}
			continue;
		}

		// The winner on each screen: the highest priority, the first of equals. A pass to a layer,
		// each over the whole stretch.
		for (u32 at = x; at < end; ++at) {
			aboveBest[at] = 0;
			aboveId[at] = 5;
			belowBest[at] = 0;
			belowId[at] = 5;
		}
		for (u32 i = 0; i < aboveCount; ++i) {
			const u32* pixels = abovePixels[i];
			u8 id = aboveIndex[i];
			for (u32 at = x; at < end; ++at) {
				u32 value = pixels[at];
				bool wins = value >> 16 > aboveBest[at] >> 16;
				aboveBest[at] = wins ? value : aboveBest[at];
				aboveId[at] = wins ? id : aboveId[at];
			}
		}
		if (objAbove) {
			for (u32 at = x; at < end; ++at) {
				u32 value = objPriority[at] << 16 | objPalette[at];
				bool wins = objPriority[at] > aboveBest[at] >> 16;
				aboveBest[at] = wins ? value : aboveBest[at];
				aboveId[at] = wins ? 4 : aboveId[at];
			}
		}
		for (u32 i = 0; i < belowCount; ++i) {
			const u32* pixels = belowPixels[i];
			u8 id = belowIndex[i];
			for (u32 at = x; at < end; ++at) {
				u32 value = pixels[at];
				bool wins = value >> 16 > belowBest[at] >> 16;
				belowBest[at] = wins ? value : belowBest[at];
				belowId[at] = wins ? id : belowId[at];
			}
		}
		if (objBelow) {
			for (u32 at = x; at < end; ++at) {
				u32 value = objPriority[at] << 16 | objPalette[at];
				bool wins = objPriority[at] > belowBest[at] >> 16;
				belowBest[at] = wins ? value : belowBest[at];
				belowId[at] = wins ? 4 : belowId[at];
			}
		}

		// Whether color math is on for each layer that can win, with the windows' say.
		bool mathFor[6];
		for (u32 n = 0; n < 4; ++n) {
			mathFor[n] = ppu.dac.bgColorEnable[n] && windowBelow;
		}
		mathFor[4] = objColorEnable && windowBelow;
		mathFor[5] = backColorEnable && windowBelow;
		u32 backColor = ppu.cgram[0];

		for (; x < end; ++x) {
			u32 below = belowBest[x];
			u32 above = aboveBest[x];
			u32 belowWinner = belowId[x], aboveWinner = aboveId[x];
			u32 belowPixel = belowWinner == 5 ? backColor : direct && belowWinner == 0 ? directColor(below, below >> 8) : ppu.cgram[below & 0xff];
			u32 abovePixel = aboveWinner == 5 ? backColor : direct && aboveWinner == 0 ? directColor(above, above >> 8) : ppu.cgram[above & 0xff];
			bool math = mathFor[aboveWinner] && (aboveWinner != 4 || (above & 0xff) >= 192);
			bool clear = belowWinner == 5;
			u32 color;
			if (!math) {
				color = windowAbove ? abovePixel : 0;
			} else {
				bool blend = dacBlend && !clear;
				bool halve = blend || !dacBlend ? dacHalve && windowAbove : false;
				color = blendWith(windowAbove ? abovePixel : 0, blend ? belowPixel : fixed, subtract, halve);
			}
			mathOn[x] = math;
			if (lineValid) {
				line[0] = brightness | color;
				line[1] = brightness | color;
				line += 2;
			}
		}
	}

	if (wide) {
		ppu.dac.math.aboveColor = wideAbove;
		ppu.dac.math.belowColor = wideBelow;
		ppu.dac.math.aboveColorEnable = wideAboveOn;
		ppu.dac.math.belowColorEnable = wideMath;
		ppu.dac.math.transparent = wideClear;
		ppu.dac.math.blendMode = wideBlend;
		ppu.dac.math.colorHalve = wideHalve;
		ppu.latch.cgramAddress = wideLatch;
	} else if (!blank) {
		// What the pixels left behind them: the last one's colors, the palette entry last looked up
		// (a direct color looks none up), and the blend of the last pixel that had color math.
		u32 below = belowBest[255], above = aboveBest[255];
		ppu.dac.math.belowColor = belowId[255] == 5 ? ppu.cgram[0] : direct && belowId[255] == 0 ? directColor(below, below >> 8) : ppu.cgram[below & 0xff];
		ppu.dac.math.aboveColor = aboveId[255] == 5 ? ppu.cgram[0] : direct && aboveId[255] == 0 ? directColor(above, above >> 8) : ppu.cgram[above & 0xff];
		ppu.dac.math.transparent = belowId[255] == 5;
		ppu.dac.math.aboveColorEnable = effects[select[255]].aboveColorEnable;
		ppu.dac.math.belowColorEnable = mathOn[255];
		for (s32 x = 255; x >= 0; --x) {
			if (!(direct && aboveId[x] == 0)) {
				ppu.latch.cgramAddress = aboveId[x] == 5 ? 0 : aboveBest[x] & 0xff;
				break;
			}
			if (!(direct && belowId[x] == 0)) {
				ppu.latch.cgramAddress = belowId[x] == 5 ? 0 : belowBest[x] & 0xff;
				break;
			}
		}
		for (s32 x = 255; x >= 0; --x) {
			if (mathOn[x]) {
				bool blend = ppu.dac.blendMode && belowId[x] != 5;
				ppu.dac.math.blendMode = blend;
				ppu.dac.math.colorHalve = blend || !ppu.dac.blendMode ? ppu.dac.colorHalve && effects[select[x]].aboveColorEnable : false;
				break;
			}
		}
	}

	// What the last pixel leaves: the backgrounds as their line left them, less what the windows
	// hide there, and the sprites' last pixel.
	// (Backgrounds that weren't fetched on this line keep whatever any window on it hid.)
	const struct WindowEffect* last = &effects[select[255]];
	if (refreshed) {
		hiddenAbove = last->hideAbove;
		hiddenBelow = last->hideBelow;
	}
	for (u32 n = 0; n < 4; ++n) {
		if (hiddenAbove >> n & 1) {
			ppu.bg[n].output.above.priority = 0;
		}
		if (hiddenBelow >> n & 1) {
			ppu.bg[n].output.below.priority = 0;
		}
	}
	s32 lastObj = 255;
	while (lastObj >= 0 && !objPriority[lastObj]) {
		--lastObj;
	}
	ppu.obj.above.priority = 0;
	ppu.obj.below.priority = 0;
	if (lastObj >= 0 && objAboveEnable) {
		ppu.obj.above.palette = objPalette[lastObj];
		if (!(last->hideAbove >> 4 & 1)) {
			ppu.obj.above.priority = objPriority[255];
		}
	}
	if (lastObj >= 0 && objBelowEnable) {
		ppu.obj.below.palette = objPalette[lastObj];
		if (!(last->hideBelow >> 4 & 1)) {
			ppu.obj.below.priority = objPriority[255];
		}
	}
	ppu.obj.x += 256;
	ppu.window.aboveColorEnable = last->aboveColorEnable;
	ppu.window.belowColorEnable = last->belowColorEnable;
	if (lineValid) {
		ppu.dac.line += 512;
		if (ppu.drawnTo < ppu.dac.line) {
			ppu.drawnTo = ppu.dac.line;
		}
	}
}

static void renderSpan(u32 start, u32 end) {
	for (u32 at = start; at < end && at <= 1016; at += 2) {
		if (at % 8 == 0) {
			ppu.counter.hcounter = at;
			objEvaluate(at >> 3);
		}
	}

	static const u8 order[4] = { 2, 0, 1, 3 };
	__typeof__(ppu.bg[0].output) endOutput[4];
	bool hires = hiresMode();
	bool draws = VCOUNTER != 0;
	// A whole line in modes 0 to 4 needs no slots for its tiled backgrounds.
	bool whole = start == 0 && end == 1080 && draws && ppu.io.bgMode <= 4;
	bool opt = ppu.io.bgMode == 2 || ppu.io.bgMode == 4;

	// Below the picture nothing is fetched or shifted: the backgrounds stay as its last line left
	// them. (ares goes on fetching to line 240.)
	bool tiled = VCOUNTER < ppu.state.vdisp;
	// Nor while the display is off; if it comes back on before the line ends, they are brought up
	// to here first, from VRAM as it is now.
	if (tiled && ppu.io.displayDisable) {
		if (ppu.blankFrom == BLANK_NONE) {
			ppu.blankFrom = start;
		}
		tiled = false;
	} else if (tiled && ppu.blankFrom != BLANK_NONE) {
		for (int i = 0; i < 4; ++i) {
			bgSpanSlots(order[i], ppu.blankFrom, start, hires, draws);
		}
		ppu.blankFrom = BLANK_NONE;
	}
	// The backgrounds that had their whole line at once.
	bool lined[4] = { false, false, false, false };
	for (int i = 0; tiled && i < 4; ++i) {
		u32 n = order[i];
		lined[n] = true;
		if (whole && ppu.bg[n].io.mode <= BG_BPP8) {
			bgLineTiles(n, true, opt);
		} else if (whole && opt && n == 2) {
			// BG3 isn't drawn in these modes, but the other two's offsets are fetched from it.
			bgLineOffsets();
			bgLineTiles(n, false, false);
		} else if (start == 0 && end == 1080 && draws && ppu.io.bgMode == 5 && ppu.bg[n].io.mode != BG_MODE7) {
			bgLineTilesWide(n, ppu.bg[n].io.mode <= BG_BPP8);
		} else if (start == 0 && end == 1080 && draws && ppu.bg[n].io.mode == BG_MODE7) {
			bgLineMode7(n);
		} else if (start == 0 && end == 1080 && draws && !hires && ppu.bg[n].io.mode == BG_INACTIVE &&
		           !(n == 2 && (ppu.io.bgMode == 2 || ppu.io.bgMode == 4))) {
			// (BG3 fetches the other two's offsets in modes 2 and 4.)
			bgLineTiles(n, false, false);
		} else {
			lined[n] = false;
			bgSpanSlots(n, start, end, hires, draws);
		}
		endOutput[n] = ppu.bg[n].output;
	}
	bool allLined = lined[0] && lined[1] && lined[2] && lined[3];

	// A whole line's sprites: their tiles laid out once, a later tile over an earlier one, as each
	// pixel would find them.
	static u8 objPalette[256];
	static u8 objPriority[256];
	bool objLine = start == 0 && end == 1080 && ppu.obj.x == 0;
	if (objLine) {
		memset(objPriority, 0, sizeof(objPriority));
		for (int n = 0; n < 34; ++n) {
			const __typeof__(ppu.obj.tile[0][0])* tile = &ppu.obj.tile[!ppu.obj.active][n];
			if (!tile->valid) {
				break;
			}
			s32 left = signedBits(tile->x, 9);
			for (s32 px = 0; px < 8; ++px) {
				s32 x = left + px;
				if (x < 0 || x > 255) {
					continue;
				}
				u32 shift = tile->hflip ? px : 7 - px;
				u32 color = 0;
				color += tile->data >> (shift + 0) & 1;
				color += tile->data >> (shift + 7) & 2;
				color += tile->data >> (shift + 14) & 4;
				color += tile->data >> (shift + 21) & 8;
				if (color) {
					objPalette[x] = tile->palette + color;
					objPriority[x] = ppu.obj.io.priority[tile->priority];
				}
			}
		}
	}

	struct WindowEffect effects[4];
	windowEffects(effects);
	// A whole line that draws, in a mode a pixel wide, is put together in one go.
	// So is one that shows nothing: its backgrounds weren't fetched, and aren't looked at.
	bool dark = ppu.io.displayDisable || (!ppu.io.overscan && VCOUNTER >= 225);
	if (objLine && (allLined || (!tiled && dark)) && draws && ppu.window.x == 0) {
		composeLine(effects, objPalette, objPriority, tiled, hires || ppu.io.pseudoHires);
		ppu.counter.hcounter = start;
		return;
	}
	for (u32 n = 0; n < 4; ++n) {
		if (lined[n]) {
			lineToSlots(n);
		}
	}
	for (u32 at = start | 2; at < end; at += 4) {
		if (at >= 58 && at <= 1078) {
			u32 x = (at - 58) >> 2;
			ppu.counter.hcounter = at;
			for (int n = 0; tiled && n < 4; ++n) {
				ppu.bg[n].output.above = spanAbove[n][x];
				ppu.bg[n].output.below = spanBelow[n][x];
			}
			if (objLine) {
				ppu.obj.above.priority = 0;
				ppu.obj.below.priority = 0;
				ppu.obj.x++;
				if (objPriority[x]) {
					if (ppu.obj.io.aboveEnable) {
						ppu.obj.above.palette = objPalette[x];
						ppu.obj.above.priority = objPriority[x];
					}
					if (ppu.obj.io.belowEnable) {
						ppu.obj.below.palette = objPalette[x];
						ppu.obj.below.priority = objPriority[x];
					}
				}
			} else {
				objRun();
			}
			windowRunWith(effects);
			dacRun();
		}
	}

	// A span ending on a sub screen slot leaves the backgrounds as that slot did.
	u32 last = end - 2;
	if (tiled && last >= 56 && last <= 1078 && (last - 56) % 4 == 0) {
		for (int n = 0; n < 4; ++n) {
			ppu.bg[n].output = endOutput[n];
		}
	}

	ppu.counter.hcounter = start;
}
#endif

static inline void tickN(u32 clocks) {
	ppu.counter.hcounter += clocks;
	if (ppu.counter.hcounter >= ppu.counter.hperiod) {
		ppu.counter.lastHperiod = ppu.counter.hperiod;
		ppu.counter.hcounter -= ppu.counter.hperiod;
		counterTickScanline(&ppu.counter);
	}
}

// How many of these two-clock steps the PPU takes before its clock is past the CPU's.
static inline u32 stepsBeforeCpu(u32 wait) {
	u64 step = ppu.th.scalar * 2;
	if (cpu.th.clock < ppu.th.clock) {
		return 1;
	}
	u64 steps = (cpu.th.clock - ppu.th.clock) / step + 1;
	return steps < wait ? steps : wait;
}

// The PPU can stop after any two-clock step and come back to it: ppu.resume says where, and what it
// was in the middle of is in ppu, never in a local variable.
#define STOP_(n) \
	do { \
		ppu.resume = (n); \
		return; \
	case (n):; \
	} while (0)
#define STOP_X(n) STOP_(n)
#define STOP() STOP_X(__COUNTER__ + 1)
// Steps the PPU does nothing in but wait go in one: as many as it can take before it is past the CPU,
// and at most to the end of the line.
#define STEP(clocks) \
	for (ppu.wait = (clocks) >> 1; ppu.wait;) { \
		u32 steps = stepsBeforeCpu(ppu.wait); \
		ppu.wait -= steps; \
		tickN(steps * 2); \
		ppu.th.clock += ppu.th.scalar * 2 * steps; \
		if (cpu.th.clock < ppu.th.clock) { \
			STOP(); \
		} \
	}

#define F ppu.fetch

void ppuRun(void) {
	const struct OamObject* sprite;
	__typeof__(ppu.obj.tile[0][0])* tile;
	u32 sx, mx, pos;

	switch (ppu.resume) {
	case 0:
		for (;;) {
			lineStart();

			if (VCOUNTER > 240) {
				STEP(ppu.counter.hperiod);
				continue;
			}

			// The visible part of the line, in spans: nothing outside the PPU can change while it runs
			// up to the CPU, so each span is drawn in one go.
			for (ppu.cycle = 0; ppu.cycle < 1080;) {
				{
					u32 steps = stepsBeforeCpu((1080 - ppu.cycle) >> 1);
					u32 end = ppu.cycle + steps * 2;
					renderSpan(ppu.cycle, end);
					tickN(end - ppu.cycle);
					ppu.th.clock += ppu.th.scalar * 2 * steps;
					ppu.cycle = end;
				}
				if (cpu.th.clock < ppu.th.clock) {
					STOP();
				}
			}

			// H = 1080: the tiles of the sprites found on this line are fetched for the next one, two
			// words in eight clocks each.
			for (F.i = 31; F.i >= 0; --F.i) {
				if (!ppu.obj.item[ppu.obj.active][F.i].valid) {
					continue;
				}

				if (ppu.io.displayDisable || VCOUNTER >= ppu.state.vdisp - 1u) {
					STEP(8);
					continue;
				}

				ppu.latch.oamAddress = ppu.obj.item[ppu.obj.active][F.i].index;
				F.sprite = ppu.latch.oamAddress;
				sprite = &ppu.oam[F.sprite];

				F.tileWidth = objectWidth(sprite) >> 3;
				F.x = sprite->x;
				F.y = (ppu.obj.y - sprite->y) & 255;
				if (ppu.obj.io.interlace) {
					F.y <<= 1;
				}

				if (sprite->vflip) {
					if (objectWidth(sprite) == objectHeight(sprite)) {
						F.y = objectHeight(sprite) - 1 - F.y;
					} else if ((u32) F.y < objectWidth(sprite)) {
						F.y = objectWidth(sprite) - 1 - F.y;
					} else {
						F.y = objectWidth(sprite) + (objectWidth(sprite) - 1) - (F.y - objectWidth(sprite));
					}
				}

				if (ppu.obj.io.interlace) {
					F.y = !sprite->vflip ? F.y + ppu.counter.field : F.y - ppu.counter.field;
				}

				F.x &= 511;
				F.y &= 255;

				F.tiledataAddress = ppu.obj.io.tiledataAddress;
				if (sprite->nameselect) {
					F.tiledataAddress += (1 + ppu.obj.io.nameselect) << 12;
				}
				F.chrx = sprite->character & 15;
				F.chry = (((sprite->character >> 4) + (F.y >> 3)) & 15) << 4;

				for (F.tx = 0; F.tx < F.tileWidth; ++F.tx) {
					sx = (F.x + (F.tx << 3)) & 511;
					if (F.x != 256 && sx >= 256 && sx + 7 < 512) {
						continue;
					}
					if (ppu.obj.tileCount++ >= 34) {
						break;
					}

					sprite = &ppu.oam[F.sprite];
					F.n = ppu.obj.tileCount - 1;
					tile = &ppu.obj.tile[ppu.obj.active][F.n];
					tile->valid = true;
					tile->x = sx;
					tile->priority = sprite->priority;
					tile->palette = 128 + (sprite->palette << 4);
					tile->hflip = sprite->hflip;

					mx = !sprite->hflip ? F.tx : F.tileWidth - 1 - F.tx;
					pos = F.tiledataAddress + ((F.chry + ((F.chrx + mx) & 15)) << 4);
					F.address = (pos & 0xfff0) + (F.y & 7);

					if (!ppu.io.displayDisable) {
						tile->data = (tile->data & 0xffff0000) | *vramAt(F.address + 0);
					}
					STEP(4);

					if (!ppu.io.displayDisable) {
						tile = &ppu.obj.tile[ppu.obj.active][F.n];
						tile->data = (tile->data & 0x0000ffff) | *vramAt(F.address + 8) << 16;
					}
					STEP(4);
				}
			}

			ppu.obj.io.rangeOver |= ppu.obj.itemCount > 32;
			ppu.obj.io.timeOver |= ppu.obj.tileCount > 34;

			// H = 1352 at most.
			STEP(ppu.counter.hperiod - HCOUNTER);
		}
	}
}

void ppuPower(bool reset) {
#if !defined(ARMSNES_DOT)
	planeSpreadInit();
#endif
	threadFrequency(&ppu.th, sys.cpuFrequency);
	counterReset(&ppu.counter);
	ppu.resume = 0;
	ppu.drawnTo = 0;
	ppu.blankFrom = BLANK_NONE;

	memset(ppuCanvas, 0, sizeof(ppuCanvas));

	if (!reset) {
		randomArray((u8*) ppu.vram, sizeof(ppu.vram));
	}

	ppu.ppu1.version = 1;
	ppu.ppu1.mdr = randomBias(0xff);
	ppu.ppu2.version = 3;
	ppu.ppu2.mdr = randomBias(0xff);

	ppu.vramMask = 0x7fff;

	memset(ppu.oam, 0, sizeof(ppu.oam));

	if (!reset) {
		randomArray((u8*) ppu.cgram, sizeof(ppu.cgram));
		for (int n = 0; n < 256; ++n) {
			ppu.cgram[n] &= 0x7fff;
		}
	}

	ppu.latch.vram = randomNext();
	ppu.latch.oam = randomNext();
	ppu.latch.cgram = randomNext();
	ppu.latch.bgofsPPU1 = randomNext();
	ppu.latch.bgofsPPU2 = randomNext() & 7;
	ppu.latch.mode7 = randomNext();
	ppu.latch.counters = false;
	ppu.latch.hcounter = 0;
	ppu.latch.vcounter = 0;
	ppu.latch.oamAddress = 0x0000;
	ppu.latch.cgramAddress = 0x00;

	ppu.io.displayDisable = true;
	ppu.io.displayBrightness = 0;

	ppu.io.oamBaseAddress = (randomNext() & ~1ull) & 0x3ff;
	ppu.io.oamAddress = randomNext() & 0x3ff;
	ppu.io.oamPriority = randomNext() & 1;

	ppu.io.bgPriority = false;
	ppu.io.bgMode = 0;

	ppu.io.hoffsetMode7 = randomNext();
	ppu.io.voffsetMode7 = randomNext();

	ppu.io.vramIncrementMode = randomBias(1) & 1;
	ppu.io.vramMapping = randomNext() & 3;
	ppu.io.vramIncrementSize = 1;

	ppu.io.vramAddress = randomNext();

	ppu.io.repeatMode7 = randomNext() & 3;
	ppu.io.vflipMode7 = randomNext() & 1;
	ppu.io.hflipMode7 = randomNext() & 1;

	ppu.io.m7a = randomNext();
	ppu.io.m7b = randomNext();
	ppu.io.m7c = randomNext();
	ppu.io.m7d = randomNext();
	ppu.io.m7x = randomNext();
	ppu.io.m7y = randomNext();

	ppu.io.cgramAddress = randomNext();
	ppu.io.cgramAddressLatch = randomNext() & 1;

	ppu.io.extbg = randomNext() & 1;
	if (!reset) {
		ppu.io.pseudoHires = randomNext() & 1;
	}
	ppu.io.overscan = false;
	ppu.io.interlace = false;

	ppu.io.hcounter = 0;
	ppu.io.vcounter = 0;

	if (!reset) {
		ppu.mosaic.size = (randomNext() & 15) + 1;
		ppu.mosaic.vcounter = 0;
		for (int n = 0; n < 4; ++n) {
			ppu.bg[n].id = n;
			bgPower(&ppu.bg[n]);
		}
		objPower();
		windowPower();
		dacPower();
	}

	updateVideoMode();
}
