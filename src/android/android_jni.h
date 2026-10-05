// The engine's calls into Java (com.telosnex.audio.AudioRoutes) for the
// Android device list and communication mode (ADR D6, D15).
//
// Dart loads the library with dlopen, so JNI_OnLoad runs only when the
// package's plugin calls System.loadLibrary (on a background thread, at
// plugin attach). Until then, and in processes without Java (the native
// test binaries), Available() is false and the engine has only the
// "default" routes. Call these on the device thread.
#ifndef TSNX_ANDROID_ANDROID_JNI_H_
#define TSNX_ANDROID_ANDROID_JNI_H_

#include <cstdint>
#include <string>

namespace tsnx::android_jni {

bool Available();

// AudioRoutes.devices(): records for android_routes::ParseDevices.
std::string Devices();

// AudioRoutes.setCommunication(). device_id 0 returns to MODE_NORMAL.
// Otherwise sets MODE_IN_COMMUNICATION and the communication device, and
// waits up to timeout_ms for Android to use it. False if Android refuses
// or does not change the route in time.
bool SetCommunication(int device_id, int type, int timeout_ms);

// Moves when Android adds or removes an audio device.
uint32_t DevicesGeneration();

}  // namespace tsnx::android_jni

#endif  // TSNX_ANDROID_ANDROID_JNI_H_
