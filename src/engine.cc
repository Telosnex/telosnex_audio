#include "engine.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <filesystem>

#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

#include "alloc_probe.h"
#include "api/audio/audio_device.h"
#include "api/audio/audio_frame.h"
#include "api/audio/audio_mixer.h"
#include "api/audio/audio_processing.h"
#include "api/audio/builtin_audio_processing_builder.h"
#if !defined(WEBRTC_IOS) && !defined(WEBRTC_ANDROID)
#include "api/audio/create_audio_device_module.h"
#endif
#include "api/environment/environment.h"
#include "api/environment/environment_factory.h"
#include "block_resampler.h"
#include "clock/audio_clock_correction.h"
#include "util/utf8_path.h"
#include "modules/audio_mixer/audio_mixer_impl.h"
#if defined(__APPLE__)
#include "apple/apple_devices.h"
#include "apple/mac_devices.h"
#endif
#if defined(WEBRTC_IOS)
#include "apple/ios_session.h"
#endif
#if defined(WEBRTC_ANDROID)
#include "android/aaudio_device.h"
#include "android/android_jni.h"
#endif
#if defined(WEBRTC_WIN)
#include "win/win_devices.h"
#endif
#include "modules/audio_mixer/output_rate_calculator.h"
#include "rtc_base/thread.h"

namespace tsnx {

namespace {

// D3: the mixer always runs at 48 kHz, so the APM render format never
// follows the sources.
class Fixed48kRate : public webrtc::OutputRateCalculator {
 public:
  int CalculateOutputRateFromRange(
      webrtc::ArrayView<const int> /*rates*/) override {
    return Engine::kMixRate;
  }
};

int16_t ToS16(float v) {
  if (v > 32767.f) return 32767;
  if (v < -32768.f) return -32768;
  return static_cast<int16_t>(std::lrintf(v));
}

int SlotOf(int32_t id) { return id % Engine::kMaxTracks; }

// Moves when the platform reports a device or route change, so the engine
// reads the lists at once instead of at the next poll.
uint32_t PlatformDevicesGeneration() {
#if defined(WEBRTC_ANDROID)
  return android_jni::DevicesGeneration();
#elif defined(WEBRTC_IOS)
  return SessionRouteGeneration();
#elif defined(__APPLE__)
  return MacDevicesGeneration();
#else
  return 0;
#endif
}

// The list entry "default" is named after the device it plays to:
// "default (Name)" (macOS) and "default: Name" (PulseAudio) become "Name".
std::string DefaultEntryName(const std::string& name) {
  if (name.rfind("default (", 0) == 0 && name.size() > 10 && name.back() == ')')
    return name.substr(9, name.size() - 10);
  if (name.rfind("default: ", 0) == 0 && name.size() > 9)
    return name.substr(9);
  return name;
}

}  // namespace

// ---- Device thread ---------------------------------------------------------

// A webrtc::Thread: AudioEngineDevice posts tasks to the thread that
// created it, so the device thread must run a WebRTC task loop.
class DeviceThread {
 public:
  DeviceThread() : thread_(webrtc::Thread::Create()) {
    thread_->SetName("tsnx_device", nullptr);
    thread_->Start();
  }
  ~DeviceThread() { thread_->Stop(); }
  void Post(std::function<void()> task) {
    thread_->PostTask([task = std::move(task)] { task(); });
  }
  void Invoke(std::function<void()> task) { thread_->BlockingCall(task); }

 private:
  std::unique_ptr<webrtc::Thread> thread_;
};

// ---- WebRTC objects ---------------------------------------------------------

class EngineTransport : public webrtc::AudioTransport {
 public:
  explicit EngineTransport(Engine* e) : e_(e) {}
  int32_t RecordedDataIsAvailable(const void* audio, size_t samples,
                                  size_t /*bytes_per_sample*/, size_t channels,
                                  uint32_t rate, uint32_t total_delay_ms,
                                  int32_t /*drift*/, uint32_t mic_level,
                                  bool /*key_pressed*/,
                                  uint32_t& new_mic_level) override {
    e_->CaptureBlockIn(static_cast<const int16_t*>(audio), samples, channels,
                       rate, total_delay_ms);
    new_mic_level = mic_level;
    return 0;
  }
  int32_t NeedMorePlayData(size_t samples, size_t /*bytes_per_sample*/,
                           size_t channels, uint32_t rate, void* audio,
                           size_t& samples_out, int64_t* elapsed_time_ms,
                           int64_t* ntp_time_ms) override {
    e_->RenderBlock(static_cast<int16_t*>(audio), samples, channels, rate);
    samples_out = samples * channels;
    *elapsed_time_ms = -1;
    *ntp_time_ms = -1;
    return 0;
  }
  void PullRenderData(int, int, size_t, size_t, void*, int64_t*,
                      int64_t*) override {}
  void OnAudioHardwareClockObservation(
      const webrtc::AudioHardwareClockObservation& o) override {
    e_->OnHardwareClock(
        {o.direction == webrtc::AudioHardwareClockDirection::kPlayout
             ? AudioHardwareClockDirection::kPlayout
             : AudioHardwareClockDirection::kCapture,
         o.monotonic_time_ns, o.position_frames, o.sample_rate_hz,
         o.generation});
  }

