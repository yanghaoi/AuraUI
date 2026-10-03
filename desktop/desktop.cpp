#include "desktop/desktop.h"

#include "core/log.h"
#include "core/win_util.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <vector>
#include <shobjidl.h>   // IDesktopWallpaper
#include <wincodec.h>   // WIC (PaintWallpaperRect ghost cover)

namespace auraui::desktop {
namespace {

// Undocumented but stable since Windows 7: asking Progman to build the wallpaper
// layer (a WorkerW that sits between the wallpaper and the icons).
//
// The parameters matter! `0x052C` with wParam == 0xD makes the shell actually
// create/show the wallpaper WorkerW and move SHELLDLL_DefView into an icon-host
// WorkerW. Sending (0, 0) - as many samples do - is a silent no-op: nothing is
// created, the wallpaper layer stays hidden, and a window parented to it is
// invisible. Verified on Windows 10 19045: before the call there is one hidden
// full-screen WorkerW; after it there are two visible ones.
constexpr UINT   kSpawnWorkerW      = 0x052C;
constexpr WPARAM kSpawnWorkerWParam = 0x0D;
constexpr LPARAM kSpawnWorkerWLParm = 0x1;

// Explorer being down is a normal, temporary state (we retry every 2s), so the
// "no host" warning is rate limited to keep the log readable.
bool ShouldWarnNoHost() {
    static unsigned long long last = 0;
    const unsigned long long now = ::GetTickCount64();
    if (last != 0 && now - last < 60000ull) return false;
    last = now;
    return true;
}

std::wstring ClassNameOf(HWND hwnd) {
    wchar_t buf[128]{};
    if (!hwnd) return L"(null)";
    const int n = ::GetClassNameW(hwnd, buf, 128);
    return std::wstring(buf, static_cast<size_t>(n > 0 ? n : 0));
}

// Monitor rect in EnumDisplayMonitors order (the same order the rest of the
// project uses for Config::monitorIndex). Falls back to the primary.
struct MonRectCtx {
    int  index = 0;
    int  want  = 0;
    RECT rc{};
    bool found = false;
    RECT primary{};
    bool havePrimary = false;
};

BOOL CALLBACK MonRectProc(HMONITOR mon, HDC, LPRECT, LPARAM param) {
    auto* ctx = reinterpret_cast<MonRectCtx*>(param);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!::GetMonitorInfoW(mon, &mi)) return TRUE;
    if (ctx->index == ctx->want) {
        ctx->rc    = mi.rcMonitor;
        ctx->found = true;
        return FALSE;
    }
    if (mi.dwFlags & MONITORINFOF_PRIMARY) {
        ctx->primary     = mi.rcMonitor;
        ctx->havePrimary = true;
    }
    ++ctx->index;
    return TRUE;
}

bool MonitorRectForIndex(int index, RECT* out) {
    if (!out) return false;
    MonRectCtx ctx;
    ctx.want = index;
    ::EnumDisplayMonitors(nullptr, nullptr, &MonRectProc, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.found) {
        *out = ctx.rc;
        return true;
    }
    if (ctx.havePrimary) {
        *out = ctx.primary;
        return true;
    }
    return false;
}

// Index of the monitor containing the rect's center (EnumDisplayMonitors
// order, the Config::monitorIndex convention); nearest monitor as fallback.
// The ghost-cover must resolve the wallpaper for the monitor the vacated rect
// actually sits on - per-monitor wallpaper setups differ.
struct MonIndexCtx {
    POINT pt{};
    int   idx      = 0;
    int   best     = 0;
    int   bestDist = INT_MAX;
};

BOOL CALLBACK MonIndexProc(HMONITOR mon, HDC, LPRECT, LPARAM param) {
    auto* ctx = reinterpret_cast<MonIndexCtx*>(param);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!::GetMonitorInfoW(mon, &mi)) return TRUE;
    const RECT& r = mi.rcMonitor;
    int dx = 0, dy = 0;
    if (ctx->pt.x < r.left) dx = r.left - ctx->pt.x;
    else if (ctx->pt.x > r.right) dx = ctx->pt.x - r.right;
    if (ctx->pt.y < r.top) dy = r.top - ctx->pt.y;
    else if (ctx->pt.y > r.bottom) dy = ctx->pt.y - r.bottom;
    const int dist = dx + dy;
    if (dist == 0) {  // center inside this monitor
        ctx->best     = ctx->idx;
        ctx->bestDist = 0;
        return FALSE;
    }
    if (dist < ctx->bestDist) {
        ctx->bestDist = dist;
        ctx->best     = ctx->idx;
    }
    ++ctx->idx;
    return TRUE;
}

