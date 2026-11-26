/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Licensed under the GNU General Public License, version 2 or (at your option) any later version: see LICENSE.
 *
 * The channels' timing, envelopes, sweep, lengths and noise counter follow SameBoy (Copyright (c) Lior
 * Halphon), under the Expat (MIT) license: see LICENSES/SameBoy.txt. */
#include "gb.h"

#include <string.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

// The sound (Pan Docs, "Audio" and "Audio Registers"), timed as SameBoy has it. Two square channels (the
// first with a sweep), a wave channel and a noise channel, run on a 2 MHz clock (2 dots a cycle) counted
// from power-on. The channels are brought up to date only when something needs them: a register access,
// an edge of the frame sequencer's bit, the frame's end. Each sample is the average of every channel's
// output over its 128 dots.

enum {
	SAMPLE_DOTS = 4194304 / GB_SAMPLE_RATE,
	SKIP_INACTIVE = 0,
	SKIP_SKIPPED,
	SKIP_SKIP,
};

static const uint8_t duties[4] = { 0x01, 0x81, 0x87, 0x7E };
static const uint8_t waveShifts[4] = { 4, 0, 1, 2 };

static uint8_t* reg(struct GB* gb, uint8_t address) {
	return &gb->apu.regs[address - 0x10];
}

// The 2 MHz cycles since power-on at time t.
static inline uint32_t apuCycle(const struct GB* gb, uint32_t t) {
	return (t - gb->apu.epoch) >> 1;
}

// The 1 MHz phase: 1 on even cycles from power-on.
static inline bool lfDiv(const struct GB* gb) {
	return !(apuCycle(gb, gb->apu.lastSync) & 1);
}

// The noise channel's alignment to 512 kHz.
static inline unsigned alignment(const struct GB* gb) {
	return apuCycle(gb, gb->apu.lastSync) & 3;
}

static void stopChannel(struct GBChannel* ch) {
	ch->on = false;
	ch->level = 0;
}

// Where a channel's output goes: each sample's mixed sums (left, right, and with channels left out
// those heard: sample 0 is the one being built), the channel's weight in each, the sample the time
// has reached, and when that sample ends.
struct Spans {
	int32_t (*sums)[4];
	int32_t weight[4];
	int outputs;
	uint32_t bucket;
	uint32_t boundary;
};

static inline void addWeighted(struct Spans* spans, int32_t amount) {
	int32_t* sums = spans->sums[spans->bucket];
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
	vst1q_s32(sums, vmlaq_n_s32(vld1q_s32(sums), vld1q_s32(spans->weight), amount));
#else
	sums[0] += amount * spans->weight[0];
	sums[1] += amount * spans->weight[1];
	if (spans->outputs > 2) {
		sums[2] += amount * spans->weight[2];
		sums[3] += amount * spans->weight[3];
	}
#endif
}

// Adds v over the dots from t to end.
static inline void addSpan(struct Spans* spans, uint32_t t, uint32_t end, int32_t v) {
	if (!spans || gbSince(end, t) <= 0) {
		return;
	}
	while (gbSince(end, spans->boundary) > 0) {
		addWeighted(spans, v * (int32_t) (spans->boundary - t));
		t = spans->boundary;
		++spans->bucket;
		spans->boundary += SAMPLE_DOTS;
	}
	addWeighted(spans, v * (int32_t) (end - t));
}

// A channel's DAC output, -15 to 15.
static inline int32_t dacOut(const struct GBChannel* ch) {
	return 2 * ch->level - 15;
}

static void squareLevel(struct GB* gb, int i) {
	struct GBChannel* ch = &gb->apu.square[i];
	if (ch->suppressed) {
		return;
	}
	uint8_t duty = duties[*reg(gb, i ? 0x16 : 0x11) >> 6];
	ch->level = duty >> (7 - ch->position) & 1 ? ch->volume : 0;
}

static void waveLevel(struct GB* gb) {
	struct GBApu* apu = &gb->apu;
	uint8_t sample = apu->wave.position & 1 ? apu->waveByte & 0xF : apu->waveByte >> 4;
	apu->wave.level = sample >> waveShifts[*reg(gb, 0x1C) >> 5 & 3];
}

static void updateLfsr(struct GB* gb) {
	struct GBApu* apu = &gb->apu;
	apu->lfsrSample = apu->lfsr & 1;
	if (apu->noise.on) {
		apu->noise.level = apu->lfsrSample ? apu->noise.volume : 0;
	}
}

static void stepLfsr(struct GB* gb) {
	struct GBApu* apu = &gb->apu;
	apu->lfsrBit7 = apu->lfsr & 0x80;
	unsigned mask = apu->narrow ? 0x4040 : 0x4000;
	bool high = (apu->lfsr ^ (apu->lfsr >> 1) ^ 1) & 1;
	apu->lfsr >>= 1;
	if (high) {
		apu->lfsr |= mask;
	} else {
		apu->lfsr &= ~mask;
	}
	updateLfsr(gb);
	apu->lfsrSteppedNarrow = apu->narrow;
}

// The squares over cycles from t: a step when the countdown runs out, moving through the duty.
static void runSquare(struct GB* gb, int i, uint32_t t, uint32_t cycles, struct Spans* spans) {
	struct GBChannel* ch = &gb->apu.square[i];
	uint32_t from = t;
	if (ch->on && cycles) {
		uint32_t left = cycles;
		ch->delay = ch->delay < left ? 0 : ch->delay - left;
		while (left > ch->countdown) {
			left -= ch->countdown + 1;
			uint32_t at = t + (cycles - left) * 2;
			addSpan(spans, from, at, dacOut(ch));
			from = at;
			ch->countdown = (ch->frequency ^ 0x7FF) * 2 + 1;
			ch->position = (ch->position + 1) & 7;
			ch->suppressed = false;
			ch->didTick = true;
			squareLevel(gb, i);
		}
		ch->justReloaded = left == 0;
		ch->countdown -= left;
	}
	addSpan(spans, from, t + cycles * 2, dacOut(ch));
}

static void runWave(struct GB* gb, uint32_t t, uint32_t cycles, struct Spans* spans) {
	struct GBApu* apu = &gb->apu;
	struct GBChannel* ch = &apu->wave;
	uint32_t from = t;
	if (cycles) {
		apu->waveJustRead = false;
		uint32_t left = cycles;
		if (ch->on) {
			while (left > ch->countdown) {
				left -= ch->countdown + 1;
				uint32_t at = t + (cycles - left) * 2;
				addSpan(spans, from, at, dacOut(ch));
				from = at;
				ch->countdown = ch->frequency ^ 0x7FF;
				ch->position = (ch->position + 1) & 31;
				apu->waveByte = gb->io[0x30 + (ch->position >> 1)];
				waveLevel(gb);
				apu->waveJustRead = true;
			}
			if (left) {
				ch->countdown -= left;
				apu->waveJustRead = false;
			}
		} else if (ch->dac && apu->wavePulsed && gb->model != GB_MODEL_AGB) {
			// Started and stopped, its DAC still on: the countdown keeps running.
			while (left > ch->countdown) {
				left -= ch->countdown + 1;
				ch->countdown = ch->frequency ^ 0x7FF;
			}
			ch->countdown -= left;
		}
	}
	addSpan(spans, from, t + cycles * 2, dacOut(ch));
}

