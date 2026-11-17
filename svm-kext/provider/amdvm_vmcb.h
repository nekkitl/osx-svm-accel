#ifndef AMDVM_VMCB_H
#define AMDVM_VMCB_H

#include <stddef.h>
#include <stdint.h>

/* AMD APM Vol. 2, VMCB control and state-save layout. */
#define AMDVM_PAGE_SIZE                 4096u
#define AMDVM_VMCB_SIZE                 4096u
#define AMDVM_IOPM_SIZE                 (3u * AMDVM_PAGE_SIZE)
#define AMDVM_MSRPM_SIZE                (2u * AMDVM_PAGE_SIZE)
#define AMDVM_VMCB_INTERCEPT_MISC1      0x00cu
#define AMDVM_VMCB_INTERCEPT_MISC2      0x010u
#define AMDVM_VMCB_INTERCEPT_CR_READ    0x000u
#define AMDVM_VMCB_INTERCEPT_CR_WRITE   0x002u
#define AMDVM_VMCB_INTERCEPT_DR_READ    0x004u
#define AMDVM_VMCB_INTERCEPT_DR_WRITE   0x006u
#define AMDVM_VMCB_INTERCEPT_EXCEPTIONS 0x008u
#define AMDVM_VMCB_IOPM_PA               0x040u
#define AMDVM_VMCB_MSRPM_PA              0x048u
#define AMDVM_VMCB_TLB_CONTROL          0x058u
#define AMDVM_VMCB_ASID                 AMDVM_VMCB_TLB_CONTROL
#define AMDVM_VMCB_TLB_CONTROL_COMMAND (AMDVM_VMCB_TLB_CONTROL + 4u)
#define AMDVM_VMCB_TSC_OFFSET            0x050u
#define AMDVM_VMCB_INTERRUPT_CONTROL    0x060u
#define AMDVM_VMCB_INTERRUPT_STATE      0x068u
#define AMDVM_VMCB_EXIT_CODE            0x070u
#define AMDVM_VMCB_EXIT_INFO1           0x078u
#define AMDVM_VMCB_EXIT_INFO2           0x080u
#define AMDVM_VMCB_EXIT_INT_INFO        0x088u
/* EXITINTINFO is two adjacent dwords; nested_ctl/NP_ENABLE begins at 0x90. */
#define AMDVM_VMCB_EXIT_INT_INFO_ERROR  0x08cu
#define AMDVM_VMCB_NEXT_RIP              0x0c8u
#define AMDVM_VMCB_DECODE_ASSIST_LENGTH  0x0d0u
#define AMDVM_VMCB_DECODE_ASSIST_BYTES   0x0d1u
#define AMDVM_VMCB_DECODE_ASSIST_MAX     15u
#define AMDVM_VMCB_NESTED_PAGING        0x090u
#define AMDVM_VMCB_NPT_ROOT              0x0b0u
#define AMDVM_VMCB_EVENT_INJECTION       0x0a8u
#define AMDVM_VMCB_CLEAN_BITS            0x0c0u
#define AMDVM_VMCB_CLEAN_INTERCEPTS       (1u << 0)
#define AMDVM_VMCB_CLEAN_IOPM             (1u << 1)
#define AMDVM_VMCB_CLEAN_ASID             (1u << 2)
#define AMDVM_VMCB_CLEAN_INTR             (1u << 3)
#define AMDVM_VMCB_CLEAN_NPT              (1u << 4)
#define AMDVM_VMCB_CLEAN_CR               (1u << 5)
#define AMDVM_VMCB_CLEAN_DR               (1u << 6)
#define AMDVM_VMCB_CLEAN_DT               (1u << 7)
#define AMDVM_VMCB_CLEAN_SEG              (1u << 8)
#define AMDVM_VMCB_CLEAN_CR2              (1u << 9)
#define AMDVM_VMCB_CLEAN_LBR              (1u << 10)
#define AMDVM_VMCB_CLEAN_AVIC             (1u << 11)
#define AMDVM_VMCB_CLEAN_CET              (1u << 12)
/* RUN currently rewrites every other cached group. */
#define AMDVM_VMCB_CLEAN_STABLE_RUN_MASK \
	(AMDVM_VMCB_CLEAN_IOPM | AMDVM_VMCB_CLEAN_ASID)
