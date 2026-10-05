// The audio engine: device, APM, a 48 kHz mixer with a fixed pool of track
// sources, the capture path, and the threads around them (ADR D1, D3, D10,
// D12, D15).
//
// Threads:
//   audio render  - the device callback. RenderBlock(). Lock-free.
//   audio capture - the device callback. CaptureBlock(). Lock-free.
//   control       - track windows (load, spill, free), deferred frees, and
//                   playout demand.
//   notifier      - sends events and capture-ready signals to Dart.
//   device        - all device (ADM) calls, in order.
// With the manual device (tests), the caller drives render, capture,
// control, and notification synchronously through ManualRender().
#ifndef TSNX_ENGINE_H_
#define TSNX_ENGINE_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "clock/hardware_clock.h"
#include "device_info.h"
#include "track.h"
#include "util/retirer.h"
#include "util/spsc_ring.h"
#include "util/wakeup.h"

namespace tsnx {

class AudioClockCorrection;
class BlockResampler;
class DriftServo;
class DeviceThread;
class PoolSource;
struct EngineWebRtc;

enum class NotifyKind : int32_t {
  // 1..6 are TrackEventKind.
  kCaptureReady = 100,
  kRequestDone = 101,
  kDevicesChanged = 102,
  kEngineError = 103,
  kOutputState = 104,
};

using NotifyFn = void (*)(int32_t kind, int32_t id, int64_t value);

struct EngineConfig {
  bool manual_device = false;
  int manual_output_channels = 2;
  int manual_delay_ms = 0;
  std::string spill_dir;
  NotifyFn notify = nullptr;
  // APM switches. Defaults: AEC3, noise suppression, AGC2, high-pass.
  bool echo_cancellation = true;
  bool noise_suppression = true;
  bool auto_gain = true;
  // macOS: use AVAudioEngine with Apple voice processing (the fork's macOS
  // device) instead of the CoreAudio device with AEC3. The APM echo
  // canceller is then off (Apple's runs instead).
  bool platform_voice_processing = false;
  // Capture clock correction (ported from the fork): 0 off, 1 observe,
  // 2 control. Frozen when capture first starts.
  int clock_correction = 0;
  // Linux: 0 auto (PulseAudio, then ALSA), 1 PulseAudio, 2 ALSA. The Pi
  // appliance uses ALSA directly.
  int linux_audio_backend = 0;
  // Seconds of idle output before the output device stops (platform only).
  double idle_stop_seconds = 3.0;
#if TSNX_ALLOC_PROBE
  // Native-test observer for the exact samples given to ProcessReverseStream.
  // The callback runs on the render thread. It must not allocate or lock.
  using RenderReferenceTestHook = void (*)(const int16_t* samples,
                                           size_t frames, size_t channels,
                                           void* context);
  RenderReferenceTestHook render_reference_test_hook = nullptr;
  void* render_reference_test_context = nullptr;
#endif
};

struct CaptureBlock {
  int64_t t_ns;
  int32_t frames;
  int32_t rate;
  int16_t data[480];
};

enum EngineError : int32_t {
  kOk = 0,
  kErrInvalidArgument = -1,
  kErrNoFreeTrack = -2,
  kErrNoTrack = -3,
  kErrDevice = -4,
  kErrUnsupportedFormat = -5,
  kErrQueueFull = -6,
  kErrBusy = -7,
  kErrUnsupportedPlatform = -8,
};

class Engine : public EventSink {
 public:
  static constexpr int kMaxTracks = 8;
  static constexpr int kMixRate = 48000;
  static constexpr int kMixFrames = kMixRate / 100;

  static std::unique_ptr<Engine> Open(const EngineConfig& config,
                                      int32_t* error);
  ~Engine() override;

  int64_t NowNs() const;
  bool manual() const { return config_.manual_device; }

  // ---- Tracks (Dart thread) ----
  int32_t CreateTrack(int rate, int channels, Retention retention,
                      int32_t* out_id);
  int32_t CreateMp3Track(const uint8_t* data, size_t size, int32_t* out_id);
  int64_t Write(int32_t id, const int16_t* pcm, int64_t frames);
  int32_t EndOfStream(int32_t id);
  int32_t Play(int32_t id);
  int32_t Pause(int32_t id);
  int32_t Seek(int32_t id, int64_t frame);
  int32_t SetRate(int32_t id, double rate);
  int32_t SetGain(int32_t id, double gain, int32_t ramp_ms);
  int32_t Flush(int32_t id);
  int32_t GetState(int32_t id, TrackStateWords* out);
  int32_t TrackFormat(int32_t id, int32_t* rate, int32_t* channels);
  int32_t DisposeTrack(int32_t id);