// The noise channel: a 14-bit counter counts every 2 to 28 cycles (NR43's divisor), and each rise of
// its bit NR43 names steps the LFSR.
static void runNoise(struct GB* gb, uint32_t t, uint32_t cycles, struct Spans* spans) {
	struct GBApu* apu = &gb->apu;
	struct GBChannel* ch = &apu->noise;
	uint32_t from = t;
	if (cycles && (apu->noiseCounting || apu->noiseBackground)) {
		uint8_t nr43 = *reg(gb, 0x22);
		uint32_t divisor = (nr43 & 7) << 2;
		if (!divisor) {
			divisor = 2;
		}
		if (!apu->noiseCountdown) {
			apu->noiseCountdown = divisor;
		}
		unsigned bit = nr43 >> 4;
		uint32_t left = cycles;
		if (left >= apu->noiseCountdown) {
			// Counts to the next rise of the bit, which steps the LFSR (only while the channel plays); then
			// every 2 << bit counts.
			if (bit < 14 && ch->on) {
				uint32_t rise = (((1u << bit) - apu->noiseCounter - 1) & ((2u << bit) - 1)) + 1;
				uint32_t need = apu->noiseCountdown + (rise - 1) * divisor;
				if (need <= left) {
					left -= need;
					apu->noiseCounter += rise;
					apu->noiseCountdown = divisor;
					uint32_t at = t + (cycles - left) * 2;
					addSpan(spans, from, at, dacOut(ch));
					from = at;
					stepLfsr(gb);
					uint32_t period = divisor << (bit + 1);
					if (left >= period) {
						// The steps in a run, the output added only where it changes.
						uint32_t steps = left / period;
						uint16_t lfsr = apu->lfsr;
						bool bit7 = lfsr & 0x80;
						unsigned mask = apu->narrow ? 0x4040 : 0x4000;
						int32_t high = 2 * ch->volume - 15, v = dacOut(ch);
						uint32_t k = 1;
						// The LFSR several steps at a time, 8 at 15 bits or 6 at 7: the outputs are the next
						// bits up, the new bits each pair's XNOR.
						unsigned batch = apu->narrow ? 6 : 8;
						for (; k + batch - 1 <= steps; k += batch) {
							uint32_t x = lfsr;
							uint32_t outs = (x >> 1) & ((1u << batch) - 1);
							uint32_t xnor = ~(x ^ (x >> 1)) & ((1u << batch) - 1);
							if (apu->narrow) {
								bit7 = x >> 12 & 1;
								lfsr = (uint16_t) (((x >> 6) & 0x181) | (xnor << 1) | (xnor << 9));
							} else {
								bit7 = x >> 14 & 1;
								lfsr = (uint16_t) ((x >> 8) | (xnor << 7));
							}
							if (spans && high != -15) {
								uint32_t changes = (outs ^ ((outs << 1) | (v == high))) & ((1u << batch) - 1);
								for (; changes; changes &= changes - 1) {
									int j = __builtin_ctz(changes);
									at = t + (cycles - left + (k + j) * period) * 2;
									addSpan(spans, from, at, v);
									from = at;
									v = outs >> j & 1 ? high : -15;
								}
							}
						}
						for (; k <= steps; ++k) {
							uint16_t before = lfsr;
							bit7 = before & 0x80;
							lfsr = lfsr >> 1;
							lfsr = (((before ^ (before >> 1) ^ 1) & 1) ? lfsr | mask : lfsr & ~mask);
							int32_t next = lfsr & 1 ? high : -15;
							if (next != v) {
								at = t + (cycles - left + k * period) * 2;
								addSpan(spans, from, at, v);
								from = at;
								v = next;
							}
						}
						left -= steps * period;
						apu->noiseCounter += steps << (bit + 1);
						apu->lfsrBit7 = bit7;
						apu->lfsr = lfsr;
						updateLfsr(gb);
					}
				}
			}
			if (left >= apu->noiseCountdown) {
				uint32_t counts = 1 + (left - apu->noiseCountdown) / divisor;
				left -= apu->noiseCountdown + (counts - 1) * divisor;
				apu->noiseCounter += counts;
				apu->noiseCountdown = divisor;
			}
			apu->noiseCounter &= 0x3FFF;
			apu->noiseDidStep = true;
		}
		apu->noiseCountdown -= left;
		apu->noiseReloaded = !left;
	}
	addSpan(spans, from, t + cycles * 2, dacOut(ch));
}

static void sweepDone(struct GB* gb) {
	struct GBApu* apu = &gb->apu;
	uint8_t nr10 = *reg(gb, 0x10);
	if (!apu->restartHold) {
		apu->sweepShadow = apu->square[0].frequency;
	}
	if (nr10 & 8) {
		apu->sweepAddend ^= 0x7FF;
	}
	if (apu->sweepShadow + apu->sweepAddend > 0x7FF && !(nr10 & 8)) {
		stopChannel(&apu->square[0]);
	}
	apu->sweepCompletedAddend = apu->sweepAddend;
}

// The sweep's timers over cycles, counting at 1 MHz.
static void runSweep(struct GB* gb, uint32_t cycles, uint32_t end) {
	struct GBApu* apu = &gb->apu;
	uint32_t ticks = cycles / 2 + ((cycles & 1) && (apuCycle(gb, end) & 1));
	if (apu->sweepReloadTimer > ticks) {
		apu->sweepReloadTimer -= ticks;
		ticks = 0;
	} else {
		if (apu->sweepReloadTimer && !apu->sweepCalcCountdown && apu->sweepInstantDone) {
			sweepDone(gb);
		}
		apu->sweepInstantDone = false;
		ticks -= apu->sweepReloadTimer;
		apu->sweepReloadTimer = 0;
	}
	if (apu->sweepCalcCountdown && ((*reg(gb, 0x10) & 7) || apu->sweepUnshifted)) {
		if (apu->sweepCalcCountdown > ticks) {
			apu->sweepCalcCountdown -= ticks;
		} else {
			apu->sweepCalcCountdown = 0;
			sweepDone(gb);
		}
	}
	apu->restartHold = apu->restartHold > cycles ? apu->restartHold - cycles : 0;
}