#define AMDVM_VMCB_GUEST_STATE            0x400u
#define AMDVM_VMCB_GUEST_ES               0x400u
#define AMDVM_VMCB_GUEST_CS               0x410u
#define AMDVM_VMCB_GUEST_SS               0x420u
#define AMDVM_VMCB_GUEST_DS               0x430u
#define AMDVM_VMCB_GUEST_FS               0x440u
#define AMDVM_VMCB_GUEST_GS               0x450u
#define AMDVM_VMCB_GUEST_GDTR             0x460u
#define AMDVM_VMCB_GUEST_LDTR             0x470u
#define AMDVM_VMCB_GUEST_IDTR             0x480u
#define AMDVM_VMCB_GUEST_TR               0x490u
#define AMDVM_VMCB_GUEST_CPL              0x4cbu
#define AMDVM_VMCB_GUEST_EFER             0x4d0u
#define AMDVM_VMCB_GUEST_CR4              0x548u
#define AMDVM_VMCB_GUEST_CR3              0x550u
#define AMDVM_VMCB_GUEST_CR0              0x558u
#define AMDVM_VMCB_GUEST_DR7              0x560u
#define AMDVM_VMCB_GUEST_DR6              0x568u
#define AMDVM_VMCB_GUEST_RFLAGS           0x570u
#define AMDVM_VMCB_GUEST_RIP               0x578u
#define AMDVM_VMCB_GUEST_RSP               0x5d8u
#define AMDVM_VMCB_GUEST_RAX               0x5f8u
#define AMDVM_VMCB_GUEST_STAR              0x600u
#define AMDVM_VMCB_GUEST_LSTAR             0x608u
#define AMDVM_VMCB_GUEST_CSTAR             0x610u
#define AMDVM_VMCB_GUEST_SFMASK            0x618u
#define AMDVM_VMCB_GUEST_KERNEL_GS_BASE    0x620u
#define AMDVM_VMCB_GUEST_SYSENTER_CS       0x628u
#define AMDVM_VMCB_GUEST_SYSENTER_ESP      0x630u
#define AMDVM_VMCB_GUEST_SYSENTER_EIP      0x638u
#define AMDVM_VMCB_GUEST_CR2               0x640u
#define AMDVM_VMCB_GUEST_PAT               0x668u

#define AMDVM_EFER_SVME                    (1ull << 12)
#define AMDVM_GUEST_PAT_ALL_WB             0x0606060606060606ull
#define AMDVM_QEMU_RESET_PAT                0x0007040600070406ull

#define AMDVM_SVM_INTERCEPT_INTR          (1u << 0)
#define AMDVM_SVM_INTERCEPT_NMI           (1u << 1)
#define AMDVM_SVM_INTERCEPT_CPUID         (1u << 18)
#define AMDVM_SVM_INTERCEPT_RSM            (1u << 19)
/* IRET remains native; SVM IRET exits are for dynamic NMI-window tracking. */
#define AMDVM_SVM_INTERCEPT_IRET           (1u << 20)
#define AMDVM_SVM_INTERCEPT_INTN           (1u << 21)
#define AMDVM_SVM_INTERCEPT_INVD           (1u << 22)
#define AMDVM_SVM_INTERCEPT_PAUSE          (1u << 23)
#define AMDVM_SVM_INTERCEPT_HLT           (1u << 24)
#define AMDVM_SVM_INTERCEPT_SHUTDOWN       (1u << 31)
#define AMDVM_SVM_INTERCEPT_RDTSC          (1u << 14)
#define AMDVM_SVM_INTERCEPT_RDPMC          (1u << 15)
#define AMDVM_SVM_INTERCEPT_SMI            (1u << 2)
#define AMDVM_SVM_INTERCEPT_INIT           (1u << 3)
#define AMDVM_SVM_INTERCEPT_IOIO          (1u << 27)
#define AMDVM_SVM_INTERCEPT_MSR           (1u << 28)
#define AMDVM_SVM_INTERCEPT_VMRUN         (1u << 0) /* misc2 */
#define AMDVM_SVM_INTERCEPT_VMMCALL       (1u << 1) /* misc2 */
#define AMDVM_SVM_INTERCEPT_VMLOAD        (1u << 2) /* misc2 */
#define AMDVM_SVM_INTERCEPT_VMSAVE        (1u << 3) /* misc2 */
#define AMDVM_SVM_INTERCEPT_STGI          (1u << 4) /* misc2 */
#define AMDVM_SVM_INTERCEPT_CLGI          (1u << 5) /* misc2 */
#define AMDVM_SVM_INTERCEPT_SKINIT        (1u << 6) /* misc2 */
#define AMDVM_SVM_INTERCEPT_RDTSCP        (1u << 7) /* misc2 */
#define AMDVM_SVM_INTERCEPT_WBINVD        (1u << 9) /* misc2 */
#define AMDVM_SVM_EXCEPTION_DB            (1u << 1)
#define AMDVM_X86_RFLAGS_TF                (1ull << 8)
#define AMDVM_X86_RFLAGS_RF                (1ull << 16)
#define AMDVM_SVM_INTERCEPT_MONITOR       (1u << 10) /* misc2 */
#define AMDVM_SVM_INTERCEPT_MWAIT         (1u << 11) /* misc2 */
#define AMDVM_SVM_INTERCEPT_XSETBV         (1u << 13) /* misc2 */
#define AMDVM_SVM_INTERCEPT_RDPRU          (1u << 14) /* misc2 */
#define AMDVM_SVM_INTERCEPT_EFER_WRITE_TRAP (1u << 15) /* misc2 */
#define AMDVM_SVM_EXIT_HLT                0x78u
#define AMDVM_SVM_EXIT_CPUID              0x72u
#define AMDVM_SVM_EXIT_INVALID             UINT64_C(0xffffffff)
#define AMDVM_SVM_EXIT_ERR                 UINT64_MAX
#define AMDVM_SVM_EXIT_NOT_RUN             (UINT64_MAX - 1u)
#define AMDVM_SVM_EXIT_NPF                0x400u
#define AMDVM_VMCB_INTERRUPT_SHADOW       (1ull << 0)
#define AMDVM_VMCB_V_INTR_MASKING         (1u << 24)
#define AMDVM_SVM_TLB_FLUSH_ALL           1u
#define AMDVM_VMCB_CR2_INDEX              2u
#define AMDVM_SVM_EXIT_WRITE_CR0          0x010u
#define AMDVM_SVM_EXIT_WRITE_CR3          0x013u
#define AMDVM_SVM_EXIT_WRITE_CR4          0x014u
#define AMDVM_VMCB_NPT_ENABLE             1u

