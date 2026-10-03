#pragma once

#include "config/config.h"
#include "monitor/snapshot.h"

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace auraui {

// ---------------------------------------------------------------------------
// Settings GUI - plain Win32 Common Controls, no external UI framework.
//
// The window edits a *draft* copy of the configuration:
//   * every control change fires onLive(draft)  -> HUD updates instantly
//   * 应用 / 确定 fires onCommit(draft)         -> config is persisted
//   * 取消 restores the configuration the window was opened with
// ---------------------------------------------------------------------------
class SettingsWindow {
public:
    using ApplyFn = std::function<void(const Config&)>;

    SettingsWindow();
    ~SettingsWindow();

    bool Create(HINSTANCE hinst, const Config& cfg);
    void Destroy();

    void Show(const Config& cfg);
    void Hide();
    // Discard the draft, restore the configuration the window was opened
    // with (both in the controls and live in the HUD) and hide. Shared by
    // BtnCancel and WM_CLOSE.
    void Cancel();
    bool IsVisible() const;

    HWND Hwnd() const { return hwnd_; }

    void SetOnLive(ApplyFn fn) { onLive_ = std::move(fn); }
    void SetOnCommit(ApplyFn fn) { onCommit_ = std::move(fn); }
    // Fresh hardware snapshot for the width guard (EnsureMinWidth) - labels
    // like the CPU brand / GPU name are what the panel must fit. Optional:
    // without it the guard only sees format-independent widths.
    void SetSnapshotProvider(std::function<Snapshot()> fn) {
        snapshotProvider_ = std::move(fn);
    }

private:
    static LRESULT CALLBACK Thunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);

    void CreateControls();
    // Move every control to its (DPI-scaled) table position. Called on
    // WM_DPICHANGED and when Show() notices a new monitor DPI - control
    // coordinates are otherwise frozen at Create()-time DPI.
    void RelayoutControls();
    // Size the window so the client area is kDesignW x kDesignH scaled by
    // the CURRENT dpi_. Controls are placed S(dpi)-scaled; the window itself
    // is created at raw design pixels, so on a scaled monitor (e.g. 2K at
    // 125%) the bottom buttons fall outside the client area without this.
    // moveX/moveY (LONG_MIN = keep position) let WM_DPICHANGED adopt the
    // system's suggested position while keeping the size authoritative.
    void ResizeToDesignDpi(LONG moveX = LONG_MIN, LONG moveY = LONG_MIN);
    void ApplyFontToChildren();
    // Create a new font for the current dpi_, hand it to the children, then
    // delete the old one (children must never hold a deleted HFONT).
    void RebuildFont();
    HFONT CreateFontForDpi() const;
    void PopulateMonitors();
    void PopulateFonts();
    void WriteControls();
    void ReadControls();
    void RefreshValueLabels();
    // Width guard, runs on 应用/确定 before the commit: measure the minimum
    // panel width the drafted settings need and, when the typed width is
    // smaller, raise it (control box included) and tell the user why.
    void EnsureMinWidth();
    void EmitLive();
    void EmitCommit();

    int  S(int v) const;  // scale a design pixel value by the window DPI
    HWND Item(int id) const;

    HWND      hwnd_  = nullptr;
    HINSTANCE hinst_ = nullptr;
    UINT      dpi_   = 96;

    Config draft_{};
    Config original_{};

    HFONT font_ = nullptr;

    // Created HWNDs, parallel to the control table in the .cpp (index-based,
    // so id-less group boxes and labels relayout too).
    std::vector<HWND> controlHwnds_;

    ApplyFn onLive_;
    ApplyFn onCommit_;
    std::function<Snapshot()> snapshotProvider_;
};

}  // namespace auraui
