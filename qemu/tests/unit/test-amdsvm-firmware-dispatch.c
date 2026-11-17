/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"

#include "../../accel/amdsvm/amdsvm-firmware-dispatch.h"
#include "../../accel/amdsvm/amdsvm-provider-lifecycle.h"
#include "../../target/i386/amdsvm/amdsvm-event.h"
#include "../../target/i386/amdsvm/amdsvm-state.h"
#include "../../target/i386/svm.h"

typedef struct FirmwareMock {
    unsigned int run_count;
    unsigned int cpuid_count;
    unsigned int rdtscp_count;
    unsigned int io_count;
    unsigned int string_io_count;
    unsigned int complete_count;
    unsigned int store_count;
    unsigned int iret_count;
    unsigned int physical_intr_count;
    unsigned int preentry_kick_count;
    uint32_t last_port;
    uint8_t last_output;
    uint64_t rdtscp_tsc;
    uint32_t rdtscp_aux;
    bool resumed_with_event;
    bool yield_on_physical_intr;
    bool fail_store;
    AMDVMEventState events;
    amdvm_x86_guest_state_v1_t stored;
} FirmwareMock;

static void test_diagnostic_page_fault_reinjection(void)
{
    const uint32_t pf_event = AMDVM_VCPU_EVENTINJ_VALID |
        AMDVM_VCPU_EVENTINJ_TYPE_EXCEPTION |
        AMDVM_VCPU_EVENTINJ_ERROR_VALID | AMDVM_VCPU_EXCEPTION_PF;
    AMDVMEventState events = { 0 };
    amdvm_vcpu_run_request_t request = { 0 };
    amdvm_x86_guest_state_v1_t guest = { 0 };

    g_assert_true(amdsvm_pf_mapping_mismatch(0, true));
    g_assert_true(amdsvm_pf_mapping_mismatch(2, true));
    g_assert_false(amdsvm_pf_mapping_mismatch(0, false));
    g_assert_false(amdsvm_pf_mapping_mismatch(1, true));
    g_assert_false(amdsvm_pf_mapping_mismatch(3, true));
    g_assert_false(amdsvm_event_valid(pf_event, 2));
    g_assert_false(amdsvm_event_valid(
        pf_event & ~AMDVM_VCPU_EVENTINJ_ERROR_VALID, 2));
    g_assert_cmpint(amdsvm_event_queue_exception(&events, pf_event, 2),
                    !=, 0);

    g_assert_cmpint(amdsvm_event_queue_page_fault(&events, 2), ==, 0);
    g_assert_cmpint(amdsvm_event_queue_page_fault(&events, 2), !=, 0);
    amdsvm_event_prepare(&events, &request);
    g_assert_cmphex(request.event_injection, ==, pf_event);
    g_assert_cmphex(request.event_injection_error_code, ==, 2);
    g_assert_cmpint(amdsvm_event_complete(&events, SVM_EXIT_PAUSE, 0,
                                          &guest), ==, 0);
    g_assert_false(events.exception_pending);
}

