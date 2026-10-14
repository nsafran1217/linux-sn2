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

#define inb inb
static inline unsigned int inb(unsigned long port)
{
	volatile unsigned char *addr = __ia64_mk_io_addr(port);
	unsigned char ret;

	ret = *addr;
	__ia64_mf_a();
	return ret;
}

#define inw inw
static inline unsigned int inw(unsigned long port)
{
	volatile unsigned short *addr = __ia64_mk_io_addr(port);
	unsigned short ret;

	ret = *addr;
	__ia64_mf_a();
	return ret;
}

#define inl inl
static inline unsigned int inl(unsigned long port)
{
	volatile unsigned int *addr = __ia64_mk_io_addr(port);
	unsigned int ret;

	ret = *addr;
	__ia64_mf_a();
	return ret;
}

#define outb outb
static inline void outb(unsigned char val, unsigned long port)
{
	volatile unsigned char *addr = __ia64_mk_io_addr(port);

	*addr = val;
	__ia64_mf_a();
}

#define outw outw
static inline void outw(unsigned short val, unsigned long port)
{
	volatile unsigned short *addr = __ia64_mk_io_addr(port);

	*addr = val;
	__ia64_mf_a();
}

#define outl outl
static inline void outl(unsigned int val, unsigned long port)
{
	volatile unsigned int *addr = __ia64_mk_io_addr(port);

	*addr = val;
	__ia64_mf_a();
}

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

#ifdef CONFIG_IA64_SGI_SN2_ONLY
/*
 * SN2 overrides port I/O and MMIO to route through SHUB.
 * Undef the generic ia64 definitions and replace with SN2 versions
 * BEFORE including asm-generic/io.h so it sees our definitions.
 */
#include <asm/sn/io.h>

#undef inb
#undef inw
#undef inl
#undef outb
#undef outw
#undef outl
#undef insb
#undef insw
#undef insl
#undef outsb
#undef outsw
#undef outsl

#define inb(p)		___sn_inb(p)
#define inw(p)		___sn_inw(p)
#define inl(p)		___sn_inl(p)
#define outb(v, p)	___sn_outb(v, p)
#define outw(v, p)	___sn_outw(v, p)
#define outl(v, p)	___sn_outl(v, p)

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

#define readb(a)	___sn_readb(a)
#define readw(a)	___sn_readw(a)
#define readl(a)	___sn_readl(a)
#define readq(a)	___sn_readq(a)
#define readb_relaxed(a)	___sn_readb_relaxed(a)
#define readw_relaxed(a)	___sn_readw_relaxed(a)
#define readl_relaxed(a)	___sn_readl_relaxed(a)
#define readq_relaxed(a)	___sn_readq_relaxed(a)

#elif defined(CONFIG_IA64_SGI_SN2)
/*
 * Kernel for SN2 and generic ia64: the port I/O accessors test the
 * platform at run time.  On SN2 they call the out-of-line SHub versions
 * in arch/ia64/sn/kernel/sn2/io.c; otherwise the native inline versions
 * above run (inb etc. below still name them).  ins*()/outs*() test once
 * per call, not per element.
 */
#include <asm/machvec.h>

extern unsigned int sn2_inb(unsigned long port);
extern unsigned int sn2_inw(unsigned long port);
extern unsigned int sn2_inl(unsigned long port);
extern void sn2_outb(unsigned char val, unsigned long port);
extern void sn2_outw(unsigned short val, unsigned long port);
extern void sn2_outl(unsigned int val, unsigned long port);
extern void sn2_insb(unsigned long port, void *dst, unsigned long count);
extern void sn2_insw(unsigned long port, void *dst, unsigned long count);
extern void sn2_insl(unsigned long port, void *dst, unsigned long count);
extern void sn2_outsb(unsigned long port, const void *src, unsigned long count);
extern void sn2_outsw(unsigned long port, const void *src, unsigned long count);
extern void sn2_outsl(unsigned long port, const void *src, unsigned long count);

