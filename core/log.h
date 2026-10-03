#pragma once

// ---------------------------------------------------------------------------
// Minimal file logger. Writes UTF-8 lines to %APPDATA%\AuraUI\AuraUI.log.
//
// Info/Warn/Error are state changes worth keeping (start/stop, mount,
// rebuilds, config reloads). Debug carries the high-frequency diagnostic
// chatter (per-event tray callbacks, underlay crops) and is only written
// when enabled via --verbose. The file rotates to AuraUI.old.log at ~512 KB
// - during a long-running session too, so the previous tail survives for
// field diagnosis.
// ---------------------------------------------------------------------------

#include <string>
#include <string_view>

namespace auraui::log {

void Init(const std::wstring& path);
void Shutdown();

// Enable/disable Debug output (e.g. from --verbose). Thread-safe.
void SetDebugEnabled(bool enabled);

void Debug(std::wstring_view msg);
void Info(std::wstring_view msg);
void Warn(std::wstring_view msg);
void Error(std::wstring_view msg);

}  // namespace auraui::log