int MonitorIndexFromRect(const RECT& rc) {
    MonIndexCtx ctx;
    ctx.pt.x = (rc.left + rc.right) / 2;
    ctx.pt.y = (rc.top + rc.bottom) / 2;
    ::EnumDisplayMonitors(nullptr, nullptr, &MonIndexProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}

// Process that owns Progman, i.e. Explorer. 0 when Explorer is not running.
DWORD DesktopProcessId() {
    HWND progman = ::FindWindowW(L"Progman", nullptr);
    if (!progman) return 0;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(progman, &pid);
    return pid;
}

// Window classes are registered per process, so ANY application can create a
// window whose class name is "WorkerW" - and FindWindowEx() matches purely on
// the name string. Observed in the wild: WorkBuddyAI.exe, Everything.exe and
// RuntimeBroker.exe all own hidden 136x39 top-level "WorkerW" windows.
// Parenting to one of those makes the HUD invisible, so every candidate must
// belong to Explorer.
bool BelongsToDesktop(HWND w, DWORD desktopPid) {
    if (!w) return false;
    if (desktopPid == 0) return true;  // Explorer unknown: cannot filter
    DWORD pid = 0;
    ::GetWindowThreadProcessId(w, &pid);
    return pid == desktopPid;
}

// The wallpaper layer: a full-screen, visible, Explorer-owned WorkerW that does
// NOT host SHELLDLL_DefView.
//
// Where it lives depends on the shell build, so both places are searched:
//   * Windows 10 ~ 11 23H2   -> a top-level WorkerW behind the icon host
//   * Windows 11 24H2+ / after 0x052C on Win10 -> a direct child of Progman
//
// Parenting to Progman itself is NOT a workaround: SHELLDLL_DefView paints its
// background from the parent's buffer, so a sibling placed below it is covered
// (and placing it above would cover the icons).
HWND FindWallpaperLayer(HWND progman, DWORD desktopPid) {
    const int screenW = ::GetSystemMetrics(SM_CXSCREEN);
    const int screenH = ::GetSystemMetrics(SM_CYSCREEN);

    HWND fromProgman = nullptr;
    HWND topLevel    = nullptr;

    auto consider = [&](HWND w, HWND* slot) {
        if (*slot || !w || !::IsWindow(w)) return;
        if (!::IsWindowVisible(w)) return;             // hidden host => invisible child
        if (!BelongsToDesktop(w, desktopPid)) return;  // skip third-party "WorkerW"
        RECT r{};
        if (!::GetWindowRect(w, &r)) return;
        if (r.right - r.left < screenW / 2 || r.bottom - r.top < screenH / 2) return;
        if (::FindWindowExW(w, nullptr, L"SHELLDLL_DefView", nullptr)) return;  // icon host
        *slot = w;
    };

    if (progman) {
        for (HWND w = ::FindWindowExW(progman, nullptr, L"WorkerW", nullptr); w;
             w = ::FindWindowExW(progman, w, L"WorkerW", nullptr)) {
            consider(w, &fromProgman);
        }
    }
    for (HWND w = ::FindWindowExW(nullptr, nullptr, L"WorkerW", nullptr); w;
         w = ::FindWindowExW(nullptr, w, L"WorkerW", nullptr)) {
        consider(w, &topLevel);
    }

    return fromProgman ? fromProgman : topLevel;
}

// Top-level window that hosts SHELLDLL_DefView (the icon layer).
struct HostSearch {
    HWND iconHost = nullptr;
};

BOOL CALLBACK EnumFindHosts(HWND top, LPARAM param) {
    auto* ctx = reinterpret_cast<HostSearch*>(param);
    if (!::FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr)) return TRUE;
    if (!ctx->iconHost) ctx->iconHost = top;
    if (::IsWindowVisible(top)) {
        ctx->iconHost = top;
        return FALSE;
    }
    return TRUE;
}

HWND FindDefViewHost() {
    HWND progman = ::FindWindowW(L"Progman", nullptr);
    if (progman && ::FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr))
        return progman;

    HostSearch ctx;
    ::EnumWindows(&EnumFindHosts, reinterpret_cast<LPARAM>(&ctx));
    return ctx.iconHost;
}

// Make the window a *real* child window before re-parenting it.
//
// MSDN (SetParent): "if hWndNewParent is not NULL, you should also clear the
// WS_POPUP bit and set the WS_CHILD style before calling SetParent."
// Skipping this leaves the window in a half-parented state where GetParent()
// returns NULL, which in turn breaks z-order handling and coordinate mapping.
void MakeChildStyle(HWND hwnd) {
    const LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
    if ((style & WS_CHILD) && !(style & WS_POPUP)) return;
    ::SetWindowLongPtrW(hwnd, GWL_STYLE, (style & ~static_cast<LONG_PTR>(WS_POPUP)) | WS_CHILD);
}

