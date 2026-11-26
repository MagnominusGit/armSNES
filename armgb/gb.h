/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Licensed under the GNU General Public License, version 2 or (at your option) any later version: see LICENSE. */
#ifndef ARMGB_GB_H
#define ARMGB_GB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// armGB: the Game Boy and Game Boy Color. Everything the machine is lives in struct GB, so a state
// is the struct itself; pointers into it are set again from offsets when a state loads.
//
// Time is counted in dots, the PPU's 4.194304 MHz clock, which no speed switch changes. A CPU
// M-cycle is 4 dots, or 2 in the Color's double speed. Times are 32-bit and compared by their
// difference, so they may wrap.

enum {
	GB_WIDTH = 160,
	GB_HEIGHT = 144,
	GB_LINE_DOTS = 456,
	GB_LINES = 154,
	GB_FRAME_DOTS = GB_LINE_DOTS * GB_LINES,
	GB_SIZE_VRAM = 0x4000,
	GB_SIZE_WRAM = 0x8000,
	GB_SIZE_OAM = 0xA0,
	GB_SIZE_HRAM = 0x7F,
	GB_SAMPLE_RATE = 32768,
	// What the frame holds where the LCD shows nothing (off, or its first frame on), besides the shades
	// 0 (lightest) to 3.
	GB_SHADE_OFF = 4,
};

// The interrupt flags.
enum {
	GB_IRQ_VBLANK = 1,
	GB_IRQ_STAT = 2,
	GB_IRQ_TIMER = 4,
	GB_IRQ_SERIAL = 8,
	GB_IRQ_JOYPAD = 16,
};

// The buttons, as libretro numbers them.
enum {
	GB_KEY_A = 1,
	GB_KEY_B = 2,
	GB_KEY_SELECT = 4,
	GB_KEY_START = 8,
	GB_KEY_RIGHT = 16,
	GB_KEY_LEFT = 32,
	GB_KEY_UP = 64,
	GB_KEY_DOWN = 128,
};

// The original Game Boy, the Pocket, the Color, and the Color inside a Game Boy Advance.
// Layer Selection's layers.
enum { GB_LAYER_BG = 1, GB_LAYER_OBJ = 2, GB_LAYER_WINDOW = 4 };

enum GBModel {
	GB_MODEL_DMG,
	GB_MODEL_MGB,
	GB_MODEL_CGB,
	GB_MODEL_AGB,
};

enum GBMapper {
	GB_MBC_NONE,
	GB_MBC1,
	GB_MBC2,
	GB_MBC3,
	GB_MBC5,
	GB_HUC1,
	GB_MBC7,
	GB_HUC3,
	GB_CAMERA,
	GB_TAMA5,
	GB_SACHEN,
	GB_PKJD,
	GB_NT_OLD1,
	GB_NT_OLD2,
	GB_NT_NEW,
	GB_WISDOM_TREE,
	GB_MMM01,
	GB_MBC6,
	GB_MEGADUCK,
};

// The Mega Duck's cartridge kinds, as the frontend numbers them: with none named (a .bin), MD1's and
// MD2's registers are both there, as they don't overlap.
enum GBDuckKind {
	GB_DUCK_AUTO,
	GB_DUCK_NONE,
	GB_DUCK_MD0,
	GB_DUCK_MD1,
	GB_DUCK_MD2,
	GB_DUCK_MD20S,
	GB_DUCK_MD25S,
};

// The Pocket Camera (SameBoy's): its sensor's registers, whether they're mapped in place of RAM, and
// a shot in progress until busyUntil; with no camera, the sensor sees noise from seed.
struct GBCamera {
	uint8_t regs[0x36];
	bool mapped;
	uint32_t busyUntil;
	uint32_t seed;
};

// HuC3 (SameBoy's): the mode written at $0000, the clock's minutes and days, its alarm, and the
// command port's index, flags and last nibble read. Its time base is GBRtc's, ticks into the minute.
struct GBHuc3 {
	uint8_t mode;
	uint16_t minutes, days;
	uint16_t alarmMinutes, alarmDays;
	bool alarmOn;
	uint8_t index, flags, nibble;
};

