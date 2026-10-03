#pragma once

#include <string>

namespace auraui {

// ---------------------------------------------------------------------------
// Persisted user configuration (INI, %APPDATA%\AuraUI\config.ini)
//
// X/Y are offsets in *physical pixels* from the top-left of the selected
// monitor, so negative virtual-desktop coordinates and multi-monitor setups
// work without special cases. Width/Height are physical pixels too; Height == 0
// means "size the panel to its content".
// ---------------------------------------------------------------------------

struct Config {
    // [General]
    bool autoStart   = false;
    int  refreshMs   = 1000;
    bool paused      = false;

    // [Display]
    int          monitorIndex = 0;   // 0 = primary
    int          x            = 80;
    int          y            = 80;
    int          width        = 320;
    int          height       = 0;   // 0 = auto-fit content
    int          opacity      = 205; // 0..255 (panel background alpha)
    std::wstring fontFamily   = L"Segoe UI";
    float        fontSize     = 14.0f;

    // [Monitor]
    bool showCpu     = true;
    bool showMemory  = true;
    bool showGpu     = true;
    bool showVram    = true;
    bool showDisk    = true;
    bool showNetwork = true;
    bool showLanIp   = true;

    // [Appearance]
    bool     showTitle    = true;
    bool     showBars     = true;
    int      cornerRadius = 10;
    int      padding      = 14;
    unsigned bgColor      = 0x0A0E14u;  // 0xRRGGBB
    unsigned accentColor  = 0x4CC2FFu;  // 0xRRGGBB

    // [State] - remembered across runs
    bool hudVisible = true;

    // Load from an INI file. Missing file / missing keys => defaults.
    static Config Load(const std::wstring& path);

    // Write the INI file (creating parent directories is the caller's job).
    bool Save(const std::wstring& path) const;

    // Clamp every field into a sane range. Called by Load() and by the settings UI.
    void Sanitize();

    bool operator==(const Config& o) const;
    bool operator!=(const Config& o) const { return !(*this == o); }
};

// --- Windows "run at login" (HKCU\...\Run, no admin required) --------------

bool SetAutoStart(bool enable);

}  // namespace auraui
