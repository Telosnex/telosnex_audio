// telosnex_audio C API. Dart binds these with @Native (lib/src/ffi.dart).
//
// Threading: every function returns without waiting for the audio thread
// (ADR D10). Functions marked "leaf" are safe as Dart leaf calls. Results of
// device operations arrive through the notify callback as
// TSNX_NOTIFY_REQUEST_DONE.
#ifndef TSNX_AUDIO_H_
#define TSNX_AUDIO_H_

#include <stdint.h>

#if defined(_WIN32)
#define TSNX_EXPORT __declspec(dllexport)
#else
#define TSNX_EXPORT __attribute__((visibility("default")))
#endif

#define TSNX_VERSION_STRING "0.1.0"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tsnx_engine tsnx_engine;

// Error codes (negative). Zero is success.
#define TSNX_OK 0
#define TSNX_ERR_INVALID_ARGUMENT -1
#define TSNX_ERR_NO_FREE_TRACK -2
#define TSNX_ERR_NO_TRACK -3
#define TSNX_ERR_DEVICE -4
#define TSNX_ERR_UNSUPPORTED_FORMAT -5
#define TSNX_ERR_QUEUE_FULL -6
// Write results.
#define TSNX_WRITE_FULL -101
#define TSNX_WRITE_ENDED -102
#define TSNX_WRITE_IO_ERROR -103

// Notify kinds. 1..6 are track events: id = track id.
#define TSNX_NOTIFY_STARTED 1
#define TSNX_NOTIFY_STARVED 2
#define TSNX_NOTIFY_RESUMED 3
#define TSNX_NOTIFY_ENDED 4
#define TSNX_NOTIFY_FAILED 5
#define TSNX_NOTIFY_FLUSHED 6  // value: cut position, source frames
#define TSNX_NOTIFY_CAPTURE_READY 100
#define TSNX_NOTIFY_REQUEST_DONE 101  // id: request id, value: result
#define TSNX_NOTIFY_DEVICES_CHANGED 102
#define TSNX_NOTIFY_ENGINE_ERROR 103
#define TSNX_NOTIFY_OUTPUT_STATE 104  // value: 1 running, 0 stopped

typedef void (*tsnx_notify_fn)(int32_t kind, int32_t id, int64_t value);

typedef struct {
  int32_t manual_device;           // 1: no device; drive with manual_render
  int32_t manual_output_channels;  // 1 or 2
  int32_t manual_delay_ms;         // simulated device delay
  int32_t echo_cancellation;       // 1 on
  int32_t noise_suppression;
  int32_t auto_gain;
  int32_t platform_voice_processing;  // macOS: Apple voice processing
  int32_t clock_correction;  // 0 off, 1 observe, 2 control (fork port)
  int32_t linux_audio_backend;  // 0 auto, 1 PulseAudio, 2 ALSA
  const char* spill_dir;           // UTF-8; may be null (no spill)
  tsnx_notify_fn notify;
} tsnx_engine_config;

typedef struct {
  int64_t status;  // 0 idle, 1 playing, 2 paused, 3 starved, 4 ended, 5 failed
  int64_t position;       // heard position, source frames
  int64_t sampled_at_ns;  // tsnx_engine_now_ns clock
  double rate;
  int64_t written;
  int64_t duration;  // -1 before end of stream
  int64_t mixer_position;
  int64_t underruns;
} tsnx_track_state;

typedef struct {
  int64_t t_ns;
  int32_t frames;
  int32_t rate;
  int16_t data[480];
} tsnx_capture_block;

TSNX_EXPORT const char* tsnx_version(void);

TSNX_EXPORT int32_t tsnx_engine_open(const tsnx_engine_config* config,
                                     tsnx_engine** out);
TSNX_EXPORT void tsnx_engine_close(tsnx_engine* e);
TSNX_EXPORT int64_t tsnx_engine_now_ns(tsnx_engine* e);  // leaf
TSNX_EXPORT int32_t tsnx_engine_manual_render(tsnx_engine* e, int32_t blocks,
                                              int16_t* out,
                                              const int16_t* capture_in);