static int firmware_mock_transport(void *opaque, uint32_t selector,
                                   const void *input, size_t input_size,
                                   void *output, size_t *output_size)
{
    FirmwareMock *mock = opaque;

    switch (selector) {
    case AMDVM_PROBE_SELECTOR_VCPU_CREATE:
    case AMDVM_PROBE_SELECTOR_VCPU_SET_RESET_ROM: {
        amdvm_vcpu_lifecycle_result_t *result = output;

        g_assert_nonnull(input);
        memset(result, 0, sizeof(*result));
        result->abi_version = AMDVM_VCPU_ABI_VERSION;
        *output_size = sizeof(*result);
        return 0;
    }
    case AMDVM_PROBE_SELECTOR_VCPU_MAP_MEMORY: {
        amdvm_vcpu_memory_map_result_t *result = output;

        g_assert_cmpuint(input_size, ==,
                         sizeof(amdvm_vcpu_memory_map_request_t));
        memset(result, 0, sizeof(*result));
        result->abi_version = AMDVM_VCPU_ABI_VERSION;
        result->range_count = 1;
        result->guest_page_count = 2;
        result->npt_page_count = 1;
        *output_size = sizeof(*result);
        return 0;
    }
    case AMDVM_PROBE_SELECTOR_VCPU_RUN: {
        const amdvm_vcpu_run_request_t *request = input;
        amdvm_vcpu_run_result_t *result = output;
        unsigned int run = mock->run_count++;

        g_assert_cmpuint(input_size, ==, sizeof(*request));
        memset(result, 0, sizeof(*result));
        result->abi_version = AMDVM_VCPU_ABI_VERSION;
        result->flags = AMDVM_VCPU_RUN_CALLED;
        result->state = request->state;
        if (run == 0 || run == 3) {
            if (run == 3) {
                g_assert_cmpuint(request->event_injection, !=, 0);
                mock->resumed_with_event = true;
            }
            result->exit_code = AMDVM_VCPU_EXPECTED_CPUID_EXIT;
            result->next_rip = request->state.rip + 2;
        } else if (run == 1) {
            result->exit_code = AMDVM_VCPU_EXPECTED_IOIO_EXIT;
            result->exit_info1 = ((uint64_t)0xe9 << 16) | (1u << 4);
            result->state.gpr[AMDSVM_GPR_RAX] = 'A';
            result->next_rip = request->state.rip + 2;
        } else {
            result->exit_code = AMDVM_HLT_EXPECTED_EXIT;
            result->state.interruptibility =
                AMDVM_X86_INTERRUPTIBILITY_STI_SHADOW;
            result->next_rip = request->state.rip + 1;
        }
        *output_size = sizeof(*result);
        return 0;
    }
    case AMDVM_PROBE_SELECTOR_VCPU_DESTROY: {
        amdvm_vcpu_lifecycle_result_t *result = output;

        memset(result, 0, sizeof(*result));
        result->abi_version = AMDVM_VCPU_ABI_VERSION;
        *output_size = sizeof(*result);
        return 0;
    }
    default:
        g_assert_not_reached();
    }
}

static int dispatch_store(void *opaque,
                          const amdvm_x86_guest_state_v1_t *state)
{
    FirmwareMock *mock = opaque;

    mock->store_count++;
    if (mock->fail_store) {
        return -EIO;
    }
    mock->stored = *state;
    return 0;
}

static int dispatch_complete(void *opaque, uint64_t exit_code,
                             uint32_t exit_int_info,
                             amdvm_x86_guest_state_v1_t *state)
{
    FirmwareMock *mock = opaque;

    mock->complete_count++;
    return amdsvm_event_complete(&mock->events, exit_code, exit_int_info,
                                 state);
}

static int dispatch_iret(void *opaque, uint64_t guest_rip,
                         uint32_t interruptibility, uint64_t guest_rflags)
{
    FirmwareMock *mock = opaque;

    g_assert_cmphex(guest_rip, ==, 0x123450);
    g_assert_true(interruptibility & AMDVM_X86_INTERRUPTIBILITY_NMI_BLOCKED);
    mock->iret_count++;
    return amdsvm_event_handle_iret(&mock->events, guest_rip,
                                    interruptibility, guest_rflags);
}

static int dispatch_cpuid(void *opaque, uint32_t leaf, uint32_t subleaf,
                          uint32_t *eax, uint32_t *ebx,
                          uint32_t *ecx, uint32_t *edx)
{
    FirmwareMock *mock = opaque;

    mock->cpuid_count++;
    *eax = leaf + 0x100;
    *ebx = 0x22222222;
    *ecx = subleaf;
    *edx = 0x44444444;
    return 0;
}

static int dispatch_rdtscp(void *opaque, uint64_t *value, uint32_t *aux)
{
    FirmwareMock *mock = opaque;

    mock->rdtscp_count++;
    *value = mock->rdtscp_tsc;
    *aux = mock->rdtscp_aux;
    return 0;
}

static int dispatch_io(void *opaque, uint32_t port, bool is_input,
                       unsigned size, uint64_t *rax)
{
    FirmwareMock *mock = opaque;

    mock->io_count++;
    mock->last_port = port;
    g_assert_false(is_input);
    g_assert_cmpuint(size, ==, 1);
    mock->last_output = *rax;
    return 0;
}

