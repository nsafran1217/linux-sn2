/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_IA64_IO_H
#define _ASM_IA64_IO_H

/*
 * This file contains the definitions for the emulated IO instructions
 * inb/inw/inl/outb/outw/outl and the "string versions" of the same
 * (insb/insw/insl/outsb/outsw/outsl). You can also use "pausing"
 * versions of the single-IO instructions (inb_p/inw_p/..).
 *
 * This file is not meant to be obfuscating: it's just complicated to
 * (a) handle it all in a way that makes gcc able to optimize it as
 * well as possible and (b) trying to avoid writing the same thing
 * over and over again with slight variations and possibly making a
 * mistake somewhere.
 *
 * Copyright (C) 1998-2003 Hewlett-Packard Co
 *	David Mosberger-Tang <davidm@hpl.hp.com>
 * Copyright (C) 1999 Asit Mallick <asit.k.mallick@intel.com>
 * Copyright (C) 1999 Don Dugger <don.dugger@intel.com>
 */

#include <linux/unaligned.h>
#include <asm/early_ioremap.h>

#define __IA64_UNCACHED_OFFSET	RGN_BASE(RGN_UNCACHED)

/*
 * The legacy I/O space defined by the ia64 architecture supports only 65536 ports, but
 * large machines may have multiple other I/O spaces so we can't place any a priori limit
 * on IO_SPACE_LIMIT.  These additional spaces are described in ACPI.
 */
#define IO_SPACE_LIMIT		0xffffffffffffffffUL

#define MAX_IO_SPACES_BITS		8
#define MAX_IO_SPACES			(1UL << MAX_IO_SPACES_BITS)
#define IO_SPACE_BITS			24
#define IO_SPACE_SIZE			(1UL << IO_SPACE_BITS)

#define IO_SPACE_NR(port)		((port) >> IO_SPACE_BITS)
#define IO_SPACE_BASE(space)		((space) << IO_SPACE_BITS)
#define IO_SPACE_PORT(port)		((port) & (IO_SPACE_SIZE - 1))

#define IO_SPACE_SPARSE_ENCODING(p)	((((p) >> 2) << 12) | ((p) & 0xfff))

struct io_space {
	unsigned long mmio_base;	/* base in MMIO space */
	int sparse;
};

extern struct io_space io_space[];
extern unsigned int num_io_spaces;

# ifdef __KERNEL__

/*
 * All MMIO iomem cookies are in region 6; anything less is a PIO cookie:
 *	0xCxxxxxxxxxxxxxxx	MMIO cookie (return from ioremap)
 *	0x000000001SPPPPPP	PIO cookie (S=space number, P..P=port)
 *
 * ioread/writeX() uses the leading 1 in PIO cookies (PIO_OFFSET) to catch
 * code that uses bare port numbers without the prerequisite pci_iomap().
 */
#define PIO_OFFSET		(1UL << (MAX_IO_SPACES_BITS + IO_SPACE_BITS))
#define PIO_MASK		(PIO_OFFSET - 1)
#define PIO_RESERVED		__IA64_UNCACHED_OFFSET
#define HAVE_ARCH_PIO_SIZE

#include <asm/intrinsics.h>
#include <asm/page.h>
#include <asm-generic/iomap.h>

/*
 * Change virtual addresses to physical addresses and vv.
 */
static inline unsigned long
virt_to_phys (volatile void *address)
{
	return (unsigned long) address - PAGE_OFFSET;
}
#define virt_to_phys virt_to_phys

static inline void*
phys_to_virt (unsigned long address)
{
	return (void *) (address + PAGE_OFFSET);
}
#define phys_to_virt phys_to_virt

#define ARCH_HAS_VALID_PHYS_ADDR_RANGE
extern u64 kern_mem_attribute (unsigned long phys_addr, unsigned long size);
extern int valid_phys_addr_range (phys_addr_t addr, size_t count); /* efi.c */
extern int valid_mmap_phys_addr_range (unsigned long pfn, size_t count);

# endif /* KERNEL */

/*
 * Memory fence w/accept.  This should never be used in code that is
 * not IA-64 specific.
 */
#define __ia64_mf_a()	ia64_mfa()

static inline void*
__ia64_mk_io_addr (unsigned long port)
{
	struct io_space *space;
	unsigned long offset;

	space = &io_space[IO_SPACE_NR(port)];
	port = IO_SPACE_PORT(port);
	if (space->sparse)
		offset = IO_SPACE_SPARSE_ENCODING(port);
	else
		offset = port;

	return (void *) (space->mmio_base | offset);
}

/*
 * For the in/out routines, we need to do "mf.a" _after_ doing the I/O access to ensure
 * that the access has completed before executing other I/O accesses.  Since we're doing
 * the accesses through an uncachable (UC) translation, the CPU will execute them in
 * program order.  However, we still need to tell the compiler not to shuffle them around
 * during optimization, which is why we use "volatile" pointers.
 */

