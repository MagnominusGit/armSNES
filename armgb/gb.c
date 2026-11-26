/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Licensed under the GNU General Public License, version 2 or (at your option) any later version: see LICENSE. */
#include "gb.h"

#include <string.h>

// The machine: loading, reset, the events and running a frame.

static void updateNext(struct GB* gb) {
	uint32_t best = gb->now + 0x40000000;
	for (uint32_t on = gb->eventsOn; on; on &= on - 1) {
		int i = __builtin_ctz(on);
		if (gbSince(gb->events[i], best) < 0) {
			best = gb->events[i];
		}
	}
	gb->nextEvent = best;
}

void gbSchedule(struct GB* gb, int event, uint32_t at) {
	gb->events[event] = at;
	gb->eventsOn |= 1u << event;
	// Sooner than the CPU expected: it stops to look.
	if (gbSince(at, gb->nextEvent) < 0) {
		gb->attention = true;
	}
	updateNext(gb);
}

void gbUnschedule(struct GB* gb, int event) {
	gb->eventsOn &= ~(1u << event);
	updateNext(gb);
}

// Runs every event due by now, earliest first.
void gbRunEvents(struct GB* gb) {
	for (;;) {
		int due = -1;
		for (uint32_t on = gb->eventsOn; on; on &= on - 1) {
			int i = __builtin_ctz(on);
			if (gbSince(gb->now, gb->events[i]) >= 0 && (due < 0 || gbSince(gb->events[i], gb->events[due]) < 0)) {
				due = i;
			}
		}
		if (due < 0) {
			break;
		}
		gb->eventsOn &= ~(1u << due);
		switch (due) {
		case GB_EVENT_PPU:
			gbPpuEvent(gb);
			break;
		case GB_EVENT_TIMER:
			gbTimerEvent(gb);
			break;
		case GB_EVENT_SERIAL:
			gbSerialEvent(gb);
			break;
		case GB_EVENT_DMA:
			gbDmaSync(gb);
			break;
		case GB_EVENT_HDMA:
			gbHdmaEvent(gb);
			break;
		case GB_EVENT_SPEED:
			gbSpeedEvent(gb);
			break;
		case GB_EVENT_STALL:
			gb->cpu.stalled = false;
			gb->attention = true;
			break;
		default:
			break;
		}
	}
	updateNext(gb);
}

void gbRequest(struct GB* gb, uint8_t irq) {
	gb->ifl |= irq;
	gb->attention = true;
}

// The speed switch (SameBoy's stop() and GB_advance_cycles()): to double speed 6 cycles on, back to
// single at once; the PPU stands still for 5 cycles (1 to double with an interrupt pending), and unless
// an interrupt is pending the CPU halts for 0x20008 cycles counted at each speed, the divider running.
enum { SWITCH_CYCLES = 0x20008, SWITCH_TO_DOUBLE = 6 };

static void setSpeed(struct GB* gb, bool doubleSpeed) {
	gbApuSync(gb);
	gbTimerSync(gb);
	gb->doubleSpeed = doubleSpeed;
	gb->mcycle = doubleSpeed ? 2 : 4;
	gb->io[0x4D] = doubleSpeed ? 0x80 : 0;
	// The timer's next overflow and the serial transfer's end, at the divider's new rate.
	gbTimerEvent(gb);
	gbSerialReplan(gb);
}

void gbSpeedEvent(struct GB* gb) {
	struct GBCpu* cpu = &gb->cpu;
	if (cpu->switching == 1) {
		setSpeed(gb, true);
		if (cpu->halted) {
			cpu->switching = 2;
			// 4 dots short of the count, where SameBoy's CPU goes on (measured with gbsync).
			gbSchedule(gb, GB_EVENT_SPEED, gb->now + (SWITCH_CYCLES - SWITCH_TO_DOUBLE) / 2 - 4);
		} else {
			cpu->switching = 0;
		}
		return;
	}
	if (cpu->switching == 2) {
		cpu->switching = 0;
		cpu->halted = false;
		gb->attention = true;
	}
}

// STOP: on the Color with a speed switch armed, switches speed; otherwise sleeps until a button.
void gbStall(struct GB* gb, uint32_t dots) {
	gb->cpu.stalled = true;
	gbSchedule(gb, GB_EVENT_STALL, gb->now + dots);
}

