#include "monitor/network.h"

#include "core/log.h"
#include "core/win_util.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace auraui {
namespace {

constexpr unsigned long long kRefreshIntervalMs = 5000;

std::wstring FormatMac(const UCHAR* addr, ULONG len) {
    if (!addr || len == 0 || len > 32) return {};
    static const wchar_t* hex = L"0123456789ABCDEF";
    std::wstring out;
    out.reserve(len * 3);
    for (ULONG i = 0; i < len; ++i) {
        if (i) out += L'-';
        out += hex[(addr[i] >> 4) & 0xF];
        out += hex[addr[i] & 0xF];
    }
    return out;
}

std::wstring SockAddrToText(const SOCKET_ADDRESS& sa) {
    // inet_ntop only. WSAAddressToStringW needs a valid in/out length
    // parameter (passing nullptr fails outright with WSAEINVAL, so its
    // "primary" path never worked) and appends ":port" for addresses that
    // carry one - neither fits "plain address text" use.
    if (!sa.lpSockaddr) return {};
    if (sa.lpSockaddr->sa_family == AF_INET) {
        char buf[INET_ADDRSTRLEN]{};
        auto* v4 = reinterpret_cast<const sockaddr_in*>(sa.lpSockaddr);
        if (::inet_ntop(AF_INET, &v4->sin_addr, buf, sizeof(buf)))
            return Utf8ToWide(buf);
        return {};
    }
    if (sa.lpSockaddr->sa_family == AF_INET6) {
        char buf[INET6_ADDRSTRLEN]{};
        auto* v6 = reinterpret_cast<const sockaddr_in6*>(sa.lpSockaddr);
        if (::inet_ntop(AF_INET6, &v6->sin6_addr, buf, sizeof(buf)))
            return Utf8ToWide(buf);
        return {};
    }
    return {};
}

bool IsUsableIpv4(const std::wstring& ip) {
    // Drop APIPA (169.254.x.x) and the unspecified address.
    return !ip.empty() && ip.rfind(L"169.254.", 0) != 0 && ip != L"0.0.0.0";
}

bool IsUsableIpv6(const std::wstring& ip) {
    if (ip.empty() || ip == L"::" || ip == L"::1") return false;
    // fe80::/10 link-local
    if (ip.size() >= 4) {
        std::wstring p = ToLower(ip.substr(0, 4));
        if (p == L"fe80") return false;
    }
    return true;
}

}  // namespace

NetworkMonitor::NetworkMonitor() {
    WSADATA wsa{};
    ::WSAStartup(MAKEWORD(2, 2), &wsa);
}

NetworkMonitor::~NetworkMonitor() { ::WSACleanup(); }

void NetworkMonitor::RefreshAdapters(bool force) {
    const unsigned long long now = ::GetTickCount64();
    if (!force && !adaptersDirty_ && !adapters_.empty() &&
        now - lastRefreshTick_ < kRefreshIntervalMs)
        return;
    if (!force && !adaptersDirty_ && adapters_.empty() &&
        lastRefreshTick_ != 0 && now - lastRefreshTick_ < kRefreshIntervalMs)
        return;

    lastRefreshTick_ = now;
    adaptersDirty_   = false;

    ULONG size = 16 * 1024;
    std::vector<BYTE> buffer(size);
    const ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST |
                        GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;

    ULONG ret = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3; ++attempt) {
        ret = ::GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                     reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
                                     &size);
        if (ret != ERROR_BUFFER_OVERFLOW) break;
        buffer.resize(size);
    }
    if (ret != NO_ERROR) {
        if (ret != ERROR_NO_DATA)
            log::Warn(L"GetAdaptersAddresses failed: " + FormatWinError(ret));
        adapters_.clear();
        selected_ = -1;
        return;
    }

    std::vector<AdapterInfo> found;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a;
         a = a->Next) {
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (a->OperStatus != IfOperStatusUp) continue;

        AdapterInfo info;
        info.luid    = a->Luid.Value;
        info.ifIndex = a->IfIndex;
        if (a->FriendlyName && *a->FriendlyName) info.name = a->FriendlyName;
        else if (a->Description && *a->Description) info.name = a->Description;
        if (a->Description && *a->Description) info.description = a->Description;
        info.mac = FormatMac(a->PhysicalAddress, a->PhysicalAddressLength);

        for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
            if (!ua->Address.lpSockaddr) continue;
            const std::wstring ip = SockAddrToText(ua->Address);
            if (ua->Address.lpSockaddr->sa_family == AF_INET) {
                if (info.ipv4.empty() && IsUsableIpv4(ip)) info.ipv4 = ip;
            } else if (ua->Address.lpSockaddr->sa_family == AF_INET6) {
                if (info.ipv6.empty() && IsUsableIpv6(ip)) info.ipv6 = ip;
            }
        }

        for (auto* gw = a->FirstGatewayAddress; gw; gw = gw->Next) {
            const std::wstring ip = SockAddrToText(gw->Address);
            if (ip.empty()) continue;
            info.hasGateway = true;
            if (info.gateway.empty()) info.gateway = ip;
        }

        if (info.ipv4.empty() && info.ipv6.empty()) continue;
        found.push_back(std::move(info));
    }

    // Rank: gateway + IPv4 first, then IPv4, then IPv6-only.
    std::stable_sort(found.begin(), found.end(), [](const AdapterInfo& a, const AdapterInfo& b) {
        auto score = [](const AdapterInfo& x) {
            return (x.hasGateway ? 4 : 0) + (!x.ipv4.empty() ? 2 : 0) + (!x.ipv6.empty() ? 1 : 0);
        };
        return score(a) > score(b);
    });

    // Keep the previous selection when the adapter is still around (match by
    // LUID, then by name), so the monitored adapter does not jump just
    // because the list was re-enumerated (e.g. a VPN briefly outranking it).
    unsigned long long keepLuid = 0;
    std::wstring       keepName;
    if (selected_ >= 0 && selected_ < static_cast<int>(adapters_.size())) {
        keepLuid = adapters_[static_cast<size_t>(selected_)].luid;
        keepName = adapters_[static_cast<size_t>(selected_)].name;
    }
    adapters_ = std::move(found);
    selected_ = adapters_.empty() ? -1 : 0;
    for (size_t i = 0; i < adapters_.size(); ++i) {
        const AdapterInfo& a = adapters_[i];
        if ((keepLuid != 0 && a.luid == keepLuid) ||
            (!keepName.empty() && a.name == keepName)) {
            selected_ = static_cast<int>(i);
            break;
        }
    }
}

