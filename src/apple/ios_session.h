// iOS audio session profiles (ADR D6) and the output delay (ADR risk 5).
#ifndef TSNX_APPLE_IOS_SESSION_H_
#define TSNX_APPLE_IOS_SESSION_H_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tsnx {

enum class SessionProfile { kNone, kMedia, kCommunication };

// Sets the shared AVAudioSession for a profile and activates it.
//   kMedia: AVAudioSessionCategoryPlayback. Media volume and routing.
//   kCommunication: PlayAndRecord, VoiceChat mode, speaker by default,
//     Bluetooth (HFP and A2DP) and AirPlay allowed.
// Returns false and logs if iOS refuses.
bool SetSessionProfile(SessionProfile profile);

// Deactivates the session so other apps' audio resumes.
void DeactivateSession();

// AVAudioSession.outputLatency + IOBufferDuration, in nanoseconds.
int64_t SessionOutputDelayNs();

// Routes as (id, name). Outputs: "default" (the current route) and
// "speaker". Inputs: "default" and AVAudioSession.availableInputs by UID.
std::vector<std::pair<std::string, std::string>> SessionOutputs();
std::vector<std::pair<std::string, std::string>> SessionInputs();
// "speaker" overrides the output to the built-in speaker; "default" removes
// the override. Returns false for an unknown id.
bool SessionSelectOutput(const std::string& id);
bool SessionSelectInput(const std::string& id);

}  // namespace tsnx

#endif  // TSNX_APPLE_IOS_SESSION_H_
