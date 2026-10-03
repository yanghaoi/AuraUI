#include "settings/settings.h"

#include "core/log.h"
#include "core/paths.h"
#include "core/version.h"
#include "core/win_util.h"
#include "hud/hud.h"
#include "settings/settings_ids.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <iterator>
#include <vector>

namespace auraui {
namespace {

constexpr wchar_t kSettingsClassName[] = L"AuraUISettingsWindow";

constexpr int kDesignW = 760;
constexpr int kDesignH = 620;

struct RefreshOption {
    const wchar_t* text;
    int            ms;
};
const RefreshOption kRefreshOptions[] = {
    {L"100 ms", 100},   {L"250 ms", 250},   {L"500 ms", 500},
    {L"1000 ms", 1000}, {L"2000 ms", 2000}, {L"5000 ms", 5000},
};

const wchar_t* const kFontChoices[] = {
    L"Segoe UI", L"Microsoft YaHei UI", L"Microsoft YaHei", L"Consolas",
    L"Cascadia Mono", L"Tahoma", L"Arial", L"宋体", L"微软雅黑",
};

const wchar_t* const kFontSizeChoices[] = {L"10", L"11", L"12", L"13", L"14",
                                           L"16", L"18", L"20", L"24"};

UINT QueryDpi(HWND hwnd) {
    using PFN_GetDpiForWindow = UINT(WINAPI*)(HWND);
    static PFN_GetDpiForWindow pfn = reinterpret_cast<PFN_GetDpiForWindow>(
        reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"user32.dll"),
                                                 "GetDpiForWindow")));
    if (pfn && hwnd) {
        const UINT dpi = pfn(hwnd);
        if (dpi) return dpi;
    }
    HDC dc = ::GetDC(nullptr);
    const UINT dpi = dc ? static_cast<UINT>(::GetDeviceCaps(dc, LOGPIXELSX)) : 96u;
    if (dc) ::ReleaseDC(nullptr, dc);
    return dpi ? dpi : 96u;
}

// Effective DPI of a monitor (MDT_EFFECTIVE), dynamically loaded - the system
// DPI via LOGPIXELSX stays 96 in a per-monitor process while the monitors
// themselves scale.
UINT DpiOfMonitor(HMONITOR mon) {
    using PFN_GetDpiForMonitor = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static PFN_GetDpiForMonitor pfn = reinterpret_cast<PFN_GetDpiForMonitor>(
        reinterpret_cast<void*>(::GetProcAddress(
            ::GetModuleHandleW(L"shcore.dll"), "GetDpiForMonitor")));
    UINT x = 96, y = 96;
    if (pfn && mon && SUCCEEDED(pfn(mon, 0 /*MDT_EFFECTIVE*/, &x, &y)) && x)
        return x;
    return 96;
}

// Effective DPI of the primary monitor - the best pre-creation estimate of
// where the settings window will land.
UINT PrimaryMonitorDpi() {
    return DpiOfMonitor(::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
}

// Effective DPI of the monitor the draft targets. The HUD enumerates monitors
// with EnumDisplayMonitors and indexes into that order, so match it here
// (PopulateMonitors fills the combo from the same enumeration); anything
// out of range falls back to the primary.
struct MonitorIndexCtx {
    int      index = 0;
    int      cur   = 0;
    HMONITOR mon   = nullptr;
};

BOOL CALLBACK MonitorIndexEnumProc(HMONITOR mon, HDC, LPRECT, LPARAM param) {
    auto* ctx = reinterpret_cast<MonitorIndexCtx*>(param);
    if (ctx->cur == ctx->index) ctx->mon = mon;
    ++ctx->cur;
    return TRUE;
}

UINT MonitorIndexDpi(int index) {
    MonitorIndexCtx ctx;
    ctx.index = index;
    ::EnumDisplayMonitors(nullptr, nullptr, &MonitorIndexEnumProc,
                          reinterpret_cast<LPARAM>(&ctx));
    if (!ctx.mon) ctx.mon = ::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    return DpiOfMonitor(ctx.mon);
}

// Center the given window size on the primary monitor's work area.
POINT CenterOnPrimaryWorkArea(int w, int h) {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    ::GetMonitorInfoW(::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY),
                      &mi);
    const RECT& wa = mi.rcWork;
    return POINT{(wa.left + wa.right - w) / 2, (wa.top + wa.bottom - h) / 2};
}

std::wstring GetText(HWND h) {
    const int n = ::GetWindowTextLengthW(h);
    if (n <= 0) return {};
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    const int got = ::GetWindowTextW(h, s.data(), n + 1);
    s.resize(static_cast<size_t>(got > 0 ? got : 0));
    return s;
}

