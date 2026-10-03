#pragma once

#include "monitor/snapshot.h"

#include <atomic>
#include <string>
#include <vector>

namespace auraui {

// Network throughput (GetIfTable2 64-bit octet counters) + adapter details
// (GetAdaptersAddresses: IPv4/IPv6/MAC/gateway). Fully offline - no external
// service is ever contacted.
class NetworkMonitor {
public:
    NetworkMonitor();
    ~NetworkMonitor();

    // elapsedSeconds = time since the previous Sample() call.
    void Sample(NetworkInfo& out, double elapsedSeconds);

    // Force a re-enumeration of adapters (used on WM_DEVICECHANGE / resume).
    void Invalidate() { adaptersDirty_ = true; }

private:
    struct AdapterInfo {
        std::wstring name;
        std::wstring description;
        std::wstring mac;
        std::wstring ipv4;
        std::wstring ipv6;
        std::wstring gateway;
        unsigned long long luid      = 0;
        unsigned long       ifIndex  = 0;
        bool                hasGateway = false;
    };

    void RefreshAdapters(bool force);

    std::vector<AdapterInfo> adapters_;
    int                selected_        = -1;
    unsigned long long lastRefreshTick_ = 0;
    // Set by the UI thread (WM_DEVICECHANGE / resume), cleared by the
    // sampler thread - atomic because the sampler runs outside the mutex
    // the setter holds (the vector itself is only ever touched on the
    // sampler thread).
    std::atomic<bool> adaptersDirty_{true};

    bool               hasPrev_ = false;
    unsigned long long prevIn_  = 0;
    unsigned long long prevOut_ = 0;
    // Which adapter the baseline octets belong to: switching the monitored
    // adapter must re-prime the delta, not subtract across two interfaces.
    unsigned long long prevLuid_ = 0;
};

}  // namespace auraui
