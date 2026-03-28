#pragma once

#include <cstdint>

namespace silicon_probe::platform::arch {

inline uint64_t tick();
inline void mfence();
inline void sfence();
inline void lfence();
inline void clflush(void* ptr);
inline void flush_complete();
inline void stream_store(void* ptr, uint32_t value);

} // namespace silicon_probe::platform::arch

#if defined(__x86_64__) || defined(_M_X64)
#include "silicon_probe/platform/detail/arch_x86.ipp"
#else
#error "Unsupported architecture"
#endif
