#include "silicon_probe/platform/os.hpp"

#include "silicon_probe/infra/logging.hpp"
#include "silicon_probe/platform/arch.hpp"

#include <cpuid.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

namespace {

constexpr uint64_t kCalibrationSleepNs = 10ULL * 1000ULL * 1000ULL;
constexpr int kCalibrationIterations = 5;
constexpr uint64_t kNsPerSecond = 1'000'000'000ULL;
constexpr size_t kDefaultCacheLineSize = 64;

struct PriorityState {
    int policy = 0;
    sched_param parameters{};
    bool valid = false;
};

PriorityState& priority_state() {
    static PriorityState state;
    return state;
}

std::string& saved_governor() {
    static std::string governor;
    return governor;
}

bool& governor_saved() {
    static bool saved = false;
    return saved;
}

uint64_t& cached_tsc_frequency() {
    static uint64_t frequency = 0;
    return frequency;
}

uint64_t calibrate_tsc() {
    if (!silicon_probe::platform::tsc_is_invariant()) {
        SPDLOG_WARN("TSC is not invariant, skipping calibration");
        return 0;
    }

    uint64_t best_cycles = 0;
    for (int iteration = 0; iteration < kCalibrationIterations; ++iteration) {
        timespec request{0, static_cast<long>(kCalibrationSleepNs)};
        timespec remainder{};

        const uint64_t start = silicon_probe::platform::arch::tick();
        const int result = clock_nanosleep(CLOCK_MONOTONIC_RAW, 0, &request, &remainder);
        if (result != 0) {
            continue;
        }
        const uint64_t end = silicon_probe::platform::arch::tick();
        const uint64_t cycles = end - start;

        if (best_cycles == 0 || cycles < best_cycles) {
            best_cycles = cycles;
        }
    }

    if (best_cycles == 0) {
        return 0;
    }

    return best_cycles * kNsPerSecond / kCalibrationSleepNs;
}

} // namespace

