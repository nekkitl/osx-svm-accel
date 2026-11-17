#ifndef AMDVM_VM_ABI_DRAFT_H
#define AMDVM_VM_ABI_DRAFT_H

/*
 * Wire layouts for the append-only VM-scoped IOUserClient API.
 * VM version 2 implements resource preparation/destruction (13–17, 21, 22),
 * and CPU GET_STATE (24), plus per-vCPU memory types (20). Diagnostic 23
 * provides read-only VMCB/NPT inspection and never executes VMRUN. RUN and
 * KICK selectors (18/19) are compiled out of dispatch by default; an explicit
 * experimental build flag is required.
 * STATE capability alone does not authorize guest execution.
 * Legacy selectors 0–12 and their wire versions are unchanged.
 */

#include <stddef.h>
#include <stdint.h>

#include "amdvm_probe_abi.h"

#define AMDVM_VM_ABI_DRAFT_VERSION 2u
#define AMDVM_VM_ABI_DRAFT_MAX_VCPUS 64u
#define AMDVM_VM_ABI_DRAFT_SELECTOR_NEGOTIATE 13u
#define AMDVM_VM_ABI_DRAFT_SELECTOR_SET_MEMORY_MAP 14u
#define AMDVM_VM_ABI_DRAFT_SELECTOR_SET_RESET_ROM 15u
#define AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_CREATE 16u
#define AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_SET_STATE 17u
#define AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_RUN 18u
#define AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_KICK 19u
#define AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_SET_MEMORY_TYPES 20u
#define AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_DESTROY 21u
#define AMDVM_VM_ABI_DRAFT_SELECTOR_VM_DESTROY 22u
#define AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_DIAGNOSTIC 23u
#define AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_GET_STATE 24u
#define AMDVM_VM_ABI_DRAFT_SELECTOR_VCPU_RUN_SHARED 25u
#define AMDVM_VM_ABI_DRAFT_SHARED_RUN_MEMORY_TYPE 1u

#define AMDVM_VM_ABI_DRAFT_FEATURE_TRANSACTIONAL_MAP (1ull << 0)
#define AMDVM_VM_ABI_DRAFT_FEATURE_PER_VCPU_NPT      (1ull << 1)
#define AMDVM_VM_ABI_DRAFT_FEATURE_VCPU_STATE_V1     (1ull << 2)
#define AMDVM_VM_ABI_DRAFT_FEATURE_VCPU_PREPARE      (1ull << 3)
#define AMDVM_VM_ABI_DRAFT_FEATURE_VCPU_MEMORY_TYPES (1ull << 4)
#define AMDVM_VM_ABI_DRAFT_FEATURE_VCPU_RUN_V1       (1ull << 5)
#define AMDVM_VM_ABI_DRAFT_FEATURE_VCPU_KICK_V1      (1ull << 6)
#define AMDVM_VM_ABI_DRAFT_FEATURE_MAP_REPLACE_V1    (1ull << 7)
#define AMDVM_VM_ABI_DRAFT_FEATURE_NMI_EVENT_V1      (1ull << 8)
#define AMDVM_VM_ABI_DRAFT_FEATURE_DIAGNOSTIC_GP_INTERCEPT (1ull << 9)
/* Guest CR8/V_TPR is transported in state ABI v3 and CR8 exits are disabled. */
#define AMDVM_VM_ABI_DRAFT_FEATURE_NATIVE_CR8_V1      (1ull << 10)
#define AMDVM_VM_ABI_DRAFT_FEATURE_NATIVE_RDTSC_V1    (1ull << 11)
#define AMDVM_VM_ABI_DRAFT_FEATURE_PROVIDER_PAUSE_STATS_V1 (1ull << 12)
#define AMDVM_VM_ABI_DRAFT_FEATURE_SHARED_VCPU_RUN_V1 (1ull << 13)
#define AMDVM_VM_ABI_DRAFT_FEATURE_ATOMIC_RUN_SLOT_V1 (1ull << 14)
#define AMDVM_VM_ABI_DRAFT_FEATURE_MTRR_PAT_CACHE_V1 (1ull << 15)
#define AMDVM_VM_ABI_DRAFT_FEATURE_NATIVE_CR3_V1  (1ull << 16)
#define AMDVM_VM_ABI_DRAFT_FEATURE_DIAGNOSTIC_PF_INTERCEPT (1ull << 17)
/*
 * v5 source compatibility with the v4 QEMU capability spelling. Both names
 * describe the same bit. The corresponding request flags are retained only
 * so QEMU can compile; provider-lockless is fail-closed in QEMU and the v5
 * provider does not accept these request bits. Do not enable that gate until
 * its wire semantics are reconciled and separately tested.
 */
