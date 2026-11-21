/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum { COMPOSITE_OFF, COMPOSITE, COMPOSITE_SVIDEO };

#define COMPOSITE_SCALE 3
#define COMPOSITE_MOST 512

// A line of RGB565 pixels, `columns` of the console's (two pixels each when hires), decoded into
// COMPOSITE_SCALE pixels a column. `line` counts from 0 down the picture; `shift` is the frame's
// carrier phase, in lines.
void compositeLine(int mode, bool pal, unsigned line, unsigned shift, const uint16_t* in, unsigned columns, bool hires,
                   uint16_t* out);
