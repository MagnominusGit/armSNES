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
 * ppu_render_common.inc, drawing one line from the registers it is given. Built once for each render
 * thread (FASTPPU_COPY 0 and 1), so each has registers and line buffers of its own, and on x86-64 with
 * GCC again for CPUs with AVX2 (2 and 3). Its templates are functions given their parameters as
 * constants. */
#include "fastppu.h"

#include <string.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__x86_64__)
#include <immintrin.h>
#endif

#if FASTPPU_COPY < 2 || (defined(__x86_64__) && !defined(__clang__))

#if FASTPPU_COPY >= 2
#pragma GCC target("avx2")
#define FASTPPU_AVX2 1
#endif

#define FASTPPU_NAME2(name, copy) name##copy
#define FASTPPU_NAME(name, copy) FASTPPU_NAME2(name, copy)
#define Line FASTPPU_NAME(fastPpuRenderLine, FASTPPU_COPY)
#define LineBlock FASTPPU_NAME(fastPpuRenderBlock, FASTPPU_COPY)

typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;
typedef int16_t int16;
typedef struct FastPpuLine FastPpuLine;
typedef struct FastPpuSprite FastPpuSprite;

#define INLINE inline __attribute__((always_inline))
#define NO_INLINE __attribute__((noinline))
#define MDFN_HOT __attribute__((hot))
#define MDFN_FASTCALL
#define MDFN_RESTRICT __restrict__
#define MDFN_LIKELY(n) __builtin_expect(!!(n), 1)
#define MDFN_UNLIKELY(n) __builtin_expect(!!(n), 0)
#define assert(n) ((void) 0)

const uint32* Line(const FastPpuLine* l, bool direct);
const uint32* LineBlock(const FastPpuLine* l);

// The registers, as the ports were written; set from the line being drawn.
static const uint16* VRAM;
static const uint16* CGRAM;
static unsigned scanline;
static uint8 Status[2];
static uint8 ScreenMode;
static uint8 BGMode;
static uint8 Mosaic;
static uint8 MosaicYOffset;
static uint8 BGSC[4];
static uint8 BGNBA[2];
static uint16 BGHOFS[4];
static uint16 BGVOFS[4];
static uint8 M7SEL;
static int16 M7Matrix[4];
static int16 M7Step[4];
static int16 M7Center[2];
static int16 M7HOFS;
static int16 M7VOFS;
static uint8 MSEnable;
static uint8 SSEnable;
static uint8 WMSettings[3];
static uint8 WMMainEnable;
static uint8 WMSubEnable;
static uint16 WMLogic;
static uint8 WindowPos[2][2];
static unsigned WindowPieces[5];
static uint8 CGWSEL;
static uint8 CGADSUB;
static uint16 FixedColor;
// armSNES: HD mode 7, the point inside each pixel mode 7 is sampled at, in 1/M7SubN of a pixel (M7SubN 0:
// the pixel's corner, as the console samples).
static int M7SubX, M7SubY;
static int M7SubN;
// armSNES: widescreen, the console column the line's first pixel is drawn for; and whether sprites are
// drawn past the console's 256 columns.
static int XShift;
static bool SpritesWide;
// armSNES: a window's masks by console column, for a shifted line.
static uint32 WinCol[256];

static struct
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

