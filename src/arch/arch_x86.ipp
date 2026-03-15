#pragma once

#include <cstdint>
#include <x86intrin.h>
#include <cpuid.h>

namespace arch {

inline uint64_t tick() {
    unsigned int aux;
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

} // namespace arch