// The sound from the last sync to end: each channel on its own over the whole stretch, mixed into the
// samples as it goes. A stretch is at most a frame or a frame sequencer step long.
static void render(struct GB* gb, uint32_t end) {
	struct GBApu* apu = &gb->apu;
	enum { MAX_SAMPLES = 1024 };
	while (gbSince(end, apu->lastSync) > 0) {
		uint32_t stop = end;
		if (gbSince(stop, apu->sampleStart + (MAX_SAMPLES - 1) * SAMPLE_DOTS) > 0) {
			stop = apu->sampleStart + (MAX_SAMPLES - 1) * SAMPLE_DOTS;
		}
		// Powered, the sound moves in whole cycles.
		if (apu->power) {
			stop -= (stop - apu->epoch) & 1;
			if (stop == apu->lastSync) {
				break;
			}
		}
		uint32_t complete = (stop - apu->sampleStart) / SAMPLE_DOTS;
		uint32_t cycles = (apuCycle(gb, stop) - apuCycle(gb, apu->lastSync)) & 0x7FFFFFFF;
		// The channels run on the 2 MHz clock's cycles; the sound follows them from the last sync.
		uint32_t t = apu->lastSync;
		if (apu->power) {
			runSweep(gb, cycles, stop);
		}
		// Each channel added straight into the mix: by NR51 to each side, at NR50's volumes; with channels
		// left out, also into what's heard.
		uint8_t nr51 = *reg(gb, 0x25);
		uint8_t nr50 = *reg(gb, 0x24);
		int32_t leftVolume = (nr50 >> 4 & 7) + 1;
		int32_t rightVolume = (nr50 & 7) + 1;
		uint8_t muted = gb->channelsMuted;
		static int32_t mix[MAX_SAMPLES][4];
		memset(mix, 0, (complete + 1) * sizeof(mix[0]));
		struct Spans spans[4];
		struct Spans* out[4];
		struct GBChannel* chs[4] = { &apu->square[0], &apu->square[1], &apu->wave, &apu->noise };
		for (int i = 0; i < 4; ++i) {
			struct Spans* sp = &spans[i];
			sp->sums = mix;
			sp->outputs = muted ? 4 : 2;
			sp->weight[0] = nr51 >> (4 + i) & 1 ? leftVolume : 0;
			sp->weight[1] = nr51 >> i & 1 ? rightVolume : 0;
			sp->weight[2] = muted >> i & 1 ? 0 : sp->weight[0];
			sp->weight[3] = muted >> i & 1 ? 0 : sp->weight[1];
			sp->bucket = (t - apu->sampleStart) / SAMPLE_DOTS;
			sp->boundary = apu->sampleStart + (sp->bucket + 1) * SAMPLE_DOTS;
			out[i] = chs[i]->dac && (sp->weight[0] || sp->weight[1]) ? sp : NULL;
		}
		runSquare(gb, 0, t, cycles, out[0]);
		runSquare(gb, 1, t, cycles, out[1]);
		runWave(gb, t, cycles, out[2]);
		runNoise(gb, t, cycles, out[3]);
		// A sample is the mean over its 128 dots, times 64: all four channels at full volume reach 30720.
		// The sample being built carries over what it has, mixed as it was.
		bool dacs = apu->square[0].dac || apu->square[1].dac || apu->wave.dac || apu->noise.dac;
		mix[0][0] += apu->partialLeft;
		mix[0][1] += apu->partialRight;
		mix[0][2] += apu->partialHeardLeft;
		mix[0][3] += apu->partialHeardRight;
		for (uint32_t k = 0; k < complete && gb->audioCount + 2 <= sizeof(gb->audio) / sizeof(gb->audio[0]); ++k) {
			gb->audioDacs[gb->audioCount / 2] = dacs;
			if (muted) {
				// Channel Selection: what's heard, beside the console's own.
				gb->heard[gb->audioCount] = mix[k][2] / 2;
				gb->heard[gb->audioCount + 1] = mix[k][3] / 2;
			}
			gb->audio[gb->audioCount++] = mix[k][0] / 2;
			gb->audio[gb->audioCount++] = mix[k][1] / 2;
		}
		apu->partialLeft = mix[complete][0];
		apu->partialRight = mix[complete][1];
		apu->partialHeardLeft = muted ? mix[complete][2] : mix[complete][0];
		apu->partialHeardRight = muted ? mix[complete][3] : mix[complete][1];
		apu->sampleStart += complete * SAMPLE_DOTS;
		apu->lastSync = stop;
	}
}


// The envelopes (SameBoy's): a 3-bit countdown steps down on the divider's eighth event while the
// envelope's clock is low. On the next rise of the sequencer's bit, at 0, it reloads from NRx2 and, if
// that isn't 0, raises the clock, which the next event takes down, moving the volume. A clock raised
// with the volume at 15 going up, or 0 going down, locks the envelope until the channel restarts.
static void setEnvelopeClock(struct GBChannel* ch, bool value, bool up) {
	if (ch->envClock == value) {
		return;
	}
	ch->envClock = value;
	if (value) {
		ch->envShouldLock = (ch->volume == 0xF && up) || (ch->volume == 0 && !up);
	} else {
		ch->envLocked |= ch->envShouldLock;
	}
}

static void tickSquareEnvelope(struct GB* gb, int i) {
	struct GBChannel* ch = &gb->apu.square[i];
	setEnvelopeClock(ch, false, false);
	uint8_t nrx2 = *reg(gb, i ? 0x17 : 0x12);
	if (ch->envLocked || !(nrx2 & 7)) {
		return;
	}
	ch->volume = (ch->volume + (nrx2 & 8 ? 1 : -1)) & 0xF;
	if (ch->on) {
		squareLevel(gb, i);
	}
}

static void tickNoiseEnvelope(struct GB* gb) {
	struct GBApu* apu = &gb->apu;
	struct GBChannel* ch = &apu->noise;
	setEnvelopeClock(ch, false, false);
	uint8_t nr42 = *reg(gb, 0x21);
	if (ch->envLocked || !(nr42 & 7)) {
		return;
	}
	ch->volume = (ch->volume + (nr42 & 8 ? 1 : -1)) & 0xF;
	if (ch->on) {
		ch->level = apu->lfsrSample ? ch->volume : 0;
	}
}

static void tickEnvelopes(struct GB* gb) {
	struct GBApu* apu = &gb->apu;
	for (int i = 0; i < 2; ++i) {
		if (apu->square[i].envClock) {
			tickSquareEnvelope(gb, i);
		}
	}
	if (apu->noise.envClock) {
		tickNoiseEnvelope(gb);
	}
}

// NRx2 written while the channel plays ("zombie mode"): the volume can move or invert, by how the
// envelope's clock lines meet.
static void envelopeWrite(struct GBChannel* ch, uint8_t value, uint8_t old) {
	if (ch->envClock) {
		ch->envCountdown = value & 7;
	}
	bool tick = (value & 7) && !(old & 7) && !ch->envLocked;
	bool invert = (value & 8) ^ (old & 8);
	if ((value & 0xF) == 8 && (old & 0xF) == 8 && !ch->envLocked) {
		tick = true;
	}
	if (invert) {
		if (value & 8) {
			if (!(old & 7) && !ch->envLocked) {
				ch->volume ^= 0xF;
			} else {
				ch->volume = (0xE - ch->volume) & 0xF;
			}
			tick = false;
		} else {
			ch->volume = (0x10 - ch->volume) & 0xF;
		}
	}
	if (tick) {
		ch->volume = (ch->volume + (value & 8 ? 1 : -1)) & 0xF;
	} else if (!(value & 7) && ch->envClock) {
		setEnvelopeClock(ch, false, false);
	}
}

// The original Game Boy and the earlier Colors take the write as two, through all bits set.
static void nrx2Glitch(struct GB* gb, struct GBChannel* ch, uint8_t value, uint8_t old) {
	if (!gbColor(gb)) {
		envelopeWrite(ch, 0xFF, old);
		envelopeWrite(ch, value, 0xFF);
	} else {
		envelopeWrite(ch, value, old);
	}
}

static void triggerSweepCalculation(struct GB* gb) {
	struct GBApu* apu = &gb->apu;
	uint8_t nr10 = *reg(gb, 0x10);
	if (!(nr10 & 0x70) || apu->sweepCountdown != 7) {
		return;
	}
	if (nr10 & 7) {
		apu->square[0].frequency = (apu->sweepAddend + apu->sweepShadow + !!(nr10 & 8)) & 0x7FF;
	}
	if (!apu->restartHold) {
		apu->sweepAddend = apu->square[0].frequency >> (nr10 & 7);
	}
	// The recalculation and overflow check come after a delay.
	apu->sweepCalcCountdown = nr10 & 7;
	apu->sweepReloadTimer = 1 + lfDiv(gb);
	if (!gb->doubleSpeed && apu->duringDivWrite) {
		apu->sweepReloadTimer = 1;
	}
	apu->sweepUnshifted = !(nr10 & 7);
	apu->sweepCountdown = ((nr10 >> 4) & 7) ^ 7;
	if (!apu->sweepCalcCountdown) {
		apu->sweepInstantDone = true;
	}
}

static void clockLength(struct GB* gb, struct GBChannel* ch) {
	if (ch->lengthOn && ch->length && !--ch->length) {
		stopChannel(ch);
	}
}

