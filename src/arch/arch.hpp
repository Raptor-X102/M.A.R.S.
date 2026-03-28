#pragma once
#include <cstdint>

namespace arch {

// Read a high-resolution timestamp (usually CPU cycles)
inline uint64_t tick();

// Full memory barrier (loads and stores)
inline void mfence();

// Store barrier
inline void sfence();

// Load barrier
inline void lfence();

// Flush a single cache line containing the given address
inline void clflush(void* ptr);

// Ensure all previous flushes are globally visible (e.g., mfence)
inline void flush_complete();

// Non-temporal 32-bit store (bypasses cache)
inline void stream_store(void* ptr, uint32_t value);

} // namespace arch

// Include architecture-specific implementation
#if defined(__x86_64__) || defined(_M_X64)
#include "arch_x86.ipp"
//#elif defined(__arm__) || defined(__aarch64__)
//#include "arch_arm.h"
//#elif defined(__riscv)
//#include "arch_riscv.h"
#else
#error "Unsupported architecture"
#endif