BOOL CALLBACK SetFontProc(HWND child, LPARAM lp) {
    ::SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(lp), TRUE);
    return TRUE;
}

BOOL CALLBACK MonitorEnumProc(HMONITOR mon, HDC, LPRECT, LPARAM param) {
    auto* out = reinterpret_cast<std::vector<std::pair<RECT, bool>>*>(param);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (::GetMonitorInfoW(mon, &mi))
        out->emplace_back(mi.rcMonitor, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0);
    return TRUE;
}

// Control layout as a table: CreateControls builds from it at the window's
// current DPI, RelayoutControls re-positions from it when the DPI changes.
// Coordinates are 96-dpi design units scaled by S().
struct ControlSpec {
    int            id;     // 0 = no control id (group boxes, plain labels)
    const wchar_t* cls;    // window class
    const wchar_t* text;   // nullptr = text composed at runtime
    DWORD          style;  // beyond WS_CHILD | WS_VISIBLE
    int x, y, w, h;
};

const ControlSpec kControls[] = {
    // ---------------- left column: 常规 ----------------
    {0, L"BUTTON", L"常规", BS_GROUPBOX, 16, 12, 352, 92},
    {ids::ChkAutoStart, L"BUTTON", L"Windows 启动时自动运行",
     BS_AUTOCHECKBOX | WS_TABSTOP, 30, 36, 300, 22},
    {0, L"STATIC", L"刷新间隔", SS_LEFT, 30, 68, 76, 20},
    {ids::CmbRefresh, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
     110, 66, 130, 220},

    // ---------------- left column: 显示 ----------------
    {0, L"BUTTON", L"显示", BS_GROUPBOX, 16, 112, 352, 268},
    {0, L"STATIC", L"显示器", SS_LEFT, 30, 140, 70, 20},
    {ids::CmbMonitor, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
     104, 138, 248, 220},
    {0, L"STATIC", L"位置 X", SS_LEFT, 30, 172, 50, 20},
    // X/Y allow '-' (monitors left of/above the primary have negative
    // coordinates); ReadControls() validates via wcstol with a fallback.
    {ids::EdtX, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 84, 170, 64, 24},
    {0, L"STATIC", L"Y", SS_LEFT, 156, 172, 16, 20},
    {ids::EdtY, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 176, 170, 64, 24},
    {0, L"STATIC", L"尺寸 宽", SS_LEFT, 30, 202, 60, 20},
    {ids::EdtW, L"EDIT", L"", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, 94, 200, 64, 24},
    {0, L"STATIC", L"高", SS_LEFT, 166, 202, 20, 20},
    {ids::EdtH, L"EDIT", L"", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, 190, 200, 64, 24},
    {ids::LblMonitorHint, L"STATIC", L"高=0 自动", SS_LEFT, 262, 204, 90, 18},
    {0, L"STATIC", L"透明度", SS_LEFT, 30, 236, 60, 20},
    {ids::SldOpacity, L"msctls_trackbar32", L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP,
     94, 232, 168, 28},
    {ids::LblOpacityValue, L"STATIC", L"", SS_LEFT, 268, 238, 92, 20},
    {0, L"STATIC", L"字体", SS_LEFT, 30, 272, 60, 20},
    {ids::CmbFont, L"COMBOBOX", L"", CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP,
     94, 270, 258, 220},
    {0, L"STATIC", L"字号", SS_LEFT, 30, 302, 60, 20},
    {ids::CmbFontSize, L"COMBOBOX", L"", CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP,
     94, 300, 90, 220},
    {ids::BtnCenter, L"BUTTON", L"移到该显示器左上角", BS_PUSHBUTTON | WS_TABSTOP,
     196, 299, 156, 26},

    // ---------------- left column: 外观 ----------------
    {0, L"BUTTON", L"外观", BS_GROUPBOX, 16, 388, 352, 180},
    {ids::ChkTitle, L"BUTTON", L"显示 主机名 标题", BS_AUTOCHECKBOX | WS_TABSTOP,
     30, 414, 300, 22},
    {ids::ChkBars, L"BUTTON", L"显示进度条", BS_AUTOCHECKBOX | WS_TABSTOP,
     30, 440, 300, 22},
    {0, L"STATIC", L"圆角", SS_LEFT, 30, 474, 60, 20},
    {ids::SldRadius, L"msctls_trackbar32", L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP,
     94, 470, 168, 28},
    {ids::LblRadiusValue, L"STATIC", L"", SS_LEFT, 268, 476, 92, 20},
    {ids::BtnOpenConfig, L"BUTTON", L"打开配置目录", BS_PUSHBUTTON | WS_TABSTOP,
     30, 518, 140, 28},

    // ---------------- right column: 监控项目 ----------------
    {0, L"BUTTON", L"监控项目", BS_GROUPBOX, 392, 12, 352, 252},
    {ids::ChkCpu, L"BUTTON", L"CPU 使用率", BS_AUTOCHECKBOX | WS_TABSTOP, 408, 40, 320, 22},
    {ids::ChkMemory, L"BUTTON", L"内存 RAM", BS_AUTOCHECKBOX | WS_TABSTOP, 408, 70, 320, 22},
    {ids::ChkGpu, L"BUTTON", L"GPU 使用率", BS_AUTOCHECKBOX | WS_TABSTOP, 408, 100, 320, 22},
    {ids::ChkVram, L"BUTTON", L"显存 VRAM", BS_AUTOCHECKBOX | WS_TABSTOP, 408, 130, 320, 22},
    {ids::ChkDisk, L"BUTTON", L"磁盘 Disk", BS_AUTOCHECKBOX | WS_TABSTOP, 408, 160, 320, 22},
    {ids::ChkNetwork, L"BUTTON", L"网络 Network (↓/↑)", BS_AUTOCHECKBOX | WS_TABSTOP,
     408, 190, 320, 22},
    {ids::ChkLanIp, L"BUTTON", L"局域网 IP", BS_AUTOCHECKBOX | WS_TABSTOP, 408, 220, 320, 22},

    // ---------------- right column: 关于 ----------------
    {0, L"BUTTON", L"关于 / 帮助", BS_GROUPBOX, 392, 272, 352, 296},
    {ids::LblAbout, L"STATIC",
     L"· HUD 位于桌面壁纸层与桌面图标之间，按 Win+D 不会隐藏。\n"
     L"· 鼠标点击可穿透 HUD，桌面图标操作不受影响。\n"
     L"· Explorer 重启后会自动重新挂载，无需手动操作。\n"
     L"· 设置改动会立即预览；“确定/应用”后才写入配置文件。\n"
     L"· 托盘图标右键：显示/隐藏 HUD、设置、重载配置、\n"
     L"  暂停监控、退出。",
     SS_LEFT, 408, 296, 322, 142},
    {ids::LblConfigPath, L"STATIC", L"", SS_LEFT, 408, 444, 322, 112},

    // ---------------- bottom row ----------------
    // Version badge between the button clusters (single source: core/version.h)
    {ids::LblVersion, L"STATIC", nullptr, SS_LEFT, 330, 588, 140, 20},
    {ids::BtnDefaults, L"BUTTON", L"恢复默认值", BS_PUSHBUTTON | WS_TABSTOP,
     16, 580, 120, 30},
    {ids::BtnApply, L"BUTTON", L"应用", BS_PUSHBUTTON | WS_TABSTOP, 476, 580, 84, 30},
    {ids::BtnOk, L"BUTTON", L"确定", BS_DEFPUSHBUTTON | WS_TABSTOP, 568, 580, 84, 30},
    {ids::BtnCancel, L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP, 660, 580, 84, 30},
};

}  // namespace

