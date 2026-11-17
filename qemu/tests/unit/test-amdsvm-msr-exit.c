/* Exercise the production AMD-SVM MSR VMEXIT decoder and emulator callbacks. */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/target-info.h"
#include "qemu/memalign.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev.h"
#include "hw/i386/apic.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "target/i386/cpu.h"
#include "target/i386/svm.h"
#include "target/i386/cpu-qom.h"
#include "target/i386/amdsvm/amdsvm-state.h"
#include "target/i386/emulate/x86_mmu.h"
#include "target/i386/emulate/x86_decode.h"
#include "../../../svm-kext/provider/amdvm_vmcb.h"
#include "../../../svm-kext/provider/amdvm_vmrun.h"
#include "../../../svm-kext/provider/amdvm_xstate.h"

ExtSaveArea x86_ext_save_areas[XSAVE_STATE_AREA_COUNT] = {
    [XSTATE_FP_BIT] = { .offset = 0, .size = 512 },
    [XSTATE_YMM_BIT] = { .offset = 576, .size = 256 },
};

bool target_big_endian(void)
{
    return false;
}

/* Offline model of the host XCR0 check performed by the privileged provider. */
amdvm_vmrun_status_t amdvm_vmrun_validate_guest_xstate(const void *area,
                                                       size_t size,
                                                       uint64_t xcr0)
{
    if ((xcr0 & ~UINT64_C(7)) ||
        amdvm_xstate_validate_standard(area, size, xcr0) != 0) {
        return AMDVM_VMRUN_INVALID_ARGUMENT;
    }
    return AMDVM_VMRUN_SUCCESS;
}

static uint8_t guest_code[16];
static X86CPU *test_cpu;
static unsigned cr3_update_calls;

/* These paths are outside the offline decoder/state-mapping tests. */
AddressSpace address_space_io;

MemTxResult address_space_rw(const AddressSpace *as, hwaddr addr,
                             MemTxAttrs attrs, void *buf,
                             hwaddr len, bool is_write)
{
    (void)as;
    (void)addr;
    (void)attrs;
    (void)buf;
    (void)len;
    (void)is_write;
    g_assert_not_reached();
    return MEMTX_ERROR;
}

void x86_update_hflags(CPUX86State *env)
{
    env->hflags &= ~(HF_CS32_MASK | HF_SS32_MASK);
    env->hflags |= (env->segs[R_CS].flags & DESC_B_MASK) >>
                   (DESC_B_SHIFT - HF_CS32_SHIFT);
    env->hflags |= (env->segs[R_SS].flags & DESC_B_MASK) >>
                   (DESC_B_SHIFT - HF_SS32_SHIFT);
}

/* The CR-exit unit tests verify validation and dispatch, not TLB internals. */
void cpu_x86_update_cr0(CPUX86State *env, uint32_t value)
{
    env->cr[0] = value | CR0_ET_MASK;
}

void cpu_x86_update_cr3(CPUX86State *env, target_ulong value)
{
    cr3_update_calls++;
    env->cr[3] = value;
}

void cpu_x86_update_cr4(CPUX86State *env, uint32_t value)
{
    env->cr[4] = value;
}

uint8_t cpu_get_apic_tpr(APICCommonState *apic)
{
    (void)apic;
    return 0;
}

void cpu_set_apic_tpr(APICCommonState *apic, uint8_t value)
{
    (void)apic;
    (void)value;
}

uint64_t cpu_get_tsc(CPUX86State *env)
{
    (void)env;
    return 0;
}

void cpu_x86_cpuid(CPUX86State *env, uint32_t index, uint32_t count,
                   uint32_t *eax, uint32_t *ebx,
                   uint32_t *ecx, uint32_t *edx)
{
    (void)env;
    (void)index;
    (void)count;
    *eax = *ebx = *ecx = *edx = 0;
}

static void register_test_cpu(void)
{
    static const TypeInfo cpu_type_info = {
        .name = TYPE_CPU,
        .parent = TYPE_DEVICE,
        .instance_size = sizeof(CPUState),
        .class_size = sizeof(CPUClass),
    };
    static const TypeInfo type_info = {
        .name = TYPE_X86_CPU,
        .parent = TYPE_CPU,
        .instance_size = sizeof(X86CPU),
        .class_size = sizeof(X86CPUClass),
    };

    module_call_init(MODULE_INIT_QOM);
    type_register_static(&cpu_type_info);
    type_register_static(&type_info);
    test_cpu = X86_CPU(object_new(TYPE_X86_CPU));
    test_cpu->phys_bits = 48;
    test_cpu->env.hflags = HF_CS32_MASK | HF_CS64_MASK;
}

int cpu_memory_rw_debug(CPUState *cpu, vaddr addr, void *buf,
                        size_t len, bool is_write)
{
    (void)cpu;
    if (is_write || addr > sizeof(guest_code) ||
        (uint64_t)len > sizeof(guest_code) - addr) {
        return -1;
    }
    memcpy(buf, guest_code + addr, len);
    return 0;
}

