// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2003-2006 Silicon Graphics, Inc.  All Rights Reserved.
 * Copyright (C) 2008-2009 MontaVista Software, Inc.
 *
 * PATA driver for the SGI IOC4 chipset.  The IOC4 has a single ATA
 * channel with MMIO register access (32-bit aligned taskfile registers)
 * and a custom DMA engine that uses 128-bit big-endian PRD entries.
 *
 * This driver hooks into the IOC4 base driver (drivers/misc/ioc4.c)
 * via the ioc4_submodule registration interface.
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/ioport.h>
#include <linux/blkdev.h>
#include <linux/scatterlist.h>
#include <linux/ioc4.h>
#include <linux/io.h>
#include <linux/libata.h>
#include <linux/dma-mapping.h>
#include <scsi/scsi_host.h>

#define DRV_NAME	"pata_sgiioc4"
#define DRV_VERSION	"2.0"

/* IOC4 register block offsets from BAR0 */
#define IOC4_INTR_OFFSET	0x000
#define IOC4_CMD_OFFSET		0x100
#define IOC4_CTRL_OFFSET	0x120
#define IOC4_DMA_OFFSET		0x140

/*
 * Interrupt register offsets (u32 index from IOC4_INTR_OFFSET).
 * Hardware register layout — matches the original sgiioc4 driver.
 */
#define IOC4_INTR_ERR_ADDR_L	0	/* PCI error address low */
#define IOC4_INTR_ERR_ADDR_H	1	/* PCI error address high */
#define IOC4_INTR_REG		3	/* Interrupt status */
#define IOC4_INTR_SET		5	/* Interrupt set */
#define IOC4_INTR_CLEAR		7	/* Interrupt clear */

/*
 * DMA register offsets (u32 index from IOC4_DMA_OFFSET).
 * Must match the original sgiioc4 driver's register map exactly:
 *   0x00 timing, 0x04 ptr_l, 0x08 ptr_h, 0x0C addr_l,
 *   0x10 addr_h, 0x14 bc_dev, 0x18 bc_mem, 0x1C ctrl, 0x20 end_addr
 */
#define IOC4_DMA_TIMING		0x00
#define IOC4_DMA_PTR_L		0x01
#define IOC4_DMA_PTR_H		0x02
#define IOC4_DMA_ADDR_L	0x03
#define IOC4_DMA_ADDR_H	0x04
#define IOC4_BC_DEV		0x05
#define IOC4_BC_MEM		0x06
#define IOC4_DMA_CTRL		0x07
#define IOC4_DMA_END_ADDR	0x08

/* DMA control/status bits */
#define IOC4_S_DMA_START	0x01
#define IOC4_S_DMA_STOP		0x02
#define IOC4_S_DMA_DIR		0x04
#define IOC4_S_DMA_ACTIVE	0x08
#define IOC4_S_DMA_ERROR	0x10

/*
 * DMA direction values written to IOC4_DMA_CTRL.
 * Named from the IOC4's perspective (as in the original driver):
 *   IOC4_DMA_WRITE (0x04) = IOC4 writes TO main memory  (host reads)
 *   IOC4_DMA_READ  (0x00) = IOC4 reads FROM main memory (host writes)
 */
#define IOC4_DMA_WRITE		0x04
#define IOC4_DMA_READ		0x00

#define IOC4_CMD_CTL_BLK_SIZE	0x20
#define IOC4_SUPPORTED_FIRMWARE_REV 46

#define IOC4_IDE_CACHELINE_SIZE	128
#define IOC4_PRD_BYTES		16	/* Each PRD entry is 16 bytes */
#define IOC4_PRD_ENTRIES	(PAGE_SIZE / (4 * IOC4_PRD_BYTES))

/*
 * Per-port private data.
 */
struct sgiioc4_priv {
	void __iomem *base;		/* BAR0 mapping */
	void __iomem *cmd_base;		/* Taskfile registers */
	void __iomem *ctl_base;		/* Device control register */
	void __iomem *dma_base;		/* DMA engine registers */
	void __iomem *intr_base;	/* Interrupt registers */

	/* DMA scatter-gather table (128-bit entries, device-accessible) */
	u32 *prd_tbl;
	dma_addr_t prd_tbl_dma;

	/* Ending DMA area — IOC4 writes 1s here on DMA completion */
	unsigned long *ending_dma;
	dma_addr_t ending_dma_dma;

	/* Track whether bmdma_start was called for this command */
	int dma_started;
};

/* --- Low-level register access helpers --- */

static inline u32 sgiioc4_intr_read(struct sgiioc4_priv *priv, int reg)
{
	return readl(priv->intr_base + reg * 4);
}

