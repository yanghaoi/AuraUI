#pragma once

#include "monitor/snapshot.h"

namespace auraui {

// Physical memory via GlobalMemoryStatusEx().
class MemoryMonitor {
public:
    void Sample(MemoryInfo& out);
};

}  // namespace auraui
