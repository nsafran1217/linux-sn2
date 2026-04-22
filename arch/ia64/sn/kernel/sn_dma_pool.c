/*
 * SN2 DMA Pool — Reserved 2GB-aligned memory for GPU DMA.
 *
 * Problem: On SN2, the PIC bridge's direct32 DMA window covers only
 * the bottom 2 GB of node memory (phys 0x30_0000_0000..0x30_7FFF_FFFF).
 * Pages above that fall to the ATE pool (1024 entries × 16 KB = 16 MB
 * total).  A GPU with multi-megabyte GTT mappings exhausts ATE
 * quickly, yielding 'sn_dma_map_phys: out of ATEs' and crashes.
 *
 * Fix: Reserve a contiguous region at node offset 0 (inside the PROM-
 * programmed direct32 window) at boot via memblock_reserve.  Hand out
 * pages from this reserve via a per-order free list (buddy-style).  A
 * TTM hook in ttm_pool_alloc_page asks this allocator first.  Pages
 * from the pool have phys addresses inside the direct32 window, so
 * their DMA mappings consume zero ATEs.
 *
 * Design constraints applied, informed by earlier failed attempts:
 *   - NO hardware writes.  PROM's p_dir_map is correct.
 *   - NO CMA.  Direct memblock_reserve + struct-page bookkeeping.
 *   - NO compound page manipulation.  We return a run of struct pages
 *     with refcount 1 each; TTM does not set __GFP_COMP and is happy
 *     with non-compound multi-page runs tracked via p->private = order.
 *   - Order >0 served when possible; NULL returned on miss so TTM can
 *     drop order and retry.
 *
 * Kernel command line:
 *   sn2_dma_pool=SIZE     — pool size in bytes (K/M/G suffix ok)
 *                           default: 768M, max: 2G, 0 disables
 *
 * The reservation sits at node offset 0 (phys 0x30_0000_0000).  On
 * systems with less than SIZE bytes free at that location the
 * reservation fails and the pool is marked inactive (no harm, just
 * reverts to previous ATE-exhausting behavior).
 */

#include <linux/atomic.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/pfn.h>
#include <linux/printk.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/types.h>

#include <asm/sn/sn_dma_pool.h>

#define SN2_POOL_DEFAULT_SIZE	(768UL << 20)	/* 768 MB */
#define SN2_POOL_MAX_SIZE	(2UL << 30)	/* 2 GB (window size) */
#define SN2_POOL_MAX_ORDER	10		/* orders 0..10 = up to 64MB runs */

/* Command-line parsed values */
static u64 sn2_pool_req_size = SN2_POOL_DEFAULT_SIZE;
static bool sn2_pool_disabled;

/* Pool state — read-mostly after init */
static phys_addr_t sn2_pool_base_phys;		/* phys of pool start */
static unsigned long sn2_pool_base_pfn;		/* pfn of pool start */
static unsigned long sn2_pool_nr_pages;		/* total pages in pool */
static bool sn2_pool_ready;			/* bitmap init complete */

/* Per-order free lists of pages.  Each entry is a struct page whose
 * lru is linked; the run starting at that page is of size 1<<order.
 * Protected by sn2_pool_lock. */
static struct list_head sn2_pool_free[SN2_POOL_MAX_ORDER + 1];
static DEFINE_SPINLOCK(sn2_pool_lock);

/* Stats — visible via sn_dma_pool_dump_counts */
static atomic64_t sn2_pool_alloc_ok;
static atomic64_t sn2_pool_alloc_fail;
static atomic64_t sn2_pool_free_ok;

/* -------------------------------------------------------------------
 * Command-line parsing
 * ------------------------------------------------------------------- */

