#pragma once

#include <cstdint>
#include <string>
#include <functional>
#include "silicon_probe/platform/cpu_vendor.hpp"

namespace silicon_probe::platform::arch {

enum InstrType : int;

inline uint64_t tick();
inline void mfence();
inline void sfence();
inline void lfence();
inline void pause() noexcept;
inline void clflush(void* ptr);
inline void flush_complete();
inline void stream_store(void* ptr, uint32_t value);
inline void serialize_pipeline() noexcept;
inline silicon_probe::platform::cpu_vendor::CpuVendor detect_vendor() noexcept;
inline void* generate_rob_code(int instr_cnt);
inline void release_rob_code();
inline void set_rob_inner_iterations(size_t its);
inline void* generate_exec_ports_codegenerate(size_t instr_cnt, const std::vector<InstrType>& types);
inline void release_exec_ports_code();

} // namespace silicon_probe::platform::arch

#if defined(__x86_64__) || defined(_M_X64)
#include "silicon_probe/platform/detail/arch_x86.ipp"
#else
#error "Unsupported architecture"
#endif