 private:
  Engine* e_;
};

struct EngineWebRtc {
  webrtc::Environment env = webrtc::CreateEnvironment();
  webrtc::scoped_refptr<webrtc::AudioProcessing> apm;
  webrtc::scoped_refptr<webrtc::AudioMixerImpl> mixer;
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm;  // device thread
#if defined(WEBRTC_ANDROID)
  AAudioRoutes* android_routes = nullptr;  // lives with adm
#endif
  std::unique_ptr<EngineTransport> transport;
  webrtc::AudioFrame mix_frame;
};

class PoolSource : public webrtc::AudioMixer::Source {
 public:
  PoolSource(Engine* e, int slot) : e_(e), slot_(slot) {}
  AudioFrameInfo GetAudioFrameWithInfo(int /*rate*/,
                                       webrtc::AudioFrame* f) override {
    Track* t = e_->slots_[slot_].load(std::memory_order_acquire);
    if (!t) return AudioFrameInfo::kMuted;
    f->UpdateFrame(0, nullptr, Engine::kMixFrames, Engine::kMixRate,
                   webrtc::AudioFrame::kNormalSpeech,
                   webrtc::AudioFrame::kVadUnknown, t->channels());
    int16_t* d = f->mutable_data();
    if (!t->Render(d, e_->render_now_ns_, e_->render_delay_ns_, *e_)) {
      f->Mute();
      return AudioFrameInfo::kMuted;
    }
    return AudioFrameInfo::kNormal;
  }
  int Ssrc() const override { return slot_; }
  int PreferredSampleRate() const override { return Engine::kMixRate; }

 private:
  Engine* e_;
  int slot_;
};

// ---- Engine -----------------------------------------------------------------

std::unique_ptr<Engine> Engine::Open(const EngineConfig& config,
                                     int32_t* error) {
  std::unique_ptr<Engine> e(new Engine(config));
  if (!e->Init(error)) return nullptr;
  return e;
}

Engine::Engine(const EngineConfig& config) : config_(config) {
  for (auto& s : slots_) s.store(nullptr);
#if defined(WEBRTC_IOS)
  // iOS runs Apple voice processing in the device; the APM echo canceller
  // stays off (as on macOS with platform_voice_processing).
  if (!config_.manual_device) config_.platform_voice_processing = true;
#endif
}

void Engine::ApplySessionProfile(bool capture) {
#if defined(WEBRTC_IOS)
  const auto want = capture ? SessionProfile::kCommunication
                            : SessionProfile::kMedia;
  if (session_profile_ == static_cast<int>(want)) return;
  // A category change restarts the output (D6): the device buffer is lost.
  if (rtc_->adm->Playing()) MarkOutputRestart();
  if (SetSessionProfile(want)) session_profile_ = static_cast<int>(want);
#else
  (void)capture;
#endif
}

bool Engine::Init(int32_t* error) {
  *error = kOk;
  if (!config_.spill_dir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(Utf8Path(config_.spill_dir), ec);
    CleanStaleSpill();
#if defined(_WIN32)
    const long pid = static_cast<long>(GetCurrentProcessId());
#else
    const long pid = static_cast<long>(getpid());
#endif
    spill_dir_ = config_.spill_dir + "/" + std::to_string(pid);
    std::filesystem::create_directories(Utf8Path(spill_dir_), ec);
  }

  rtc_ = std::make_unique<EngineWebRtc>();
  webrtc::AudioProcessing::Config apm;
  apm.echo_canceller.enabled =
      config_.echo_cancellation && !config_.platform_voice_processing;
  apm.noise_suppression.enabled = config_.noise_suppression;
  apm.gain_controller2.enabled = config_.auto_gain;
  apm.gain_controller2.adaptive_digital.enabled = config_.auto_gain;
  apm.high_pass_filter.enabled = true;
  rtc_->apm = webrtc::BuiltinAudioProcessingBuilder(apm).Build(rtc_->env);
  rtc_->mixer = webrtc::AudioMixerImpl::Create(
      std::make_unique<Fixed48kRate>(), /*use_limiter=*/true);
  for (int i = 0; i < kMaxTracks; ++i) {
    pool_.push_back(std::make_unique<PoolSource>(this, i));
    rtc_->mixer->AddSource(pool_.back().get());  // D12: once, at start
  }
  render_f_in_.reset(new float[kMixFrames * 2]);
  render_f_out_.reset(new float[kMixFrames * 2]);
  render_apm_out_.reset(new int16_t[kMixFrames * 2]);
  cap_f_.resize(480 * 2);
  cap_f48_.resize(kMixFrames);
  cap_s48_.resize(kMixFrames);
  cap_fout_.resize(kMixFrames);
  rtc_->transport = std::make_unique<EngineTransport>(this);
  // Clock correction (fork port). A hardware clock producer exists only for
  // ALSA (workplan step 7); the callback estimator works everywhere.
  clock_ = std::make_shared<AudioClockCorrection>(/*supported=*/true);
  if (config_.clock_correction > 0)
    clock_->Configure(
        static_cast<AudioClockCorrectionMode>(config_.clock_correction));
  servo_block_.resize(480 * 2);
  WarmUp(config_.manual_device ? std::clamp(config_.manual_output_channels, 1, 2)
                               : 2);

  if (config_.manual_device) {
    device_delay_ns_.store(int64_t{config_.manual_delay_ms} * 1000000);
    return true;
  }

  device_thread_ = std::make_unique<DeviceThread>();
  bool ok = false;
  bool unsupported = false;
  device_thread_->Invoke([&] {
#if defined(__APPLE__)
    if (config_.platform_voice_processing)
      rtc_->adm = CreateAppleVoiceProcessingAdm(rtc_->env);
#endif
    auto layer = webrtc::AudioDeviceModule::kPlatformDefaultAudio;
#if defined(WEBRTC_LINUX)
    if (config_.linux_audio_backend == 1)
      layer = webrtc::AudioDeviceModule::kLinuxPulseAudio;
    else if (config_.linux_audio_backend == 2)
      layer = webrtc::AudioDeviceModule::kLinuxAlsaAudio;
#endif
#if defined(WEBRTC_IOS)
    // iOS: AVAudioEngine with Apple voice processing (the fork's device).
    rtc_->adm = CreateAppleVoiceProcessingAdm(rtc_->env);
#elif defined(WEBRTC_ANDROID)
    // Android: AAudio, media audio, AEC3 in the APM.
    if (!AAudioAvailable()) {
      unsupported = true;
      return;
    }
    rtc_->adm = CreateAAudioAdm(
        rtc_->env, [this] { MarkOutputRestart(); }, &rtc_->android_routes);
#else
    if (!rtc_->adm)
      rtc_->adm = webrtc::CreateAudioDeviceModule(rtc_->env, layer);
#endif
    if (!rtc_->adm || rtc_->adm->Init() != 0) return;
    rtc_->adm->RegisterAudioCallback(rtc_->transport.get());
    RefreshDevices();
    ok = true;
  });
  if (!ok) {
    *error = unsupported ? kErrUnsupportedPlatform : kErrDevice;
    device_thread_->Invoke([&] {
#if defined(WEBRTC_ANDROID)
      rtc_->android_routes = nullptr;
#endif
      rtc_->adm = nullptr;
    });
    return false;
  }
  control_thread_ = std::thread([this] { ControlLoop(); });
  notifier_thread_ = std::thread([this] { NotifierLoop(); });
  return true;
}

Engine::~Engine() {
  stopping_.store(true);
  control_wakeup_.Signal();
  notify_wakeup_.Signal();
  if (control_thread_.joinable()) control_thread_.join();
  if (notifier_thread_.joinable()) notifier_thread_.join();
  if (device_thread_) {
    device_thread_->Invoke([&] {
      if (!rtc_->adm) return;
      if (rtc_->adm->Recording()) rtc_->adm->StopRecording();
      if (rtc_->adm->Playing()) rtc_->adm->StopPlayout();
      rtc_->adm->RegisterAudioCallback(nullptr);
      rtc_->adm->Terminate();
#if defined(WEBRTC_ANDROID)
      rtc_->android_routes = nullptr;
#endif
      rtc_->adm = nullptr;
    });
    device_thread_.reset();
  }
  for (auto& s : slots_) delete s.exchange(nullptr);
  retirer_.ReclaimAll();
  if (!spill_dir_.empty()) {
    std::error_code ec;
    std::filesystem::remove_all(Utf8Path(spill_dir_), ec);
  }
}

void Engine::CleanStaleSpill() {
  std::error_code ec;
  for (const auto& entry :
       std::filesystem::directory_iterator(Utf8Path(config_.spill_dir), ec)) {
    // u8string(): string() can fail on Windows for names outside the code
    // page, and this build has no exceptions.
    const std::u8string u8 = entry.path().filename().u8string();
    const std::string name(u8.begin(), u8.end());
    if (name.empty() || name.size() > 9 ||
        !std::all_of(name.begin(), name.end(), ::isdigit))
      continue;
    const long pid = std::strtol(name.c_str(), nullptr, 10);
#if defined(_WIN32)
    if (pid == static_cast<long>(GetCurrentProcessId())) continue;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                               static_cast<DWORD>(pid))) {
      DWORD code = 0;
      const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
      CloseHandle(h);
      if (alive) continue;
    } else if (GetLastError() == ERROR_ACCESS_DENIED) {
      continue;
    }
#else
    if (pid == static_cast<long>(getpid())) continue;
    if (kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM) continue;
#endif
    std::filesystem::remove_all(entry.path(), ec);
  }
}

