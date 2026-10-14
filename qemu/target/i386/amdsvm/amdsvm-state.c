/*
 * Local experimental AMD-SVM <-> QEMU x86 architectural state mapping.
 * XSTATE bytes use QEMU's standard-format xsave helper and a bounded 4 KiB ABI
 * payload, matching the provider's wired guest XSAVE page.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "hw/core/cpu.h"
#include "hw/i386/apic.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/memory-internal.h"
#include "qemu/rcu.h"
#include "../cpu.h"
#include "../svm.h"
#include "emulate/x86_emu.h"
#include "emulate/x86_decode.h"
#include "amdsvm-state.h"
#include "amdsvm-msr.h"

#define AMDVM_EFER_SVME (1ull << 12)
#define AMDVM_XSTATE_BV_OFFSET 512u
#define AMDVM_XCOMP_BV_OFFSET 520u
#define AMDVM_XSTATE_RESERVED_OFFSET 528u
#define AMDVM_XSTATE_RESERVED_SIZE 48u
#define AMDSVM_NPF_ROM_MOVSB_BATCH_MAX 64u
#define AMDSVM_IOIO_STRING_BATCH_MAX 64u
#define AMDSVM_VGA_STOS_BATCH_MAX 64u
#define AMDSVM_VGA_APERTURE_START UINT64_C(0x000a0000)
#define AMDSVM_VGA_APERTURE_END   UINT64_C(0x000c0000)

static bool amdsvm_emulator_initialized;
static bool amdsvm_emulator_io_failed;

uint32_t amdsvm_state_apic_id(CPUState *cpu)
{
    return X86_CPU(cpu)->apic_id;
}

void amdsvm_state_alloc_emulator_buffer(CPUState *cs)
{
    if (!X86_CPU(cs)->env.emu_mmio_buf) {
        X86_CPU(cs)->env.emu_mmio_buf = g_new(char, 4096);
    }
}

void amdsvm_state_free_emulator_buffer(CPUState *cs)
{
    g_clear_pointer(&X86_CPU(cs)->env.emu_mmio_buf, g_free);
}

static X86Seg amdsvm_modrm_default_segment(const struct x86_decode *decode)
{
    if (decode->addressing_size == 2) {
        if (decode->modrm.rm == 2 || decode->modrm.rm == 3 ||
            (decode->modrm.rm == 6 && decode->modrm.mod != 0)) {
            return R_SS;
        }
    } else if (decode->addressing_size == 4) {
        if (decode->modrm.rm == 4 && decode->sib_present) {
            if ((decode->modrm.mod != 0 || decode->sib.base != 5) &&
                (decode->sib.base == R_ESP ||
                 decode->sib.base == R_EBP)) {
                return R_SS;
            }
        } else if ((decode->modrm.mod != 0 ||
                    decode->modrm.rm != R_EBP) &&
                   (decode->modrm.rm == R_ESP ||
                    decode->modrm.rm == R_EBP)) {
            return R_SS;
        }
    }
    return R_DS;
}

static unsigned amdsvm_operand_size(CPUX86State *env, bool override)
{
    bool code32 = (env->cr[0] & 1u) != 0 &&
                  (env->eflags & (1u << 17)) == 0 &&
                  (env->segs[R_CS].flags & DESC_B_MASK) != 0;
    unsigned size = code32 ? 4u : 2u;

    return override ? (size == 4u ? 2u : 4u) : size;
}

static enum x86_decode_cmd amdsvm_imm8_group_cmd(unsigned group)
{
    static const enum x86_decode_cmd commands[] = {
        X86_DECODE_CMD_ADD, X86_DECODE_CMD_OR, X86_DECODE_CMD_ADC,
        X86_DECODE_CMD_SBB, X86_DECODE_CMD_AND, X86_DECODE_CMD_SUB,
        X86_DECODE_CMD_XOR,
    };

    return group < ARRAY_SIZE(commands) ? commands[group] : X86_DECODE_CMD_INVL;
}

static void amdsvm_emulator_handle_io(CPUState *cs, uint16_t port,
                                      void *data, int direction,
                                      int size, int count)
{
    uint8_t *cursor = data;
    int i;

    (void)cs;

    for (i = 0; i < count; i++) {
        if (address_space_rw(&address_space_io, port,
                             MEMTXATTRS_UNSPECIFIED, cursor, size,
                             direction) != MEMTX_OK) {
            amdsvm_emulator_io_failed = true;
            return;
        }
        cursor += size;
    }
}

static void amdsvm_emulator_read_segment_descriptor(
    CPUState *cs, struct x86_segment_descriptor *desc, enum X86Seg seg)
{
    const SegmentCache *cache = &X86_CPU(cs)->env.segs[seg];
    uint32_t limit;

    memset(desc, 0, sizeof(*desc));
    desc->type = (cache->flags & DESC_TYPE_MASK) >> DESC_TYPE_SHIFT;
    desc->s = (cache->flags & DESC_S_MASK) >> DESC_S_SHIFT;
    desc->dpl = (cache->flags & DESC_DPL_MASK) >> DESC_DPL_SHIFT;
    desc->p = (cache->flags & DESC_P_MASK) >> DESC_P_SHIFT;
    desc->avl = (cache->flags & DESC_AVL_MASK) >> DESC_AVL_SHIFT;
    desc->l = (cache->flags & DESC_L_MASK) >> DESC_L_SHIFT;
    desc->db = (cache->flags & DESC_B_MASK) >> DESC_B_SHIFT;
    desc->g = (cache->flags & DESC_G_MASK) >> DESC_G_SHIFT;

    /* SegmentCache may hold limits that need granularity conversion. */
    if (!desc->g && cache->limit <= 0xfffff) {
        limit = cache->limit;
    } else {
        limit = cache->limit >> 12;
        desc->g = 1;
    }
    x86_set_segment_limit(desc, limit);
    x86_set_segment_base(desc, cache->base);
}

static bool amdsvm_emulator_is_user_mode(CPUState *cs)
{
    return (X86_CPU(cs)->env.hflags & HF_CPL_MASK) == 3;
}

static void amdsvm_msr_gp(CPUX86State *env)
{
    x86_emul_raise_exception(env, EXCP0D_GPF, 0);
}

static bool amdsvm_msr_read(CPUState *cs)
{
    CPUX86State *env = &X86_CPU(cs)->env;
    uint32_t msr = (uint32_t)env->regs[R_ECX];
    uint64_t value;
    AMDVMMTRRRegister mtrr;

    if (amdsvm_msr_decode_mtrr(msr, &mtrr)) {
        switch (mtrr.kind) {
        case AMDVM_MTRR_VARIABLE_BASE:
            value = env->mtrr_var[mtrr.index].base;
            break;
        case AMDVM_MTRR_VARIABLE_MASK:
            value = env->mtrr_var[mtrr.index].mask;
            break;
        case AMDVM_MTRR_FIXED_64K:
        case AMDVM_MTRR_FIXED_16K:
        case AMDVM_MTRR_FIXED_4K:
            value = env->mtrr_fixed[mtrr.index];
            break;
        case AMDVM_MTRR_DEFAULT_TYPE:
            value = env->mtrr_deftype;
            break;
        case AMDVM_MTRR_CAPABILITY:
            value = (env->features[FEAT_1_EDX] & CPUID_MTRR) ?
                MSR_MTRRcap_VCNT | MSR_MTRRcap_FIXRANGE_SUPPORT |
                    MSR_MTRRcap_WC_SUPPORTED : 0;
            break;
        default:
            g_assert_not_reached();
        }
        goto read_value;
    }
    if (amdsvm_msr_is_tcg_compat_amd(msr)) {
        value = 0;
        goto read_value;
    }

    switch (msr) {
    case MSR_IA32_SYSENTER_CS: value = env->sysenter_cs; break;
    case MSR_IA32_SYSENTER_ESP: value = env->sysenter_esp; break;
    case MSR_IA32_SYSENTER_EIP: value = env->sysenter_eip; break;
    case MSR_EFER: value = env->efer; break;
    case MSR_PAT: value = env->pat; break;
    case MSR_IA32_UCODE_REV: value = X86_CPU(cs)->ucode_rev; break;
    case MSR_STAR: value = env->star; break;
#ifdef TARGET_X86_64
    case MSR_LSTAR: value = env->lstar; break;
    case MSR_CSTAR: value = env->cstar; break;
    case MSR_FMASK: value = env->fmask; break;
    case MSR_FSBASE: value = env->segs[R_FS].base; break;
    case MSR_GSBASE: value = env->segs[R_GS].base; break;
    case MSR_KERNELGSBASE: value = env->kernelgsbase; break;
#endif
    case MSR_IA32_APICBASE:
        if (!X86_CPU(cs)->apic_state) {
            amdsvm_msr_gp(env);
            return true;
        }
        value = cpu_get_apic_base(X86_CPU(cs)->apic_state);
        break;
    default:
        amdsvm_msr_gp(env);
        return true;
    }

read_value:
    env->regs[R_EAX] = (uint32_t)value;
    env->regs[R_EDX] = (uint32_t)(value >> 32);
    return false;
}

static bool amdsvm_pat_value_valid(uint64_t value)
{
    unsigned i;

    for (i = 0; i < 8; i++) {
        switch ((value >> (i * 8)) & 0xff) {
        case 0: case 1: case 4: case 5: case 6: case 7:
            break;
        default:
            return false;
        }
    }
    return true;
}

static bool amdsvm_npf_targets_vga_mmio(uint64_t exit_info1,
                                        uint64_t gpa, unsigned width)
{
    return exit_info1 == ((UINT64_C(1) << 32) | 6u) && width != 0 &&
           gpa >= AMDSVM_VGA_APERTURE_START &&
           gpa < AMDSVM_VGA_APERTURE_END &&
           width <= AMDSVM_VGA_APERTURE_END - gpa;
}

static bool amdsvm_npf_targets_unassigned(uint64_t exit_info1,
                                          uint64_t gpa, unsigned width)
{
    MemoryRegion *mr;
    hwaddr xlat, len = width;

    if (exit_info1 != ((UINT64_C(1) << 32) | 6u) || width == 0) {
        return false;
    }

    RCU_READ_LOCK_GUARD();
    mr = address_space_translate(&address_space_memory, gpa, &xlat, &len,
                                 true, MEMTXATTRS_UNSPECIFIED);
    return mr && len >= width && mr->ops == &unassigned_mem_ops;
}

static bool amdsvm_npf_targets_ram(uint64_t gpa, unsigned width,
                                   bool is_write)
{
    MemoryRegion *mr;
    hwaddr xlat, len = width;

    if (width == 0) {
        return false;
    }

    RCU_READ_LOCK_GUARD();
    mr = address_space_translate(&address_space_memory, gpa, &xlat, &len,
                                 is_write, MEMTXATTRS_UNSPECIFIED);
    return mr && len >= width && memory_region_is_ram(mr) &&
           !memory_region_is_rom(mr);
}

static bool amdsvm_npf_targets_mmio(uint64_t exit_info1, uint64_t gpa,
                                    unsigned width, bool is_write)
{
    MemoryRegion *mr;
    hwaddr xlat, len = width;
    uint64_t expected_info1 = (UINT64_C(1) << 32) |
                              (is_write ? 6u : 4u);

    if (exit_info1 != expected_info1 || width == 0) {
        return false;
    }

    RCU_READ_LOCK_GUARD();
    mr = address_space_translate(&address_space_memory, gpa, &xlat, &len,
                                 is_write, MEMTXATTRS_UNSPECIFIED);
    return mr && len >= width && mr->ops &&
           !memory_region_is_ram(mr) && !memory_region_is_rom(mr) &&
           mr->ops != &unassigned_mem_ops;
}

static bool amdsvm_npf_targets_vga_rom_read(uint64_t exit_info1,
                                            uint64_t gpa, unsigned width)
{
    MemoryRegion *mr;
    hwaddr xlat, len = width;

    if (exit_info1 != ((UINT64_C(1) << 32) | 4u) || width == 0) {
        return false;
    }

    RCU_READ_LOCK_GUARD();
    mr = address_space_translate(&address_space_memory, gpa, &xlat, &len,
                                 false, MEMTXATTRS_UNSPECIFIED);
    return mr && len >= width && memory_region_is_rom(mr) &&
           strcmp(memory_region_name(mr), "vga.rom") == 0;
}

static int amdsvm_emulate_mmio_memory_write(CPUX86State *env,
                                            struct x86_decode *decode,
                                            Error **errp)
{
    target_ulong original_rip = env->eip;

    exec_instruction(env, decode);
    if (env->eip != original_rip + decode->len) {
        error_setg(errp, "AMD-SVM MMIO write emulation did not complete");
        return -EIO;
    }
    return 0;
}

static bool amdsvm_msr_write(CPUState *cs)
{
    CPUX86State *env = &X86_CPU(cs)->env;
    uint32_t msr = (uint32_t)env->regs[R_ECX];
    uint64_t value = (uint32_t)env->regs[R_EAX] |
                     ((uint64_t)(uint32_t)env->regs[R_EDX] << 32);
    uint64_t update_mask = 0;
    AMDVMMTRRRegister mtrr;

    if (amdsvm_msr_decode_mtrr(msr, &mtrr)) {
        switch (mtrr.kind) {
        case AMDVM_MTRR_VARIABLE_BASE:
            if (!amdsvm_mtrr_base_value_valid(value, X86_CPU(cs)->phys_bits) ||
                !amdsvm_mtrr_base_matches_mask(value,
                    env->mtrr_var[mtrr.index].mask,
                    X86_CPU(cs)->phys_bits)) {
                amdsvm_msr_gp(env);
                return true;
            }
            env->mtrr_var[mtrr.index].base = value;
            return false;
        case AMDVM_MTRR_VARIABLE_MASK:
            if (!amdsvm_mtrr_mask_value_valid(value, X86_CPU(cs)->phys_bits) ||
                !amdsvm_mtrr_base_matches_mask(
                    env->mtrr_var[mtrr.index].base, value,
                    X86_CPU(cs)->phys_bits)) {
                amdsvm_msr_gp(env);
                return true;
            }
            env->mtrr_var[mtrr.index].mask = value;
            return false;
        case AMDVM_MTRR_FIXED_64K:
        case AMDVM_MTRR_FIXED_16K:
        case AMDVM_MTRR_FIXED_4K:
            if (!amdsvm_mtrr_fixed_value_valid(value)) {
                amdsvm_msr_gp(env);
                return true;
            }
            env->mtrr_fixed[mtrr.index] = value;
            return false;
        case AMDVM_MTRR_DEFAULT_TYPE:
            if (!amdsvm_mtrr_default_value_valid(value)) {
                amdsvm_msr_gp(env);
                return true;
            }
            env->mtrr_deftype = value;
            return false;
        case AMDVM_MTRR_CAPABILITY:
            break;
        default:
            g_assert_not_reached();
        }
    }
    if (amdsvm_msr_is_tcg_compat_amd(msr)) {
        return false;
    }

    switch (msr) {
    case MSR_IA32_SYSENTER_CS: env->sysenter_cs = value & 0xffff; break;
    case MSR_IA32_SYSENTER_ESP: env->sysenter_esp = value; break;
    case MSR_IA32_SYSENTER_EIP: env->sysenter_eip = value; break;
    case MSR_STAR: env->star = value; break;
    case MSR_PAT:
        if (!amdsvm_pat_value_valid(value)) {
            amdsvm_msr_gp(env);
            return true;
        }
        env->pat = value;
        break;
    case MSR_EFER:
        if (env->features[FEAT_8000_0001_EDX] & CPUID_EXT2_SYSCALL) {
            update_mask |= MSR_EFER_SCE;
        }
        if (env->features[FEAT_8000_0001_EDX] & CPUID_EXT2_LM) {
            update_mask |= MSR_EFER_LME;
        }
        if (env->features[FEAT_8000_0001_EDX] & CPUID_EXT2_FFXSR) {
            update_mask |= MSR_EFER_FFXSR;
        }
        if (env->features[FEAT_8000_0001_EDX] & CPUID_EXT2_NX) {
            update_mask |= MSR_EFER_NXE;
        }
        /* SVME is host-owned and must never be enabled by guest MSR writes. */
        cpu_load_efer(env, (env->efer & ~update_mask) |
                           (value & update_mask));
        break;
#ifdef TARGET_X86_64
    case MSR_LSTAR: env->lstar = value; break;
    case MSR_CSTAR: env->cstar = value; break;
    case MSR_FMASK: env->fmask = value; break;
    case MSR_FSBASE: env->segs[R_FS].base = value; break;
    case MSR_GSBASE: env->segs[R_GS].base = value; break;
    case MSR_KERNELGSBASE: env->kernelgsbase = value; break;
#endif
    case MSR_IA32_APICBASE:
        if (!X86_CPU(cs)->apic_state ||
            (value & MSR_IA32_APICBASE_RESERVED)) {
            amdsvm_msr_gp(env);
            return true;
        }
        if (cpu_set_apic_base(X86_CPU(cs)->apic_state, value) < 0) {
            amdsvm_msr_gp(env);
            return true;
        }
        break;
    default:
        amdsvm_msr_gp(env);
        return true;
    }
    return false;
}

static const struct x86_emul_ops amdsvm_emulator_ops = {
    .handle_io = amdsvm_emulator_handle_io,
    .read_segment_descriptor = amdsvm_emulator_read_segment_descriptor,
    .simulate_rdmsr = amdsvm_msr_read,
    .simulate_wrmsr = amdsvm_msr_write,
    .is_user_mode = amdsvm_emulator_is_user_mode,
};

void amdsvm_state_init_emulator(void)
{
    if (!amdsvm_emulator_initialized) {
        init_emu(&amdsvm_emulator_ops);
        init_decoder();
        amdsvm_emulator_initialized = true;
    }
}

