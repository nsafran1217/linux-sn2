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
#ifdef CONFIG_IA64_SGI_SN2
extern void __sn_mmiowb(void);
#define mmiowb()	__sn_mmiowb()
#else
#define mmiowb()	ia64_mfa()
#endif

#include <asm-generic/mmiowb.h>

#endif	/* _ASM_IA64_MMIOWB_H */
