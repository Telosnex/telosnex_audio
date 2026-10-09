#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "engine.h"
#include "tsnx_audio.h"
#include "util/dart_cobject.h"

struct tsnx_engine {
  std::unique_ptr<tsnx::Engine> engine;
  // The owner's event port (tsnx_engine_config); post_cobject is null when
  // events go to a callback.
  int64_t notify_port = 0;
  tsnx_post_cobject_fn post_cobject = nullptr;
};

static_assert(sizeof(tsnx_track_state) == sizeof(tsnx::TrackStateWords));
static_assert(sizeof(tsnx_capture_block) == sizeof(tsnx::CaptureBlock));
static_assert(static_cast<int>(tsnx::DeviceKind::kOther) ==
                  TSNX_DEVICE_KIND_OTHER &&
              static_cast<int>(tsnx::DeviceKind::kSpeaker) ==
                  TSNX_DEVICE_KIND_SPEAKER &&
              static_cast<int>(tsnx::DeviceKind::kEarpiece) ==
                  TSNX_DEVICE_KIND_EARPIECE &&
              static_cast<int>(tsnx::DeviceKind::kMicrophone) ==
                  TSNX_DEVICE_KIND_MICROPHONE &&
              static_cast<int>(tsnx::DeviceKind::kWired) ==
                  TSNX_DEVICE_KIND_WIRED &&
              static_cast<int>(tsnx::DeviceKind::kUsb) == TSNX_DEVICE_KIND_USB &&
              static_cast<int>(tsnx::DeviceKind::kBluetooth) ==
                  TSNX_DEVICE_KIND_BLUETOOTH &&
              static_cast<int>(tsnx::DeviceKind::kAirPlay) ==
                  TSNX_DEVICE_KIND_AIRPLAY);

namespace {
tsnx::Engine* E(tsnx_engine* e) { return e ? e->engine.get() : nullptr; }

// Open engines. A Flutter hot restart ends the isolate that opened an engine
// without closing it; the engine keeps the device. tsnx_engine_open closes
// such engines so that one engine owns the device (ADR I9).
std::mutex& OpenEnginesMutex() {
  static std::mutex mu;
  return mu;
}
std::vector<tsnx_engine*>& OpenEngines() {
  static auto* engines = new std::vector<tsnx_engine*>();
  return *engines;
}

// True when the owner's event port is closed: its isolate is gone. A live
// owner gets a null message, which it ignores.
bool OwnerGone(tsnx_engine* e) {
  if (!e->post_cobject) return false;
  tsnx::DartCObject ping;
  ping.type = tsnx::kDartCObjectNull;
  return !e->post_cobject(e->notify_port, &ping);
}

// Closes the engines whose owner is gone.
void CloseOrphans() {
  std::vector<tsnx_engine*> orphans;
  {
    std::lock_guard<std::mutex> lock(OpenEnginesMutex());
    auto& open = OpenEngines();
    auto gone = std::stable_partition(
        open.begin(), open.end(), [](tsnx_engine* e) { return !OwnerGone(e); });
    orphans.assign(gone, open.end());
    open.erase(gone, open.end());
  }
  for (auto* e : orphans) delete e;
}
int64_t MapWrite(int64_t r) {
  switch (r) {
    case tsnx::kStoreFull:
      return TSNX_WRITE_FULL;
    case tsnx::kStoreEnded:
      return TSNX_WRITE_ENDED;
    case tsnx::kStoreIoError:
      return TSNX_WRITE_IO_ERROR;
    default:
      return r;
  }
}
// Truncates at a UTF-8 character boundary: Dart rejects a cut character.
void CopyStr(const std::string& s, char* out, int32_t cap) {
  if (!out || cap <= 0) return;
  size_t n = std::min<size_t>(s.size(), static_cast<size_t>(cap - 1));
  while (n > 0 && n < s.size() && (s[n] & 0xC0) == 0x80) --n;
  std::memcpy(out, s.data(), n);
  out[n] = 0;
}
}  // namespace

