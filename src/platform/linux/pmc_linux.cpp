// pmc_linux.cpp
#include "silicon_probe/platform/pmc.hpp"
#include "silicon_probe/infra/logging.hpp"
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>
#include <cstdint>
#include <mutex>
#include <perfmon/pfmlib_perf_event.h>

namespace silicon_probe::platform::pmc {

// Map platform-independent EventType to Linux hardware config
static uint64_t map_event_to_perf_config(EventType ev) {
    switch (ev) {
        case EventType::BRANCH_MISSES:      return PERF_COUNT_HW_BRANCH_MISSES;
        case EventType::BRANCH_INSTRUCTIONS:return PERF_COUNT_HW_BRANCH_INSTRUCTIONS;
        case EventType::CPU_CYCLES:         return PERF_COUNT_HW_CPU_CYCLES;
        case EventType::INSTRUCTIONS:       return PERF_COUNT_HW_INSTRUCTIONS;
        case EventType::CACHE_REFERENCES:   return PERF_COUNT_HW_CACHE_REFERENCES;
        case EventType::CACHE_MISSES:       return PERF_COUNT_HW_CACHE_MISSES;
        case EventType::BUS_CYCLES:         return PERF_COUNT_HW_BUS_CYCLES;
        case EventType::REF_CPU_CYCLES:     return PERF_COUNT_HW_REF_CPU_CYCLES;
        default:                            return 0; // should not happen
    }
}

static int open_perf_counter(EventType event) {
    struct perf_event_attr attr = {};
    attr.size = sizeof(attr);
    attr.type = PERF_TYPE_HARDWARE;
    attr.config = map_event_to_perf_config(event);
    attr.disabled = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.exclude_idle = 1;

    int fd = syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0);
    return fd;
}

static bool encode_event(const std::string& event_name, struct perf_event_attr& attr) {
    static std::once_flag init_flag;
    std::call_once(init_flag, []() {
        pfm_initialize();
    });

    pfm_perf_encode_arg_t arg = {};
    arg.attr = &attr;
    arg.size = sizeof(pfm_perf_encode_arg_t);
    attr.size = sizeof(struct perf_event_attr);

    int ret = pfm_get_os_event_encoding(event_name.c_str(), PFM_PLM3, PFM_OS_PERF_EVENT_EXT, &arg);

    return (ret == PFM_SUCCESS);
}

static int open_raw_counter_by_name(const std::string& event_name) {
    struct perf_event_attr attr = {};
    if (!encode_event(event_name, attr)) {
        return -1;
    }
    attr.disabled = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.exclude_idle = 1;

    int fd = syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0);
    return fd;
}

class PmcGroupLinux final : public PmcGroup {
public:
    PmcGroupLinux(std::vector<int> fds, std::vector<EventType> types)
        : fds_(std::move(fds)), event_types_(std::move(types)) {
        if (!valid()) {
            cleanup();
            throw std::runtime_error("Failed to open one or more counters");
        }
    }

    ~PmcGroupLinux() override { cleanup(); }

    void reset() override {
        for (int fd : fds_) {
            if (fd >= 0) ioctl(fd, PERF_EVENT_IOC_RESET, 0);
        }
    }

    void enable() override {
        for (int fd : fds_) {
            if (fd >= 0) ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
        }
    }

    void disable() override {
        for (int fd : fds_) {
            if (fd >= 0) ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
        }
    }

    CounterValues read() const override {
        CounterValues cv;
        cv.values.reserve(fds_.size());
        bool all_ok = true;
        for (int fd : fds_) {
            uint64_t val = 0;
            if (fd >= 0 && ::read(fd, &val, sizeof(val)) == sizeof(val)) {
                cv.values.push_back(val);
            } else {
                cv.values.push_back(0);
                all_ok = false;
            }
        }
        cv.valid = all_ok && !cv.values.empty();
        return cv;
    }