static bool amdsvm_io_address_size_matches(uint64_t exit_info1,
                                           int address_size)
{
    uint64_t encoded = (exit_info1 >> 7) & 7u;
    uint64_t expected = address_size == 2 ? 1u :
                        address_size == 4 ? 2u :
                        address_size == 8 ? 4u : 0u;

    return expected != 0 && encoded == expected;
}

static bool amdsvm_io_segment_matches(uint64_t exit_info1,
                                      const struct x86_decode *decode)
{
    unsigned encoded = (unsigned)((exit_info1 >> 10) & 7u);
    unsigned expected = 3u; /* DS is the default source for OUTS. */

    if (decode->cmd == X86_DECODE_CMD_INS) {
        expected = 0u; /* INS always writes to ES. */
    } else {
        switch (decode->segment_override) {
        case PREFIX_CS_SEG_OVERRIDE: expected = 1u; break;
        case PREFIX_SS_SEG_OVERRIDE: expected = 2u; break;
        case PREFIX_DS_SEG_OVERRIDE: expected = 3u; break;
        case PREFIX_ES_SEG_OVERRIDE: expected = 0u; break;
        case PREFIX_FS_SEG_OVERRIDE: expected = 4u; break;
        case PREFIX_GS_SEG_OVERRIDE: expected = 5u; break;
        default: break;
        }
    }
    return encoded == expected;
}

int amdsvm_handle_ioio_string_exit(CPUState *cs, uint64_t exit_info1,
                                   uint64_t next_rip,
                                   amdvm_x86_guest_state_v1_t *updated_state,
                                   Error **errp)
{
    X86CPU *x86_cpu = X86_CPU(cs);
    CPUX86State *env = &x86_cpu->env;
    struct x86_decode decode = { 0 };
    x86_insn_stream stream = { 0 };
    uint8_t instruction[15];
    target_ulong linear_ip;
    target_ulong original_rip = env->eip;
    uint64_t original_count = 0;
    uint64_t batch_count = 0;
    unsigned i;
    unsigned port;
    unsigned size_bits;
    int expected_size;
    int expected_cmd;
    bool is_input;
    bool has_rep;

    if (!updated_state || !amdsvm_emulator_initialized ||
        (exit_info1 & UINT64_C(0xffffffff0000e002)) != 0 ||
        !(exit_info1 & (1u << 2))) {
        error_setg(errp, "AMD-SVM string IOIO exit has invalid/reserved information");
        return -EINVAL;
    }
    port = (unsigned)((exit_info1 >> 16) & 0xffffu);
    is_input = (exit_info1 & 1u) != 0;
    has_rep = (exit_info1 & (1u << 3)) != 0;
    size_bits = (unsigned)((exit_info1 >> 4) & 7u);
    expected_size = size_bits == 1u ? 1 : size_bits == 2u ? 2 :
                    size_bits == 4u ? 4 : 0;
    expected_cmd = is_input ? X86_DECODE_CMD_INS : X86_DECODE_CMD_OUTS;
    if (!expected_size) {
        error_setg(errp, "AMD-SVM string IOIO has invalid operand-size encoding");
        return -EINVAL;
    }

    if (!(env->hflags & HF_CS64_MASK)) {
        if (env->segs[R_CS].base > UINT64_MAX - env->eip) {
            error_setg(errp, "AMD-SVM string IOIO instruction address overflowed");
            return -EINVAL;
        }
        linear_ip = env->segs[R_CS].base + env->eip;
    } else {
        linear_ip = env->eip;
    }
    for (i = 0; i < sizeof(instruction); i++) {
        if (linear_ip > UINT64_MAX - i ||
            cpu_memory_rw_debug(cs, linear_ip + i, &instruction[i], 1, 0) != 0) {
            break;
        }
    }
    if (i == 0) {
        error_setg(errp, "AMD-SVM could not read the guest string-I/O instruction");
        return -EFAULT;
    }
    stream.bytes = instruction;
    stream.len = i;
    if (decode_instruction_stream(env, &decode, &stream) == 0 ||
        decode.len <= 0 || decode.len > sizeof(instruction) ||
        decode.len > 15 || decode.cmd != expected_cmd ||
        decode.operand_size != expected_size ||
        !amdsvm_io_address_size_matches(exit_info1, decode.addressing_size) ||
        !amdsvm_io_segment_matches(exit_info1, &decode) ||
        (decode.rep != 0) != has_rep ||
        (env->regs[R_EDX] & 0xffffu) != port ||
        !amdsvm_msr_next_rip_valid(env->eip, decode.len,
            (env->hflags & HF_CS64_MASK) != 0,
            (env->hflags & HF_CS32_MASK) != 0, next_rip)) {
        error_setg(errp, "AMD-SVM string IOIO metadata does not match decoded guest instruction");
        return -EINVAL;
    }

    if (has_rep) {
        original_count = read_reg(env, R_ECX, decode.addressing_size);
        if (original_count != 0) {
            /*
             * Batch a small bounded number of string-I/O elements per
             * VMEXIT. SeaBIOS uses REP INSD for CD-ROM PIO; one element per
             * entry makes boot throughput unusable. Keep a finite batch so
             * QMP stop/kick still gets frequent VMEXIT opportunities.
             */
            batch_count = MIN(original_count,
                              (uint64_t)AMDSVM_IOIO_STRING_BATCH_MAX);
            write_reg(env, R_ECX, batch_count, decode.addressing_size);
        }
    }
    amdsvm_emulator_io_failed = false;
    exec_instruction(env, &decode);
    if (amdsvm_emulator_io_failed || env->eip != next_rip) {
        error_setg(errp, "AMD-SVM string IOIO emulation failed during memory or port access");
        return -EIO;
    }
    if (has_rep && original_count > batch_count) {
        write_reg(env, R_ECX, original_count - batch_count,
                  decode.addressing_size);
        env->eip = original_rip;
    } else if (env->eip != next_rip) {
        error_setg(errp, "AMD-SVM string IOIO emulator produced an unexpected RIP");
        return -EINVAL;
    }
    return amdsvm_state_load(cs, updated_state);
}

static int amdsvm_sync_shadow_write(uint64_t gpa, uint64_t shadow_hva,
                                    uint64_t shadow_bytes, size_t length,
                                    const uint8_t *write_data, Error **errp)
{
    uint8_t current[AMDSVM_NPF_ROM_MOVSB_BATCH_MAX];
    MemTxResult tx;

    if (shadow_hva == 0 || shadow_bytes < length || length == 0 ||
        length > sizeof(current) || (!write_data && length > 8) ||
        shadow_hva > UINT64_MAX - length) {
        error_setg(errp, "AMD-SVM firmware shadow write has invalid backing");
        return -EINVAL;
    }
    if (write_data) {
        memcpy((void *)(uintptr_t)shadow_hva, write_data, length);
        return 0;
    }
    tx = address_space_read(&address_space_memory, gpa,
                            MEMTXATTRS_UNSPECIFIED, current, length);
    if (tx != MEMTX_OK) {
        error_setg(errp, "AMD-SVM firmware shadow write could not read the live RAM mapping");
        return -EIO;
    }
    memcpy((void *)(uintptr_t)shadow_hva, current, length);
    return 0;
}

