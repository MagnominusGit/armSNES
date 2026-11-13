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
// RAM and registers start at 0 at power on, as on ares's no-entropy setting, not ares's usual
// stripes; some unlicensed games and betas read memory they never wrote.
extern bool systemClearPowerOn;

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
u32 busMirrorOver(u32 address, u32 size);
// An address in a memory of size bytes; one past its end is mirrored down as the hardware's
// address lines would have it.
static inline u32 busMirror(u32 address, u32 size) {
	return address < size ? address : busMirrorOver(address, size);
}
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
	bool gsu;
	bool sdd1;
	bool obc1;
	bool cx4;
	bool spc7110;
	bool epsonRtc;
	bool sharpRtc;
	bool st018;
	bool mcc;
	// A memory-pack slot, on the BS-X base cartridge or a game's board.
	bool bsSlot;
	// The Sufami Turbo's two cartridge slots.
	bool sufami;
	// The Super Game Boy's ICD2 and its Game Boy slot.
	bool icd;
	bool msu1;
	// An event cartridge's uPD78214.
	bool competition;
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
struct Mouse {
	bool latched;
	u8 counter;
	// 0 slow, 1 normal, 2 fast.
	u8 speed;
	bool left, right;
	// How far it moved, and which way: 1 is left, or up.
	u8 cx, cy;
	bool dx, dy;
};
// The Super Scope, or one or two Justifiers.
struct Gun {
	bool latched;
	u8 counter;
	// Where each gun points, by the CPU's counters; below 0 is off the screen.
	s32 cx[2], cy[2];
	u8 buttons[2];
	bool triggerValue, turboEdge, pauseEdge, cursor;
	bool offscreen, turboOld, triggerLock, pauseLock;
	// The Justifier whose turn it is.
	bool active;
};
struct Input {
	// The first port's pad, then the second's, or its Super Multitap's four.
	struct Pad pad[5];
	struct Mouse mouse[2];
	struct Gun gun;
	bool tapLatched;
};
extern struct Input input;

enum { DEVICE_NONE, DEVICE_PAD, DEVICE_MOUSE, DEVICE_MULTITAP, DEVICE_SUPER_SCOPE, DEVICE_JUSTIFIER, DEVICE_JUSTIFIERS };
// A gun's buttons; the Justifier's Start is its pause.
enum { GUN_TRIGGER, GUN_CURSOR, GUN_TURBO, GUN_PAUSE };
extern u8 inputDevice[2];
// A gun is in the second port.
extern bool inputGuns;

void inputPower(void);
// What is plugged into a port. The first takes a pad or a mouse.
void inputSetDevice(int port, int device);
u8 controllerData(int port);
void controllerLatch(int port, bool data);
void inputBeam(u32 vcounter, u32 hcounter);
// The frontend's: the buttons held on a pad now, one bit each by the numbers above. Pad 0 is the
// first port's and 1 the second's; a Super Multitap's are 1 to 4.
u16 inputPoll(int pad);
// How far a port's mouse moved since it was last asked, and its buttons.
void inputPollMouse(int port, int* x, int* y, bool* left, bool* right);
// Where a gun points in the picture, 256 wide, or below 0 when off it, and its buttons.
void inputPollGun(int gun, int* x, int* y, u8* buttons);

void busFinish(void);
void cartMap(void);
void systemFrameEvent(void);

// --- S-PPU, drawn dot by dot ---

enum { BG_BPP2, BG_BPP4, BG_BPP8, BG_MODE7, BG_INACTIVE };
enum { BLANK_NONE = 0xffff };
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
	// Where on this line the backgrounds stopped being fetched because the display was off, or
	// BLANK_NONE: if it comes back on before the line ends, they are brought up to it from there.
	u16 blankFrom;

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

// --- Super FX (GSU) ---

struct GsuFrame {
	u32 ret;
	u32 a, b, c, d, e;
};
struct GsuPixelCache {
	u16 offset;
	u8 bitpend;
	u8 data[8];
};
struct Gsu {
	struct Thread th;

	// Where it stopped, and the subroutines it was in: each frame has its return state, its
	// arguments and its locals.
	u32 state;
	struct GsuFrame stack[16];
	u8 depth;
	u32 result;
	u8 opcode;
	// What PIPE gives back, and a low byte an instruction keeps while it reads the high one.
	u8 pipeOut;
	u8 lo;