void gbStop(struct GB* gb) {
	gb->attention = true;
	if (gb->cgbMode && (gb->io[0x4D] & 1)) {
		struct GBCpu* cpu = &gb->cpu;
		bool pending = gb->ie & gb->ifl & 0x1F;
		bool toDouble = !gb->doubleSpeed;
		gb->io[0x4D] &= 0x80;
		// A PPU half a dot late gives that back going to double speed.
		uint32_t freeze = pending ? (toDouble ? 1 : 0) : 5;
		if (toDouble && gb->ppu.halfDotLate) {
			gb->ppu.halfDotLate = false;
			freeze -= 1;
		}
		gbPpuFreeze(gb, freeze);
		if (!pending) {
			cpu->halted = true;
			cpu->justHalted = true;
		}
		if (toDouble) {
			cpu->switching = 1;
			gbSchedule(gb, GB_EVENT_SPEED, gb->now + SWITCH_TO_DOUBLE);
		} else {
			// Back at single speed the PPU is half a dot late again.
			gb->ppu.halfDotLate = gb->ppu.lcdc & 0x80;
			setSpeed(gb, false);
			if (!pending) {
				cpu->switching = 2;
				gbSchedule(gb, GB_EVENT_SPEED, gb->now + SWITCH_CYCLES);
			}
		}
		return;
	}
	gb->cpu.stopped = true;
}

// Work RAM as the original Game Boy's tends to come up: 256-byte blocks of mostly 0s and mostly 1s,
// alternating with address bit 8 and swapped by bit 11, with about one bit in fifty the other way.
// The stray bits come from a fixed seed, so every run starts alike.
static void powerOnRam(struct GB* gb) {
	uint32_t seed = 0x2F6E2B1;
	for (unsigned i = 0; i < GB_SIZE_WRAM; ++i) {
		uint8_t value = ((i >> 8) ^ (i >> 11)) & 1 ? 0xFF : 0x00;
		for (int bit = 0; bit < 8; ++bit) {
			seed ^= seed << 13;
			seed ^= seed >> 17;
			seed ^= seed << 5;
			if (seed % 50 == 0) {
				value ^= 1 << bit;
			}
		}
		gb->wram[i] = value;
	}
}

// The state the boot ROM leaves (Pan Docs, "Power Up Sequence").
// VRAM as the original Game Boy's boot ROM leaves it (Pan Docs, Power Up Sequence): the header's logo,
// each bit made 2x2, in tiles 1-24 from $8010, the (R) mark in tile $19, and both on the map. A Sachen
// cartridge shows the boot ROM its own logo through its scrambled address lines (armGBA's): the first
// kind's from a half page up.
static void bootLogo(struct GB* gb) {
	static const uint8_t mark[8] = { 0x3C, 0x42, 0xB9, 0xA5, 0xB9, 0xA5, 0x42, 0x3C };
	uint8_t logo[0x30];
	for (int i = 0; i < 0x30; ++i) {
		if (gb->cart.mapper == GB_SACHEN) {
			uint16_t from = gb->rom[0x104] == 0xCE && gb->rom[0x144] == 0xED ? 0x184 : 0x104;
			logo[i] = gb->rom[gbCartSachenAddress(from + i)];
		} else {
			logo[i] = gb->rom[0x104 + i];
		}
	}
	uint8_t* tile = gb->vram + 0x10;
	for (int i = 0; i < 0x30; ++i) {
		for (int half = 1; half >= 0; --half) {
			uint8_t nibble = logo[i] >> (half * 4) & 0xF;
			uint8_t doubled = 0;
			for (int bit = 0; bit < 4; ++bit) {
				doubled |= (nibble >> bit & 1) * (3 << bit * 2);
			}
			tile[0] = tile[2] = doubled;
			tile += 4;
		}
	}
	for (int i = 0; i < 8; ++i) {
		gb->vram[0x190 + i * 2] = mark[i];
	}
	for (int i = 0; i < 12; ++i) {
		gb->vram[0x1904 + i] = 1 + i;
		gb->vram[0x1924 + i] = 13 + i;
	}
	gb->vram[0x1910] = 0x19;
}

