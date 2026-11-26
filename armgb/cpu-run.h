/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Licensed under the GNU General Public License, version 2 or (at your option) any later version: see LICENSE.
 *
 * The access and HALT timing follow SameBoy (Copyright (c) Lior Halphon), under the Expat (MIT) license:
 * see LICENSES/SameBoy.txt. */

// The part of the CPU that moves time, included by cpu.c twice: at the console's own clock, and with
// the CPU Clock away from 100% (CPU_OVERCLOCKED), each of its functions named through CPU().

static inline __attribute__((always_inline)) void CPU(tick)(struct GB* gb) {
	if (CPU_OVERCLOCKED) {
		gbCpuOverclockedCycle(gb);
	} else {
		gb->now += gb->mcycle;
	}
}

static inline __attribute__((always_inline)) uint8_t CPU(readNow)(struct GB* gb, uint16_t address) {
	const uint8_t* page = gb->readMap[address >> 12];
	if (page) {
		return page[address & 0xFFF];
	}
	// High RAM, which OAM DMA leaves alone.
	if (address >= 0xFF80 && address != 0xFFFF) {
		return gb->hram[address & 0x7F];
	}
	return gbRead(gb, address);
}

static inline __attribute__((always_inline)) uint8_t CPU(read8)(struct GB* gb, uint16_t address) {
	uint8_t value = CPU(readNow)(gb, address);
	CPU(tick)(gb);
	return value;
}

static inline __attribute__((always_inline)) void CPU(write8)(struct GB* gb, uint16_t address, uint8_t value) {
	uint8_t* page = gb->writeMap[address >> 12];
	if (page) {
		page[address & 0xFFF] = value;
	} else if (address >= 0xFF80 && address != 0xFFFF) {
		gb->hram[address & 0x7F] = value;
	} else {
		gbWrite(gb, address, value);
	}
	CPU(tick)(gb);
}

static inline __attribute__((always_inline)) uint8_t CPU(fetch)(struct GB* gb) {
	struct GBCpu* cpu = &gb->cpu;
	return CPU(read8)(gb, cpu->pc++);
}

static inline __attribute__((always_inline)) uint16_t CPU(fetch16)(struct GB* gb) {
	uint8_t lo = CPU(fetch)(gb);
	return lo | CPU(fetch)(gb) << 8;
}

static inline __attribute__((always_inline)) uint8_t CPU(getR)(struct GB* gb, int r) {
	struct GBCpu* cpu = &gb->cpu;
	switch (r) {
	case 0:
		return cpu->b;
	case 1:
		return cpu->c;
	case 2:
		return cpu->d;
	case 3:
		return cpu->e;
	case 4:
		return cpu->h;
	case 5:
		return cpu->l;
	case 6:
		return CPU(read8)(gb, getHL(cpu));
	default:
		return cpu->a;
	}
}

static inline __attribute__((always_inline)) void CPU(setR)(struct GB* gb, int r, uint8_t value) {
	struct GBCpu* cpu = &gb->cpu;
	switch (r) {
	case 0:
		cpu->b = value;
		break;
	case 1:
		cpu->c = value;
		break;
	case 2:
		cpu->d = value;
		break;
	case 3:
		cpu->e = value;
		break;
	case 4:
		cpu->h = value;
		break;
	case 5:
		cpu->l = value;
		break;
	case 6:
		CPU(write8)(gb, getHL(cpu), value);
		break;
	default:
		cpu->a = value;
		break;
	}
}

static inline __attribute__((always_inline)) void CPU(push)(struct GB* gb, uint16_t value) {
	struct GBCpu* cpu = &gb->cpu;
	CPU(write8)(gb, --cpu->sp, value >> 8);
	CPU(write8)(gb, --cpu->sp, value);
}

static inline __attribute__((always_inline)) uint16_t CPU(pop)(struct GB* gb) {
	struct GBCpu* cpu = &gb->cpu;
	uint8_t lo = CPU(read8)(gb, cpu->sp++);
	return lo | CPU(read8)(gb, cpu->sp++) << 8;
}

