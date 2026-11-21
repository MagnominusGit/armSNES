#ifndef CRC32_H
#define CRC32_H

#ifdef HAVE_ZLIB_H
#include "gbint.h"
#include <zlib.h>
#else

#include <cstring>

namespace gambatte {

gb_ulong crc32(gb_ulong crc, void const *buf, std::size_t size);

}

#endif

#endif
