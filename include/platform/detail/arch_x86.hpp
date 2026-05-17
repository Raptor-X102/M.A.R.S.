// arch_x86.hpp
#pragma once

#include <cpuid.h>
#include <array>
#include <cstdint>
#include <x86intrin.h>
#include <cstring>
#include <string>
#include <asmjit/x86.h>
#include <stdexcept>
#include <memory>
#include <unordered_map>
#include <cstdio>
#include <mutex>
#include <vector>
#include "infra/logging.hpp"
#include "shared_types/cpu_vendor.hpp"

namespace silicon_probe::platform::arch {

// ============================================================================
// AsmJit version helpers (template implementations must be in header)
// ============================================================================

#if defined(ASMJIT_LIBRARY_VERSION) && ASMJIT_LIBRARY_VERSION >= ASMJIT_LIBRARY_MAKE_VERSION(1, 21, 0)
template <typename Assembler>
inline asmjit::Label asmjit_new_label(Assembler& assembler) {
    return assembler.new_label();
}

inline void asmjit_set_logger(asmjit::CodeHolder& code, asmjit::Logger* logger) {
    code.set_logger(logger);
}

inline size_t asmjit_code_size(const asmjit::CodeHolder& code) {
    return code.code_size();
}
#else
template <typename Assembler>
inline asmjit::Label asmjit_new_label(Assembler& assembler) {
    return assembler.newLabel();
}

inline void asmjit_set_logger(asmjit::CodeHolder& code, asmjit::Logger* logger) {
    code.setLogger(logger);
}

inline size_t asmjit_code_size(const asmjit::CodeHolder& code) {
    return code.codeSize();
}
#endif

// ============================================================================
// Low-level x86 utilities (declarations only)
// ============================================================================

inline void mfence() { _mm_mfence(); }
inline void sfence() { _mm_sfence(); }
inline void lfence() { _mm_lfence(); }

inline uint64_t tick() {
    mfence();
    unsigned int aux = 0;
    uint64_t t = __rdtscp(&aux);
    mfence();
    return t;
}

inline void pause() noexcept { _mm_pause(); }
inline void clflush(void* ptr) { _mm_clflush(ptr); }
inline void flush_complete() { _mm_mfence(); }

inline void stream_store(void* ptr, uint32_t value) {
    _mm_stream_si32(static_cast<int*>(ptr), static_cast<int>(value));
}

inline void serialize_pipeline() noexcept {
    __asm__ __volatile__(
        "lfence\n\t"
        "cpuid\n\t"
        "lfence\n\t"
        ::: "rax", "rbx", "rcx", "rdx", "memory"
    );
}

inline void write_non_temporal(int *p, int a) {
    _mm_stream_si32(p, a);
}

using CpuVendor = silicon_probe::platform::cpu_vendor::CpuVendor;
inline CpuVendor detect_vendor() noexcept {
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    __cpuid_count(0, 0, eax, ebx, ecx, edx);
    
    char vendor[13] = {0};
    std::memcpy(vendor, &ebx, 4);
    std::memcpy(vendor + 4, &edx, 4);
    std::memcpy(vendor + 8, &ecx, 4);
     
    using CpuVendorID = CpuVendor::CpuVendorID;
    CpuVendorID id = CpuVendorID::Unknown;
    
    if (std::strcmp(vendor, "GenuineIntel") == 0) {
        id = CpuVendorID::Intel;
    } else if (std::strcmp(vendor, "AuthenticAMD") == 0) { 
        id = CpuVendorID::AMD;
    }

    return {id, std::string{vendor}};
}

// ============================================================================
// Instruction types for code generation
// ============================================================================

enum InstrType : int {
    NOP,
    ADD_IMM1,
    SUB_IMM1,
    MUL_FLOAT,
    ADD_REG,
    MOV_IMM,
    XOR_ZERO,
    INC,
    DEC,
    SUB_REG,
    IMUL_REG,
    AND_REG,
    OR_REG,
    SHL_IMM1,
    SHR_IMM1,
    NOT,
    NEG,
    LOAD_FROM_RCX,   // mov reg, [rcx]
    STORE_TO_RCX,    // mov [rcx], reg
    LOAD_FROM_RDX,   // mov reg, [rdx]
    STORE_TO_RDX
};

// ============================================================================
// ROB (Reorder Buffer) code generation
// ============================================================================

namespace x86_rob_detail {

class RobCodeGenerator {
public:
    static constexpr size_t kDefaultIterations = 8192;
    static constexpr int kUnroll = 17;
    static constexpr size_t kBufEntries = 4 * 1024 * 1024;