// MBC7 (SameBoy's): the accelerometer's latched readings, and the 93LC56 EEPROM's serial port: its
// pins as last written, the bits being shifted out, the command being shifted in and the data bits it
// still takes.
struct GBMbc7 {
	bool secondary;
	uint16_t x, y;
	bool cs, clk, di, dout;
	uint16_t readBits;
	uint16_t command;
	uint8_t argumentBits;
	bool writeEnabled;
};

// Unlicensed cartridges built on MBC5 that scramble bank numbers or data (told apart, as mGBA does,
// by a second logo at $184 their boot code checks).
enum GBVariant {
	GB_PLAIN,
	GB_SINTAX,
	GB_BBD,
	GB_HITEK,
	GB_GGB81,
	GB_LI_CHENG,
	GB_VAST_FAME,
};

// Vast Fame's protection (armGBA's, after taizou's findings in hhugboy): MBC5 with a lock set up by
// writes to $6000-$7FFF, each turning a running value right a bit and XORing the byte into it, stored
// by address. With them the game arms a read of a ROM address that makes the next one to four ROM reads
// give bytes it supplied, and bank 0 from an address up read from another bank. The running value
// starts at $00 on Vast Fame's own cartridges and $10 on mainland releases of the same games; nothing in
// the ROM says which, so a command or a read that only makes sense the other way decides.
struct GBVastFame {
	bool config;
	uint8_t start;
	uint8_t running;
	uint8_t at6000;
	uint8_t at7000[11];
	// For each register, after how many turns it was last written (index 11: $6000).
	uint8_t turnOf[12];
	uint8_t turns;
	uint8_t left;
	bool decided;
	// A cartridge whose logo GGB-81's games carry too reads its data in GGB-81's orders until its lock
	// is first opened.
	bool opened;
	uint8_t dataMode;
};

// The events, each at a time; the earliest decides when the CPU next stops to run them.
enum {
	GB_EVENT_PPU,
	GB_EVENT_TIMER,
	GB_EVENT_SERIAL,
	GB_EVENT_FRAME,
	GB_EVENT_DMA,
	GB_EVENT_HDMA,
	GB_EVENT_SPEED,
	GB_EVENT_STALL,
	GB_EVENTS,
};

struct GBCpu {
	uint8_t a, f, b, c, d, e, h, l;
	uint16_t sp, pc;
	bool ime;
	// EI takes effect after the instruction that follows it.
	bool imePending;
	bool halted;
	bool justHalted;
	// HALT with IME off and an interrupt already pending: the next byte is read twice.
	bool haltBug;
	bool stopped;
	bool locked;
	// Held at power on until GB_EVENT_STALL (gbStall).
	bool stalled;
	// A speed switch: 1 while the speed is about to change to double, 2 while halted until it is done
	// (an interrupt wakes it sooner).
	uint8_t switching;
};

struct GBTimer {
	// The 16-bit counter DIV is the top of, in CPU T-cycles, as of lastSync; while it's held (a speed
	// switch's STOP), lastSync is ahead of now and the counter waits for it.
	uint16_t counter;
	uint32_t lastSync;
	// A hold not yet seen by the sound, which counts the divider's edges: it ends at holdEnd.
	bool held;
	uint32_t holdEnd;
	uint8_t tima, tma, tac;
	// TIMA overflowed: it reads 0 until reloadAt, when it takes TMA and the interrupt is requested.
	bool reloading;
	uint32_t reloadAt;
	// When it last took TMA: a write to TIMA in that M-cycle is lost, and one to TMA lands in TIMA.
	uint32_t reloadedAt;
};

