/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "gambatte.h"

#include <cstring>

// The Super Game Boy's Game Boy: Gambatte, with the ICD2 in place of its own SGB.

extern "C" {
#include "gb.h"
}

namespace {

gambatte::GB* gb;
gambatte::uint_least32_t pixels[160 * 144];
// Gambatte may write up to 2064 samples past what it was asked for.
gambatte::uint_least32_t sound[GB_MAX_DOTS / 2 + 2064];

unsigned joyp(void*, unsigned p14, unsigned p15) {
	return icdJoypWrite(p14, p15);
}

void line(void*, unsigned ly, gambatte::uint_least32_t const* row) {
	icdLine(ly, reinterpret_cast<const uint32_t*>(row));
}

}

extern "C" {

bool gbLoad(const unsigned char* rom, size_t size, const unsigned char* boot, size_t bootSize) {
	gbUnload();
	gb = new gambatte::GB;
	unsigned const flags = gambatte::GB::SGB_MODE | gambatte::GB::READONLY_SAV;
	if (gb->load(reinterpret_cast<const char*>(rom), size, flags) != gambatte::LOADRES_OK ||
	    gb->loadBios(reinterpret_cast<const char*>(boot), bootSize) < 0) {
		gbUnload();
		return false;
	}
	gb->setIcd(joyp, line, 0);
	gb->reset(0);
	return true;
}

void gbUnload(void) {
	delete gb;
	gb = 0;
}

bool gbLoaded(void) {
	return gb != 0;
}

void gbReset(void) {
	gb->reset(0);
}

unsigned gbRun(unsigned dots, unsigned* samples, const unsigned** audio) {
	std::size_t count;
	// A run can stop at the end of a frame before any time passed.
	do {
		count = dots / 2 ? dots / 2 : 1;
		gb->runFor(pixels, 160, sound, count);
	} while (!count);
	gb->updateVideo();
	*samples = count;
	*audio = reinterpret_cast<const unsigned*>(sound);
	return count * 2;
}

unsigned char* gbSaveRam(unsigned* size) {
	unsigned char* data = 0;
	int length = 0;
	if (!gb || !gb->getMemoryArea(3, &data, &length) || length <= 0) {
		*size = 0;
		return 0;
	}
	*size = length;
	return data;
}

// Gambatte's state, then its picture: a line drawn in part goes on from the pixels already there.
size_t gbStateSize(void) {
	return gb ? gb->exactStateSize() + sizeof(pixels) : 0;
}

void gbStateSave(void* data) {
	std::size_t const size = gb->exactStateSize();
	gb->saveExactState(static_cast<char*>(data));
	std::memcpy(static_cast<char*>(data) + size, pixels, sizeof(pixels));
}

bool gbStateLoad(const void* data, size_t size) {
	std::size_t const exact = gb->exactStateSize();
	if (size != exact + sizeof(pixels)) {
		return false;
	}
	std::memcpy(pixels, static_cast<const char*>(data) + exact, sizeof(pixels));
	return gb->loadExactState(static_cast<const char*>(data), exact);
}

}
