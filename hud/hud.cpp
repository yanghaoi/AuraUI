#include "hud/hud.h"

#include "core/log.h"
#include "core/win_util.h"
#include "desktop/desktop.h"

#include <algorithm>
#include <cmath>

namespace auraui {

// Explicit IIDs: avoids any dependency on __uuidof / dxguid. extern (defined
// here, declared in hud.h): the settings window's width guard
// (RequiredPanelWidth) lives in hud_render.cpp and shares them.
extern const GUID kIID_ID2D1Factory = {0x06152247, 0x6f50, 0x465a,
                                       {0x92, 0x45, 0x11, 0x8b, 0xfd, 0x3b, 0x60, 0x07}};
extern const GUID kIID_IDWriteFactory = {0xb859ee5a, 0xd838, 0x4b5b,
                                         {0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48}};

namespace {

constexpr wchar_t kHudClassName[] = L"AuraUIHudWindow";

UINT QueryDpiForWindow(HWND hwnd) {
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
    return dpi ? dpi : 96;
}

// Index of the monitor with exactly this rect in EnumDisplayMonitors order -
// the same order Config::monitorIndex refers to. -1 when not found.
struct MonitorIndexCtx {
    RECT rc{};
    int  index  = 0;
    int  result = -1;
};

BOOL CALLBACK MonitorIndexProc(HMONITOR mon, HDC, LPRECT, LPARAM param) {
    auto* ctx = reinterpret_cast<MonitorIndexCtx*>(param);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (::GetMonitorInfoW(mon, &mi) && ::EqualRect(&mi.rcMonitor, &ctx->rc)) {
        ctx->result = ctx->index;
        return FALSE;
    }
    ++ctx->index;
    return TRUE;
}

int MonitorIndexFromRect(const RECT& rc) {
    MonitorIndexCtx ctx;
    ctx.rc = rc;
    ::EnumDisplayMonitors(nullptr, nullptr, &MonitorIndexProc,
                          reinterpret_cast<LPARAM>(&ctx));
    return ctx.result;
}

}  // namespace

Hud::Hud() = default;

Hud::~Hud() {
    // Stop the underlay worker before tearing anything down.
    {
        std::lock_guard<std::mutex> lock(underlayMtx_);
        underlayQuit_ = true;
    }
    underlayCv_.notify_all();
    if (underlayThread_.joinable()) underlayThread_.join();

    Destroy();
}

LRESULT CALLBACK Hud::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Hud* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<Hud*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<Hud*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) return self->WndProc(msg, wp, lp);
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Hud::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST:
            // Move mode: the whole surface acts as a caption so the native drag
            // loop (smooth move, Esc to cancel a single drag) moves the window.
            // Otherwise never swallow mouse input: the desktop keeps working.
            return moveMode_ ? HTCAPTION : HTTRANSPARENT;

        case WM_NCLBUTTONDBLCLK:
            if (moveMode_) {
                EndMove(true);  // double-click = drop it here
                return 0;
            }
            break;

        case WM_NCRBUTTONUP:
            if (moveMode_) {
                EndMove(true);  // right-click = drop it here
                return 0;
            }
            break;

        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = ::BeginPaint(hwnd_, &ps);
            if (dc) {
                if (!ulwOk_ && memDC_ && dib_) {
                    ::BitBlt(dc, 0, 0, pixelWidth_, pixelHeight_, memDC_, 0, 0, SRCCOPY);
                }
                ::EndPaint(hwnd_, &ps);
            }
            return 0;
        }

        case WM_DISPLAYCHANGE:
            OnDisplayChange();
            return 0;

        case WM_DPICHANGED:
            OnDisplayChange();
            return 0;

        case WM_SETTINGCHANGE:
            RequestRepaint();
            return 0;

        case WM_WINDOWPOSCHANGED: {
            auto* wp2 = reinterpret_cast<WINDOWPOS*>(lp);
            if (wp2 && (wp2->flags & SWP_NOSIZE) == 0) {
                // Keep our cached size in sync if the desktop resized us.
                pixelWidth_ = wp2->cx;
                pixelHeight_ = wp2->cy;
            }
            return ::DefWindowProcW(hwnd_, msg, wp, lp);
        }

        case WM_NCDESTROY: {
            HWND dying = hwnd_;
            ::SetWindowLongPtrW(dying, GWLP_USERDATA, 0);
            hwnd_ = nullptr;
            moveMode_ = false;  // the dragged window is gone; nothing left to finish
            return ::DefWindowProcW(dying, msg, wp, lp);
        }

        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, msg, wp, lp);
}

