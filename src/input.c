/* Copyright (c) 2004-2025 ares team, Near et al
 * Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Ported from ares (ISC); see LICENSES/ISC-ares.txt. */
#include "snes.h"

// The two controller ports. Each takes a standard pad or a mouse; the second also a Super Multitap
// with four pads, a Super Scope or one or two Justifiers, whose light is taken from where the beam
// is, as ares takes it.

struct Input input;
u8 inputDevice[2] = { DEVICE_PAD, DEVICE_PAD };
bool inputGuns;

void inputPower(void) {
	memset(&input, 0, sizeof(input));
	input.gun.cx[0] = input.gun.cx[1] = -1;
	input.gun.cy[0] = input.gun.cy[1] = -1;
	inputGuns = inputDevice[1] >= DEVICE_SUPER_SCOPE;
}

void inputSetDevice(int port, int device) {
	// The guns' light is only wired to the second port's pin.
	if (port == 0 && device != DEVICE_NONE && device != DEVICE_MOUSE) {
		device = DEVICE_PAD;
	}
	inputDevice[port] = device;
	inputGuns = inputDevice[1] >= DEVICE_SUPER_SCOPE;
}

// --- The standard pad: a 16-bit shift register that takes the buttons when its latch falls ---

static u8 padData(struct Pad* pad, int which) {
	if (pad->latched) {
		pad->buttons = (pad->buttons & ~(1 << PAD_B)) | (inputPoll(which) & 1 << PAD_B);
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

static void padLatch(struct Pad* pad, int which, bool data) {
	if (pad->latched == data) {
		return;
	}
	pad->latched = data;
	pad->counter = 0;

	if (pad->latched == 0) {
		pad->buttons = inputPoll(which);
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

// --- The mouse: 32 bits, the buttons and speed, then how far it moved each way ---

static u8 mouseData(struct Mouse* mouse) {
	if (mouse->latched) {
		// A read while latched steps the speed.
		mouse->speed = (mouse->speed + 1) % 3;
		return 0;
	}

	u8 n = mouse->counter++;
	if (n < 8) {
		return 0;
	}
	switch (n) {
	case 8: return mouse->right;
	case 9: return mouse->left;
	case 10: return mouse->speed >> 1 & 1;
	case 11: return mouse->speed & 1;
	// The device signature.
	case 12:
	case 13:
	case 14:
		return 0;
	case 15: return 1;
	case 16: return mouse->dy;
	case 24: return mouse->dx;
	}
	if (n < 24) {
		return mouse->cy >> (23 - n) & 1;
	}
	if (n < 32) {
		return mouse->cx >> (31 - n) & 1;
	}

	mouse->counter = 32;
	return 1;
}

static void mouseLatch(struct Mouse* mouse, int port, bool data) {
	if (mouse->latched == data) {
		return;
	}
	mouse->latched = data;
	mouse->counter = 0;

	// Left and up are below 0.
	int x = 0, y = 0;
	bool left = false, right = false;
	inputPollMouse(port, &x, &y, &left, &right);
	mouse->left = left;
	mouse->right = right;
	mouse->dx = x < 0;
	mouse->dy = y < 0;
	if (x < 0) {
		x = -x;
	}
	if (y < 0) {
		y = -y;
	}
	// The speeds: slow, normal and fast.
	if (mouse->speed == 1) {
		x += x / 2;
		y += y / 2;
	}
	if (mouse->speed == 2) {
		x *= 2;
		y *= 2;
	}
	mouse->cx = x > 127 ? 127 : x;
	mouse->cy = y > 127 ? 127 : y;
}

// --- The light guns ---

#define G input.gun

static bool gunOff(int n) {
	return G.cx[n] < 0 || G.cy[n] < 0 || G.cx[n] >= 256 || G.cy[n] >= (s32) ppu.state.vdisp;
}

// Called as the CPU's counters move, with a gun in the second port: the beam reaching where the gun
// points strobes the port's pin, which latches the PPU's counters; a new frame takes the gun's place.
void inputBeam(u32 vcounter, u32 hcounter) {
	if (vcounter == 0 && hcounter == 0) {
		for (int n = 0; n < 2; n++) {
			int x = -1, y = -1;
			u8 buttons = 0;
			inputPollGun(n, &x, &y, &buttons);
			G.cx[n] = x;
			// The picture's first line is the counter's line 1.
			G.cy[n] = y < 0 ? -1 : y + 1;
			G.buttons[n] = buttons;
		}
		return;
	}
	int n = 0;
	if (inputDevice[1] != DEVICE_SUPER_SCOPE) {
		// One Justifier is seen on every other latch; the second, when there, on the others.
		if (G.active && inputDevice[1] == DEVICE_JUSTIFIER) {
			return;
		}
		n = G.active;
	}
	if (gunOff(n) || vcounter != (u32) G.cy[n] || hcounter != (u32) (G.cx[n] + 24) * 4) {
		return;
	}
	if (cpu.io.pio & 0x80) {
		ppuLatchCounters();
	}
	cpu.io.pio |= 0x80;
}

static u8 scopeData(void) {
	if (G.counter == 0) {
		u8 buttons = G.buttons[0];
		// Turbo is a switch, moved by a press.
		bool turbo = buttons >> GUN_TURBO & 1;
		if (turbo && !G.turboOld) {
			G.turboEdge = !G.turboEdge;
		}
		G.turboOld = turbo;

		// The trigger fires once a press, or for as long as it is held with turbo on.
		G.triggerValue = false;
		bool trigger = buttons >> GUN_TRIGGER & 1;
		if (trigger && (G.turboEdge || !G.triggerLock)) {
			G.triggerValue = true;
			G.triggerLock = true;
		} else if (!trigger) {
			G.triggerLock = false;
		}

		G.cursor = buttons >> GUN_CURSOR & 1;

		// Pause is seen once a press.
		G.pauseEdge = false;
		bool pause = buttons >> GUN_PAUSE & 1;
		if (pause && !G.pauseLock) {
			G.pauseEdge = true;
			G.pauseLock = true;
		} else if (!pause) {
			G.pauseLock = false;
		}

		G.offscreen = gunOff(0);
	}

	switch (G.counter++) {
	case 0: return G.triggerValue & !G.offscreen;
	case 1: return G.cursor;
	case 2: return G.turboEdge;
	case 3: return G.pauseEdge;
	case 4: return 0;
	case 5: return 0;
	case 6: return G.offscreen;
	// Noise.
	case 7: return 0;
	}

	if (G.counter > 8) {
		G.counter = 8;
	}
	return 1;
}

static u8 justifierData(void) {
	u8 n = G.counter++;
	if (n < 12) {
		return 0;
	}
	bool two = inputDevice[1] == DEVICE_JUSTIFIERS;
	switch (n) {
	// The device signature.
	case 12: return 1;
	case 13: return 1;
	case 14: return 1;
	case 15: return 0;
	case 16: return 0;
	case 17: return 1;
	case 18: return 0;
	case 19: return 1;
	case 20: return 0;
	case 21: return 1;
	case 22: return 0;
	case 23: return 1;
	case 24: return G.buttons[0] >> GUN_TRIGGER & 1;
	case 25: return two && (G.buttons[1] >> GUN_TRIGGER & 1);
	case 26: return G.buttons[0] >> GUN_PAUSE & 1;
	case 27: return two && (G.buttons[1] >> GUN_PAUSE & 1);
	case 28: return G.active;
	case 29: return 0;
	case 30: return 0;
	case 31: return 0;
	}

	if (G.counter > 32) {
		G.counter = 32;
	}
	return 1;
}

static void gunLatch(bool data) {
	if (G.latched == data) {
		return;
	}
	G.latched = data;
	G.counter = 0;
	// The Justifiers take turns, even when there is only one.
	if (!G.latched && inputDevice[1] != DEVICE_SUPER_SCOPE) {
		G.active = !G.active;
	}
}

// --- The ports ---

// The pad a port's own pad is for the frontend: 0, and 1 on the second port, where a Super Multitap
// has 1 to 4.
u8 controllerData(int port) {
	switch (inputDevice[port]) {
	case DEVICE_PAD: return padData(&input.pad[port], port);
	case DEVICE_MOUSE: return mouseData(&input.mouse[port]);
	case DEVICE_MULTITAP:
		// What tells a game it is there.
		if (input.tapLatched) {
			return 2;
		}
		// The port's pin picks the pair of pads.
		if (cpu.io.pio & 0x80) {
			return padData(&input.pad[1], 1) | padData(&input.pad[2], 2) << 1;
		}
		return padData(&input.pad[3], 3) | padData(&input.pad[4], 4) << 1;
	case DEVICE_SUPER_SCOPE: return scopeData();
	case DEVICE_JUSTIFIER:
	case DEVICE_JUSTIFIERS:
		return justifierData();
	}
	return 0;
}

void controllerLatch(int port, bool data) {
	switch (inputDevice[port]) {
	case DEVICE_PAD: padLatch(&input.pad[port], port, data); break;
	case DEVICE_MOUSE: mouseLatch(&input.mouse[port], port, data); break;
	case DEVICE_MULTITAP:
		input.tapLatched = data;
		for (int n = 1; n <= 4; n++) {
			padLatch(&input.pad[n], n, data);
		}
		break;
	case DEVICE_SUPER_SCOPE:
	case DEVICE_JUSTIFIER:
	case DEVICE_JUSTIFIERS:
		gunLatch(data);
		break;
	}
}
