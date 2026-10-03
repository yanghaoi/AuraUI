#include "monitor/sysinfo.h"

#include "core/log.h"

#include <windows.h>

#include <cctype>
#include <cstdint>
#include <utility>
#include <vector>

namespace auraui::sysinfo {
namespace {

// ---------------------------------------------------------------------------
// CPU identity: the registry view the HAL populates at boot.
//   HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0
//     ProcessorNameString        "Intel(R) Core(TM) i5-6500 CPU @ 3.20GHz"
//     NumberOfCores / NumberOfLogicalProcessors (DWORD)
// ---------------------------------------------------------------------------

std::wstring CleanCpuBrand(const std::wstring& raw) {
    std::wstring s = raw;
    // Drop the clock suffix (" @ 3.20GHz") - it duplicates what the CPU row's
    // usage/uptime context does not need, and it is the longest part.
    const size_t at = s.find(L" @ ");
    if (at != std::wstring::npos) s.resize(at);
    // Drop a bare "CPU" / "Processor" / "APU" token (standalone word only -
    // "Core", "Ryzen" etc. must survive).
    const std::wstring drop[] = {L"CPU", L"Processor", L"APU", L"with Radeon Graphics"};
    for (const auto& w : drop) {
        const size_t p = s.find(w);
        if (p != std::wstring::npos) {
            const bool leftOk  = (p == 0) || s[p - 1] == L' ';
            const size_t end   = p + w.size();
            const bool rightOk = (end == s.size()) || s[end] == L' ';
            if (leftOk && rightOk) s.erase(p, w.size());
        }
    }
    // Collapse runs of spaces left by the removals.
    std::wstring out;
    out.reserve(s.size());
    bool space = false;
    for (wchar_t c : s) {
        if (c == L' ') {
            space = !out.empty();
        } else {
            if (space) out += L' ';
            space = false;
            out += c;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// RAM identity: raw SMBIOS via GetSystemFirmwareTable("RSMB"). Type 17 records
// (Memory Device) carry the form factor, module type, clock and size. Pure
// Win32, no WMI dependency - parsed once.
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
struct RawSmbiosHeader {
    BYTE  used20CallMethod;
    BYTE  smbiosMajorVersion;
    BYTE  smbiosMinorVersion;
    BYTE  dmiRevision;
    DWORD length;
};
#pragma pack(pop)

struct SmbiosView {
    const BYTE* data = nullptr;
    size_t      size = 0;
};

// Advance to the next structure: formatted part + double-NUL-terminated
// strings. The length byte at offset 1 covers the whole formatted area
// INCLUDING the 4-byte header, so the strings start at p + len - not 4 past it.
const BYTE* NextStructure(const BYTE* p, const BYTE* end) {
    if (p + 4 > end) return end;
    const BYTE len = p[1];
    const BYTE* s = p + len;  // string area
    while (s + 1 < end && !(s[0] == 0 && s[1] == 0)) ++s;
    return (s + 1 < end) ? s + 2 : end;
}

const wchar_t* MemoryTypeName(BYTE type) {
    switch (type) {
        case 0x12: return L"DDR";
        case 0x13: return L"DDR2";
        case 0x18: return L"DDR3";
        case 0x1A: return L"DDR4";
        case 0x22: return L"DDR5";
        case 0x1D: return L"LPDDR3";
        case 0x1E: return L"LPDDR4";
        case 0x23: return L"LPDDR5";
        default:   return nullptr;  // Other / Unknown / exotic
    }
}

std::wstring BuildRamDesc(unsigned long long totalPhysBytes) {
    // ---------------------------------------------------------------------------
    // Defense in depth (field reports: vendor SMBIOS tables produced 0GB/16GB
    // on real 64GB/32GB machines):
    //   * the CAPACITY always comes from the OS (GlobalMemoryStatusEx, passed
    //     in by the sampler) - rounded to the nearest GiB so 64GB physical
    //     reads "64GB" despite reserved-memory holes;
    //   * SMBIOS is trusted only for type and clock, every field sanity-clamped;
    //   * the SMBIOS capacity sum is kept purely as a cross-check for the
    //     diagnostic log (module list + sum vs OS total).
    // ---------------------------------------------------------------------------

    unsigned long long smbiosBytes = 0;
    unsigned           speedMHz    = 0;
    const wchar_t*     typeName    = nullptr;
    unsigned           modules     = 0;

    // GetSystemFirmwareTable wants the signature as a DWORD ('RSMB').
    const DWORD rsmb = 0x52534D42;
    DWORD needed = ::GetSystemFirmwareTable(rsmb, 0, nullptr, 0);
    std::vector<BYTE> buf(needed ? needed : 1);
    if (needed >= sizeof(RawSmbiosHeader) &&
        ::GetSystemFirmwareTable(rsmb, 0, buf.data(), needed) == needed) {
        const auto* hdr = reinterpret_cast<const RawSmbiosHeader*>(buf.data());
        const BYTE* p   = buf.data() + sizeof(RawSmbiosHeader);
        // RawSMBIOSData = 8-byte header + Length bytes of table (the API
        // returns 8 + Length). Scan the FULL table - an end of
        // `buf.data() + hdr->length` stopped 8 bytes short, silently dropping
        // the tail (end-of-table entry / last Type 17 record) - and clamp the
        // length to what was actually returned so a corrupt value cannot push
        // `end` past the buffer.
        DWORD tableLen = hdr->length;
        if (tableLen > needed - sizeof(RawSmbiosHeader))
            tableLen = needed - sizeof(RawSmbiosHeader);
        const BYTE* end = p + tableLen;

        while (p + 4 <= end) {
            const BYTE type = p[0];
            const BYTE len  = p[1];
            if (type == 17 && len >= 0x15) {
                // Type 17 (Memory Device):
                //   0x0C size word (0x7FFF => see 0x1C extended dword)
                //   0x11 memory type byte, 0x14 speed word,
                //   0x20 configured speed word (structure length >= 0x22)
                // Note: Size bit15=1 means KB granularity (pre-32MB modules).
                // It is deliberately read as MB anyway: nothing modern emits
                // it, the sum only feeds the diagnostic log / cross-check,
                // and the displayed capacity is OS-authoritative either way.
                const WORD sizeWord = p[0x0C] | (p[0x0D] << 8);
                unsigned long long moduleBytes = 0;
                if (sizeWord != 0 && sizeWord != 0x7FFF) {
                    moduleBytes = static_cast<unsigned long long>(sizeWord & 0x7FFF) * 1024 * 1024;
                } else if (len >= 0x20) {
                    const DWORD ext = p[0x1C] | (p[0x1D] << 8) | (p[0x1E] << 16) |
                                      (static_cast<DWORD>(p[0x1F]) << 24);
                    if (ext != 0) moduleBytes = ext * 1024ull * 1024ull;
                }
                // A real DIMM is 128MB..2TiB; anything else is a table quirk.
                const unsigned long long kMinModule = 128ull * 1024 * 1024;
                const unsigned long long kMaxModule = 2ull << 40;
                if (moduleBytes >= kMinModule && moduleBytes <= kMaxModule) {
                    ++modules;
                    smbiosBytes += moduleBytes;

                    // Memory-type byte hunt: the spec puts it at 0x11, but some
                    // tables (observed: an SMBIOS "8.0" vendor table) insert one
                    // extra byte after Form Factor, pushing the real type to
                    // 0x12 while 0x11 reads Other/Unknown. Shift only when 0x11
                    // is explicitly Other(01)/Unknown(02)/unset and 0x12 holds a
                    // specific known type.
                    BYTE typeOfs = 0x11;
                    const BYTE t11 = p[0x11];
                    if ((t11 == 0x00 || t11 == 0x01 || t11 == 0x02) && len > 0x12 &&
                        MemoryTypeName(p[0x12]))
                        typeOfs = 0x12;
                    if (!typeName) typeName = MemoryTypeName(p[typeOfs]);

                    // Speed sits at typeOfs + 3 (type, 2-byte detail, speed) in
                    // both layouts; configured speed stays at 0x20. Sanity
                    // clamp: anything outside 400..9600 MHz is not a clock.
                    auto wordAt = [&](BYTE o) -> unsigned {
                        return p[o] | (p[o + 1] << 8);
                    };
                    unsigned s = 0;
                    if (len > typeOfs + 4) {
                        const unsigned v = wordAt(typeOfs + 3);
                        if (v >= 400 && v <= 9600) s = v;
                    }
                    if (len >= 0x22) {
                        const unsigned cfg = wordAt(0x20);
                        if (cfg >= 400 && cfg <= 9600) s = cfg;  // what runs
                    }
                    if (s) speedMHz = (speedMHz == 0) ? s : speedMHz;
                }
            }
            p = NextStructure(p, end);
            if (p == end) break;
        }
    }

    // Capacity: the OS value is authoritative; nearest-GiB rounding absorbs
    // the reserved-memory hole (63.9GiB -> "64GB"). Without an OS value (never
    // in practice) fall back to the SMBIOS sum, but only if it is sane.
    unsigned long long gb = 0;
    bool fromSmbios = false;
    if (totalPhysBytes >= 1024ull * 1024 * 1024) {
        const unsigned long long GiB = 1024ull * 1024 * 1024;
        gb = (totalPhysBytes + GiB / 2) / GiB;
    } else if (smbiosBytes >= 1024ull * 1024 * 1024) {
        gb = smbiosBytes / (1024ull * 1024 * 1024);
        fromSmbios = true;
    }

    log::Info(L"sysinfo: ram modules=" + std::to_wstring(modules) + L" smbiosSum=" +
              std::to_wstring(static_cast<unsigned>(smbiosBytes / (1024ull * 1024))) +
              L"MB osTotal=" +
              std::to_wstring(static_cast<unsigned>(totalPhysBytes / (1024ull * 1024))) +
              L"MB type=" + (typeName ? typeName : L"?") + L" speed=" +
              std::to_wstring(speedMHz) + L"MHz" +
              (fromSmbios ? L" (capacity from smbios!)" : L""));

    if (gb == 0) return {};  // neither source knew - HUD shows plain "RAM"

    wchar_t out[96]{};
    if (typeName && speedMHz)
        ::swprintf(out, 96, L"%ls %uMHz %lluGB", typeName, speedMHz, gb);
    else if (typeName)
        ::swprintf(out, 96, L"%ls %lluGB", typeName, gb);
    else
        ::swprintf(out, 96, L"%lluGB", gb);
    return out;
}

}  // namespace

// Core/thread counts via GetLogicalProcessorInformation: the registry has no
// reliable per-core keys. Count RelationProcessorCore entries (physical cores)
// and sum the bits of each core's affinity mask (logical processors).
static void CountTopology(unsigned* cores, unsigned* threads) {
    *cores = *threads = 0;
    DWORD bytes = 0;
    ::GetLogicalProcessorInformation(nullptr, &bytes);
    if (bytes == 0) return;
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> info(
        bytes / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    if (!::GetLogicalProcessorInformation(info.data(), &bytes)) return;
    for (const auto& e : info) {
        if (e.Relationship != RelationProcessorCore) continue;
        ++*cores;
        ULONG_PTR mask = e.ProcessorMask;
        while (mask) {
            *threads += static_cast<unsigned>(mask & 1u);
            mask >>= 1;
        }
    }
}

// One enumeration shared by both accessors (the previous pair of magic
// statics walked the SYSTEM_LOGICAL_PROCESSOR_INFORMATION table twice).
static const std::pair<unsigned, unsigned>& Topology() {
    static const std::pair<unsigned, unsigned> topo = [] {
        unsigned c = 0, t = 0;
        CountTopology(&c, &t);
        return std::pair<unsigned, unsigned>(c, t);
    }();
    return topo;
}

const std::wstring& CpuBrand() {
    static const std::wstring brand = [] {
        DWORD type = 0;
        wchar_t raw[128]{};
        DWORD size = sizeof(raw) - sizeof(wchar_t);
        if (::RegGetValueW(HKEY_LOCAL_MACHINE,
                           L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                           L"ProcessorNameString", RRF_RT_REG_SZ, &type, raw,
                           &size) != ERROR_SUCCESS)
            return std::wstring();
        return CleanCpuBrand(raw);
    }();
    return brand;
}

unsigned CpuCores() { return Topology().first; }

unsigned CpuThreads() { return Topology().second; }

const std::wstring& RamDesc(unsigned long long totalPhysBytes) {
    static const std::wstring desc = BuildRamDesc(totalPhysBytes);
    return desc;
}

}  // namespace auraui::sysinfo