#define __IA64_SN2_IN(sz, type)						\
static inline unsigned int __ia64_in##sz(unsigned long port)		\
{									\
	if (ia64_is_sn2())						\
		return sn2_in##sz(port);				\
	return in##sz(port);						\
}
#define __IA64_SN2_OUT(sz, type)					\
static inline void __ia64_out##sz(type val, unsigned long port)	\
{									\
	if (ia64_is_sn2())						\
		sn2_out##sz(val, port);					\
	else								\
		out##sz(val, port);					\
}
#define __IA64_SN2_INS(sz)						\
static inline void __ia64_ins##sz(unsigned long port, void *dst,	\
				  unsigned long count)			\
{									\
	if (ia64_is_sn2())						\
		sn2_ins##sz(port, dst, count);				\
	else								\
		ins##sz(port, dst, count);				\
}
#define __IA64_SN2_OUTS(sz)						\
static inline void __ia64_outs##sz(unsigned long port, const void *src,	\
				   unsigned long count)			\
{									\
	if (ia64_is_sn2())						\
		sn2_outs##sz(port, src, count);				\
	else								\
		outs##sz(port, src, count);				\
}

__IA64_SN2_IN(b, unsigned char)
__IA64_SN2_IN(w, unsigned short)
__IA64_SN2_IN(l, unsigned int)
__IA64_SN2_OUT(b, unsigned char)
__IA64_SN2_OUT(w, unsigned short)
__IA64_SN2_OUT(l, unsigned int)
__IA64_SN2_INS(b)
__IA64_SN2_INS(w)
__IA64_SN2_INS(l)
__IA64_SN2_OUTS(b)
__IA64_SN2_OUTS(w)
__IA64_SN2_OUTS(l)

#undef __IA64_SN2_IN
#undef __IA64_SN2_OUT
#undef __IA64_SN2_INS
#undef __IA64_SN2_OUTS

#undef inb
#undef inw
#undef inl
#undef outb
#undef outw
#undef outl
#undef insb
#undef insw
#undef insl
#undef outsb
#undef outsw
#undef outsl

#define inb	__ia64_inb
#define inw	__ia64_inw
#define inl	__ia64_inl
#define outb	__ia64_outb
#define outw	__ia64_outw
#define outl	__ia64_outl
#define insb	__ia64_insb
#define insw	__ia64_insw
#define insl	__ia64_insl
#define outsb	__ia64_outsb
#define outsw	__ia64_outsw
#define outsl	__ia64_outsl

#endif /* CONFIG_IA64_SGI_SN2 */

#define xlate_dev_mem_ptr xlate_dev_mem_ptr
#include <asm-generic/io.h>
#undef PCI_IOBASE

#if defined(CONFIG_IA64_SGI_SN2) && !defined(CONFIG_IA64_SGI_SN2_ONLY)
/*
 * readX() on SN2 adds mf.a and a PIC DMA flush after the load.  Wrap the
 * asm-generic versions (readb etc. below still name them) with a run
 * time test.  The _relaxed and writeX() accessors are the same on both
 * platforms and are not wrapped.
 */
extern u8 sn2_readb(const volatile void __iomem *addr);
extern u16 sn2_readw(const volatile void __iomem *addr);
extern u32 sn2_readl(const volatile void __iomem *addr);
extern u64 sn2_readq(const volatile void __iomem *addr);

#define __IA64_SN2_READ(sz, type)					\
static inline type __ia64_read##sz(const volatile void __iomem *addr)	\
{									\
	if (ia64_is_sn2())						\
		return sn2_read##sz(addr);				\
	return read##sz(addr);						\
}

__IA64_SN2_READ(b, u8)
__IA64_SN2_READ(w, u16)
__IA64_SN2_READ(l, u32)
__IA64_SN2_READ(q, u64)

#undef __IA64_SN2_READ

#undef readb
#undef readw
#undef readl
#undef readq

#define readb	__ia64_readb
#define readw	__ia64_readw
#define readl	__ia64_readl
#define readq	__ia64_readq
#endif

# endif /* __KERNEL__ */

#endif /* _ASM_IA64_IO_H */
