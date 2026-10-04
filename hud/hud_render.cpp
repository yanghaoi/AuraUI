#include "core/log.h"
#include "core/win_util.h"
#include "desktop/desktop.h"
#include "hud/hud.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <wincodec.h>

namespace auraui {
namespace {

struct MonitorEntry {
    RECT  rc{};
    bool  primary = false;
    RECT  work{};
};BOOL CALLBACK EnumMonitorsProc(HMONITOR mon, HDC, LPRECT, LPARAM param) {
    auto* out = reinterpret_cast<std::vector<MonitorEntry>*>(param);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (::GetMonitorInfoW(mon, &mi)) {
        MonitorEntry e;
        e.rc      = mi.rcMonitor;
        e.work    = mi.rcWork;
        e.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        out->push_back(e);
    }
    return TRUE;
}

std::vector<MonitorEntry> EnumMonitors() {
    std::vector<MonitorEntry> list;
    ::EnumDisplayMonitors(nullptr, nullptr, &EnumMonitorsProc,
                          reinterpret_cast<LPARAM>(&list));
    return list;
}

RECT MonitorRectFor(int index) {
    const std::vector<MonitorEntry> list = EnumMonitors();
    if (list.empty()) {
        RECT r{};
        ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0);
        return r;
    }
    if (index >= 0 && index < static_cast<int>(list.size())) return list[static_cast<size_t>(index)].rc;
    for (const auto& e : list)
        if (e.primary) return e.rc;
    return list.front().rc;
}

UINT SystemDpi() {
    HDC dc = ::GetDC(nullptr);
    const UINT dpi = dc ? static_cast<UINT>(::GetDeviceCaps(dc, LOGPIXELSX)) : 96u;
    if (dc) ::ReleaseDC(nullptr, dc);
    return dpi ? dpi : 96u;
}

// LastWriteTime of the underlay source. The resolved path is often the
// shell's TranscodedWallpaper cache, which is rewritten IN PLACE - the stamp
// is the only way the underlay key notices a content change.
unsigned long long FileStamp(const std::wstring& path) {
    if (path.empty()) return 0;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return 0;
    return (static_cast<unsigned long long>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
           fad.ftLastWriteTime.dwLowDateTime;
}

// Machine name for the panel title (cached: GetComputerNameW on every render
// would be wasted work; the name only changes on a reboot).
const std::wstring& HostName() {
    static const std::wstring name = [] {
        wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1]{};
        DWORD n = ARRAYSIZE(buf);
        if (!::GetComputerNameExW(ComputerNameDnsHostname, buf, &n) || n == 0)
            return std::wstring(L"SYSTEM");
        return std::wstring(buf, n);
    }();
    return name;
}

// Move-mode hint band text. Shared with Render(), which reserves panel width
// for it (the band must not clip either).
constexpr wchar_t kMoveHint[] = L"拖动移动位置 · 双击或右键完成";

// Write a top-down 32bpp BGRA buffer as a bottom-up BMP file.
bool WriteBmp32(const std::wstring& path, const void* bits, int w, int h) {
    if (!bits || w <= 0 || h <= 0) return false;

    const DWORD imageBytes = static_cast<DWORD>(w) * 4u * static_cast<DWORD>(h);

    BITMAPFILEHEADER fh{};
    fh.bfType    = 0x4D42;  // "BM"
    fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fh.bfSize    = fh.bfOffBits + imageBytes;

    BITMAPINFOHEADER ih{};
    ih.biSize        = sizeof(ih);
    ih.biWidth       = w;
    ih.biHeight      = h;  // positive => bottom-up
    ih.biPlanes      = 1;
    ih.biBitCount    = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage   = imageBytes;

    HANDLE f = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    bool ok = ::WriteFile(f, &fh, sizeof(fh), &written, nullptr) &&
              ::WriteFile(f, &ih, sizeof(ih), &written, nullptr);

    // Flip rows: our DIB is top-down, BMP wants bottom-up.
    if (ok) {
        const auto* src = static_cast<const unsigned char*>(bits);
        const size_t stride = static_cast<size_t>(w) * 4u;
        std::vector<unsigned char> row(stride);
        for (int y = h - 1; y >= 0 && ok; --y) {
            std::memcpy(row.data(), src + static_cast<size_t>(y) * stride, stride);
            ok = ::WriteFile(f, row.data(), static_cast<DWORD>(stride), &written, nullptr) != FALSE;
        }
    }

    ::CloseHandle(f);
    return ok;
}

}  // namespace

