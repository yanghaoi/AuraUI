#pragma once

#include "config/config.h"
#include "hud/theme.h"
#include "monitor/snapshot.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace auraui {

// Explicit IIDs (defined in hud.cpp; shared with hud_render.cpp's width
// measurement and the settings window's width guard).
extern const GUID kIID_ID2D1Factory;
extern const GUID kIID_IDWriteFactory;

// ---------------------------------------------------------------------------
// The desktop HUD window.
//
// Rendering strategy
// ------------------
// ATTACHED MODE: a plain (non-layered) WS_CHILD of the wallpaper WorkerW that
// paints via WM_PAINT/BitBlt. Cross-process layered children are not composited
// on some shell builds (see docs/technical-notes.md), so per-pixel transparency
// is faked with a "wallpaper underlay": the wallpaper region behind the panel
// is decoded on a worker thread, cropped to panel size, and drawn under the
// translucent panel - the GDI surface stays fully opaque.
//
// MOVE MODE: the window detaches into a top-level WS_EX_TOPMOST popup and uses
// UpdateLayeredWindow for the real per-pixel drag look.
//
// Both modes draw with Direct2D (ID2D1DCRenderTarget) into a 32bpp premultiplied
// DIB - no DXGI swap chain, no device-loss handling, no GPU dependency.
// ---------------------------------------------------------------------------
class Hud {
public:
    // Posted to the notify window (see SetNotifyWindow) when the underlay
    // worker finished a crop; the owner routes it to ApplyUnderlayResult().
    static constexpr UINT kUnderlayReadyMsg = WM_APP + 6;

    Hud();
    ~Hud();

    bool Create(HINSTANCE hinst, const Config& cfg);
    void Destroy();

    HWND Hwnd() const { return hwnd_; }

    // Full re-apply: re-measures, re-creates text formats if needed, re-parents.
    void ApplyConfig(const Config& cfg);

    void SetSnapshot(const Snapshot& snap);

    // Cheap: just marks the window dirty.
    void RequestRepaint();

    void SetVisible(bool visible);
    bool Visible() const { return visible_; }

    // Re-parent to the desktop layer and re-position (Explorer restart / display change).
    // Returns false when the desktop layer is not available yet (retried by the app).
    // allowSpawn: whether Progman may be nudged into building the wallpaper
    // layer; timer-driven callers must pass false (see desktop::Attach).
    bool Reattach(bool allowSpawn);

    // Monitor layout or DPI changed.
    void OnDisplayChange();

    // Move/resize according to the current config without touching the parent.
    void Reposition();

    // --- Move mode ----------------------------------------------------------
    // Pops the HUD out of the desktop layer into a topmost, mouse-interactive
    // top-level window that can be dragged with the mouse (the whole surface
    // behaves like a title bar). Finishing (double-click / right-click on the
    // HUD) reports the dropped position via onMoveFinished and re-attaches.
    void BeginMove();
    void EndMove(bool applyPosition);
    bool InMoveMode() const { return moveMode_; }
    void SetOnMoveFinished(
        std::function<void(bool applied, int x, int y, int monitorIndex)> cb) {
        onMoveFinished_ = std::move(cb);
    }

    // Live opacity preview from the settings slider (does not touch Config).
    void SetOpacityPreview(int opacity);

    // --- Wallpaper underlay / desktop repaint hooks --------------------------
    // Re-read the wallpaper state (path / placement / background color) and
    // schedule an async re-crop of the underlay. Call when the wallpaper may
    // have changed (WM_SETTINGCHANGE, display change, periodic poll).
    void RefreshUnderlay();

    // The app replays the wallpaper (SPI_SETDESKWALLPAPER) to rebuild the
    // desktop layer after a vacate. The shell then churns its cache chain
    // for up to tens of seconds (CachedFiles deleted + rewritten, desktop
    // transiently on a different tier). During the settle window the
    // underlay re-checks its key EVERY render instead of every 60th, so the
    // crop tracks the churn and lands on the final generation (observed
    // regeneration: 6-20s; window is generous because a poll is cheap).
    void NotifyWallpaperReplayed() { underlaySettle_ = 120; }

    // Move the completed worker result into the render pipeline (UI thread).
    void ApplyUnderlayResult();

    // Window that receives kUnderlayReadyMsg from the worker thread. Must be
    // set before Create() and outlive all windows. Thread-safe: the worker
    // reads it under the underlay mutex before posting.
    void SetNotifyWindow(HWND w);

    // Called whenever the HUD vacates a rectangle on the desktop layer (moved /
    // hidden / destroyed) so the owner can coalesce the repaint requests.
    // Without a callback the desktop is refreshed immediately.
    using VacateCallback = std::function<void(const RECT*)>;
    void SetOnVacate(VacateCallback cb) { onVacate_ = std::move(cb); }