static MDFN_HOT MDFN_FASTCALL INLINE void GetOPTStrip(const unsigned n, const bool size16, const bool mode4)
{
 unsigned VOFS = BGVOFS[n];
 unsigned HOFS = BGHOFS[n] + XShift;
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

static INLINE unsigned DirColCvt(unsigned inpix, unsigned palbase)
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

#undef FASTPPU_SIMD_TILE
#if defined(__aarch64__) || defined(FASTPPU_AVX2)
#define FASTPPU_SIMD_TILE 1
// armSNES: a 2 or 4 bpp tile's 8 pixels at once, as DrawBG's loop makes them: the color from the
// palette, the priority bits where the pixel isn't 0. bpa holds bitplanes 0-1, bpb 2-3.
#if !defined(__aarch64__)
static INLINE __m128i TilePlane(__m128i m, uint32 b, char value)
{
 return _mm_and_si128(_mm_cmpeq_epi8(_mm_and_si128(_mm_set1_epi8((char)b), m), m), _mm_set1_epi8(value));
}
#endif

static INLINE void DrawTile(uint32* MDFN_RESTRICT target, uint32 bpa, uint32 bpb, const uint16* cgr, uint32 prio, bool h_flip, const unsigned bpp)
{
 static const uint8 bits[2][8] = { { 0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01 }, { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80 } };
#if defined(__aarch64__)
 const uint8x8_t m = vld1_u8(bits[h_flip]);
 uint8x8_t pix = vand_u8(vtst_u8(vdup_n_u8((uint8)bpa), m), vdup_n_u8(1));
 pix = vorr_u8(pix, vand_u8(vtst_u8(vdup_n_u8((uint8)(bpa >> 8)), m), vdup_n_u8(2)));
 if(bpp == 4)
 {
  pix = vorr_u8(pix, vand_u8(vtst_u8(vdup_n_u8((uint8)bpb), m), vdup_n_u8(4)));
  pix = vorr_u8(pix, vand_u8(vtst_u8(vdup_n_u8((uint8)(bpb >> 8)), m), vdup_n_u8(8)));
 }
 const uint8x16x2_t pal = vld2q_u8((const uint8*)cgr);
 const uint8x8_t lo = vqtbl1_u8(pal.val[0], pix);
 const uint8x8_t hi = vqtbl1_u8(pal.val[1], pix);
 const uint16x8_t color = vreinterpretq_u16_u8(vcombine_u8(vzip1_u8(lo, hi), vzip2_u8(lo, hi)));
 const uint16x8_t shown = vreinterpretq_u16_s16(vmovl_s8(vreinterpret_s8_u8(vtst_u8(pix, pix))));
 const uint16x8_t pr = vandq_u16(shown, vdupq_n_u16((uint16)prio));
 vst1q_u32(target + 0, vreinterpretq_u32_u16(vzip1q_u16(pr, color)));
 vst1q_u32(target + 4, vreinterpretq_u32_u16(vzip2q_u16(pr, color)));
#else
 const __m128i m = _mm_loadl_epi64((const __m128i*)bits[h_flip]);
 __m128i pix = _mm_or_si128(TilePlane(m, bpa, 1), TilePlane(m, bpa >> 8, 2));
 if(bpp == 4)
  pix = _mm_or_si128(pix, _mm_or_si128(TilePlane(m, bpb, 4), TilePlane(m, bpb >> 8, 8)));
 const __m128i split = _mm_setr_epi8(0, 2, 4, 6, 8, 10, 12, 14, 1, 3, 5, 7, 9, 11, 13, 15);
 const __m128i p0 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)cgr), split);
 const __m128i p1 = bpp == 4 ? _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(cgr + 8)), split) : p0;
 const __m128i lo = _mm_shuffle_epi8(_mm_unpacklo_epi64(p0, p1), pix);
 const __m128i hi = _mm_shuffle_epi8(_mm_unpackhi_epi64(p0, p1), pix);
 const __m128i color = _mm_unpacklo_epi8(lo, hi);
 const __m128i blank = _mm_cmpeq_epi8(pix, _mm_setzero_si128());
 const __m128i pr = _mm_andnot_si128(_mm_unpacklo_epi8(blank, blank), _mm_set1_epi16((short)prio));
 _mm_storeu_si128((__m128i*)(target + 0), _mm_unpacklo_epi16(pr, color));
 _mm_storeu_si128((__m128i*)(target + 4), _mm_unpackhi_epi16(pr, color));
#endif
}

// armSNES: a sprite tile's 8 pixels (bitplanes 0-3 a byte each in data), as DrawSprites's loop draws
// them: only where the pixel isn't 0, over what is there.
static INLINE void DrawSpriteTile(uint32* MDFN_RESTRICT target, uint32 data, const uint16* cgr, uint32 prio, bool h_flip)
{
#if defined(__aarch64__)
 _Alignas(16) uint32 drawn[8];
 DrawTile(drawn, data & 0xFFFF, data >> 16, cgr, prio, h_flip, 4);
 for(unsigned half = 0; half < 2; half++)
 {
  const uint32x4_t d = vld1q_u32(drawn + half * 4);
  const uint32x4_t shown = vtstq_u32(d, vdupq_n_u32(0xFFFF));
  vst1q_u32(target + half * 4, vbslq_u32(shown, d, vld1q_u32(target + half * 4)));
 }
#else
 _Alignas(16) uint32 drawn[8];
 DrawTile(drawn, data & 0xFFFF, data >> 16, cgr, prio, h_flip, 4);
 for(unsigned half = 0; half < 2; half++)
 {
  const __m128i d = _mm_load_si128((const __m128i*)(drawn + half * 4));
  const __m128i blank = _mm_cmpeq_epi32(_mm_and_si128(d, _mm_set1_epi32(0xFFFF)), _mm_setzero_si128());
  const __m128i old = _mm_loadu_si128((const __m128i*)(target + half * 4));
  _mm_storeu_si128((__m128i*)(target + half * 4), _mm_or_si128(_mm_and_si128(blank, old), _mm_andnot_si128(blank, d)));
 }
#endif
}
#endif