// Decode the wallpaper into a PANEL-SIZED, opaque 32bpp premultiplied-BGRA
// crop for the underlay: apply the shell's placement style to find the source
// rectangle behind the panel, fill the whole buffer with the user's background
// (letterbox) color, then clip + scale the matching image region into place.
// This never retains the full-resolution image - a 1920x1080 wallpaper costs
// ~8MB of pixels, a panel-sized crop ~400KB.
bool Hud::DecodeUnderlayCrop(const UnderlayKey& req, std::vector<std::uint8_t>* out) {
    IWICImagingFactory* factory = nullptr;
    HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IWICImagingFactory,
                                    reinterpret_cast<void**>(&factory));
    IWICBitmapDecoder* decoder = nullptr;
    if (SUCCEEDED(hr))
        hr = factory->CreateDecoderFromFilename(req.path.c_str(), nullptr, GENERIC_READ,
                                                WICDecodeMetadataCacheOnDemand, &decoder);
    IWICBitmapFrameDecode* frame = nullptr;
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    UINT iw = 0, ih = 0;
    if (SUCCEEDED(hr)) hr = frame->GetSize(&iw, &ih);
    if (SUCCEEDED(hr) && (iw == 0 || ih == 0 || iw * ih > 8192u * 8192u))
        hr = E_INVALIDARG;

    // Placement math (must match what the shell shows): where does the image
    // land on the monitor, at which scale? style: 0 center, 1 tile, 2 stretch,
    // 3 fit, 4 fill (and unknown values -> fill), 5 span.
    float srcL = 0, srcT = 0, srcR = 0, srcB = 0;
    float dstL = 0, dstT = 0, dstR = 0, dstB = 0;
    if (SUCCEEDED(hr)) {
        const float mw = static_cast<float>(req.mon.right - req.mon.left);
        const float mh = static_cast<float>(req.mon.bottom - req.mon.top);
        const float px = static_cast<float>(req.px);
        const float py = static_cast<float>(req.py);
        const float w  = static_cast<float>(req.w);
        const float h  = static_cast<float>(req.h);
        const float fiw = static_cast<float>(iw);
        const float fih = static_cast<float>(ih);

        float drawX = 0, drawY = 0, drawW = mw, drawH = mh;
        float sx = 1.0f, sy = 1.0f;
        switch (req.style) {
            case 2:  // stretch
            case 5:  // span (approximated per monitor)
                sx = mw / fiw;
                sy = mh / fih;
                break;
            case 3: {  // fit (letterbox)
                const float sc = std::min(mw / fiw, mh / fih);
                sx = sy = sc;
                drawW = fiw * sc;
                drawH = fih * sc;
                drawX = (mw - drawW) / 2.0f;
                drawY = (mh - drawH) / 2.0f;
                break;
            }
            case 0:  // center (native size, letterbox)
                drawW = fiw;
                drawH = fih;
                drawX = (mw - fiw) / 2.0f;
                drawY = (mh - fih) / 2.0f;
                break;
            case 1:  // tile: caller never schedules a crop for tile
            default: {  // fill (shell default, incl. undocumented values)
                const float sc = std::max(mw / fiw, mh / fih);
                sx = sy = sc;
                drawW = fiw * sc;
                drawH = fih * sc;
                drawX = (mw - drawW) / 2.0f;
                drawY = (mh - drawH) / 2.0f;
                break;
            }
        }

        // Image-space rect behind the panel, clamped to the image; the panel
        // rect shrinks correspondingly (letterbox bands stay background color).
        srcL = (px - req.mon.left - drawX) / sx;
        srcT = (py - req.mon.top - drawY) / sy;
        srcR = (px - req.mon.left + w - drawX) / sx;
        srcB = (py - req.mon.top + h - drawY) / sy;
        dstL = 0.0f;
        dstT = 0.0f;
        dstR = w;
        dstB = h;
        if (srcL < 0.0f) {
            dstL -= srcL * sx;
            srcL = 0.0f;
        }
        if (srcT < 0.0f) {
            dstT -= srcT * sy;
            srcT = 0.0f;
        }
        if (srcR > fiw) {
            dstR -= (srcR - fiw) * sx;
            srcR = fiw;
        }
        if (srcB > fih) {
            dstB -= (srcB - fih) * sy;
            srcB = fih;
        }
    }

    // Compose: background fill, then the clipped+scaled image region.
    if (SUCCEEDED(hr)) {
        const unsigned b = (req.bg & 0xFF);          // 0x00RRGGBB -> BGRA
        const unsigned g = (req.bg >> 8) & 0xFF;
        const unsigned r = (req.bg >> 16) & 0xFF;
        const std::uint8_t bgPix[4] = {static_cast<std::uint8_t>(b),
                                       static_cast<std::uint8_t>(g),
                                       static_cast<std::uint8_t>(r), 0xFF};
        out->assign(static_cast<size_t>(req.w) * req.h * 4, 0);
        for (size_t i = 0; i < out->size(); i += 4)
            std::memcpy(&(*out)[i], bgPix, 4);

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
                hr = scaler->Initialize(clipper, dstW, dstH,
                                        WICBitmapInterpolationModeFant);

            IWICFormatConverter* conv = nullptr;
            if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&conv);
            if (SUCCEEDED(hr))
                hr = conv->Initialize(scaler, GUID_WICPixelFormat32bppPBGRA,
                                      WICBitmapDitherTypeNone, nullptr, 0.0f,
                                      WICBitmapPaletteTypeCustom);
            if (SUCCEEDED(hr)) {
                // CopyPixels strides its output by cbStride while the target
                // buffer's real row stride is req.w*4 - they differ whenever
                // the letterbox clamp shrank the region (panel hanging off
                // the monitor/image edge). Copy into a contiguous scratch
                // buffer first, then blit row by row, or every row after the
                // first shifts and the crop skews diagonally.
                std::vector<std::uint8_t> scratch(
                    static_cast<size_t>(dstW) * dstH * 4);
                hr = conv->CopyPixels(nullptr, dstW * 4,
                                      static_cast<UINT>(scratch.size()),
                                      scratch.data());
                if (SUCCEEDED(hr)) {
                    auto* dst = out->data() +
                                (static_cast<size_t>(dstT) * req.w +
                                 static_cast<size_t>(dstL)) * 4;
                    for (UINT row = 0; row < dstH; ++row) {
                        std::memcpy(dst + static_cast<size_t>(row) * req.w * 4,
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

// Panel-sized opaque crop in the shell background color (0x00RRGGBB -> BGRA).
bool Hud::MakeSolidCrop(const UnderlayKey& req, std::vector<std::uint8_t>* out) {
    if (req.w <= 0 || req.h <= 0) return false;
    const std::uint8_t bgPix[4] = {static_cast<std::uint8_t>(req.bg & 0xFF),
                                   static_cast<std::uint8_t>((req.bg >> 8) & 0xFF),
                                   static_cast<std::uint8_t>((req.bg >> 16) & 0xFF), 0xFF};
    out->assign(static_cast<size_t>(req.w) * req.h * 4, 0);
    for (size_t i = 0; i < out->size(); i += 4)
        std::memcpy(&(*out)[i], bgPix, 4);
    return true;
}

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------

D2D1_COLOR_F Rgb(unsigned rgb, float alpha) {
    return D2D1::ColorF(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
                        static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                        static_cast<float>(rgb & 0xFF) / 255.0f, alpha);
}

Theme Theme::FromConfig(const Config& cfg) {
    Theme t;
    t.bg        = Rgb(cfg.bgColor, std::clamp(cfg.opacity / 255.0f, 0.0f, 1.0f));
    t.border    = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f);
    t.separator = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.07f);
    t.title     = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.55f);
    t.label     = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.70f);
    t.value     = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f);
    t.muted     = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.42f);
    t.accent    = Rgb(cfg.accentColor, 1.0f);
    t.track     = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.13f);
    t.warn      = D2D1::ColorF(1.00f, 0.63f, 0.24f, 1.0f);
    t.crit      = D2D1::ColorF(1.00f, 0.35f, 0.35f, 1.0f);
    return t;
}

