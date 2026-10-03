#pragma once

#include <windows.h>
#include <shellapi.h>

#include <string>

namespace auraui {

// Shell notification-area icon + context menu.
class Tray {
public:
    // Message the owner window receives for tray events (lp = mouse message).
    static constexpr UINT kCallbackMessage = WM_APP + 1;

    enum Command : int {
        CmdNone = 0,
        CmdToggleHud,
        CmdSettings,
        CmdMoveHud,
        CmdReload,
        CmdPause,
        CmdExit,
    };

    Tray();
    ~Tray();

    bool Create(HWND owner, HINSTANCE hinst, const std::wstring& tooltip);
    void Destroy();

    // False when the icon is not on the taskbar (initial NIM_ADD failed, or
    // after Destroy) - the app's health check retries Create until it sticks.
    bool Added() const { return added_; }

    void SetTooltip(const std::wstring& text);
    void SetPaused(bool paused) { paused_ = paused; }
    void SetHudVisible(bool visible) { hudVisible_ = visible; }

    // Call from the owner window's WndProc for Tray::kCallbackMessage.
    // Returns the chosen Command (CmdNone when the menu was dismissed).
    int HandleMessage(WPARAM wp, LPARAM lp);

    // One-shot balloon after the first successful start.
    void ShowStartupBalloon();

private:
    void ShowContextMenu(POINT pt);

    HWND              owner_    = nullptr;
    HINSTANCE         hinst_    = nullptr;
    NOTIFYICONDATAW   nid_{};
    bool              added_    = false;
    bool              paused_   = false;
    bool              hudVisible_ = true;
    bool              balloonShown_ = false;

    // True when Shell_NotifyIcon(NIM_SETVERSION) accepted NOTIFYICON_VERSION_4,
    // which changes how the callback message is packed (see HandleMessage).
    bool version4_ = false;
};

}  // namespace auraui
