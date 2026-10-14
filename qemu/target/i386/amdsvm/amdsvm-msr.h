/* Portable checks and register decoding shared with AMD-SVM offline tests. */
#ifndef AMDSVM_MSR_H
#define AMDSVM_MSR_H

#include <stdbool.h>
#include <stdint.h>

typedef enum AMDVMMTRRKind {
    AMDVM_MTRR_VARIABLE_BASE,
    AMDVM_MTRR_VARIABLE_MASK,
    AMDVM_MTRR_FIXED_64K,
    AMDVM_MTRR_FIXED_16K,
    AMDVM_MTRR_FIXED_4K,
    AMDVM_MTRR_DEFAULT_TYPE,
    AMDVM_MTRR_CAPABILITY,
} AMDVMMTRRKind;

typedef struct AMDVMMTRRRegister {
    AMDVMMTRRKind kind;
    unsigned index;
} AMDVMMTRRRegister;

#define AMDVM_MTRR_VARIABLE_COUNT 8u
#define AMDVM_MTRR_FIXED_COUNT 11u

typedef struct AMDVMMTRRVariableRange {
    uint64_t base;
    uint64_t mask;
} AMDVMMTRRVariableRange;

typedef struct AMDVMMTRRState {
    uint64_t fixed[AMDVM_MTRR_FIXED_COUNT];
    AMDVMMTRRVariableRange variable[AMDVM_MTRR_VARIABLE_COUNT];
    uint64_t default_type;
    unsigned phys_bits;
    bool fixed_supported;
} AMDVMMTRRState;

#define AMDVM_MTRR_TYPE_UC 0u
#define AMDVM_MTRR_TYPE_WC 1u
#define AMDVM_MTRR_TYPE_WT 4u
#define AMDVM_MTRR_TYPE_WP 5u
#define AMDVM_MTRR_TYPE_WB 6u
#define AMDVM_MTRR_DEFAULT_ENABLE (1u << 11)
#define AMDVM_MTRR_FIXED_ENABLE   (1u << 10)
#define AMDVM_MTRR_MASK_VALID     (1u << 11)

static inline bool amdsvm_mtrr_type_valid(uint8_t type)
{
    return type == AMDVM_MTRR_TYPE_UC || type == AMDVM_MTRR_TYPE_WC ||
           type == AMDVM_MTRR_TYPE_WT || type == AMDVM_MTRR_TYPE_WP ||
           type == AMDVM_MTRR_TYPE_WB;
}

static inline uint64_t amdsvm_mtrr_address_mask(unsigned phys_bits)
{
    return phys_bits >= 12 && phys_bits <= 52 ?
           ((UINT64_C(1) << phys_bits) - 1u) & ~UINT64_C(0xfff) : 0;
}

static inline bool amdsvm_mtrr_fixed_value_valid(uint64_t value)
{
    unsigned i;

    for (i = 0; i < 8; i++) {
        if (!amdsvm_mtrr_type_valid((uint8_t)(value >> (i * 8)))) {
            return false;
        }
    }
    return true;
}

static inline bool amdsvm_mtrr_default_value_valid(uint64_t value)
{
    return (value & ~UINT64_C(0xcff)) == 0 &&
           amdsvm_mtrr_type_valid((uint8_t)value);
}

static inline bool amdsvm_mtrr_base_value_valid(uint64_t value,
                                                 unsigned phys_bits)
{
    uint64_t address_mask = amdsvm_mtrr_address_mask(phys_bits);

    return address_mask != 0 && (value & UINT64_C(0xf00)) == 0 &&
           (value & ~(address_mask | UINT64_C(0xff))) == 0 &&
           amdsvm_mtrr_type_valid((uint8_t)value);
}