MMUTranslateResult x86_read_mem(CPUState *cpu, void *data,
                                target_ulong addr, int bytes)
{
    (void)cpu;
    (void)data;
    (void)addr;
    (void)bytes;
    g_assert_not_reached();
}

MMUTranslateResult x86_write_mem(CPUState *cpu, void *data,
                                 target_ulong addr, int bytes)
{
    (void)cpu;
    (void)data;
    (void)addr;
    (void)bytes;
    g_assert_not_reached();
}

MMUTranslateResult x86_read_mem_priv(CPUState *cpu, void *data,
                                     target_ulong addr, int bytes)
{
    return x86_read_mem(cpu, data, addr, bytes);
}

MMUTranslateResult x86_write_mem_priv(CPUState *cpu, void *data,
                                      target_ulong addr, int bytes)
{
    return x86_write_mem(cpu, data, addr, bytes);
}

void cpu_load_efer(CPUX86State *env, uint64_t value)
{
    env->efer = value;
    env->hflags &= ~(HF_LMA_MASK | HF_SVME_MASK);
    if (value & MSR_EFER_LMA) {
        env->hflags |= HF_LMA_MASK;
    }
    if (value & MSR_EFER_SVME) {
        env->hflags |= HF_SVME_MASK;
    }
}

uint64_t cpu_get_apic_base(APICCommonState *apic)
{
    (void)apic;
    g_assert_not_reached();
}

int cpu_set_apic_base(APICCommonState *apic, uint64_t value)
{
    (void)apic;
    (void)value;
    g_assert_not_reached();
}

static void reset_instruction(uint8_t opcode0, uint8_t opcode1,
                              uint64_t msr, uint64_t rip)
{
    CPUX86State *env = &test_cpu->env;

    memset(guest_code, 0x90, sizeof(guest_code));
    guest_code[0] = opcode0;
    guest_code[1] = opcode1;
    memset(env->regs, 0, sizeof(env->regs));
    env->regs[R_ECX] = msr;
    env->eip = rip;
    env->exception_injected = 0;
    env->has_error_code = 0;
    env->exception_nr = -1;
}

static void test_rdmsr_uses_qemu_state(void)
{
    CPUX86State *env = &test_cpu->env;
    uint32_t event = UINT32_MAX;
    uint32_t error_code = UINT32_MAX;
    Error *err = NULL;

    reset_instruction(0x0f, 0x32, MSR_EFER, 0);
    env->efer = MSR_EFER_LME | MSR_EFER_NXE;
    g_assert_true(amdsvm_handle_msr_exit(CPU(test_cpu), 0, 2,
                                         &event, &error_code, &err));
    g_assert_null(err);
    g_assert_cmpuint(env->regs[R_EAX], ==,
                     (uint32_t)(MSR_EFER_LME | MSR_EFER_NXE));
    g_assert_cmpuint(env->regs[R_EDX], ==, 0);
    g_assert_cmpuint(env->eip, ==, 2);
    g_assert_cmpuint(event, ==, 0);
    g_assert_cmpuint(error_code, ==, 0);
}

static void test_wrmsr_mutates_qemu_state(void)
{
    CPUX86State *env = &test_cpu->env;
    uint32_t event = 0;
    uint32_t error_code = 0;
    Error *err = NULL;

    reset_instruction(0x0f, 0x30, MSR_IA32_SYSENTER_CS, 0);
    env->regs[R_EAX] = 0x12345678;
    env->regs[R_EDX] = 0xabcdef01;
    g_assert_true(amdsvm_handle_msr_exit(CPU(test_cpu), 1, 2,
                                         &event, &error_code, &err));
    g_assert_null(err);
    g_assert_cmpuint(env->sysenter_cs, ==, 0x5678);
    g_assert_cmpuint(env->eip, ==, 2);
    g_assert_cmpuint(event, ==, 0);
}

static void test_direction_and_nrip_fail_closed(void)
{
    uint32_t event = 0;
    uint32_t error_code = 0;
    Error *err = NULL;

    reset_instruction(0x0f, 0x30, MSR_IA32_SYSENTER_CS, 0);
    g_assert_false(amdsvm_handle_msr_exit(CPU(test_cpu), 0, 2,
                                          &event, &error_code, &err));
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_cmpuint(test_cpu->env.eip, ==, 0);

    g_assert_false(amdsvm_handle_msr_exit(CPU(test_cpu), 1, 3,
                                          &event, &error_code, &err));
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmpuint(test_cpu->env.eip, ==, 0);
}

