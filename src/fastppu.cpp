/******************************************************************************/
/* Mednafen Fast SNES Emulation Module                                        */
/******************************************************************************/
/* ppu_render_common.inc:
**  Copyright (C) 2015-2019 Mednafen Team
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of the GNU General Public License
** as published by the Free Software Foundation; either version 2
** of the License, or (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software Foundation, Inc.,
** 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/
/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Fast mode's line renderer: snes_faust's (Supafaust's) single-threaded one, from its
 * ppu_render_common.inc, drawing one line from the registers fastPpuLine is given. */
#include "fastppu.h"

#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <utility>

typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;
typedef int16_t int16;

#define INLINE inline __attribute__((always_inline))
#define NO_INLINE __attribute__((noinline))
#define MDFN_HOT __attribute__((hot))
#define MDFN_FASTCALL
#define MDFN_RESTRICT __restrict__
#define MDFN_LIKELY(n) __builtin_expect(!!(n), 1)
#define MDFN_UNLIKELY(n) __builtin_expect(!!(n), 0)
#define assert(n) ((void) 0)

namespace {

// The registers, as the ports were written; set from the line being drawn.
const uint16* VRAM;
const uint16* CGRAM;
unsigned scanline;
uint8 Status[2];
uint8 ScreenMode;
uint8 BGMode;
uint8 Mosaic;
uint8 MosaicYOffset;
uint8 BGSC[4];
uint8 BGNBA[2];
uint16 BGHOFS[4];
uint16 BGVOFS[4];
uint8 M7SEL;
int16 M7Matrix[4];
int16 M7Center[2];
int16 M7HOFS;
int16 M7VOFS;
uint8 MSEnable;
uint8 SSEnable;
uint8 WMSettings[3];
uint8 WMMainEnable;
uint8 WMSubEnable;
uint16 WMLogic;
uint8 WindowPos[2][2];
unsigned WindowPieces[5];
uint8 CGWSEL;
uint8 CGADSUB;
uint16 FixedColor;

struct
{
 uint32 objbuf[8 + 256 + 8];
 uint16 SpriteTileTab[16];
} PPU;

static struct
{
 uint32 OutputLUT[384];
 union
 {
  uint32 bg[4][8 + 256 + 16];		// 8(x scroll), 16(tile overflow, mosaic)
  uint32 bghr[2][16 + 512 + 32];	// BG modes 5 and 6