bool Hud::Create(HINSTANCE hinst, const Config& cfg) {
    hinst_ = hinst;
    cfg_   = cfg;
    moveMode_ = false;
    // Fresh window: no rectangle has been placed yet (the cache belongs to the
    // previous, now destroyed window - vacating "its" rect would refresh an
    // area it never occupied).
    placedX_ = INT_MAX;
    placedY_ = INT_MAX;
    placedW_ = -1;
    placedH_ = -1;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = 0;
    wc.lpfnWndProc   = &Hud::WndProcThunk;
    wc.hInstance     = hinst;
    wc.hCursor       = nullptr;
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kHudClassName;
    wc.hIcon = static_cast<HICON>(::LoadImageW(hinst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                               0, 0, LR_DEFAULTSIZE | LR_SHARED));
    wc.hIconSm = wc.hIcon;

    if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        log::Error(L"Hud: RegisterClassEx failed: " + LastErrorString());
        return false;
    }

    // Deliberately created WITHOUT WS_EX_LAYERED. A layered child window
    // (ULW or SetLayeredWindowAttributes mode) stops being composited after the
    // cross-process SetParent into Explorer's WorkerW on Win10 19045/RDP: every
    // property reads back healthy (WS_VISIBLE, valid SLWA state, successful
    // ULW) yet nothing reaches the screen, and a cleared layered bit can never
    // be re-added to a child window. A plain GDI child paints through the
    // normal redirection surface, which is immune to all of this - the whole
    // desktop-layer lifetime runs in that mode (ulwOk_=false below). Move mode
    // re-adds WS_EX_LAYERED after Detach(), when the window is a top-level
    // popup again and layering works reliably, for the per-pixel drag look.
    const DWORD exStyle = WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;

    hwnd_ = ::CreateWindowExW(exStyle, kHudClassName, L"AuraUI", WS_POPUP,
                              0, 0, std::max(120, cfg_.width), 240,
                              nullptr, nullptr, hinst, this);
    if (!hwnd_) {
        log::Error(L"Hud: CreateWindowEx failed: " + LastErrorString());
        return false;
    }

    // Never show up in Alt+Tab / the taskbar / the shell's window lists.
    ::ShowWindow(hwnd_, SW_HIDE);

    // Desktop-layer lifetime runs in plain GDI mode (see the exStyle comment):
    // opaque content painted via WM_PAINT into the normal redirection surface.
    ulwOk_      = false;
    opaqueFill_ = true;

    RefreshDpi();
    if (!CreateDeviceIndependentResources()) return false;

    // Stay hidden through the first render: until Attach() succeeds the
    // window is a floating top-level popup over the desktop, and Attach may
    // block up to seconds waiting for the shell to build the wallpaper
    // layer. visible_ is restored only once the window is safely inside the
    // desktop layer (or handed to the health check for retry).
    visible_ = false;
    Render();

    const bool attached = desktop::Attach(hwnd_) != FALSE;
    if (!attached) {
        log::Warn(L"Hud: initial desktop attach failed, will retry");
    }
    // Forget the placement bookkeeping from the unattached pass: the
    // coordinates change meaning once SetParent re-bases them on the host,
    // and a stale cache would misfire the vacated-rect wallpaper refresh.
    placedX_ = INT_MAX;
    placedY_ = INT_MAX;
    placedW_ = -1;
    placedH_ = -1;
    visible_ = cfg_.hudVisible;
    Render();  // place again now that we know our parent
    RefreshUnderlay();

    // An unattached window is a floating top-level popup over everything -
    // keep it hidden until the health check re-parents it into the desktop
    // layer (Reattach -> Render -> PlaceWindow shows it again).
    if (!visible_ || !attached) ::ShowWindow(hwnd_, SW_HIDE);

    log::Info(L"Hud created (" + FmtInt(pixelWidth_) + L"x" + FmtInt(pixelHeight_) + L")");
    return true;
}

void Hud::DiscardDeviceResources() {
    if (underlayBmp_) {
        underlayBmp_->Release();
        underlayBmp_ = nullptr;
    }
    if (dcTarget_) {
        dcTarget_->Release();
        dcTarget_ = nullptr;
    }
    if (oldBitmap_ && memDC_) {
        ::SelectObject(memDC_, oldBitmap_);
        oldBitmap_ = nullptr;
    }
    if (dib_) {
        ::DeleteObject(dib_);
        dib_ = nullptr;
        dibBits_ = nullptr;
    }
    if (memDC_) {
        ::DeleteDC(memDC_);
        memDC_ = nullptr;
    }
    dibWidth_ = dibHeight_ = 0;
}

