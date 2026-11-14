/* Copyright (c) 2004-2025 ares team, Near et al
 * Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Ported from ares (ISC); see LICENSES/ISC-ares.txt. */
// Included where STEP(clocks) says how the PPU waits: step by step in ppuRun, not at all in Fast mode.
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