void gbReset(struct GB* gb) {
	const uint8_t* rom = gb->rom;
	size_t romSize = gb->romSize;
	void (*serialOut)(void*, uint8_t) = gb->serialOut;
	void* user = gb->user;
	uint8_t model = gb->model;
	struct GBCart cart = gb->cart;
	// Everything starts over but the cartridge's RAM.
	memset(gb, 0, offsetof(struct GB, sram));
	memset((uint8_t*) gb + offsetof(struct GB, sram) + sizeof(gb->sram), 0, offsetof(struct GB, rom) - offsetof(struct GB, sram) - sizeof(gb->sram));
	gb->rom = rom;
	gb->romSize = romSize;
	gb->serialOut = serialOut;
	gb->user = user;
	gb->model = model;
	gb->frameLimit = GB_FRAME_DOTS;
	// A reset starts the cartridge's mapper over; its battery-backed RAM and clocks run on.
	gbCartInit(gb);
	gb->colorsBuilt = false;
	if (gb->cheatCart) {
		gbCheatCartReset(gb->cheatCart);
	}
	gb->cart.rtc = cart.rtc;
	gb->cart.huc3.minutes = cart.huc3.minutes;
	gb->cart.huc3.days = cart.huc3.days;
	gb->cart.huc3.alarmMinutes = cart.huc3.alarmMinutes;
	gb->cart.huc3.alarmDays = cart.huc3.alarmDays;
	gb->cart.huc3.alarmOn = cart.huc3.alarmOn;
	// The Color's boot ROM runs in Color mode and leaves it by KEY0; without one, the header decides.
	// A Sachen cartridge's header reads through its scrambled address lines.
	uint8_t flag = gb->cart.mapper == GB_SACHEN ? rom[gbCartSachenAddress(0x143)] : rom[0x143];
	gb->cgbMode = gbColor(gb) && (gb->boot || (flag & 0x80));
	gbBlankFrame(gb);
	if (!gbColor(gb)) {
		powerOnRam(gb);
	}
	gb->mcycle = 4;
	struct GBCpu* cpu = &gb->cpu;
	gb->joypadSelect = 0;
	gb->wramBank = 1;
	if (gb->boot || gb->megaDuck) {
		// Power on into the boot ROM: everything at 0 but the divider, which starts at 8 (as SameBoy
		// has it), and the LCD off. The Mega Duck has no boot ROM: it starts the cartridge at 0000.
		gb->bootMapped = !gb->megaDuck;
		if (gb->duckLaptop) {
			cpu->sp = 0xFFFF;
		}
		gb->timer.counter = 8;
		gb->timer.reloadedAt = gb->now - 0x10000;
		gbMapRom(gb);
		gbApuReset(gb);
		gbPpuReset(gb);
		gb->ppu.lcdc = 0;
		gbUnschedule(gb, GB_EVENT_PPU);
		updateNext(gb);
		return;
	}
	// As each model's boot ROM leaves them (Pan Docs, Power Up Sequence): the Advance's B is 1 and
	// its Z clear, the Pocket's A is FF.
	if (gbColor(gb)) {
		cpu->a = 0x11;
		cpu->f = model == GB_MODEL_AGB ? 0x00 : 0x80;
		cpu->b = model == GB_MODEL_AGB ? 0x01 : 0x00;
		cpu->d = 0xFF;
		cpu->e = 0x56;
		cpu->l = 0x0D;
	} else {
		cpu->a = model == GB_MODEL_MGB ? 0xFF : 0x01;
		cpu->f = 0xB0;
		cpu->c = 0x13;
		cpu->e = 0xD8;
		cpu->h = 0x01;
		cpu->l = 0x4D;
	}
	if (!gbColor(gb)) {
		bootLogo(gb);
	} else if (gb->cgbMode) {
		// The Color's boot ROM leaves the background's palettes white.
		for (int i = 0; i < 64; i += 2) {
			gb->cgb.bgPalette[i] = 0xFF;
			gb->cgb.bgPalette[i + 1] = 0x7F;
		}
	} else {
		// An original Game Boy game on the Color: the four shades as grays, for the background and both
		// sprite palettes, where the boot ROM would have chosen colors.
		static const uint16_t grays[4] = { 0x7FFF, 0x5294, 0x294A, 0x0000 };
		gb->cgb.compatShades = true;
		for (int i = 0; i < 8; ++i) {
			gb->cgb.bgPalette[i] = gb->cgb.objPalette[i] = gb->cgb.objPalette[8 + i] = grays[i >> 1] >> (i & 1 ? 8 : 0);
		}
	}
	cpu->sp = 0xFFFE;
	cpu->pc = 0x100;
	gb->timer.counter = 0xABCC;
	gb->timer.reloadedAt = gb->now - 0x10000;
	gb->ifl = 0x01;
	gbMapRom(gb);
	gbApuReset(gb);
	gb->apu.power = true;
	gb->apu.regs[0x24 - 0x10] = 0x77;
	gb->apu.regs[0x25 - 0x10] = 0xF3;
	gbPpuReset(gb);
	updateNext(gb);
}

// The screen showing nothing, as with the LCD off.
void gbBlankFrame(struct GB* gb) {
	for (int y = 0; y < GB_HEIGHT; ++y) {
		for (int x = 0; x < GB_WIDTH; ++x) {
			gb->frame[y][x] = gbColor(gb) ? 0x7FFF : GB_SHADE_OFF;
			if (gb->video) {
				gb->video[y * GB_WIDTH + x] = gbColor(gb) && gb->videoTable ? gb->videoTable[0x7FFF] : gb->videoColors[4];
			}
		}
	}
}

