/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Fast mode's line renderer on threads of their own. The renderer is fastppu_render.c, built once for
 * each render thread, so each has registers and line buffers of its own, and on x86-64 again for
 * CPUs with AVX2. */
#define _GNU_SOURCE
#include "fastppu.h"

#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <unistd.h>

typedef struct FastPpuLine FastPpuLine;

const uint32_t* fastPpuRenderLine0(const FastPpuLine* l, bool direct);
const uint32_t* fastPpuRenderBlock0(const FastPpuLine* l);
const uint32_t* fastPpuRenderLine1(const FastPpuLine* l, bool direct);
const uint32_t* fastPpuRenderBlock1(const FastPpuLine* l);

#if defined(__x86_64__) && !defined(__clang__)
const uint32_t* fastPpuRenderLine2(const FastPpuLine* l, bool direct);
const uint32_t* fastPpuRenderBlock2(const FastPpuLine* l);
const uint32_t* fastPpuRenderLine3(const FastPpuLine* l, bool direct);
const uint32_t* fastPpuRenderBlock3(const FastPpuLine* l);

static const uint32_t* (*const LineFor[2][2])(const FastPpuLine*, bool) = { { fastPpuRenderLine0, fastPpuRenderLine1 }, { fastPpuRenderLine2, fastPpuRenderLine3 } };
static const uint32_t* (*const BlockFor[2][2])(const FastPpuLine*) = { { fastPpuRenderBlock0, fastPpuRenderBlock1 }, { fastPpuRenderBlock2, fastPpuRenderBlock3 } };

static bool Avx2(void) {
	static int known;
	if (!known) {
		__builtin_cpu_init();
		known = __builtin_cpu_supports("avx2") ? 1 : -1;
	}
	return known > 0;
}
#else
static const uint32_t* (*const LineFor[2][2])(const FastPpuLine*, bool) = { { fastPpuRenderLine0, fastPpuRenderLine1 }, { fastPpuRenderLine0, fastPpuRenderLine1 } };
static const uint32_t* (*const BlockFor[2][2])(const FastPpuLine*) = { { fastPpuRenderBlock0, fastPpuRenderBlock1 }, { fastPpuRenderBlock0, fastPpuRenderBlock1 } };

static bool Avx2(void) {
	return false;
}
#endif

void fastPpuLine(const FastPpuLine* l) {
	LineFor[Avx2()][0](l, false);
}

// The renderer on threads of their own, fed in order through a ring of commands. Each thread takes in
// every VRAM and CGRAM write and draws every other line.

enum { RING = 1 << 16, POOL = 1024, THREADS = 2 };
enum { CMD_VRAM, CMD_CGRAM, CMD_LINE };

static uint16_t VramCopy[THREADS][32768];
static uint16_t CgramCopy[THREADS][256];
static uint64_t Ring[RING];
static FastPpuLine Pool[POOL];
// Each thread's writes on cache lines of their own, so the others' reads don't take them back and forth.
static _Alignas(64) uint32_t RingHead;  // written by the main thread
static _Alignas(64) uint32_t RingTail[THREADS];  // by each render thread
static _Alignas(64) uint32_t HeadLocal, TailSeen;  // the main thread's: where it has written to, the oldest tail it last saw
static _Alignas(64) uint32_t LinesGiven;
static _Alignas(64) uint32_t LinesDone[THREADS];
static void (*Sink)(uint32_t* at, const uint32_t* pixels);
static void (*HdSink)(uint32_t* at, const uint32_t* block, unsigned scale, unsigned columns);
static FastPpuLine Scratch;

// A line for the sinks: a wide line only as a block, an HD line both ways.
static void Draw(const FastPpuLine* l, unsigned id) {
	const bool block = HdSink && l->out && (l->hdScale > 1 || l->wide);
	if (!block || (!l->wide && !l->blockOnly)) {
		const uint32_t* pixels = LineFor[Avx2()][id](l, Sink != NULL);
		if (Sink && l->out) {
			Sink(l->out, pixels);
		}
	}
	if (block) {
		HdSink(l->out, BlockFor[Avx2()][id](l), l->hdScale > 1 ? l->hdScale : 1, 256 + 2 * l->wide);
	}
}

