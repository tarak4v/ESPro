/*
 * minimp3 implementation — compiled once as a translation unit.
 * The header is auto-downloaded by CMakeLists.txt.
 *
 * Public-domain (CC0) MP3 decoder by lieff:
 * https://github.com/lieff/minimp3
 */

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD       /* Xtensa has no SSE/NEON */
#define MINIMP3_ONLY_MP3      /* We only need Layer-3 */
#include "minimp3.h"