static void test_unknown_msr_reads_zero_like_tcg(void)
{
    uint32_t event = 0;
    uint32_t error_code = UINT32_MAX;
    Error *err = NULL;

    reset_instruction(0x0f, 0x32, UINT32_C(0xdeadbeef), 0);
    test_cpu->env.regs[R_EAX] = UINT32_MAX;
    test_cpu->env.regs[R_EDX] = UINT32_MAX;
    g_assert_true(amdsvm_handle_msr_exit(CPU(test_cpu), 0, 2,
                                         &event, &error_code, &err));
    g_assert_null(err);
    g_assert_cmpuint(event, ==, 0);
    g_assert_cmpuint(error_code, ==, 0);
    g_assert_cmpuint(test_cpu->env.regs[R_EAX], ==, 0);
    g_assert_cmpuint(test_cpu->env.regs[R_EDX], ==, 0);
    g_assert_cmpuint(test_cpu->env.eip, ==, 2);
}

static void reset_cr_instruction(const uint8_t *bytes, size_t len)
{
    CPUX86State *env = &test_cpu->env;

    memset(guest_code, 0x90, sizeof(guest_code));
    memcpy(guest_code, bytes, len);
    memset(env->regs, 0, sizeof(env->regs));
    env->eip = 0;
    env->segs[R_CS].base = 0;
    env->hflags = HF_CS64_MASK | HF_LMA_MASK;
    /* QEMU's x86_is_long_mode() requires both LME and LMA. */
    env->efer = MSR_EFER_LME | MSR_EFER_LMA;
    memset(env->cr, 0, sizeof(env->cr));
    env->cr[0] = CR0_ET_MASK | CR0_PE_MASK | CR0_PG_MASK;
    env->cr[3] = UINT64_C(0x1000);
    env->cr[4] = CR4_PAE_MASK;
    env->exception_injected = 0;
    env->has_error_code = 0;
    env->exception_nr = -1;
}

static void test_mov_from_cr0_uses_guest_state(void)
{
    static const uint8_t code[] = { 0x0f, 0x20, 0xc0 }; /* mov rax, cr0 */
    uint32_t event = 0;
    uint32_t error_code = 0;
    Error *err = NULL;

    reset_cr_instruction(code, sizeof(code));
    test_cpu->env.cr[0] = CR0_ET_MASK | CR0_PE_MASK | CR0_PG_MASK;
    g_assert_true(amdsvm_handle_cr_exit(CPU(test_cpu), SVM_EXIT_READ_CR0,
        (UINT64_C(1) << 63) | 0, 3, &event, &error_code, &err));
    g_assert_null(err);
    g_assert_cmpuint(test_cpu->env.regs[R_EAX], ==, test_cpu->env.cr[0]);
    g_assert_cmpuint(test_cpu->env.eip, ==, 3);
    g_assert_cmpuint(event, ==, 0);
}

static void test_mov_to_cr3_from_extended_gpr(void)
{
    static const uint8_t code[] = { 0x41, 0x0f, 0x22, 0xd8 }; /* mov cr3, r8 */
    uint32_t event = 0;
    uint32_t error_code = 0;
    Error *err = NULL;

    reset_cr_instruction(code, sizeof(code));
    test_cpu->env.regs[R_R8] = 0x12345000;
    g_assert_true(amdsvm_handle_cr_exit(CPU(test_cpu), SVM_EXIT_WRITE_CR3,
        (UINT64_C(1) << 63) | 8, 4, &event, &error_code, &err));
    g_assert_null(err);
    g_assert_cmpuint(test_cpu->env.cr[3], ==, 0x12345000);
    g_assert_cmpuint(test_cpu->env.eip, ==, 4);
    g_assert_cmpuint(event, ==, 0);
}

static void test_invalid_cr0_write_injects_gp_without_advancing(void)
{
    static const uint8_t code[] = { 0x0f, 0x22, 0xc0 }; /* mov cr0, rax */
    uint32_t event = 0;
    uint32_t error_code = 0;
    Error *err = NULL;

    reset_cr_instruction(code, sizeof(code));
    test_cpu->env.cr[0] = CR0_ET_MASK | CR0_PE_MASK | CR0_PG_MASK;
    test_cpu->env.regs[R_EAX] = CR0_NW_MASK; /* NW without CD is invalid. */
    g_assert_true(amdsvm_handle_cr_exit(CPU(test_cpu), SVM_EXIT_WRITE_CR0,
        (UINT64_C(1) << 63) | 0, 3, &event, &error_code, &err));
    g_assert_null(err);
    g_assert_cmpuint(test_cpu->env.cr[0], ==,
                     CR0_ET_MASK | CR0_PE_MASK | CR0_PG_MASK);
    g_assert_cmpuint(test_cpu->env.eip, ==, 0);
    g_assert_cmpuint(event, ==, AMDVM_VCPU_EVENTINJ_VALID |
        AMDVM_VCPU_EVENTINJ_TYPE_EXCEPTION |
        AMDVM_VCPU_EVENTINJ_ERROR_VALID | AMDVM_VCPU_EXCEPTION_GP);
    g_assert_cmpuint(error_code, ==, 0);
}

