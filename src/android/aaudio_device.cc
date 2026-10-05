#include "android/aaudio_device.h"

#include <aaudio/AAudio.h>
#include <dlfcn.h>
#include <sys/system_properties.h>
#include <time.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "android/android_jni.h"
#include "android/android_routes.h"
#include "api/array_view.h"
#include "api/audio/audio_device_defines.h"
#include "api/make_ref_counted.h"
#include "api/task_queue/pending_task_safety_flag.h"
#include "api/task_queue/task_queue_base.h"
#include "api/units/time_delta.h"
#include "modules/audio_device/audio_device_buffer.h"
#include "modules/audio_device/audio_device_generic.h"
#include "modules/audio_device/audio_device_impl.h"
#include "modules/audio_device/fine_audio_buffer.h"
#include "rtc_base/logging.h"

namespace tsnx {
namespace {

constexpr int kRate = 48000;
constexpr int kOutputChannels = 2;
constexpr int kInputChannels = 1;
// A Bluetooth headset needs about a second to open its call link.
constexpr int kCommunicationTimeoutMs = 4000;

namespace routes = android_routes;

// AAudio is API 26. The app's minSdk is 24, so the library cannot link
// libaaudio.so; the functions load at run time.
struct AAudioApi {
  aaudio_result_t (*createStreamBuilder)(AAudioStreamBuilder**);
  void (*setDirection)(AAudioStreamBuilder*, aaudio_direction_t);
  void (*setSampleRate)(AAudioStreamBuilder*, int32_t);
  void (*setChannelCount)(AAudioStreamBuilder*, int32_t);
  void (*setFormat)(AAudioStreamBuilder*, aaudio_format_t);
  void (*setSharingMode)(AAudioStreamBuilder*, aaudio_sharing_mode_t);
  void (*setPerformanceMode)(AAudioStreamBuilder*, aaudio_performance_mode_t);
  void (*setUsage)(AAudioStreamBuilder*, aaudio_usage_t);              // 28
  void (*setInputPreset)(AAudioStreamBuilder*, aaudio_input_preset_t);  // 28
  void (*setDeviceId)(AAudioStreamBuilder*, int32_t);
  void (*setDataCallback)(AAudioStreamBuilder*, AAudioStream_dataCallback,
                          void*);
  void (*setErrorCallback)(AAudioStreamBuilder*, AAudioStream_errorCallback,
                           void*);
  aaudio_result_t (*openStream)(AAudioStreamBuilder*, AAudioStream**);
  aaudio_result_t (*deleteBuilder)(AAudioStreamBuilder*);
  aaudio_result_t (*requestStart)(AAudioStream*);
  aaudio_result_t (*requestStop)(AAudioStream*);
  aaudio_result_t (*close)(AAudioStream*);
  int32_t (*getSampleRate)(AAudioStream*);
  int32_t (*getDeviceId)(AAudioStream*);
  int32_t (*getChannelCount)(AAudioStream*);
  int32_t (*getFramesPerBurst)(AAudioStream*);
  int32_t (*getBufferSizeInFrames)(AAudioStream*);
  aaudio_result_t (*setBufferSizeInFrames)(AAudioStream*, int32_t);
  int32_t (*getBufferCapacityInFrames)(AAudioStream*);
  int32_t (*getXRunCount)(AAudioStream*);
  int64_t (*getFramesWritten)(AAudioStream*);
  aaudio_result_t (*getTimestamp)(AAudioStream*, clockid_t, int64_t*,
                                  int64_t*);
  aaudio_stream_state_t (*getState)(AAudioStream*);
  const char* (*resultToText)(aaudio_result_t);
};

template <typename F>
bool Load(void* lib, const char* name, F* out, bool required = true) {
  *out = reinterpret_cast<F>(dlsym(lib, name));
  return *out != nullptr || !required;
}

const AAudioApi* Api() {
  static const AAudioApi* api = []() -> const AAudioApi* {
    void* lib = dlopen("libaaudio.so", RTLD_NOW);
    if (!lib) return nullptr;
    static AAudioApi a;
    bool ok = Load(lib, "AAudio_createStreamBuilder", &a.createStreamBuilder) &&
              Load(lib, "AAudioStreamBuilder_setDirection", &a.setDirection) &&
              Load(lib, "AAudioStreamBuilder_setSampleRate", &a.setSampleRate) &&
              Load(lib, "AAudioStreamBuilder_setChannelCount",
                   &a.setChannelCount) &&
              Load(lib, "AAudioStreamBuilder_setFormat", &a.setFormat) &&
              Load(lib, "AAudioStreamBuilder_setSharingMode",
                   &a.setSharingMode) &&
              Load(lib, "AAudioStreamBuilder_setPerformanceMode",
                   &a.setPerformanceMode) &&
              Load(lib, "AAudioStreamBuilder_setUsage", &a.setUsage, false) &&
              Load(lib, "AAudioStreamBuilder_setInputPreset",
                   &a.setInputPreset, false) &&
              Load(lib, "AAudioStreamBuilder_setDeviceId", &a.setDeviceId) &&
              Load(lib, "AAudioStreamBuilder_setDataCallback",
                   &a.setDataCallback) &&
              Load(lib, "AAudioStreamBuilder_setErrorCallback",
                   &a.setErrorCallback) &&
              Load(lib, "AAudioStreamBuilder_openStream", &a.openStream) &&
              Load(lib, "AAudioStreamBuilder_delete", &a.deleteBuilder) &&
              Load(lib, "AAudioStream_requestStart", &a.requestStart) &&
              Load(lib, "AAudioStream_requestStop", &a.requestStop) &&
              Load(lib, "AAudioStream_close", &a.close) &&
              Load(lib, "AAudioStream_getSampleRate", &a.getSampleRate) &&
              Load(lib, "AAudioStream_getDeviceId", &a.getDeviceId) &&
              Load(lib, "AAudioStream_getChannelCount", &a.getChannelCount) &&
              Load(lib, "AAudioStream_getFramesPerBurst",
                   &a.getFramesPerBurst) &&
              Load(lib, "AAudioStream_getBufferSizeInFrames",
                   &a.getBufferSizeInFrames) &&
              Load(lib, "AAudioStream_setBufferSizeInFrames",
                   &a.setBufferSizeInFrames) &&
              Load(lib, "AAudioStream_getBufferCapacityInFrames",
                   &a.getBufferCapacityInFrames) &&
              Load(lib, "AAudioStream_getXRunCount", &a.getXRunCount) &&
              Load(lib, "AAudioStream_getFramesWritten", &a.getFramesWritten) &&
              Load(lib, "AAudioStream_getTimestamp", &a.getTimestamp) &&
              Load(lib, "AAudioStream_getState", &a.getState) &&
              Load(lib, "AAudio_convertResultToText", &a.resultToText);
    return ok ? &a : nullptr;
  }();
  return api;
}

// adb shell setprop debug.tsnx.routes 1 (android_routes.h).
bool DebugRoutes() {
  char value[PROP_VALUE_MAX] = {};
  return __system_property_get("debug.tsnx.routes", value) > 0 &&
         std::strcmp(value, "1") == 0;
}

int64_t MonotonicNs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t{ts.tv_sec} * 1000000000 + ts.tv_nsec;
}

class AAudioDevice final : public webrtc::AudioDeviceGeneric,
                           public AAudioRoutes {
 public:
  explicit AAudioDevice(std::function<void()> on_output_restart)
      : on_output_restart_(std::move(on_output_restart)),
        thread_(webrtc::TaskQueueBase::Current()),
        safety_(webrtc::PendingTaskSafetyFlag::Create()) {
    out_.output = true;
  }
  ~AAudioDevice() override {
    safety_->SetNotAlive();
    Close(out_);
    Close(in_);
    SetCommunication(0);
  }

  int32_t ActiveAudioLayer(
      webrtc::AudioDeviceModule::AudioLayer& layer) const override {
    layer = webrtc::AudioDeviceModule::kAndroidAAudioAudio;
    return 0;
  }
  InitStatus Init() override {
    if (!Api()) {
      RTC_LOG(LS_ERROR) << "AAudio is not available (Android 8 or later)";
      return InitStatus::OTHER_ERROR;
    }
    return InitStatus::OK;
  }
  int32_t Terminate() override {
    StopPlayout();
    StopRecording();
    SetCommunication(0);
    return 0;
  }
  bool Initialized() const override { return Api() != nullptr; }

  // Route lists (android_routes.h). The engine calls PlayoutDevices() and
  // then RecordingDevices() and the names; all use the list that
  // PlayoutDevices() read.
  int16_t PlayoutDevices() override {
    RefreshRoutes(/*force=*/false);
    return static_cast<int16_t>(lists_.outputs.size());
  }
  int16_t RecordingDevices() override {
    return static_cast<int16_t>(lists_.inputs.size());
  }
  int32_t PlayoutDeviceName(uint16_t index,
                            char name[webrtc::kAdmMaxDeviceNameSize],
                            char guid[webrtc::kAdmMaxGuidSize]) override {
    return RouteName(lists_.outputs, index, name, guid);
  }
  int32_t RecordingDeviceName(uint16_t index,
                              char name[webrtc::kAdmMaxDeviceNameSize],
                              char guid[webrtc::kAdmMaxGuidSize]) override {
    return RouteName(lists_.inputs, index, name, guid);
  }
  int32_t SetPlayoutDevice(uint16_t index) override {
    if (index >= lists_.outputs.size()) return -1;
    return Select(routes::SelectOutput(selection_, lists_.outputs[index].id));
  }
  int32_t SetPlayoutDevice(
      webrtc::AudioDeviceModule::WindowsDeviceType) override {
    return -1;
  }
  int32_t SetRecordingDevice(uint16_t index) override {
    if (index >= lists_.inputs.size()) return -1;
    return Select(routes::SelectInput(selection_, lists_.inputs[index].id));
  }
  int32_t SetRecordingDevice(
      webrtc::AudioDeviceModule::WindowsDeviceType) override {
    return -1;
  }

  // AAudioRoutes. devices_ is as new as the last PlayoutDevices() call or
  // stream open.
  DeviceInfo CurrentOutput() override {
    return routes::CurrentOutput(selection_, devices_);
  }
  DeviceInfo CurrentInput() override {
    return routes::CurrentInput(selection_, devices_);
  }

  int32_t PlayoutIsAvailable(bool& available) override {
    available = Api() != nullptr;
    return 0;
  }
  int32_t InitPlayout() override {
    if (playing_) return -1;
    if (!out_.stream && !Open(out_)) return -1;
    return 0;
  }
  bool PlayoutIsInitialized() const override { return out_.stream; }
  int32_t RecordingIsAvailable(bool& available) override {
    available = Api() != nullptr;
    return 0;
  }
  int32_t InitRecording() override {
    if (recording_) return -1;
    if (!in_.stream && !Open(in_)) return -1;
    return 0;
  }
  bool RecordingIsInitialized() const override { return in_.stream; }

  int32_t StartPlayout() override {
    if (playing_) return 0;
    if (!out_.stream && !Open(out_)) return -1;
    if (!Start(out_)) return -1;
    playing_ = true;
    return 0;
  }
  int32_t StopPlayout() override {
    playing_ = false;
    Close(out_);
    ReleaseWhenIdle();
    return 0;
  }
  bool Playing() const override { return playing_; }
  int32_t StartRecording() override {
    if (recording_) return 0;
    if (!in_.stream && !Open(in_)) return -1;
    if (!Start(in_)) return -1;
    recording_ = true;
    return 0;
  }
  int32_t StopRecording() override {
    recording_ = false;
    Close(in_);
    ReleaseWhenIdle();
    return 0;
  }
  bool Recording() const override { return recording_; }

  int32_t InitSpeaker() override { return 0; }
  bool SpeakerIsInitialized() const override { return true; }
  int32_t InitMicrophone() override { return 0; }
  bool MicrophoneIsInitialized() const override { return true; }

  int32_t SpeakerVolumeIsAvailable(bool& available) override {
    available = false;
    return 0;
  }
  int32_t SetSpeakerVolume(uint32_t) override { return -1; }
  int32_t SpeakerVolume(uint32_t&) const override { return -1; }
  int32_t MaxSpeakerVolume(uint32_t&) const override { return -1; }
  int32_t MinSpeakerVolume(uint32_t&) const override { return -1; }
  int32_t MicrophoneVolumeIsAvailable(bool& available) override {
    available = false;
    return 0;
  }
  int32_t SetMicrophoneVolume(uint32_t) override { return -1; }
  int32_t MicrophoneVolume(uint32_t&) const override { return -1; }
  int32_t MaxMicrophoneVolume(uint32_t&) const override { return -1; }
  int32_t MinMicrophoneVolume(uint32_t&) const override { return -1; }
  int32_t SpeakerMuteIsAvailable(bool& available) override {
    available = false;
    return 0;
  }
  int32_t SetSpeakerMute(bool) override { return -1; }
  int32_t SpeakerMute(bool&) const override { return -1; }
  int32_t MicrophoneMuteIsAvailable(bool& available) override {
    available = false;
    return 0;
  }
  int32_t SetMicrophoneMute(bool) override { return -1; }
  int32_t MicrophoneMute(bool&) const override { return -1; }

  int32_t StereoPlayoutIsAvailable(bool& available) override {
    available = true;
    return 0;
  }
  int32_t SetStereoPlayout(bool enable) override { return enable ? 0 : -1; }
  int32_t StereoPlayout(bool& enabled) const override {
    enabled = true;
    return 0;
  }
  int32_t StereoRecordingIsAvailable(bool& available) override {
    available = false;
    return 0;
  }
  int32_t SetStereoRecording(bool enable) override { return enable ? -1 : 0; }
  int32_t StereoRecording(bool& enabled) const override {
    enabled = false;
    return 0;
  }

  // Risk 5: from AAudioStream_getTimestamp, updated in each output callback.
  int32_t PlayoutDelay(uint16_t& delay_ms) const override {
    delay_ms = static_cast<uint16_t>(
        out_.delay_ms.load(std::memory_order_relaxed));
    return 0;
  }

  void AttachAudioBuffer(webrtc::AudioDeviceBuffer* buffer) override {
    buffer_ = buffer;
  }

 private:
  struct Stream {
    bool output = false;
    AAudioStream* stream = nullptr;
    int rate = 0;
    int channels = 0;
    int burst = 0;
    int32_t xruns = 0;
    uint32_t generation = 0;
    int64_t last_clock_ns = 0;
    std::atomic<int> delay_ms{0};
    std::unique_ptr<webrtc::FineAudioBuffer> fine;
    AAudioDevice* owner = nullptr;
  };

  static int32_t RouteName(const std::vector<routes::Route>& list,
                           uint16_t index,
                           char name[webrtc::kAdmMaxDeviceNameSize],
                           char guid[webrtc::kAdmMaxGuidSize]) {
    if (index >= list.size()) return -1;
    std::snprintf(name, webrtc::kAdmMaxDeviceNameSize, "%s",
                  list[index].name.c_str());
    std::snprintf(guid, webrtc::kAdmMaxGuidSize, "%s", list[index].id.c_str());
    return 0;
  }

  // ---- Routes (device thread) ----

  // Reads the device list when Android reported a change (or `force`).
  // When the route for the selection changed, moves the open streams.
  // Only this changes lists_, so the engine's indexes match it.
  void RefreshRoutes(bool force) {
    const uint32_t gen = android_jni::DevicesGeneration();
    if (!force && gen == devices_gen_ && !lists_.outputs.empty()) return;
    devices_gen_ = gen;
    devices_ = routes::ParseDevices(android_jni::Devices());
    lists_ = routes::ListRoutes(devices_, DebugRoutes());
    if (out_.stream || in_.stream) ApplyPlan();
  }

  int32_t Select(const routes::Selection& next) {
    const routes::Selection prev = selection_;
    selection_ = next;
    if (ApplyPlan()) return 0;
    selection_ = prev;
    ApplyPlan();
    return -1;
  }

  // Moves the device to the plan for the selection. With no open stream,
  // only records it: Open() applies it.
  bool ApplyPlan() {
    const routes::Plan next = routes::MakePlan(selection_, devices_);
    if (!out_.stream && !in_.stream) {
      plan_ = next;
      return true;
    }
    if (next == plan_ && communication_ == next.communication_device)
      return true;
    if (!SetCommunication(next.communication_device)) return false;
    const bool mode = next.communication() != plan_.communication();
    const bool move_out = mode || next.output_device != plan_.output_device;
    const bool move_in = mode || next.input_device != plan_.input_device;
    plan_ = next;
    bool ok = true;
    if (move_out && out_.stream) ok &= Restart(out_);
    if (move_in && in_.stream) ok &= Restart(in_);
    return ok;
  }

  // Enters or leaves communication mode (Java). True when Android uses
  // `device` (0: MODE_NORMAL).
  bool SetCommunication(int device) {
    if (device == communication_) return true;
    if (!android_jni::Available()) return device == 0;
    int type = 0;
    for (const auto& d : devices_)
      if (d.id == device) type = d.type;
    const bool ok =
        android_jni::SetCommunication(device, type, kCommunicationTimeoutMs);
    // On failure Java returns to MODE_NORMAL.
    communication_ = ok ? device : 0;
    RTC_LOG(LS_INFO) << "Android communication device " << device
                     << (ok ? "" : " refused");
    return ok;
  }

  // Closes and opens a running stream with the current plan. The output
  // buffer is lost (ADR I10).
  bool Restart(Stream& st) {
    const bool running = st.output ? playing_ : recording_;
    if (st.output && running && on_output_restart_) on_output_restart_();
    Close(st);
    if (!Open(st)) {
      if (st.output) playing_ = false;
      else recording_ = false;
      return false;
    }
    if (running && !Start(st)) {
      if (st.output) playing_ = false;
      else recording_ = false;
      return false;
    }
    return true;
  }

  // Communication mode ends a second after the last stream closes: the
  // engine closes and opens the output to change a route, and the mode
  // must not drop in between.
  void ReleaseWhenIdle() {
    if (communication_ == 0 || !thread_) return;
    thread_->PostDelayedTask(webrtc::SafeTask(safety_,
                                              [this] {
                                                if (!out_.stream &&
                                                    !in_.stream)
                                                  SetCommunication(0);
                                              }),
                             webrtc::TimeDelta::Seconds(1));
  }

  bool Open(Stream& st) {
    const AAudioApi* a = Api();
    if (!a || !buffer_) return false;
    if (!out_.stream && !in_.stream) {
      // The first stream: read the devices, and enter the mode the
      // selection needs. If Android refuses, play with Android's choice.
      devices_ = routes::ParseDevices(android_jni::Devices());
      plan_ = routes::MakePlan(selection_, devices_);
      if (!SetCommunication(plan_.communication_device)) plan_ = {};
    }
    const int device = st.output ? plan_.output_device : plan_.input_device;
    AAudioStreamBuilder* b = nullptr;
    if (a->createStreamBuilder(&b) != AAUDIO_OK) return false;
    a->setDirection(b, st.output ? AAUDIO_DIRECTION_OUTPUT
                                 : AAUDIO_DIRECTION_INPUT);
    a->setSampleRate(b, kRate);
    a->setChannelCount(b, st.output ? kOutputChannels : kInputChannels);
    a->setFormat(b, AAUDIO_FORMAT_PCM_I16);
    a->setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    a->setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    // D6 on Android: media audio at all times. The mic preset has no
    // platform echo canceller (AEC3 runs in the APM). Before Android 9
    // these are the defaults.
    // In communication mode (the earpiece, a Bluetooth microphone) the
    // output follows the communication device.
    if (st.output && a->setUsage)
      a->setUsage(b, plan_.communication() ? AAUDIO_USAGE_VOICE_COMMUNICATION
                                           : AAUDIO_USAGE_MEDIA);
    if (!st.output && a->setInputPreset)
      a->setInputPreset(b, AAUDIO_INPUT_PRESET_VOICE_RECOGNITION);
    if (device != 0) a->setDeviceId(b, device);
    st.owner = this;
    a->setDataCallback(b, &AAudioDevice::DataCallback, &st);
    a->setErrorCallback(b, &AAudioDevice::ErrorCallback, &st);
    AAudioStream* s = nullptr;
    const aaudio_result_t r = a->openStream(b, &s);
    a->deleteBuilder(b);
    if (r != AAUDIO_OK) {
      RTC_LOG(LS_ERROR) << "AAudio open " << (st.output ? "output" : "input")
                        << ": " << a->resultToText(r);
      return false;
    }
    st.stream = s;
    st.rate = a->getSampleRate(s);
    st.channels = a->getChannelCount(s);
    st.burst = a->getFramesPerBurst(s);
    st.xruns = 0;
    st.last_clock_ns = 0;
    if (++st.generation == 0) ++st.generation;
    if (st.output) {
      // Two bursts: low latency. Underruns add a burst at a time.
      a->setBufferSizeInFrames(s, 2 * st.burst);
      buffer_->SetPlayoutSampleRate(st.rate);
      buffer_->SetPlayoutChannels(st.channels);
    } else {
      buffer_->SetRecordingSampleRate(st.rate);
      buffer_->SetRecordingChannels(st.channels);
      st.delay_ms.store(st.rate > 0 ? st.burst * 1000 / st.rate : 0);
    }
    st.fine = std::make_unique<webrtc::FineAudioBuffer>(buffer_);
    RTC_LOG(LS_INFO) << "AAudio " << (st.output ? "output" : "input") << ": "
                     << st.rate << " Hz, " << st.channels << " ch, burst "
                     << st.burst << ", buffer " << a->getBufferSizeInFrames(s)
                     << ", device " << a->getDeviceId(s) << " (asked "
                     << device << ")"
                     << (plan_.communication() ? ", communication" : "");
    return true;
  }

  bool Start(Stream& st) {
    const aaudio_result_t r = Api()->requestStart(st.stream);
    if (r != AAUDIO_OK) {
      RTC_LOG(LS_ERROR) << "AAudio start: " << Api()->resultToText(r);
      Close(st);
      return false;
    }
    return true;
  }

  void Close(Stream& st) {
    if (!st.stream) return;
    Api()->requestStop(st.stream);
    Api()->close(st.stream);  // waits for the callback to return
    st.stream = nullptr;
    st.fine.reset();
  }

  // Device thread. AAudio forbids closing a stream from its callbacks.
  void Reopen(bool output) {
    Stream& st = output ? out_ : in_;
    const bool running = output ? playing_ : recording_;
    if (!running) return;
    RTC_LOG(LS_WARNING) << "AAudio " << (output ? "output" : "input")
                        << " disconnected; reopening";
    // A device left or came: the plan for the selection can differ now.
    // lists_ stays as the engine last read it, so its indexes hold.
    const routes::Plan old = plan_;
    devices_ = routes::ParseDevices(android_jni::Devices());
    plan_ = routes::MakePlan(selection_, devices_);
    if (!SetCommunication(plan_.communication_device)) plan_ = {};
    Restart(st);
    // The other stream moves too if its route changed.
    Stream& other = output ? in_ : out_;
    const bool mode = plan_.communication() != old.communication();
    const bool moved = output ? plan_.input_device != old.input_device
                              : plan_.output_device != old.output_device;
    if (other.stream && (mode || moved)) Restart(other);
  }

  static aaudio_data_callback_result_t DataCallback(AAudioStream*, void* user,
                                                    void* data,
                                                    int32_t frames) {
    Stream* st = static_cast<Stream*>(user);
    return st->owner->OnData(*st, data, frames);
  }

  static void ErrorCallback(AAudioStream*, void* user, aaudio_result_t error) {
    Stream* st = static_cast<Stream*>(user);
    if (error != AAUDIO_ERROR_DISCONNECTED) return;
    AAudioDevice* self = st->owner;
    const bool output = st->output;
    if (!self->thread_) return;
    self->thread_->PostTask(webrtc::SafeTask(
        self->safety_, [self, output] { self->Reopen(output); }));
  }

  aaudio_data_callback_result_t OnData(Stream& st, void* data,
                                       int32_t frames) {
    const AAudioApi* a = Api();
    const size_t samples = static_cast<size_t>(frames) * st.channels;
    if (st.output) {
      const int32_t xruns = a->getXRunCount(st.stream);
      if (xruns > st.xruns) {
        st.xruns = xruns;
        const int32_t size = a->getBufferSizeInFrames(st.stream) + st.burst;
        if (size <= a->getBufferCapacityInFrames(st.stream))
          a->setBufferSizeInFrames(st.stream, size);
      }
      UpdateOutputDelay(st);
      st.fine->GetPlayoutData(
          webrtc::ArrayView<int16_t>(static_cast<int16_t*>(data), samples),
          st.delay_ms.load(std::memory_order_relaxed));
    } else {
      st.fine->DeliverRecordedData(
          webrtc::ArrayView<const int16_t>(static_cast<const int16_t*>(data),
                                           samples),
          st.delay_ms.load(std::memory_order_relaxed));
    }
    ObserveClock(st);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
  }

  // Time from writing the next frame to hearing it.
  void UpdateOutputDelay(Stream& st) {
    int64_t frame = 0, time_ns = 0;
    if (Api()->getTimestamp(st.stream, CLOCK_MONOTONIC, &frame, &time_ns) !=
            AAUDIO_OK ||
        st.rate <= 0)
      return;
    const int64_t next = Api()->getFramesWritten(st.stream);
    const int64_t presented =
        time_ns + (next - frame) * 1000000000 / st.rate;
    const int64_t ms = (presented - MonotonicNs()) / 1000000;
    if (ms >= 0 && ms < 1000) st.delay_ms.store(static_cast<int>(ms));
  }

  // The hardware frame position for clock correction.
  void ObserveClock(Stream& st) {
    int64_t frame = 0, time_ns = 0;
    if (Api()->getTimestamp(st.stream, CLOCK_MONOTONIC, &frame, &time_ns) !=
            AAUDIO_OK ||
        frame < 0 || time_ns == st.last_clock_ns)
      return;
    if (st.last_clock_ns != 0 && time_ns - st.last_clock_ns < 5000000) return;
    st.last_clock_ns = time_ns;
    buffer_->DeliverHardwareClockObservation(
        {st.output ? webrtc::AudioHardwareClockDirection::kPlayout
                   : webrtc::AudioHardwareClockDirection::kCapture,
         time_ns, frame, static_cast<uint32_t>(st.rate), st.generation});
  }

  const std::function<void()> on_output_restart_;
  webrtc::TaskQueueBase* const thread_;
  const webrtc::scoped_refptr<webrtc::PendingTaskSafetyFlag> safety_;
  webrtc::AudioDeviceBuffer* buffer_ = nullptr;
  Stream out_;
  Stream in_;
  bool playing_ = false;    // device thread
  bool recording_ = false;  // device thread
  // Routes (device thread).
  std::vector<routes::RawDevice> devices_;
  routes::RouteLists lists_;
  uint32_t devices_gen_ = 0;
  routes::Selection selection_;
  routes::Plan plan_;       // the plan of the open streams
  int communication_ = 0;   // the communication device Android uses
};

}  // namespace

webrtc::scoped_refptr<webrtc::AudioDeviceModule> CreateAAudioAdm(
    const webrtc::Environment& env, std::function<void()> on_output_restart,
    AAudioRoutes** routes) {
  auto device = std::make_unique<AAudioDevice>(std::move(on_output_restart));
  *routes = device.get();
  auto adm = webrtc::make_ref_counted<webrtc::AudioDeviceModuleImpl>(
      env, webrtc::AudioDeviceModule::kAndroidAAudioAudio, std::move(device),
      /*create_detached=*/true);
  adm->AttachAudioBuffer();
  return adm;
}

}  // namespace tsnx
