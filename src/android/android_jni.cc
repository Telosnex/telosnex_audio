#include "android/android_jni.h"

#include <jni.h>

#include <atomic>

#include "rtc_base/logging.h"

namespace tsnx::android_jni {
namespace {

std::atomic<JavaVM*> g_vm{nullptr};
jclass g_routes = nullptr;  // global ref, set before g_vm
jmethodID g_devices = nullptr;
jmethodID g_set_communication = nullptr;
std::atomic<uint32_t> g_generation{1};

void JNICALL DevicesChanged(JNIEnv*, jclass) {
  g_generation.fetch_add(1, std::memory_order_relaxed);
}

// The JNIEnv of this thread. Attaches for the scope if needed: the device
// thread is a native thread, and it must not exit attached.
class Env {
 public:
  Env() : vm_(g_vm.load(std::memory_order_acquire)) {
    if (!vm_) return;
    void* env = nullptr;
    const jint rc = vm_->GetEnv(&env, JNI_VERSION_1_6);
    if (rc == JNI_OK) {
      env_ = static_cast<JNIEnv*>(env);
    } else if (rc == JNI_EDETACHED &&
               vm_->AttachCurrentThread(&env_, nullptr) == JNI_OK) {
      attached_ = true;
    } else {
      env_ = nullptr;
    }
  }
  ~Env() {
    if (attached_) vm_->DetachCurrentThread();
  }
  JNIEnv* get() const { return env_; }

  // Logs and clears a pending Java exception. True if there was one.
  bool Failed(const char* what) const {
    if (!env_->ExceptionCheck()) return false;
    env_->ExceptionDescribe();
    env_->ExceptionClear();
    RTC_LOG(LS_ERROR) << "AudioRoutes." << what << " threw";
    return true;
  }

 private:
  JavaVM* vm_;
  JNIEnv* env_ = nullptr;
  bool attached_ = false;
};

}  // namespace

bool Available() { return g_vm.load(std::memory_order_acquire) != nullptr; }

std::string Devices() {
  Env env;
  if (!env.get()) return std::string();
  auto s = static_cast<jstring>(
      env.get()->CallStaticObjectMethod(g_routes, g_devices));
  if (env.Failed("devices") || !s) return std::string();
  const char* chars = env.get()->GetStringUTFChars(s, nullptr);
  std::string out = chars ? chars : "";
  if (chars) env.get()->ReleaseStringUTFChars(s, chars);
  env.get()->DeleteLocalRef(s);
  return out;
}

bool SetCommunication(int device_id, int type, int timeout_ms) {
  Env env;
  if (!env.get()) return device_id == 0;
  const jboolean ok = env.get()->CallStaticBooleanMethod(
      g_routes, g_set_communication, device_id, type, timeout_ms);
  if (env.Failed("setCommunication")) return false;
  return ok == JNI_TRUE;
}

uint32_t DevicesGeneration() {
  return g_generation.load(std::memory_order_relaxed);
}

}  // namespace tsnx::android_jni

// Runs when AudioRoutes calls System.loadLibrary("telosnex_audio"), with
// that class loader, so FindClass sees the app's classes.
extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
  using namespace tsnx::android_jni;
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK)
    return JNI_VERSION_1_6;
  jclass c = env->FindClass("com/telosnex/audio/AudioRoutes");
  if (!c) {
    env->ExceptionClear();
    RTC_LOG(LS_WARNING) << "com.telosnex.audio.AudioRoutes not found";
    return JNI_VERSION_1_6;
  }
  g_devices = env->GetStaticMethodID(c, "devices", "()Ljava/lang/String;");
  g_set_communication =
      env->GetStaticMethodID(c, "setCommunication", "(III)Z");
  static const JNINativeMethod kNatives[] = {
      {"nativeDevicesChanged", "()V",
       reinterpret_cast<void*>(&DevicesChanged)}};
  if (!g_devices || !g_set_communication ||
      env->RegisterNatives(c, kNatives, 1) != JNI_OK) {
    env->ExceptionClear();
    RTC_LOG(LS_ERROR) << "AudioRoutes does not match the native library";
    return JNI_VERSION_1_6;
  }
  g_routes = static_cast<jclass>(env->NewGlobalRef(c));
  env->DeleteLocalRef(c);
  g_vm.store(vm, std::memory_order_release);
  // An engine that opened before this reads the device list now.
  g_generation.fetch_add(1, std::memory_order_relaxed);
  return JNI_VERSION_1_6;
}
