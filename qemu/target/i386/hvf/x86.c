/*
 * Copyright (C) 2016 Veertu Inc,
 * Copyright (C) 2017 Google Inc,
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"

#include "cpu.h"
#include "emulate/x86_decode.h"
#include "emulate/x86_emu.h"
#include "vmcs.h"
#include "vmx.h"
#include "emulate/x86_mmu.h"
#include "x86_descr.h"
#include "x86hvf.h"

/* static uint32_t x86_segment_access_rights(struct x86_segment_descriptor *var)
{
   uint32_t ar;

   if (!var->p) {
       ar = 1 << 16;
       return ar;
   }

   ar = var->type & 15;
   ar |= (var->s & 1) << 4;
   ar |= (var->dpl & 3) << 5;
   ar |= (var->p & 1) << 7;
   ar |= (var->avl & 1) << 12;
   ar |= (var->l & 1) << 13;
   ar |= (var->db & 1) << 14;
   ar |= (var->g & 1) << 15;
   return ar;
}*/

bool hvf_x86_read_segment_descriptor(CPUState *cpu,
                                     struct x86_segment_descriptor *desc,
                                     x86_segment_selector sel)
{
    target_ulong base;
    uint32_t limit;

    memset(desc, 0, sizeof(*desc));

    /* valid gdt descriptors start from index 1 */
    if (!sel.index && GDT_SEL == sel.ti) {
        return false;
    }

    if (GDT_SEL == sel.ti) {
        base  = rvmcs(cpu->accel->fd, VMCS_GUEST_GDTR_BASE);
        limit = rvmcs(cpu->accel->fd, VMCS_GUEST_GDTR_LIMIT);
    } else {
        base  = rvmcs(cpu->accel->fd, VMCS_GUEST_LDTR_BASE);
        limit = rvmcs(cpu->accel->fd, VMCS_GUEST_LDTR_LIMIT);
    }

    if (sel.index * 8 >= limit) {
        return false;
    }

    x86_read_mem_priv(cpu, desc, base + sel.index * 8, sizeof(*desc));
    return true;
}

bool x86_write_segment_descriptor(CPUState *cpu,
                                  struct x86_segment_descriptor *desc,
                                  x86_segment_selector sel)
{
    target_ulong base;
    uint32_t limit;
    
    if (GDT_SEL == sel.ti) {
        base  = rvmcs(cpu->accel->fd, VMCS_GUEST_GDTR_BASE);
        limit = rvmcs(cpu->accel->fd, VMCS_GUEST_GDTR_LIMIT);
    } else {
        base  = rvmcs(cpu->accel->fd, VMCS_GUEST_LDTR_BASE);
        limit = rvmcs(cpu->accel->fd, VMCS_GUEST_LDTR_LIMIT);
    }
    
    if (sel.index * 8 >= limit) {
        printf("%s: gdt limit\n", __func__);
        return false;
    }
    x86_write_mem_priv(cpu, desc, base + sel.index * 8, sizeof(*desc));
    return true;
}

bool hvf_x86_read_call_gate(CPUState *cpu, struct x86_call_gate *idt_desc,
                            int gate)
{
    target_ulong base  = rvmcs(cpu->accel->fd, VMCS_GUEST_IDTR_BASE);
    uint32_t limit = rvmcs(cpu->accel->fd, VMCS_GUEST_IDTR_LIMIT);

    memset(idt_desc, 0, sizeof(*idt_desc));
    if (gate * 8 >= limit) {
        printf("%s: idt limit\n", __func__);
        return false;
    }

    x86_read_mem_priv(cpu, idt_desc, base + gate * 8, sizeof(*idt_desc));
    return true;
}

static bool hvf_x86_is_protected(CPUState *cpu)
{
    uint64_t cr0 = rvmcs(cpu->accel->fd, VMCS_GUEST_CR0);
    return cr0 & CR0_PE_MASK;
}

bool hvf_x86_is_real(CPUState *cpu)
{
    return !hvf_x86_is_protected(cpu);
}
