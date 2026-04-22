/* SPDX-License-Identifier: GPL-2.0 */
/*
 * SN2 DMA Pool — reserved memory for GPU DMA that lives inside the
 * PIC direct32 window.  See arch/ia64/sn/kernel/sn_dma_pool.c for the
 * problem statement and design.
 */
#ifndef _ASM_IA64_SN_SN_DMA_POOL_H
#define _ASM_IA64_SN_SN_DMA_POOL_H

#include <linux/gfp.h>
#include <linux/mm_types.h>

#ifdef CONFIG_SN2_DMA_POOL

/* Called early during setup_arch, after find_memory.  memblock
 * is active; struct page is not yet valid. */
void __init sn_dma_pool_reserve(void);

/* True once the bitmap allocator is initialized and serving. */
bool sn_dma_pool_active(void);

/* Allocate a run of 1<<order contiguous pages from the pool.
 * Returns head page, or NULL on miss.  gfp_flags: __GFP_ZERO honored. */
struct page *sn_dma_pool_alloc(unsigned int order, gfp_t gfp_flags);

/* Return a run previously obtained from sn_dma_pool_alloc. */
void sn_dma_pool_free(struct page *page, unsigned int order);

/* True if a page (any page) is inside the pool.  Used by TTM's free
 * path to decide whether to call sn_dma_pool_free or __free_pages. */
bool sn_dma_pool_contains(struct page *page);

/* Print pool counters to dmesg — safe to call at any time. */
void sn_dma_pool_dump_counts(void);

#else  /* !CONFIG_SN2_DMA_POOL */

static inline void sn_dma_pool_reserve(void) { }
static inline bool sn_dma_pool_active(void) { return false; }
static inline struct page *sn_dma_pool_alloc(unsigned int order, gfp_t f)
{ return NULL; }
static inline void sn_dma_pool_free(struct page *p, unsigned int order) { }
static inline bool sn_dma_pool_contains(struct page *p) { return false; }
static inline void sn_dma_pool_dump_counts(void) { }

#endif /* CONFIG_SN2_DMA_POOL */

#endif /* _ASM_IA64_SN_SN_DMA_POOL_H */
