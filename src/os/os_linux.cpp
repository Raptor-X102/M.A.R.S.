#include "os_linux.hpp"
#include "logger.hpp"
#include "arch.hpp" // Included here for tick()

#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fstream>
#include <cpuid.h>
#include <cstring>
#include <ctime>

namespace os {

static const uint64_t CALIBRATION_SLEEP_NS = 10 * 1000000;
static const int CALIBRATION_ITERATIONS = 5;
static const uint64_t NS_PER_SEC = 1000000000ULL;
static const size_t DEFAULT_CACHE_LINE_SIZE = 64;

// --- Functions ---

bool tsc_is_invariant() {
    unsigned int eax, ebx, ecx, edx;
    __cpuid_count(0x80000007, 0, eax, ebx, ecx, edx);
    return (edx & (1 << 8)) != 0;                    
}


void bind_thread_to_cpu(int cpu) {
    LOG_DEBUG_STREAM << "Binding thread to CPU " << cpu;
    
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    
    int ret = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    if (ret != 0) {
        std::string msg = "Failed to bind thread to CPU " + std::to_string(cpu) + 
                          ", error: " + std::to_string(ret);
        LOG_ERROR_STREAM << msg;
        throw SystemError(msg);
    }
    
    LOG_INFO_STREAM << "Thread bound to CPU " << cpu;
}

size_t cache_line_size() {
    long size = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    size_t result = size > 0 ? static_cast<size_t>(size) : DEFAULT_CACHE_LINE_SIZE;
    
    if (size > 0) {
        LOG_DEBUG_STREAM << "Cache line size from sysconf: " << result;
    } else {
        LOG_WARNING_STREAM << "sysconf failed, using default cache line size: " << result;
    }
    
    return result;
}

static uint64_t cached_tsc_freq = 0;

static uint64_t calibrate_tsc() {
    LOG_INFO_STREAM << "Starting TSC calibration";
    
    if (!tsc_is_invariant()) {
        LOG_WARNING_STREAM << "TSC is not invariant, skipping calibration";
        return 0;
    }

    uint64_t best_cycles = 0;
    uint64_t best_ns = 0;

    for (int i = 0; i < CALIBRATION_ITERATIONS; ++i) {
        struct timespec req = {0, static_cast<long>(CALIBRATION_SLEEP_NS)};
        struct timespec rem;
        
        uint64_t start = arch::tick();
        int ret = clock_nanosleep(CLOCK_MONOTONIC_RAW, 0, &req, &rem);
        if (ret != 0) {
            LOG_DEBUG_STREAM << "Calibration iteration " << i << " failed, ret=" << ret;
            continue;
        }
        uint64_t end = arch::tick();

        uint64_t cycles = end - start;
        uint64_t ns = CALIBRATION_SLEEP_NS;

        if (best_cycles == 0 || cycles < best_cycles) {
            best_cycles = cycles;
            best_ns = ns;
        }
        
        LOG_DEBUG_STREAM << "Iteration " << i << ": " << cycles << " cycles in " << ns << " ns";
    }

    if (best_cycles == 0) {
        LOG_ERROR_STREAM << "TSC calibration failed: no valid measurements";
        return 0;
    }
    
    uint64_t freq = best_cycles * NS_PER_SEC / best_ns;
    LOG_INFO_STREAM << "TSC calibration complete: " << freq << " Hz";
    return freq;
}

uint64_t tick_frequency() {
    if (cached_tsc_freq == 0) {
        LOG_DEBUG_STREAM << "TSC frequency not cached, performing calibration";
        cached_tsc_freq = calibrate_tsc();
        if (cached_tsc_freq == 0) {
            LOG_WARNING_STREAM << "TSC calibration failed, frequency unknown";
        }
    } else {
        LOG_DEBUG_STREAM << "Using cached TSC frequency: " << cached_tsc_freq;
    }
    return cached_tsc_freq;
}

void* huge_alloc(size_t size) {
    const size_t huge_page_size = 2 * 1024 * 1024;
    size_t aligned_size = (size + huge_page_size - 1) & ~(huge_page_size - 1);
    
    void* ptr = mmap(nullptr, aligned_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (ptr == MAP_FAILED) {
        LOG_ERROR_STREAM << "Huge page allocation failed: " << strerror(errno);
        return nullptr;
    }
    
    LOG_DEBUG_STREAM << "Huge page allocated: " << ptr << ", size: " << aligned_size;
    return ptr;
}

void huge_free(void* ptr, size_t size) {
    if (ptr) {
        const size_t huge_page_size = 2 * 1024 * 1024;
        size_t aligned_size = (size + huge_page_size - 1) & ~(huge_page_size - 1);
        
        int ret = munmap(ptr, aligned_size);
        if (ret != 0) {
            LOG_ERROR_STREAM << "Huge page free failed: " << strerror(errno);
        } else {
            LOG_DEBUG_STREAM << "Huge page freed: " << ptr;
        }
    }
}

void* aligned_alloc(size_t alignment, size_t size) {
    LOG_DEBUG_STREAM << "Allocating " << size << " bytes aligned to " << alignment;
    
    void* ptr = nullptr;
    int ret = posix_memalign(&ptr, alignment, size);
    if (ret != 0) {
        std::string msg = "posix_memalign failed: error " + std::to_string(ret);
        LOG_ERROR_STREAM << msg;
        throw std::bad_alloc();
    }
    
    LOG_DEBUG_STREAM << "Aligned allocation successful: " << ptr;
    return ptr;
}

void aligned_free(void* ptr) {
    LOG_DEBUG_STREAM << "Freeing aligned memory: " << ptr;
    free(ptr);
}

static struct PriorityState {
    int policy;
    struct sched_param param;
    bool valid = false;
} old_state;

void set_realtime_priority() {
    LOG_INFO_STREAM << "Setting realtime priority";
    
    if (pthread_getschedparam(pthread_self(), &old_state.policy, &old_state.param) != 0) {
        std::string msg = "Failed to get current thread priority";
        LOG_ERROR_STREAM << msg;
        throw SystemError(msg);
    }
    old_state.valid = true;

    struct sched_param rt_param;
    rt_param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    
    int ret = pthread_setschedparam(pthread_self(), SCHED_FIFO, &rt_param);
    if (ret != 0) {
        old_state.valid = false;
        std::string msg = "Failed to set realtime priority, error: " + std::to_string(ret);
        LOG_ERROR_STREAM << msg;
        if (ret == EPERM) {
            throw PermissionError("Realtime priority requires CAP_SYS_NICE capability. Run with sudo.");
        }
        throw SystemError(msg);
    }

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        LOG_WARNING_STREAM << "mlockall failed, memory may be paged";
    }
    
    LOG_INFO_STREAM << "Realtime priority set, priority=" << rt_param.sched_priority;
}

void restore_priority() {
    LOG_DEBUG_STREAM << "Restoring thread priority";
    
    munlockall();
    
    if (!old_state.valid) {
        LOG_WARNING_STREAM << "No saved priority state to restore";
        return;
    }
    
    int ret = pthread_setschedparam(pthread_self(), old_state.policy, &old_state.param);
    if (ret != 0) {
        LOG_ERROR_STREAM << "Failed to restore priority, error: " << ret;
        throw SystemError("Failed to restore thread priority");
    }
    
    old_state.valid = false;
    LOG_DEBUG_STREAM << "Priority restored successfully";
}

static std::string original_governor;
static bool governor_saved = false;

void lock_cpu_frequency() {
    LOG_INFO_STREAM << "Locking CPU frequency";
    
    int cpu = sched_getcpu();
    if (cpu < 0) {
        std::string msg = "Failed to get current CPU";
        LOG_ERROR_STREAM << msg;
        throw SystemError(msg);
    }

    std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpufreq/scaling_governor";
    
    std::ifstream read_file(path);
    if (!read_file.is_open()) {
        std::string msg = "Failed to open governor file for reading: " + path;
        LOG_ERROR_STREAM << msg;
        throw PermissionError(msg + ". Run with sudo or check permissions.");
    }
    
    read_file >> original_governor;
    if (read_file.fail()) {
        std::string msg = "Failed to read current governor";
        LOG_ERROR_STREAM << msg;
        throw SystemError(msg);
    }
    governor_saved = true;
    read_file.close();
    
    LOG_DEBUG_STREAM << "Current governor: " << original_governor;

    std::ofstream write_file(path);
    if (!write_file.is_open()) {
        std::string msg = "Failed to open governor file for writing: " + path;
        LOG_ERROR_STREAM << msg;
        throw PermissionError(msg + ". Run with sudo or check permissions.");
    }
    
    write_file << "performance";
    if (write_file.fail()) {
        std::string msg = "Failed to set governor to performance";
        LOG_ERROR_STREAM << msg;
        throw PermissionError(msg + ". Run with sudo or check permissions.");
    }
    
    LOG_INFO_STREAM << "CPU frequency locked to performance mode";
}

void restore_cpu_frequency() {
    LOG_DEBUG_STREAM << "Restoring CPU frequency governor";
    
    if (!governor_saved) {
        LOG_WARNING_STREAM << "No saved governor state to restore";
        return;
    }

    int cpu = sched_getcpu();
    if (cpu < 0) {
        LOG_ERROR_STREAM << "Failed to get current CPU";
        governor_saved = false;
        return;
    }

    std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpufreq/scaling_governor";
    
    std::ofstream file(path);
    if (!file.is_open()) {
        LOG_ERROR_STREAM << "Failed to open governor file: " << path;
        governor_saved = false;
        throw SystemError("Failed to restore CPU governor: cannot open file");
    }
    
    file << original_governor;
    if (file.fail()) {
        LOG_ERROR_STREAM << "Failed to restore CPU governor";
        governor_saved = false;
        throw SystemError("Failed to restore CPU governor: write failed");
    }
    
    LOG_INFO_STREAM << "CPU governor restored to: " << original_governor;
    governor_saved = false;
}

// --- Scoped Classes Implementation ---

ScopedPriority::ScopedPriority() { 
    LOG_DEBUG_STREAM << "ScopedPriority: acquiring realtime priority";
    set_realtime_priority(); 
}

ScopedPriority::~ScopedPriority() { 
    LOG_DEBUG_STREAM << "ScopedPriority: restoring priority";
    try {
        restore_priority();
    } catch (...) {
        LOG_ERROR_STREAM << "Exception in ScopedPriority destructor";
    }
}

ScopedFrequencyLock::ScopedFrequencyLock() { 
    LOG_DEBUG_STREAM << "ScopedFrequencyLock: acquiring frequency lock";
    lock_cpu_frequency(); 
}

ScopedFrequencyLock::~ScopedFrequencyLock() { 
    LOG_DEBUG_STREAM << "ScopedFrequencyLock: releasing frequency lock";
    try {
        restore_cpu_frequency();
    } catch (...) {
        LOG_ERROR_STREAM << "Exception in ScopedFrequencyLock destructor";
    }
}

} // namespace os