D2D1_COLOR_F BarColorFor(const Theme& t, float fraction) {
    if (fraction >= 0.95f) return t.crit;
    if (fraction >= 0.85f) return t.warn;
    return t.accent;
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

std::vector<Hud::Item> Hud::BuildItems(float s) const {
    return BuildItemsFor(cfg_, snapshot_, s);
}

std::vector<Hud::Item> Hud::BuildItemsFor(const Config& cfg, const Snapshot& sn, float s) {
    std::vector<Item> items;

    const float fs   = std::max(6.0f, cfg.fontSize * s);
    const float rowH = fs * 1.32f;
    const float barH = std::max(4.0f, fs * 0.42f);
    const float gap  = 7.0f * s;

    if (cfg.showTitle) {
        Item it;
        it.kind     = Item::Kind::Title;
        it.label    = HostName();
        it.h        = fs * 1.55f;
        it.gapAfter = gap * 0.8f;
        items.push_back(it);
    }

    auto meter = [&](const std::wstring& label, const std::wstring& value, float frac,
                     bool bar, float valueW = 56.0f) {
        Item it;
        it.kind     = Item::Kind::Meter;
        it.label    = label;
        it.value    = value;
        it.fraction = std::clamp(frac, 0.0f, 1.0f);
        it.bar      = bar && cfg.showBars;
        it.valueW   = valueW;
        it.h        = rowH + (it.bar ? (barH + 5.0f * s) : 0.0f);
        it.gapAfter = gap;
        items.push_back(it);
    };

    auto meterLabel = [](const wchar_t* base, const std::wstring& extra) {
        std::wstring s = base;
        if (!extra.empty()) {
            s += L"  ";
            s += extra;
        }
        return s;
    };

    if (cfg.showCpu) {
        // "CPU  Intel(R) Core(TM) i5-6500 · 4C8T"
        std::wstring extra = sn.cpu.brand;
        if (sn.cpu.cores > 0) {
            wchar_t ct[24]{};
            ::swprintf(ct, 24, L"%uC%uT", sn.cpu.cores,
                       sn.cpu.threads ? sn.cpu.threads : sn.cpu.cores);
            if (!extra.empty()) extra += L" · ";
            extra += ct;
        }
        meter(meterLabel(L"CPU", extra), FmtPercent(sn.cpu.usage),
              static_cast<float>(sn.cpu.usage / 100.0), true);
    }

    if (cfg.showMemory)
        meter(meterLabel(L"RAM", sn.memory.desc), FmtPercent(sn.memory.percent),
              static_cast<float>(sn.memory.percent / 100.0), true);

    if (cfg.showGpu) {
        if (sn.gpu.hasUsage)
            meter(meterLabel(L"GPU", sn.gpu.name), FmtPercent(sn.gpu.usage),
                  static_cast<float>(sn.gpu.usage / 100.0), true);
        else
            meter(meterLabel(L"GPU", sn.gpu.name), L"N/A", 0.0f, false);
    }

    if (cfg.showVram) {
        if (sn.gpu.hasVram && sn.gpu.vramTotal > 0) {
            const float frac = static_cast<float>(static_cast<double>(sn.gpu.vramUsed) /
                                                  static_cast<double>(sn.gpu.vramTotal));
            meter(L"VRAM", FmtBytes(sn.gpu.vramUsed) + L" / " + FmtBytes(sn.gpu.vramTotal),
                  frac, true, 140.0f);
        } else {
            meter(L"VRAM", L"N/A", 0.0f, false);
        }
    }

    if (cfg.showDisk) {
        if (sn.disk.valid) {
            std::wstring label = L"DISK ";
            label += sn.disk.letter;
            label += L":";
            meter(label, FmtPercent(sn.disk.percent),
                  static_cast<float>(sn.disk.percent / 100.0), true);
        } else {
            meter(L"DISK", L"N/A", 0.0f, false);
        }
    }

    if (cfg.showNetwork) {
        Item it;
        it.kind     = Item::Kind::Net;
        it.label    = L"NET";
        it.sub1     = std::wstring(L"\x2193 ") + FmtSpeed(sn.network.downBps);
        it.sub2     = std::wstring(L"\x2191 ") + FmtSpeed(sn.network.upBps);
        it.h        = rowH + fs * 1.32f * 2.0f;
        it.gapAfter = gap;
        items.push_back(it);
    }

    if (cfg.showLanIp) {
        Item it;
        it.kind     = Item::Kind::Text;
        it.label    = L"LAN";
        it.value    = sn.network.ipv4.empty() ? std::wstring(L"--") : sn.network.ipv4;
        it.h        = rowH;
        it.gapAfter = 0.0f;
        items.push_back(it);
    }

    if (!items.empty()) items.back().gapAfter = 0.0f;
    return items;
}

// Shared format builder: same NO_WRAP (+ optional trailing alignment) setup
// everywhere, so width measurement and rendering always see identical text
// layout properties.
IDWriteTextFormat* Hud::MakeTextFormat(IDWriteFactory* dw, const std::wstring& family,
                                       float size, DWRITE_FONT_WEIGHT weight,
                                       bool trailing) {
    if (!dw) return nullptr;
    IDWriteTextFormat* fmt = nullptr;
    if (FAILED(dw->CreateTextFormat(family.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                    DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &fmt)) ||
        !fmt)
        return nullptr;
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (trailing) fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    return fmt;
}

// Single-line advance width of txt under fmt. NO_WRAP makes the layout box
// width irrelevant; 0 on empty input or failure.
float Hud::MeasureTextWidth(IDWriteFactory* dw, IDWriteTextFormat* fmt,
                            const std::wstring& txt) {
    if (!dw || !fmt || txt.empty()) return 0.0f;
    IDWriteTextLayout* lay = nullptr;
    if (FAILED(dw->CreateTextLayout(txt.c_str(), static_cast<UINT32>(txt.size()), fmt,
                                    100000.0f, 10000.0f, &lay)) ||
        !lay)
        return 0.0f;
    DWRITE_TEXT_METRICS m{};
    lay->GetMetrics(&m);
    lay->Release();
    return m.widthIncludingTrailingWhitespace;
}

// The single implementation of the value-zone split: the label zone is
// [pad, width-valueW] and the value zone [width-valueW, width-pad], so a row
// fits when width >= pad + labelW + gap + valueW + pad. The value part is
// max(zone floor, measured text + pad) -- one function shared by the panel
// width floor (ContentMinWidth) and the drawing path (RenderToTarget).
float Hud::ValueZoneWidth(IDWriteFactory* dw, IDWriteTextFormat* fmtValueR,
                          const std::wstring& value, float floorW,
                          float pad, float s) {
    return std::max(floorW, MeasureTextWidth(dw, fmtValueR, value) + pad + 2.0f * s);
}

float Hud::ContentMinWidth(const std::vector<Item>& items, IDWriteFactory* dw,
                           IDWriteTextFormat* fmtTitle, IDWriteTextFormat* fmtLabel,
                           IDWriteTextFormat* fmtValueR, float s, float pad) {
    const float gap = 6.0f * s;  // minimum visual gap between label and value zones
    float need = 0.0f;
    for (const Item& it : items) {
        float w = 0.0f;
        switch (it.kind) {
            case Item::Kind::Title:
                w = pad + MeasureTextWidth(dw, fmtTitle, it.label) + pad;
                break;
            case Item::Kind::Net:
                // Three full-width lines, all in the label format.
                w = pad + std::max({MeasureTextWidth(dw, fmtLabel, it.label),
                                    MeasureTextWidth(dw, fmtLabel, it.sub1),
                                    MeasureTextWidth(dw, fmtLabel, it.sub2)}) +
                    pad;
                break;
            case Item::Kind::Meter:
            case Item::Kind::Text:
            default: {
                const float floorW = (it.valueW > 0.0f ? it.valueW : 56.0f) * s;
                const float valueW =
                    ValueZoneWidth(dw, fmtValueR, it.value, floorW, pad, s);
                w = pad + MeasureTextWidth(dw, fmtLabel, it.label) + gap + valueW + pad;
                break;
            }
        }
        need = std::max(need, w);
    }
    return need;
}

// Settings-side width guard: same fonts, same items, same math as the
// renderer - one source of truth for "how wide must the panel be".
float Hud::RequiredPanelWidth(const Config& cfg, const Snapshot& snap, UINT dpi) {
    IDWriteFactory* dw = nullptr;
    if (FAILED(::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, kIID_IDWriteFactory,
                                     reinterpret_cast<IUnknown**>(&dw))) ||
        !dw)
        return 0.0f;

    const float s  = static_cast<float>(dpi) / 96.0f;
    const float fs = std::max(6.0f, cfg.fontSize * s);
    const std::wstring family =
        cfg.fontFamily.empty() ? std::wstring(L"Segoe UI") : cfg.fontFamily;

    IDWriteTextFormat* fmtTitle  = MakeTextFormat(dw, family, fs * 1.10f,
                                                  DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
    IDWriteTextFormat* fmtLabel  = MakeTextFormat(dw, family, fs,
                                                  DWRITE_FONT_WEIGHT_NORMAL, false);
    IDWriteTextFormat* fmtValueR = MakeTextFormat(dw, family, fs,
                                                  DWRITE_FONT_WEIGHT_SEMI_BOLD, true);
    const float need = ContentMinWidth(BuildItemsFor(cfg, snap, s), dw, fmtTitle, fmtLabel,
                                       fmtValueR, s, cfg.padding * s);

    if (fmtTitle) fmtTitle->Release();
    if (fmtLabel) fmtLabel->Release();
    if (fmtValueR) fmtValueR->Release();
    dw->Release();
    return need;
}

float Hud::LayoutItems(std::vector<Item>& items, float s, float width) const {
    (void)width;
    const float pad = cfg_.padding * s;
    float y = pad;
    for (auto& it : items) {
        it.y = y;
        y += it.h + it.gapAfter;
    }
    if (!items.empty()) y -= items.back().gapAfter;
    return y + pad;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void Hud::RenderToTarget(ID2D1RenderTarget* rt, const std::vector<Item>& items, float s,
                         float width, float height) {
    const Theme theme   = Theme::FromConfig(cfg_);
    const float pad     = cfg_.padding * s;
    const float radius  = std::max(0.0f, cfg_.cornerRadius * s);
    const float fs      = std::max(6.0f, cfg_.fontSize * s);
    const float lineH   = fs * 1.32f;
    const float barH    = std::max(4.0f, fs * 0.42f);
    const float right   = width - pad;

    for (int attempt = 0; attempt < 2; ++attempt) {
        // Create the underlay bitmap lazily against the CURRENT render
        // target: after a D2DERR_RECREATE_TARGET retry the target is new and
        // the old bitmap was discarded with it, so this must re-run per
        // attempt or the retried frame loses the wallpaper backdrop.
        if (!moveMode_ && !underlayBmp_ && !underlayPix_.empty() && underlayW_ > 0 &&
            underlayH_ > 0) {
            const D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f, 96.0f);
            const HRESULT hr = rt->CreateBitmap(D2D1_SIZE_U{static_cast<UINT>(underlayW_),
                                                            static_cast<UINT>(underlayH_)},
                                                underlayPix_.data(),
                                                static_cast<UINT32>(underlayW_) * 4, bp,
                                                &underlayBmp_);
            if (FAILED(hr)) {
                log::Warn(L"Hud: underlay bitmap creation failed: " + HresultString(hr));
                // Keep the pixels: the next frame retries the (cheap) bitmap
                // creation against the recovered target instead of paying a
                // full wallpaper re-decode for a target-side hiccup.
            }
        }

        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());

        if (underlayBmp_ && !moveMode_) {
            // Attached mode with underlay: transparent clear, then the opaque
            // wallpaper crop, then the panel on top with its configured
            // background alpha. The GDI surface ends up fully opaque
            // (BitBlt-safe) while LOOKING translucent.
            rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        } else if (opaqueFill_) {
            rt->Clear(Rgb(cfg_.bgColor, 1.0f));
        } else {
            rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        }

        ComPtr<ID2D1SolidColorBrush> mainBrush;
        ComPtr<ID2D1SolidColorBrush> accentBrush;
        ComPtr<ID2D1SolidColorBrush> trackBrush;

        const HRESULT bhr = rt->CreateSolidColorBrush(theme.value, mainBrush.put());
        rt->CreateSolidColorBrush(theme.accent, accentBrush.put());
        rt->CreateSolidColorBrush(theme.track, trackBrush.put());
        if (FAILED(bhr) || !mainBrush) {
            rt->EndDraw();
            return;
        }

        const D2D1_ROUNDED_RECT panel = D2D1::RoundedRect(
            D2D1::RectF(0.5f, 0.5f, width - 0.5f, height - 0.5f), radius, radius);

        // The wallpaper crop is drawn stretched over the whole panel; while a
        // resize is pending the old crop is approximately right and gets
        // replaced by the worker's fresh one within a few frames.
        if (underlayBmp_ && !moveMode_)
            rt->DrawBitmap(underlayBmp_, D2D1::RectF(0.0f, 0.0f, width, height));

        mainBrush->SetColor(theme.bg);
        rt->FillRoundedRectangle(panel, mainBrush);

        mainBrush->SetColor(theme.border);
        rt->DrawRoundedRectangle(panel, mainBrush, 1.0f);

        auto drawText = [&](const std::wstring& txt, IDWriteTextFormat* fmt,
                            const D2D1_RECT_F& r, const D2D1_COLOR_F& color) {
            if (!fmt || txt.empty()) return;
            mainBrush->SetColor(color);
            rt->DrawTextW(txt.c_str(), static_cast<UINT32>(txt.size()), fmt, r, mainBrush,
                          D2D1_DRAW_TEXT_OPTIONS_CLIP);
        };

        // Right-aligned values are NO_WRAP: text wider than its zone overflows
        // to the LEFT and is clipped there, losing the leading digits (a fixed
        // 140px VRAM zone showed "612.4 MB / 12.0 GB" as ".4 MB / 12.0 GB").
        // The zone rect is [width - w, width - pad], so w must cover the
        // measured text PLUS the pad; never shrink below the row's floor.
        // The math lives in ValueZoneWidth - the same function the panel
        // width floor (ContentMinWidth) uses.

        for (const Item& it : items) {
            switch (it.kind) {
                case Item::Kind::Title: {
                    const D2D1_RECT_F r = D2D1::RectF(pad, it.y, right, it.y + it.h);
                    drawText(it.label, fmtTitle_, r, theme.title);

                    mainBrush->SetColor(theme.separator);
                    const float ly = it.y + it.h - 3.0f * s;
                    rt->DrawLine(D2D1::Point2F(pad, ly), D2D1::Point2F(right, ly),
                                 mainBrush, 1.0f);
                    break;
                }

                case Item::Kind::Meter: {
                    // The label zone is most of the row - it carries hardware
                    // identity ("CPU  Intel(R) Core(TM) i5-6500 · 4C8T"), which
                    // is far longer than the right-aligned percentage. Rows
                    // with wider values (VRAM "11.3 GB / 12.0 GB") carry their
                    // own zone floor; ValueZoneWidth grows it to the real text.
                    const float valueW = ValueZoneWidth(
                        dwriteFactory_, fmtValueR_, it.value,
                        (it.valueW > 0.0f ? it.valueW : 56.0f) * s, pad, s);
                    const D2D1_RECT_F lr = D2D1::RectF(pad, it.y, width - valueW, it.y + lineH);
                    const D2D1_RECT_F vr = D2D1::RectF(width - valueW, it.y, right, it.y + lineH);
                    drawText(it.label, fmtLabel_, lr, theme.label);
                    drawText(it.value, fmtValueR_, vr, theme.value);

                    if (it.bar) {
                        const float top = it.y + lineH;
                        const float w   = right - pad;
                        const D2D1_ROUNDED_RECT track = D2D1::RoundedRect(
                            D2D1::RectF(pad, top, right, top + barH), barH * 0.5f,
                            barH * 0.5f);
                        rt->FillRoundedRectangle(track, trackBrush);

                        float fillW = w * it.fraction;
                        if (it.fraction > 0.0f) fillW = std::max(fillW, barH);
                        if (fillW > 0.0f) {
                            const D2D1_ROUNDED_RECT fill = D2D1::RoundedRect(
                                D2D1::RectF(pad, top, pad + fillW, top + barH),
                                barH * 0.5f, barH * 0.5f);
                            accentBrush->SetColor(BarColorFor(theme, it.fraction));
                            rt->FillRoundedRectangle(fill, accentBrush);
                        }
                    }
                    break;
                }

                case Item::Kind::Net: {
                    const D2D1_RECT_F lr = D2D1::RectF(pad, it.y, right, it.y + lineH);
                    drawText(it.label, fmtLabel_, lr, theme.title);
                    drawText(it.sub1, fmtLabel_,
                             D2D1::RectF(pad, it.y + lineH, right, it.y + lineH * 2.0f),
                             theme.value);
                    drawText(it.sub2, fmtLabel_,
                             D2D1::RectF(pad, it.y + lineH * 2.0f, right, it.y + lineH * 3.0f),
                             theme.value);
                    break;
                }

                case Item::Kind::Text:
                default: {
                    // Same zone split as Meter rows - the only difference is
                    // the value format's weight in older builds; a uniform
                    // floor keeps ContentMinWidth's math valid for both.
                    const float valueW = ValueZoneWidth(
                        dwriteFactory_, fmtValueR_, it.value,
                        (it.valueW > 0.0f ? it.valueW : 56.0f) * s, pad, s);
                    const D2D1_RECT_F lr = D2D1::RectF(pad, it.y, width - valueW, it.y + lineH);
                    const D2D1_RECT_F vr = D2D1::RectF(width - valueW, it.y, right, it.y + lineH);
                    drawText(it.label, fmtLabel_, lr, theme.label);
                    drawText(it.value, fmtValueR_, vr, theme.value);
                    break;
                }
            }
        }

        // Move-mode hint band along the bottom edge (Render() reserves the
        // extra height for it).
        if (moveMode_) {
            const float bandH   = lineH * 1.7f;
            const float bandTop = height - bandH;

            mainBrush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.50f));
            rt->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(0.5f, bandTop, width - 0.5f, height - 0.5f),
                                  radius, radius),
                mainBrush);

            const std::wstring hint = kMoveHint;
            mainBrush->SetColor(theme.accent);
            rt->DrawTextW(hint.c_str(), static_cast<UINT32>(hint.size()), fmtSmall_,
                          D2D1::RectF(pad, bandTop + bandH * 0.18f, right, height - 0.5f),
                          mainBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        const HRESULT hr = rt->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            DiscardDeviceResources();
            if (!EnsureBackBuffer(pixelWidth_, pixelHeight_)) return;
            rt = dcTarget_;
            if (!rt) return;
            continue;
        }
        if (FAILED(hr)) log::Warn(L"Hud: EndDraw failed: " + HresultString(hr));
        return;
    }
}