extern "C" {

TSNX_EXPORT int32_t tsnx_engine_open(const tsnx_engine_config* c,
                                     tsnx_engine** out) {
  if (!c || !out) return TSNX_ERR_INVALID_ARGUMENT;
  tsnx::EngineConfig config;
  config.manual_device = c->manual_device != 0;
  config.manual_output_channels = c->manual_output_channels;
  config.manual_delay_ms = c->manual_delay_ms;
  config.echo_cancellation = c->echo_cancellation != 0;
  config.noise_suppression = c->noise_suppression != 0;
  config.auto_gain = c->auto_gain != 0;
  config.platform_voice_processing = c->platform_voice_processing != 0;
  config.clock_correction = c->clock_correction;
  config.linux_audio_backend = c->linux_audio_backend;
  if (c->spill_dir) config.spill_dir = c->spill_dir;
  config.notify = c->notify;
  config.notify_port = c->notify_port;
  config.post_cobject = c->post_cobject;
  // Before Open, so that the orphan releases the device first.
  CloseOrphans();
  int32_t error = 0;
  auto engine = tsnx::Engine::Open(config, &error);
  if (!engine) return error ? error : TSNX_ERR_DEVICE;
  auto* e = new tsnx_engine{std::move(engine), c->notify_port, c->post_cobject};
  {
    std::lock_guard<std::mutex> lock(OpenEnginesMutex());
    OpenEngines().push_back(e);
  }
  *out = e;
  return TSNX_OK;
}

TSNX_EXPORT void tsnx_engine_close(tsnx_engine* e) {
  if (!e) return;
  {
    std::lock_guard<std::mutex> lock(OpenEnginesMutex());
    auto& open = OpenEngines();
    open.erase(std::remove(open.begin(), open.end(), e), open.end());
  }
  delete e;
}

TSNX_EXPORT int64_t tsnx_engine_now_ns(tsnx_engine* e) {
  return E(e) ? E(e)->NowNs() : 0;
}

TSNX_EXPORT int32_t tsnx_engine_manual_render(tsnx_engine* e, int32_t blocks,
                                              int16_t* out,
                                              const int16_t* capture_in) {
  if (!E(e) || !out) return TSNX_ERR_INVALID_ARGUMENT;
  return E(e)->ManualRender(blocks, out, capture_in);
}

TSNX_EXPORT int32_t tsnx_engine_manual_output_restart(tsnx_engine* e,
                                                      int32_t gap_ms) {
  return E(e) ? E(e)->ManualOutputRestart(gap_ms) : TSNX_ERR_INVALID_ARGUMENT;
}

TSNX_EXPORT double tsnx_engine_erle_db(tsnx_engine* e) {
  return E(e) ? E(e)->EchoReturnLossEnhancement() : 0;
}

TSNX_EXPORT int64_t tsnx_engine_render_format_changes(tsnx_engine* e) {
  return E(e) ? E(e)->render_format_changes() : -1;
}

TSNX_EXPORT int64_t tsnx_engine_take_capture_energy(tsnx_engine* e,
                                                    double* pre_apm,
                                                    double* post_apm) {
  if (!E(e) || !pre_apm || !post_apm) return TSNX_ERR_INVALID_ARGUMENT;
  int64_t blocks = 0;
  E(e)->TakeCaptureEnergy(pre_apm, post_apm, &blocks);
  return blocks;
}

TSNX_EXPORT int32_t tsnx_engine_output_delay_ms(tsnx_engine* e) {
  return E(e) ? static_cast<int32_t>(E(e)->device_delay_ms()) : 0;
}

TSNX_EXPORT int32_t tsnx_engine_clock_state(tsnx_engine* e, double* applied_ppm,
                                            int32_t* engaged) {
  if (!E(e) || !applied_ppm || !engaged) return TSNX_ERR_INVALID_ARGUMENT;
  bool on = false;
  const int32_t mode = E(e)->ClockCorrectionState(applied_ppm, &on);
  *engaged = on;
  return mode;
}

TSNX_EXPORT int32_t tsnx_track_create(tsnx_engine* e, int32_t rate,
                                      int32_t channels, int32_t retention,
                                      int32_t* out_id) {
  if (!E(e) || !out_id || retention < 0 || retention > 1)
    return TSNX_ERR_INVALID_ARGUMENT;
  return E(e)->CreateTrack(rate, channels,
                           static_cast<tsnx::Retention>(retention), out_id);
}

TSNX_EXPORT int32_t tsnx_track_create_mp3(tsnx_engine* e, const uint8_t* data,
                                          int64_t size, int32_t* out_id) {
  if (!E(e) || !data || size <= 0 || !out_id) return TSNX_ERR_INVALID_ARGUMENT;
  return E(e)->CreateMp3Track(data, static_cast<size_t>(size), out_id);
}

TSNX_EXPORT int64_t tsnx_track_write(tsnx_engine* e, int32_t id,
                                     const int16_t* pcm, int64_t frames) {
  if (!E(e) || (!pcm && frames > 0)) return TSNX_ERR_INVALID_ARGUMENT;
  return MapWrite(E(e)->Write(id, pcm, frames));
}

TSNX_EXPORT int32_t tsnx_track_end_of_stream(tsnx_engine* e, int32_t id) {
  return E(e) ? E(e)->EndOfStream(id) : TSNX_ERR_INVALID_ARGUMENT;
}
TSNX_EXPORT int32_t tsnx_track_play(tsnx_engine* e, int32_t id) {
  return E(e) ? E(e)->Play(id) : TSNX_ERR_INVALID_ARGUMENT;
}
TSNX_EXPORT int32_t tsnx_track_pause(tsnx_engine* e, int32_t id) {
  return E(e) ? E(e)->Pause(id) : TSNX_ERR_INVALID_ARGUMENT;
}
TSNX_EXPORT int32_t tsnx_track_seek(tsnx_engine* e, int32_t id,
                                    int64_t frame) {
  return E(e) ? E(e)->Seek(id, frame) : TSNX_ERR_INVALID_ARGUMENT;
}
TSNX_EXPORT int32_t tsnx_track_set_rate(tsnx_engine* e, int32_t id,
                                        double rate) {
  return E(e) ? E(e)->SetRate(id, rate) : TSNX_ERR_INVALID_ARGUMENT;
}
TSNX_EXPORT int32_t tsnx_track_set_gain(tsnx_engine* e, int32_t id,
                                        double gain, int32_t ramp_ms) {
  return E(e) ? E(e)->SetGain(id, gain, ramp_ms) : TSNX_ERR_INVALID_ARGUMENT;
}
TSNX_EXPORT int32_t tsnx_track_flush(tsnx_engine* e, int32_t id) {
  return E(e) ? E(e)->Flush(id) : TSNX_ERR_INVALID_ARGUMENT;
}
TSNX_EXPORT int32_t tsnx_track_get_state(tsnx_engine* e, int32_t id,
                                     tsnx_track_state* out) {
  if (!E(e) || !out) return TSNX_ERR_INVALID_ARGUMENT;
  return E(e)->GetState(id, reinterpret_cast<tsnx::TrackStateWords*>(out));
}
TSNX_EXPORT int32_t tsnx_track_format(tsnx_engine* e, int32_t id,
                                      int32_t* rate, int32_t* channels) {
  if (!E(e) || !rate || !channels) return TSNX_ERR_INVALID_ARGUMENT;
  return E(e)->TrackFormat(id, rate, channels);
}
TSNX_EXPORT int32_t tsnx_track_dispose(tsnx_engine* e, int32_t id) {
  return E(e) ? E(e)->DisposeTrack(id) : TSNX_ERR_INVALID_ARGUMENT;
}

TSNX_EXPORT int32_t tsnx_capture_start(tsnx_engine* e, int32_t rate,
                                       int32_t request_id) {
  return E(e) ? E(e)->StartCapture(rate, request_id)
              : TSNX_ERR_INVALID_ARGUMENT;
}
TSNX_EXPORT int32_t tsnx_capture_stop(tsnx_engine* e, int32_t request_id) {
  return E(e) ? E(e)->StopCapture(request_id) : TSNX_ERR_INVALID_ARGUMENT;
}
TSNX_EXPORT int32_t tsnx_capture_read(tsnx_engine* e, tsnx_capture_block* out,
                                      int32_t max_blocks) {
  if (!E(e) || !out || max_blocks < 0) return TSNX_ERR_INVALID_ARGUMENT;
  return E(e)->ReadCapture(reinterpret_cast<tsnx::CaptureBlock*>(out),
                           max_blocks);
}
TSNX_EXPORT int32_t tsnx_capture_dropped(tsnx_engine* e) {
  return E(e) ? E(e)->capture_dropped() : 0;
}

TSNX_EXPORT int32_t tsnx_device_count(tsnx_engine* e, int32_t kind) {
  if (!E(e)) return TSNX_ERR_INVALID_ARGUMENT;
  return static_cast<int32_t>(E(e)->Devices(kind == 1).size());
}
TSNX_EXPORT int32_t tsnx_device_get(tsnx_engine* e, int32_t kind,
                                    int32_t index, char* id, int32_t id_cap,
                                    char* name, int32_t name_cap) {
  if (!E(e)) return TSNX_ERR_INVALID_ARGUMENT;
  const auto list = E(e)->Devices(kind == 1);
  if (index < 0 || static_cast<size_t>(index) >= list.size())
    return TSNX_ERR_INVALID_ARGUMENT;
  CopyStr(list[index].id, id, id_cap);
  CopyStr(list[index].name, name, name_cap);
  return TSNX_OK;
}
TSNX_EXPORT int32_t tsnx_device_current(tsnx_engine* e, int32_t kind,
                                        char* id, int32_t id_cap, char* name,
                                        int32_t name_cap,
                                        int32_t* device_kind) {
  if (!E(e) || !device_kind) return TSNX_ERR_INVALID_ARGUMENT;
  tsnx::DeviceInfo d;
  if (!E(e)->CurrentDevice(kind == 1, &d)) return TSNX_ERR_DEVICE;
  CopyStr(d.id, id, id_cap);
  CopyStr(d.name, name, name_cap);
  *device_kind = static_cast<int32_t>(d.kind);
  return TSNX_OK;
}
TSNX_EXPORT int32_t tsnx_device_select(tsnx_engine* e, int32_t kind,
                                       const char* id, int32_t request_id) {
  if (!E(e) || !id) return TSNX_ERR_INVALID_ARGUMENT;
  return kind == 1 ? E(e)->SelectInput(id, request_id)
                   : E(e)->SelectOutput(id, request_id);
}

}  // extern "C"
