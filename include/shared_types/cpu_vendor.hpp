#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace silicon_probe::platform::cpu_vendor {

// Simple CPU vendor description.
class CpuVendor {
   public:
    // Known vendor IDs used in the code.
    enum class CpuVendorID : uint8_t { Unknown = 0, Intel = 1, AMD = 2 };

   private:
    CpuVendorID id_ = CpuVendorID::Unknown;
    std::string name_;

   public:
    CpuVendor() = default;
    CpuVendor(CpuVendorID id, std::string name) : id_(id), name_(std::move(name)) {}

    bool operator==(CpuVendorID rhs) const noexcept { return id_ == rhs; }

    // Returns the original vendor string.
    std::string_view name() const noexcept { return name_; }

    // Returns the normalized vendor ID.
    CpuVendorID id() const noexcept { return id_; }
};

// Vendor-specific event names for some benchmarks.
struct CpuEvents {
    std::string issued_event;   // Event for issued uops or instructions.
    std::string retired_event;  // Event for retired uops or instructions.
    std::string stalls_event;   // Event for stalls.

    CpuEvents() = default;
    CpuEvents(std::string i, std::string r, std::string s)
        : issued_event(std::move(i)), retired_event(std::move(r)), stalls_event(std::move(s)) {}
};

}  // namespace silicon_probe::platform::cpu_vendor
