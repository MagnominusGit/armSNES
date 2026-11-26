/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Licensed under the GNU General Public License, version 2 or (at your option) any later version: see LICENSE.
 *
 * The access and HALT timing follow SameBoy (Copyright (c) Lior Halphon), under the Expat (MIT) license:
 * see LICENSES/SameBoy.txt. */
#include "gb.h"

// The Sharp SM83, one instruction at a time. Every memory access takes an M-cycle and comes at its
// start, as SameBoy (MIT) times them. Opcodes and flags as Pan Docs
// and the gbdev opcode tables give them.

enum {
	FLAG_Z = 0x80,
	FLAG_N = 0x40,
	FLAG_H = 0x20,
	FLAG_C = 0x10,
};

static inline uint16_t pair(uint8_t hi, uint8_t lo) {
	return hi << 8 | lo;
}

static inline uint16_t getHL(const struct GBCpu* cpu) {
	return pair(cpu->h, cpu->l);
}

static inline void setHL(struct GBCpu* cpu, uint16_t value) {
	cpu->h = value >> 8;
	cpu->l = value;
}

// BC, DE, HL, SP by the two bits most 16-bit instructions use.
static uint16_t getRR(const struct GBCpu* cpu, int rr) {
	switch (rr) {
	case 0:
		return pair(cpu->b, cpu->c);
	case 1:
		return pair(cpu->d, cpu->e);
	case 2:
		return getHL(cpu);
	default:
		return cpu->sp;
	}
}

static void setRR(struct GBCpu* cpu, int rr, uint16_t value) {
	switch (rr) {
	case 0:
		cpu->b = value >> 8;
		cpu->c = value;
		break;
	case 1:
		cpu->d = value >> 8;
		cpu->e = value;
		break;
	case 2:
		setHL(cpu, value);
		break;
	default:
		cpu->sp = value;
		break;
	}
}

static bool condition(const struct GBCpu* cpu, int cc) {
	switch (cc) {
	case 0:
		return !(cpu->f & FLAG_Z);
	case 1:
		return cpu->f & FLAG_Z;
	case 2:
		return !(cpu->f & FLAG_C);
	default:
		return cpu->f & FLAG_C;
	}
}

// The eight ALU operations on A, by the opcode's middle bits.
static inline __attribute__((always_inline)) void alu(struct GBCpu* cpu, int op, uint8_t value) {
	uint8_t a = cpu->a;
	unsigned carry = cpu->f & FLAG_C ? 1 : 0;
	unsigned result;
	switch (op) {
	case 0: // ADD
	case 1: // ADC
		if (op == 0) {
			carry = 0;
		}
		result = a + value + carry;
		cpu->f = ((result & 0xFF) ? 0 : FLAG_Z) | (((a & 0xF) + (value & 0xF) + carry) > 0xF ? FLAG_H : 0) |
		         (result > 0xFF ? FLAG_C : 0);
		cpu->a = result;
		break;
	case 2: // SUB
	case 3: // SBC
	case 7: // CP
		if (op != 3) {
			carry = 0;
		}
		result = a - value - carry;
		cpu->f = ((result & 0xFF) ? 0 : FLAG_Z) | FLAG_N | ((int) (a & 0xF) - (int) (value & 0xF) - (int) carry < 0 ? FLAG_H : 0) |
		         (result > 0xFF ? FLAG_C : 0);
		if (op != 7) {
			cpu->a = result;
		}
		break;
	case 4: // AND
		cpu->a = a & value;
		cpu->f = (cpu->a ? 0 : FLAG_Z) | FLAG_H;
		break;
	case 5: // XOR
		cpu->a = a ^ value;
		cpu->f = cpu->a ? 0 : FLAG_Z;
		break;
	default: // OR
		cpu->a = a | value;
		cpu->f = cpu->a ? 0 : FLAG_Z;
		break;
	}
}

static void daa(struct GBCpu* cpu) {
	unsigned a = cpu->a;
	uint8_t f = cpu->f;
	if (f & FLAG_N) {
		if (f & FLAG_H) {
			a -= 6;
		}
		if (f & FLAG_C) {
			a -= 0x60;
		}
	} else {
		if ((f & FLAG_H) || (a & 0xF) > 9) {
			a += 6;
		}
		if ((f & FLAG_C) || a > 0x9F) {
			a += 0x60;
		}
	}
	cpu->f = (f & (FLAG_N | FLAG_C)) | (!(f & FLAG_N) && a > 0xFF ? FLAG_C : 0);
	cpu->a = a;
	if (!cpu->a) {
		cpu->f |= FLAG_Z;
	}
}

