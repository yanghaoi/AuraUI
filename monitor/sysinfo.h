#pragma once

#include <string>

namespace auraui::sysinfo {

// Static hardware identity, queried once per process (it does not change while
// running). Cheap to call repeatedly: the work happens on first use.

// Cleaned CPU brand string, e.g. "Intel(R) Core(TM) i5-6500"
// (the registry carries "... CPU @ 3.20GHz" - the clock suffix and the word
// "CPU" are stripped). Empty when unavailable.
const std::wstring& CpuBrand();

// Physical cores / logical processors (SMT), e.g. 4 / 8. 0 when unknown.
unsigned CpuCores();
unsigned CpuThreads();

// RAM description as shown on the HUD row, e.g. "DDR4 2133MHz 32GB".
// totalPhysBytes is the OS-visible capacity (GlobalMemoryStatusEx) and is the
// AUTHORITY for the GB figure - SMBIOS Type 17 tables vary between vendors
// (shifted layouts, missing extended-size fields) and are trusted only for
// the module type and clock, with sanity checks and a diagnostic log.
// Call with 0 to fall back to an SMBIOS-only sum.
const std::wstring& RamDesc(unsigned long long totalPhysBytes);

}  // namespace auraui::sysinfo
