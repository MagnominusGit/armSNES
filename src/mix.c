/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "snes.h"

// A cartridge's own sound: its samples averaged down to the DSP's rate, then added to the DSP's.

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
	m->write++;
	if (m->write - m->read > MIXER_SAMPLES) {
		m->read = m->write - MIXER_SAMPLES;
	}
	m->sumLeft = m->sumRight = 0;
	m->summed = 0;
}

void mixerOutput(struct Mixer* m, s32* left, s32* right) {
	if (m->read == m->write) {
		return;
	}
	u32 slot = m->read++ % MIXER_SAMPLES;
	s32 l = *left + m->samples[slot][0];
	s32 r = *right + m->samples[slot][1];
	*left = l > 32767 ? 32767 : l < -32768 ? -32768 : l;
	*right = r > 32767 ? 32767 : r < -32768 ? -32768 : r;
}
