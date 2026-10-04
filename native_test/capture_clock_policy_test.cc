// Ported from the Telosnex libwebrtc fork: test/capture_clock_policy_test.cc
// Host-only lifetime/concurrency regression. No audio devices or WebRTC build
// required: c++ -std=c++17 -pthread -I. test/capture_clock_policy_test.cc -o /tmp/clock-test
#include "clock/capture_clock_policy.h"

#include <atomic>
#include <memory>

#include "test.h"
#include <thread>
#include <vector>

namespace {
struct Servo {
  explicit Servo(int generation) : generation(generation) {}
  int generation;
};
}  // namespace

TEST(CaptureClockPolicyLifetimeAndRaces) {
  using Policy = tsnx::CaptureClockPolicy<Servo>;
  Policy policy(0, nullptr);
  CHECK(!policy.Read().servo);
  int allocations = 0;
  auto create = [&] { return std::make_shared<Servo>(++allocations); };
  CHECK(policy.Configure(2, create) == 0);
  auto before_capture = policy.Read();
  CHECK(policy.Configure(2, create) == 0);
  CHECK(allocations == 1);  // repeat control cannot reset estimates/FIFOs
  CHECK(policy.Read(true).servo == before_capture.servo);
  CHECK(policy.Configure(0, create) == -2);
  CHECK(policy.Configure(1, create) == -2);
  CHECK(policy.Configure(2, create) == 0);
  CHECK(allocations == 1);
  // A peer detaching or capture demand cycling has no reset method. The
  // shared policy remains frozen; reattaching the same mode is idempotent.
  CHECK(policy.Read(true).servo == before_capture.servo);

  Policy fallback(0, nullptr);
  fallback.Read(true);  // RTP/direct getUserMedia started before the profile
  CHECK(fallback.Configure(2, create) == -2);
  CHECK(!fallback.Read().servo);

  Policy observe(1, create());  // legacy env observe includes callback fallback
  CHECK(!observe.Read().observe_only);
  auto old = observe.Read().servo;
  CHECK(observe.Configure(1, create) == 0);
  CHECK(observe.Read().observe_only);
  CHECK(observe.Read().servo != old);
  CHECK(old->generation > 0);  // in-flight render observers retain ownership
  observe.Read(true);
  CHECK(observe.Configure(1, create) == 0);

  Policy invalid(0, nullptr);
  CHECK(invalid.Configure(4, create) == -1);
  CHECK(!invalid.Read().servo);

  // Configure/capture/observer races never replace a frozen instance or expose
  // an expired servo. Repeat with genuinely competing starts, not mocked locks.
  for (int iteration = 0; iteration < 100; ++iteration) {
    Policy raced(0, nullptr);
    std::atomic<bool> go{false};
    std::shared_ptr<Servo> captured;
    int status = -3;
    std::thread capture([&] {
      while (!go.load()) {}
      captured = raced.Read(true).servo;
    });
    std::thread configure([&] {
      while (!go.load()) {}
      status = raced.Configure(2, [] { return std::make_shared<Servo>(42); });
    });
    std::thread observer([&] {
      while (!go.load()) {}
      for (int i = 0; i < 100; ++i) {
        auto snapshot = raced.Read();
        if (snapshot.servo) CHECK(snapshot.servo->generation == 42);
      }
    });
    go.store(true);
    capture.join();
    configure.join();
    observer.join();
    CHECK(status == 0 || status == -2);
    CHECK(raced.Read().servo == captured);
    CHECK(raced.Read().capture_started);
  }
}
