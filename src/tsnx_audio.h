// telosnex_audio C API. Dart binds these with @Native (lib/src/ffi.dart).
#ifndef TSNX_AUDIO_H_
#define TSNX_AUDIO_H_

#include <stdint.h>

#if defined(_WIN32)
#define TSNX_EXPORT __declspec(dllexport)
#else
#define TSNX_EXPORT __attribute__((visibility("default")))
#endif

#define TSNX_VERSION_STRING "0.1.0"

#ifdef __cplusplus
extern "C" {
#endif

TSNX_EXPORT const char* tsnx_version(void);

#ifdef __cplusplus
}
#endif

#endif  // TSNX_AUDIO_H_