static inline void sgiioc4_intr_write(struct sgiioc4_priv *priv, int reg, u32 val)
{
	writel(val, priv->intr_base + reg * 4);
}

static inline u32 sgiioc4_dma_read(struct sgiioc4_priv *priv, int reg)
{
	return readl(priv->dma_base + reg * 4);
}

static inline void sgiioc4_dma_write(struct sgiioc4_priv *priv, int reg, u32 val)
{
	writel(val, priv->dma_base + reg * 4);
}

/* --- Interrupt handling --- */

static int sgiioc4_check_irq(struct ata_port *ap)
{
	struct sgiioc4_priv *priv = ap->private_data;

	return sgiioc4_intr_read(priv, IOC4_INTR_REG) & 0x03;
}

static void sgiioc4_clear_irq(struct ata_port *ap)
{
	struct sgiioc4_priv *priv = ap->private_data;
	u32 intr_reg;

	intr_reg = sgiioc4_intr_read(priv, IOC4_INTR_REG);
	if (intr_reg & 0x03) {
		/*
		 * Reading status has the side effect of clearing the IRQ.
		 * Read twice — first to clear, second to verify.
		 */
		u8 stat = ioread8(ap->ioaddr.status_addr);
		int count = 0;

		stat = ioread8(ap->ioaddr.status_addr);
		while ((stat & ATA_BUSY) && (count++ < 100)) {
			udelay(1);
			stat = ioread8(ap->ioaddr.status_addr);
		}

		if (intr_reg & 0x02) {
			/* PCI bus error during DMA */
			u32 err_low, err_high, pci_stat;

			err_low = sgiioc4_intr_read(priv, IOC4_INTR_ERR_ADDR_L);
			err_high = sgiioc4_intr_read(priv, IOC4_INTR_ERR_ADDR_H);
			pci_read_config_dword(to_pci_dev(ap->host->dev),
					      PCI_COMMAND, &pci_stat);
			ata_port_err(ap, "PCI bus error during DMA: "
				     "status-cmd 0x%x addr 0x%x%x\n",
				     pci_stat, err_high, err_low);
			pci_write_config_dword(to_pci_dev(ap->host->dev),
					       PCI_COMMAND, 0x00000146);
		}

		/* Clear interrupt and error bits */
		sgiioc4_intr_write(priv, IOC4_INTR_REG, 0x03);
	}
}

/* --- DMA engine helpers --- */

static u32 sgiioc4_wait_dma_stop(struct sgiioc4_priv *priv)
{
	u32 dma_ctrl;
	int count = 0;

	dma_ctrl = sgiioc4_dma_read(priv, IOC4_DMA_CTRL);
	while ((dma_ctrl & IOC4_S_DMA_STOP) && (count++ < 200)) {
		udelay(1);
		dma_ctrl = sgiioc4_dma_read(priv, IOC4_DMA_CTRL);
	}
	return dma_ctrl;
}

/* --- libata port operations --- */

static int sgiioc4_port_start(struct ata_port *ap)
{
	struct sgiioc4_priv *priv = ap->private_data;
	struct pci_dev *pdev = to_pci_dev(ap->host->dev);

	/* Allocate DMA PRD table — must be page-aligned for IOC4 */
	priv->prd_tbl = dma_alloc_coherent(&pdev->dev, PAGE_SIZE,
					    &priv->prd_tbl_dma, GFP_KERNEL);
	if (!priv->prd_tbl)
		return -ENOMEM;

	/* Allocate ending DMA area — IOC4 writes 1s here on completion */
	priv->ending_dma = dma_alloc_coherent(&pdev->dev,
					      IOC4_IDE_CACHELINE_SIZE,
					      &priv->ending_dma_dma, GFP_KERNEL);
	if (!priv->ending_dma) {
		dma_free_coherent(&pdev->dev, PAGE_SIZE,
				  priv->prd_tbl, priv->prd_tbl_dma);
		priv->prd_tbl = NULL;
		return -ENOMEM;
	}

	return 0;
}

static void sgiioc4_port_stop(struct ata_port *ap)
{
	struct sgiioc4_priv *priv = ap->private_data;
	struct pci_dev *pdev = to_pci_dev(ap->host->dev);

	if (priv->ending_dma) {
		dma_free_coherent(&pdev->dev, IOC4_IDE_CACHELINE_SIZE,
				  priv->ending_dma, priv->ending_dma_dma);
		priv->ending_dma = NULL;
	}
	if (priv->prd_tbl) {
		dma_free_coherent(&pdev->dev, PAGE_SIZE,
				  priv->prd_tbl, priv->prd_tbl_dma);
		priv->prd_tbl = NULL;
	}
}