std::string Engine::SpillPath(int32_t id) {
  if (spill_dir_.empty()) return std::string();
  return spill_dir_ + "/track_" + std::to_string(id) + ".pcm";
}

int64_t Engine::NowNs() const {
  return config_.manual_device ? manual_now_ns_ : MonotonicNowNs();
}

// ---- Tracks -----------------------------------------------------------------

Track* Engine::Lookup(int32_t id) const {
  if (id < 0) return nullptr;
  Track* t = slots_[SlotOf(id)].load(std::memory_order_acquire);
  return (t && t->id() == id) ? t : nullptr;
}

int32_t Engine::CreateTrack(int rate, int channels, Retention retention,
                            int32_t* out_id) {
  if (rate < 8000 || rate > 48000 || rate % 100 != 0 || channels < 1 ||
      channels > 2)
    return kErrUnsupportedFormat;
  std::lock_guard<std::mutex> lock(tracks_mu_);
  for (int s = 0; s < kMaxTracks; ++s) {
    if (slots_[s].load()) continue;
    const int32_t id = ++gens_[s] * kMaxTracks + s;
    auto store = std::make_unique<TrackStore>(
        rate, channels, retention,
        retention == Retention::kAll ? SpillPath(id) : std::string());
    slots_[s].store(new Track(id, std::move(store)),
                    std::memory_order_release);
    *out_id = id;
    return kOk;
  }
  return kErrNoFreeTrack;
}

int32_t Engine::CreateMp3Track(const uint8_t* data, size_t size,
                               int32_t* out_id) {
  auto store = TrackStore::OpenMp3(data, size);
  if (!store) return kErrUnsupportedFormat;
  std::lock_guard<std::mutex> lock(tracks_mu_);
  for (int s = 0; s < kMaxTracks; ++s) {
    if (slots_[s].load()) continue;
    const int32_t id = ++gens_[s] * kMaxTracks + s;
    slots_[s].store(new Track(id, std::move(store)),
                    std::memory_order_release);
    *out_id = id;
    control_wakeup_.Signal();
    return kOk;
  }
  return kErrNoFreeTrack;
}

int64_t Engine::Write(int32_t id, const int16_t* pcm, int64_t frames) {
  Track* t = Lookup(id);
  if (!t) return kErrNoTrack;
  return t->store().Write(pcm, frames, t->playhead(), retirer_);
}

int32_t Engine::EndOfStream(int32_t id) {
  Track* t = Lookup(id);
  if (!t) return kErrNoTrack;
  t->store().EndOfStream();
  return kOk;
}

int32_t Engine::Enqueue(const Command& c) {
  if (!Lookup(c.track_id)) return kErrNoTrack;
  {
    std::lock_guard<std::mutex> lock(cmd_mu_);
    if (!commands_.Push(c)) return kErrQueueFull;
  }
  control_wakeup_.Signal();
  return kOk;
}