namespace silicon_probe::platform {

struct ScopedThreadAffinity::cpu_set_t_storage {
    cpu_set_t set;
};

bool tsc_is_invariant() {
    unsigned int eax = 0;
    unsigned int ebx = 0;
    unsigned int ecx = 0;
    unsigned int edx = 0;
    __cpuid_count(0x80000007, 0, eax, ebx, ecx, edx);
    return (edx & (1u << 8)) != 0U;
}

void bind_thread_to_cpu(int cpu) {
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    CPU_SET(cpu, &affinity);

    const int result = pthread_setaffinity_np(pthread_self(), sizeof(affinity), &affinity);
    if (result != 0) {
        throw SystemError("Failed to bind thread to CPU " + std::to_string(cpu) + ": " + std::to_string(result));
    }
}

size_t cache_line_size() {
    const long value = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    return value > 0 ? static_cast<size_t>(value) : kDefaultCacheLineSize;
}

uint64_t tick_frequency() {
    uint64_t& frequency = cached_tsc_frequency();
    if (frequency == 0) {
        frequency = calibrate_tsc();
    }
    return frequency;
}

void* huge_alloc(size_t size) {
    constexpr size_t huge_page_size = 2 * 1024 * 1024;
    const size_t aligned_size = (size + huge_page_size - 1) & ~(huge_page_size - 1);
    void* ptr = mmap(nullptr, aligned_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    return ptr == MAP_FAILED ? nullptr : ptr;
}

void huge_free(void* ptr, size_t size) {
    if (ptr == nullptr) {
        return;
    }

    constexpr size_t huge_page_size = 2 * 1024 * 1024;
    const size_t aligned_size = (size + huge_page_size - 1) & ~(huge_page_size - 1);
    munmap(ptr, aligned_size);
}

void* aligned_alloc(size_t alignment, size_t size) {
    void* ptr = nullptr;
    const int result = posix_memalign(&ptr, alignment, size);
    if (result != 0) {
        throw std::bad_alloc();
    }
    return ptr;
}

void aligned_free(void* ptr) {
    std::free(ptr);
}

void set_realtime_priority() {
    auto& state = priority_state();
    if (pthread_getschedparam(pthread_self(), &state.policy, &state.parameters) != 0) {
        throw SystemError("Failed to capture current thread scheduling parameters");
    }
    state.valid = true;

    sched_param realtime{};
    realtime.sched_priority = sched_get_priority_max(SCHED_FIFO);
    const int result = pthread_setschedparam(pthread_self(), SCHED_FIFO, &realtime);
    if (result != 0) {
        state.valid = false;
        if (result == EPERM) {
            throw PermissionError("Realtime priority requires CAP_SYS_NICE. Run with sudo or disable the flag.");
        }
        throw SystemError("Failed to set realtime priority: " + std::to_string(result));
    }

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        SPDLOG_WARN("mlockall failed, memory may still be paged");
    }
}

void restore_priority() {
    auto& state = priority_state();
    munlockall();

    if (!state.valid) {
        return;
    }

    const int result = pthread_setschedparam(pthread_self(), state.policy, &state.parameters);
    if (result != 0) {
        throw SystemError("Failed to restore thread priority: " + std::to_string(result));
    }

    state.valid = false;
}

void lock_cpu_frequency() {
    const int cpu = sched_getcpu();
    if (cpu < 0) {
        throw SystemError("Failed to obtain current CPU id");
    }

    const std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpufreq/scaling_governor";
    std::ifstream input(path);
    if (!input.is_open()) {
        throw PermissionError("Failed to open governor file for reading: " + path);
    }

    input >> saved_governor();
    if (input.fail()) {
        throw SystemError("Failed to read current CPU governor");
    }
    governor_saved() = true;

    std::ofstream output(path);
    if (!output.is_open()) {
        throw PermissionError("Failed to open governor file for writing: " + path);
    }

    output << "performance";
    if (output.fail()) {
        throw PermissionError("Failed to switch governor to performance mode");
    }
}

void restore_cpu_frequency() {
    if (!governor_saved()) {
        return;
    }

    const int cpu = sched_getcpu();
    if (cpu < 0) {
        governor_saved() = false;
        throw SystemError("Failed to obtain current CPU id while restoring governor");
    }

    const std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpufreq/scaling_governor";
    std::ofstream output(path);
    if (!output.is_open()) {
        governor_saved() = false;
        throw SystemError("Failed to open governor file for restore: " + path);
    }

    output << saved_governor();
    if (output.fail()) {
        governor_saved() = false;
        throw SystemError("Failed to restore CPU governor");
    }

    governor_saved() = false;
}

ScopedThreadAffinity::ScopedThreadAffinity(int cpu)
    : previous_affinity_(new cpu_set_t_storage{}) {
    CPU_ZERO(&previous_affinity_->set);
    if (pthread_getaffinity_np(pthread_self(), sizeof(previous_affinity_->set), &previous_affinity_->set) != 0) {
        delete previous_affinity_;
        previous_affinity_ = nullptr;
        throw SystemError("Failed to capture current thread affinity");
    }

    previous_cpu_count_ = CPU_COUNT(&previous_affinity_->set);
    bind_thread_to_cpu(cpu);
    active_ = true;
    SPDLOG_INFO("Bound thread to CPU {}", cpu);
}

ScopedThreadAffinity::~ScopedThreadAffinity() {
    if (previous_affinity_ == nullptr) {
        return;
    }

    if (active_) {
        const int result = pthread_setaffinity_np(pthread_self(), sizeof(previous_affinity_->set), &previous_affinity_->set);
        if (result != 0) {
            SPDLOG_ERROR("Failed to restore previous thread affinity: {}", result);
        }
    }

    delete previous_affinity_;
    previous_affinity_ = nullptr;
}

ScopedPriority::ScopedPriority() {
    set_realtime_priority();
}

ScopedPriority::~ScopedPriority() {
    try {
        restore_priority();
    } catch (const std::exception& error) {
        SPDLOG_ERROR("Failed to restore priority: {}", error.what());
    }
}

ScopedFrequencyLock::ScopedFrequencyLock() {
    lock_cpu_frequency();
}

ScopedFrequencyLock::~ScopedFrequencyLock() {
    try {
        restore_cpu_frequency();
    } catch (const std::exception& error) {
        SPDLOG_ERROR("Failed to restore CPU frequency governor: {}", error.what());
    }
}

ScopedMeasurementEnvironment::ScopedMeasurementEnvironment(const MeasurementEnvironmentOptions& options) {
    if (options.cpu.has_value()) {
        affinity_.emplace(*options.cpu);
    }
    if (options.realtime_priority) {
        priority_.emplace();
    }
    if (options.lock_frequency) {
        frequency_lock_.emplace();
    }
}

} // namespace silicon_probe::platform
