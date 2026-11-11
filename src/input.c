/* Copyright (c) 2004-2025 ares team, Near et al
 * Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Ported from ares (ISC); see LICENSES/ISC-ares.txt. */
#include "snes.h"

// The two controller ports, each with a standard pad: a 16-bit shift register that takes the
// buttons when its latch falls.

struct Input input;

void inputPower(void) {
	memset(&input, 0, sizeof(input));
}

u8 controllerData(int port) {
	struct Pad* pad = &input.pad[port];
	if (pad->latched) {
		pad->buttons = (pad->buttons & ~(1 << PAD_B)) | (inputPoll(port) & 1 << PAD_B);
		return pad->buttons >> PAD_B & 1;
	}

	// The D-pad can't have up and down, or left and right, pressed together.
	switch (pad->counter++) {
	case 0: return pad->buttons >> PAD_B & 1;
	case 1: return pad->buttons >> PAD_Y & 1;
	case 2: return pad->buttons >> PAD_SELECT & 1;
	case 3: return pad->buttons >> PAD_START & 1;
	case 4: return pad->upLatch;
	case 5: return pad->downLatch;
	case 6: return pad->leftLatch;
	case 7: return pad->rightLatch;
	case 8: return pad->buttons >> PAD_A & 1;
	case 9: return pad->buttons >> PAD_X & 1;
	case 10: return pad->buttons >> PAD_L & 1;
	case 11: return pad->buttons >> PAD_R & 1;
	// The device signature.
	case 12:
	case 13:
	case 14:
	case 15:
		return 0;
	}

	pad->counter = 16;
	return 1;
}

void controllerLatch(int port, bool data) {
	struct Pad* pad = &input.pad[port];
	if (pad->latched == data) {
		return;
	}
	pad->latched = data;
	pad->counter = 0;

	if (pad->latched == 0) {
		pad->buttons = inputPoll(port);
		bool up = pad->buttons >> PAD_UP & 1;
		bool down = pad->buttons >> PAD_DOWN & 1;
		bool left = pad->buttons >> PAD_LEFT & 1;
		bool right = pad->buttons >> PAD_RIGHT & 1;

		if (!(up & down)) {
			pad->yHold = 0;
			pad->upLatch = up;
			pad->downLatch = down;
		} else if (!pad->yHold) {
			pad->yHold = 1;
			bool swap = pad->upLatch;
			pad->upLatch = pad->downLatch;
			pad->downLatch = swap;
		}

		if (!(left & right)) {
			pad->xHold = 0;
			pad->leftLatch = left;
			pad->rightLatch = right;
		} else if (!pad->xHold) {
			pad->xHold = 1;
			bool swap = pad->leftLatch;
			pad->leftLatch = pad->rightLatch;
			pad->rightLatch = swap;
		}
	}
}