static inline bool amdsvm_mtrr_mask_value_valid(uint64_t value,
                                                 unsigned phys_bits)
{
    uint64_t address_mask = amdsvm_mtrr_address_mask(phys_bits);
    uint64_t mask;

    if (address_mask == 0 ||
        (value & ~(address_mask | AMDVM_MTRR_MASK_VALID)) != 0 ||
        (value & UINT64_C(0x7ff)) != 0) {
        return false;
    }
    if ((value & AMDVM_MTRR_MASK_VALID) == 0) {
        return true;
    }
    mask = value & address_mask;
    if (mask == 0) {
        /* A valid zero mask covers the complete implemented physical space. */
        return phys_bits < 52;
    }
    mask >>= 12;
    while ((mask & 1u) == 0) {
        mask >>= 1;
    }
    /* PhysMask is one contiguous run; leading zero bits are significant. */
    return (mask & (mask + 1u)) == 0;
}

static inline bool amdsvm_mtrr_base_matches_mask(uint64_t base,
                                                  uint64_t mask,
                                                  unsigned phys_bits)
{
    uint64_t address_mask = amdsvm_mtrr_address_mask(phys_bits);
    unsigned low_bit = 0;

    if (address_mask == 0 || (mask & AMDVM_MTRR_MASK_VALID) == 0) {
        return true;
    }
    mask &= address_mask;
    if (mask == 0) {
        return (base & address_mask) == 0;
    }
    mask >>= 12;
    while ((mask & 1u) == 0) {
        ++low_bit;
        mask >>= 1;
    }
    return ((base & address_mask) &
            ((UINT64_C(1) << (12u + low_bit)) - 1u)) == 0;
}

static inline bool amdsvm_mtrr_types_combine(uint8_t first, uint8_t second,
                                             uint8_t *result)
{
    if (!result || !amdsvm_mtrr_type_valid(first) ||
        !amdsvm_mtrr_type_valid(second)) {
        return false;
    }
    if (first == second) {
        *result = first;
        return true;
    }
    if (first == AMDVM_MTRR_TYPE_UC || second == AMDVM_MTRR_TYPE_UC) {
        *result = AMDVM_MTRR_TYPE_UC;
        return true;
    }
    if ((first == AMDVM_MTRR_TYPE_WB && second == AMDVM_MTRR_TYPE_WT) ||
        (first == AMDVM_MTRR_TYPE_WT && second == AMDVM_MTRR_TYPE_WB)) {
        *result = AMDVM_MTRR_TYPE_WT;
        return true;
    }
    return false;
}

static inline bool amdsvm_mtrr_fixed_type_at(const AMDVMMTRRState *state,
                                              uint64_t gpa, uint8_t *type)
{
    unsigned reg, slot;

    if (gpa >= UINT64_C(0x100000) || !state->fixed_supported || !type) {
        return false;
    }
    if (gpa < UINT64_C(0x80000)) {
        reg = 0;
        slot = (unsigned)(gpa >> 16);
    } else if (gpa < UINT64_C(0xa0000)) {
        reg = 1;
        slot = (unsigned)((gpa - UINT64_C(0x80000)) >> 13);
    } else if (gpa < UINT64_C(0xc0000)) {
        reg = 2;
        slot = (unsigned)((gpa - UINT64_C(0xa0000)) >> 13);
    } else {
        reg = 3u + (unsigned)((gpa - UINT64_C(0xc0000)) >> 15);
        slot = (unsigned)((gpa >> 12) & 7u);
    }
    *type = (uint8_t)(state->fixed[reg] >> (slot * 8u));
    return amdsvm_mtrr_type_valid(*type);
}