	u16 r[16];
	bool modified[16];
	u16 sfr;
	u8 pipeline;
	u16 ramaddr;
	u8 pbr, rombr;
	bool rambr;
	u16 cbr;
	u8 scbr;
	struct {
		u32 ht;
		bool ron, ran;
		u32 md;
	} scmr;
	u8 colr;
	struct {
		bool obj, freezehigh, highnibble, dither, transparent;
	} por;
	bool bramr;
	u8 vcr;
	struct {
		bool irq, ms0;
	} cfgr;
	bool clsr;
	u32 romcl;
	u8 romdr;
	u32 ramcl;
	u16 ramar;
	u8 ramdr;
	u32 sreg, dreg;

	u8 cache[512];
	bool cacheValid[32];
	struct GsuPixelCache pixelcache[2];
};
extern struct Gsu gsu;

void gsuPower(void);
void gsuRun(void);
u8 gsuReadIO(u32 address, u8 data);
void gsuWriteIO(u32 address, u8 data);
u8 gsuReadRomCPU(u32 address, u8 data);
void gsuWriteRomCPU(u32 address, u8 data);
u8 gsuReadRamCPU(u32 address, u8 data);
void gsuWriteRamCPU(u32 address, u8 data);

// --- S-DD1 ---

struct Sdd1 {
	u8 r4800, r4801, r4804, r4805, r4806, r4807;
	struct {
		u32 address;  // 24 bits
		u16 size;
	} dma[8];
	bool dmaReady;
	// The decompressor
	u32 imOffset, imBitCount;
	struct {
		u8 mpsCount;
		bool lpsIndex;
	} bg[8];
	struct {
		u8 status, mps;
	} contextInfo[32];
	u8 cmBitplanesInfo, cmContextBitsInfo, cmBitNumber, cmCurrentBitplane;
	u16 cmPreviousBitplaneBits[8];
	u8 olBitplanesInfo, olR0, olR1, olR2;
};
extern struct Sdd1 sdd1;

void sdd1Power(void);
void sdd1Map(void);
u8 sdd1ReadIO(u32 address, u8 data);
void sdd1WriteIO(u32 address, u8 data);
u8 sdd1ReadMCU(u32 address, u8 data);
void sdd1WriteMCU(u32 address, u8 data);

// --- OBC1 ---

struct Obc1 {
	u16 baseptr;
	u8 address;
	u8 shift;
};
extern struct Obc1 obc1;

void obc1Power(void);
u8 obc1Read(u32 address, u8 data);
void obc1Write(u32 address, u8 data);

// --- Cx4 (Hitachi HG51B) ---

struct Cx4Frame {
	u32 ret;
	u32 a, b, c, d, e;
};
struct Cx4 {
	struct Thread th;

	// Where it stopped, and the subroutines it was in, as the Super FX keeps them.
	u32 state;
	struct Cx4Frame stack[4];
	u8 depth;
	bool result;
	u16 opcode;

	struct {
		u16 pb;
		u8 pc;
		bool n, z, c, v, i;
		u32 a;
		u16 p;
		u64 mul;
		u32 mdr, rom, ram, mar, dpr;
		u32 gpr[16];
	} r;
	struct {
		bool lock, halt, irq, rom;
		u8 vector[32];
		u8 waitRom, waitRam;
		bool suspendEnable;
		u8 suspendDuration;
		struct {
			bool enable, page, preload, lock[2];
			u32 address[2];
			u32 base;
			u16 pb;
			u8 pc;
		} cache;
		struct {
			bool enable;
			u32 source, target;
			u16 length;
		} dma;
		struct {
			bool enable, reading, writing;
			u8 pending;
			u32 address;
		} bus;
	} io;
	u32 callStack[8];
	u16 programRAM[2][256];
	u8 dataRAM[3072];
};
extern struct Cx4 cx4;

void cx4Load(const u8* firmware);
void cx4Power(void);
void cx4Run(void);
u8 cx4ReadIO(u32 address, u8 data);
void cx4WriteIO(u32 address, u8 data);
u8 cx4ReadRomCPU(u32 address, u8 data);
void cx4WriteRomCPU(u32 address, u8 data);
u8 cx4ReadRamCPU(u32 address, u8 data);
void cx4WriteRamCPU(u32 address, u8 data);
u8 cx4ReadDataRamCPU(u32 address, u8 data);
void cx4WriteDataRamCPU(u32 address, u8 data);

// --- SPC7110 ---

