/* Copyright (c) 2004-2025 ares team, Near et al
 * Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Ported from ares (ISC); see LICENSES/ISC-ares.txt. */
#ifndef ARMSNES_SNES_H
#define ARMSNES_SNES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

// Each processor keeps its own clock, in units that make the clocks of processors of different
// frequencies comparable. A processor that needs another's state runs it until it has caught up. The
// order they run in, down to equal clocks, is ares's, whose results this core reproduces.
struct Thread {
	u64 clock;
	u64 scalar;
};
#define THREAD_SECOND ((u64) -1 >> 1)

static inline void threadFrequency(struct Thread* thread, double frequency) {
	thread->scalar = THREAD_SECOND / (u64) (frequency + 0.5);
}

static inline void threadStep(struct Thread* thread, u32 clocks) {
	thread->clock += thread->scalar * clocks;
}

// nall's boolean edge helpers.
static inline bool flipTo(bool* data, bool value) {
	if (*data != value) {
		*data = value;
		return true;
	}
	return false;
}
static inline bool raiseTo(bool* data, bool value) {
	bool raised = !*data && value;
	*data = value;
	return raised;
}
static inline bool lowerTo(bool* data, bool value) {
	bool lowered = *data && !value;
	*data = value;
	return lowered;
}
static inline s32 sclamp16(s32 x) {
	return x > 0x7fff ? 0x7fff : x < -0x8000 ? -0x8000 : x;
}
// A value kept in a signed field of this many bits.
static inline s32 signedBits(s32 x, int bits) {
	return (s32) ((u32) x << (32 - bits)) >> (32 - bits);
}

// The machine: NTSC or PAL, and its two crystal frequencies.
struct System {
	bool pal;
	double cpuFrequency;
	double apuFrequency;
	// ares's power-on RAM patterns come from this generator, seeded with zero.
	u64 randomState;
	u64 randomIncrement;
	u32 frames;
	bool frameEvent;
};
extern struct System sys;

void systemLoad(bool pal);
void systemPower(bool reset);
// Runs until the PPU finishes a frame and the CPU finishes the instruction it was in.
void systemRunFrame(void);
u64 randomNext(void);
u64 randomBias(u64 bias);
void randomArray(u8* buffer, u32 size);

// --- S-SMP (SPC700) ---

struct SmpTimer {
	u8 stage0;
	u8 stage1;
	u8 stage2;
	u8 stage3;  // 4 bits
	bool line;
	bool enable;
	u8 target;
};

struct Smp {
	struct Thread th;
	u16 pc;
	u8 a, x, y, s;
	bool c, z, i, h, b, p, v, n;
	bool wait, stop;

	// $2140-$2143 as the CPU wrote them, and $f4-$f7 as the SMP wrote them.
	u8 fromCpu[4];
	u8 toCpu[4];
	bool timersDisable;
	bool ramWritable;
	bool ramDisable;
	bool timersEnable;
	u8 externalWaitStates;
	u8 internalWaitStates;
	bool iplromEnable;
	u8 dspAddress;
	u8 aux4, aux5;
	struct SmpTimer timer[3];

	// Where the instruction under way stopped, and what it holds between its cycles.
	u32 state;
	u16 ad;
	u16 ta, tb;
	u8 dr, dw, tc, td;
	u8 pend;
};
extern struct Smp smp;

void smpPower(bool reset);
// Runs the SMP until its clock is past the CPU's.
void smpRun(void);

// --- S-DSP ---

struct DspVoice {
	u8 index;
	s8 volume[2];
	u16 pitch;  // 14 bits
	u8 source;
	u8 adsr0, adsr1, gain, envx;
	bool keyon, keyoff, modulate, noise, echo, end;
	s16 buffer[12];
	u8 bufferOffset;  // 4 bits
	u16 gaussianOffset;
	u16 brrAddress;
	u8 brrOffset;  // 4 bits
	u8 keyonDelay;  // 3 bits
	u8 envelopeMode;  // 2 bits
	u16 envelope;  // 11 bits
	s32 _envelope;
	bool _keylatch, _keyon, _keyoff, _modulate, _noise, _echo, _end, _looped;
};