// The divider's 512 Hz falling edge: the frame sequencer's step.
static void divEvent(struct GB* gb) {
	struct GBApu* apu = &gb->apu;
	if (!apu->power) {
		return;
	}
	if (apu->skipDivEvent == SKIP_SKIP) {
		apu->skipDivEvent = SKIP_SKIPPED;
		return;
	}
	if (apu->skipDivEvent == SKIP_SKIPPED) {
		apu->skipDivEvent = SKIP_INACTIVE;
	} else {
		++apu->divider;
	}
	struct GBChannel* envs[3] = { &apu->square[0], &apu->square[1], &apu->noise };
	if ((apu->divider & 7) == 7) {
		for (int i = 0; i < 3; ++i) {
			if (!envs[i]->envClock) {
				envs[i]->envCountdown = (envs[i]->envCountdown - 1) & 7;
			}
		}
	}
	if (gb->doubleSpeed && gb->model == GB_MODEL_CGB) {
		apu->envelopePending = true;
		apu->envelopeAt = apu->lastSync + 2;
	} else {
		tickEnvelopes(gb);
	}
	if (apu->divider & 1) {
		clockLength(gb, &apu->square[0]);
		clockLength(gb, &apu->square[1]);
		struct GBChannel* wave = &apu->wave;
		if (wave->lengthOn && wave->length && !--wave->length) {
			if (wave->on && gb->model == GB_MODEL_AGB) {
				if (wave->countdown == 0) {
					apu->waveByte = gb->io[0x30 + (((wave->position + 1) & 0xF) >> 1)];
				} else if (wave->countdown == 9) {
					apu->waveByte = gb->io[0x30];
				}
			}
			stopChannel(wave);
		}
		clockLength(gb, &apu->noise);
	}
	if ((apu->divider & 3) == 3) {
		apu->sweepCountdown = (apu->sweepCountdown + 1) & 7;
		triggerSweepCalculation(gb);
	}
}

// Its rising edge: envelopes whose countdown ran out reload it and raise their clocks.
static void divSecondaryEvent(struct GB* gb) {
	struct GBApu* apu = &gb->apu;
	if (!apu->power) {
		return;
	}
	struct GBChannel* envs[3] = { &apu->square[0], &apu->square[1], &apu->noise };
	uint8_t nrx2[3] = { *reg(gb, 0x12), *reg(gb, 0x17), *reg(gb, 0x21) };
	for (int i = 0; i < 3; ++i) {
		if (envs[i]->on && envs[i]->envCountdown == 0) {
			envs[i]->envCountdown = nrx2[i] & 7;
			setEnvelopeClock(envs[i], envs[i]->envCountdown != 0, nrx2[i] & 8);
		}
	}
}

// The counter bit whose falling edges step the frame sequencer: 512 Hz at either speed.
static unsigned sequencerBit(const struct GB* gb) {
	return gb->doubleSpeed ? 13 : 12;
}

// While the sweep's timers run, the sound moves an M-cycle at a time, as their ends can stop channel 1.
static bool sweepBusy(struct GB* gb) {
	struct GBApu* apu = &gb->apu;
	return apu->sweepReloadTimer || apu->restartHold || (apu->sweepCalcCountdown && ((*reg(gb, 0x10) & 7) || apu->sweepUnshifted));
}

static void advance(struct GB* gb, uint32_t end) {
	struct GBApu* apu = &gb->apu;
	while (apu->power && sweepBusy(gb) && gbSince(end, apu->lastSync) > gb->mcycle) {
		render(gb, apu->lastSync + gb->mcycle);
	}
	render(gb, end);
}

static void writeNoiseControl(struct GB* gb, uint8_t value);

void gbApuSync(struct GB* gb) {
	struct GBApu* apu = &gb->apu;
	int32_t dots = gbSince(gb->now, apu->lastSync);
	if (dots <= 0) {
		return;
	}
	if (!apu->power) {
		render(gb, gb->now);
		return;
	}
	// The divider's counter at the last sync, from its value now.
	gbTimerSync(gb);
	uint32_t per = gb->doubleSpeed ? 2 : 1;
	uint32_t from = apu->lastSync;
	uint32_t c = (uint16_t) (gb->timer.counter - (uint32_t) dots * per);
	struct GBTimer* t = &gb->timer;
	if (t->held) {
		// DIV was held (a speed switch's STOP) since the last sync: its edges start from where it ran
		// again, at 0.
		if (gbSince(t->holdEnd, gb->now) > 0) {
			from = t->holdEnd;
			c = 0;
		} else {
			t->held = false;
			if (gbSince(t->holdEnd, from) > 0) {
				from = t->holdEnd;
				c = (uint16_t) (t->counter - (uint32_t) gbSince(gb->now, from) * per);
			}
		}
	}
	// Each edge of the sequencer's bit: falling, a step; rising, the envelopes' reload. Between them,
	// the delayed envelope step and the original's delayed noise start.
	unsigned bit = sequencerBit(gb);
	for (;;) {
		uint32_t edge = ((c >> bit) + 1) << bit;
		uint32_t next = from + (edge - c + per - 1) / per;
		int kind = 0;
		if (apu->envelopePending && gbSince(apu->envelopeAt, next) < 0) {
			next = apu->envelopeAt;
			kind = 1;
		}
		if (apu->noiseDelayed && gbSince(apu->noiseStartAt, next) < 0) {
			next = apu->noiseStartAt;
			kind = 2;
		}
		if (gbSince(next, gb->now) > 0) {
			break;
		}
		if (kind == 2) {
			advance(gb, next);
			apu->noiseDelayed = false;
			writeNoiseControl(gb, *reg(gb, 0x23) | 0x80);
			continue;
		}
		// The divider's events come before the 2 MHz cycles of their M-cycle (SameBoy's order).
		advance(gb, kind == 0 && gbSince(next - gb->mcycle, apu->lastSync) > 0 ? next - gb->mcycle : next);
		if (kind == 1) {
			apu->envelopePending = false;
			tickEnvelopes(gb);
			continue;
		}
		if (edge >> bit & 1) {
			divSecondaryEvent(gb);
		} else {
			divEvent(gb);
		}
		from = next;
		c = edge & 0xFFFF;
	}
	advance(gb, gb->now);
}

// Writing DIV can make the frame sequencer's bit fall.
void gbApuDivReset(struct GB* gb) {
	struct GBApu* apu = &gb->apu;
	gbApuSync(gb);
	if (apu->power && gb->timer.counter >> sequencerBit(gb) & 1) {
		apu->duringDivWrite = true;
		divEvent(gb);
		apu->duringDivWrite = false;
	}
}


