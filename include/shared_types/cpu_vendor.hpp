#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace silicon_probe::platform::cpu_vendor {

class CpuVendor {
   public:
    enum class CpuVendorID : uint8_t { Unknown = 0, Intel = 1, AMD = 2 };

   private:
    CpuVendorID id_ = CpuVendorID::Unknown;
    std::string name_;

   public:
    CpuVendor() = default;
    CpuVendor(CpuVendorID id, std::string name) : id_(id), name_(std::move(name)) {}

    bool operator==(CpuVendorID rhs) const noexcept { return id_ == rhs; }

    std::string_view name() const noexcept { return name_; }

    CpuVendorID id() const noexcept { return id_; }
};

struct CpuEvents {
    std::string issued_event;
    std::string retired_event;
    std::string stalls_event;

    CpuEvents() = default;
    CpuEvents(std::string i, std::string r, std::string s)
        : issued_event(std::move(i)), retired_event(std::move(r)), stalls_event(std::move(s)) {}
};

}  // namespace silicon_probe::platform::cpu_vendor