// SP plus a signed byte, for ADD SP,e and LD HL,SP+e: flags from the low byte's unsigned addition.
static uint16_t spOffset(struct GBCpu* cpu, uint8_t e) {
	uint16_t sp = cpu->sp;
	cpu->f = ((sp & 0xF) + (e & 0xF) > 0xF ? FLAG_H : 0) | ((sp & 0xFF) + e > 0xFF ? FLAG_C : 0);
	return sp + (int8_t) e;
}

// Ends HALT or STOP when the time comes: through events up to the next one.
static void idle(struct GB* gb, uint32_t until) {
	uint32_t target = gbSince(gb->nextEvent, until) < 0 ? gb->nextEvent : until;
	int32_t ahead = gbSince(target, gb->now);
	if (ahead > 0) {
		gb->now += (ahead + gb->mcycle - 1) / gb->mcycle * gb->mcycle;
	}
}

// An M-cycle of HALT. The original Game Boy looks for an interrupt 2 dots into each M-cycle but the
// first, the Color at the start of each, as SameBoy (MIT) has it. Returns whether the CPU woke, at
// the end of the M-cycle that saw the interrupt; otherwise moves on to the M-cycle that will.
static bool wake(struct GB* gb, uint32_t until) {
	struct GBCpu* cpu = &gb->cpu;
	unsigned later = gbColor(gb) ? 0 : 2;
	unsigned offset = cpu->justHalted ? 0 : later;
	cpu->justHalted = false;
	uint32_t start = gb->now;
	gb->now = start + offset;
	if (gbSince(gb->now, gb->nextEvent) >= 0) {
		gbRunEvents(gb);
	}
	// A speed switch done: the CPU goes on at once.
	if (!cpu->halted) {
		gb->now = start;
		return true;
	}
	if (gb->ie & gb->ifl & 0x1F) {
		cpu->halted = false;
		if (cpu->switching == 2) {
			cpu->switching = 0;
		}
		gb->now = start + gb->mcycle;
		return true;
	}
	uint32_t target = gbSince(gb->nextEvent, until) < 0 ? gb->nextEvent : until;
	int32_t ahead = gbSince(target, start + later);
	uint32_t cycles = ahead > 0 ? (ahead + gb->mcycle - 1) / gb->mcycle : 1;
	gb->now = start + (cycles ? cycles : 1) * gb->mcycle;
	return false;
}

#define CPU(name) name
#define CPU_OVERCLOCKED 0
#include "cpu-run.h"
#undef CPU
#undef CPU_OVERCLOCKED
#define CPU(name) name##Overclocked
#define CPU_OVERCLOCKED 1
#include "cpu-run.h"
#undef CPU
#undef CPU_OVERCLOCKED

void gbCpuRun(struct GB* gb, uint32_t until) {
	if (gb->cpuQuarters == 4) {
		run(gb, until);
	} else {
		runOverclocked(gb, until);
	}
}

// The CPU Clock above 100% (armGBA's): an M-cycle takes 16 / quarters
// of the console's T-cycles, the part left over carried to the next; in double speed the CPU moves a
// whole dot at a time and goes no faster than 200%, as the picture can't take more than one of its
// accesses in a dot. The rest of the console keeps its own pace. For two lines after a read of STAT or
// LY the CPU keeps the console's pace too, for code timed against the picture.
void gbCpuOverclockedCycle(struct GB* gb) {
	if (gb->cpuPaced) {
		gb->cpuPaced = gb->cpuPaced > gb->mcycle ? gb->cpuPaced - gb->mcycle : 0;
		gb->now += gb->mcycle;
		return;
	}
	gb->cpuCarry += 16;
	if (!gb->doubleSpeed) {
		uint32_t dots = gb->cpuCarry / gb->cpuQuarters;
		gb->cpuCarry -= dots * gb->cpuQuarters;
		gb->now += dots;
	} else {
		unsigned quarters = gb->cpuQuarters > 8 ? 8 : gb->cpuQuarters;
		uint32_t halves = gb->cpuCarry / (2 * quarters) * 2;
		gb->cpuCarry -= halves * quarters;
		gb->now += halves / 2;
	}
}

void gbSetCpuQuarters(struct GB* gb, unsigned quarters, bool keepCarry) {
	if (quarters != gb->cpuQuarters) {
		gb->cpuQuarters = quarters;
		if (!keepCarry) {
			gb->cpuCarry = 0;
		}
	}
}