void MakePopupStyle(HWND hwnd) {
    const LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
    if ((style & WS_POPUP) && !(style & WS_CHILD)) return;
    ::SetWindowLongPtrW(hwnd, GWL_STYLE, (style & ~static_cast<LONG_PTR>(WS_CHILD)) | WS_POPUP);
}

}  // namespace

UINT TaskbarCreatedMessage() {
    static const UINT msg = ::RegisterWindowMessageW(L"TaskbarCreated");
    return msg;
}

HWND FindIconHost() { return FindDefViewHost(); }

HWND FindIconView() {
    HWND host = FindDefViewHost();
    if (!host) return nullptr;
    return ::FindWindowExW(host, nullptr, L"SHELLDLL_DefView", nullptr);
}

HWND FindWallpaperHost(bool allowSpawn) {
    HWND progman = ::FindWindowW(L"Progman", nullptr);
    const DWORD desktopPid = DesktopProcessId();

    // Find, and if nothing suitable exists, nudge Progman and look again. The
    // nudge is the only way to get a wallpaper layer on systems where the shell
    // has not built one yet (it is created lazily). Timer-driven callers pass
    // allowSpawn=false so a health check never churns the shell.
    const int attempts = allowSpawn ? 5 : 1;
    for (int i = 0; i < attempts; ++i) {
        if (HWND host = FindWallpaperLayer(progman, desktopPid)) return host;

        if (!allowSpawn || !progman) break;

        // Rate-limit the nudge: it makes the shell build an extra WorkerW
        // and each attempt can block for the full timeout. A retry loop
        // (the health check fires every 2s) must not hammer a half-dead
        // shell - one spawn attempt per 30s at most.
        static ULONGLONG lastSpawnTick = 0;
        const ULONGLONG now            = ::GetTickCount64();
        if (lastSpawnTick != 0 && now - lastSpawnTick < 30000ull) break;
        lastSpawnTick = now;

        DWORD_PTR ignored = 0;
        // ABORTIFHUNG: return immediately when Progman is not responding
        // instead of waiting out the timeout on a hung shell.
        ::SendMessageTimeoutW(progman, kSpawnWorkerW, kSpawnWorkerWParam, kSpawnWorkerWLParm,
                              SMTO_ABORTIFHUNG, 1000, &ignored);
    }

    if (ShouldWarnNoHost()) {
        log::Warn(std::wstring(L"desktop::FindWallpaperHost: no wallpaper layer found (Progman=") +
                  (progman ? L"present" : L"missing") + L")");
    }
    return nullptr;
}

HWND ZOrderAnchor(HWND parent) {
    if (!parent) return nullptr;
    HWND iconView = ::FindWindowExW(parent, nullptr, L"SHELLDLL_DefView", nullptr);
    return iconView ? iconView : HWND_BOTTOM;
}

bool EnsureZOrder(HWND hwnd) {
    if (!hwnd || !::IsWindow(hwnd)) return false;

    HWND parent = ::GetParent(hwnd);
    if (!parent) return false;

    // The canonical placement: insert the window directly *after* the icon view
    // in the parent's child z-order, i.e. immediately below the desktop icons.
    // HWND_BOTTOM is used when this host has no SHELLDLL_DefView of its own -
    // the normal case for the wallpaper layer, because the icons live in a
    // different window (the icon host) above us.
    HWND anchor   = ZOrderAnchor(parent);
    HWND iconView = (anchor != HWND_BOTTOM) ? anchor : nullptr;

    if (iconView) {
        if (::GetWindow(hwnd, GW_HWNDPREV) == iconView) return true;  // already correct
    } else {
        if (::GetWindow(hwnd, GW_HWNDLAST) == hwnd) return true;      // already bottom-most
    }

    if (!::SetWindowPos(hwnd, anchor, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)) {
        log::Warn(L"desktop::EnsureZOrder: SetWindowPos failed: " + LastErrorString());
        return false;
    }
    return true;
}

bool Attach(HWND hwnd, bool allowSpawn) {
    if (!hwnd || !::IsWindow(hwnd)) return false;

    HWND host = FindWallpaperHost(allowSpawn);
    if (!host) return false;

    if (::GetParent(hwnd) != host) {
        const LONG_PTR oldStyle = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
        MakeChildStyle(hwnd);
        if (!::SetParent(hwnd, host)) {
            log::Warn(L"desktop::Attach: SetParent failed: " + LastErrorString());
            // Roll back so the window does not end up a child without a parent.
            ::SetWindowLongPtrW(hwnd, GWL_STYLE, oldStyle);
            return false;
        }
    }

    return EnsureZOrder(hwnd);
}

void Detach(HWND hwnd) {
    if (!hwnd || !::IsWindow(hwnd)) return;
    if (!::GetParent(hwnd)) return;
    ::SetParent(hwnd, nullptr);
    MakePopupStyle(hwnd);
}