/* Resolve guest MTRR type for one GPA. False means malformed or undefined. */
static inline bool amdsvm_mtrr_effective_type(const AMDVMMTRRState *state,
                                              uint64_t gpa, uint8_t *type)
{
    uint64_t address_mask;
    uint8_t matched_type = 0;
    bool matched = false;
    unsigned i;

    if (!state || !type || !amdsvm_mtrr_default_value_valid(
            state->default_type) ||
        (address_mask = amdsvm_mtrr_address_mask(state->phys_bits)) == 0 ||
        gpa >= (UINT64_C(1) << state->phys_bits)) {
        return false;
    }
    if ((state->default_type & AMDVM_MTRR_DEFAULT_ENABLE) == 0) {
        *type = AMDVM_MTRR_TYPE_UC;
        return true;
    }
    if ((state->default_type & AMDVM_MTRR_FIXED_ENABLE) &&
        !state->fixed_supported) {
        return false;
    }
    if ((state->default_type & AMDVM_MTRR_FIXED_ENABLE) &&
        gpa < UINT64_C(0x100000)) {
        return amdsvm_mtrr_fixed_type_at(state, gpa, type);
    }

    for (i = 0; i < AMDVM_MTRR_VARIABLE_COUNT; i++) {
        const AMDVMMTRRVariableRange *range = &state->variable[i];
        uint8_t candidate;

        if (!amdsvm_mtrr_base_value_valid(range->base, state->phys_bits) ||
            !amdsvm_mtrr_mask_value_valid(range->mask, state->phys_bits) ||
            !amdsvm_mtrr_base_matches_mask(range->base, range->mask,
                                           state->phys_bits)) {
            return false;
        }
        if ((range->mask & AMDVM_MTRR_MASK_VALID) == 0 ||
            (gpa & address_mask & range->mask) !=
            (range->base & address_mask & range->mask)) {
            continue;
        }
        candidate = (uint8_t)range->base;
        if (!matched) {
            matched_type = candidate;
            matched = true;
        } else if (!amdsvm_mtrr_types_combine(matched_type, candidate,
                                             &matched_type)) {
            return false;
        }
    }
    *type = matched ? matched_type : (uint8_t)state->default_type;
    return true;
}

static inline bool amdsvm_msr_decode_mtrr(uint32_t msr,
                                           AMDVMMTRRRegister *reg)
{
    if (!reg) {
        return false;
    }
    if (msr >= 0x200u && msr <= 0x20fu) {
        reg->kind = (msr & 1u) ? AMDVM_MTRR_VARIABLE_MASK :
                                AMDVM_MTRR_VARIABLE_BASE;
        reg->index = (msr - 0x200u) / 2u;
        return true;
    }
    if (msr == 0x250u) {
        reg->kind = AMDVM_MTRR_FIXED_64K;
        reg->index = 0;
        return true;
    }
    if (msr >= 0x258u && msr <= 0x259u) {
        reg->kind = AMDVM_MTRR_FIXED_16K;
        reg->index = msr - 0x258u + 1u;
        return true;
    }
    if (msr >= 0x268u && msr <= 0x26fu) {
        reg->kind = AMDVM_MTRR_FIXED_4K;
        reg->index = msr - 0x268u + 3u;
        return true;
    }
    if (msr == 0x2ffu) {
        reg->kind = AMDVM_MTRR_DEFAULT_TYPE;
        reg->index = 0;
        return true;
    }
    if (msr == 0xfeu) {
        reg->kind = AMDVM_MTRR_CAPABILITY;
        reg->index = 0;
        return true;
    }
    return false;
}

static inline bool amdsvm_msr_is_tcg_compat_amd(uint32_t msr)
{
    return (msr >= 0xc0010000u && msr <= 0xc0010003u) ||
           msr == 0xc0010007u || msr == 0xc0010010u ||
           msr == 0xc0010015u || msr == 0xc0010055u ||
           msr == 0xc0010114u || msr == 0xc0011029u;
}

static inline bool amdsvm_msr_exit_direction_valid(uint64_t exit_info1,
                                                    bool rdmsr,
                                                    bool wrmsr)
{
    return exit_info1 <= 1u &&
           (exit_info1 == 0 ? rdmsr && !wrmsr : wrmsr && !rdmsr);
}

static inline bool amdsvm_msr_next_rip_valid(uint64_t rip, unsigned length,
                                              bool cs64, bool cs32,
                                              uint64_t next_rip)
{
    uint64_t expected;

    if (length == 0 || length > 15 || rip > UINT64_MAX - length) {
        return false;
    }
    expected = rip + length;
    if (!cs64) {
        expected &= cs32 ? UINT32_MAX : UINT16_MAX;
    }
    return expected == next_rip;
}

#endif