static inline unsigned int __ia64_inb_native(unsigned long port)
{
	volatile unsigned char *addr = __ia64_mk_io_addr(port);
	unsigned char ret;

	ret = *addr;
	__ia64_mf_a();
	return ret;
}

static inline unsigned int __ia64_inw_native(unsigned long port)
{
	volatile unsigned short *addr = __ia64_mk_io_addr(port);
	unsigned short ret;

	ret = *addr;
	__ia64_mf_a();
	return ret;
}

static inline unsigned int __ia64_inl_native(unsigned long port)
{
	volatile unsigned int *addr = __ia64_mk_io_addr(port);
	unsigned int ret;

	ret = *addr;
	__ia64_mf_a();
	return ret;
}

static inline void __ia64_outb_native(unsigned char val, unsigned long port)
{
	volatile unsigned char *addr = __ia64_mk_io_addr(port);

	*addr = val;
	__ia64_mf_a();
}

static inline void __ia64_outw_native(unsigned short val, unsigned long port)
{
	volatile unsigned short *addr = __ia64_mk_io_addr(port);

	*addr = val;
	__ia64_mf_a();
}

static inline void __ia64_outl_native(unsigned int val, unsigned long port)
{
	volatile unsigned int *addr = __ia64_mk_io_addr(port);

	*addr = val;
	__ia64_mf_a();
}

/*
 * When CONFIG_IA64_SGI_SN2=y the public inb/outb/... are static-inline
 * runtime dispatchers defined in the SN2 block near the end of this
 * file.  Define them here as direct wrappers over the native versions
 * only when SN2 support is compiled out.
 */
#ifndef CONFIG_IA64_SGI_SN2
#define inb inb
static inline unsigned int inb(unsigned long port) { return __ia64_inb_native(port); }

#define inw inw
static inline unsigned int inw(unsigned long port) { return __ia64_inw_native(port); }

#define inl inl
static inline unsigned int inl(unsigned long port) { return __ia64_inl_native(port); }

#define outb outb
static inline void outb(unsigned char val, unsigned long port) { __ia64_outb_native(val, port); }

#define outw outw
static inline void outw(unsigned short val, unsigned long port) { __ia64_outw_native(val, port); }

#define outl outl
static inline void outl(unsigned int val, unsigned long port) { __ia64_outl_native(val, port); }

#define insb insb
static inline void insb(unsigned long port, void *dst, unsigned long count)
{
	unsigned char *dp = dst;

	while (count--)
		*dp++ = inb(port);
}

#define insw insw
static inline void insw(unsigned long port, void *dst, unsigned long count)
{
	unsigned short *dp = dst;

	while (count--)
		put_unaligned(inw(port), dp++);
}

#define insl insl
static inline void insl(unsigned long port, void *dst, unsigned long count)
{
	unsigned int *dp = dst;

	while (count--)
		put_unaligned(inl(port), dp++);
}

#define outsb outsb
static inline void outsb(unsigned long port, const void *src,
		unsigned long count)
{
	const unsigned char *sp = src;

	while (count--)
		outb(*sp++, port);
}

#define outsw outsw
static inline void outsw(unsigned long port, const void *src,
		unsigned long count)
{
	const unsigned short *sp = src;

	while (count--)
		outw(get_unaligned(sp++), port);
}

#define outsl outsl
static inline void outsl(unsigned long port, const void *src,
		unsigned long count)
{
	const unsigned int *sp = src;

	while (count--)
		outl(get_unaligned(sp++), port);
}
#endif /* !CONFIG_IA64_SGI_SN2 */

# ifdef __KERNEL__

#define _PAGE_IOREMAP pgprot_val(PAGE_KERNEL)

extern void __iomem * ioremap_uc(unsigned long offset, unsigned long size);

#define ioremap_prot ioremap_prot
#define ioremap_cache ioremap
#define ioremap_uc ioremap_uc
#define iounmap iounmap

/*
 * String version of IO memory access ops:
 */
extern void memcpy_fromio(void *dst, const volatile void __iomem *src, long n);
extern void memcpy_toio(volatile void __iomem *dst, const void *src, long n);
extern void memset_io(volatile void __iomem *s, int c, long n);

#define memcpy_fromio memcpy_fromio
#define memcpy_toio memcpy_toio
#define memset_io memset_io

#ifdef CONFIG_IA64_SGI_SN2
/*
 * SN2-aware build: port I/O and MMIO dispatch through the
 * sn2_platform_key static key at runtime.  Native and SN2 (SHUB)
 * paths are both compiled in; the static key steers each call.
 * Defined before <asm-generic/io.h> is included so the generic
 * MMIO wrappers don't emit their own readb/readw/readl/readq.
 */
