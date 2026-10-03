#pragma once

// ---------------------------------------------------------------------------
// Minimal NVML (NVIDIA Management Library) ABI subset.
//
// The NVIDIA SDK is intentionally NOT a build dependency: nvml.dll is loaded
// with LoadLibraryW() at runtime and every entry point is resolved with
// GetProcAddress(). On machines without an NVIDIA GPU (or without the driver)
// the application simply reports "N/A" and keeps running.
// ---------------------------------------------------------------------------

namespace auraui::nvml {

using nvmlReturn_t = int;
using nvmlDevice_t = void*;

constexpr nvmlReturn_t kSuccess = 0;

struct Utilization {
    unsigned int gpu    = 0;
    unsigned int memory = 0;
};

struct Memory {
    unsigned long long total = 0;
    unsigned long long free  = 0;
    unsigned long long used  = 0;
};

enum TemperatureSensor : int { kTemperatureGpu = 0 };

// Entry point signatures (classic ABI).
using PFN_Init                 = nvmlReturn_t (*)();
using PFN_Shutdown             = nvmlReturn_t (*)();
using PFN_GetCount             = nvmlReturn_t (*)(unsigned int*);
using PFN_GetHandleByIndex     = nvmlReturn_t (*)(unsigned int, nvmlDevice_t*);
using PFN_GetName              = nvmlReturn_t (*)(nvmlDevice_t, char*, unsigned int);
using PFN_GetUtilizationRates  = nvmlReturn_t (*)(nvmlDevice_t, Utilization*);
using PFN_GetMemoryInfo        = nvmlReturn_t (*)(nvmlDevice_t, Memory*);
using PFN_GetTemperature       = nvmlReturn_t (*)(nvmlDevice_t, int, unsigned int*);
using PFN_GetPowerUsage        = nvmlReturn_t (*)(nvmlDevice_t, unsigned int*);

// Resolved symbol table. Any member may be null.
struct Api {
    PFN_Init                init                 = nullptr;
    PFN_Shutdown            shutdown             = nullptr;
    PFN_GetCount            getCount             = nullptr;
    PFN_GetHandleByIndex    getHandleByIndex     = nullptr;
    PFN_GetName             getName              = nullptr;
    PFN_GetUtilizationRates getUtilizationRates  = nullptr;
    PFN_GetMemoryInfo       getMemoryInfo        = nullptr;
    PFN_GetTemperature      getTemperature       = nullptr;
    PFN_GetPowerUsage       getPowerUsage        = nullptr;

    bool Complete() const {
        return init && shutdown && getCount && getHandleByIndex && getUtilizationRates &&
               getMemoryInfo;
    }
};

}  // namespace auraui::nvml
