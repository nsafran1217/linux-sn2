/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal machine vector compatibility shim for SN2 support.
 *
 * The full machvec dispatch infrastructure was removed in v5.4.
 * This header provides compile-time platform identification so that
 * SN2 platform code and drivers can use ia64_platform_is("sn2")
 * without requiring the runtime dispatch system.
 */
#ifndef _ASM_IA64_MACHVEC_H
#define _ASM_IA64_MACHVEC_H

#include <linux/string.h>

#ifdef CONFIG_IA64_SGI_SN2

static inline int ia64_platform_is(const char *name)
{
	return strcmp(name, "sn2") == 0;
}

#define ia64_platform_name	"sn2"
#define ia64_platform_is	ia64_platform_is

#else /* !CONFIG_IA64_SGI_SN2 */

static inline int ia64_platform_is(const char *name)
{
	return strcmp(name, "dig") == 0;
}

#define ia64_platform_name	"dig"
#define ia64_platform_is	ia64_platform_is

#endif /* CONFIG_IA64_SGI_SN2 */

#endif /* _ASM_IA64_MACHVEC_H */