struct Spc7110Decompressor {
	struct {
		u8 prediction;
		u8 swap;
	} context[5][15];
	u32 bpp;
	u32 offset;
	u32 bits;
	u16 range;
	u16 input;
	u8 output;
	u64 pixels;
	u64 colormap;
	u32 result;
};
struct Spc7110 {
	struct Thread th;
	// Where it stopped: 0 at the top of its loop, or in the wait before a decompression, a
	// multiplication or a division.
	u32 state;

	// The decompression unit.
	u8 r4801, r4802, r4803, r4804, r4805, r4806, r4807, r4809, r480a, r480b, r480c;
	bool dcuPending;
	u8 dcuMode;
	u32 dcuAddress;
	u32 dcuOffset;
	u8 dcuTile[32];
	struct Spc7110Decompressor dcu;

	// The data port.
	u8 r4810, r4811, r4812, r4813, r4814, r4815, r4816, r4817, r4818, r481a;

	// The ALU.
	u8 r4820, r4821, r4822, r4823, r4824, r4825, r4826, r4827;
	u8 r4828, r4829, r482a, r482b, r482c, r482d, r482e, r482f;
	bool mulPending, divPending;

	// The memory controller.
	u8 r4830, r4831, r4832, r4833, r4834;
};
extern struct Spc7110 spc7110;

void spc7110Load(void);
void spc7110Power(void);
void spc7110Run(void);
u8 spc7110Read(u32 address, u8 data);
void spc7110Write(u32 address, u8 data);
u8 spc7110ReadMCU(u32 address, u8 data);
void spc7110WriteMCU(u32 address, u8 data);
u8 spc7110ReadRam(u32 address, u8 data);
void spc7110WriteRam(u32 address, u8 data);

// --- Real-time clocks: the Epson RTC-4513 and the Sharp S-RTC ---

struct EpsonRtc {
	struct Thread th;
	u32 clocks;
	u32 seconds;
	u8 chipselect;
	u8 state;
	u8 mdr;
	u8 offset;
	u32 wait;
	bool ready;
	bool holdtick;

	u8 secondlo, secondhi;
	bool batteryfailure;
	u8 minutelo, minutehi;
	bool resync;
	u8 hourlo, hourhi;
	bool meridian;
	u8 daylo, dayhi;
	bool dayram;
	u8 monthlo, monthhi, monthram;
	u8 yearlo, yearhi;
	u8 weekday;
	bool hold, calendar, irqflag, roundseconds, irqmask, irqduty;
	u8 irqperiod;
	bool pause, stop, atime, test;
};
extern struct EpsonRtc epsonRtc;

struct SharpRtc {
	struct Thread th;
	u8 state;
	s32 index;
	u32 second, minute, hour, day, month, year, weekday;
};
extern struct SharpRtc sharpRtc;

void epsonRtcLoad(void);
void epsonRtcPower(void);
void epsonRtcRun(void);
u8 epsonRtcRead(u32 address, u8 data);
void epsonRtcWrite(u32 address, u8 data);
void sharpRtcLoad(void);
void sharpRtcPower(void);
void sharpRtcRun(void);
u8 sharpRtcRead(u32 address, u8 data);
void sharpRtcWrite(u32 address, u8 data);

// A clock's time as the frontend keeps it while the game is off: 8 bytes of the clock, then the host's
// time when they were saved (seconds since 1970). Loading moves the clock on by the time since then.
bool rtcPresent(void);
void rtcSave(u8 data[16], s64 now);
void rtcLoad(const u8 data[16], s64 now);

// --- ST018 (ARM6) ---

struct ArmPsr {
	u8 m;
	bool t, f, i, v, c, z, n;
	bool readonly;
};
struct ArmInstruction {
	u32 address;
	u32 instruction;
	bool irq;
	bool thumb;
};
struct St018 {
	struct Thread th;
	// Where it stopped, and what an instruction keeps across a stop.
	u32 state;
	u32 mode, address, word, value, offset, access, rn, rnEnd, idles;
	u16 list;
	u8 index;
	bool usr;
	u8 cpsrMode;

	// The registers: r8-r14 of the user modes, and each other mode's own.
	u32 r[16];
	u32 fiq[7];
	u32 irqBank[2], svcBank[2], abtBank[2], undBank[2];
	u32 rNull;
	struct ArmPsr cpsr, spsrNull, spsrFiq, spsrIrq, spsrSvc, spsrAbt, spsrUnd;
	struct {
		bool reload;
		struct ArmInstruction fetch, decode, execute;
	} pipeline;
	u32 opcode;
	bool carry, irq, nonsequential;

