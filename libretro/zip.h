/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <stddef.h>

// One file in a zip archive, stored or deflated, found by its name in the central directory, and read
// at any offset: a deflated one is inflated as far as each read needs, and kept. zipOpen gives NULL if
// the archive or the file isn't there or can't be read; zipRead gives the bytes read, short at the end
// (or where the data is damaged).
struct ZipFile;
struct ZipFile* zipOpen(const char* archive, const char* name, size_t* size);
size_t zipRead(struct ZipFile* file, size_t offset, unsigned char* to, size_t bytes);
void zipClose(struct ZipFile* file);
// Whether the archive has a file whose name match takes (given the name and its length, not ended by
// a 0, and with).
int zipFind(const char* archive, int (*match)(const char* name, size_t length, const void* with), const void* with);