// The noise channel's start and NR43's glitches, as SameBoy has them.
static void prepareNoiseStart(struct GB* gb) {
	struct GBApu* apu = &gb->apu;
	/*
	 TODO: When restarting a channel right after starting it, before it has the chance to tick the counter, things
	 behave differently. Only certain behaviors of this edge case are emulated.
	*/
	
	/*
	 TODO: Restarting a channel in double speed mode under CGB-C and older is not accurate if the divisor is 0 or 1.
		   Specifically in the 0 case, the initial LFSR value seems to be deterministic, but dependant on various
		   parameters. It is neither 0 or the equaly unexplained 0x0055.
	*/
	apu->noiseCounting = *reg(gb, 0x21) & 0xF8; // Resets on APU off and DAC disable
	bool was_started_with_dac_disabled = apu->noiseStartedDacOff;
	apu->noiseStartedDacOff = !apu->noiseCounting;
	unsigned divisor = (*reg(gb, 0x22) & 0x07);
	bool was_background_counting = apu->noiseBackground;
	apu->noiseBackground = true;
	bool instant_step = false;
	bool div_1_glitch = false;
	
	if (divisor > 1 && apu->noiseCountdown == 1) {
		apu->noiseCounter++;
		apu->noiseCounter &= 0x3FFF;
	}
	else if (divisor > 1 && apu->noiseCountdown == 2 && apu->noise.on && !gbColor(gb) && gb->doubleSpeed) {
		apu->noiseCounter++;
		apu->noiseCounter &= 0x3FFF;
	}
	else if (apu->noiseCountdown == 2 &&
		(alignment(gb) & 3) == 0 &&
		apu->noise.on) {
		if (divisor == 0) {
			divisor = 8;
		}
		else if (divisor == 1) {
			if (!apu->noiseDidStep) {
				div_1_glitch = true;
			}
			
			uint16_t mask = 1 << (*reg(gb, 0x22) >> 4);
			bool old_bit = apu->noiseCounter & mask;
			apu->noiseCounter++;
			apu->noiseCounter &= 0x3FFF;
			bool new_bit = apu->noiseCounter & mask;
			
			if ((new_bit && !old_bit)) {
				instant_step = true;
			}
		}
	}
	apu->noiseCountdown = divisor == 0? 6 : divisor * 4 + 6;
	if  (alignment(gb) & 1) {
		if (!divisor) {
			if (!gbColor(gb)) {
				apu->noiseCountdown++;
			}
			else if (was_background_counting) {
				apu->noiseCountdown--;
			}
			else {
				apu->noiseCountdown++;
			}
		}
		else {
			if (alignment(gb) & 2) {
				if (divisor == 1 && !apu->noise.on) {
					apu->noiseCountdown++;
				}
				else {
					apu->noiseCountdown -= 3;
				}
			}
			else {
				apu->noiseCountdown--;
				if (divisor == 1 && apu->noise.on) {
					apu->noiseCountdown -= 4;
				}
			}
		}
	}
	else {
		if (divisor) {
			if (alignment(gb) & 2) {
				if (gb->doubleSpeed && !gbColor(gb) && divisor == 1) {
					apu->noiseCountdown += 2;
				}
				else {
					apu->noiseCountdown -= 2;
				}
			}
			else if (divisor > 1 && (!gb->doubleSpeed || gbColor(gb))) {
				apu->noiseCountdown -= 4;
			}
			/* TODO: This quirk seems way too specific */
			else if (divisor == 1 && apu->noise.on && !(*reg(gb, 0x22) & 0xf0)) {
				apu->noiseCountdown -= 4;
			}
		}
		else if (gb->doubleSpeed && !gbColor(gb)) {
			apu->noiseCountdown += 2;
		}
	}
	
	/* Background counting glitches */
	/* TODO: Double speed mode not tested */
	if (divisor > 1) {
		if (!apu->noiseCounting && !(alignment(gb) & 3)) {
			apu->noiseCountdown += 4;
		}
	}
	else {
		if (was_background_counting && !apu->noise.on && !(alignment(gb) & 3)) {
			if (divisor == 0) {
				if (was_started_with_dac_disabled) { // TODO: Why is it different?
					apu->noiseCountdown += 28;
				}
			}
			else {
				apu->noiseCountdown -= 4;
			}
		}
	}
	
	/* TODO: This is weird, is the clock going out of sync? */
	if (!divisor && !gbColor(gb) && was_background_counting && !apu->noise.on && gb->doubleSpeed) {
		apu->noiseCountdown--;
	}
	if (div_1_glitch) {
		apu->noiseCountdown -= 4;
	}
	
	if (!divisor && apu->noise.on && (alignment(gb) & 3) == 3) {
		/* TODO: I have no clue where this number comes from, but this number is confirmed for this edge case even for
				 side LFSR, despite being seemingly arbitrary. */
		apu->lfsr = 0x0055;
	}
	else {
		apu->lfsr = 0;
	}
	if (instant_step) {
		stepLfsr(gb);
	}
}