int amdsvm_handle_npf_write_exit(
    CPUState *cs, uint64_t exit_info1, uint64_t fault_gpa,
    const uint8_t instruction_bytes[15], uint8_t instruction_length,
    uint64_t immutable_rom_bytes, uint64_t shadow_backing_hva,
    uint64_t shadow_backing_bytes, Error **errp)
{
    CPUX86State *env;
    struct x86_decode decode = { 0 };
    x86_insn_stream stream = { 0 };
    uint8_t values[AMDSVM_NPF_ROM_MOVSB_BATCH_MAX];
    uint64_t src_gpa, dst_gpa, src_linear, dst_linear;
    target_ulong si, di, count, batch;
    MemTxResult tx;
    bool is_rom_movsb, is_rom_mov_imm, is_rom_mov_reg;
    bool is_rom_mov_reg_es, is_rom_mov_reg_ao;
    bool is_rom_or_imm8, is_rom_or_reg;
    bool is_rom_mov_moffs, is_rom_mov_moffs_es, is_rom_pop_rm;
    bool is_rom_mov_seg_ao, is_rom_call_reg, is_rom_call_rel;
    bool is_rom_push_reg;
    bool is_vga_rom_cmp;
    bool is_vga_string_stos;
    bool is_mmio_moffs_load, is_mmio_moffs_store;
    bool is_mmio_mov_reg_load, is_mmio_movzx_reg_load;
    bool is_mmio_mov_reg_store;
    bool is_vga_string_movs;
    bool is_rom_arith_mem;
    bool unassigned_npf;
    bool mmio_read_npf;
    bool mmio_write_npf;
    unsigned mov_imm_prefix, mov_imm_size, mov_reg_prefix;
    bool mov_imm_has_op_prefix, mov_imm_has_addr_prefix;
    unsigned mov_reg_opcode_offset;
    unsigned mov_reg_ao_opcode_offset;
    bool mov_reg_ao_has_op_prefix, mov_reg_ao_has_addr_prefix;
    unsigned or_prefix, moffs_prefix;
    bool or_has_op_prefix, or_has_addr_prefix;
    unsigned arith_opcode_offset;
    bool arith_has_op_prefix, arith_has_addr_prefix;
    unsigned es_moffs_opcode_offset, es_moffs_operand_prefix;
    unsigned mmio_reg_load_opcode_offset;
    unsigned mmio_movzx_opcode_offset;
    unsigned pop_opcode_offset;
    bool pop_has_addr_prefix, pop_has_op_prefix;
    unsigned push_opcode_offset;
    int rc = -EINVAL;

    if (!cs || !instruction_bytes || instruction_length < 2 ||
        instruction_length > 15) {
        error_setg(errp, "AMD-SVM NPF is missing guest state or instruction bytes");
        return -EINVAL;
    }
    env = &X86_CPU(cs)->env;
    mmio_read_npf = exit_info1 == ((UINT64_C(1) << 32) | 4u);
    mmio_write_npf = exit_info1 == ((UINT64_C(1) << 32) | 6u);
    if ((shadow_backing_hva == 0) != (shadow_backing_bytes == 0) ||
        (shadow_backing_hva != 0 &&
         (immutable_rom_bytes == 0 ||
          shadow_backing_hva > UINT64_MAX - shadow_backing_bytes))) {
        error_setg(errp, "AMD-SVM firmware shadow backing is inconsistent");
        return -EINVAL;
    }

    /*
     * Firmware string writes are deliberately narrow. REP MOVSB may target
     * the committed BIOS shadow mapping or a validated VGA aperture byte;
     * VGA accesses are issued one byte at a time through QEMU's address space.
     * A paged guest may also fault on a device MMIO load/store, but only the
     * decoded, GPA-verified MOV forms below are allowed through that path.
     * Other NPFs remain fail-closed.
     */
    bool vga_npf = amdsvm_npf_targets_vga_mmio(exit_info1, fault_gpa, 1);
    unassigned_npf = amdsvm_npf_targets_unassigned(exit_info1, fault_gpa, 1);

    if ((exit_info1 != ((UINT64_C(1) << 32) | 7u) &&
         !vga_npf && !unassigned_npf && !mmio_read_npf &&
         !mmio_write_npf) ||
        ((env->cr[0] & (UINT64_C(1) << 31)) != 0 &&
         !mmio_read_npf && !mmio_write_npf) ||
        (env->eflags & (1u << 17)) != 0 ||
        !amdsvm_emulator_initialized) {
        error_setg(errp,
                   "AMD-SVM NPF is outside the supported firmware or validated paged-MMIO profile (info1=0x%016" PRIx64 " gpa=0x%016" PRIx64 " cr0=0x%016" PRIx64 " eflags=0x%016" PRIx64 " vm=%u emulator=%u bytes=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x)",
                   exit_info1, fault_gpa, env->cr[0],
                   (uint64_t)env->eflags,
                   (unsigned)((env->eflags >> 17) & 1u),
                   amdsvm_emulator_initialized,
                   instruction_bytes[0], instruction_bytes[1],
                   instruction_bytes[2], instruction_bytes[3],
                   instruction_bytes[4], instruction_bytes[5],
                   instruction_bytes[6], instruction_bytes[7]);
        return -ENOTSUP;
    }

    is_rom_movsb = instruction_bytes[0] == PREFIX_REP &&
                   instruction_bytes[1] == 0xa4;
    mov_imm_prefix = 0;
    mov_imm_has_op_prefix = false;
    mov_imm_has_addr_prefix = false;
    while (mov_imm_prefix < instruction_length &&
           (instruction_bytes[mov_imm_prefix] == 0x66 ||
            instruction_bytes[mov_imm_prefix] == 0x67)) {
        if (instruction_bytes[mov_imm_prefix] == 0x66) {
            if (mov_imm_has_op_prefix) {
                break;
            }
            mov_imm_has_op_prefix = true;
        } else {
            if (mov_imm_has_addr_prefix) {
                break;
            }
            mov_imm_has_addr_prefix = true;
        }
        mov_imm_prefix++;
    }
    is_rom_mov_imm = instruction_length >= mov_imm_prefix + 2u &&
                     (instruction_bytes[mov_imm_prefix] == 0xc6 ||
                      instruction_bytes[mov_imm_prefix] == 0xc7) &&
                     (instruction_bytes[mov_imm_prefix + 1u] & 0x38u) == 0 &&
                     (instruction_bytes[mov_imm_prefix + 1u] >> 6) != 3;
    if (instruction_length > mov_imm_prefix &&
        instruction_bytes[mov_imm_prefix] == 0xc6) {
        mov_imm_size = 1u;
    } else {
        mov_imm_size = amdsvm_operand_size(env, mov_imm_has_op_prefix);
    }
    mov_reg_prefix = instruction_bytes[0] == 0x66 ? 1u : 0u;
    mov_reg_opcode_offset = mov_reg_prefix;
    is_rom_mov_reg = instruction_length >= mov_reg_prefix + 2u &&
                     (instruction_bytes[mov_reg_prefix] == 0x88 ||
                      instruction_bytes[mov_reg_prefix] == 0x89);
    mov_reg_opcode_offset = 1u;
    if (instruction_length >= 2u &&
        instruction_bytes[0] == PREFIX_ES_SEG_OVERRIDE &&
        instruction_bytes[1] == 0x66) {
        mov_reg_opcode_offset++;
    }
    is_rom_mov_reg_es = instruction_length > mov_reg_opcode_offset &&
                        instruction_bytes[0] == PREFIX_ES_SEG_OVERRIDE &&
                        (instruction_bytes[mov_reg_opcode_offset] == 0x88 ||
                         instruction_bytes[mov_reg_opcode_offset] == 0x89);
    is_rom_mov_reg |= is_rom_mov_reg_es;
    mov_reg_ao_opcode_offset = 0;
    mov_reg_ao_has_op_prefix = false;
    mov_reg_ao_has_addr_prefix = false;
    while (mov_reg_ao_opcode_offset < instruction_length &&
           (instruction_bytes[mov_reg_ao_opcode_offset] == 0x66 ||
            instruction_bytes[mov_reg_ao_opcode_offset] == 0x67)) {
        if (instruction_bytes[mov_reg_ao_opcode_offset] == 0x66) {
            if (mov_reg_ao_has_op_prefix) {
                break;
            }
            mov_reg_ao_has_op_prefix = true;
        } else {
            if (mov_reg_ao_has_addr_prefix) {
                break;
            }
            mov_reg_ao_has_addr_prefix = true;
        }
        mov_reg_ao_opcode_offset++;
    }
    is_rom_mov_reg_ao = mov_reg_ao_has_addr_prefix &&
                        instruction_length >= mov_reg_ao_opcode_offset + 2u &&
                        (instruction_bytes[mov_reg_ao_opcode_offset] == 0x88 ||
                         instruction_bytes[mov_reg_ao_opcode_offset] == 0x89);
    is_rom_mov_reg |= is_rom_mov_reg_ao;
    if (amdsvm_npf_targets_mmio(exit_info1, fault_gpa, 1, true)) {
        /* A mapped device MOV belongs to the MMIO path, not ROM shadowing. */
        is_rom_mov_reg = false;
    }
    or_prefix = 0;
    or_has_op_prefix = false;
    or_has_addr_prefix = false;
    while (or_prefix < instruction_length &&
           (instruction_bytes[or_prefix] == 0x66 ||
            instruction_bytes[or_prefix] == 0x67)) {
        if (instruction_bytes[or_prefix] == 0x66) {
            if (or_has_op_prefix) {
                break;
            }
            or_has_op_prefix = true;
        } else {
            if (or_has_addr_prefix) {
                break;
            }
            or_has_addr_prefix = true;
        }
        or_prefix++;
    }
    is_rom_or_imm8 = instruction_length >= or_prefix + 2u &&
                     instruction_bytes[or_prefix] == 0x83 &&
                     ((instruction_bytes[or_prefix + 1u] >> 3) & 7u) <= 6u &&
                     (instruction_bytes[or_prefix + 1u] >> 6) != 3;
    is_rom_or_reg = instruction_length >= 1u &&
                    (instruction_bytes[0] == 0x08 ||
                     instruction_bytes[0] == 0x09);
    moffs_prefix = instruction_bytes[0] == 0x66 ? 1u : 0u;
    is_rom_mov_moffs = instruction_length >= moffs_prefix + 1u &&
                       (instruction_bytes[moffs_prefix] == 0xa2 ||
                        instruction_bytes[moffs_prefix] == 0xa3);
    es_moffs_opcode_offset = 1u;
    es_moffs_operand_prefix = 0u;
    if (instruction_length >= 2u &&
        instruction_bytes[0] == PREFIX_ES_SEG_OVERRIDE &&
        instruction_bytes[1] == 0x66) {
        es_moffs_opcode_offset++;
        es_moffs_operand_prefix = 1u;
    }
    is_rom_mov_moffs_es = instruction_length > es_moffs_opcode_offset &&
                          instruction_bytes[0] == PREFIX_ES_SEG_OVERRIDE &&
                          (instruction_bytes[es_moffs_opcode_offset] == 0xa2 ||
                           instruction_bytes[es_moffs_opcode_offset] == 0xa3);
    is_rom_mov_moffs |= is_rom_mov_moffs_es;
    pop_opcode_offset = 0;
    pop_has_addr_prefix = false;
    pop_has_op_prefix = false;
    while (pop_opcode_offset < instruction_length &&
           (instruction_bytes[pop_opcode_offset] == 0x67 ||
            instruction_bytes[pop_opcode_offset] == 0x66)) {
        if (instruction_bytes[pop_opcode_offset] == 0x67) {
            if (pop_has_addr_prefix) {
                break;
            }
            pop_has_addr_prefix = true;
        } else {
            if (pop_has_op_prefix) {
                break;
            }
            pop_has_op_prefix = true;
        }
        pop_opcode_offset++;
    }
    is_rom_pop_rm = pop_has_addr_prefix &&
                    instruction_length >= pop_opcode_offset + 2u &&
                    instruction_bytes[pop_opcode_offset] == 0x8f &&
                    (instruction_bytes[pop_opcode_offset + 1u] & 0x38u) == 0 &&
                    (instruction_bytes[pop_opcode_offset + 1u] >> 6) != 3;
    is_rom_mov_seg_ao = instruction_length >= 3u &&
                        instruction_bytes[0] == 0x67 &&
                        instruction_bytes[1] == 0x8c &&
                        (instruction_bytes[2] >> 6) != 3;
    unsigned call_opcode_offset = instruction_bytes[0] == 0x66 ? 1u : 0u;
    is_rom_call_reg = instruction_length >= call_opcode_offset + 2u &&
                      instruction_bytes[call_opcode_offset] == 0xff &&
                      ((instruction_bytes[call_opcode_offset + 1u] >> 3) & 7u) == 2u &&
                      (instruction_bytes[call_opcode_offset + 1u] >> 6) == 3;
    is_rom_call_rel = instruction_length >= call_opcode_offset + 2u &&
                      instruction_bytes[call_opcode_offset] == 0xe8;
    push_opcode_offset = instruction_bytes[0] == 0x66 ? 1u : 0u;
    is_rom_push_reg = instruction_length > push_opcode_offset &&
                      instruction_bytes[push_opcode_offset] >= 0x50 &&
                      instruction_bytes[push_opcode_offset] <= 0x57;
    is_mmio_moffs_load = mmio_read_npf && instruction_length >= 1u &&
                         (instruction_bytes[0] == 0xa0 ||
                          instruction_bytes[0] == 0xa1);
    is_mmio_moffs_store = mmio_write_npf && instruction_length >= 1u &&
                          (instruction_bytes[0] == 0xa2 ||
                           instruction_bytes[0] == 0xa3);
    mmio_reg_load_opcode_offset = 0;
    while (mmio_reg_load_opcode_offset < instruction_length &&
           (instruction_bytes[mmio_reg_load_opcode_offset] == 0x66 ||
            instruction_bytes[mmio_reg_load_opcode_offset] == 0x67 ||
            ((env->hflags & HF_CS64_MASK) &&
             instruction_bytes[mmio_reg_load_opcode_offset] >= 0x40 &&
             instruction_bytes[mmio_reg_load_opcode_offset] <= 0x4f))) {
        mmio_reg_load_opcode_offset++;
    }
    is_mmio_mov_reg_load = mmio_read_npf &&
        instruction_length >= mmio_reg_load_opcode_offset + 2u &&
        (instruction_bytes[mmio_reg_load_opcode_offset] == 0x8a ||
         instruction_bytes[mmio_reg_load_opcode_offset] == 0x8b);
    mmio_movzx_opcode_offset = 0;
    while (mmio_movzx_opcode_offset < instruction_length &&
           (instruction_bytes[mmio_movzx_opcode_offset] == 0x66 ||
            instruction_bytes[mmio_movzx_opcode_offset] == 0x67 ||
            ((env->hflags & HF_CS64_MASK) &&
             instruction_bytes[mmio_movzx_opcode_offset] >= 0x40 &&
             instruction_bytes[mmio_movzx_opcode_offset] <= 0x4f))) {
        mmio_movzx_opcode_offset++;
    }
    is_mmio_movzx_reg_load = mmio_read_npf &&
        instruction_length >= mmio_movzx_opcode_offset + 3u &&
        instruction_bytes[mmio_movzx_opcode_offset] == 0x0f &&
        (instruction_bytes[mmio_movzx_opcode_offset + 1u] == 0xb6 ||
         instruction_bytes[mmio_movzx_opcode_offset + 1u] == 0xb7);
    mmio_reg_load_opcode_offset = 0;
    {
        bool has_op_prefix = false;
        bool has_addr_prefix = false;
        bool has_es_prefix = false;
        bool has_rex_prefix = false;

        while (mmio_reg_load_opcode_offset < instruction_length) {
            uint8_t prefix = instruction_bytes[mmio_reg_load_opcode_offset];

            if (prefix == 0x66 && !has_op_prefix) {
                has_op_prefix = true;
            } else if (prefix == 0x67 && !has_addr_prefix) {
                has_addr_prefix = true;
            } else if (prefix == PREFIX_ES_SEG_OVERRIDE && !has_es_prefix) {
                has_es_prefix = true;
            } else if ((env->hflags & HF_CS64_MASK) &&
                       prefix >= 0x40 && prefix <= 0x4f &&
                       !has_rex_prefix) {
                has_rex_prefix = true;
            } else {
                break;
            }
            mmio_reg_load_opcode_offset++;
        }
        is_mmio_mov_reg_store = mmio_write_npf &&
            instruction_length >= mmio_reg_load_opcode_offset + 2u &&
            (instruction_bytes[mmio_reg_load_opcode_offset] == 0x88 ||
             instruction_bytes[mmio_reg_load_opcode_offset] == 0x89);
    }
    is_vga_rom_cmp = mmio_read_npf && instruction_length >= 3u &&
                     instruction_bytes[0] == 0x66 &&
                     instruction_bytes[1] == 0x81 &&
                     (instruction_bytes[2] & 0x38u) == 0x38u &&
                     (instruction_bytes[2] >> 6) != 3;
    /* SeaBIOS VGA text clear: REP STOSB/W/D. */
    is_vga_string_stos = mmio_write_npf && instruction_length >= 2u &&
        ((instruction_bytes[0] == 0x66 && instruction_length >= 3u &&
          instruction_bytes[1] == PREFIX_REP &&
          instruction_bytes[2] == 0xab) ||
         (instruction_bytes[0] == PREFIX_REP &&
          instruction_bytes[1] == 0xab));
    is_vga_string_movs = (mmio_read_npf || mmio_write_npf) &&
        ((instruction_bytes[0] == PREFIX_REP &&
          instruction_length >= 2u &&
          (instruction_bytes[1] == 0xa4 || instruction_bytes[1] == 0xa5)) ||
         (instruction_bytes[0] == 0x66 && instruction_length >= 3u &&
          instruction_bytes[1] == PREFIX_REP &&
          (instruction_bytes[2] == 0xa4 || instruction_bytes[2] == 0xa5)));
    arith_opcode_offset = 0;
    arith_has_op_prefix = false;
    arith_has_addr_prefix = false;
    while (arith_opcode_offset < instruction_length &&
           (instruction_bytes[arith_opcode_offset] == 0x66 ||
            instruction_bytes[arith_opcode_offset] == 0x67)) {
        if (instruction_bytes[arith_opcode_offset] == 0x66) {
            if (arith_has_op_prefix) {
                break;
            }
            arith_has_op_prefix = true;
        } else {
            if (arith_has_addr_prefix) {
                break;
            }
            arith_has_addr_prefix = true;
        }
        arith_opcode_offset++;
    }
    is_rom_arith_mem = instruction_length >= arith_opcode_offset + 2u &&
                       (instruction_bytes[arith_opcode_offset] == 0x00 ||
                        instruction_bytes[arith_opcode_offset] == 0x01 ||
                        instruction_bytes[arith_opcode_offset] == 0x28 ||
                        instruction_bytes[arith_opcode_offset] == 0x29 ||
                        (instruction_bytes[arith_opcode_offset] == 0xfe &&
                         ((instruction_bytes[arith_opcode_offset + 1u] >> 3) & 7u) <= 1u));
    if ((is_rom_mov_imm &&
         instruction_length < mov_imm_prefix + 2u + mov_imm_size) ||
        (is_rom_mov_reg && instruction_length < mov_reg_prefix + 2u) ||
        (is_rom_or_imm8 && instruction_length < or_prefix + 3u) ||
        (is_rom_or_reg && instruction_length < 2u) ||
        ((is_rom_mov_moffs || is_mmio_moffs_load) &&
         instruction_length < moffs_prefix + 5u)) {
        error_setg(errp, "AMD-SVM ROM MOV instruction bytes are truncated");
        return -EINVAL;
    }
    stream.bytes = (uint8_t *)instruction_bytes;
    stream.len = instruction_length;
    if ((!is_mmio_moffs_load && !is_mmio_mov_reg_load &&
         !is_mmio_movzx_reg_load &&
         !is_mmio_mov_reg_store &&
         !is_vga_rom_cmp && !is_vga_string_stos && !is_vga_string_movs &&
         !is_rom_pop_rm &&
         !is_rom_mov_seg_ao && !is_rom_call_reg && !is_rom_call_rel &&
         !is_rom_push_reg &&
         !is_rom_movsb && !is_rom_mov_imm &&
         !is_rom_mov_reg && !is_rom_or_imm8 && !is_rom_or_reg &&
         !is_rom_mov_moffs &&
         !is_rom_arith_mem) ||
        decode_instruction_stream(env, &decode, &stream) == 0 ||
        (decode.addressing_size != 4 &&
         !(is_mmio_mov_reg_load && decode.addressing_size == 8) &&
         !(is_mmio_movzx_reg_load && decode.addressing_size == 8) &&
         !(is_mmio_mov_reg_store && decode.addressing_size == 8) &&
        !((is_vga_string_stos || is_vga_string_movs) &&
          (env->hflags & HF_CS64_MASK) && decode.addressing_size == 8) &&
         !((is_rom_mov_moffs_es || is_rom_mov_reg_es ||
            is_rom_mov_seg_ao || is_rom_call_reg || is_rom_call_rel ||
            is_mmio_mov_reg_load ||
            is_mmio_movzx_reg_load ||
            is_mmio_mov_reg_store ||
            is_vga_string_stos ||
            is_vga_string_movs ||
            is_rom_movsb ||
            is_rom_push_reg ||
            is_rom_arith_mem ||
            is_rom_or_imm8 ||
            (is_rom_mov_reg && !is_rom_mov_reg_ao &&
             !decode.addr_size_override) ||
            is_rom_mov_reg_ao || is_rom_pop_rm ||
            (is_rom_mov_imm && mov_imm_has_addr_prefix)) &&
           decode.addressing_size == 2)) ||
        (decode.segment_override != 0 &&
         !((is_rom_mov_moffs_es || is_rom_mov_reg_es) &&
           decode.segment_override == PREFIX_ES_SEG_OVERRIDE) &&
         !(is_mmio_mov_reg_store &&
           decode.segment_override == PREFIX_ES_SEG_OVERRIDE)) ||
        decode.lock || (decode.addr_size_override &&
                        !is_rom_pop_rm && !is_rom_mov_reg_ao &&
                        !is_mmio_mov_reg_load &&
                        !is_mmio_movzx_reg_load &&
                        !is_mmio_mov_reg_store &&
                        !is_rom_arith_mem &&
                        !is_rom_or_imm8 &&
                        !is_vga_string_stos &&
                        !is_vga_string_movs &&
                        !is_rom_movsb &&
                        !(is_rom_mov_imm && mov_imm_has_addr_prefix) &&
                        !is_rom_mov_seg_ao) ||
        (is_rom_movsb &&
         (decode.len != 2 || decode.cmd != X86_DECODE_CMD_MOVS ||
          decode.operand_size != 1 || decode.rep != PREFIX_REP ||
          decode.op_size_override ||
          (decode.addressing_size != 2 && decode.addressing_size != 4))) ||
        (is_rom_mov_imm &&
         (decode.cmd != X86_DECODE_CMD_MOV ||
          decode.modrm.reg != 0 || decode.modrm.mod == 3 ||
          decode.op[0].type != X86_VAR_RM ||
          decode.op[1].type != X86_VAR_IMMEDIATE ||
          decode.operand_size != (int)mov_imm_size || decode.rep != 0 ||
          (decode.op_size_override != 0) != mov_imm_has_op_prefix ||
          (mov_imm_has_addr_prefix &&
           ((decode.addressing_size != 2 && decode.addressing_size != 4) ||
            decode.addr_size_override != 0x67)))) ||
        (is_rom_mov_reg &&
         (decode.cmd != X86_DECODE_CMD_MOV || decode.rep != 0 ||
          decode.modrm.mod == 3 || decode.op[0].type != X86_VAR_RM ||
          (is_rom_mov_reg_es &&
           ((env->cr[0] & 1u) != 0 || decode.addressing_size != 2 ||
            decode.op_size_override !=
                (mov_reg_opcode_offset == 2u ? 0x66 : 0))) ||
          (is_rom_mov_reg_ao &&
           ((decode.addressing_size != 2 && decode.addressing_size != 4) ||
            decode.addr_size_override != 0x67 ||
            decode.op_size_override !=
                (mov_reg_ao_has_op_prefix ? 0x66 : 0) ||
            (instruction_bytes[mov_reg_ao_opcode_offset] == 0x88 ?
                 decode.operand_size != 1 :
                 (decode.operand_size != 2 &&
                  decode.operand_size != 4)))) ||
          decode.operand_size !=
              (instruction_bytes[is_rom_mov_reg_es ?
                   mov_reg_opcode_offset :
                   is_rom_mov_reg_ao ? mov_reg_ao_opcode_offset :
                   mov_reg_prefix] == 0x88 ? 1 :
               amdsvm_operand_size(env, decode.op_size_override != 0)) ||
          (!is_rom_mov_reg_es && !is_rom_mov_reg_ao &&
           (decode.op_size_override != 0) != (mov_reg_prefix != 0)))) ||
        (is_rom_or_imm8 &&
         (decode.len > instruction_length ||
          decode.cmd != amdsvm_imm8_group_cmd(decode.modrm.reg) ||
          decode.rep != 0 || decode.modrm.mod == 3 ||
          decode.op[0].type != X86_VAR_RM ||
          decode.op[1].type != X86_VAR_IMMEDIATE ||
          (decode.operand_size != 2 && decode.operand_size != 4) ||
          decode.operand_size !=
              amdsvm_operand_size(env, or_has_op_prefix) ||
          (decode.op_size_override != 0) != or_has_op_prefix ||
          (decode.addr_size_override != 0) != or_has_addr_prefix)) ||
        (is_rom_or_reg &&
         (decode.cmd != X86_DECODE_CMD_OR || decode.rep != 0 ||
          decode.modrm.mod == 3 || decode.op[0].type != X86_VAR_RM ||
          decode.op[1].type != X86_VAR_REG ||
          decode.operand_size != (instruction_bytes[0] == 0x08 ? 1 :
                                  (decode.op_size_override ? 2 : 4)))) ||
        (is_rom_mov_moffs &&
         (decode.len != (int)(moffs_prefix + 1u +
                              decode.addressing_size +
                              (is_rom_mov_moffs_es ?
                               1u + es_moffs_operand_prefix : 0u)) ||
          decode.cmd != X86_DECODE_CMD_MOV || decode.rep != 0 ||
          (is_rom_mov_moffs_es &&
           ((env->cr[0] & 1u) != 0 || decode.addressing_size != 2 ||
            decode.op_size_override !=
                (es_moffs_operand_prefix ? 0x66 : 0) ||
            decode.operand_size !=
                (instruction_bytes[es_moffs_opcode_offset] == 0xa2 ? 1 :
                 (es_moffs_operand_prefix ? 4 : 2)))) ||
          (decode.addr_size_override && !is_rom_mov_moffs_es) ||
          (!is_rom_mov_moffs_es &&
           decode.op_size_override != (moffs_prefix ? 0x66 : 0)) ||
          decode.op[0].type != X86_VAR_OFFSET ||
          (!is_rom_mov_moffs_es && decode.operand_size !=
              (instruction_bytes[moffs_prefix] == 0xa2 ? 1 :
               (moffs_prefix ? 2 : 4))))) ||
        (is_rom_pop_rm &&
         (decode.cmd != X86_DECODE_CMD_POP || decode.rep != 0 ||
          (decode.addressing_size != 2 && decode.addressing_size != 4) ||
          decode.addr_size_override != 0x67 ||
          decode.op_size_override != (pop_has_op_prefix ? 0x66 : 0) ||
          (decode.operand_size != 2 && decode.operand_size != 4) ||
          decode.modrm.reg != 0 || decode.modrm.mod == 3 ||
          decode.op[0].type != X86_VAR_RM)) ||
        (is_rom_mov_seg_ao &&
         (decode.cmd != X86_DECODE_CMD_MOV_FROM_SEG || decode.rep != 0 ||
          decode.lock || decode.addr_size_override != 0x67 ||
          (decode.addressing_size != 2 && decode.addressing_size != 4) ||
          decode.op_size_override != 0 || decode.operand_size != 2 ||
          decode.modrm.mod == 3 || decode.modrm.reg > 5 ||
          decode.op[0].type != X86_VAR_RM)) ||
        (is_rom_call_reg &&
         (decode.cmd != X86_DECODE_CMD_CALL_NEAR_ABS_INDIRECT ||
          decode.rep != 0 || decode.lock || decode.modrm.reg != 2 ||
          decode.modrm.mod != 3 ||
          (decode.operand_size != 2 && decode.operand_size != 4) ||
          (decode.op_size_override != 0) != (call_opcode_offset != 0))) ||
        (is_rom_call_rel &&
         (decode.cmd != X86_DECODE_CMD_CALL_NEAR || decode.rep != 0 ||
          decode.lock || decode.op[0].type != X86_VAR_IMMEDIATE ||
          (decode.operand_size != 2 && decode.operand_size != 4) ||
          (decode.op_size_override != 0) != (call_opcode_offset != 0))) ||
        (is_rom_push_reg &&
         (decode.cmd != X86_DECODE_CMD_PUSH || decode.rep != 0 ||
          decode.lock || decode.op[0].type != X86_VAR_REG ||
          (decode.operand_size != 2 && decode.operand_size != 4) ||
          (decode.op_size_override != 0) != (push_opcode_offset != 0))) ||
        (is_mmio_moffs_load &&
         (decode.len != 5 || decode.cmd != X86_DECODE_CMD_MOV ||
          decode.rep != 0 || decode.addr_size_override ||
          decode.op_size_override != 0 ||
          decode.op[1].type != X86_VAR_OFFSET ||
          decode.operand_size != (instruction_bytes[0] == 0xa0 ? 1 : 4))) ||
        (is_mmio_mov_reg_load &&
         (decode.len < mmio_reg_load_opcode_offset + 2u ||
          decode.len > instruction_length ||
          decode.cmd != X86_DECODE_CMD_MOV || decode.rep != 0 ||
          decode.lock || decode.modrm.mod == 3 ||
          decode.op[0].type != X86_VAR_REG ||
          decode.op[1].type != X86_VAR_RM ||
          (instruction_bytes[mmio_reg_load_opcode_offset] == 0x8a ?
               decode.operand_size != 1 :
               (decode.operand_size != 2 && decode.operand_size != 4 &&
                !((env->hflags & HF_CS64_MASK) &&
                  decode.operand_size == 8))) ||
          (decode.op_size_override != 0) !=
              (memchr(instruction_bytes, 0x66,
                      mmio_reg_load_opcode_offset) != NULL) ||
          (decode.addr_size_override != 0) !=
              (memchr(instruction_bytes, 0x67,
                      mmio_reg_load_opcode_offset) != NULL))) ||
        (is_mmio_movzx_reg_load &&
         (decode.len < mmio_movzx_opcode_offset + 3u ||
          decode.len > instruction_length ||
          decode.cmd != X86_DECODE_CMD_MOVZX || decode.rep != 0 ||
          decode.lock || decode.modrm.mod == 3 ||
          decode.op[0].type != X86_VAR_REG ||
          decode.op[1].type != X86_VAR_RM ||
          (decode.operand_size != 2 && decode.operand_size != 4 &&
           decode.operand_size != 8) ||
          (instruction_bytes[mmio_movzx_opcode_offset + 1u] == 0xb6 &&
           decode.operand_size == 1) ||
          (decode.op_size_override != 0) !=
              (memchr(instruction_bytes, 0x66,
                      mmio_movzx_opcode_offset) != NULL) ||
          (decode.addr_size_override != 0) !=
              (memchr(instruction_bytes, 0x67,
                      mmio_movzx_opcode_offset) != NULL))) ||
        (is_mmio_mov_reg_store &&
         (decode.len < mmio_reg_load_opcode_offset + 2u ||
          decode.len > instruction_length ||
          decode.cmd != X86_DECODE_CMD_MOV || decode.rep != 0 ||
          decode.lock || decode.modrm.mod == 3 ||
          decode.op[0].type != X86_VAR_RM ||
          decode.op[1].type != X86_VAR_REG ||
          (decode.operand_size != 1 && decode.operand_size != 2 &&
           decode.operand_size != 4 &&
           !((env->hflags & HF_CS64_MASK) &&
             decode.operand_size == 8)) ||
          (decode.op_size_override != 0) !=
              (memchr(instruction_bytes, 0x66,
                      mmio_reg_load_opcode_offset) != NULL) ||
          (decode.addr_size_override != 0) !=
              (memchr(instruction_bytes, 0x67,
                      mmio_reg_load_opcode_offset) != NULL))) ||
        (is_vga_string_stos &&
         (decode.len != (instruction_bytes[0] == 0x66 ? 3 : 2) ||
          decode.cmd != X86_DECODE_CMD_STOS ||
          decode.rep != PREFIX_REP || decode.lock ||
          decode.op_size_override !=
              (instruction_bytes[0] == 0x66 ? 0x66 : 0) ||
          (decode.addressing_size != 2 && decode.addressing_size != 4 &&
           !((env->hflags & HF_CS64_MASK) &&
             decode.addressing_size == 8)) ||
          decode.addr_size_override != 0 || decode.segment_override != 0 ||
          (decode.operand_size != 2 && decode.operand_size != 4))) ||
        (is_vga_string_movs &&
         (decode.len != (instruction_bytes[0] == 0x66 ? 3 : 2) ||
          decode.cmd != X86_DECODE_CMD_MOVS ||
          decode.rep != PREFIX_REP || decode.lock ||
          decode.op_size_override !=
              (instruction_bytes[0] == 0x66 ? 0x66 : 0) ||
          (decode.addressing_size != 2 && decode.addressing_size != 4 &&
           !((env->hflags & HF_CS64_MASK) &&
             decode.addressing_size == 8)) ||
          decode.addr_size_override != 0 || decode.segment_override != 0 ||
          (decode.operand_size != 1 && decode.operand_size != 2 &&
           decode.operand_size != 4) ||
          (instruction_bytes[instruction_bytes[0] == 0x66 ? 2 : 1] == 0xa4 &&
           decode.operand_size != 1)))) {
        error_setg(errp,
                   "AMD-SVM NPF instruction is outside the supported ROM-write subset (len=%u bytes=%02x:%02x:%02x:%02x:%02x decode=%u/%u/%d/%d/%d/%d imm=%u/%d/%d op=%d addr=%d mmio-load=%u mmio-zx=%u load-off=%u mmio-store=%u rom-reg=%u es=%u ao=%u mod=%u op0=%u op1=%u seg=%u lock=%u addrpre=%u op-pre=%u CS=%08x DS=%016" PRIx64 "/%016" PRIx64 " SS=%016" PRIx64 "/%016" PRIx64 ")",
                   instruction_length, instruction_bytes[0],
                   instruction_bytes[1], instruction_bytes[2],
                   instruction_bytes[3], instruction_bytes[4], decode.len,
                   decode.cmd, decode.addressing_size, decode.operand_size,
                   decode.rep, decode.op_size_override, mov_imm_size,
                   mov_imm_has_op_prefix, mov_imm_has_addr_prefix,
                   decode.op_size_override,
                   decode.addr_size_override,
                   is_mmio_mov_reg_load, is_mmio_movzx_reg_load,
                   mmio_reg_load_opcode_offset,
                   is_mmio_mov_reg_store, is_rom_mov_reg,
                   is_rom_mov_reg_es, is_rom_mov_reg_ao,
                   decode.modrm.mod, decode.op[0].type, decode.op[1].type,
                   decode.segment_override, decode.lock,
                   decode.addr_size_override != 0,
                   decode.op_size_override != 0,
                   env->segs[R_CS].flags,
                   (uint64_t)env->segs[R_DS].base,
                   (uint64_t)env->segs[R_DS].limit,
                   (uint64_t)env->segs[R_SS].base,
                   (uint64_t)env->segs[R_SS].limit);
        return -EINVAL;
    }

    if (is_vga_rom_cmp &&
        (decode.cmd != X86_DECODE_CMD_CMP || decode.rep != 0 ||
         decode.lock || decode.addressing_size != 4 ||
         decode.op_size_override != 0x66 || decode.operand_size != 2 ||
         decode.modrm.mod == 3 || decode.modrm.reg != 7 ||
         decode.op[0].type != X86_VAR_RM ||
         decode.op[1].type != X86_VAR_IMMEDIATE)) {
        error_setg(errp, "AMD-SVM VGA ROM compare is outside the supported 16-bit immediate form");
        return -EINVAL;
    }

    if (is_vga_string_stos) {
        uint64_t original_count, batch, offset, linear, gpa;
        uint64_t max_in_aperture, max_in_segment;
        target_ulong original_rip = env->eip;
        target_ulong expected_di;
        unsigned width = decode.operand_size;
        unsigned address_size = decode.addressing_size;
        bool long_mode = (env->hflags & HF_CS64_MASK) != 0;

        if (env->eflags & DF_MASK) {
            error_setg(errp, "AMD-SVM VGA REP STOS with backward direction is unsupported");
            return -ENOTSUP;
        }
        original_count = read_reg(env, R_ECX, address_size);
        offset = read_reg(env, R_EDI, address_size);
        if (original_count == 0 ||
            (!long_mode &&
             (offset > env->segs[R_ES].limit ||
              width - 1u > env->segs[R_ES].limit - offset ||
              env->segs[R_ES].base > UINT64_MAX - offset))) {
            error_setg(errp, "AMD-SVM VGA REP STOS count or ES range is invalid");
            return -EOVERFLOW;
        }
        linear = long_mode ? offset : env->segs[R_ES].base + offset;
        if (mmu_gva_to_gpa(cs, linear, &gpa,
                           MMU_TRANSLATE_VALIDATE_WRITE) !=
                MMU_TRANSLATE_SUCCESS || gpa != fault_gpa ||
            !amdsvm_npf_targets_vga_mmio(exit_info1, gpa, width)) {
            error_setg(errp, "AMD-SVM VGA REP STOS destination does not match the VGA NPF GPA");
            return -EINVAL;
        }

        max_in_aperture =
            (AMDSVM_VGA_APERTURE_END - gpa) / width;
        max_in_segment = long_mode ? UINT64_MAX :
            (env->segs[R_ES].limit - offset + 1u) / width;
        batch = MIN(original_count, (uint64_t)AMDSVM_VGA_STOS_BATCH_MAX);
        batch = MIN(batch, max_in_aperture);
        batch = MIN(batch, max_in_segment);
        if (batch == 0) {
            error_setg(errp, "AMD-SVM VGA REP STOS has no safe element in the VGA aperture");
            return -EOVERFLOW;
        }

        write_reg(env, R_ECX, batch, address_size);
        exec_instruction(env, &decode);
        expected_di = offset + batch * width;
        if (address_size == 2) {
            expected_di &= UINT64_C(0xffff);
        } else if (address_size == 4) {
            expected_di &= UINT64_C(0xffffffff);
        }
        if (env->eip != original_rip + decode.len ||
            read_reg(env, R_ECX, address_size) != 0 ||
            read_reg(env, R_EDI, address_size) != expected_di) {
            error_setg(errp, "AMD-SVM VGA REP STOS emulation did not complete its bounded batch");
            return -EIO;
        }
        if (original_count > batch) {
            write_reg(env, R_ECX, original_count - batch, address_size);
            env->eip = original_rip;
        }
        return 0;
    }

    if (is_vga_rom_cmp) {
        target_ulong original_rip = env->eip;
        uint64_t linear, gpa;

        calc_modrm_operand(env, &decode, &decode.op[0]);
        linear = decode.op[0].addr;
        if (mmu_gva_to_gpa(cs, linear, &gpa, 0) !=
                MMU_TRANSLATE_SUCCESS || gpa != fault_gpa ||
            !amdsvm_npf_targets_vga_rom_read(exit_info1, gpa,
                                             decode.operand_size)) {
            error_setg(errp, "AMD-SVM compare does not target mapped VGA ROM");
            return -EINVAL;
        }
        exec_instruction(env, &decode);
        if (env->eip != original_rip + decode.len) {
            error_setg(errp, "AMD-SVM VGA ROM compare emulation did not complete");
            return -EIO;
        }
        return 0;
    }

    if (is_rom_pop_rm) {
        uint64_t linear, gpa, stack_pointer, stack_linear, value = 0;
        unsigned width = decode.operand_size;
        unsigned stack_address_size;
        target_ulong original_rip = env->eip;

        if (shadow_backing_hva == 0 || immutable_rom_bytes < width) {
            error_setg(errp, "AMD-SVM POP memory write lacks a ROM shadow");
            return -ENOTSUP;
        }
        calc_modrm_operand(env, &decode, &decode.op[0]);
        linear = decode.op[0].addr;
        if (mmu_gva_to_gpa(cs, linear, &gpa,
                           MMU_TRANSLATE_VALIDATE_WRITE) !=
                MMU_TRANSLATE_SUCCESS || gpa != fault_gpa) {
            error_setg(errp, "AMD-SVM POP destination does not match the NPF GPA");
            return -EINVAL;
        }
        stack_address_size =
            (env->segs[R_SS].flags & DESC_B_MASK) != 0 ? 4u : 2u;
        stack_pointer = read_reg(env, R_ESP, stack_address_size);
        if (stack_pointer > env->segs[R_SS].limit ||
            width - 1u > env->segs[R_SS].limit - stack_pointer ||
            env->segs[R_SS].base > UINT64_MAX - stack_pointer) {
            error_setg(errp, "AMD-SVM POP stack read exceeds SS");
            return -EOVERFLOW;
        }
        stack_linear = env->segs[R_SS].base + stack_pointer;
        if (x86_read_mem(cs, &value, stack_linear, width) !=
                MMU_TRANSLATE_SUCCESS ||
            x86_write_mem(cs, &value, linear, width) !=
                MMU_TRANSLATE_SUCCESS) {
            error_setg(errp, "AMD-SVM POP stack read or ROM shadow write failed");
            return -EIO;
        }
        write_reg(env, R_ESP, stack_pointer + width, stack_address_size);
        env->eip += decode.len;
        if (env->eip != original_rip + decode.len ||
            amdsvm_sync_shadow_write(gpa, shadow_backing_hva,
                shadow_backing_bytes, width, NULL, errp) != 0) {
            if (!errp || !*errp) {
                error_setg(errp, "AMD-SVM POP firmware shadow sync failed");
            }
            return -EIO;
        }
        return 0;
    }

    if (is_rom_mov_seg_ao) {
        uint64_t linear, gpa;
        uint16_t selector;
        target_ulong original_rip = env->eip;

        if (shadow_backing_hva == 0 || immutable_rom_bytes < 2u) {
            error_setg(errp, "AMD-SVM MOV segment store lacks a ROM shadow");
            return -ENOTSUP;
        }
        calc_modrm_operand(env, &decode, &decode.op[0]);
        linear = decode.op[0].addr;
        if (mmu_gva_to_gpa(cs, linear, &gpa,
                           MMU_TRANSLATE_VALIDATE_WRITE) !=
                MMU_TRANSLATE_SUCCESS || gpa != fault_gpa) {
            error_setg(errp, "AMD-SVM MOV segment destination does not match the NPF GPA");
            return -EINVAL;
        }
        selector = env->segs[decode.modrm.reg].selector;
        if (x86_write_mem(cs, &selector, linear, sizeof(selector)) !=
                MMU_TRANSLATE_SUCCESS) {
            error_setg(errp, "AMD-SVM MOV segment shadow write failed");
            return -EIO;
        }
        env->eip += decode.len;
        if (env->eip != original_rip + decode.len ||
            amdsvm_sync_shadow_write(gpa, shadow_backing_hva,
                shadow_backing_bytes, sizeof(selector), NULL, errp) != 0) {
            if (!errp || !*errp) {
                error_setg(errp, "AMD-SVM MOV segment firmware shadow sync failed");
            }
            return -EIO;
        }
        return 0;
    }

    if (is_rom_call_reg || is_rom_call_rel) {
        uint64_t stack_pointer, new_stack_pointer, stack_linear;
        target_ulong target = 0;
        unsigned width = decode.operand_size;
        unsigned stack_address_size =
            (env->segs[R_SS].flags & DESC_B_MASK) != 0 ? 4u : 2u;
        target_ulong original_rip = env->eip;
        uint64_t return_rip;

        if (shadow_backing_hva == 0 || immutable_rom_bytes < width) {
            error_setg(errp, "AMD-SVM CALL stack write lacks a ROM shadow");
            return -ENOTSUP;
        }
        if (is_rom_call_reg) {
            calc_modrm_operand(env, &decode, &decode.op[0]);
            if (read_val_ext(env, &decode.op[0], width, &target)) {
                error_setg(errp, "AMD-SVM CALL target register could not be read");
                return -EIO;
            }
        } else {
            target = original_rip + decode.len + (int64_t)decode.op[0].val;
        }
        stack_pointer = read_reg(env, R_ESP, stack_address_size);
        if (stack_pointer < width) {
            error_setg(errp, "AMD-SVM CALL stack pointer underflows");
            return -EOVERFLOW;
        }
        new_stack_pointer = stack_pointer - width;
        if (new_stack_pointer > env->segs[R_SS].limit ||
            width - 1u > env->segs[R_SS].limit - new_stack_pointer ||
            env->segs[R_SS].base > UINT64_MAX - new_stack_pointer) {
            error_setg(errp, "AMD-SVM CALL stack write exceeds SS");
            return -EOVERFLOW;
        }
        stack_linear = env->segs[R_SS].base + new_stack_pointer;
        return_rip = original_rip + decode.len;
        if (x86_write_mem(cs, &return_rip, stack_linear, width) !=
                MMU_TRANSLATE_SUCCESS) {
            error_setg(errp, "AMD-SVM CALL return address write failed");
            return -EIO;
        }
        write_reg(env, R_ESP, new_stack_pointer, stack_address_size);
        env->eip = width == 2 ? (uint16_t)target : (uint32_t)target;
        if (amdsvm_sync_shadow_write(fault_gpa, shadow_backing_hva,
                shadow_backing_bytes, width, NULL, errp) != 0) {
            return -EIO;
        }
        return 0;
    }

    if (is_rom_push_reg) {
        uint64_t stack_pointer, new_stack_pointer, stack_linear, value = 0;
        unsigned width = decode.operand_size;
        unsigned stack_address_size =
            (env->segs[R_SS].flags & DESC_B_MASK) != 0 ? 4u : 2u;

        if (shadow_backing_hva == 0 || immutable_rom_bytes < width) {
            error_setg(errp, "AMD-SVM PUSH register write lacks a ROM shadow");
            return -ENOTSUP;
        }
        if (read_val_ext(env, &decode.op[0], width, &value)) {
            error_setg(errp, "AMD-SVM PUSH source register could not be read");
            return -EIO;
        }
        stack_pointer = read_reg(env, R_ESP, stack_address_size);
        if (stack_pointer < width) {
            error_setg(errp, "AMD-SVM PUSH stack pointer underflows");
            return -EOVERFLOW;
        }
        new_stack_pointer = stack_pointer - width;
        if (new_stack_pointer > env->segs[R_SS].limit ||
            width - 1u > env->segs[R_SS].limit - new_stack_pointer ||
            env->segs[R_SS].base > UINT64_MAX - new_stack_pointer) {
            error_setg(errp, "AMD-SVM PUSH stack write exceeds SS");
            return -EOVERFLOW;
        }
        stack_linear = env->segs[R_SS].base + new_stack_pointer;
        if (x86_write_mem(cs, &value, stack_linear, width) !=
                MMU_TRANSLATE_SUCCESS) {
            error_setg(errp, "AMD-SVM PUSH stack write failed");
            return -EIO;
        }
        write_reg(env, R_ESP, new_stack_pointer, stack_address_size);
        env->eip += decode.len;
        if (amdsvm_sync_shadow_write(fault_gpa, shadow_backing_hva,
                shadow_backing_bytes, width, NULL, errp) != 0) {
            return -EIO;
        }
        return 0;
    }

    if (is_mmio_moffs_load) {
        uint64_t offset = (uint32_t)ldl_le_p(instruction_bytes + 1u);
        uint64_t linear, gpa;
        unsigned width = instruction_bytes[0] == 0xa0 ? 1u : 4u;
        target_ulong original_rip = env->eip;

        if (offset > UINT32_MAX || offset > env->segs[R_DS].limit ||
            width - 1u > env->segs[R_DS].limit - offset ||
            env->segs[R_DS].base > UINT64_MAX - offset) {
            error_setg(errp,
                       "AMD-SVM MMIO MOV moffs address exceeds DS (offset=0x%" PRIx64 " base=0x%" PRIx64 " limit=0x%x width=%u gpa=0x%" PRIx64 ")",
                       offset, env->segs[R_DS].base,
                       env->segs[R_DS].limit, width, fault_gpa);
            return -ENOTSUP;
        }
        linear = env->segs[R_DS].base + offset;
        if (mmu_gva_to_gpa(cs, linear, &gpa, 0) !=
                MMU_TRANSLATE_SUCCESS || gpa != fault_gpa ||
            !amdsvm_npf_targets_mmio(exit_info1, gpa, width, false)) {
            error_setg(errp, "AMD-SVM MMIO MOV moffs does not target a mapped device read");
            return -EINVAL;
        }
        exec_instruction(env, &decode);
        if (env->eip != original_rip + decode.len) {
            error_setg(errp, "AMD-SVM MMIO MOV moffs read did not complete");
            return -EIO;
        }
        return 0;
    }

    if (is_mmio_mov_reg_load) {
        target_ulong original_rip = env->eip;
        uint64_t linear, gpa;
        unsigned width = decode.operand_size;

        calc_modrm_operand(env, &decode, &decode.op[1]);
        linear = decode.op[1].addr;
        if (mmu_gva_to_gpa(cs, linear, &gpa, 0) !=
                MMU_TRANSLATE_SUCCESS || gpa != fault_gpa ||
            !amdsvm_npf_targets_mmio(exit_info1, gpa, width, false)) {
            error_setg(errp,
                       "AMD-SVM register MOV read does not target a mapped MMIO device");
            return -EINVAL;
        }
        exec_instruction(env, &decode);
        if (env->eip != original_rip + decode.len) {
            error_setg(errp,
                       "AMD-SVM register MOV MMIO read did not complete");
            return -EIO;
        }
        return 0;
    }

    if (is_mmio_movzx_reg_load) {
        target_ulong original_rip = env->eip;
        uint64_t linear, gpa;
        unsigned width =
            instruction_bytes[mmio_movzx_opcode_offset + 1u] == 0xb6 ? 1u : 2u;

        calc_modrm_operand(env, &decode, &decode.op[1]);
        linear = decode.op[1].addr;
        if (mmu_gva_to_gpa(cs, linear, &gpa, 0) !=
                MMU_TRANSLATE_SUCCESS || gpa != fault_gpa ||
            !amdsvm_npf_targets_mmio(exit_info1, gpa, width, false)) {
            error_setg(errp,
                       "AMD-SVM MOVZX source does not target the reported MMIO NPF");
            return -EINVAL;
        }
        exec_instruction(env, &decode);
        if (env->eip != original_rip + decode.len) {
            error_setg(errp,
                       "AMD-SVM MOVZX MMIO read did not complete");
            return -EIO;
        }
        return 0;
    }

    if (is_mmio_mov_reg_store) {
        uint64_t linear, gpa, offset;
        unsigned width = decode.operand_size;

        calc_modrm_operand(env, &decode, &decode.op[0]);
        linear = decode.op[0].addr;
        if (mmu_gva_to_gpa(cs, linear, &gpa,
                           MMU_TRANSLATE_VALIDATE_WRITE) !=
                MMU_TRANSLATE_SUCCESS || gpa != fault_gpa) {
            error_setg(errp,
                       "AMD-SVM register MOV destination does not match the NPF");
            return -EINVAL;
        }
        if (amdsvm_npf_targets_mmio(exit_info1, gpa, width, true)) {
            return amdsvm_emulate_mmio_memory_write(env, &decode, errp);
        }
        if (decode.segment_override != PREFIX_ES_SEG_OVERRIDE) {
            error_setg(errp,
                       "AMD-SVM register MOV store is neither assigned MMIO nor a validated ES VGA access");
            return -ENOTSUP;
        }
        if (linear < env->segs[R_ES].base) {
            error_setg(errp,
                       "AMD-SVM VGA register MOV destination underflows ES");
            return -EOVERFLOW;
        }
        offset = linear - env->segs[R_ES].base;
        if (offset > env->segs[R_ES].limit ||
            width - 1u > env->segs[R_ES].limit - offset ||
            !amdsvm_npf_targets_vga_mmio(exit_info1, gpa, width)) {
            error_setg(errp,
                       "AMD-SVM register MOV destination is outside the validated VGA aperture");
            return -EINVAL;
        }
        return amdsvm_emulate_mmio_memory_write(env, &decode, errp);
    }

    if (is_mmio_moffs_store) {
        uint64_t offset = (uint32_t)ldl_le_p(instruction_bytes + 1u);
        uint64_t linear, gpa;
        unsigned width = instruction_bytes[0] == 0xa2 ? 1u : 4u;
        target_ulong original_rip = env->eip;

        if (offset > env->segs[R_DS].limit ||
            width - 1u > env->segs[R_DS].limit - offset ||
            env->segs[R_DS].base > UINT64_MAX - offset) {
            error_setg(errp,
                       "AMD-SVM MMIO MOV moffs store exceeds DS (offset=0x%" PRIx64 " base=0x%" PRIx64 " limit=0x%x width=%u gpa=0x%" PRIx64 ")",
                       offset, env->segs[R_DS].base,
                       env->segs[R_DS].limit, width, fault_gpa);
            return -ENOTSUP;
        }
        linear = env->segs[R_DS].base + offset;
        if (mmu_gva_to_gpa(cs, linear, &gpa,
                           MMU_TRANSLATE_VALIDATE_WRITE) !=
                MMU_TRANSLATE_SUCCESS || gpa != fault_gpa ||
            !amdsvm_npf_targets_mmio(exit_info1, gpa, width, true)) {
            error_setg(errp, "AMD-SVM MMIO MOV moffs does not target a mapped device write");
            return -EINVAL;
        }
        exec_instruction(env, &decode);
        if (env->eip != original_rip + decode.len) {
            error_setg(errp, "AMD-SVM MMIO MOV moffs write did not complete");
            return -EIO;
        }
        return 0;
    }

    if (is_rom_arith_mem &&
        (decode.len > instruction_length ||
         decode.cmd != (instruction_bytes[arith_opcode_offset] == 0xfe ?
                        (((instruction_bytes[arith_opcode_offset + 1u] >> 3) & 7u) == 0 ?
                         X86_DECODE_CMD_INC : X86_DECODE_CMD_DEC) :
                        ((instruction_bytes[arith_opcode_offset] == 0x00 ||
                          instruction_bytes[arith_opcode_offset] == 0x01) ?
                         X86_DECODE_CMD_ADD : X86_DECODE_CMD_SUB)) ||
         decode.modrm.mod == 3 ||
         decode.op[0].type != X86_VAR_RM || decode.rep != 0 ||
         ((instruction_bytes[arith_opcode_offset] == 0x00 ||
           instruction_bytes[arith_opcode_offset] == 0x28 ||
           instruction_bytes[arith_opcode_offset] == 0xfe) &&
          decode.operand_size != 1) ||
         ((instruction_bytes[arith_opcode_offset] == 0x01 ||
           instruction_bytes[arith_opcode_offset] == 0x29) &&
          (decode.operand_size != amdsvm_operand_size(env,
                                                       arith_has_op_prefix) ||
           (decode.op_size_override != 0) != arith_has_op_prefix)) ||
         (decode.addr_size_override != 0) != arith_has_addr_prefix ||
         (arith_has_addr_prefix &&
          (decode.addressing_size != 2 && decode.addressing_size != 4)))) {
        error_setg(errp,
                   "AMD-SVM ROM arithmetic is outside the supported flat memory-destination form");
        return -EINVAL;
    }

    if (is_rom_mov_imm || is_rom_mov_reg) {
        unsigned width = is_rom_mov_imm ? mov_imm_size :
                         decode.operand_size;
        uint64_t linear, gpa;

        if (is_rom_mov_reg) {
            calc_modrm_operand(env, &decode, &decode.op[0]);
            linear = decode.op[0].addr;
            if (is_rom_mov_reg_es) {
                uint64_t offset;

                if (linear < env->segs[R_ES].base) {
                    error_setg(errp, "AMD-SVM real-mode ES MOV address underflows the segment base");
                    return -EOVERFLOW;
                }
                offset = linear - env->segs[R_ES].base;
                if (offset > env->segs[R_ES].limit ||
                    width - 1u > env->segs[R_ES].limit - offset) {
                    error_setg(errp, "AMD-SVM real-mode ES MOV crosses the segment limit");
                    return -EOVERFLOW;
                }
            }
        } else {
            calc_modrm_operand(env, &decode, &decode.op[0]);
            linear = decode.op[0].addr;
        }
        if ((is_rom_mov_imm && mov_imm_has_addr_prefix) ||
            (is_rom_mov_reg && !is_rom_mov_reg_es)) {
            X86Seg segment = amdsvm_modrm_default_segment(&decode);
            uint64_t base = env->segs[segment].base;
            uint64_t limit = env->segs[segment].limit;
            uint64_t offset;

            if (linear < base) {
                error_setg(errp, "AMD-SVM C6/C7 destination underflows its segment base");
                return -EOVERFLOW;
            }
            offset = linear - base;
            if (offset > limit || width - 1u > limit - offset ||
                base > UINT64_MAX - offset || base + offset != linear) {
                error_setg(errp, "AMD-SVM C6/C7 destination exceeds its segment limit");
                return -EOVERFLOW;
            }
        }
        if (mmu_gva_to_gpa(cs, linear, &gpa,
                           MMU_TRANSLATE_VALIDATE_WRITE) !=
                MMU_TRANSLATE_SUCCESS || gpa != fault_gpa) {
            error_setg(errp, "AMD-SVM MOV destination does not match the NPF");
            return -EINVAL;
        }

        if (immutable_rom_bytes < width) {
            if (amdsvm_npf_targets_vga_mmio(exit_info1, gpa, width)) {
                return amdsvm_emulate_mmio_memory_write(env, &decode, errp);
            }
            if (amdsvm_npf_targets_mmio(exit_info1, gpa, width, true)) {
                return amdsvm_emulate_mmio_memory_write(env, &decode, errp);
            }
            if (amdsvm_npf_targets_unassigned(exit_info1, gpa, width)) {
                target_ulong original_rip = env->eip;

                exec_instruction(env, &decode);
                if (env->eip != original_rip + decode.len) {
                    error_setg(errp, "AMD-SVM unassigned MOV write emulation did not complete");
                    return -EIO;
                }
                return 0;
            }
            {
                error_setg(errp,
                           "AMD-SVM MOV destination is outside immutable ROM and VGA MMIO (gpa=0x%" PRIx64 " width=%u rom_remaining=0x%" PRIx64 " shadow=0x%" PRIx64 "+0x%" PRIx64 " unassigned=%u)",
                           gpa, width, immutable_rom_bytes,
                           shadow_backing_hva, shadow_backing_bytes,
                           unassigned_npf);
                return -EINVAL;
            }
        }

        if (shadow_backing_hva != 0) {
            target_ulong original_rip = env->eip;

            exec_instruction(env, &decode);
            if (env->eip != original_rip + decode.len ||
                amdsvm_sync_shadow_write(gpa, shadow_backing_hva,
                    shadow_backing_bytes, width, NULL, errp) != 0) {
                if (!errp || !*errp) {
                    error_setg(errp, "AMD-SVM MOV firmware shadow emulation failed");
                }
                return -EIO;
            }
        } else {
            /* QEMU ignores writes to immutable ROM until chipset PAM enables
             * a RAM shadow. This path preserves the ordinary ROM behavior. */
            env->eip += decode.len;
        }
        return 0;
    }

    if (is_rom_or_imm8 || is_rom_or_reg) {
        uint64_t linear, gpa;
        unsigned width = decode.operand_size;
        target_ulong original_rip = env->eip;

        if (is_rom_or_reg) {
            if (env->segs[R_DS].base != 0 ||
                env->segs[R_DS].limit != UINT32_MAX ||
                env->segs[R_SS].base != 0 ||
                env->segs[R_SS].limit != UINT32_MAX) {
                error_setg(errp, "AMD-SVM register OR is outside flat firmware segments");
                return -ENOTSUP;
            }
            calc_modrm_operand(env, &decode, &decode.op[0]);
            linear = decode.op[0].addr;
        } else {
            X86Seg segment = amdsvm_modrm_default_segment(&decode);
            uint64_t base = env->segs[segment].base;
            uint64_t limit = env->segs[segment].limit;
            uint64_t offset;

            calc_modrm_operand(env, &decode, &decode.op[0]);
            linear = decode.op[0].addr;
            if (linear < base) {
                error_setg(errp, "AMD-SVM immediate arithmetic destination underflows its segment base");
                return -EOVERFLOW;
            }
            offset = linear - base;
            if (offset > limit || width - 1u > limit - offset ||
                base > UINT64_MAX - offset || base + offset != linear) {
                error_setg(errp, "AMD-SVM immediate arithmetic destination exceeds its segment limit");
                return -EOVERFLOW;
            }
        }
        if (mmu_gva_to_gpa(cs, linear, &gpa,
                           MMU_TRANSLATE_VALIDATE_WRITE) !=
                MMU_TRANSLATE_SUCCESS || gpa != fault_gpa) {
            error_setg(errp, "AMD-SVM immediate arithmetic destination does not match the NPF");
            return -EINVAL;
        }

        if (immutable_rom_bytes < width) {
            if (amdsvm_npf_targets_vga_mmio(exit_info1, gpa, width)) {
                return amdsvm_emulate_mmio_memory_write(env, &decode, errp);
            }
            if (amdsvm_npf_targets_mmio(exit_info1, gpa, width, true)) {
                return amdsvm_emulate_mmio_memory_write(env, &decode, errp);
            }
            if (amdsvm_npf_targets_mmio(exit_info1, gpa, width, true)) {
                return amdsvm_emulate_mmio_memory_write(env, &decode, errp);
            }
            if (!amdsvm_npf_targets_unassigned(exit_info1, gpa, width)) {
                error_setg(errp, "AMD-SVM immediate arithmetic destination is outside immutable ROM and VGA MMIO");
                return -EINVAL;
            }
        }

        /* The shared QEMU emulator supplies the RMW and flag semantics. Its
         * ROM write follows QEMU's normal immutable-ROM behavior (discard). */
        exec_instruction(env, &decode);
        if (env->eip != original_rip + decode.len) {
            error_setg(errp, "AMD-SVM ROM immediate arithmetic emulation did not complete");
            return -EIO;
        }
        if (shadow_backing_hva != 0 &&
            amdsvm_sync_shadow_write(gpa, shadow_backing_hva,
                shadow_backing_bytes, width, NULL, errp) != 0) {
            return -EIO;
        }
        return 0;
    }

    if (is_rom_mov_moffs) {
        if (is_rom_mov_moffs_es) {
            uint64_t offset = decode.op[0].addr;
            uint64_t linear, gpa;
            unsigned width = decode.operand_size;
            target_ulong original_rip = env->eip;

            if (offset > env->segs[R_ES].limit ||
                width - 1u > env->segs[R_ES].limit - offset ||
                env->segs[R_ES].base > UINT64_MAX - offset) {
                error_setg(errp, "AMD-SVM real-mode ES MOV moffs exceeds the segment");
                return -EOVERFLOW;
            }
            linear = decode_linear_addr(env, &decode, offset, R_DS);
            if (mmu_gva_to_gpa(cs, linear, &gpa,
                               MMU_TRANSLATE_VALIDATE_WRITE) !=
                    MMU_TRANSLATE_SUCCESS || gpa != fault_gpa) {
                error_setg(errp, "AMD-SVM real-mode ES MOV moffs does not match the NPF GPA");
                return -EINVAL;
            }
            if (immutable_rom_bytes < width &&
                !amdsvm_npf_targets_unassigned(exit_info1, gpa, width)) {
                error_setg(errp, "AMD-SVM real-mode ES MOV moffs is outside immutable ROM");
                return -EINVAL;
            }
            if (shadow_backing_hva != 0) {
                exec_instruction(env, &decode);
                if (env->eip != original_rip + decode.len ||
                    amdsvm_sync_shadow_write(gpa, shadow_backing_hva,
                        shadow_backing_bytes, width, NULL, errp) != 0) {
                    if (!errp || !*errp) {
                        error_setg(errp, "AMD-SVM real-mode ES firmware shadow write failed");
                    }
                    return -EIO;
                }
            } else {
                env->eip += decode.len;
            }
            return 0;
        }
        uint64_t offset = (uint32_t)ldl_le_p(instruction_bytes + moffs_prefix + 1u);
        uint64_t linear, gpa;
        unsigned width = decode.operand_size;

        if (offset > env->segs[R_DS].limit ||
            width - 1u > env->segs[R_DS].limit - offset ||
            env->segs[R_DS].base > UINT64_MAX - offset) {
            error_setg(errp, "AMD-SVM MOV moffs destination overflows its segment");
            return -EOVERFLOW;
        }
        linear = env->segs[R_DS].base + offset;
        if (mmu_gva_to_gpa(cs, linear, &gpa,
                           MMU_TRANSLATE_VALIDATE_WRITE) !=
                MMU_TRANSLATE_SUCCESS || gpa != fault_gpa) {
            error_setg(errp, "AMD-SVM MOV moffs destination does not match the NPF");
            return -EINVAL;
        }
        if (immutable_rom_bytes < width) {
            if (amdsvm_npf_targets_vga_mmio(exit_info1, gpa, width)) {
                return amdsvm_emulate_mmio_memory_write(env, &decode, errp);
            }
            if (amdsvm_npf_targets_mmio(exit_info1, gpa, width, true)) {
                return amdsvm_emulate_mmio_memory_write(env, &decode, errp);
            }
            if (!amdsvm_npf_targets_unassigned(exit_info1, gpa, width)) {
                error_setg(errp, "AMD-SVM MOV moffs destination is outside immutable ROM and VGA MMIO");
                return -EINVAL;
            }
        }
        if (shadow_backing_hva != 0) {
            target_ulong original_rip = env->eip;

            exec_instruction(env, &decode);
            if (env->eip != original_rip + decode.len ||
                amdsvm_sync_shadow_write(gpa, shadow_backing_hva,
                    shadow_backing_bytes, width, NULL, errp) != 0) {
                if (!errp || !*errp) {
                    error_setg(errp, "AMD-SVM MOV firmware shadow emulation failed");
                }
                return -EIO;
            }
        } else {
            /* A2/A3 only store a register and have no other state effects. */
            env->eip += decode.len;
        }
        return 0;
    }

    if (is_rom_arith_mem) {
        uint64_t linear, gpa;
        unsigned width = decode.operand_size;
        target_ulong original_rip = env->eip;
        X86Seg segment = amdsvm_modrm_default_segment(&decode);
        uint64_t base = env->segs[segment].base;
        uint64_t limit = env->segs[segment].limit;
        uint64_t offset;

        calc_modrm_operand(env, &decode, &decode.op[0]);
        linear = decode.op[0].addr;
        if (linear < base) {
            error_setg(errp, "AMD-SVM arithmetic destination underflows its segment base");
            return -EOVERFLOW;
        }
        offset = linear - base;
        if (offset > limit || width - 1u > limit - offset ||
            base > UINT64_MAX - offset || base + offset != linear) {
            error_setg(errp, "AMD-SVM arithmetic destination exceeds its segment limit");
            return -EOVERFLOW;
        }
        if (mmu_gva_to_gpa(cs, linear, &gpa,
                           MMU_TRANSLATE_VALIDATE_WRITE) !=
                MMU_TRANSLATE_SUCCESS || gpa != fault_gpa) {
            error_setg(errp, "AMD-SVM arithmetic destination does not match the NPF GPA (rip=0x%llx fault=0x%llx linear=0x%llx gpa=0x%llx)",
                       (unsigned long long)env->eip,
                       (unsigned long long)fault_gpa,
                       (unsigned long long)linear,
                       (unsigned long long)gpa);
            return -EINVAL;
        }
        if (immutable_rom_bytes < width) {
            if (amdsvm_npf_targets_vga_mmio(exit_info1, gpa, width)) {
                return amdsvm_emulate_mmio_memory_write(env, &decode, errp);
            }
            if (amdsvm_npf_targets_mmio(exit_info1, gpa, width, true)) {
                return amdsvm_emulate_mmio_memory_write(env, &decode, errp);
            }
            if (!amdsvm_npf_targets_unassigned(exit_info1, gpa, width)) {
                error_setg(errp, "AMD-SVM arithmetic destination is outside immutable ROM and VGA MMIO");
                return -EINVAL;
            }
        }
        exec_instruction(env, &decode);
        if (env->eip != original_rip + decode.len) {
            error_setg(errp, "AMD-SVM ROM arithmetic emulation did not complete");
            return -EIO;
        }
        if (shadow_backing_hva != 0 &&
            amdsvm_sync_shadow_write(gpa, shadow_backing_hva,
                shadow_backing_bytes, width, NULL, errp) != 0) {
            return -EIO;
        }
        return 0;
    }

    if (is_vga_string_movs) {
        target_ulong original_rip = env->eip;
        unsigned address_size = decode.addressing_size;
        unsigned width = decode.operand_size;
        uint64_t count = read_reg(env, R_ECX, address_size);
        uint64_t si = read_reg(env, R_ESI, address_size);
        uint64_t di = read_reg(env, R_EDI, address_size);
        uint64_t address_limit = address_size == 2 ? UINT64_C(0x10000) :
                                                     UINT64_C(0x100000000);
        uint64_t src_gpa, dst_gpa, src_linear, dst_linear;

        if (count == 0 || (env->eflags & DF_MASK) ||
            si > env->segs[R_DS].limit ||
            width - 1u > env->segs[R_DS].limit - si ||
            di > env->segs[R_ES].limit ||
            width - 1u > env->segs[R_ES].limit - di ||
            width > address_limit - si || width > address_limit - di ||
            env->segs[R_DS].base > UINT64_MAX - si ||
            env->segs[R_ES].base > UINT64_MAX - di) {
            error_setg(errp,
                       "AMD-SVM VGA REP MOVS count, direction, or segment range is invalid");
            return -EOVERFLOW;
        }
        src_linear = env->segs[R_DS].base + si;
        dst_linear = env->segs[R_ES].base + di;
        if (mmu_gva_to_gpa(cs, src_linear, &src_gpa, 0) !=
                MMU_TRANSLATE_SUCCESS ||
            mmu_gva_to_gpa(cs, dst_linear, &dst_gpa,
                           MMU_TRANSLATE_VALIDATE_WRITE) !=
                MMU_TRANSLATE_SUCCESS) {
            error_setg(errp,
                       "AMD-SVM VGA REP MOVS source or destination translation failed");
            return -EINVAL;
        }
        if (mmio_read_npf) {
            if (src_gpa != fault_gpa ||
                src_gpa < AMDSVM_VGA_APERTURE_START ||
                src_gpa >= AMDSVM_VGA_APERTURE_END ||
                !amdsvm_npf_targets_mmio(exit_info1, src_gpa, width,
                                         false) ||
                !amdsvm_npf_targets_ram(dst_gpa, width, true)) {
                error_setg(errp,
                           "AMD-SVM VGA REP MOVS read is outside VGA-to-RAM transfer");
                return -EINVAL;
            }
        } else if (mmio_write_npf) {
            if (dst_gpa != fault_gpa ||
                !amdsvm_npf_targets_mmio(exit_info1, dst_gpa, width, true) ||
                dst_gpa < AMDSVM_VGA_APERTURE_START ||
                dst_gpa >= AMDSVM_VGA_APERTURE_END ||
                !amdsvm_npf_targets_ram(src_gpa, width, false)) {
                error_setg(errp,
                           "AMD-SVM VGA REP MOVS write is outside RAM-to-VGA transfer");
                return -EINVAL;
            }
        } else {
            error_setg(errp, "AMD-SVM VGA REP MOVS has an invalid NPF direction");
            return -EINVAL;
        }

        write_reg(env, R_ECX, 1, address_size);
        exec_instruction(env, &decode);
        if (env->eip != original_rip + decode.len ||
            read_reg(env, R_ECX, address_size) != 0 ||
            read_reg(env, R_ESI, address_size) != si + width ||
            read_reg(env, R_EDI, address_size) != di + width) {
            error_setg(errp,
                       "AMD-SVM VGA REP MOVS emulation did not complete one element");
            return -EIO;
        }
        if (count > 1) {
            write_reg(env, R_ECX, count - 1u, address_size);
            env->eip = original_rip;
        }
        return 0;
    }

    count = read_reg(env, R_ECX, decode.addressing_size);
    if (count == 0) {
        error_setg(errp, "AMD-SVM REP MOVSB NPF has a zero count");
        return -EINVAL;
    }
    si = read_reg(env, R_ESI, decode.addressing_size);
    di = read_reg(env, R_EDI, decode.addressing_size);
    if (env->eflags & DF_MASK) {
        error_setg(errp, "AMD-SVM ROM MOVSB NPF with backward direction is unsupported");
        return -ENOTSUP;
    }
    if (immutable_rom_bytes == 0 &&
        !amdsvm_npf_targets_vga_mmio(exit_info1, fault_gpa, 1) &&
        !amdsvm_npf_targets_mmio(exit_info1, fault_gpa, 1, true) &&
        !unassigned_npf) {
        error_setg(errp, "AMD-SVM REP MOVSB NPF destination is not an immutable ROM range in the committed guest map");
        return -ENOTSUP;
    }
    if (immutable_rom_bytes != 0) {
        batch = MIN(count, AMDSVM_NPF_ROM_MOVSB_BATCH_MAX);
        batch = MIN(batch, immutable_rom_bytes);
        if (shadow_backing_hva != 0) {
            batch = MIN(batch, shadow_backing_bytes);
        }
    } else {
        /* MMIO and unmapped string writes are issued one element at a time
         * to preserve callbacks and revalidate every guest physical byte. */
        batch = 1;
    }
    batch = MIN(batch,
                (decode.addressing_size == 2 ? UINT64_C(0x10000) :
                                               UINT64_C(0x100000000)) - si);
    batch = MIN(batch,
                (decode.addressing_size == 2 ? UINT64_C(0x10000) :
                                               UINT64_C(0x100000000)) - di);
    if (env->segs[R_DS].base > UINT64_MAX - si ||
        env->segs[R_ES].base > UINT64_MAX - di ||
        si > env->segs[R_DS].limit || di > env->segs[R_ES].limit ||
        batch == 0 || batch - 1u > env->segs[R_DS].limit - si ||
        batch - 1u > env->segs[R_ES].limit - di) {
        error_setg(errp, "AMD-SVM REP MOVSB segment address overflowed");
        return -EOVERFLOW;
    }
    src_linear = env->segs[R_DS].base + si;
    dst_linear = env->segs[R_ES].base + di;
    if (mmu_gva_to_gpa(cs, src_linear, &src_gpa, 0) !=
            MMU_TRANSLATE_SUCCESS ||
        mmu_gva_to_gpa(cs, dst_linear, &dst_gpa,
                       MMU_TRANSLATE_VALIDATE_WRITE) !=
            MMU_TRANSLATE_SUCCESS || dst_gpa != fault_gpa ||
        (immutable_rom_bytes == 0 &&
         !amdsvm_npf_targets_vga_mmio(exit_info1, dst_gpa, 1) &&
         !amdsvm_npf_targets_unassigned(exit_info1, dst_gpa, 1))) {
        error_setg(errp, "AMD-SVM REP MOVSB addresses do not match the reported NPF GPA");
        return -EINVAL;
    }
    if (shadow_backing_hva != 0 && src_gpa != dst_gpa &&
        src_gpa < dst_gpa + batch && dst_gpa < src_gpa + batch) {
        batch = 1;
    }

    tx = address_space_read(&address_space_memory, src_gpa,
                            MEMTXATTRS_UNSPECIFIED, values, batch);
    if (tx != MEMTX_OK) {
        error_setg(errp, "AMD-SVM REP MOVSB source could not be read from QEMU memory");
        rc = -EIO;
        return -EIO;
    }

    if (shadow_backing_hva != 0) {
        tx = address_space_write(&address_space_memory, dst_gpa,
                                 MEMTXATTRS_UNSPECIFIED, values, batch);
        if (tx != MEMTX_OK ||
            amdsvm_sync_shadow_write(dst_gpa, shadow_backing_hva,
                shadow_backing_bytes, batch, values, errp) != 0) {
            if (tx != MEMTX_OK) {
                error_setg(errp, "AMD-SVM firmware shadow RAM rejected the emulated byte batch");
            }
            return -EIO;
        }
    } else if (immutable_rom_bytes == 0 && !unassigned_npf) {
        tx = address_space_write(&address_space_memory, dst_gpa,
                                 MEMTXATTRS_UNSPECIFIED, values, 1);
        if (tx != MEMTX_OK) {
            error_setg(errp, "AMD-SVM REP MOVSB VGA MMIO destination rejected the byte write");
            return -EIO;
        }
    }

    /* QEMU guest writes to read-only ROM are ignored. VGA MMIO has already
     * received its byte through the normal QEMU address-space callbacks. */
    write_reg(env, R_ESI, si + batch, decode.addressing_size);
    write_reg(env, R_EDI, di + batch, decode.addressing_size);
    count -= batch;
    write_reg(env, R_ECX, count, decode.addressing_size);
    if (count == 0) {
        env->eip += decode.len;
    }
    rc = 0;
    return rc;
}