SettingsWindow::SettingsWindow() = default;

SettingsWindow::~SettingsWindow() { Destroy(); }

int SettingsWindow::S(int v) const { return ::MulDiv(v, static_cast<int>(dpi_), 96); }

HWND SettingsWindow::Item(int id) const {
    return hwnd_ ? ::GetDlgItem(hwnd_, id) : nullptr;
}

LRESULT CALLBACK SettingsWindow::Thunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SettingsWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<SettingsWindow*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<SettingsWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->WndProc(msg, wp, lp);
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT SettingsWindow::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);

            switch (id) {
                case ids::BtnApply:
                    ReadControls();
                    EnsureMinWidth();
                    EmitCommit();
                    return 0;

                case ids::BtnOk:
                    ReadControls();
                    EnsureMinWidth();
                    EmitCommit();
                    Hide();
                    return 0;

                case ids::BtnCancel:
                    Cancel();
                    return 0;

                case ids::BtnDefaults:
                    draft_ = Config{};
                    draft_.monitorIndex = original_.monitorIndex;
                    WriteControls();
                    EmitLive();
                    return 0;

                case ids::BtnCenter:
                    draft_.x = 0;
                    draft_.y = 0;
                    WriteControls();
                    EmitLive();
                    return 0;

                case ids::BtnOpenConfig:
                    ::ShellExecuteW(nullptr, L"open", paths::AppDataDir().c_str(), nullptr,
                                    nullptr, SW_SHOWNORMAL);
                    return 0;

                default:
                    break;
            }

            const bool live =
                (code == BN_CLICKED) || (code == CBN_SELCHANGE) || (code == CBN_EDITCHANGE) ||
                (code == EN_KILLFOCUS);
            // CBN_SELENDOK: picking an item from a CBS_DROPDOWN list posts
            // CBN_SELCHANGE while the edit control still shows the OLD text -
            // GetWindowText() in that moment reads the pre-selection value and
            // the change is silently lost. By CBN_SELENDOK the edit text is
            // the picked one, so re-read there.
            const bool comboFinalized = (code == CBN_SELENDOK);
            if (live || comboFinalized) {
                ReadControls();
                RefreshValueLabels();
                EmitLive();
            }
            return 0;
        }

        case WM_HSCROLL: {
            HWND ctl = reinterpret_cast<HWND>(lp);
            if (ctl == Item(ids::SldOpacity) || ctl == Item(ids::SldRadius)) {
                ReadControls();
                RefreshValueLabels();
                EmitLive();
            }
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            // Make the two hint labels slightly dimmer.
            HWND ctl = reinterpret_cast<HWND>(lp);
            if (ctl == Item(ids::LblMonitorHint) || ctl == Item(ids::LblConfigPath)) {
                HDC dc = reinterpret_cast<HDC>(wp);
                ::SetTextColor(dc, RGB(110, 118, 130));
                ::SetBkMode(dc, TRANSPARENT);
                return reinterpret_cast<LRESULT>(::GetSysColorBrush(COLOR_BTNFACE));
            }
            break;
        }

        case WM_DPICHANGED: {
            // PerMonitorV2: rescale the window, the font and every control
            // position. Trust the suggested rect only for the POSITION - the
            // SIZE is computed from the authoritative new DPI instead: during
            // session DPI transitions (RDP reconnects) the system can deliver
            // a suggested rect scaled by a STALE factor, which once shrank
            // the freshly-scaled window right back and left the buttons
            // outside the client area.
            const RECT* sug = reinterpret_cast<const RECT*>(lp);
            const UINT dpi  = HIWORD(wp);
            if (dpi) dpi_ = dpi;
            ResizeToDesignDpi(sug ? sug->left : LONG_MIN,
                              sug ? sug->top : LONG_MIN);
            RebuildFont();
            RelayoutControls();
            return 0;
        }

        case WM_CLOSE:
            // The X button means "cancel", exactly like BtnCancel: preview
            // values live only in the HUD until applied, and closing the
            // window must not leave them as the persistence baseline.
            Cancel();
            return 0;

        case WM_NCDESTROY: {
            HWND dying = hwnd_;
            ::SetWindowLongPtrW(dying, GWLP_USERDATA, 0);
            hwnd_ = nullptr;
            return ::DefWindowProcW(dying, msg, wp, lp);
        }

        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, msg, wp, lp);
}

