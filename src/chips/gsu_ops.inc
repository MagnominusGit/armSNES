/* Copyright (c) 2004-2025 ares team, Near et al
 * Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Ported from ares (ISC); see LICENSES/ISC-ares.txt. */
// The GSU's instructions, made twice by gsu.c: as cases of Exact's state machine, where CALL enters
// a subroutine that can stop, and of Balanced mode's loop, where CALL is a plain call.
	// $00 stop
	case 0x00:
		if (gsu.cfgr.irq == 0) {
			SET_FLAG(SFR_IRQ, 1);
			irqRaised = true;
		}
		SET_FLAG(SFR_G, 0);
		stoppedAt = GSU_CLOCK < stoppedAt ? GSU_CLOCK : stoppedAt;
		GSU_PIPELINE = 0x01;
		regsReset();
		goto next;

	// $01 nop
	case 0x01:
		regsReset();
		goto next;

	// $02 cache
	case 0x02:
		if (gsu.cbr != (gsu.r[15] & 0xfff0)) {
			gsu.cbr = gsu.r[15] & 0xfff0;
			flushCache();
		}
		regsReset();
		goto next;

	// $03 lsr
	case 0x03:
		SET_FLAG(SFR_CY, SR() & 1);
		SET_DR(SR() >> 1);
		SET_FLAG(SFR_S, DR() & 0x8000);
		SET_FLAG(SFR_Z, DR() == 0);
		regsReset();
		goto next;

	// $04 rol
	case 0x04: {
		bool carry = SR() & 0x8000;
		SET_DR((SR() << 1) | FLAG(SFR_CY));
		SET_FLAG(SFR_S, DR() & 0x8000);
		SET_FLAG(SFR_CY, carry);
		SET_FLAG(SFR_Z, DR() == 0);
		regsReset();
		goto next;
	}

	// $05-$0f branches
	case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0a:
	case 0x0b: case 0x0c: case 0x0d: case 0x0e: case 0x0f:
		PIPE(gsu.result);
		{
			bool s = FLAG(SFR_S), ov = FLAG(SFR_OV), z = FLAG(SFR_Z), cy = FLAG(SFR_CY);
			bool take = false;
			switch (gsu.opcode) {
			case 0x05: take = 1; break;
			case 0x06: take = (s ^ ov) == 0; break;
			case 0x07: take = (s ^ ov) == 1; break;
			case 0x08: take = z == 0; break;
			case 0x09: take = z == 1; break;
			case 0x0a: take = s == 0; break;
			case 0x0b: take = s == 1; break;
			case 0x0c: take = cy == 0; break;
			case 0x0d: take = cy == 1; break;
			case 0x0e: take = ov == 0; break;
			case 0x0f: take = ov == 1; break;
			}
			if (take) {
				setR(15, gsu.r[15] + (s8) gsu.result);
			}
		}
		goto next;

	// $10-$1f to rN / move rN
	case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
	case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d: case 0x1e: case 0x1f:
		n = gsu.opcode & 15;
		if (!FLAG(SFR_B)) {
			gsu.dreg = n;
		} else {
			setR(n, SR());
			regsReset();
		}
		goto next;

	// $20-$2f with rN
	case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
	case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x2c: case 0x2d: case 0x2e: case 0x2f:
		gsu.sreg = gsu.opcode & 15;
		gsu.dreg = gsu.opcode & 15;
		SET_FLAG(SFR_B, 1);
		goto next;

	// $30-$3b stw / stb (rN)
	case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35:
	case 0x36: case 0x37: case 0x38: case 0x39: case 0x3a: case 0x3b:
		gsu.ramaddr = gsu.r[gsu.opcode & 15];
		WRITE_RAM_BUFFER(gsu.ramaddr, SR());
		if (!FLAG(SFR_ALT1)) {
			WRITE_RAM_BUFFER(gsu.ramaddr ^ 1, SR() >> 8);
		}
		regsReset();
		goto next;

	// $3c loop
	case 0x3c:
		setR(12, gsu.r[12] - 1);
		SET_FLAG(SFR_S, gsu.r[12] & 0x8000);
		SET_FLAG(SFR_Z, gsu.r[12] == 0);
		if (!FLAG(SFR_Z)) {
			setR(15, gsu.r[13]);
		}
		regsReset();
		goto next;

	// $3d-$3f alt1, alt2, alt3
	case 0x3d:
		SET_FLAG(SFR_B, 0);
		SET_FLAG(SFR_ALT1, 1);
		goto next;
	case 0x3e:
		SET_FLAG(SFR_B, 0);
		SET_FLAG(SFR_ALT2, 1);
		goto next;
	case 0x3f:
		SET_FLAG(SFR_B, 0);
		SET_FLAG(SFR_ALT1, 1);
		SET_FLAG(SFR_ALT2, 1);
		goto next;

	// $40-$4b ldw / ldb (rN)
	case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45:
	case 0x46: case 0x47: case 0x48: case 0x49: case 0x4a: case 0x4b:
		gsu.ramaddr = gsu.r[gsu.opcode & 15];
		READ_RAM_BUFFER(gsu.result, gsu.ramaddr);
		SET_DR((u8) gsu.result);
		if (!FLAG(SFR_ALT1)) {
			READ_RAM_BUFFER(gsu.result, gsu.ramaddr ^ 1);
			SET_DR(DR() | (u8) gsu.result << 8);
		}
		regsReset();
		goto next;

	// $4c plot / rpix
	case 0x4c:
		if (!FLAG(SFR_ALT1)) {
			CALL(SUB_PLOT, gsu.r[1], gsu.r[2], 0);
			setR(1, gsu.r[1] + 1);
		} else {
			CALL(SUB_RPIX, gsu.r[1], gsu.r[2], 0);
			SET_DR(gsu.result);
			SET_FLAG(SFR_S, DR() & 0x8000);
			SET_FLAG(SFR_Z, DR() == 0);
		}
		regsReset();
		goto next;

	// $4d swap
	case 0x4d:
		SET_DR(SR() >> 8 | SR() << 8);
		SET_FLAG(SFR_S, DR() & 0x8000);
		SET_FLAG(SFR_Z, DR() == 0);
		regsReset();
		goto next;

	// $4e color / cmode
	case 0x4e:
		if (!FLAG(SFR_ALT1)) {
			gsu.colr = color(SR());
		} else {
			setPOR(SR());
		}
		regsReset();
		goto next;

	// $4f not
	case 0x4f:
		SET_DR(~SR());
		SET_FLAG(SFR_S, DR() & 0x8000);
		SET_FLAG(SFR_Z, DR() == 0);
		regsReset();
		goto next;

	// $50-$5f add / adc / add # / adc #
	case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
	case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f: {
		n = gsu.opcode & 15;
		if (!FLAG(SFR_ALT2)) {
			n = gsu.r[n];
		}
		s32 r = SR() + n + (FLAG(SFR_ALT1) ? FLAG(SFR_CY) : 0);
		SET_FLAG(SFR_OV, ~(SR() ^ n) & (n ^ r) & 0x8000);
		SET_FLAG(SFR_S, r & 0x8000);
		SET_FLAG(SFR_CY, r >= 0x10000);
		SET_FLAG(SFR_Z, (u16) r == 0);
		SET_DR(r);
		regsReset();
		goto next;
	}

	// $60-$6f sub / sbc / sub # / cmp
	case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:
	case 0x68: case 0x69: case 0x6a: case 0x6b: case 0x6c: case 0x6d: case 0x6e: case 0x6f: {
		n = gsu.opcode & 15;
		if (!FLAG(SFR_ALT2) || FLAG(SFR_ALT1)) {
			n = gsu.r[n];
		}
		s32 r = SR() - n - (!FLAG(SFR_ALT2) && FLAG(SFR_ALT1) ? !FLAG(SFR_CY) : 0);
		SET_FLAG(SFR_OV, (SR() ^ n) & (SR() ^ r) & 0x8000);
		SET_FLAG(SFR_S, r & 0x8000);
		SET_FLAG(SFR_CY, r >= 0);
		SET_FLAG(SFR_Z, (u16) r == 0);
		if (!FLAG(SFR_ALT2) || !FLAG(SFR_ALT1)) {
			SET_DR(r);
		}
		regsReset();
		goto next;
	}

	// $70 merge
	case 0x70:
		SET_DR((gsu.r[7] & 0xff00) | (gsu.r[8] >> 8));
		SET_FLAG(SFR_OV, DR() & 0xc0c0);
		SET_FLAG(SFR_S, DR() & 0x8080);
		SET_FLAG(SFR_CY, DR() & 0xe0e0);
		SET_FLAG(SFR_Z, DR() & 0xf0f0);
		regsReset();
		goto next;

	// $71-$7f and / bic / and # / bic #
	case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78:
	case 0x79: case 0x7a: case 0x7b: case 0x7c: case 0x7d: case 0x7e: case 0x7f:
		n = gsu.opcode & 15;
		if (!FLAG(SFR_ALT2)) {
			n = gsu.r[n];
		}
		SET_DR(SR() & (FLAG(SFR_ALT1) ? ~n : n));
		SET_FLAG(SFR_S, DR() & 0x8000);
		SET_FLAG(SFR_Z, DR() == 0);
		regsReset();
		goto next;

	// $80-$8f mult / umult / mult # / umult #
	case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
	case 0x88: case 0x89: case 0x8a: case 0x8b: case 0x8c: case 0x8d: case 0x8e: case 0x8f:
		n = gsu.opcode & 15;
		if (!FLAG(SFR_ALT2)) {
			n = gsu.r[n];
		}
		SET_DR(!FLAG(SFR_ALT1) ? (u16) ((s8) SR() * (s8) n) : (u16) ((u8) SR() * (u8) n));
		SET_FLAG(SFR_S, DR() & 0x8000);
		SET_FLAG(SFR_Z, DR() == 0);
		regsReset();
		if (!gsu.cfgr.ms0) {
			STEP(gsu.clsr ? 1 : 2);
		}
		goto next;

	// $90 sbk
	case 0x90:
		WRITE_RAM_BUFFER(gsu.ramaddr ^ 0, SR() >> 0);
		WRITE_RAM_BUFFER(gsu.ramaddr ^ 1, SR() >> 8);
		regsReset();
		goto next;

	// $91-$94 link #N
	case 0x91: case 0x92: case 0x93: case 0x94:
		setR(11, gsu.r[15] + (gsu.opcode & 15));
		regsReset();
		goto next;

	// $95 sex
	case 0x95:
		SET_DR((s8) SR());
		SET_FLAG(SFR_S, DR() & 0x8000);
		SET_FLAG(SFR_Z, DR() == 0);
		regsReset();
		goto next;

	// $96 asr / div2
	case 0x96:
		SET_FLAG(SFR_CY, SR() & 1);
		SET_DR(((s16) SR() >> 1) + (FLAG(SFR_ALT1) ? ((SR() + 1) >> 16) : 0));
		SET_FLAG(SFR_S, DR() & 0x8000);
		SET_FLAG(SFR_Z, DR() == 0);
		regsReset();
		goto next;

	// $97 ror
	case 0x97: {
		bool carry = SR() & 1;
		SET_DR((FLAG(SFR_CY) << 15) | (SR() >> 1));
		SET_FLAG(SFR_S, DR() & 0x8000);
		SET_FLAG(SFR_CY, carry);
		SET_FLAG(SFR_Z, DR() == 0);
		regsReset();
		goto next;
	}

	// $98-$9d jmp / ljmp rN
	case 0x98: case 0x99: case 0x9a: case 0x9b: case 0x9c: case 0x9d:
		n = gsu.opcode & 15;
		if (!FLAG(SFR_ALT1)) {
			setR(15, gsu.r[n]);
		} else {
			gsu.pbr = gsu.r[n] & 0x7f;
			setR(15, SR());
			gsu.cbr = gsu.r[15] & 0xfff0;
			flushCache();
		}
		regsReset();
		goto next;

	// $9e lob
	case 0x9e:
		SET_DR(SR() & 0xff);
		SET_FLAG(SFR_S, DR() & 0x80);
		SET_FLAG(SFR_Z, DR() == 0);
		regsReset();
		goto next;

	// $9f fmult / lmult
	case 0x9f: {
		u32 result = (s16) SR() * (s16) gsu.r[6];
		if (FLAG(SFR_ALT1)) {
			setR(4, result);
		}
		SET_DR(result >> 16);
		SET_FLAG(SFR_S, DR() & 0x8000);
		SET_FLAG(SFR_CY, result & 0x8000);
		SET_FLAG(SFR_Z, DR() == 0);
		regsReset();
		STEP((gsu.cfgr.ms0 ? 3 : 7) * (gsu.clsr ? 1 : 2));
		goto next;
	}

	// $a0-$af ibt rN,#pp / lms rN,(yy) / sms (yy),rN
	case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xa4: case 0xa5: case 0xa6: case 0xa7:
	case 0xa8: case 0xa9: case 0xaa: case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf:
		if (FLAG(SFR_ALT1)) {
			PIPE(gsu.result);
			gsu.ramaddr = gsu.result << 1;
			READ_RAM_BUFFER(gsu.result, gsu.ramaddr ^ 0);
			gsu.lo = gsu.result;
			READ_RAM_BUFFER(gsu.result, gsu.ramaddr ^ 1);
			setR(gsu.opcode & 15, (u8) gsu.result << 8 | gsu.lo);
		} else if (FLAG(SFR_ALT2)) {
			PIPE(gsu.result);
			gsu.ramaddr = gsu.result << 1;
			WRITE_RAM_BUFFER(gsu.ramaddr ^ 0, gsu.r[gsu.opcode & 15] >> 0);
			WRITE_RAM_BUFFER(gsu.ramaddr ^ 1, gsu.r[gsu.opcode & 15] >> 8);
		} else {
			PIPE(gsu.result);
			setR(gsu.opcode & 15, (s8) gsu.result);
		}
		regsReset();
		goto next;

	// $b0-$bf from rN / moves rN
	case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7:
	case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf:
		n = gsu.opcode & 15;
		if (!FLAG(SFR_B)) {
			gsu.sreg = n;
		} else {
			SET_DR(gsu.r[n]);
			SET_FLAG(SFR_OV, DR() & 0x80);
			SET_FLAG(SFR_S, DR() & 0x8000);
			SET_FLAG(SFR_Z, DR() == 0);
			regsReset();
		}
		goto next;

	// $c0 hib
	case 0xc0:
		SET_DR(SR() >> 8);
		SET_FLAG(SFR_S, DR() & 0x80);
		SET_FLAG(SFR_Z, DR() == 0);
		regsReset();
		goto next;

	// $c1-$cf or / xor / or # / xor #
	case 0xc1: case 0xc2: case 0xc3: case 0xc4: case 0xc5: case 0xc6: case 0xc7: case 0xc8:
	case 0xc9: case 0xca: case 0xcb: case 0xcc: case 0xcd: case 0xce: case 0xcf:
		n = gsu.opcode & 15;
		if (!FLAG(SFR_ALT2)) {
			n = gsu.r[n];
		}
		SET_DR(!FLAG(SFR_ALT1) ? (SR() | n) : (SR() ^ n));
		SET_FLAG(SFR_S, DR() & 0x8000);
		SET_FLAG(SFR_Z, DR() == 0);
		regsReset();
		goto next;

	// $d0-$de inc rN
	case 0xd0: case 0xd1: case 0xd2: case 0xd3: case 0xd4: case 0xd5: case 0xd6: case 0xd7:
	case 0xd8: case 0xd9: case 0xda: case 0xdb: case 0xdc: case 0xdd: case 0xde:
		n = gsu.opcode & 15;
		setR(n, gsu.r[n] + 1);
		SET_FLAG(SFR_S, gsu.r[n] & 0x8000);
		SET_FLAG(SFR_Z, gsu.r[n] == 0);
		regsReset();
		goto next;

	// $df getc / ramb / romb
	case 0xdf:
		if (!FLAG(SFR_ALT2)) {
			READ_ROM_BUFFER(gsu.result);
			gsu.colr = color(gsu.result);
		} else if (!FLAG(SFR_ALT1)) {
			SYNC_RAM_BUFFER();
			gsu.rambr = SR() & 0x01;
		} else {
			SYNC_ROM_BUFFER();
			gsu.rombr = SR() & 0x7f;
		}
		regsReset();
		goto next;

	// $e0-$ee dec rN
	case 0xe0: case 0xe1: case 0xe2: case 0xe3: case 0xe4: case 0xe5: case 0xe6: case 0xe7:
	case 0xe8: case 0xe9: case 0xea: case 0xeb: case 0xec: case 0xed: case 0xee:
		n = gsu.opcode & 15;
		setR(n, gsu.r[n] - 1);
		SET_FLAG(SFR_S, gsu.r[n] & 0x8000);
		SET_FLAG(SFR_Z, gsu.r[n] == 0);
		regsReset();
		goto next;

	// $ef getb / getbh / getbl / getbs
	case 0xef:
		READ_ROM_BUFFER(gsu.result);
		switch (FLAG(SFR_ALT2) << 1 | FLAG(SFR_ALT1)) {
		case 0: SET_DR(gsu.result); break;
		case 1: SET_DR(gsu.result << 8 | (u8) SR()); break;
		case 2: SET_DR((SR() & 0xff00) | gsu.result); break;
		case 3: SET_DR((s8) gsu.result); break;
		}
		regsReset();
		goto next;

	// $f0-$ff iwt rN,#xx / lm rN,(xx) / sm (xx),rN
	case 0xf0: case 0xf1: case 0xf2: case 0xf3: case 0xf4: case 0xf5: case 0xf6: case 0xf7:
	case 0xf8: case 0xf9: case 0xfa: case 0xfb: case 0xfc: case 0xfd: case 0xfe: case 0xff:
		if (FLAG(SFR_ALT1)) {
			PIPE(gsu.result);
			gsu.ramaddr = gsu.result;
			PIPE(gsu.result);
			gsu.ramaddr |= gsu.result << 8;
			READ_RAM_BUFFER(gsu.result, gsu.ramaddr ^ 0);
			gsu.lo = gsu.result;
			READ_RAM_BUFFER(gsu.result, gsu.ramaddr ^ 1);
			setR(gsu.opcode & 15, (u8) gsu.result << 8 | gsu.lo);
		} else if (FLAG(SFR_ALT2)) {
			PIPE(gsu.result);
			gsu.ramaddr = gsu.result;
			PIPE(gsu.result);
			gsu.ramaddr |= gsu.result << 8;
			WRITE_RAM_BUFFER(gsu.ramaddr ^ 0, gsu.r[gsu.opcode & 15] >> 0);
			WRITE_RAM_BUFFER(gsu.ramaddr ^ 1, gsu.r[gsu.opcode & 15] >> 8);
		} else {
			PIPE(gsu.result);
			gsu.lo = gsu.result;
			PIPE(gsu.result);
			setR(gsu.opcode & 15, (u8) gsu.result << 8 | gsu.lo);
		}
		regsReset();
		goto next;
