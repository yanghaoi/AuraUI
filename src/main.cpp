// ---------------------------------------------------------------------------
// AuraUI - native Windows desktop system-monitor HUD.
//
//   C++20 / Win32 / Direct2D / DirectWrite / Common Controls
//   No .NET, no Qt, no Electron, no web runtime, no network access.
// ---------------------------------------------------------------------------

#include "app/app.h"
#include "core/log.h"
#include "core/paths.h"
#include "hud/hud.h"
#include "monitor/sampler.h"
#include "tray/tray.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <string.h>

namespace {

// Our manifest lives at RT_MANIFEST resource id 2 (see assets/app.rc).
//
// Why id 2 and not the usual id 1?
//   MinGW-w64 (binutils >= 2.44, shipped with GCC 16) automatically links
//   lib/default-manifest.o, which already contains an RT_MANIFEST resource with
//   id 1. Supplying a second id-1 manifest makes ld fail with
//   ".rsrc merge failure: multiple non-default manifests".
//   Putting ours at id 2 avoids the clash; we then activate it explicitly so
//   the Common Controls v6 dependency still takes effect.
constexpr int kManifestResourceId = 2;

ULONG_PTR g_actCtxCookie = 0;
HANDLE    g_actCtx       = INVALID_HANDLE_VALUE;

void ActivateEmbeddedManifest() {
    ACTCTXW ctx{};
    ctx.cbSize          = sizeof(ctx);
    ctx.dwFlags         = ACTCTX_FLAG_RESOURCE_NAME_VALID | ACTCTX_FLAG_HMODULE_VALID;
    ctx.hModule         = ::GetModuleHandleW(nullptr);
    ctx.lpResourceName  = MAKEINTRESOURCEW(kManifestResourceId);

    g_actCtx = ::CreateActCtxW(&ctx);
    if (g_actCtx == INVALID_HANDLE_VALUE) {
        // Not fatal: the app runs, it just falls back to the legacy control look.
        return;
    }
    if (!::ActivateActCtx(g_actCtx, &g_actCtxCookie)) {
        ::ReleaseActCtx(g_actCtx);
        g_actCtx = INVALID_HANDLE_VALUE;
        g_actCtxCookie = 0;
    }
}

void DeactivateEmbeddedManifest() {
    if (g_actCtxCookie) {
        ::DeactivateActCtx(0, g_actCtxCookie);
        g_actCtxCookie = 0;
    }
    if (g_actCtx != INVALID_HANDLE_VALUE) {
        ::ReleaseActCtx(g_actCtx);
        g_actCtx = INVALID_HANDLE_VALUE;
    }
}

// The embedded manifest declares PerMonitorV2, but because it is not the
// process default manifest we also set DPI awareness through the API.
void TrySetPerMonitorV2() {
    using PFN_SetProcessDpiAwarenessContext = BOOL(WINAPI*)(HANDLE);
    static PFN_SetProcessDpiAwarenessContext pfn =
        reinterpret_cast<PFN_SetProcessDpiAwarenessContext>(reinterpret_cast<void*>(
            ::GetProcAddress(::GetModuleHandleW(L"user32.dll"),
                             "SetProcessDpiAwarenessContext")));
    if (!pfn) return;
    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
    pfn(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)));
}

// Logs which comctl32 got bound - "System32\comctl32.dll" means v5 (no visual
// styles), a WinSxS path means v6 was activated successfully.
void LogCommonControlsBinding() {
    HMODULE mod = ::GetModuleHandleW(L"comctl32.dll");
    if (!mod) return;
    wchar_t path[MAX_PATH]{};
    if (::GetModuleFileNameW(mod, path, MAX_PATH))
        auraui::log::Info(std::wstring(L"comctl32 bound to: ") + path);
}

void NotifyExistingInstance() {
    HWND existing = ::FindWindowW(L"AuraUIMessageWindow", L"AuraUI");
    if (existing)
        ::PostMessageW(existing, WM_COMMAND,
                       MAKEWPARAM(static_cast<UINT>(auraui::Tray::CmdSettings), 0), 0);
}

struct Options {
    bool         dump    = false;
    bool         verbose = false;
    std::wstring dumpPath;
};