void NetworkMonitor::Sample(NetworkInfo& out, double elapsedSeconds) {
    RefreshAdapters(false);

    if (adapters_.empty()) {
        out = NetworkInfo{};
        hasPrev_ = false;
        return;
    }
    if (selected_ < 0 || selected_ >= static_cast<int>(adapters_.size())) selected_ = 0;

    const AdapterInfo& a = adapters_[static_cast<size_t>(selected_)];
    out.connected = true;
    out.adapter   = a.name;
    out.ipv4      = a.ipv4;
    out.ipv6      = a.ipv6;
    out.mac       = a.mac;
    out.gateway   = a.gateway;

    PMIB_IF_TABLE2 table = nullptr;
    if (::GetIfTable2(&table) != NO_ERROR || !table) {
        // No baseline advance: keeping the stale prevIn_/prevOut_ would make
        // the next successful tick divide two intervals of traffic by one
        // interval (~2x rate spike).
        hasPrev_ = false;
        out.downBps = out.upBps = 0.0;
        return;
    }

    unsigned long long in = 0, outp = 0;
    bool found = false;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2& row = table->Table[i];
        if (row.InterfaceLuid.Value == a.luid ||
            (a.luid == 0 && row.InterfaceIndex == a.ifIndex)) {
            in    = row.InOctets;
            outp  = row.OutOctets;
            found = true;
            break;
        }
    }
    // MinGW exposes the generic FreeMibTable(); FreeIfTable2() is the same entry point.
    ::FreeMibTable(table);

    if (!found) {
        adaptersDirty_ = true;  // adapter went away; re-enumerate next tick
        hasPrev_ = false;
        out.downBps = out.upBps = 0.0;
        return;
    }

    // Selection switched to a different adapter (unplug/re-rank): its octet
    // counters start a fresh cumulative series. A cross-adapter delta would
    // report a bogus one-tick spike (potentially TB/s) - re-prime instead.
    if (hasPrev_ && a.luid != 0 && a.luid != prevLuid_) hasPrev_ = false;

    if (hasPrev_ && elapsedSeconds > 0.001) {
        const unsigned long long dIn =
            (in >= prevIn_) ? in - prevIn_ : 0;  // guard against counter reset
        const unsigned long long dOut =
            (outp >= prevOut_) ? outp - prevOut_ : 0;
        out.downBps = static_cast<double>(dIn) / elapsedSeconds;
        out.upBps   = static_cast<double>(dOut) / elapsedSeconds;
    } else {
        out.downBps = out.upBps = 0.0;
    }

    prevIn_  = in;
    prevOut_ = outp;
    prevLuid_ = a.luid;
    hasPrev_ = true;
}

}  // namespace auraui