static void test_cr_exit_rejects_missing_decode_assist(void)
{
    static const uint8_t code[] = { 0x0f, 0x20, 0xc0 };
    uint32_t event = 0;
    uint32_t error_code = 0;
    Error *err = NULL;

    reset_cr_instruction(code, sizeof(code));
    g_assert_false(amdsvm_handle_cr_exit(CPU(test_cpu), SVM_EXIT_READ_CR0,
        0, 3, &event, &error_code, &err));
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_cmpuint(test_cpu->env.eip, ==, 0);
}

static void test_cr4_pcide_requires_paging(void)
{
    static const uint8_t code[] = { 0x0f, 0x22, 0xe0 }; /* mov cr4, rax */
    uint32_t event = 0;
    uint32_t error_code = 0;
    Error *err = NULL;

    reset_cr_instruction(code, sizeof(code));
    /* Isolate the paging prerequisite checked by the PCIDE transition. */
    test_cpu->env.cr[0] &= ~CR0_PG_MASK;
    test_cpu->env.regs[R_EAX] = CR4_PCIDE_MASK;
    g_assert_true(amdsvm_handle_cr_exit(CPU(test_cpu), SVM_EXIT_WRITE_CR4,
        (UINT64_C(1) << 63) | 0, 3, &event, &error_code, &err));
    g_assert_null(err);
    g_assert_cmpuint(test_cpu->env.cr[4], ==, CR4_PAE_MASK);
    g_assert_cmpuint(test_cpu->env.eip, ==, 0);
    g_assert_cmpuint(event, ==, AMDVM_VCPU_EVENTINJ_VALID |
        AMDVM_VCPU_EVENTINJ_TYPE_EXCEPTION |
        AMDVM_VCPU_EVENTINJ_ERROR_VALID | AMDVM_VCPU_EXCEPTION_GP);
}

static void test_qemu_guest_state_xstate_roundtrip(void)
{
    CPUX86State *env = &test_cpu->env;
    amdvm_x86_guest_state_v1_t state;
    amdvm_vmcb_t *vmcb;
    uint64_t gprs[15] = { 0 };
    void *guest_xstate;
    uint64_t expected_xmm0_low = UINT64_C(0x123456789abcdef0);
    uint64_t expected_xmm0_high = UINT64_C(0x0fedcba987654321);
    uint64_t expected_ymm0_low = UINT64_C(0xfeedfacecafebeef);
    uint64_t expected_ymm0_high = UINT64_C(0x0123456789abcdef);
    uint64_t guest_xmm0_low = UINT64_C(0x1111222233334444);
    uint64_t guest_ymm0_low = UINT64_C(0xaaaabbbbccccdddd);

    memset(env, 0, sizeof(*env));
    env->regs[R_EAX] = UINT64_C(0x1122334455667788);
    env->eip = 0x1000;
    env->eflags = 2;
    env->cr[0] = CR0_ET_MASK | CR0_PE_MASK;
    env->cr[4] = CR4_PAE_MASK;
    env->efer = MSR_EFER_LME | MSR_EFER_LMA;
    env->pat = UINT64_C(0x0007040600070406);
    env->xcr0 = 7;
    env->xstate_bv = 7;
    env->mxcsr = 0x1f80;
    env->xmm_regs[0].ZMM_Q(0) = expected_xmm0_low;
    env->xmm_regs[0].ZMM_Q(1) = expected_xmm0_high;
    env->xmm_regs[0].ZMM_Q(2) = expected_ymm0_low;
    env->xmm_regs[0].ZMM_Q(3) = expected_ymm0_high;

    g_assert_cmpint(amdsvm_state_load(CPU(test_cpu), &state), ==, 0);
    g_assert_cmpuint(state.gpr[0], ==, env->regs[R_EAX]);
    g_assert_cmpuint(state.xcr0, ==, 7);
    g_assert_cmpuint(state.xstate[512], ==, 7);

    /* Exercise the real QEMU ABI encoder and provider mapper in one chain. */
    vmcb = qemu_memalign(AMDVM_PAGE_SIZE, sizeof(*vmcb));
    guest_xstate = qemu_memalign(AMDVM_PAGE_SIZE, AMDVM_X86_XSTATE_MAX_SIZE);
    g_assert_nonnull(vmcb);
    g_assert_nonnull(guest_xstate);
    amdvm_vmcb_clear(vmcb);
    g_assert_cmpint(amdvm_vmcb_load_x86_state_v1(vmcb, &state, gprs,
                                                 guest_xstate,
                                                 AMDVM_X86_XSTATE_MAX_SIZE),
                    ==, 0);
    g_assert_cmpmem(guest_xstate, AMDVM_X86_XSTATE_MAX_SIZE,
                    state.xstate, AMDVM_X86_XSTATE_MAX_SIZE);

    memcpy((uint8_t *)guest_xstate + 160, &guest_xmm0_low,
           sizeof(guest_xmm0_low));
    memcpy((uint8_t *)guest_xstate + 576, &guest_ymm0_low,
           sizeof(guest_ymm0_low));
    amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_RAX,
                       UINT64_C(0x8877665544332211));
    amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_RIP, 0x1234);
    g_assert_cmpint(amdvm_vmcb_store_x86_state_v1(vmcb, &state, gprs,
                                                  guest_xstate,
                                                  AMDVM_X86_XSTATE_MAX_SIZE),
                    ==, 0);
    g_assert_cmpint(amdsvm_state_validate(&state), ==, 0);
    qemu_vfree(guest_xstate);
    qemu_vfree(vmcb);

    env->regs[R_EAX] = 0;
    env->eip = 0;
    env->xmm_regs[0].ZMM_Q(0) = 0;
    env->xmm_regs[0].ZMM_Q(1) = 0;
    env->xmm_regs[0].ZMM_Q(2) = 0;
    env->xmm_regs[0].ZMM_Q(3) = 0;
    g_assert_cmpint(amdsvm_state_store(CPU(test_cpu), &state), ==, 0);
    g_assert_cmpuint(env->regs[R_EAX], ==,
                     UINT64_C(0x8877665544332211));
    g_assert_cmpuint(env->eip, ==, 0x1234);
    g_assert_cmpuint(env->xmm_regs[0].ZMM_Q(0), ==, guest_xmm0_low);
    g_assert_cmpuint(env->xmm_regs[0].ZMM_Q(1), ==, expected_xmm0_high);
    g_assert_cmpuint(env->xmm_regs[0].ZMM_Q(2), ==, guest_ymm0_low);
    g_assert_cmpuint(env->xmm_regs[0].ZMM_Q(3), ==, expected_ymm0_high);
}

