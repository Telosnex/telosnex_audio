#include "apple/mac_devices.h"

#include <TargetConditionals.h>

#if TARGET_OS_OSX
#import <CoreAudio/CoreAudio.h>
#include <dispatch/dispatch.h>

#include <atomic>
#include <cctype>
#include <cstdlib>

namespace tsnx {
namespace {

AudioObjectPropertyAddress Address(
    AudioObjectPropertySelector selector,
    AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
  return {selector, scope, kAudioObjectPropertyElementMain};
}

AudioDeviceID DefaultDevice(bool input) {
  const auto a = Address(input ? kAudioHardwarePropertyDefaultInputDevice
                               : kAudioHardwarePropertyDefaultOutputDevice);
  AudioDeviceID id = kAudioObjectUnknown;
  UInt32 size = sizeof(id);
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr,
                                 &size, &id) != noErr)
    return kAudioObjectUnknown;
  return id;
}

AudioDeviceID DeviceForUid(const std::string& uid) {
  CFStringRef ref = CFStringCreateWithCString(nullptr, uid.c_str(),
                                              kCFStringEncodingUTF8);
  if (!ref) return kAudioObjectUnknown;
  AudioDeviceID id = kAudioObjectUnknown;
  const auto a = Address(kAudioHardwarePropertyTranslateUIDToDevice);
  UInt32 size = sizeof(id);
  const OSStatus st = AudioObjectGetPropertyData(
      kAudioObjectSystemObject, &a, sizeof(ref), &ref, &size, &id);
  CFRelease(ref);
  return st == noErr ? id : kAudioObjectUnknown;
}

AudioDeviceID Resolve(const std::string& id, bool input) {
  if (id == "default") return DefaultDevice(input);
  bool digits = !id.empty();
  for (char c : id) digits &= std::isdigit(static_cast<unsigned char>(c));
  if (digits)
    return static_cast<AudioDeviceID>(std::strtoul(id.c_str(), nullptr, 10));
  return DeviceForUid(id);
}

bool Name(AudioDeviceID device, std::string* out) {
  const auto a = Address(kAudioObjectPropertyName);
  CFStringRef name = nullptr;
  UInt32 size = sizeof(name);
  if (AudioObjectGetPropertyData(device, &a, 0, nullptr, &size, &name) !=
          noErr ||
      !name)
    return false;
  char buf[512] = {};
  const bool ok =
      CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
  CFRelease(name);
  if (ok) *out = buf;
  return ok;
}

DeviceKind Kind(AudioDeviceID device, bool input) {
  const auto t = Address(kAudioDevicePropertyTransportType);
  UInt32 transport = 0;
  UInt32 size = sizeof(transport);
  if (AudioObjectGetPropertyData(device, &t, 0, nullptr, &size, &transport) !=
      noErr)
    return DeviceKind::kOther;
  switch (transport) {
    case kAudioDeviceTransportTypeBuiltIn: {
      // The headphone jack: data source 'hdpn' (out) or 'emic' (in).
      const auto d = Address(kAudioDevicePropertyDataSource,
                             input ? kAudioObjectPropertyScopeInput
                                   : kAudioObjectPropertyScopeOutput);
      UInt32 source = 0;
      size = sizeof(source);
      const bool has = AudioObjectGetPropertyData(device, &d, 0, nullptr,
                                                  &size, &source) == noErr;
      if (has && (source == 'hdpn' || source == 'emic'))
        return DeviceKind::kWired;
      return input ? DeviceKind::kMicrophone : DeviceKind::kSpeaker;
    }
    case kAudioDeviceTransportTypeUSB:
      return DeviceKind::kUsb;
    case kAudioDeviceTransportTypeBluetooth:
    case kAudioDeviceTransportTypeBluetoothLE:
      return DeviceKind::kBluetooth;
    case kAudioDeviceTransportTypeAirPlay:
      return DeviceKind::kAirPlay;
    default:
      return DeviceKind::kOther;
  }
}

std::atomic<uint32_t> g_generation{0};

}  // namespace

bool MacDeviceDetails(const std::string& id, bool input, std::string* name,
                      DeviceKind* kind) {
  const AudioDeviceID device = Resolve(id, input);
  if (device == kAudioObjectUnknown) return false;
  std::string n;
  if (!Name(device, &n)) return false;
  *name = n;
  *kind = Kind(device, input);
  return true;
}

uint32_t MacDevicesGeneration() {
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    // Never removed: the listener only moves a counter.
    AudioObjectPropertyListenerBlock bump =
        ^(UInt32, const AudioObjectPropertyAddress*) {
          g_generation.fetch_add(1, std::memory_order_relaxed);
        };
    dispatch_queue_t queue = dispatch_get_global_queue(QOS_CLASS_UTILITY, 0);
    for (AudioObjectPropertySelector s :
         {kAudioHardwarePropertyDevices,
          kAudioHardwarePropertyDefaultOutputDevice,
          kAudioHardwarePropertyDefaultInputDevice}) {
      const auto a = Address(s);
      AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &a, queue,
                                          bump);
    }
  });
  return g_generation.load(std::memory_order_relaxed);
}

}  // namespace tsnx

#endif  // TARGET_OS_OSX