static MDFN_HOT MDFN_FASTCALL INLINE void DrawBG(const unsigned n, const unsigned y, uint32 prio_or, const bool size16, const unsigned bpp, const bool palbase_n, const bool opt, const bool hires, const bool dircolor)
{
 _Alignas(4) uint16 tab[16];
 const bool MosaicOn = Mosaic & (1U << n);
 unsigned VOFS = y - (MosaicOn ? MosaicYOffset : 0);
 unsigned HOFS = BGHOFS[n] + XShift;
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

#ifdef FASTPPU_SIMD_TILE
  if(bpp == 2 || bpp == 4)
   DrawTile(target, vchr[0], bpp == 4 ? vchr[8] : 0, cgr, eff_prio_or, h_flip, bpp);
  else
#endif
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
   HOFS += XShift;

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
static MDFN_HOT MDFN_FASTCALL INLINE void DrawMODE7(unsigned line_y, uint16 prio_or, uint32 prio_or_bg1, const bool extbg, const bool dircolor)
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

 // armSNES: HD mode 7's perspective correction, the matrix as it changes to the row sampled.
 int16 m[4];
 for(unsigned k = 0; k < 4; k++)
  m[k] = M7SubN > 1 ? (int16)(M7Matrix[k] + M7Step[k] * M7SubY / M7SubN) : M7Matrix[k];

 x = M7Mul(m[0], hoca) + M7Mul(m[1], line_y) + M7Mul(m[1], voca) + (M7Center[0] << 8);
 y = M7Mul(m[2], hoca) + M7Mul(m[3], line_y) + M7Mul(m[3], voca) + (M7Center[1] << 8);

 xinc = m[0];
 yinc = m[2];

 if(h_flip)
 {
  x += 255 * xinc;
  y += 255 * yinc;

  xinc = -xinc;
  yinc = -yinc;
 }

 x += XShift * (int)xinc;
 y += XShift * (int)yinc;

 if(M7SubN > 1)
 {
  const int dy = v_flip ? -M7SubY : M7SubY;
  x += (m[1] * dy + (int)xinc * M7SubX) / M7SubN;
  y += (m[3] * dy + (int)yinc * M7SubX) / M7SubN;
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

  (linebuf.bg[0] + 8)[i] = ((dircolor ? DirColCvt(pix, 0) : CGRAM[pix]) << 16) | (pix ? prio_or : 0);
  if(extbg)
   (linebuf.bg[1] + 8)[i] = ((dircolor ? DirColCvt(pix & 0x7F, 0) : CGRAM[pix & 0x7F]) << 16) | ((pix & 0x7F) ? (uint16)(prio_or_bg1 >> ((pix & 0x80) >> 3)) : 0);

  x += xinc;
  y += yinc;
 }
}

// DrawMODE7 with EXTBG's second layer or not, and direct color or the palette.
static MDFN_HOT void DrawMODE7Plain(unsigned line_y, uint16 prio_or, uint32 prio_or_bg1) { DrawMODE7(line_y, prio_or, prio_or_bg1, false, false); }
static MDFN_HOT void DrawMODE7Direct(unsigned line_y, uint16 prio_or, uint32 prio_or_bg1) { DrawMODE7(line_y, prio_or, prio_or_bg1, false, true); }
static MDFN_HOT void DrawMODE7Ext(unsigned line_y, uint16 prio_or, uint32 prio_or_bg1) { DrawMODE7(line_y, prio_or, prio_or_bg1, true, false); }
static MDFN_HOT void DrawMODE7ExtDirect(unsigned line_y, uint16 prio_or, uint32 prio_or_bg1) { DrawMODE7(line_y, prio_or, prio_or_bg1, true, true); }


// Y mosaic is handled in DrawBG
#pragma GCC push_options
#pragma GCC optimize("no-unroll-loops,no-peel-loops,no-crossjumping")
static MDFN_HOT MDFN_FASTCALL INLINE void DoXMosaicBody(unsigned layernum, uint32* MDFN_RESTRICT buf, const bool hires)
{
 if(!(Mosaic & (1U << layernum)))
  return;

 if(!hires && !(Mosaic & 0xF0))
  return;

 // armSNES: a shifted line's blocks start on the console's columns; one begun before the line's first
 // pixel takes that pixel.
 if(XShift)
 {
  const int size = (Mosaic >> 4) + 1;
  for(int c = 0; c < 256; c++)
  {
   int m = (c + XShift) % size;
   int start = c - (m < 0 ? m + size : m);
   if(start < 0)
    start = 0;
   if(hires)
   {
    buf[2 * c + 0] = buf[2 * start];
    buf[2 * c + 1] = buf[2 * start];
   }
   else
    buf[c] = buf[start];
  }
  return;
 }

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

static MDFN_HOT MDFN_FASTCALL NO_INLINE void DoXMosaic(unsigned layernum, uint32* MDFN_RESTRICT buf)
{
 DoXMosaicBody(layernum, buf, false);
}

static MDFN_HOT MDFN_FASTCALL NO_INLINE void DoXMosaicHires(unsigned layernum, uint32* MDFN_RESTRICT buf)
{
 DoXMosaicBody(layernum, buf, true);
}
#pragma GCC pop_options

static INLINE void SwapPieces(unsigned a, unsigned b)
{
 const unsigned t = WindowPieces[a];
 WindowPieces[a] = WindowPieces[b];
 WindowPieces[b] = t;
}

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
  SwapPieces(0, 2);

 if(WindowPieces[1] > WindowPieces[3])
  SwapPieces(1, 3);

 if(WindowPieces[1] > WindowPieces[2])
  SwapPieces(1, 2);

 WindowPieces[4] = 0x100;
}