// The CB-prefixed rotates, shifts, swap and bit operations.
static inline __attribute__((always_inline)) void CPU(prefixed)(struct GB* gb) {
	struct GBCpu* cpu = &gb->cpu;
	uint8_t op = CPU(fetch)(gb);
	int r = op & 7;
	int bit = op >> 3 & 7;
	uint8_t value = CPU(getR)(gb, r);
	uint8_t carryIn = cpu->f & FLAG_C ? 1 : 0;
	uint8_t result;
	switch (op >> 6) {
	case 0: {
		bool carryOut;
		switch (bit) {
		case 0: // RLC
			carryOut = value >> 7;
			result = value << 1 | value >> 7;
			break;
		case 1: // RRC
			carryOut = value & 1;
			result = value >> 1 | value << 7;
			break;
		case 2: // RL
			carryOut = value >> 7;
			result = value << 1 | carryIn;
			break;
		case 3: // RR
			carryOut = value & 1;
			result = value >> 1 | carryIn << 7;
			break;
		case 4: // SLA
			carryOut = value >> 7;
			result = value << 1;
			break;
		case 5: // SRA
			carryOut = value & 1;
			result = (value >> 1) | (value & 0x80);
			break;
		case 6: // SWAP
			carryOut = false;
			result = value << 4 | value >> 4;
			break;
		default: // SRL
			carryOut = value & 1;
			result = value >> 1;
			break;
		}
		cpu->f = (result ? 0 : FLAG_Z) | (carryOut ? FLAG_C : 0);
		CPU(setR)(gb, r, result);
		break;
	}
	case 1: // BIT
		cpu->f = (cpu->f & FLAG_C) | FLAG_H | (value & (1 << bit) ? 0 : FLAG_Z);
		break;
	case 2: // RES
		CPU(setR)(gb, r, value & ~(1 << bit));
		break;
	default: // SET
		CPU(setR)(gb, r, value | 1 << bit);
		break;
	}
}

// Runs the interrupt with the lowest flag set and enabled: three M-cycles, then PC's high byte pushed
// in the fourth and its low byte in the fifth (SameBoy). IE is taken after the high byte (which can
// write it), IF as the low byte is written (its old value, if that write is to IF); if that leaves
// nothing pending, PC becomes 0. The flag is cleared half an M-cycle later, so the same interrupt
// requested again by then is taken with it.
static void CPU(dispatch)(struct GB* gb) {
	struct GBCpu* cpu = &gb->cpu;
	cpu->ime = false;
	CPU(tick)(gb);
	CPU(tick)(gb);
	CPU(tick)(gb);
	CPU(write8)(gb, --cpu->sp, cpu->pc >> 8);
	uint8_t enabled = gb->ie;
	if (gbSince(gb->now, gb->nextEvent) >= 0) {
		gbRunEvents(gb);
	}
	uint8_t pending = enabled & gb->ifl & 0x1F;
	CPU(write8)(gb, --cpu->sp, cpu->pc);
	if (!pending) {
		cpu->pc = 0;
		return;
	}
	int n = __builtin_ctz(pending);
	unsigned half = gb->mcycle / 2;
	gb->now -= half;
	if (gbSince(gb->now, gb->nextEvent) >= 0) {
		gbRunEvents(gb);
	}
	gb->now += half;
	gb->ifl &= ~(1 << n);
	cpu->pc = 0x40 + n * 8;
}