bool IsAttached(HWND hwnd) {
    if (!hwnd || !::IsWindow(hwnd)) return false;

    HWND parent = ::GetParent(hwnd);
    if (!parent || !::IsWindow(parent)) return false;

    // A hidden parent makes the HUD invisible even though its own WS_VISIBLE is
    // set - IsWindowVisible() of a child depends on the whole ancestor chain.
    if (!::IsWindowVisible(parent)) return false;

    const std::wstring cls = ClassNameOf(parent);
    if (cls != L"WorkerW" && cls != L"Progman") return false;

    // The desktop layout can change under us at runtime (Win+D migrates
    // SHELLDLL_DefView between hosts, the wallpaper layer can be rebuilt), so
    // the real question is not "is the parent a desktop window" but "is it
    // still the host we would pick right now".
    const HWND want = FindWallpaperHost(/*allowSpawn=*/false);
    if (want && want != parent) return false;

    return true;
}

// Decode the wallpaper region that shows at `rect_screen` (screen coords)
// into a top-down 32bpp BGRA buffer (w x h). Placement math mirrors
// Hud::DecodeUnderlayCrop - keep the two in sync. The wallpaper is resolved
// for the monitor the rect sits on (per-monitor wallpaper setups differ).
// Returns false when there is no decodable placement (solid / Spotlight /
// TILE / SPAN).
bool DecodeRegionToBGRA(const RECT& rect_screen, int w, int h, void* out) {
    const int monIdx = MonitorIndexFromRect(rect_screen);
    const WallpaperInfo wi = CurrentWallpaper(monIdx);
    if (wi.path.empty() || wi.style == 1 || wi.style == 5) return false;

    RECT mrcBox{};
    if (!MonitorRectForIndex(monIdx, &mrcBox)) return false;
    const RECT& mrc = mrcBox;
    const float mw = static_cast<float>(mrc.right - mrc.left);
    const float mh = static_cast<float>(mrc.bottom - mrc.top);

    IWICImagingFactory* factory = nullptr;
    HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IWICImagingFactory,
                                    reinterpret_cast<void**>(&factory));
    IWICBitmapDecoder* decoder = nullptr;
    if (SUCCEEDED(hr))
        hr = factory->CreateDecoderFromFilename(wi.path.c_str(), nullptr, GENERIC_READ,
                                                WICDecodeMetadataCacheOnDemand, &decoder);
    IWICBitmapFrameDecode* frame = nullptr;
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    UINT iw = 0, ih = 0;
    if (SUCCEEDED(hr)) hr = frame->GetSize(&iw, &ih);
    // Same bound as Hud::DecodeUnderlayCrop (the underlay mirror of this
    // function): a corrupt / absurdly large wallpaper file must fail the
    // decode instead of allocating dstW * dstH * 4 bytes from bogus
    // dimensions downstream.
    if (SUCCEEDED(hr) && (iw == 0 || ih == 0 || iw * ih > 8192u * 8192u))
        hr = E_INVALIDARG;

    float srcL = 0, srcT = 0, srcR = 0, srcB = 0;
    float dstL = 0, dstT = 0, dstR = 0, dstB = 0;
    if (SUCCEEDED(hr)) {
        const float fiw = static_cast<float>(iw);
        const float fih = static_cast<float>(ih);
        float drawX = 0, drawY = 0, drawW = mw, drawH = mh;
        float sx = 1.0f, sy = 1.0f;
        switch (wi.style) {
            case 2: case 5:
                sx = mw / fiw; sy = mh / fih;
                break;
            case 3: {
                const float sc = std::min(mw / fiw, mh / fih);
                sx = sy = sc; drawW = fiw * sc; drawH = fih * sc;
                drawX = (mw - drawW) / 2.0f; drawY = (mh - drawH) / 2.0f;
                break;
            }
            case 0:
                drawW = fiw; drawH = fih;
                drawX = (mw - fiw) / 2.0f; drawY = (mh - fih) / 2.0f;
                break;
            default: {
                const float sc = std::max(mw / fiw, mh / fih);
                sx = sy = sc; drawW = fiw * sc; drawH = fih * sc;
                drawX = (mw - drawW) / 2.0f; drawY = (mh - drawH) / 2.0f;
                break;
            }
        }
        srcL = (rect_screen.left - mrc.left - drawX) / sx;
        srcT = (rect_screen.top - mrc.top - drawY) / sy;
        srcR = (rect_screen.right - mrc.left - drawX) / sx;
        srcB = (rect_screen.bottom - mrc.top - drawY) / sy;
        dstL = 0; dstT = 0; dstR = static_cast<float>(w); dstB = static_cast<float>(h);
        if (srcL < 0.0f) { dstL -= srcL * sx; srcL = 0.0f; }
        if (srcT < 0.0f) { dstT -= srcT * sy; srcT = 0.0f; }
        if (srcR > fiw) { dstR -= (srcR - fiw) * sx; srcR = fiw; }
        if (srcB > fih) { dstB -= (srcB - fih) * sy; srcB = fih; }
    }

    // Background fill + clipped/scaled image region.
    if (SUCCEEDED(hr)) {
        const unsigned b = (wi.bg & 0xFF);
        const unsigned g = (wi.bg >> 8) & 0xFF;
        const unsigned r = (wi.bg >> 16) & 0xFF;
        const std::uint8_t bgPix[4] = {static_cast<std::uint8_t>(b),
                                       static_cast<std::uint8_t>(g),
                                       static_cast<std::uint8_t>(r), 0xFF};
        auto* dst = static_cast<std::uint8_t*>(out);
        for (int i = 0; i < w * h; ++i) std::memcpy(dst + i * 4, bgPix, 4);

        if (srcR > srcL && srcB > srcT) {
            IWICBitmapClipper* clipper = nullptr;
            if (SUCCEEDED(hr)) hr = factory->CreateBitmapClipper(&clipper);
            WICRect wr{};
            wr.X      = static_cast<INT>(srcL);
            wr.Y      = static_cast<INT>(srcT);
            wr.Width  = static_cast<INT>(srcR) - wr.X;
            wr.Height = static_cast<INT>(srcB) - wr.Y;
            if (SUCCEEDED(hr)) hr = clipper->Initialize(frame, &wr);

            IWICBitmapScaler* scaler = nullptr;
            if (SUCCEEDED(hr)) hr = factory->CreateBitmapScaler(&scaler);
            const UINT dstW = static_cast<UINT>(dstR - dstL);
            const UINT dstH = static_cast<UINT>(dstB - dstT);
            if (SUCCEEDED(hr) && (dstW == 0 || dstH == 0)) hr = E_INVALIDARG;
            if (SUCCEEDED(hr))
                hr = scaler->Initialize(clipper, dstW, dstH, WICBitmapInterpolationModeFant);

            IWICFormatConverter* conv = nullptr;
            if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&conv);
            if (SUCCEEDED(hr))
                hr = conv->Initialize(scaler, GUID_WICPixelFormat32bppPBGRA,
                                      WICBitmapDitherTypeNone, nullptr, 0.0,
                                      WICBitmapPaletteTypeCustom);
            if (SUCCEEDED(hr)) {
                // CopyPixels strides its output by cbStride while the target
                // buffer's real row stride is w*4 - the two differ whenever
                // the letterbox clamp shrank the region (rect hanging off
                // the monitor/image). Copy into a contiguous scratch buffer
                // first, then blit row by row, or every row after the first
                // shifts and the region skews diagonally ("smear").
                std::vector<std::uint8_t> scratch(
                    static_cast<size_t>(dstW) * dstH * 4);
                hr = conv->CopyPixels(nullptr, dstW * 4,
                                      static_cast<UINT>(scratch.size()),
                                      scratch.data());
                if (SUCCEEDED(hr)) {
                    auto* dst = static_cast<std::uint8_t*>(out) +
                                (static_cast<size_t>(dstT) * w +
                                 static_cast<size_t>(dstL)) * 4;
                    for (UINT row = 0; row < dstH; ++row) {
                        std::memcpy(dst + static_cast<size_t>(row) * w * 4,
                                    scratch.data() +
                                        static_cast<size_t>(row) * dstW * 4,
                                    static_cast<size_t>(dstW) * 4);
                    }
                }
            }
            if (conv) conv->Release();
            if (scaler) scaler->Release();
            if (clipper) clipper->Release();
        }
    }

    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
    return SUCCEEDED(hr);
}

