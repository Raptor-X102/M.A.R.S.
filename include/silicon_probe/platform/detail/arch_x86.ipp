#pragma once

#include <cpuid.h>
#include <cstdint>
#include <x86intrin.h>

namespace silicon_probe::platform::arch {

inline uint64_t tick() {
    unsigned int aux = 0;
    return __rdtscp(&aux);
}

inline void mfence() { _mm_mfence(); }
inline void sfence() { _mm_sfence(); }
inline void lfence() { _mm_lfence(); }

inline void clflush(void* ptr) {
    _mm_clflush(ptr);
}

inline void flush_complete() {
    _mm_mfence();
}

inline void stream_store(void* ptr, uint32_t value) {
    _mm_stream_si32(static_cast<int*>(ptr), static_cast<int>(value));
}

} // namespace silicon_probe::platform::arch