	// The byte ports each way, the timer, and the reset line.
	struct {
		bool cputoarmReady, armtocpuReady;
		u8 cputoarmData, armtocpuData;
		u32 timer, timerlatch;
		bool reset, ready, signal;
	} bridge;
	u8 programRAM[16384];
};
extern struct St018 st018;

void st018Load(const u8* firmware);
void st018Power(void);
void st018Run(void);
u8 st018Read(u32 address, u8 data);
void st018Write(u32 address, u8 data);

// --- BS-X: the base cartridge's MCC and the memory pack ---

struct MccRegisters {
	bool mapping, psramEnableLo, psramEnableHi;
	u8 psramMapping;
	bool romEnableLo, romEnableHi, exEnableLo, exEnableHi, exMapping;
	bool internallyWritable, externallyWritable;
};
struct Mcc {
	bool irqFlag, irqEnable;
	// The registers in use, and the ones written, which a commit makes the ones in use.
	struct MccRegisters r, w;
};
extern struct Mcc mcc;

struct BsBlock {
	u32 erased;
	bool locked, erasing;
	bool statusVppLow, statusQueueFull, statusAborted, statusFailed, statusLocked, statusReady;
};
struct BsMemory {
	struct Thread th;
	u32 state;
	u32 erasingBlock;
	bool rom, writable;
	u16 vendor, device;
	u64 serial;
	u8 page[2][256];
	struct BsBlock blocks[64];
	bool compatibleVppLow, compatibleWriteFailed, compatibleEraseFailed, compatibleEraseSuspended, compatibleReady;
	bool globalPage, globalPageReady, globalPageAvailable, globalQueueFull, globalSleeping, globalFailed;
	bool globalSuspended, globalReady;
	u8 mode, readyBusyMode;
	struct {
		bool valid;
		u32 address;
		u8 data;
	} queue[4];
};
extern struct BsMemory bsmemory;

void mccLoad(void);
void mccPower(void);
u8 mccRead(u32 address, u8 data);
void mccWrite(u32 address, u8 data);
u8 mccReadMCU(u32 address, u8 data);
void mccWriteMCU(u32 address, u8 data);
u8* mccPsram(u32* size);
void bsLoad(const u8* data, size_t size);
void bsPower(void);
void bsRun(void);
// The Satellaview base unit's receiver, there with the BS-X cartridge.
struct Satellaview {
	u8 r2188, r2189, r218a, r218b, r218c, r218e, r218f;
	u8 r2190, r2191, r2193, r2194, r2196, r2197, r2199;
	u8 rtcCounter, rtcHour, rtcMinute, rtcSecond;
};
extern struct Satellaview satellaview;
// The frontend's: the time its clock gives, in seconds since 1970.
extern s64 satellaviewNow;
void satellaviewPower(void);
u8 satellaviewRead(u32 address, u8 data);
void satellaviewWrite(u32 address, u8 data);
bool bsPresent(void);
bool bsRunning(void);
u8 bsRead(u32 address, u8 data);
void bsWrite(u32 address, u8 data);
u8* bsData(u32* size);
// A memory pack put in the slot, after the game is loaded and before it is powered on.
bool cartInsertPack(const u8* data, size_t size);

// --- A cartridge's own sound, added to the DSP's ---

#define MIXER_SAMPLES 1024

struct Mixer {
	s32 sumLeft, sumRight;
	u32 summed, phase;
	s16 samples[MIXER_SAMPLES][2];
	u32 read, write;
};

// Each input sample moves the phase on by step; a DSP sample is due each time it reaches rate.
void mixerInput(struct Mixer* m, s16 left, s16 right, u32 step, u32 rate);
// Adds the next sample, if there is one, to a DSP sample.
void mixerOutput(struct Mixer* m, s32* left, s32* right);

// --- The Super Game Boy's ICD2 ---

// The Game Boy's clocks run at a time.
#define ICD_SLICE 256

struct Icd {
	struct Thread th;
	// The SGB2's own oscillator, or 0 for the CPU's; and that over the control port's divider.
	u32 frequency;
	u32 clockFrequency;
	u8 joypId;
	bool pulseLock, strobeLock, packetLock;
	u8 packetOffset;
	bool packetReady;
	u8 bitData, bitOffset;
	bool previousP15;
	u8 output[4 * 512];
	u8 readBank;
	u16 readAddress;
	u8 writeBank;
	u8 r6003;
	u8 joypads[4];
	u8 r7000[16];
	u8 mltReq;
	u8 hcounter, vcounter;
	struct Mixer sound;
};
extern struct Icd icd;

