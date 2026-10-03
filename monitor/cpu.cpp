#include "monitor/cpu.h"

#include <windows.h>

#include <algorithm>

namespace auraui {
namespace {

unsigned long long ToU64(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

}  // namespace

void CpuMonitor::Reset() {
    hasPrev_    = false;
    prevIdle_   = prevKernel_ = prevUser_ = 0;
    hasLast_    = false;
    last_       = 0.0;
}

double CpuMonitor::Sample() {
    FILETIME idle{}, kernel{}, user{};
    if (!::GetSystemTimes(&idle, &kernel, &user)) {
        return last_;
    }

    const unsigned long long i = ToU64(idle);
    const unsigned long long k = ToU64(kernel);  // kernel includes idle time
    const unsigned long long u = ToU64(user);

    if (!hasPrev_) {
        prevIdle_ = i;
        prevKernel_ = k;
        prevUser_ = u;
        hasPrev_ = true;
        return last_;
    }

    const unsigned long long dIdle   = i - prevIdle_;
    const unsigned long long dKernel = k - prevKernel_;
    const unsigned long long dUser   = u - prevUser_;

    prevIdle_   = i;
    prevKernel_ = k;
    prevUser_   = u;

    // Total processor time = kernel (incl. idle) + user.
    const unsigned long long total = dKernel + dUser;
    if (total == 0) return last_;

    const double busy = static_cast<double>(total) - static_cast<double>(dIdle);
    double pct = 100.0 * busy / static_cast<double>(total);
    pct = std::clamp(pct, 0.0, 100.0);

    // Light smoothing: raw GetSystemTimes deltas can be jumpy at short intervals.
    last_    = hasLast_ ? (last_ * 0.35 + pct * 0.65) : pct;
    hasLast_ = true;
    return last_;
}

}  // namespace auraui
