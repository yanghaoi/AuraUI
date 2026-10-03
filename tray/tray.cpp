#include "tray/tray.h"

#include "core/log.h"
#include "core/version.h"
#include "core/win_util.h"

#include <cwchar>
#include <iterator>

namespace auraui {
namespace {

std::wstring Hex(unsigned v) {
    wchar_t b[16];
    ::swprintf(b, 16, L"%X", v);
    return b;
}

}  // namespace

Tray::Tray() = default;

Tray::~Tray() { Destroy(); }

bool Tray::Create(HWND owner, HINSTANCE hinst, const std::wstring& tooltip) {
    if (added_) return true;
    if (!owner || !::IsWindow(owner)) return false;

    owner_ = owner;
    hinst_ = hinst;

    nid_ = NOTIFYICONDATAW{};
    nid_.cbSize           = sizeof(NOTIFYICONDATAW);
    nid_.hWnd             = owner;
    nid_.uID              = 1;
    // NIF_SHOWTIP is mandatory for hover tooltips on NOTIFYICON_VERSION_4
    // icons - without it the shell accepts szTip but never shows it.
    nid_.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid_.uCallbackMessage = kCallbackMessage;
    nid_.hIcon = static_cast<HICON>(
        ::LoadImageW(hinst, MAKEINTRESOURCEW(1), IMAGE_ICON, ::GetSystemMetrics(SM_CXSMICON),
                     ::GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    if (!nid_.hIcon) nid_.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);

    const size_t maxTip = std::size(nid_.szTip) - 1;
    const size_t n = (tooltip.size() < maxTip) ? tooltip.size() : maxTip;
    std::wmemcpy(nid_.szTip, tooltip.c_str(), n);
    nid_.szTip[n] = L'\0';

    if (!::Shell_NotifyIconW(NIM_ADD, &nid_)) {
        log::Warn(L"Tray: Shell_NotifyIcon(NIM_ADD) failed: " + LastErrorString());
        return false;
    }

    // Opt in to NOTIFYICON_VERSION_4. This is NOT cosmetic - it changes the
    // callback message layout (see HandleMessage) and gives us real screen
    // coordinates plus keyboard activation. Track whether it took effect so the
    // handler can fall back to the legacy layout.
    nid_.uVersion = NOTIFYICON_VERSION_4;
    version4_ = ::Shell_NotifyIconW(NIM_SETVERSION, &nid_) != FALSE;
    if (!version4_)
        log::Warn(L"Tray: NIM_SETVERSION(NOTIFYICON_VERSION_4) failed, "
                  L"falling back to the legacy callback layout");

    added_ = true;
    return true;
}

void Tray::Destroy() {
    if (!added_) return;
    ::Shell_NotifyIconW(NIM_DELETE, &nid_);
    added_ = false;
}

void Tray::SetTooltip(const std::wstring& text) {
    if (!added_) return;
    const size_t maxTip = std::size(nid_.szTip) - 1;
    const size_t n = (text.size() < maxTip) ? text.size() : maxTip;
    std::wmemcpy(nid_.szTip, text.c_str(), n);
    nid_.szTip[n] = L'\0';
    ::Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

void Tray::ShowStartupBalloon() {
    if (!added_ || balloonShown_) return;
    balloonShown_ = true;

    nid_.uFlags |= NIF_INFO;
    ::wcsncpy(nid_.szInfo,
              L"已挂载至桌面壁纸层，Win+D 不会消失。\n左键打开设置面板，右键打开菜单。",
              std::size(nid_.szInfo) - 1);
    ::wcsncpy(nid_.szInfoTitle, (L"AuraUI v" + VersionW()).c_str(),
              std::size(nid_.szInfoTitle) - 1);
    nid_.dwInfoFlags = NIIF_NOSOUND;
    ::Shell_NotifyIconW(NIM_MODIFY, &nid_);
    nid_.uFlags &= ~NIF_INFO;
}

void Tray::ShowContextMenu(POINT pt) {
    HMENU menu = ::CreatePopupMenu();
    if (!menu) return;

    ::AppendMenuW(menu, MF_STRING, CmdToggleHud, hudVisible_ ? L"隐藏 HUD" : L"显示 HUD");
    ::AppendMenuW(menu, MF_STRING, CmdSettings, L"设置...");
    ::AppendMenuW(menu, MF_STRING, CmdMoveHud, L"移动 HUD 位置...");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, CmdReload, L"重新加载配置");
    // The label itself carries the state (checked style alone was too subtle and
    // read as "nothing happened" after clicking).
    ::AppendMenuW(menu, MF_STRING, CmdPause, paused_ ? L"开始监控" : L"暂停监控");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, CmdExit, L"退出");

    // Sanity check the coordinates: a bogus point puts the menu somewhere the
    // user cannot see (behind the taskbar, off-screen) and it looks like
    // "right click does nothing".
    const int vx = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (pt.x < vx || pt.x > vx + vw || pt.y < vy || pt.y > vy + vh) {
        log::Warn(L"Tray: menu point " + FmtInt(pt.x) + L"," + FmtInt(pt.y) +
                  L" is outside the virtual screen - falling back to the cursor");
        ::GetCursorPos(&pt);
    }

    // A popup menu only behaves (keyboard input, dismiss on outside click) when
    // its owner is the foreground window. SetForegroundWindow on its own often
    // fails because the shell holds the foreground lock, so attach to the
    // current foreground thread first - that is the reliable way.
    HWND fg = ::GetForegroundWindow();
    const DWORD fgThread = fg ? ::GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD myThread = ::GetCurrentThreadId();
    bool attached = false;
    if (fgThread != 0 && fgThread != myThread) {
        attached = ::AttachThreadInput(myThread, fgThread, TRUE) != FALSE;
    }
    const BOOL fgOk = ::SetForegroundWindow(owner_);
    if (attached) ::AttachThreadInput(myThread, fgThread, FALSE);

    log::Info(L"Tray: showing menu at " + FmtInt(pt.x) + L"," + FmtInt(pt.y) +
              L" (SetForegroundWindow=" + std::wstring(fgOk ? L"ok" : L"failed") +
              L", attached=" + std::wstring(attached ? L"yes" : L"no") + L")");

    const int cmd = static_cast<int>(::TrackPopupMenu(
        menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, owner_,
        nullptr));

    ::DestroyMenu(menu);
    ::PostMessageW(owner_, WM_NULL, 0, 0);

    log::Info(L"Tray: menu returned command " + FmtInt(cmd));

    if (cmd != CmdNone) {
        // Re-dispatch through the owner so all handling lives in one place.
        ::PostMessageW(owner_, WM_COMMAND, MAKEWPARAM(static_cast<UINT>(cmd), 0), 0);
    }
}

int Tray::HandleMessage(WPARAM wp, LPARAM lp) {
    // Callback layouts:
    //
    //   NOTIFYICON_VERSION_4 (we opt in via NIM_SETVERSION in Create()):
    //     wParam : LOWORD = x, HIWORD = y  (screen coordinates, signed shorts)
    //     lParam : LOWORD = event, HIWORD = icon id
    //
    //   legacy (when NIM_SETVERSION failed / was not accepted):
    //     wParam : icon id
    //     lParam : mouse message (values fit in 16 bits)
    //
    // LOWORD(lp) therefore holds the event in BOTH layouts, so it is read from
    // there unconditionally; only the coordinate source depends on the layout.
    const UINT event  = LOWORD(lp);
    const UINT iconId = HIWORD(lp);

    POINT pt{};
    if (version4_) {
        pt.x = static_cast<short>(LOWORD(wp));
        pt.y = static_cast<short>(HIWORD(wp));
    } else {
        ::GetCursorPos(&pt);
    }

    // Hover traffic (WM_MOUSEMOVE fires per pixel) goes to the debug level
    // only - it was ~2/3 of a default log.
    if (event != WM_MOUSEMOVE)
        log::Debug(L"Tray callback: event=0x" + Hex(event) +
                   std::wstring(version4_ ? L" (v4)" : L" (legacy)") + L" iconId=" +
                   FmtInt(iconId) + L" pt=" + FmtInt(pt.x) + L"," + FmtInt(pt.y));

    // In v4 mode the shell delivers BOTH the raw mouse messages (WM_RBUTTONUP,
    // WM_LBUTTONUP, WM_LBUTTONDBLCLK, ...) AND the abstract events
    // (WM_CONTEXTMENU, NIN_SELECT) for the same physical click. Only the
    // abstract ones are acted on in v4 mode: handling the raw ones as well runs
    // every action twice per click, which showed up as the context menu
    // re-opening ("flashing") the instant a menu item had been chosen.
    switch (event) {
        case WM_CONTEXTMENU:  // right click (v4, and the keyboard menu key)
            if (version4_) ShowContextMenu(pt);
            return CmdNone;

        case WM_RBUTTONUP:    // right click (legacy)
            if (!version4_) ShowContextMenu(pt);
            return CmdNone;

        // Primary action, same as a left click.
        case NIN_SELECT:      // left click / Enter / Space on the focused icon (v4)
        case NIN_KEYSELECT:
        case NIN_BALLOONUSERCLICK:
            ::PostMessageW(owner_, WM_COMMAND, MAKEWPARAM(CmdSettings, 0), 0);
            return CmdNone;

        case WM_LBUTTONUP:    // left click / double click (legacy)
        case WM_LBUTTONDBLCLK:
            if (!version4_)
                ::PostMessageW(owner_, WM_COMMAND, MAKEWPARAM(CmdSettings, 0), 0);
            return CmdNone;

        default:
            // WM_MOUSEMOVE fires for every pixel the cursor travels over the
            // icon and would flood the log; everything else is worth a line.
            if (event != WM_MOUSEMOVE)
                log::Info(L"Tray callback ignored: wp=0x" + Hex(static_cast<unsigned>(wp)) +
                          L" lp=0x" + Hex(static_cast<unsigned>(lp)));
            return CmdNone;
    }
}

}  // namespace auraui
