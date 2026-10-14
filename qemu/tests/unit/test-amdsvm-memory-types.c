#include "qemu/osdep.h"
#include "target/i386/amdsvm/amdsvm-memory.h"

static void test_alpine_tcg_trace(void)
{
    AMDVMGuestMemoryMap map = { 0 };
    AMDVMMTRRState mtrr = { 0 };
    amdvm_vcpu_set_memory_types_request_t request = { 0 };
    Error *err = NULL;

    map.count = 4;
    map.reset_rom_page_present = true;
    map.ranges[0] = (AMDVMGuestMemoryRange) {
        .guest_physical_address = 0,
        .size = 0x80000,
    };
    map.ranges[1] = (AMDVMGuestMemoryRange) {
        .guest_physical_address = 0x80000,
        .size = 0x20000,
    };
    map.ranges[2] = (AMDVMGuestMemoryRange) {
        .guest_physical_address = 0xa0000,
        .size = 0x20000,
    };
    map.ranges[3] = (AMDVMGuestMemoryRange) {
        .guest_physical_address = 0xc0000,
        .size = 0x40000,
    };

    mtrr.phys_bits = 40;
    mtrr.fixed_supported = true;
    mtrr.default_type = AMDVM_MTRR_DEFAULT_ENABLE |
        AMDVM_MTRR_FIXED_ENABLE | AMDVM_MTRR_TYPE_WB;
    mtrr.fixed[0] = UINT64_C(0x0606060606060606);
    mtrr.fixed[1] = UINT64_C(0x0606060606060606);
    mtrr.fixed[2] = 0; /* TCG trace: A0000-BFFFF is UC. */
    for (unsigned i = 3; i < AMDVM_MTRR_FIXED_COUNT; i++) {
        mtrr.fixed[i] = UINT64_C(0x0505050505050505);
    }
    mtrr.variable[0].base = UINT64_C(0x80000000) | AMDVM_MTRR_TYPE_UC;
    mtrr.variable[0].mask = UINT64_C(0xff80000800); /* 2-4 GiB UC. */

    g_assert_true(amdsvm_build_memory_type_request(&map, &mtrr,
                                                   AMDVM_GUEST_MTRR_EXACT,
                                                   &request, &err));
    g_assert_null(err);
    g_assert_cmpuint(request.abi_version, ==, AMDVM_VCPU_ABI_VERSION);
    g_assert_cmpuint(request.range_count, ==, 5);
    g_assert_cmpuint(request.ranges[0].guest_physical_address, ==, 0);
    g_assert_cmpuint(request.ranges[0].size, ==, 0x80000);
    g_assert_cmpuint(request.ranges[0].memory_type, ==, AMDVM_MTRR_TYPE_WB);
    g_assert_cmpuint(request.ranges[1].guest_physical_address, ==, 0x80000);
    g_assert_cmpuint(request.ranges[1].size, ==, 0x20000);
    g_assert_cmpuint(request.ranges[1].memory_type, ==, AMDVM_MTRR_TYPE_WB);
    g_assert_cmpuint(request.ranges[2].guest_physical_address, ==, 0xa0000);
    g_assert_cmpuint(request.ranges[2].size, ==, 0x20000);
    g_assert_cmpuint(request.ranges[2].memory_type, ==, AMDVM_MTRR_TYPE_UC);
    g_assert_cmpuint(request.ranges[3].guest_physical_address, ==, 0xc0000);
    g_assert_cmpuint(request.ranges[3].size, ==, 0x40000);
    g_assert_cmpuint(request.ranges[3].memory_type, ==, AMDVM_MTRR_TYPE_WP);
    g_assert_cmpuint(request.ranges[4].guest_physical_address,
                     ==, AMDVM_VCPU_RESET_ROM_GPA);
    g_assert_cmpuint(request.ranges[4].memory_type, ==, AMDVM_MTRR_TYPE_UC);

    g_assert_true(amdsvm_build_memory_type_request(&map, &mtrr,
                                                   AMDVM_GUEST_MTRR_COMPAT_ALL_WB,
                                                   &request, &err));
    g_assert_null(err);
    g_assert_cmpuint(request.range_count, ==, 5);
    for (unsigned i = 0; i < request.range_count; i++) {
        g_assert_cmpuint(request.ranges[i].memory_type, ==,
                         AMDVM_MTRR_TYPE_WB);
    }
}