// ---------------------------------------------------------------------------
// Present
// ---------------------------------------------------------------------------

int Hud::EffectiveOpacity() const {
    return previewOpacity_ >= 0 ? previewOpacity_ : cfg_.opacity;
}

bool Hud::Present() {
    if (!hwnd_ || !memDC_ || !dib_) return true;

    if (ulwOk_) {
        POINT src{0, 0};
        SIZE  size{pixelWidth_, pixelHeight_};
        BLENDFUNCTION bf{};
        bf.BlendOp             = AC_SRC_OVER;
        bf.BlendFlags          = 0;
        bf.SourceConstantAlpha = 255;
        bf.AlphaFormat         = AC_SRC_ALPHA;

        // pptDst = nullptr => keep the position we already set via SetWindowPos,
        // which sidesteps parent-relative coordinate ambiguity for child windows.
        if (::UpdateLayeredWindow(hwnd_, nullptr, nullptr, &size, memDC_, &src, 0, &bf,
                                  ULW_ALPHA)) {
            return true;
        }

        const DWORD err = ::GetLastError();
        log::Warn(L"UpdateLayeredWindow unsupported here (" + FormatWinError(err) +
                  L") - using constant-alpha fallback");
        ulwOk_      = false;
        opaqueFill_ = true;
        return false;
    }

    // Constant-alpha fallback. Only meaningful while the window actually has
    // WS_EX_LAYERED (the move-mode popup when ULW failed there); the attached
    // desktop-layer child is intentionally non-layered and paints opaquely via
    // WM_PAINT, so skip the call that would fail silently every second.
    if (::GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) & WS_EX_LAYERED)
        ::SetLayeredWindowAttributes(hwnd_, 0, static_cast<BYTE>(EffectiveOpacity()),
                                     LWA_ALPHA);
    ::InvalidateRect(hwnd_, nullptr, FALSE);
    ::UpdateWindow(hwnd_);
    return true;
}