static MDFN_HOT MDFN_FASTCALL INLINE void DoWindowBody(unsigned layernum, uint32* MDFN_RESTRICT buf, const bool cwin, const bool hires)
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

   if(XShift)
   {
    for(; i < WindowPieces[piece]; i++)
     WinCol[i] = masker[w];
    continue;
   }

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

 // armSNES: past the console's 256 columns, the window is as it is at the nearest one.
 if(XShift)
 {
  for(int c = 0; c < 256; c++)
  {
   const int at = c + XShift;
   const uint32 eff_mask = WinCol[at < 0 ? 0 : at > 255 ? 255 : at];
   if(hires)
   {
    (buf + 0)[c << 1] &= eff_mask;
    (buf + 1)[c << 1] &= eff_mask;
   }
   else
    buf[c] &= eff_mask;
  }
 }
}

static MDFN_HOT MDFN_FASTCALL void DoWindow(unsigned layernum, uint32* MDFN_RESTRICT buf)
{
 DoWindowBody(layernum, buf, false, false);
}

static MDFN_HOT MDFN_FASTCALL void DoWindowHires(unsigned layernum, uint32* MDFN_RESTRICT buf)
{
 DoWindowBody(layernum, buf, false, true);
}

static MDFN_HOT MDFN_FASTCALL void DoColorWindow(unsigned layernum, uint32* MDFN_RESTRICT buf)
{
 DoWindowBody(layernum, buf, true, false);
}


static INLINE void DoBGLayer(unsigned n, uint32 bgprio, const unsigned bpp, const bool palbase_n, const bool opt, const bool hires)
{
 if(bpp == 8 && (CGWSEL & 0x01))
 {
  if(BGMode & (0x10 << n))
   DrawBG(n, scanline, bgprio, true,  bpp, palbase_n, opt, hires, true);
  else
   DrawBG(n, scanline, bgprio, false, bpp, palbase_n, opt, hires, true);
 }
 else
 {
  if(BGMode & (0x10 << n))
   DrawBG(n, scanline, bgprio, true,  bpp, palbase_n, opt, hires, false);
  else
   DrawBG(n, scanline, bgprio, false, bpp, palbase_n, opt, hires, false);
 }

 if(hires)
 {
  DoXMosaicHires(n, &linebuf.bghr[n][8]);
  DoWindowHires(n, &linebuf.bghr[n][8]);
 }
 else
 {
  DoXMosaic(n, &linebuf.bg[n][8]);
  DoWindow(n, &linebuf.bg[n][8]);
 }

 // armSNES: widescreen, as bsnes-hd does it: a layer one screen wide at no scroll is taken for a status
 // bar, kept to the console's 256 columns.
 if(XShift && !(BGSC[n] & 0x1) && !BGHOFS[n] && !BGVOFS[n])
 {
  uint32* buf = hires ? &linebuf.bghr[n][8] : &linebuf.bg[n][8];
  for(int c = 0; c < 256; c++)
  {
   if((unsigned)(c + XShift) >= 256)
   {
    if(hires)
     buf[2 * c] = buf[2 * c + 1] = 0;
    else
     buf[c] = 0;
   }
  }
 }
}

// A layer for each of DoBGLayer's uses: its bits a pixel, the BG mode 0 palettes, offset-per-tile, hires.
static MDFN_HOT void DoBGLayer2(unsigned n, uint32 bgprio) { DoBGLayer(n, bgprio, 2, false, false, false); }
static MDFN_HOT void DoBGLayer4(unsigned n, uint32 bgprio) { DoBGLayer(n, bgprio, 4, false, false, false); }
static MDFN_HOT void DoBGLayer8(unsigned n, uint32 bgprio) { DoBGLayer(n, bgprio, 8, false, false, false); }
static MDFN_HOT void DoBGLayerMode0(unsigned n, uint32 bgprio) { DoBGLayer(n, bgprio, 2, true, false, false); }
static MDFN_HOT void DoBGLayer2Opt(unsigned n, uint32 bgprio) { DoBGLayer(n, bgprio, 2, false, true, false); }
static MDFN_HOT void DoBGLayer4Opt(unsigned n, uint32 bgprio) { DoBGLayer(n, bgprio, 4, false, true, false); }
static MDFN_HOT void DoBGLayer8Opt(unsigned n, uint32 bgprio) { DoBGLayer(n, bgprio, 8, false, true, false); }
static MDFN_HOT void DoBGLayer2Hires(unsigned n, uint32 bgprio) { DoBGLayer(n, bgprio, 2, false, false, true); }
static MDFN_HOT void DoBGLayer4Hires(unsigned n, uint32 bgprio) { DoBGLayer(n, bgprio, 4, false, false, true); }