static int dispatch_string_io(
    void *opaque, const amdvm_x86_guest_state_v1_t *state,
    uint64_t exit_info1, uint64_t next_rip,
    amdvm_x86_guest_state_v1_t *updated_state)
{
    FirmwareMock *mock = opaque;

    mock->string_io_count++;
    g_assert_cmphex(mock->stored.rip, ==, state->rip);
    g_assert_cmphex(mock->stored.gpr[AMDSVM_GPR_RSI], ==,
                    state->gpr[AMDSVM_GPR_RSI]);
    g_assert_cmphex(exit_info1, ==, UINT64_C(0x20ca4));
    g_assert_false((exit_info1 & 1u) != 0);
    g_assert_cmphex(next_rip, ==, state->rip + 2);
    *updated_state = *state;
    updated_state->rip = next_rip;
    updated_state->gpr[AMDSVM_GPR_RSI]++;
    mock->stored = *updated_state; /* Model CPUX86State mutation in handler. */
    return 0;
}

static AMDVMFirmwareDispatchAction dispatch_preentry_kick(void *opaque)
{
    FirmwareMock *mock = opaque;

    mock->preentry_kick_count++;
    return AMDVM_FIRMWARE_DISPATCH_CONTINUE;
}

static AMDVMFirmwareDispatchAction dispatch_physical_intr(void *opaque)
{
    FirmwareMock *mock = opaque;

    mock->physical_intr_count++;
    return mock->yield_on_physical_intr ?
           AMDVM_FIRMWARE_DISPATCH_YIELDED :
           AMDVM_FIRMWARE_DISPATCH_CONTINUE;
}

static void test_persistent_firmware_exit_loop(void)
{
    AMDVMProviderLifecycle lifecycle;
    FirmwareMock mock = { 0 };
    amdvm_vcpu_create_request_t create = { 0 };
    amdvm_vcpu_set_reset_rom_request_t rom = { 0 };
    amdvm_vcpu_memory_map_request_t map = { 0 };
    amdvm_vcpu_memory_map_result_t map_result = { 0 };
    amdvm_vcpu_run_request_t request = { 0 };
    amdvm_vcpu_run_result_t result = { 0 };
    const AMDVMFirmwareDispatchOps ops = {
        .store_state = dispatch_store,
        .complete_event = dispatch_complete,
        .emulate_cpuid = dispatch_cpuid,
        .emulate_io = dispatch_io,
        .handle_preentry_kick = dispatch_preentry_kick,
    };

    amdsvm_provider_lifecycle_init(&lifecycle, firmware_mock_transport,
                                   &mock);
    create.abi_version = AMDVM_VCPU_ABI_VERSION;
    map.abi_version = AMDVM_VCPU_ABI_VERSION;
    map.range_count = 1;
    g_assert_cmpint(amdsvm_provider_prepare(&lifecycle, &create, &rom,
                  &map, 1, &map_result), ==, 0);

    request.abi_version = AMDVM_VCPU_ABI_VERSION;
    request.state.rip = 0xfffffff0;
    request.state.rflags = 2;
    request.state.gpr[AMDSVM_GPR_RAX] = 7;
    for (unsigned int i = 0; i < 3; i++) {
        g_assert_cmpint(amdsvm_provider_run(&lifecycle, &request, &result), ==,
                        0);
        AMDVMFirmwareDispatchAction action =
            amdsvm_dispatch_firmware_exit(&request, &result, &ops, &mock);

        if (i < 2) {
            g_assert_cmpint(action, ==, AMDVM_FIRMWARE_DISPATCH_CONTINUE);
            request.state = mock.stored;
        } else {
            g_assert_cmpint(action, ==, AMDVM_FIRMWARE_DISPATCH_HALTED);
            g_assert_cmpuint(mock.stored.interruptibility, ==, 0);
            g_assert_cmphex(mock.stored.rip, ==, 0xfffffff5);
        }
        if (i == 0) {
            g_assert_cmphex(mock.stored.gpr[AMDSVM_GPR_RAX], ==, 0x107);
            g_assert_cmphex(mock.stored.rip, ==, 0xfffffff2);
        }
    }

    request.state = mock.stored;
    request.event_injection = 1;
    g_assert_cmpint(amdsvm_provider_run(&lifecycle, &request, &result), ==, 0);
    g_assert_cmpint(amdsvm_dispatch_firmware_exit(&request, &result, &ops,
                  &mock), ==, AMDVM_FIRMWARE_DISPATCH_CONTINUE);
    g_assert_true(mock.resumed_with_event);
    g_assert_cmpuint(mock.run_count, ==, 4);
    g_assert_cmpuint(mock.cpuid_count, ==, 2);
    g_assert_cmpuint(mock.io_count, ==, 1);
    g_assert_cmpuint(mock.last_port, ==, 0xe9);
    g_assert_cmpuint(mock.last_output, ==, 'A');
    g_assert_cmpuint(mock.complete_count, ==, 4);
    result.flags |= AMDVM_VCPU_RUN_KICKED;
    result.flags |= 1u << 8;
    g_assert_cmpint(amdsvm_dispatch_firmware_exit(&request, &result, &ops,
                  &mock), ==, AMDVM_FIRMWARE_DISPATCH_ERROR);
    g_assert_cmpint(amdsvm_provider_destroy(&lifecycle), ==, 0);
}

