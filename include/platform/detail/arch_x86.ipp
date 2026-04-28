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
#include "infra/logging.hpp"
#include <random>

namespace silicon_probe::platform::arch {

inline uint64_t tick() {
    _mm_lfence();
    unsigned int aux = 0;
    uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
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

namespace x86_exec_ports_detail {

class ExecPortsCodeGenerator {
public:
    static ExecPortsCodeGenerator& instance() {
        static ExecPortsCodeGenerator gen;
        return gen;
    }

    void* generate(size_t instr_cnt, const std::vector<InstrType>& types) {
        if (types.empty() || instr_cnt == 0) return nullptr;

        std::vector<EmitterFunc> emitters;
        emitters.reserve(types.size());
        for (InstrType t : types) {
            emitters.push_back(get_emitter(t));
        }

        asmjit::CodeHolder code;
        code.init(runtime_.environment());
        asmjit::x86::Assembler a(&code);
        std::unique_ptr<asmjit::FileLogger> logger;
        if (log_file_) {
            ++gen_call_count_;
            fprintf(log_file_, "\n\n;;; ========================================\n");
            fprintf(log_file_, ";;; Generated function #%d (instr_cnt=%zu, types: ", gen_call_count_, instr_cnt);
            for (auto t : types) fprintf(log_file_, "%d ", (int)t);
            fprintf(log_file_, ")\n;;; ========================================\n");
            fflush(log_file_);
            logger = std::make_unique<asmjit::FileLogger>(log_file_);
            code.set_logger(logger.get());
        }

        a.push(asmjit::x86::rbx);
        a.push(asmjit::x86::rbp);
        a.push(asmjit::x86::rsi);
        a.push(asmjit::x86::rdi);
        a.push(asmjit::x86::r12);
        a.push(asmjit::x86::r13);
        a.push(asmjit::x86::r14);
        a.push(asmjit::x86::r15);
        uint64_t dbuf1_addr = reinterpret_cast<uint64_t>(dbuf1_);
        uint64_t dbuf2_addr = reinterpret_cast<uint64_t>(dbuf2_);
        a.mov(asmjit::x86::rcx, asmjit::imm(dbuf1_addr));
        a.mov(asmjit::x86::rdx, asmjit::imm(dbuf2_addr));

        a.mov(asmjit::x86::eax, asmjit::imm(0x3F8147AE));
        a.movd(asmjit::x86::xmm(15), asmjit::x86::eax);
        a.shl(asmjit::x86::rax, asmjit::imm(32));
        a.movd(asmjit::x86::xmm(15), asmjit::x86::eax);

        size_t num_types = emitters.size();
        for (size_t i = 0; i < instr_cnt; ++i) {
            emitters[i % num_types](a, i);
        }

        a.pop(asmjit::x86::r15);
        a.pop(asmjit::x86::r14);
        a.pop(asmjit::x86::r13);
        a.pop(asmjit::x86::r12);
        a.pop(asmjit::x86::rdi);
        a.pop(asmjit::x86::rsi);
        a.pop(asmjit::x86::rbp);
        a.pop(asmjit::x86::rbx);
        a.ret();

        void* fn = nullptr;
        if (runtime_.add(&fn, &code) == asmjit::kErrorOk) {
            functions_.push_back(fn);
            __builtin___clear_cache(reinterpret_cast<char*>(fn),
                                    reinterpret_cast<char*>(fn) + code.code_size());
        }
        return fn;
    }

    void release_all() {
        for (void* fn : functions_) {
            if (fn) runtime_.release(fn);
        }
        functions_.clear();
    }

    void release_current() { release_all(); }

private:
    static constexpr size_t kBufEntries = 4 * 1024 * 1024;

    void* dbuf1_ = nullptr;
    void* dbuf2_ = nullptr;
    void* dbuf2_orig_ = nullptr;
    size_t dbuf_size_ = 0;
    FILE* log_file_ = nullptr;
    int gen_call_count_ = 0;

    void init_buffers() {
        dbuf_size_ = kBufEntries * sizeof(void*);
        dbuf1_ = mmap(nullptr, dbuf_size_, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        dbuf2_orig_ = mmap(nullptr, dbuf_size_, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        if (dbuf1_ == MAP_FAILED || dbuf2_orig_ == MAP_FAILED) {
            throw std::runtime_error("Failed to allocate buffers for ExecPortsCodeGenerator");
        }

        void** p1 = static_cast<void**>(dbuf1_);
        void** p2 = static_cast<void**>(dbuf2_orig_);
        for (size_t i = 0; i < kBufEntries; ++i) {
            p1[i] = &p1[i];
            p2[i] = &p2[i];
        }

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

    ExecPortsCodeGenerator() {
        init_buffers();
        log_file_ = fopen("exec_ports_code_dump.txt", "w");
        if (!log_file_) {
            SPDLOG_WARN("failed to open logging file");
        }
    }

    ~ExecPortsCodeGenerator() {
        release_all();
        if (dbuf1_) munmap(dbuf1_, dbuf_size_);
        if (dbuf2_orig_) munmap(dbuf2_orig_, dbuf_size_);
        if (log_file_) fclose(log_file_);
    }

    using EmitterFunc = void(*)(asmjit::x86::Assembler&, size_t idx);

    static constexpr asmjit::x86::Gp kAllRegs[] = {
        asmjit::x86::rax, asmjit::x86::rbx,
        asmjit::x86::rbp, asmjit::x86::rsi, asmjit::x86::rdi,
        asmjit::x86::r8,  asmjit::x86::r9,  asmjit::x86::r10, asmjit::x86::r11,
        asmjit::x86::r12, asmjit::x86::r13, asmjit::x86::r14, asmjit::x86::r15
    };
    static constexpr size_t kNumRegs = sizeof(kAllRegs) / sizeof(kAllRegs[0]);

    static auto dst_reg(size_t idx) { return kAllRegs[idx % kNumRegs]; }
    static auto src_reg(size_t idx) { return kAllRegs[(idx + 1) % kNumRegs]; }
    static auto dst_xmm(size_t idx) { return asmjit::x86::xmm(idx % 16); }
    static auto src_xmm(size_t idx) { return asmjit::x86::xmm((idx + 1) % 16); }

    static void emit_nop(asmjit::x86::Assembler& a, size_t) { a.nop(); }
    static void emit_add_imm1(asmjit::x86::Assembler& a, size_t) { a.add(dst_reg(0), asmjit::imm(1)); }
    static void emit_sub_imm1(asmjit::x86::Assembler& a, size_t idx) { a.sub(dst_reg(idx), asmjit::imm(1)); }
    static void emit_mul_float(asmjit::x86::Assembler& a, size_t) { a.mulss(dst_xmm(1), asmjit::x86::xmm(15)); }
    static void emit_add_reg(asmjit::x86::Assembler& a, size_t) { a.add(asmjit::x86::rax, asmjit::x86::rax); }
    static void emit_mov_imm(asmjit::x86::Assembler& a, size_t idx) { a.mov(dst_reg(idx), asmjit::imm(static_cast<int64_t>(idx))); }
    static void emit_xor_zero(asmjit::x86::Assembler& a, size_t idx) { a.xor_(dst_reg(idx), dst_reg(idx)); }
    static void emit_inc(asmjit::x86::Assembler& a, size_t idx) { a.inc(dst_reg(idx)); }
    static void emit_dec(asmjit::x86::Assembler& a, size_t idx) { a.dec(dst_reg(idx)); }
    static void emit_sub_reg(asmjit::x86::Assembler& a, size_t idx) { a.sub(dst_reg(idx), src_reg(idx)); }
    static void emit_imul_reg(asmjit::x86::Assembler& a, size_t idx) { a.imul(dst_reg(idx), src_reg(idx)); }
    static void emit_and_reg(asmjit::x86::Assembler& a, size_t idx) { a.and_(dst_reg(idx), src_reg(idx)); }
    static void emit_or_reg(asmjit::x86::Assembler& a, size_t idx) { a.or_(dst_reg(idx), src_reg(idx)); }
    static void emit_shl_imm1(asmjit::x86::Assembler& a, size_t idx) { a.shl(dst_reg(idx), asmjit::imm(1)); }
    static void emit_shr_imm1(asmjit::x86::Assembler& a, size_t idx) { a.shr(dst_reg(idx), asmjit::imm(1)); }
    static void emit_not(asmjit::x86::Assembler& a, size_t idx) { a.not_(dst_reg(idx)); }
    static void emit_neg(asmjit::x86::Assembler& a, size_t idx) { a.neg(dst_reg(idx)); }
    static void emit_load_from_rcx(asmjit::x86::Assembler& a, size_t) { a.add(asmjit::x86::rbx, asmjit::x86::ptr(asmjit::x86::rcx)); }
    static void emit_store_to_rcx(asmjit::x86::Assembler& a, size_t idx) { a.mov(asmjit::x86::ptr(asmjit::x86::rcx), src_reg(idx)); }
    static void emit_load_from_rdx(asmjit::x86::Assembler& a, size_t) { a.add(asmjit::x86::rbx, asmjit::x86::ptr(asmjit::x86::rdx)); }
    static void emit_store_to_rdx(asmjit::x86::Assembler& a, size_t idx) { a.mov(asmjit::x86::ptr(asmjit::x86::rdx), src_reg(idx)); }

    static EmitterFunc get_emitter(InstrType type) {
        static const EmitterFunc table[] = {
            emit_nop,           // NOP
            emit_add_imm1,      // ADD_IMM1
            emit_sub_imm1,      // SUB_IMM1
            emit_mul_float,     // MUL_FLOAT
            emit_add_reg,       // ADD_REG
            emit_mov_imm,       // MOV_IMM
            emit_xor_zero,      // XOR_ZERO
            emit_inc,           // INC
            emit_dec,           // DEC
            emit_sub_reg,       // SUB_REG
            emit_imul_reg,      // IMUL_REG
            emit_and_reg,       // AND_REG
            emit_or_reg,        // OR_REG
            emit_shl_imm1,      // SHL_IMM1
            emit_shr_imm1,      // SHR_IMM1
            emit_not,           // NOT
            emit_neg,           // NEG
            emit_load_from_rcx, // LOAD_FROM_RCX
            emit_store_to_rcx,  // STORE_TO_RCX
            emit_load_from_rdx, // LOAD_FROM_RDX
            emit_store_to_rdx   // STORE_TO_RDX
        };
        return table[static_cast<int>(type)];
    }

    asmjit::JitRuntime runtime_;
    std::vector<void*> functions_;
};

} // namespace x86_exec_ports_detail

inline void* generate_exec_ports_codegenerate(size_t instr_cnt, const std::vector<InstrType>& types) {
    return x86_exec_ports_detail::ExecPortsCodeGenerator::instance().generate(instr_cnt, types);
}

inline void release_exec_ports_code() {
    x86_exec_ports_detail::ExecPortsCodeGenerator::instance().release_all();
}

namespace x86_uops_cache_detail {

class UopsCacheCodeGenerator {
public:
    static UopsCacheCodeGenerator& instance() {
        static UopsCacheCodeGenerator gen;
        return gen;
    }

    void* generate(size_t instr_cnt, size_t iterations, const std::vector<InstrType>& types) {
        release_current();

        if (types.empty() || instr_cnt == 0) return nullptr;

        std::vector<EmitterFunc> emitters;
        emitters.reserve(types.size());
        for (InstrType t : types) {
            emitters.push_back(get_emitter(t));
        }

        asmjit::CodeHolder code;
        code.init(runtime_.environment());
        asmjit::x86::Assembler a(&code);
        std::unique_ptr<asmjit::FileLogger> logger;
        if (log_file_) {
            ++gen_call_count_;
            fprintf(log_file_, "\n\n;;; ========================================\n");
            fprintf(log_file_, ";;; Generated function #%d (instr_cnt=%zu, types: ", gen_call_count_, instr_cnt);
            for (auto t : types) fprintf(log_file_, "%d ", (int)t);
            fprintf(log_file_, ")\n;;; ========================================\n");
            fflush(log_file_);
            logger = std::make_unique<asmjit::FileLogger>(log_file_);
            code.set_logger(logger.get());
        }

        a.push(asmjit::x86::rbx);
        a.push(asmjit::x86::rbp);
        a.push(asmjit::x86::rsi);
        a.push(asmjit::x86::rdi);
        a.push(asmjit::x86::r12);
        a.push(asmjit::x86::r13);
        a.push(asmjit::x86::r14);
        a.push(asmjit::x86::r15);
        a.mov(asmjit::x86::rcx, asmjit::imm(iterations));
        a.align(asmjit::AlignMode::kCode, 16);
        asmjit::Label loop_start = a.new_label();
        a.bind(loop_start);

        size_t num_types = emitters.size();
        for (size_t i = 0; i < instr_cnt; ++i) {
            emitters[i % num_types](a, i);
        }

        a.dec(asmjit::x86::rcx);
        a.jnz(loop_start);
        a.pop(asmjit::x86::r15);
        a.pop(asmjit::x86::r14);
        a.pop(asmjit::x86::r13);
        a.pop(asmjit::x86::r12);
        a.pop(asmjit::x86::rdi);
        a.pop(asmjit::x86::rsi);
        a.pop(asmjit::x86::rbp);
        a.pop(asmjit::x86::rbx);
        a.ret();

        void* fn = nullptr;
        if (runtime_.add(&fn, &code) == asmjit::kErrorOk) {
            current_function_ = fn;
            __builtin___clear_cache(reinterpret_cast<char*>(fn),
                                    reinterpret_cast<char*>(fn) + code.code_size());
        }
        return fn;
    }

    void release_current() {
        if (current_function_) {
            runtime_.release(current_function_);
            current_function_ = nullptr;
        }
    }

private:
    FILE* log_file_ = nullptr;
    int gen_call_count_ = 0;
    void* current_function_ = nullptr;  
    asmjit::JitRuntime runtime_;

    UopsCacheCodeGenerator() {
        log_file_ = fopen("uops_cache_code_dump.txt", "w");
        if (!log_file_) {
            SPDLOG_WARN("failed to open logging file");
        }
    }

    ~UopsCacheCodeGenerator() {
        release_current();
        if (log_file_) fclose(log_file_);
    }

    using EmitterFunc = void(*)(asmjit::x86::Assembler&, size_t idx);

    static constexpr asmjit::x86::Gp kAllRegs[] = {
        asmjit::x86::rax, asmjit::x86::rbx,
        asmjit::x86::rbp, asmjit::x86::rsi, asmjit::x86::rdi,
        asmjit::x86::r8,  asmjit::x86::r9,  asmjit::x86::r10, asmjit::x86::r11,
        asmjit::x86::r12, asmjit::x86::r13, asmjit::x86::r14, asmjit::x86::r15
    };
    static constexpr size_t kNumRegs = sizeof(kAllRegs) / sizeof(kAllRegs[0]);

    static auto dst_reg(size_t idx) { return kAllRegs[idx % kNumRegs]; }
    static auto src_reg(size_t idx) { return kAllRegs[(idx + 1) % kNumRegs]; }

    static void emit_add_reg(asmjit::x86::Assembler& a, size_t /*idx*/) { a.add(dst_reg(0), dst_reg(0)); }
    static void emit_add_imm1(asmjit::x86::Assembler& a, size_t idx) { a.add(dst_reg(idx), asmjit::imm(1)); }
    static void emit_nop(asmjit::x86::Assembler& a, size_t) { a.nop(); }

    // TODO: either change logic, or add more emitters
    static EmitterFunc get_emitter(InstrType type) {
        static const EmitterFunc table[] = {
            emit_nop,           // NOP
            emit_add_imm1,      // ADD_IMM1
            emit_nop,           // NOP
            emit_nop,           // NOP
            emit_add_reg,       // ADD_REG
            emit_nop,           // NOP
            emit_nop,           // NOP
            emit_nop,           // NOP
            emit_nop,           // NOP
            emit_nop,           // NOP
            emit_nop,           // NOP
            emit_nop,           // NOP
            emit_nop,           // NOP
            emit_nop,           // NOP
            emit_nop,           // NOP
            emit_nop,           // NOP
            emit_nop,           // NOP
            emit_nop,           // NOP
            emit_nop,           // NOP
            emit_nop,           // NOP
            emit_nop,           // NOP
        };
        return table[static_cast<int>(type)];
    }
};

} // namespace x86_uops_cache_detail

inline void* generate_uops_cache_codegenerate(size_t instr_cnt, 
                                              size_t iterations, 
                                              const std::vector<InstrType>& types) {
    return x86_uops_cache_detail::UopsCacheCodeGenerator::instance().generate(instr_cnt,iterations, types);
}

inline void release_uops_cache_code() {
    x86_uops_cache_detail::UopsCacheCodeGenerator::instance().release_current();
}

namespace x86_branch_target_buffer_detail {

class BranchTargetBufferCodeGenerator {
public:
    static BranchTargetBufferCodeGenerator& instance() {
        static BranchTargetBufferCodeGenerator gen;
        return gen;
    }

    std::vector<void*> generate(size_t blocks_cnt, size_t iterations, int alignment) {
        release_all();
        
        std::unique_ptr<asmjit::FileLogger> logger;
        if (log_file_) {
            ++gen_call_count_;
            fprintf(log_file_, "\n\n;;; ========================================\n");
            fprintf(log_file_, ";;; Generated function #%d (blocks_cnt=%zu, alignment=%d", gen_call_count_, blocks_cnt, alignment);
            fprintf(log_file_, ")\n;;; ========================================\n");
            fflush(log_file_);
            logger = std::make_unique<asmjit::FileLogger>(log_file_);
        }

        auto generate_body_func = [&](asmjit::x86::Assembler& a) {
            std::vector<asmjit::Label> labels(blocks_cnt);
            for (auto& label : labels) {
                label = a.new_label();
                a.lea(asmjit::x86::r11, asmjit::x86::ptr(label));
                a.jmp(asmjit::x86::r11);
                a.align(asmjit::AlignMode::kCode, alignment);
                a.bind(label);
            }
        };

        auto generate_warmup = [&]() -> void* {
            asmjit::CodeHolder code;
            code.init(runtime_.environment());
            code.set_logger(logger.get());
            asmjit::x86::Assembler a(&code);

            a.align(asmjit::AlignMode::kCode, alignment);
            generate_body_func(a);
            a.ret();
            void* fn = nullptr;
            if (runtime_.add(&fn, &code) == asmjit::kErrorOk) {
                warmup_function_ = fn;
                __builtin___clear_cache(reinterpret_cast<char*>(fn),
                                        reinterpret_cast<char*>(fn) + code.code_size());
            }

            return fn;
        };

        auto generate_measure = [&]() -> void* {
            asmjit::CodeHolder code;
            code.init(runtime_.environment());
            code.set_logger(logger.get());
            asmjit::x86::Assembler a(&code);

            a.mov(asmjit::x86::rcx, asmjit::imm(iterations));
            a.align(asmjit::AlignMode::kCode, alignment);
            asmjit::Label loop_start = a.new_label();
            a.bind(loop_start);
            generate_body_func(a);
            a.dec(asmjit::x86::rcx);
            a.jnz(loop_start);
            a.ret();

            void* fn = nullptr;
            if (runtime_.add(&fn, &code) == asmjit::kErrorOk) {
                warmup_function_ = fn;
                __builtin___clear_cache(reinterpret_cast<char*>(fn),
                                        reinterpret_cast<char*>(fn) + code.code_size());
            }

            return fn;
        };
        
        return {generate_warmup(), generate_measure()};
    }

    void release_measure_func() {
        if (measure_function_) {
            runtime_.release(measure_function_);
            measure_function_ = nullptr;
        }
    }

    void release_warmup_func() {
        if (warmup_function_) {
            runtime_.release(warmup_function_);
            warmup_function_ = nullptr;
        }
    }

    void release_all() {
        release_warmup_func();
        release_measure_func();
    }

private:
    FILE* log_file_ = nullptr;
    int gen_call_count_ = 0;
    void* warmup_function_ = nullptr;  
    void* measure_function_ = nullptr;  
    asmjit::JitRuntime runtime_;

    BranchTargetBufferCodeGenerator() {
        log_file_ = fopen("branch_target_buffer_code_dump.txt", "w");
        if (!log_file_) {
            SPDLOG_WARN("failed to open logging file");
        }
    }

    ~BranchTargetBufferCodeGenerator() {
        release_all();
        if (log_file_) fclose(log_file_);
    }
};

} // namespace x86_branch_target_buffer_detail

inline std::vector<void*> generate_branch_target_buffer_code(size_t blocks_cnt, 
                                                             size_t iterations, 
                                                             int alignment) {
    return x86_branch_target_buffer_detail::BranchTargetBufferCodeGenerator::instance().generate(blocks_cnt,
                                                                                                 iterations,
                                                                                                 alignment);
}

inline void release_branch_target_buffer_code() {
    x86_branch_target_buffer_detail::BranchTargetBufferCodeGenerator::instance().release_all();
}
} // namespace silicon_probe::platform::arch