    // Diagnostic / preview: render the current configuration + snapshot into a
    // 32-bit BMP without needing a window. Used by `AuraUI.exe --dump <file>`.
    bool DumpToBmp(const std::wstring& path);

    // Minimum panel width (px) that fits every enabled row's text at the given
    // config + snapshot, measured with the same fonts the renderer uses. The
    // settings window guards the width field with it; the renderer enforces
    // the same floor at render time (with shrink hysteresis - see Render()).
    // dpi is the effective DPI of the monitor the panel lives on.
    static float RequiredPanelWidth(const Config& cfg, const Snapshot& snap, UINT dpi);

    // Pixels of the last measured layout (for the settings GUI preview).
    int PanelWidth() const { return pixelWidth_; }
    int PanelHeight() const { return pixelHeight_; }

private:
    struct Item {
        enum class Kind { Title, Meter, Text, Net, Spacer } kind = Kind::Text;
        std::wstring label;
        std::wstring value;
        std::wstring sub1;   // network: download line
        std::wstring sub2;   // network: upload line
        float        fraction = 0.0f;
        bool         bar      = false;
        float        y        = 0.0f;
        float        h        = 0.0f;
        float        gapAfter = 0.0f;
        float        valueW   = 56.0f;  // Meter: FLOOR width of the right-aligned
                                        // value zone; the renderer grows it to
                                        // the measured text so leading digits
                                        // are never clipped (NO_WRAP + CLIP)
    };