#define AMDVM_VM_ABI_DRAFT_FEATURE_ATOMIC_SLOT_LIFETIME_V1 \
	AMDVM_VM_ABI_DRAFT_FEATURE_ATOMIC_RUN_SLOT_V1
#define AMDVM_VM_ABI_DRAFT_VCPU_CREATE_ATOMIC_SLOT (1u << 0)
#define AMDVM_VM_ABI_DRAFT_VCPU_RUN_ATOMIC_SLOT    (1u << 2)
#define AMDVM_VM_ABI_DRAFT_MEMORY_MAP_ATOMIC_SLOT_MODE (1u << 1)
#define AMDVM_VM_ABI_DRAFT_VCPU_RUN_DIAGNOSTIC_INTERCEPT_GP (1u << 0)
#define AMDVM_VM_ABI_DRAFT_VCPU_RUN_NATIVE_RDTSC      (1u << 1)
#define AMDVM_VM_ABI_DRAFT_VCPU_RUN_MTRR_CACHE         (1u << 3)
#define AMDVM_VM_ABI_DRAFT_VCPU_RUN_PROVIDER_PAUSE     (1u << 4)
#define AMDVM_VM_ABI_DRAFT_VCPU_RUN_NATIVE_CR3     (1u << 5)
#define AMDVM_VM_ABI_DRAFT_VCPU_RUN_DIAGNOSTIC_INTERCEPT_PF (1u << 6)

#define AMDVM_VM_ABI_DRAFT_VCPU_DIAG_NPT_ENABLED       (1u << 0)
#define AMDVM_VM_ABI_DRAFT_VCPU_DIAG_NPT_ROOT_MATCH    (1u << 1)
#define AMDVM_VM_ABI_DRAFT_VCPU_DIAG_RESET_TRANSLATES  (1u << 2)
#define AMDVM_VM_ABI_DRAFT_VCPU_DIAG_APIC_UNMAPPED     (1u << 3)
#define AMDVM_VM_ABI_DRAFT_VCPU_DIAG_RESET_BACKING_MATCH (1u << 4)

#define AMDVM_VM_ABI_DRAFT_NPT_STATUS_MAPPED    0
#define AMDVM_VM_ABI_DRAFT_NPT_STATUS_UNMAPPED  1
#define AMDVM_VM_ABI_DRAFT_NPT_STATUS_INVALID  (-1)

/* SET_MEMORY_MAP may replace the current map only after all vCPUs are destroyed. */
#define AMDVM_VM_ABI_DRAFT_MEMORY_MAP_REPLACE (1u << 0)

static inline int
amdvm_vm_abi_draft_memory_map_flags_valid(uint32_t flags)
{
	return (flags & ~AMDVM_VM_ABI_DRAFT_MEMORY_MAP_REPLACE) == 0;
}

typedef struct amdvm_vm_abi_draft_negotiate_request {
	uint32_t requested_version;
	uint32_t structure_size;
	uint64_t reserved[2];
} amdvm_vm_abi_draft_negotiate_request_t;

typedef struct amdvm_vm_abi_draft_negotiate_result {
	uint32_t supported_version;
	uint32_t structure_size;
	uint32_t max_vcpus;
	uint32_t max_memory_ranges;
	uint64_t feature_flags;
	uint64_t max_guest_bytes;
	uint64_t max_mapped_bytes;
} amdvm_vm_abi_draft_negotiate_result_t;

