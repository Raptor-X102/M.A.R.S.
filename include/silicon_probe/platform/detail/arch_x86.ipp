#pragma once

#include <cpuid.h>
#include <cstdint>
#include <x86intrin.h>
#include <cstring>
#include <string>
#include <asmjit/x86.h>
#include <stdexcept>
#include <memory>
#include <unordered_map>
#include <unistd.h>
#include <sys/mman.h>
#include "silicon_probe/infra/logging.hpp"
#include <random>

namespace silicon_probe::platform::arch {

inline uint64_t tick() {
    unsigned int aux = 0;
    return __rdtscp(&aux);
}

inline void mfence() { _mm_mfence(); }
inline void sfence() { _mm_sfence(); }
inline void lfence() { _mm_lfence(); }
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

using CpuVendor = silicon_probe::platform::cpu_vendor::CpuVendor;

inline CpuVendor detect_vendor() noexcept {
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    __cpuid_count(0, 0, eax, ebx, ecx, edx);
    
    char vendor[13] = {0};
    std::memcpy(vendor, &ebx, 4);
    std::memcpy(vendor + 4, &edx, 4);
    std::memcpy(vendor + 8, &ecx, 3);
     
    using CpuVendorID = CpuVendor::CpuVendorID;
    CpuVendorID id = CpuVendorID::Unknown;
    
    if (std::strcmp(vendor, "GenuineIntel") == 0) {
        id = CpuVendorID::Intel;
    } else if (std::strcmp(vendor, "AuthenticAMD") == 0) { 
        id = CpuVendorID::AMD;
    }

    return {id, std::string{vendor}};
}

namespace x86_rob_detail {

// Predefined filler patterns matching Wong's add_filler()
static const bool is_xmm[128] = {
    0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0
};

class RobCodeGenerator {
public:
    static constexpr size_t kDefaultIterations = 8192;
    static constexpr int kUnroll = 17;
    static constexpr size_t kBufEntries = 4 * 1024 * 1024;

    static RobCodeGenerator& instance() {
        static RobCodeGenerator gen;
        return gen;
    }

    void set_iterations(size_t its) { iterations_ = its; }

    void* generate(int filler_cnt, int instr_type = 4) {
        release_current();

        asmjit::CodeHolder code;
        code.init(runtime_.environment());
        asmjit::x86::Assembler a(&code);

        // ----- Prologue: align and save callee-saved registers -----
        for (int i = 0; i < 16; ++i) a.nop();     // room for misaligned entry
        a.push(asmjit::x86::rbx);
        a.push(asmjit::x86::rbp);
        a.push(asmjit::x86::rsi);
        a.push(asmjit::x86::rdi);

        // Load buffer pointers (R8 and R9 as temporaries, then move to RCX/RDX)
        uint64_t dbuf1_addr = reinterpret_cast<uint64_t>(dbuf1_);
        uint64_t dbuf2_addr = reinterpret_cast<uint64_t>(dbuf2_);
        a.mov(asmjit::x86::rcx, asmjit::imm(dbuf1_addr));
        a.mov(asmjit::x86::rdx, asmjit::imm(dbuf2_addr));

        // Loop counter
        a.mov(asmjit::x86::rax, asmjit::imm(iterations_));

        // Initialize filler registers (RBX, RBP, RSI, RDI) with non‑zero values
        a.sub(asmjit::x86::rbx, asmjit::imm(1));
        a.sub(asmjit::x86::rbp, asmjit::imm(2));
        a.sub(asmjit::x86::rsi, asmjit::imm(3));
        a.sub(asmjit::x86::rdi, asmjit::imm(4));

        // Align loop start to 16 bytes
        a.align(asmjit::AlignMode::kCode, 16);
        asmjit::Label loop_start = a.new_label();
        a.bind(loop_start);

        int filler_idx = 0;
        const int icount = filler_cnt + 1;   // Wong's 'icount' = fillers between loads + 1

        for (int u = kUnroll - 1; u >= 0; --u) {
            // First block: 16 fillers (using j + icount - 1 - 16 index)
            for (int j = 0; j < 16; ++j) {
                emit_filler(a, instr_type, j + icount - 1 - 16, filler_idx);
            }

            // Load from RCX (first dependency)
            a.mov(asmjit::x86::rcx, asmjit::x86::ptr(asmjit::x86::rcx));

            // Second block: (icount - 1) fillers
            for (int j = 0; j < icount - 1; ++j) {
                emit_filler(a, instr_type, j, filler_idx);
            }

            // Load from RDX (second dependency)
            a.mov(asmjit::x86::rdx, asmjit::x86::ptr(asmjit::x86::rdx));

            // Third block: (icount - 1 - 16) fillers, subtract 1 on last unroll if not XMM
            int rem = icount - 1 - 16;
            if (u == 0 && !is_xmm[instr_type]) {
                rem -= 1;
            }
            for (int j = 0; j < rem; ++j) {
                emit_filler(a, instr_type, j, filler_idx);
            }
        }

        // Decrement loop counter and branch
        a.sub(asmjit::x86::rax, asmjit::imm(1));
        a.jnz(loop_start);

        // ----- Epilogue -----
        a.pop(asmjit::x86::rdi);
        a.pop(asmjit::x86::rsi);
        a.pop(asmjit::x86::rbp);
        a.pop(asmjit::x86::rbx);
        a.ret();

        if (runtime_.add(&current_fn_, &code) != asmjit::kErrorOk) {
            current_fn_ = nullptr;
        }

        if (current_fn_) {
            __builtin___clear_cache(reinterpret_cast<char*>(current_fn_),
                                    reinterpret_cast<char*>(current_fn_) + code.code_size());
        }

        return current_fn_;
    }