  struct
  {
   uint32 main[256];
   uint32 dummy[8 + 16 + (8 + 256 + 16)];
   uint32 sub[256];
  };
 };
} linebuf;

template<bool size16, bool mode4 = false>
static MDFN_HOT MDFN_FASTCALL void GetOPTStrip(const unsigned n)
{
 unsigned VOFS = BGVOFS[n];
 unsigned HOFS = BGHOFS[n];
 unsigned tm_w_mask = ((BGSC[n] & 0x1) << 10);
 unsigned tm_h_shift = ((BGSC[n] & 0x2) ? ((BGSC[n] & 0x1) ? 3 : 2) : 24);

 uint32 tmbase = (BGSC[n] & 0xFC) << 8;
 uint32 tmoffs;
 uint32 tmxor;
 bool tile_num_offs = false;

 if(size16)
 {
  tile_num_offs = (HOFS & 0x8);
  HOFS >>= 1;
  VOFS >>= 1;
 }

 tmoffs = (HOFS >> 3) & 0x1F;
 tmoffs += (HOFS << 2) & tm_w_mask;
 tmoffs += ((VOFS << 2) & 0x3E0) | ((VOFS & 0x100) << tm_h_shift);

 {
  unsigned VOFS_Next = BGVOFS[n] + 8;
  unsigned tmoffs_next;

  if(size16)
   VOFS_Next >>= 1;

  tmoffs_next = (HOFS >> 3) & 0x1F;
  tmoffs_next += (HOFS << 2) & tm_w_mask;
  tmoffs_next += ((VOFS_Next << 2) & 0x3E0) | ((VOFS_Next & 0x100) << tm_h_shift);

  tmxor = tmoffs ^ tmoffs_next;
 }

 for(unsigned i = 0; i < 33; i++)
 {
  uint32 tmp = VRAM[(tmbase + tmoffs) & 0x7FFF];

  if(mode4)
  {
   tmp <<= (tmp & 0x8000) >> 11;
  }
  else
  {
   tmp |= (VRAM[(tmbase + (tmoffs ^ tmxor)) & 0x7FFF] << 16);
  }
  linebuf.bg[2][i] = tmp;

  if(!size16 || tile_num_offs)
  {
   tmoffs++;

   if(!(tmoffs & 0x1F))
   {
    tmoffs -= 0x20;
    tmoffs ^= tm_w_mask;
   }
  }

  if(size16)
   tile_num_offs = !tile_num_offs;
 }
}

static INLINE unsigned DirColCvt(unsigned inpix, unsigned palbase = 0)
{
 unsigned ret;

 ret =  ((inpix & 0x07) << 2);
 ret |= ((inpix & 0x38) << 4);
 ret |= ((inpix & 0xC0) << 7);

 // armSNES: the palette's bits 0-2 add to red, green and blue, as the S-PPU does it.
 ret |= (palbase & 0x4) << 10;
 ret |= (palbase & 0x2) << 5;
 ret |= (palbase & 0x1) << 1;

 return ret;
}

template<bool size16, unsigned bpp, bool palbase_n = false, bool opt = false, bool hires = false, bool dircolor = false>
static MDFN_HOT MDFN_FASTCALL void DrawBG(const unsigned n, const unsigned y, uint32 prio_or)
{
 alignas(4) uint16 tab[16];
 const bool MosaicOn = Mosaic & (1U << n);
 unsigned VOFS = y - (MosaicOn ? MosaicYOffset : 0);
 unsigned HOFS = BGHOFS[n];
 unsigned tm_w_mask = ((BGSC[n] & 0x1) << 10);
 unsigned tm_h_shift = ((BGSC[n] & 0x2) ? ((BGSC[n] & 0x1) ? 3 : 2) : 24);

 if(bpp == 4 || bpp == 2)
 {
  memcpy(tab, PPU.SpriteTileTab, sizeof(tab));
 }

 //if(scanline == 100 && n == 0)
 // MDFN_DispMessage("%d %d --- BGHOFS0=%u BGHOFS1=%u\n", hires, size16, BGHOFS[0], BGHOFS[1]);

 if(hires && (ScreenMode & 0x01))
 {
  VOFS <<= 1;

  if(!MosaicOn)
   VOFS += Status[1] >> 7;
 }

 VOFS += BGVOFS[n];
 //
 unsigned tile_y_offs = (VOFS & 0x7);
 //
 //
 //
 if(hires)
  HOFS <<= 1;

 uint32* target;

 // Make sure we go with [8] and not [7], or else we'll potentially have an invalid bounds pointer
 // in the pixel blitting loop further down in this function.
 if(hires)
  target = &linebuf.bghr[n][8];
 else
  target = &linebuf.bg[n][8];

 target -= HOFS & 0x7;

 //printf("%02x %04x\n", BGSC[n], BGVOFS[n]);

 uint32 chrbase = ((BGNBA[n >> 1] >> ((n & 1) * 4)) & 0xF) << 12;
 uint32 tmbase = (BGSC[n] & 0xFC) << 8;
 uint32 tmoffs;
 uint32 tile_num_offs = 0;

 if(hires || size16)
 {
  tile_num_offs |= ((HOFS & 0x8) >> 3);
  HOFS >>= 1;
 }

 if(size16)
 {
  tile_num_offs |= ((VOFS & 0x8) << 1);
  VOFS >>= 1;
 }

 tmoffs = (HOFS >> 3) & 0x1F;
 tmoffs += (HOFS << 2) & tm_w_mask;
 tmoffs += ((VOFS << 2) & 0x3E0) | ((VOFS & 0x100) << tm_h_shift);

 for(unsigned i = 0; i < ((hires ? 64 : 32) + 1); i++)
 {
  const uint32 nte = VRAM[(tmbase + tmoffs) & 0x7FFF];
  const uint32 v_flip_xor = ((int16)nte >> 16) & 0x7;
  const bool h_flip = nte & 0x4000;
  const uint32 eff_prio_or = ((nte & 0x2000) ? (prio_or >> 16) : (prio_or >> 0)) & 0xFFFF;
  const uint32 pal_base = ((nte & 0x1C00) >> 10);
  uint32 tno_flipped = 0;
  uint32 tile_num;

  if(size16 || hires)
   tno_flipped = tile_num_offs ^ h_flip;

  if(size16)
   tno_flipped ^= (v_flip_xor << 2) & 0x10;

  tile_num = (nte + tno_flipped) & 0x3FF;

  //printf("%3d, %3d, %04x, %04x --- %04x\n", y, i, tmbase, (tmbase + tmoffs) & 0x7FFF, nte);

  const uint16* const vchr = &VRAM[(chrbase + (tile_y_offs ^ v_flip_xor) + (tile_num * (bpp / 2) * 8) + 0) & 0x7FFF];
  const uint16* const cgr = CGRAM + (bpp == 8 ? 0 : ((pal_base << bpp) + (palbase_n ? (n << 5) : 0)));
  //
  const size_t subtarg_inc = h_flip ? 1 : -1;
  const uint32* subtarg_bound = target + (h_flip ? 8 : -1);
  uint32* subtarg = target + (h_flip ? 0 : 7);

  if(bpp == 2)
  {
   uint32 tmp[4] =
	{ (uint32)cgr[0] << 16,
	 ((uint32)cgr[1] << 16) | eff_prio_or,
	 ((uint32)cgr[2] << 16) | eff_prio_or,
	 ((uint32)cgr[3] << 16) | eff_prio_or };
   uint32 bp = vchr[0];
   uint32 hm = (tab[bp & 0xF] << 0) + (tab[(bp >> 4) & 0xF] << 16) + (tab[(bp >> 8) & 0xF] << 1) + (tab[(bp >> 12) & 0xF] << 17);

#if 0
   if(h_flip)
   {
    target[0] = tmp[((bp     ) & 0x01) | ((bp >>  7) & 0x02)];
    target[1] = tmp[((bp >> 1) & 0x01) | ((bp >>  8) & 0x02)];
    target[2] = tmp[((bp >> 2) & 0x01) | ((bp >>  9) & 0x02)];
    target[3] = tmp[((bp >> 3) & 0x01) | ((bp >> 10) & 0x02)];
    target[4] = tmp[((bp >> 4) & 0x01) | ((bp >> 11) & 0x02)];
    target[5] = tmp[((bp >> 5) & 0x01) | ((bp >> 12) & 0x02)];
    target[6] = tmp[((bp >> 6) & 0x01) | ((bp >> 13) & 0x02)];
    target[7] = tmp[((bp >> 7) & 0x01) | ((bp >> 14) & 0x02)];
   }
   else
   {
    target[0] = tmp[((bp >> 7) & 0x01) | ((bp >> 14) & 0x02)];
    target[1] = tmp[((bp >> 6) & 0x01) | ((bp >> 13) & 0x02)];
    target[2] = tmp[((bp >> 5) & 0x01) | ((bp >> 12) & 0x02)];
    target[3] = tmp[((bp >> 4) & 0x01) | ((bp >> 11) & 0x02)];
    target[4] = tmp[((bp >> 3) & 0x01) | ((bp >> 10) & 0x02)];
    target[5] = tmp[((bp >> 2) & 0x01) | ((bp >>  9) & 0x02)];
    target[6] = tmp[((bp >> 1) & 0x01) | ((bp >>  8) & 0x02)];
    target[7] = tmp[((bp     ) & 0x01) | ((bp >>  7) & 0x02)];
   }
#else
   if(h_flip)
   {
    target[0] = tmp[(hm >> 0) & 0x3];
    target[1] = tmp[(hm >> 4) & 0x3];
    target[2] = tmp[(hm >> 8) & 0x3];
    target[3] = tmp[(hm >> 12) & 0x3];
    target[4] = tmp[(hm >> 16) & 0x3];
    target[5] = tmp[(hm >> 20) & 0x3];
    target[6] = tmp[(hm >> 24) & 0x3];
    target[7] = tmp[(hm >> 28) & 0x3];
   }
   else
   {
    target[0] = tmp[(hm >> 28) & 0x3];
    target[1] = tmp[(hm >> 24) & 0x3];
    target[2] = tmp[(hm >> 20) & 0x3];
    target[3] = tmp[(hm >> 16) & 0x3];
    target[4] = tmp[(hm >> 12) & 0x3];
    target[5] = tmp[(hm >>  8) & 0x3];
    target[6] = tmp[(hm >>  4) & 0x3];
    target[7] = tmp[(hm >>  0) & 0x3];
   }
#endif
  }
  else if(bpp == 4)
  {
   uint32 bpa = vchr[0];
   uint32 bpb = vchr[8];
   uint32 hm = (tab[bpa & 0xF] << 0) + (tab[(bpa >> 4) & 0xF] << 16) + (tab[(bpa >> 8) & 0xF] << 1) + (tab[(bpa >> 12) & 0xF] << 17) + (tab[bpb & 0xF] << 2) + (tab[(bpb >> 4) & 0xF] << 18) + (tab[(bpb >> 8) & 0xF] << 3) + (tab[(bpb >> 12) & 0xF] << 19);
   size_t pix;

   pix = (hm >> 0) & 0xF; subtarg[0]           = (cgr[pix] << 16) | (pix ? eff_prio_or : 0);
   pix = (hm >> 4) & 0xF; subtarg[subtarg_inc] = (cgr[pix] << 16) | (pix ? eff_prio_or : 0);
   subtarg += subtarg_inc << 1;
   pix = (hm >>  8) & 0xF; subtarg[0]           = (cgr[pix] << 16) | (pix ? eff_prio_or : 0);
   pix = (hm >> 12) & 0xF; subtarg[subtarg_inc] = (cgr[pix] << 16) | (pix ? eff_prio_or : 0);
   subtarg += subtarg_inc << 1;
   pix = (hm >> 16) & 0xF; subtarg[0]           = (cgr[pix] << 16) | (pix ? eff_prio_or : 0);
   pix = (hm >> 20) & 0xF; subtarg[subtarg_inc] = (cgr[pix] << 16) | (pix ? eff_prio_or : 0);
   subtarg += subtarg_inc << 1;
   pix = (hm >> 24) & 0xF; subtarg[0]           = (cgr[pix] << 16) | (pix ? eff_prio_or : 0);
   pix = (hm >> 28) & 0xF; subtarg[subtarg_inc] = (cgr[pix] << 16) | (pix ? eff_prio_or : 0);
   subtarg += subtarg_inc << 1;
#if 0
   uint32 bp = vchr[0] | ((uint32)vchr[8] << 16);
   uint32 bp2;
   size_t pix;
   for(; MDFN_LIKELY(subtarg != subtarg_bound); subtarg += subtarg_inc << 1, bp >>= 2)
   {
    bp2 = bp & 0x01010101; pix = (uint8)((bp2     ) | (bp2 >> 7) | (bp2 >> 14) | (bp2 >> 21)); subtarg[0]           = (cgr[pix] << 16) | (pix ? eff_prio_or : 0);
    bp2 = bp & 0x02020202; pix = (uint8)((bp2 >> 1) | (bp2 >> 8) | (bp2 >> 15) | (bp2 >> 22)); subtarg[subtarg_inc] = (cgr[pix] << 16) | (pix ? eff_prio_or : 0);
   }
#endif
  }
  else if(bpp == 8)
  {
   uint64 bp = vchr[0] | ((uint32)vchr[8] << 16) | ((uint64)vchr[16] << 32) | ((uint64)vchr[24] << 48);
   for(; MDFN_LIKELY(subtarg != subtarg_bound); subtarg += subtarg_inc, bp >>= 1)
   {
    const uint64 bp2 = bp & 0x0101010101010101ULL;
    const size_t pix = (uint8)(bp2 | (bp2 >> 7) | (bp2 >> 14) | (bp2 >> 21) | (bp2 >> 28) | (bp2 >> 35) | (bp2 >> 42) | (bp2 >> 49));
    *subtarg = ((dircolor ? DirColCvt(pix, pal_base) : cgr[pix]) << 16) | (pix ? eff_prio_or : 0);
   }
  }

  if(!(size16 || hires) || (tile_num_offs & 1))
  {
   tmoffs++;

   if(!(tmoffs & 0x1F))
   {
    tmoffs -= 0x20;
    tmoffs ^= tm_w_mask;
   }
  }

  if(size16 || hires)
   tile_num_offs ^= 1;

  if(opt)
  {
   unsigned hvo = linebuf.bg[2][i];

   HOFS = BGHOFS[n];
   if(hvo & (0x2000 << n))
   {
    HOFS = (uint16)hvo;
   }

   if(hires)
    HOFS <<= 1;

   HOFS += ((i + 1) << 3);

   VOFS = BGVOFS[n];

   if(hvo & (0x20000000 << n))
    VOFS = (uint16)(hvo >> 16);
   VOFS += (y - ((Mosaic & (1U << n)) ? MosaicYOffset : 0));

   tile_y_offs = (VOFS & 0x7);

   tile_num_offs = 0;

   if(hires || size16)
   {
    tile_num_offs |= ((HOFS & 0x8) >> 3);
    HOFS >>= 1;
   }

   if(size16)
   {
    tile_num_offs |= ((VOFS & 0x8) << 1);
    VOFS >>= 1;
   }
   tmoffs = (HOFS >> 3) & 0x1F;
   tmoffs += (HOFS << 2) & tm_w_mask;
   tmoffs += ((VOFS << 2) & 0x3E0) | ((VOFS & 0x100) << tm_h_shift);
  }

  target += 8;
 }
}

static INLINE int16 funny(int16 val)
{
 int16 ret = val & 0x3FF;

 if(val & 0x2000)
  ret |= ~0x3FF;

 return ret;
}

static INLINE int M7Mul(int16 matval, int16 ov)
{
 return (matval * ov) &~ 0x3F;
}

// Mode 7, scary cake time!
template<bool extbg, bool dircolor>
static MDFN_HOT MDFN_FASTCALL void DrawMODE7(unsigned line_y, uint16 prio_or, uint32 prio_or_bg1 = 0)
{
 const bool h_flip = M7SEL & 0x01;
 const bool v_flip = M7SEL & 0x02;
 const bool size = M7SEL & 0x80;
 const bool empty_fill = M7SEL & 0x40;

 unsigned x, y;
 unsigned xinc, yinc;

 line_y -= ((Mosaic & 0x1) ? MosaicYOffset : 0);

 if(v_flip)
  line_y ^= 0xFF;

 int16 hoca = funny(M7HOFS - M7Center[0]);
 int16 voca = funny(M7VOFS - M7Center[1]);

 x = M7Mul(M7Matrix[0], hoca) + M7Mul(M7Matrix[1], line_y) + M7Mul(M7Matrix[1], voca) + (M7Center[0] << 8);
 y = M7Mul(M7Matrix[2], hoca) + M7Mul(M7Matrix[3], line_y) + M7Mul(M7Matrix[3], voca) + (M7Center[1] << 8);

 xinc = M7Matrix[0];
 yinc = M7Matrix[2];

 if(h_flip)
 {
  x += 255 * xinc;
  y += 255 * yinc;

  xinc = -xinc;
  yinc = -yinc;
 }

 for(unsigned i = 0; i < 256; i++)
 {
  unsigned pix;
  unsigned xi = x >> 8;
  unsigned yi = y >> 8;
  uint8 tilenum, tiledata;

  tilenum = VRAM[(((yi & 0x3FF) >> 3) << 7) | ((xi & 0x3FF) >> 3)];
  tiledata = VRAM[(tilenum << 6) + ((yi & 0x7) << 3) + (xi & 0x7)] >> 8;

  if(size && ((xi | yi) &~ 0x3FF))
  {
   tiledata = 0;
   if(empty_fill)
    tiledata = VRAM[((yi & 0x7) << 3) + (xi & 0x7)] >> 8;
  }

  pix = tiledata;

  (linebuf.bg[0] + 8)[i] = ((dircolor ? DirColCvt(pix) : CGRAM[pix]) << 16) | (pix ? prio_or : 0);
  if(extbg)
   (linebuf.bg[1] + 8)[i] = ((dircolor ? DirColCvt(pix & 0x7F) : CGRAM[pix & 0x7F]) << 16) | ((pix & 0x7F) ? (uint16)(prio_or_bg1 >> ((pix & 0x80) >> 3)) : 0);

  x += xinc;
  y += yinc;
 }
}


// Y mosaic is handled in DrawBG
#pragma GCC push_options
#pragma GCC optimize("no-unroll-loops,no-peel-loops,no-crossjumping")
template<bool hires = false>
static MDFN_HOT MDFN_FASTCALL NO_INLINE void DoXMosaic(unsigned layernum, uint32* MDFN_RESTRICT buf)
{
 if(!(Mosaic & (1U << layernum)))
  return;

 if(!hires && !(Mosaic & 0xF0))
  return;

 if(hires)
 {
  const unsigned sub_max = Mosaic >> 4;

  for(unsigned x = 0; x < 512;)
  {
   uint32 b = buf[x];
   for(int sub = sub_max; sub >= 0; sub--, x += 2)
   {
    buf[x + 0] = b;
    buf[x + 1] = b;
   }
  }
 }
 else switch(Mosaic >> 4)
 {
  case 0x1: for(unsigned x = 0; x < 256; x += 0x2) { uint32 b = buf[x]; buf[x + 1] = b; } break;
  case 0x2: for(unsigned x = 0; x < 256; x += 0x3) { uint32 b = buf[x]; buf[x + 1] = b; buf[x + 2] = b; } break;
  case 0x3: for(unsigned x = 0; x < 256; x += 0x4) { uint32 b = buf[x]; buf[x + 1] = b; buf[x + 2] = b; buf[x + 3] = b; } break;
  case 0x4: for(unsigned x = 0; x < 256; x += 0x5) { uint32 b = buf[x]; buf[x + 1] = b; buf[x + 2] = b; buf[x + 3] = b; buf[x + 4] = b; } break;
  case 0x5: for(unsigned x = 0; x < 256; x += 0x6) { uint32 b = buf[x]; buf[x + 1] = b; buf[x + 2] = b; buf[x + 3] = b; buf[x + 4] = b; buf[x + 5] = b; } break;
  case 0x6: for(unsigned x = 0; x < 256; x += 0x7) { uint32 b = buf[x]; buf[x + 1] = b; buf[x + 2] = b; buf[x + 3] = b; buf[x + 4] = b; buf[x + 5] = b; buf[x + 6] = b; } break;
  case 0x7: for(unsigned x = 0; x < 256; x += 0x8) { uint32 b = buf[x]; buf[x + 1] = b; buf[x + 2] = b; buf[x + 3] = b; buf[x + 4] = b; buf[x + 5] = b; buf[x + 6] = b; buf[x + 7] = b; } break;
  case 0x8: for(unsigned x = 0; x < 256; x += 0x9) { uint32 b = buf[x]; buf[x + 1] = b; buf[x + 2] = b; buf[x + 3] = b; buf[x + 4] = b; buf[x + 5] = b; buf[x + 6] = b; buf[x + 7] = b; buf[x + 8] = b; } break;
  case 0x9: for(unsigned x = 0; x < 256; x += 0xA) { uint32 b = buf[x]; buf[x + 1] = b; buf[x + 2] = b; buf[x + 3] = b; buf[x + 4] = b; buf[x + 5] = b; buf[x + 6] = b; buf[x + 7] = b; buf[x + 8] = b; buf[x + 9] = b; } break;
  case 0xA: for(unsigned x = 0; x < 256; x += 0xB) { uint32 b = buf[x]; buf[x + 1] = b; buf[x + 2] = b; buf[x + 3] = b; buf[x + 4] = b; buf[x + 5] = b; buf[x + 6] = b; buf[x + 7] = b; buf[x + 8] = b; buf[x + 9] = b; buf[x + 10] = b; } break;
  case 0xB: for(unsigned x = 0; x < 256; x += 0xC) { uint32 b = buf[x]; buf[x + 1] = b; buf[x + 2] = b; buf[x + 3] = b; buf[x + 4] = b; buf[x + 5] = b; buf[x + 6] = b; buf[x + 7] = b; buf[x + 8] = b; buf[x + 9] = b; buf[x + 10] = b; buf[x + 11] = b; } break;
  case 0xC: for(unsigned x = 0; x < 256; x += 0xD) { uint32 b = buf[x]; buf[x + 1] = b; buf[x + 2] = b; buf[x + 3] = b; buf[x + 4] = b; buf[x + 5] = b; buf[x + 6] = b; buf[x + 7] = b; buf[x + 8] = b; buf[x + 9] = b; buf[x + 10] = b; buf[x + 11] = b; buf[x + 12] = b; } break;
  case 0xD: for(unsigned x = 0; x < 256; x += 0xE) { uint32 b = buf[x]; buf[x + 1] = b; buf[x + 2] = b; buf[x + 3] = b; buf[x + 4] = b; buf[x + 5] = b; buf[x + 6] = b; buf[x + 7] = b; buf[x + 8] = b; buf[x + 9] = b; buf[x + 10] = b; buf[x + 11] = b; buf[x + 12] = b; buf[x + 13] = b; } break;
  case 0xE: for(unsigned x = 0; x < 256; x += 0xF) { uint32 b = buf[x]; buf[x + 1] = b; buf[x + 2] = b; buf[x + 3] = b; buf[x + 4] = b; buf[x + 5] = b; buf[x + 6] = b; buf[x + 7] = b; buf[x + 8] = b; buf[x + 9] = b; buf[x + 10] = b; buf[x + 11] = b; buf[x + 12] = b; buf[x + 13] = b; buf[x + 14] = b; } break;
  case 0xF: for(unsigned x = 0; x < 256; x += 0x10) { uint32 b = buf[x]; buf[x + 1] = b; buf[x + 2] = b; buf[x + 3] = b; buf[x + 4] = b; buf[x + 5] = b; buf[x + 6] = b; buf[x + 7] = b; buf[x + 8] = b; buf[x + 9] = b; buf[x + 10] = b; buf[x + 11] = b; buf[x + 12] = b; buf[x + 13] = b; buf[x + 14] = b; buf[x + 15] = b;} break;
 }
}
#pragma GCC pop_options

static INLINE void CalcWindowPieces(void)
{
 WindowPieces[0] = WindowPos[0][0];
 WindowPieces[1] = WindowPos[0][1] + 1;

 if(WindowPieces[0] > WindowPieces[1])
  WindowPieces[0] = WindowPieces[1] = 0;

 WindowPieces[2] = WindowPos[1][0];
 WindowPieces[3] = WindowPos[1][1] + 1;

 if(WindowPieces[2] > WindowPieces[3])
  WindowPieces[2] = WindowPieces[3] = 0;

 if(WindowPieces[0] > WindowPieces[2])
  std::swap(WindowPieces[0], WindowPieces[2]);

 if(WindowPieces[1] > WindowPieces[3])
  std::swap(WindowPieces[1], WindowPieces[3]);

 if(WindowPieces[1] > WindowPieces[2])
  std::swap(WindowPieces[1], WindowPieces[2]);

 WindowPieces[4] = 0x100;
}

template<bool cwin = false, bool hires = false>
static MDFN_HOT MDFN_FASTCALL void DoWindow(unsigned layernum, uint32* MDFN_RESTRICT buf)
{
 const unsigned mask_settings = (WMSettings[layernum >> 1] >> ((layernum & 1) << 2)) & 0xF;
 const unsigned mask_logic = (WMLogic >> (layernum * 2)) & 0x3;
 uint32 masker[2];	// out, in
 bool W0Enabled, W1Enabled;
 bool W0Invert, W1Invert;

 //if(mask_settings)
 // printf("Layer: %u, mask_settings = 0x%02x\n", layernum, mask_settings);

 // mask_settings
 // d0 = window1 inversion
 // d1 = window1 enable
 // d2 = window2 inversion
 // d3 = window2 enable

 W0Invert = mask_settings & 0x01;
 W0Enabled = mask_settings & 0x02;

 W1Invert = mask_settings & 0x04;
 W1Enabled = mask_settings & 0x08;

 if(!W0Enabled && !W1Enabled && !cwin)
  return;

 masker[0] = ~0U;
 masker[1] = ~0U;

 if(cwin)
 {
  assert(layernum == 5);

  // ~1, not ~3 (otherwise will break half-color math testing stuff).
  switch((CGWSEL >> 4) & 0x3)
  {
   case 0: break;
   case 1: masker[0] &= ~1; break;
   case 2: masker[1] &= ~1; break;
   case 3: masker[0] &= ~1; masker[1] &= ~1; break;
  }

  switch((CGWSEL >> 6) & 0x3)
  {
   case 0: break;
   case 1: masker[0] &= 9; break;
   case 2: masker[1] &= 9; break;
   case 3: masker[0] &= 9; masker[1] &= 9; break;
  }
 }
 else
 {
  if(WMMainEnable & (1U << layernum))
  {
   masker[1] &= ~0x00FF;
  }

  if(WMSubEnable & (1U << layernum))
  {
   masker[1] &= ~0xFF00;
  }

  if(!((WMMainEnable | WMSubEnable) & (1U << layernum)))
   return;
 }

#if 0
 if(scanline == 100 || scanline == 140 || scanline == 50)
 {
  printf("Scanline=%3u, Layer %u window: masker[0]=0x%08x, masker[1]=0x%08x, W0Enabled=%d, W1Enabled=%d, W0Invert=%d, W1Invert=%d, mask_logic=0x%02x, Window0 %u...%u, Window1 %u...%u --- CGWSEL=0x%02x, CGADSUB=0x%02x, MSEnable=0x%02x, SSEnable=0x%02x, BGMode=0x%02x, WMMainEnable=0x%02x, WMSubEnable=0x%02x, WMSettings=0x%02x 0x%02x 0x%02x\n",
	scanline,
	layernum, 
	masker[0], masker[1],
	W0Enabled, W1Enabled,
	W0Invert, W1Invert,
	mask_logic,
	WindowPos[0][0], WindowPos[0][1],	
	WindowPos[1][0], WindowPos[1][1],
	CGWSEL,
        CGADSUB,
	MSEnable,
	SSEnable,
	BGMode,
	WMMainEnable,
	WMSubEnable,
	WMSettings[0],
	WMSettings[1],
	WMSettings[2]);
 }
#endif

#if 0
 if(cwin && scanline == 100)
 {
  printf("Color window: masker[0]=0x%08x, masker[1]=0x%08x, W0Enabled=%d, W1Enabled=%d, W0Invert=%d, W1Invert=%d, mask_logic=0x%02x, Window0 %u...%u, Window1 %u...%u\n", masker[0], masker[1],
	W0Enabled, W1Enabled,
	W0Invert, W1Invert,
	mask_logic,
	WindowPos[0][0], WindowPos[0][1],	
	WindowPos[1][0], WindowPos[1][1]);
 }
#endif

 {
  unsigned i = 0;

  for(unsigned piece = 0; piece < 5; piece++)
  {
   bool wir[2];
   bool w = false;

   wir[0] = (i >= WindowPos[0][0] && i <= WindowPos[0][1]) ^ W0Invert;
   wir[1] = (i >= WindowPos[1][0] && i <= WindowPos[1][1]) ^ W1Invert;

   if(W0Enabled && W1Enabled)
   {
    switch(mask_logic)
    {
     case 0: w = wir[0] | wir[1]; break;
     case 1: w = wir[0] & wir[1]; break;
     case 2: w = wir[0] ^ wir[1]; break;
     case 3: w = !(wir[0] ^ wir[1]); break;
    }
   }
   else if(W0Enabled)
    w = wir[0];
   else if(W1Enabled)
    w = wir[1];

   //if(scanline == 100)
   //{
   // printf(" Apply mask 0x%08x to %u ... %u --- wir[0]=%u, wir[1]=%u, w=%u\n", masker[w], i, WindowPieces[piece], wir[0], wir[1], w);
   //}

   for(uint32 eff_mask = masker[w]; MDFN_LIKELY(i < WindowPieces[piece]); i++)
   {
    if(hires)
    {
     (buf + 0)[i << 1] &= eff_mask;
     (buf + 1)[i << 1] &= eff_mask;
    }
    else
     buf[i] &= eff_mask;
   }
  }
 }
}


template<unsigned bpp, bool palbase_n = false, bool opt = false, bool hires = false>
static INLINE void DoBGLayer(unsigned n, uint32 bgprio)
{
 if(bpp == 8 && (CGWSEL & 0x01))
 {
  if(BGMode & (0x10 << n))
   DrawBG<true,  bpp, palbase_n, opt, hires, true>(n, scanline, bgprio);
  else
   DrawBG<false, bpp, palbase_n, opt, hires, true>(n, scanline, bgprio);
 }
 else
 {
  if(BGMode & (0x10 << n))
   DrawBG<true,  bpp, palbase_n, opt, hires, false>(n, scanline, bgprio);
  else
   DrawBG<false, bpp, palbase_n, opt, hires, false>(n, scanline, bgprio);
 }

 if(hires)
 {
  DoXMosaic<true>(n, &linebuf.bghr[n][8]);
  DoWindow<false, true>(n, &linebuf.bghr[n][8]);
 }
 else
 {
  DoXMosaic(n, &linebuf.bg[n][8]);
  DoWindow(n, &linebuf.bg[n][8]);
 }
}

template<bool half, bool subtract>
static MDFN_HOT MDFN_FASTCALL uint32 CMath(uint32 tmp, uint32 other_color)
{
 if(half)
 {
  if(subtract)
  {
   uint32 diff = tmp - other_color + 0x8420;
   uint32 borrow = (diff - ((tmp ^ other_color) & 0x8420)) & 0x8420;

   tmp = (((diff - borrow) & (borrow - (borrow >> 5))) & 0x7BDE) >> 1;
  }
  else
  {
   tmp = ((tmp + other_color) - ((tmp ^ other_color) & 0x0421)) >> 1;
  }
 }
 else
 {
  if(subtract)
  {
   uint32 diff = tmp - other_color + 0x8420;
   uint32 borrow = (diff - ((tmp ^ other_color) & 0x8420)) & 0x8420;

   tmp = (diff - borrow) & (borrow - (borrow >> 5));
  }
  else
  {
   uint32 sum = tmp + other_color;
   uint32 carry = (sum - ((tmp ^ other_color) & 0x421)) & 0x8420;

   tmp = (sum - carry) | (carry - (carry >> 5));
  }
 }

 return tmp;
}

// armSNES: the canvas's pixel, the color with the brightness above it.
static uint32 OutBrightness;

static INLINE uint32 ConvertRGB555(uint32 tmp)
{
 return (tmp & 0x7FFF) | OutBrightness;
}

template<bool any_hires, unsigned cmath_mode, bool hires_cmath_add_subscreen = false, typename T>
static MDFN_HOT MDFN_FASTCALL NO_INLINE void MixMainSubSubSubMarine(T* MDFN_RESTRICT target)
{
 if(any_hires)
 {
  // armSNES: color math on a line twice as wide as the S-PPU does it. The main screen pixel is mathed
  // as on any line; the sub screen pixel left of it with the math settings and the color of the main
  // screen pixel before it.
  const bool subtract = cmath_mode & 2;
  uint32 prev_main = 0, prev_sub = 0;

  for(unsigned i = 0; i < 256; i++)
  {
   uint32 main = linebuf.main[i];
   uint32 sub = linebuf.sub[i];
   uint32 main_color = (main & 2) ? main >> 16 : 0;
   uint32 sub_color = sub >> 16;
   uint32 out_main = main_color;
   uint32 out_sub = (prev_main & 2) ? sub_color : 0;

   if(main & 1)
   {
    const bool with_sub = hires_cmath_add_subscreen && !(sub & 0x8);
    const bool half = (cmath_mode & 1) && (main & 2) && (with_sub || !hires_cmath_add_subscreen);
    const uint32 other = with_sub ? sub_color : FixedColor;

    if(subtract)
     out_main = half ? CMath<true, true>(main_color, other) : CMath<false, true>(main_color, other);
    else
     out_main = half ? CMath<true, false>(main_color, other) : CMath<false, false>(main_color, other);
   }

   if(prev_main & 1)
   {
    const bool with_main = hires_cmath_add_subscreen && !(prev_sub & 0x8);
    const bool half = (cmath_mode & 1) && (prev_main & 2) && (with_main || !hires_cmath_add_subscreen);
    const uint32 other = with_main ? prev_main >> 16 : FixedColor;

    if(subtract)
     out_sub = half ? CMath<true, true>(out_sub, other) : CMath<false, true>(out_sub, other);
    else
     out_sub = half ? CMath<true, false>(out_sub, other) : CMath<false, false>(out_sub, other);
   }

   target[(i << 1) + 0] = ConvertRGB555(out_sub);
   target[(i << 1) + 1] = ConvertRGB555(out_main);
   prev_main = main;
   prev_sub = sub;
  }
 }
 else
 {
  for(unsigned i = 0; i < 256; i++)
  {
   uint32 main = linebuf.main[i];
   uint32 sub = linebuf.sub[i];
   uint32 tmp = main >> 16;

   if(main & 1)
   {
    uint16 other_color = sub >> 16;

    //assert(main != sub);

    if((cmath_mode & 1) && (main & sub & 2))	// Halving mathing
     tmp = CMath<true, (bool)(cmath_mode & 2)>(tmp, other_color);
    else
     tmp = CMath<false, (bool)(cmath_mode & 2)>(tmp, other_color);
   }

   target[(i << 1) + 0] = target[(i << 1) + 1] = ConvertRGB555(tmp);
  }
 }
}

template<bool any_hires, bool rgb565, typename T>
static INLINE void MixMainSub(T* MDFN_RESTRICT target)
{
 if(any_hires && MDFN_UNLIKELY(CGWSEL & 0x2))
 {
  switch((CGADSUB >> 6) & 0x3)
  {
   case 0: MixMainSubSubSubMarine<any_hires, 0, true>(target); break;
   case 1: MixMainSubSubSubMarine<any_hires, 1, true>(target); break;
   case 2: MixMainSubSubSubMarine<any_hires, 2, true>(target); break;
   case 3: MixMainSubSubSubMarine<any_hires, 3, true>(target); break;
  }
 }
 else 
 {
  switch((CGADSUB >> 6) & 0x3)
  {
   case 0: MixMainSubSubSubMarine<any_hires, 0>(target); break;
   case 1: MixMainSubSubSubMarine<any_hires, 1>(target); break;
   case 2: MixMainSubSubSubMarine<any_hires, 2>(target); break;
   case 3: MixMainSubSubSubMarine<any_hires, 3>(target); break;
  }
 }

}

#ifdef ARCH_X86
template<unsigned w, bool sub_unique>
static INLINE void PrioHelper(uint32& main, uint32& sub, uint32 np, uint32 nps = 0)
{
 if(sub_unique)
 {
  asm("cmpb %%cl, %%al\n\t"  "cmovb %%ecx, %%eax\n\t" : "=a"(main), "=b"(sub) : "a"(main), "b"(sub), "c"(np) : "cc");
  asm("cmpb %%dh, %%bh\n\t"  "cmovb %%edx, %%ebx\n\t" : "=a"(main), "=b"(sub) : "a"(main), "b"(sub), "d"(nps) : "cc");
 }
 else
 {
  if(w & 1)
  {
   asm("cmpb %%dl, %%al\n\t"  "cmovb %%edx, %%eax\n\t" : "=a"(main), "=b"(sub) : "a"(main), "b"(sub), "d"(np) : "cc");
   asm("cmpb %%dh, %%bh\n\t"  "cmovb %%edx, %%ebx\n\t" : "=a"(main), "=b"(sub) : "a"(main), "b"(sub), "d"(np) : "cc");
  }
  else
  {
   asm("cmpb %%cl, %%al\n\t"  "cmovb %%ecx, %%eax\n\t" : "=a"(main), "=b"(sub) : "a"(main), "b"(sub), "c"(np) : "cc");
   asm("cmpb %%ch, %%bh\n\t"  "cmovb %%ecx, %%ebx\n\t" : "=a"(main), "=b"(sub) : "a"(main), "b"(sub), "c"(np) : "cc");
  }
 }
}
#else
template<unsigned w, bool sub_unique>
static INLINE void PrioHelper(uint32& main, uint32& sub, uint32 np, uint32 nps = 0)
{
 if((uint8)np > (uint8)main)
  main = np;

 if(sub_unique)
 {
  if((uint16)nps > (uint16)sub)
   sub = nps;
 }
 else
 {
  if((uint16)np > (uint16)sub)
   sub = np;
 }
}
#endif

// hrop =  1 for modes 5 and 6 hires
// hrop = -1 for pseudo-hires
template<bool mix_bg0, bool mix_bg1, bool mix_bg2, bool mix_bg3, int hrop>
static MDFN_HOT void MixLayersSub(void)
{
 uint32 main_back, sub_back;

 main_back = (CGRAM[0] << 16) | ((CGADSUB >> 5) & 1) | 2 | 0x808;

 //
 // Doing the subscreen FixedColor color math optimization doesn't really work out in hires mode...
 //
 if(hrop)
  sub_back = CGRAM[0] << 16;
 else
 {
  sub_back = (FixedColor << 16);

  // If only color mathing with FixedColor and not subscreen per-se, set half-math-allow bit(0x2), and force the priority to be above
  // any other layers that might otherwise unintentionally get mixed in.
  if(!(CGWSEL & 0x2))
   sub_back |= 0xF002;
 }

 sub_back |= 0x808;

 for(unsigned i = 0; i < 256; i++)
 {
  uint32 main = main_back, sub = sub_back;

  PrioHelper<0, false>(main, sub, (PPU.objbuf + 8)[i]);

  if(mix_bg0)
  {
   if(hrop == 1)
    PrioHelper<1, true>(main, sub, (linebuf.bghr[0] + 8 + 1)[i * 2], (linebuf.bghr[0] + 8 + 0)[i * 2]);
   else
    PrioHelper<1, false>(main, sub, (linebuf.bg[0] + 8)[i]);
  }
 
  if(mix_bg1)
  {
   if(hrop == 1)
    PrioHelper<2, true>(main, sub, (linebuf.bghr[1] + 8 + 1)[i * 2], (linebuf.bghr[1] + 8 + 0)[i * 2]);
   else
    PrioHelper<2, false>(main, sub, (linebuf.bg[1] + 8)[i]);
  }

  if(mix_bg2)
   PrioHelper<3, false>(main, sub, (linebuf.bg[2] + 8)[i]);

  if(mix_bg3)
   PrioHelper<4, false>(main, sub, (linebuf.bg[3] + 8)[i]);

  linebuf.main[i] = main;
  linebuf.sub[i] = sub;
 }
}

template<bool mix_bg0, bool mix_bg1, bool mix_bg2, bool mix_bg3, bool hires = false>
static INLINE void MixLayers(void)
{
 if(MDFN_UNLIKELY(hires))
  MixLayersSub<mix_bg0, mix_bg1, mix_bg2, mix_bg3,  hires>();
 else if(MDFN_UNLIKELY(ScreenMode & 0x08))
  MixLayersSub<mix_bg0, mix_bg1, mix_bg2, mix_bg3, -1>();
 else
  MixLayersSub<mix_bg0, mix_bg1, mix_bg2, mix_bg3, 0>();
}

static INLINE void GetBGPrioWCMBits(uint32* bgprio, unsigned count)
{
 for(unsigned i = 0; i < count; i++)
 {
  bgprio[i] |= (((CGADSUB >> i) & 1) * 0x00010001) | 0x00020002;

  if((SSEnable & (1U << i)))
   bgprio[i] |= (bgprio[i] & 0x00F000F0) << 8;

  if(!(MSEnable & (1U << i)))
   bgprio[i] &= ~0x00F000F0;
 }
}

static INLINE void DrawBGAndMixToMS(void)
{
 if((BGMode & 0x7) == 1)
 {
  uint32 bgprio[3] = { 0x00B00080, 0x00A00070, 0x00500020 + ((uint32)(BGMode & 0x8) << 20) };

  GetBGPrioWCMBits(bgprio, sizeof(bgprio) / sizeof(bgprio[0]));
  
  DoBGLayer<4>(0, bgprio[0]);
  DoBGLayer<4>(1, bgprio[1]);
  DoBGLayer<2>(2, bgprio[2]);

  MixLayers<true, true, true, false>();
 }
 else if((BGMode & 0x7) == 0)
 {
  uint32 bgprio[4] = { 0x00B00080, 0x00A00070, 0x00500020, 0x00400010};

  GetBGPrioWCMBits(bgprio, sizeof(bgprio) / sizeof(bgprio[0]));

  for(unsigned i = 0; i < 4; i++)
   DoBGLayer<2, true>(i, bgprio[i]);

  MixLayers<true, true, true, true>();
 }
 else if((BGMode & 0x3) == 2)
 {
  uint32 bgprio[2] = { 0x00A00040, 0x00800020 };

  GetBGPrioWCMBits(bgprio, sizeof(bgprio) / sizeof(bgprio[0]));

  if(BGMode & (0x10 << 2))
   GetOPTStrip<true>(2);
  else
   GetOPTStrip<false>(2);

  DoBGLayer<4, false, true>(0, bgprio[0]);
  DoBGLayer<4, false, true>(1, bgprio[1]);

  MixLayers<true, true, false, false>();
 }
 else if((BGMode & 0x7) == 3)
 {
  uint32 bgprio[2] = { 0x00A00040, 0x00800020 };

  GetBGPrioWCMBits(bgprio, sizeof(bgprio) / sizeof(bgprio[0]));

  DoBGLayer<8>(0, bgprio[0]);
  DoBGLayer<4>(1, bgprio[1]);

  MixLayers<true, true, false, false>();
 }
 else if((BGMode & 0x7) == 4)
 {
  uint32 bgprio[2] = { 0x00A00040, 0x00800020 };

  GetBGPrioWCMBits(bgprio, sizeof(bgprio) / sizeof(bgprio[0]));

  if(BGMode & (0x10 << 2))
   GetOPTStrip<true,  true>(2);
  else
   GetOPTStrip<false, true>(2);

  DoBGLayer<8, false, true>(0, bgprio[0]);
  DoBGLayer<2, false, true>(1, bgprio[1]);

  MixLayers<true, true, false, false>();
 }

 else if((BGMode & 0x7) == 5)
 {
  uint32 bgprio[2] = { 0x00A00040, 0x00800020 };

  GetBGPrioWCMBits(bgprio, sizeof(bgprio) / sizeof(bgprio[0]));

  DoBGLayer<4, false, false, true>(0, bgprio[0]);
  DoBGLayer<2, false, false, true>(1, bgprio[1]);

  MixLayers<true, true, false, false, true>();
 }
 else if((BGMode & 0x7) == 7)
 {
  if(MDFN_UNLIKELY(ScreenMode & 0x40))	// "EXTBG"
  {
   uint32 bgprio[2] = { 0x0040, 0x00800020 };

   GetBGPrioWCMBits(bgprio, sizeof(bgprio) / sizeof(bgprio[0]));

   if(CGWSEL & 1)
    DrawMODE7<true, true>(scanline, bgprio[0], bgprio[1]);
   else
    DrawMODE7<true, false>(scanline, bgprio[0], bgprio[1]);
   DoXMosaic(0, &linebuf.bg[0][8]);
   DoWindow(0, &linebuf.bg[0][8]);
   DoXMosaic(1, &linebuf.bg[1][8]);
   DoWindow(1, &linebuf.bg[1][8]);
   MixLayers<true, true, false, false>();
  }
  else
  {
   uint32 bgprio[1] = { 0x0040 };

   GetBGPrioWCMBits(bgprio, sizeof(bgprio) / sizeof(bgprio[0]));

   if(CGWSEL & 1)
    DrawMODE7<false, true>(scanline, bgprio[0]);
   else
    DrawMODE7<false, false>(scanline, bgprio[0]);
   DoXMosaic(0, &linebuf.bg[0][8]);
   DoWindow(0, &linebuf.bg[0][8]);
   MixLayers<true, false, false, false>();
  }
 }
}

// armSNES: the line's sprite tiles, as fetched by the S-PPU, laid out as DrawSprites lays out its own.
static INLINE void DrawSprites(const FastPpuLine* l)
{
 unsigned prio_or_mask = 0xFFFF;

 memset(PPU.objbuf + 8, 0, sizeof(PPU.objbuf[0]) * 256);

 if(!(CGADSUB & 0x10))
  prio_or_mask &= ~0x0001;

 if(!(SSEnable & 0x10))
  prio_or_mask &= ~0xF000;

 if(!(MSEnable & 0x10))
  prio_or_mask &= ~0x00F0;

 for(unsigned n = 0; n < l->spriteCount; n++)
 {
  const FastPpuSprite* t = &l->sprites[n];

  if(t->x <= -8 || t->x >= 256)
   continue;

  uint32* tb = PPU.objbuf + 8 + t->x;
  const uint32 prio_or = ((t->priority + 1) * 0x3030 | ((t->palette & 0x40) >> 6) | 2) & prio_or_mask;

  for(unsigned px = 0; px < 8; px++)
  {
   const unsigned shift = t->hflip ? px : 7 - px;
   const unsigned color = ((t->data >> shift) & 1) | ((t->data >> (shift + 7)) & 2) | ((t->data >> (shift + 14)) & 4) | ((t->data >> (shift + 21)) & 8);

   if(color)
    tb[px] = (CGRAM[(uint8)(t->palette + color)] << 16) | prio_or;
  }
 }
}

} // namespace

