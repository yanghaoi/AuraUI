#include "app/app.h"

#include "core/log.h"
#include "core/paths.h"
#include "core/win_util.h"
#include "core/version.h"
#include "desktop/desktop.h"

#include <wtsapi32.h>

#include <algorithm>
#include <vector>

namespace auraui {
namespace {

constexpr wchar_t kMsgClassName[] = L"AuraUIMessageWindow";
constexpr UINT    kHealthIntervalMs = 2000;

// Tray hover tooltip: just the product name.
constexpr wchar_t kTrayTooltip[] = L"AuraUI";

BOOL CALLBACK FindMonitorProc(HMONITOR mon, HDC, LPRECT, LPARAM param) {
    auto* out = reinterpret_cast<std::vector<std::pair<RECT, bool>>*>(param);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (::GetMonitorInfoW(mon, &mi))
        out->emplace_back(mi.rcMonitor, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0);
    return TRUE;
}

RECT MonitorRectFor(int index) {
    std::vector<std::pair<RECT, bool>> list;
    ::EnumDisplayMonitors(nullptr, nullptr, &FindMonitorProc, reinterpret_cast<LPARAM>(&list));
    if (list.empty()) {
        RECT r{};
        ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0);
        return r;
    }
    if (index >= 0 && index < static_cast<int>(list.size())) return list[static_cast<size_t>(index)].first;
    for (const auto& e : list)
        if (e.second) return e.first;
    return list.front().first;
}

bool SameRect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

}  // namespace

App::App() = default;

App::~App() { Shutdown(); }

LRESULT CALLBACK App::MsgWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<App*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->msgWnd_ = hwnd;
    } else {
        self = reinterpret_cast<App*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->Handle(msg, wp, lp);
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

bool App::CreateMessageWindow() {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = 0;
    wc.lpfnWndProc   = &App::MsgWndProc;
    wc.hInstance     = hinst_;
    wc.lpszClassName = kMsgClassName;
    wc.hIcon = static_cast<HICON>(::LoadImageW(hinst_, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0,
                                               LR_DEFAULTSIZE | LR_SHARED));

    if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        log::Error(L"App: RegisterClassEx failed: " + LastErrorString());
        return false;
    }

    // The window is a real (not message-only) top-level window parked off-screen
    // and *shown*. Two reasons it is not simply hidden:
    //   * a window that has never been shown cannot become the foreground window,
    //     and TrackPopupMenu needs a foreground owner or the tray menu is
    //     dismissed the instant it appears;
    //   * message-only (HWND_MESSAGE) windows never receive the TaskbarCreated
    //     broadcast or tray callbacks at all.
    // WS_EX_TOOLWINDOW keeps it out of the taskbar and Alt+Tab.
    const DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    msgWnd_ = ::CreateWindowExW(exStyle, kMsgClassName, L"AuraUI", WS_POPUP,
                                -32000, -32000, 1, 1, nullptr, nullptr, hinst_, this);
    if (!msgWnd_) {
        log::Error(L"App: CreateWindowEx failed: " + LastErrorString());
        return false;
    }
    ::ShowWindow(msgWnd_, SW_SHOWNOACTIVATE);
    return true;
}