static MDFN_HOT MDFN_FASTCALL INLINE uint32 CMath(uint32 tmp, uint32 other_color, const bool half, const bool subtract)
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

static MDFN_HOT MDFN_FASTCALL INLINE void MixMainSubSubSubMarineBody(uint32* MDFN_RESTRICT target, const bool any_hires, const unsigned cmath_mode, const bool hires_cmath_add_subscreen)
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
     out_main = half ? CMath(main_color, other, true, true) : CMath(main_color, other, false, true);
    else
     out_main = half ? CMath(main_color, other, true, false) : CMath(main_color, other, false, false);
   }

   if(prev_main & 1)
   {
    const bool with_main = hires_cmath_add_subscreen && !(prev_sub & 0x8);
    const bool half = (cmath_mode & 1) && (prev_main & 2) && (with_main || !hires_cmath_add_subscreen);
    const uint32 other = with_main ? prev_main >> 16 : FixedColor;

    if(subtract)
     out_sub = half ? CMath(out_sub, other, true, true) : CMath(out_sub, other, false, true);
    else
     out_sub = half ? CMath(out_sub, other, true, false) : CMath(out_sub, other, false, false);
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
     tmp = CMath(tmp, other_color, true, cmath_mode & 2);
    else
     tmp = CMath(tmp, other_color, false, cmath_mode & 2);
   }

   target[(i << 1) + 0] = target[(i << 1) + 1] = ConvertRGB555(tmp);
  }
 }
}

// MixMainSubSubSubMarine for each of its uses: hires or not, the color math mode, hires math with the sub screen.
#define MARINE(name, any_hires, cmath_mode, hires_cmath_add_subscreen) \
 static MDFN_HOT MDFN_FASTCALL NO_INLINE void name(uint32* MDFN_RESTRICT target) { MixMainSubSubSubMarineBody(target, any_hires, cmath_mode, hires_cmath_add_subscreen); }
MARINE(Marine0, false, 0, false)
MARINE(Marine1, false, 1, false)
MARINE(Marine2, false, 2, false)
MARINE(Marine3, false, 3, false)
MARINE(MarineHires0, true, 0, false)
MARINE(MarineHires1, true, 1, false)
MARINE(MarineHires2, true, 2, false)
MARINE(MarineHires3, true, 3, false)
MARINE(MarineHiresSub0, true, 0, true)
MARINE(MarineHiresSub1, true, 1, true)
MARINE(MarineHiresSub2, true, 2, true)
MARINE(MarineHiresSub3, true, 3, true)
#undef MARINE

static void (*const MarineFor[3][4])(uint32* MDFN_RESTRICT target) =
{
 { Marine0, Marine1, Marine2, Marine3 },
 { MarineHires0, MarineHires1, MarineHires2, MarineHires3 },
 { MarineHiresSub0, MarineHiresSub1, MarineHiresSub2, MarineHiresSub3 },
};

static INLINE void MixMainSub(uint32* MDFN_RESTRICT target, const bool any_hires)
{
 if(any_hires && MDFN_UNLIKELY(CGWSEL & 0x2))
  MarineFor[2][(CGADSUB >> 6) & 0x3](target);
 else
  MarineFor[any_hires][(CGADSUB >> 6) & 0x3](target);
}