typedef struct amdvm_vmcb {
	uint8_t bytes[AMDVM_VMCB_SIZE];
} amdvm_vmcb_t __attribute__((aligned(AMDVM_PAGE_SIZE)));

_Static_assert(sizeof(amdvm_vmcb_t) == AMDVM_VMCB_SIZE, "VMCB must be one page");
_Static_assert(_Alignof(amdvm_vmcb_t) == AMDVM_PAGE_SIZE, "VMCB alignment");
_Static_assert(AMDVM_VMCB_GUEST_EFER == AMDVM_VMCB_GUEST_STATE + 0xd0u,
	"VMCB guest EFER offset");
_Static_assert(AMDVM_VMCB_GUEST_TR == AMDVM_VMCB_GUEST_STATE + 0x90u,
	"VMCB guest TR offset");
_Static_assert(AMDVM_VMCB_GUEST_RAX == AMDVM_VMCB_GUEST_STATE + 0x1f8u,
	"VMCB guest RAX offset");
_Static_assert(AMDVM_VMCB_GUEST_PAT == AMDVM_VMCB_GUEST_STATE + 0x268u,
	"VMCB guest PAT offset");
_Static_assert(AMDVM_VMCB_EXIT_INT_INFO_ERROR + sizeof(uint32_t) ==
	AMDVM_VMCB_NESTED_PAGING, "EXITINTINFO error must precede NP_ENABLE");

