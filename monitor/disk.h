#pragma once

#include "monitor/snapshot.h"

#include <string>

namespace auraui {

// Free/used space of one volume via GetDiskFreeSpaceExW().
// Defaults to the volume that hosts Windows.
class DiskMonitor {
public:
    DiskMonitor();

    void SetDrive(wchar_t letter);   // 'C'
    wchar_t Drive() const { return letter_; }

    // Re-resolve the drive if it disappeared (e.g. removable media removed).
    void Reset();

    void Sample(DiskInfo& out);

private:
    wchar_t     letter_ = L'C';
    std::wstring root_;  // "C:\\"
};

}  // namespace auraui