void icdPower(bool reset);
void icdRun(void);
u8 icdRead(u32 address, u8 data);
void icdWrite(u32 address, u8 data);
// The Game Boy game for the slot (and the Game Boy's boot ROM), after the base is loaded and before
// it is powered on. Its save RAM becomes the save.
bool cartInsertGameBoy(const u8* rom, size_t size, const u8* boot, size_t bootSize);

// --- The event cartridges' uPD78214 ---

enum { COMPETITION_CC92 = 1, COMPETITION_PF94 };

struct Competition {
	struct Thread th;
	u32 timer;
	u8 status, select;
	bool timerActive, scoreActive;
	u32 timerSecondsRemaining, scoreSecondsRemaining;
};
extern struct Competition competition;

// The board and the dump, cut into the program ROM and the three levels'.
void competitionLoad(int board, const u8* data, const u32 sizes[4]);
void competitionPower(void);
void competitionRun(void);
u8 competitionRead(u32 address, u8 data);
void competitionWrite(u32 address, u8 data);
u8 competitionReadMCU(u32 address, u8 data);
void competitionWriteMCU(u32 address, u8 data);

// The hash of data as 64 lowercase hex digits.
void sha256Hex(const u8* data, size_t size, char out[65]);

// --- Pirate cartridges' protection chips and bank switches ---

enum { PIRATE_LATCH = 1, PIRATE_TEKKEN2, PIRATE_20, PIRATE_LATCH_BANK, PIRATE_FIXED };

struct Pirate {
	u8 board;
	u8 latch, prot, baseBank;
};
extern struct Pirate pirate;

// For tests: the chip a cartridge that isn't known gets.
extern int cartTryPirate;
void pirateLoad(int board);
void piratePower(void);
u8 pirateLatchRead(u32 address, u8 data);
void pirateLatchWrite(u32 address, u8 data);
u8 pirateBankRead(u32 address, u8 data);
void pirateBankWrite(u32 address, u8 data);
u8 pirateFixedRead(u32 address, u8 data);
u8 pirateOpenRead(u32 address, u8 data);
void pirateNoWrite(u32 address, u8 data);
u8 pirateTekkenRead(u32 address, u8 data);
void pirateTekkenWrite(u32 address, u8 data);
u8 pirate20Read(u32 address, u8 data);
void pirate20Write(u32 address, u8 data);

// --- The MSU-1 ---

struct Msu1 {
	struct Thread th;
	u32 dataSeekOffset, dataReadOffset;
	u32 audioPlayOffset, audioLoopOffset;
	u16 audioTrack;
	u8 audioVolume;
	u32 audioResumeTrack, audioResumeOffset;
	bool audioError, audioPlay, audioRepeat, audioBusy, dataBusy;
	struct Mixer sound;
};
extern struct Msu1 msu1;

// Whether path.msu is there; its tracks are path-1.pcm and on.
bool msu1Load(const char* path);
void msu1Unload(void);
void msu1Power(void);
void msu1Reopen(void);
void msu1Run(void);
u8 msu1Read(u32 address, u8 data);
void msu1Write(u32 address, u8 data);
// The MSU-1's files, by the game's path without its extension, after the game is loaded and before
// it is powered on. False if there is no data file.
bool cartLoadMsu1(const char* path);

// --- The Sufami Turbo's slots ---

// A cartridge put in slot 0 (A) or 1 (B), after the base is loaded and before it is powered on.
bool sufamiInsert(int slot, const u8* data, size_t size);
u8* sufamiRam(int slot, u32* size);
u8 sufamiReadRomA(u32 address, u8 data);
u8 sufamiReadRomB(u32 address, u8 data);
void sufamiWriteRom(u32 address, u8 data);
u8 sufamiReadRamA(u32 address, u8 data);
u8 sufamiReadRamB(u32 address, u8 data);
void sufamiWriteRamA(u32 address, u8 data);
void sufamiWriteRamB(u32 address, u8 data);
// A cartridge for a Sufami Turbo slot, after the base is loaded. Slot A's save RAM becomes the save.
bool cartInsertSufami(int slot, const u8* data, size_t size);

// The S-CPU's DMA registers, for a chip that watches them.
u8 cpuReadDMA(u32 address, u8 data);
void cpuWriteDMA(u32 address, u8 data);

#endif