bool App::Init(HINSTANCE hinst, const std::wstring& configPath) {
    hinst_      = hinst;
    configPath_ = configPath;

    if (!CreateMessageWindow()) return false;

    taskbarCreatedMsg_ = desktop::TaskbarCreatedMessage();
    lastExplorerPid_   = TaskbarExplorerPid();

    // The HUD's underlay worker posts its results here; every vacated desktop
    // rectangle is coalesced through the debounced refresh (NoteVacatedRect).
    hud_.SetNotifyWindow(msgWnd_);
    hud_.SetOnVacate([this](const RECT* r) { NoteVacatedRect(r); });

    const bool configExisted =
        ::GetFileAttributesW(configPath_.c_str()) != INVALID_FILE_ATTRIBUTES;
    cfg_ = Config::Load(configPath_);
    if (!configExisted) {
        log::Info(L"No config found, writing defaults to " + configPath_);
        cfg_.Save(configPath_);
    }

    // Reconcile both ways: a hand-edited AutoStart=0 must still remove a
    // stale Run entry from an earlier apply (the settings commit path always
    // calls this; startup used to only ever add).
    SetAutoStart(cfg_.autoStart);

    if (!hud_.Create(hinst_, cfg_)) {
        log::Error(L"App: HUD creation failed");
        return false;
    }
    hud_.SetVisible(cfg_.hudVisible);
    hud_.SetOnMoveFinished([this](bool applied, int x, int y, int monitorIndex) {
        if (applied && x > -32768 && x < 32768 && y > -32768 && y < 32768) {
            if (monitorIndex >= 0) cfg_.monitorIndex = monitorIndex;
            cfg_.x = x;
            cfg_.y = y;
            cfg_.Sanitize();
            cfg_.Save(configPath_);
            log::Info(L"App: HUD moved to monitor " + FmtInt(cfg_.monitorIndex) +
                      L" at " + FmtInt(cfg_.x) + L"," + FmtInt(cfg_.y));
        }
        // Rebuild the window instead of re-parenting it back into the desktop
        // layer: SetParent journeys are exactly what used to wedge the HUD's
        // layered composition. The HUD now renders through the plain GDI /
        // SetLayeredWindowAttributes path (see Hud::Create), whose redirection
        // surface survives re-parenting, so the rebuild is mostly belt and
        // braces - it also re-applies the dropped position in one step.
        ::PostMessageW(msgWnd_, kMsgRebuildHud, 0, 0);
    });

    settings_.Create(hinst_, cfg_);
    settings_.SetOnLive([this](const Config& c) { ApplyLive(c); });
    settings_.SetOnCommit([this](const Config& c) { ApplyConfig(c, true); });
    settings_.SetSnapshotProvider([this] { return sampler_.Get(); });

    tray_.Create(msgWnd_, hinst_, kTrayTooltip);
    tray_.SetPaused(cfg_.paused);
    tray_.SetHudVisible(cfg_.hudVisible);

    sampler_.SetNotify([this] {
        if (msgWnd_) ::PostMessageW(msgWnd_, kMsgSnapshot, 0, 0);
    });
    StartSampler();

    // Startup snapshot push: the tick message is dropped while paused, so an
    // app that was last shut down paused would otherwise paint an all-zero
    // panel until monitoring is resumed.
    hud_.SetSnapshot(sampler_.Get());

    ::SetTimer(msgWnd_, kTimerHealth, kHealthIntervalMs, nullptr);
    lastMonitorRect_ = MonitorRectFor(cfg_.monitorIndex);

    // Session reconnect / unlock: DWM rebuilds its surfaces and RDP clients
    // keep the last encoded frame - force a desktop-layer refresh then.
    wtsRegistered_ =
        ::WTSRegisterSessionNotification(msgWnd_, NOTIFY_FOR_THIS_SESSION) != FALSE;

    tray_.ShowStartupBalloon();
    log::Info(L"AuraUI v" + VersionW() + L" started");
    return true;
}

void App::StartSampler() {
    sampler_.SetPaused(cfg_.paused);
    sampler_.Start(cfg_.refreshMs);
}

void App::UpdateTrayTooltip() {
    // Tooltip stays the plain product name; state is visible in the context
    // menu labels (开始/暂停监控, 显示/隐藏 HUD) instead.
    tray_.SetTooltip(kTrayTooltip);
    tray_.SetPaused(cfg_.paused);
    tray_.SetHudVisible(cfg_.hudVisible);
}

void App::ApplyLive(const Config& cfg) {
    // paused / hudVisible are tray-controlled runtime state with no settings
    // control for either - the settings draft only carries a stale snapshot
    // of them. Overwriting cfg_ with the draft used to silently revert a
    // pause made from the tray while the settings window was open, and since
    // this path never touched the sampler the next tray toggle then acted on
    // the reverted flag and "did nothing" (label and state desynced).
    const bool paused     = cfg_.paused;
    const bool hudVisible = cfg_.hudVisible;

    cfg_ = cfg;
    cfg_.Sanitize();
    cfg_.paused     = paused;
    cfg_.hudVisible = hudVisible;

    hud_.ApplyConfig(cfg_);
    // Live means live: the refresh-interval combo previews too, instead of
    // silently only taking effect on "apply".
    sampler_.SetInterval(cfg_.refreshMs);
    // Self-healing sync: keep the sampler aligned with the preserved state.
    sampler_.SetPaused(cfg_.paused);
}

void App::ApplyConfig(const Config& cfg, bool persist) {
    const bool paused     = cfg_.paused;  // see ApplyLive: tray-owned state,
    const bool hudVisible = cfg_.hudVisible;  // not overwritable from settings

    cfg_ = cfg;
    cfg_.Sanitize();
    cfg_.paused     = paused;
    cfg_.hudVisible = hudVisible;

    if (persist) {
        SetAutoStart(cfg_.autoStart);
        cfg_.Save(configPath_);
    }

    hud_.ApplyConfig(cfg_);
    sampler_.SetInterval(cfg_.refreshMs);
    sampler_.SetPaused(cfg_.paused);
    UpdateTrayTooltip();
}

