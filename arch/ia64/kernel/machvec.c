// SPDX-License-Identifier: GPL-2.0
/*
 * Boot-time SN2 (SGI Altix) platform detection.
 *
 * With CONFIG_IA64_SGI_SN2=y and CONFIG_IA64_SGI_SN2_ONLY=n the kernel
 * boots on both SN2 and generic ia64 hardware.  ia64_platform_detect()
 * sets ia64_sn2_platform when the ACPI XSDT OEM ID says SGI, and every
 * platform-conditional path tests it through ia64_is_sn2().  This is the
 * same test the pre-5.4 generic machvec did in acpi_get_sysname().
 *
 * With CONFIG_IA64_SGI_SN2_ONLY=y, ia64_is_sn2() is constant true and the
 * detection only refuses to boot on non-SGI hardware.
 */

#include <linux/acpi.h>
#include <linux/efi.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/string.h>

#include <asm/machvec.h>

#ifndef CONFIG_IA64_SGI_SN2_ONLY
/* Exported because readX()/inX()/mmiowb() test it inline in modules. */
bool ia64_sn2_platform __read_mostly;
EXPORT_SYMBOL(ia64_sn2_platform);
#endif

static bool __init acpi_oem_is_sn2(char *oem_id)
{
	struct acpi_table_rsdp *rsdp;
	struct acpi_table_xsdt *xsdt;

	if (efi.acpi20 == EFI_INVALID_TABLE_ADDR)
		return false;

	rsdp = (struct acpi_table_rsdp *)__va(efi.acpi20);
	if (strncmp(rsdp->signature, ACPI_SIG_RSDP, sizeof(ACPI_SIG_RSDP) - 1))
		return false;

	xsdt = (struct acpi_table_xsdt *)__va(rsdp->xsdt_physical_address);
	if (strncmp(xsdt->header.signature, ACPI_SIG_XSDT,
		    sizeof(ACPI_SIG_XSDT) - 1))
		return false;

	memcpy(oem_id, xsdt->header.oem_id, ACPI_OEM_ID_SIZE);

	/* SGI OEM ID, and not a UV system ("UV" in the OEM table ID). */
	return !strncmp(xsdt->header.oem_id, "SGI", 3) &&
	       strncmp(xsdt->header.oem_table_id + 4, "UV", 2);
}

/*
 * Called from setup_arch() after efi_init() and before
 * early_console_setup(): the SN2 early console checks
 * ia64_is_sn2().  ACPICA isn't initialised yet, so read the
 * XSDT header straight from the EFI config table.
 */
void __init ia64_platform_detect(void)
{
	char oem_id[ACPI_OEM_ID_SIZE + 1] = "";
	bool sn2 = acpi_oem_is_sn2(oem_id);

#ifdef CONFIG_IA64_SGI_SN2_ONLY
	if (!sn2)
		panic("This kernel was built for SGI Altix (SN2) only "
		      "(CONFIG_IA64_SGI_SN2_ONLY), but the ACPI OEM ID is '%s'\n",
		      oem_id);
#else
	ia64_sn2_platform = sn2;
#endif

	pr_info("ia64: platform %s (ACPI OEM ID '%s')\n",
		sn2 ? "SN2" : "generic", oem_id);
}
