#include "config/config.h"

#include "core/log.h"
#include "core/paths.h"
#include "core/win_util.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace auraui {
namespace {

constexpr const wchar_t* kRunKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kRunValue = L"AuraUI";

std::wstring ReadStr(const std::wstring& file, const wchar_t* section,
                     const wchar_t* key, const std::wstring& def) {
    std::vector<wchar_t> buf(1024);
    for (;;) {
        const DWORD n = ::GetPrivateProfileStringW(section, key, def.c_str(),
                                                   buf.data(),
                                                   static_cast<DWORD>(buf.size()),
                                                   file.c_str());
        // The Unicode profile API never returns more than nSize-2 characters
        // (it truncates there and writes a double NUL): hitting exactly that
        // bound means the value may be longer - grow and retry. (The old
        // `n < buf.size() - 1` test was always true, making the growth dead
        // code and long values silently truncated.)
        if (n < buf.size() - 2) return std::wstring(buf.data(), n);
        buf.resize(buf.size() * 2);
    }
}

int ReadInt(const std::wstring& file, const wchar_t* section, const wchar_t* key,
            int def) {
    return static_cast<int>(
        ::GetPrivateProfileIntW(section, key, def, file.c_str()));
}

std::wstring ReadHexOrDec(const std::wstring& file, const wchar_t* section,
                          const wchar_t* key, unsigned def) {
    wchar_t tmp[32];
    ::swprintf(tmp, 32, L"%06X", def);
    return ReadStr(file, section, key, tmp);
}

unsigned ParseColor(const std::wstring& s, unsigned def) {
    const std::wstring t = Trim(s);
    if (t.empty()) return def;
    if (t.size() > 2 && t[0] == L'0' && (t[1] == L'x' || t[1] == L'X')) {
        return static_cast<unsigned>(wcstoul(t.c_str() + 2, nullptr, 16)) & 0xFFFFFFu;
    }
    // Bare 6-digit hex is the documented format; anything else is treated as decimal.
    if (t.size() == 6 && t.find_first_not_of(L"0123456789abcdefABCDEF") == std::wstring::npos)
        return static_cast<unsigned>(wcstoul(t.c_str(), nullptr, 16)) & 0xFFFFFFu;
    return static_cast<unsigned>(wcstoul(t.c_str(), nullptr, 10)) & 0xFFFFFFu;
}

void WriteColorLine(std::wstring& s, const wchar_t* key, unsigned rgb) {
    wchar_t tmp[16];
    ::swprintf(tmp, 16, L"%06X", rgb & 0xFFFFFFu);
    s += key;
    s += L'=';
    s += tmp;
    s += L"\r\n";
}

// The INI grammar has no escaping: embedded line breaks or NULs would
// corrupt the file structure (the profile-API writer had the same constraint).
std::wstring SanitizeIniValue(std::wstring v) {
    for (wchar_t& c : v)
        if (c == L'\r' || c == L'\n' || c == L'\0') c = L' ';
    return v;
}

// The file stays in the system code page. Measured on Win10 19045: the W
// profile APIs do NOT honor a UTF-16LE BOM (they read raw ANSI bytes and miss
// every section), so the encoding is a hard constraint, not a preference.
// Consequence handled in the settings UI: a font name not representable in
// CP_ACP would be saved as '??' and silently fall back after a restart - such
// fonts are filtered out of the font combo (EncodableInAcp there).
std::string WideToAcp(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_ACP, 0, wide.data(),
                                        static_cast<int>(wide.size()), nullptr, 0,
                                        nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_ACP, 0, wide.data(), static_cast<int>(wide.size()),
                          out.data(), n, nullptr, nullptr);
    return out;
}

}  // namespace

void Config::Sanitize() {
    if (refreshMs < 100) refreshMs = 100;
    if (refreshMs > 60000) refreshMs = 60000;

    if (monitorIndex < 0) monitorIndex = 0;

    if (width < 120) width = 120;
    if (width > 4000) width = 4000;
    if (height < 0) height = 0;
    if (height > 4000) height = 4000;

    if (opacity < 0) opacity = 0;
    if (opacity > 255) opacity = 255;

    if (fontSize < 6.0f) fontSize = 6.0f;
    if (fontSize > 96.0f) fontSize = 96.0f;

    if (fontFamily.empty()) fontFamily = L"Segoe UI";

    // 32 is a design bound, not an arbitrary cap: the corner arc must stay
    // clear of the text column (padding-left 14px). Measured on an r=64
    // render, the arc reaches x≈17.5 at the title row while glyphs start at
    // x=14 - text pokes outside the panel fill. The corner cut-outs also
    // expose the underlay crop directly against the real desktop; their area
    // grows quadratically with r and past ~32 the approximation seam becomes
    // visible ("translucent corner" artifacts).
    if (cornerRadius < 0) cornerRadius = 0;
    if (cornerRadius > 32) cornerRadius = 32;

    if (padding < 0) padding = 0;
    if (padding > 64) padding = 64;

    bgColor &= 0xFFFFFFu;
    accentColor &= 0xFFFFFFu;

    // Clamp position into a generous virtual-desktop envelope.
    if (x < -32768) x = -32768;
    if (x > 32768) x = 32768;
    if (y < -32768) y = -32768;
    if (y > 32768) y = 32768;
}

