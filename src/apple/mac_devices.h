// macOS CoreAudio details for the engine's device lists (ADR D15).
#ifndef TSNX_APPLE_MAC_DEVICES_H_
#define TSNX_APPLE_MAC_DEVICES_H_

#include <cstdint>
#include <string>

#include "device_info.h"

namespace tsnx {

// The name and kind of the device behind an engine list ID: "default" (the
// system default device), a CoreAudio AudioDeviceID in decimal (the
// CoreAudio ADM), or a device UID (the AVAudioEngine ADM). False if the
// device is not found.
bool MacDeviceDetails(const std::string& id, bool input, std::string* name,
                      DeviceKind* kind);

// Moves when a device comes or goes or a default device changes.
uint32_t MacDevicesGeneration();

}  // namespace tsnx

#endif  // TSNX_APPLE_MAC_DEVICES_H_