/*
 * Build the IOC4 scatter-gather table.
 *
 * IOC4 uses 128-bit PRD entries in big-endian format:
 *   [32-bit zero] [32-bit BE phys addr] [32-bit zero] [EOL | 16-bit BE count]
 * The last entry has bit 31 set in the final dword (EOL marker).
 */
static enum ata_completion_errors sgiioc4_qc_prep(struct ata_queued_cmd *qc)
{
	struct sgiioc4_priv *priv = qc->ap->private_data;
	u32 *tbl = priv->prd_tbl;
	unsigned int count = 0;
	struct scatterlist *sg;
	unsigned int si;

	for_each_sg(qc->sg, sg, qc->n_elem, si) {
		dma_addr_t addr = sg_dma_address(sg);
		u32 len = sg_dma_len(sg);

		while (len) {
			u32 bcount = 0x10000 - (addr & 0xffff);

			if (bcount > len)
				bcount = len;

			if (count >= IOC4_PRD_ENTRIES) {
				ata_port_err(qc->ap, "DMA table overflow\n");
				return AC_ERR_SYSTEM;
			}

			tbl[0] = 0;
			tbl[1] = cpu_to_be32(addr);
			tbl[2] = 0;
			tbl[3] = cpu_to_be32(bcount);

			tbl += 4;
			count++;
			addr += bcount;
			len -= bcount;
		}
	}

	/* Set EOL on last entry */
	if (count) {
		tbl -= 4;
		tbl[3] |= cpu_to_be32(0x80000000);
	}

	return AC_ERR_OK;
}

static void sgiioc4_bmdma_setup(struct ata_queued_cmd *qc)
{
	struct ata_port *ap = qc->ap;
	struct sgiioc4_priv *priv = ap->private_data;
	u32 ioc4_dma, dma_dir;
	int write = (qc->dma_dir == DMA_TO_DEVICE);

	priv->dma_started = 0;

	/* Stop any active DMA */
	ioc4_dma = sgiioc4_dma_read(priv, IOC4_DMA_CTRL);
	if (ioc4_dma & IOC4_S_DMA_ACTIVE) {
		ata_port_warn(ap, "DMA still active from previous transfer\n");
		sgiioc4_dma_write(priv, IOC4_DMA_CTRL, IOC4_S_DMA_STOP);
		ioc4_dma = sgiioc4_wait_dma_stop(priv);
		if (ioc4_dma & IOC4_S_DMA_STOP)
			ata_port_err(ap, "DMA STOP bit still asserted\n");
	}

	/* Clear any previous DMA error */
	ioc4_dma = sgiioc4_dma_read(priv, IOC4_DMA_CTRL);
	if (ioc4_dma & IOC4_S_DMA_ERROR) {
		ata_port_warn(ap, "DMA error from previous transfer: 0x%x\n",
			      ioc4_dma);
		sgiioc4_dma_write(priv, IOC4_DMA_CTRL, IOC4_S_DMA_STOP);
		sgiioc4_wait_dma_stop(priv);
	}

	/* Load the scatter-gather list address */
	sgiioc4_dma_write(priv, IOC4_DMA_PTR_L,
			  cpu_to_le32(priv->prd_tbl_dma));

	/* Clear and load the ending DMA address */
	memset(priv->ending_dma, 0, IOC4_IDE_CACHELINE_SIZE);
	sgiioc4_dma_write(priv, IOC4_DMA_END_ADDR,
			  cpu_to_le32(priv->ending_dma_dma));

	/*
	 * Set DMA direction in the control register.
	 * IOC4_DMA_READ (0x00)  = IOC4 reads from memory (host->device)
	 * IOC4_DMA_WRITE (0x04) = IOC4 writes to memory  (device->host)
	 */
	dma_dir = write ? IOC4_DMA_READ : IOC4_DMA_WRITE;
	sgiioc4_dma_write(priv, IOC4_DMA_CTRL, dma_dir);
}

static void sgiioc4_bmdma_start(struct ata_queued_cmd *qc)
{
	struct sgiioc4_priv *priv = qc->ap->private_data;
	u32 reg;

	reg = sgiioc4_dma_read(priv, IOC4_DMA_CTRL);
	sgiioc4_dma_write(priv, IOC4_DMA_CTRL, reg | IOC4_S_DMA_START);

	priv->dma_started = 1;
}