static int __init sn2_pool_parse(char *str)
{
	u64 size;
	char *end;

	if (!str)
		return 0;
	if (!strcmp(str, "off") || !strcmp(str, "0")) {
		sn2_pool_disabled = true;
		return 0;
	}
	size = memparse(str, &end);
	if (end == str) {
		pr_warn("sn2_dma_pool: malformed size '%s'\n", str);
		return 0;
	}
	if (size > SN2_POOL_MAX_SIZE) {
		pr_warn("sn2_dma_pool: size %llu capped to 2 GB\n", size);
		size = SN2_POOL_MAX_SIZE;
	}
	sn2_pool_req_size = size;
	return 0;
}
early_param("sn2_dma_pool", sn2_pool_parse);

/* -------------------------------------------------------------------
 * Boot-time reservation — called after find_memory()
 *
 * We reserve at phys 0x3000000000 (NASID 0 cacheable, AS_CAC bits
 * 37:36 = 11).  Size is command-line configurable.  The kernel
 * already allocated some stuff in low memory (text, per-cpu, initial
 * pgtables), so we use memblock_phys_alloc_range constrained to
 * [0x3000000000, 0x3000000000+size] to find a free aligned chunk.
 * ------------------------------------------------------------------- */

void __init sn_dma_pool_reserve(void)
{
	phys_addr_t want_base = 0x3000000000ULL;
	phys_addr_t want_end  = want_base + SN2_POOL_MAX_SIZE;  /* within 2GB window */
	phys_addr_t got;

	if (sn2_pool_disabled) {
		pr_info("sn_dma_pool: disabled by command line\n");
		return;
	}
	if (sn2_pool_req_size == 0) {
		sn2_pool_disabled = true;
		return;
	}
	if (sn2_pool_req_size & (PAGE_SIZE - 1)) {
		pr_warn("sn_dma_pool: rounding size up to PAGE_SIZE\n");
		sn2_pool_req_size = PAGE_ALIGN(sn2_pool_req_size);
	}

	/* memblock_phys_alloc_range: find a chunk of sn2_pool_req_size bytes,
	 * aligned to 2MB (for nicer struct-page alignment and to allow
	 * the allocator to serve up to order-5 requests cleanly), within
	 * [want_base, want_end).  Returns phys addr or 0 on failure. */
	got = memblock_phys_alloc_range(sn2_pool_req_size,
					 SZ_2M, want_base, want_end);
	if (!got) {
		pr_warn("sn_dma_pool: failed to reserve %llu bytes in [0x%llx,0x%llx)\n",
			(u64)sn2_pool_req_size,
			(u64)want_base, (u64)want_end);
		sn2_pool_disabled = true;
		return;
	}

	sn2_pool_base_phys = got;
	sn2_pool_base_pfn  = PFN_DOWN(got);
	sn2_pool_nr_pages  = sn2_pool_req_size >> PAGE_SHIFT;

	pr_info("sn_dma_pool: reserved %llu bytes at phys 0x%llx (pfn 0x%lx, %lu pages)\n",
		(u64)sn2_pool_req_size, (u64)got,
		sn2_pool_base_pfn, sn2_pool_nr_pages);
}

/* -------------------------------------------------------------------
 * Buddy-style free-list initialization — at arch_initcall
 *
 * By this point, struct page is valid for the reserved range.  We
 * build per-order free lists by walking the pool, inserting the
 * largest possible power-of-2-aligned run at each step.  This is the
 * standard initialization pattern for a buddy allocator.
 * ------------------------------------------------------------------- */

static void sn2_pool_add_run(unsigned long pfn, unsigned int order)
{
	struct page *p = pfn_to_page(pfn);
	unsigned long i, count = 1UL << order;

	/* Sanity — pfn must be aligned to 1<<order pages */
	if (WARN_ON(pfn & ((1UL << order) - 1)))
		return;

	/* Pages from memblock_phys_alloc_range are reserved in memblock
	 * and never enter the buddy free lists.  Their struct pages are
	 * initialized by memmap_init_reserved_pages but not placed on
	 * any allocator's list.  We take ownership: refcount 0, owned
	 * by the pool.  When we hand one out in sn_dma_pool_alloc we
	 * bump it to 1 (matching alloc_pages semantics). */
	for (i = 0; i < count; i++)
		set_page_count(p + i, 0);

	INIT_LIST_HEAD(&p->lru);
	list_add(&p->lru, &sn2_pool_free[order]);
}

