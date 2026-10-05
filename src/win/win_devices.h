// Windows endpoint details for the engine's device lists (ADR D15).
#ifndef TSNX_WIN_WIN_DEVICES_H_
#define TSNX_WIN_WIN_DEVICES_H_

#include <string>

namespace tsnx {

// The endpoint ID (IMMDevice::GetId, UTF-8) of the Windows default device
// for the console role: "Default Device" in the Sound settings. The Core
// Audio ADM uses these IDs as device GUIDs. Empty if there is none.
std::string WinDefaultEndpointId(bool input);

}  // namespace tsnx

#endif  // TSNX_WIN_WIN_DEVICES_H_