void App::ReloadConfig() {
    cfg_ = Config::Load(configPath_);
    hud_.ApplyConfig(cfg_);
    sampler_.SetInterval(cfg_.refreshMs);
    sampler_.SetPaused(cfg_.paused);
    hud_.SetVisible(cfg_.hudVisible);
    UpdateTrayTooltip();
    if (settings_.IsVisible()) settings_.Show(cfg_);
    log::Info(L"Configuration reloaded");
}

void App::ToggleHud() {
    if (hud_.InMoveMode()) hud_.EndMove(false);  // cannot hide a window mid-drag
    cfg_.hudVisible = !cfg_.hudVisible;
    hud_.SetVisible(cfg_.hudVisible);
    cfg_.Save(configPath_);
    UpdateTrayTooltip();
}

void App::TogglePause() {
    cfg_.paused = !cfg_.paused;
    sampler_.SetPaused(cfg_.paused);
    cfg_.Save(configPath_);
    UpdateTrayTooltip();
}

void App::OpenSettings() {
    if (!settings_.Hwnd()) {
        settings_.Create(hinst_, cfg_);
        settings_.SetOnLive([this](const Config& c) { ApplyLive(c); });
        settings_.SetOnCommit([this](const Config& c) { ApplyConfig(c, true); });
        settings_.SetSnapshotProvider([this] { return sampler_.Get(); });
    }
    settings_.Show(cfg_);
}

void App::BeginHudMove() {
    if (hud_.InMoveMode()) {
        // Second click on the menu item = "drop it here" - a recovery path in
        // case the double-click / right-click gesture did not land.
        hud_.EndMove(true);
        return;
    }
    if (!hud_.Hwnd()) return;
    // Moving a hidden HUD means "show it and let me place it" - the choice
    // persists, otherwise the post-move rebuild would hide it again right
    // after the user had placed it.
    if (!cfg_.hudVisible) {
        cfg_.hudVisible = true;
        cfg_.Save(configPath_);
        UpdateTrayTooltip();
        log::Info(L"App: HUD was hidden, showing it for the move (persisted)");
    }
    hud_.BeginMove();
}

void App::HealthCheck() {
    if (shuttingDown_) return;

    HWND hud = hud_.Hwnd();

    // 0) Explorer was killed: the desktop WorkerW went away and Windows
    //    destroyed our HUD together with it. Rebuild it.
    if (!hud) {
        if (!hud_.Create(hinst_, cfg_)) {
            if (!hudRecreateWarned_) {
                hudRecreateWarned_ = true;
                log::Warn(L"HUD window is gone and could not be recreated yet; retrying");
            }
            return;
        }
        hudRecreateWarned_ = false;
        wasAttached_       = true;
        lastMonitorRect_   = MonitorRectFor(cfg_.monitorIndex);
        log::Info(L"HUD window recreated after the desktop layer was rebuilt");
        return;
    }

    // The user is placing the HUD by hand: re-attaching or re-anchoring it from
    // here would yank it out of their hands mid-drag.
    if (hud_.InMoveMode()) return;

    // 1) Explorer restarted / desktop layer rebuilt => re-parent.
    const bool attached = desktop::IsAttached(hud);
    if (!attached) {
        if (wasAttached_) {
            log::Info(L"Desktop layer lost - re-attaching HUD (parent=" +
                      desktop::DescribeHost(::GetParent(hud)) + L")");
        }
        // allowSpawn=false: the health check must not nudge Progman (each
        // nudge spawns an extra WorkerW and can block up to a second).
        wasAttached_ = hud_.Reattach(/*allowSpawn=*/false);
    } else {
        wasAttached_ = true;

        // 2) Something moved us: re-assert "immediately below the desktop icons".
        desktop::EnsureZOrder(hud);
    }

    // 3) Monitor layout changed (resolution / hot-plug / scaling).
    const RECT mon = MonitorRectFor(cfg_.monitorIndex);
    if (!SameRect(mon, lastMonitorRect_)) {
        lastMonitorRect_ = mon;
        hud_.OnDisplayChange();
        sampler_.InvalidateNetwork();
    }

    // 4) The tray icon never made it onto the taskbar (Explorer was busy at
    // startup) - retry until it sticks.
    if (!tray_.Added()) tray_.Create(msgWnd_, hinst_, kTrayTooltip);
}