bool SettingsWindow::Create(HINSTANCE hinst, const Config& cfg) {
    hinst_ = hinst;
    draft_ = cfg;
    original_ = cfg;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &SettingsWindow::Thunk;
    wc.hInstance     = hinst;
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kSettingsClassName;
    wc.hIcon = static_cast<HICON>(::LoadImageW(hinst, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0,
                                               LR_DEFAULTSIZE | LR_SHARED));
    wc.hIconSm = wc.hIcon;

    if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        log::Error(L"Settings: RegisterClassEx failed: " + LastErrorString());
        return false;
    }

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    // Create with an EXPLICIT position and the DPI-scaled size - no
    // CW_USEDEFAULT. A CW_USEDEFAULT window's first ShowWindow applies the
    // system's deferred placement (cascade position + CREATE-time size),
    // silently discarding any size set while hidden (measured: a 956x810
    // resize reverted to the 765x648 create size on first show, hiding the
    // OK/Apply buttons on scaled monitors).
    const UINT dpi0 = PrimaryMonitorDpi();
    RECT r{0, 0, ::MulDiv(kDesignW, static_cast<int>(dpi0), 96),
                ::MulDiv(kDesignH, static_cast<int>(dpi0), 96)};
    ::AdjustWindowRectEx(&r, style, FALSE, 0);
    const POINT pos =
        CenterOnPrimaryWorkArea(r.right - r.left, r.bottom - r.top);

    hwnd_ = ::CreateWindowExW(0, kSettingsClassName, L"AuraUI · 设置 — Ambient Desktop Monitor", style,
                              pos.x, pos.y, r.right - r.left,
                              r.bottom - r.top, nullptr, nullptr, hinst, this);
    if (!hwnd_) {
        log::Error(L"Settings: CreateWindowEx failed: " + LastErrorString());
        return false;
    }

    dpi_ = QueryDpi(hwnd_);
    ResizeToDesignDpi();  // CW_USEDEFAULT may land on a monitor with scaling:
                          // controls are placed S(dpi)-scaled but the window
                          // was created at raw design pixels - without this
                          // the bottom buttons (S(580)+S(30)) fall outside the
                          // unscaled client area (620px) on e.g. a 2K screen
                          // at 125%/150%.
    CreateControls();
    ApplyFontToChildren();
    PopulateMonitors();
    PopulateFonts();
    WriteControls();
    return true;
}