// Cover the ghost a vacated child leaves on the wallpaper WorkerW by painting
// the correct wallpaper pixels DIRECTLY onto the host's surface. The WorkerW
// never repaints exposed areas itself, and the surgical RedrawWindow pass
// does not clear the frozen frame (measured). Painting beats the historical
// SPI replay, which makes the shell churn its wallpaper caches for up to 20s
// (visible as a transient "translucent edges" window). Returns false when the
// pixels could not be produced.
bool PaintWallpaperRect(HWND host, const RECT& rect_host) {
    if (!host || ::IsRectEmpty(&rect_host)) return false;
    const int w = rect_host.right - rect_host.left;
    const int h = rect_host.bottom - rect_host.top;

    RECT rect_screen = rect_host;
    ::MapWindowPoints(host, nullptr, reinterpret_cast<POINT*>(&rect_screen), 2);

    // Resolve the wallpaper for the monitor the rect is on (not implicitly
    // monitor 0) - per-monitor wallpaper setups would otherwise paint the
    // wrong image over the ghost, "successfully" bypassing the replay
    // fallback.
    const WallpaperInfo wi = CurrentWallpaper(MonitorIndexFromRect(rect_screen));
    const bool solid = wi.path.empty() || wi.style == 1 || wi.style == 5;

    HDC hdc = ::GetDC(host);
    if (!hdc) return false;
    bool ok = false;
    HDC mem     = ::CreateCompatibleDC(hdc);
    void* bits  = nullptr;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;  // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HBITMAP dib = ::CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (mem && dib && bits) {
        bool filled = false;
        if (solid) {
            const std::uint8_t bgPix[4] = {static_cast<std::uint8_t>(wi.bg & 0xFF),
                                           static_cast<std::uint8_t>((wi.bg >> 8) & 0xFF),
                                           static_cast<std::uint8_t>((wi.bg >> 16) & 0xFF), 0xFF};
            auto* dst = static_cast<std::uint8_t*>(bits);
            for (int i = 0; i < w * h; ++i) std::memcpy(dst + i * 4, bgPix, 4);
            filled = true;
        } else {
            filled = DecodeRegionToBGRA(rect_screen, w, h, bits);
        }
        if (filled) {
            HGDIOBJ old = ::SelectObject(mem, dib);
            ok = ::BitBlt(hdc, rect_host.left, rect_host.top, w, h, mem, 0, 0,
                          SRCCOPY) != FALSE;
            ::SelectObject(mem, old);
        }
    }
    if (dib) ::DeleteObject(dib);
    if (mem) ::DeleteDC(mem);
    ::ReleaseDC(host, hdc);
    if (ok) {
        using PFN_DwmFlush = HRESULT(WINAPI*)();
        if (HMODULE dwm = ::GetModuleHandleW(L"dwmapi.dll")) {
            auto pfn = reinterpret_cast<PFN_DwmFlush>(
                reinterpret_cast<void*>(::GetProcAddress(dwm, "DwmFlush")));
            if (pfn) pfn();
        }
    }
    return ok;
}