struct Dsp {
	struct Thread th;
	u8 apuram[0x10000];
	u8 registers[128];
	u8 phase;

	struct {
		u16 counter;  // 15 bits
		bool sample;
	} clock;
	struct {
		bool reset, mute;
		s8 volume[2];
		s32 output[2];  // 17 bits
	} mainvol;
	struct {
		s8 feedback;
		s8 volume[2];
		s8 fir[8];
		s16 history[2][8];
		u8 page;
		u8 delay;  // 4 bits
		bool readonly;
		s32 input[2];  // 17 bits
		s32 output[2];  // 17 bits
		u8 _page;
		bool _readonly;
		u16 _address;
		u16 _offset;
		u16 _length;
		u8 _historyOffset;  // 3 bits
	} echo;
	struct {
		u8 frequency;  // 5 bits
		u16 lfsr;  // 15 bits
	} noise;
	struct {
		u8 bank, _bank, _source;
		u16 _address, _nextAddress;
		u8 _header, _byte;
	} brr;
	struct {
		u8 adsr0, envx, outx;
		u16 pitch;  // 15 bits
		s16 output;
	} latch;
	struct DspVoice voice[8];
};
extern struct Dsp dsp;

void dspPower(bool reset);
// Runs the DSP until its clock is past the SMP's.
void dspRun(void);
u8 dspRead(u8 address);
void dspWrite(u8 address, u8 data);

// The frontend's: one stereo sample at the DSP's rate (the APU crystal / 768).
void audioSample(s16 left, s16 right);

// --- The H/V counters. The CPU and the PPU each keep their own, so they can run out of step. ---

struct Counter {
	bool interlace;
	bool field;
	u32 vperiod;
	u32 hperiod;
	u32 vcounter;
	u32 hcounter;
	u32 lastVperiod;
	u32 lastHperiod;
};

// --- S-CPU (65C816, DMA, timers, joypad polling) ---

// Little-endian hosts only: the bytes of a register are laid over it.
union Reg16 {
	u16 w;
	struct {
		u8 l, h;
	};
};
union Reg24 {
	u32 d;
	u16 w;
	struct {
		u8 l, h, b, unused;
	};
};

struct W65816 {
	union Reg24 pc;
	union Reg16 a, x, y, z, s, d;
	u8 b;
	bool c, zf, i, df, xf, m, vf, n;
	bool e;
	bool irq;
	bool wai;
	bool stp;
	u16 vector;
	u32 mar;
	u8 mdr;
	union Reg24 u, v, w;
	// A resumable 65816's (the SA-1's): the address of the access under way and its data.
	u32 ad;
	u8 dr, dw;
	u8 op;
};

struct DmaChannel {
	bool dmaEnable;
	bool hdmaEnable;
	u8 transferMode;  // 3 bits
	bool fixedTransfer;
	bool reverseTransfer;
	bool unused;
	bool indirect;
	bool direction;
	u8 targetAddress;
	u16 sourceAddress;
	u8 sourceBank;
	u16 transferSize;  // also the indirect address
	u8 indirectBank;
	u16 hdmaAddress;
	u8 lineCounter;
	u8 unknown;
	bool hdmaCompleted;
	bool hdmaDoTransfer;
};

struct Cpu {
	struct Thread th;
	struct W65816 r;
	struct Counter counter;
	u8 wram[0x20000];

	u32 counterCpu;
	u32 counterDma;

