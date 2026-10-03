#include "monitor/memory.h"

#include <windows.h>

#include <algorithm>

namespace auraui {

void MemoryMonitor::Sample(MemoryInfo& out) {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (!::GlobalMemoryStatusEx(&ms)) {
        return;  // keep previous values
    }

    out.total     = ms.ullTotalPhys;
    out.available = ms.ullAvailPhys;
    out.used      = (ms.ullTotalPhys > ms.ullAvailPhys) ? ms.ullTotalPhys - ms.ullAvailPhys : 0;

    const double pct = (ms.ullTotalPhys == 0)
                           ? 0.0
                           : 100.0 * static_cast<double>(out.used) /
                                 static_cast<double>(ms.ullTotalPhys);
    out.percent = std::clamp(pct, 0.0, 100.0);
}

}  // namespace auraui
