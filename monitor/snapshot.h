#pragma once

#include <cstdint>
#include <string>

namespace auraui {

// ---------------------------------------------------------------------------
// One immutable-ish sample of the machine. Produced on the sampler thread and
// consumed (by copy) on the UI thread.
// ---------------------------------------------------------------------------

struct CpuInfo {
    double usage = 0.0;  // 0..100

    // Static identity (filled on the first sample): cleaned brand string and
    // core/thread counts, e.g. "Intel(R) Core(TM) i5-6500" / 4 / 8.
    std::wstring brand;
    unsigned cores   = 0;
    unsigned threads = 0;
};

struct MemoryInfo {
    unsigned long long total     = 0;
    unsigned long long used      = 0;
    unsigned long long available = 0;
    double             percent   = 0.0;  // 0..100

    // Static identity (filled on the first sample), e.g. "DDR4 2133MHz 32GB".
    std::wstring desc;
};

struct GpuInfo {
    bool         present  = false;  // at least one display adapter was found
    bool         nvml     = false;  // NVIDIA NVML is loaded and healthy
    std::wstring name;

    bool   hasUsage = false;
    double usage    = 0.0;  // 0..100

    bool               hasVram   = false;
    unsigned long long vramTotal = 0;
    unsigned long long vramUsed  = 0;

    bool   hasTemp = false;
    double temperatureC = 0.0;

    bool   hasPower = false;
    double powerW   = 0.0;
};

struct DiskInfo {
    bool               valid   = false;
    wchar_t            letter  = L'C';
    unsigned long long total   = 0;
    unsigned long long free    = 0;
    unsigned long long used    = 0;
    double             percent = 0.0;
};

struct NetworkInfo {
    bool         connected = false;
    double       downBps   = 0.0;
    double       upBps     = 0.0;
    std::wstring adapter;   // friendly name
    std::wstring ipv4;
    std::wstring ipv6;
    std::wstring mac;
    std::wstring gateway;
};

struct Snapshot {
    CpuInfo     cpu;
    MemoryInfo  memory;
    GpuInfo     gpu;
    DiskInfo    disk;
    NetworkInfo network;

    unsigned long long sampleIndex = 0;
    double             elapsedSec  = 0.0;
};

}  // namespace auraui