int32_t Engine::Play(int32_t id) { return Enqueue({Command::kPlay, id, 0, 0}); }
int32_t Engine::Pause(int32_t id) {
  return Enqueue({Command::kPause, id, 0, 0});
}
int32_t Engine::Seek(int32_t id, int64_t frame) {
  return Enqueue({Command::kSeek, id, frame, 0});
}
int32_t Engine::SetRate(int32_t id, double rate) {
  if (!(rate >= 0.5 && rate <= 3.0)) return kErrInvalidArgument;
  return Enqueue({Command::kRate, id, 0, rate});
}
int32_t Engine::SetGain(int32_t id, double gain, int32_t ramp_ms) {
  if (!(gain >= 0.0 && gain <= 4.0) || ramp_ms < 0) return kErrInvalidArgument;
  return Enqueue({Command::kGain, id, ramp_ms, gain});
}
int32_t Engine::Flush(int32_t id) {
  Track* t = Lookup(id);
  if (!t) return kErrNoTrack;
  return Enqueue({Command::kFlush, id, t->store().written(), 0});
}

int32_t Engine::GetState(int32_t id, TrackStateWords* out) {
  Track* t = Lookup(id);
  if (!t) return kErrNoTrack;
  *out = t->State();
  return kOk;
}

int32_t Engine::TrackFormat(int32_t id, int32_t* rate, int32_t* channels) {
  Track* t = Lookup(id);
  if (!t) return kErrNoTrack;
  *rate = t->sample_rate();
  *channels = t->channels();
  return kOk;
}

int32_t Engine::DisposeTrack(int32_t id) {
  std::lock_guard<std::mutex> lock(tracks_mu_);
  Track* t = Lookup(id);
  if (!t) return kErrNoTrack;
  slots_[SlotOf(id)].store(nullptr, std::memory_order_seq_cst);
  retirer_.Retire([t] { delete t; });
  if (config_.manual_device) retirer_.Reclaim();
  return kOk;
}

void Engine::ApplyCommands() {
  Command c;
  bool any = false;
  while (commands_.Pop(&c)) {
    any = true;
    Track* t = slots_[SlotOf(c.track_id)].load(std::memory_order_acquire);
    if (!t || t->id() != c.track_id) continue;
    switch (c.kind) {
      case Command::kPlay:
        t->Play();
        break;
      case Command::kPause:
        t->Pause();
        break;
      case Command::kSeek:
        t->Seek(c.i);
        break;
      case Command::kRate:
        t->SetRate(c.d);
        break;
      case Command::kGain:
        t->SetGain(c.d, c.i * t->sample_rate() / 1000);
        break;
      case Command::kFlush:
        t->Flush(c.i);
        break;
    }
  }
  // A seek or flush moves the playhead: let the control thread load the
  // window now rather than at its next 20 ms tick.
  if (any) control_wakeup_.Signal();
}

void Engine::PublishAll(int64_t now_ns) {
  for (auto& s : slots_) {
    Track* t = s.load(std::memory_order_acquire);
    if (t) t->Publish(now_ns, *this);
  }
}

void Engine::PushRt(const TrackEvent& e) {
  if (!rt_events_.Push(e)) rt_events_dropped_.fetch_add(1);
}

// ---- Render (audio thread) ------------------------------------------------

void Engine::RenderBlock(int16_t* out, size_t frames, size_t channels,
                         uint32_t rate) {
  RtScope rt;
  const size_t samples = frames * channels;
  if (render_lock_.exchange(true, std::memory_order_acquire)) {
    std::memset(out, 0, samples * sizeof(int16_t));
    return;
  }
  retirer_.BeginBlock();
  const int64_t now = NowNs();
  last_render_ns_.store(now, std::memory_order_relaxed);
  ApplyCommands();

  const int ch = static_cast<int>(std::clamp<size_t>(channels, 1, 2));
  if (ch != render_channels_) {
    render_channels_ = ch;
    ++render_format_changes_;
  }
  // I10: an output restart loses the audio in the device buffer.
  int64_t stop_ns = restart_stop_ns_.exchange(0, std::memory_order_acq_rel);
  if (prev_render_ns_ != 0) {
    const int64_t prev_end = prev_render_ns_ + 10'000'000;
    if (stop_ns != 0) stop_ns = std::min(stop_ns, prev_end);
    else if (now - prev_render_ns_ > kRestartGapNs) stop_ns = prev_end;
    if (stop_ns != 0) {
      for (auto& s : slots_) {
        Track* t = s.load(std::memory_order_acquire);
        if (t) t->OnOutputRestart(stop_ns);
      }
    }
  }
  prev_render_ns_ = now;
  render_now_ns_ = now;
  render_delay_ns_ = device_delay_ns_.load(std::memory_order_relaxed);
  webrtc::AudioFrame& mix = rtc_->mix_frame;
  rtc_->mixer->Mix(ch, &mix);
  const webrtc::StreamConfig rc(kMixRate, ch);
  // I3: keep the render reference separate from the APM destination. The
  // device gets the exact mixed samples that enter ProcessReverseStream,
  // even if an APM implementation writes different samples to its output.
  int16_t* mixed = mix.mutable_data();
#if TSNX_ALLOC_PROBE
  if (config_.render_reference_test_hook)
    config_.render_reference_test_hook(mixed, kMixFrames, ch,
                                       config_.render_reference_test_context);
#endif
  rtc_->apm->ProcessReverseStream(mixed, rc, rc, render_apm_out_.get());
  apm_render_rate_ = kMixRate;

  if (rate == static_cast<uint32_t>(kMixRate) && frames == kMixFrames &&
      static_cast<size_t>(ch) == channels) {
    std::memcpy(out, mixed, samples * sizeof(int16_t));
  } else if (rate % 100 == 0 && frames * 100 == rate &&
             static_cast<size_t>(ch) == channels) {
    if (!render_resampler_ ||
        render_resampler_->dst_frames() != static_cast<int>(frames) ||
        render_resampler_->channels() != ch) {
      // Device format change: rare, and allocates.
      render_resampler_ =
          std::make_unique<BlockResampler>(kMixRate, rate, ch);
    }
    for (int i = 0; i < kMixFrames * ch; ++i) render_f_in_[i] = mixed[i];
    render_resampler_->Process(render_f_in_.get(), render_f_out_.get());
    for (size_t i = 0; i < samples; ++i) out[i] = ToS16(render_f_out_[i]);
  } else {
    std::memset(out, 0, samples * sizeof(int16_t));
  }

  if (DriftServo* servo = servo_.load(std::memory_order_acquire))
    servo->OnRenderFrames(frames, rate);
  PublishAll(now);
  retirer_.EndBlock();
  render_lock_.store(false, std::memory_order_release);
  if (rt_events_.Size() > 0) notify_wakeup_.Signal();
}