static pthread_t Thread[THREADS];
static unsigned Count, Wanted;  // the threads running, and asked for (0: by the cores there are)
static pthread_mutex_t Lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t Wake = PTHREAD_COND_INITIALIZER;
static bool Started, Sleeping[THREADS], Stopping;

static void* RenderThread(void* arg) {
	const unsigned id = (unsigned) (uintptr_t) arg;

	for (;;) {
		uint32_t tail = RingTail[id];

		while (tail != __atomic_load_n(&RingHead, __ATOMIC_ACQUIRE)) {
			const uint64_t c = Ring[tail & (RING - 1)];
			const uint32_t kind = c >> 56;

			if (kind == CMD_VRAM) {
				VramCopy[id][(c >> 16) & 0x7FFF] = (uint16_t) c;
			} else if (kind == CMD_CGRAM) {
				CgramCopy[id][(c >> 16) & 0xFF] = (uint16_t) c;
			} else {
				const uint32_t slot = (uint32_t) c & (POOL - 1);

				if (Count == 1 || (slot & 1) == id) {
					FastPpuLine* l = &Pool[slot];
					l->vram = VramCopy[id];
					l->cgram = CgramCopy[id];
					Draw(l, id);
				}
				__atomic_store_n(&LinesDone[id], LinesDone[id] + 1, __ATOMIC_RELEASE);
			}
			tail++;
			__atomic_store_n(&RingTail[id], tail, __ATOMIC_RELEASE);
		}

		for (unsigned spin = 0; spin < 20 && tail == __atomic_load_n(&RingHead, __ATOMIC_ACQUIRE); spin++) {
			sched_yield();
		}

		// Sleeping is set before the ring is looked at again, and Publish looks at Sleeping after it adds
		// to the ring, both in one order for both sides: one of them sees the other.
		pthread_mutex_lock(&Lock);
		__atomic_store_n(&Sleeping[id], true, __ATOMIC_SEQ_CST);
		while (tail == __atomic_load_n(&RingHead, __ATOMIC_SEQ_CST) && !Stopping) {
			pthread_cond_wait(&Wake, &Lock);
		}
		__atomic_store_n(&Sleeping[id], false, __ATOMIC_SEQ_CST);
		const bool stop = Stopping && tail == __atomic_load_n(&RingHead, __ATOMIC_SEQ_CST);
		pthread_mutex_unlock(&Lock);
		if (stop) {
			return NULL;
		}
	}
	return NULL;
}

static void WakeIfSleeping(void) {
	bool any = false;

	for (unsigned n = 0; n < Count; n++) {
		any |= __atomic_load_n(&Sleeping[n], __ATOMIC_SEQ_CST);
	}

	if (any) {
		pthread_mutex_lock(&Lock);
		pthread_cond_broadcast(&Wake);
		pthread_mutex_unlock(&Lock);
	}
}

// How far the thread furthest behind is from the head.
static uint32_t Behind(void) {
	uint32_t most = 0;

	for (unsigned n = 0; n < Count; n++) {
		const uint32_t d = HeadLocal - __atomic_load_n(&RingTail[n], __ATOMIC_ACQUIRE);
		most = d > most ? d : most;
	}
	return most;
}

// What was written to the ring, given to the threads, which are woken for it.
static void Publish(void) {
	__atomic_store_n(&RingHead, HeadLocal, __ATOMIC_SEQ_CST);
	WakeIfSleeping();
}

// The same, but sleeping threads woken only every WAKE_LINES lines: a line comes about every 60
// microseconds, and woken for each, the threads slept again between them, so the CPU's thread made a
// system call a line to wake them, and they spun waiting. Threads still awake take each line at once.
enum { WAKE_LINES = 8 };

static void PublishLine(void) {
	__atomic_store_n(&RingHead, HeadLocal, __ATOMIC_SEQ_CST);
	if (!(LinesGiven % WAKE_LINES)) {
		WakeIfSleeping();
	}
}

// VRAM and CGRAM writes are given to the threads only with the line or fence that always follows them.
static inline void Push(uint64_t c) {
	if (HeadLocal - TailSeen >= RING) {
		TailSeen = HeadLocal - Behind();
		if (HeadLocal - TailSeen >= RING) {
			Publish();
		}
		while (HeadLocal - TailSeen >= RING) {
			sched_yield();
			WakeIfSleeping();
			TailSeen = HeadLocal - Behind();
		}
	}

	Ring[HeadLocal++ & (RING - 1)] = c;
}