static void test_physical_interrupt_and_preentry_kick(void)
{
    FirmwareMock mock = { 0 };
    amdvm_vcpu_run_request_t request = { 0 };
    amdvm_vcpu_run_result_t result = { 0 };
    const AMDVMFirmwareDispatchOps ops = {
        .store_state = dispatch_store,
        .complete_event = dispatch_complete,
        .handle_physical_intr = dispatch_physical_intr,
        .handle_preentry_kick = dispatch_preentry_kick,
    };

    request.abi_version = AMDVM_VCPU_ABI_VERSION;
    request.state.rip = 0xfffffff2;
    request.state.rflags = 2;
    result.abi_version = AMDVM_VCPU_ABI_VERSION;
    result.flags = AMDVM_VCPU_RUN_CALLED;
    result.state = request.state;
    result.exit_code = AMDVM_VCPU_EXIT_PHYSICAL_INTR;

    g_assert_cmpint(amdsvm_dispatch_firmware_exit(&request, &result, &ops,
                  &mock), ==, AMDVM_FIRMWARE_DISPATCH_CONTINUE);
    g_assert_cmpuint(mock.physical_intr_count, ==, 1);
    g_assert_cmpuint(mock.complete_count, ==, 1);

    mock.yield_on_physical_intr = true;
    g_assert_cmpint(amdsvm_dispatch_firmware_exit(&request, &result, &ops,
                  &mock), ==, AMDVM_FIRMWARE_DISPATCH_YIELDED);
    g_assert_cmpuint(mock.physical_intr_count, ==, 2);
    g_assert_cmpuint(mock.complete_count, ==, 2);

    result.flags = AMDVM_VCPU_RUN_CALLED | AMDVM_VCPU_RUN_KICKED;
    g_assert_cmpint(amdsvm_dispatch_firmware_exit(&request, &result, &ops,
                  &mock), ==, AMDVM_FIRMWARE_DISPATCH_YIELDED);
    g_assert_cmpuint(mock.physical_intr_count, ==, 3);
    g_assert_cmpuint(mock.complete_count, ==, 3);

    result.flags = AMDVM_VCPU_RUN_KICKED;
    result.exit_code = UINT64_MAX;
    result.state = request.state;
    g_assert_cmpint(amdsvm_dispatch_firmware_exit(&request, &result, &ops,
                  &mock), ==, AMDVM_FIRMWARE_DISPATCH_CONTINUE);
    g_assert_cmpuint(mock.preentry_kick_count, ==, 1);
    g_assert_cmpuint(mock.complete_count, ==, 3);
}