struct GBPpu {
	// The line the PPU is on (LY is the register, which changes a few dots into a line), when the line
	// started, and the step it takes next, at nextAt.
	uint8_t line;
	uint8_t ly;
	uint8_t step;
	uint32_t lineStart;
	uint32_t lineEnd;
	uint32_t drawStart;
	uint32_t nextAt;
	uint16_t mode3Length;
	// The objects on the line, found at mode 3's start, in OAM order: at most ten.
	uint8_t objectCount;
	uint8_t objects[10];
	uint8_t lcdc, stat, scy, scx, lyc, bgp, obp0, obp1, wy, wx, dma;
	// The mode STAT shows, and the one its interrupt line goes by (-1: none), which differ for a dot
	// at some edges.
	uint8_t mode;
	int8_t modeForInterrupt;
	// The LY that LYC is compared with: -1 while LY changes.
	int16_t lyCompare;
	bool coincidence;
	bool lycLine;
	// The STAT interrupt line: a request goes out when it rises.
	bool statLine;
	// The window's own line counter, and whether LY has matched WY this frame.
	uint8_t windowLine;
	bool windowSeen;
	// The line being shown just after the LCD is switched on, which has no OAM scan.
	bool firstLine;
	// Started by a Color at single speed: half a dot late, which reads as a whole one until double speed.
	bool halfDotLate;
	// The frame the LCD was switched on in, which the screen doesn't show.
	bool hiddenFrame;
	// When the CPU is locked out of OAM and VRAM, which isn't quite when STAT's mode says.
	bool oamReadBlocked, oamWriteBlocked, vramReadBlocked, vramWriteBlocked;
};

struct GBSerial {
	uint8_t sb, sc;
	uint8_t bits;
	// A transfer on the console's own clock ends as the divider's counter reaches this.
	uint32_t end;
	// On a cable: a transfer on the console's own clock ends with nothing shifted in, for the other
	// end to give its byte (gbLinkStatus); clocked says one has ended.
	bool linked;
	bool clocked;
};

// gbLinkStatus's requests (the frontend's link code asks these); a value below 256 shifts that byte in,
// ending a transfer under way.
enum {
	GB_LINK_CLOCKED = 256, // whether a transfer on the console's own clock has ended
	GB_LINK_ACK = 257,     // clears that
	GB_LINK_OUT = 258,     // the byte being sent (SB)
	GB_LINK_ON = 264,
	GB_LINK_OFF = 265,
	GB_LINK_STOP = 266,    // ends the transfer under way, stopping the console's own clock
	GB_LINK_CONTROL = 267, // SC
	GB_LINK_LEFT = 268,    // dots until the transfer under way on the console's own clock ends, or -1
};

// A sound channel; length, envelope and sweep are clocked by the frame sequencer.
struct GBChannel {
	bool on;
	bool dac;
	bool lengthOn;
	uint16_t length;
	uint16_t frequency;
	// The squares and the wave channel: 2 MHz cycles to the next step, less one.
	uint16_t countdown;
	uint8_t position;
	uint8_t volume;
	// What the channel puts out now, 0 to 15 (held between its steps).
	uint8_t level;
	// The envelope (SameBoy's): its 3-bit countdown, its clock, and whether that clock locked it.
	uint8_t envCountdown;
	bool envClock, envShouldLock, envLocked;
	// The squares (SameBoy's): the output held at 0 after a start until the first step, whether a
	// step was taken since the start, whether one was taken on the last cycle, and the start's delay.
	bool suppressed, didTick, justReloaded;
	uint8_t delay;
};

// The sound, its timing as SameBoy has it: a 2 MHz clock counted from power-on (the time saved as
// epoch), the frame sequencer's divider from the divider's 512 Hz edges.
struct GBApu {
	bool power;
	struct GBChannel square[2];
	struct GBChannel wave;
	struct GBChannel noise;
	uint32_t epoch;
	uint8_t divider;
	uint8_t skipDivEvent;
	bool duringDivWrite;
	// The Color at double speed takes the envelopes' step an M-cycle after the divider's.
	bool envelopePending;
	uint32_t envelopeAt;
	// The sweep (channel 1).
	uint8_t sweepCountdown, sweepCalcCountdown, sweepReloadTimer, restartHold;
	uint16_t sweepAddend, sweepShadow, sweepCompletedAddend;
	bool sweepUnshifted, sweepInstantDone;
	// The wave channel's sample byte, whether it read one on the last cycle, and whether it was started.
	uint8_t waveByte;
	bool waveJustRead, wavePulsed;
	// The noise channel: its LFSR, the counter whose bit steps it, and that counter's countdown.
	uint16_t lfsr;
	bool narrow, lfsrSample, lfsrSteppedNarrow, lfsrBit7;
	uint16_t noiseCounter;
	uint8_t noiseCountdown;
	// The original's start of the noise channel, put off to a 512 kHz edge.
	bool noiseDelayed;
	uint32_t noiseStartAt;
	bool noiseReloaded, noiseDidStep, noiseCounting, noiseBackground, noiseStartedDacOff;
	uint8_t regs[0x30];
	uint32_t lastSync;
	// Sample building: the time the sample being built starts, and its mixed sums so far.
	uint32_t sampleStart;
	int32_t partialLeft, partialRight;
	int32_t partialHeardLeft, partialHeardRight;
};

