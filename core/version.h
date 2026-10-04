#pragma once
// ---------------------------------------------------------------------------
// Single source of truth for the product version.
//
// Consumed by:
//   * assets/app.rc  (PE VERSIONINFO - included via windres, RC_INVOKED)
//   * app/app.cpp    (startup log line)
//   * settings       (corner label)
//   * tray           (startup balloon title)
//
// Bump here and rebuild - everything else follows. Keep AURAUI_VER_STRING in
// sync with the four numbers (stringifying in windres-preprocessed .rc is not
// worth the macro machinery).
// ---------------------------------------------------------------------------

#define AURAUI_VER_MAJOR 0
#define AURAUI_VER_MINOR 2
#define AURAUI_VER_PATCH 3
#define AURAUI_VER_BUILD 0

// "major.minor.patch.build" for the VERSIONINFO resource.
#define AURAUI_VER_STRING "0.2.3.0"

#ifndef RC_INVOKED
#include <string>

namespace auraui {

// "major.minor.patch" (wide), composed from the numbers so it cannot drift.
inline std::wstring VersionW() {
    return std::to_wstring(AURAUI_VER_MAJOR) + L"." +
           std::to_wstring(AURAUI_VER_MINOR) + L"." +
           std::to_wstring(AURAUI_VER_PATCH);
}

}  // namespace auraui
#endif  // RC_INVOKED
