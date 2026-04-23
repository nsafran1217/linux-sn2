// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal runtime machine vector shim for IA-64.
 *
 * Exposes one static key, sn2_platform_key, that is enabled at boot
 * by ia64_platform_detect() when SN2 (SGI Altix) hardware is detected
 * via the ACPI XSDT/RSDT OEM ID ("SGI").
 *
 * Platform-conditional code paths (I/O, mmiowb, IPI, TLB purge, SN2
 * init hooks) dispatch on this key whenever CONFIG_IA64_SGI_SN2 is
 * built in.  Without HAVE_ARCH_JUMP_LABEL on IA-64, static_branch
 * falls back to a global-variable load + test + branch; once arch
 * jump-label support lands, the dispatch becomes a patched nop/branch
 * with no data dependency on the fast path.
 *
 * Copyright (C) 2026 SN2 Revival Project
 */

#include <linux/acpi.h>
#include <linux/efi.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/jump_label.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/string.h>

#include <asm/machvec.h>

/*
 * Populated at boot by ia64_platform_detect() so that existing code
 * reading this variable (e.g. /proc display, diagnostic messages)
 * keeps seeing the detected platform name.  The canonical runtime
 * source of truth for platform checks is the sn2_platform_key
 * static key, not this string.
 */
extern char ia64_platform_name[64];

/*
 * Default OFF.  A non-SN2 boot leaves the key disabled and the
 * generic ia64 fast paths run.  ia64_platform_detect() flips the
 * key on iff we are on SN2 hardware.
 */
DEFINE_STATIC_KEY_FALSE(sn2_platform_key);
EXPORT_SYMBOL_GPL(sn2_platform_key);

/*
 * ia64_platform_detect - set sn2_platform_key based on ACPI OEM ID
 *
 * Called from setup_arch() right after efi_init() / io_port_init() /
 * uv_probe_system_type() and BEFORE early_console_setup().  The SN2
 * early console hook (sn_serial_console_early_setup) calls
 * ia64_platform_is("sn2"), so the static key must already reflect
 * the detected platform by then.
 *
 * We read the XSDT header's OEM ID directly from the EFI config
 * table (the same approach uv_probe_system_type() uses), rather
 * than going through ACPICA's acpi_get_table().  ACPICA has not
 * run acpi_table_init() yet at this point, and we want to be the
 * very first thing that runs after efi_init().
 */
void __init ia64_platform_detect(void)
{
	struct acpi_table_rsdp *rsdp;
	struct acpi_table_xsdt *xsdt;
	const char *name = "dig";

	if (efi.acpi20 == EFI_INVALID_TABLE_ADDR) {
		pr_info("ia64: platform detection: no ACPI 2.0 RSDP, assuming generic\n");
		goto out;
	}

	rsdp = (struct acpi_table_rsdp *)__va(efi.acpi20);
	if (strncmp(rsdp->signature, ACPI_SIG_RSDP,
		    sizeof(ACPI_SIG_RSDP) - 1)) {
		pr_info("ia64: platform detection: bad RSDP signature, assuming generic\n");
		goto out;
	}

	xsdt = (struct acpi_table_xsdt *)__va(rsdp->xsdt_physical_address);
	if (strncmp(xsdt->header.signature, ACPI_SIG_XSDT,
		    sizeof(ACPI_SIG_XSDT) - 1)) {
		pr_info("ia64: platform detection: bad XSDT signature, assuming generic\n");
		goto out;
	}

	if (!strncmp(xsdt->header.oem_id, "SGI", 3) &&
	    strncmp(xsdt->header.oem_table_id + 4, "UV", 2)) {
		/* SGI OEM ID, not a UV system -> SN2 (Altix). */
		static_branch_enable(&sn2_platform_key);
		name = "sn2";
		pr_info("ia64: platform detected: SN2 (OEM ID '%.6s')\n",
			xsdt->header.oem_id);
	} else {
		pr_info("ia64: platform detected: generic (OEM ID '%.6s')\n",
			xsdt->header.oem_id);
	}

out:
	strscpy(ia64_platform_name, name, sizeof(ia64_platform_name));
}