void Hud::PlaceWindow() {
    if (!hwnd_) return;

    const RECT mon = MonitorRectFor(cfg_.monitorIndex);
    POINT pt{mon.left + cfg_.x, mon.top + cfg_.y};

    // A config restored onto a smaller/other monitor (its own monitor
    // disconnected) can compute a rect that misses the target entirely -
    // invisible HUD, recoverable only through the settings window. Pull a
    // fully-off-monitor placement back inside; deliberate partial edge-hang
    // positions still survive (only a fully invisible rect is corrected).
    // Move mode is skipped: the user owns the position while dragging.
    if (!moveMode_ && pixelWidth_ > 0 && pixelHeight_ > 0 &&
        (pt.x + pixelWidth_ <= mon.left || pt.x >= mon.right ||
         pt.y + pixelHeight_ <= mon.top || pt.y >= mon.bottom)) {
        pt.x = (pixelWidth_ < mon.right - mon.left)
                   ? std::clamp(pt.x, mon.left, mon.right - pixelWidth_)
                   : mon.left;
        pt.y = (pixelHeight_ < mon.bottom - mon.top)
                   ? std::clamp(pt.y, mon.top, mon.bottom - pixelHeight_)
                   : mon.top;
    }

    HWND host = ::GetParent(hwnd_);
    if (host) ::MapWindowPoints(HWND_DESKTOP, host, &pt, 1);

    // Inside the desktop layer we want to sit immediately below the icons, so use
    // the same anchor as desktop::EnsureZOrder. Without a host the window is
    // still top-level, and HWND_BOTTOM would drop it *behind* the shell.
    HWND insertAfter = host ? desktop::ZOrderAnchor(host) : HWND_TOP;
    if (!insertAfter) insertAfter = HWND_TOP;

    // Only a window that lives in the desktop layer (or is being dragged as
    // a top-level popup) may be shown - a plain top-level HUD would float
    // over the desktop. Cheap parent check: the window only ever becomes a
    // child through desktop::Attach.
    const bool mayShow = visible_ && (moveMode_ || ::GetParent(hwnd_) != nullptr);
    const UINT flags = SWP_NOACTIVATE | (mayShow ? SWP_SHOWWINDOW : SWP_HIDEWINDOW);

    // The old rectangle is what the desktop layer needs to repaint once the
    // window vacates it (moved / resized / hidden).
    RECT oldRect{};
    const bool hadOld = (placedX_ != INT_MAX) &&
                        ::GetWindowRect(hwnd_, &oldRect) != FALSE;
    const bool moved = (pt.x != placedX_ || pt.y != placedY_ ||
                        pixelWidth_ != placedW_ || pixelHeight_ != placedH_);

    ::SetWindowPos(hwnd_, insertAfter, pt.x, pt.y, pixelWidth_, pixelHeight_, flags);

    // When the window actually changed its rectangle: (a) the desktop layer
    // shows stale pixels where it used to be - the wallpaper WorkerW never
    // repaints exposed areas on its own, but only when a previous on-screen
    // rectangle existed (the very first placement vacates nothing), and (b)
    // the underlay crop must be recomputed for the new rectangle.
    if (moved) {
        if (hadOld) VacateRect(oldRect);
        placedX_ = pt.x;
        placedY_ = pt.y;
        placedW_ = pixelWidth_;
        placedH_ = pixelHeight_;
        ScheduleUnderlay();
    }
}