static inline __attribute__((always_inline)) void CPU(step)(struct GB* gb, uint8_t op) {
	struct GBCpu* cpu = &gb->cpu;
	switch (op) {
	case 0x00: // NOP
		break;
	case 0x01:
	case 0x11:
	case 0x21:
	case 0x31: // LD rr,nn
		setRR(cpu, op >> 4, CPU(fetch16)(gb));
		break;
	case 0x02: // LD (BC),A
		CPU(write8)(gb, pair(cpu->b, cpu->c), cpu->a);
		break;
	case 0x12: // LD (DE),A
		CPU(write8)(gb, pair(cpu->d, cpu->e), cpu->a);
		break;
	case 0x22: { // LD (HL+),A
		uint16_t hl = getHL(cpu);
		CPU(write8)(gb, hl, cpu->a);
		setHL(cpu, hl + 1);
		break;
	}
	case 0x32: { // LD (HL-),A
		uint16_t hl = getHL(cpu);
		CPU(write8)(gb, hl, cpu->a);
		setHL(cpu, hl - 1);
		break;
	}
	case 0x0A: // LD A,(BC)
		cpu->a = CPU(read8)(gb, pair(cpu->b, cpu->c));
		break;
	case 0x1A: // LD A,(DE)
		cpu->a = CPU(read8)(gb, pair(cpu->d, cpu->e));
		break;
	case 0x2A: { // LD A,(HL+)
		uint16_t hl = getHL(cpu);
		cpu->a = CPU(read8)(gb, hl);
		setHL(cpu, hl + 1);
		break;
	}
	case 0x3A: { // LD A,(HL-)
		uint16_t hl = getHL(cpu);
		cpu->a = CPU(read8)(gb, hl);
		setHL(cpu, hl - 1);
		break;
	}
	case 0x03:
	case 0x13:
	case 0x23:
	case 0x33: // INC rr
		setRR(cpu, op >> 4, getRR(cpu, op >> 4) + 1);
		CPU(tick)(gb);
		break;
	case 0x0B:
	case 0x1B:
	case 0x2B:
	case 0x3B: // DEC rr
		setRR(cpu, op >> 4, getRR(cpu, op >> 4) - 1);
		CPU(tick)(gb);
		break;
	case 0x04:
	case 0x0C:
	case 0x14:
	case 0x1C:
	case 0x24:
	case 0x2C:
	case 0x34:
	case 0x3C: { // INC r
		int r = op >> 3 & 7;
		uint8_t value = CPU(getR)(gb, r) + 1;
		cpu->f = (cpu->f & FLAG_C) | (value ? 0 : FLAG_Z) | ((value & 0xF) == 0 ? FLAG_H : 0);
		CPU(setR)(gb, r, value);
		break;
	}
	case 0x05:
	case 0x0D:
	case 0x15:
	case 0x1D:
	case 0x25:
	case 0x2D:
	case 0x35:
	case 0x3D: { // DEC r
		int r = op >> 3 & 7;
		uint8_t value = CPU(getR)(gb, r) - 1;
		cpu->f = (cpu->f & FLAG_C) | FLAG_N | (value ? 0 : FLAG_Z) | ((value & 0xF) == 0xF ? FLAG_H : 0);
		CPU(setR)(gb, r, value);
		break;
	}
	case 0x06:
	case 0x0E:
	case 0x16:
	case 0x1E:
	case 0x26:
	case 0x2E:
	case 0x36:
	case 0x3E: // LD r,n
		CPU(setR)(gb, op >> 3 & 7, CPU(fetch)(gb));
		break;
	case 0x07: // RLCA
		cpu->f = cpu->a >> 7 ? FLAG_C : 0;
		cpu->a = cpu->a << 1 | cpu->a >> 7;
		break;
	case 0x0F: // RRCA
		cpu->f = cpu->a & 1 ? FLAG_C : 0;
		cpu->a = cpu->a >> 1 | cpu->a << 7;
		break;
	case 0x17: { // RLA
		uint8_t carry = cpu->f & FLAG_C ? 1 : 0;
		cpu->f = cpu->a >> 7 ? FLAG_C : 0;
		cpu->a = cpu->a << 1 | carry;
		break;
	}
	case 0x1F: { // RRA
		uint8_t carry = cpu->f & FLAG_C ? 0x80 : 0;
		cpu->f = cpu->a & 1 ? FLAG_C : 0;
		cpu->a = cpu->a >> 1 | carry;
		break;
	}
	case 0x08: { // LD (nn),SP
		uint16_t address = CPU(fetch16)(gb);
		CPU(write8)(gb, address, cpu->sp);
		CPU(write8)(gb, address + 1, cpu->sp >> 8);
		break;
	}
	case 0x09:
	case 0x19:
	case 0x29:
	case 0x39: { // ADD HL,rr
		uint16_t hl = getHL(cpu);
		uint16_t value = getRR(cpu, op >> 4);
		unsigned sum = hl + value;
		cpu->f = (cpu->f & FLAG_Z) | ((hl & 0xFFF) + (value & 0xFFF) > 0xFFF ? FLAG_H : 0) | (sum > 0xFFFF ? FLAG_C : 0);
		setHL(cpu, sum);
		CPU(tick)(gb);
		break;
	}
	case 0x10: { // STOP
		// DIV resets (held an M-cycle longer with IME off), and the second byte is skipped only when
		// no interrupt is pending (SameBoy's stop()).
		bool pending = gb->ie & gb->ifl & 0x1F;
		if (gb->cgbMode && (gb->io[0x4D] & 1) && gb->doubleSpeed) {
			// Back to single speed, the CPU is stopped through the second byte's read, so DIV stands
			// still through it, and with IME off an M-cycle more (AGE's spsw-div and spsw-mode0).
			gbTimerSwitchReset(gb, true);
			if (!pending) {
				CPU(fetch)(gb);
			}
			gbTimerHold(gb, cpu->ime ? 0 : 4);
		} else {
			bool switching = gb->cgbMode && (gb->io[0x4D] & 1);
			if (cpu->ime) {
				switching ? gbTimerSwitchReset(gb, false) : gbTimerWrite(gb, 0x04, 0);
			}
			if (!pending) {
				CPU(fetch)(gb);
			}
			if (!cpu->ime) {
				switching ? gbTimerSwitchReset(gb, false) : gbTimerWrite(gb, 0x04, 0);
			}
		}
		gbStop(gb);
		break;
	}
	case 0x18: { // JR e
		int8_t e = CPU(fetch)(gb);
		cpu->pc += e;
		CPU(tick)(gb);
		break;
	}
	case 0x20:
	case 0x28:
	case 0x30:
	case 0x38: { // JR cc,e
		int8_t e = CPU(fetch)(gb);
		if (condition(cpu, op >> 3 & 3)) {
			cpu->pc += e;
			CPU(tick)(gb);
		}
		break;
	}
	case 0x27:
		daa(cpu);
		break;
	case 0x2F: // CPL
		cpu->a = ~cpu->a;
		cpu->f |= FLAG_N | FLAG_H;
		break;
	case 0x37: // SCF
		cpu->f = (cpu->f & FLAG_Z) | FLAG_C;
		break;
	case 0x3F: // CCF
		cpu->f = (cpu->f & FLAG_Z) | ((cpu->f & FLAG_C) ^ FLAG_C);
		break;
	case 0x76: { // HALT
		gb->attention = true;
		// Interrupts requested up to now, events due during the fetch included.
		if (gbSince(gb->now, gb->nextEvent) >= 0) {
			gbRunEvents(gb);
		}
		uint8_t pending = gb->ie & gb->ifl & 0x1F;
		// With an interrupt already pending, HALT doesn't stop: with IME on, the interrupt is taken at
		// once and returns to the HALT (SameBoy); with IME off, the halt bug.
		if (pending && cpu->ime) {
			--cpu->pc;
		} else if (pending && !cpu->ime) {
			cpu->haltBug = true;
		} else {
			cpu->halted = true;
			cpu->justHalted = true;
		}
		break;
	}
	case 0xC0:
	case 0xC8:
	case 0xD0:
	case 0xD8: // RET cc
		CPU(tick)(gb);
		if (condition(cpu, op >> 3 & 3)) {
			cpu->pc = CPU(pop)(gb);
			CPU(tick)(gb);
		}
		break;
	case 0xC9: // RET
		cpu->pc = CPU(pop)(gb);
		CPU(tick)(gb);
		break;
	case 0xD9: // RETI
		cpu->pc = CPU(pop)(gb);
		CPU(tick)(gb);
		cpu->ime = true;
		gb->attention = true;
		break;
	case 0xC1:
	case 0xD1:
	case 0xE1: // POP rr
		setRR(cpu, op >> 4 & 3, CPU(pop)(gb));
		break;
	case 0xF1: { // POP AF
		uint16_t value = CPU(pop)(gb);
		cpu->a = value >> 8;
		cpu->f = value & 0xF0;
		break;
	}
	case 0xC5:
	case 0xD5:
	case 0xE5: // PUSH rr
		CPU(tick)(gb);
		CPU(push)(gb, getRR(cpu, op >> 4 & 3));
		break;
	case 0xF5: // PUSH AF
		CPU(tick)(gb);
		CPU(push)(gb, pair(cpu->a, cpu->f));
		break;
	case 0xC2:
	case 0xCA:
	case 0xD2:
	case 0xDA: { // JP cc,nn
		uint16_t address = CPU(fetch16)(gb);
		if (condition(cpu, op >> 3 & 3)) {
			cpu->pc = address;
			CPU(tick)(gb);
		}
		break;
	}
	case 0xC3: // JP nn
		cpu->pc = CPU(fetch16)(gb);
		CPU(tick)(gb);
		break;
	case 0xE9: // JP HL
		cpu->pc = getHL(cpu);
		break;
	case 0xC4:
	case 0xCC:
	case 0xD4:
	case 0xDC: { // CALL cc,nn
		uint16_t address = CPU(fetch16)(gb);
		if (condition(cpu, op >> 3 & 3)) {
			CPU(tick)(gb);
			CPU(push)(gb, cpu->pc);
			cpu->pc = address;
		}
		break;
	}
	case 0xCD: { // CALL nn
		uint16_t address = CPU(fetch16)(gb);
		CPU(tick)(gb);
		CPU(push)(gb, cpu->pc);
		cpu->pc = address;
		break;
	}
	case 0xC7:
	case 0xCF:
	case 0xD7:
	case 0xDF:
	case 0xE7:
	case 0xEF:
	case 0xF7:
	case 0xFF: // RST
		CPU(tick)(gb);
		CPU(push)(gb, cpu->pc);
		cpu->pc = op & 0x38;
		break;
	case 0xC6:
	case 0xCE:
	case 0xD6:
	case 0xDE:
	case 0xE6:
	case 0xEE:
	case 0xF6:
	case 0xFE: // ALU A,n
		alu(cpu, op >> 3 & 7, CPU(fetch)(gb));
		break;
	case 0xCB:
		CPU(prefixed)(gb);
		break;
	case 0xE0: // LDH (n),A
		CPU(write8)(gb, 0xFF00 | CPU(fetch)(gb), cpu->a);
		break;
	case 0xF0: // LDH A,(n)
		cpu->a = CPU(read8)(gb, 0xFF00 | CPU(fetch)(gb));
		break;
	case 0xE2: // LD (C),A
		CPU(write8)(gb, 0xFF00 | cpu->c, cpu->a);
		break;
	case 0xF2: // LD A,(C)
		cpu->a = CPU(read8)(gb, 0xFF00 | cpu->c);
		break;
	case 0xEA: // LD (nn),A
		CPU(write8)(gb, CPU(fetch16)(gb), cpu->a);
		break;
	case 0xFA: // LD A,(nn)
		cpu->a = CPU(read8)(gb, CPU(fetch16)(gb));
		break;
	case 0xE8: { // ADD SP,e
		uint8_t e = CPU(fetch)(gb);
		cpu->sp = spOffset(cpu, e);
		CPU(tick)(gb);
		CPU(tick)(gb);
		break;
	}
	case 0xF8: { // LD HL,SP+e
		uint8_t e = CPU(fetch)(gb);
		setHL(cpu, spOffset(cpu, e));
		CPU(tick)(gb);
		break;
	}
	case 0xF9: // LD SP,HL
		cpu->sp = getHL(cpu);
		CPU(tick)(gb);
		break;
	case 0xF3: // DI
		cpu->ime = false;
		cpu->imePending = false;
		break;
	case 0xFB: // EI
		cpu->imePending = true;
		gb->attention = true;
		break;
	case 0xD3:
	case 0xDB:
	case 0xDD:
	case 0xE3:
	case 0xE4:
	case 0xEB:
	case 0xEC:
	case 0xED:
	case 0xF4:
	case 0xFC:
	case 0xFD: // The unused opcodes lock the CPU up.
		cpu->locked = true;
		gb->attention = true;
		break;
	// LD r,r' and ALU A,r: a case each, so each knows its registers.
#define LD_R_R(op) \
	case op: \
		CPU(setR)(gb, (op) >> 3 & 7, CPU(getR)(gb, (op) & 7)); \
		break;
#define ALU_R(op) \
	case op: \
		alu(cpu, (op) >> 3 & 7, CPU(getR)(gb, (op) & 7)); \
		break;
#define EIGHT(m, op) m(op) m(op + 1) m(op + 2) m(op + 3) m(op + 4) m(op + 5) m(op + 6) m(op + 7)
	EIGHT(LD_R_R, 0x40)
	EIGHT(LD_R_R, 0x48)
	EIGHT(LD_R_R, 0x50)
	EIGHT(LD_R_R, 0x58)
	EIGHT(LD_R_R, 0x60)
	EIGHT(LD_R_R, 0x68)
	LD_R_R(0x70) LD_R_R(0x71) LD_R_R(0x72) LD_R_R(0x73) LD_R_R(0x74) LD_R_R(0x75) LD_R_R(0x77)
	EIGHT(LD_R_R, 0x78)
	EIGHT(ALU_R, 0x80)
	EIGHT(ALU_R, 0x88)
	EIGHT(ALU_R, 0x90)
	EIGHT(ALU_R, 0x98)
	EIGHT(ALU_R, 0xA0)
	EIGHT(ALU_R, 0xA8)
	EIGHT(ALU_R, 0xB0)
	EIGHT(ALU_R, 0xB8)
#undef EIGHT
#undef ALU_R
#undef LD_R_R
	}
}