/* One complete map transaction; ranges use the existing fixed 32-byte shape. */
typedef struct amdvm_vm_abi_draft_memory_map_request {
	uint32_t abi_version;
	uint32_t structure_size;
	uint32_t range_count;
	uint32_t flags;
	uint64_t expected_generation;
	uint64_t reserved[2];
	amdvm_vcpu_memory_range_t ranges[AMDVM_VCPU_MEMORY_MAX_RANGES];
} amdvm_vm_abi_draft_memory_map_request_t;

typedef struct amdvm_vm_abi_draft_memory_map_result {
	uint32_t abi_version;
	uint32_t structure_size;
	int32_t context_status;
	uint32_t range_count;
	uint64_t map_generation;
	uint64_t guest_page_count;
	uint64_t pinned_page_count;
} amdvm_vm_abi_draft_memory_map_result_t;

typedef struct amdvm_vm_abi_draft_vcpu_create_request {
	uint32_t abi_version;
	uint32_t structure_size;
	uint32_t vcpu_id;
	uint32_t apic_id;
	uint32_t reset_profile;
	uint32_t flags;
	uint64_t map_generation;
	uint64_t reserved[2];
} amdvm_vm_abi_draft_vcpu_create_request_t;

typedef struct amdvm_vm_abi_draft_vcpu_create_result {
	uint32_t abi_version;
	uint32_t structure_size;
	uint32_t vcpu_id;
	uint32_t flags;
	int32_t context_status;
	uint32_t reserved;
	uint64_t map_generation;
	uint64_t npt_page_count;
} amdvm_vm_abi_draft_vcpu_create_result_t;

typedef struct amdvm_vm_abi_draft_vcpu_lifecycle_request {
	uint32_t abi_version;
	uint32_t structure_size;
	uint32_t vcpu_id;
	uint32_t reserved;
	uint64_t map_generation;
} amdvm_vm_abi_draft_vcpu_lifecycle_request_t;

typedef struct amdvm_vm_abi_draft_vm_destroy_request {
	uint32_t abi_version;
	uint32_t structure_size;
	uint32_t flags;
	uint32_t reserved;
	uint64_t expected_generation;
} amdvm_vm_abi_draft_vm_destroy_request_t;

typedef struct amdvm_vm_abi_draft_lifecycle_result {
	uint32_t abi_version;
	uint32_t structure_size;
	int32_t context_status;
	uint32_t flags;
	uint64_t map_generation;
} amdvm_vm_abi_draft_lifecycle_result_t;

/* One reset ROM page is VM-wide and installed before the RAM map is committed. */
typedef struct amdvm_vm_abi_draft_reset_rom_request {
	uint32_t abi_version;
	uint32_t structure_size;
	uint64_t reserved[2];
	uint8_t page[AMDVM_VCPU_RESET_ROM_SIZE];
} amdvm_vm_abi_draft_reset_rom_request_t;

/* vCPU IDs are QEMU CPU indexes; apic_id remains guest-visible identity. */
typedef struct amdvm_vm_abi_draft_vcpu_state_request {
	uint32_t abi_version;
	uint32_t structure_size;
	uint32_t vcpu_id;
	uint32_t flags;
	uint64_t map_generation;
	amdvm_x86_guest_state_v1_t state;
} amdvm_vm_abi_draft_vcpu_state_request_t;

typedef struct amdvm_vm_abi_draft_vcpu_state_result {
	uint32_t abi_version;
	uint32_t structure_size;
	uint32_t vcpu_id;
	int32_t context_status;
	uint64_t map_generation;
	amdvm_x86_guest_state_v1_t state;
} amdvm_vm_abi_draft_vcpu_state_result_t;

/* One RUN request contains the full input state; every exit returns full state. */
typedef struct amdvm_vm_abi_draft_vcpu_run_request {
	uint32_t abi_version;
	uint32_t structure_size;
	uint32_t vcpu_id;
	uint32_t flags;
	uint64_t map_generation;
	uint64_t kick_generation;
	uint32_t event_injection;
	uint32_t event_injection_error_code;
	amdvm_x86_guest_state_v1_t state;
	/* Host-TSC-relative VMCB offset. Valid only with RUN_NATIVE_RDTSC. */
	uint64_t tsc_offset;
} amdvm_vm_abi_draft_vcpu_run_request_t;