#ifdef ARCH_X86
static INLINE void PrioHelper(uint32* main, uint32* sub, uint32 np, uint32 nps, const unsigned w, const bool sub_unique)
{
 if(sub_unique)
 {
  asm("cmpb %%cl, %%al\n\t"  "cmovb %%ecx, %%eax\n\t" : "=a"(*main), "=b"(*sub) : "a"(*main), "b"(*sub), "c"(np) : "cc");
  asm("cmpb %%dh, %%bh\n\t"  "cmovb %%edx, %%ebx\n\t" : "=a"(*main), "=b"(*sub) : "a"(*main), "b"(*sub), "d"(nps) : "cc");
 }
 else
 {
  if(w & 1)
  {
   asm("cmpb %%dl, %%al\n\t"  "cmovb %%edx, %%eax\n\t" : "=a"(*main), "=b"(*sub) : "a"(*main), "b"(*sub), "d"(np) : "cc");
   asm("cmpb %%dh, %%bh\n\t"  "cmovb %%edx, %%ebx\n\t" : "=a"(*main), "=b"(*sub) : "a"(*main), "b"(*sub), "d"(np) : "cc");
  }
  else
  {
   asm("cmpb %%cl, %%al\n\t"  "cmovb %%ecx, %%eax\n\t" : "=a"(*main), "=b"(*sub) : "a"(*main), "b"(*sub), "c"(np) : "cc");
   asm("cmpb %%ch, %%bh\n\t"  "cmovb %%ecx, %%ebx\n\t" : "=a"(*main), "=b"(*sub) : "a"(*main), "b"(*sub), "c"(np) : "cc");
  }
 }
}
#else
static INLINE void PrioHelper(uint32* main, uint32* sub, uint32 np, uint32 nps, const unsigned w, const bool sub_unique)
{
 if((uint8)np > (uint8)*main)
  *main = np;

 if(sub_unique)
 {
  if((uint16)nps > (uint16)*sub)
   *sub = nps;
 }
 else
 {
  if((uint16)np > (uint16)*sub)
   *sub = np;
 }
}
#endif

// hrop =  1 for modes 5 and 6 hires
// hrop = -1 for pseudo-hires
static MDFN_HOT INLINE void MixLayersSub(const bool mix_bg0, const bool mix_bg1, const bool mix_bg2, const bool mix_bg3, const int hrop)
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

  PrioHelper(&main, &sub, (PPU.objbuf + 8)[i], 0, 0, false);

  if(mix_bg0)
  {
   if(hrop == 1)
    PrioHelper(&main, &sub, (linebuf.bghr[0] + 8 + 1)[i * 2], (linebuf.bghr[0] + 8 + 0)[i * 2], 1, true);
   else
    PrioHelper(&main, &sub, (linebuf.bg[0] + 8)[i], 0, 1, false);
  }
 
  if(mix_bg1)
  {
   if(hrop == 1)
    PrioHelper(&main, &sub, (linebuf.bghr[1] + 8 + 1)[i * 2], (linebuf.bghr[1] + 8 + 0)[i * 2], 2, true);
   else
    PrioHelper(&main, &sub, (linebuf.bg[1] + 8)[i], 0, 2, false);
  }

  if(mix_bg2)
   PrioHelper(&main, &sub, (linebuf.bg[2] + 8)[i], 0, 3, false);

  if(mix_bg3)
   PrioHelper(&main, &sub, (linebuf.bg[3] + 8)[i], 0, 4, false);

  linebuf.main[i] = main;
  linebuf.sub[i] = sub;
 }
}

static INLINE void MixLayers(const bool mix_bg0, const bool mix_bg1, const bool mix_bg2, const bool mix_bg3, const bool hires)
{
 if(MDFN_UNLIKELY(hires))
  MixLayersSub(mix_bg0, mix_bg1, mix_bg2, mix_bg3,  hires);
 else if(MDFN_UNLIKELY(ScreenMode & 0x08))
  MixLayersSub(mix_bg0, mix_bg1, mix_bg2, mix_bg3, -1);
 else
  MixLayersSub(mix_bg0, mix_bg1, mix_bg2, mix_bg3, 0);
}

// MixLayers for the layers each BG mode has.
static MDFN_HOT void MixLayers1(void) { MixLayers(true, false, false, false, false); }
static MDFN_HOT void MixLayers2(void) { MixLayers(true, true, false, false, false); }
static MDFN_HOT void MixLayers3(void) { MixLayers(true, true, true, false, false); }
static MDFN_HOT void MixLayers4(void) { MixLayers(true, true, true, true, false); }
static MDFN_HOT void MixLayers2Hires(void) { MixLayers(true, true, false, false, true); }

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

// The mode 7 layers drawn and mixed with the sprites.
static INLINE void DrawMode7AndMix(void)
{
 if(MDFN_UNLIKELY(ScreenMode & 0x40))	// "EXTBG"
 {
  uint32 bgprio[2] = { 0x0040, 0x00800020 };

  GetBGPrioWCMBits(bgprio, sizeof(bgprio) / sizeof(bgprio[0]));

  if(CGWSEL & 1)
   DrawMODE7ExtDirect(scanline, bgprio[0], bgprio[1]);
  else
   DrawMODE7Ext(scanline, bgprio[0], bgprio[1]);
  DoXMosaic(0, &linebuf.bg[0][8]);
  DoWindow(0, &linebuf.bg[0][8]);
  DoXMosaic(1, &linebuf.bg[1][8]);
  DoWindow(1, &linebuf.bg[1][8]);
  MixLayers2();
 }
 else
 {
  uint32 bgprio[1] = { 0x0040 };

  GetBGPrioWCMBits(bgprio, sizeof(bgprio) / sizeof(bgprio[0]));

  if(CGWSEL & 1)
   DrawMODE7Direct(scanline, bgprio[0], 0);
  else
   DrawMODE7Plain(scanline, bgprio[0], 0);
  DoXMosaic(0, &linebuf.bg[0][8]);
  DoWindow(0, &linebuf.bg[0][8]);
  MixLayers1();
 }
}

