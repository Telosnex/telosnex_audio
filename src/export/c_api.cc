#include <cstring>
#include <string>

#include "engine.h"
#include "tsnx_audio.h"

struct tsnx_engine {
  std::unique_ptr<tsnx::Engine> engine;
};

static_assert(sizeof(tsnx_track_state) == sizeof(tsnx::TrackStateWords));
static_assert(sizeof(tsnx_capture_block) == sizeof(tsnx::CaptureBlock));

namespace {
tsnx::Engine* E(tsnx_engine* e) { return e ? e->engine.get() : nullptr; }
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
void CopyStr(const std::string& s, char* out, int32_t cap) {
  if (!out || cap <= 0) return;
  const size_t n = std::min<size_t>(s.size(), static_cast<size_t>(cap - 1));
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
  if (c->spill_dir) config.spill_dir = c->spill_dir;
  config.notify = c->notify;
  int32_t error = 0;
  auto engine = tsnx::Engine::Open(config, &error);
  if (!engine) return error ? error : TSNX_ERR_DEVICE;
  *out = new tsnx_engine{std::move(engine)};
  return TSNX_OK;
}

TSNX_EXPORT void tsnx_engine_close(tsnx_engine* e) { delete e; }

TSNX_EXPORT int64_t tsnx_engine_now_ns(tsnx_engine* e) {
  return E(e) ? E(e)->NowNs() : 0;
}

TSNX_EXPORT int32_t tsnx_engine_manual_render(tsnx_engine* e, int32_t blocks,
                                              int16_t* out,
                                              const int16_t* capture_in) {
  if (!E(e) || !out) return TSNX_ERR_INVALID_ARGUMENT;
  return E(e)->ManualRender(blocks, out, capture_in);
}

TSNX_EXPORT double tsnx_engine_erle_db(tsnx_engine* e) {
  return E(e) ? E(e)->EchoReturnLossEnhancement() : 0;
}

TSNX_EXPORT int64_t tsnx_engine_render_format_changes(tsnx_engine* e) {
  return E(e) ? E(e)->render_format_changes() : -1;
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
TSNX_EXPORT int32_t tsnx_device_select(tsnx_engine* e, int32_t kind,
                                       const char* id, int32_t request_id) {
  if (!E(e) || !id) return TSNX_ERR_INVALID_ARGUMENT;
  return kind == 1 ? E(e)->SelectInput(id, request_id)
                   : E(e)->SelectOutput(id, request_id);
}

}  // extern "C"