static void test_state_store_cr3_uses_qemu_update(void)
{
    CPUX86State *env = &test_cpu->env;
    amdvm_x86_guest_state_v1_t state;
    unsigned before;

    memset(env, 0, sizeof(*env));
    env->eflags = 2;
    env->cr[0] = CR0_ET_MASK | CR0_PE_MASK | CR0_PG_MASK;
    env->cr[3] = 0x1000;
    env->cr[4] = CR4_PAE_MASK;
    env->efer = MSR_EFER_LME | MSR_EFER_LMA;
    env->xcr0 = XSTATE_FP_MASK;
    env->xstate_bv = 0;
    env->mxcsr = 0x1f80;
    env->dr[6] = DR6_FIXED_1;
    env->dr[7] = DR7_FIXED_1;
    env->pat = UINT64_C(0x0007040600070406);

    g_assert_cmpint(amdsvm_state_load(CPU(test_cpu), &state), ==, 0);
    state.cr[3] = 0x2000;
    before = cr3_update_calls;
    g_assert_cmpint(amdsvm_state_store(CPU(test_cpu), &state), ==, 0);
    g_assert_cmpuint(env->cr[3], ==, 0x2000);
    g_assert_cmpuint(cr3_update_calls, ==, before + 1);

    /* An unchanged CR3 must not trigger a redundant TLB update. */
    before = cr3_update_calls;
    g_assert_cmpint(amdsvm_state_store(CPU(test_cpu), &state), ==, 0);
    g_assert_cmpuint(cr3_update_calls, ==, before);

    /* NOFLUSH without PCIDE is invalid and fails before mutating CR3. */
    state.cr[3] = UINT64_C(1) << 63;
    g_assert_cmpint(amdsvm_state_store(CPU(test_cpu), &state), ==, -EINVAL);
    g_assert_cmpuint(env->cr[3], ==, 0x2000);
    g_assert_cmpuint(cr3_update_calls, ==, before);
}

static void test_state_cr2_roundtrip(void)
{
    CPUX86State *env = &test_cpu->env;
    amdvm_x86_guest_state_v1_t state;
    unsigned before;

    memset(env, 0, sizeof(*env));
    env->eflags = 2;
    env->cr[0] = CR0_ET_MASK | CR0_PE_MASK | CR0_PG_MASK;
    env->cr[2] = UINT64_C(0x1111222233334444);
    env->cr[3] = 0x1000;
    env->cr[4] = CR4_PAE_MASK;
    env->efer = MSR_EFER_LME | MSR_EFER_LMA;
    env->xcr0 = XSTATE_FP_MASK;
    env->xstate_bv = 0;
    env->mxcsr = 0x1f80;
    env->dr[6] = DR6_FIXED_1;
    env->dr[7] = DR7_FIXED_1;
    env->pat = UINT64_C(0x0007040600070406);

    g_assert_cmpint(amdsvm_state_load(CPU(test_cpu), &state), ==, 0);
    g_assert_cmphex(state.cr[2], ==, UINT64_C(0x1111222233334444));
    state.cr[2] = UINT64_C(0xaaaabbbbccccdddd);
    before = cr3_update_calls;
    g_assert_cmpint(amdsvm_state_store(CPU(test_cpu), &state), ==, 0);
    g_assert_cmphex(env->cr[2], ==, UINT64_C(0xaaaabbbbccccdddd));
    g_assert_cmpuint(env->cr[3], ==, 0x1000);
    g_assert_cmpuint(cr3_update_calls, ==, before);
    g_assert_cmpint(amdsvm_state_load(CPU(test_cpu), &state), ==, 0);
    g_assert_cmphex(state.cr[2], ==, UINT64_C(0xaaaabbbbccccdddd));
}

