#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <memory>

namespace silicon_probe::platform::pmc {

class CounterHandle {
public:
    CounterHandle() = default;
    explicit CounterHandle(int native_fd) : native_fd_(native_fd) {}
    [[nodiscard]] bool valid() const noexcept { return native_fd_ >= 0; }
    [[nodiscard]] int native_fd() const noexcept { return native_fd_; }
private:
    int native_fd_ = -1;
};

struct CounterValues {
    uint64_t issued = 0;
    uint64_t retired = 0;
    uint64_t stalls = 0;
    bool valid = false;
};

class PmcGroup {
public:
    virtual ~PmcGroup() = default;
    
    [[nodiscard]] static std::unique_ptr<PmcGroup> create(
        const std::string& issued_event,
        const std::string& retired_event,
        const std::string& stalls_event
    );
    
    virtual void reset() = 0;
    virtual void enable() = 0;
    virtual void disable() = 0;
    
    [[nodiscard]] virtual CounterValues read() const = 0;
    
    [[nodiscard]] static bool is_supported() noexcept;
};

} // namespace silicon_probe::platform::pmc
