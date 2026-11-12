/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "snes.h"

#include <string.h>

// SHA-256 (FIPS 180-4), for knowing a dump by the hash ares's database gives it.

static const u32 roundConstants[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static u32 rotate(u32 x, int n) {
	return x >> n | x << (32 - n);
}

static void block(u32 h[8], const u8* p) {
	u32 w[64];
	for (int i = 0; i < 16; i++) {
		w[i] = (u32) p[i * 4] << 24 | p[i * 4 + 1] << 16 | p[i * 4 + 2] << 8 | p[i * 4 + 3];
	}
	for (int i = 16; i < 64; i++) {
		u32 s0 = rotate(w[i - 15], 7) ^ rotate(w[i - 15], 18) ^ w[i - 15] >> 3;
		u32 s1 = rotate(w[i - 2], 17) ^ rotate(w[i - 2], 19) ^ w[i - 2] >> 10;
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
	for (int i = 0; i < 64; i++) {
		u32 t1 = k + (rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)) + ((e & f) ^ (~e & g)) + roundConstants[i] + w[i];
		u32 t2 = (rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
		k = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	h[0] += a;
	h[1] += b;
	h[2] += c;
	h[3] += d;
	h[4] += e;
	h[5] += f;
	h[6] += g;
	h[7] += k;
}

// The hash of data as 64 lowercase hex digits.
void sha256Hex(const u8* data, size_t size, char out[65]) {
	u32 h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
	size_t n = 0;
	for (; n + 64 <= size; n += 64) {
		block(h, data + n);
	}
	u8 last[128] = { 0 };
	size_t rest = size - n;
	memcpy(last, data + n, rest);
	last[rest] = 0x80;
	size_t length = rest + 9 <= 64 ? 64 : 128;
	u64 bits = (u64) size * 8;
	for (int i = 0; i < 8; i++) {
		last[length - 1 - i] = bits >> (i * 8);
	}
	block(h, last);
	if (length == 128) {
		block(h, last + 64);
	}
	static const char digits[] = "0123456789abcdef";
	for (int i = 0; i < 32; i++) {
		u8 byte = h[i / 4] >> (24 - i % 4 * 8);
		out[i * 2] = digits[byte >> 4];
		out[i * 2 + 1] = digits[byte & 15];
	}
	out[64] = 0;
}