void Engine::MarkOutputRestart() {
  int64_t expected = 0;
  restart_stop_ns_.compare_exchange_strong(expected, NowNs());
}

int32_t Engine::ManualOutputRestart(int32_t gap_ms) {
  if (!config_.manual_device || gap_ms < 0) return kErrInvalidArgument;
  MarkOutputRestart();
  manual_now_ns_ += int64_t{gap_ms} * 1000000;
  return kOk;
}

// Runs the APM and mixer once on this (non-real-time) thread, so their
// first-use initialization does not allocate on the audio threads (I1).
void Engine::WarmUp(int channels) {
  std::vector<int16_t> zeros(kMixFrames * 2, 0);
  const webrtc::StreamConfig rc(kMixRate, channels);
  rtc_->mixer->Mix(channels, &rtc_->mix_frame);
  rtc_->apm->ProcessReverseStream(zeros.data(), rc, rc, zeros.data());
  const webrtc::StreamConfig cc(kMixRate, 1);
  rtc_->apm->set_stream_delay_ms(0);
  rtc_->apm->ProcessStream(zeros.data(), cc, cc, zeros.data());
  render_channels_ = channels;
  render_format_changes_ = 1;
  capture_in_rate_ = kMixRate;
  capture_in_rs_ = std::make_unique<BlockResampler>(kMixRate, kMixRate, 1);
}

// Applies commands and publishes state while no device is rendering, so
// seek and pause show up in state without output running.
void Engine::IdleApply() {
  if (render_lock_.exchange(true, std::memory_order_acquire)) return;
  retirer_.BeginBlock();
  ApplyCommands();
  PublishAll(NowNs());
  retirer_.EndBlock();
  render_lock_.store(false, std::memory_order_release);
  if (rt_events_.Size() > 0) notify_wakeup_.Signal();
}

// ---- Capture (capture thread) -------------------------------------------

void Engine::CaptureBlockIn(const int16_t* in, size_t frames, size_t channels,
                            uint32_t rate, uint32_t total_delay_ms) {
  RtScope rt;
  if (!capture_active_.load(std::memory_order_acquire)) return;
  DriftServo* servo = servo_.load(std::memory_order_acquire);
  if (servo && channels <= 2) {
    const bool observe = servo_observe_only_.load(std::memory_order_relaxed);
    const size_t blocks = servo->PushCaptureAndCorrect(in, frames, rate,
                                                       channels, !observe);
    if (!observe && servo->engaged()) {
      const size_t block_frames = rate / 100;
      for (size_t i = 0; i < blocks; ++i) {
        if (!servo->PopBlock(servo_block_.data(), block_frames)) break;
        ProcessCapture(servo_block_.data(), block_frames, channels, rate,
                       total_delay_ms);
      }
      return;
    }
  }
  ProcessCapture(in, frames, channels, rate, total_delay_ms);
}

void Engine::OnHardwareClock(const AudioHardwareClockObservation& o) {
  if (DriftServo* servo = servo_.load(std::memory_order_acquire))
    servo->OnHardwareClockObservation(o);
}

int32_t Engine::ClockCorrectionState(double* applied_ppm, bool* engaged) {
  const auto st = clock_->GetState();
  *applied_ppm = st.applied_ppm;
  *engaged = st.engaged;
  return st.mode;
}

