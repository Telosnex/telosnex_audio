// The part of Dart_CObject (dart_native_api.h) that the engine posts to a
// Dart port. The Dart DL API major version (2) fixes this layout; Dart
// checks the version before it gives the engine a port.
#ifndef TSNX_UTIL_DART_COBJECT_H_
#define TSNX_UTIL_DART_COBJECT_H_

#include <cstdint>

namespace tsnx {

// Dart_CObject_Type values.
inline constexpr int32_t kDartCObjectNull = 0;
inline constexpr int32_t kDartCObjectInt64 = 3;
inline constexpr int32_t kDartCObjectArray = 6;

struct DartCObject {
  int32_t type;
  union {
    int64_t as_int64;
    struct {
      intptr_t length;
      DartCObject** values;
    } as_array;
    // Dart_CObject's largest member (as_external_typed_data).
    void* size[5];
  } value;
};

}  // namespace tsnx

#endif  // TSNX_UTIL_DART_COBJECT_H_