static void sgiioc4_bmdma_stop(struct ata_queued_cmd *qc)
{
	struct ata_port *ap = qc->ap;
	struct sgiioc4_priv *priv = ap->private_data;
	u32 ioc4_dma, bc_dev, bc_mem;
	int num, valid = 0, cnt = 0;

	/*
	 * If DMA was never started (e.g. error handler called after
	 * bmdma_setup but before bmdma_start, as happens in ATAPI
	 * error paths), skip the stop/completion sequence entirely.
	 */
	if (!priv->dma_started)
		return;

	priv->dma_started = 0;

	/* Signal DMA engine to stop */
	sgiioc4_dma_write(priv, IOC4_DMA_CTRL, IOC4_S_DMA_STOP);
	ioc4_dma = sgiioc4_wait_dma_stop(priv);

	if (ioc4_dma & IOC4_S_DMA_STOP)
		ata_port_err(ap, "DMA STOP bit still 1: reg 0x%x\n", ioc4_dma);

	/*
	 * The IOC4 will DMA 1's to the ending DMA area to indicate that
	 * previous data DMA is complete.  This is necessary because of
	 * relaxed ordering between register reads and DMA writes on Altix.
	 */
	while ((cnt++ < 200) && !valid) {
		for (num = 0; num < 16; num++) {
			if (priv->ending_dma[num]) {
				valid = 1;
				break;
			}
		}
		udelay(1);
	}
	if (!valid)
		ata_port_warn(ap, "DMA completion not signaled\n");

	/* Check byte counts match (from original driver) */
	bc_dev = sgiioc4_dma_read(priv, IOC4_BC_DEV);
	bc_mem = sgiioc4_dma_read(priv, IOC4_BC_MEM);
	if ((bc_dev & 0x01FF) || (bc_mem & 0x1FF)) {
		if (bc_dev > bc_mem + 8)
			ata_port_warn(ap,
				      "byte_count_dev %d != byte_count_mem %d\n",
				      bc_dev, bc_mem);
	}
}

static u8 sgiioc4_bmdma_status(struct ata_port *ap)
{
	struct sgiioc4_priv *priv = ap->private_data;
	u32 dma_ctrl = sgiioc4_dma_read(priv, IOC4_DMA_CTRL);
	u8 status = 0;

	if (sgiioc4_check_irq(ap))
		status |= ATA_DMA_INTR;
	if (dma_ctrl & IOC4_S_DMA_ACTIVE)
		status |= ATA_DMA_ACTIVE;
	if (dma_ctrl & IOC4_S_DMA_ERROR)
		status |= ATA_DMA_ERR;

	return status;
}

/*
 * Force all devices to PIO.
 *
 * The IOC4 IDE port is only used for optical drives (CD/DVD-ROM)
 * on Altix IO9/IO10 cards.  The original IDE driver polled for DRQ
 * after the PACKET command, but libata's ATAPI DMA path waits for
 * an interrupt that the IOC4 doesn't reliably generate for ATAPI
 * DRQ assertion, causing HSM violations.  PIO is adequate for
 * optical media throughput.
 */
static unsigned long sgiioc4_mode_filter(struct ata_device *adev,
					 unsigned long xfer_mask)
{
	xfer_mask &= ~(ATA_MASK_MWDMA | ATA_MASK_UDMA);
	return xfer_mask;
}

static void sgiioc4_freeze(struct ata_port *ap)
{
	sgiioc4_clear_irq(ap);
}

static void sgiioc4_thaw(struct ata_port *ap)
{
	struct sgiioc4_priv *priv = ap->private_data;

	sgiioc4_clear_irq(ap);
	/* Re-enable interrupts */
	sgiioc4_intr_write(priv, IOC4_INTR_SET, 0x03);
}

static void sgiioc4_error_handler(struct ata_port *ap)
{
	sgiioc4_clear_irq(ap);
	ata_bmdma_error_handler(ap);
}

static struct scsi_host_template sgiioc4_sht = {
	ATA_BMDMA_SHT(DRV_NAME),
	.sg_tablesize		= IOC4_PRD_ENTRIES,
	.dma_boundary		= PAGE_SIZE - 1,
};

static struct ata_port_operations sgiioc4_ops = {
	.inherits		= &ata_bmdma_port_ops,
	.mode_filter		= sgiioc4_mode_filter,
	.port_start		= sgiioc4_port_start,
	.port_stop		= sgiioc4_port_stop,
	.qc_prep		= sgiioc4_qc_prep,
	.bmdma_setup		= sgiioc4_bmdma_setup,
	.bmdma_start		= sgiioc4_bmdma_start,
	.bmdma_stop		= sgiioc4_bmdma_stop,
	.bmdma_status		= sgiioc4_bmdma_status,
	.freeze			= sgiioc4_freeze,
	.thaw			= sgiioc4_thaw,
	.error_handler		= sgiioc4_error_handler,
};