static void test_qemu_reset_vector_state_maps_to_provider(void)
{
    CPUX86State *env = &test_cpu->env;
    amdvm_x86_guest_state_v1_t state;
    uint32_t code_segment_flags = DESC_P_MASK | DESC_S_MASK | DESC_CS_MASK |
                                  DESC_R_MASK | DESC_A_MASK;
    uint32_t data_segment_flags = DESC_P_MASK | DESC_S_MASK | DESC_W_MASK |
                                  DESC_A_MASK;
    unsigned i;

    memset(env, 0, sizeof(*env));
    env->eip = 0xfff0;
    env->eflags = 2;
    env->cr[0] = 0x60000010 | CR0_ET_MASK;
    env->cr[4] = 0;
    env->xcr0 = XSTATE_FP_MASK;
    env->xstate_bv = 0;
    env->mxcsr = 0x1f80;
    env->pat = UINT64_C(0x0007040600070406);
    env->dr[6] = DR6_FIXED_1;
    env->dr[7] = DR7_FIXED_1;

    env->segs[R_CS].selector = 0xf000;
    env->segs[R_CS].base = 0xffff0000;
    env->segs[R_CS].limit = 0xffff;
    env->segs[R_CS].flags = code_segment_flags;
    for (i = 0; i < 6; i++) {
        if (i == R_CS) {
            continue;
        }
        env->segs[i].selector = 0;
        env->segs[i].base = 0;
        env->segs[i].limit = 0xffff;
        env->segs[i].flags = data_segment_flags;
    }
    env->gdt.limit = 0xffff;
    env->idt.limit = 0xffff;
    env->ldt.limit = 0xffff;
    env->ldt.flags = DESC_P_MASK | (2 << DESC_TYPE_SHIFT);
    env->tr.limit = 0xffff;
    env->tr.flags = DESC_P_MASK | (11 << DESC_TYPE_SHIFT);

    g_assert_cmpint(amdsvm_state_load(CPU(test_cpu), &state), ==, 0);
    g_assert_cmphex(state.rip, ==, 0xfff0);
    g_assert_cmphex(state.rflags, ==, 2);
    g_assert_cmphex(state.cr[0], ==, 0x60000010);
    g_assert_cmphex(state.cr[4], ==, 0);
    g_assert_cmphex(state.segments[AMDVM_X86_SEG_CS].selector, ==, 0xf000);
    g_assert_cmphex(state.segments[AMDVM_X86_SEG_CS].base, ==, 0xffff0000);
    g_assert_cmphex(state.segments[AMDVM_X86_SEG_CS].limit, ==, 0xffff);
    g_assert_cmphex(state.segments[AMDVM_X86_SEG_CS].attributes,
                    ==, 0x9b);
    g_assert_cmphex(state.segments[AMDVM_X86_SEG_GDTR].limit, ==, 0xffff);
    g_assert_cmphex(state.segments[AMDVM_X86_SEG_IDTR].limit, ==, 0xffff);
    g_assert_cmphex(state.segments[AMDVM_X86_SEG_TR].attributes,
                    ==, 0x8b);
    g_assert_cmphex(state.segments[AMDVM_X86_SEG_CS].base + state.rip,
                    ==, UINT64_C(0xfffffff0));

}