	struct {
		u32 clockCount;
		bool irqLock;
		u32 dramRefreshPosition;
		u32 dramRefresh;  // 0 = not refreshed; 1 = refresh active; 2 = refresh inactive
		u32 hdmaSetupPosition;
		bool hdmaSetupTriggered;
		u32 hdmaPosition;
		bool hdmaTriggered;
		bool nmiValid, nmiLine, nmiTransition, nmiPending, nmiHold;
		bool irqValid, irqLine, irqTransition, irqPending, irqHold;
		bool resetPending;
		bool interruptPending;
		bool dmaActive;
		bool dmaPending;
		bool hdmaPending;
		bool hdmaMode;  // 0 = init, 1 = run
		u32 autoJoypadCounter;
		u8 autoJoypadPort1;
		u8 autoJoypadPort2;
		bool cpuLatch;
		bool autoJoypadLatch;
	} status;

	struct {
		u32 wramAddress;  // 17 bits
		bool hirqEnable, virqEnable, irqEnable, nmiEnable, autoJoypadPoll;
		u8 pio;
		u8 wrmpya, wrmpyb;
		u16 wrdiva;
		u8 wrdivb;
		u16 htime;  // 12 bits
		u16 vtime;  // 9 bits
		u32 romSpeed;
		u8 version;
		u16 rddiv, rdmpy;
		u16 joy1, joy2, joy3, joy4;
	} io;

	struct {
		u32 mpyctr;
		u32 divctr;
		u32 shift;
	} alu;

	struct DmaChannel channels[8];
};
extern struct Cpu cpu;

void cpuPower(bool reset);
void cpuMap(void);
// One instruction, an interrupt's entry, or one cycle of a waiting or stopped CPU.
void cpuMain(void);

void counterReset(struct Counter* counter);
static inline u32 counterVcounterAt(const struct Counter* counter, u32 offset) {
	if (offset <= counter->hcounter) {
		return counter->vcounter;
	}
	if (counter->vcounter > 0) {
		return counter->vcounter - 1;
	}
	return counter->lastVperiod - 1;
}
static inline u32 counterHcounterAt(const struct Counter* counter, u32 offset) {
	if (offset <= counter->hcounter) {
		return counter->hcounter - offset;
	}
	return counter->hcounter + counter->lastHperiod - offset;
}
// One PPU dot is four clocks, but dots 323 and 327 are six, except on the short line.
static inline u32 counterHdot(const struct Counter* counter) {
	if (counter->hperiod == 1360) {
		return counter->hcounter >> 2;
	}
	return (counter->hcounter - ((counter->hcounter > 1292) << 1) - ((counter->hcounter > 1310) << 1)) >> 2;
}
// Moves on to the next line: its number and length. Whoever owns the counter then does its own work.
void counterTickScanline(struct Counter* counter);

// --- The bus ---

typedef u8 (*BusReader)(u32 address, u8 data);
typedef void (*BusWriter)(u32 address, u8 data);

void busReset(void);
// Maps addresses like "00-3f,80-bf:8000-ffff" to a reader and a writer. They are given the offset
// ares's map() works out from size, base and mask.
void busMap(BusReader reader, BusWriter writer, const char* address, u32 size, u32 base, u32 mask);
u8 busRead(u32 address, u8 data);
void busWrite(u32 address, u8 data);
u32 busMirror(u32 address, u32 size);
u32 busReduce(u32 address, u32 mask);

// --- The cartridge ---

struct Cart {
	// The game, which the frontend keeps.
	const u8* rom;
	u32 romSize;
	u8* ram;
	u32 ramSize;
	bool pal;
	char board[32];
	// What a game couldn't be loaded without: a part of its board this core lacks, or a firmware file.
	char missing[64];
	bool necdsp;
	bool sa1;
	// What the game saves to: the save RAM, or a chip's own memory. Little-endian hosts only.
	u8* saveRam;
	u32 saveRamSize;
};
extern struct Cart cart;

