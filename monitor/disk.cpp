#include "monitor/disk.h"

#include "core/log.h"

#include <windows.h>

#include <algorithm>

namespace auraui {
namespace {

wchar_t SystemDriveLetter() {
    wchar_t winDir[MAX_PATH]{};
    const UINT n = ::GetWindowsDirectoryW(winDir, MAX_PATH);
    if (n >= 2 && n < MAX_PATH && winDir[1] == L':') return winDir[0];
    return L'C';
}

}  // namespace

DiskMonitor::DiskMonitor() { Reset(); }

void DiskMonitor::Reset() {
    letter_ = SystemDriveLetter();
    root_.assign({letter_, L':', L'\\'});
}

void DiskMonitor::SetDrive(wchar_t letter) {
    if (letter >= L'a' && letter <= L'z') letter = static_cast<wchar_t>(letter - L'a' + L'A');
    letter_ = letter;
    root_.assign({letter_, L':', L'\\'});
}

void DiskMonitor::Sample(DiskInfo& out) {
    if (root_.empty()) Reset();

    ULARGE_INTEGER freeToCaller{}, total{}, totalFree{};
    if (!::GetDiskFreeSpaceExW(root_.c_str(), &freeToCaller, &total, &totalFree)) {
        // Volume vanished (or no permission). Report it as unavailable instead of crashing.
        out.valid = false;
        out.letter = letter_;
        return;
    }

    out.valid   = true;
    out.letter  = letter_;
    out.total   = total.QuadPart;
    out.free    = totalFree.QuadPart;
    out.used    = (total.QuadPart > totalFree.QuadPart) ? total.QuadPart - totalFree.QuadPart : 0;

    const double pct = (total.QuadPart == 0)
                           ? 0.0
                           : 100.0 * static_cast<double>(out.used) /
                                 static_cast<double>(total.QuadPart);
    out.percent = std::clamp(pct, 0.0, 100.0);
}

}  // namespace auraui
