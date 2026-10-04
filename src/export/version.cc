// Version symbol. The rest of the C API is in src/c_api.cc (tsnx_core);
// the exported-symbols list keeps it in the link.
#include "tsnx_audio.h"

extern "C" TSNX_EXPORT const char* tsnx_version(void) { return TSNX_VERSION_STRING; }