// Works out the board from the game's header. False if it is no game, if its board needs what this
// core hasn't got (cart.missing says what), or if its firmware isn't to be had.
bool cartLoad(const u8* rom, u32 size);
void cartPower(bool reset);
// Brings the cartridge's processors up to the CPU's clock.
void cartSync(void);
// The frontend's: fills data with size bytes of the firmware file of that name. False if it has none.
bool cartFirmware(const char* name, u8* data, size_t size);

// --- Controllers ---

enum {
	PAD_B, PAD_Y, PAD_SELECT, PAD_START, PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_A, PAD_X, PAD_L, PAD_R,
};

struct Pad {
	bool latched;
	u8 counter;
	u16 buttons;
	bool yHold, upLatch, downLatch;
	bool xHold, leftLatch, rightLatch;
};
struct Input {
	struct Pad pad[2];
};
extern struct Input input;

void inputPower(void);
u8 controllerData(int port);
void controllerLatch(int port, bool data);
// The frontend's: the buttons held on this port now, one bit each by the numbers above.
u16 inputPoll(int port);

void busFinish(void);
void cartMap(void);
void systemFrameEvent(void);

// --- S-PPU, drawn dot by dot ---

enum { BG_BPP2, BG_BPP4, BG_BPP8, BG_MODE7, BG_INACTIVE };
enum { CANVAS_WIDTH = 564, CANVAS_HEIGHT = 576 };

struct BgPixel {
	u8 priority;  // 0 = transparent
	u8 palette;
	u8 paletteGroup;
};

struct BgTile {
	u16 address;
	u16 character;  // 10 bits
	u8 palette;
	u8 paletteGroup;
	u8 priority;
	bool hmirror;
	bool vmirror;
	u16 data[4];
};

struct Background {
	u8 id;
	struct {
		u8 screenSize;
		u16 screenAddress;
		u16 tiledataAddress;
		bool tileSize;
		u8 mode;
		u8 priority[2];
		bool aboveEnable;
		bool belowEnable;
		u16 hoffset;
		u16 voffset;
	} io;
	struct {
		struct BgPixel above;
		struct BgPixel below;
	} output;
	struct {
		bool enable;
		u16 hcounter;
		u16 hoffset;
		struct BgPixel pixel;
	} mosaic;
	// Offset-per-tile: set in BG3, used by BG1 and BG2.
	struct {
		u16 hoffset;
		u16 voffset;
	} opt;
	struct BgTile tiles[66];
	u8 renderingIndex;  // 7 bits
	u8 pixelCounter;  // 3 bits
};

struct OamObject {
	u16 x;  // 9 bits
	u8 y;
	u8 character;
	bool nameselect;
	bool vflip;
	bool hflip;
	u8 priority;
	u8 palette;
	bool size;
};

struct WindowLayer {
	bool oneInvert, oneEnable, twoInvert, twoEnable;
	u8 mask;
	bool aboveEnable, belowEnable;
};

struct Ppu {
	struct Thread th;
	struct Counter counter;
	struct {
		bool interlace;
		bool overscan;
		u16 vdisp;
	} state;

	u16 vram[0x10000];
	u16 vramMask;
	struct OamObject oam[128];
	u16 cgram[256];

	struct {
		u8 version;
		u8 mdr;
	} ppu1, ppu2;

	struct {
		u16 vram;
		u8 oam;
		u8 cgram;
		u8 bgofsPPU1;
		u8 bgofsPPU2;  // 3 bits
		u8 mode7;
		bool counters;
		bool hcounter;
		bool vcounter;
		u16 oamAddress;  // 10 bits
		u8 cgramAddress;
	} latch;

	struct {
		u8 displayBrightness;
		bool displayDisable;
		u16 oamBaseAddress;  // 10 bits
		u16 oamAddress;  // 10 bits
		bool oamPriority;
		u8 bgMode;
		bool bgPriority;
		u16 hoffsetMode7;
		u16 voffsetMode7;
		u8 vramIncrementSize;
		u8 vramMapping;
		bool vramIncrementMode;
		u16 vramAddress;
		bool hflipMode7;
		bool vflipMode7;
		u8 repeatMode7;
		u16 m7a, m7b, m7c, m7d, m7x, m7y;
		u8 cgramAddress;
		bool cgramAddressLatch;
		bool interlace;
		bool overscan;
		bool pseudoHires;
		bool extbg;
		u16 hcounter;
		u16 vcounter;
	} io;