    static RobCodeGenerator& instance();
    void set_iterations(size_t its);
    void* generate(int filler_cnt, int instr_type = 4);
    void release_current();

private:
    RobCodeGenerator();
    ~RobCodeGenerator();
    RobCodeGenerator(const RobCodeGenerator&) = delete;
    RobCodeGenerator& operator=(const RobCodeGenerator&) = delete;

    void init_buffers();
    static void emit_filler(asmjit::x86::Assembler& a, int instr_type,
                            size_t& seq_counter, int& global_idx, int /*idx_hint*/);

    asmjit::JitRuntime runtime_;
    void* current_fn_ = nullptr;
    void* dbuf1_ = nullptr;
    void* dbuf2_ = nullptr;
    void* dbuf2_orig_ = nullptr;
    size_t dbuf_size_ = 0;
    size_t iterations_ = kDefaultIterations;
};

} // namespace x86_rob_detail

inline void* generate_rob_code(int filler_cnt, int instr_type) {
    return x86_rob_detail::RobCodeGenerator::instance().generate(filler_cnt, instr_type);
}

inline void release_rob_code() {
    x86_rob_detail::RobCodeGenerator::instance().release_current();
}

inline void set_rob_inner_iterations(size_t its) {
    x86_rob_detail::RobCodeGenerator::instance().set_iterations(its);
}

// ============================================================================
// Execution ports code generation
// ============================================================================

namespace x86_exec_ports_detail {

class ExecPortsCodeGenerator {
public:
    static ExecPortsCodeGenerator& instance();
    ExecPortsCodeGenerator(const ExecPortsCodeGenerator&) = delete;
    ExecPortsCodeGenerator& operator=(const ExecPortsCodeGenerator&) = delete;

    void* generate(size_t instr_cnt, const std::vector<InstrType>& types);
    void release_all();
    void release_current();

private:
    static constexpr size_t kBufEntries = 4 * 1024 * 1024;

    ExecPortsCodeGenerator();
    ~ExecPortsCodeGenerator();

    void init_buffers();

    using EmitterFunc = void(*)(asmjit::x86::Assembler&, size_t idx);

   static constexpr asmjit::x86::Gp kAllRegs[] = {
        asmjit::x86::rax, asmjit::x86::rbx,
        asmjit::x86::rbp, asmjit::x86::rsi, asmjit::x86::rdi,
        asmjit::x86::r8,  asmjit::x86::r9,  asmjit::x86::r10, asmjit::x86::r11,
        asmjit::x86::r12, asmjit::x86::r13, asmjit::x86::r14, asmjit::x86::r15
    }; 

    static constexpr size_t kNumRegs = sizeof(kAllRegs) / sizeof(kAllRegs[0]);

    static auto dst_reg(size_t idx);
    static auto src_reg(size_t idx);
    static auto dst_xmm(size_t idx);

    static void emit_nop(asmjit::x86::Assembler& a, size_t);
    static void emit_add_imm1(asmjit::x86::Assembler& a, size_t);
    static void emit_sub_imm1(asmjit::x86::Assembler& a, size_t idx);
    static void emit_mul_float(asmjit::x86::Assembler& a, size_t);
    static void emit_add_reg(asmjit::x86::Assembler& a, size_t);
    static void emit_mov_imm(asmjit::x86::Assembler& a, size_t idx);
    static void emit_xor_zero(asmjit::x86::Assembler& a, size_t idx);
    static void emit_inc(asmjit::x86::Assembler& a, size_t idx);
    static void emit_dec(asmjit::x86::Assembler& a, size_t idx);
    static void emit_sub_reg(asmjit::x86::Assembler& a, size_t idx);
    static void emit_imul_reg(asmjit::x86::Assembler& a, size_t idx);
    static void emit_and_reg(asmjit::x86::Assembler& a, size_t idx);
    static void emit_or_reg(asmjit::x86::Assembler& a, size_t idx);
    static void emit_shl_imm1(asmjit::x86::Assembler& a, size_t idx);
    static void emit_shr_imm1(asmjit::x86::Assembler& a, size_t idx);
    static void emit_not(asmjit::x86::Assembler& a, size_t idx);
    static void emit_neg(asmjit::x86::Assembler& a, size_t idx);
    static void emit_load_from_rcx(asmjit::x86::Assembler& a, size_t);
    static void emit_store_to_rcx(asmjit::x86::Assembler& a, size_t idx);
    static void emit_load_from_rdx(asmjit::x86::Assembler& a, size_t);
    static void emit_store_to_rdx(asmjit::x86::Assembler& a, size_t idx);

