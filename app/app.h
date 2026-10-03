#pragma once

#include "config/config.h"
#include "hud/hud.h"
#include "monitor/sampler.h"
#include "settings/settings.h"
#include "tray/tray.h"

#include <windows.h>

#include <string>

namespace auraui {

// ---------------------------------------------------------------------------
// Application glue: one hidden top-level message window that owns the tray
// icon, the health-check timer and the Explorer/display broadcast handling.
//
// The HUD itself is a *child* of the desktop WorkerW, so it cannot receive
// broadcast messages - that is exactly why this window exists.
// ---------------------------------------------------------------------------
class App {
public:
    App();
    ~App();

    bool Init(HINSTANCE hinst, const std::wstring& configPath);
    int  Run();

private:
    static LRESULT CALLBACK MsgWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    enum : UINT {
        kMsgSnapshot   = WM_APP + 2,  // posted by the sampler thread
        kMsgRebuildHud = WM_APP + 3,  // deferred HUD recreate after a move
        kTimerHealth   = 1,
        kTimerWallpaperRefresh = 2,   // debounced desktop-layer repaint
    };

    bool CreateMessageWindow();
    void ApplyConfig(const Config& cfg, bool persist);
    void ApplyLive(const Config& cfg);
    void ReloadConfig();
    void ToggleHud();
    void TogglePause();
    void OpenSettings();
    void BeginHudMove();
    void UpdateTrayTooltip();
    void StartSampler();
    void HealthCheck();
    void Shutdown();

    // Coalesced desktop-layer repaint: the HUD reports every vacated rectangle
    // here; the union is flushed once, 250ms after the last report, instead of
    // replaying the wallpaper per event (geometry slider drags fire dozens of
    // vacate reports per second).
    void NoteVacatedRect(const RECT* vacated);
    void FlushWallpaperRefresh();

    // PID of the process owning the taskbar (0 when Explorer is down). Used to
    // tell a real Explorer restart from a mere TaskbarCreated re-broadcast
    // (DPI changes send those too).
    static DWORD TaskbarExplorerPid();

    HINSTANCE    hinst_      = nullptr;
    HWND         msgWnd_     = nullptr;
    std::wstring configPath_;
    std::wstring className_;

    Config         cfg_{};
    Hud            hud_;
    Sampler        sampler_;
    SettingsWindow settings_;
    Tray           tray_;

    UINT taskbarCreatedMsg_ = 0;
    RECT lastMonitorRect_{};
    bool wasAttached_       = false;
    bool hudRecreateWarned_ = false;
    bool shuttingDown_      = false;

    // Debounced wallpaper refresh state (see NoteVacatedRect).
    bool wallpaperRefreshPending_  = false;
    bool wallpaperRefreshHasRect_  = false;
    bool wallpaperRefreshWhole_    = false;  // desktop-wide request, sticky until flushed
    RECT wallpaperRefreshRect_{};

    // Explorer crash detection (TaskbarCreated PID comparison).
    DWORD lastExplorerPid_      = 0;
    ULONGLONG lastExplorerCrashTick_ = 0;

    // WTS session notification (reconnect / unlock -> refresh desktop layer).
    bool wtsRegistered_ = false;
};

}  // namespace auraui