	struct {
		u8 size;  // 5 bits
		u8 vcounter;  // 5 bits
	} mosaic;

	struct Background bg[4];

	struct {
		struct {
			bool aboveEnable;
			bool belowEnable;
			bool interlace;
			u16 tiledataAddress;
			u8 nameselect;
			u8 baseSize;
			u8 firstSprite;  // 7 bits
			u8 priority[4];
			bool rangeOver;
			bool timeOver;
		} io;
		u8 latchFirstSprite;
		u32 x;
		u32 y;
		u32 itemCount;
		u32 tileCount;
		bool active;
		struct {
			bool valid;
			u8 index;
		} item[2][32];
		struct {
			bool valid;
			u16 x;  // 9 bits
			u8 priority;
			u8 palette;
			bool hflip;
			u32 data;
		} tile[2][34];
		struct {
			u8 priority;
			u8 palette;
		} above, below;
	} obj;

	struct {
		struct WindowLayer bg[4];
		struct WindowLayer obj;
		struct {
			bool oneEnable, oneInvert, twoEnable, twoInvert;
			u8 mask;
			u8 aboveMask, belowMask;
		} col;
		u8 oneLeft, oneRight, twoLeft, twoRight;
		bool aboveColorEnable;
		bool belowColorEnable;
		u32 x;
		bool oneActive, oneStopped, twoActive, twoStopped;
	} window;

	struct {
		bool directColor;
		bool blendMode;
		bool bgColorEnable[4];
		bool objColorEnable;
		bool backColorEnable;
		bool colorHalve;
		bool colorMode;
		u8 colorRed, colorGreen, colorBlue;
		struct {
			u16 aboveColor;
			bool aboveColorEnable;
			u16 belowColor;
			bool belowColorEnable;
			bool transparent;
			bool blendMode;
			bool colorHalve;
		} math;
		// Where on the canvas the line being drawn is, if it shows.
		bool lineValid;
		u32 line;
	} dac;
	// How much of the canvas this frame has drawn: a DMA can take the PPU into the next frame before
	// the CPU's instruction ends, so a state can be taken with some of it drawn.
	u32 drawnTo;

	// Where the PPU stopped, and what it was in the middle of.
	u32 resume;
	u32 cycle;
	u32 wait;
	struct {
		s32 i;
		u32 tx;
		u32 n;
		u32 tileWidth;
		s32 x;
		s32 y;
		u16 tiledataAddress;
		u16 chrx;
		u16 chry;
		u16 address;
		u8 sprite;
	} fetch;
};
extern struct Ppu ppu;
// The frame being drawn, CANVAS_WIDTH x CANVAS_HEIGHT: each pixel is four bits of brightness over a
// 15-bit color. A line is two rows apart when interlaced, and every pixel is two wide.
extern u32 ppuCanvas[CANVAS_WIDTH * CANVAS_HEIGHT];

void ppuPower(bool reset);
void ppuMap(void);
// Runs the PPU until its clock is past the CPU's.
void ppuRun(void);
void ppuLatchCounters(void);
// The frontend's: the canvas of the frame just finished.
void videoFrame(const u32* canvas);

// --- Saved states ---

size_t stateSize(void);
// Taken between frames, after systemRunFrame. False if size is too small.
bool stateSave(void* data, size_t size);
// False if the state is another build's, another game's, or cut short.
bool stateLoad(const void* data, size_t size);

// --- NEC uPD7725 / uPD96050 (DSP-1 to DSP-4, ST010, ST011) ---

