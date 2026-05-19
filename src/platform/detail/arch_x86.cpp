#include "platform/detail/arch_x86.hpp"
#include <random>
#include <unistd.h>
#include <sys/mman.h>

namespace silicon_probe::platform::arch {

// ============================================================================
// ROB detail definitions
// ============================================================================

namespace x86_rob_detail {

// Helper to test if instruction type uses XMM/YMM registers
static bool is_xmm_instruction(int instr_type) {
    return (instr_type >= 8 && instr_type <= 14) || (instr_type >= 18 && instr_type <= 19);
}

RobCodeGenerator& RobCodeGenerator::instance() {
    static RobCodeGenerator gen;
    return gen;
}

void RobCodeGenerator::set_iterations(size_t its) { iterations_ = its; }

void* RobCodeGenerator::generate(int filler_cnt, int instr_type) {
    release_current();

    asmjit::CodeHolder code;
    code.init(runtime_.environment());
    asmjit::x86::Assembler a(&code);

    // Prologue: save callee-saved registers
    a.push(asmjit::x86::rbx);
    a.push(asmjit::x86::rbp);
    a.push(asmjit::x86::rsi);
    a.push(asmjit::x86::rdi);

    // Load buffer pointers (using RCX, RDX)
    uint64_t dbuf1_addr = reinterpret_cast<uint64_t>(dbuf1_);
    uint64_t dbuf2_addr = reinterpret_cast<uint64_t>(dbuf2_);
    a.mov(asmjit::x86::rcx, asmjit::imm(dbuf1_addr));
    a.mov(asmjit::x86::rdx, asmjit::imm(dbuf2_addr));

    // Loop counter
    a.mov(asmjit::x86::rax, asmjit::imm(iterations_));

    // Initialize filler registers with arbitrary values (original method)
    a.sub(asmjit::x86::rbx, asmjit::imm(1));
    a.sub(asmjit::x86::rbp, asmjit::imm(2));
    a.sub(asmjit::x86::rsi, asmjit::imm(3));
    a.sub(asmjit::x86::rdi, asmjit::imm(4));

    // Align loop start to 16 bytes
    a.align(asmjit::AlignMode::kCode, 16);
    asmjit::Label loop_start = asmjit_new_label(a);
    a.bind(loop_start);

    // Filler sequence counter (replaces static icount)
    size_t filler_seq = 0;
    int filler_global = 0;

    const int icount = filler_cnt + 1;  // Wong's 'icount'

    for (int u = kUnroll - 1; u >= 0; --u) {
        // 1) 16 fillers before first load
        for (int j = 0; j < 16; ++j) {
            emit_filler(a, instr_type, filler_seq, filler_global, j + icount - 1 - 16);
        }
        // Load from RCX (first dependency)
        a.mov(asmjit::x86::rcx, asmjit::x86::ptr(asmjit::x86::rcx));

        // 2) (icount - 1) fillers
        for (int j = 0; j < icount - 1; ++j) {
            emit_filler(a, instr_type, filler_seq, filler_global, j);
        }
        // Load from RDX (second dependency)
        a.mov(asmjit::x86::rdx, asmjit::x86::ptr(asmjit::x86::rdx));

        // 3) Remaining fillers (icount - 1 - 16)
        int rem = icount - 1 - 16;
        if (u == 0 && !is_xmm_instruction(instr_type)) {
            rem -= 1;
        }
        for (int j = 0; j < rem; ++j) {
            emit_filler(a, instr_type, filler_seq, filler_global, j);
        }
    }

    // Decrement loop counter and branch
    a.sub(asmjit::x86::rax, asmjit::imm(1));
    a.jnz(loop_start);

    // Epilogue
    a.pop(asmjit::x86::rdi);
    a.pop(asmjit::x86::rsi);
    a.pop(asmjit::x86::rbp);
    a.pop(asmjit::x86::rbx);
    a.ret();

    asmjit::Error err = runtime_.add(&current_fn_, &code);
    if (err != asmjit::kErrorOk) {
        // Optionally log error; here we simply set to nullptr
        current_fn_ = nullptr;
    }

    if (current_fn_) {
        __builtin___clear_cache(
            reinterpret_cast<char*>(current_fn_), reinterpret_cast<char*>(current_fn_) + asmjit_code_size(code)
        );
    }

    return current_fn_;
}

void RobCodeGenerator::release_current() {
    if (current_fn_) {
        runtime_.release(current_fn_);
        current_fn_ = nullptr;
    }
}

RobCodeGenerator::RobCodeGenerator() { init_buffers(); }

RobCodeGenerator::~RobCodeGenerator() {
    release_current();
    if (dbuf1_)
        munmap(dbuf1_, dbuf_size_);
    if (dbuf2_orig_)
        munmap(dbuf2_orig_, dbuf_size_);
}

void RobCodeGenerator::init_buffers() {
    dbuf_size_  = kBufEntries * sizeof(void*);
    dbuf1_      = mmap(nullptr, dbuf_size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    dbuf2_orig_ = mmap(nullptr, dbuf_size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

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

void RobCodeGenerator::
    emit_filler(asmjit::x86::Assembler& a, int instr_type, size_t& seq_counter, int& global_idx, int /*idx_hint*/) {
    const size_t i       = seq_counter;
    const int reg_ids[4] = {3, 5, 6, 7};  // rbx, rbp, rsi, rdi
    asmjit::x86::Gp reg  = asmjit::x86::gpb(reg_ids[i & 3]);

    switch (instr_type) {
        case 0:  // add reg, reg
            a.add(reg, reg);
            break;
        case 1:  // nop
            a.nop();
            break;
        case 2:  // mov reg, reg
            a.mov(reg, reg);
            break;
        case 3:  // cmp reg, reg
            a.cmp(reg, reg);
            break;
        case 4:  // two-byte nop (66 90)
            a.emit(0x66);
            a.nop();
            break;
        case 5:  // xor reg, reg
            a.xor_(reg, reg);
            break;
        case 6:  // xor reg, reg+1
            a.xor_(reg, asmjit::x86::gpb(reg_ids[(i + 1) & 3]));
            break;
        case 7:  // mov reg, reg+1
            a.mov(reg, asmjit::x86::gpb(reg_ids[(i + 1) & 3]));
            break;
        case 8:  // movaps xmm, xmm
            a.movaps(asmjit::x86::xmm(i & 7), asmjit::x86::xmm((i + 1) & 7));
            break;
        case 9:  // movdqa xmm, xmm (SSE2)
        case 12:
            a.movdqa(asmjit::x86::xmm(i & 7), asmjit::x86::xmm((i + 1) & 7));
            break;
        case 10:  // xorps xmm, xmm
            a.xorps(asmjit::x86::xmm(i & 7), asmjit::x86::xmm(i & 7));
            break;
        case 11:  // xorps xmm, xmm+1
            a.xorps(asmjit::x86::xmm(i & 7), asmjit::x86::xmm((i + 1) & 7));
            break;
        case 13:  // vmovdqa xmm, xmm (AVX)
            a.vmovdqa(asmjit::x86::xmm(i & 7), asmjit::x86::xmm((i + 1) & 7));
            break;
        case 14:  // vmovdqa ymm, ymm (AVX)
            a.vmovdqa(asmjit::x86::ymm(i & 7), asmjit::x86::ymm((i + 1) & 7));
            break;
        case 15:  // movdqa xmm, xmm+1 (SSE2, low 4)
            a.movdqa(asmjit::x86::xmm(((i & 3) + 0)), asmjit::x86::xmm(((i + 1) & 3) + 0));
            break;
        case 16:  // vmovdqa xmm, xmm+1 (AVX, low 4)
            a.vmovdqa(asmjit::x86::xmm(((i & 3) + 0)), asmjit::x86::xmm(((i + 1) & 3) + 0));
            break;
        case 17:  // vmovdqa ymm, ymm+1 (AVX, low 4)
            a.vmovdqa(asmjit::x86::ymm(((i & 3) + 0)), asmjit::x86::ymm(((i + 1) & 3) + 0));
            break;
        case 18:  // vxorps ymm, ymm, ymm
            a.vxorps(asmjit::x86::ymm(i & 7), asmjit::x86::ymm(i & 7), asmjit::x86::ymm(i & 7));
            break;
        case 19:  // vxorps ymm, ymm, ymm+1
            a.vxorps(asmjit::x86::ymm(i & 7), asmjit::x86::ymm(i & 7), asmjit::x86::ymm((i + 1) & 7));
            break;
        case 20:  // conditional: xorps or add
            if (seq_counter & 1) {
                a.xorps(asmjit::x86::xmm(i & 7), asmjit::x86::xmm((i + 1) & 7));
            } else {
                if (sizeof(void*) == 4) {
                    a.add(asmjit::x86::gpb(reg_ids[i & 3]), asmjit::x86::gpb(reg_ids[i & 3]));
                } else {
                    a.add(asmjit::x86::rbx, asmjit::x86::rbx);
                }
            }
            break;
        case 21:  // conditional: vxorps or add
            if ((global_idx >> 2) & 1) {
                a.vxorps(asmjit::x86::ymm(i & 7), asmjit::x86::ymm(i & 7), asmjit::x86::ymm((i + 1) & 7));
            } else {
                a.add(asmjit::x86::rbx, asmjit::x86::rbx);
            }
            break;
        case 22:  // xor reg, reg+1 (same as case 6)
            a.xor_(reg, asmjit::x86::gpb(reg_ids[(i + 1) & 3]));
            break;
        case 23:  // sub reg, imm(i)
            a.sub(reg, asmjit::imm(i));
            break;
        case 24:  // add rbx, rbx
            a.add(asmjit::x86::rbx, asmjit::x86::rbx);
            break;
        case 25:  // mov rbx, rcx
            a.mov(asmjit::x86::rbx, asmjit::x86::rcx);
            break;
        default:
            a.nop();
            break;
    }
    ++seq_counter;
    ++global_idx;
}

}  // namespace x86_rob_detail

// ============================================================================
// ExecPorts detail definitions
// ============================================================================

namespace x86_exec_ports_detail {

ExecPortsCodeGenerator& ExecPortsCodeGenerator::instance() {
    static ExecPortsCodeGenerator gen;
    return gen;
}

void* ExecPortsCodeGenerator::generate(size_t instr_cnt, const std::vector<InstrType>& types) {
    if (types.empty() || instr_cnt == 0)
        return nullptr;

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
        for (auto t : types)
            fprintf(log_file_, "%d ", (int)t);
        fprintf(log_file_, ")\n;;; ========================================\n");
        fflush(log_file_);
        logger = std::make_unique<asmjit::FileLogger>(log_file_);
        asmjit_set_logger(code, logger.get());
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
        __builtin___clear_cache(reinterpret_cast<char*>(fn), reinterpret_cast<char*>(fn) + asmjit_code_size(code));
    }
    return fn;
}

void ExecPortsCodeGenerator::release_all() {
    for (void* fn : functions_) {
        if (fn)
            runtime_.release(fn);
    }
    functions_.clear();
}

void ExecPortsCodeGenerator::release_current() { release_all(); }

ExecPortsCodeGenerator::ExecPortsCodeGenerator() {
    init_buffers();
    log_file_ = fopen("exec_ports_code_dump.txt", "w");
    if (!log_file_) {
        SPDLOG_WARN("failed to open logging file");
    }
}

ExecPortsCodeGenerator::~ExecPortsCodeGenerator() {
    release_all();
    if (dbuf1_)
        munmap(dbuf1_, dbuf_size_);
    if (dbuf2_orig_)
        munmap(dbuf2_orig_, dbuf_size_);
    if (log_file_)
        fclose(log_file_);
}

void ExecPortsCodeGenerator::init_buffers() {
    dbuf_size_  = kBufEntries * sizeof(void*);
    dbuf1_      = mmap(nullptr, dbuf_size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    dbuf2_orig_ = mmap(nullptr, dbuf_size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

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

auto ExecPortsCodeGenerator::dst_reg(size_t idx) { return kAllRegs[idx % kNumRegs]; }

auto ExecPortsCodeGenerator::src_reg(size_t idx) { return kAllRegs[(idx + 1) % kNumRegs]; }

auto ExecPortsCodeGenerator::dst_xmm(size_t idx) { return asmjit::x86::xmm(idx % 16); }

void ExecPortsCodeGenerator::emit_nop(asmjit::x86::Assembler& a, size_t) { a.nop(); }
void ExecPortsCodeGenerator::emit_add_imm1(asmjit::x86::Assembler& a, size_t) { a.add(dst_reg(0), asmjit::imm(1)); }
void ExecPortsCodeGenerator::emit_sub_imm1(asmjit::x86::Assembler& a, size_t idx) {
    a.sub(dst_reg(idx), asmjit::imm(1));
}
void ExecPortsCodeGenerator::emit_mul_float(asmjit::x86::Assembler& a, size_t) {
    a.mulss(dst_xmm(1), asmjit::x86::xmm(15));
}
void ExecPortsCodeGenerator::emit_add_reg(asmjit::x86::Assembler& a, size_t) {
    a.add(asmjit::x86::rax, asmjit::x86::rax);
}
void ExecPortsCodeGenerator::emit_mov_imm(asmjit::x86::Assembler& a, size_t idx) {
    a.mov(dst_reg(idx), asmjit::imm(static_cast<int64_t>(idx)));
}
void ExecPortsCodeGenerator::emit_xor_zero(asmjit::x86::Assembler& a, size_t idx) {
    a.xor_(dst_reg(idx), dst_reg(idx));
}
void ExecPortsCodeGenerator::emit_inc(asmjit::x86::Assembler& a, size_t idx) { a.inc(dst_reg(idx)); }
void ExecPortsCodeGenerator::emit_dec(asmjit::x86::Assembler& a, size_t idx) { a.dec(dst_reg(idx)); }
void ExecPortsCodeGenerator::emit_sub_reg(asmjit::x86::Assembler& a, size_t idx) { a.sub(dst_reg(idx), src_reg(idx)); }
void ExecPortsCodeGenerator::emit_imul_reg(asmjit::x86::Assembler& a, size_t idx) {
    a.imul(dst_reg(idx), src_reg(idx));
}
void ExecPortsCodeGenerator::emit_and_reg(asmjit::x86::Assembler& a, size_t idx) { a.and_(dst_reg(idx), src_reg(idx)); }
void ExecPortsCodeGenerator::emit_or_reg(asmjit::x86::Assembler& a, size_t idx) { a.or_(dst_reg(idx), src_reg(idx)); }
void ExecPortsCodeGenerator::emit_shl_imm1(asmjit::x86::Assembler& a, size_t idx) {
    a.shl(dst_reg(idx), asmjit::imm(1));
}
void ExecPortsCodeGenerator::emit_shr_imm1(asmjit::x86::Assembler& a, size_t idx) {
    a.shr(dst_reg(idx), asmjit::imm(1));
}
void ExecPortsCodeGenerator::emit_not(asmjit::x86::Assembler& a, size_t idx) { a.not_(dst_reg(idx)); }
void ExecPortsCodeGenerator::emit_neg(asmjit::x86::Assembler& a, size_t idx) { a.neg(dst_reg(idx)); }
void ExecPortsCodeGenerator::emit_load_from_rcx(asmjit::x86::Assembler& a, size_t) {
    a.add(asmjit::x86::rbx, asmjit::x86::ptr(asmjit::x86::rcx));
}
void ExecPortsCodeGenerator::emit_store_to_rcx(asmjit::x86::Assembler& a, size_t idx) {
    a.mov(asmjit::x86::ptr(asmjit::x86::rcx), src_reg(idx));
}
void ExecPortsCodeGenerator::emit_load_from_rdx(asmjit::x86::Assembler& a, size_t) {
    a.add(asmjit::x86::rbx, asmjit::x86::ptr(asmjit::x86::rdx));
}
void ExecPortsCodeGenerator::emit_store_to_rdx(asmjit::x86::Assembler& a, size_t idx) {
    a.mov(asmjit::x86::ptr(asmjit::x86::rdx), src_reg(idx));
}

ExecPortsCodeGenerator::EmitterFunc ExecPortsCodeGenerator::get_emitter(InstrType type) {
    static constexpr std::array<EmitterFunc, 21> table{{
        emit_nop,            // NOP
        emit_add_imm1,       // ADD_IMM1
        emit_sub_imm1,       // SUB_IMM1
        emit_mul_float,      // MUL_FLOAT
        emit_add_reg,        // ADD_REG
        emit_mov_imm,        // MOV_IMM
        emit_xor_zero,       // XOR_ZERO
        emit_inc,            // INC
        emit_dec,            // DEC
        emit_sub_reg,        // SUB_REG
        emit_imul_reg,       // IMUL_REG
        emit_and_reg,        // AND_REG
        emit_or_reg,         // OR_REG
        emit_shl_imm1,       // SHL_IMM1
        emit_shr_imm1,       // SHR_IMM1
        emit_not,            // NOT
        emit_neg,            // NEG
        emit_load_from_rcx,  // LOAD_FROM_RCX
        emit_store_to_rcx,   // STORE_TO_RCX
        emit_load_from_rdx,  // LOAD_FROM_RDX
        emit_store_to_rdx    // STORE_TO_RDX
    }};
    return table.at(static_cast<size_t>(type));
}

}  // namespace x86_exec_ports_detail

// ============================================================================
// UopsCache detail definitions
// ============================================================================

namespace x86_uops_cache_detail {

UopsCacheCodeGenerator& UopsCacheCodeGenerator::instance() {
    static UopsCacheCodeGenerator gen;
    return gen;
}

void* UopsCacheCodeGenerator::generate(size_t instr_cnt, size_t iterations, const std::vector<InstrType>& types) {
    std::lock_guard<std::mutex> lock(mutex_);
    release_current_impl();

    if (types.empty() || instr_cnt == 0)
        return nullptr;

    asmjit::CodeHolder code;
    code.init(runtime_.environment());

    std::unique_ptr<asmjit::FileLogger> logger;
    if (log_file_) {
        ++gen_call_count_;
        fprintf(log_file_, "\n\n;;; ========================================\n");
        fprintf(
            log_file_, ";;; Generated function #%d (instr_cnt=%zu, iterations=%zu, types: ", gen_call_count_, instr_cnt,
            iterations
        );
        for (auto t : types)
            fprintf(log_file_, "%d ", static_cast<int>(t));
        fprintf(log_file_, ")\n;;; ========================================\n");
        fflush(log_file_);
        logger = std::make_unique<asmjit::FileLogger>(log_file_);
        asmjit_set_logger(code, logger.get());
    }

    asmjit::x86::Assembler a(&code);

    // Save non-volatile registers (per System V AMD64 ABI)
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
    asmjit::Label loop_start = asmjit_new_label(a);
    a.bind(loop_start);

    // Generate the sequence of instructions
    for (size_t i = 0; i < instr_cnt; ++i) {
        InstrType type = types[i % types.size()];
        emit_instruction(a, i, type);
    }

    a.dec(asmjit::x86::rcx);
    a.jnz(loop_start);

    // Restore registers
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
        // Clear instruction cache (required for JIT on some architectures)
        clear_cache(fn, asmjit_code_size(code));
    }
    return fn;
}

void UopsCacheCodeGenerator::release_current() {
    std::lock_guard<std::mutex> lock(mutex_);
    release_current_impl();
}

UopsCacheCodeGenerator::UopsCacheCodeGenerator() : log_file_(nullptr), gen_call_count_(0), current_function_(nullptr) {}

UopsCacheCodeGenerator::~UopsCacheCodeGenerator() {
    release_current_impl();
    if (log_file_)
        fclose(log_file_);
}

void UopsCacheCodeGenerator::enable_logging_impl(const char* filename) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (log_file_)
        fclose(log_file_);
    log_file_ = fopen(filename, "w");
    if (!log_file_) {
        SPDLOG_WARN("UopsCacheCodeGenerator: failed to open log file '{}'", filename);
    }
}

void UopsCacheCodeGenerator::disable_logging_impl() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (log_file_) {
        fclose(log_file_);
        log_file_ = nullptr;
    }
}

void UopsCacheCodeGenerator::release_current_impl() {
    if (current_function_) {
        runtime_.release(current_function_);
        current_function_ = nullptr;
    }
}

void UopsCacheCodeGenerator::clear_cache(void* addr, size_t size) {
#if defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache(reinterpret_cast<char*>(addr), reinterpret_cast<char*>(addr) + size);
#elif defined(_MSC_VER)
    FlushInstructionCache(GetCurrentProcess(), addr, size);
#else
    // Fallback: do nothing
#endif
}

void UopsCacheCodeGenerator::emit_instruction(asmjit::x86::Assembler& a, size_t idx, InstrType type) {
    // Choose destination register based on idx to break dependencies
    asmjit::x86::Gp dst = dst_reg(idx);

    switch (type) {
        case InstrType::NOP:
            a.nop();
            break;
        case InstrType::ADD_IMM1:
            a.add(dst, asmjit::imm(1));
            break;
        case InstrType::ADD_REG:
            // Use different source register to avoid self-dependency
            a.add(dst, dst_reg(idx + 1));
            break;
        default:
            // Unknown instruction type – emit nop
            a.nop();
            break;
    }
}

asmjit::x86::Gp UopsCacheCodeGenerator::dst_reg(size_t idx) {
    static constexpr asmjit::x86::Gp kAllRegs[] = {asmjit::x86::rax, asmjit::x86::rbx, asmjit::x86::rbp,
                                                   asmjit::x86::rsi, asmjit::x86::rdi, asmjit::x86::r8,
                                                   asmjit::x86::r9,  asmjit::x86::r10, asmjit::x86::r11,
                                                   asmjit::x86::r12, asmjit::x86::r13, asmjit::x86::r14,
                                                   asmjit::x86::r15};
    constexpr size_t kNumRegs                   = sizeof(kAllRegs) / sizeof(kAllRegs[0]);
    return kAllRegs[idx % kNumRegs];
}

}  // namespace x86_uops_cache_detail

// ============================================================================
// BranchTargetBuffer detail definitions
// ============================================================================

namespace x86_branch_target_buffer_detail {

BranchTargetBufferCodeGenerator& BranchTargetBufferCodeGenerator::instance() {
    static BranchTargetBufferCodeGenerator gen;
    return gen;
}

std::vector<void*> BranchTargetBufferCodeGenerator::generate(size_t blocks_cnt, size_t iterations, int alignment) {
    release_all();

    std::unique_ptr<asmjit::FileLogger> logger;
    if (log_file_) {
        ++gen_call_count_;
        fprintf(log_file_, "\n\n;;; ========================================\n");
        fprintf(
            log_file_, ";;; Generated function #%d (blocks_cnt=%zu, alignment=%d", gen_call_count_, blocks_cnt,
            alignment
        );
        fprintf(log_file_, ")\n;;; ========================================\n");
        fflush(log_file_);
        logger = std::make_unique<asmjit::FileLogger>(log_file_);
    }

    auto generate_body_func = [&](asmjit::x86::Assembler& a) {
        std::vector<asmjit::Label> labels(blocks_cnt);
        for (auto& label : labels) {
            label = asmjit_new_label(a);
            a.lea(asmjit::x86::r11, asmjit::x86::ptr(label));
            a.jmp(asmjit::x86::r11);
            a.align(asmjit::AlignMode::kCode, alignment);
            a.bind(label);
        }
    };

    auto generate_warmup = [&]() -> void* {
        asmjit::CodeHolder code;
        code.init(runtime_.environment());
        asmjit_set_logger(code, logger.get());
        asmjit::x86::Assembler a(&code);

        a.align(asmjit::AlignMode::kCode, alignment);
        generate_body_func(a);
        a.ret();
        void* fn = nullptr;
        if (runtime_.add(&fn, &code) == asmjit::kErrorOk) {
            warmup_function_ = fn;
            __builtin___clear_cache(reinterpret_cast<char*>(fn), reinterpret_cast<char*>(fn) + asmjit_code_size(code));
        }

        return fn;
    };

    auto generate_measure = [&]() -> void* {
        asmjit::CodeHolder code;
        code.init(runtime_.environment());
        asmjit_set_logger(code, logger.get());
        asmjit::x86::Assembler a(&code);

        a.mov(asmjit::x86::rcx, asmjit::imm(iterations));
        a.align(asmjit::AlignMode::kCode, alignment);
        asmjit::Label loop_start = asmjit_new_label(a);
        a.bind(loop_start);
        generate_body_func(a);
        a.dec(asmjit::x86::rcx);
        a.jnz(loop_start);
        a.ret();

        void* fn = nullptr;
        if (runtime_.add(&fn, &code) == asmjit::kErrorOk) {
            measure_function_ = fn;
            __builtin___clear_cache(reinterpret_cast<char*>(fn), reinterpret_cast<char*>(fn) + asmjit_code_size(code));
        }

        return fn;
    };

    return {generate_warmup(), generate_measure()};
}

void BranchTargetBufferCodeGenerator::release_measure_func() {
    if (measure_function_) {
        runtime_.release(measure_function_);
        measure_function_ = nullptr;
    }
}

void BranchTargetBufferCodeGenerator::release_warmup_func() {
    if (warmup_function_) {
        runtime_.release(warmup_function_);
        warmup_function_ = nullptr;
    }
}

void BranchTargetBufferCodeGenerator::release_all() {
    release_warmup_func();
    release_measure_func();
}

BranchTargetBufferCodeGenerator::BranchTargetBufferCodeGenerator()
    : log_file_(nullptr), gen_call_count_(0), warmup_function_(nullptr), measure_function_(nullptr) {
    log_file_ = fopen("branch_target_buffer_code_dump.txt", "w");
    if (!log_file_) {
        SPDLOG_WARN("failed to open logging file");
    }
}

BranchTargetBufferCodeGenerator::~BranchTargetBufferCodeGenerator() {
    release_all();
    if (log_file_)
        fclose(log_file_);
}

}  // namespace x86_branch_target_buffer_detail

}  // namespace silicon_probe::platform::arch
