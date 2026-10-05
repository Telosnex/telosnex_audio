#include "android/android_routes.h"

#include <cstdlib>
#include <initializer_list>

namespace tsnx::android_routes {
namespace {

bool IsOneOf(int type, std::initializer_list<int> types) {
  for (int t : types)
    if (t == type) return true;
  return false;
}

// The first device of the first type in `types` (the order is the
// preference), in one direction.
const RawDevice* Find(const std::vector<RawDevice>& devices, bool sink,
                      std::initializer_list<int> types) {
  for (int t : types)
    for (const RawDevice& d : devices)
      if (d.sink == sink && d.type == t) return &d;
  return nullptr;
}

const std::initializer_list<int> kWiredOut = {kWiredHeadset, kWiredHeadphones,
                                              kUsbHeadset};
const std::initializer_list<int> kUsbOut = {kUsbDevice, kUsbAccessory};
// Bluetooth for media, without communication mode.
const std::initializer_list<int> kBluetoothMediaOut = {
    kBluetoothA2dp, kBleHeadset, kBleSpeaker, kHearingAid};
// Bluetooth communication devices: the headset in a call.
const std::initializer_list<int> kBluetoothCallOut = {kBluetoothSco,
                                                      kBleHeadset};
const std::initializer_list<int> kBluetoothIn = {kBluetoothSco, kBleHeadset};
const std::initializer_list<int> kUsbIn = {kUsbHeadset, kUsbDevice,
                                           kUsbAccessory};

std::string BuiltinMicId(const RawDevice& d) {
  return d.address.empty() ? "microphone" : "microphone-" + d.address;
}

std::string NameOr(const RawDevice& d, const char* fallback) {
  return d.name.empty() ? fallback : d.name;
}

std::string OutputName(const RawDevice& d) {
  switch (d.type) {
    case kSpeaker:
      return "Speaker";
    case kEarpiece:
      return "Earpiece";
    case kWiredHeadset:
    case kWiredHeadphones:
      return "Wired headset";
    case kUsbDevice:
    case kUsbAccessory:
    case kUsbHeadset:
      return NameOr(d, "USB audio");
    default:
      return NameOr(d, "Bluetooth");
  }
}

DeviceKind KindOf(int type) {
  switch (type) {
    case kSpeaker:
      return DeviceKind::kSpeaker;
    case kEarpiece:
      return DeviceKind::kEarpiece;
    case kWiredHeadset:
    case kWiredHeadphones:
      return DeviceKind::kWired;
    case kUsbDevice:
    case kUsbAccessory:
    case kUsbHeadset:
      return DeviceKind::kUsb;
    case kBluetoothSco:
    case kBluetoothA2dp:
    case kHearingAid:
    case kBleHeadset:
    case kBleSpeaker:
      return DeviceKind::kBluetooth;
    case kBuiltinMic:
      return DeviceKind::kMicrophone;
    default:
      return DeviceKind::kOther;
  }
}

const RawDevice* ById(const std::vector<RawDevice>& devices, bool sink,
                      int id) {
  if (id == 0) return nullptr;
  for (const RawDevice& d : devices)
    if (d.sink == sink && d.id == id) return &d;
  return nullptr;
}

std::string InputName(const RawDevice& d) {
  switch (d.type) {
    case kBuiltinMic:
      return d.address.empty() ? "Built-in microphone"
                               : "Built-in microphone (" + d.address + ")";
    case kWiredHeadset:
      return "Wired headset microphone";
    default:
      return NameOr(d, "Microphone");
  }
}

// Android's media output when nothing is selected.
const RawDevice* MediaDefaultOut(const std::vector<RawDevice>& devices) {
  if (auto* d = Find(devices, true, kBluetoothMediaOut)) return d;
  if (auto* d = Find(devices, true, kWiredOut)) return d;
  if (auto* d = Find(devices, true, kUsbOut)) return d;
  return Find(devices, true, {kSpeaker});
}

// Android's microphone for VOICE_RECOGNITION when nothing is selected.
const RawDevice* DefaultIn(const std::vector<RawDevice>& devices) {
  if (auto* d = Find(devices, false, {kWiredHeadset})) return d;
  if (auto* d = Find(devices, false, kUsbIn)) return d;
  return Find(devices, false, {kBuiltinMic});
}

const RawDevice* FindInput(const std::vector<RawDevice>& devices,
                           const std::string& id) {
  if (id == "wired-headset") return Find(devices, false, {kWiredHeadset});
  if (id == "usb") return Find(devices, false, kUsbIn);
  if (id == "bluetooth") return Find(devices, false, kBluetoothIn);
  for (const RawDevice& d : devices) {
    if (d.sink || d.type != kBuiltinMic) continue;
    if (BuiltinMicId(d) == id || "microphone-" + std::to_string(d.id) == id)
      return &d;
  }
  return nullptr;
}

std::string Field(const std::string& line, size_t* pos) {
  const size_t tab = line.find('\t', *pos);
  const size_t end = tab == std::string::npos ? line.size() : tab;
  std::string f = line.substr(*pos, end - *pos);
  *pos = tab == std::string::npos ? line.size() : tab + 1;
  return f;
}

}  // namespace

std::vector<RawDevice> ParseDevices(const std::string& text) {
  std::vector<RawDevice> out;
  size_t start = 0;
  while (start < text.size()) {
    size_t nl = text.find('\n', start);
    if (nl == std::string::npos) nl = text.size();
    const std::string line = text.substr(start, nl - start);
    start = nl + 1;
    if (line.empty()) continue;
    size_t pos = 0;
    RawDevice d;
    d.id = std::atoi(Field(line, &pos).c_str());
    d.type = std::atoi(Field(line, &pos).c_str());
    d.sink = Field(line, &pos) == "1";
    d.name = Field(line, &pos);
    d.address = Field(line, &pos);
    if (d.id > 0) out.push_back(std::move(d));
  }
  return out;
}

RouteLists ListRoutes(const std::vector<RawDevice>& devices,
                      bool debug_routes) {
  RouteLists r;
  const RawDevice* out_default = MediaDefaultOut(devices);
  r.outputs.push_back(
      {"default", out_default ? OutputName(*out_default) : "System default"});
  if (auto* d = Find(devices, true, {kSpeaker}))
    r.outputs.push_back({"speaker", OutputName(*d)});
  const RawDevice* wired = Find(devices, true, kWiredOut);
  // As in flutter_webrtc: a wired headset replaces the earpiece.
  if (!wired) {
    if (auto* d = Find(devices, true, {kEarpiece}))
      r.outputs.push_back({"earpiece", OutputName(*d)});
  }
  if (wired) r.outputs.push_back({"wired-headset", OutputName(*wired)});
  if (auto* d = Find(devices, true, kUsbOut))
    r.outputs.push_back({"usb", OutputName(*d)});
  if (auto* d = Find(devices, true, kBluetoothMediaOut))
    r.outputs.push_back({"bluetooth", OutputName(*d)});
  else if (auto* d = Find(devices, true, kBluetoothCallOut))
    r.outputs.push_back({"bluetooth", OutputName(*d)});
  if (debug_routes && Find(devices, true, {kSpeaker}))
    r.outputs.push_back({"debug-speaker-call", "Speaker (call mode, debug)"});

  const RawDevice* in_default = DefaultIn(devices);
  r.inputs.push_back(
      {"default", in_default ? InputName(*in_default) : "System default"});
  for (const RawDevice& d : devices) {
    if (d.sink || d.type != kBuiltinMic) continue;
    std::string id = BuiltinMicId(d);
    for (const Route& seen : r.inputs)
      if (seen.id == id) id = "microphone-" + std::to_string(d.id);
    r.inputs.push_back({id, InputName(d)});
  }
  if (auto* d = Find(devices, false, {kWiredHeadset}))
    r.inputs.push_back({"wired-headset", InputName(*d)});
  if (auto* d = Find(devices, false, kUsbIn))
    r.inputs.push_back({"usb", InputName(*d)});
  if (auto* d = Find(devices, false, kBluetoothIn))
    r.inputs.push_back({"bluetooth", InputName(*d)});
  return r;
}

Selection SelectOutput(Selection selection, const std::string& id) {
  selection.output = id;
  // The headset microphone needs the headset as the communication device.
  if (selection.input == "bluetooth" && id != "bluetooth" && id != "default")
    selection.input = "default";
  return selection;
}

Selection SelectInput(Selection selection, const std::string& id) {
  selection.input = id;
  if (id == "bluetooth") selection.output = "bluetooth";
  return selection;
}

Plan MakePlan(const Selection& selection,
              const std::vector<RawDevice>& devices) {
  Plan p;
  const RawDevice* in = selection.input == "default"
                            ? nullptr
                            : FindInput(devices, selection.input);
  if (in && IsOneOf(in->type, kBluetoothIn)) {
    // The headset in a call: its communication device has the same type.
    const RawDevice* call = Find(devices, true, {in->type});
    if (!call) call = Find(devices, true, kBluetoothCallOut);
    if (call) {
      p.communication_device = call->id;
      p.communication_type = call->type;
      p.output_device = call->id;
      p.input_device = in->id;
      return p;
    }
    in = nullptr;  // no way to open the headset microphone
  }
  if (in) p.input_device = in->id;

  const std::string& out = selection.output;
  if (out == "speaker") {
    if (auto* d = Find(devices, true, {kSpeaker})) p.output_device = d->id;
  } else if (out == "debug-speaker-call") {
    if (auto* d = Find(devices, true, {kSpeaker})) {
      p.communication_device = d->id;
      p.communication_type = d->type;
      p.output_device = d->id;
    }
  } else if (out == "earpiece") {
    if (!Find(devices, true, kWiredOut)) {
      if (auto* d = Find(devices, true, {kEarpiece})) {
        p.communication_device = d->id;
        p.communication_type = d->type;
        p.output_device = d->id;
      }
    }
  } else if (out == "wired-headset") {
    if (auto* d = Find(devices, true, kWiredOut)) p.output_device = d->id;
  } else if (out == "usb") {
    if (auto* d = Find(devices, true, kUsbOut)) p.output_device = d->id;
  } else if (out == "bluetooth") {
    if (auto* d = Find(devices, true, kBluetoothMediaOut)) {
      p.output_device = d->id;
    } else if (auto* d = Find(devices, true, kBluetoothCallOut)) {
      // A headset without media audio (HFP only) plays only in a call.
      p.communication_device = d->id;
      p.communication_type = d->type;
      p.output_device = d->id;
    }
  }
  return p;
}

DeviceInfo CurrentOutput(const Selection& selection,
                         const std::vector<RawDevice>& devices) {
  const Plan p = MakePlan(selection, devices);
  if (auto* d = ById(devices, true, p.output_device))
    return {selection.output, OutputName(*d), KindOf(d->type)};
  if (auto* d = MediaDefaultOut(devices))
    return {"default", OutputName(*d), KindOf(d->type)};
  return {"default", "System default", DeviceKind::kOther};
}

DeviceInfo CurrentInput(const Selection& selection,
                        const std::vector<RawDevice>& devices) {
  const Plan p = MakePlan(selection, devices);
  if (auto* d = ById(devices, false, p.input_device))
    return {selection.input, InputName(*d), KindOf(d->type)};
  if (auto* d = DefaultIn(devices))
    return {"default", InputName(*d), KindOf(d->type)};
  return {"default", "System default", DeviceKind::kOther};
}

}  // namespace tsnx::android_routes
