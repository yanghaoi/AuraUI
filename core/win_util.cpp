#include "core/win_util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>

namespace auraui {

std::wstring FormatWinError(DWORD err) {
    if (err == 0) return L"(no error)";

    LPWSTR buf = nullptr;
    const DWORD n = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buf), 0, nullptr);

    std::wstring out;
    if (n && buf) {
        out.assign(buf, n);
        while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n' || out.back() == L' '))
            out.pop_back();
    }
    if (buf) ::LocalFree(buf);

    wchar_t code[32];
    ::swprintf(code, 32, L" [0x%08lX]", static_cast<unsigned long>(err));
    return out.empty() ? std::wstring(code + 1) : out + code;
}

std::wstring LastErrorString() { return FormatWinError(::GetLastError()); }

std::wstring HresultString(HRESULT hr) {
    return FormatWinError(static_cast<DWORD>(hr));
}

std::wstring Utf8ToWide(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                        static_cast<int>(utf8.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                          out.data(), n);
    return out;
}

std::string WideToUtf8(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                        static_cast<int>(wide.size()), nullptr, 0,
                                        nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                          out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Trim(std::wstring s) {
    auto notSpace = [](wchar_t c) { return !iswspace(static_cast<wint_t>(c)); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s;
}

std::wstring FmtInt(long long v) {
    wchar_t b[32];
    ::swprintf(b, 32, L"%lld", v);
    return b;
}

std::wstring FmtFloat(double v, int decimals) {
    if (decimals < 0) decimals = 0;
    if (decimals > 6) decimals = 6;
    wchar_t fmt[8];
    ::swprintf(fmt, 8, L"%%.%df", decimals);
    wchar_t b[64];
    ::swprintf(b, 64, fmt, v);
    return b;
}

std::wstring FmtPercent(double v) {
    if (v < 0) v = 0;
    if (v > 999) v = 999;
    wchar_t b[32];
    ::swprintf(b, 32, L"%d%%", static_cast<int>(std::lround(v)));
    return b;
}

std::wstring FmtBytes(unsigned long long b) {
    static const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB", L"PB"};
    double v = static_cast<double>(b);
    int i = 0;
    while (v >= 1024.0 && i < 5) {
        v /= 1024.0;
        ++i;
    }
    return FmtFloat(v, i == 0 ? 0 : 1) + L" " + units[i];
}

std::wstring FmtSpeed(double bytesPerSec) {
    if (bytesPerSec < 0) bytesPerSec = 0;
    return FmtBytes(static_cast<unsigned long long>(bytesPerSec)) + L"/s";
}

}  // namespace auraui
