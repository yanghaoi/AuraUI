#pragma once

#include <string>

namespace auraui::paths {

// Full path of the running executable (e.g. C:\path\to\AuraUI.exe)
std::wstring ExePath();
std::wstring ExeDir();

// %APPDATA%\AuraUI  (created on first use)
std::wstring AppDataDir();

// %APPDATA%\AuraUI\config.ini
std::wstring ConfigFile();

// %APPDATA%\AuraUI\AuraUI.log
std::wstring LogFile();

}  // namespace auraui::paths