extern "C" void fastPpuLine(const FastPpuLine* l)
{
 static bool ready;

 if(!ready)
 {
  for(unsigned i = 0; i < 16; i++)
   PPU.SpriteTileTab[i] = (i & 0x1) | ((i & 0x2) << 3) | ((i & 0x4) << 6) | ((i & 0x8) << 9);
  ready = true;
 }

 if(!l->out)
  return;

 if(l->blank)
 {
  for(unsigned i = 0; i < 512; i++)
   l->out[i] = l->brightness;
  return;
 }

 VRAM = l->vram;
 CGRAM = l->cgram;
 scanline = l->y;
 Status[1] = l->field << 7;
 ScreenMode = l->screenMode;
 BGMode = l->bgMode;
 Mosaic = l->mosaic;
 MosaicYOffset = l->mosaicYOffset;
 memcpy(BGSC, l->bgsc, sizeof(BGSC));
 memcpy(BGNBA, l->bgnba, sizeof(BGNBA));
 memcpy(BGHOFS, l->bghofs, sizeof(BGHOFS));
 memcpy(BGVOFS, l->bgvofs, sizeof(BGVOFS));
 M7SEL = l->m7sel;
 memcpy(M7Matrix, l->m7Matrix, sizeof(M7Matrix));
 memcpy(M7Center, l->m7Center, sizeof(M7Center));
 M7HOFS = l->m7hofs;
 M7VOFS = l->m7vofs;
 MSEnable = l->msEnable;
 SSEnable = l->ssEnable;
 memcpy(WMSettings, l->wmSettings, sizeof(WMSettings));
 WMMainEnable = l->wmMainEnable;
 WMSubEnable = l->wmSubEnable;
 WMLogic = l->wmLogic;
 memcpy(WindowPos, l->windowPos, sizeof(WindowPos));
 CGWSEL = l->cgwsel;
 CGADSUB = l->cgadsub;
 FixedColor = l->fixedColor;
 OutBrightness = l->brightness;

 CalcWindowPieces();
 DrawSprites(l);
 DoWindow(4, &PPU.objbuf[8]);
 DrawBGAndMixToMS();
 DoWindow<true>(5, linebuf.main);

 if((BGMode & 0x7) == 0x5 || (BGMode & 0x7) == 0x6 || (ScreenMode & 0x08))
  MixMainSub<true, false>(l->out);
 else
  MixMainSub<false, false>(l->out);

 if(l->hiresFirstBlack)
  l->out[0] = l->brightness;
}