Config Config::Load(const std::wstring& path) {
    Config c;
    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        c.Sanitize();
        return c;
    }

    c.autoStart  = ReadInt(path, L"General", L"AutoStart", c.autoStart ? 1 : 0) != 0;
    c.refreshMs  = ReadInt(path, L"General", L"RefreshInterval", c.refreshMs);
    c.paused     = ReadInt(path, L"General", L"Paused", c.paused ? 1 : 0) != 0;

    c.monitorIndex = ReadInt(path, L"Display", L"Monitor", c.monitorIndex);
    c.x            = ReadInt(path, L"Display", L"X", c.x);
    c.y            = ReadInt(path, L"Display", L"Y", c.y);
    c.width        = ReadInt(path, L"Display", L"Width", c.width);
    c.height       = ReadInt(path, L"Display", L"Height", c.height);
    c.opacity      = ReadInt(path, L"Display", L"Opacity", c.opacity);
    c.fontFamily   = ReadStr(path, L"Display", L"Font", c.fontFamily);
    c.fontSize     = static_cast<float>(ReadInt(path, L"Display", L"FontSize",
                                                static_cast<int>(c.fontSize)));

    c.showCpu     = ReadInt(path, L"Monitor", L"CPU", c.showCpu ? 1 : 0) != 0;
    c.showMemory  = ReadInt(path, L"Monitor", L"Memory", c.showMemory ? 1 : 0) != 0;
    c.showGpu     = ReadInt(path, L"Monitor", L"GPU", c.showGpu ? 1 : 0) != 0;
    c.showVram    = ReadInt(path, L"Monitor", L"VRAM", c.showVram ? 1 : 0) != 0;
    c.showDisk    = ReadInt(path, L"Monitor", L"Disk", c.showDisk ? 1 : 0) != 0;
    c.showNetwork = ReadInt(path, L"Monitor", L"Network", c.showNetwork ? 1 : 0) != 0;
    c.showLanIp   = ReadInt(path, L"Monitor", L"LANIP", c.showLanIp ? 1 : 0) != 0;

    c.showTitle = ReadInt(path, L"Appearance", L"ShowTitle", c.showTitle ? 1 : 0) != 0;
    c.showBars  = ReadInt(path, L"Appearance", L"ShowBars", c.showBars ? 1 : 0) != 0;
    c.cornerRadius = ReadInt(path, L"Appearance", L"CornerRadius", c.cornerRadius);
    c.padding      = ReadInt(path, L"Appearance", L"Padding", c.padding);
    c.bgColor      = ParseColor(ReadHexOrDec(path, L"Appearance", L"Background", c.bgColor),
                                c.bgColor);
    c.accentColor  = ParseColor(ReadHexOrDec(path, L"Appearance", L"Accent", c.accentColor),
                                c.accentColor);

    c.hudVisible = ReadInt(path, L"State", L"HudVisible", c.hudVisible ? 1 : 0) != 0;

    c.Sanitize();
    return c;
}