bool RefreshWallpaper(const RECT* vacated, bool replay) {
    // 1) Paint-over: cover the ghost directly on the wallpaper WorkerW's
    //    surface with the correct wallpaper pixels. No shell involvement, no
    //    cache churn - the SPI replay below is what used to cause the
    //    transient "translucent edges" window after every HUD move.
    bool painted = false;
    if (vacated && !::IsRectEmpty(vacated)) {
        if (HWND host = FindWallpaperHost(/*allowSpawn=*/false)) {
            RECT rc = *vacated;
            ::MapWindowPoints(nullptr, host, reinterpret_cast<POINT*>(&rc), 2);
            painted = PaintWallpaperRect(host, rc);
            if (!painted) {
                // Legacy surgical invalidation - insufficient on its own
                // (measured: the WorkerW's frozen frame survives it), kept as
                // a nudge alongside the replay fallback.
                ::RedrawWindow(host, &rc, nullptr,
                               RDW_INVALIDATE | RDW_ERASE | RDW_FRAME |
                                   RDW_NOINTERNALPAINT);
            }
        }
    }

    // 2) Fallback: re-apply the current wallpaper BY ITS REAL PATH so the
    //    shell rebuilds the desktop layer. Only reached when paint-over
    //    failed (no decodable wallpaper, host not found) or for the
    //    desktop-wide refresh (vacated == nullptr, session reconnect), where
    //    RDP-stale-frame clearing genuinely needs it. Costs a 6-20s shell
    //    cache churn - the caller is expected to put the underlay into
    //    settle-follow mode when this returns true.
    //    SPI with a NULL path can turn the desktop pure black on some setups
    //    (observed when hiding the HUD), so the actual image path is replayed.
    //    SKIPPED on the Win11 24H2+ "raised desktop" (Progman created with
    //    WS_EX_NOREDIRECTIONBITMAP): re-applying the wallpaper there destroys
    //    the current wallpaper WorkerW outright (Lively gates the identical
    //    call for exactly this reason).
    bool replayed = false;
    if (replay && !painted && !IsRaisedDesktop()) {
        const WallpaperInfo wi = CurrentWallpaper();
        // Replay by the ORIGINAL path, never the display cache tier
        // (wi.path): re-setting the wallpaper from a cache file makes the
        // shell re-encode an already-re-encoded image, degrading the desktop
        // one JPEG generation per replay (measured: CachedImage shrank
        // 296033 -> 292907 bytes after a single replay, and the desktop
        // drifted away from the underlay).
        const std::wstring& path = wi.replayPath.empty() ? wi.path : wi.replayPath;
        if (!path.empty()) {
            HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                        nullptr, OPEN_EXISTING, 0, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                ::CloseHandle(file);
                // SENDWININICHANGE only: with SPIF_UPDATEINIFILE the replay
                // would write the wallpaper back into HKCU\Control
                // Panel\Desktop - mutating the user's own settings (ending
                // slideshows, and on per-monitor wallpaper setups resetting
                // every other monitor to monitor 0's image). The
                // session-local replay is what makes the shell rebuild the
                // desktop layer; persistence is not wanted here.
                ::SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0,
                                        const_cast<LPWSTR>(path.c_str()),
                                        SPIF_SENDWININICHANGE);
                replayed = true;
            }
        }
    }

    // Nudge DWM so an RDP client picks up the dirty region promptly.
    // (PaintWallpaperRect flushes on its own when it painted.)
    if (!painted) {
        if (HMODULE dwm = ::GetModuleHandleW(L"dwmapi.dll")) {
            using PFN_DwmFlush = HRESULT(WINAPI*)();
            auto pfn = reinterpret_cast<PFN_DwmFlush>(
                reinterpret_cast<void*>(::GetProcAddress(dwm, "DwmFlush")));
            if (pfn) pfn();
        }
    }
    return replayed;
}