    // Everything the underlay crop depends on; a change re-schedules the worker.
    struct UnderlayKey {
        std::wstring path;
        int          style = 0;      // shell placement (0 center .. 5 span)
        unsigned     bg    = 0;      // 0x00RRGGBB letterbox color
        RECT         mon{};          // monitor rect
        int          px = 0, py = 0; // panel position, screen coords
        int          w  = 0, h  = 0; // panel size
        // LastWriteTime of the decoded file. The path may be the shell's
        // TranscodedWallpaper cache, which is REWRITTEN in place (wallpaper
        // change, slideshow rotation) - the stamp is what makes the crop
        // follow those updates despite a constant path string.
        unsigned long long stamp = 0;
        bool operator==(const UnderlayKey& o) const {
            return path == o.path && style == o.style && bg == o.bg &&
                   mon.left == o.mon.left && mon.top == o.mon.top &&
                   mon.right == o.mon.right && mon.bottom == o.mon.bottom &&
                   px == o.px && py == o.py && w == o.w && h == o.h &&
                   stamp == o.stamp;
        }
    };

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);

    bool  CreateDeviceIndependentResources();
    bool  CreateDeviceResources();
    void  DiscardDeviceResources();
    bool  EnsureBackBuffer(int w, int h);
    void  RefreshDpi();
    void  RebuildTextFormats();

    static std::vector<Item> BuildItemsFor(const Config& cfg, const Snapshot& sn, float s);
    std::vector<Item> BuildItems(float s) const;
    float             LayoutItems(std::vector<Item>& items, float s, float width) const;

    // Shared text measurement (used by the value-zone split, the content width
    // floor and RequiredPanelWidth). Returns 0 for empty input / no factory.
    static IDWriteTextFormat* MakeTextFormat(IDWriteFactory* dw, const std::wstring& family,
                                             float size, DWRITE_FONT_WEIGHT weight,
                                             bool trailing);
    static float MeasureTextWidth(IDWriteFactory* dw, IDWriteTextFormat* fmt,
                                  const std::wstring& txt);
    // The ONE implementation of the right-aligned value-zone width:
    // max(zone floor, measured text + pad). ContentMinWidth (panel width
    // floor) and RenderToTarget (drawing) both call this, so the zone split
    // can never drift between "measured" and "drawn".
    static float ValueZoneWidth(IDWriteFactory* dw, IDWriteTextFormat* fmtValueR,
                                const std::wstring& value, float floorW,
                                float pad, float s);
    // Minimum width so no label or right-aligned value is clipped on any row.
    static float ContentMinWidth(const std::vector<Item>& items, IDWriteFactory* dw,
                                 IDWriteTextFormat* fmtTitle, IDWriteTextFormat* fmtLabel,
                                 IDWriteTextFormat* fmtValueR, float s, float pad);

    void              RenderToTarget(ID2D1RenderTarget* rt, const std::vector<Item>& items,
                                     float s, float width, float height);
    void              Render();
    bool              Present();          // false => caller must redraw (fallback switch)
    void              PlaceWindow();
    int               EffectiveOpacity() const;

    bool  CanShowOnDesktop() const;       // attached (or move mode) - not floating
    void  VacateRect(const RECT& r);      // route through onVacate_ if set

    void              ScheduleUnderlay(); // latest-wins request to the worker
    UnderlayKey       BuildUnderlayKey() const;
    void              UnderlayWorker();   // thread body: decode -> crop -> compose
    static bool       DecodeUnderlayCrop(const UnderlayKey& req,
                                         std::vector<std::uint8_t>* out);
    // Panel-sized opaque crop in the shell background color: the underlay
    // for setups with no decodable image (solid-color desktop, Spotlight
    // without a cache) and for TILE/SPAN placements, whose geometry a
    // per-monitor crop cannot express. Keeps the panel alpha meaningful -
    // the panel composites over the background color instead of turning
    // fully opaque.
    static bool       MakeSolidCrop(const UnderlayKey& req,
                                    std::vector<std::uint8_t>* out);

    float Scale() const { return static_cast<float>(dpi_) / 96.0f; }

    HWND      hwnd_  = nullptr;
    HINSTANCE hinst_ = nullptr;
    bool      visible_ = true;
    HWND      notifyWnd_ = nullptr;

    // Move mode state (see BeginMove/EndMove).
    bool moveMode_ = false;
    std::function<void(bool applied, int x, int y, int monitorIndex)> onMoveFinished_;
    VacateCallback onVacate_;

    bool ulwOk_       = true;   // UpdateLayeredWindow works for this window
    bool opaqueFill_  = false;  // fallback path fills the whole surface

    Config   cfg_{};
    Snapshot snapshot_{};

    UINT dpi_         = 96;
    int  pixelWidth_  = 0;
    int  pixelHeight_ = 0;
    int  previewOpacity_ = -1;

    // Content-driven width floor (see ContentMinWidth). Grows instantly when
    // a row needs more room, shrinks only after the smaller requirement has
    // held for several consecutive renders - value strings change length
    // second to second and an un-damped floor would make the panel breathe
    // (and re-crop the wallpaper underlay on every pixel of change).
    float widthFloor_        = 0.0f;
    int   widthShrinkStreak_ = 0;

    // Last rectangle the window was placed at (parent-relative). Used to fire
    // the desktop-layer refresh only when the rectangle actually changes.
    int placedX_ = INT_MAX;
    int placedY_ = INT_MAX;
    int placedW_ = -1;
    int placedH_ = -1;

    // --- Wallpaper underlay state -------------------------------------------
    // The worker thread decodes the wallpaper, applies the shell placement
    // math, and composes a panel-sized opaque crop (letterbox areas filled
    // with the user's background color). The full-resolution image is never
    // retained: a 1920x1080 wallpaper costs ~8MB of pixels plus ~8MB of D2D
    // bitmap, versus ~400KB for a panel-sized crop.
    UnderlayKey underlayKey_{};         // last scheduled request (UI thread)
    unsigned    underlayCheck_ = 0;     // render counter for the periodic poll
    // >0 while the shell's wallpaper cache is expected to be churning after
    // our SPI replay (see NotifyWallpaperReplayed): poll every render.
    unsigned    underlaySettle_ = 0;
    ID2D1Bitmap* underlayBmp_  = nullptr;
    std::vector<std::uint8_t> underlayPix_;  // applied crop, panel-size BGRA
    int underlayW_ = 0;
    int underlayH_ = 0;

    // Worker plumbing (all *Req/Incoming/Quit members guarded by the mutex).
    std::thread           underlayThread_;
    std::mutex            underlayMtx_;
    std::condition_variable underlayCv_;
    UnderlayKey           underlayReq_;
    bool                  underlayHasReq_  = false;
    bool                  underlayQuit_    = false;
    // Set by the worker when the last request failed to decode; lets
    // ScheduleUnderlay re-queue an identical key (the render fast path
    // re-tries "as fast as we render", which the key dedup would swallow).
    bool                  underlayReqFailed_ = false;
    std::vector<std::uint8_t> underlayIncoming_;
    int                  underlayIncomingW_ = 0;
    int                  underlayIncomingH_ = 0;
    bool                 underlayIncomingValid_ = false;

    ID2D1Factory*        d2dFactory_    = nullptr;
    IDWriteFactory*      dwriteFactory_ = nullptr;
    ID2D1DCRenderTarget* dcTarget_      = nullptr;

    IDWriteTextFormat* fmtTitle_ = nullptr;
    IDWriteTextFormat* fmtLabel_ = nullptr;
    IDWriteTextFormat* fmtValue_ = nullptr;
    IDWriteTextFormat* fmtValueR_ = nullptr;
    IDWriteTextFormat* fmtSmall_ = nullptr;
    IDWriteTextFormat* fmtSmallR_ = nullptr;

    HDC     memDC_     = nullptr;
    HBITMAP dib_       = nullptr;
    void*   dibBits_   = nullptr;
    HGDIOBJ oldBitmap_ = nullptr;
    int     dibWidth_  = 0;
    int     dibHeight_ = 0;
};

}  // namespace auraui
