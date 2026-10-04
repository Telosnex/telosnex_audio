// Minimal "libwebrtc as an audio engine" probe: no PeerConnection, no
// VoiceEngine, no AudioState. The app owns ADM + APM + mixer directly.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "api/audio/audio_device.h"
#include "api/audio/audio_frame.h"
#include "api/audio/audio_mixer.h"
#include "api/audio/audio_processing.h"
#include "api/audio/builtin_audio_processing_builder.h"
#include "api/audio/create_audio_device_module.h"
#include "api/environment/environment.h"
#include "api/environment/environment_factory.h"
#include "api/make_ref_counted.h"
#include "modules/audio_mixer/audio_mixer_impl.h"

using namespace webrtc;

// A mixer source: 24 kHz mono PCM pulled on the render clock (stand-in for
// the app's PCM playout queue / TTS / Live output).
class ToneSource : public AudioMixer::Source {
 public:
  explicit ToneSource(int hz) : hz_(hz) {}
  AudioFrameInfo GetAudioFrameWithInfo(int sample_rate_hz, AudioFrame* f) override {
    const size_t n = sample_rate_hz / 100;
    f->UpdateFrame(0, nullptr, n, sample_rate_hz, AudioFrame::kNormalSpeech,
                   AudioFrame::kVadActive, 1);
    int16_t* d = f->mutable_data();
    for (size_t i = 0; i < n; ++i, ++t_)
      d[i] = static_cast<int16_t>(4000 * std::sin(2 * M_PI * hz_ * t_ / sample_rate_hz));
    return AudioFrameInfo::kNormal;
  }
  int Ssrc() const override { return 1; }
  int PreferredSampleRate() const override { return 24000; }
 private:
  int hz_; uint64_t t_ = 0;
};

// Offline proof that the APM's AEC3 uses the render reference: feed a render
// signal, synthesize a delayed+attenuated echo as "mic" input, measure ERLE.
static void OfflineAec(AudioProcessing* apm) {
  const int rate = 48000, n = rate / 100, delay_frames = 6;  // 60 ms echo path
  StreamConfig cfg(rate, 1);
  std::vector<std::vector<float>> history;
  double in_e = 0, out_e = 0; uint32_t seed = 1;
  for (int k = 0; k < 1000; ++k) {  // 10 s
    std::vector<float> render(n), cap(n);
    for (int i = 0; i < n; ++i) {  // speech-ish: modulated noise
      seed = seed * 1664525u + 1013904223u;
      float noise = ((seed >> 9) / 8388608.0f - 1.0f);
      render[i] = 9000.f * noise * (0.6f + 0.4f * std::sin(2 * M_PI * 3 * (k * n + i) / rate));
    }
    history.push_back(render);
    const float* rptr = render.data();
    apm->ProcessReverseStream(&rptr, cfg, cfg, (float* const*)&rptr);
    if ((int)history.size() > delay_frames)
      for (int i = 0; i < n; ++i) cap[i] = 0.5f * history[history.size() - 1 - delay_frames][i];
    for (int i = 0; i < n; ++i) cap[i] /= 32768.f;
    if (k >= 500) for (float s : cap) in_e += s * s;
    float* cptr = cap.data();
    apm->set_stream_delay_ms(0);
    apm->ProcessStream(&cptr, cfg, cfg, &cptr);
    if (k >= 500) for (float s : cap) out_e += s * s;
  }
  auto st = apm->GetStatistics();
  std::printf("offline AEC3: echo in %.1f dB, out %.1f dB, suppression %.1f dB; APM delay est=%s ms\n",
              10 * std::log10(in_e + 1e-12), 10 * std::log10(out_e + 1e-12),
              10 * std::log10((in_e + 1e-12) / (out_e + 1e-12)),
              st.delay_ms ? std::to_string(*st.delay_ms).c_str() : "n/a");
}

// The whole "engine": capture -> APM -> tap; mixer -> APM reverse -> speaker.
class Engine : public AudioTransport {
 public:
  Engine(AudioProcessing* apm, AudioMixer* mixer) : apm_(apm), mixer_(mixer) {}
  int32_t RecordedDataIsAvailable(const void* audio, size_t samples, size_t bps,
                                  size_t ch, uint32_t rate, uint32_t delay_ms,
                                  int32_t, uint32_t, bool, uint32_t&) override {
    rec_.fetch_add(1); return 0;  // probe never opens the mic
  }
  int32_t NeedMorePlayData(size_t samples, size_t bps, size_t ch, uint32_t rate,
                           void* out, size_t& samples_out, int64_t*, int64_t*) override {
    frame_.sample_rate_hz_ = rate;
    mixer_->Mix(ch, &frame_);
    StreamConfig rc(frame_.sample_rate_hz_, frame_.num_channels_);
    apm_->ProcessReverseStream(frame_.data(), rc, rc, frame_.mutable_data());  // AEC render reference
    std::memcpy(out, frame_.data(), samples * ch * sizeof(int16_t));
    samples_out = samples;
    play_.fetch_add(1); return 0;
  }
  void PullRenderData(int, int, size_t, size_t, void*, int64_t*, int64_t*) override {}
  std::atomic<int> rec_{0}, play_{0};
 private:
  AudioProcessing* apm_; AudioMixer* mixer_; AudioFrame frame_;
};

int main(int argc, char** argv) {
  const bool play = argc > 1 && std::strcmp(argv[1], "--play") == 0;
  Environment env = CreateEnvironment();
  AudioProcessing::Config c;
  c.echo_canceller.enabled = true;
  c.noise_suppression.enabled = true;
  c.gain_controller2.enabled = true;
  c.high_pass_filter.enabled = true;
  scoped_refptr<AudioProcessing> apm = BuiltinAudioProcessingBuilder(c).Build(env);
  std::printf("APM built: aec=%d ns=%d agc2=%d hpf=%d\n", apm->GetConfig().echo_canceller.enabled,
              apm->GetConfig().noise_suppression.enabled, apm->GetConfig().gain_controller2.enabled,
              apm->GetConfig().high_pass_filter.enabled);
  OfflineAec(apm.get());

  scoped_refptr<AudioProcessing> apm_live = BuiltinAudioProcessingBuilder(c).Build(env);
  scoped_refptr<AudioMixerImpl> mixer = AudioMixerImpl::Create();
  ToneSource tone(440);
  mixer->AddSource(&tone);
  Engine engine(apm_live.get(), mixer.get());

  scoped_refptr<AudioDeviceModule> adm =
      CreateAudioDeviceModule(env, AudioDeviceModule::kPlatformDefaultAudio);
  if (!adm || adm->Init() != 0) { std::printf("ADM init failed\n"); return 1; }
  char name[kAdmMaxDeviceNameSize], guid[kAdmMaxGuidSize];
  std::printf("ADM: %d playout devices, %d recording devices\n", adm->PlayoutDevices(), adm->RecordingDevices());
  for (int i = 0; i < adm->PlayoutDevices(); ++i)
    if (adm->PlayoutDeviceName(i, name, guid) == 0) std::printf("  out[%d] %s\n", i, name);
  adm->RegisterAudioCallback(&engine);
  if (play) {
    adm->SetPlayoutDevice(AudioDeviceModule::kDefaultDevice);
    adm->InitPlayout(); adm->StartPlayout();
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    adm->StopPlayout();
    std::printf("played 700 ms: %d render callbacks through mixer+APM\n", engine.play_.load());
  }
  adm->RegisterAudioCallback(nullptr);
  adm->Terminate();
  return 0;
}