// OAM DMA (Pan Docs, "OAM DMA Transfer"): written at start, an M-cycle's wait, then a byte an M-cycle.
struct GBDma {
	bool active;
	uint16_t source;
	uint32_t start;
	uint8_t done;
	// Restarted while moving bytes: the old transfer keeps OAM locked while the new one waits.
	bool overlap;
	// Dots a byte takes: an M-cycle of the CPU's, at the CPU Clock.
	uint8_t step;
};

// The Game Boy Color's own hardware (Pan Docs, "CGB Registers").
struct GBCgb {
	// Palette RAM, 8 palettes of 4 colors as little-endian RGB555, and the index registers.
	uint8_t bgPalette[64];
	uint8_t objPalette[64];
	// An original Game Boy game started without a boot ROM: drawn in the frontend's shades.
	bool compatShades;
	uint8_t bcps, ocps;
	// Objects by OAM position (0) or by X (1, as the original Game Boy).
	uint8_t opri;
	uint8_t key0;
	uint8_t undocumented[4];
	uint8_t rp;
	// VRAM DMA: where it reads and writes, the 16-byte blocks left, and whether it waits for H-blanks.
	uint16_t hdmaSource, hdmaDest;
	uint8_t hdmaBlocks;
	bool hdmaHblank;
	// An H-blank DMA block due, moved after the CPU's next opcode fetch (SameBoy).
	bool hdmaDue;
};

// MBC3's clock: seconds, minutes, hours, the day's low 8 bits and DH (day bit 8, halt, day carry), as
// they count and as last latched; the time into the current second, GB_RTC_SECOND to a second; and how
// far it has counted: in the console's time (Game Time), or the device's (Real Time).
enum { GB_RTC_SECOND = 0x400000 };

struct GBRtc {
	uint8_t regs[5];
	uint8_t latched[5];
	uint32_t ticks;
	uint32_t lastSync;
	// Game Time at a Clock Speed: hundredths of a tick left over.
	uint32_t fraction;
	int64_t lastHost;
};

struct GBCart {
	uint8_t mapper;
	bool hasRam, hasBattery, hasRtc;
	bool ramEnabled;
	uint16_t romBank;
	uint8_t ramBank;
	uint8_t mbc1Mode;
	uint8_t mbc1Low;
	uint8_t mbc1Upper;
	// MBC1 multicart (MBC1M): the upper bits bank by 16 banks, not 32.
	bool mbc1Multicart;
	uint32_t romBanks;
	uint32_t ramSize;
	struct GBRtc rtc;
	// An unlicensed variant's state: the bit orders chosen for bank numbers and for data, Sintax's
	// bank as written and its four XOR keys; and what the switchable bank's bytes read as.
	uint8_t variant;
	uint8_t bankOrder, dataOrder;
	uint8_t bankWritten;
	uint8_t xorKeys[4];
	bool romTransformed;
	uint8_t romLut[256];
	struct GBVastFame vf;
	// HuC1: RAM's place taken by the infrared port.
	bool irMode;
	struct GBMbc7 mbc7;
	struct GBHuc3 huc3;
	struct GBCamera camera;
	// MBC3 past 2 MiB of ROM or 32 KiB of RAM (MBC30): 8-bit ROM banks, eight RAM banks.
	bool mbc30;
	// Mappers that switch bank 0 too (bank0Mapped), or the switchable area as two 8 KiB halves (halves),
	// and MBC6's two 4 KiB RAM halves.
	bool bank0Mapped, halves;
	uint16_t bank0;
	uint16_t half[2];
	uint8_t ramHalf[2];
	// mappers.c's: Sachen's bank, base and mask; NT's bank, base, game size, bit order and split;
	// MMM01's lock and base; PKJD's counters.
	uint8_t sachenBank, sachenBase, sachenMask;
	uint8_t ntBank, ntBase, ntCount;
	bool ntSwapped, ntSplit;
	bool mmm01Locked;
	uint16_t mmm01Base;
	uint8_t pkjdCounters[2];
	// The Mega Duck's cartridge kind (GBDuckKind).
	uint8_t duckKind;
};