// armSNES: the renderer on a thread of its own, fed in order through a ring of commands.
namespace {

enum { RING = 1 << 16, POOL = 1024 };
enum { CMD_VRAM, CMD_CGRAM, CMD_LINE };

uint16 VramCopy[32768];
uint16 CgramCopy[256];
uint64 Ring[RING];
FastPpuLine Pool[POOL];
uint32 RingHead, RingTail;   // written by the main thread, by the render thread
uint32 HeadLocal, TailSeen;  // the main thread's: where it has written to, the tail it last saw
void (*Sink)(const uint32_t* line);
FastPpuLine Scratch;
uint32 LinesGiven, LinesDone;
pthread_t Thread;
pthread_mutex_t Lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t Wake = PTHREAD_COND_INITIALIZER;
bool Started, Sleeping, Stopping;

void* RenderThread(void*)
{
 for(;;)
 {
  uint32 tail = RingTail;

  while(tail != __atomic_load_n(&RingHead, __ATOMIC_ACQUIRE))
  {
   const uint64 c = Ring[tail & (RING - 1)];
   const uint32 kind = c >> 56;

   if(kind == CMD_VRAM)
    VramCopy[(c >> 16) & 0x7FFF] = (uint16)c;
   else if(kind == CMD_CGRAM)
    CgramCopy[(c >> 16) & 0xFF] = (uint16)c;
   else
   {
    FastPpuLine* l = &Pool[(uint32)c & (POOL - 1)];
    l->vram = VramCopy;
    l->cgram = CgramCopy;
    fastPpuLine(l);
    if(Sink && l->out)
     Sink(l->out);
    __atomic_store_n(&LinesDone, LinesDone + 1, __ATOMIC_RELEASE);
   }
   tail++;
   __atomic_store_n(&RingTail, tail, __ATOMIC_RELEASE);
  }

  for(unsigned spin = 0; spin < 2000 && tail == __atomic_load_n(&RingHead, __ATOMIC_ACQUIRE); spin++)
   sched_yield();

  // Sleeping is set before the ring is looked at again, and Push looks at Sleeping after it adds
  // to the ring, both in one order for both threads: one of them sees the other.
  pthread_mutex_lock(&Lock);
  __atomic_store_n(&Sleeping, true, __ATOMIC_SEQ_CST);
  while(tail == __atomic_load_n(&RingHead, __ATOMIC_SEQ_CST) && !Stopping)
   pthread_cond_wait(&Wake, &Lock);
  __atomic_store_n(&Sleeping, false, __ATOMIC_SEQ_CST);
  const bool stop = Stopping && tail == __atomic_load_n(&RingHead, __ATOMIC_SEQ_CST);
  pthread_mutex_unlock(&Lock);
  if(stop)
   return NULL;
 }
 return NULL;
}

void WakeIfSleeping(void)
{
 if(__atomic_load_n(&Sleeping, __ATOMIC_SEQ_CST))
 {
  pthread_mutex_lock(&Lock);
  pthread_cond_signal(&Wake);
  pthread_mutex_unlock(&Lock);
 }
}

// What was written to the ring, given to the thread, which is woken for it.
void Publish(void)
{
 __atomic_store_n(&RingHead, HeadLocal, __ATOMIC_SEQ_CST);
 WakeIfSleeping();
}

// VRAM and CGRAM writes are given to the thread only with the line or fence that always follows them.
inline void Push(uint64 c)
{
 if(HeadLocal - TailSeen >= RING)
 {
  TailSeen = __atomic_load_n(&RingTail, __ATOMIC_ACQUIRE);
  if(HeadLocal - TailSeen >= RING)
   Publish();
  while(HeadLocal - TailSeen >= RING)
  {
   sched_yield();
   WakeIfSleeping();
   TailSeen = __atomic_load_n(&RingTail, __ATOMIC_ACQUIRE);
  }
 }

 Ring[HeadLocal++ & (RING - 1)] = c;
}

} // namespace