static void test_string_io_dispatch(void)
{
    FirmwareMock mock = { 0 };
    amdvm_vcpu_run_request_t request = { 0 };
    amdvm_vcpu_run_result_t result = { 0 };
    const AMDVMFirmwareDispatchOps ops = {
        .store_state = dispatch_store,
        .complete_event = dispatch_complete,
        .emulate_string_io = dispatch_string_io,
    };

    request.abi_version = AMDVM_VCPU_ABI_VERSION;
    request.state.rip = 0x6fab;
    request.state.rflags = 2;
    result.abi_version = AMDVM_VCPU_ABI_VERSION;
    result.flags = AMDVM_VCPU_RUN_CALLED;
    result.state = request.state;
    result.state.rip = 0x6fb0;
    result.state.gpr[AMDSVM_GPR_RSI] = 41;
    mock.stored.rip = 0x1111; /* QEMU's prior state can be stale. */
    result.exit_code = AMDVM_VCPU_EXPECTED_IOIO_EXIT;
    result.exit_info1 = UINT64_C(0x20ca4);
    result.next_rip = result.state.rip + 2;

    g_assert_cmpint(amdsvm_dispatch_firmware_exit(&request, &result, &ops,
                  &mock), ==, AMDVM_FIRMWARE_DISPATCH_CONTINUE);
    g_assert_cmpuint(mock.string_io_count, ==, 1);
    g_assert_cmphex(mock.stored.rip, ==, result.next_rip);
    g_assert_cmpuint(mock.stored.gpr[AMDSVM_GPR_RSI], ==, 42);
    g_assert_cmpuint(mock.store_count, ==, 1);
    g_assert_cmpuint(mock.complete_count, ==, 1);

    mock.fail_store = true;
    g_assert_cmpint(amdsvm_dispatch_firmware_exit(&request, &result, &ops,
                  &mock), ==, AMDVM_FIRMWARE_DISPATCH_ERROR);
    g_assert_cmpuint(mock.string_io_count, ==, 1);
    g_assert_cmpuint(mock.store_count, ==, 2);
}

static void test_pause_dispatch(void)
{
    FirmwareMock mock = { 0 };
    amdvm_vcpu_run_request_t request = { 0 };
    amdvm_vcpu_run_result_t result = { 0 };
    const AMDVMFirmwareDispatchOps ops = {
        .store_state = dispatch_store,
        .complete_event = dispatch_complete,
    };

    request.abi_version = AMDVM_VCPU_ABI_VERSION;
    request.state.rip = 0x123400;
    result.abi_version = AMDVM_VCPU_ABI_VERSION;
    result.flags = AMDVM_VCPU_RUN_CALLED;
    result.state = request.state;
    result.exit_code = SVM_EXIT_PAUSE;
    result.next_rip = request.state.rip + 2;

    g_assert_cmpint(amdsvm_dispatch_firmware_exit(&request, &result, &ops,
                  &mock), ==, AMDVM_FIRMWARE_DISPATCH_CONTINUE);
    g_assert_cmphex(mock.stored.rip, ==, request.state.rip + 2);
    g_assert_cmpuint(mock.complete_count, ==, 1);

    result.next_rip++;
    g_assert_cmpint(amdsvm_dispatch_firmware_exit(&request, &result, &ops,
                  &mock), ==, AMDVM_FIRMWARE_DISPATCH_ERROR);
}

static void test_svm_exit_err_is_completed_reset_exit(void)
{
    amdvm_vcpu_run_request_t request = { 0 };
    amdvm_vcpu_run_result_t result = { 0 };
    const AMDVMFirmwareDispatchOps ops = { 0 };
    FirmwareMock mock = { 0 };

    request.state.abi_version = AMDVM_X86_STATE_ABI_VERSION;
    result.abi_version = AMDVM_VCPU_ABI_VERSION;
    result.flags = AMDVM_VCPU_RUN_CALLED;
    result.owner_status = 0;
    result.vmrun_status = 0;
    result.exit_code = SVM_EXIT_ERR;
    g_assert_cmpint(amdsvm_dispatch_firmware_exit(&request, &result, &ops,
                  &mock), ==, AMDVM_FIRMWARE_DISPATCH_RESET);
    g_assert_cmpuint(mock.complete_count, ==, 0);

    result.exit_code = SVM_EXIT_NOT_RUN;
    g_assert_cmpint(amdsvm_dispatch_firmware_exit(&request, &result, &ops,
                  &mock), ==, AMDVM_FIRMWARE_DISPATCH_ERROR);
}

static void test_emulated_exit_commits_state_once(void)
{
    amdvm_vcpu_run_request_t request = { 0 };
    amdvm_vcpu_run_result_t result = { 0 };
    FirmwareMock mock = { 0 };
    const AMDVMFirmwareDispatchOps ops = {
        .store_state = dispatch_store,
        .complete_event = dispatch_complete,
        .emulate_cpuid = dispatch_cpuid,
    };

    request.state.abi_version = AMDVM_X86_STATE_ABI_VERSION;
    result.abi_version = AMDVM_VCPU_ABI_VERSION;
    result.flags = AMDVM_VCPU_RUN_CALLED;
    result.owner_status = 0;
    result.vmrun_status = 0;
    result.exit_code = AMDVM_VCPU_EXPECTED_CPUID_EXIT;
    result.state = request.state;
    result.state.rip = 0x1000;
    result.state.gpr[AMDSVM_GPR_RAX] = 7;
    result.next_rip = 0x1002;
    g_assert_cmpint(amdsvm_dispatch_firmware_exit(&request, &result, &ops,
                  &mock), ==, AMDVM_FIRMWARE_DISPATCH_CONTINUE);
    g_assert_cmpuint(mock.complete_count, ==, 1);
    g_assert_cmpuint(mock.store_count, ==, 1);
    g_assert_cmphex(mock.stored.rip, ==, 0x1002);
}

