#include "system/gpu.h"

#include "core/log.h"
#include "core/win_util.h"

#include <dxgi.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace auraui {
namespace {

// Local copies of the two IIDs we need, so we do not depend on dxguid.
const GUID kIID_IDXGIFactory1 = {0x770aae78, 0xf26f, 0x4dba,
                                 {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
const GUID kIID_IDXGIAdapter1 = {0x29038f61, 0x3839, 0x4626,
                                 {0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05}};

constexpr unsigned long long kRetryIntervalMs = 30 * 1000;

// Consecutive dead samples tolerated before unloading NVML for a fresh init
// (1 Hz sampling => ~5s of N/A before a recovery attempt).
constexpr int kNvmlRecoverStreak = 5;

template <typename T>
T Resolve(HMODULE m, const char* name) {
    return reinterpret_cast<T>(reinterpret_cast<void*>(::GetProcAddress(m, name)));
}

}  // namespace

GpuMonitor::GpuMonitor() = default;

GpuMonitor::~GpuMonitor() { Shutdown(); }

void GpuMonitor::Shutdown() {
    // Identical to UnloadNvml(); kept as a separate name for the shutdown
    // call sites (dtor, Sampler::Stop). The throttle reset makes a
    // Stop->Start cycle attempt NVML immediately instead of waiting out the
    // 30s retry interval.
    UnloadNvml();
    lastNvmlAttemptTick_ = 0;
    nvmlFailStreak_      = 0;
}

void GpuMonitor::UnloadNvml() {
    if (nvmlUp_ && api_.shutdown) api_.shutdown();
    nvmlUp_ = false;
    device_ = nullptr;
    api_    = nvml::Api{};
    if (module_) {
        ::FreeLibrary(module_);
        module_ = nullptr;
    }
    nvmlCoreFailStreak_ = 0;
}

void GpuMonitor::TryLoadNvml() {
    const unsigned long long now = ::GetTickCount64();
    if (lastNvmlAttemptTick_ != 0 && now - lastNvmlAttemptTick_ < kRetryIntervalMs) return;

    UnloadNvml();
    // Stamp after the unload: UnloadNvml clears the tick (so a recovery
    // unload gets an immediate re-attempt), and a failed attempt must still
    // be throttled from here.
    lastNvmlAttemptTick_ = now;
    nvmlNoDevice_        = false;

    // Absolute paths only: avoids any DLL-planting risk from the working
    // directory and makes the search deterministic.
    std::vector<std::wstring> candidates;

    wchar_t sysRoot[MAX_PATH]{};
    if (::GetEnvironmentVariableW(L"SystemRoot", sysRoot, MAX_PATH))
        candidates.push_back(std::wstring(sysRoot) + L"\\System32\\nvml.dll");

    wchar_t pf[MAX_PATH]{};
    if (::GetEnvironmentVariableW(L"ProgramW6432", pf, MAX_PATH) ||
        ::GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH)) {
        candidates.push_back(std::wstring(pf) + L"\\NVIDIA Corporation\\NVSMI\\nvml.dll");
    }

    HMODULE m = nullptr;
    for (const auto& path : candidates) {
        m = ::LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (m) break;
    }
    if (!m) {
        ++nvmlFailStreak_;
        if (nvmlFailStreak_ == 1) log::Info(L"NVML not available (no NVIDIA driver)");
        return;
    }

    api_.init                = Resolve<nvml::PFN_Init>(m, "nvmlInit_v2");
    if (!api_.init) api_.init = Resolve<nvml::PFN_Init>(m, "nvmlInit");
    api_.shutdown            = Resolve<nvml::PFN_Shutdown>(m, "nvmlShutdown");
    api_.getCount            = Resolve<nvml::PFN_GetCount>(m, "nvmlDeviceGetCount_v2");
    if (!api_.getCount) api_.getCount = Resolve<nvml::PFN_GetCount>(m, "nvmlDeviceGetCount");
    api_.getHandleByIndex = Resolve<nvml::PFN_GetHandleByIndex>(m, "nvmlDeviceGetHandleByIndex_v2");
    if (!api_.getHandleByIndex)
        api_.getHandleByIndex = Resolve<nvml::PFN_GetHandleByIndex>(m, "nvmlDeviceGetHandleByIndex");
    api_.getName             = Resolve<nvml::PFN_GetName>(m, "nvmlDeviceGetName");
    api_.getUtilizationRates = Resolve<nvml::PFN_GetUtilizationRates>(m, "nvmlDeviceGetUtilizationRates");
    api_.getMemoryInfo       = Resolve<nvml::PFN_GetMemoryInfo>(m, "nvmlDeviceGetMemoryInfo");
    api_.getTemperature      = Resolve<nvml::PFN_GetTemperature>(m, "nvmlDeviceGetTemperature");
    api_.getPowerUsage       = Resolve<nvml::PFN_GetPowerUsage>(m, "nvmlDeviceGetPowerUsage");

    if (!api_.Complete()) {
        ::FreeLibrary(m);
        ++nvmlFailStreak_;
        log::Warn(L"nvml.dll loaded but required symbols are missing");
        return;
    }

    if (api_.init() != nvml::kSuccess) {
        ::FreeLibrary(m);
        ++nvmlFailStreak_;
        log::Warn(L"nvmlInit failed");
        return;
    }

    module_ = m;
    nvmlUp_ = true;

    unsigned int count = 0;
    if (api_.getCount(&count) == nvml::kSuccess) {
        if (count == 0) {
            // NVML up but no NVIDIA adapter: latch so the recovery path does
            // not LoadLibrary-churn on systems that will never have a device.
            nvmlNoDevice_ = true;
        } else if (api_.getHandleByIndex(0, &device_) != nvml::kSuccess) {
            device_ = nullptr;  // retried via the recovery path in Sample()
        }
    }
    log::Info(L"NVML initialised");
}

void GpuMonitor::QueryAdapterName() {
    adapterQueried_ = true;

    IDXGIFactory1* factory = nullptr;
    if (FAILED(::CreateDXGIFactory1(kIID_IDXGIFactory1,
                                    reinterpret_cast<void**>(&factory))) ||
        !factory)
        return;

    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (!adapter) break;

        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
            !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            if (!desc.Description[0]) {
                adapter->Release();
                continue;
            }
            adapterName_ = desc.Description;
            adapter->Release();
            break;
        }
        adapter->Release();
    }
    factory->Release();
}

