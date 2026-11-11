/* Copyright (c) 2004-2025 ares team, Near et al
 * Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Ported from ares (ISC); see LICENSES/ISC-ares.txt. */
#include "snes.h"

// The S-DSP: eight BRR voices, noise, echo. A sample takes 32 steps of 24 clocks, and the DSP stops
// after any step that puts its clock past the SMP's.

struct Dsp dsp;

enum { RELEASE, ATTACK, DECAY, SUSTAIN };

// The interpolation table, as ares builds it at power on.
static const s16 gaussianTable[512] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2,
	2, 2, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 5, 5, 5, 5,
	6, 6, 6, 6, 7, 7, 7, 8, 8, 8, 9, 9, 9, 10, 10, 10,
	11, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 15, 16, 16, 17, 17,
	18, 19, 19, 20, 20, 21, 21, 22, 23, 23, 24, 24, 25, 26, 27, 27,
	28, 29, 29, 30, 31, 32, 32, 33, 34, 35, 36, 36, 37, 38, 39, 40,
	41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56,
	58, 59, 60, 61, 62, 64, 65, 66, 67, 69, 70, 71, 73, 74, 76, 77,
	78, 80, 81, 83, 84, 86, 87, 89, 90, 92, 94, 95, 97, 99, 100, 102,
	104, 106, 107, 109, 111, 113, 115, 117, 118, 120, 122, 124, 126, 128, 130, 132,
	134, 137, 139, 141, 143, 145, 147, 150, 152, 154, 156, 159, 161, 163, 166, 168,
	171, 173, 175, 178, 180, 183, 186, 188, 191, 193, 196, 199, 201, 204, 207, 210,
	212, 215, 218, 221, 224, 227, 230, 233, 236, 239, 242, 245, 248, 251, 254, 257,
	260, 263, 267, 270, 273, 276, 280, 283, 286, 290, 293, 297, 300, 304, 307, 311,
	314, 318, 321, 325, 328, 332, 336, 339, 343, 347, 351, 354, 358, 362, 366, 370,
	374, 378, 381, 385, 389, 393, 397, 401, 405, 410, 414, 418, 422, 426, 430, 434,
	439, 443, 447, 451, 456, 460, 464, 469, 473, 477, 482, 486, 491, 495, 499, 504,
	508, 513, 517, 522, 527, 531, 536, 540, 545, 550, 554, 559, 563, 568, 573, 577,
	582, 587, 592, 596, 601, 606, 611, 615, 620, 625, 630, 635, 640, 644, 649, 654,
	659, 664, 669, 674, 678, 683, 688, 693, 698, 703, 708, 713, 718, 723, 728, 732,
	737, 742, 747, 752, 757, 762, 767, 772, 777, 782, 787, 792, 797, 802, 806, 811,
	816, 821, 826, 831, 836, 841, 846, 851, 855, 860, 865, 870, 875, 880, 884, 889,
	894, 899, 904, 908, 913, 918, 923, 927, 932, 937, 941, 946, 951, 955, 960, 965,
	969, 974, 978, 983, 988, 992, 997, 1001, 1005, 1010, 1014, 1019, 1023, 1027, 1032, 1036,
	1040, 1045, 1049, 1053, 1057, 1061, 1066, 1070, 1074, 1078, 1082, 1086, 1090, 1094, 1098, 1102,
	1106, 1109, 1113, 1117, 1121, 1125, 1128, 1132, 1136, 1139, 1143, 1146, 1150, 1153, 1157, 1160,
	1164, 1167, 1170, 1174, 1177, 1180, 1183, 1186, 1190, 1193, 1196, 1199, 1202, 1205, 1207, 1210,
	1213, 1216, 1219, 1221, 1224, 1227, 1229, 1232, 1234, 1237, 1239, 1241, 1244, 1246, 1248, 1251,
	1253, 1255, 1257, 1259, 1261, 1263, 1265, 1267, 1269, 1270, 1272, 1274, 1275, 1277, 1279, 1280,
	1282, 1283, 1284, 1286, 1287, 1288, 1290, 1291, 1292, 1293, 1294, 1295, 1296, 1297, 1297, 1298,
	1299, 1300, 1300, 1301, 1302, 1302, 1303, 1303, 1303, 1304, 1304, 1304, 1304, 1304, 1305, 1305,
};

