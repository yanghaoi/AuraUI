#pragma once

#include "monitor/snapshot.h"
#include "system/nvml_api.h"

#include <windows.h>

#include <string>

namespace auraui {

// GPU telemetry.
//
// * NVIDIA  -> NVML (dynamically loaded, no SDK needed): usage, VRAM, temp, power.
// * others  -> the adapter name is still reported via DXGI so the HUD can show
//              "Intel(R) UHD Graphics  N/A" instead of a blank row.
//
// The class never throws and never crashes when the driver is absent.
class GpuMonitor {
public:
    GpuMonitor();
    ~GpuMonitor();

    void Shutdown();

    // Called on the sampler thread.
    void Sample(GpuInfo& out);

private:
    void TryLoadNvml();
    void UnloadNvml();
    void QueryAdapterName();

    HMODULE            module_   = nullptr;
    nvml::Api          api_{};
    nvml::nvmlDevice_t device_   = nullptr;
    bool               nvmlUp_   = false;

    unsigned long long lastNvmlAttemptTick_ = 0;
    int                nvmlFailStreak_      = 0;
    // Consecutive dead samples (core calls failing / handle missing) before
    // the device is considered lost and NVML is unloaded for a fresh init.
    // Detection takes ~5s (1 Hz sampling); the re-init itself still respects
    // the 30s attempt throttle in TryLoadNvml.
    int                nvmlCoreFailStreak_  = 0;
    // NVML is up but reported zero adapters - nothing to recover from, so the
    // recovery path must not churn LoadLibrary/unload cycles on such systems.
    bool               nvmlNoDevice_        = false;

    bool         adapterQueried_ = false;
    std::wstring adapterName_;
};

}  // namespace auraui
