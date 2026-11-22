/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "zip.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static uint32_t le16(const unsigned char* p) {
	return p[0] | p[1] << 8;
}

static uint32_t le32(const unsigned char* p) {
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t) p[3] << 24;
}

// The central directory's offset and size, from its end record (within the last 64 KiB and 22 bytes:
// a comment may follow it).
static int findDirectory(FILE* file, long* offset, long* size, long* entries) {
	if (fseek(file, 0, SEEK_END)) {
		return 0;
	}
	long length = ftell(file);
	long back = length < 65536 + 22 ? length : 65536 + 22;
	unsigned char* tail = malloc(back);
	if (!tail || fseek(file, length - back, SEEK_SET) || fread(tail, 1, back, file) != (size_t) back) {
		free(tail);
		return 0;
	}
	int found = 0;
	for (long at = back - 22; at >= 0; --at) {
		if (le32(tail + at) == 0x06054b50) {
			*entries = le16(tail + at + 10);
			*size = le32(tail + at + 12);
			*offset = le32(tail + at + 16);
			found = 1;
			break;
		}
	}
	free(tail);
	return found;
}

struct ZipFile {
	FILE* file;
	// Where the data starts, how it's packed, and its sizes.
	long start;
	int deflated;
	uint32_t packed, size;
	// What has been inflated so far (all of it, for a stored file, read as asked).
	unsigned char* data;
	size_t ready;
	uint32_t taken;
	z_stream z;
	unsigned char in[1 << 15];
};

struct ZipFile* zipOpen(const char* archive, const char* name, size_t* size) {
	FILE* file = fopen(archive, "rb");
	if (!file) {
		return NULL;
	}
	struct ZipFile* zip = NULL;
	unsigned char* directory = NULL;
	long dirOffset, dirSize, entries;
	if (!findDirectory(file, &dirOffset, &dirSize, &entries) || !(directory = malloc(dirSize ? dirSize : 1)) ||
	    fseek(file, dirOffset, SEEK_SET) || fread(directory, 1, dirSize, file) != (size_t) dirSize) {
		goto done;
	}
	size_t nameLength = strlen(name);
	const unsigned char* entry = NULL;
	for (long at = 0, n = 0; n < entries && at + 46 <= dirSize; ++n) {
		const unsigned char* e = directory + at;
		if (le32(e) != 0x02014b50) {
			break;
		}
		uint32_t length = le16(e + 28);
		if (at + 46 + length > dirSize) {
			break;
		}
		if (length == nameLength && !memcmp(e + 46, name, length)) {
			entry = e;
			break;
		}
		at += 46 + length + le16(e + 30) + le16(e + 32);
	}
	unsigned char local[30];
	uint32_t method = entry ? le16(entry + 10) : ~0u;
	if (!entry || (method != 0 && method != 8) || fseek(file, le32(entry + 42), SEEK_SET) || fread(local, 1, 30, file) != 30 ||
	    le32(local) != 0x04034b50 || !(zip = calloc(1, sizeof(*zip)))) {
		goto done;
	}
	zip->file = file;
	zip->start = le32(entry + 42) + 30 + le16(local + 26) + le16(local + 28);
	zip->deflated = method == 8;
	zip->packed = le32(entry + 20);
	zip->size = le32(entry + 24);
	// A deflated file's whole size at once: pages are only taken as they're filled.
	if (zip->deflated && (!(zip->data = malloc(zip->size ? zip->size : 1)) || inflateInit2(&zip->z, -MAX_WBITS) != Z_OK)) {
		free(zip->data);
		free(zip);
		zip = NULL;
		goto done;
	}
	*size = zip->size;
	file = NULL;
done:
	free(directory);
	if (file) {
		fclose(file);
	}
	return zip;
}

// Inflates on to at least end.
static void inflateTo(struct ZipFile* zip, size_t end) {
	while (zip->ready < end && zip->ready < zip->size) {
		if (!zip->z.avail_in) {
			uint32_t chunk = zip->packed - zip->taken < sizeof(zip->in) ? zip->packed - zip->taken : sizeof(zip->in);
			if (!chunk || fseek(zip->file, zip->start + zip->taken, SEEK_SET) || fread(zip->in, 1, chunk, zip->file) != chunk) {
				return;
			}
			zip->taken += chunk;
			zip->z.next_in = zip->in;
			zip->z.avail_in = chunk;
		}
		zip->z.next_out = zip->data + zip->ready;
		zip->z.avail_out = zip->size - zip->ready;
		int status = inflate(&zip->z, Z_SYNC_FLUSH);
		zip->ready = zip->size - zip->z.avail_out;
		if (status != Z_OK && status != Z_BUF_ERROR) {
			// The end, or damaged data: no more than this.
			zip->packed = zip->taken;
			zip->z.avail_in = 0;
			return;
		}
	}
}

size_t zipRead(struct ZipFile* zip, size_t offset, unsigned char* to, size_t bytes) {
	if (offset >= zip->size) {
		return 0;
	}
	size_t end = zip->size - offset < bytes ? zip->size : offset + bytes;
	if (!zip->deflated) {
		if (fseek(zip->file, zip->start + offset, SEEK_SET)) {
			return 0;
		}
		return fread(to, 1, end - offset, zip->file);
	}
	if (end > zip->ready) {
		// Ahead in steps, not a read's few bytes at a time.
		inflateTo(zip, end + (1 << 16));
	}
	if (end > zip->ready) {
		end = zip->ready > offset ? zip->ready : offset;
	}
	memcpy(to, zip->data + offset, end - offset);
	return end - offset;
}

void zipClose(struct ZipFile* zip) {
	if (!zip) {
		return;
	}
	if (zip->deflated) {
		inflateEnd(&zip->z);
	}
	free(zip->data);
	fclose(zip->file);
	free(zip);
}
