#ifndef AMDSVM_NATIVE_CR3_POLICY_H
#define AMDSVM_NATIVE_CR3_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#define AMDSVM_CR4_PCIDE (UINT64_C(1) << 17)

/* Native MOV-to/from-CR3 is safe only when hardware and guest agree on
 * MAXPHYADDR. Keep PCID mode on QEMU's checked/emulated path for now. */
static inline bool amdsvm_native_cr3_policy_safe(uint32_t guest_phys_bits,
                                                 uint32_t host_phys_bits,
                                                 uint64_t guest_cr4)
{
    return guest_phys_bits >= 32 && guest_phys_bits == host_phys_bits &&
           !(guest_cr4 & AMDSVM_CR4_PCIDE);
}

#endif