// How many lines the thread furthest behind has still to pass.
static uint32_t LinesBehind(void) {
	uint32_t most = 0;

	for (unsigned n = 0; n < Count; n++) {
		const uint32_t d = LinesGiven - __atomic_load_n(&LinesDone[n], __ATOMIC_ACQUIRE);
		most = d > most ? d : most;
	}
	return most;
}

static void Start(void) {
	unsigned wanted = Wanted;

	if (!wanted) {
		wanted = fastPpuCores() >= 4 ? 2 : 1;
	}
	Count = 0;
	for (unsigned n = 0; n < wanted && n < THREADS; n++) {
		RingTail[n] = RingTail[0];
		LinesDone[n] = LinesDone[0];
		if (n) {
			memcpy(VramCopy[n], VramCopy[0], sizeof(VramCopy[n]));
			memcpy(CgramCopy[n], CgramCopy[0], sizeof(CgramCopy[n]));
		}
		if (pthread_create(&Thread[n], NULL, RenderThread, (void*) (uintptr_t) n)) {
			break;
		}
		Count = n + 1;
	}
	Started = Count > 0;
}

void fastPpuThreads(unsigned count) {
	fastPpuStop();
	Wanted = count;
}

FastPpuLine* fastPpuNext(bool threaded) {
	if (threaded && !Started) {
		Start();
	}
	if (!threaded || !Started) {
		return &Scratch;
	}

	while (LinesBehind() >= POOL) {
		Publish();
		sched_yield();
	}

	return &Pool[LinesGiven & (POOL - 1)];
}

void fastPpuSubmit(FastPpuLine* l) {
	if (l == &Scratch) {
		Draw(l, 0);
		return;
	}

	Push((uint64_t) CMD_LINE << 56 | (LinesGiven++ & (POOL - 1)));
	PublishLine();
}

void fastPpuVram(uint32_t address, uint16_t data) {
	if (Started) {
		Push((uint64_t) CMD_VRAM << 56 | (uint64_t) (address & 0x7FFF) << 16 | data);
	}
}

void fastPpuCgram(uint32_t address, uint16_t data) {
	if (Started) {
		Push((uint64_t) CMD_CGRAM << 56 | (uint64_t) (address & 0xFF) << 16 | data);
	}
}

void fastPpuFence(void) {
	if (!Started) {
		return;
	}

	Publish();
	while (Behind()) {
		WakeIfSleeping();
		sched_yield();
	}
	TailSeen = HeadLocal;
}

void fastPpuSetSink(void (*sink)(uint32_t* at, const uint32_t* pixels)) {
	fastPpuStop();
	Sink = sink;
}

unsigned fastPpuCores(void) {
#if defined(__linux__)
	cpu_set_t set;
	if (!sched_getaffinity(0, sizeof(set), &set) && CPU_COUNT(&set) > 0) {
		return CPU_COUNT(&set);
	}
#endif
	long cores = sysconf(_SC_NPROCESSORS_ONLN);
	return cores > 0 ? (unsigned) cores : 1;
}

void fastPpuSetHdSink(void (*sink)(uint32_t* at, const uint32_t* block, unsigned scale, unsigned columns)) {
	fastPpuStop();
	HdSink = sink;
}

bool fastPpuDirect(void) {
	return Sink != NULL;
}

void fastPpuStop(void) {
	if (!Started) {
		return;
	}

	fastPpuFence();
	pthread_mutex_lock(&Lock);
	Stopping = true;
	pthread_cond_broadcast(&Wake);
	pthread_mutex_unlock(&Lock);
	for (unsigned n = 0; n < Count; n++) {
		pthread_join(Thread[n], NULL);
	}
	Stopping = false;
	Started = false;
}

void fastPpuResync(const uint16_t* vram, const uint16_t* cgram) {
	fastPpuFence();
	for (unsigned n = 0; n < THREADS; n++) {
		memcpy(VramCopy[n], vram, sizeof(VramCopy[n]));
		memcpy(CgramCopy[n], cgram, sizeof(CgramCopy[n]));
	}
}
