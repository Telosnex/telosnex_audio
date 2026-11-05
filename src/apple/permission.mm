// Microphone permission on iOS and macOS (src/permission.h).
#include "permission.h"

#import <AVFoundation/AVFoundation.h>

namespace tsnx::permission {

int32_t Status() {
  switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]) {
    case AVAuthorizationStatusAuthorized:
      return kGranted;
    case AVAuthorizationStatusNotDetermined:
      return kNotDetermined;
    default:  // denied or restricted
      return kDenied;
  }
}

void Request(std::function<void(int32_t)> done) {
  const int32_t status = Status();
  if (status != kNotDetermined) {
    done(status);
    return;
  }
  // The app's Info.plist needs NSMicrophoneUsageDescription: without it
  // the system ends the process here.
  [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                           completionHandler:^(BOOL granted) {
                             done(granted ? kGranted : kDenied);
                           }];
}

}  // namespace tsnx::permission
