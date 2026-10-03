#include "core/log.h"

#include "core/win_util.h"

#include <windows.h>

#include <mutex>

namespace auraui::log {
namespace {

constexpr unsigned long long kMaxLogBytes = 512ull * 1024ull;  // ~512 KB cap

std::mutex   g_mutex;
HANDLE       g_file = INVALID_HANDLE_VALUE;
std::wstring g_path;
bool         g_debug = false;
unsigned long long g_bytes = 0;  // bytes written since the file was opened

// Close, rename the current log to *.old (replacing any previous .old) and
// reopen fresh. Caller holds g_mutex.
void RotateLocked() {
    if (g_file != INVALID_HANDLE_VALUE) {
        ::CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    bool moved = false;
    if (!g_path.empty() &&
        ::GetFileAttributesW(g_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const std::wstring old = g_path + L".old";
        ::DeleteFileW(old.c_str());
        // Rename, not delete: the pre-incident tail is exactly what field
        // diagnosis needs (e.g. the SMBIOS cross-check line).
        moved = ::MoveFileW(g_path.c_str(), old.c_str()) != FALSE;
    }
    if (!moved && !g_path.empty()) {
        // The rename fails while another process holds the log open (a text
        // editor or log viewer keeps it open without FILE_SHARE_DELETE). Fall
        // back to truncating in place so the 512 KB cap still holds - and say
        // so via OutputDebugString at most once an hour: logging through
        // WriteLine here would deadlock on g_mutex (we are inside it) and
        // otherwise spam once per write past the cap.
        static unsigned long long lastWarn = 0;
        const unsigned long long now = ::GetTickCount64();
        if (now - lastWarn >= 3600000ull) {
            lastWarn = now;
            ::OutputDebugStringW(L"AuraUI: log rotation blocked (file in use by another process) - truncated instead\n");
        }
    }
    if (!g_path.empty()) {
        g_file = ::CreateFileW(g_path.c_str(), FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        // Truncate on the fallback path: OPEN_ALWAYS kept the old content,
        // and FILE_APPEND_DATA writes ignore the file pointer.
        if (!moved && g_file != INVALID_HANDLE_VALUE) ::SetEndOfFile(g_file);
    }
    g_bytes = 0;
}

unsigned long long CurrentFileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
        return 0;
    return (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
}

void WriteLine(const wchar_t* level, std::wstring_view msg) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file == INVALID_HANDLE_VALUE || g_path.empty()) return;

    SYSTEMTIME st{};
    ::GetLocalTime(&st);

    wchar_t stamp[64];
    ::swprintf(stamp, 64, L"%04u-%02u-%02u %02u:%02u:%02u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    std::wstring line;
    line.reserve(msg.size() + 48);
    line += L"[";
    line += stamp;
    line += L"] [";
    line += level;
    line += L"] ";
    line.append(msg.data(), msg.size());
    line += L"\r\n";

    const std::string utf8 = WideToUtf8(line);
    DWORD written = 0;
    ::WriteFile(g_file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    g_bytes += written;
    // Enforce the cap during the session as well: a long-running log must
    // not grow without bound just because the process never restarted.
    if (g_bytes > kMaxLogBytes) RotateLocked();
}

}  // namespace

void Init(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_path = path;

    if (CurrentFileSize(path) > kMaxLogBytes) {
        RotateLocked();
        return;
    }
    g_file = ::CreateFileW(path.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    g_bytes = CurrentFileSize(path);
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file != INVALID_HANDLE_VALUE) {
        ::CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
}

void SetDebugEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_debug = enabled;
}

void Debug(std::wstring_view msg) {
    if (!g_debug) return;
    WriteLine(L"DBG ", msg);
}
void Info(std::wstring_view msg) { WriteLine(L"INFO", msg); }
void Warn(std::wstring_view msg) { WriteLine(L"WARN", msg); }
void Error(std::wstring_view msg) { WriteLine(L"ERR ", msg); }

}  // namespace auraui::log
