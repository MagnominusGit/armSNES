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
#include <unistd.h>
#include <utility>
#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__x86_64__)
#include <immintrin.h>
#endif

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

// armSNES: one renderer for each render thread.
namespace R0 {
#include "fastppu_render.inc"
}
namespace R1 {
#include "fastppu_render.inc"
}

// armSNES: on x86-64 with GCC, the same again for CPUs with AVX2, chosen as the renderer starts.
#if defined(__x86_64__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC target("avx2")
#define FASTPPU_AVX2 1
namespace A0 {
#include "fastppu_render.inc"
}
namespace A1 {
#include "fastppu_render.inc"
}
#undef FASTPPU_AVX2
#pragma GCC pop_options

static const uint32* (*const LineFor[2][2])(const FastPpuLine*, bool) = { { R0::Line, R1::Line }, { A0::Line, A1::Line } };
static const bool Avx2 = (__builtin_cpu_init(), __builtin_cpu_supports("avx2"));
#else
static const uint32* (*const LineFor[2][2])(const FastPpuLine*, bool) = { { R0::Line, R1::Line }, { R0::Line, R1::Line } };
static const bool Avx2 = false;
#endif

extern "C" void fastPpuLine(const FastPpuLine* l)
{
 LineFor[Avx2][0](l, false);
}

// armSNES: the renderer on threads of their own, fed in order through a ring of commands. Each thread
// takes in every VRAM and CGRAM write and draws every other line.
namespace {

enum { RING = 1 << 16, POOL = 1024, THREADS = 2 };
enum { CMD_VRAM, CMD_CGRAM, CMD_LINE };

uint16 VramCopy[THREADS][32768];
uint16 CgramCopy[THREADS][256];
uint64 Ring[RING];
FastPpuLine Pool[POOL];
// Each thread's writes on cache lines of their own, so the others' reads don't take them back and forth.
alignas(64) uint32 RingHead;  // written by the main thread
alignas(64) uint32 RingTail[THREADS];  // by each render thread
alignas(64) uint32 HeadLocal, TailSeen;  // the main thread's: where it has written to, the oldest tail it last saw
alignas(64) uint32 LinesGiven;
alignas(64) uint32 LinesDone[THREADS];
void (*Sink)(uint32_t* at, const uint32_t* pixels);
FastPpuLine Scratch;
pthread_t Thread[THREADS];
unsigned Count, Wanted;  // the threads running, and asked for (0: by the cores there are)
pthread_mutex_t Lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t Wake = PTHREAD_COND_INITIALIZER;
bool Started, Sleeping[THREADS], Stopping;

void* RenderThread(void* arg)
{
 const unsigned id = (unsigned)(uintptr_t)arg;

 for(;;)
 {
  uint32 tail = RingTail[id];

  while(tail != __atomic_load_n(&RingHead, __ATOMIC_ACQUIRE))
  {
   const uint64 c = Ring[tail & (RING - 1)];
   const uint32 kind = c >> 56;

   if(kind == CMD_VRAM)
    VramCopy[id][(c >> 16) & 0x7FFF] = (uint16)c;
   else if(kind == CMD_CGRAM)
    CgramCopy[id][(c >> 16) & 0xFF] = (uint16)c;
   else
   {
    const uint32 slot = (uint32)c & (POOL - 1);

    if(Count == 1 || (slot & 1) == id)
    {
     FastPpuLine* l = &Pool[slot];
     l->vram = VramCopy[id];
     l->cgram = CgramCopy[id];
     const uint32* pixels = LineFor[Avx2][id](l, Sink != NULL);
     if(Sink && l->out)
      Sink(l->out, pixels);
    }
    __atomic_store_n(&LinesDone[id], LinesDone[id] + 1, __ATOMIC_RELEASE);
   }
   tail++;
   __atomic_store_n(&RingTail[id], tail, __ATOMIC_RELEASE);
  }

  for(unsigned spin = 0; spin < 20 && tail == __atomic_load_n(&RingHead, __ATOMIC_ACQUIRE); spin++)
   sched_yield();

  // Sleeping is set before the ring is looked at again, and Publish looks at Sleeping after it adds
  // to the ring, both in one order for both sides: one of them sees the other.
  pthread_mutex_lock(&Lock);
  __atomic_store_n(&Sleeping[id], true, __ATOMIC_SEQ_CST);
  while(tail == __atomic_load_n(&RingHead, __ATOMIC_SEQ_CST) && !Stopping)
   pthread_cond_wait(&Wake, &Lock);
  __atomic_store_n(&Sleeping[id], false, __ATOMIC_SEQ_CST);
  const bool stop = Stopping && tail == __atomic_load_n(&RingHead, __ATOMIC_SEQ_CST);
  pthread_mutex_unlock(&Lock);
  if(stop)
   return NULL;
 }
 return NULL;
}

void WakeIfSleeping(void)
{
 bool any = false;

 for(unsigned n = 0; n < Count; n++)
  any |= __atomic_load_n(&Sleeping[n], __ATOMIC_SEQ_CST);

 if(any)
 {
  pthread_mutex_lock(&Lock);
  pthread_cond_broadcast(&Wake);
  pthread_mutex_unlock(&Lock);
 }
}

// How far the thread furthest behind is from the head.
uint32 Behind(void)
{
 uint32 most = 0;

 for(unsigned n = 0; n < Count; n++)
 {
  const uint32 d = HeadLocal - __atomic_load_n(&RingTail[n], __ATOMIC_ACQUIRE);
  most = d > most ? d : most;
 }
 return most;
}

// What was written to the ring, given to the threads, which are woken for it.
void Publish(void)
{
 __atomic_store_n(&RingHead, HeadLocal, __ATOMIC_SEQ_CST);
 WakeIfSleeping();
}

// VRAM and CGRAM writes are given to the threads only with the line or fence that always follows them.
inline void Push(uint64 c)
{
 if(HeadLocal - TailSeen >= RING)
 {
  TailSeen = HeadLocal - Behind();
  if(HeadLocal - TailSeen >= RING)
   Publish();
  while(HeadLocal - TailSeen >= RING)
  {
   sched_yield();
   WakeIfSleeping();
   TailSeen = HeadLocal - Behind();
  }
 }

 Ring[HeadLocal++ & (RING - 1)] = c;
}

// How many lines the thread furthest behind has still to pass.
uint32 LinesBehind(void)
{
 uint32 most = 0;

 for(unsigned n = 0; n < Count; n++)
 {
  const uint32 d = LinesGiven - __atomic_load_n(&LinesDone[n], __ATOMIC_ACQUIRE);
  most = d > most ? d : most;
 }
 return most;
}

void Start(void)
{
 unsigned wanted = Wanted;

 if(!wanted)
  wanted = sysconf(_SC_NPROCESSORS_ONLN) >= 4 ? 2 : 1;
 Count = 0;
 for(unsigned n = 0; n < wanted && n < THREADS; n++)
 {
  RingTail[n] = RingTail[0];
  LinesDone[n] = LinesDone[0];
  if(n)
  {
   memcpy(VramCopy[n], VramCopy[0], sizeof(VramCopy[n]));
   memcpy(CgramCopy[n], CgramCopy[0], sizeof(CgramCopy[n]));
  }
  if(pthread_create(&Thread[n], NULL, RenderThread, (void*)(uintptr_t)n))
   break;
  Count = n + 1;
 }
 Started = Count > 0;
}

} // namespace

