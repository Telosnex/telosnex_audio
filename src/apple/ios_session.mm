#include "apple/ios_session.h"

#include <TargetConditionals.h>

#if TARGET_OS_IPHONE
#import <AVFoundation/AVFoundation.h>

#include <atomic>
#include <cstdio>

namespace tsnx {

namespace {
bool g_speaker_override = false;
}  // namespace

bool SetSessionProfile(SessionProfile profile) {
  @autoreleasepool {
    AVAudioSession* session = [AVAudioSession sharedInstance];
    NSError* error = nil;
    BOOL ok = NO;
    switch (profile) {
      case SessionProfile::kNone:
        return true;
      case SessionProfile::kMedia:
        ok = [session setCategory:AVAudioSessionCategoryPlayback
                             mode:AVAudioSessionModeDefault
                          options:0
                            error:&error];
        break;
      case SessionProfile::kCommunication: {
        AVAudioSessionCategoryOptions options =
            AVAudioSessionCategoryOptionDefaultToSpeaker |
            AVAudioSessionCategoryOptionAllowBluetooth |
            AVAudioSessionCategoryOptionAllowBluetoothA2DP |
            AVAudioSessionCategoryOptionAllowAirPlay;
        ok = [session setCategory:AVAudioSessionCategoryPlayAndRecord
                             mode:AVAudioSessionModeVoiceChat
                          options:options
                            error:&error];
        break;
      }
    }
    if (ok) ok = [session setActive:YES error:&error];
    if (ok && profile == SessionProfile::kCommunication && g_speaker_override)
      [session overrideOutputAudioPort:AVAudioSessionPortOverrideSpeaker
                                 error:nil];
    if (!ok) {
      std::fprintf(stderr, "telosnex_audio: audio session profile %d: %s\n",
                   static_cast<int>(profile),
                   error.localizedDescription.UTF8String ?: "unknown error");
    }
    return ok;
  }
}

void DeactivateSession() {
  @autoreleasepool {
    NSError* error = nil;
    [[AVAudioSession sharedInstance]
          setActive:NO
        withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
              error:&error];
  }
}

int64_t SessionOutputDelayNs() {
  AVAudioSession* session = [AVAudioSession sharedInstance];
  const double s = session.outputLatency + session.IOBufferDuration;
  return static_cast<int64_t>(s * 1e9);
}

namespace {
std::string Str(NSString* s) { return s.UTF8String ? s.UTF8String : ""; }
}  // namespace

std::vector<std::pair<std::string, std::string>> SessionOutputs() {
  @autoreleasepool {
    AVAudioSession* session = [AVAudioSession sharedInstance];
    std::string route;
    for (AVAudioSessionPortDescription* p in session.currentRoute.outputs) {
      if (!route.empty()) route += ", ";
      route += Str(p.portName);
    }
    return {{"default", route.empty() ? "System default" : route},
            {"speaker", "Speaker"}};
  }
}

std::vector<std::pair<std::string, std::string>> SessionInputs() {
  @autoreleasepool {
    std::vector<std::pair<std::string, std::string>> out = {
        {"default", "System default"}};
    for (AVAudioSessionPortDescription* p in
         [AVAudioSession sharedInstance].availableInputs)
      out.emplace_back(Str(p.UID), Str(p.portName));
    return out;
  }
}

bool SessionSelectOutput(const std::string& id) {
  if (id != "default" && id != "speaker") return false;
  g_speaker_override = id == "speaker";
  NSError* error = nil;
  // The override applies only to PlayAndRecord; Playback already uses the
  // speaker unless a headset or Bluetooth route is connected.
  [[AVAudioSession sharedInstance]
      overrideOutputAudioPort:g_speaker_override
                                  ? AVAudioSessionPortOverrideSpeaker
                                  : AVAudioSessionPortOverrideNone
                        error:&error];
  return true;
}

namespace {

DeviceKind OutputKind(NSString* type) {
  if ([type isEqualToString:AVAudioSessionPortBuiltInSpeaker])
    return DeviceKind::kSpeaker;
  if ([type isEqualToString:AVAudioSessionPortBuiltInReceiver])
    return DeviceKind::kEarpiece;
  if ([type isEqualToString:AVAudioSessionPortHeadphones] ||
      [type isEqualToString:AVAudioSessionPortLineOut])
    return DeviceKind::kWired;
  if ([type isEqualToString:AVAudioSessionPortUSBAudio])
    return DeviceKind::kUsb;
  if ([type isEqualToString:AVAudioSessionPortBluetoothA2DP] ||
      [type isEqualToString:AVAudioSessionPortBluetoothHFP] ||
      [type isEqualToString:AVAudioSessionPortBluetoothLE])
    return DeviceKind::kBluetooth;
  if ([type isEqualToString:AVAudioSessionPortAirPlay])
    return DeviceKind::kAirPlay;
  return DeviceKind::kOther;
}

DeviceKind InputKind(NSString* type) {
  if ([type isEqualToString:AVAudioSessionPortBuiltInMic])
    return DeviceKind::kMicrophone;
  if ([type isEqualToString:AVAudioSessionPortHeadsetMic] ||
      [type isEqualToString:AVAudioSessionPortLineIn])
    return DeviceKind::kWired;
  if ([type isEqualToString:AVAudioSessionPortUSBAudio])
    return DeviceKind::kUsb;
  if ([type isEqualToString:AVAudioSessionPortBluetoothHFP] ||
      [type isEqualToString:AVAudioSessionPortBluetoothLE])
    return DeviceKind::kBluetooth;
  return DeviceKind::kOther;
}

std::atomic<uint32_t> g_route_generation{0};

}  // namespace

bool SessionCurrentOutput(DeviceInfo* out) {
  @autoreleasepool {
    AVAudioSessionPortDescription* port =
        [AVAudioSession sharedInstance].currentRoute.outputs.firstObject;
    if (!port) return false;
    const bool speaker =
        g_speaker_override &&
        [port.portType isEqualToString:AVAudioSessionPortBuiltInSpeaker];
    *out = {speaker ? "speaker" : "default", Str(port.portName),
            OutputKind(port.portType)};
    return true;
  }
}

bool SessionCurrentInput(DeviceInfo* out) {
  @autoreleasepool {
    AVAudioSession* session = [AVAudioSession sharedInstance];
    AVAudioSessionPortDescription* port =
        session.currentRoute.inputs.firstObject;
    if (!port) return false;
    AVAudioSessionPortDescription* preferred = session.preferredInput;
    const bool selected = preferred && [preferred.UID isEqualToString:port.UID];
    *out = {selected ? Str(port.UID) : "default", Str(port.portName),
            InputKind(port.portType)};
    return true;
  }
}

uint32_t SessionRouteGeneration() {
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    // Never removed: the observer only moves a counter.
    [[NSNotificationCenter defaultCenter]
        addObserverForName:AVAudioSessionRouteChangeNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification*) {
                  g_route_generation.fetch_add(1, std::memory_order_relaxed);
                }];
  });
  return g_route_generation.load(std::memory_order_relaxed);
}

bool SessionSelectInput(const std::string& id) {
  @autoreleasepool {
    AVAudioSession* session = [AVAudioSession sharedInstance];
    NSError* error = nil;
    if (id == "default") return [session setPreferredInput:nil error:&error];
    for (AVAudioSessionPortDescription* p in session.availableInputs) {
      if (Str(p.UID) == id) return [session setPreferredInput:p error:&error];
    }
    return false;
  }
}

}  // namespace tsnx

#endif  // TARGET_OS_IPHONE