static void nr43Write(struct GB* gb, uint8_t new) {
	struct GBApu* apu = &gb->apu;
	/*
		NR43 writes cause glitch signals to the LFSR. They are often non-deterministic, and
		they're revision and instance specific.  This implementation is trying to emulate a
		simplified and deterministic "variant" of specific instances of revisions I own.
	 
		For more details:
		https://github.com/LIJI32/SameBoy/issues/397#issuecomment-3733625631
	*/
	
	/*
		TODO: Non-determinism aside, this is currently only 100% accurate in CGB-E mode, where
		my specific CGB-E is currently emulated.  My CGB-D, under rare cases, samples a second
		intermediate value,  and this is not  currently emulated.  AGB revisions are extremely
		glitchy, and are hard to research.
	 
		Due to FF-write glitches in pre-CGB-D revisions, all writes (even no-change writes) go
		through 3 intermediate values by definition. Also, the effective counter value used is
		ORed with the next (or previous, timing needs to be verified) value.
	*/
	bool old_narrow = apu->narrow;
	apu->narrow = new & 8;
	uint8_t old = *reg(gb, 0x22);
	*reg(gb, 0x22) = new;
	
	if ((old & 0xF0) == (new & 0xF0)) return;
	
	uint16_t effective_counter = apu->noiseCounter;
	if (!gbColor(gb) && apu->noiseReloaded) {
		effective_counter |= (effective_counter - 1) & 0x3FFF;
	}
	bool old_bit = (effective_counter >> (old >> 4)) & 1;

	uint8_t glitch_value = (old & 0x7F) | (new & 0x80);
	bool glitch_bit = (effective_counter >> (glitch_value >> 4)) & 1;
	bool new_bit = (effective_counter >> (new >> 4)) & 1;
	bool force_glitch = false;

	
	if (gb->model == GB_MODEL_AGB) {
		/* AGB behavior is very glitchy and incosistent. It can have 2 intermediate values for
		   NR43, and sometimes even 3, and the pattern isn't very consistent. This is a *very*
		   rough approximation of the behavior.
		 
		   Due to having so up to 3 intermediate value, glitch behavior is complicated to look
		   into, so currently CGB-E behavior is arbitrarily used if a glitch occurs. */
		
		uint8_t glitch_value2 = 0;
		if (new >= 0x80 && old >= 0x80) {
			glitch_value  = (old & 0xCF) | (new & 0x30);
			glitch_value2 = (old & 0x8F) | (new & 0x70);
		}
		else {
			glitch_value = (old & 0xDF) | (new & 0x20);
			glitch_value2 = (old & 0xCF) | (new & 0x30);
		}
		glitch_bit = (apu->noiseCounter >> (glitch_value >> 4)) & 1;
		uint8_t glitch_bit2 = (apu->noiseCounter >> (glitch_value2 >> 4)) & 1;
		if (glitch_bit != glitch_bit2) {
			if (new_bit == old_bit) {
				glitch_bit = !new_bit;
			}
			else if (!glitch_bit && old_bit) {
				force_glitch = true;
			}
		}
	}
	
	/* Step LFSR */
	
	if ((old_bit == new_bit && new_bit != glitch_bit) || force_glitch) {
		/* Glitching write.  Has two categories,  both have  non-deterministic
		   variants. These are the most common variants of the two categories,
		   which are deterministic. */
		if (new_bit) {
			/* Category 1 */
			if (gbColor(gb)) {
				if (!(new & 0x80)) {
					stepLfsr(gb);
				}
				else {
					/* Only happens under this odd condition */
					uint8_t t1 = (old >> 4) & 7;
					uint8_t t2 = (new >> 4) & 7;
					
					if ((t1 ^ 7) + t2 > 7 || ((t1 ^ 7) & t2)) {
						/* Copy bit 8 to bit 7 */
						apu->lfsr &= ~0x80;
						apu->lfsr |= (apu->lfsr >> 1) & 0x80;
						
						/* All specific cases have non-deterministic behaviors involved */
						if ((t1 == 0 || t1 == 4) && t2 == 3) {
							apu->lfsr &= (apu->lfsr >> 1) | 0x545;
							updateLfsr(gb);
						}
						else if (t1 == 2 && t2 == 3) {
							uint16_t mask = 0x555;
							if ((apu->lfsr & 0xC) == 0xC) {
								mask |= 8;
							}
							if ((apu->lfsr & 0xC00) == 0xC00) {
								mask |= 0x800;
							}
							
							apu->lfsr &= (apu->lfsr >> 1) | mask;
							updateLfsr(gb);
						}
						if (!apu->narrow && old_narrow && apu->lfsrSteppedNarrow) {
							/* TODO: Behaves weirder in non-deterministic t1 == 0/4 scenarios? */
							if (apu->lfsrBit7) {
								apu->lfsr |= 0x40;
							}
							else {
								apu->lfsr &= ~0x40;
							}
						}
						apu->lfsr |= apu->narrow ? 0x4040 : 0x4000;
						/* TODO: verify */
						apu->lfsrSteppedNarrow = apu->narrow;
					}
				}
			}
		}
		else {
			/* Category 2 */
			if (gbColor(gb)) {
				static const uint8_t glitch_map[8 * 8] = {
				/*	8		  9		  A		  B		  C		  D		 */
										  [002] = 4, [003] = 2, [004] = 2, [005] = 2, // 0
										  [012] = 2, [013] = 4, [014] = 2, [015] = 2, // 1
					[020] = 1, [021] = 2,			[023] = 1, [024] = 5, [025] = 3, // 2
																[034] = 2, [035] = 2, // 3
							   [041] = 2, [042] = 2, [043] = 2,					   // 4
					[050] = 6,			[052] = 2, [053] = 2,					   // 5
				};
				
				/* The following transitions are a bit non-deterministic:
				 2 -> 8, 0 -> A, 2 -> C */
				
				unsigned glitch = new & 0x80? glitch_map[((old & 0x70) >> 1) | ((new & 0x70) >> 4)] : 0;
				switch (glitch) {
					case 1: /* Step, followed by bit 1 &= bit 0 */
					case 6: /* Variant of type 1: LFSR bit - 1 glitched by LFSR bit, LFSR bit - 2, and bit 0 */
						stepLfsr(gb);
						if (glitch == 6) {
							/* TODO: Verify wide mode */
							if ((apu->narrow &&
								((apu->lfsr & 0x71) == 0x20)) ||
								(apu->lfsr & 0x71) == 0x61) {
								apu->lfsr &= ~0x20;
							}
							if ((apu->lfsr & 0x7001) == 0x2000 ||
								(apu->lfsr & 0x7001) == 0x6001) {
								apu->lfsr &= ~0x2000;
							}
						}
						if ((apu->lfsr & 0x3) == 2) {
							apu->lfsr &= ~2;
						}
						break;
					case 2: { /* Step, bitwise AND with previous, except for bit 0 */
						uint16_t prev = apu->lfsr;
						stepLfsr(gb);
						apu->lfsr &= prev | 1;
						break;
					}
						
					case 5:; /* Non deterministic variant of type 3:
							  The LFSR is unset if bit 0 & 1 are 0b10.
							  Bit 3 is complex and non-deterministic (TODO: wide mode) */
						if ((apu->lfsr & 0x3) == 2) {
							apu->lfsr &= apu->narrow? ~0x4040 : ~0x4000;
						}
						
						if ((apu->lfsr & 0x19) == 8) {
							apu->lfsr &= ~8;
						}
						__attribute__((fallthrough));
					case 3: /* No step, bit 0 = bit 1, some other bits have AND glitches with next*/
						apu->lfsr &= ~1;
						apu->lfsr |= (apu->lfsr >> 1) & 1;
						
						updateLfsr(gb);
						/* TODO: verify */
						apu->lfsrSteppedNarrow = apu->narrow;
						break;
						
					case 4: { /* Step, bit 1 &= bit 0, LFSR bit -1 &= LFSR bit */
						uint16_t prev = apu->lfsr;
						stepLfsr(gb);
						apu->lfsr &= prev | (apu->narrow? ~0x2022 : ~0x2002);
						break;
					}
						
					default: /* No glitch, plain step*/
						stepLfsr(gb);
						break;
						
				}
			}
			else {
				stepLfsr(gb);
			}
		}
	}
	else if (!old_bit && new_bit) {
		if (!gbColor(gb)) {
			bool previous_narrow = apu->narrow;
			apu->narrow = true;
			stepLfsr(gb);
			apu->narrow = previous_narrow;
			if ((new & 0xf0) <= 0x20 && glitch_bit && !(effective_counter & 8)) { // No clue why that specific bit is tested
				// Non-deterministic, not fully tested for revision differences and wide mode
				// Step twice?
				stepLfsr(gb);
				apu->lfsr &= ~(apu->narrow? 0x4040 : 0x4000);
				apu->lfsr |= (apu->lfsr & (apu->narrow? 0x2020 : 0x2000)) << 1;
			}
		}
		else {
			stepLfsr(gb);
		}
	}
	else if (!gbColor(gb)) {
		if ((new & 0xf0) <= 0x20 && !glitch_bit && !new_bit && !old_bit && (effective_counter & 8)) { // No clue why that specific bit is tested
			// Step twice?
			stepLfsr(gb);
		}
	}
}


static void nr10WriteGlitch(struct GB* gb, uint8_t value) {
	struct GBApu* apu = &gb->apu;
	uint8_t nr10 = *reg(gb, 0x10);
	if (!gbColor(gb)) {
		if (apu->sweepReloadTimer > 1 || (apu->sweepReloadTimer == 1 && !lfDiv(gb))) {
			return;
		}
		if (apu->sweepCalcCountdown && !(nr10 & 7) && lfDiv(gb)) {
			if (--apu->sweepCalcCountdown <= 1) {
				apu->sweepCalcCountdown = 0;
				sweepDone(gb);
			}
		}
		return;
	}
	if (apu->sweepReloadTimer == 2) {
		// Just reloaded: reloaded again.
		apu->sweepCalcCountdown = value & 7;
		if (!apu->sweepCalcCountdown) {
			apu->sweepReloadTimer = 0;
		}
	}
	if ((value & 7) && !(nr10 & 7) && !lfDiv(gb) && apu->sweepCalcCountdown > 1) {
		if (!--apu->sweepCalcCountdown) {
			sweepDone(gb);
		}
	}
}