// Samples per counter event; rate 0 never triggers.
static const u16 counterRate[32] = {
	0, 2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256, 192, 160, 128, 96, 80,
	64, 48, 40, 32, 24, 20, 16, 12, 10, 8, 6, 5, 4, 3, 2, 1,
};
// The counters aren't aligned at zero for every rate.
static const u16 counterOffset[32] = {
	0, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536,
	0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 0, 0,
};

static void counterTick(void) {
	if (!dsp.clock.counter) {
		dsp.clock.counter = 2048 * 5 * 3;
	}
	dsp.clock.counter--;
}

static bool counterPoll(u32 rate) {
	if (rate == 0) {
		return false;
	}
	return (dsp.clock.counter + counterOffset[rate]) % counterRate[rate] == 0;
}

u8 dspRead(u8 address) {
	return dsp.registers[address & 0x7f];
}

void dspWrite(u8 address, u8 data) {
	address &= 0x7f;
	dsp.registers[address] = data;

	switch (address) {
	case 0x0c:
		dsp.mainvol.volume[0] = data;
		break;
	case 0x1c:
		dsp.mainvol.volume[1] = data;
		break;
	case 0x2c:
		dsp.echo.volume[0] = data;
		break;
	case 0x3c:
		dsp.echo.volume[1] = data;
		break;
	case 0x4c:
		for (int n = 0; n < 8; ++n) {
			dsp.voice[n].keyon = data >> n & 1;
			dsp.voice[n]._keylatch = data >> n & 1;
		}
		break;
	case 0x5c:
		for (int n = 0; n < 8; ++n) {
			dsp.voice[n].keyoff = data >> n & 1;
		}
		break;
	case 0x6c:
		dsp.noise.frequency = data & 0x1f;
		dsp.echo.readonly = data & 0x20;
		dsp.mainvol.mute = data & 0x40;
		dsp.mainvol.reset = data & 0x80;
		break;
	case 0x7c:
		for (int n = 0; n < 8; ++n) {
			dsp.voice[n]._end = 0;
		}
		// Cleared whatever is written.
		dsp.registers[0x7c] = 0;
		break;
	case 0x0d:
		dsp.echo.feedback = data;
		break;
	case 0x2d:
		for (int n = 0; n < 8; ++n) {
			dsp.voice[n].modulate = data >> n & 1;
		}
		// Voice 0 has no voice before it to be modulated by.
		dsp.voice[0].modulate = 0;
		break;
	case 0x3d:
		for (int n = 0; n < 8; ++n) {
			dsp.voice[n].noise = data >> n & 1;
		}
		break;
	case 0x4d:
		for (int n = 0; n < 8; ++n) {
			dsp.voice[n].echo = data >> n & 1;
		}
		break;
	case 0x5d:
		dsp.brr.bank = data;
		break;
	case 0x6d:
		dsp.echo.page = data;
		break;
	case 0x7d:
		dsp.echo.delay = data & 15;
		break;
	}

	struct DspVoice* v = &dsp.voice[address >> 4 & 7];
	switch (address & 15) {
	case 0x00:
		v->volume[0] = data;
		break;
	case 0x01:
		v->volume[1] = data;
		break;
	case 0x02:
		v->pitch = (v->pitch & 0x3f00) | data;
		break;
	case 0x03:
		v->pitch = (v->pitch & 0x00ff) | (data & 0x3f) << 8;
		break;
	case 0x04:
		v->source = data;
		break;
	case 0x05:
		v->adsr0 = data;
		break;
	case 0x06:
		v->adsr1 = data;
		break;
	case 0x07:
		v->gain = data;
		break;
	case 0x08:
		dsp.latch.envx = data;
		break;
	case 0x09:
		dsp.latch.outx = data;
		break;
	case 0x0f:
		dsp.echo.fir[address >> 4 & 7] = data;
		break;
	}
}

