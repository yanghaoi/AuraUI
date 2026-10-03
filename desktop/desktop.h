#pragma once

#include <windows.h>

#include <string>

namespace auraui::desktop {

// ---------------------------------------------------------------------------
// Windows Explorer desktop-layer plumbing.
//
// Correct topology (after the wallpaper layer has been built):
//
//     WorkerW  (icon host)          <- contains SHELLDLL_DefView -> SysListView32
//     WorkerW  (wallpaper layer)    <- we parent HERE
//     Progman
//
// The wallpaper layer is a full-screen, VISIBLE, Explorer-owned WorkerW that
// does NOT host SHELLDLL_DefView. Its location depends on the shell build
// (top-level on Win10~11 23H2, a direct child of Progman on 24H2+), so both are
// searched.
//
// It is created lazily: when it does not exist yet, `0x052C` must be sent to
// Progman to make the shell build it. The parameters matter - wParam must be
// 0xD; sending (0,0) is a silent no-op and the window then has nowhere visible
// to live.
//
// Parenting to Progman itself does NOT work: SHELLDLL_DefView paints its
// background from the parent's buffer, so a sibling placed below it is covered
// by that background, and placing it above would cover the icons.
//
// We never use HWND_TOPMOST.
// ---------------------------------------------------------------------------

// The "TaskbarCreated" broadcast id, registered once. Explorer sends it every
// time it (re)starts, which is how we detect explorer.exe restarts.
UINT TaskbarCreatedMessage();

// Top-level window that hosts SHELLDLL_DefView (the icon layer).
HWND FindIconHost();

// SHELLDLL_DefView itself (the window that actually draws the icons).
// Returns nullptr when Explorer is down.
HWND FindIconView();

// The window to parent the HUD to, chosen by desktop layout:
//   * layout A (Progman hosts the icon view) -> Progman
//   * layout B (a WorkerW hosts the icon view) -> the first *visible* WorkerW
//     behind it in z-order
// Hidden hosts are never chosen: a child of a hidden window is invisible too.
//
// allowSpawn=false suppresses the 0x052C nudge (which creates an extra WorkerW
// every time) - use that from timer-driven callers such as the health check.
// Returns nullptr when no usable host exists (Explorer is down / restarting).
HWND FindWallpaperHost(bool allowSpawn = true);

// Parent `hwnd` to the desktop layer and place it directly below the icon
// view. allowSpawn=false suppresses the 0x052C nudge: timer-driven callers
// (the 2s health check) must not churn the shell or block on it - see
// FindWallpaperHost.
bool Attach(HWND hwnd, bool allowSpawn = true);

// Re-assert the z-order without re-parenting: `hwnd` must sit immediately below
// SHELLDLL_DefView (above the wallpaper, below the icons). Returns true when the
// window is already correctly placed, false when it had to be moved.
bool EnsureZOrder(HWND hwnd);

// The window to pass as `hWndInsertAfter` so that a child of `parent` ends up
// immediately below the desktop icons:
//   * the parent's own SHELLDLL_DefView when it has one,
//   * otherwise HWND_BOTTOM (the wallpaper-WorkerW layout).
// Returns nullptr when `parent` is null.
HWND ZOrderAnchor(HWND parent);

// Un-parent the window (shutdown).
void Detach(HWND hwnd);

// True while `hwnd` is parented to a live desktop host.
bool IsAttached(HWND hwnd);

// Clear the ghost a vacated child leaves on the wallpaper WorkerW (it never
// repaints exposed areas - its GDI redirection surface keeps the child's last
// frame frozen on screen). Strategy, in order:
//   1) paint-over: decode the wallpaper region for `vacated` (resolved for
//      the monitor the rect is on) and BitBlt it straight onto the host's
//      surface (GetDC is valid cross-process) - no shell involvement, no
//      cache churn;
//   2) surgical RedrawWindow nudge + SPI_SETDESKWALLPAPER replay by the
//      original path (replay=true) - only when paint-over failed or for the
//      desktop-wide refresh (vacated == nullptr, session reconnect). The
//      replay makes the shell churn its wallpaper caches for 6-20s.
// Returns true when the SPI replay ran (the caller should put the underlay
// into settle-follow mode then).
bool RefreshWallpaper(const RECT* vacated = nullptr, bool replay = true);

// Current wallpaper as seen by the shell for the given monitor
// (Config::monitorIndex order). Prefers IDesktopWallpaper (per-monitor path +
// canonical placement enum - the registry holds undocumented values like 10 on
// some builds) and falls back to the registry view. path is empty for
// solid-color / Spotlight-style setups (no single file).
struct WallpaperInfo {
    // What the underlay must DECODE: the shell's display cache tier
    // (CachedFiles\CachedImage -> TranscodedWallpaper -> original). See
    // CurrentWallpaper for the measured motivation.
    std::wstring path;
    // What SPI_SETDESKWALLPAPER replays must use: the ORIGINAL resolved
    // path, NEVER the display cache. Replaying a cache file makes the shell
    // re-encode an already-re-encoded image - the desktop degrades one JPEG
    // generation on every replay and the underlay drifts (measured).
    std::wstring replayPath;
    int      style = 4;   // 0 center, 1 tile, 2 stretch, 3 fit, 4 fill, 5 span
    bool     tile  = false;
    unsigned bg    = 0;   // 0x00RRGGBB letterbox / solid background color
};
WallpaperInfo CurrentWallpaper(int monitorIndex = 0);

// True on the Win11 24H2+ "raised desktop" shell (Progman created with
// WS_EX_NOREDIRECTIONBITMAP, layered SHELLDLL_DefView child). Wallpaper-layer
// handling differs there - see RefreshWallpaper.
bool IsRaisedDesktop();

// Human readable class name of a host window, for the log.
std::wstring DescribeHost(HWND host);

}  // namespace auraui::desktop