    void release_current() {
        if (current_fn_) {
            runtime_.release(current_fn_);
            current_fn_ = nullptr;
        }
    }

private:
    RobCodeGenerator() {
        init_buffers();
    }

    ~RobCodeGenerator() {
        release_current();
        if (dbuf1_) munmap(dbuf1_, dbuf_size_);
        if (dbuf2_orig_) munmap(dbuf2_orig_, dbuf_size_);
    }

    RobCodeGenerator(const RobCodeGenerator&) = delete;
    RobCodeGenerator& operator=(const RobCodeGenerator&) = delete;

    void init_buffers() {
        dbuf_size_ = kBufEntries * sizeof(void*);
        dbuf1_ = mmap(nullptr, dbuf_size_, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        dbuf2_orig_ = mmap(nullptr, dbuf_size_, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        if (dbuf1_ == MAP_FAILED || dbuf2_orig_ == MAP_FAILED) {
            throw std::runtime_error("Failed to allocate buffers for ROB measurer");
        }

        void** p1 = static_cast<void**>(dbuf1_);
        void** p2 = static_cast<void**>(dbuf2_orig_);
        for (size_t i = 0; i < kBufEntries; ++i) {
            p1[i] = &p1[i];
            p2[i] = &p2[i];
        }

        // Random permutation to avoid predictable access patterns
        std::mt19937_64 rng(12345);
        const size_t cycle_len = 8192 / sizeof(void*);
        for (size_t i = kBufEntries - 1; i > 0; --i) {
            if ((i & 0x1ff) == 0 && i >= cycle_len) {
                size_t k = (rng() % (i / cycle_len)) * cycle_len + (i % cycle_len);
                std::swap(p1[i], p1[k]);
                std::swap(p2[i], p2[k]);
            }
        }

        size_t offset = (kBufEntries / 4) * sizeof(void*);
        offset -= offset % 64;
        dbuf2_ = static_cast<char*>(dbuf2_orig_) + offset;
    }

    void emit_filler(asmjit::x86::Assembler& a, int instr_type, int /*idx*/, int& global_idx) {
        static int icount = 0;
        const int i = icount;
        const int reg[4] = {3, 5, 6, 7};  // EBX=3, EBP=5, ESI=6, EDI=7
        
        int i2 = (global_idx >> 2) & 1;
        
        switch (instr_type) {
            case 0: // add (ebx, ebp, esi, edi), (ebx, ebp, esi, edi)
                a.add(asmjit::x86::gpb(reg[i&3]), asmjit::x86::gpb(reg[i&3]));
                break;
            case 1: // nop
                a.nop();
                break;
            case 2: // mov (ebx, ebp, esi, edi), (ebx, ebp, esi, edi)
                a.mov(asmjit::x86::gpb(reg[i&3]), asmjit::x86::gpb(reg[i&3]));
                break;
            case 3: // cmp (ebx, ebp, esi, edi), (ebx, ebp, esi, edi)
                a.cmp(asmjit::x86::gpb(reg[i&3]), asmjit::x86::gpb(reg[i&3]));
                break;
            case 4: // two-byte nop 66 90
                a.emit(0x66);
                a.nop();
                break;
            case 5: // xor (ebx, ebp, esi, edi), (ebx, ebp, esi, edi)
                a.xor_(asmjit::x86::gpb(reg[i&3]), asmjit::x86::gpb(reg[i&3]));
                break;
            case 6: // xor (ebx, ebp, esi, edi), (edi, ebx, ebp, esi)
                a.xor_(asmjit::x86::gpb(reg[i&3]), asmjit::x86::gpb(reg[(i+1)&3]));
                break;
            case 7: // mov (ebx, ebp, esi, edi), (edi, ebx, ebp, esi)
                a.mov(asmjit::x86::gpb(reg[i&3]), asmjit::x86::gpb(reg[(i+1)&3]));
                break;
            case 8: // movaps xmm, xmm
                a.movaps(asmjit::x86::xmm(i&7), asmjit::x86::xmm((i+1)&7));
                break;
            case 9: // movdqa xmm, xmm SSE2
            case 12:
                a.movdqa(asmjit::x86::xmm(i&7), asmjit::x86::xmm((i+1)&7));
                break;
            case 10: // xorps xmm, xmm
                a.xorps(asmjit::x86::xmm(i&7), asmjit::x86::xmm(i&7));
                break;
            case 11: // xorps xmm, xmm+1
                a.xorps(asmjit::x86::xmm(i&7), asmjit::x86::xmm((i+1)&7));
                break;
            case 13: // movdqa xmm, xmm AVX
                a.vmovdqa(asmjit::x86::xmm(i&7), asmjit::x86::xmm((i+1)&7));
                break;
            case 14: // movdqa ymm, ymm AVX
                a.vmovdqa(asmjit::x86::ymm(i&7), asmjit::x86::ymm((i+1)&7));
                break;
            case 15: // movdqa xmm, xmm+1 SSE2
                a.movdqa(asmjit::x86::xmm(((i&3)+0)), asmjit::x86::xmm(((i+1)&3)+0));
                break;
            case 16: // movdqa xmm, xmm+1 AVX
                a.vmovdqa(asmjit::x86::xmm(((i&3)+0)), asmjit::x86::xmm(((i+1)&3)+0));
                break;
            case 17: // movdqa ymm, ymm+1 AVX
                a.vmovdqa(asmjit::x86::ymm(((i&3)+0)), asmjit::x86::ymm(((i+1)&3)+0));
                break;
            case 18: // vxorps ymm, ymm, ymm AVX
                a.vxorps(asmjit::x86::ymm(i&7), asmjit::x86::ymm(i&7), asmjit::x86::ymm(i&7));
                break;
            case 19: // vxorps ymm, ymm, ymm+1 AVX
                a.vxorps(asmjit::x86::ymm(i&7), asmjit::x86::ymm(i&7), asmjit::x86::ymm((i+1)&7));
                break;
            case 20:
                if (icount & 1) {
                    a.xorps(asmjit::x86::xmm(i&7), asmjit::x86::xmm((i+1)&7));
                } else {
                    if (sizeof(void*) == 4) {
                        a.add(asmjit::x86::gpb(reg[i&3]), asmjit::x86::gpb(reg[i&3]));
                    } else {
                        a.add(asmjit::x86::rbx, asmjit::x86::rbx);
                    }
                }
                break;
            case 21:
                if (i2 & 1) {
                    a.vxorps(asmjit::x86::ymm(i&7), asmjit::x86::ymm(i&7), asmjit::x86::ymm((i+1)&7));
                } else {
                    a.add(asmjit::x86::rbx, asmjit::x86::rbx);
                }
                break;
            case 22:
                a.xor_(asmjit::x86::gpb(reg[i&3]), asmjit::x86::gpb(reg[(i+1)&3]));
                break;
            case 23: // sub reg, val
                a.sub(asmjit::x86::gpb(reg[i&3]), asmjit::imm(i));
                break;
            case 24: // add64
                a.add(asmjit::x86::rbx, asmjit::x86::rbx);
                break;
            case 25: // mov64
                a.mov(asmjit::x86::rbx, asmjit::x86::rcx);
                break;
            default:
                a.nop();
                break;
        }
        
        icount++;
        global_idx++;
    }

    asmjit::JitRuntime runtime_;
    void* current_fn_ = nullptr;
    void* dbuf1_ = nullptr;
    void* dbuf2_ = nullptr;
    void* dbuf2_orig_ = nullptr;
    size_t dbuf_size_ = 0;
    size_t iterations_ = kDefaultIterations;
};

} // namespace x86_rob_detail

inline void* generate_rob_code(int filler_cnt, int instr_type = 4) {
    return x86_rob_detail::RobCodeGenerator::instance().generate(filler_cnt, instr_type);
}

inline void release_rob_code() {
    x86_rob_detail::RobCodeGenerator::instance().release_current();
}

inline void set_rob_inner_iterations(size_t its) {
    x86_rob_detail::RobCodeGenerator::instance().set_iterations(its);
}

} // namespace silicon_probe::platform::arch
