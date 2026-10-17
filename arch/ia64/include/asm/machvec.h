/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal machine vector shim for SN2 support.
 *
 * The full machvec dispatch infrastructure was removed in v5.4.  This
 * header provides a single platform test, ia64_is_sn2(), for SN2
 * platform code and drivers.
 */
#ifndef _ASM_IA64_MACHVEC_H
#define _ASM_IA64_MACHVEC_H

#include <linux/compiler.h>
#include <linux/types.h>

static __always_inline bool ia64_is_sn2(void)
{
	return IS_ENABLED(CONFIG_IA64_SGI_SN2);
}

#endif /* _ASM_IA64_MACHVEC_H */
