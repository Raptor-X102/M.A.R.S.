#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "platform/os_errors.hpp"

namespace silicon_probe::platform {

struct MeasurementEnvironmentOptions {
    std::optional<int> cpu;
    bool realtime_priority = false;
    bool lock_frequency = false;
};

void bind_thread_to_cpu(int cpu);
size_t cache_line_size();
bool tsc_is_invariant();
uint64_t tick_frequency();
void* huge_alloc(size_t size);
void huge_free(void* ptr, size_t size);
void* aligned_alloc(size_t alignment, size_t size);
void aligned_free(void* ptr);
void set_realtime_priority();
void restore_priority();
void lock_cpu_frequency();
void restore_cpu_frequency();
void disable_turbo_boost();
void restore_turbo_boost();

class ScopedThreadAffinity {
   public:
    explicit ScopedThreadAffinity(int cpu);
    ~ScopedThreadAffinity();

    ScopedThreadAffinity(const ScopedThreadAffinity&) = delete;
    ScopedThreadAffinity& operator=(const ScopedThreadAffinity&) = delete;

   private:
    bool active_ = false;
    int previous_cpu_count_ = 0;
    struct cpu_set_t_storage;
    cpu_set_t_storage* previous_affinity_ = nullptr;
};

class ScopedPriority {
   public:
    ScopedPriority();
    ~ScopedPriority();

    ScopedPriority(const ScopedPriority&) = delete;
    ScopedPriority& operator=(const ScopedPriority&) = delete;
};

class ScopedFrequencyLock {
   public:
    ScopedFrequencyLock();
    ~ScopedFrequencyLock();

    ScopedFrequencyLock(const ScopedFrequencyLock&) = delete;
    ScopedFrequencyLock& operator=(const ScopedFrequencyLock&) = delete;
};

class ScopedMeasurementEnvironment {
   public:
    explicit ScopedMeasurementEnvironment(const MeasurementEnvironmentOptions& options);
    ~ScopedMeasurementEnvironment() = default;

    ScopedMeasurementEnvironment(const ScopedMeasurementEnvironment&) = delete;
    ScopedMeasurementEnvironment& operator=(const ScopedMeasurementEnvironment&) = delete;

   private:
    std::optional<ScopedThreadAffinity> affinity_;
    std::optional<ScopedPriority> priority_;
    std::optional<ScopedFrequencyLock> frequency_lock_;
};

}  // namespace silicon_probe::platform
