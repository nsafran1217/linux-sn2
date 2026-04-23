/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal machine vector compatibility shim for SN2 support.
 *
 * Two modes:
 *
 *  1. CONFIG_IA64_SGI_SN2=y (default)
 *       Single-binary kernel that boots on both SN2 and non-SN2.
 *       ia64_platform_is("sn2") dispatches on the sn2_platform_key
 *       static key, which is enabled at boot by ia64_platform_detect()
 *       when ACPI reports an SGI OEM ID.
 *
 *  2. CONFIG_IA64_SGI_SN2=n
 *       Non-SN2-only kernel.  ia64_platform_is("sn2") is a compile-
 *       time constant false and no SN2 code is linked in.
 *
 * Until HAVE_ARCH_JUMP_LABEL is implemented for ia64, the static
 * branch falls back to a plain global-variable load + test + branch.
 * Call sites that pass literal strings to ia64_platform_is() still
 * get compile-time folding of the strcmp because GCC evaluates
 * __builtin_strcmp() of two string literals at compile time.
 */
#ifndef _ASM_IA64_MACHVEC_H
#define _ASM_IA64_MACHVEC_H

#include <linux/string.h>

#ifdef CONFIG_IA64_SGI_SN2

#include <linux/jump_label.h>

DECLARE_STATIC_KEY_FALSE(sn2_platform_key);

/* Called from setup_arch() before any platform-conditional code runs. */
void ia64_platform_detect(void);

/**
 * ia64_is_sn2 - runtime check: are we on SN2 hardware?
 *
 * Marked unlikely so the compiler lays out the generic (non-SN2) path
 * as the fall-through.  With arch jump-label support this becomes a
 * patched nop on SN2 and a patched unconditional branch on non-SN2;
 * without it, it is a load + test + conditional branch on every call.
 */
static __always_inline bool ia64_is_sn2(void)
{
	return static_branch_unlikely(&sn2_platform_key);
}

/*
 * Callers pass string literals in every current use site, so the
 * strcmp() here is folded to a compile-time constant by GCC.  The
 * static-key evaluation only remains live for the literal that
 * matches "sn2".  For unknown literals we fall back to returning
 * zero.
 */
static __always_inline int ia64_platform_is(const char *name)
{
	if (__builtin_strcmp(name, "sn2") == 0)
		return ia64_is_sn2();
	if (__builtin_strcmp(name, "dig") == 0)
		return !ia64_is_sn2();
	return 0;
}

#else /* !CONFIG_IA64_SGI_SN2 */

static __always_inline bool ia64_is_sn2(void)
{
	return false;
}

static inline int ia64_platform_is(const char *name)
{
	return strcmp(name, "dig") == 0;
}

#endif /* CONFIG_IA64_SGI_SN2 */

#endif /* _ASM_IA64_MACHVEC_H */