extern "C" void fastPpuThreads(unsigned count)
{
 fastPpuStop();
 Wanted = count;
}

extern "C" FastPpuLine* fastPpuNext(bool threaded)
{
 if(threaded && !Started)
  Start();
 if(!threaded || !Started)
  return &Scratch;

 while(LinesBehind() >= POOL)
  sched_yield();

 return &Pool[LinesGiven & (POOL - 1)];
}

extern "C" void fastPpuSubmit(FastPpuLine* l)
{
 if(l == &Scratch)
 {
  const uint32* pixels = LineFor[Avx2][0](l, Sink != NULL);
  if(Sink && l->out)
   Sink(l->out, pixels);
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
 while(Behind())
 {
  WakeIfSleeping();
  sched_yield();
 }
 TailSeen = HeadLocal;
}

extern "C" void fastPpuSetSink(void (*sink)(uint32_t* at, const uint32_t* pixels))
{
 fastPpuStop();
 Sink = sink;
}

extern "C" bool fastPpuDirect(void)
{
 return Sink != NULL;
}

extern "C" void fastPpuStop(void)
{
 if(!Started)
  return;

 fastPpuFence();
 pthread_mutex_lock(&Lock);
 Stopping = true;
 pthread_cond_broadcast(&Wake);
 pthread_mutex_unlock(&Lock);
 for(unsigned n = 0; n < Count; n++)
  pthread_join(Thread[n], NULL);
 Stopping = false;
 Started = false;
}

extern "C" void fastPpuResync(const uint16_t* vram, const uint16_t* cgram)
{
 fastPpuFence();
 for(unsigned n = 0; n < THREADS; n++)
 {
  memcpy(VramCopy[n], vram, sizeof(VramCopy[n]));
  memcpy(CgramCopy[n], cgram, sizeof(CgramCopy[n]));
 }
}