void SettingsWindow::Destroy() {
    if (hwnd_) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    controlHwnds_.clear();
    if (font_) {
        ::DeleteObject(font_);
        font_ = nullptr;
    }
}

void SettingsWindow::CreateControls() {
    controlHwnds_.clear();
    controlHwnds_.reserve(std::size(kControls));
    for (const ControlSpec& c : kControls) {
        HWND h = ::CreateWindowExW(
            0, c.cls, c.text ? c.text : L"", WS_CHILD | WS_VISIBLE | c.style, S(c.x),
            S(c.y), S(c.w), S(c.h), hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(c.id)), hinst_, nullptr);
        controlHwnds_.push_back(h);
    }
    if (HWND v = Item(ids::LblVersion))
        ::SetWindowTextW(v, (L"v" + VersionW()).c_str());
}

void SettingsWindow::RelayoutControls() {
    if (controlHwnds_.size() != std::size(kControls)) return;
    for (size_t i = 0; i < std::size(kControls); ++i) {
        const ControlSpec& c = kControls[i];
        if (controlHwnds_[i])
            ::MoveWindow(controlHwnds_[i], S(c.x), S(c.y), S(c.w), S(c.h), FALSE);
    }
}

void SettingsWindow::ResizeToDesignDpi(LONG moveX, LONG moveY) {
    if (!hwnd_) return;
    const DWORD style = static_cast<DWORD>(::GetWindowLongPtrW(hwnd_, GWL_STYLE));
    const DWORD ex    = static_cast<DWORD>(::GetWindowLongPtrW(hwnd_, GWL_EXSTYLE));
    RECT r{0, 0, ::MulDiv(kDesignW, static_cast<int>(dpi_), 96),
                ::MulDiv(kDesignH, static_cast<int>(dpi_), 96)};
    // Prefer the DPI-aware frame math (plain AdjustWindowRectEx sizes the
    // frame with 96-DPI metrics - the client area still comes out exact).
    using PFN_AdjustWindowRectExForDpi =
        BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static PFN_AdjustWindowRectExForDpi pfn =
        reinterpret_cast<PFN_AdjustWindowRectExForDpi>(reinterpret_cast<void*>(
            ::GetProcAddress(::GetModuleHandleW(L"user32.dll"),
                             "AdjustWindowRectExForDpi")));
    if (pfn)
        pfn(&r, style, FALSE, ex, dpi_);
    else
        ::AdjustWindowRectEx(&r, style, FALSE, ex);
    const bool move = moveX != LONG_MIN;
    ::SetWindowPos(hwnd_, nullptr, moveX, moveY, r.right - r.left,
                   r.bottom - r.top,
                   (move ? 0 : SWP_NOMOVE) | SWP_NOZORDER | SWP_NOACTIVATE);
}