static void test_segment_attribute_bit_mapping(void)
{
    CPUX86State *env = &test_cpu->env;
    amdvm_x86_guest_state_v1_t state;
    uint32_t qemu_flags = DESC_TYPE_MASK | DESC_S_MASK | DESC_DPL_MASK |
                          DESC_P_MASK | DESC_AVL_MASK | DESC_L_MASK |
                          DESC_B_MASK | DESC_G_MASK;
    uint16_t vmcb_attributes = amdsvm_segment_attributes_from_qemu_flags(
        qemu_flags);

    /* VMCB packs AVL/L/DB/G in bits 8..11, unlike QEMU's bit 20..23 flags. */
    g_assert_cmphex(vmcb_attributes, ==, 0x0fff);
    g_assert_cmphex(amdsvm_segment_flags_from_attributes(vmcb_attributes),
                    ==, qemu_flags);
    g_assert_cmphex(amdsvm_segment_attributes_from_qemu_flags(
                        DESC_S_MASK | DESC_P_MASK |
                        DESC_CS_MASK | DESC_R_MASK | DESC_A_MASK |
                        DESC_B_MASK | DESC_G_MASK),
                    ==, 0x0c9b);

    memset(env, 0, sizeof(*env));
    env->xcr0 = XSTATE_FP_MASK;
    env->xstate_bv = 0;
    env->mxcsr = 0x1f80;
    env->eflags = 2;
    env->dr[6] = DR6_FIXED_1;
    env->dr[7] = DR7_FIXED_1;
    g_assert_cmpint(amdsvm_state_load(CPU(test_cpu), &state), ==, 0);
    state.segments[AMDVM_X86_SEG_CS].attributes = 0x0c9b;
    state.segments[AMDVM_X86_SEG_SS].attributes = 0x0c93;
    g_assert_cmpint(amdsvm_state_store(CPU(test_cpu), &state), ==, 0);
    g_assert_true((env->hflags & HF_CS32_MASK) != 0);
    g_assert_true((env->hflags & HF_SS32_MASK) != 0);
    g_assert_cmpint(amdsvm_state_load(CPU(test_cpu), &state), ==, 0);
    g_assert_cmphex(state.segments[AMDVM_X86_SEG_CS].attributes, ==, 0x0c9b);
    g_assert_cmphex(state.segments[AMDVM_X86_SEG_SS].attributes, ==, 0x0c93);
}

static void test_mmio_rmw_decoder_profile(void)
{
    static const uint8_t rtl8139_and_word[] = {
        0x66, 0x81, 0xa1, 0xe0, 0x00, 0x00, 0x00, 0xfc, 0xff,
    };
    static const uint8_t locked_and_word[] = {
        0xf0, 0x66, 0x81, 0xa1, 0xe0, 0x00, 0x00, 0x00, 0xfc, 0xff,
    };
    static const uint8_t cmp_word[] = {
        0x66, 0x81, 0xb9, 0xe0, 0x00, 0x00, 0x00, 0xfc, 0xff,
    };
    x86_insn_stream stream;
    struct x86_decode decode;
    CPUX86State *env = &test_cpu->env;

    env->hflags |= HF_CS64_MASK | HF_LMA_MASK;
    env->cr[0] |= CR0_PE_MASK | CR0_PG_MASK;
    env->efer |= MSR_EFER_LME | MSR_EFER_LMA;
    stream = (x86_insn_stream) {
        .bytes = rtl8139_and_word,
        .len = sizeof(rtl8139_and_word),
    };
    memset(&decode, 0, sizeof(decode));
    g_assert_cmpuint(decode_instruction_stream(env, &decode, &stream), >, 0);
    g_assert_cmpint(decode.cmd, ==, X86_DECODE_CMD_AND);
    g_assert_cmpint(decode.op[0].type, ==, X86_VAR_RM);
    g_assert_true(amdsvm_state_npf_mmio_rmw_supported(&decode));

    stream.bytes = locked_and_word;
    stream.len = sizeof(locked_and_word);
    memset(&decode, 0, sizeof(decode));
    g_assert_cmpuint(decode_instruction_stream(env, &decode, &stream), >, 0);
    g_assert_true(decode.lock);
    g_assert_false(amdsvm_state_npf_mmio_rmw_supported(&decode));

    stream.bytes = cmp_word;
    stream.len = sizeof(cmp_word);
    memset(&decode, 0, sizeof(decode));
    g_assert_cmpuint(decode_instruction_stream(env, &decode, &stream), >, 0);
    g_assert_cmpint(decode.cmd, ==, X86_DECODE_CMD_CMP);
    g_assert_false(amdsvm_state_npf_mmio_rmw_supported(&decode));
}