static void test_wbinvd_dispatch(void)
{
    FirmwareMock mock = { 0 };
    amdvm_vcpu_run_request_t request = { 0 };
    amdvm_vcpu_run_result_t result = { 0 };
    const AMDVMFirmwareDispatchOps ops = {
        .store_state = dispatch_store,
        .complete_event = dispatch_complete,
    };

    request.abi_version = AMDVM_VCPU_ABI_VERSION;
    request.state.rip = 0x234500;
    result.abi_version = AMDVM_VCPU_ABI_VERSION;
    result.flags = AMDVM_VCPU_RUN_CALLED;
    result.state = request.state;
    result.exit_code = SVM_EXIT_WBINVD;
    result.next_rip = request.state.rip + 2;

    g_assert_cmpint(amdsvm_dispatch_firmware_exit(&request, &result, &ops,
                  &mock), ==, AMDVM_FIRMWARE_DISPATCH_CONTINUE);
    g_assert_cmphex(mock.stored.rip, ==, request.state.rip + 2);
    g_assert_cmpuint(mock.complete_count, ==, 1);

    result.exit_info1 = 1;
    g_assert_cmpint(amdsvm_dispatch_firmware_exit(&request, &result, &ops,
                  &mock), ==, AMDVM_FIRMWARE_DISPATCH_ERROR);
}

static void test_rdtscp_dispatch(void)
{
    FirmwareMock mock = {
        .rdtscp_tsc = UINT64_C(0x12345678abcdef01),
        .rdtscp_aux = UINT32_C(0x76543210),
    };
    amdvm_vcpu_run_request_t request = { 0 };
    amdvm_vcpu_run_result_t result = { 0 };
    const AMDVMFirmwareDispatchOps ops = {
        .store_state = dispatch_store,
        .complete_event = dispatch_complete,
        .emulate_rdtscp = dispatch_rdtscp,
    };

    request.abi_version = AMDVM_VCPU_ABI_VERSION;
    request.state.rip = 0x123400;
    request.state.rflags = 2;
    result.abi_version = AMDVM_VCPU_ABI_VERSION;
    result.flags = AMDVM_VCPU_RUN_CALLED;
    result.state = request.state;
    result.exit_code = SVM_EXIT_RDTSCP;
    result.next_rip = request.state.rip + 3;

    g_assert_cmpint(amdsvm_dispatch_firmware_exit(&request, &result, &ops,
                  &mock), ==, AMDVM_FIRMWARE_DISPATCH_CONTINUE);
    g_assert_cmpuint(mock.rdtscp_count, ==, 1);
    g_assert_cmphex(mock.stored.rip, ==, request.state.rip + 3);
    g_assert_cmphex(mock.stored.gpr[AMDSVM_GPR_RAX], ==, 0xabcdef01);
    g_assert_cmphex(mock.stored.gpr[AMDSVM_GPR_RDX], ==, 0x12345678);
    g_assert_cmphex(mock.stored.gpr[AMDSVM_GPR_RCX], ==, 0x76543210);

    result.state.cr[4] = UINT64_C(1) << 2;
    result.state.segments[AMDVM_X86_SEG_CS].selector = 3;
    g_assert_cmpint(amdsvm_dispatch_firmware_exit(&request, &result, &ops,
                  &mock), ==, AMDVM_FIRMWARE_DISPATCH_ERROR);
    g_assert_cmpuint(mock.rdtscp_count, ==, 1);

    result.state.segments[AMDVM_X86_SEG_CS].selector = 0;
    result.next_rip++;
    g_assert_cmpint(amdsvm_dispatch_firmware_exit(&request, &result, &ops,
                  &mock), ==, AMDVM_FIRMWARE_DISPATCH_ERROR);
    g_assert_cmpuint(mock.rdtscp_count, ==, 1);
}