HFONT SettingsWindow::CreateFontForDpi() const {
    return ::CreateFontW(-::MulDiv(9, static_cast<int>(dpi_), 72), 0, 0, 0, FW_NORMAL,
                         FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

void SettingsWindow::ApplyFontToChildren() {
    if (!hwnd_) return;
    if (!font_) font_ = CreateFontForDpi();
    if (font_) ::EnumChildWindows(hwnd_, &SetFontProc, reinterpret_cast<LPARAM>(font_));
}

void SettingsWindow::RebuildFont() {
    // Create the replacement FIRST: deleting the old font while children
    // still hold its handle would leave them painting with a dangling HFONT.
    HFONT fresh = CreateFontForDpi();
    if (!fresh) return;
    HFONT old = font_;
    font_      = fresh;
    if (hwnd_) ::EnumChildWindows(hwnd_, &SetFontProc, reinterpret_cast<LPARAM>(font_));
    if (old) ::DeleteObject(old);
}

void SettingsWindow::PopulateMonitors() {
    HWND combo = Item(ids::CmbMonitor);
    if (!combo) return;

    ::SendMessageW(combo, CB_RESETCONTENT, 0, 0);

    std::vector<std::pair<RECT, bool>> monitors;
    ::EnumDisplayMonitors(nullptr, nullptr, &MonitorEnumProc,
                          reinterpret_cast<LPARAM>(&monitors));

    for (size_t i = 0; i < monitors.size(); ++i) {
        const RECT& rc = monitors[i].first;
        // Built by concatenation on purpose: %s / %ls semantics differ between
        // MSVCRT and MinGW's ANSI stdio, so we avoid string format specifiers.
        std::wstring text = L"显示器 ";
        text += FmtInt(static_cast<long long>(i + 1));
        text += L"   ";
        text += FmtInt(rc.right - rc.left);
        text += L"x";
        text += FmtInt(rc.bottom - rc.top);
        text += L"   @ ";
        text += FmtInt(rc.left);
        text += L",";
        text += FmtInt(rc.top);
        if (monitors[i].second) text += L"   (主)";
        ::SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    if (monitors.empty()) {
        ::SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"主显示器"));
    }
}

// Fonts whose names cannot be represented in the system code page are hidden
// from the combo: the INI stores values as CP_ACP bytes (measured on Win10
// 19045: the W profile APIs do not honor a UTF-16 BOM, so ANSI is a hard
// constraint), and saving such a name would write '??' - the font then
// silently falls back after a restart.
bool EncodableInAcp(const wchar_t* s) {
    BOOL usedDefault = FALSE;
    const int n = ::WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, s, -1,
                                        nullptr, 0, nullptr, &usedDefault);
    return n > 1 && !usedDefault;
}

