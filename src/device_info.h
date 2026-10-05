// An audio device in the engine's lists, and the kind of a device
// (ADR D15). Shared by the engine and the platform route code.
#ifndef TSNX_DEVICE_INFO_H_
#define TSNX_DEVICE_INFO_H_

#include <cstdint>
#include <string>

namespace tsnx {

// The values are the C API's TSNX_DEVICE_KIND_*.
enum class DeviceKind : int32_t {
  kOther = 0,
  kSpeaker = 1,     // the built-in speaker
  kEarpiece = 2,    // the phone earpiece
  kMicrophone = 3,  // a built-in microphone
  kWired = 4,       // wired headphones or a wired headset
  kUsb = 5,
  kBluetooth = 6,
  kAirPlay = 7,
};

struct DeviceInfo {
  std::string id;
  std::string name;
  // Set only for the current device (Engine::CurrentDevice).
  DeviceKind kind = DeviceKind::kOther;
  bool operator==(const DeviceInfo&) const = default;
};

}  // namespace tsnx

#endif  // TSNX_DEVICE_INFO_H_
