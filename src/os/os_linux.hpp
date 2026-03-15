#pragma once
#include "logger.hpp"
#include "arch.hpp"
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fstream>
#include <cpuid.h>
#include <cstring>
#include <ctime>

namespace os {

// --- Exceptions ---
class SystemError : public std::runtime_error {
public:
    explicit SystemError(const std::string& msg) : std::runtime_error(msg) {}
};

class PermissionError : public SystemError {
public:
    explicit PermissionError(const std::string& msg) : SystemError(msg) {}
};

class ResourceError : public SystemError {
public:
    explicit ResourceError(const std::string& msg) : SystemError(msg) {}
};

bool tsc_is_invariant();
void bind_thread_to_cpu(int cpu);
size_t cache_line_size();
uint64_t tick_frequency();
void* huge_alloc(size_t size);
void huge_free(void* ptr, size_t size);
void* aligned_alloc(size_t alignment, size_t size);
void aligned_free(void* ptr);
void set_realtime_priority();
void restore_priority();
void lock_cpu_frequency();
void restore_cpu_frequency();

// Классы RAII
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