bool Config::Save(const std::wstring& path) const {
    // Single-pass serialization. The previous implementation issued ~25
    // WritePrivateProfileStringW calls (each a full read-modify-write of the
    // file) plus a CREATE_ALWAYS header rewrite - whose truncation window
    // could lose the whole config on a crash mid-save, and whose
    // prefix-matching header check kept re-inserting the comment block on
    // files migrated from the legacy product. One composed buffer, written
    // to a temp file and moved into place: every save rewrites exactly one
    // header, and a crash can no longer leave a half-written config.
    auto kv = [](std::wstring& s, const wchar_t* key, const std::wstring& value) {
        s += key;
        s += L'=';
        s += value;
        s += L"\r\n";
    };
    auto kvInt = [&kv](std::wstring& s, const wchar_t* key, int value) {
        kv(s, key, FmtInt(value));
    };
    auto kvBool = [&kv](std::wstring& s, const wchar_t* key, bool value) {
        kv(s, key, value ? L"1" : L"0");
    };
    auto section = [](std::wstring& s, const wchar_t* name) {
        s += L"\r\n[";
        s += name;
        s += L"]\r\n";
    };

    std::wstring s;
    s.reserve(1024);

    s += L"; AuraUI configuration\r\n";
    s += L"; Rewritten by the settings window on every apply; hand edits are\r\n";
    s += L"; kept until then. Values use the system code page.\r\n";
    s += L";\r\n";
    s += L"; [Display] X/Y are pixel offsets from the selected monitor's top-left.\r\n";
    s += L"; Height=0 means \"auto-fit to content\". Colors are RRGGBB hex.\r\n";

    section(s, L"Meta");
    kvInt(s, L"Format", 2);  // serializer version (full rewrite, single header)

    section(s, L"General");
    kvBool(s, L"AutoStart", autoStart);
    kvInt(s, L"RefreshInterval", refreshMs);
    kvBool(s, L"Paused", paused);

    section(s, L"Display");
    kvInt(s, L"Monitor", monitorIndex);
    kvInt(s, L"X", x);
    kvInt(s, L"Y", y);
    kvInt(s, L"Width", width);
    kvInt(s, L"Height", height);
    kvInt(s, L"Opacity", opacity);
    kv(s, L"Font", SanitizeIniValue(fontFamily));
    kvInt(s, L"FontSize", static_cast<int>(fontSize + 0.5f));

    section(s, L"Monitor");
    kvBool(s, L"CPU", showCpu);
    kvBool(s, L"Memory", showMemory);
    kvBool(s, L"GPU", showGpu);
    kvBool(s, L"VRAM", showVram);
    kvBool(s, L"Disk", showDisk);
    kvBool(s, L"Network", showNetwork);
    kvBool(s, L"LANIP", showLanIp);

    section(s, L"Appearance");
    kvBool(s, L"ShowTitle", showTitle);
    kvBool(s, L"ShowBars", showBars);
    kvInt(s, L"CornerRadius", cornerRadius);
    kvInt(s, L"Padding", padding);
    WriteColorLine(s, L"Background", bgColor);
    WriteColorLine(s, L"Accent", accentColor);

    section(s, L"State");
    kvBool(s, L"HudVisible", hudVisible);

    const std::string bytes = WideToAcp(s);

    const std::wstring tmp = path + L".tmp";
    HANDLE f = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        log::Warn(L"Config::Save failed (temp file): " + path);
        return false;
    }
    DWORD written = 0;
    const bool wrote =
        ::WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
        written == bytes.size();
    ::CloseHandle(f);
    if (!wrote || !::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        ::DeleteFileW(tmp.c_str());
        log::Warn(L"Config::Save failed: " + path);
        return false;
    }
    return true;
}

bool Config::operator==(const Config& o) const {
    return autoStart == o.autoStart && refreshMs == o.refreshMs && paused == o.paused &&
           monitorIndex == o.monitorIndex && x == o.x && y == o.y && width == o.width &&
           height == o.height && opacity == o.opacity && fontFamily == o.fontFamily &&
           fontSize == o.fontSize && showCpu == o.showCpu && showMemory == o.showMemory &&
           showGpu == o.showGpu && showVram == o.showVram && showDisk == o.showDisk &&
           showNetwork == o.showNetwork && showLanIp == o.showLanIp &&
           showTitle == o.showTitle && showBars == o.showBars &&
           cornerRadius == o.cornerRadius && padding == o.padding &&
           bgColor == o.bgColor && accentColor == o.accentColor &&
           hudVisible == o.hudVisible;
}

// ---------------------------------------------------------------------------
// Auto start
// ---------------------------------------------------------------------------

bool SetAutoStart(bool enable) {
    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE,
                          nullptr, &key, nullptr) != ERROR_SUCCESS) {
        log::Warn(L"SetAutoStart: RegCreateKeyEx failed");
        return false;
    }

    bool ok = true;
    if (enable) {
        std::wstring cmd = L"\"" + paths::ExePath() + L"\"";
        const DWORD bytes = static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t));
        ok = ::RegSetValueExW(key, kRunValue, 0, REG_SZ,
                              reinterpret_cast<const BYTE*>(cmd.c_str()),
                              bytes) == ERROR_SUCCESS;
    } else {
        const LSTATUS st = ::RegDeleteValueW(key, kRunValue);
        ok = (st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND);
    }
    ::RegCloseKey(key);

    if (!ok) log::Warn(enable ? L"SetAutoStart(true) failed" : L"SetAutoStart(false) failed");
    return ok;
}

}  // namespace auraui