// A state is the struct up to the cartridge's pointer: everything after it is set again on loading.
size_t gbStateSize(void) {
	return offsetof(struct GB, rom);
}

void gbSaveState(const struct GB* gb, void* data) {
	memcpy(data, gb, gbStateSize());
}

bool gbLoadState(struct GB* gb, const void* data, size_t size) {
	if (size < gbStateSize()) {
		return false;
	}
	memcpy(gb, data, gbStateSize());
	gb->colorsBuilt = false;
	gbMapRom(gb);
	// The device's time goes on from now, not from when the state was saved.
	gb->cart.rtc.lastHost = gb->hostMicros;
	// A frontend's picture isn't in a state: blank while the screen shows nothing, else redrawn by
	// the next frame.
	if (gb->video && (!(gb->ppu.lcdc & 0x80) || gb->ppu.hiddenFrame)) {
		gbBlankFrame(gb);
	}
	return true;
}

bool gbLoad(struct GB* gb, const uint8_t* rom, size_t size, enum GBModel model) {
	if (size < 0x150) {
		return false;
	}
	const uint8_t* boot = gb->boot;
	size_t bootSize = gb->bootSize;
	uint32_t* video = gb->video;
	const uint32_t* videoTable = gb->videoTable;
	uint8_t layersHidden = gb->layersHidden, channelsMuted = gb->channelsMuted;
	unsigned cpuQuarters = gb->cpuQuarters ? gb->cpuQuarters : 4;
	bool rtcRealTime = gb->rtcRealTime;
	unsigned rtcPercent = gb->rtcSpeedSet ? gb->rtcPercent : 100;
	bool rtcSpeedSet = gb->rtcSpeedSet;
	int64_t hostMicros = gb->hostMicros;
	uint8_t megaDuck = gb->megaDuck;
	bool duckLaptop = gb->duckLaptop;
	uint32_t videoColors[5];
	memcpy(videoColors, gb->videoColors, sizeof(videoColors));
	memset(gb, 0, sizeof(*gb));
	gb->boot = boot;
	gb->bootSize = bootSize;
	gb->video = video;
	gb->videoTable = videoTable;
	gb->layersHidden = layersHidden;
	gb->cpuQuarters = cpuQuarters;
	gb->channelsMuted = channelsMuted;
	gb->rtcRealTime = rtcRealTime;
	gb->rtcPercent = rtcPercent;
	gb->rtcSpeedSet = rtcSpeedSet;
	gb->hostMicros = hostMicros;
	gb->megaDuck = megaDuck;
	gb->duckLaptop = duckLaptop;
	memcpy(gb->videoColors, videoColors, sizeof(videoColors));
	gb->rom = rom;
	gb->romSize = size;
	gb->model = model;
	// Cartridge RAM with no save yet reads as all 1s, as other emulators start it.
	memset(gb->sram, 0xFF, sizeof(gb->sram));
	if (!gbCartInit(gb)) {
		return false;
	}
	gbReset(gb);
	return true;
}

// Runs to the start of the next V-blank, or a frame's time with the LCD off.
void gbRunFrame(struct GB* gb) {
	gb->audioCount = 0;
	uint32_t dots = 4 * GB_FRAME_DOTS;
	gbRunFor(gb, &dots);
}

bool gbRunFor(struct GB* gb, uint32_t* dots) {
	uint32_t start = gb->now;
	uint32_t until = start + *dots;
	gb->frameEnded = false;
	while (!gb->frameEnded && gbSince(until, gb->now) > 0) {
		gbCpuRun(gb, gbSince(gb->frameLimit, until) < 0 ? gb->frameLimit : until);
		if (!gb->frameEnded && gbSince(gb->now, gb->frameLimit) >= 0) {
			// With the LCD on, a frame goes on to V-blank, however late it started.
			if (gb->ppu.lcdc & 0x80) {
				gb->frameLimit = gb->now + GB_FRAME_DOTS;
			} else {
				gb->frameEnded = true;
			}
		}
	}
	if (gb->frameEnded) {
		gb->frameLimit = gb->now + GB_FRAME_DOTS;
		gbApplySharkCodes(gb);
		// The clock counts in a time that wraps after some minutes: up to date every frame.
		if (gb->cart.hasRtc) {
			gbRtcSync(gb);
		}
	}
	gbApuSync(gb);
	gb->elapsed = gbTime(gb);
	gb->elapsedAt = gb->now;
	*dots = gb->now - start;
	return gb->frameEnded;
}