void Hud::SetNotifyWindow(HWND w) {
    // Mutex-guarded: the worker thread reads this to post its results, and
    // App::Shutdown nulls it while that thread may be mid-decode.
    std::lock_guard<std::mutex> lock(underlayMtx_);
    notifyWnd_ = w;
}

void Hud::Destroy() {
    if (hwnd_) {
        RECT vacated{};
        const bool hadRect = ::GetWindowRect(hwnd_, &vacated) != FALSE;
        desktop::Detach(hwnd_);
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        // Erase the frame the window leaves on the desktop layer.
        if (hadRect) VacateRect(vacated);
    }
    DiscardDeviceResources();

    // Drop the underlay: the next Create() re-schedules it for the new window
    // (the crop depends on the panel rect, which may have changed).
    {
        std::lock_guard<std::mutex> lock(underlayMtx_);
        underlayReqFailed_ = false;
    }
    if (underlayBmp_) {
        underlayBmp_->Release();
        underlayBmp_ = nullptr;
    }
    underlayPix_.clear();
    underlayW_ = underlayH_ = 0;
    underlayKey_ = UnderlayKey{};

    auto releaseFmt = [](IDWriteTextFormat*& p) {
        if (p) {
            p->Release();
            p = nullptr;
        }
    };
    releaseFmt(fmtTitle_);
    releaseFmt(fmtLabel_);
    releaseFmt(fmtValue_);
    releaseFmt(fmtValueR_);
    releaseFmt(fmtSmall_);
    releaseFmt(fmtSmallR_);

    if (d2dFactory_) {
        d2dFactory_->Release();
        d2dFactory_ = nullptr;
    }
    if (dwriteFactory_) {
        dwriteFactory_->Release();
        dwriteFactory_ = nullptr;
    }
}

bool Hud::CreateDeviceIndependentResources() {
    if (!d2dFactory_) {
        const HRESULT hr = ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                               kIID_ID2D1Factory, nullptr,
                                               reinterpret_cast<void**>(&d2dFactory_));
        if (FAILED(hr)) {
            log::Error(L"Hud: D2D1CreateFactory failed: " + HresultString(hr));
            return false;
        }
    }

    if (!dwriteFactory_) {
        const HRESULT hr = ::DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, kIID_IDWriteFactory,
            reinterpret_cast<IUnknown**>(&dwriteFactory_));
        if (FAILED(hr)) {
            log::Error(L"Hud: DWriteCreateFactory failed: " + HresultString(hr));
            return false;
        }
    }

    RebuildTextFormats();
    return fmtLabel_ != nullptr;
}

