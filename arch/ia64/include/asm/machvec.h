/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal machine vector shim for SN2 support.
 *
 * The full machvec dispatch infrastructure was removed in v5.4.  This
 * header replaces it with a single platform test, ia64_is_sn2(), in
 * three build modes:
 *
 *   IA64_SGI_SN2=n                     constant false; no SN2 code built
 *   IA64_SGI_SN2=y, IA64_SGI_SN2_ONLY=y constant true; Altix-only kernel
 *   IA64_SGI_SN2=y, IA64_SGI_SN2_ONLY=n runtime test of ia64_sn2_platform,
 *                                      set by ia64_platform_detect()
 *
 * In the constant modes every "if (ia64_is_sn2())" folds away, so those
 * kernels are identical to ones built with #ifdefs.  In the runtime mode
 * the test is a load + compare + branch, with the generic path as the
 * fall-through.
 *
 * The flag is a plain bool, not a static key: ia64 has no
 * HAVE_ARCH_JUMP_LABEL, so static_branch_unlikely() would be an
 * atomic_read(), a volatile load, which ia64 gcc emits as ld.acq at
 * every readX()/inX() site.  The flag is written once, before secondary
 * CPUs start, so a plain load is enough.
 */
#ifndef _ASM_IA64_MACHVEC_H
#define _ASM_IA64_MACHVEC_H

#include <linux/compiler.h>
#include <linux/types.h>

#if defined(CONFIG_IA64_SGI_SN2) && !defined(CONFIG_IA64_SGI_SN2_ONLY)

extern bool ia64_sn2_platform;

static __always_inline bool ia64_is_sn2(void)
{
	return unlikely(ia64_sn2_platform);
}

#else

static __always_inline bool ia64_is_sn2(void)
{
	return IS_ENABLED(CONFIG_IA64_SGI_SN2);
}

#endif

#ifdef CONFIG_IA64_SGI_SN2
/* Called from setup_arch() before any platform-conditional code runs. */
void ia64_platform_detect(void);
#else
static inline void ia64_platform_detect(void) { }
#endif

/*
 * SN2 entry points called from generic ia64 code.  They are declared in
 * every configuration so that the callers need no #ifdef: without SN2
 * support ia64_is_sn2() is constant false and the compiler discards the
 * call.
 */
struct mm_struct;
struct msi_desc;
struct pci_bus;
struct pci_dev;
struct task_struct;

void sn_setup(char **cmdline_p);
void sn_cpu_init(void);
void sn_set_io_os_features(void);
void sn_flush_all_caches(long addr, long bytes);
void sn_io_acpi_init(void);
void sn_irq_init(void);
void sn_timer_interrupt(int irq, void *dev_id);
void sn_migrate(struct task_struct *task);
bool sn_cpu_disable_allowed(int cpu);
void sn2_send_IPI(int cpuid, int vector, int delivery_mode, int redirect);
void sn2_global_tlb_purge(struct mm_struct *mm, unsigned long start,
			  unsigned long end, unsigned long nbits);
int sn_setup_msi_irq(struct pci_dev *pdev, struct msi_desc *entry);
void sn_teardown_msi_irq(unsigned int irq);
void sn_pci_fixup_bus(struct pci_bus *bus);
int sn_serial_console_early_setup(void);

#endif /* _ASM_IA64_MACHVEC_H */