static int __init sn2_pool_init(void)
{
	unsigned long pfn, end_pfn;
	int i;

	if (sn2_pool_disabled || sn2_pool_nr_pages == 0)
		return 0;

	for (i = 0; i <= SN2_POOL_MAX_ORDER; i++)
		INIT_LIST_HEAD(&sn2_pool_free[i]);

	pfn      = sn2_pool_base_pfn;
	end_pfn  = pfn + sn2_pool_nr_pages;

	/* Break the pool into power-of-2-aligned runs, largest first at
	 * each step.  Classic buddy init. */
	while (pfn < end_pfn) {
		unsigned int order = SN2_POOL_MAX_ORDER;

		while (order > 0) {
			unsigned long size = 1UL << order;
			if ((pfn & (size - 1)) == 0 && pfn + size <= end_pfn)
				break;
			order--;
		}
		sn2_pool_add_run(pfn, order);
		pfn += 1UL << order;
	}

	sn2_pool_ready = true;
	pr_info("sn_dma_pool: initialized buddy free lists\n");
	return 0;
}
arch_initcall(sn2_pool_init);

/* -------------------------------------------------------------------
 * Allocator core — buddy split / coalesce
 * ------------------------------------------------------------------- */

/* Must hold sn2_pool_lock. */
static struct page *sn2_pool_take_locked(unsigned int order)
{
	unsigned int o;
	struct page *p;

	if (order > SN2_POOL_MAX_ORDER)
		return NULL;

	/* Find the smallest free run ≥ order */
	for (o = order; o <= SN2_POOL_MAX_ORDER; o++) {
		if (!list_empty(&sn2_pool_free[o]))
			break;
	}
	if (o > SN2_POOL_MAX_ORDER)
		return NULL;

	p = list_first_entry(&sn2_pool_free[o], struct page, lru);
	list_del_init(&p->lru);

	/* Split down to the requested order.  The upper half of each
	 * split goes back on the free list one order lower. */
	while (o > order) {
		unsigned long pfn;
		struct page *buddy;
		o--;
		pfn = page_to_pfn(p) + (1UL << o);
		buddy = pfn_to_page(pfn);
		INIT_LIST_HEAD(&buddy->lru);
		list_add(&buddy->lru, &sn2_pool_free[o]);
	}
	return p;
}

/* Must hold sn2_pool_lock.  'order' is the order of the run starting
 * at page p.  Attempts to coalesce with a buddy at the same order. */
static void sn2_pool_give_locked(struct page *p, unsigned int order)
{
	unsigned long pfn = page_to_pfn(p);

	while (order < SN2_POOL_MAX_ORDER) {
		unsigned long buddy_pfn = pfn ^ (1UL << order);
		struct page *buddy;
		struct list_head *lh;
		bool found = false;

		/* Is buddy_pfn inside pool bounds and free at this order? */
		if (buddy_pfn < sn2_pool_base_pfn ||
		    buddy_pfn + (1UL << order) >
		    sn2_pool_base_pfn + sn2_pool_nr_pages)
			break;

		buddy = pfn_to_page(buddy_pfn);

		list_for_each(lh, &sn2_pool_free[order]) {
			if (lh == &buddy->lru) {
				found = true;
				break;
			}
		}
		if (!found)
			break;

		list_del_init(&buddy->lru);
		if (buddy_pfn < pfn) {
			pfn = buddy_pfn;
			p = buddy;
		}
		order++;
	}

	INIT_LIST_HEAD(&p->lru);
	list_add(&p->lru, &sn2_pool_free[order]);
}

