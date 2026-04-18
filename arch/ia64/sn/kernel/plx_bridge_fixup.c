// SPDX-License-Identifier: GPL-2.0
/*
 * SGI Altix SN2 PCI-to-PCI bridge prefetchable window fixup
 *
 * On SN2, CPU-visible BAR addresses (resource->start) include NASID,
 * widget, and device-register bits that the bridge never sees.  The
 * PCI bus address -- what the bridge window must match -- lives in
 * the raw BAR in config space, per SGI Porting Guide 007-4520-007
 * Example 7-1 on page 65.  Read config space directly.
 * 
 * This *should* automatically configure the window, but
 * has only been tested on GPUs with 256MB VRAM BARs.
 * It also *should* work for any PCI-e card that needs
 * windows configured, but again, untested
 */

#include <linux/pci.h>
#include <linux/init.h>
#include <linux/printk.h>

/* Vendor and Device ID are hardcoded for a PLX 8111 PCI 
to PCI-e bridge. These are the cheap ones available on eBay
with a full size PCI-e slot. Any other bridges are untested*/

#define PLX_VENDOR_ID   0x10b5
#define PLX_8111_DEVICE_ID 0x8111

static void sn2_plx_bridge_fixup(struct pci_dev *pdev)
{
	struct pci_dev *child;
	u64 need_start = U64_MAX;
	u64 need_end = 0;
	u64 cur_start, cur_end;
	u16 base16, limit16, new_base, new_limit;
	bool found = false;
	int i;

	if (pdev->hdr_type != PCI_HEADER_TYPE_BRIDGE || !pdev->subordinate)
		return;

	dev_info(&pdev->dev, "SN2 PLX fixup: scanning bus %02x\n",
		 pdev->subordinate->number);

	list_for_each_entry(child, &pdev->subordinate->devices, bus_list) {
		for (i = 0; i < PCI_STD_NUM_BARS; i++) {
			struct resource *r = &child->resource[i];
			u32 bar_lo, bar_hi = 0;
			u64 bus_addr, bus_end, size;

			if (!(r->flags & IORESOURCE_MEM))
				continue;
			if (!(r->flags & IORESOURCE_PREFETCH))
				continue;
			size = resource_size(r);
			if (!size)
				continue;

			/* Read raw BAR from config space.  On SN2 this is
			 * the PCI bus address, unlike resource->start which
			 * is a CPU-mapped address with NASID/widget bits. */
			pci_read_config_dword(child,
				PCI_BASE_ADDRESS_0 + 4 * i, &bar_lo);

			/* Must be a memory BAR. */
			if (bar_lo & PCI_BASE_ADDRESS_SPACE_IO)
				continue;

			/* Pull the upper dword for 64-bit BARs. */
			if ((bar_lo & PCI_BASE_ADDRESS_MEM_TYPE_MASK) ==
			    PCI_BASE_ADDRESS_MEM_TYPE_64 &&
			    i + 1 < PCI_STD_NUM_BARS) {
				pci_read_config_dword(child,
					PCI_BASE_ADDRESS_0 + 4 * (i + 1),
					&bar_hi);
			}

			bus_addr = ((u64)bar_hi << 32) |
				   (bar_lo & PCI_BASE_ADDRESS_MEM_MASK);
			bus_end = bus_addr + size - 1;

			dev_info(&pdev->dev,
				 "  %s BAR%d: cpu %pR  pci-bus 0x%llx-0x%llx\n",
				 pci_name(child), i, r,
				 (u64)bus_addr, (u64)bus_end);

			if (!bus_addr) {
				dev_info(&pdev->dev,
					 "    (BAR unconfigured, skipping)\n");
				continue;
			}
			if (bus_end >> 32) {
				dev_info(&pdev->dev,
					 "    (above 4GB, pref window is 32-bit; skipping)\n");
				continue;
			}

			if (bus_addr < need_start)
				need_start = bus_addr;
			if (bus_end > need_end)
				need_end = bus_end;
			found = true;
		}
	}

	if (!found) {
		dev_info(&pdev->dev,
			 "SN2 PLX fixup: no usable prefetchable BARs downstream\n");
		return;
	}

	pci_read_config_word(pdev, PCI_PREF_MEMORY_BASE,  &base16);
	pci_read_config_word(pdev, PCI_PREF_MEMORY_LIMIT, &limit16);
	cur_start = ((u64)(base16  & PCI_PREF_RANGE_MASK)) << 16;
	cur_end   = (((u64)(limit16 & PCI_PREF_RANGE_MASK)) << 16) | 0xFFFFF;

	dev_info(&pdev->dev,
		 "SN2 PLX fixup: downstream needs 0x%llx-0x%llx\n",
		 need_start, need_end);
	dev_info(&pdev->dev,
		 "  window before: base=0x%04x limit=0x%04x (0x%llx-0x%llx)\n",
		 base16, limit16, cur_start, cur_end);

	if ((base16 & PCI_PREF_RANGE_MASK) &&
	    cur_start <= need_start && cur_end >= need_end) {
		dev_info(&pdev->dev,
			 "  window already covers needed range, skipping\n");
		return;
	}

	new_base  = (u16)((need_start >> 16) & PCI_PREF_RANGE_MASK);
	new_limit = (u16)((need_end   >> 16) & PCI_PREF_RANGE_MASK);

	pci_write_config_word(pdev, PCI_PREF_MEMORY_BASE,  new_base);
	pci_write_config_word(pdev, PCI_PREF_MEMORY_LIMIT, new_limit);

	if ((base16 & PCI_PREF_RANGE_TYPE_MASK) == PCI_PREF_RANGE_TYPE_64) {
		pci_write_config_dword(pdev, PCI_PREF_BASE_UPPER32,  0);
		pci_write_config_dword(pdev, PCI_PREF_LIMIT_UPPER32, 0);
	}

	pci_read_config_word(pdev, PCI_PREF_MEMORY_BASE,  &base16);
	pci_read_config_word(pdev, PCI_PREF_MEMORY_LIMIT, &limit16);
	dev_info(&pdev->dev,
		 "  window after:  base=0x%04x limit=0x%04x\n",
		 base16, limit16);

	if ((base16 & PCI_PREF_RANGE_MASK) != new_base ||
	    (limit16 & PCI_PREF_RANGE_MASK) != new_limit)
		dev_err(&pdev->dev,
			"SN2 PLX fixup: write-back verification FAILED\n");
}

DECLARE_PCI_FIXUP_FINAL(PLX_VENDOR_ID, PLX_8111_DEVICE_ID, sn2_plx_bridge_fixup);