#include <asm/machvec.h>
#include <asm/sn/io.h>

#define inb inb
static inline unsigned int inb(unsigned long p)
{
	if (ia64_is_sn2())
		return ___sn_inb(p);
	return __ia64_inb_native(p);
}
#define inw inw
static inline unsigned int inw(unsigned long p)
{
	if (ia64_is_sn2())
		return ___sn_inw(p);
	return __ia64_inw_native(p);
}
#define inl inl
static inline unsigned int inl(unsigned long p)
{
	if (ia64_is_sn2())
		return ___sn_inl(p);
	return __ia64_inl_native(p);
}
#define outb outb
static inline void outb(unsigned char v, unsigned long p)
{
	if (ia64_is_sn2())
		___sn_outb(v, p);
	else
		__ia64_outb_native(v, p);
}
#define outw outw
static inline void outw(unsigned short v, unsigned long p)
{
	if (ia64_is_sn2())
		___sn_outw(v, p);
	else
		__ia64_outw_native(v, p);
}
#define outl outl
static inline void outl(unsigned int v, unsigned long p)
{
	if (ia64_is_sn2())
		___sn_outl(v, p);
	else
		__ia64_outl_native(v, p);
}

#define insb(p, d, c)	do { unsigned char *_d = (void *)(d); int _c = (c); \
			  while (_c--) *_d++ = inb(p); } while (0)
#define insw(p, d, c)	do { unsigned short *_d = (void *)(d); int _c = (c); \
			  while (_c--) { put_unaligned(inw(p), _d); _d++; } } while (0)
#define insl(p, d, c)	do { unsigned int *_d = (void *)(d); int _c = (c); \
			  while (_c--) { put_unaligned(inl(p), _d); _d++; } } while (0)
#define outsb(p, s, c)	do { const unsigned char *_s = (const void *)(s); int _c = (c); \
			  while (_c--) outb(*_s++, p); } while (0)
#define outsw(p, s, c)	do { const unsigned short *_s = (const void *)(s); int _c = (c); \
			  while (_c--) { outw(get_unaligned(_s), p); _s++; } } while (0)
#define outsl(p, s, c)	do { const unsigned int *_s = (const void *)(s); int _c = (c); \
			  while (_c--) { outl(get_unaligned(_s), p); _s++; } } while (0)

/*
 * MMIO accessors.  The native path replicates asm-generic/io.h's
 * barrier + volatile load + rmb sequence inline so we don't have
 * to cross-call into asm-generic from here.
 */
#define readb readb
static inline u8 readb(const volatile void __iomem *addr)
{
	u8 val;
	if (ia64_is_sn2())
		return ___sn_readb(addr);
	barrier();
	val = *(const volatile u8 __force *)addr;
	rmb();
	return val;
}
#define readw readw
static inline u16 readw(const volatile void __iomem *addr)
{
	u16 val;
	if (ia64_is_sn2())
		return ___sn_readw(addr);
	barrier();
	val = *(const volatile u16 __force *)addr;
	rmb();
	return val;
}
#define readl readl
static inline u32 readl(const volatile void __iomem *addr)
{
	u32 val;
	if (ia64_is_sn2())
		return ___sn_readl(addr);
	barrier();
	val = *(const volatile u32 __force *)addr;
	rmb();
	return val;
}
#define readq readq
static inline u64 readq(const volatile void __iomem *addr)
{
	u64 val;
	if (ia64_is_sn2())
		return ___sn_readq(addr);
	barrier();
	val = *(const volatile u64 __force *)addr;
	rmb();
	return val;
}
#define readb_relaxed readb_relaxed
static inline u8 readb_relaxed(const volatile void __iomem *addr)
{
	if (ia64_is_sn2())
		return ___sn_readb_relaxed(addr);
	return *(const volatile u8 __force *)addr;
}
#define readw_relaxed readw_relaxed
static inline u16 readw_relaxed(const volatile void __iomem *addr)
{
	if (ia64_is_sn2())
		return ___sn_readw_relaxed(addr);
	return *(const volatile u16 __force *)addr;
}
#define readl_relaxed readl_relaxed
static inline u32 readl_relaxed(const volatile void __iomem *addr)
{
	if (ia64_is_sn2())
		return ___sn_readl_relaxed(addr);
	return *(const volatile u32 __force *)addr;
}
#define readq_relaxed readq_relaxed
static inline u64 readq_relaxed(const volatile void __iomem *addr)
{
	if (ia64_is_sn2())
		return ___sn_readq_relaxed(addr);
	return *(const volatile u64 __force *)addr;
}

#endif /* CONFIG_IA64_SGI_SN2 */

#define xlate_dev_mem_ptr xlate_dev_mem_ptr
#include <asm-generic/io.h>
#undef PCI_IOBASE

# endif /* __KERNEL__ */

#endif /* _ASM_IA64_IO_H */
