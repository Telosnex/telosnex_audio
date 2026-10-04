// Ported from the Telosnex libwebrtc fork: test/aec_guarantees/aec_guarantees.test.cc
// (servo and estimator guarantees; the self-echo gate and tap tests stay in the fork).
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "clock/audio_clock_correction.h"
#include "clock/drift_servo.h"
#include "clock/hardware_clock_estimator.h"
#include "gtest_compat.h"

namespace tsnx {
namespace {

constexpr uint32_t kRate = 48000;
constexpr size_t kBlock = kRate / 100;

std::vector<int16_t> SignalFrame(int frame_index, float gain = 1.0f) {
  std::vector<int16_t> out(kBlock);
  const double frame_gain =
      gain * (0.35 + 0.65 * (0.5 + 0.5 * std::sin(frame_index * 0.071)));
  const double f1 = 260.0 + 37.0 * (frame_index % 23);
  const double f2 = 900.0 + 83.0 * (frame_index % 41);
  for (size_t i = 0; i < out.size(); ++i) {
    const double t = (frame_index * kBlock + i) / static_cast<double>(kRate);
    const double sample = frame_gain * (0.55 * std::sin(2.0 * M_PI * f1 * t) +
                                        0.35 * std::sin(2.0 * M_PI * f2 * t));
    out[i] = static_cast<int16_t>(
        std::lround(std::clamp(sample, -0.95, 0.95) * 32767.0));
  }
  return out;
}

AudioHardwareClockObservation HardwareObservation(
    AudioHardwareClockDirection direction, int index, double normalized_rate,
    uint32_t generation = 1) {
  constexpr int64_t kBaseNs = 1000000000LL;
  constexpr int64_t kIntervalNs = 10000000LL;
  const int64_t jitter_ns =
      ((index *
        (direction == AudioHardwareClockDirection::kPlayout ? 37 : 53)) %
           201 -
       100) *
      1000;
  const int64_t time_ns = kBaseNs + index * kIntervalNs + jitter_ns;
  const double elapsed_seconds = (time_ns - kBaseNs) * 1e-9;
  const int64_t exact_position =
      std::llround(elapsed_seconds * kRate * normalized_rate);
  // Model the reference USB device's 1 ms hardware-position granularity.
  const int64_t quantized_position = std::llround(exact_position / 48.0) * 48;
  return {direction, time_ns, quantized_position, kRate, generation};
}

template <typename Sink>
void FeedHardwareClocks(Sink* sink, int first, int count,
                        double capture_relative_rate, uint32_t generation = 1) {
  for (int i = first; i < first + count; ++i) {
    sink->Add(HardwareObservation(AudioHardwareClockDirection::kPlayout, i, 1.0,
                                  generation));
    sink->Add(HardwareObservation(AudioHardwareClockDirection::kCapture, i,
                                  capture_relative_rate, generation));
  }
}

template <>
void FeedHardwareClocks<DriftServo>(DriftServo* servo, int first, int count,
                                    double capture_relative_rate,
                                    uint32_t generation) {
  for (int i = first; i < first + count; ++i) {
    servo->OnHardwareClockObservation(HardwareObservation(
        AudioHardwareClockDirection::kPlayout, i, 1.0, generation));
    servo->OnHardwareClockObservation(
        HardwareObservation(AudioHardwareClockDirection::kCapture, i,
                            capture_relative_rate, generation));
  }
}

std::string ReadTextFile(const std::string& path) {
  std::ifstream file(path);
  return std::string(std::istreambuf_iterator<char>(file),
                     std::istreambuf_iterator<char>());
}

TEST(HardwareClockEstimatorGuaranteeTest,
     QuantizedPositionsConvergeWithinSixSeconds) {
  HardwareClockEstimator estimator;
  std::optional<HardwareClockEstimator::Estimate> latest;
  for (int i = 0; i < 650; ++i) {
    estimator.Add(
        HardwareObservation(AudioHardwareClockDirection::kPlayout, i, 1.0));
    auto update = estimator.Add(HardwareObservation(
        AudioHardwareClockDirection::kCapture, i, 1.0 - 1700.0e-6));
    if (update.estimate) latest = update.estimate;
  }
  ASSERT_TRUE(latest.has_value());
  EXPECT_NEAR(latest->relative_ppm, -1700.0, 80.0);
  EXPECT_LT(latest->uncertainty_ppm, DriftServo::kHardwareMaxUncertaintyPpm);
  EXPECT_GE(latest->span_seconds, HardwareClockEstimator::kMinSpanSeconds);
}

TEST(HardwareClockEstimatorGuaranteeTest,
     CounterGenerationResetInvalidatesTheWindow) {
  HardwareClockEstimator estimator;
  std::optional<HardwareClockEstimator::Estimate> latest;
  for (int i = 0; i < 650; ++i) {
    estimator.Add(
        HardwareObservation(AudioHardwareClockDirection::kPlayout, i, 1.0));
    auto update = estimator.Add(HardwareObservation(
        AudioHardwareClockDirection::kCapture, i, 1.0 - 1700.0e-6));
    if (update.estimate) latest = update.estimate;
  }
  ASSERT_TRUE(latest.has_value());

  bool estimated_after_reset = false;
  for (int i = 650; i < 850; ++i) {
    auto playout =
        HardwareObservation(AudioHardwareClockDirection::kPlayout, i, 1.0, 2);
    auto capture = HardwareObservation(AudioHardwareClockDirection::kCapture, i,
                                       1.0 - 1700.0e-6, 2);
    // A new ALSA generation also establishes a new position origin.
    playout.position_frames -=
        HardwareObservation(AudioHardwareClockDirection::kPlayout, 650, 1.0, 2)
            .position_frames;
    capture.position_frames -=
        HardwareObservation(AudioHardwareClockDirection::kCapture, 650,
                            1.0 - 1700.0e-6, 2)
            .position_frames;
    if (estimator.Add(playout).estimate || estimator.Add(capture).estimate)
      estimated_after_reset = true;
  }
  EXPECT_FALSE(estimated_after_reset);
  EXPECT_GE(estimator.resets(), 2);
}

TEST(DriftServoGuaranteeTest, HardwareObserveModeNeverChangesCorrection) {
  DriftServo servo;
  servo.SetHardwareClockMode(DriftServo::HardwareClockMode::kObserve);
  FeedHardwareClocks(&servo, 0, 700, 1.0 - 1700.0e-6);
  const auto stats = servo.GetStats();
  EXPECT_TRUE(stats.hardware_ready);
  EXPECT_FALSE(stats.hardware_controlling);
  EXPECT_FALSE(stats.engaged);
  EXPECT_DOUBLE_EQ(stats.applied_ppm, 0.0);
}

TEST(DriftServoGuaranteeTest, HardwareControlEngagesWithoutSeedInSeconds) {
  DriftServo servo;
  servo.SetHardwareClockMode(DriftServo::HardwareClockMode::kControl);
  FeedHardwareClocks(&servo, 0, 700, 1.0 - 1700.0e-6);
  const auto stats = servo.GetStats();
  EXPECT_TRUE(stats.hardware_ready);
  EXPECT_TRUE(stats.hardware_controlling);
  EXPECT_TRUE(stats.engaged);
  EXPECT_NEAR(stats.hardware_measured_ppm, -1700.0, 80.0);
  EXPECT_NEAR(stats.applied_ppm, -1700.0, 100.0);
}

TEST(DriftServoGuaranteeTest, HardwareControlSupersedesStartupSeed) {
  DriftServo servo;
  servo.SetHardwareClockMode(DriftServo::HardwareClockMode::kControl);
  servo.SeedRatio(-1500.0);
  FeedHardwareClocks(&servo, 0, 700, 1.0 - 1700.0e-6);
  const auto stats = servo.GetStats();
  EXPECT_TRUE(stats.hardware_controlling);
  EXPECT_NEAR(stats.applied_ppm, -1700.0, 100.0);
}

TEST(DriftServoGuaranteeTest, InSpecHardwareClockNeverEngages) {
  DriftServo servo;
  servo.SetHardwareClockMode(DriftServo::HardwareClockMode::kControl);
  FeedHardwareClocks(&servo, 0, 700, 1.0 + 25.0e-6);
  const auto stats = servo.GetStats();
  EXPECT_TRUE(stats.hardware_ready);
  EXPECT_FALSE(stats.hardware_controlling);
  EXPECT_FALSE(stats.engaged);
  EXPECT_DOUBLE_EQ(stats.applied_ppm, 0.0);
}

TEST(DriftServoGuaranteeTest, InSpecClockNeverEngages) {
  DriftServo servo;
  std::vector<int16_t> zeros(kBlock, 0);
  for (int i = 0; i < 6000; ++i) {
    servo.PushCaptureAndCorrect(zeros.data(), kBlock, kRate, 1);
    servo.OnRenderFrames(kBlock, kRate);
  }
  const auto stats = servo.GetStats();
  EXPECT_FALSE(stats.engaged);
  EXPECT_NEAR(stats.measured_ppm, 0.0, 1.0);
  EXPECT_GE(stats.windows, 5);
}

TEST(DriftServoGuaranteeTest, LargeClockErrorEngagesWithoutSeed) {
  DriftServo servo;
  std::vector<int16_t> zeros(kBlock, 0);
  constexpr double kCapturePerRender = 1.0 - 1700.0e-6;
  double capture_credit = 0.0;
  for (int i = 0; i < 8000; ++i) {
    capture_credit += kCapturePerRender;
    while (capture_credit >= 1.0) {
      servo.PushCaptureAndCorrect(zeros.data(), kBlock, kRate, 1);
      capture_credit -= 1.0;
    }
    servo.OnRenderFrames(kBlock, kRate);
  }
  const auto stats = servo.GetStats();
  EXPECT_TRUE(stats.engaged);
  EXPECT_NEAR(stats.measured_ppm, -1700.0, 150.0);
  EXPECT_LE(std::abs(stats.applied_ppm), DriftServo::kMaxCorrectionPpm);
}

TEST(DriftServoGuaranteeTest,
     ValidSeedEngagesImmediatelyAndInvalidSeedDoesNot) {
  DriftServo valid;
  valid.SeedRatio(-1700.0);
  EXPECT_TRUE(valid.GetStats().engaged);
  EXPECT_NEAR(valid.GetStats().applied_ppm, -1700.0, 0.1);

  DriftServo too_small;
  too_small.SeedRatio(DriftServo::kEngagePpm / 2.0);
  EXPECT_FALSE(too_small.GetStats().engaged);

  DriftServo too_large;
  too_large.SeedRatio(DriftServo::kMaxCorrectionPpm + 1.0);
  EXPECT_FALSE(too_large.GetStats().engaged);
}

TEST(CaptureClockProfileTest, ControlPreservesRealServoAcrossSamePolicyStarts) {
  tsnx::AudioClockCorrection correction(true);
  using Mode = tsnx::AudioClockCorrectionMode;
  ASSERT_EQ(correction.Configure(Mode::kOff), 0);  // ignore test host env
  ASSERT_EQ(correction.Configure(Mode::kControl), 0);
  auto snapshot = correction.Read(true);
  ASSERT_NE(snapshot.servo, nullptr);
  FeedHardwareClocks(snapshot.servo.get(), 0, 700, 1.0 - 1700.0e-6);
  ASSERT_TRUE(correction.GetState().hardware_controlling);
  const auto before = snapshot.servo->GetStats();
  EXPECT_EQ(correction.Configure(Mode::kControl), 0);
  EXPECT_EQ(correction.Read(true).servo, snapshot.servo);
  EXPECT_EQ(snapshot.servo->GetStats().hardware_estimates, before.hardware_estimates);
  EXPECT_EQ(correction.Configure(Mode::kOff), -2);
  EXPECT_EQ(correction.Configure(Mode::kObserve), -2);
}

TEST(CaptureClockProfileTest, ObserveBypassesEvenEngagedCallbackFallback) {
  tsnx::AudioClockCorrection correction(true);
  using Mode = tsnx::AudioClockCorrectionMode;
  ASSERT_EQ(correction.Configure(Mode::kOff), 0);
  ASSERT_EQ(correction.Configure(Mode::kObserve), 0);
  auto snapshot = correction.Read(true);
  ASSERT_TRUE(snapshot.observe_only);
  auto input = SignalFrame(0);
  const auto original = input;
  // A callback estimate may engage its internal model, but explicit observe
  // must never feed corrected samples to APM or build an unconsumed FIFO.
  snapshot.servo->SeedRatio(-1700);
  for (int i = 0; i < 1000; ++i) {
    EXPECT_EQ(snapshot.servo->PushCaptureAndCorrect(
        input.data(), kBlock, kRate, 1, !snapshot.observe_only), 0u);
    snapshot.servo->OnRenderFrames(kBlock, kRate);
  }
  EXPECT_EQ(input, original);
  EXPECT_FALSE(correction.GetState().engaged);
  EXPECT_DOUBLE_EQ(correction.GetState().applied_ppm, 0);
  std::vector<int16_t> output(kBlock);
  EXPECT_FALSE(snapshot.servo->PopBlock(output.data(), kBlock));
}

}  // namespace
}  // namespace tsnx
