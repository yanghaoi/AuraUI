#pragma once

// ---------------------------------------------------------------------------
// Small Win32 helpers shared across the project.
// ---------------------------------------------------------------------------

#include <windows.h>

#include <string>
#include <string_view>
#include <utility>

namespace auraui {

// --- diagnostics -----------------------------------------------------------

std::wstring FormatWinError(DWORD err);
std::wstring LastErrorString();
std::wstring HresultString(HRESULT hr);

// --- string helpers --------------------------------------------------------

std::wstring Utf8ToWide(std::string_view utf8);
std::string  WideToUtf8(std::wstring_view wide);

std::wstring Trim(std::wstring s);
std::wstring ToLower(std::wstring s);

// --- number formatting (no <format> dependency, keeps MinGW happy) ---------

std::wstring FmtInt(long long v);
std::wstring FmtFloat(double v, int decimals);
std::wstring FmtPercent(double v);            // "23%"
std::wstring FmtBytes(unsigned long long b);  // "4.2 GB"
std::wstring FmtSpeed(double bytesPerSec);    // "12.4 MB/s"

// --- COM smart pointer -----------------------------------------------------

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(std::nullptr_t) {}
    ComPtr(const ComPtr& other) : p_(other.p_) {
        if (p_) p_->AddRef();
    }
    ComPtr(ComPtr&& other) noexcept : p_(other.p_) { other.p_ = nullptr; }

    ComPtr& operator=(const ComPtr& other) {
        if (this != &other) {
            if (other.p_) other.p_->AddRef();
            reset();
            p_ = other.p_;
        }
        return *this;
    }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            reset();
            p_ = other.p_;
            other.p_ = nullptr;
        }
        return *this;
    }
    ~ComPtr() { reset(); }

    T*  get() const { return p_; }
    T** put() { reset(); return &p_; }
    T*  operator->() const { return p_; }
    operator T*() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

    void reset() {
        if (p_) {
            p_->Release();
            p_ = nullptr;
        }
    }

private:
    T* p_ = nullptr;
};

}  // namespace auraui
