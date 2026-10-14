/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"

#include "../../target/i386/amdsvm/amdsvm-memory-map.h"

static void test_eight_gib_splits_into_pin_safe_ranges(void)
{
    amdvm_vcpu_memory_map_request_t request = { 0 };
    const uint64_t eight_gib = UINT64_C(8) * 1024 * 1024 * 1024;
    uint64_t guest = 0x100000;
    uint64_t user = 0x100000000;
    uint64_t total = 0;

    /* Eight GiB is a supported request, not the ABI's configured ceiling. */
    g_assert_cmpuint(AMDVM_VCPU_MEMORY_MAX_GUEST_RAM_BYTES, >=, eight_gib);
    g_assert_cmpuint(AMDVM_VCPU_MEMORY_MAX_TOTAL_BYTES, ==,
                     AMDVM_VCPU_MEMORY_MAX_GUEST_RAM_BYTES +
                         AMDVM_VCPU_MEMORY_ROM_ALIAS_ALLOWANCE_BYTES);
    g_assert_cmpint(amdsvm_memory_map_append_range(&request, guest, user,
                  eight_gib,
                  AMDVM_VCPU_MEMORY_READ | AMDVM_VCPU_MEMORY_WRITE), ==, 0);
    g_assert_cmpuint(request.range_count, ==, 3);

    for (uint32_t i = 0; i < request.range_count; i++) {
        const amdvm_vcpu_memory_range_t *range = &request.ranges[i];

        g_assert_cmpuint(range->guest_physical_address, ==, guest + total);
        g_assert_cmpuint(range->user_virtual_address, ==, user + total);
        g_assert_cmpuint(range->size, >, 0);
        g_assert_cmpuint(range->size, <=, AMDVM_VCPU_MEMORY_MAX_RANGE_BYTES);
        g_assert_cmpuint(range->size & 4095u, ==, 0);
        g_assert_cmpuint(range->flags, ==,
                         AMDVM_VCPU_MEMORY_READ | AMDVM_VCPU_MEMORY_WRITE);
        g_assert_cmpuint(range->reserved, ==, 0);
        total += range->size;
    }
    g_assert_cmpuint(total, ==, eight_gib);
    g_assert_cmpuint(request.ranges[2].size, ==, 8192);

    /* Read-only PC firmware aliases may add bounded mapping overhead. */
    g_assert_cmpint(amdsvm_memory_map_append_range(&request,
                  guest + total, user + total,
                  AMDVM_VCPU_MEMORY_ROM_ALIAS_ALLOWANCE_BYTES,
                  AMDVM_VCPU_MEMORY_READ | AMDVM_VCPU_MEMORY_EXEC), ==, 0);
    g_assert_cmpuint(request.range_count, ==, 4);
    g_assert_cmpuint(total + request.ranges[3].size, ==,
                     eight_gib +
                         AMDVM_VCPU_MEMORY_ROM_ALIAS_ALLOWANCE_BYTES);

    /* A firmware alias does not consume writable-RAM capacity. */
    g_assert_cmpint(amdsvm_memory_map_append_range(&request,
                  guest + total + request.ranges[3].size,
                  user + total + request.ranges[3].size, 4096,
                  AMDVM_VCPU_MEMORY_READ | AMDVM_VCPU_MEMORY_WRITE), ==,
                  0);
    g_assert_cmpuint(request.range_count, ==, 5);
}

static void test_sixteen_gib_is_not_limited_by_old_eight_gib_preset(void)
{
    amdvm_vcpu_memory_map_request_t request = { 0 };
    const uint64_t sixteen_gib = UINT64_C(16) * 1024 * 1024 * 1024;

    g_assert_cmpint(amdsvm_memory_map_append_range(&request, 0x100000,
                  0x100000000, sixteen_gib,
                  AMDVM_VCPU_MEMORY_READ | AMDVM_VCPU_MEMORY_WRITE), ==, 0);
    g_assert_cmpuint(request.range_count, ==, 5);
    g_assert_cmpuint(request.ranges[4].size, ==, 16384);
}

static void test_negotiated_writable_capacity_fails_atomically(void)
{
    amdvm_vcpu_memory_map_request_t request = { 0 };

    g_assert_cmpint(amdsvm_memory_map_append_range(&request, 0x100000,
                  0x100000000, AMDVM_VCPU_MEMORY_MAX_GUEST_RAM_BYTES,
                  AMDVM_VCPU_MEMORY_READ | AMDVM_VCPU_MEMORY_WRITE), ==, 0);
    g_assert_cmpuint(request.range_count, ==, AMDVM_VCPU_MEMORY_MAX_RANGES);
    g_assert_cmpint(amdsvm_memory_map_append_range(&request,
                  0x100000 + AMDVM_VCPU_MEMORY_MAX_GUEST_RAM_BYTES,
                  0x100000000 + AMDVM_VCPU_MEMORY_MAX_GUEST_RAM_BYTES, 4096,
                  AMDVM_VCPU_MEMORY_READ | AMDVM_VCPU_MEMORY_WRITE), ==,
                  -E2BIG);
    g_assert_cmpuint(request.range_count, ==, AMDVM_VCPU_MEMORY_MAX_RANGES);
}

