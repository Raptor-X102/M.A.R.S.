#include "silicon_probe/platform/pmc.hpp"
#include "silicon_probe/platform/arch.hpp"
#include "silicon_probe/infra/logging.hpp"

#include <perfmon/pfmlib.h>
#include <perfmon/pfmlib_perf_event.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>

namespace silicon_probe::platform::pmc {

static int open_perf_counter_linux(const char* event_name) {
    struct perf_event_attr attr{};
    attr.size = sizeof(attr);
    attr.disabled = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.exclude_idle = 1;

    int ret = pfm_get_perf_event_encoding(event_name, PFM_PLM3, &attr, nullptr, nullptr);
    if (ret != PFM_SUCCESS) {
        SPDLOG_ERROR("libpfm4 failed to encode '{}': {}", event_name, pfm_strerror(ret));
        return -1;
    }

    int fd = static_cast<int>(syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0));
    if (fd < 0) {
        SPDLOG_ERROR("perf_event_open failed for '{}': {}", event_name, std::strerror(errno));
        return -1;
    }
    return fd;
}

// Linux-specific implementation
class PmcGroupLinux final : public PmcGroup {
public:
    PmcGroupLinux(int fd_issued, int fd_retired, int fd_stalls)
        : fd_issued_(fd_issued), fd_retired_(fd_retired), fd_stalls_(fd_stalls) {
        if (!valid()) {
            cleanup();
            throw std::runtime_error("Failed to open one or more PMC counters");
        }
    }
    
    ~PmcGroupLinux() override {
        cleanup();
    }
    
    void reset() override {
        if (fd_issued_ >= 0) ioctl(fd_issued_, PERF_EVENT_IOC_RESET, 0);
        if (fd_retired_ >= 0) ioctl(fd_retired_, PERF_EVENT_IOC_RESET, 0);
        if (fd_stalls_ >= 0) ioctl(fd_stalls_, PERF_EVENT_IOC_RESET, 0);
    }
    
    void enable() override {
        if (fd_issued_ >= 0) ioctl(fd_issued_, PERF_EVENT_IOC_ENABLE, 0);
        if (fd_retired_ >= 0) ioctl(fd_retired_, PERF_EVENT_IOC_ENABLE, 0);
        if (fd_stalls_ >= 0) ioctl(fd_stalls_, PERF_EVENT_IOC_ENABLE, 0);
    }
    
    void disable() override {
        if (fd_issued_ >= 0) ioctl(fd_issued_, PERF_EVENT_IOC_DISABLE, 0);
        if (fd_retired_ >= 0) ioctl(fd_retired_, PERF_EVENT_IOC_DISABLE, 0);
        if (fd_stalls_ >= 0) ioctl(fd_stalls_, PERF_EVENT_IOC_DISABLE, 0);
    }
    
    [[nodiscard]] CounterValues read() const override {
        CounterValues values;
        values.issued = read_single(fd_issued_);
        values.retired = read_single(fd_retired_);
        values.stalls = read_single(fd_stalls_);
        values.valid = (fd_issued_ >= 0 && fd_retired_ >= 0 && fd_stalls_ >= 0);
        return values;
    }
    
private:
    [[nodiscard]] bool valid() const noexcept {
        return fd_issued_ >= 0 && fd_retired_ >= 0 && fd_stalls_ >= 0;
    }
    
    void cleanup() {
        if (fd_issued_ >= 0) close(fd_issued_);
        if (fd_retired_ >= 0) close(fd_retired_);
        if (fd_stalls_ >= 0) close(fd_stalls_);
    }
    
    [[nodiscard]] static uint64_t read_single(int fd) {
        uint64_t val = 0;
        if (fd >= 0) {
            ::read(fd, &val, sizeof(val));
        }
        return val;
    }
    
    int fd_issued_ = -1;
    int fd_retired_ = -1;
    int fd_stalls_ = -1;
};

// Factory method
std::unique_ptr<PmcGroup> PmcGroup::create(
    const std::string& issued_event,
    const std::string& retired_event,
    const std::string& stalls_event
) {
    int fd_issued = open_perf_counter_linux(issued_event.c_str());
    int fd_retired = open_perf_counter_linux(retired_event.c_str());
    int fd_stalls = open_perf_counter_linux(stalls_event.c_str());
    
    // If any counter failed, close the ones that succeeded and return nullptr
    if (fd_issued < 0 || fd_retired < 0 || fd_stalls < 0) {
        SPDLOG_ERROR("fd_issued({}) < 0 || fd_retired({}) < 0 || fd_stalls({}) < 0",
                      fd_issued,           fd_retired,           fd_stalls
                    );
        if (fd_issued >= 0) close(fd_issued);
        if (fd_retired >= 0) close(fd_retired);
        if (fd_stalls >= 0) close(fd_stalls);
        return nullptr;
    }
    
    return std::make_unique<PmcGroupLinux>(fd_issued, fd_retired, fd_stalls);
}

bool PmcGroup::is_supported() noexcept {
    // Check if perf_event_open is available and we have permissions
    struct perf_event_attr attr{};
    attr.size = sizeof(attr);
    attr.type = PERF_TYPE_SOFTWARE;
    attr.config = PERF_COUNT_SW_CPU_CLOCK;
    attr.disabled = 1;
    
    int fd = static_cast<int>(syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0));
    if (fd >= 0) {
        close(fd);
        return true;
    }
    return false;
}

} // namespace silicon_probe::platform::pmc
