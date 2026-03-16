#pragma once

#include "os_fwd.hpp"
#include <cstddef>
#include <cstdint>

namespace os {

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

} // namespace os