// Runs instructions until the time reaches until or V-blank starts, stopping for events as they come
// due.
static void CPU(run)(struct GB* gb, uint32_t until) {
	struct GBCpu* cpu = &gb->cpu;
	while (gbSince(until, gb->now) > 0 && !gb->frameEnded) {
		gb->attention = false;
		if (gbSince(gb->now, gb->nextEvent) >= 0) {
			gbRunEvents(gb);
		}
		if (cpu->stopped || cpu->locked || cpu->stalled) {
			idle(gb, until);
			continue;
		}
		if (cpu->halted && !wake(gb, until)) {
			continue;
		}
		uint8_t pending = gb->ie & gb->ifl & 0x1F;
		if (cpu->ime && pending) {
			CPU(dispatch)(gb);
			continue;
		}
		if (cpu->imePending) {
			cpu->imePending = false;
			cpu->ime = true;
			gb->attention = true;
		}
		// After the first fetch: the halt bug (the fetch doesn't move PC), and an H-blank DMA block due.
		bool first = cpu->haltBug || gb->cgb.hdmaDue;
		// Then on without the checks above while nothing needs them and no event is due (an event put
		// sooner raises attention).
		uint32_t limit = gbSince(gb->nextEvent, until) < 0 ? gb->nextEvent : until;
		do {
			uint8_t op = CPU(fetch)(gb);
			if (__builtin_expect(first, 0)) {
				first = false;
				if (cpu->haltBug) {
					cpu->haltBug = false;
					--cpu->pc;
				}
				if (gb->cgb.hdmaDue) {
					gbHdmaRun(gb);
				}
			}
			CPU(step)(gb, op);
		} while (!gb->attention && gbSince(limit, gb->now) > 0);
	}
}
