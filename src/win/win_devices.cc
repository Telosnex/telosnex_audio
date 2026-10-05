#include "win/win_devices.h"

#include <windows.h>
#include <mmdeviceapi.h>

namespace tsnx {

std::string WinDefaultEndpointId(bool input) {
  // The device thread already uses COM (the ADM's MTA); this only adds a
  // reference to it.
  const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  std::string result;
  IMMDeviceEnumerator* enumerator = nullptr;
  if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                 CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                 reinterpret_cast<void**>(&enumerator)))) {
    IMMDevice* device = nullptr;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(
            input ? eCapture : eRender, eConsole, &device))) {
      LPWSTR id = nullptr;
      if (SUCCEEDED(device->GetId(&id))) {
        const int n =
            WideCharToMultiByte(CP_UTF8, 0, id, -1, nullptr, 0, nullptr, nullptr);
        if (n > 1) {
          result.resize(n - 1);
          WideCharToMultiByte(CP_UTF8, 0, id, -1, result.data(), n, nullptr,
                              nullptr);
        }
        CoTaskMemFree(id);
      }
      device->Release();
    }
    enumerator->Release();
  }
  if (SUCCEEDED(init)) CoUninitialize();
  return result;
}

}  // namespace tsnx