bool amdsvm_handle_msr_exit(CPUState *cs, uint64_t exit_info1,
                            uint64_t next_rip, uint32_t *event_injection,
                            uint32_t *event_error_code, Error **errp)
{
    CPUX86State *env = &X86_CPU(cs)->env;
    struct x86_decode decode = { 0 };
    x86_insn_stream stream;
    uint8_t instruction[15];
    target_ulong linear_ip;
    target_ulong original_rip = env->eip;
    unsigned i;

    if (!event_injection || !event_error_code) {
        error_setg(errp, "AMD-SVM MSR VMEXIT has no exception result storage");
        return false;
    }
    *event_injection = 0;
    *event_error_code = 0;

    if (exit_info1 > 1) {
        error_setg(errp, "AMD-SVM MSR VMEXIT has reserved EXITINFO1 bits set");
        return false;
    }
    if (!(env->hflags & HF_CS64_MASK)) {
        if (env->segs[R_CS].base > UINT64_MAX - env->eip) {
            error_setg(errp, "AMD-SVM MSR VMEXIT instruction address overflowed");
            return false;
        }
        linear_ip = env->segs[R_CS].base + env->eip;
    } else {
        linear_ip = env->eip;
    }
    for (i = 0; i < sizeof(instruction); i++) {
        if (linear_ip > UINT64_MAX - i ||
            cpu_memory_rw_debug(cs, linear_ip + i, &instruction[i], 1, 0) != 0) {
            break;
        }
    }
    if (i < 2) {
        error_setg(errp, "AMD-SVM could not read the guest MSR instruction bytes");
        return false;
    }
    stream.bytes = instruction;
    stream.len = i;
    if (!amdsvm_emulator_initialized) {
        error_setg(errp, "AMD-SVM x86 emulator was not initialized");
        return false;
    }
    if (decode_instruction_stream(env, &decode, &stream) == 0 ||
        decode.len <= 0 || decode.len > sizeof(instruction) ||
        decode.len > 15) {
        error_setg(errp, "AMD-SVM MSR VMEXIT direction does not match decoded guest instruction");
        return false;
    }

    if (!amdsvm_msr_exit_direction_valid(exit_info1,
            decode.cmd == X86_DECODE_CMD_RDMSR,
            decode.cmd == X86_DECODE_CMD_WRMSR)) {
        error_setg(errp, "AMD-SVM MSR VMEXIT direction does not match decoded guest instruction");
        return false;
    }
    if (!amdsvm_msr_next_rip_valid(env->eip, decode.len,
            (env->hflags & HF_CS64_MASK) != 0,
            (env->hflags & HF_CS32_MASK) != 0, next_rip)) {
        error_setg(errp, "AMD-SVM MSR VMEXIT NRIP mismatch (RIP 0x%llx, decoded length %d, NRIP 0x%llx)",
                   (unsigned long long)env->eip, decode.len,
                   (unsigned long long)next_rip);
        return false;
    }

    env->exception_injected = 0;
    env->has_error_code = 0;
    env->exception_nr = -1;
    if ((decode.cmd == X86_DECODE_CMD_RDMSR ?
         amdsvm_msr_read(cs) : amdsvm_msr_write(cs)) ||
        env->exception_injected) {
        if (env->exception_nr != EXCP0D_GPF || !env->has_error_code ||
            env->error_code != 0 || env->eip != original_rip) {
            error_setg(errp, "AMD-SVM MSR VMEXIT raised an unsupported guest exception (vector %d, error 0x%x)",
                       env->exception_nr, env->error_code);
            return false;
        }
        *event_injection = AMDVM_VCPU_EVENTINJ_VALID |
            AMDVM_VCPU_EVENTINJ_TYPE_EXCEPTION |
            AMDVM_VCPU_EVENTINJ_ERROR_VALID | AMDVM_VCPU_EXCEPTION_GP;
        env->exception_injected = 0;
        env->has_error_code = 0;
        env->exception_nr = -1;
        return true;
    }
    env->eip = next_rip;
    return true;
}

