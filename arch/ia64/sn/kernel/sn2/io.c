/* 
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2003 Silicon Graphics, Inc. All rights reserved.
 *
 * A kernel that runs on both SN2 and generic ia64 calls these from the
 * accessors in asm/io.h when ia64_is_sn2() is true, so the SN2 bodies
 * aren't inlined at every call site.  We wrap the inlines from
 * asm/sn/io.h here.  An IA64_SGI_SN2_ONLY kernel inlines them directly
 * and doesn't need these.
 */

#include <linux/export.h>
#include <linux/unaligned.h>
#include <asm/io.h>
#include <asm/sn/io.h>

#ifndef CONFIG_IA64_SGI_SN2_ONLY

#define SN2_PORT_IO(sz, type)						\
unsigned int sn2_in##sz(unsigned long port)				\
{									\
	return ___sn_in##sz(port);					\
}									\
EXPORT_SYMBOL(sn2_in##sz);						\
									\
void sn2_out##sz(type val, unsigned long port)				\
{									\
	___sn_out##sz(val, port);					\
}									\
EXPORT_SYMBOL(sn2_out##sz);						\
									\
void sn2_ins##sz(unsigned long port, void *dst, unsigned long count)	\
{									\
	type *d = dst;							\
									\
	while (count--)							\
		put_unaligned(___sn_in##sz(port), d++);			\
}									\
EXPORT_SYMBOL(sn2_ins##sz);						\
									\
void sn2_outs##sz(unsigned long port, const void *src,			\
		  unsigned long count)					\
{									\
	const type *s = src;						\
									\
	while (count--)							\
		___sn_out##sz(get_unaligned(s++), port);		\
}									\
EXPORT_SYMBOL(sn2_outs##sz);

SN2_PORT_IO(b, unsigned char)
SN2_PORT_IO(w, unsigned short)
SN2_PORT_IO(l, unsigned int)

#define SN2_READ(sz, type)						\
type sn2_read##sz(const volatile void __iomem *addr)			\
{									\
	return ___sn_read##sz(addr);					\
}									\
EXPORT_SYMBOL(sn2_read##sz);

SN2_READ(b, u8)
SN2_READ(w, u16)
SN2_READ(l, u32)
SN2_READ(q, u64)

#endif /* !CONFIG_IA64_SGI_SN2_ONLY */