// Diagnostics: `AuraUI.exe --dump <file.bmp>` renders one frame of the
// HUD into a BMP and exits. Useful for verifying the renderer without a desktop.
// `AuraUI.exe --verbose` enables debug-level logging (per-event tray
// callbacks, underlay crops) for the session.
Options ParseCommandLine() {
    Options o;
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (!argv) return o;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--dump") == 0 && i + 1 < argc) {
            o.dump     = true;
            o.dumpPath = argv[++i];
        } else if (_wcsicmp(argv[i], L"--verbose") == 0) {
            o.verbose = true;
        }
    }
    ::LocalFree(argv);
    return o;
}

// Drive-absolute (C:\...), UNC (\\server\share) or rooted (\foo).
bool IsAbsolutePath(const std::wstring& p) {
    if (p.size() >= 2 && p[1] == L':') return true;
    if (p.size() >= 2 && (p[0] == L'\\' || p[0] == L'/')) return true;
    return false;
}

int RunDump(const std::wstring& path) {
    const auraui::Config cfg = auraui::Config::Load(auraui::paths::ConfigFile());

    auraui::Sampler sampler;
    sampler.Start(cfg.refreshMs);
    // CPU and network are delta-based, so we need at least two samples.
    ::Sleep(static_cast<DWORD>(cfg.refreshMs) * 2 + 400);
    const auraui::Snapshot snap = sampler.Get();
    sampler.Stop();

    auraui::Hud hud;
    hud.ApplyConfig(cfg);
    hud.SetSnapshot(snap);
    const bool ok = hud.DumpToBmp(path);

    auraui::log::Info(std::wstring(ok ? L"Dump written: " : L"Dump FAILED: ") + path);
    return ok ? 0 : 2;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, LPWSTR, int) {
    // COM (STA) for WIC (wallpaper decoding) and shell components.
    struct ComGuard {
        HRESULT hr;
        ComGuard() : hr(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
        // Pair only on success (S_FALSE included - it also needs a matching
        // uninit): uninitializing after a failed init would decrement
        // somebody else's apartment count.
        ~ComGuard() {
            if (SUCCEEDED(hr)) ::CoUninitialize();
        }
    } comGuard;

    ActivateEmbeddedManifest();
    TrySetPerMonitorV2();

    // Capture the launch directory BEFORE the chdir below: --dump takes a
    // user-supplied path, and a relative one must land where the user ran the
    // program - not inside %APPDATA%\AuraUI (where it used to go silently).
    std::wstring launchDir;
    {
        std::wstring buf(MAX_PATH, L'\0');
        for (;;) {
            const DWORD n = ::GetCurrentDirectoryW(static_cast<DWORD>(buf.size()),
                                                   buf.data());
            if (n == 0) break;
            if (n < buf.size()) { buf.resize(n); launchDir = buf; break; }
            buf.resize(buf.size() * 2);
        }
    }

    // Keep third-party side effects (the NVIDIA driver drops a umdlogs folder
    // into the process working directory) out of wherever the user launched us
    // from. All of our own paths are absolute, so this is safe.
    const std::wstring appDir = auraui::paths::AppDataDir();
    if (!appDir.empty()) ::SetCurrentDirectoryW(appDir.c_str());

    Options opts = ParseCommandLine();
    if (opts.verbose) auraui::log::SetDebugEnabled(true);

    // A relative --dump path belongs next to the user, not in %APPDATA%.
    if (opts.dump && !opts.dumpPath.empty() && !launchDir.empty() &&
        !IsAbsolutePath(opts.dumpPath))
        opts.dumpPath = launchDir + L"\\" + opts.dumpPath;

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES |
                ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS;
    ::InitCommonControlsEx(&icc);

    if (opts.dump) {
        auraui::log::Init(auraui::paths::LogFile());
        const int rc = RunDump(opts.dumpPath);
        auraui::log::Shutdown();
        DeactivateEmbeddedManifest();
        return rc;
    }

    HANDLE mutex = ::CreateMutexW(nullptr, TRUE, L"Local\\AuraUI_SingleInstance");
    if (mutex && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        NotifyExistingInstance();
        ::CloseHandle(mutex);
        DeactivateEmbeddedManifest();
        return 0;
    }

    auraui::log::Init(auraui::paths::LogFile());

    int rc = 1;
    {
        auraui::App app;
        if (app.Init(hinst, auraui::paths::ConfigFile())) {
            LogCommonControlsBinding();
            rc = app.Run();
        }
    }

    auraui::log::Shutdown();
    DeactivateEmbeddedManifest();
    if (mutex) ::CloseHandle(mutex);
    return rc;
}
