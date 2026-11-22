/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "snes.h"

// A cartridge's own sound: its samples averaged down to the DSP's rate, then added to the DSP's.

// The Super Game Boy's samples again, in a ring four times as long. In Fast mode the SMP's thread mixes
// them up to where they were when it was told to run (apuGameBoyWrite), while this thread goes on
// putting new ones in, up to a frame ahead: in the mixer's own ring they could take the place of ones
// still to be mixed. So for the Super Game Boy, keeping only the newest MIXER_SAMPLES is done as they're
// mixed, which lets the same ones go as doing it as they're put in, and they're read from here. A state
// keeps the mixer's own ring, which loading one copies here (mixerLoaded).
enum { SHADOW = 4 * MIXER_SAMPLES };
static s16 shadow[SHADOW][2];

// Each input sample moves the phase on by step; a DSP sample is due each time it reaches rate.
void mixerInput(struct Mixer* m, s16 left, s16 right, u32 step, u32 rate) {
	m->sumLeft += left;
	m->sumRight += right;
	m->summed++;
	m->phase += step;
	if (m->phase < rate) {
		return;
	}
	m->phase -= rate;
	u32 slot = m->write % MIXER_SAMPLES;
	m->samples[slot][0] = m->sumLeft / (s32) m->summed;
	m->samples[slot][1] = m->sumRight / (s32) m->summed;
	if (m == &icd.sound) {
		shadow[m->write % SHADOW][0] = m->samples[slot][0];
		shadow[m->write % SHADOW][1] = m->samples[slot][1];
	} else if (m->write + 1 - m->read > MIXER_SAMPLES) {
		m->read = m->write + 1 - MIXER_SAMPLES;
	}
	m->write++;
	m->sumLeft = m->sumRight = 0;
	m->summed = 0;
}

void mixerLoaded(struct Mixer* m) {
	if (m != &icd.sound) {
		return;
	}
	u32 from = m->write - m->read > MIXER_SAMPLES ? m->write - MIXER_SAMPLES : m->read;
	for (u32 n = from; n != m->write; ++n) {
		shadow[n % SHADOW][0] = m->samples[n % MIXER_SAMPLES][0];
		shadow[n % SHADOW][1] = m->samples[n % MIXER_SAMPLES][1];
	}
}

void mixerOutput(struct Mixer* m, s32* left, s32* right, u32 volume) {
	mixerOutputUpTo(m, left, right, volume, m->write);
}

void mixerOutputUpTo(struct Mixer* m, s32* left, s32* right, u32 volume, u32 write) {
	u32 read = m->read;
	bool const gameBoy = m == &icd.sound;
	if (gameBoy && write - read > MIXER_SAMPLES) {
		read = write - MIXER_SAMPLES;
	}
	if (read == write) {
		m->read = read;
		return;
	}
	m->read = read + 1;
	const s16* sample = gameBoy ? shadow[read % SHADOW] : m->samples[read % MIXER_SAMPLES];
	s32 l = *left + sample[0] * (s32) volume / 100;
	s32 r = *right + sample[1] * (s32) volume / 100;
	*left = l > 32767 ? 32767 : l < -32768 ? -32768 : l;
	*right = r > 32767 ? 32767 : r < -32768 ? -32768 : r;
}