  // ---- Capture and devices ----
  // Asynchronous: the result arrives as kRequestDone(request_id, result).
  int32_t StartCapture(int32_t rate, int32_t request_id);
  int32_t StopCapture(int32_t request_id);
  int32_t SelectOutput(const std::string& id, int32_t request_id);
  int32_t SelectInput(const std::string& id, int32_t request_id);
  // Copies up to `max_blocks` capture blocks. Returns the count.
  int32_t ReadCapture(CaptureBlock* out, int32_t max_blocks);
  std::vector<DeviceInfo> Devices(bool input);
  // The device in use (ADR D15): the entry of Devices() in effect (the
  // selection, or "default" when the selection follows the system or its
  // device is gone), with the name and kind of the device that plays or
  // records. False without a device (manual) or when the platform does
  // not say. Selections update it before their request completes.
  bool CurrentDevice(bool input, DeviceInfo* out);
  int32_t capture_dropped() const { return capture_dropped_.load(); }

  // ---- Manual device (tests) ----
  // Renders `blocks` 10 ms blocks into `out` (48 kHz, manual_output_channels
  // interleaved). `capture_in` (nullable) holds the same number of 48 kHz
  // mono blocks for the capture path.
  int32_t ManualRender(int32_t blocks, int16_t* out, const int16_t* capture_in);

  // ---- Diagnostics ----
  int64_t render_format_changes() const { return render_format_changes_; }
  int64_t apm_render_rate() const { return apm_render_rate_; }
  double EchoReturnLossEnhancement() const;
  // Sum of squared samples before and after the APM since the last call.
  void TakeCaptureEnergy(double* pre, double* post, int64_t* blocks);
  int64_t device_delay_ms() const { return device_delay_ns_.load() / 1000000; }

  // ---- Device callbacks (audio threads) ----
  void RenderBlock(int16_t* out, size_t frames, size_t channels,
                   uint32_t rate);
  // The output stops now and restarts later; the device drops its buffer.
  // Any thread. The next render moves playing tracks back (ADR I10).
  void MarkOutputRestart();
  // Manual device: simulate an output restart with a `gap_ms` silence.
  int32_t ManualOutputRestart(int32_t gap_ms);
  void CaptureBlockIn(const int16_t* in, size_t frames, size_t channels,
                      uint32_t rate, uint32_t total_delay_ms);
  // Clock-correction state for diagnostics.
  int32_t ClockCorrectionState(double* applied_ppm, bool* engaged);
  // ALSA hardware position (device threads). Feeds the drift servo.
  void OnHardwareClock(const AudioHardwareClockObservation& o);

  // EventSink (audio thread).
  void PushRt(const TrackEvent& e) override;

 private:
  friend class PoolSource;
  struct Command {
    enum Kind : int32_t { kPlay, kPause, kSeek, kRate, kGain, kFlush } kind;
    int32_t track_id;
    int64_t i;
    double d;
  };

  explicit Engine(const EngineConfig& config);
  bool Init(int32_t* error);
  Track* Lookup(int32_t id) const;  // Dart thread
  int32_t Enqueue(const Command& c);
  void ApplyCommands();
  void WarmUp(int channels);
  void PublishAll(int64_t now_ns);
  void IdleApply();
  void Notify(int32_t kind, int32_t id, int64_t value);  // non-audio threads
  void ControlPass();
  void NotifierPass();
  void ControlLoop();
  void NotifierLoop();
  void UpdatePlayoutDemand();
  void RefreshDevices();
  std::string SpillPath(int32_t id);
  void CleanStaleSpill();

  EngineConfig config_;
  std::string spill_dir_;

  std::unique_ptr<EngineWebRtc> rtc_;  // APM, mixer, ADM, transport
  std::vector<std::unique_ptr<PoolSource>> pool_;
  std::unique_ptr<DeviceThread> device_thread_;

  // Track slots (D12). Dart thread writes, audio thread reads.
  std::atomic<Track*> slots_[kMaxTracks];
  int32_t gens_[kMaxTracks] = {};
  std::mutex tracks_mu_;  // Dart thread vs control thread

