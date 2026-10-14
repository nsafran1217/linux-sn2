/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _ASM_IA64_MMIOWB_H
#define _ASM_IA64_MMIOWB_H

/**
 * mmiowb - I/O write barrier
 *
 * Ensure ordering of I/O space writes.  This will make sure that writes
 * following the barrier will arrive after all previous writes.  For most
 * ia64 platforms, this is a simple 'mf.a' instruction.
 */
#if defined(CONFIG_IA64_SGI_SN2_ONLY)
extern void __sn_mmiowb(void);
#define mmiowb()	__sn_mmiowb()
#elif defined(CONFIG_IA64_SGI_SN2)
#include <asm/sn/pda_types.h>
#include <asm/sn/shub_mmr.h>
/*
 * SN2 waits for the SHub PIO write status instead of issuing mf.a.  This
 * is __sn_mmiowb() inlined: mmiowb() is inlined into every spin_unlock(),
 * and a call there, even in the never-taken SN2 branch, would cost those
 * leaf functions a register frame on generic hardware.
 *
 * The test is on the per-cpu PDA rather than ia64_is_sn2(): the PDA sits
 * at a fixed per-cpu address (one load, no GOT indirection, also from
 * modules), and pio_write_status_addr stays NULL except on an SN2 CPU
 * that has run sn_cpu_init().  Before that, mf.a is the right barrier.
 */
static __always_inline void __ia64_mmiowb(void)
{
	struct pda_s *p = &__ia64_per_cpu_var(pda_percpu);
	volatile unsigned long *adr = p->pio_write_status_addr;

	if (unlikely(adr)) {
		unsigned long val = p->pio_write_status_val;

		while ((*adr & SH_PIO_WRITE_STATUS_PENDING_WRITE_COUNT_MASK) != val)
			ia64_hint(ia64_hint_pause);
	} else {
		ia64_mfa();
	}
}
#define mmiowb()	__ia64_mmiowb()
#else
#define mmiowb()	ia64_mfa()
#endif

#include <asm-generic/mmiowb.h>

#endif	/* _ASM_IA64_MMIOWB_H */