static void test_one_two_gib_synthetic_contiguous_span_fits_one_descriptor(void)
{
    const uint64_t sizes[] = {
        UINT64_C(1) * 1024 * 1024 * 1024,
        UINT64_C(2) * 1024 * 1024 * 1024,
    };

    for (size_t i = 0; i < ARRAY_SIZE(sizes); i++) {
        amdvm_vcpu_memory_map_request_t request = { 0 };

        g_assert_cmpint(amdsvm_memory_map_append_range(
                            &request, 0x100000, 0x100000000, sizes[i],
                            AMDVM_VCPU_MEMORY_READ |
                                AMDVM_VCPU_MEMORY_WRITE), ==, 0);
        g_assert_cmpuint(request.range_count, ==, 1);
        g_assert_cmpuint(request.ranges[0].guest_physical_address, ==,
                         0x100000);
        g_assert_cmpuint(request.ranges[0].user_virtual_address, ==,
                         0x100000000);
        g_assert_cmpuint(request.ranges[0].size, ==, sizes[i]);
        g_assert_cmpuint(request.ranges[0].reserved, ==, 0);
    }
}

static void test_capacity_failure_is_atomic(void)
{
    amdvm_vcpu_memory_map_request_t request = { 0 };
    const uint32_t existing = AMDVM_VCPU_MEMORY_MAX_RANGES - 1;
    const uint64_t existing_bytes = (uint64_t)existing * 4096;

    request.range_count = existing;
    for (uint32_t i = 0; i < existing; i++) {
        request.ranges[i].size = 4096;
    }
    request.ranges[existing].size = 123;
    g_assert_cmpint(amdsvm_memory_map_append_range(&request, 0x200000,
                  0x200000, AMDVM_VCPU_MEMORY_MAX_TOTAL_BYTES - existing_bytes,
                  AMDVM_VCPU_MEMORY_READ), ==, -ENOSPC);
    g_assert_cmpuint(request.range_count, ==, existing);
    g_assert_cmpuint(request.ranges[existing].size, ==, 123);
}

static void test_total_cap_and_overflow_fail_closed(void)
{
    amdvm_vcpu_memory_map_request_t request = { 0 };

    g_assert_cmpint(amdsvm_memory_map_append_range(&request, 0x1000,
                  0x100000, AMDVM_VCPU_MEMORY_MAX_TOTAL_BYTES + 4096,
                  AMDVM_VCPU_MEMORY_READ), ==, -E2BIG);
    g_assert_cmpuint(request.range_count, ==, 0);
    g_assert_cmpint(amdsvm_memory_map_append_range(&request, UINT64_MAX - 4095,
                  0x100000, 8192, AMDVM_VCPU_MEMORY_READ), ==, -EINVAL);
    g_assert_cmpuint(request.range_count, ==, 0);
}

static void test_only_immutable_rom_backing_may_alias(void)
{
    const uint32_t rom = AMDVM_VCPU_MEMORY_READ | AMDVM_VCPU_MEMORY_EXEC;
    const uint32_t ram = rom | AMDVM_VCPU_MEMORY_WRITE;

    g_assert_true(amdsvm_memory_backing_alias_allowed(rom, rom));
    g_assert_false(amdsvm_memory_backing_alias_allowed(rom, ram));
    g_assert_false(amdsvm_memory_backing_alias_allowed(ram, ram));
    g_assert_false(amdsvm_memory_backing_alias_allowed(
        AMDVM_VCPU_MEMORY_READ, AMDVM_VCPU_MEMORY_READ));
}