// A cheat cartridge in front of the game (cheatcart.c): what a state keeps of it, the frontend's
// controls, and its flash chip (or the Game Genie's program) as it is and as it was built.
enum GBCheatKind {
	GB_CHEAT_ACTION_REPLAY,
	GB_CHEAT_XPLODER,
	GB_CHEAT_GAME_GENIE,
};

// Cheat codes typed in (cheats.c): Game Genie's ROM patches and GameShark's writes each frame.
enum { GB_MAX_CHEATS = 64 };

struct GBGenieCode {
	uint16_t address;
	uint8_t value, compare;
	bool compares;
};

struct GBSharkCode {
	uint8_t type, value;
	uint16_t address;
};

struct GBCheatState {
	uint8_t regs[0x20];
	uint8_t shown, boot;
	uint8_t flashStep, flashId, flashLoad;
	uint8_t pageHigh, pageLow;
	// The first Action Replay: the game's first 512 bytes show for its program to look at.
	uint8_t lowPage;
	// The Xploder: the last byte the game wrote to 4000-5FFF, which 0004 reads.
	uint8_t gameBank;
	// The Game Genie: codes chosen in the frontend's options, nine digits (0-15, 0x10 past the end)
	// and a count of 6 or 9 (0 for none) for each of the three slots.
	uint8_t fill[27];
	uint8_t fillCount[3];
	// The Action Replay's 32 KiB, the Xploder's 128 KiB; a state holds as much as the cartridge has.
	uint8_t ram[0x20000];
};

struct GBCheatCart {
	struct GBCheatState s;
	uint8_t kind;
	// The first Action Replay; whether a game is on top, else the cartridge space behind is empty.
	bool first, hasGame;
	bool button, switchOn;
	// Whether what it keeps with the power off was written since the frontend last cleared this.
	bool flashChanged;
	uint8_t* flash;
	uint8_t* factory;
	size_t flashSize;
};

struct GB {
	struct GBCpu cpu;
	uint8_t model;
	bool cgbMode;
	bool doubleSpeed;
	// Dots per M-cycle: 4, or 2 at double speed.
	uint8_t mcycle;
	// The CPU Clock above 100%: the part of a dot carried to the next M-cycle, and the dots the CPU
	// still keeps the console's own pace for after reading STAT or LY.
	uint32_t cpuCarry;
	uint32_t cpuPaced;
	uint32_t now;
	// Dots run since power on, as of now = elapsedAt (gbTime adds the rest).
	uint64_t elapsed;
	uint32_t elapsedAt;
	uint32_t events[GB_EVENTS];
	uint32_t eventsOn;
	uint32_t nextEvent;
	uint8_t ie, ifl;
	uint8_t joypadSelect;
	uint8_t keys;
	struct GBTimer timer;
	struct GBPpu ppu;
	struct GBSerial serial;
	struct GBCart cart;
	struct GBDma dma;
	struct GBApu apu;
	struct GBCgb cgb;
	bool bootMapped;
	uint8_t wramBank;
	uint8_t vramBank;
	uint8_t vram[GB_SIZE_VRAM];
	uint8_t wram[GB_SIZE_WRAM];
	uint8_t oam[GB_SIZE_OAM];
	uint8_t hram[GB_SIZE_HRAM];
	uint8_t io[0x80];
	uint8_t sram[0x20000];
	uint16_t frame[GB_HEIGHT][GB_WIDTH];
	// The frame's sound, left and right interleaved, at GB_SAMPLE_RATE: the frontend takes it.
	int16_t audio[4096];
	// With channels left out (channelsMuted), the same samples as heard; not otherwise.
	int16_t heard[4096];
	uint32_t audioCount;
	// For each sample pair, whether any channel's DAC was on (the console's output capacitor is
	// connected only then).
	uint8_t audioDacs[2048];
	uint32_t frameCount;
	// Set when V-blank starts, so a frame's run can end there.
	bool frameEnded;
	// When the frame under way ends with the LCD off (a frame's time after the last ended); with the
	// LCD on, a frame goes on to V-blank however late.
	uint32_t frameLimit;
	// Set when the CPU must look again before its next instruction: an interrupt requested or enabled,
	// HALT, STOP, EI, RETI, a lock-up or the frame's end.
	bool attention;

