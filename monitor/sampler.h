#pragma once

#include "monitor/cpu.h"
#include "monitor/disk.h"
#include "monitor/memory.h"
#include "monitor/network.h"
#include "monitor/snapshot.h"
#include "system/gpu.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace auraui {

// ---------------------------------------------------------------------------
// Background sampling thread.
//
// All Win32/WMI/NVML calls happen here so the UI thread only ever copies a
// Snapshot and paints it. The notify callback runs on this thread and is
// expected to be cheap (it posts a message to the UI thread).
// ---------------------------------------------------------------------------
class Sampler {
public:
    using NotifyFn = std::function<void()>;

    Sampler();
    ~Sampler();

    void SetNotify(NotifyFn fn);

    void Start(int intervalMs);
    void Stop();

    void SetInterval(int intervalMs);

    void SetPaused(bool paused);
    bool Paused() const { return paused_.load(std::memory_order_relaxed); }

    void InvalidateNetwork();

    // Thread-safe copy of the most recent sample.
    Snapshot Get() const;

private:
    void Loop();
    void SampleOnce();

    mutable std::mutex mutex_;
    Snapshot           snapshot_;

    std::thread             worker_;
    std::condition_variable cv_;
    std::mutex              waitMutex_;
    bool                    stopping_ = false;
    bool                    wake_     = false;
    int                     intervalMs_ = 1000;

    std::atomic<bool> paused_{false};

    NotifyFn notify_;

    CpuMonitor     cpu_;
    MemoryMonitor  memory_;
    DiskMonitor    disk_;
    NetworkMonitor network_;
    GpuMonitor     gpu_;

    unsigned long long sampleIndex_ = 0;
    unsigned long long lastTick_    = 0;
};

}  // namespace auraui