Hud::UnderlayKey Hud::BuildUnderlayKey() const {
    UnderlayKey key;
    const desktop::WallpaperInfo wi = desktop::CurrentWallpaper(cfg_.monitorIndex);
    key.path  = wi.path;
    key.style = wi.style;
    key.bg    = wi.bg;
    key.stamp = FileStamp(key.path);
    key.mon   = MonitorRectFor(cfg_.monitorIndex);
    key.px    = key.mon.left + cfg_.x;
    key.py    = key.mon.top + cfg_.y;
    key.w     = pixelWidth_ > 0 ? pixelWidth_ : std::max(80, cfg_.width);
    // Mirror the effective height Render() uses (max(h, 24) floor): a crop
    // decoded for the raw configured height would be stretched over the
    // actually-drawn panel height and misalign against the real desktop.
    key.h     = cfg_.height > 0 ? std::max(cfg_.height, 24)
                                : (pixelHeight_ > 0 ? pixelHeight_ : 240);
    return key;
}

void Hud::ScheduleUnderlay() {
    if (!hwnd_ || moveMode_) return;

    const UnderlayKey key = BuildUnderlayKey();
    {
        std::lock_guard<std::mutex> lock(underlayMtx_);
        // A failed decode must not be swallowed by the key dedup: the render
        // fast path re-schedules "as fast as we render" exactly so a transient
        // failure (shell rewriting its wallpaper cache) recovers immediately.
        if (key == underlayKey_ && !underlayReqFailed_) return;
        underlayReqFailed_ = false;
    }
    underlayKey_ = key;

    // Setups without a decodable image (solid color / Spotlight without a
    // cache) and TILE/SPAN placements get a solid crop in the shell
    // background color from the worker (MakeSolidCrop) - the panel keeps its
    // configured alpha either way.

    if (!underlayThread_.joinable())
        underlayThread_ = std::thread(&Hud::UnderlayWorker, this);
    {
        std::lock_guard<std::mutex> lock(underlayMtx_);
        underlayReq_    = key;  // latest-wins: overwrite a not-yet-started job
        underlayHasReq_ = true;
    }
    underlayCv_.notify_one();
}