static void envelopeRun(struct DspVoice* v) {
	s32 envelope = v->envelope;

	if (v->envelopeMode == RELEASE) {
		envelope -= 0x8;
		if (envelope < 0) {
			envelope = 0;
		}
		v->envelope = envelope;
		return;
	}

	s32 rate;
	s32 envelopeData = v->adsr1;
	if (dsp.latch.adsr0 & 0x80) {
		if (v->envelopeMode >= DECAY) {
			envelope--;
			envelope -= envelope >> 8;
			rate = envelopeData & 0x1f;
			if (v->envelopeMode == DECAY) {
				rate = (dsp.latch.adsr0 >> 4 & 7) * 2 + 16;
			}
		} else {
			rate = (dsp.latch.adsr0 & 15) * 2 + 1;
			envelope += rate < 31 ? 0x20 : 0x400;
		}
	} else {
		envelopeData = v->gain;
		s32 mode = envelopeData >> 5;
		if (mode < 4) {
			envelope = envelopeData << 4;
			rate = 31;
		} else {
			rate = envelopeData & 0x1f;
			if (mode == 4) {
				envelope -= 0x20;
			} else if (mode < 6) {
				envelope--;
				envelope -= envelope >> 8;
			} else {
				envelope += 0x20;
				if (mode > 6 && (u32) v->_envelope >= 0x600) {
					// Mode 7 rises on two slopes.
					envelope += 0x8 - 0x20;
				}
			}
		}
	}

	if ((envelope >> 8) == (envelopeData >> 5) && v->envelopeMode == DECAY) {
		v->envelopeMode = SUSTAIN;
	}
	v->_envelope = envelope;

	// Unsigned, because a linear decrease going under zero ends up here too.
	if ((u32) envelope > 0x7ff) {
		envelope = (envelope < 0 ? 0 : 0x7ff);
		if (v->envelopeMode == ATTACK) {
			v->envelopeMode = DECAY;
		}
	}

	if (counterPoll(rate)) {
		v->envelope = envelope;
	}
}

static s32 gaussianInterpolate(const struct DspVoice* v) {
	u8 offset = v->gaussianOffset >> 4;
	const s16* forward = gaussianTable + 255 - offset;
	const s16* reverse = gaussianTable + offset;

	offset = (v->bufferOffset + (v->gaussianOffset >> 12)) % 12;
	s32 output;
	output = forward[0] * v->buffer[offset] >> 11;
	if (++offset >= 12) {
		offset = 0;
	}
	output += forward[256] * v->buffer[offset] >> 11;
	if (++offset >= 12) {
		offset = 0;
	}
	output += reverse[256] * v->buffer[offset] >> 11;
	if (++offset >= 12) {
		offset = 0;
	}
	output = (s16) output;
	output += reverse[0] * v->buffer[offset] >> 11;
	return sclamp16(output) & ~1;
}

static void brrDecode(struct DspVoice* v) {
	// brr._byte was read on the clock before.
	s32 nybbles = dsp.brr._byte << 8 | dsp.apuram[(u16) (v->brrAddress + v->brrOffset + 1)];

	const s32 filter = dsp.brr._header >> 2 & 3;
	const s32 scale = dsp.brr._header >> 4 & 15;

	for (int n = 0; n < 4; ++n) {
		s32 s = (s16) nybbles >> 12;
		nybbles <<= 4;

		if (scale <= 12) {
			s <<= scale;
			s >>= 1;
		} else {
			s &= ~0x7ff;
		}

		s32 offset = v->bufferOffset;
		if (--offset < 0) {
			offset = 11;
		}
		const s32 p1 = v->buffer[offset];
		if (--offset < 0) {
			offset = 11;
		}
		const s32 p2 = v->buffer[offset] >> 1;

		switch (filter) {
		case 0:
			break;
		case 1:
			s += p1 >> 1;
			s += (-p1) >> 5;
			break;
		case 2:
			s += p1;
			s -= p2;
			s += p2 >> 4;
			s += (p1 * -3) >> 6;
			break;
		case 3:
			s += p1;
			s -= p2;
			s += (p1 * -13) >> 7;
			s += (p2 * 3) >> 4;
			break;
		}

		s = sclamp16(s);
		s = (s16) (s << 1);
		v->buffer[v->bufferOffset] = s;
		if (++v->bufferOffset >= 12) {
			v->bufferOffset = 0;
		}
	}
}

