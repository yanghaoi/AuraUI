#include "core/paths.h"

#include <windows.h>
#include <shlobj.h>

namespace auraui::paths {
namespace {

std::wstring EnsureDir(std::wstring dir) {
    if (dir.empty()) return dir;
    if (dir.back() == L'\\') dir.pop_back();
    if (!::CreateDirectoryW(dir.c_str(), nullptr)) {
        const DWORD e = ::GetLastError();
        if (e != ERROR_ALREADY_EXISTS) {
            // Not fatal: callers tolerate a missing directory (config falls back to defaults).
        }
    }
    return dir;
}

}  // namespace

std::wstring ExePath() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(),
                                             static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

std::wstring ExeDir() {
    std::wstring p = ExePath();
    const size_t pos = p.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? std::wstring() : p.substr(0, pos);
}

std::wstring AppDataDir() {
    PWSTR roaming = nullptr;
    std::wstring base;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr,
                                         &roaming)) &&
        roaming) {
        base = roaming;
        ::CoTaskMemFree(roaming);
    }
    if (base.empty()) {
        wchar_t buf[MAX_PATH]{};
        const DWORD n = ::GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH) base = buf;
    }
    if (base.empty()) base = ExeDir();

    const std::wstring dir = EnsureDir(base + L"\\AuraUI");
    return dir;
}

std::wstring ConfigFile() { return AppDataDir() + L"\\config.ini"; }

std::wstring LogFile() { return AppDataDir() + L"\\AuraUI.log"; }

}  // namespace auraui::paths