void Engine::ProcessCapture(const int16_t* in, size_t frames, size_t channels,
                            uint32_t rate, uint32_t total_delay_ms) {
  if (rate % 100 != 0 || frames * 100 != rate || channels < 1 ||
      frames > 480)
    return;
  for (size_t i = 0; i < frames; ++i) {
    float acc = 0;
    for (size_t c = 0; c < channels; ++c) acc += in[i * channels + c];
    cap_f_[i] = acc / static_cast<float>(channels);
  }
  if (static_cast<int>(rate) != capture_in_rate_) {
    capture_in_rate_ = static_cast<int>(rate);
    capture_in_rs_ = std::make_unique<BlockResampler>(rate, kMixRate, 1);
  }
  capture_in_rs_->Process(cap_f_.data(), cap_f48_.data());
  for (int i = 0; i < kMixFrames; ++i) cap_s48_[i] = ToS16(cap_f48_[i]);

  const webrtc::StreamConfig cc(kMixRate, 1);
  double pre = 0, post = 0;
  for (int i = 0; i < kMixFrames; ++i)
    pre += static_cast<double>(cap_s48_[i]) * cap_s48_[i];
  rtc_->apm->set_stream_delay_ms(static_cast<int>(total_delay_ms));
  rtc_->apm->ProcessStream(cap_s48_.data(), cc, cc, cap_s48_.data());
  for (int i = 0; i < kMixFrames; ++i)
    post += static_cast<double>(cap_s48_[i]) * cap_s48_[i];
  cap_energy_pre_.fetch_add(pre, std::memory_order_relaxed);
  cap_energy_post_.fetch_add(post, std::memory_order_relaxed);
  cap_energy_blocks_.fetch_add(1, std::memory_order_relaxed);

  const int out_rate = capture_rate_.load(std::memory_order_relaxed);
  CaptureBlock* b = capture_ring_.Reserve();
  if (!b) {
    capture_dropped_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  b->t_ns = NowNs();
  b->rate = out_rate;
  b->frames = out_rate / 100;
  if (out_rate == kMixRate) {
    std::memcpy(b->data, cap_s48_.data(), kMixFrames * sizeof(int16_t));
  } else {
    if (out_rate != capture_out_rate_) {
      capture_out_rate_ = out_rate;
      capture_out_rs_ = std::make_unique<BlockResampler>(kMixRate, out_rate, 1);
    }
    for (int i = 0; i < kMixFrames; ++i) cap_f48_[i] = cap_s48_[i];
    capture_out_rs_->Process(cap_f48_.data(), cap_fout_.data());
    for (int i = 0; i < b->frames; ++i) b->data[i] = ToS16(cap_fout_[i]);
  }
  capture_ring_.Commit();
  capture_signal_.store(true, std::memory_order_release);
  notify_wakeup_.Signal();
}

int32_t Engine::ReadCapture(CaptureBlock* out, int32_t max_blocks) {
  int32_t n = 0;
  while (n < max_blocks && capture_ring_.Pop(&out[n])) ++n;
  return n;
}

void Engine::TakeCaptureEnergy(double* pre, double* post, int64_t* blocks) {
  *pre = cap_energy_pre_.exchange(0);
  *post = cap_energy_post_.exchange(0);
  *blocks = cap_energy_blocks_.exchange(0);
}

double Engine::EchoReturnLossEnhancement() const {
  const auto stats = rtc_->apm->GetStatistics();
  return stats.echo_return_loss_enhancement.value_or(NAN);
}

// ---- Capture control and devices -----------------------------------------

int32_t Engine::StartCapture(int32_t rate, int32_t request_id) {
  if (rate < 8000 || rate > 48000 || rate % 100 != 0)
    return kErrInvalidArgument;
  capture_rate_.store(rate);
  if (!servo_hold_) {
    auto snapshot = clock_->Read(/*capture=*/true);
    servo_hold_ = snapshot.servo;
    servo_observe_only_.store(snapshot.observe_only);
    servo_.store(servo_hold_.get(), std::memory_order_release);
  }
  if (config_.manual_device) {
    capture_active_.store(true);
    Notify(static_cast<int32_t>(NotifyKind::kRequestDone), request_id, kOk);
    NotifierPass();
    return kOk;
  }
  device_thread_->Post([this, request_id] {
    int32_t result = kOk;
    auto& adm = rtc_->adm;
    if (!adm->Recording()) {
      ApplySessionProfile(/*capture=*/true);
      if (!ApplySelectedDevice(/*input=*/true) || adm->InitRecording() != 0 ||
          adm->StartRecording() != 0)
        result = kErrDevice;
    }
    capture_active_.store(result == kOk);
    Notify(static_cast<int32_t>(NotifyKind::kRequestDone), request_id, result);
    control_wakeup_.Signal();
  });
  return kOk;
}

int32_t Engine::StopCapture(int32_t request_id) {
  capture_active_.store(false);
  if (config_.manual_device) {
    Notify(static_cast<int32_t>(NotifyKind::kRequestDone), request_id, kOk);
    NotifierPass();
    return kOk;
  }
  device_thread_->Post([this, request_id] {
    if (rtc_->adm->Recording()) rtc_->adm->StopRecording();
    Notify(static_cast<int32_t>(NotifyKind::kRequestDone), request_id, kOk);
  });
  return kOk;
}

void Engine::RefreshDevices() {
  std::vector<DeviceInfo> outs, ins;
  DeviceInfo cur_out, cur_in;
  bool has_out = false, has_in = false;
#if defined(WEBRTC_IOS)
  // iOS routes come from the audio session, not the device module.
  for (auto& [id, n] : SessionOutputs()) outs.push_back({id, n});
  for (auto& [id, n] : SessionInputs()) ins.push_back({id, n});
  has_out = SessionCurrentOutput(&cur_out);
  has_in = SessionCurrentInput(&cur_in);
#else
  auto& adm = rtc_->adm;
  auto read = [&](bool input) {
    std::vector<DeviceInfo> list;
    char name[webrtc::kAdmMaxDeviceNameSize];
    char guid[webrtc::kAdmMaxGuidSize];
    const int n = input ? adm->RecordingDevices() : adm->PlayoutDevices();
    for (int i = 0; i < n; ++i) {
      name[0] = guid[0] = 0;
      if ((input ? adm->RecordingDeviceName(i, name, guid)
                 : adm->PlayoutDeviceName(i, name, guid)) != 0)
        continue;
      DeviceInfo d{guid[0] ? guid : std::to_string(i), name};
#if defined(WEBRTC_LINUX) && !defined(WEBRTC_ANDROID)
      // PulseAudio and ALSA list the system default first, without an ID.
      if (i == 0 && !guid[0]) d.id = "default";
#endif
      if (d.id == "default") d.name = DefaultEntryName(d.name);
      list.push_back(std::move(d));
    }
#if defined(WEBRTC_WIN)
    // The Core Audio ADM has no "default" entry. The engine's list starts
    // with one, which AdmSelect maps to the default device.
    const std::string def = WinDefaultEndpointId(input);
    std::string def_name = "System default";
    for (const auto& d : list)
      if (!def.empty() && d.id == def) def_name = d.name;
    list.insert(list.begin(), DeviceInfo{"default", def_name});
#endif
    return list;
  };
  // Outputs first: on Android, PlayoutDevices() reads the device list.
  outs = read(/*input=*/false);
  ins = read(/*input=*/true);
#if defined(WEBRTC_ANDROID)
  if (rtc_->android_routes) {
    cur_out = rtc_->android_routes->CurrentOutput();
    cur_in = rtc_->android_routes->CurrentInput();
    has_out = has_in = true;
  }
#else
  has_out = ResolveCurrent(/*input=*/false, outs, &cur_out);
  has_in = ResolveCurrent(/*input=*/true, ins, &cur_in);
#endif
#endif
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(devices_mu_);
    changed = outs != outputs_ || ins != inputs_ ||
              has_out != has_current_output_ ||
              has_in != has_current_input_ || cur_out != current_output_ ||
              cur_in != current_input_;
    outputs_ = std::move(outs);
    inputs_ = std::move(ins);
    current_output_ = std::move(cur_out);
    current_input_ = std::move(cur_in);
    has_current_output_ = has_out;
    has_current_input_ = has_in;
  }
  if (changed)
    Notify(static_cast<int32_t>(NotifyKind::kDevicesChanged), 0, 0);
}

