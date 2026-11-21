//
//   Copyright (C) 2007 by sinamas <sinamas at users.sourceforge.net>
//
//   This program is free software; you can redistribute it and/or modify
//   it under the terms of the GNU General Public License version 2 as
//   published by the Free Software Foundation.
//
//   This program is distributed in the hope that it will be useful,
//   but WITHOUT ANY WARRANTY; without even the implied warranty of
//   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//   GNU General Public License version 2 for more details.
//
//   You should have received a copy of the GNU General Public License
//   version 2 along with this program; if not, write to the
//   Free Software Foundation, Inc.,
//   51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA.
//

#ifndef GAMBATTE_INT_H
#define GAMBATTE_INT_H

// armGBA: Gambatte's long and unsigned long, 32 bits on every build. Its counters wrap at 32 bits,
// as they do where long is 32 bits (32-bit ARM, Windows); with a 64-bit long, times that have gone
// negative compare and save differently, and a 32-bit and a 64-bit device in netplay fall out of
// step.
typedef int gb_long;
typedef unsigned gb_ulong;

#ifdef HAVE_CSTDINT

#include <cstdint>

namespace gambatte {
using std::uint_least32_t;
using std::uint_least16_t;
}

#elif defined(HAVE_STDINT_H)

#include <stdint.h>

namespace gambatte {
using ::uint_least32_t;
using ::uint_least16_t;
}

#else

namespace gambatte {
#ifdef CHAR_LEAST_32
typedef unsigned char uint_least32_t;
#elif defined(SHORT_LEAST_32)
typedef unsigned short uint_least32_t;
#elif defined(INT_LEAST_32)
typedef unsigned uint_least32_t;
#else
typedef unsigned long uint_least32_t;
#endif

#ifdef CHAR_LEAST_16
typedef unsigned char uint_least16_t;
#else
typedef unsigned short uint_least16_t;
#endif
}

#endif

#endif