#define AMDVM_SVM_EXITINFO_CR_VALID (UINT64_C(1) << 63)
#define AMDVM_SVM_EXITINFO_REG_MASK UINT64_C(0x0f)
#define AMDVM_CR0_ALLOWED_MASK (CR0_PE_MASK | CR0_MP_MASK | CR0_EM_MASK | \
    CR0_TS_MASK | CR0_ET_MASK | CR0_NE_MASK | CR0_WP_MASK | CR0_AM_MASK | \
    CR0_NW_MASK | CR0_CD_MASK | CR0_PG_MASK)

static const int amdsvm_cr_gpr_map[16] = {
    R_EAX, R_ECX, R_EDX, R_EBX, R_ESP, R_EBP, R_ESI, R_EDI,
    R_R8, R_R9, R_R10, R_R11, R_R12, R_R13, R_R14, R_R15,
};

unsigned amdsvm_state_read_guest_instruction(CPUState *cs,
                                             uint8_t instruction[15])
{
    CPUX86State *env;
    target_ulong linear_ip;
    unsigned i;

    if (!cs || !instruction) {
        return 0;
    }
    memset(instruction, 0xcc, 15);
    env = &X86_CPU(cs)->env;
    if (env->hflags & HF_CS64_MASK) {
        linear_ip = env->eip;
    } else {
        if (env->segs[R_CS].base > UINT64_MAX - env->eip) {
            return 0;
        }
        linear_ip = env->segs[R_CS].base + env->eip;
    }
    for (i = 0; i < 15; i++) {
        if (linear_ip > UINT64_MAX - i ||
            cpu_memory_rw_debug(cs, linear_ip + i, &instruction[i], 1, 0) != 0) {
            break;
        }
    }
    return i;
}