void SettingsWindow::PopulateFonts() {
    HWND fontCombo = Item(ids::CmbFont);
    if (fontCombo) {
        ::SendMessageW(fontCombo, CB_RESETCONTENT, 0, 0);
        for (const wchar_t* f : kFontChoices) {
            if (!EncodableInAcp(f)) continue;
            ::SendMessageW(fontCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(f));
        }
    }

    HWND sizeCombo = Item(ids::CmbFontSize);
    if (sizeCombo) {
        ::SendMessageW(sizeCombo, CB_RESETCONTENT, 0, 0);
        for (const wchar_t* s : kFontSizeChoices)
            ::SendMessageW(sizeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    }

    HWND refresh = Item(ids::CmbRefresh);
    if (refresh) {
        ::SendMessageW(refresh, CB_RESETCONTENT, 0, 0);
        for (const auto& opt : kRefreshOptions) {
            const LRESULT idx = ::SendMessageW(refresh, CB_ADDSTRING, 0,
                                               reinterpret_cast<LPARAM>(opt.text));
            if (idx >= 0)
                ::SendMessageW(refresh, CB_SETITEMDATA, static_cast<WPARAM>(idx),
                               static_cast<LPARAM>(opt.ms));
        }
    }
}

void SettingsWindow::WriteControls() {
    if (!hwnd_) return;

    auto setCheck = [&](int id, bool v) {
        HWND h = Item(id);
        if (h) ::SendMessageW(h, BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0);
    };
    auto setText = [&](int id, const std::wstring& v) {
        HWND h = Item(id);
        if (h) ::SetWindowTextW(h, v.c_str());
    };

    setCheck(ids::ChkAutoStart, draft_.autoStart);

    if (HWND h = Item(ids::CmbRefresh)) {
        // A hand-edited interval that matches no preset must not be silently
        // rewritten to 100 ms: leave the combo empty instead (CB_SETCURSEL -1
        // clears the selection; ReadControls keeps the draft value when
        // nothing is selected).
        int sel = -1;
        for (int i = 0; i < static_cast<int>(std::size(kRefreshOptions)); ++i) {
            if (kRefreshOptions[i].ms == draft_.refreshMs) {
                sel = i;
                break;
            }
        }
        ::SendMessageW(h, CB_SETCURSEL, static_cast<WPARAM>(sel), 0);
    }

    if (HWND h = Item(ids::CmbMonitor)) {
        const LRESULT count = ::SendMessageW(h, CB_GETCOUNT, 0, 0);
        int sel = draft_.monitorIndex;
        if (sel < 0 || sel >= static_cast<int>(count)) sel = 0;
        ::SendMessageW(h, CB_SETCURSEL, static_cast<WPARAM>(sel), 0);
    }

    setText(ids::EdtX, FmtInt(draft_.x));
    setText(ids::EdtY, FmtInt(draft_.y));
    setText(ids::EdtW, FmtInt(draft_.width));
    setText(ids::EdtH, FmtInt(draft_.height));

    if (HWND h = Item(ids::SldOpacity)) {
        ::SendMessageW(h, TBM_SETRANGE, TRUE, MAKELPARAM(0, 255));
        ::SendMessageW(h, TBM_SETPOS, TRUE, draft_.opacity);
    }

    if (HWND h = Item(ids::CmbFont)) ::SetWindowTextW(h, draft_.fontFamily.c_str());

    if (HWND h = Item(ids::CmbFontSize)) {
        wchar_t buf[16];
        ::swprintf(buf, 16, L"%d", static_cast<int>(draft_.fontSize + 0.5f));
        ::SetWindowTextW(h, buf);
    }

    setCheck(ids::ChkCpu, draft_.showCpu);
    setCheck(ids::ChkMemory, draft_.showMemory);
    setCheck(ids::ChkGpu, draft_.showGpu);
    setCheck(ids::ChkVram, draft_.showVram);
    setCheck(ids::ChkDisk, draft_.showDisk);
    setCheck(ids::ChkNetwork, draft_.showNetwork);
    setCheck(ids::ChkLanIp, draft_.showLanIp);

    setCheck(ids::ChkTitle, draft_.showTitle);
    setCheck(ids::ChkBars, draft_.showBars);

    if (HWND h = Item(ids::SldRadius)) {
        // 0..32 matches Config::Sanitize's design bound (the corner arc must
        // not reach the text column; see the Sanitize comment).
        ::SendMessageW(h, TBM_SETRANGE, TRUE, MAKELPARAM(0, 32));
        ::SendMessageW(h, TBM_SETPOS, TRUE, draft_.cornerRadius);
    }

    RefreshValueLabels();

    // NOTE: static controls do not wrap on '\', so a fully expanded path would
    // be hard-clipped. Show the %APPDATA% form (always short) and let the
    // "打开配置目录" button reveal the real location.
    const std::wstring info =
        L"配置目录\n"
        L"%APPDATA%\\AuraUI\\\n"
        L"    config.ini\n"
        L"    AuraUI.log\n"
        L"\n完整路径见“打开配置目录”。";
    if (HWND h = Item(ids::LblConfigPath)) ::SetWindowTextW(h, info.c_str());
}

void SettingsWindow::RefreshValueLabels() {
    if (HWND h = Item(ids::SldOpacity)) {
        const int pos = static_cast<int>(::SendMessageW(h, TBM_GETPOS, 0, 0));
        if (HWND lbl = Item(ids::LblOpacityValue))
            ::SetWindowTextW(lbl, (FmtInt(pos) + L" / 255").c_str());
    }
    if (HWND h = Item(ids::SldRadius)) {
        const int pos = static_cast<int>(::SendMessageW(h, TBM_GETPOS, 0, 0));
        if (HWND lbl = Item(ids::LblRadiusValue))
            ::SetWindowTextW(lbl, (FmtInt(pos) + L" px").c_str());
    }
}

void SettingsWindow::ReadControls() {
    if (!hwnd_) return;

    auto isChecked = [&](int id) {
        HWND h = Item(id);
        return h && ::SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
    };
    auto readInt = [&](int id, int fallback) {
        HWND h = Item(id);
        if (!h) return fallback;
        const std::wstring t = Trim(GetText(h));
        if (t.empty()) return fallback;
        wchar_t* end = nullptr;
        const long v = ::wcstol(t.c_str(), &end, 10);
        if (end == t.c_str()) return fallback;
        return static_cast<int>(v);
    };

    draft_.autoStart = isChecked(ids::ChkAutoStart);

    if (HWND h = Item(ids::CmbRefresh)) {
        const LRESULT sel = ::SendMessageW(h, CB_GETCURSEL, 0, 0);
        if (sel >= 0) {
            const LRESULT data = ::SendMessageW(h, CB_GETITEMDATA, static_cast<WPARAM>(sel), 0);
            if (data != CB_ERR && data > 0) draft_.refreshMs = static_cast<int>(data);
        }
    }

    if (HWND h = Item(ids::CmbMonitor)) {
        const LRESULT sel = ::SendMessageW(h, CB_GETCURSEL, 0, 0);
        if (sel >= 0) draft_.monitorIndex = static_cast<int>(sel);
    }

    draft_.x      = readInt(ids::EdtX, draft_.x);
    draft_.y      = readInt(ids::EdtY, draft_.y);
    draft_.width  = readInt(ids::EdtW, draft_.width);
    draft_.height = readInt(ids::EdtH, draft_.height);

    if (HWND h = Item(ids::SldOpacity))
        draft_.opacity = static_cast<int>(::SendMessageW(h, TBM_GETPOS, 0, 0));

    if (HWND h = Item(ids::CmbFont)) {
        const std::wstring t = Trim(GetText(h));
        if (!t.empty()) draft_.fontFamily = t;
    }

    if (HWND h = Item(ids::CmbFontSize)) {
        const std::wstring t = Trim(GetText(h));
        if (!t.empty()) {
            const int v = static_cast<int>(::wcstol(t.c_str(), nullptr, 10));
            if (v >= 6 && v <= 96) draft_.fontSize = static_cast<float>(v);
        }
    }

    draft_.showCpu     = isChecked(ids::ChkCpu);
    draft_.showMemory  = isChecked(ids::ChkMemory);
    draft_.showGpu     = isChecked(ids::ChkGpu);
    draft_.showVram    = isChecked(ids::ChkVram);
    draft_.showDisk    = isChecked(ids::ChkDisk);
    draft_.showNetwork = isChecked(ids::ChkNetwork);
    draft_.showLanIp   = isChecked(ids::ChkLanIp);

    draft_.showTitle = isChecked(ids::ChkTitle);
    draft_.showBars  = isChecked(ids::ChkBars);

    if (HWND h = Item(ids::SldRadius))
        draft_.cornerRadius = static_cast<int>(::SendMessageW(h, TBM_GETPOS, 0, 0));

    draft_.Sanitize();
}

void SettingsWindow::EmitLive() {
    if (onLive_) onLive_(draft_);
}

// Width guard: measure the minimum panel width the drafted settings need
// (same fonts + snapshot text the HUD renders) and raise the typed width to
// it with an explanation popup when it falls short. The renderer enforces the
// same floor at render time, so this popup is about making the silent
// auto-adjustment visible and deliberate at the moment the user sets it.
void SettingsWindow::EnsureMinWidth() {
    if (!hwnd_) return;

    const Snapshot snap = snapshotProvider_ ? snapshotProvider_() : Snapshot{};
    const float need =
        Hud::RequiredPanelWidth(draft_, snap, MonitorIndexDpi(draft_.monitorIndex));
    const int needPx = static_cast<int>(std::ceil(need));
    if (needPx <= draft_.width) return;

    draft_.width = needPx;
    draft_.Sanitize();
    if (HWND h = Item(ids::EdtW)) ::SetWindowTextW(h, FmtInt(draft_.width).c_str());

    const std::wstring msg =
        L"按当前字号与硬件名称计算，面板宽度至少需要 " + FmtInt(needPx) +
        L" px 才能完整显示文字，已自动调整宽度。\n\n"
        L"如需更窄的面板，可以减小字号或关闭部分显示项。";
    ::MessageBoxW(hwnd_, msg.c_str(), L"AuraUI", MB_OK | MB_ICONINFORMATION);
}

void SettingsWindow::EmitCommit() {
    if (onCommit_) onCommit_(draft_);
    original_ = draft_;
}

void SettingsWindow::Show(const Config& cfg) {
    if (!hwnd_) return;

    draft_    = cfg;
    original_ = cfg;

    const UINT dpi = QueryDpi(hwnd_);
    if (dpi != dpi_) {
        dpi_ = dpi;
        RebuildFont();
        RelayoutControls();
    }
    // Idempotent size assertion: heals any state where a stale suggested
    // rect (DPI-transition race) left the window scale inconsistent with
    // the controls.
    ResizeToDesignDpi();

    // The window is created once and reused for the process lifetime: re-read
    // the monitor list on every open, or a dock/undock (or resolution change)
    // since creation leaves stale entries - old resolution/position, or an
    // index that no longer matches a real display.
    PopulateMonitors();

    WriteControls();
    ::ShowWindow(hwnd_, SW_SHOWNORMAL);
    // A CW_USEDEFAULT window is placed at its CREATE-time size by the first
    // ShowWindow - sizes set while it was hidden are discarded (measured:
    // 956x810 set at Create/Show reverted to 765x648 the moment the window
    // was shown). Re-assert the DPI-scaled size after showing.
    ResizeToDesignDpi();
    ::SetForegroundWindow(hwnd_);
}

void SettingsWindow::Hide() {
    if (hwnd_) ::ShowWindow(hwnd_, SW_HIDE);
}

void SettingsWindow::Cancel() {
    draft_ = original_;
    WriteControls();
    EmitLive();
    Hide();
}

bool SettingsWindow::IsVisible() const {
    return hwnd_ && ::IsWindowVisible(hwnd_);
}

}  // namespace auraui