static void writeSquareControl(struct GB* gb, int i, uint8_t value) {
	struct GBApu* apu = &gb->apu;
	struct GBChannel* ch = &apu->square[i];
	uint8_t old = *reg(gb, i ? 0x19 : 0x14);
	bool wasOn = ch->on;
	bool cgbE = gb->model == GB_MODEL_CGB;
	// The frequency's high bits leaving 7 just before a step: the step comes from the old frequency, but
	// the position doesn't move.
	if (!(value & 0x80) && ch->on && (old & 7) == 7 && (value & 7) != 7) {
		if (cgbE || (ch->countdown & 1)) {
			if (ch->didTick && ch->countdown >> 1 == (ch->frequency ^ 0x7FF)) {
				ch->position = (ch->position - 1) & 7;
				ch->suppressed = false;
			}
		}
	}
	uint16_t oldFrequency = ch->frequency;
	ch->frequency = (ch->frequency & 0xFF) | (value & 7) << 8;
	if (ch->justReloaded) {
		ch->countdown = (ch->frequency ^ 0x7FF) * 2 + 1;
	}
	if (value & 0x80) {
		// The position isn't reset by a start, only by turning the sound off.
		ch->envLocked = false;
		ch->envClock = false;
		ch->didTick = false;
		bool unsuppressed = false;
		if (!ch->on) {
			if (cgbE && !(value & 4) && !(((ch->countdown - ch->delay) / 2) & 0x400)) {
				ch->position = (ch->position + 1) & 7;
				unsuppressed = true;
			}
			ch->delay = 6 - lfDiv(gb);
			ch->countdown = (ch->frequency ^ 0x7FF) * 2 + ch->delay;
		} else {
			unsigned extra = 0;
			if (cgbE) {
				if (!ch->justReloaded && !(value & 4) && !(((ch->countdown - 1 - ch->delay) / 2) & 0x400)) {
					ch->position = (ch->position + 1) & 7;
					ch->suppressed = false;
				} else if (ch->frequency == 0x7FF && oldFrequency != 0x7FF && ch->suppressed) {
					extra += 2;
				}
			}
			// Already playing, it starts 2 cycles sooner.
			ch->delay = 4 - lfDiv(gb) + extra;
			ch->countdown = (ch->frequency ^ 0x7FF) * 2 + ch->delay;
		}
		uint8_t nrx2 = *reg(gb, i ? 0x17 : 0x12);
		ch->volume = nrx2 >> 4;
		// The new volume is heard at once by a channel already playing.
		if (ch->on) {
			squareLevel(gb, i);
		}
		ch->envCountdown = nrx2 & 7;
		if ((nrx2 & 0xF8) && !ch->on) {
			ch->on = true;
			ch->level = 0;
			ch->suppressed = !unsuppressed;
		}
		if (!ch->length) {
			ch->length = 0x40;
			ch->lengthOn = false;
		}
		if (i == 0) {
			uint8_t nr10 = *reg(gb, 0x10);
			apu->sweepInstantDone = false;
			apu->sweepShadow = 0;
			apu->sweepCompletedAddend = 0;
			if (nr10 & 7) {
				// The overflow check also comes on a start when the shift isn't 0.
				apu->sweepCalcCountdown = nr10 & 7;
				apu->sweepReloadTimer = (lfDiv(gb) ^ !gb->doubleSpeed) && !gbColor(gb) ? 3 : 2;
				apu->sweepUnshifted = false;
				if (!wasOn) {
					++apu->sweepReloadTimer;
				}
				apu->sweepAddend = ch->frequency >> (nr10 & 7);
			} else {
				apu->sweepAddend = 0;
			}
			apu->restartHold = 2 - lfDiv(gb) + (gbColor(gb) ? 2 : 0);
			apu->sweepCountdown = ((nr10 >> 4) & 7) ^ 7;
		}
	}
	if ((value & 0x40) && !ch->lengthOn && (apu->divider & 1) && ch->length) {
		if (!--ch->length) {
			if (value & 0x80) {
				ch->length = 0x3F;
			} else {
				stopChannel(ch);
			}
		}
	}
	ch->lengthOn = value & 0x40;
}

static void writeWaveControl(struct GB* gb, uint8_t value) {
	struct GBApu* apu = &gb->apu;
	struct GBChannel* ch = &apu->wave;
	ch->frequency = (ch->frequency & 0xFF) | (value & 7) << 8;
	if (value & 0x80) {
		apu->wavePulsed = true;
		// The original Game Boy, restarted a cycle before the channel reads wave RAM: the read goes over
		// the start of wave RAM, byte 0 with the byte, or bytes 0-3 with its aligned four.
		if (!gbColor(gb) && ch->on && ch->countdown == 0) {
			unsigned offset = ((ch->position + 1) >> 1) & 0xF;
			if (offset < 4 && gb->model != GB_MODEL_MGB) {
				gb->io[0x30] = gb->io[0x30 + offset];
			} else {
				memcpy(&gb->io[0x30], &gb->io[0x30 + (offset & ~3)], 4);
			}
		}
		ch->position = 0;
		if (ch->on && ch->countdown == 0) {
			apu->waveByte = gb->io[0x30];
		}
		if (ch->dac) {
			ch->on = true;
			// The sample doesn't change yet: the byte held, its high half.
			ch->level = (apu->waveByte >> 4) >> waveShifts[*reg(gb, 0x1C) >> 5 & 3];
		}
		ch->countdown = (ch->frequency ^ 0x7FF) + 3;
		if (!ch->length) {
			ch->length = 0x100;
			ch->lengthOn = false;
		}
	}
	if ((value & 0x40) && !ch->lengthOn && (apu->divider & 1) && ch->length) {
		if (!--ch->length) {
			if (value & 0x80) {
				ch->length = 0xFF;
			} else {
				stopChannel(ch);
			}
		}
	}
	ch->lengthOn = value & 0x40;
}

static void writeNoiseControl(struct GB* gb, uint8_t value) {
	struct GBApu* apu = &gb->apu;
	struct GBChannel* ch = &apu->noise;
	if (value & 0x80) {
		ch->envLocked = false;
		ch->envClock = false;
		if (!gbColor(gb) && (alignment(gb) & 3) != 0) {
			// The original starts it at the next 512 kHz edge.
			apu->noiseDelayed = true;
			apu->noiseStartAt = apu->lastSync + 6 * 2;
		} else {
			apu->lfsr = 0;
			prepareNoiseStart(gb);
			uint8_t nr42 = *reg(gb, 0x21);
			ch->volume = nr42 >> 4;
			apu->lfsrSample = false;
			ch->envCountdown = nr42 & 7;
			apu->noiseDidStep = alignment(gb) == 2;
			if (nr42 & 0xF8) {
				ch->on = true;
				ch->level = 0;
			}
			if (!ch->length) {
				ch->length = 0x40;
				ch->lengthOn = false;
			}
		}
	}
	if ((value & 0x40) && !ch->lengthOn && (apu->divider & 1) && ch->length) {
		if (!--ch->length) {
			if (value & 0x80) {
				ch->length = 0x3F;
			} else {
				stopChannel(ch);
			}
		}
	}
	ch->lengthOn = value & 0x40;
	*reg(gb, 0x23) = value;
}

// Power on: the channels as new, the 2 MHz clock from now; the sound built so far kept.
static void powerOn(struct GB* gb) {
	struct GBApu* apu = &gb->apu;
	uint8_t regs[0x30];
	memcpy(regs, apu->regs, sizeof(regs));
	uint32_t lastSync = apu->lastSync, sampleStart = apu->sampleStart;
	int32_t partials[4] = { apu->partialLeft, apu->partialRight, apu->partialHeardLeft, apu->partialHeardRight };
	memset(apu, 0, sizeof(*apu));
	memcpy(apu->regs, regs, sizeof(regs));
	apu->lastSync = lastSync;
	apu->sampleStart = sampleStart;
	apu->partialLeft = partials[0], apu->partialRight = partials[1];
	apu->partialHeardLeft = partials[2], apu->partialHeardRight = partials[3];
	apu->epoch = gb->now;
	apu->square[0].countdown = apu->square[1].countdown = 0xFFFF;
	// Turned on with the sequencer's bit set, the first event is skipped.
	gbTimerSync(gb);
	if (gb->timer.counter >> sequencerBit(gb) & 1) {
		apu->skipDivEvent = SKIP_SKIP;
		apu->divider = 1;
	}
	apu->power = true;
}