static bool amdsvm_cr_exit_code(uint64_t exit_code, unsigned *cr,
                                bool *write)
{
    switch (exit_code) {
    case SVM_EXIT_READ_CR0: *cr = 0; *write = false; return true;
    case SVM_EXIT_READ_CR2: *cr = 2; *write = false; return true;
    case SVM_EXIT_READ_CR3: *cr = 3; *write = false; return true;
    case SVM_EXIT_READ_CR4: *cr = 4; *write = false; return true;
    case SVM_EXIT_WRITE_CR0: *cr = 0; *write = true; return true;
    case SVM_EXIT_WRITE_CR2: *cr = 2; *write = true; return true;
    case SVM_EXIT_WRITE_CR3: *cr = 3; *write = true; return true;
    case SVM_EXIT_WRITE_CR4: *cr = 4; *write = true; return true;
    default: return false;
    }
}

static bool amdsvm_cr_write_valid(CPUX86State *env, X86CPU *cpu,
                                  unsigned cr, uint64_t value,
                                  uint64_t *normalized)
{
    switch (cr) {
    case 0:
        if ((value & ~((uint64_t)AMDVM_CR0_ALLOWED_MASK)) ||
            ((value & CR0_NW_MASK) && !(value & CR0_CD_MASK)) ||
            ((env->efer & MSR_EFER_LME) && (value & CR0_PG_MASK) &&
             (!(value & CR0_PE_MASK) || !(env->cr[4] & CR4_PAE_MASK))) ||
            ((env->efer & MSR_EFER_LMA) && (value & CR0_PG_MASK) &&
             !(value & CR0_PE_MASK))) {
            return false;
        }
        *normalized = (uint32_t)value;
        return true;
    case 2:
        /* CR2 contains the linear address of the most recent page fault. */
        *normalized = value;
        return true;
    case 3:
        if (!(env->efer & MSR_EFER_LMA)) {
            *normalized = (uint32_t)value;
            return true;
        }
        if (cpu->phys_bits < 12 || cpu->phys_bits > 52) {
            return false;
        }
        {
            uint64_t physical_mask =
                ((UINT64_C(1) << cpu->phys_bits) - 1) & ~UINT64_C(0xfff);
            uint64_t low_mask = (env->cr[4] & CR4_PCIDE_MASK) ?
                UINT64_C(0xfff) : UINT64_C(0x018);
            uint64_t noflush = UINT64_C(1) << 63;

            if ((value & noflush) && !(env->cr[4] & CR4_PCIDE_MASK)) {
                return false;
            }
            if (value & ~(physical_mask | low_mask | noflush)) {
                return false;
            }
            /* QEMU's software TLB has no PCID/no-flush tagging. */
            *normalized = value & ~noflush;
            return true;
        }
    case 4:
        if (value > UINT32_MAX ||
            (value & cr4_reserved_bits(env)) ||
            (((value ^ env->cr[4]) & CR4_LA57_MASK) &&
             (env->hflags & HF_CS64_MASK)) ||
            ((env->efer & MSR_EFER_LMA) && !(value & CR4_PAE_MASK)) ||
            ((value & CR4_PCIDE_MASK) &&
             (!(env->efer & MSR_EFER_LMA) ||
              !(env->cr[0] & CR0_PG_MASK) ||
              (!(env->cr[4] & CR4_PCIDE_MASK) &&
               (env->cr[3] & UINT64_C(0xfff)) != 0)))) {
            return false;
        }
        *normalized = (uint32_t)value;
        return true;
    default:
        return false;
    }
}