static void test_nmi_iret_dispatch(void)
{
    FirmwareMock mock = { 0 };
    amdvm_vcpu_run_request_t request = { 0 };
    amdvm_vcpu_run_result_t result = { 0 };
    const AMDVMFirmwareDispatchOps ops = {
        .store_state = dispatch_store,
        .complete_event = dispatch_complete,
        .handle_iret = dispatch_iret,
    };

    request.abi_version = AMDVM_VCPU_ABI_VERSION;
    request.state.rip = 0x123450;
    result.abi_version = AMDVM_VCPU_ABI_VERSION;
    result.flags = AMDVM_VCPU_RUN_CALLED;
    result.state = request.state;
    result.state.interruptibility = AMDVM_X86_INTERRUPTIBILITY_NMI_BLOCKED;
    result.exit_code = SVM_EXIT_IRET;

    g_assert_cmpint(amdsvm_dispatch_firmware_exit(&request, &result, &ops,
                  &mock), ==, AMDVM_FIRMWARE_DISPATCH_CONTINUE);
    g_assert_cmpuint(mock.iret_count, ==, 1);
    g_assert_true(mock.events.nmi_iret_step);

    /* The provider's internal #DB must be consumed only for its BS step. */
    request.state = mock.stored;
    result.state = request.state;
    result.state.rip++;
    result.state.rflags |= (1ull << 8) | (1ull << 16);
    result.state.dr[6] = 1ull << 14;
    result.exit_code = AMDVM_VCPU_EXPECTED_DB_EXIT;
    g_assert_cmpint(amdsvm_dispatch_firmware_exit(&request, &result, &ops,
                  &mock), ==, AMDVM_FIRMWARE_DISPATCH_CONTINUE);
    g_assert_false(mock.events.nmi_iret_step);
    g_assert_false(mock.stored.interruptibility &
                   AMDVM_X86_INTERRUPTIBILITY_NMI_BLOCKED);
    g_assert_cmpuint(mock.stored.rflags & ((1ull << 8) | (1ull << 16)), ==, 0);

    /* A guest breakpoint cause must fail closed and retain the step state. */
    mock.events.nmi_iret_step = true;
    mock.events.nmi_iret_rip = 0x123460;
    mock.events.nmi_iret_guest_rflags = 2u;
    request.state = mock.stored;
    result.state = request.state;
    result.state.rip = 0x123461;
    result.state.interruptibility = AMDVM_X86_INTERRUPTIBILITY_NMI_BLOCKED |
        AMDVM_X86_INTERRUPTIBILITY_NMI_IRET_STEP;
    result.state.rflags |= (1ull << 8) | (1ull << 16);
    result.state.dr[6] = (1ull << 14) | 1u;
    result.exit_code = AMDVM_VCPU_EXPECTED_DB_EXIT;
    g_assert_cmpint(amdsvm_dispatch_firmware_exit(&request, &result, &ops,
                  &mock), ==, AMDVM_FIRMWARE_DISPATCH_ERROR);
    g_assert_true(mock.events.nmi_iret_step);
}

static void test_xsetbv_values(void)
{
    g_assert_true(amdsvm_xsetbv_value_valid(1, 0, 0, 7));
    g_assert_true(amdsvm_xsetbv_value_valid(3, 0, 0, 7));
    g_assert_true(amdsvm_xsetbv_value_valid(7, 0, 0, 7));
    g_assert_false(amdsvm_xsetbv_value_valid(0, 0, 0, 7));
    g_assert_false(amdsvm_xsetbv_value_valid(5, 0, 0, 7));
    g_assert_false(amdsvm_xsetbv_value_valid(7, 0, 0, 3));
    g_assert_false(amdsvm_xsetbv_value_valid(3, 1, 0, 7));
    g_assert_false(amdsvm_xsetbv_value_valid(3, 0, 3, 7));
    g_assert_false(amdsvm_xsetbv_value_valid(0x100000007ull, 0, 0, 7));
}