	// Not saved: the cartridge and where it is mapped, and the frontend's hooks.
	const uint8_t* rom;
	size_t romSize;
	const uint8_t* boot;
	size_t bootSize;
	// Where each 4 KiB of ROM and work RAM is; readMap is the same, with NULL where reads must go
	// through gbRead (the boot ROM, OAM DMA).
	const uint8_t* memMap[16];
	const uint8_t* readMap[16];
	// The work RAM's pages, which the CPU writes straight to; NULL for the rest.
	uint8_t* writeMap[16];
	// A frontend's picture, when it wants colors drawn there instead of shades in frame: the four
	// shades' colors, then the screen's with the LCD off.
	uint32_t* video;
	uint32_t videoColors[5];
	// With a frontend's picture, the Color's 32768 RGB555 colors as it shows them.
	const uint32_t* videoTable;
	// Serial bytes shifted out with no partner, for the test runner.
	void (*serialOut)(void* user, uint8_t byte);
	void* user;
	// A Super Game Boy's ICD2, set by a frontend that has one: JOYP's select bits as written (write)
	// or read, giving back the four input bits; and each line's shades once it is drawn.
	uint8_t (*sgbJoypad)(void* user, uint8_t select, bool write);
	void (*sgbLine)(void* user, int line, const uint16_t* shades);
	// The CPU Clock, in quarters of the console's own (4), set by the frontend.
	uint8_t cpuQuarters;
	// Called when a transfer on the console's own clock ends on a cable, for the other end's byte.
	void (*linkClocked)(void* user);
	// Layer Selection's hidden layers and Channel Selection's channels left out (bits 0-3), set by the
	// frontend: only what's drawn and heard changes.
	uint8_t layersHidden;
	uint8_t channelsMuted;
	// MBC7's accelerometer, set by the frontend: the offsets from lying flat (0x70 a cartridge on its
	// side, lower to the right and towards the player), and a flick for its next reading.
	int16_t tiltX, tiltY;
	bool flick;
	// The clock's source, set by the frontend: the device's time (in microseconds, given each frame)
	// or the console's, at rtcPercent of its speed.
	bool rtcRealTime;
	uint16_t rtcPercent;
	// Whether the frontend set rtcPercent (0 is stopped); else it is 100.
	bool rtcSpeedSet;
	int64_t hostMicros;
	// A Mega Duck, set by the frontend before loading: 0, or 1 + its cartridge's GBDuckKind; and
	// whether it is the laptop, whose system ROM starts with SP at FFFF.
	uint8_t megaDuck;
	bool duckLaptop;
	// Set by the frontend for a frame it won't show: lines aren't drawn.
	bool noDraw;
	// A Color game's palettes as drawn (through videoTable), built when first needed after palette
	// RAM, a reset, a loaded state or the frontend's colours change them (which clear colorsBuilt).
	bool colorsBuilt;
	uint32_t bgColors[8][4], objColors[8][4];
	// A cheat cartridge in front of the game, set by the frontend (and powered on by gbReset).
	struct GBCheatCart* cheatCart;
	// Cheat codes, set by the frontend after loading (gbAddCheat).
	struct GBGenieCode genie[GB_MAX_CHEATS];
	struct GBSharkCode shark[GB_MAX_CHEATS];
	uint8_t genieCount, sharkCount;
};