#ifdef __cplusplus
extern "C" {
#endif
struct amdvm_vcpu_run_request;
struct amdvm_vcpu_entry_snapshot;
struct amdvm_npt;
void amdvm_vmcb_clear(amdvm_vmcb_t *vmcb);
void amdvm_vmcb_set_u16(amdvm_vmcb_t *vmcb, uint32_t offset, uint16_t value);
void amdvm_vmcb_set_u32(amdvm_vmcb_t *vmcb, uint32_t offset, uint32_t value);
void amdvm_vmcb_set_u64(amdvm_vmcb_t *vmcb, uint32_t offset, uint64_t value);
uint64_t amdvm_vmcb_get_u64(const amdvm_vmcb_t *vmcb, uint32_t offset);
/* Force VMRUN to reload all guest state from this VMCB, ignoring cached state. */
void amdvm_vmcb_clear_clean_bits(amdvm_vmcb_t *vmcb);
/* Stock-XNU stop handling depends on host interrupts forcing a VMEXIT. */
int amdvm_vmcb_external_interrupt_exit_enabled(const amdvm_vmcb_t *vmcb);
/* Accept the supported event-injection forms used by normal emulation. */
int amdvm_vmcb_validate_event_injection(uint32_t event_injection,
	uint32_t error_code);
/* Diagnostic #PF reinjection is permitted only with the PF-intercept gate. */
int amdvm_vmcb_validate_event_injection_diagnostic_pf(
	uint32_t event_injection, uint32_t error_code);
struct amdvm_x86_guest_state_v1;
/* Validate architectural invariants before any VMCB or SVM state changes. */
int amdvm_vmcb_validate_x86_state_v1(
	const struct amdvm_x86_guest_state_v1 *state);
int amdvm_vmcb_load_x86_state_v1(amdvm_vmcb_t *vmcb,
	const struct amdvm_x86_guest_state_v1 *state, uint64_t gprs[15],
	void *guest_xstate, size_t guest_xstate_size);
/* Caller has already passed amdvm_vmcb_validate_x86_state_v1(state). */
int amdvm_vmcb_load_x86_state_v1_validated(amdvm_vmcb_t *vmcb,
	const struct amdvm_x86_guest_state_v1 *state, uint64_t gprs[15],
	void *guest_xstate, size_t guest_xstate_size);
/* Cached state groups whose current VMCB contents equal the next input. */
uint32_t amdvm_vmcb_state_clean_candidates(const amdvm_vmcb_t *vmcb,
	const struct amdvm_x86_guest_state_v1 *state);
/* Temporarily single-step through an intercepted NMI-handler IRET. */
int amdvm_vmcb_arm_nmi_iret_step(amdvm_vmcb_t *vmcb,
	const struct amdvm_x86_guest_state_v1 *state,
	uint32_t *saved_exception_intercepts);
void amdvm_vmcb_finish_nmi_iret_step(amdvm_vmcb_t *vmcb,
	uint32_t saved_exception_intercepts, uint64_t original_rflags,
	struct amdvm_x86_guest_state_v1 *result_state);
/* Temporarily add diagnostic exception intercepts and restore the exact mask. */
int amdvm_vmcb_add_exception_intercepts(amdvm_vmcb_t *vmcb, uint32_t mask,
	uint32_t *saved_exception_intercepts);
void amdvm_vmcb_restore_exception_intercepts(amdvm_vmcb_t *vmcb,
	uint32_t saved_exception_intercepts);
/* Shared production/offline path for staging one QEMU RUN request. */
int amdvm_vmcb_stage_run_request(amdvm_vmcb_t *vmcb, uint64_t gprs[15],
	void *guest_xstate, const struct amdvm_vcpu_run_request *request,
	int full_state);
/* Temporarily allow native RDTSC with a QEMU-synchronized TSC offset. */
int amdvm_vmcb_prepare_native_rdtsc(amdvm_vmcb_t *vmcb, uint64_t tsc_offset,
	uint32_t *saved_misc1);
void amdvm_vmcb_restore_rdtsc_intercept(amdvm_vmcb_t *vmcb,
	uint32_t saved_misc1);
/* Temporarily allow native MOV-from-CR2 for one VMRUN. */
int amdvm_vmcb_prepare_native_cr2_read(amdvm_vmcb_t *vmcb,
	uint16_t *saved_cr_read);
void amdvm_vmcb_restore_cr2_read_intercept(amdvm_vmcb_t *vmcb,
	uint16_t saved_cr_read);
/* Temporarily allow native CR3 reads and writes for one VMRUN. */
int amdvm_vmcb_prepare_native_cr3(amdvm_vmcb_t *vmcb,
	uint16_t *saved_cr_read, uint16_t *saved_cr_write);
void amdvm_vmcb_restore_native_cr3(amdvm_vmcb_t *vmcb,
	uint16_t saved_cr_read, uint16_t saved_cr_write);
int amdvm_vmcb_capture_entry_snapshot(const amdvm_vmcb_t *vmcb,
	const struct amdvm_npt *npt, uint64_t guest_gpa_page,
	size_t entry_offset,
	const void *backing_virtual_address, uint64_t backing_physical_address,
	size_t backing_size, const uint64_t gprs[15], uint64_t host_xcr0,
	uint64_t guest_xcr0, uint32_t guest_xstate_size,
	struct amdvm_vcpu_entry_snapshot *snapshot);
int amdvm_vmcb_store_x86_state_v1(const amdvm_vmcb_t *vmcb,
	struct amdvm_x86_guest_state_v1 *state, const uint64_t gprs[15],
	const void *guest_xstate, size_t guest_xstate_size);
/* Clears the VMCB and seeds the restrictive NPT/intercept control profile. */
int amdvm_vmcb_configure_probe(amdvm_vmcb_t *vmcb, uint64_t npt_root_pa,
	uint64_t iopm_pa, uint64_t msrpm_pa, uint32_t asid);
/* Seed a fixed real-mode guest that begins at GPA 0 and executes HLT. */
int amdvm_vmcb_configure_realmode_test_v1(amdvm_vmcb_t *vmcb);
/* QEMU x86 reset state, with EFER.SVME set because the SVM guest requires it. */
int amdvm_vmcb_configure_qemu_reset_vector_v1(amdvm_vmcb_t *vmcb);
int amdvm_svm_init_all_intercept_bitmaps(void *iopm, size_t iopm_size,
	void *msrpm, size_t msrpm_size);
#ifdef __cplusplus
}
#endif

#endif
