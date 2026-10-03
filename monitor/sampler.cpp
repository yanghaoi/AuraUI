#include "monitor/sampler.h"

#include "core/log.h"
#include "monitor/sysinfo.h"

#include <windows.h>

#include <algorithm>
#include <chrono>

namespace auraui {

Sampler::Sampler() = default;

Sampler::~Sampler() { Stop(); }

void Sampler::SetNotify(NotifyFn fn) {
    std::lock_guard<std::mutex> lock(waitMutex_);
    notify_ = std::move(fn);
}

void Sampler::Start(int intervalMs) {
    Stop();
    {
        std::lock_guard<std::mutex> lock(waitMutex_);
        intervalMs_ = std::max(50, intervalMs);
        stopping_   = false;
        wake_       = false;
    }
    lastTick_ = 0;
    cpu_.Reset();
    disk_.Reset();
    // Synchronous first sample: fills RAM/disk/hardware identity immediately
    // and primes the delta baselines (CPU/network), so the first painted
    // frame - and a HUD that starts paused, which never receives tick
    // messages - carries real values instead of zeros. Delta metrics stay
    // 0% until the next tick by nature.
    SampleOnce();
    worker_ = std::thread([this] { Loop(); });
}

void Sampler::Stop() {
    {
        std::lock_guard<std::mutex> lock(waitMutex_);
        if (worker_.joinable()) {
            stopping_ = true;
            wake_     = true;
        }
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    // Unconditional (idempotent - see GpuMonitor::Shutdown): an early return
    // for a never-started worker used to skip it, leaking the release to the
    // destructor and making Stop() alone an incomplete teardown.
    gpu_.Shutdown();
}

void Sampler::SetInterval(int intervalMs) {
    {
        std::lock_guard<std::mutex> lock(waitMutex_);
        intervalMs_ = std::max(50, intervalMs);
        wake_       = true;
    }
    cv_.notify_all();
}

void Sampler::SetPaused(bool paused) {
    paused_.store(paused, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(waitMutex_);
        wake_ = true;
    }
    cv_.notify_all();
}

void Sampler::InvalidateNetwork() {
    std::lock_guard<std::mutex> lock(waitMutex_);
    network_.Invalidate();
}

Snapshot Sampler::Get() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void Sampler::SampleOnce() {
    const unsigned long long now = ::GetTickCount64();
    double elapsed = 0.0;
    if (lastTick_ != 0) elapsed = static_cast<double>(now - lastTick_) / 1000.0;
    lastTick_ = now;

    Snapshot s;
    s.sampleIndex = ++sampleIndex_;
    s.elapsedSec  = elapsed;

    s.cpu.usage = cpu_.Sample();
    memory_.Sample(s.memory);
    disk_.Sample(s.disk);
    network_.Sample(s.network, elapsed);
    gpu_.Sample(s.gpu);

    // Static hardware identity rides along on every sample (the lookups are
    // cached inside sysinfo - this is just two short string copies).
    s.cpu.brand   = sysinfo::CpuBrand();
    s.cpu.cores   = sysinfo::CpuCores();
    s.cpu.threads = sysinfo::CpuThreads();
    // memory_.Sample() above has filled s.memory.total - the OS-authoritative
    // capacity RamDesc uses for the GB figure.
    s.memory.desc = sysinfo::RamDesc(s.memory.total);
    if (s.sampleIndex == 1) {
        log::Info(L"HW: cpu=\"" + s.cpu.brand + L"\" " +
                  std::to_wstring(s.cpu.cores) + L"C" +
                  std::to_wstring(s.cpu.threads) + L"T  ram=\"" + s.memory.desc +
                  L"\"");
    }

    NotifyFn notify;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = s;
    }
    {
        std::lock_guard<std::mutex> lock(waitMutex_);
        notify = notify_;
    }
    if (notify) notify();
}

void Sampler::Loop() {
    ::SetThreadDescription(::GetCurrentThread(), L"AuraUI-Sampler");

    std::unique_lock<std::mutex> lock(waitMutex_);
    // Absolute cadence: the next tick fires intervalMs after the previous
    // TICK, not after the (sometimes slow) sampling run - a relative back-off
    // would drift by the sampling duration on every round.
    auto next = std::chrono::steady_clock::now() + std::chrono::milliseconds(intervalMs_);
    while (!stopping_) {
        cv_.wait_until(lock, next, [this] { return stopping_ || wake_; });
        if (stopping_) break;

        const bool wasWoken = wake_;
        wake_ = false;

        if (paused_.load(std::memory_order_relaxed)) {
            // Still wake up on demand (interval/pause changes), but skip
            // sampling - and re-anchor the cadence so the wait never spins
            // on an already-past deadline.
            next = std::chrono::steady_clock::now() +
                   std::chrono::milliseconds(intervalMs_);
            continue;
        }

        lock.unlock();
        SampleOnce();
        lock.lock();

        if (wasWoken) {
            // Interval changed under us: restart the cadence at the new rate.
            next = std::chrono::steady_clock::now() +
                   std::chrono::milliseconds(intervalMs_);
        } else {
            next += std::chrono::milliseconds(intervalMs_);
            // Fell behind (slow sample, suspended machine): skip the missed
            // ticks instead of bursting through them.
            const auto now = std::chrono::steady_clock::now();
            if (next <= now) next = now + std::chrono::milliseconds(intervalMs_);
        }
    }
}

}  // namespace auraui
