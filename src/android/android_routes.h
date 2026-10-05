// Android audio routes (ADR D6, D15): the device list, the route IDs, and
// what a selection does. Pure C++, so the native tests run it on every
// host. android_jni.cc gets the device list from AudioManager.
//
// Route IDs are the flutter_webrtc IDs:
//   outputs: default, speaker, earpiece, wired-headset, usb, bluetooth
//   inputs:  default, microphone[-<address>], wired-headset, usb, bluetooth
// The earpiece and the Bluetooth microphone need communication mode. One
// communication device carries both directions, so a Bluetooth microphone
// also moves the output to the headset.
#ifndef TSNX_ANDROID_ANDROID_ROUTES_H_
#define TSNX_ANDROID_ANDROID_ROUTES_H_

#include <string>
#include <vector>

#include "device_info.h"

namespace tsnx::android_routes {

// AudioDeviceInfo.TYPE_* values.
enum DeviceType : int {
  kEarpiece = 1,
  kSpeaker = 2,
  kWiredHeadset = 3,
  kWiredHeadphones = 4,
  kBluetoothSco = 7,
  kBluetoothA2dp = 8,
  kUsbDevice = 11,
  kUsbAccessory = 12,
  kBuiltinMic = 15,
  kUsbHeadset = 22,
  kHearingAid = 23,
  kBleHeadset = 26,
  kBleSpeaker = 27,
};

// One AudioDeviceInfo. A device with both directions is two entries.
struct RawDevice {
  int id = 0;  // AudioDeviceInfo.getId(), also the AAudio device ID
  int type = 0;
  bool sink = false;  // an output
  std::string name;   // getProductName()
  std::string address;
};

// Parses the records of AudioRoutes.devices():
// "id\ttype\tsink\tname\taddress\n" for each device.
std::vector<RawDevice> ParseDevices(const std::string& text);

struct Route {
  std::string id;
  std::string name;
};

struct RouteLists {
  std::vector<Route> outputs;
  std::vector<Route> inputs;
};

// The first entry of each list is "default". Its name is the device that
// Android picks for media output or for the microphone.
// `debug_routes` adds the output "debug-speaker-call": the speaker in
// communication mode, so a device without an earpiece or a Bluetooth
// headset (the emulator) can test that mode. The Android device sets it
// from the system property debug.tsnx.routes=1.
RouteLists ListRoutes(const std::vector<RawDevice>& devices,
                      bool debug_routes = false);

// The selected route IDs.
struct Selection {
  std::string output = "default";
  std::string input = "default";
  bool operator==(const Selection&) const = default;
};

// Selects a route in one direction. The other direction changes when the
// two need different communication devices: the last selection wins.
Selection SelectOutput(Selection selection, const std::string& id);
Selection SelectInput(Selection selection, const std::string& id);

// What the device does for a selection. A selected route that is not in
// `devices` (for example a headset that turned off) acts as "default".
struct Plan {
  // MODE_IN_COMMUNICATION with this communication device (an output), and
  // output with USAGE_VOICE_COMMUNICATION. 0: MODE_NORMAL and USAGE_MEDIA.
  int communication_device = 0;
  int communication_type = 0;
  int output_device = 0;  // AAudio device ID; 0 lets Android choose
  int input_device = 0;
  bool communication() const { return communication_device != 0; }
  bool operator==(const Plan&) const = default;
};

Plan MakePlan(const Selection& selection, const std::vector<RawDevice>& devices);

// The route in use for a selection (Engine::CurrentDevice): the selected
// route, or "default" when the selection is "default" or its device is
// gone. The name and kind are those of the device that plays or records;
// for "default", the device Android picks (the name of the "default"
// entry of ListRoutes).
DeviceInfo CurrentOutput(const Selection& selection,
                         const std::vector<RawDevice>& devices);
DeviceInfo CurrentInput(const Selection& selection,
                        const std::vector<RawDevice>& devices);

}  // namespace tsnx::android_routes

#endif  // TSNX_ANDROID_ANDROID_ROUTES_H_