// Echo

static s32 calculateFIR(int channel, s32 index) {
	s32 sample = dsp.echo.history[channel][(dsp.echo._historyOffset + index + 1) & 7];
	return (sample * dsp.echo.fir[index]) >> 6;
}

static s16 echoOutput(int channel) {
	s16 mainvolOutput = dsp.mainvol.output[channel] * dsp.mainvol.volume[channel] >> 7;
	s16 echoOut = dsp.echo.input[channel] * dsp.echo.volume[channel] >> 7;
	return sclamp16(mainvolOutput + echoOut);
}

static void echoRead(int channel) {
	u16 address = dsp.echo._address + channel * 2;
	u8 lo = dsp.apuram[address++];
	u8 hi = dsp.apuram[address++];
	s32 s = (s16) ((hi << 8) + lo);
	dsp.echo.history[channel][dsp.echo._historyOffset] = s >> 1;
}

static void echoWrite(int channel) {
	if (!dsp.echo._readonly) {
		u16 address = dsp.echo._address + channel * 2;
		s32 sample = dsp.echo.output[channel];
		dsp.apuram[address++] = sample;
		dsp.apuram[address++] = sample >> 8;
	}
	dsp.echo.output[channel] = 0;
}

static void echo22(void) {
	dsp.echo._historyOffset = (dsp.echo._historyOffset + 1) & 7;
	dsp.echo._address = (dsp.echo._page << 8) + dsp.echo._offset;
	echoRead(0);

	s32 l = calculateFIR(0, 0);
	s32 r = calculateFIR(1, 0);
	dsp.echo.input[0] = signedBits(l, 17);
	dsp.echo.input[1] = signedBits(r, 17);
}

static void echo23(void) {
	s32 l = calculateFIR(0, 1) + calculateFIR(0, 2);
	s32 r = calculateFIR(1, 1) + calculateFIR(1, 2);
	dsp.echo.input[0] = signedBits(dsp.echo.input[0] + l, 17);
	dsp.echo.input[1] = signedBits(dsp.echo.input[1] + r, 17);
	echoRead(1);
}

static void echo24(void) {
	s32 l = calculateFIR(0, 3) + calculateFIR(0, 4) + calculateFIR(0, 5);
	s32 r = calculateFIR(1, 3) + calculateFIR(1, 4) + calculateFIR(1, 5);
	dsp.echo.input[0] = signedBits(dsp.echo.input[0] + l, 17);
	dsp.echo.input[1] = signedBits(dsp.echo.input[1] + r, 17);
}

static void echo25(void) {
	s32 l = dsp.echo.input[0] + calculateFIR(0, 6);
	s32 r = dsp.echo.input[1] + calculateFIR(1, 6);
	l = (s16) l;
	r = (s16) r;
	l += (s16) calculateFIR(0, 7);
	r += (s16) calculateFIR(1, 7);
	dsp.echo.input[0] = sclamp16(l) & ~1;
	dsp.echo.input[1] = sclamp16(r) & ~1;
}

static void echo26(void) {
	// The left output is kept for the next clock, so both go out together.
	dsp.mainvol.output[0] = echoOutput(0);

	s32 l = dsp.echo.output[0] + (s16) (dsp.echo.input[0] * dsp.echo.feedback >> 7);
	s32 r = dsp.echo.output[1] + (s16) (dsp.echo.input[1] * dsp.echo.feedback >> 7);
	dsp.echo.output[0] = sclamp16(l) & ~1;
	dsp.echo.output[1] = sclamp16(r) & ~1;
}