    static EmitterFunc get_emitter(InstrType type);

    void* dbuf1_ = nullptr;
    void* dbuf2_ = nullptr;
    void* dbuf2_orig_ = nullptr;
    size_t dbuf_size_ = 0;
    FILE* log_file_ = nullptr;
    int gen_call_count_ = 0;

    asmjit::JitRuntime runtime_;
    std::vector<void*> functions_;
};

} // namespace x86_exec_ports_detail

inline void* generate_exec_ports_code(size_t instr_cnt, const std::vector<InstrType>& types) {
    return x86_exec_ports_detail::ExecPortsCodeGenerator::instance().generate(instr_cnt, types);
}

inline void release_exec_ports_code() {
    x86_exec_ports_detail::ExecPortsCodeGenerator::instance().release_all();
}

// ============================================================================
// µops cache code generation
// ============================================================================

namespace x86_uops_cache_detail {

class UopsCacheCodeGenerator {
public:
    static UopsCacheCodeGenerator& instance();

    static inline void enable_logging(const char* filename) {
        instance().enable_logging_impl(filename);
    }

    static inline void disable_logging() {
        instance().disable_logging_impl();
    }

    void* generate(size_t instr_cnt, size_t iterations, const std::vector<InstrType>& types);
    void release_current();

private:
    UopsCacheCodeGenerator();
    ~UopsCacheCodeGenerator();

    void enable_logging_impl(const char* filename);
    void disable_logging_impl();
    void release_current_impl();
    void clear_cache(void* addr, size_t size);

    static void emit_instruction(asmjit::x86::Assembler& a, size_t idx, InstrType type);
    static asmjit::x86::Gp dst_reg(size_t idx);

    FILE* log_file_;
    int gen_call_count_;
    void* current_function_;
    asmjit::JitRuntime runtime_;
    std::mutex mutex_;
};

} // namespace x86_uops_cache_detail

inline void* generate_uops_cache_code(size_t instr_cnt, size_t iterations, const std::vector<InstrType>& types) {
    return x86_uops_cache_detail::UopsCacheCodeGenerator::instance().generate(instr_cnt, iterations, types);
}

inline void release_uops_cache_code() {
    x86_uops_cache_detail::UopsCacheCodeGenerator::instance().release_current();
}

inline void enable_uops_cache_code_logging(const char* filename) {
    x86_uops_cache_detail::UopsCacheCodeGenerator::enable_logging(filename);
}

inline void disable_uops_cache_code_logging() {
    x86_uops_cache_detail::UopsCacheCodeGenerator::disable_logging();
}

// ============================================================================
// Branch Target Buffer code generation
// ============================================================================

namespace x86_branch_target_buffer_detail {

class BranchTargetBufferCodeGenerator {
public:
    static BranchTargetBufferCodeGenerator& instance();
    BranchTargetBufferCodeGenerator(const BranchTargetBufferCodeGenerator&) = delete;
    BranchTargetBufferCodeGenerator& operator=(const BranchTargetBufferCodeGenerator&) = delete;

    std::vector<void*> generate(size_t blocks_cnt, size_t iterations, int alignment);
    void release_measure_func();
    void release_warmup_func();
    void release_all();

private:
    BranchTargetBufferCodeGenerator();
    ~BranchTargetBufferCodeGenerator();

    FILE* log_file_;
    int gen_call_count_;
    void* warmup_function_;
    void* measure_function_;
    asmjit::JitRuntime runtime_;
};

} // namespace x86_branch_target_buffer_detail

inline std::vector<void*> generate_branch_target_buffer_code(size_t blocks_cnt, size_t iterations, int alignment) {
    return x86_branch_target_buffer_detail::BranchTargetBufferCodeGenerator::instance().generate(blocks_cnt, iterations, alignment);
}

inline void release_branch_target_buffer_code() {
    x86_branch_target_buffer_detail::BranchTargetBufferCodeGenerator::instance().release_all();
}

} // namespace silicon_probe::platform::arch