/* -------------------------------------------------------------------
 * Public API — called from the TTM hook
 * ------------------------------------------------------------------- */

bool sn_dma_pool_active(void)
{
	return sn2_pool_ready;
}
EXPORT_SYMBOL_GPL(sn_dma_pool_active);

struct page *sn_dma_pool_alloc(unsigned int order, gfp_t gfp_flags)
{
	struct page *p;
	unsigned long flags;
	unsigned long i, count;

	if (!sn2_pool_ready)
		return NULL;
	if (order > SN2_POOL_MAX_ORDER) {
		atomic64_inc(&sn2_pool_alloc_fail);
		return NULL;
	}

	spin_lock_irqsave(&sn2_pool_lock, flags);
	p = sn2_pool_take_locked(order);
	spin_unlock_irqrestore(&sn2_pool_lock, flags);

	if (!p) {
		atomic64_inc(&sn2_pool_alloc_fail);
		return NULL;
	}

	/* Each page in the run gets refcount 1, mirroring the
	 * alloc_pages() contract (non-compound).  TTM calls
	 * __free_pages(p, order) at free time which will call
	 * put_page_testzero on each constituent — we need them to be
	 * properly refcounted for that.  Actually no — TTM's free path
	 * goes through our sn_dma_pool_free since sn_dma_pool_contains
	 * returns true, so the refcount behavior only matters if
	 * somebody else calls put_page on these pages.  Set to 1 for
	 * safety and to match alloc_pages semantics.
	 *
	 * Also zero the memory since TTM may rely on __GFP_ZERO being
	 * honored on fresh allocations. */
	count = 1UL << order;
	for (i = 0; i < count; i++) {
		struct page *sub = p + i;
		set_page_count(sub, 1);
		if (gfp_flags & __GFP_ZERO) {
			void *addr = page_address(sub);
			if (addr)
				memset(addr, 0, PAGE_SIZE);
		}
	}

	atomic64_inc(&sn2_pool_alloc_ok);
	return p;
}
EXPORT_SYMBOL_GPL(sn_dma_pool_alloc);

void sn_dma_pool_free(struct page *p, unsigned int order)
{
	unsigned long flags;
	unsigned long i, count;

	if (!sn2_pool_ready || !p)
		return;
	if (order > SN2_POOL_MAX_ORDER) {
		WARN_ONCE(1, "sn_dma_pool_free: bad order %u\n", order);
		return;
	}

	/* Drop all refcounts to 0 — mirrors what the buddy allocator
	 * expects when receiving pages back. */
	count = 1UL << order;
	for (i = 0; i < count; i++)
		set_page_count(p + i, 0);

	spin_lock_irqsave(&sn2_pool_lock, flags);
	sn2_pool_give_locked(p, order);
	spin_unlock_irqrestore(&sn2_pool_lock, flags);

	atomic64_inc(&sn2_pool_free_ok);
}
EXPORT_SYMBOL_GPL(sn_dma_pool_free);

bool sn_dma_pool_contains(struct page *p)
{
	unsigned long pfn;

	if (!sn2_pool_ready || !p)
		return false;
	pfn = page_to_pfn(p);
	return pfn >= sn2_pool_base_pfn &&
	       pfn <  sn2_pool_base_pfn + sn2_pool_nr_pages;
}
EXPORT_SYMBOL_GPL(sn_dma_pool_contains);

void sn_dma_pool_dump_counts(void)
{
	if (!sn2_pool_ready) {
		pr_info("[SN2-POOL] inactive\n");
		return;
	}
	pr_info("[SN2-POOL] base_pfn=0x%lx nr_pages=%lu  alloc_ok=%llu alloc_fail=%llu free_ok=%llu\n",
		sn2_pool_base_pfn, sn2_pool_nr_pages,
		(u64)atomic64_read(&sn2_pool_alloc_ok),
		(u64)atomic64_read(&sn2_pool_alloc_fail),
		(u64)atomic64_read(&sn2_pool_free_ok));
}
EXPORT_SYMBOL_GPL(sn_dma_pool_dump_counts);