static void append_pc_1g_memory_map(AMDVMGuestMemoryMap *map,
                                   uint64_t rom_hva,
                                   uint64_t low_bios_hva,
                                   uint64_t bios_alias_hva,
                                   const uint8_t *bios_backing)
{
    const uint32_t ram = AMDSVM_GUEST_RANGE_READ |
                         AMDSVM_GUEST_RANGE_WRITE |
                         AMDSVM_GUEST_RANGE_EXEC;
    const uint32_t rom = AMDSVM_GUEST_RANGE_READ |
                         AMDSVM_GUEST_RANGE_EXEC;
    Error *err = NULL;

    g_assert_true(amdsvm_memory_map_append_flatview_range(
        map, 0, 0x100000000, 0xc0000, ram, NULL, &err));
    g_assert_null(err);
    g_assert_true(amdsvm_memory_map_append_flatview_range(
        map, 0xc0000, rom_hva, 0x20000, rom, NULL, &err));
    g_assert_null(err);
    g_assert_true(amdsvm_memory_map_append_flatview_range(
        map, 0xe0000, low_bios_hva, 0x20000, rom, NULL, &err));
    g_assert_null(err);
    g_assert_true(amdsvm_memory_map_append_flatview_range(
        map, 0x100000, 0x110000000, 0x3ff00000, ram, NULL, &err));
    g_assert_null(err);
    g_assert_true(amdsvm_memory_map_append_flatview_range(
        map, 0xfffc0000, bios_alias_hva, 0x40000, rom,
        bios_backing, &err));
    g_assert_null(err);
}

static void test_pc_1g_flatview_splits_reset_rom_and_coalesces_by_hva(void)
{
    const uint64_t bios_alias_hva = UINT64_C(0x300000000);
    AMDVMGuestMemoryMap separated = { 0 };
    AMDVMGuestMemoryMap contiguous = { 0 };
    uint8_t *bios = g_malloc0(0x40000);

    for (uint32_t i = 0; i < 0x1000; i++) {
        bios[0x3f000 + i] = (uint8_t)(i * 17u + 3u);
    }

    append_pc_1g_memory_map(&separated, 0x200000000,
                            bios_alias_hva + 0x20000, bios_alias_hva, bios);
    g_assert_cmpuint(separated.count, ==, 5);
    g_assert_cmpuint(separated.total_ram, ==, UINT64_C(0x4003f000));
    g_assert_cmpuint(separated.total_writable_bytes, ==,
                     UINT64_C(0x3ffc0000));
    g_assert_true(separated.reset_rom_page_present);
    g_assert_cmpmem(separated.reset_rom_page,
                    sizeof(separated.reset_rom_page), bios + 0x3f000,
                    sizeof(separated.reset_rom_page));
    g_assert_cmpuint(separated.ranges[4].guest_physical_address, ==,
                     UINT64_C(0xfffc0000));
    g_assert_cmpuint(separated.ranges[4].size, ==, UINT64_C(0x3f000));

    /* The only adjacent same-permission PC spans are the two low ROMs. */
    append_pc_1g_memory_map(&contiguous, bios_alias_hva,
                            bios_alias_hva + 0x20000,
                            bios_alias_hva, bios);
    g_assert_cmpuint(contiguous.count, ==, 4);
    g_assert_cmpuint(contiguous.ranges[1].guest_physical_address, ==,
                     UINT64_C(0xc0000));
    g_assert_cmpuint(contiguous.ranges[1].host_virtual_address, ==,
                     bios_alias_hva);
    g_assert_cmpuint(contiguous.ranges[1].size, ==, UINT64_C(0x40000));
    g_assert_cmpuint(contiguous.total_ram, ==, separated.total_ram);
    g_assert_cmpuint(contiguous.total_writable_bytes, ==,
                     separated.total_writable_bytes);
    g_assert_true(contiguous.reset_rom_page_present);
    g_assert_cmpmem(contiguous.reset_rom_page,
                    sizeof(contiguous.reset_rom_page), bios + 0x3f000,
                    sizeof(contiguous.reset_rom_page));

    /* A writable section cannot supply the firmware reset-vector page. */
    AMDVMGuestMemoryMap rejected = { 0 };
    Error *err = NULL;
    g_assert_false(amdsvm_memory_map_append_flatview_range(
        &rejected, AMDVM_VCPU_RESET_ROM_GPA, 0x400000000, 0x1000,
        AMDSVM_GUEST_RANGE_READ | AMDSVM_GUEST_RANGE_WRITE,
        bios, &err));
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmpuint(rejected.count, ==, 0);
    g_assert_cmpuint(rejected.total_ram, ==, 0);
    g_assert_false(rejected.reset_rom_page_present);

    g_free(bios);
}