  std::mutex cmd_mu_;  // producers only
  SpscRing<Command> commands_{256};
  SpscRing<TrackEvent> rt_events_{1024};
  std::atomic<int64_t> rt_events_dropped_{0};
  Retirer retirer_;

  // Render state (audio thread).
  std::atomic<bool> render_lock_{false};
  int64_t render_now_ns_ = 0;
  int64_t render_delay_ns_ = 0;
  std::unique_ptr<float[]> render_f_in_;
  std::unique_ptr<float[]> render_f_out_;
  std::unique_ptr<int16_t[]> render_apm_out_;
  std::unique_ptr<BlockResampler> render_resampler_;
  int render_channels_ = 0;
  int64_t render_format_changes_ = 0;
  int64_t apm_render_rate_ = 0;
  std::atomic<int64_t> device_delay_ns_{0};
  std::atomic<int64_t> last_render_ns_{0};
  int64_t manual_now_ns_ = 0;
  int64_t prev_render_ns_ = 0;  // audio thread
  std::atomic<int64_t> restart_stop_ns_{0};
  // A render gap longer than this is an output restart the engine did not
  // start (a route change inside the device module).
  static constexpr int64_t kRestartGapNs = 250'000'000;

  void ProcessCapture(const int16_t* in, size_t frames, size_t channels,
                      uint32_t rate, uint32_t total_delay_ms);

  // Clock correction. The servo pointer is fixed once capture starts.
  std::shared_ptr<AudioClockCorrection> clock_;
  std::shared_ptr<DriftServo> servo_hold_;
  std::atomic<DriftServo*> servo_{nullptr};
  std::atomic<bool> servo_observe_only_{false};
  std::vector<int16_t> servo_block_;

  // Capture state (capture thread).
  std::atomic<bool> capture_active_{false};
  std::atomic<int32_t> capture_rate_{48000};
  std::unique_ptr<BlockResampler> capture_in_rs_;
  int capture_in_rate_ = 0;
  std::unique_ptr<BlockResampler> capture_out_rs_;
  int capture_out_rate_ = 0;
  std::vector<float> cap_f_;
  std::vector<float> cap_f48_;
  std::vector<int16_t> cap_s48_;
  std::vector<float> cap_fout_;
  SpscRing<CaptureBlock> capture_ring_{256};
  std::atomic<int32_t> capture_dropped_{0};
  std::atomic<bool> capture_signal_{false};
  std::atomic<double> cap_energy_pre_{0};
  std::atomic<double> cap_energy_post_{0};
  std::atomic<int64_t> cap_energy_blocks_{0};

  // Threads.
  std::atomic<bool> stopping_{false};
  Wakeup control_wakeup_;
  Wakeup notify_wakeup_;
  std::thread control_thread_;
  std::thread notifier_thread_;
  std::mutex events_mu_;
  std::deque<TrackEvent> events_;  // non-audio producers

  // Device state (device thread writes; others read under devices_mu_).
  std::mutex devices_mu_;
  std::vector<DeviceInfo> inputs_;
  std::vector<DeviceInfo> outputs_;
  DeviceInfo current_input_;
  DeviceInfo current_output_;
  bool has_current_input_ = false;
  bool has_current_output_ = false;
  std::atomic<bool> playout_running_{false};
  std::atomic<bool> playout_pending_{false};
  int64_t idle_since_ns_ = 0;
  int64_t last_delay_poll_ns_ = 0;
  int64_t last_device_poll_ns_ = 0;
  // Selected device IDs ("" for none). Device thread. The ADM takes an
  // index, and the index of a device changes when the list changes.
  std::string selected_output_;
  std::string selected_input_;
  // The index of `id` in the current list, or -1.
  int DeviceIndex(bool input, const std::string& id);
  // Gives the ADM the selected device before it opens a stream.
  bool ApplySelectedDevice(bool input);
  // Selects entry `index` of the engine's list in the ADM. Windows: the
  // engine's list has "default" first, which the ADM's list does not.
  int32_t AdmSelect(bool input, int index);
  // The current device from the lists and the selection (device thread).
  bool ResolveCurrent(bool input, const std::vector<DeviceInfo>& list,
                      DeviceInfo* out);
  uint32_t devices_generation_ = 0;  // control thread
  int session_profile_ = 0;  // device thread; iOS SessionProfile (D6)
  // iOS: moves the shared audio session to the profile for the current
  // capture state before the device starts. Device thread.
  void ApplySessionProfile(bool capture);
};

}  // namespace tsnx

#endif  // TSNX_ENGINE_H_