/* -------------------------------------------------------------------
 * /proc/sn_dma_pool — on-demand status read
 *
 * Example output:
 *   pool: active
 *   base_phys:     0x3050000000
 *   base_pfn:      0x305000
 *   total_pages:   12288         (= 768 MB @ 64KB pages)
 *   free_pages:    9144          (= 571 MB free)
 *   used_pages:    3144          (= 197 MB in use)
 *   alloc_ok:      52341
 *   alloc_fail:    17
 *   free_ok:       49197
 *   free-by-order:
 *     order  0:  42 runs (42 pages, 2.6 MB)
 *     order  5:  18 runs (576 pages, 36 MB)
 *     order  8:  34 runs (8704 pages, 544 MB)
 *     ...
 * ------------------------------------------------------------------- */

#ifdef CONFIG_PROC_FS
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

static int sn_dma_pool_proc_show(struct seq_file *m, void *v)
{
	unsigned long flags, free_pages = 0;
	unsigned int order;
	unsigned long per_order_runs[SN2_POOL_MAX_ORDER + 1] = { 0 };

	if (!sn2_pool_ready) {
		seq_puts(m, "pool: inactive\n");
		return 0;
	}

	spin_lock_irqsave(&sn2_pool_lock, flags);
	for (order = 0; order <= SN2_POOL_MAX_ORDER; order++) {
		struct list_head *lh;
		unsigned long runs = 0;
		list_for_each(lh, &sn2_pool_free[order])
			runs++;
		per_order_runs[order] = runs;
		free_pages += runs << order;
	}
	spin_unlock_irqrestore(&sn2_pool_lock, flags);

	seq_puts(m,   "pool:          active\n");
	seq_printf(m, "base_phys:     0x%llx\n", (u64)sn2_pool_base_phys);
	seq_printf(m, "base_pfn:      0x%lx\n",  sn2_pool_base_pfn);
	seq_printf(m, "total_pages:   %lu (%lu MB)\n",
		   sn2_pool_nr_pages,
		   (sn2_pool_nr_pages * PAGE_SIZE) >> 20);
	seq_printf(m, "free_pages:    %lu (%lu MB)\n",
		   free_pages, (free_pages * PAGE_SIZE) >> 20);
	seq_printf(m, "used_pages:    %lu (%lu MB)\n",
		   sn2_pool_nr_pages - free_pages,
		   ((sn2_pool_nr_pages - free_pages) * PAGE_SIZE) >> 20);
	seq_printf(m, "alloc_ok:      %llu\n",
		   (u64)atomic64_read(&sn2_pool_alloc_ok));
	seq_printf(m, "alloc_fail:    %llu\n",
		   (u64)atomic64_read(&sn2_pool_alloc_fail));
	seq_printf(m, "free_ok:       %llu\n",
		   (u64)atomic64_read(&sn2_pool_free_ok));
	seq_puts(m,   "free-by-order:\n");
	for (order = 0; order <= SN2_POOL_MAX_ORDER; order++) {
		unsigned long runs = per_order_runs[order];
		unsigned long pages = runs << order;
		if (runs == 0)
			continue;
		seq_printf(m, "  order %2u:  %5lu runs (%lu pages, %lu KB)\n",
			   order, runs, pages,
			   (pages * PAGE_SIZE) >> 10);
	}
	return 0;
}

static int __init sn_dma_pool_proc_init(void)
{
	if (!sn2_pool_ready)
		return 0;
	proc_create_single("sn_dma_pool", 0, NULL, sn_dma_pool_proc_show);
	return 0;
}
/* Must run after arch_initcall(sn2_pool_init) so sn2_pool_ready
 * is set.  fs_initcall runs later. */
fs_initcall(sn_dma_pool_proc_init);
#endif