static void test_undefined_overlap_fails_closed(void)
{
    AMDVMGuestMemoryMap map = { 0 };
    AMDVMMTRRState mtrr = { 0 };
    amdvm_vcpu_set_memory_types_request_t request = { 0 };
    Error *err = NULL;
    const uint64_t mask = UINT64_C(0x0000000fff000800);

    map.count = 1;
    map.reset_rom_page_present = true;
    map.ranges[0] = (AMDVMGuestMemoryRange) {
        .guest_physical_address = 0x10000000,
        .size = 0x1000,
    };
    mtrr.phys_bits = 36;
    mtrr.default_type = AMDVM_MTRR_DEFAULT_ENABLE | AMDVM_MTRR_TYPE_WB;
    mtrr.variable[0] = (AMDVMMTRRVariableRange) {
        .base = 0x10000000 | AMDVM_MTRR_TYPE_WB,
        .mask = mask,
    };
    mtrr.variable[1] = (AMDVMMTRRVariableRange) {
        .base = 0x10000000 | AMDVM_MTRR_TYPE_WC,
        .mask = mask,
    };

    g_assert_false(amdsvm_build_memory_type_request(&map, &mtrr,
                                                    AMDVM_GUEST_MTRR_EXACT,
                                                    &request, &err));
    g_assert_nonnull(err);
    g_assert_cmpuint(request.range_count, ==, 0);
    error_free(err);
}

static void test_provider_split_ranges_remain_exact_in_memory_type_map(void)
{
    AMDVMGuestMemoryMap map = { 0 };
    AMDVMMTRRState mtrr = { 0 };
    amdvm_vcpu_set_memory_types_request_t request = { 0 };
    Error *err = NULL;
    const uint64_t first = AMDVM_VCPU_MEMORY_MAX_RANGE_BYTES;
    const uint64_t remainder = (UINT64_C(5) << 30) - first;

    /* The provider pins each descriptor separately; MTRR/NPT ranges must
     * preserve those same boundaries even when adjacent GPAs are continuous. */
    map.count = 2;
    map.reset_rom_page_present = true;
    map.ranges[0] = (AMDVMGuestMemoryRange) {
        .guest_physical_address = UINT64_C(4) << 30,
        .host_virtual_address = UINT64_C(16) << 30,
        .size = first,
        .flags = AMDSVM_GUEST_RANGE_READ | AMDSVM_GUEST_RANGE_WRITE,
    };
    map.ranges[1] = (AMDVMGuestMemoryRange) {
        .guest_physical_address = (UINT64_C(4) << 30) + first,
        .host_virtual_address = (UINT64_C(16) << 30) + first,
        .size = remainder,
        .flags = AMDSVM_GUEST_RANGE_READ | AMDSVM_GUEST_RANGE_WRITE,
    };
    mtrr.phys_bits = 48;
    mtrr.default_type = AMDVM_MTRR_DEFAULT_ENABLE | AMDVM_MTRR_TYPE_WB;

    g_assert_true(amdsvm_build_memory_type_request(
        &map, &mtrr, AMDVM_GUEST_MTRR_COMPAT_ALL_WB, &request, &err));
    g_assert_null(err);
    g_assert_cmpuint(request.range_count, ==, 3);
    g_assert_cmpuint(request.ranges[0].guest_physical_address,
                     ==, AMDVM_VCPU_RESET_ROM_GPA);
    g_assert_cmpuint(request.ranges[0].size, ==, AMDSVM_GUEST_PAGE_SIZE);
    g_assert_cmpuint(request.ranges[1].guest_physical_address,
                     ==, map.ranges[0].guest_physical_address);
    g_assert_cmpuint(request.ranges[1].size, ==, first);
    g_assert_cmpuint(request.ranges[2].guest_physical_address,
                     ==, map.ranges[1].guest_physical_address);
    g_assert_cmpuint(request.ranges[2].size, ==, remainder);
    g_assert_cmpuint(request.ranges[1].size + request.ranges[2].size,
                     ==, UINT64_C(5) << 30);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/amdsvm/memory-types/alpine-tcg-trace",
                    test_alpine_tcg_trace);
    g_test_add_func("/amdsvm/memory-types/undefined-overlap",
                    test_undefined_overlap_fails_closed);
    g_test_add_func("/amdsvm/memory-types/provider-split-ranges",
                    test_provider_split_ranges_remain_exact_in_memory_type_map);
    return g_test_run();
}