void GpuMonitor::Sample(GpuInfo& out) {
    if (!adapterQueried_) QueryAdapterName();

    if (!nvmlUp_) TryLoadNvml();

    out = GpuInfo{};
    out.name = adapterName_;

    if (!nvmlUp_ || !device_) {
        // NVML is up but no usable device handle while one is expected: count
        // the dead samples and re-init after a few instead of latching "N/A"
        // until the app restarts (driver reset / TDR invalidates the handle).
        if (nvmlUp_ && !device_ && !nvmlNoDevice_ &&
            ++nvmlCoreFailStreak_ >= kNvmlRecoverStreak) {
            log::Info(L"NVML: no device handle - unloading for re-init");
            UnloadNvml();  // next sample re-enters TryLoadNvml immediately
        }
        out.present = !adapterName_.empty();
        out.nvml    = false;
        return;  // usage stays N/A; the HUD renders "N/A" instead of a bar
    }

    out.present = true;
    out.nvml    = true;

    // Prefer the NVML device name (DXGI may report the iGPU on hybrid laptops).
    if (api_.getName) {
        char buf[128]{};
        if (api_.getName(device_, buf, sizeof(buf)) == nvml::kSuccess && buf[0]) {
            out.name = Utf8ToWide(buf);
        }
    }

    nvml::Utilization util{};
    const bool utilOk = api_.getUtilizationRates(device_, &util) == nvml::kSuccess;
    if (utilOk) {
        out.hasUsage = true;
        out.usage    = std::clamp(static_cast<double>(util.gpu), 0.0, 100.0);
    }

    nvml::Memory mem{};
    const bool memOk = api_.getMemoryInfo(device_, &mem) == nvml::kSuccess;
    if (memOk && mem.total > 0) {
        out.hasVram   = true;
        out.vramTotal = mem.total;
        out.vramUsed  = mem.used;
    }

    // Both core calls dead in the same tick means NVML lost the device
    // (driver reset / TDR). After a few consecutive dead ticks, drop NVML
    // entirely; the next sample re-initializes with a fresh handle.
    if (!(utilOk || (memOk && mem.total > 0))) {
        if (++nvmlCoreFailStreak_ >= kNvmlRecoverStreak) {
            log::Info(L"NVML: device lost - unloading for re-init");
            UnloadNvml();
        }
    } else {
        nvmlCoreFailStreak_ = 0;
    }

    if (api_.getTemperature) {
        unsigned int t = 0;
        if (api_.getTemperature(device_, nvml::kTemperatureGpu, &t) == nvml::kSuccess) {
            out.hasTemp      = true;
            out.temperatureC = static_cast<double>(t);
        }
    }

    if (api_.getPowerUsage) {
        unsigned int mw = 0;
        if (api_.getPowerUsage(device_, &mw) == nvml::kSuccess) {
            out.hasPower = true;
            out.powerW   = static_cast<double>(mw) / 1000.0;
        }
    }
}

}  // namespace auraui