/* Append-only wire compatibility: old RUN clients omit tsc_offset. */
#define AMDVM_VM_ABI_DRAFT_VCPU_RUN_REQUEST_V3_SIZE \
	(sizeof(amdvm_vm_abi_draft_vcpu_run_request_t))
#define AMDVM_VM_ABI_DRAFT_VCPU_RUN_REQUEST_V2_SIZE \
	offsetof(amdvm_vm_abi_draft_vcpu_run_request_t, tsc_offset)

typedef struct amdvm_vm_abi_draft_vcpu_run_result {
	uint32_t abi_version;
	uint32_t structure_size;
	uint32_t vcpu_id;
	uint32_t flags;
	int32_t context_status;
	int32_t owner_status;
	int32_t run_status;
	uint32_t exit_int_info;
	uint64_t map_generation;
	uint64_t exit_code;
	uint64_t exit_info1;
	uint64_t exit_info2;
	uint64_t next_rip;
	uint32_t instruction_length;
	uint8_t instruction_bytes[15];
	amdvm_x86_guest_state_v1_t state;
} amdvm_vm_abi_draft_vcpu_run_result_t;

typedef struct amdvm_vm_abi_draft_vcpu_kick_request {
	uint32_t abi_version;
	uint32_t structure_size;
	uint32_t vcpu_id;
	uint32_t flags;
	uint64_t map_generation;
	uint64_t kick_generation;
} amdvm_vm_abi_draft_vcpu_kick_request_t;

/* KICK only publishes a generation; caller must first signal the target vCPU
 * host thread so a running guest can exit through its host interrupt path. */
#define AMDVM_VM_ABI_DRAFT_VCPU_KICK_HOST_SIGNAL_SENT (1u << 0)
/* Read-only aggregate PAUSE counters; does not publish a kick generation. */
#define AMDVM_VM_ABI_DRAFT_VCPU_KICK_REPORT_PAUSE_STATS (1u << 1)

typedef struct amdvm_vm_abi_draft_vcpu_kick_result {
	uint32_t abi_version;
	uint32_t structure_size;
	uint32_t vcpu_id;
	uint32_t flags;
	int32_t context_status;
	uint32_t reserved;
	uint64_t map_generation;
	uint64_t kick_generation;
} amdvm_vm_abi_draft_vcpu_kick_result_t;

typedef struct amdvm_vm_abi_draft_memory_type_range {
	uint64_t guest_physical_address;
	uint64_t size;
	uint32_t memory_type;
	uint32_t reserved;
} amdvm_vm_abi_draft_memory_type_range_t;

typedef struct amdvm_vm_abi_draft_vcpu_memory_types_request {
	uint32_t abi_version;
	uint32_t structure_size;
	uint32_t vcpu_id;
	uint32_t range_count;
	uint64_t map_generation;
	uint64_t reserved[2];
	amdvm_vm_abi_draft_memory_type_range_t ranges[
	    AMDVM_VCPU_MEMORY_TYPE_MAX_RANGES];
} amdvm_vm_abi_draft_vcpu_memory_types_request_t;

typedef struct amdvm_vm_abi_draft_vcpu_memory_types_result {
	uint32_t abi_version;
	uint32_t structure_size;
	uint32_t vcpu_id;
	uint32_t range_count;
	int32_t context_status;
	uint32_t reserved;
	uint64_t map_generation;
	uint64_t updated_page_count;
} amdvm_vm_abi_draft_vcpu_memory_types_result_t;

typedef struct amdvm_vm_abi_draft_vcpu_diagnostic_request {
	uint32_t abi_version;
	uint32_t structure_size;
	uint32_t vcpu_id;
	uint32_t reserved;
	uint64_t map_generation;
} amdvm_vm_abi_draft_vcpu_diagnostic_request_t;