bool amdsvm_handle_cr_exit(CPUState *cs, uint64_t exit_code,
                           uint64_t exit_info1, uint64_t next_rip,
                           uint32_t *event_injection,
                           uint32_t *event_error_code, Error **errp)
{
    CPUX86State *env = &X86_CPU(cs)->env;
    X86CPU *cpu = X86_CPU(cs);
    struct x86_decode decode = { 0 };
    x86_insn_stream stream;
    uint8_t instruction[15];
    target_ulong linear_ip;
    target_ulong original_rip = env->eip;
    unsigned cr, reg;
    bool write;
    uint64_t value;
    uint64_t normalized;
    unsigned i;

    if (!event_injection || !event_error_code) {
        error_setg(errp, "AMD-SVM CR VMEXIT has no exception result storage");
        return false;
    }
    *event_injection = 0;
    *event_error_code = 0;

    if (!amdsvm_cr_exit_code(exit_code, &cr, &write)) {
        error_setg(errp, "AMD-SVM unsupported CR VMEXIT 0x%llx",
                   (unsigned long long)exit_code);
        return false;
    }
    if ((exit_info1 & ~(AMDVM_SVM_EXITINFO_CR_VALID |
                        AMDVM_SVM_EXITINFO_REG_MASK)) ||
        !(exit_info1 & AMDVM_SVM_EXITINFO_CR_VALID)) {
        error_setg(errp, "AMD-SVM CR VMEXIT lacks valid decode-assist data");
        return false;
    }
    reg = exit_info1 & AMDVM_SVM_EXITINFO_REG_MASK;

    if (!(env->hflags & HF_CS64_MASK)) {
        if (env->segs[R_CS].base > UINT64_MAX - env->eip) {
            error_setg(errp, "AMD-SVM CR VMEXIT instruction address overflowed");
            return false;
        }
        linear_ip = env->segs[R_CS].base + env->eip;
    } else {
        linear_ip = env->eip;
    }
    for (i = 0; i < sizeof(instruction); i++) {
        if (linear_ip > UINT64_MAX - i ||
            cpu_memory_rw_debug(cs, linear_ip + i, &instruction[i], 1, 0) != 0) {
            break;
        }
    }
    if (i < 3) {
        error_setg(errp, "AMD-SVM could not read the guest CR instruction bytes");
        return false;
    }
    stream.bytes = instruction;
    stream.len = i;
    if (!amdsvm_emulator_initialized ||
        decode_instruction_stream(env, &decode, &stream) == 0 ||
        decode.len <= 0 || decode.len > sizeof(instruction) ||
        decode.len > 15 || !decode.is_modrm || decode.modrm.mod != 3 ||
        decode.modrm.reg + (decode.rex.r << 3) != cr ||
        decode.modrm.rm + (decode.rex.b << 3) != reg ||
        (write && decode.cmd != X86_DECODE_CMD_MOV_TO_CR) ||
        (!write && decode.cmd != X86_DECODE_CMD_MOV_FROM_CR)) {
        error_setg(errp, "AMD-SVM CR VMEXIT does not match the decoded MOV CR instruction");
        return false;
    }
    if (!amdsvm_msr_next_rip_valid(env->eip, decode.len,
            (env->hflags & HF_CS64_MASK) != 0,
            (env->hflags & HF_CS32_MASK) != 0, next_rip)) {
        error_setg(errp, "AMD-SVM CR VMEXIT NRIP mismatch (RIP 0x%llx, length %d, NRIP 0x%llx)",
                   (unsigned long long)env->eip, decode.len,
                   (unsigned long long)next_rip);
        return false;
    }

    if (env->hflags & HF_CPL_MASK) {
        /* MOV CRx is privileged; the intercept does not waive guest CPL. */
        x86_emul_raise_exception(env, EXCP0D_GPF, 0);
        *event_injection = AMDVM_VCPU_EVENTINJ_VALID |
            AMDVM_VCPU_EVENTINJ_TYPE_EXCEPTION |
            AMDVM_VCPU_EVENTINJ_ERROR_VALID | AMDVM_VCPU_EXCEPTION_GP;
        env->exception_injected = 0;
        env->has_error_code = 0;
        env->exception_nr = -1;
        return true;
    }

    env->exception_injected = 0;
    env->has_error_code = 0;
    env->exception_nr = -1;
    if (!write) {
        switch (cr) {
        case 0: value = env->cr[0]; break;
        case 2: value = env->cr[2]; break;
        case 3: value = env->cr[3]; break;
        case 4: value = env->cr[4]; break;
        default: g_assert_not_reached();
        }
        if (!(env->hflags & HF_CS64_MASK)) {
            value = (uint32_t)value;
        }
        env->regs[amdsvm_cr_gpr_map[reg]] = value;
    } else {
        value = env->regs[amdsvm_cr_gpr_map[reg]];
        if (!(env->hflags & HF_CS64_MASK)) {
            value = (uint32_t)value;
        }
        if (!amdsvm_cr_write_valid(env, cpu, cr, value, &normalized)) {
            x86_emul_raise_exception(env, EXCP0D_GPF, 0);
            if (env->exception_nr != EXCP0D_GPF || !env->has_error_code ||
                env->error_code != 0 || env->eip != original_rip) {
                error_setg(errp, "AMD-SVM invalid CR write produced unexpected QEMU exception state");
                return false;
            }
            *event_injection = AMDVM_VCPU_EVENTINJ_VALID |
                AMDVM_VCPU_EVENTINJ_TYPE_EXCEPTION |
                AMDVM_VCPU_EVENTINJ_ERROR_VALID | AMDVM_VCPU_EXCEPTION_GP;
            env->exception_injected = 0;
            env->has_error_code = 0;
            env->exception_nr = -1;
            return true;
        }
        switch (cr) {
        case 0: cpu_x86_update_cr0(env, normalized); break;
        case 2: env->cr[2] = normalized; break;
        case 3: cpu_x86_update_cr3(env, normalized); break;
        case 4: cpu_x86_update_cr4(env, normalized); break;
        default: g_assert_not_reached();
        }
    }
    env->eip = next_rip;
    return true;
}

static bool amdsvm_dr_exit_decode(uint64_t exit_code, unsigned *dr,
                                  bool *write)
{
    if (exit_code >= SVM_EXIT_READ_DR0 &&
        exit_code <= SVM_EXIT_READ_DR0 + 7u) {
        *dr = exit_code - SVM_EXIT_READ_DR0;
        *write = false;
        return true;
    }
    if (exit_code >= SVM_EXIT_WRITE_DR0 &&
        exit_code <= SVM_EXIT_WRITE_DR0 + 7u) {
        *dr = exit_code - SVM_EXIT_WRITE_DR0;
        *write = true;
        return true;
    }
    return false;
}

bool amdsvm_handle_dr_exit(CPUState *cs, uint64_t exit_code,
                           uint64_t exit_info1, uint64_t next_rip,
                           Error **errp)
{
    CPUX86State *env = &X86_CPU(cs)->env;
    struct x86_decode decode = { 0 };
    x86_insn_stream stream;
    uint8_t instruction[15];
    target_ulong linear_ip, original_rip = env->eip;
    unsigned dr, gpr, actual_dr, i;
    bool write;
    uint64_t value;

    if (!amdsvm_dr_exit_decode(exit_code, &dr, &write) ||
        (exit_info1 & ~UINT64_C(0x0f)) != 0 || !amdsvm_emulator_initialized) {
        error_setg(errp, "AMD-SVM DR VMEXIT has invalid code or decode-assist register");
        return false;
    }
    gpr = (unsigned)(exit_info1 & 0x0fu);
    if (!(env->hflags & HF_CS64_MASK)) {
        if (env->segs[R_CS].base > UINT64_MAX - env->eip) {
            error_setg(errp, "AMD-SVM DR VMEXIT instruction address overflowed");
            return false;
        }
        linear_ip = env->segs[R_CS].base + env->eip;
    } else {
        linear_ip = env->eip;
    }
    for (i = 0; i < sizeof(instruction); i++) {
        if (linear_ip > UINT64_MAX - i ||
            cpu_memory_rw_debug(cs, linear_ip + i, &instruction[i], 1, 0) != 0) {
            break;
        }
    }
    if (i < 3) {
        error_setg(errp, "AMD-SVM could not read the guest DR instruction bytes");
        return false;
    }
    stream.bytes = instruction;
    stream.len = i;
    if (decode_instruction_stream(env, &decode, &stream) == 0 ||
        decode.len < 3 || decode.len > i || !decode.is_modrm ||
        decode.modrm.mod != 3 || decode.modrm.rm + (decode.rex.b << 3) != gpr ||
        decode.modrm.reg + (decode.rex.r << 3) != dr ||
        (write && decode.cmd != X86_DECODE_CMD_MOV_TO_DR) ||
        (!write && decode.cmd != X86_DECODE_CMD_MOV_FROM_DR) ||
        decode.rep != 0 || decode.lock || decode.addr_size_override) {
        error_setg(errp,
                   "AMD-SVM DR VMEXIT does not match decoded MOV DR (code=0x%llx info1=0x%llx rip=0x%llx nrip=0x%llx bytes=%02x:%02x:%02x)",
                   (unsigned long long)exit_code,
                   (unsigned long long)exit_info1,
                   (unsigned long long)original_rip,
                   (unsigned long long)next_rip,
                   instruction[0], instruction[1], instruction[2]);
        return false;
    }
    if (!amdsvm_msr_next_rip_valid(env->eip, decode.len,
            (env->hflags & HF_CS64_MASK) != 0,
            (env->hflags & HF_CS32_MASK) != 0, next_rip)) {
        error_setg(errp, "AMD-SVM DR VMEXIT NRIP mismatch");
        return false;
    }
    if (env->hflags & HF_CPL_MASK) {
        error_setg(errp, "AMD-SVM MOV DR at nonzero guest CPL is not emulated yet");
        return false;
    }
    if (dr == 4 || dr == 5) {
        if (env->cr[4] & CR4_DE_MASK) {
            error_setg(errp, "AMD-SVM MOV DR4/DR5 with CR4.DE set requires #UD injection");
            return false;
        }
        dr += 2;
    }
    actual_dr = dr;
    if (env->dr[7] & DR7_GD) {
        error_setg(errp, "AMD-SVM MOV DR with DR7.GD requires #DB injection");
        return false;
    }

    if (write) {
        value = env->regs[amdsvm_cr_gpr_map[gpr]];
        if (!(env->hflags & HF_CS64_MASK)) {
            value = (uint32_t)value;
        }
        if ((actual_dr == 6 || actual_dr == 7) &&
            (value & DR_RESERVED_MASK)) {
            error_setg(errp, "AMD-SVM MOV DR6/DR7 contains reserved high bits");
            return false;
        }
        if (actual_dr < 4 &&
            (env->dr[7] & (3u << (actual_dr * 2u)))) {
            error_setg(errp,
                       "AMD-SVM MOV DR0-DR3 while its hardware breakpoint is enabled is not emulated yet");
            return false;
        }
        switch (actual_dr) {
        case 0: case 1: case 2: case 3:
            env->dr[actual_dr] = value;
            break;
        case 6:
            env->dr[6] = value | DR6_FIXED_1;
            break;
        case 7:
            cpu_x86_update_dr7(env, (uint32_t)value);
            break;
        default:
            g_assert_not_reached();
        }
    } else {
        value = env->dr[actual_dr];
        if (!(env->hflags & HF_CS64_MASK)) {
            value = (uint32_t)value;
        }
        env->regs[amdsvm_cr_gpr_map[gpr]] = value;
    }
    env->eip = next_rip;
    return true;
}

int amdsvm_state_validate(const amdvm_x86_guest_state_v1_t *state)
{
    uint64_t xstate_bv;
    uint64_t xcomp_bv;
    unsigned i;

    if (!state || state->abi_version != AMDVM_X86_STATE_ABI_VERSION ||
        (state->interruptibility & ~(AMDVM_X86_INTERRUPTIBILITY_STI_SHADOW |
         AMDVM_X86_INTERRUPTIBILITY_NMI_BLOCKED |
         AMDVM_X86_INTERRUPTIBILITY_NMI_IRET_STEP)) ||
        ((state->interruptibility & AMDVM_X86_INTERRUPTIBILITY_NMI_IRET_STEP) &&
         !(state->interruptibility & AMDVM_X86_INTERRUPTIBILITY_NMI_BLOCKED)) ||
        state->xstate_reserved ||
        state->xstate_size != AMDVM_X86_XSTATE_MAX_SIZE ||
        (state->rflags & 2u) == 0 ||
        (state->cr[1] != 0) ||
        /* SVM is not exposed as a guest feature in this provider ABI. */
        (state->efer & AMDVM_EFER_SVME)) {
        return -EINVAL;
    }

    memcpy(&xstate_bv, &state->xstate[AMDVM_XSTATE_BV_OFFSET],
           sizeof(xstate_bv));
    memcpy(&xcomp_bv, &state->xstate[AMDVM_XCOMP_BV_OFFSET],
           sizeof(xcomp_bv));
    /* The migration image retains disabled components across XSETBV.
     * Execution XCR0 is validated separately; only x87/SSE/AVX are supported. */
    if ((xstate_bv & ~UINT64_C(7)) || (state->xcr0 & ~UINT64_C(7)) ||
        xcomp_bv != 0 ||
        (state->xcr0 & 1u) == 0 ||
        ((state->xcr0 & (1ull << 2)) &&
         !(state->xcr0 & (1ull << 1)))) {
        return -EINVAL;
    }
    for (i = 0; i < AMDVM_XSTATE_RESERVED_SIZE; i++) {
        if (state->xstate[AMDVM_XSTATE_RESERVED_OFFSET + i]) {
            return -EINVAL;
        }
    }
    return 0;
}