static INLINE void DrawBGAndMixToMS(void)
{
 if((BGMode & 0x7) == 1)
 {
  uint32 bgprio[3] = { 0x00B00080, 0x00A00070, 0x00500020 + ((uint32)(BGMode & 0x8) << 20) };

  GetBGPrioWCMBits(bgprio, sizeof(bgprio) / sizeof(bgprio[0]));
  
  DoBGLayer4(0, bgprio[0]);
  DoBGLayer4(1, bgprio[1]);
  DoBGLayer2(2, bgprio[2]);

  MixLayers3();
 }
 else if((BGMode & 0x7) == 0)
 {
  uint32 bgprio[4] = { 0x00B00080, 0x00A00070, 0x00500020, 0x00400010};

  GetBGPrioWCMBits(bgprio, sizeof(bgprio) / sizeof(bgprio[0]));

  for(unsigned i = 0; i < 4; i++)
   DoBGLayerMode0(i, bgprio[i]);

  MixLayers4();
 }
 else if((BGMode & 0x3) == 2)
 {
  uint32 bgprio[2] = { 0x00A00040, 0x00800020 };

  GetBGPrioWCMBits(bgprio, sizeof(bgprio) / sizeof(bgprio[0]));

  if(BGMode & (0x10 << 2))
   GetOPTStrip(2, true, false);
  else
   GetOPTStrip(2, false, false);

  DoBGLayer4Opt(0, bgprio[0]);
  DoBGLayer4Opt(1, bgprio[1]);

  MixLayers2();
 }
 else if((BGMode & 0x7) == 3)
 {
  uint32 bgprio[2] = { 0x00A00040, 0x00800020 };

  GetBGPrioWCMBits(bgprio, sizeof(bgprio) / sizeof(bgprio[0]));

  DoBGLayer8(0, bgprio[0]);
  DoBGLayer4(1, bgprio[1]);

  MixLayers2();
 }
 else if((BGMode & 0x7) == 4)
 {
  uint32 bgprio[2] = { 0x00A00040, 0x00800020 };

  GetBGPrioWCMBits(bgprio, sizeof(bgprio) / sizeof(bgprio[0]));

  if(BGMode & (0x10 << 2))
   GetOPTStrip(2, true,  true);
  else
   GetOPTStrip(2, false, true);

  DoBGLayer8Opt(0, bgprio[0]);
  DoBGLayer2Opt(1, bgprio[1]);

  MixLayers2();
 }

 else if((BGMode & 0x7) == 5)
 {
  uint32 bgprio[2] = { 0x00A00040, 0x00800020 };

  GetBGPrioWCMBits(bgprio, sizeof(bgprio) / sizeof(bgprio[0]));

  DoBGLayer4Hires(0, bgprio[0]);
  DoBGLayer2Hires(1, bgprio[1]);

  MixLayers2Hires();
 }
 else if((BGMode & 0x7) == 7)
  DrawMode7AndMix();
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

  const int x = t->x - XShift;
  if(x <= -8 || x >= 256)
   continue;

  uint32* tb = PPU.objbuf + 8 + x;
  const uint32 prio_or = ((t->priority + 1) * 0x3030 | ((t->palette & 0x40) >> 6) | 2) & prio_or_mask;

#ifdef FASTPPU_SIMD_TILE
  DrawSpriteTile(tb, t->data, CGRAM + (uint8)t->palette, prio_or, t->hflip);
  continue;
#endif
  for(unsigned px = 0; px < 8; px++)
  {
   const unsigned shift = t->hflip ? px : 7 - px;
   const unsigned color = ((t->data >> shift) & 1) | ((t->data >> (shift + 7)) & 2) | ((t->data >> (shift + 14)) & 4) | ((t->data >> (shift + 21)) & 8);

   if(color)
    tb[px] = (CGRAM[(uint8)(t->palette + color)] << 16) | prio_or;
  }
 }

 // armSNES: sprites kept to the console's 256 columns.
 if(XShift && !SpritesWide)
 {
  for(int c = 0; c < 256; c++)
  {
   if((unsigned)(c + XShift) >= 256)
    PPU.objbuf[8 + c] = 0;
  }
 }
}

// armSNES: where a line goes when it isn't drawn into the canvas (fastPpuSetSink).
static uint32 Direct[512];

