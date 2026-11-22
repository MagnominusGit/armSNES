/* Copyright (c) 2026 Magnominus
 * SPDX-License-Identifier: GPL-2.0-or-later */

// What armSNES adds to the shared app (app-hooks.h has what each hook is for).

#include "app-hooks.h"
#include "rc_consoles.h"

#include <string.h>

// A Game Boy game in the Super Game Boy has no achievements here: the core gives the Super
// Nintendo's memory.
static uint32_t cheevosConsole(const uint8_t* data, size_t size) {
	static const uint8_t logo[4] = { 0xce, 0xed, 0x66, 0x66 };
	bool const gameBoy = size >= 0x150 && !memcmp(data + 0x104, logo, sizeof(logo));
	return gameBoy ? RC_CONSOLE_UNKNOWN : RC_CONSOLE_SUPER_NINTENDO;
}

// The core's: the app takes a larger picture during a game as it comes (see libretro.c).
extern bool libretroAnySize;

static void start(void) {
	libretroAnySize = true;
}

const struct AppHooks appHooks = {
	.start = start,
	.cheevosConsole = cheevosConsole,
};