// The Win11 24H2+ shell builds the desktop with a Progman that has
// WS_EX_NOREDIRECTIONBITMAP and a layered SHELLDLL_DefView child ("raised
// desktop"). Wallpaper handling differs there (see RefreshWallpaper).
bool IsRaisedDesktop() {
    HWND progman = ::FindWindowW(L"Progman", nullptr);
    if (!progman) return false;
    constexpr LONG_PTR kExNoRedirectionBitmap = 0x00200000;
    return (::GetWindowLongPtrW(progman, GWL_EXSTYLE) & kExNoRedirectionBitmap) != 0;
}

WallpaperInfo CurrentWallpaper(int monitorIndex) {
    WallpaperInfo wi;

    // Preferred source: IDesktopWallpaper gives the per-monitor image (the
    // registry only reflects the last one set globally) and the placement as a
    // canonical enum (the registry carries undocumented values such as 10 on
    // some builds, which would otherwise be guessed as "fill").
    bool havePath = false;
    bool haveStyle = false;
    IDesktopWallpaper* dpw = nullptr;
    if (SUCCEEDED(::CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL,
                                     IID_IDesktopWallpaper,
                                     reinterpret_cast<void**>(&dpw))) &&
        dpw) {
        UINT count = 0;
        if (SUCCEEDED(dpw->GetMonitorDevicePathCount(&count)) && count > 0) {
            // Map the app-wide EnumDisplayMonitors index (the Config::monitorIndex
            // convention) to IDesktopWallpaper's device-path index by geometry:
            // the two enumerations are not documented to agree, and a naive
            // index would decode another monitor's wallpaper image on
            // multi-monitor setups. Monitor rects are unique (positions
            // differ), so an exact match is unambiguous; the naive index stays
            // the fallback when geometry resolution is unavailable.
            UINT idx = (monitorIndex >= 0 && static_cast<UINT>(monitorIndex) < count)
                           ? static_cast<UINT>(monitorIndex)
                           : 0;
            RECT target{};
            if (MonitorRectForIndex(monitorIndex, &target)) {
                for (UINT i = 0; i < count; ++i) {
                    LPWSTR probe = nullptr;
                    if (FAILED(dpw->GetMonitorDevicePathAt(i, &probe)) || !probe) continue;
                    RECT r{};
                    const bool haveRect = SUCCEEDED(dpw->GetMonitorRECT(probe, &r));
                    ::CoTaskMemFree(probe);
                    if (haveRect && ::EqualRect(&r, &target)) {
                        idx = i;
                        break;
                    }
                }
            }
            LPWSTR devPath = nullptr;
            if (SUCCEEDED(dpw->GetMonitorDevicePathAt(idx, &devPath)) && devPath) {
                LPWSTR wpath = nullptr;
                if (SUCCEEDED(dpw->GetWallpaper(devPath, &wpath)) && wpath) {
                    if (wpath[0]) {
                        wi.path = wpath;
                        havePath = true;
                    }
                    ::CoTaskMemFree(wpath);
                }
                ::CoTaskMemFree(devPath);
            }
        }
        DESKTOP_WALLPAPER_POSITION pos = DWPOS_FILL;
        if (SUCCEEDED(dpw->GetPosition(&pos)) &&
            pos >= DWPOS_CENTER && pos <= DWPOS_SPAN) {
            wi.style = static_cast<int>(pos);
            haveStyle = true;
        }
        COLORREF bg = 0;
        if (SUCCEEDED(dpw->GetBackgroundColor(&bg))) {
            wi.bg = static_cast<unsigned>(((GetRValue(bg) & 0xFF) << 16) |
                                          ((GetGValue(bg) & 0xFF) << 8) |
                                          (GetBValue(bg) & 0xFF));
        }
        dpw->Release();
    }

    // Registry fallback (also fills in whatever COM did not provide).
    if (!havePath) {
        wchar_t raw[MAX_PATH + 1]{};
        DWORD size = sizeof(raw) - sizeof(wchar_t);
        DWORD type = 0;
        if (::RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"Wallpaper",
                           RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, &type, raw,
                           &size) == ERROR_SUCCESS &&
            raw[0]) {
            wchar_t expanded[MAX_PATH + 1]{};
            if (::ExpandEnvironmentStringsW(raw, expanded, MAX_PATH))
                wi.path = expanded;
            else
                wi.path = raw;
        }
    }
    if (!haveStyle) {
        // placement style: WallpaperStyle 0..5, TileWallpaper overrides to tile
        wchar_t buf[16]{};
        DWORD size = sizeof(buf) - sizeof(wchar_t);
        if (::RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"WallpaperStyle",
                           RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
            wi.style = ::_wtoi(buf);
        size = sizeof(buf) - sizeof(wchar_t);
        if (::RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"TileWallpaper",
                           RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
            wi.tile = ::_wtoi(buf) != 0;
        if (wi.tile) wi.style = 1;
    }
    if (wi.bg == 0) {
        // letterbox / solid background color: HKCU\Control Panel\Colors\Background
        wchar_t color[32]{};
        DWORD size = sizeof(color) - sizeof(wchar_t);
        if (::RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Colors", L"Background",
                           RRF_RT_REG_SZ, nullptr, color, &size) == ERROR_SUCCESS) {
            int r = 0, g = 0, b = 0;
            if (::swscanf(color, L"%d %d %d", &r, &g, &b) == 3)
                wi.bg = static_cast<unsigned>(((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF));
        }
    }

    // The shell does NOT display the file resolved above - it renders a
    // chain of re-encoded caches. Measured on this machine (PrintWindow of
    // the wallpaper layer vs each candidate file, far-region fit):
    //   1. Themes\CachedFiles\CachedImage_<W>_<H>_POS*.jpg - pre-fitted to
    //      the monitor and placed 1:1. Mean|diff| vs screen: 0.65/255 -
    //      THIS is what DWM displays.
    //   2. Themes\TranscodedWallpaper - first re-encode (err ~15 if used).
    //   3. The registry/COM original (err ~15 plus encode deltas).
    // The underlay must decode #1 or the rounded-corner cut-outs sit the
    // crop of a *different encode* directly against the real screen - the
    // user-visible "translucent corner" residue. The caches are rewritten
    // in place (wallpaper change / slideshow rotation), so the underlay key
    // carries the file's LastWriteTime.
    // replayPath keeps the ORIGINAL resolved path: SPI_SETDESKWALLPAPER
    // replays must never feed a cache tier back to the shell, or it
    // re-encodes an already-re-encoded image and the desktop degrades a
    // JPEG generation per replay (measured: CachedImage 296033 ->
    // 292907 bytes after one replay, desktop drifting away from the crop).
    wi.replayPath = wi.path;
    if (!wi.path.empty()) {
        wchar_t appdata[MAX_PATH]{};
        if (::GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) {
            const std::wstring themes = std::wstring(appdata) + L"\\Microsoft\\Windows\\Themes";

            // Tier 1: monitor-matched CachedImage. The file is exactly the
            // monitor's size, so the "stretch" placement becomes a 1:1 map -
            // the decode math then reproduces the screen pixels exactly.
            RECT mon{};
            if (MonitorRectForIndex(monitorIndex, &mon)) {
                wchar_t pattern[64]{};
                ::swprintf(pattern, 64, L"CachedImage_%ld_%ld_POS*.jpg",
                           static_cast<long>(mon.right - mon.left),
                           static_cast<long>(mon.bottom - mon.top));
                WIN32_FIND_DATAW fd{};
                HANDLE find = ::FindFirstFileW((themes + L"\\CachedFiles\\" + pattern).c_str(),
                                               &fd);
                if (find != INVALID_HANDLE_VALUE) {
                    wi.path  = themes + L"\\CachedFiles\\" + fd.cFileName;
                    wi.style = 2;  // monitor-sized => stretch is an exact 1:1
                    ::FindClose(find);
                    return wi;
                }
            }

            // Tier 2: TranscodedWallpaper (first re-encode).
            const std::wstring transcoded = themes + L"\\TranscodedWallpaper";
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (::GetFileAttributesExW(transcoded.c_str(), GetFileExInfoStandard, &fad) &&
                (fad.nFileSizeHigh || fad.nFileSizeLow) &&
                (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                wi.path = transcoded;
            }
        }
    }

    return wi;
}

std::wstring DescribeHost(HWND host) {
    if (!host) return L"(none)";
    return ClassNameOf(host);
}

}  // namespace auraui::desktop