typedef struct amdvm_vm_abi_draft_vcpu_diagnostic_result {
	uint32_t abi_version;
	uint32_t structure_size;
	uint32_t vcpu_id;
	uint32_t flags;
	int32_t context_status;
	uint32_t reserved;
	uint64_t map_generation;
	amdvm_vcpu_diagnostic_result_t snapshot;
	uint32_t nested_paging_control;
	uint32_t npt_root_matches;
	int32_t reset_translation_status;
	int32_t apic_translation_status;
	uint64_t npt_root;
	uint64_t reset_translation_hpa;
} amdvm_vm_abi_draft_vcpu_diagnostic_result_t;

#ifdef __cplusplus
static_assert(sizeof(amdvm_x86_guest_state_v1_t) == 4608,
	"VM ABI draft full guest state size");
static_assert(offsetof(amdvm_x86_guest_state_v1_t, xstate) == 512,
	"VM ABI draft guest XSTATE offset");
static_assert(sizeof(amdvm_vm_abi_draft_negotiate_request_t) == 24,
	"VM ABI draft negotiate request size");
static_assert(sizeof(amdvm_vm_abi_draft_negotiate_result_t) == 40,
	"VM ABI draft negotiate result size");
static_assert(sizeof(amdvm_vm_abi_draft_memory_map_request_t) == 2088,
	"VM ABI draft memory map request size");
static_assert(sizeof(amdvm_vm_abi_draft_memory_map_result_t) == 40,
	"VM ABI draft memory map result size");
static_assert(sizeof(amdvm_vm_abi_draft_vcpu_create_request_t) == 48,
	"VM ABI draft vCPU create request size");
static_assert(sizeof(amdvm_vm_abi_draft_vcpu_create_result_t) == 40,
	"VM ABI draft vCPU create result size");
static_assert(sizeof(amdvm_vm_abi_draft_vcpu_lifecycle_request_t) == 24,
	"VM ABI draft vCPU lifecycle request size");
static_assert(sizeof(amdvm_vm_abi_draft_vm_destroy_request_t) == 24,
	"VM ABI draft VM destroy request size");
static_assert(sizeof(amdvm_vm_abi_draft_lifecycle_result_t) == 24,
	"VM ABI draft lifecycle result size");
static_assert(sizeof(amdvm_vm_abi_draft_reset_rom_request_t) == 4120,
	"VM ABI draft reset ROM request size");
static_assert(sizeof(amdvm_vm_abi_draft_vcpu_state_request_t) == 4632,
	"VM ABI draft vCPU state request size");
static_assert(sizeof(amdvm_vm_abi_draft_vcpu_state_result_t) == 4632,
	"VM ABI vCPU GET_STATE result size");
static_assert(sizeof(amdvm_vm_abi_draft_vcpu_run_request_t) == 4656,
	"VM ABI draft vCPU RUN request size");
static_assert(sizeof(amdvm_vm_abi_draft_vcpu_run_result_t) == 4704,
	"VM ABI draft vCPU RUN result size");
static_assert(sizeof(amdvm_vm_abi_draft_vcpu_kick_request_t) == 32,
	"VM ABI draft vCPU kick request size");
static_assert(sizeof(amdvm_vm_abi_draft_vcpu_kick_result_t) == 40,
	"VM ABI draft vCPU kick result size");
static_assert(sizeof(amdvm_vm_abi_draft_memory_type_range_t) == 24,
	"VM ABI draft memory type range size");
static_assert(sizeof(amdvm_vm_abi_draft_vcpu_memory_types_request_t) == 6184,
	"VM ABI draft vCPU memory types request size");
static_assert(sizeof(amdvm_vm_abi_draft_vcpu_memory_types_result_t) == 40,
	"VM ABI draft vCPU memory types result size");
static_assert(sizeof(amdvm_vm_abi_draft_vcpu_diagnostic_request_t) == 24,
	"VM ABI draft vCPU diagnostic request size");
static_assert(sizeof(amdvm_vm_abi_draft_vcpu_diagnostic_result_t) == 160,
	"VM ABI draft vCPU diagnostic result size");
#endif

#endif
