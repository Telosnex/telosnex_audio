// Android route rules (src/android/android_routes.h), with device lists as
// AudioManager.getDevices() gives them on a phone.
#include "android/android_routes.h"

#include <string>
#include <vector>

#include "test.h"

using namespace tsnx::android_routes;

namespace {

std::vector<RawDevice> Phone() {
  return {
      {1, kEarpiece, true, "Pixel 8", ""},
      {2, kSpeaker, true, "Pixel 8", ""},
      {3, kBuiltinMic, false, "Pixel 8", "bottom"},
      {4, kBuiltinMic, false, "Pixel 8", "back"},
  };
}

// A headset with A2DP (media) and HFP (SCO, with its microphone).
std::vector<RawDevice> PhoneWithHeadset() {
  auto d = Phone();
  d.push_back({20, kBluetoothA2dp, true, "Pixel Buds", "AA:BB"});
  d.push_back({21, kBluetoothSco, true, "Pixel Buds", "AA:BB"});
  d.push_back({22, kBluetoothSco, false, "Pixel Buds", "AA:BB"});
  return d;
}

std::vector<RawDevice> PhoneWithWired() {
  auto d = Phone();
  d.push_back({30, kWiredHeadset, true, "", ""});
  d.push_back({31, kWiredHeadset, false, "", ""});
  return d;
}

std::string Ids(const std::vector<Route>& routes) {
  std::string s;
  for (const Route& r : routes) s += (s.empty() ? "" : ",") + r.id;
  return s;
}

const Route* ById(const std::vector<Route>& routes, const std::string& id) {
  for (const Route& r : routes)
    if (r.id == id) return &r;
  return nullptr;
}

}  // namespace

TEST(AndroidRoutesParse) {
  const auto d = ParseDevices(
      "2\t2\t1\tPixel 8\t\n"
      "22\t7\t0\tPixel Buds\tAA:BB\n"
      "bad line\n"
      "\n");
  CHECK_EQ(d.size(), 2);
  CHECK(d[0].id == 2 && d[0].type == kSpeaker && d[0].sink);
  CHECK(d[1].id == 22 && !d[1].sink && d[1].name == "Pixel Buds" &&
        d[1].address == "AA:BB");
}

TEST(AndroidRoutesListPhone) {
  const auto r = ListRoutes(Phone());
  CHECK(Ids(r.outputs) == "default,speaker,earpiece");
  CHECK(r.outputs[0].name == "Speaker");
  CHECK(Ids(r.inputs) == "default,microphone-bottom,microphone-back");
  CHECK(ById(r.inputs, "microphone-back")->name ==
        "Built-in microphone (back)");
}

TEST(AndroidRoutesListHeadsetAndWired) {
  auto r = ListRoutes(PhoneWithHeadset());
  CHECK(Ids(r.outputs) == "default,speaker,earpiece,bluetooth");
  CHECK(r.outputs[0].name == "Pixel Buds");  // media goes to A2DP
  CHECK(ById(r.outputs, "bluetooth")->name == "Pixel Buds");
  CHECK(ById(r.inputs, "bluetooth")->name == "Pixel Buds");

  r = ListRoutes(PhoneWithWired());
  // As in flutter_webrtc: a wired headset replaces the earpiece.
  CHECK(Ids(r.outputs) == "default,speaker,wired-headset");
  CHECK(r.outputs[0].name == "Wired headset");
  CHECK(r.inputs[0].name == "Wired headset microphone");
  CHECK(ById(r.inputs, "wired-headset") != nullptr);
}

TEST(AndroidRoutesMediaSelectionsStayOutOfCallMode) {
  const auto d = PhoneWithHeadset();
  Selection s;
  CHECK(MakePlan(s, d) == Plan{});
  s = SelectOutput(s, "speaker");
  Plan p = MakePlan(s, d);
  CHECK(!p.communication());
  CHECK_EQ(p.output_device, 2);
  s = SelectOutput(s, "bluetooth");
  p = MakePlan(s, d);
  CHECK(!p.communication());
  CHECK_EQ(p.output_device, 20);  // A2DP
  s = SelectInput(s, "microphone-back");
  p = MakePlan(s, d);
  CHECK(!p.communication());
  CHECK_EQ(p.input_device, 4);
}

TEST(AndroidRoutesEarpieceUsesCallMode) {
  Selection s = SelectOutput({}, "earpiece");
  Plan p = MakePlan(s, Phone());
  CHECK_EQ(p.communication_device, 1);
  CHECK_EQ(p.communication_type, kEarpiece);
  CHECK_EQ(p.output_device, 1);
  // With a wired headset there is no earpiece; the selection acts as
  // default until the headset goes.
  p = MakePlan(s, PhoneWithWired());
  CHECK(p == Plan{});
}

TEST(AndroidRoutesBluetoothMicMovesOutputToTheHeadset) {
  const auto d = PhoneWithHeadset();
  Selection s = SelectOutput({}, "speaker");
  s = SelectInput(s, "bluetooth");
  CHECK(s.output == "bluetooth");
  Plan p = MakePlan(s, d);
  CHECK_EQ(p.communication_device, 21);  // SCO, not A2DP
  CHECK_EQ(p.communication_type, kBluetoothSco);
  CHECK_EQ(p.output_device, 21);
  CHECK_EQ(p.input_device, 22);

  // The default output keeps the headset microphone.
  CHECK(SelectOutput(s, "default").input == "bluetooth");
  // Another output: the last selection wins.
  s = SelectOutput(s, "earpiece");
  CHECK(s.input == "default");
  p = MakePlan(s, d);
  CHECK_EQ(p.communication_device, 1);
  CHECK_EQ(p.input_device, 0);
  // Back to the speaker: out of call mode.
  s = SelectOutput(s, "speaker");
  CHECK(!MakePlan(s, d).communication());
}

TEST(AndroidRoutesMissingDeviceActsAsDefault) {
  Selection s = SelectInput({}, "bluetooth");
  // The headset turned off: no call mode, Android chooses.
  CHECK(MakePlan(s, Phone()) == Plan{});
  // It comes back: the selection holds.
  CHECK(MakePlan(s, PhoneWithHeadset()).communication());

  // A headset with HFP only plays only in a call.
  auto d = Phone();
  d.push_back({21, kBluetoothSco, true, "Car", ""});
  Plan p = MakePlan(SelectOutput({}, "bluetooth"), d);
  CHECK_EQ(p.communication_device, 21);
  CHECK(ById(ListRoutes(d).outputs, "bluetooth")->name == "Car");
}

TEST(AndroidRoutesDebugSpeakerCall) {
  CHECK(ById(ListRoutes(Phone()).outputs, "debug-speaker-call") == nullptr);
  const auto r = ListRoutes(Phone(), /*debug_routes=*/true);
  CHECK(ById(r.outputs, "debug-speaker-call") != nullptr);
  const Plan p = MakePlan(SelectOutput({}, "debug-speaker-call"), Phone());
  CHECK_EQ(p.communication_device, 2);
  CHECK_EQ(p.output_device, 2);
}