void Hud::RebuildTextFormats() {
    if (!dwriteFactory_) return;

    auto release = [](IDWriteTextFormat*& p) {
        if (p) {
            p->Release();
            p = nullptr;
        }
    };
    release(fmtTitle_);
    release(fmtLabel_);
    release(fmtValue_);
    release(fmtValueR_);
    release(fmtSmall_);
    release(fmtSmallR_);

    const float s  = Scale();
    const float fs = std::max(6.0f, cfg_.fontSize * s);
    const std::wstring family = cfg_.fontFamily.empty() ? std::wstring(L"Segoe UI")
                                                        : cfg_.fontFamily;

    fmtTitle_  = MakeTextFormat(dwriteFactory_, family, fs * 1.10f,
                                DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
    fmtLabel_  = MakeTextFormat(dwriteFactory_, family, fs,
                                DWRITE_FONT_WEIGHT_NORMAL, false);
    fmtValue_  = MakeTextFormat(dwriteFactory_, family, fs,
                                DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
    fmtValueR_ = MakeTextFormat(dwriteFactory_, family, fs,
                                DWRITE_FONT_WEIGHT_SEMI_BOLD, true);
    fmtSmall_  = MakeTextFormat(dwriteFactory_, family, fs * 0.82f,
                                DWRITE_FONT_WEIGHT_NORMAL, false);
    fmtSmallR_ = MakeTextFormat(dwriteFactory_, family, fs * 0.82f,
                                DWRITE_FONT_WEIGHT_NORMAL, true);
}

bool Hud::CreateDeviceResources() {
    if (!d2dFactory_) return false;
    if (dcTarget_) return true;

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        0.0f, 0.0f, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);

    const HRESULT hr = d2dFactory_->CreateDCRenderTarget(&props, &dcTarget_);
    if (FAILED(hr) || !dcTarget_) {
        log::Error(L"Hud: CreateDCRenderTarget failed: " + HresultString(hr));
        dcTarget_ = nullptr;
        return false;
    }

    // ClearType needs an opaque backdrop; our surface is transparent.
    dcTarget_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dcTarget_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    return true;
}

bool Hud::EnsureBackBuffer(int w, int h) {
    if (w <= 0 || h <= 0) return false;

    if (!CreateDeviceResources()) return false;

    if (dib_ && memDC_ && dcTarget_ && w == dibWidth_ && h == dibHeight_) {
        return true;
    }

    // (Re)create the DIB + memory DC.
    if (oldBitmap_ && memDC_) {
        ::SelectObject(memDC_, oldBitmap_);
        oldBitmap_ = nullptr;
    }
    if (dib_) {
        ::DeleteObject(dib_);
        dib_ = nullptr;
        dibBits_ = nullptr;
    }
    if (!memDC_) memDC_ = ::CreateCompatibleDC(nullptr);
    if (!memDC_) {
        log::Error(L"Hud: CreateCompatibleDC failed");
        return false;
    }

    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;  // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    dib_ = ::CreateDIBSection(memDC_, &bi, DIB_RGB_COLORS, &dibBits_, nullptr, 0);
    if (!dib_ || !dibBits_) {
        log::Error(L"Hud: CreateDIBSection failed: " + LastErrorString());
        return false;
    }
    oldBitmap_ = ::SelectObject(memDC_, dib_);

    RECT r{0, 0, w, h};
    const HRESULT hr = dcTarget_->BindDC(memDC_, &r);
    if (FAILED(hr)) {
        log::Error(L"Hud: BindDC failed: " + HresultString(hr));
        return false;
    }

    dibWidth_  = w;
    dibHeight_ = h;
    return true;
}

void Hud::RefreshDpi() {
    const UINT dpi = QueryDpiForWindow(hwnd_);
    if (dpi == dpi_) return;
    dpi_ = dpi;
    RebuildTextFormats();
}

void Hud::ApplyConfig(const Config& cfg) {
    cfg_ = cfg;
    previewOpacity_ = -1;
    // The width floor depends on fonts, padding and the snapshot text; restart
    // the hysteresis from scratch instead of shrinking through the delay (or
    // keeping a floor that no longer applies).
    widthFloor_        = 0.0f;
    widthShrinkStreak_ = 0;

    if (!hwnd_) return;

    RefreshDpi();
    RebuildTextFormats();

    if (visible_ != cfg_.hudVisible) visible_ = cfg_.hudVisible;

    Render();
    RefreshUnderlay();
    if (visible_) {
        if (CanShowOnDesktop()) ::ShowWindow(hwnd_, SW_SHOWNA);
        else ::ShowWindow(hwnd_, SW_HIDE);  // unattached: stay hidden, not floating
    } else {
        RECT vacated{};
        const bool wasShown = hwnd_ && ::IsWindowVisible(hwnd_) != FALSE &&
                              ::GetWindowRect(hwnd_, &vacated) != FALSE;
        ::ShowWindow(hwnd_, SW_HIDE);
        if (wasShown) VacateRect(vacated);
    }
}

void Hud::SetSnapshot(const Snapshot& snap) {
    snapshot_ = snap;
}

void Hud::RequestRepaint() {
    if (!hwnd_) return;
    Render();
}

void Hud::SetVisible(bool visible) {
    const bool wasVisible = visible_;
    RECT vacated{};
    const bool hadRect = (!visible && wasVisible && hwnd_ &&
                          ::GetWindowRect(hwnd_, &vacated) != FALSE);
    visible_ = visible;
    if (!hwnd_) return;
    if (visible) {
        if (CanShowOnDesktop()) {
            ::ShowWindow(hwnd_, SW_SHOWNA);
            Render();
        }
        // Not attached: the health check shows the window once it re-parents
        // into the desktop layer - never leave it floating on top.
    } else {
        ::ShowWindow(hwnd_, SW_HIDE);
        if (hadRect) {
            // The frame the HUD leaves behind on the desktop layer must be
            // explicitly erased - the wallpaper WorkerW never repaints exposed
            // areas (see desktop::RefreshWallpaper).
            VacateRect(vacated);
        }
    }
}

void Hud::SetOpacityPreview(int opacity) {
    previewOpacity_ = std::clamp(opacity, 0, 255);
    if (hwnd_) Render();
}

bool Hud::Reattach(bool allowSpawn) {
    if (!hwnd_) return false;
    if (!desktop::Attach(hwnd_, allowSpawn)) return false;
    Render();
    RefreshUnderlay();
    return true;
}

void Hud::OnDisplayChange() {
    if (!hwnd_) return;
    RefreshDpi();
    RebuildTextFormats();
    Render();
    RefreshUnderlay();
}

void Hud::Reposition() {
    if (!hwnd_) return;
    PlaceWindow();
}

bool Hud::CanShowOnDesktop() const {
    return moveMode_ || (hwnd_ && desktop::IsAttached(hwnd_));
}

void Hud::VacateRect(const RECT& r) {
    if (onVacate_) onVacate_(&r);
    else desktop::RefreshWallpaper(&r);
}

void Hud::RefreshUnderlay() {
    if (!hwnd_ || moveMode_) return;
    ScheduleUnderlay();
}

void Hud::BeginMove() {
    if (!hwnd_ || moveMode_) return;

    // Screen rect of the window as it sits inside the desktop layer. After
    // Detach() the same position must be re-asserted as a top-level window:
    // SetParent() reinterprets coordinates against the new parent, so without
    // this the HUD would jump.
    RECT r{};
    const bool haveRect = ::GetWindowRect(hwnd_, &r) != FALSE;

    desktop::Detach(hwnd_);  // child of WorkerW -> WS_POPUP top-level again
    if (haveRect) VacateRect(r);  // erase the vacated frame

    // Back on the top-level band the standard layered pipeline works again, so
    // the drag surface gets the full per-pixel treatment. Re-adding
    // WS_EX_LAYERED here is the safe direction: it is only ever unreliable on a
    // *child* window, and the drop path recreates the window anyway.
    const LONG_PTR ex = ::GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    ::SetWindowLongPtrW(hwnd_, GWL_EXSTYLE,
                        ((ex & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT)) &
                         ~static_cast<LONG_PTR>(WS_EX_NOACTIVATE)) |
                        WS_EX_LAYERED | WS_EX_TOPMOST);
    ulwOk_      = true;
    opaqueFill_ = false;

    moveMode_  = true;
    visible_   = true;  // App::BeginHudMove persists cfg.hudVisible=true before
                       // calling this, so the post-move rebuild shows the HUD

    Render();  // re-lays out with the hint band; deliberately skips PlaceWindow

    if (haveRect)
        ::SetWindowPos(hwnd_, HWND_TOPMOST, r.left, r.top, pixelWidth_, pixelHeight_,
                       SWP_NOACTIVATE | SWP_SHOWWINDOW);
    else
        ::ShowWindow(hwnd_, SW_SHOWNA);

    log::Info(L"Hud: move mode on");
}