void Hud::UnderlayWorker() {
    const HRESULT comHr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    for (;;) {
        UnderlayKey req;
        {
            std::unique_lock<std::mutex> lock(underlayMtx_);
            underlayCv_.wait(lock, [this] { return underlayQuit_ || underlayHasReq_; });
            if (underlayQuit_) break;
            req = underlayReq_;
            underlayHasReq_ = false;
        }

        std::vector<std::uint8_t> pix;
        // No decodable image or TILE/SPAN placement: solid crop in the shell
        // background color so the panel keeps its translucency; otherwise
        // decode the actual wallpaper region.
        const bool solid = req.path.empty() || req.style == 1 || req.style == 5;
        if (!(solid ? MakeSolidCrop(req, &pix) : DecodeUnderlayCrop(req, &pix))) {
            log::Warn(L"Hud: wallpaper underlay decode failed: " + req.path);
            {
                std::lock_guard<std::mutex> lock(underlayMtx_);
                underlayReqFailed_ = true;  // lets ScheduleUnderlay re-queue this key
            }
            continue;  // keep the previous crop; the render cadence re-tries
        }

        HWND notify = nullptr;
        {
            std::lock_guard<std::mutex> lock(underlayMtx_);
            if (underlayQuit_) break;
            underlayIncoming_      = std::move(pix);
            underlayIncomingW_     = req.w;
            underlayIncomingH_     = req.h;
            underlayIncomingValid_ = true;
            notify = notifyWnd_;  // read under the lock: SetNotifyWindow may race
        }
        if (notify)
            ::PostMessageW(notify, kUnderlayReadyMsg, 0, 0);
    }
    // Pair only on success (S_FALSE included): uninitializing after a failed
    // init would decrement somebody else's apartment count.
    if (SUCCEEDED(comHr)) ::CoUninitialize();
}