static void echo27(void) {
	s32 outl = dsp.mainvol.output[0];
	s32 outr = echoOutput(1);
	dsp.mainvol.output[0] = 0;
	dsp.mainvol.output[1] = 0;

	if (dsp.mainvol.mute) {
		outl = 0;
		outr = 0;
	}
	audioSample(outl, outr);
}

static void echo28(void) {
	dsp.echo._readonly = dsp.echo.readonly;
}

static void echo29(void) {
	dsp.echo._page = dsp.echo.page;

	if (!dsp.echo._offset) {
		dsp.echo._length = dsp.echo.delay << 11;
	}
	dsp.echo._offset += 4;
	if (dsp.echo._offset >= dsp.echo._length) {
		dsp.echo._offset = 0;
	}

	echoWrite(0);
	dsp.echo._readonly = dsp.echo.readonly;
}

static void echo30(void) {
	echoWrite(1);
}

// Voices

static void voiceOutput(struct DspVoice* v, int channel) {
	s32 amp = dsp.latch.output * v->volume[channel] >> 7;

	dsp.mainvol.output[channel] = signedBits(dsp.mainvol.output[channel] + amp, 17);
	dsp.mainvol.output[channel] = sclamp16(dsp.mainvol.output[channel]);

	if (v->_echo) {
		dsp.echo.output[channel] = signedBits(dsp.echo.output[channel] + amp, 17);
		dsp.echo.output[channel] = sclamp16(dsp.echo.output[channel]);
	}
}

static void voice1(struct DspVoice* v) {
	dsp.brr._address = (dsp.brr._bank << 8) + (dsp.brr._source << 2);
	dsp.brr._source = v->source;
}

static void voice2(struct DspVoice* v) {
	// The sample pointer, whether or not it's needed.
	u16 address = dsp.brr._address;
	if (!v->keyonDelay) {
		address += 2;
	}
	u8 lo = dsp.apuram[address++];
	u8 hi = dsp.apuram[address++];
	dsp.brr._nextAddress = lo | hi << 8;

	dsp.latch.adsr0 = v->adsr0;

	// The pitch is read over two clocks.
	dsp.latch.pitch = v->pitch & 0xff;
}

static void voice3a(struct DspVoice* v) {
	dsp.latch.pitch |= v->pitch & ~0xff;
}

static void voice3b(struct DspVoice* v) {
	dsp.brr._byte = dsp.apuram[(u16) (v->brrAddress + v->brrOffset)];
	dsp.brr._header = dsp.apuram[v->brrAddress];
}

static void voice3c(struct DspVoice* v) {
	// Pitch modulation by the voice before.
	if (v->_modulate) {
		dsp.latch.pitch = (dsp.latch.pitch + ((dsp.latch.output >> 5) * dsp.latch.pitch >> 10)) & 0x7fff;
	}

	if (v->keyonDelay) {
		// BRR decoding starts on the next sample.
		if (v->keyonDelay == 5) {
			v->brrAddress = dsp.brr._nextAddress;
			v->brrOffset = 1;
			v->bufferOffset = 0;
			dsp.brr._header = 0;
		}

		// The envelope never runs during key on.
		v->envelope = 0;
		v->_envelope = 0;

		// No BRR decoding until the last three samples.
		v->gaussianOffset = 0;
		v->keyonDelay--;
		if (v->keyonDelay & 3) {
			v->gaussianOffset = 0x4000;
		}

		// The pitch is never added during key on.
		dsp.latch.pitch = 0;
	}

	s32 output = gaussianInterpolate(v);

	if (v->_noise) {
		output = (s16) (dsp.noise.lfsr << 1);
	}

	dsp.latch.output = (output * v->envelope >> 11) & ~1;
	v->envx = v->envelope >> 4;

	// Silence at once at the end of a sample or on soft reset.
	if (dsp.mainvol.reset || (dsp.brr._header & 3) == 1) {
		v->envelopeMode = RELEASE;
		v->envelope = 0;
	}

	if (dsp.clock.sample) {
		if (v->_keyoff) {
			v->envelopeMode = RELEASE;
		}
		if (v->_keyon) {
			v->keyonDelay = 5;
			v->envelopeMode = ATTACK;
		}
	}

	// The envelope for the next sample.
	if (!v->keyonDelay) {
		envelopeRun(v);
	}
}