void App::NoteVacatedRect(const RECT* vacated) {
    if (shuttingDown_ || !msgWnd_) {
        desktop::RefreshWallpaper(vacated);
        return;
    }
    if (vacated && !::IsRectEmpty(vacated)) {
        if (wallpaperRefreshHasRect_) {
            ::UnionRect(&wallpaperRefreshRect_, &wallpaperRefreshRect_, vacated);
        } else {
            wallpaperRefreshRect_   = *vacated;
            wallpaperRefreshHasRect_ = true;
        }
    } else {
        // Desktop-wide request: covers every rect accumulated so far. Sticky
        // until the flush - a rect report arriving within the debounce window
        // must not downgrade the refresh (the WorkerW never repaints exposed
        // areas on its own, so the uncovered rest would keep its ghost).
        wallpaperRefreshWhole_ = true;
    }
    wallpaperRefreshPending_ = true;
    // Restarts the timeout: the flush fires 250ms after the LAST report, so a
    // slider drag (dozens of vacate reports per second) results in exactly one
    // wallpaper replay instead of one per tick.
    ::SetTimer(msgWnd_, kTimerWallpaperRefresh, 250, nullptr);
}

void App::FlushWallpaperRefresh() {
    if (!msgWnd_) return;
    ::KillTimer(msgWnd_, kTimerWallpaperRefresh);
    if (!wallpaperRefreshPending_) return;
    wallpaperRefreshPending_ = false;
    const bool whole          = wallpaperRefreshWhole_;
    const bool hasRect        = wallpaperRefreshHasRect_;
    const RECT rect           = wallpaperRefreshRect_;
    wallpaperRefreshWhole_    = false;
    wallpaperRefreshHasRect_  = false;
    // A pending desktop-wide request subsumes every accumulated rect: replay
    // only that rect when it is genuinely a rect-only refresh.
    const bool replayed =
        desktop::RefreshWallpaper((whole || !hasRect) ? nullptr : &rect);
    if (replayed) hud_.NotifyWallpaperReplayed();
}

DWORD App::TaskbarExplorerPid() {
    HWND tray = ::FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) return 0;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(tray, &pid);
    return pid;
}