void Hud::EndMove(bool applyPosition) {
    if (!moveMode_ || !hwnd_) return;
    moveMode_ = false;

    // Final position, as monitor-relative physical pixels (the Config::x/y
    // convention), before any style/parent change invalidates it.
    int x = -1, y = -1, monIdx = -1;
    if (applyPosition) {
        RECT r{};
        if (::GetWindowRect(hwnd_, &r)) {
            const HMONITOR mon =
                ::MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (mon && ::GetMonitorInfoW(mon, &mi)) {
                x      = r.left - mi.rcMonitor.left;
                y      = r.top - mi.rcMonitor.top;
                monIdx = MonitorIndexFromRect(mi.rcMonitor);
            }
        }
    }

    // Back to the passive desktop-layer look; re-parenting is the caller's job
    // (the onMoveFinished handler) since it also updates the stored config.
    const LONG_PTR ex = ::GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    ::SetWindowLongPtrW(hwnd_, GWL_EXSTYLE,
                        (ex & ~static_cast<LONG_PTR>(WS_EX_TOPMOST)) |
                            WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
    // WS_EX_TOPMOST is owned by the window manager: a direct style edit is not
    // honored, it must be removed through SetWindowPos(HWND_NOTOPMOST).
    ::SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    log::Info(L"Hud: move mode off (apply=" + std::wstring(applyPosition ? L"yes" : L"no") +
              L", pos=" + FmtInt(x) + L"," + FmtInt(y) + L", monitor=" + FmtInt(monIdx) +
              L")");

    if (onMoveFinished_) onMoveFinished_(applyPosition, x, y, monIdx);
}

}  // namespace auraui
