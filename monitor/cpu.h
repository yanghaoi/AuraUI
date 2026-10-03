#pragma once

#include "monitor/snapshot.h"

namespace auraui {

// CPU usage from GetSystemTimes() deltas. The first Sample() only primes the
// baseline and returns 0.
class CpuMonitor {
public:
    void   Reset();
    double Sample();

private:
    bool               hasPrev_    = false;
    unsigned long long prevIdle_   = 0;
    unsigned long long prevKernel_ = 0;
    unsigned long long prevUser_   = 0;
    // Distinguishes "no value yet" from a genuine 0% reading (the previous
    // 0.0-default sentinel suppressed smoothing after real idle samples).
    bool               hasLast_    = false;
    double             last_       = 0.0;
};

}  // namespace auraui