static void voice3(struct DspVoice* v) {
	voice3a(v);
	voice3b(v);
	voice3c(v);
}

static void voice4(struct DspVoice* v) {
	v->_looped = 0;
	if (v->gaussianOffset >= 0x4000) {
		brrDecode(v);
		v->brrOffset = (v->brrOffset + 2) & 15;
		if (v->brrOffset >= 9) {
			// The next BRR block.
			v->brrAddress = (u16) (v->brrAddress + 9);
			if (dsp.brr._header & 1) {
				v->brrAddress = dsp.brr._nextAddress;
				v->_looped = 1;
			}
			v->brrOffset = 1;
		}
	}

	v->gaussianOffset = (v->gaussianOffset & 0x3fff) + dsp.latch.pitch;

	// Pitch modulation mustn't get too far ahead.
	if (v->gaussianOffset > 0x7fff) {
		v->gaussianOffset = 0x7fff;
	}

	voiceOutput(v, 0);
}

static void voice5(struct DspVoice* v) {
	voiceOutput(v, 1);

	// ENDX, OUTX and ENVX don't update if they were written one or two clocks earlier.
	v->_end |= v->_looped;

	if (v->keyonDelay == 5) {
		v->_end = 0;
	}
}

static void voice6(struct DspVoice* v) {
	(void) v;
	dsp.latch.outx = dsp.latch.output >> 8;
}

static void voice7(struct DspVoice* v) {
	u8 endx = 0;
	for (int n = 0; n < 8; ++n) {
		endx |= dsp.voice[n]._end << n;
	}
	dsp.registers[0x7c] = endx;
	dsp.latch.envx = v->envx;
}

static void voice8(struct DspVoice* v) {
	dsp.registers[v->index | 0x09] = dsp.latch.outx;
}

static void voice9(struct DspVoice* v) {
	dsp.registers[v->index | 0x08] = dsp.latch.envx;
}

static void misc27(void) {
	for (int n = 0; n < 8; ++n) {
		dsp.voice[n]._modulate = dsp.voice[n].modulate;
	}
}

static void misc28(void) {
	for (int n = 0; n < 8; ++n) {
		dsp.voice[n]._noise = dsp.voice[n].noise;
		dsp.voice[n]._echo = dsp.voice[n].echo;
	}
	dsp.brr._bank = dsp.brr.bank;
}

static void misc29(void) {
	dsp.clock.sample = !dsp.clock.sample;
	if (dsp.clock.sample) {
		// KON is cleared 63 clocks after it was last read.
		for (int n = 0; n < 8; ++n) {
			dsp.voice[n]._keylatch &= !dsp.voice[n]._keyon;
		}
	}
}

static void misc30(void) {
	if (dsp.clock.sample) {
		for (int n = 0; n < 8; ++n) {
			dsp.voice[n]._keyon = dsp.voice[n]._keylatch;
			dsp.voice[n]._keyoff = dsp.voice[n].keyoff;
		}
	}

	counterTick();

	if (counterPoll(dsp.noise.frequency)) {
		s32 feedback = dsp.noise.lfsr << 13 ^ dsp.noise.lfsr << 14;
		dsp.noise.lfsr = (feedback & 0x4000) | dsp.noise.lfsr >> 1;
	}
}