// gb.c
bool gbLoad(struct GB*, const uint8_t* rom, size_t size, enum GBModel);
// Whether the console has the Color's hardware (a Color or an Advance).
static inline bool gbColor(const struct GB* gb) {
	return gb->model >= GB_MODEL_CGB;
}
void gbReset(struct GB*);
void gbRunFrame(struct GB*);
// Runs for up to *dots, stopping early where a frame ends; *dots becomes the dots run. Returns whether
// a frame ended. The sound made goes on from audioCount, which the caller empties.
bool gbRunFor(struct GB*, uint32_t* dots);
void gbSchedule(struct GB*, int event, uint32_t at);
void gbUnschedule(struct GB*, int event);
void gbRunEvents(struct GB*);
void gbRequest(struct GB*, uint8_t irq);
void gbStop(struct GB*);
void gbSpeedEvent(struct GB*);
// Holds the CPU for some dots from power on: the Game Boy Advance's own start before the Color's boot
// ROM runs.
void gbStall(struct GB*, uint32_t dots);
void gbBlankFrame(struct GB*);
// Dots run since power on.
static inline uint64_t gbTime(const struct GB* gb) {
	return gb->elapsed + (uint32_t) (gb->now - gb->elapsedAt);
}
size_t gbStateSize(void);
void gbSaveState(const struct GB*, void* data);
bool gbLoadState(struct GB*, const void* data, size_t size);
// The Mega Duck laptop's slot: the cartridge there takes the running ROM's place (a power of two in
// size), its mapper as at power on; the memory card's RAM stays.
// cheats.c
void gbClearCheats(struct GB*);
bool gbAddCheat(struct GB*, const char* code);
bool gbCheatsOnPage(const struct GB*, unsigned page);
uint8_t gbCheatRomByte(const struct GB*, uint16_t address, uint8_t value);
void gbApplySharkCodes(struct GB*);

// cheatcart.c
bool gbCheatCartInit(struct GBCheatCart*, const uint8_t* program, size_t size, bool hasGame);
void gbCheatCartFree(struct GBCheatCart*);
void gbCheatCartReset(struct GBCheatCart*);
uint8_t gbCheatCartRead(struct GB*, uint16_t address, bool dma);
void gbCheatCartWrite(struct GB*, uint16_t address, uint8_t value);
bool gbCheatCartReadRam(struct GB*, uint16_t address, uint8_t* value);
bool gbCheatCartWriteRam(struct GB*, uint16_t address, uint8_t value);
uint8_t* gbCheatCartKept(struct GBCheatCart*, size_t* size);
size_t gbCheatCartStateSize(const struct GBCheatCart*);
void gbCheatCartSaveState(const struct GBCheatCart*, uint8_t* data);
void gbCheatCartLoadState(struct GBCheatCart*, const uint8_t* data);
void gbSwapMegaDuckRom(struct GB*, const uint8_t* rom, size_t size, enum GBDuckKind);

// cpu.c
void gbCpuRun(struct GB*, uint32_t until);

// memory.c
void gbDmaSync(struct GB*);
void gbDmaStart(struct GB*, uint8_t page);
uint8_t gbRead(struct GB*, uint16_t address);
uint8_t gbCartRomRead(struct GB*, uint16_t address);
void gbWrite(struct GB*, uint16_t address, uint8_t value);
void gbMapRom(struct GB*);
uint8_t gbIoRead(struct GB*, uint8_t reg);
void gbIoWrite(struct GB*, uint8_t reg, uint8_t value);

// timer.c
void gbTimerSync(struct GB*);
uint8_t gbTimerRead(struct GB*, uint8_t reg);
void gbTimerWrite(struct GB*, uint8_t reg, uint8_t value);
void gbTimerEvent(struct GB*);
void gbTimerHold(struct GB*, uint32_t dots);
void gbTimerSwitchReset(struct GB*, bool toSingle);