LRESULT App::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == taskbarCreatedMsg_ && taskbarCreatedMsg_ != 0) {
        // TaskbarCreated also fires for non-crash reasons (e.g. DPI changes),
        // so compare the Explorer PID first: only a real restart needs the
        // heavy re-mount (Lively does the same check).
        const DWORD explorerPid = TaskbarExplorerPid();
        const bool explorerCrashed =
            explorerPid != 0 && lastExplorerPid_ != 0 && explorerPid != lastExplorerPid_;
        lastExplorerPid_ = explorerPid ? explorerPid : lastExplorerPid_;

        // The taskbar (and with it the notification area) is new regardless -
        // the tray icon must be re-added in both cases.
        tray_.Destroy();
        tray_.Create(msgWnd_, hinst_, kTrayTooltip);
        UpdateTrayTooltip();

        if (!explorerCrashed) return 0;  // e.g. DPI change: nothing to remount

        const ULONGLONG now = ::GetTickCount64();
        if (lastExplorerCrashTick_ != 0 && now - lastExplorerCrashTick_ < 30000) {
            // Crash storm: don't hammer the half-dead shell; the 2s health
            // check keeps retrying with its natural cadence.
            log::Warn(L"Explorer is restarting repeatedly - deferring HUD remount");
            lastExplorerCrashTick_ = now;
            return 0;
        }
        lastExplorerCrashTick_ = now;

        log::Info(L"Explorer restarted (TaskbarCreated) - re-mounting HUD");

        // Never touch the HUD while the user is dragging it into place; the
        // move-finish path (or the next health check) re-attaches it.
        if (!hud_.InMoveMode()) {
            if (!hud_.Hwnd()) {
                // The old desktop layer took our window down with it.
                if (hud_.Create(hinst_, cfg_)) {
                    hudRecreateWarned_ = false;
                    log::Info(L"HUD window recreated after Explorer restart");
                }
            } else {
                // Explorer just restarted - it is alive by definition, so
                // spawning the wallpaper layer on demand is safe here.
                hud_.Reattach(/*allowSpawn=*/true);
            }

            lastMonitorRect_ = MonitorRectFor(cfg_.monitorIndex);
        }
        return 0;
    }

    switch (msg) {
        case WM_COMMAND: {
            switch (LOWORD(wp)) {
                case Tray::CmdToggleHud:
                    ToggleHud();
                    return 0;
                case Tray::CmdSettings:
                    OpenSettings();
                    return 0;
                case Tray::CmdMoveHud:
                    BeginHudMove();
                    return 0;
                case Tray::CmdReload:
                    ReloadConfig();
                    return 0;
                case Tray::CmdPause:
                    TogglePause();
                    return 0;
                case Tray::CmdExit:
                    ::PostQuitMessage(0);
                    return 0;
                default:
                    break;
            }
            break;
        }

        case Tray::kCallbackMessage:
            tray_.HandleMessage(wp, lp);
            return 0;

        case kMsgSnapshot: {
            if (!cfg_.hudVisible || cfg_.paused) return 0;
            hud_.SetSnapshot(sampler_.Get());
            hud_.RequestRepaint();
            return 0;
        }

        case kMsgRebuildHud: {
            hud_.Destroy();
            if (!hud_.Create(hinst_, cfg_)) {
                log::Warn(L"App: HUD recreate after move failed; health check will retry");
            }
            if (settings_.IsVisible()) settings_.Show(cfg_);
            return 0;
        }

        case Hud::kUnderlayReadyMsg:
            hud_.ApplyUnderlayResult();
            return 0;

        case WM_TIMER:
            if (wp == kTimerHealth) {
                HealthCheck();
                return 0;
            }
            if (wp == kTimerWallpaperRefresh) {
                FlushWallpaperRefresh();
                return 0;
            }
            break;

        case WM_DISPLAYCHANGE:
            hud_.OnDisplayChange();
            lastMonitorRect_ = MonitorRectFor(cfg_.monitorIndex);
            sampler_.InvalidateNetwork();
            return 0;

        case WM_DEVICECHANGE:
            sampler_.InvalidateNetwork();
            return 0;

        case WM_SETTINGCHANGE:
            // The wallpaper is the one setting whose pixels the HUD embeds:
            // reload the underlay immediately instead of waiting for the
            // 60-render cadence (up to 60s of a stale wallpaper otherwise).
            if (wp == SPI_SETDESKWALLPAPER) hud_.RefreshUnderlay();
            hud_.RequestRepaint();
            return 0;

        case WM_WTSSESSION_CHANGE:
            // Session reconnect / unlock: DWM rebuilds its surfaces and RDP
            // clients keep showing the last encoded frame until a region is
            // marked dirty - refresh the underlay and the whole desktop layer.
            if (wp == WTS_CONSOLE_CONNECT || wp == WTS_REMOTE_CONNECT ||
                wp == WTS_SESSION_LOGON || wp == WTS_SESSION_UNLOCK) {
                log::Info(L"Session reconnect/unlock - refreshing desktop layer");
                hud_.RefreshUnderlay();
                hud_.RequestRepaint();
                NoteVacatedRect(nullptr);  // desktop-wide, debounced
            }
            return 0;

        case WM_QUERYENDSESSION:
            return TRUE;

        case WM_ENDSESSION:
            if (wp) {
                Shutdown();
                // Shutdown() clears GWLP_USERDATA before destroying the
                // window, so the WM_DESTROY dispatched by DestroyWindow()
                // falls through to DefWindowProc and never posts the quit -
                // the process would linger until Windows force-kills it,
                // stalling every logoff/shutdown by the kill timeout.
                ::PostQuitMessage(0);
            }
            return 0;

        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return ::DefWindowProcW(msgWnd_, msg, wp, lp);
}

int App::Run() {
    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    Shutdown();
    return static_cast<int>(msg.wParam);
}

void App::Shutdown() {
    if (shuttingDown_) return;
    shuttingDown_ = true;

    if (msgWnd_) {
        ::KillTimer(msgWnd_, kTimerHealth);
        ::KillTimer(msgWnd_, kTimerWallpaperRefresh);
    }

    // The underlay worker outlives msgWnd_ (joined only in ~Hud): detach it
    // now so it cannot PostMessageW into a destroyed - or worse, recycled -
    // window handle during teardown.
    hud_.SetNotifyWindow(nullptr);

    // Flush a pending debounced refresh now: quitting with a stale frame on
    // the desktop layer would leave a ghost behind for good.
    FlushWallpaperRefresh();

    if (msgWnd_ && wtsRegistered_) {
        ::WTSUnRegisterSessionNotification(msgWnd_);
        wtsRegistered_ = false;
    }

    sampler_.Stop();

    cfg_.Save(configPath_);

    settings_.Destroy();
    tray_.Destroy();
    hud_.Destroy();

    if (msgWnd_) {
        ::SetWindowLongPtrW(msgWnd_, GWLP_USERDATA, 0);
        ::DestroyWindow(msgWnd_);
        msgWnd_ = nullptr;
    }
    log::Info(L"AuraUI stopped");
}

}  // namespace auraui