bool Engine::ResolveCurrent(bool input, const std::vector<DeviceInfo>& list,
                            DeviceInfo* out) {
  if (list.empty()) return false;
  // A selection whose device is gone acts as the first entry, the default
  // (ApplySelectedDevice).
  const std::string& selected = input ? selected_input_ : selected_output_;
  const DeviceInfo* d = &list[0];
  for (const auto& e : list)
    if (!selected.empty() && e.id == selected) d = &e;
  *out = *d;
#if defined(__APPLE__) && !defined(WEBRTC_IOS)
  MacDeviceDetails(out->id, input, &out->name, &out->kind);
#endif
  return true;
}

bool Engine::CurrentDevice(bool input, DeviceInfo* out) {
  std::lock_guard<std::mutex> lock(devices_mu_);
  if (!(input ? has_current_input_ : has_current_output_)) return false;
  *out = input ? current_input_ : current_output_;
  return true;
}

int32_t Engine::AdmSelect(bool input, int index) {
  auto& adm = rtc_->adm;
#if defined(WEBRTC_WIN)
  if (index == 0) {
    constexpr auto kDefault = webrtc::AudioDeviceModule::kDefaultDevice;
    return input ? adm->SetRecordingDevice(kDefault)
                 : adm->SetPlayoutDevice(kDefault);
  }
  --index;
#endif
  return input ? adm->SetRecordingDevice(index) : adm->SetPlayoutDevice(index);
}

std::vector<DeviceInfo> Engine::Devices(bool input) {
  std::lock_guard<std::mutex> lock(devices_mu_);
  return input ? inputs_ : outputs_;
}

int Engine::DeviceIndex(bool input, const std::string& id) {
  if (id.empty()) return -1;
  std::lock_guard<std::mutex> lock(devices_mu_);
  const auto& list = input ? inputs_ : outputs_;
  for (size_t i = 0; i < list.size(); ++i)
    if (list[i].id == id) return static_cast<int>(i);
  return -1;
}

bool Engine::ApplySelectedDevice(bool input) {
#if defined(WEBRTC_ANDROID)
  // The Android device keeps the selection, also while the selected device
  // is gone, and a selection in one direction can change the other
  // (android_routes.h).
  (void)input;
  return true;
#else
  const int index =
      DeviceIndex(input, input ? selected_input_ : selected_output_);
  return AdmSelect(input, index < 0 ? 0 : index) == 0;
#endif
}

int32_t Engine::SelectOutput(const std::string& id, int32_t request_id) {
  if (config_.manual_device) {
    Notify(static_cast<int32_t>(NotifyKind::kRequestDone), request_id, kOk);
    NotifierPass();
    return kOk;
  }
  device_thread_->Post([this, id, request_id] {
#if defined(WEBRTC_IOS)
    const bool ok = SessionSelectOutput(id);
    RefreshDevices();
    Notify(static_cast<int32_t>(NotifyKind::kRequestDone), request_id,
           ok ? kOk : kErrInvalidArgument);
    return;
#endif
    const int index = DeviceIndex(/*input=*/false, id);
    int32_t result = kOk;
    if (index < 0) {
      result = kErrInvalidArgument;
    } else {
      auto& adm = rtc_->adm;
#if defined(WEBRTC_ANDROID)
      // The Android device moves its open streams itself, and only when
      // the route changes (ADR I10 through the restart callback).
      if (AdmSelect(/*input=*/false, index) != 0) result = kErrDevice;
      // A stream that did not open again: playout demand starts it.
      if (!adm->Playing()) playout_running_.store(false);
#else
      const bool was_playing = adm->Playing();
      if (was_playing) {
        MarkOutputRestart();
        adm->StopPlayout();
      }
      const std::string previous = selected_output_;
      selected_output_ = id;
      if (AdmSelect(/*input=*/false, index) != 0) {
        result = kErrDevice;
        selected_output_ = previous;
      }
      if (was_playing &&
          (adm->InitPlayout() != 0 || adm->StartPlayout() != 0)) {
        result = kErrDevice;
        playout_running_.store(false);
      }
#endif
    }
    // The current device is new when the request completes.
    RefreshDevices();
    Notify(static_cast<int32_t>(NotifyKind::kRequestDone), request_id, result);
  });
  return kOk;
}

int32_t Engine::SelectInput(const std::string& id, int32_t request_id) {
  if (config_.manual_device) {
    Notify(static_cast<int32_t>(NotifyKind::kRequestDone), request_id, kOk);
    NotifierPass();
    return kOk;
  }
  device_thread_->Post([this, id, request_id] {
#if defined(WEBRTC_IOS)
    const bool ok = SessionSelectInput(id);
    RefreshDevices();
    Notify(static_cast<int32_t>(NotifyKind::kRequestDone), request_id,
           ok ? kOk : kErrInvalidArgument);
    return;
#endif
    const int index = DeviceIndex(/*input=*/true, id);
    int32_t result = kOk;
    if (index < 0) {
      result = kErrInvalidArgument;
    } else {
      auto& adm = rtc_->adm;
#if defined(WEBRTC_ANDROID)
      // A Bluetooth microphone also moves the output (android_routes.h).
      if (AdmSelect(/*input=*/true, index) != 0) result = kErrDevice;
      if (!adm->Playing()) playout_running_.store(false);
#else
      const bool was_recording = adm->Recording();
      if (was_recording) adm->StopRecording();
      const std::string previous = selected_input_;
      selected_input_ = id;
      if (AdmSelect(/*input=*/true, index) != 0) {
        result = kErrDevice;
        selected_input_ = previous;
      }
      if (was_recording &&
          (adm->InitRecording() != 0 || adm->StartRecording() != 0))
        result = kErrDevice;
#endif
    }
    RefreshDevices();
    Notify(static_cast<int32_t>(NotifyKind::kRequestDone), request_id, result);
  });
  return kOk;
}

