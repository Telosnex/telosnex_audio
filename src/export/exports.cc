// The shared library's only translation unit. The core lives in tsnx_core;
// this file keeps the C API symbols alive in the link.
#include "tsnx_audio.h"

extern "C" TSNX_EXPORT const char* tsnx_version(void) { return TSNX_VERSION_STRING; }