// Manual device: the output stops, stays silent for `gap_ms`, and starts
// again with an empty device buffer (tests of ADR I10).
TSNX_EXPORT int32_t tsnx_engine_manual_output_restart(tsnx_engine* e,
                                                      int32_t gap_ms);
TSNX_EXPORT double tsnx_engine_erle_db(tsnx_engine* e);
TSNX_EXPORT int64_t tsnx_engine_render_format_changes(tsnx_engine* e);
// Diagnostics: capture energy (sum of squares, 48 kHz mono) before and after
// the APM since the last call, and the device output delay.
TSNX_EXPORT int64_t tsnx_engine_take_capture_energy(tsnx_engine* e,
                                                    double* pre_apm,
                                                    double* post_apm);
TSNX_EXPORT int32_t tsnx_engine_output_delay_ms(tsnx_engine* e);
// Clock correction: returns the mode (0 off, 1 observe, 2 control, 3 legacy);
// `engaged` is 1 while the capture resampler is active.
TSNX_EXPORT int32_t tsnx_engine_clock_state(tsnx_engine* e, double* applied_ppm,
                                            int32_t* engaged);

TSNX_EXPORT int32_t tsnx_track_create(tsnx_engine* e, int32_t rate,
                                      int32_t channels, int32_t retention,
                                      int32_t* out_id);
TSNX_EXPORT int32_t tsnx_track_create_mp3(tsnx_engine* e, const uint8_t* data,
                                          int64_t size, int32_t* out_id);
TSNX_EXPORT int64_t tsnx_track_write(tsnx_engine* e, int32_t id,
                                     const int16_t* pcm,
                                     int64_t frames);  // leaf
TSNX_EXPORT int32_t tsnx_track_end_of_stream(tsnx_engine* e, int32_t id);
TSNX_EXPORT int32_t tsnx_track_play(tsnx_engine* e, int32_t id);   // leaf
TSNX_EXPORT int32_t tsnx_track_pause(tsnx_engine* e, int32_t id);  // leaf
TSNX_EXPORT int32_t tsnx_track_seek(tsnx_engine* e, int32_t id,
                                    int64_t frame);  // leaf
TSNX_EXPORT int32_t tsnx_track_set_rate(tsnx_engine* e, int32_t id,
                                        double rate);  // leaf
TSNX_EXPORT int32_t tsnx_track_set_gain(tsnx_engine* e, int32_t id,
                                        double gain,
                                        int32_t ramp_ms);  // leaf
TSNX_EXPORT int32_t tsnx_track_flush(tsnx_engine* e, int32_t id);  // leaf
TSNX_EXPORT int32_t tsnx_track_get_state(tsnx_engine* e, int32_t id,
                                     tsnx_track_state* out);  // leaf
TSNX_EXPORT int32_t tsnx_track_format(tsnx_engine* e, int32_t id,
                                      int32_t* rate, int32_t* channels);
TSNX_EXPORT int32_t tsnx_track_dispose(tsnx_engine* e, int32_t id);

TSNX_EXPORT int32_t tsnx_capture_start(tsnx_engine* e, int32_t rate,
                                       int32_t request_id);
TSNX_EXPORT int32_t tsnx_capture_stop(tsnx_engine* e, int32_t request_id);
TSNX_EXPORT int32_t tsnx_capture_read(tsnx_engine* e, tsnx_capture_block* out,
                                      int32_t max_blocks);  // leaf
TSNX_EXPORT int32_t tsnx_capture_dropped(tsnx_engine* e);

// Devices. kind: 0 output, 1 input. Strings are UTF-8, NUL-terminated,
// truncated to `cap` bytes.
TSNX_EXPORT int32_t tsnx_device_count(tsnx_engine* e, int32_t kind);
TSNX_EXPORT int32_t tsnx_device_get(tsnx_engine* e, int32_t kind,
                                    int32_t index, char* id, int32_t id_cap,
                                    char* name, int32_t name_cap);
TSNX_EXPORT int32_t tsnx_device_select(tsnx_engine* e, int32_t kind,
                                       const char* id, int32_t request_id);

#ifdef __cplusplus
}
#endif

#endif  // TSNX_AUDIO_H_