static void test_cpuid_xstate_profile(void)
{
    uint32_t eax, ebx, ecx, edx;

    eax = ebx = ecx = edx = UINT32_MAX;
    amdsvm_cpuid_mask_xstate_profile(0x0d, 0, 1,
                                     &eax, &ebx, &ecx, &edx);
    g_assert_cmphex(eax, ==, 0x7);
    g_assert_cmphex(ebx, ==, 0x240);
    g_assert_cmphex(ecx, ==, 0x340);
    g_assert_cmphex(edx, ==, 0);

    eax = ebx = ecx = edx = UINT32_MAX;
    amdsvm_cpuid_mask_xstate_profile(0x0d, 0, 7,
                                     &eax, &ebx, &ecx, &edx);
    g_assert_cmphex(ebx, ==, 0x340);

    eax = ebx = ecx = edx = UINT32_MAX;
    amdsvm_cpuid_mask_xstate_profile(0x0d, 1, 7,
                                     &eax, &ebx, &ecx, &edx);
    g_assert_cmphex(eax, ==, 0x7);
    g_assert_cmphex(ebx | ecx | edx, ==, 0);

    eax = ebx = ecx = edx = UINT32_MAX;
    amdsvm_cpuid_mask_xstate_profile(0x0d, 2, 7,
                                     &eax, &ebx, &ecx, &edx);
    g_assert_cmphex(eax, ==, 0x100);
    g_assert_cmphex(ebx, ==, 0x240);
    g_assert_cmphex(ecx | edx, ==, 0);

    eax = ebx = ecx = edx = UINT32_MAX;
    amdsvm_cpuid_mask_xstate_profile(0x0d, 3, 7,
                                     &eax, &ebx, &ecx, &edx);
    g_assert_cmphex(eax | ebx | ecx | edx, ==, 0);

    eax = ebx = ecx = edx = UINT32_MAX;
    amdsvm_cpuid_mask_xstate_profile(7, 0, 7,
                                     &eax, &ebx, &ecx, &edx);
    g_assert_cmphex(ebx & ((1u << 14) | (1u << 16) | (1u << 31)), ==, 0);
    g_assert_cmphex(ecx & ((1u << 3) | (1u << 4) | (1u << 7)), ==, 0);
    g_assert_cmphex(edx & ((1u << 20) | (1u << 24)), ==, 0);

    eax = ebx = ecx = edx = UINT32_MAX;
    amdsvm_cpuid_mask_xstate_profile(7, 1, 7,
                                     &eax, &ebx, &ecx, &edx);
    g_assert_cmphex(eax & ((1u << 5) | (1u << 21)), ==, 0);
    g_assert_cmphex(edx & ((1u << 8) | (1u << 19) | (1u << 21)), ==, 0);

    eax = ebx = ecx = edx = UINT32_MAX;
    amdsvm_cpuid_mask_xstate_profile(0x1d, 0, 7,
                                     &eax, &ebx, &ecx, &edx);
    g_assert_cmphex(eax | ebx | ecx | edx, ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/amdsvm/firmware/persistent-exit-loop",
                    test_persistent_firmware_exit_loop);
    g_test_add_func("/amdsvm/firmware/physical-interrupt-and-preentry-kick",
                    test_physical_interrupt_and_preentry_kick);
    g_test_add_func("/amdsvm/firmware/string-io-dispatch",
                    test_string_io_dispatch);
    g_test_add_func("/amdsvm/firmware/pause-dispatch",
                    test_pause_dispatch);
    g_test_add_func("/amdsvm/firmware/svm-exit-err-reset",
                    test_svm_exit_err_is_completed_reset_exit);
    g_test_add_func("/amdsvm/firmware/emulated-exit-store-once",
                    test_emulated_exit_commits_state_once);
    g_test_add_func("/amdsvm/firmware/wbinvd-dispatch",
                    test_wbinvd_dispatch);
    g_test_add_func("/amdsvm/firmware/rdtscp-dispatch",
                    test_rdtscp_dispatch);
    g_test_add_func("/amdsvm/firmware/nmi-iret-dispatch",
                    test_nmi_iret_dispatch);
    g_test_add_func("/amdsvm/firmware/diagnostic-page-fault-reinjection",
                    test_diagnostic_page_fault_reinjection);
    g_test_add_func("/amdsvm/firmware/xsetbv-values", test_xsetbv_values);
    g_test_add_func("/amdsvm/firmware/cpuid-xstate-profile",
                    test_cpuid_xstate_profile);
    return g_test_run();
}