void gbApuWrite(struct GB* gb, uint8_t address, uint8_t value) {
	struct GBApu* apu = &gb->apu;
	gbApuSync(gb);
	if (address >= 0x30) {
		if (apu->wave.on) {
			// While it plays, the byte being read takes the write; on the original Game Boy, only
			// if it was read on the last cycle; on the Advance, never (Pan Docs).
			if ((!gbColor(gb) && !apu->waveJustRead) || gb->model == GB_MODEL_AGB) {
				return;
			}
			address = 0x30 + apu->wave.position / 2;
		}
		gb->io[address] = value;
		return;
	}
	bool lengthRegister = address == 0x11 || address == 0x16 || address == 0x1B || address == 0x20;
	if (!apu->power && address != 0x26 && (gbColor(gb) || !lengthRegister)) {
		// Off, only the original Game Boy's length counters can be written.
		return;
	}
	uint8_t old = *reg(gb, address);
	switch (address) {
	case 0x26: {
		uint16_t lengths[4] = { apu->square[0].length, apu->square[1].length, apu->wave.length, apu->noise.length };
		if ((value & 0x80) && !apu->power) {
			powerOn(gb);
		} else if (!(value & 0x80) && apu->power) {
			// Off: every register cleared.
			uint32_t lastSync = apu->lastSync, sampleStart = apu->sampleStart;
			int32_t partials[4] = { apu->partialLeft, apu->partialRight, apu->partialHeardLeft, apu->partialHeardRight };
			uint8_t wave[0x10];
			memcpy(wave, &apu->regs[0x20], sizeof(wave));
			memset(apu, 0, sizeof(*apu));
			memcpy(&apu->regs[0x20], wave, sizeof(wave));
			apu->lastSync = lastSync;
			apu->sampleStart = sampleStart;
			apu->partialLeft = partials[0], apu->partialRight = partials[1];
			apu->partialHeardLeft = partials[2], apu->partialHeardRight = partials[3];
		}
		if (!gbColor(gb) && (value & 0x80)) {
			apu->square[0].length = lengths[0];
			apu->square[1].length = lengths[1];
			apu->wave.length = lengths[2];
			apu->noise.length = lengths[3];
		}
		*reg(gb, address) = value;
		return;
	}
	case 0x10:
		if (apu->sweepCalcCountdown || apu->sweepReloadTimer) {
			nr10WriteGlitch(gb, value);
		}
		*reg(gb, 0x10) = value;
		{
			bool oldNegate = (old & 8) || !gbColor(gb);
			if (apu->sweepShadow + apu->sweepCompletedAddend + oldNegate > 0x7FF && !(value & 8)) {
				stopChannel(&apu->square[0]);
			}
		}
		triggerSweepCalculation(gb);
		return;
	case 0x11:
	case 0x16:
		apu->square[address == 0x16].length = 0x40 - (value & 0x3F);
		if (!apu->power) {
			value &= 0x3F;
		}
		break;
	case 0x12:
	case 0x17: {
		struct GBChannel* ch = &apu->square[address == 0x17];
		ch->dac = (value & 0xF8) != 0;
		*reg(gb, address) = value;
		if (!ch->dac) {
			stopChannel(ch);
		} else if (ch->on) {
			nrx2Glitch(gb, ch, value, old);
			squareLevel(gb, address == 0x17);
		}
		return;
	}
	case 0x13:
	case 0x18: {
		struct GBChannel* ch = &apu->square[address == 0x18];
		ch->frequency = (ch->frequency & 0x700) | value;
		if (ch->justReloaded) {
			ch->countdown = (ch->frequency ^ 0x7FF) * 2 + 1;
		}
		break;
	}
	case 0x14:
	case 0x19:
		writeSquareControl(gb, address == 0x19, value);
		break;
	case 0x1A:
		apu->wave.dac = value & 0x80;
		if (!apu->wave.dac) {
			apu->wavePulsed = false;
			if (apu->wave.on) {
				if (apu->wave.countdown == 0 && gb->model != GB_MODEL_AGB) {
					apu->waveByte = gb->io[0x30 + (gb->cpu.pc & 0xF)];
				} else if (apu->waveJustRead && !gbColor(gb)) {
					apu->waveByte = gb->io[0x30 + 0xA];
				}
			}
			stopChannel(&apu->wave);
		}
		break;
	case 0x1B:
		apu->wave.length = 0x100 - value;
		break;
	case 0x1C:
		*reg(gb, address) = value;
		if (apu->wave.on) {
			waveLevel(gb);
		}
		return;
	case 0x1D:
		apu->wave.frequency = (apu->wave.frequency & 0x700) | value;
		break;
	case 0x1E:
		writeWaveControl(gb, value);
		break;
	case 0x20:
		apu->noise.length = 0x40 - (value & 0x3F);
		break;
	case 0x21: {
		struct GBChannel* ch = &apu->noise;
		ch->dac = (value & 0xF8) != 0;
		if (!ch->dac) {
			if (ch->on && (*reg(gb, 0x22) & 7)) {
				if (apu->noiseCountdown <= 2) {
					++apu->noiseCounter;
				}
				apu->noiseBackground = false;
			}
			*reg(gb, address) = value;
			stopChannel(ch);
			apu->noiseCounting = false;
		} else if (ch->on) {
			nrx2Glitch(gb, ch, value, old);
			ch->level = apu->lfsrSample ? ch->volume : 0;
		}
		*reg(gb, address) = value;
		return;
	}
	case 0x22: {
		if (apu->noiseReloaded) {
			unsigned divisor = (value & 7) << 2;
			if (!divisor) {
				divisor = 2;
			}
			static const uint8_t newer[4] = { 2, 1, 0, 3 }, older[4] = { 2, 1, 4, 3 };
			apu->noiseCountdown = divisor + (divisor == 2 ? 0 : (gbColor(gb) ? newer : older)[alignment(gb)]);
		}
		if (!gbColor(gb)) {
			if (apu->noiseReloaded) {
				bool oldBit = (apu->noiseCounter >> (old >> 4)) & 1;
				bool glitchBit = (apu->noiseCounter >> 7) & 1;
				bool newBit = (apu->noiseCounter >> (value >> 4)) & 1;
				if (!oldBit && newBit && glitchBit) {
					uint16_t previous = (apu->noiseCounter - 1) & 0x3FFF;
					oldBit = (previous >> (old >> 4)) & 1;
					glitchBit = (previous >> 7) & 1;
					newBit = (previous >> (value >> 4)) & 1;
					if (oldBit && !newBit && glitchBit) {
						stepLfsr(gb);
					}
				}
			}
			nr43Write(gb, 0xFF);
		}
		nr43Write(gb, value);
		return;
	}
	case 0x23:
		writeNoiseControl(gb, value);
		return;
	default:
		break;
	}
	*reg(gb, address) = value;
}

uint8_t gbApuRead(struct GB* gb, uint8_t address) {
	struct GBApu* apu = &gb->apu;
	gbApuSync(gb);
	if (address >= 0x30) {
		if (apu->wave.on) {
			// While it plays, a read gets the byte the channel reads; on the original Game Boy only if it
			// was read on the last cycle, and never on the Advance (Pan Docs).
			if ((!gbColor(gb) && !apu->waveJustRead) || gb->model == GB_MODEL_AGB) {
				return 0xFF;
			}
			return gb->io[0x30 + apu->wave.position / 2];
		}
		return gb->io[address];
	}
	if (address == 0x26) {
		return (apu->power ? 0x80 : 0) | (apu->square[0].on ? 1 : 0) | (apu->square[1].on ? 2 : 0) |
		       (apu->wave.on ? 4 : 0) | (apu->noise.on ? 8 : 0);
	}
	return *reg(gb, address);
}

// PCM12 and PCM34 (the Color's): each channel's output now, 0 to 15, two to a byte.
uint8_t gbApuPcm(struct GB* gb, bool high) {
	struct GBApu* apu = &gb->apu;
	gbApuSync(gb);
	const struct GBChannel* a = high ? &apu->wave : &apu->square[0];
	const struct GBChannel* b = high ? &apu->noise : &apu->square[1];
	return (a->on ? a->level : 0) | (b->on ? b->level << 4 : 0);
}

void gbApuReset(struct GB* gb) {
	struct GBApu* apu = &gb->apu;
	memset(apu, 0, sizeof(*apu));
	apu->lastSync = gb->now;
	apu->sampleStart = gb->now;
	apu->epoch = gb->now;
	apu->square[0].countdown = apu->square[1].countdown = 0xFFFF;
}