// ppu.c
void gbPpuReset(struct GB*);
void gbPpuSync(struct GB*);
uint8_t gbPpuRead(struct GB*, uint8_t reg);
void gbPpuWrite(struct GB*, uint8_t reg, uint8_t value);
void gbPpuEvent(struct GB*);
void gbPpuReplan(struct GB*);
bool gbPpuInHblank(struct GB*);
void gbPpuFreeze(struct GB*, uint32_t dots);
// ppu-simd.c: a line's tiles through their palettes (see there).
void gbExpandTiles(uint32_t* line, uint8_t* info, const uint64_t* pixels, const uint32_t* const* colors, const uint8_t* front, int count);
void gbShadeLine(uint32_t* out, const uint8_t* index, const uint32_t* lut, int count);
void gbShadeLinePlain(uint32_t* out, const uint8_t* index, const uint32_t* lut, int count);
uint64_t gbFindObjects(const uint8_t* oam, int line, int height);
uint64_t gbFindObjectsPlain(const uint8_t* oam, int line, int height);
void gbExpandTilesPlain(uint32_t* line, uint8_t* info, const uint64_t* pixels, const uint32_t* const* colors, const uint8_t* front, int count);

// apu.c
void gbApuReset(struct GB*);
void gbApuSync(struct GB*);
void gbApuDivReset(struct GB*);
uint8_t gbApuRead(struct GB*, uint8_t reg);
uint8_t gbApuPcm(struct GB*, bool high);
void gbApuWrite(struct GB*, uint8_t reg, uint8_t value);

// serial.c
void gbSerialWrite(struct GB*, uint8_t reg, uint8_t value);
void gbSerialEvent(struct GB*);
void gbSerialReplan(struct GB*);
void gbSerialDivReset(struct GB*, uint16_t old);
// The CPU Clock: quarters of the console's own (4 to 16); keepCarry when a state loaded was run at it.
void gbSetCpuQuarters(struct GB*, unsigned quarters, bool keepCarry);
// An M-cycle of the CPU's when overclocked.
void gbCpuOverclockedCycle(struct GB*);
int gbLinkStatus(struct GB*, int request);

// cgb.c
uint8_t gbCgbRead(struct GB*, uint8_t reg);
void gbCgbWrite(struct GB*, uint8_t reg, uint8_t value);
void gbHdmaHblank(struct GB*, uint32_t at);
void gbHdmaEvent(struct GB*);
void gbHdmaRun(struct GB*);

// cart.c
bool gbCartInit(struct GB*);
void gbCartWrite(struct GB*, uint16_t address, uint8_t value);
uint8_t gbCartReadRam(struct GB*, uint16_t address);
void gbCartWriteRam(struct GB*, uint16_t address, uint8_t value);
uint16_t gbTama5Bank(struct GB*);
void gbMapperInit(struct GB*);
void gbMegaDuckWrite(struct GB*, uint16_t address, uint8_t value);
void gbMapperWrite(struct GB*, uint16_t address, uint8_t value);
bool gbPkjdRead(struct GB*, uint8_t* value);
bool gbPkjdWrite(struct GB*, uint8_t value);
// Where a Sachen cartridge's scrambled address lines read its header page.
uint16_t gbCartSachenAddress(uint16_t address);
uint8_t gbTama5Read(struct GB*, uint16_t address);
void gbTama5Write(struct GB*, uint16_t address, uint8_t value);
// A ROM read on a Vast Fame cartridge, where its protection may answer instead.
uint8_t gbCartReadVastFame(struct GB*, uint16_t address);
// A switchable-bank ROM byte as an unlicensed cartridge scrambles it.
static inline uint8_t gbCartRomByte(const struct GB* gb, uint8_t raw) {
	return gb->cart.romTransformed ? gb->cart.romLut[raw] : raw;
}
void gbRtcSync(struct GB*);
// The clock as 11 values: DH, DL, H, M, S, the ticks into the second, then the latched DH to S.
void gbRtcGet(struct GB*, uint32_t r[11]);
void gbRtcSet(struct GB*, const uint32_t r[11]);
void gbRtcSetSpeed(struct GB*, unsigned percent);
void gbRtcSetRealTime(struct GB*, bool on);
// HuC3's clock as the frontend's file keeps it: ticks into the minute (4 bytes, big-endian), then 256
// nibbles with the minutes at $10 and the days at $13, 3 each, lowest first.
enum { GB_HUC3_STATE = 0x104 };
void gbHuc3Get(struct GB*, uint8_t state[GB_HUC3_STATE]);
void gbHuc3Set(struct GB*, const uint8_t state[GB_HUC3_STATE]);

static inline int32_t gbSince(uint32_t now, uint32_t then) {
	return (int32_t) (now - then);
}

#endif