static void step(int phase) {
	struct DspVoice* voice = dsp.voice;
	switch (phase) {
	case 0: voice5(&voice[0]); voice2(&voice[1]); break;
	case 1: voice6(&voice[0]); voice3(&voice[1]); break;
	case 2: voice7(&voice[0]); voice4(&voice[1]); voice1(&voice[3]); break;
	case 3: voice8(&voice[0]); voice5(&voice[1]); voice2(&voice[2]); break;
	case 4: voice9(&voice[0]); voice6(&voice[1]); voice3(&voice[2]); break;
	case 5: voice7(&voice[1]); voice4(&voice[2]); voice1(&voice[4]); break;
	case 6: voice8(&voice[1]); voice5(&voice[2]); voice2(&voice[3]); break;
	case 7: voice9(&voice[1]); voice6(&voice[2]); voice3(&voice[3]); break;
	case 8: voice7(&voice[2]); voice4(&voice[3]); voice1(&voice[5]); break;
	case 9: voice8(&voice[2]); voice5(&voice[3]); voice2(&voice[4]); break;
	case 10: voice9(&voice[2]); voice6(&voice[3]); voice3(&voice[4]); break;
	case 11: voice7(&voice[3]); voice4(&voice[4]); voice1(&voice[6]); break;
	case 12: voice8(&voice[3]); voice5(&voice[4]); voice2(&voice[5]); break;
	case 13: voice9(&voice[3]); voice6(&voice[4]); voice3(&voice[5]); break;
	case 14: voice7(&voice[4]); voice4(&voice[5]); voice1(&voice[7]); break;
	case 15: voice8(&voice[4]); voice5(&voice[5]); voice2(&voice[6]); break;
	case 16: voice9(&voice[4]); voice6(&voice[5]); voice3(&voice[6]); break;
	case 17: voice1(&voice[0]); voice7(&voice[5]); voice4(&voice[6]); break;
	case 18: voice8(&voice[5]); voice5(&voice[6]); voice2(&voice[7]); break;
	case 19: voice9(&voice[5]); voice6(&voice[6]); voice3(&voice[7]); break;
	case 20: voice1(&voice[1]); voice7(&voice[6]); voice4(&voice[7]); break;
	case 21: voice8(&voice[6]); voice5(&voice[7]); voice2(&voice[0]); break;
	case 22: voice3a(&voice[0]); voice9(&voice[6]); voice6(&voice[7]); echo22(); break;
	case 23: voice7(&voice[7]); echo23(); break;
	case 24: voice8(&voice[7]); echo24(); break;
	case 25: voice3b(&voice[0]); voice9(&voice[7]); echo25(); break;
	case 26: echo26(); break;
	case 27: misc27(); echo27(); break;
	case 28: misc28(); echo28(); break;
	case 29: misc29(); echo29(); break;
	case 30: misc30(); voice3c(&voice[0]); echo30(); break;
	case 31: voice4(&voice[0]); voice1(&voice[2]); break;
	}
}

void dspRun(void) {
	do {
		step(dsp.phase);
		dsp.phase = (dsp.phase + 1) & 31;
		threadStep(&dsp.th, 3 * 8);
	} while (!(smp.th.clock < dsp.th.clock));
}

void dspPower(bool reset) {
	threadFrequency(&dsp.th, sys.apuFrequency);
	if (!reset) {
		randomArray(dsp.apuram, sizeof(dsp.apuram));
		randomArray(dsp.registers, sizeof(dsp.registers));
		// The counter keeps running through a reset.
		memset(&dsp.clock, 0, sizeof(dsp.clock));
		dsp.clock.sample = true;
	}
	dsp.phase = 0;

	memset(&dsp.mainvol, 0, sizeof(dsp.mainvol));
	dsp.mainvol.reset = true;
	dsp.mainvol.mute = true;
	memset(&dsp.echo, 0, sizeof(dsp.echo));
	dsp.echo.readonly = true;
	memset(&dsp.noise, 0, sizeof(dsp.noise));
	dsp.noise.lfsr = 0x4000;
	memset(&dsp.brr, 0, sizeof(dsp.brr));
	memset(&dsp.latch, 0, sizeof(dsp.latch));
	for (int n = 0; n < 8; ++n) {
		memset(&dsp.voice[n], 0, sizeof(dsp.voice[n]));
		dsp.voice[n].brrOffset = 1;
		dsp.voice[n].index = n << 4;
	}
}