void Hud::ApplyUnderlayResult() {
    std::vector<std::uint8_t> pix;
    int w = 0, h = 0;
    {
        std::lock_guard<std::mutex> lock(underlayMtx_);
        if (!underlayIncomingValid_) return;
        pix = std::move(underlayIncoming_);
        w   = underlayIncomingW_;
        h   = underlayIncomingH_;
        underlayIncomingValid_ = false;
        underlayReqFailed_     = false;
        underlayIncoming_.clear();
        underlayIncoming_.shrink_to_fit();
    }

    underlayPix_ = std::move(pix);
    underlayW_   = w;
    underlayH_   = h;
    if (underlayBmp_) {
        underlayBmp_->Release();
        underlayBmp_ = nullptr;  // recreated lazily against the render target
    }
    log::Debug(L"Hud: wallpaper underlay ready (" + FmtInt(w) + L"x" + FmtInt(h) + L")");
    RequestRepaint();
}

void Hud::Render() {
    if (!hwnd_) return;

    // Slideshow / spotlight: re-check the wallpaper state periodically so the
    // underlay follows the visible wallpaper. Key comparison makes the common
    // case a cheap no-op; a decode only happens when something changed.
    // Two fast paths poll EVERY render instead of every 60th:
    //   * right after our wallpaper replay the shell churns its cache chain
    //     for seconds (CachedFiles deleted + rewritten, desktop transiently
    //     on a different tier) - the crop must track it to the final state,
    //     or a ~6s "translucent edges" window follows a HUD move;
    //   * with no crop at all (decode raced a cache rewrite) every second
    //     of dark corners is visible - retry as fast as we render.
    if (!moveMode_ && (underlaySettle_ > 0 || underlayPix_.empty() ||
                       ++underlayCheck_ >= 60u)) {
        if (underlaySettle_ > 0) --underlaySettle_;
        underlayCheck_ = 0;
        ScheduleUnderlay();
    }

    RefreshDpi();
    const float s  = Scale();
    const float fs = std::max(6.0f, cfg_.fontSize * s);
    std::vector<Item> items = BuildItems(s);

    // Content-driven minimum width: never clip a label or a right-aligned
    // value, whatever the configured width. Move mode adds the hint band,
    // which is drawn full-width and must not clip either.
    float need = ContentMinWidth(items, dwriteFactory_, fmtTitle_, fmtLabel_, fmtValueR_,
                                 s, cfg_.padding * s);
    if (moveMode_)
        need = std::max(need, cfg_.padding * s +
                                  MeasureTextWidth(dwriteFactory_, fmtSmall_, kMoveHint) +
                                  cfg_.padding * s);

    // Grow instantly, shrink only after the smaller requirement has held for
    // five consecutive renders: value strings change length second to second
    // and an un-damped floor would make the panel breathe (each pixel of
    // width change also re-crops the wallpaper underlay on the desktop).
    if (need > widthFloor_ + 0.5f) {
        widthFloor_ = need;
        widthShrinkStreak_ = 0;
    } else if (need < widthFloor_ - 0.5f) {
        if (++widthShrinkStreak_ >= 5) {
            widthFloor_ = need;
            widthShrinkStreak_ = 0;
        }
    } else {
        widthShrinkStreak_ = 0;
    }

    const float width   = std::max({80.0f, static_cast<float>(cfg_.width), widthFloor_});
    const float content = LayoutItems(items, s, width);

    int h = cfg_.height > 0 ? cfg_.height : static_cast<int>(std::ceil(content));
    h = std::max(h, 24);
    if (moveMode_) h += static_cast<int>(std::ceil(fs * 1.32f * 1.7f));  // hint band

    pixelWidth_  = static_cast<int>(width);
    pixelHeight_ = h;

    if (!EnsureBackBuffer(pixelWidth_, pixelHeight_)) return;

    RenderToTarget(dcTarget_, items, s, static_cast<float>(pixelWidth_),
                   static_cast<float>(pixelHeight_));

    // Move mode: the user owns the position - updating the layered window
    // without pptDst keeps it, so the per-second snapshot repaint must not
    // snap the window back to the config position mid-drag.
    if (!moveMode_) PlaceWindow();

    if (!Present()) {
        // Just switched to the constant-alpha fallback: redraw opaquely and re-present.
        RenderToTarget(dcTarget_, items, s, static_cast<float>(pixelWidth_),
                       static_cast<float>(pixelHeight_));
        if (!moveMode_) PlaceWindow();
        Present();
    }
}

bool Hud::DumpToBmp(const std::wstring& path) {
    // No window involved: this exists so the renderer can be verified (and the
    // panel previewed) without an interactive desktop session.
    dpi_ = SystemDpi();
    if (!CreateDeviceIndependentResources()) {
        log::Error(L"Hud::DumpToBmp: device independent resources failed");
        return false;
    }

    const float s     = Scale();
    std::vector<Item> items = BuildItems(s);
    // One-shot render: apply the content width floor directly (no hysteresis).
    const float width = std::max({80.0f, static_cast<float>(cfg_.width),
                                  ContentMinWidth(items, dwriteFactory_, fmtTitle_, fmtLabel_,
                                                  fmtValueR_, s, cfg_.padding * s)});
    const float content = LayoutItems(items, s, width);

    int h = cfg_.height > 0 ? cfg_.height : static_cast<int>(std::ceil(content));
    h = std::max(h, 24);

    pixelWidth_  = static_cast<int>(width);
    pixelHeight_ = h;

    if (!EnsureBackBuffer(pixelWidth_, pixelHeight_)) return false;

    RenderToTarget(dcTarget_, items, s, static_cast<float>(pixelWidth_),
                   static_cast<float>(pixelHeight_));

    const bool ok = WriteBmp32(path, dibBits_, pixelWidth_, pixelHeight_);
    if (!ok) log::Error(L"Hud::DumpToBmp: could not write " + path);
    return ok;
}

}  // namespace auraui