// ---- Control and notifier threads ---------------------------------------

void Engine::Notify(int32_t kind, int32_t id, int64_t value) {
  {
    std::lock_guard<std::mutex> lock(events_mu_);
    events_.push_back({kind, id, value});
  }
  notify_wakeup_.Signal();
}

void Engine::ControlPass() {
  {
    std::lock_guard<std::mutex> lock(tracks_mu_);
    for (auto& s : slots_) {
      Track* t = s.load(std::memory_order_acquire);
      if (t) t->store().Maintain(t->playhead(), retirer_);
    }
  }
  retirer_.Reclaim();
}

void Engine::UpdatePlayoutDemand() {
  const int64_t now = MonotonicNowNs();
  bool demand = capture_active_.load();
  for (auto& s : slots_) {
    Track* t = s.load(std::memory_order_acquire);
    if (t && t->wants_output()) demand = true;
  }
  // A queued play() has not reached the track yet.
  if (commands_.Size() > 0) demand = true;
  if (demand) idle_since_ns_ = 0;
  const bool running = playout_running_.load();
  if (demand && !running && !playout_pending_.exchange(true)) {
    device_thread_->Post([this] {
      auto& adm = rtc_->adm;
      ApplySessionProfile(capture_active_.load());
      int32_t ok = ApplySelectedDevice(/*input=*/false) &&
                   adm->InitPlayout() == 0 && adm->StartPlayout() == 0;
      playout_running_.store(ok);
      playout_pending_.store(false);
      if (!ok)
        Notify(static_cast<int32_t>(NotifyKind::kEngineError), 0, kErrDevice);
      Notify(static_cast<int32_t>(NotifyKind::kOutputState), 0, ok);
    });
  } else if (!demand && running) {
    if (idle_since_ns_ == 0) idle_since_ns_ = now;
    if (now - idle_since_ns_ >
            static_cast<int64_t>(config_.idle_stop_seconds * 1e9) &&
        !playout_pending_.exchange(true)) {
      idle_since_ns_ = 0;
      device_thread_->Post([this] {
        if (rtc_->adm->Playing()) rtc_->adm->StopPlayout();
#if defined(WEBRTC_IOS)
        // Back to the media profile (D6) at the next start; let other apps'
        // audio resume now.
        if (!rtc_->adm->Recording()) {
          DeactivateSession();
          session_profile_ = 0;
        }
#endif
        playout_running_.store(false);
        playout_pending_.store(false);
        Notify(static_cast<int32_t>(NotifyKind::kOutputState), 0, 0);
      });
    }
  }
  if (running && now - last_delay_poll_ns_ > 250'000'000) {
    last_delay_poll_ns_ = now;
    device_thread_->Post([this] {
#if defined(WEBRTC_IOS)
      // AudioEngineDevice reports 0 ms (ADR risk 5).
      device_delay_ns_.store(SessionOutputDelayNs());
#else
      uint16_t ms = 0;
      if (rtc_->adm->PlayoutDelay(&ms) == 0)
        device_delay_ns_.store(int64_t{ms} * 1000000);
#endif
    });
  }
  // The platform reported a device or route change: read the lists now.
  const uint32_t gen = PlatformDevicesGeneration();
  if (gen != devices_generation_) {
    devices_generation_ = gen;
    last_device_poll_ns_ = 0;
  }
  if (now - last_device_poll_ns_ > 2'000'000'000) {
    last_device_poll_ns_ = now;
    device_thread_->Post([this] { RefreshDevices(); });
  }
}

void Engine::ControlLoop() {
  while (!stopping_.load()) {
    control_wakeup_.Wait(20);
    if (stopping_.load()) break;
    ControlPass();
    UpdatePlayoutDemand();
    // No render in the last 50 ms: apply commands here.
    if (MonotonicNowNs() - last_render_ns_.load() > 50'000'000) IdleApply();
  }
}

void Engine::NotifierPass() {
  NotifyFn fn = config_.notify;
  TrackEvent e;
  while (rt_events_.Pop(&e))
    if (fn) fn(e.kind, e.track_id, e.value);
  std::deque<TrackEvent> pending;
  {
    std::lock_guard<std::mutex> lock(events_mu_);
    pending.swap(events_);
  }
  for (const auto& p : pending)
    if (fn) fn(p.kind, p.track_id, p.value);
  if (capture_signal_.exchange(false) && fn)
    fn(static_cast<int32_t>(NotifyKind::kCaptureReady), 0,
       static_cast<int64_t>(capture_ring_.Size()));
}

void Engine::NotifierLoop() {
  while (!stopping_.load()) {
    notify_wakeup_.Wait(50);
    NotifierPass();
  }
}

// ---- Manual device --------------------------------------------------------

int32_t Engine::ManualRender(int32_t blocks, int16_t* out,
                             const int16_t* capture_in) {
  if (!config_.manual_device || blocks < 0) return kErrInvalidArgument;
  const int ch = std::clamp(config_.manual_output_channels, 1, 2);
  for (int32_t b = 0; b < blocks; ++b) {
    ControlPass();
    RenderBlock(out + static_cast<size_t>(b) * kMixFrames * ch, kMixFrames,
                ch, kMixRate);
    if (capture_in)
      CaptureBlockIn(capture_in + static_cast<size_t>(b) * kMixFrames,
                     kMixFrames, 1, kMixRate, config_.manual_delay_ms);
    manual_now_ns_ += 10'000'000;
  }
  ControlPass();
  NotifierPass();
  return kOk;
}

}  // namespace tsnx