    std::vector<EventType> get_event_types() const override {
        return event_types_;
    }

private:
    bool valid() const noexcept {
        for (int fd : fds_) {
            if (fd < 0) return false;
        }
        return !fds_.empty();
    }

    void cleanup() {
        for (int fd : fds_) {
            if (fd >= 0) close(fd);
        }
    }

    std::vector<int> fds_;
    std::vector<EventType> event_types_;
};

// Factory method
std::unique_ptr<PmcGroup> PmcGroup::create(const std::vector<EventType>& events) {
    std::vector<int> fds;
    fds.reserve(events.size());

    for (EventType ev : events) {
        int fd = open_perf_counter(ev);
        if (fd < 0) {
            for (int opened : fds) close(opened);
            return nullptr;
        }
        fds.push_back(fd);
    }

    return std::make_unique<PmcGroupLinux>(std::move(fds), events);
}

bool PmcGroup::is_supported() noexcept {
    struct perf_event_attr attr = {};
    attr.size = sizeof(attr);
    attr.type = PERF_TYPE_SOFTWARE;
    attr.config = PERF_COUNT_SW_CPU_CLOCK;
    attr.disabled = 1;
    int fd = syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0);
    if (fd >= 0) {
        close(fd);
        return true;
    }
    return false;
}

class PmcGroupRawLinux final : public PmcGroup {
public:
    PmcGroupRawLinux(std::vector<int> fds, std::vector<std::string> names)
        : fds_(std::move(fds)), event_names_(std::move(names)) {
        if (!valid()) cleanup();
        //print_initial_values();
    }

    ~PmcGroupRawLinux() override { cleanup(); }

    void reset() override {
        for (int fd : fds_) {
            if (fd >= 0) ioctl(fd, PERF_EVENT_IOC_RESET, 0);
        }
    }

    void enable() override {
        for (int fd : fds_) {
            if (fd >= 0) ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
        }
    }

    void disable() override {
        for (int fd : fds_) {
            if (fd >= 0) ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
        }
    }

    CounterValues read() const override {
        CounterValues cv;
        cv.values.reserve(fds_.size());
        bool ok = true;
        for (int fd : fds_) {
            uint64_t val = 0;
            if (fd >= 0 && ::read(fd, &val, sizeof(val)) == sizeof(val)) {
                cv.values.push_back(val);
            } else {
                cv.values.push_back(0);
                ok = false;
            }
        }
        cv.valid = ok && !cv.values.empty();
        return cv;
    }

    std::vector<EventType> get_event_types() const override { return {}; }

    const std::vector<std::string>& get_event_names() const { return event_names_; }

private:
    bool valid() const noexcept {
        for (int fd : fds_) if (fd < 0) return false;
        return !fds_.empty();
    }

    void cleanup() {
        for (int fd : fds_) if (fd >= 0) close(fd);
    }

    void print_initial_values() const {
        auto init = read();
        if (!init.valid) {
            SPDLOG_WARN("Failed to read initial counter values");
            return;
        }
        SPDLOG_INFO("Initial counter values (before any reset/enable):");
        for (size_t i = 0; i < event_names_.size() && i < init.values.size(); ++i) {
            SPDLOG_INFO("  {} = {}", event_names_[i], init.values[i]);
        }
        bool non_zero = false;
        for (auto v : init.values) {
            if (v > 100) { non_zero = true; break; }
        }
        if (non_zero) {
            SPDLOG_WARN("Initial counter values are not near zero! Will need to compute differences.");
        }
    }

    std::vector<int> fds_;
    std::vector<std::string> event_names_;
};

std::unique_ptr<PmcGroup> PmcGroup::create_raw(const std::vector<std::string>& event_names) {
    std::vector<int> fds;
    fds.reserve(event_names.size());
    for (const auto& name : event_names) {
        int fd = open_raw_counter_by_name(name);
        if (fd < 0) {
            for (int f : fds) close(f);
            return nullptr;
        }
        fds.push_back(fd);
    }
    return std::make_unique<PmcGroupRawLinux>(std::move(fds), event_names);
}

} // namespace