static void test_pc_8g_flatview_fits_ram_and_rom_alias_limits(void)
{
    const uint32_t ram_flags = AMDSVM_GUEST_RANGE_READ |
                               AMDSVM_GUEST_RANGE_WRITE |
                               AMDSVM_GUEST_RANGE_EXEC;
    const uint32_t rom_flags = AMDSVM_GUEST_RANGE_READ |
                               AMDSVM_GUEST_RANGE_EXEC;
    const uint64_t ram_hva = UINT64_C(0x100000000);
    const uint64_t bios_hva = UINT64_C(0x300000000);
    AMDVMGuestMemoryMap map = { 0 };
    amdvm_vcpu_memory_map_request_t request = { 0 };
    uint8_t *bios = g_malloc0(0x40000);
    Error *err = NULL;

    g_assert_true(amdsvm_memory_map_append_flatview_range(
        &map, 0, ram_hva, 0xc0000, ram_flags, NULL, &err));
    g_assert_null(err);
    g_assert_true(amdsvm_memory_map_append_flatview_range(
        &map, 0xc0000, UINT64_C(0x200000000), 0x20000,
        rom_flags, NULL, &err));
    g_assert_null(err);
    g_assert_true(amdsvm_memory_map_append_flatview_range(
        &map, 0xe0000, bios_hva + 0x20000, 0x20000,
        rom_flags, NULL, &err));
    g_assert_null(err);
    g_assert_true(amdsvm_memory_map_append_flatview_range(
        &map, 0x100000, ram_hva + 0x100000, 0xbff00000,
        ram_flags, NULL, &err));
    g_assert_null(err);
    g_assert_true(amdsvm_memory_map_append_flatview_range(
        &map, 0xfffc0000, bios_hva, 0x40000,
        rom_flags, bios, &err));
    g_assert_null(err);
    g_assert_true(amdsvm_memory_map_append_flatview_range(
        &map, UINT64_C(0x100000000), ram_hva + 0xc0000000,
        UINT64_C(0x140000000), ram_flags, NULL, &err));
    g_assert_null(err);

    g_assert_cmpuint(map.total_writable_bytes, ==, UINT64_C(0x1fffc0000));
    g_assert_cmpuint(map.total_ram, ==, UINT64_C(0x20003f000));
    g_assert_cmpuint(map.total_writable_bytes, <=,
                     AMDVM_VCPU_MEMORY_MAX_GUEST_RAM_BYTES);
    g_assert_cmpuint(map.total_ram, <=, AMDVM_VCPU_MEMORY_MAX_TOTAL_BYTES);
    g_assert_true(map.reset_rom_page_present);

    for (uint32_t i = 0; i < map.count; i++) {
        const AMDVMGuestMemoryRange *source = &map.ranges[i];
        uint32_t flags = AMDVM_VCPU_MEMORY_READ;
        if (source->flags & AMDSVM_GUEST_RANGE_WRITE) {
            flags |= AMDVM_VCPU_MEMORY_WRITE;
        }
        if (source->flags & AMDSVM_GUEST_RANGE_EXEC) {
            flags |= AMDVM_VCPU_MEMORY_EXEC;
        }
        g_assert_cmpint(amdsvm_memory_map_append_range(&request,
            source->guest_physical_address, source->host_virtual_address,
            source->size, flags), ==, 0);
    }
    g_assert_cmpuint(request.range_count, ==, 7);
    g_assert_cmpuint(request.ranges[5].size, ==,
                     AMDVM_VCPU_MEMORY_MAX_RANGE_BYTES);
    g_assert_cmpuint(request.ranges[6].size, ==, UINT64_C(0x40001000));

    error_free(err);
    g_free(bios);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/amdsvm/memory-map/eight-gib-split",
                    test_eight_gib_splits_into_pin_safe_ranges);
    g_test_add_func("/amdsvm/memory-map/sixteen-gib-no-old-preset",
                    test_sixteen_gib_is_not_limited_by_old_eight_gib_preset);
    g_test_add_func("/amdsvm/memory-map/negotiated-writable-cap",
                    test_negotiated_writable_capacity_fails_atomically);
    g_test_add_func("/amdsvm/memory-map/synthetic-contiguous-span",
                    test_one_two_gib_synthetic_contiguous_span_fits_one_descriptor);
    g_test_add_func("/amdsvm/memory-map/capacity-atomic",
                    test_capacity_failure_is_atomic);
    g_test_add_func("/amdsvm/memory-map/cap-and-overflow",
                    test_total_cap_and_overflow_fail_closed);
    g_test_add_func("/amdsvm/memory-map/immutable-rom-alias",
                    test_only_immutable_rom_backing_may_alias);
    g_test_add_func("/amdsvm/memory-map/pc-1g-flatview-reset-rom",
                    test_pc_1g_flatview_splits_reset_rom_and_coalesces_by_hva);
    g_test_add_func("/amdsvm/memory-map/pc-8g-flatview-alias-cap",
                    test_pc_8g_flatview_fits_ram_and_rom_alias_limits);
    return g_test_run();
}