extern "C" FastPpuLine* fastPpuNext(bool threaded)
{
 if(threaded && !Started)
  Started = pthread_create(&Thread, NULL, RenderThread, NULL) == 0;
 if(!threaded || !Started)
  return &Scratch;

 while(LinesGiven - __atomic_load_n(&LinesDone, __ATOMIC_ACQUIRE) >= POOL)
  sched_yield();

 return &Pool[LinesGiven & (POOL - 1)];
}

extern "C" void fastPpuSubmit(FastPpuLine* l)
{
 if(l == &Scratch)
 {
  fastPpuLine(l);
  return;
 }

 Push((uint64)CMD_LINE << 56 | (LinesGiven++ & (POOL - 1)));
 Publish();
}

extern "C" void fastPpuVram(uint32_t address, uint16_t data)
{
 if(Started)
  Push((uint64)CMD_VRAM << 56 | (uint64)(address & 0x7FFF) << 16 | data);
}

extern "C" void fastPpuCgram(uint32_t address, uint16_t data)
{
 if(Started)
  Push((uint64)CMD_CGRAM << 56 | (uint64)(address & 0xFF) << 16 | data);
}

extern "C" void fastPpuFence(void)
{
 if(!Started)
  return;

 Publish();
 while(__atomic_load_n(&RingTail, __ATOMIC_ACQUIRE) != HeadLocal)
 {
  WakeIfSleeping();
  sched_yield();
 }
 TailSeen = HeadLocal;
}

extern "C" void fastPpuSetSink(void (*sink)(const uint32_t* line))
{
 fastPpuStop();
 Sink = sink;
}

extern "C" void fastPpuStop(void)
{
 if(!Started)
  return;

 fastPpuFence();
 pthread_mutex_lock(&Lock);
 Stopping = true;
 pthread_cond_signal(&Wake);
 pthread_mutex_unlock(&Lock);
 pthread_join(Thread, NULL);
 Stopping = false;
 Started = false;
}

extern "C" void fastPpuResync(const uint16_t* vram, const uint16_t* cgram)
{
 fastPpuFence();
 memcpy(VramCopy, vram, sizeof(VramCopy));
 memcpy(CgramCopy, cgram, sizeof(CgramCopy));
}