void amdsvm_state_get_mtrr(CPUState *cs, AMDVMMTRRState *state)
{
    X86CPU *cpu = X86_CPU(cs);
    CPUX86State *env = &cpu->env;

    memset(state, 0, sizeof(*state));
    memcpy(state->fixed, env->mtrr_fixed, sizeof(state->fixed));
    for (unsigned i = 0; i < AMDVM_MTRR_VARIABLE_COUNT; i++) {
        state->variable[i].base = env->mtrr_var[i].base;
        state->variable[i].mask = env->mtrr_var[i].mask;
    }
    state->default_type = env->mtrr_deftype;
    state->phys_bits = cpu->phys_bits;
    state->fixed_supported = (env->features[FEAT_1_EDX] & CPUID_MTRR) != 0;
}

uint64_t amdsvm_state_get_pat(CPUState *cs)
{
    return X86_CPU(cs)->env.pat;
}

int amdsvm_pending_guest_interrupts(CPUState *cs)
{
    int pending = qatomic_read(&cs->interrupt_request);

    if (pending & CPU_INTERRUPT_POLL) {
        cpu_reset_interrupt(cs, CPU_INTERRUPT_POLL);
        if (X86_CPU(cs)->apic_state) {
            apic_poll_irq(X86_CPU(cs)->apic_state);
        }
        pending = qatomic_read(&cs->interrupt_request);
    }

    /* HARD is supported below through VMCB EVENTINJ. */
    return pending & (CPU_INTERRUPT_INIT | CPU_INTERRUPT_SIPI |
                      CPU_INTERRUPT_TPR | CPU_INTERRUPT_SMI |
                      CPU_INTERRUPT_MCE | CPU_INTERRUPT_VIRQ);
}

unsigned amdsvm_state_process_async_events(CPUState *cs, bool trace)
{
    static unsigned trace_count;
    unsigned applied = 0;
    X86CPU *cpu = X86_CPU(cs);
    CPUX86State *env = &cpu->env;
    int before_irq = qatomic_read(&cs->interrupt_request);
    target_ulong before_rip = env->eip;
    uint16_t before_cs = env->segs[R_CS].selector;
    bool before_halted = cs->halted;

    /*
     * The provider state is synchronized into env before this helper runs.
     * Follow QEMU's KVM/HVF ordering so AP INIT/SIPI changes the architectural
     * state before the next AMDVM SET_STATE/VMRUN roundtrip.
     */
    if (cpu_test_interrupt(cs, CPU_INTERRUPT_INIT) &&
        !(env->hflags & HF_SMM_MASK)) {
        do_cpu_init(cpu);
        applied |= AMDSVM_ASYNC_INIT_APPLIED;
    }
    if (cpu_test_interrupt(cs, CPU_INTERRUPT_POLL)) {
        cpu_reset_interrupt(cs, CPU_INTERRUPT_POLL);
        if (cpu->apic_state) {
            apic_poll_irq(cpu->apic_state);
        }
    }
    if ((cpu_test_interrupt(cs, CPU_INTERRUPT_HARD) &&
         x86_cpu_interrupts_enabled(env)) ||
        cpu_test_interrupt(cs, CPU_INTERRUPT_NMI)) {
        cs->halted = false;
    }
    if (cpu_test_interrupt(cs, CPU_INTERRUPT_SIPI)) {
        bool was_halted = cs->halted;

        cpu_reset_interrupt(cs, CPU_INTERRUPT_SIPI);
        do_cpu_sipi(cpu);
        if (was_halted && !cs->halted) {
            applied |= AMDSVM_ASYNC_SIPI_STARTED;
        }
    }
    if (cpu_test_interrupt(cs, CPU_INTERRUPT_TPR)) {
        cpu_reset_interrupt(cs, CPU_INTERRUPT_TPR);
        if (cpu->apic_state) {
            apic_handle_tpr_access_report(cpu->apic_state, env->eip,
                                          env->tpr_access_type);
        }
    }
    if (trace && (applied || trace_count < 64)) {
        int after_irq = qatomic_read(&cs->interrupt_request);

        if (before_irq != after_irq || before_rip != env->eip ||
            before_cs != env->segs[R_CS].selector ||
            before_halted != cs->halted) {
            trace_count++;
            error_report("AMDVM_ASYNC_EVENT vcpu=%d before_irq=0x%x after_irq=0x%x rip_before=0x%llx rip_after=0x%llx cs_before=%04x cs_after=%04x halted_before=%u halted_after=%u",
                         cs->cpu_index, before_irq, after_irq,
                         (unsigned long long)before_rip,
                         (unsigned long long)env->eip, before_cs,
                         env->segs[R_CS].selector, before_halted,
                         cs->halted);
        }
        if (applied) {
            error_report("AMDVM_AP_START_STATE vcpu=%d apic_id=%u applied=0x%x cs=%04x base=0x%llx limit=0x%x attrib=0x%x rip=0x%llx cr0=0x%llx cr3=0x%llx cr4=0x%llx efer=0x%llx rflags=0x%llx halted=%u",
                         cs->cpu_index, amdsvm_state_apic_id(cs), applied,
                         env->segs[R_CS].selector,
                         (unsigned long long)env->segs[R_CS].base,
                         env->segs[R_CS].limit,
                         amdsvm_segment_attributes_from_qemu_flags(
                             env->segs[R_CS].flags),
                         (unsigned long long)env->eip,
                         (unsigned long long)env->cr[0],
                         (unsigned long long)env->cr[3],
                         (unsigned long long)env->cr[4],
                         (unsigned long long)env->efer,
                         (unsigned long long)env->eflags, cs->halted);
        }
    }
    return applied;
}

void amdsvm_prepare_irq_injection(CPUState *cs,
                                  AMDVMEventState *pending_events,
                                  amdvm_vcpu_run_request_t *run)
{
    CPUX86State *env = &X86_CPU(cs)->env;

    if (!pending_events || !run) {
        return;
    }
    /* APIC updates from the I/O thread request POLL, not HARD. After HLT
     * there is no physical VMEXIT to run the usual poll handler: resolve it
     * on this vCPU before deciding whether a guest IRQ can wake the provider. */
    if (cpu_test_interrupt(cs, CPU_INTERRUPT_POLL)) {
        cpu_reset_interrupt(cs, CPU_INTERRUPT_POLL);
        if (X86_CPU(cs)->apic_state) {
            apic_poll_irq(X86_CPU(cs)->apic_state);
        }
    }
    if (!pending_events->exception_pending && !pending_events->irq_pending &&
        !pending_events->nmi_pending &&
        !(env->hflags2 & HF2_NMI_MASK) &&
        cpu_test_interrupt(cs, CPU_INTERRUPT_NMI)) {
        if (amdsvm_event_queue_nmi(pending_events) == 0) {
            cpu_reset_interrupt(cs, CPU_INTERRUPT_NMI);
        }
    }
    if (!pending_events->exception_pending && !pending_events->nmi_pending &&
        !pending_events->irq_pending &&
        x86_cpu_interrupts_enabled(env) &&
        cpu_test_interrupt(cs, CPU_INTERRUPT_HARD)) {
        int vector = cpu_get_pic_interrupt(env);

        /* Match QEMU's HVF path: acknowledge once, then retain until consumed. */
        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
        if (vector >= 0 && vector <= 255) {
            pending_events->irq_vector = (uint8_t)vector;
            pending_events->irq_pending = true;
        }
    }
    amdsvm_event_prepare(pending_events, run);
}

static const enum X86Seg amdsvm_segment_map[AMDVM_X86_SEGMENT_COUNT] = {
    [AMDVM_X86_SEG_ES] = R_ES,
    [AMDVM_X86_SEG_CS] = R_CS,
    [AMDVM_X86_SEG_SS] = R_SS,
    [AMDVM_X86_SEG_DS] = R_DS,
    [AMDVM_X86_SEG_FS] = R_FS,
    [AMDVM_X86_SEG_GS] = R_GS,
};

uint16_t amdsvm_segment_attributes_from_qemu_flags(uint32_t flags)
{
    return ((flags & DESC_TYPE_MASK) >> DESC_TYPE_SHIFT) |
           ((flags & DESC_S_MASK) >> (DESC_S_SHIFT - 4)) |
           ((flags & DESC_DPL_MASK) >> (DESC_DPL_SHIFT - 5)) |
           ((flags & DESC_P_MASK) >> (DESC_P_SHIFT - 7)) |
           ((flags & DESC_AVL_MASK) >> (DESC_AVL_SHIFT - 8)) |
           ((flags & DESC_L_MASK) >> (DESC_L_SHIFT - 9)) |
           ((flags & DESC_B_MASK) >> (DESC_B_SHIFT - 10)) |
           ((flags & DESC_G_MASK) >> (DESC_G_SHIFT - 11));
}

uint32_t amdsvm_segment_flags_from_attributes(uint16_t attributes)
{
    return ((attributes & 0x000fu) << DESC_TYPE_SHIFT) |
           ((attributes & 0x0010u) << (DESC_S_SHIFT - 4)) |
           ((attributes & 0x0060u) << (DESC_DPL_SHIFT - 5)) |
           ((attributes & 0x0080u) << (DESC_P_SHIFT - 7)) |
           ((attributes & 0x0100u) << (DESC_AVL_SHIFT - 8)) |
           ((attributes & 0x0200u) << (DESC_L_SHIFT - 9)) |
           ((attributes & 0x0400u) << (DESC_B_SHIFT - 10)) |
           ((attributes & 0x0800u) << (DESC_G_SHIFT - 11));
}

static void amdsvm_segment_from_qemu(amdvm_x86_segment_state_t *dst,
                                     const SegmentCache *src)
{
    dst->selector = src->selector;
    dst->attributes = amdsvm_segment_attributes_from_qemu_flags(src->flags);
    dst->limit = src->limit;
    dst->base = src->base;
}

static void amdsvm_segment_to_qemu(SegmentCache *dst,
                                   const amdvm_x86_segment_state_t *src)
{
    dst->selector = src->selector;
    dst->flags = amdsvm_segment_flags_from_attributes(src->attributes);
    dst->limit = src->limit;
    dst->base = src->base;
}

int amdsvm_state_load(CPUState *cs, amdvm_x86_guest_state_v1_t *state)
{
    X86CPU *cpu = X86_CPU(cs);
    CPUX86State *env = &cpu->env;
    unsigned i;

    memset(state, 0, sizeof(*state));
    state->abi_version = AMDVM_X86_STATE_ABI_VERSION;
    for (i = 0; i < AMDVM_X86_GPR_COUNT; i++) {
        state->gpr[i] = env->regs[i];
    }
    state->rip = env->eip;
    state->rflags = env->eflags;
    state->interruptibility = (env->hflags & HF_INHIBIT_IRQ_MASK) ?
        AMDVM_X86_INTERRUPTIBILITY_STI_SHADOW : 0;
    if (env->hflags2 & HF2_NMI_MASK) {
        state->interruptibility |= AMDVM_X86_INTERRUPTIBILITY_NMI_BLOCKED;
    }
    for (i = 0; i < 6; i++) {
        amdsvm_segment_from_qemu(&state->segments[i],
                                 &env->segs[amdsvm_segment_map[i]]);
    }
    amdsvm_segment_from_qemu(&state->segments[AMDVM_X86_SEG_GDTR], &env->gdt);
    amdsvm_segment_from_qemu(&state->segments[AMDVM_X86_SEG_LDTR], &env->ldt);
    amdsvm_segment_from_qemu(&state->segments[AMDVM_X86_SEG_IDTR], &env->idt);
    amdsvm_segment_from_qemu(&state->segments[AMDVM_X86_SEG_TR], &env->tr);
    for (i = 0; i < ARRAY_SIZE(state->cr); i++) {
        state->cr[i] = env->cr[i];
    }
    state->efer = env->efer;
    for (i = 0; i < ARRAY_SIZE(state->dr); i++) {
        state->dr[i] = env->dr[i];
    }
    state->pat = env->pat;
    state->xcr0 = env->xcr0;
    state->xstate_size = AMDVM_X86_XSTATE_MAX_SIZE;
    x86_cpu_xsave_all_areas(cpu, state->xstate, state->xstate_size);
    state->sysenter_cs = env->sysenter_cs;
    state->sysenter_esp = env->sysenter_esp;
    state->sysenter_eip = env->sysenter_eip;
    state->star = env->star;
#ifdef TARGET_X86_64
    state->lstar = env->lstar;
    state->cstar = env->cstar;
    state->sfmask = env->fmask;
    state->kernel_gs_base = env->kernelgsbase;
#endif
    return amdsvm_state_validate(state);
}

int amdsvm_state_store(CPUState *cs,
                       const amdvm_x86_guest_state_v1_t *state)
{
    X86CPU *cpu = X86_CPU(cs);
    CPUX86State *env = &cpu->env;
    unsigned i;

    if (amdsvm_state_validate(state) != 0) {
        return -EINVAL;
    }
    for (i = 0; i < AMDVM_X86_GPR_COUNT; i++) {
        env->regs[i] = state->gpr[i];
    }
    env->eip = state->rip;
    env->eflags = state->rflags;
    if (state->interruptibility & AMDVM_X86_INTERRUPTIBILITY_STI_SHADOW) {
        env->hflags |= HF_INHIBIT_IRQ_MASK;
    } else {
        env->hflags &= ~HF_INHIBIT_IRQ_MASK;
    }
    if (state->interruptibility & AMDVM_X86_INTERRUPTIBILITY_NMI_BLOCKED) {
        env->hflags2 |= HF2_NMI_MASK;
    } else {
        env->hflags2 &= ~HF2_NMI_MASK;
    }
    for (i = 0; i < 6; i++) {
        amdsvm_segment_to_qemu(&env->segs[amdsvm_segment_map[i]],
                               &state->segments[i]);
    }
    amdsvm_segment_to_qemu(&env->gdt, &state->segments[AMDVM_X86_SEG_GDTR]);
    amdsvm_segment_to_qemu(&env->ldt, &state->segments[AMDVM_X86_SEG_LDTR]);
    amdsvm_segment_to_qemu(&env->idt, &state->segments[AMDVM_X86_SEG_IDTR]);
    amdsvm_segment_to_qemu(&env->tr, &state->segments[AMDVM_X86_SEG_TR]);
    for (i = 0; i < ARRAY_SIZE(state->cr); i++) {
        env->cr[i] = state->cr[i];
    }
    env->efer = state->efer;
    for (i = 0; i < ARRAY_SIZE(state->dr); i++) {
        env->dr[i] = state->dr[i];
    }
    env->pat = state->pat;
    env->xcr0 = state->xcr0;
    x86_cpu_xrstor_all_areas(cpu, state->xstate, state->xstate_size);
    env->sysenter_cs = state->sysenter_cs;
    env->sysenter_esp = state->sysenter_esp;
    env->sysenter_eip = state->sysenter_eip;
    env->star = state->star;
#ifdef TARGET_X86_64
    env->lstar = state->lstar;
    env->cstar = state->cstar;
    env->fmask = state->sfmask;
    env->kernelgsbase = state->kernel_gs_base;
#endif
    /* Segment and control state updates must refresh QEMU's derived mode flags. */
    x86_update_hflags(env);
    return 0;
}

int amdsvm_state_read_tsc(CPUState *cs, uint64_t *value)
{
    CPUX86State *env;

    if (!cs || !value) {
        return -EINVAL;
    }
    env = &X86_CPU(cs)->env;
    *value = cpu_get_tsc(env) + env->tsc_offset;
    return 0;
}

void amdsvm_emulate_cpuid(CPUState *cs, uint32_t leaf, uint32_t subleaf,
                          uint32_t *eax, uint32_t *ebx, uint32_t *ecx,
                          uint32_t *edx)
{
    cpu_x86_cpuid(&X86_CPU(cs)->env, leaf, subleaf,
                  eax, ebx, ecx, edx);
    /* This provider does not implement nested SVM for the guest. */
    if (leaf == 0x80000001u) {
        *ecx &= ~CPUID_EXT3_SVM;
    } else if (leaf == 0x8000000au) {
        *eax = *ebx = *ecx = *edx = 0;
    }
}

/* Disabled component contents survive XSETBV; provider restores host XCR0. */
int amdsvm_handle_xsetbv_exit(CPUState *cs, uint64_t next_rip,
                             uint32_t *event_injection)
{
    CPUX86State *env = &X86_CPU(cs)->env;
    uint64_t value = ((uint64_t)(uint32_t)env->regs[R_EDX] << 32) |
                     (uint32_t)env->regs[R_EAX];
    uint64_t supported = ((uint64_t)env->features[FEAT_XSAVE_XCR0_HI] << 32) |
                         env->features[FEAT_XSAVE_XCR0_LO];

    *event_injection = 0;
    /* Hardware delivers #UD before the intercept if OSXSAVE is clear. */
    if (!(env->cr[4] & CR4_OSXSAVE_MASK)) {
        return -EINVAL;
    }
    if (!amdsvm_xsetbv_value_valid(value, (uint32_t)env->regs[R_ECX],
                                   env->segs[R_CS].selector & 3, supported)) {
        *event_injection = AMDVM_VCPU_EVENTINJ_VALID |
                           AMDVM_VCPU_EVENTINJ_TYPE_EXCEPTION |
                           AMDVM_VCPU_EVENTINJ_ERROR_VALID |
                           AMDVM_VCPU_EXCEPTION_GP;
        return 0;
    }
    env->xcr0 = value;
    env->eip = next_rip;
    return 0;
}