struct NecDspFlag {
	bool ov0, ov1, z, c, s0, s1;
};
struct NecDspStatus {
	bool p0, p1, ei, sic, soc, drc, dma, drs, usf0, usf1, rqm;
};
struct NecDsp {
	struct Thread th;
	u32 frequency;
	bool uPD96050;
	u16 stack[16];
	u16 pc, rp, dp;
	u8 sp;
	u16 si, so;
	s16 k, l, m, n, a, b;
	u16 tr, trb, dr;
	struct NecDspStatus sr;
	bool siack, soack;
	struct NecDspFlag flagA, flagB;
	u16 dataRAM[2048];
};
// The chip's program and data ROMs: the firmware, not part of a saved state.
struct NecDspFirmware {
	u32 programROM[16384];
	u16 dataROM[2048];
};
extern struct NecDsp necdsp;
extern struct NecDspFirmware necdspFirmware;

void necdspPower(void);
void necdspRun(void);
u8 necdspRead(u32 address, u8 data);
void necdspWrite(u32 address, u8 data);
u8 necdspReadRAM(u32 address, u8 data);
void necdspWriteRAM(u32 address, u8 data);

// --- SA-1 ---

struct Sa1 {
	struct Thread th;
	struct W65816 r;

	// Where it stopped, and the calls into its reads, writes and idle cycles it was in.
	u32 state;
	u32 stack[4];
	u8 depth;

	struct {
		bool interruptPending;
		u16 scanlines;
		u16 vcounter;
		u16 hcounter;
	} status;
	u32 dmaLine;
	bool bwramDma;
	// A normal DMA one of its writes started, and what it has between steps.
	bool dmaStart;
	u8 dmaData;
	u32 dmaSource;
	u32 dmaTarget;

	struct {
		bool sa1_irq, sa1_rdyb, sa1_resb, sa1_nmi;
		u8 smeg;
		bool cpu_irqen, chdma_irqen;
		bool cpu_irqcl, chdma_irqcl;
		u16 crv, cnv, civ;
		bool cpu_irq, cpu_ivsw, cpu_nvsw;
		u8 cmeg;
		bool sa1_irqen, timer_irqen, dma_irqen, sa1_nmien;
		bool sa1_irqcl, timer_irqcl, dma_irqcl, sa1_nmicl;
		u16 snv, siv;
		bool hvselb, ven, hen;
		u16 hcnt, vcnt;
		bool cbmode, dbmode, ebmode, fbmode;
		u32 cb, db, eb, fb;
		u8 sbm;
		bool sw46;
		u8 cbm;
		bool swen, cwen;
		u8 bwp, siwp, ciwp;
		bool dmaen, dprio, cden, cdsel, dd;
		u8 sd;
		bool chdend;
		u8 dmasize, dmacb;
		u32 dsa, dda;  // 24 bits
		u16 dtc;
		bool bbf;
		u8 brf[16];
		bool acm, md;
		u16 ma, mb;
		bool hl;
		u8 vb;
		u32 va;  // 24 bits
		u8 vbit;
		bool cpu_irqfl, chdma_irqfl;
		bool sa1_irqfl, timer_irqfl, dma_irqfl, sa1_nmifl;
		u16 hcr, vcr;
		u64 mr;
		bool overflow;
	} io;

	u8 iram[0x800];
};
extern struct Sa1 sa1;

void sa1Power(void);
void sa1Run(void);
u8 sa1ReadIOCPU(u32 address, u8 data);
void sa1WriteIOCPU(u32 address, u8 data);
u8 sa1ReadRomCPU(u32 address, u8 data);
void sa1WriteRomCPU(u32 address, u8 data);
u8 sa1ReadBwramCPU(u32 address, u8 data);
void sa1WriteBwramCPU(u32 address, u8 data);
u8 sa1ReadIramCPU(u32 address, u8 data);
void sa1WriteIramCPU(u32 address, u8 data);

#endif