static int sgiioc4_init_one(struct ioc4_driver_data *idd)
{
	struct pci_dev *pdev = idd->idd_pdev;
	struct ata_host *host;
	struct ata_port *ap;
	struct sgiioc4_priv *priv;
	void __iomem *base;
	unsigned long bar0;
	int rc;

	/* PCI-RT variant does not have IDE */
	if (idd->idd_variant == IOC4_VARIANT_PCI_RT)
		return 0;

	dev_info(&pdev->dev, "%s: IOC4 ATA controller, revision %d\n",
		 DRV_NAME, pdev->revision);

	if (pdev->revision < IOC4_SUPPORTED_FIRMWARE_REV) {
		dev_err(&pdev->dev, "firmware revision %d is too old "
			"(need >= %d)\n",
			pdev->revision, IOC4_SUPPORTED_FIRMWARE_REV);
		return -EAGAIN;
	}

	rc = pcim_enable_device(pdev);
	if (rc)
		return rc;

	/* Map BAR0 */
	bar0 = pci_resource_start(pdev, 0);
	base = pcim_iomap(pdev, 0, 0);
	if (!base) {
		dev_err(&pdev->dev, "failed to map BAR 0 at 0x%lx\n", bar0);
		return -ENOMEM;
	}

	/* Allocate host with one port */
	host = ata_host_alloc(&pdev->dev, 1);
	if (!host)
		return -ENOMEM;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->base = base;
	priv->cmd_base = base + IOC4_CMD_OFFSET;
	priv->ctl_base = base + IOC4_CTRL_OFFSET;
	priv->dma_base = base + IOC4_DMA_OFFSET;
	priv->intr_base = base + IOC4_INTR_OFFSET;

	/* Configure the single ATA port */
	ap = host->ports[0];
	ap->ops = &sgiioc4_ops;
	ap->private_data = priv;
	ap->pio_mask = ATA_PIO4;
	ap->mwdma_mask = ATA_MWDMA2;

	/*
	 * Set up taskfile register addresses.  IOC4 has 32-bit aligned
	 * registers, so each successive register is at +4 bytes.
	 */
	ap->ioaddr.cmd_addr = priv->cmd_base;
	ap->ioaddr.data_addr = priv->cmd_base + 0 * 4;
	ap->ioaddr.error_addr = priv->cmd_base + 1 * 4;
	ap->ioaddr.feature_addr = priv->cmd_base + 1 * 4;
	ap->ioaddr.nsect_addr = priv->cmd_base + 2 * 4;
	ap->ioaddr.lbal_addr = priv->cmd_base + 3 * 4;
	ap->ioaddr.lbam_addr = priv->cmd_base + 4 * 4;
	ap->ioaddr.lbah_addr = priv->cmd_base + 5 * 4;
	ap->ioaddr.device_addr = priv->cmd_base + 6 * 4;
	ap->ioaddr.status_addr = priv->cmd_base + 7 * 4;
	ap->ioaddr.command_addr = priv->cmd_base + 7 * 4;
	ap->ioaddr.ctl_addr = priv->ctl_base;
	ap->ioaddr.altstatus_addr = priv->ctl_base;

	/* PCI bus mastering (ioc4.c already calls pci_set_master, but be safe) */
	pci_set_master(pdev);

	/* Initialize chipset IRQ — enable IDE interrupt */
	writel(0x03, priv->intr_base + IOC4_INTR_SET * 4);

	rc = ata_host_activate(host, pdev->irq, ata_bmdma_interrupt,
			       IRQF_SHARED, &sgiioc4_sht);
	if (rc)
		dev_err(&pdev->dev, "failed to activate host: %d\n", rc);

	return rc;
}

static struct ioc4_submodule sgiioc4_submodule = {
	.is_name	= "IOC4_ata",
	.is_owner	= THIS_MODULE,
	.is_probe	= sgiioc4_init_one,
};

static int __init sgiioc4_init(void)
{
	return ioc4_register_submodule(&sgiioc4_submodule);
}

static void __exit sgiioc4_exit(void)
{
	ioc4_unregister_submodule(&sgiioc4_submodule);
}

late_initcall(sgiioc4_init);
module_exit(sgiioc4_exit);

MODULE_AUTHOR("Aniket Malatpure/Jeremy Higdon");
MODULE_DESCRIPTION("PATA driver for SGI IOC4 chipset (libata)");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);
MODULE_ALIAS("sgiioc4");