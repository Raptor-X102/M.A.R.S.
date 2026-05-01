// pmc.hpp
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace silicon_probe::platform::pmc {

// Platform-independent event types
enum class EventType : int {
    BRANCH_MISSES,
    BRANCH_INSTRUCTIONS,
    CPU_CYCLES,
    INSTRUCTIONS,
    CACHE_REFERENCES,
    CACHE_MISSES,
    BUS_CYCLES,
    REF_CPU_CYCLES,
};

struct CounterValues {
    std::vector<uint64_t> values;  // order matches the events in group
    bool valid = false;
};

class PmcGroup {
   public:
    virtual ~PmcGroup() = default;

    // Create a group of counters for the given event types
    static std::unique_ptr<PmcGroup> create(const std::vector<EventType>& events);
    static std::unique_ptr<PmcGroup> create_raw(const std::vector<std::string>& event_names);

    virtual void reset() = 0;
    virtual void enable() = 0;
    virtual void disable() = 0;
    virtual CounterValues read() const = 0;

    virtual std::vector<EventType> get_event_types() const = 0;

    static bool is_supported() noexcept;
};

}  // namespace silicon_probe::platform::pmc