static void test_mmio_read_decoder_profile(void)
{
    static const uint8_t rtl8139_test_byte[] = { 0x44, 0x84, 0x40, 0x5a };
    static const uint8_t rtl8139_cmp_byte[] = {
        0x80, 0xb8, 0x8b, 0x00, 0x00, 0x00, 0x00,
    };
    static const uint8_t rtl8139_bt_word[] = {
        0x66, 0x0f, 0xba, 0x60, 0x62, 0x0c,
    };
    static const uint8_t locked_test_byte[] = { 0xf0, 0x44, 0x84, 0x40, 0x5a };
    x86_insn_stream stream;
    struct x86_decode decode;
    CPUX86State *env = &test_cpu->env;

    env->hflags |= HF_CS64_MASK | HF_LMA_MASK;
    env->cr[0] |= CR0_PE_MASK | CR0_PG_MASK;
    env->efer |= MSR_EFER_LME | MSR_EFER_LMA;
    stream = (x86_insn_stream) {
        .bytes = rtl8139_test_byte,
        .len = sizeof(rtl8139_test_byte),
    };
    memset(&decode, 0, sizeof(decode));
    g_assert_cmpuint(decode_instruction_stream(env, &decode, &stream), >, 0);
    g_assert_cmpint(decode.cmd, ==, X86_DECODE_CMD_TST);
    g_assert_true(amdsvm_state_npf_mmio_read_supported(&decode));

    stream.bytes = rtl8139_cmp_byte;
    stream.len = sizeof(rtl8139_cmp_byte);
    memset(&decode, 0, sizeof(decode));
    g_assert_cmpuint(decode_instruction_stream(env, &decode, &stream), >, 0);
    g_assert_cmpint(decode.cmd, ==, X86_DECODE_CMD_CMP);
    g_assert_true(amdsvm_state_npf_mmio_read_supported(&decode));

    stream.bytes = rtl8139_bt_word;
    stream.len = sizeof(rtl8139_bt_word);
    memset(&decode, 0, sizeof(decode));
    g_assert_cmpuint(decode_instruction_stream(env, &decode, &stream), >, 0);
    g_assert_cmpint(decode.cmd, ==, X86_DECODE_CMD_BT);
    g_assert_true(amdsvm_state_npf_mmio_read_supported(&decode));

    stream.bytes = locked_test_byte;
    stream.len = sizeof(locked_test_byte);
    memset(&decode, 0, sizeof(decode));
    g_assert_cmpuint(decode_instruction_stream(env, &decode, &stream), >, 0);
    g_assert_true(decode.lock);
    g_assert_false(amdsvm_state_npf_mmio_read_supported(&decode));
}

static void test_cpuid_publishes_known_tsc_frequency(void)
{
    CPUX86State *env = &test_cpu->env;
    uint32_t eax, ebx, ecx, edx;

    test_cpu->vmware_cpuid_freq = true;
    env->tsc_khz = 3400001;
    env->apic_bus_freq = 400000000;

    amdsvm_emulate_cpuid(CPU(test_cpu), 0x40000000, 0,
                         &eax, &ebx, &ecx, &edx);
    g_assert_cmphex(eax, ==, 0x40000010);
    g_assert_cmphex(ebx, ==, 0);
    g_assert_cmphex(ecx, ==, 0);
    g_assert_cmphex(edx, ==, 0);

    amdsvm_emulate_cpuid(CPU(test_cpu), 0x40000010, 0,
                         &eax, &ebx, &ecx, &edx);
    g_assert_cmpuint(eax, ==, 3400001);
    g_assert_cmpuint(ebx, ==, 400000);
    g_assert_cmphex(ecx, ==, 0);
    g_assert_cmphex(edx, ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    register_test_cpu();
    amdsvm_state_init_emulator();
    g_test_add_func("/amdsvm/msr/rdmsr-state", test_rdmsr_uses_qemu_state);
    g_test_add_func("/amdsvm/msr/wrmsr-state", test_wrmsr_mutates_qemu_state);
    g_test_add_func("/amdsvm/msr/direction-and-nrip", test_direction_and_nrip_fail_closed);
    g_test_add_func("/amdsvm/msr/unknown-reads-zero-like-tcg",
                    test_unknown_msr_reads_zero_like_tcg);
    g_test_add_func("/amdsvm/cr/mov-from-cr0",
                    test_mov_from_cr0_uses_guest_state);
    g_test_add_func("/amdsvm/cr/mov-to-cr3-r8",
                    test_mov_to_cr3_from_extended_gpr);
    g_test_add_func("/amdsvm/cr/invalid-write-gp",
                    test_invalid_cr0_write_injects_gp_without_advancing);
    g_test_add_func("/amdsvm/cr/missing-decode-assist",
                    test_cr_exit_rejects_missing_decode_assist);
    g_test_add_func("/amdsvm/cr/pcide-requires-paging",
                    test_cr4_pcide_requires_paging);
    g_test_add_func("/amdsvm/state/legacy-xstate-roundtrip",
                    test_qemu_guest_state_xstate_roundtrip);
    g_test_add_func("/amdsvm/state/cr3-update-path",
                    test_state_store_cr3_uses_qemu_update);
    g_test_add_func("/amdsvm/state/cr2-roundtrip",
                    test_state_cr2_roundtrip);
    g_test_add_func("/amdsvm/state/qemu-reset-vector",
                    test_qemu_reset_vector_state_maps_to_provider);
    g_test_add_func("/amdsvm/state/segment-attribute-bit-mapping",
                    test_segment_attribute_bit_mapping);
    g_test_add_func("/amdsvm/npf/mmio-rmw-decoder-profile",
                    test_mmio_rmw_decoder_profile);
    g_test_add_func("/amdsvm/npf/mmio-read-decoder-profile",
                    test_mmio_read_decoder_profile);
    g_test_add_func("/amdsvm/cpuid/known-tsc-frequency",
                    test_cpuid_publishes_known_tsc_frequency);
    return g_test_run();
}
