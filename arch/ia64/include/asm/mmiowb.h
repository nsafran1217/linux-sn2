/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _ASM_IA64_MMIOWB_H
#define _ASM_IA64_MMIOWB_H

/**
 * mmiowb - I/O write barrier
 *
 * Ensure ordering of I/O space writes.  This will make sure that writes
 * following the barrier will arrive after all previous writes.  For most
 * ia64 platforms, this is a simple 'mf.a' instruction.  On SN2, it is
 * __sn_mmiowb() which additionally flushes per-CPU SHUB write buffers
 * before emitting mf.a, so that writes from one CPU are observable by
 * another CPU's subsequent reads across the SHUB fabric.
 */
#ifdef CONFIG_IA64_SGI_SN2
#include <asm/machvec.h>
extern void __sn_mmiowb(void);
static inline void __ia64_mmiowb_runtime(void)
{
	if (ia64_is_sn2())
		__sn_mmiowb();
	else
		ia64_mfa();
}
#define mmiowb()	__ia64_mmiowb_runtime()
#else
#define mmiowb()	ia64_mfa()
#endif

#include <asm-generic/mmiowb.h>

#endif	/* _ASM_IA64_MMIOWB_H */