const uint32* Line(const FastPpuLine* l, bool direct)
{
 static bool ready;

 if(!ready)
 {
  for(unsigned i = 0; i < 16; i++)
   PPU.SpriteTileTab[i] = (i & 0x1) | ((i & 0x2) << 3) | ((i & 0x4) << 6) | ((i & 0x8) << 9);
  ready = true;
 }

 if(!l->out)
  return NULL;

 uint32* const out = direct ? Direct : l->out;

 if(l->blank)
 {
  for(unsigned i = 0; i < 512; i++)
   out[i] = l->brightness;
  return out;
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
 memcpy(M7Step, l->m7Step, sizeof(M7Step));
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
 DoColorWindow(5, linebuf.main);

 if((BGMode & 0x7) == 0x5 || (BGMode & 0x7) == 0x6 || (ScreenMode & 0x08))
  MixMainSub(out, true);
 else
  MixMainSub(out, false);

 if(l->hiresFirstBlack && -XShift >= 0 && -XShift < 256)
  out[2 * -XShift] = l->brightness;
 return out;
}

// armSNES: HD mode 7, the line Line last drew again with only its mode 7 layers sampled anew: the sprites,
// windows and registers are as it left them.
static const uint32* Mode7Again(const FastPpuLine* l)
{
 DrawMode7AndMix();
 DoColorWindow(5, linebuf.main);

 if(ScreenMode & 0x08)
  MixMainSub(Direct, true);
 else
  MixMainSub(Direct, false);

 if(l->hiresFirstBlack && -XShift >= 0 && -XShift < 256)
  Direct[2 * -XShift] = l->brightness;
 return Direct;
}

// armSNES: widescreen, one row of a line with wide columns more each side, as hires pairs: drawn in
// two passes from wide columns left of the console's first, the second overlapping the first by 16, so
// mosaic blocks and offset-per-tile columns start where they do on the console. The columns between
// match the line drawn on its own.
static void DrawWide(const FastPpuLine* l, uint32* row)
{
 const int wide = l->wide;

 if(!wide)
 {
  memcpy(row, Line(l, true), 512 * sizeof(uint32));
  return;
 }

 if(!l->wideLayers)
 {
  memset(row, 0, 2 * wide * sizeof(uint32));
  memcpy(row + 2 * wide, Line(l, true), 512 * sizeof(uint32));
  memset(row + 2 * wide + 512, 0, 2 * wide * sizeof(uint32));
  return;
 }

 SpritesWide = l->wideSprites;
 XShift = -wide;
 memcpy(row, Line(l, true), 512 * sizeof(uint32));
 XShift = 256 - wide - 16;
 memcpy(row + 512, Line(l, true) + 2 * 16, 4 * wide * sizeof(uint32));
 XShift = 0;
}

// armSNES: HD mode 7 and widescreen, the line as fastPpuSetHdSink gives it: with HD, drawn again for
// each point of a grid of scale by scale inside its pixels, scale rows of its columns * scale.
static uint32 Block[8 * 8 * (256 + 2 * FAST_PPU_WIDE_MOST)];

const uint32* LineBlock(const FastPpuLine* l)
{
 const unsigned n = l->hdScale > 1 ? l->hdScale : 1;
 const unsigned columns = 256 + 2 * l->wide;

 if(n == 1)
 {
  DrawWide(l, Block);
  return Block;
 }

 // As DrawWide draws them: each pass drawn once by Line, where the first point is sampled, then only
 // its mode 7 layers for the others. A pass is the shift, the first column taken and where it goes.
 const int wide = l->wide;
 struct Pass { int shift; unsigned from, to, count; } passes[2] = { { 0, 0, (unsigned)wide, 256 }, { 0, 0, 0, 0 } };
 unsigned count = 1;

 if(wide && l->wideLayers)
 {
  SpritesWide = l->wideSprites;
  passes[0] = (struct Pass){ -wide, 0, 0, 256 };
  passes[1] = (struct Pass){ 256 - wide - 16, 16, 256, 2 * (unsigned)wide };
  count = 2;
 }
 else if(wide)
 {
  for(unsigned j = 0; j < n; j++)
  {
   memset(Block + j * columns * n, 0, wide * n * sizeof(uint32));
   memset(Block + j * columns * n + (256 + wide) * n, 0, wide * n * sizeof(uint32));
  }
 }

 M7SubN = n;
 for(unsigned p = 0; p < count; p++)
 {
  XShift = passes[p].shift;
  for(unsigned j = 0; j < n; j++)
  {
   for(unsigned i = 0; i < n; i++)
   {
    M7SubX = i;
    M7SubY = j;
    const uint32* row = (i | j) ? Mode7Again(l) : Line(l, true);
    uint32* to = Block + j * columns * n + passes[p].to * n + i;
    for(unsigned x = 0; x < passes[p].count; x++)
     to[x * n] = row[2 * (passes[p].from + x)];
   }
  }
 }
 XShift = 0;
 M7SubN = 0;
 return Block;
}

#endif
