/*
**  IA64 System Bus Adapter (SBA) I/O MMU manager
**
**	(c) Copyright 2002-2004 Alex Williamson
**	(c) Copyright 2002-2003 Grant Grundler
**	(c) Copyright 2002-2004 Hewlett-Packard Company
**
**	Portions (c) 2000 Grant Grundler (from parisc I/O MMU code)
**	Portions (c) 1999 Dave S. Miller (from sparc64 I/O MMU code)
**
**	This program is free software; you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**      the Free Software Foundation; either version 2 of the License, or
**      (at your option) any later version.
**
**
** This module initializes the IOC (I/O Controller) found on HP
** McKinley machines and their successors.
**
*/

#include <linux/config.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/pci.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/acpi.h>
#include <linux/efi.h>

#include <asm/delay.h>		/* ia64_get_itc() */
#include <asm/io.h>
#include <asm/page.h>		/* PAGE_OFFSET */
#include <asm/system.h>		/* wmb() */
#include <asm/bitops.h>		/* hweight64() */


#define PFX "IOC: "

/*
** Enabling timing search of the pdir resource map.  Output in /proc.
** Disabled by default to optimize performance.
*/
#undef PDIR_SEARCH_TIMING

/*
** This option allows cards capable of 64bit DMA to bypass the IOMMU.  If
** not defined, all DMA will be 32bit and go through the TLB.
*/
#define ALLOW_IOV_BYPASS

/*
** If a device prefetches beyond the end of a valid pdir entry, it will cause
** a hard failure, ie. MCA.  Version 3.0 and later of the zx1 LBA should
** disconnect on 4k boundaries and prevent such issues.  If the device is
** particularly agressive, this option will keep the entire pdir valid such
** that prefetching will hit a valid address.  This could severely impact
** error containment, and is therefore off by default.  The page that is
** used for spill-over is poisoned, so that should help debugging somewhat.
*/
#undef FULL_VALID_PDIR

#define ENABLE_MARK_CLEAN

/*
** The number of debug flags is a clue - this code is fragile.
*/
#undef DEBUG_SBA_INIT
#undef DEBUG_SBA_RUN
#undef DEBUG_SBA_RUN_SG
#undef DEBUG_SBA_RESOURCE
#undef ASSERT_PDIR_SANITY
#undef DEBUG_LARGE_SG_ENTRIES
#undef DEBUG_BYPASS

#if defined(FULL_VALID_PDIR) && defined(ASSERT_PDIR_SANITY)
#error FULL_VALID_PDIR and ASSERT_PDIR_SANITY are mutually exclusive
#endif

#define SBA_INLINE	__inline__
/* #define SBA_INLINE */

#ifdef DEBUG_SBA_INIT
#define DBG_INIT(x...)	printk(x)
#else
#define DBG_INIT(x...)
#endif

#ifdef DEBUG_SBA_RUN
#define DBG_RUN(x...)	printk(x)
#else
#define DBG_RUN(x...)
#endif

#ifdef DEBUG_SBA_RUN_SG
#define DBG_RUN_SG(x...)	printk(x)
#else
#define DBG_RUN_SG(x...)
#endif


#ifdef DEBUG_SBA_RESOURCE
#define DBG_RES(x...)	printk(x)
#else
#define DBG_RES(x...)
#endif

#ifdef DEBUG_BYPASS
#define DBG_BYPASS(x...)	printk(x)
#else
#define DBG_BYPASS(x...)
#endif

#ifdef ASSERT_PDIR_SANITY
#define ASSERT(expr) \
        if(!(expr)) { \
                printk( "\n" __FILE__ ":%d: Assertion " #expr " failed!\n",__LINE__); \
                panic(#expr); \
        }
#else
#define ASSERT(expr)
#endif

/*
** The number of pdir entries to "free" before issuing
** a read to PCOM register to flush out PCOM writes.
** Interacts with allocation granularity (ie 4 or 8 entries
** allocated and free'd/purged at a time might make this
** less interesting).
*/
#define DELAYED_RESOURCE_CNT	16

#define DEFAULT_DMA_HINT_REG	0

#define ZX1_IOC_ID	((PCI_DEVICE_ID_HP_ZX1_IOC << 16) | PCI_VENDOR_ID_HP)
#define REO_IOC_ID	((PCI_DEVICE_ID_HP_REO_IOC << 16) | PCI_VENDOR_ID_HP)
#define SX1000_IOC_ID	((PCI_DEVICE_ID_HP_SX1000_IOC << 16) | PCI_VENDOR_ID_HP)

#define ZX1_IOC_OFFSET	0x1000	/* ACPI reports SBA, we want IOC */

#define IOC_FUNC_ID	0x000
#define IOC_FCLASS	0x008	/* function class, bist, header, rev... */
#define IOC_IBASE	0x300	/* IO TLB */
#define IOC_IMASK	0x308
#define IOC_PCOM	0x310
#define IOC_TCNFG	0x318
#define IOC_PDIR_BASE	0x320

/* AGP GART driver looks for this */
#define ZX1_SBA_IOMMU_COOKIE	0x0000badbadc0ffeeUL

/*
** The zx1 IOC supports 4/8/16/64KB page sizes (see TCNFG register)
**
** Some IOCs (sx1000) can run at the above pages sizes, but are
** really only supported using the IOC at a 4k page size.
**
** iovp_size could only be greater than PAGE_SIZE if we are
** confident the drivers really only touch the next physical
** page iff that driver instance owns it.
*/
static unsigned long iovp_size;
static unsigned long iovp_shift;
static unsigned long iovp_mask;

struct ioc {
	void		*ioc_hpa;	/* I/O MMU base address */
	char		*res_map;	/* resource map, bit == pdir entry */
	u64		*pdir_base;	/* physical base address */
	unsigned long	ibase;		/* pdir IOV Space base */
	unsigned long	imask;		/* pdir IOV Space mask */

	unsigned long	*res_hint;	/* next avail IOVP - circular search */
	spinlock_t	res_lock;
	unsigned long	hint_mask_pdir;	/* bits used for DMA hints */
	unsigned int	res_bitshift;	/* from the RIGHT! */
	unsigned int	res_size;	/* size of resource map in bytes */
	unsigned int	hint_shift_pdir;
	unsigned long	dma_mask;
#if DELAYED_RESOURCE_CNT > 0
	int saved_cnt;
	struct sba_dma_pair {
		dma_addr_t	iova;
		size_t		size;
	} saved[DELAYED_RESOURCE_CNT];
#endif

#ifdef PDIR_SEARCH_TIMING
#define SBA_SEARCH_SAMPLE	0x100
	unsigned long avg_search[SBA_SEARCH_SAMPLE];
	unsigned long avg_idx;	/* current index into avg_search */
#endif

	/* Stuff we don't need in performance path */
	struct ioc	*next;		/* list of IOC's in system */
	acpi_handle	handle;		/* for multiple IOC's */
	const char 	*name;
	unsigned int	func_id;
	unsigned int	rev;		/* HW revision of chip */
	u32		iov_size;
	unsigned int	pdir_size;	/* in bytes, determined by IOV Space size */
	struct pci_dev	*sac_only_dev;
};

static struct ioc *ioc_list;
static int reserve_sba_gart = 1;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0)
#define sba_sg_address(sg)	(page_address((sg)->page) + (sg)->offset)
#else
#define sba_sg_address(sg)	((sg)->address ? (sg)->address : \
                                  page_address((sg)->page) + (sg)->offset)
#endif

#ifdef FULL_VALID_PDIR
static u64 prefetch_spill_page;
#endif

#ifdef CONFIG_PCI
# define GET_IOC(dev)	((struct ioc *) PCI_CONTROLLER(dev)->iommu)
#else
# define GET_IOC(dev)	NULL
#endif

/*
** DMA_CHUNK_SIZE is used by the SCSI mid-layer to break up
** (or rather not merge) DMA's into managable chunks.
** On parisc, this is more of the software/tuning constraint
** rather than the HW. I/O MMU allocation alogorithms can be
** faster with smaller size is (to some degree).
*/
#define DMA_CHUNK_SIZE  (BITS_PER_LONG*iovp_size)

#define ROUNDUP(x,y) ((x + ((y)-1)) & ~((y)-1))

/************************************
** SBA register read and write support
**
** BE WARNED: register writes are posted.
**  (ie follow writes which must reach HW with a read)
**
*/
#define READ_REG(addr)       __raw_readq(addr)
#define WRITE_REG(val, addr) __raw_writeq(val, addr)

#ifdef DEBUG_SBA_INIT

/**
 * sba_dump_tlb - debugging only - print IOMMU operating parameters
 * @hpa: base address of the IOMMU
 *
 * Print the size/location of the IO MMU PDIR.
 */
static void
sba_dump_tlb(char *hpa)
{
	DBG_INIT("IO TLB at 0x%p\n", (void *)hpa);
	DBG_INIT("IOC_IBASE    : %016lx\n", READ_REG(hpa+IOC_IBASE));
	DBG_INIT("IOC_IMASK    : %016lx\n", READ_REG(hpa+IOC_IMASK));
	DBG_INIT("IOC_TCNFG    : %016lx\n", READ_REG(hpa+IOC_TCNFG));
	DBG_INIT("IOC_PDIR_BASE: %016lx\n", READ_REG(hpa+IOC_PDIR_BASE));
	DBG_INIT("\n");
}
#endif


#ifdef ASSERT_PDIR_SANITY

/**
 * sba_dump_pdir_entry - debugging only - print one IOMMU PDIR entry
 * @ioc: IO MMU structure which owns the pdir we are interested in.
 * @msg: text to print ont the output line.
 * @pide: pdir index.
 *
 * Print one entry of the IO MMU PDIR in human readable form.
 */
static void
sba_dump_pdir_entry(struct ioc *ioc, char *msg, uint pide)
{
	/* start printing from lowest pde in rval */
	u64 *ptr = &ioc->pdir_base[pide  & ~(BITS_PER_LONG - 1)];
	unsigned long *rptr = (unsigned long *) &ioc->res_map[(pide >>3) & -sizeof(unsigned long)];
	uint rcnt;

	printk(KERN_DEBUG "SBA: %s rp %p bit %d rval 0x%lx\n",
		 msg, rptr, pide & (BITS_PER_LONG - 1), *rptr);

	rcnt = 0;
	while (rcnt < BITS_PER_LONG) {
		printk(KERN_DEBUG "%s %2d %p %016Lx\n",
		       (rcnt == (pide & (BITS_PER_LONG - 1)))
		       ? "    -->" : "       ",
		       rcnt, ptr, (unsigned long long) *ptr );
		rcnt++;
		ptr++;
	}
	printk(KERN_DEBUG "%s", msg);
}


/**
 * sba_check_pdir - debugging only - consistency checker
 * @ioc: IO MMU structure which owns the pdir we are interested in.
 * @msg: text to print ont the output line.
 *
 * Verify the resource map and pdir state is consistent
 */
static int
sba_check_pdir(struct ioc *ioc, char *msg)
{
	u64 *rptr_end = (u64 *) &(ioc->res_map[ioc->res_size]);
	u64 *rptr = (u64 *) ioc->res_map;	/* resource map ptr */
	u64 *pptr = ioc->pdir_base;	/* pdir ptr */
	uint pide = 0;

	while (rptr < rptr_end) {
		u64 rval;
		int rcnt; /* number of bits we might check */

		rval = *rptr;
		rcnt = 64;

		while (rcnt) {
			/* Get last byte and highest bit from that */
			u32 pde = ((u32)((*pptr >> (63)) & 0x1));
			if ((rval & 0x1) ^ pde)
			{
				/*
				** BUMMER!  -- res_map != pdir --
				** Dump rval and matching pdir entries
				*/
				sba_dump_pdir_entry(ioc, msg, pide);
				return(1);
			}
			rcnt--;
			rval >>= 1;	/* try the next bit */
			pptr++;
			pide++;
		}
		rptr++;	/* look at next word of res_map */
	}
	/* It'd be nice if we always got here :^) */
	return 0;
}


/**
 * sba_dump_sg - debugging only - print Scatter-Gather list
 * @ioc: IO MMU structure which owns the pdir we are interested in.
 * @startsg: head of the SG list
 * @nents: number of entries in SG list
 *
 * print the SG list so we can verify it's correct by hand.
 */
static void
sba_dump_sg( struct ioc *ioc, struct scatterlist *startsg, int nents)
{
	while (nents-- > 0) {
		printk(KERN_DEBUG " %d : DMA %08lx/%05x CPU %p\n", nents,
		       startsg->dma_address, startsg->dma_length,
		       sba_sg_address(startsg));
		startsg++;
	}
}

static void
sba_check_sg( struct ioc *ioc, struct scatterlist *startsg, int nents)
{
	struct scatterlist *the_sg = startsg;
	int the_nents = nents;

	while (the_nents-- > 0) {
		if (sba_sg_address(the_sg) == 0x0UL)
			sba_dump_sg(NULL, startsg, nents);
		the_sg++;
	}
}

#endif /* ASSERT_PDIR_SANITY */




/**************************************************************
*
*   I/O Pdir Resource Management
*
*   Bits set in the resource map are in use.
*   Each bit can represent a number of pages.
*   LSbs represent lower addresses (IOVA's).
*
***************************************************************/
#define PAGES_PER_RANGE 1	/* could increase this to 4 or 8 if needed */

/* Convert from IOVP to IOVA and vice versa. */
#define SBA_IOVA(ioc,iovp,offset,hint_reg) ((ioc->ibase) | (iovp) | (offset))
#define SBA_IOVP(ioc,iova) ((iova) & ~(ioc->ibase))

#define PDIR_ENTRY_SIZE	sizeof(u64)

#define PDIR_INDEX(iovp)   ((iovp)>>iovp_shift)

#define RESMAP_MASK(n)    ~(~0UL << (n))
#define RESMAP_IDX_MASK   (sizeof(unsigned long) - 1)


/**
 * For most cases the normal get_order is sufficient, however it limits us
 * to PAGE_SIZE being the minimum mapping alignment and TC flush granularity.
 * It only incurs about 1 clock cycle to use this one with the static variable
 * and makes the code more intuitive.
 */
static SBA_INLINE int
get_iovp_order (unsigned long size)
{
	long double d = size - 1;
	long order;

	__asm__ ("getf.exp %0=%1" : "=r"(order) : "f"(d));
	order = order - iovp_shift - 0xffff + 1;
	if (order < 0)
		order = 0;
	return order;
}

/**
 * sba_search_bitmap - find free space in IO PDIR resource bitmap
 * @ioc: IO MMU structure which owns the pdir we are interested in.
 * @bits_wanted: number of entries we need.
 *
 * Find consecutive free bits in resource bitmap.
 * Each bit represents one entry in the IO Pdir.
 * Cool perf optimization: search for log2(size) bits at a time.
 */
static SBA_INLINE unsigned long
sba_search_bitmap(struct ioc *ioc, unsigned long bits_wanted)
{
	unsigned long *res_ptr = ioc->res_hint;
	unsigned long *res_end = (unsigned long *) &(ioc->res_map[ioc->res_size]);
	unsigned long pide = ~0UL;

	ASSERT(((unsigned long) ioc->res_hint & (sizeof(unsigned long) - 1UL)) == 0);
	ASSERT(res_ptr < res_end);

	/*
	 * N.B.  REO/Grande defect AR2305 can cause TLB fetch timeouts
	 * if a TLB entry is purged while in use.  sba_mark_invalid()
	 * purges IOTLB entries in power-of-two sizes, so we also
	 * allocate IOVA space in power-of-two sizes.
	 */
	bits_wanted = 1UL << get_iovp_order(bits_wanted << PAGE_SHIFT);
	if (bits_wanted > (BITS_PER_LONG/2)) {
		/* Search word at a time - no mask needed */
		for(; res_ptr < res_end; ++res_ptr) {
			if (*res_ptr == 0) {
				*res_ptr = RESMAP_MASK(bits_wanted);
				pide = ((unsigned long)res_ptr - (unsigned long)ioc->res_map);
				pide <<= 3;	/* convert to bit address */
				break;
			}
		}
		/* point to the next word on next pass */
		res_ptr++;
		ioc->res_bitshift = 0;
	} else {
		/*
		** Search the resource bit map on well-aligned values.
		** "o" is the alignment.
		** We need the alignment to invalidate I/O TLB using
		** SBA HW features in the unmap path.
		*/
		unsigned long o = 1 << get_iovp_order(bits_wanted << iovp_shift);
		uint bitshiftcnt = ROUNDUP(ioc->res_bitshift, o);
		unsigned long mask;

		if (bitshiftcnt >= BITS_PER_LONG) {
			bitshiftcnt = 0;
			res_ptr++;
		}
		mask = RESMAP_MASK(bits_wanted) << bitshiftcnt;

		DBG_RES("%s() o %ld %p", __FUNCTION__, o, res_ptr);
		while(res_ptr < res_end)
		{ 
			DBG_RES("    %p %lx %lx\n", res_ptr, mask, *res_ptr);
			ASSERT(0 != mask);
			if(0 == ((*res_ptr) & mask)) {
				*res_ptr |= mask;     /* mark resources busy! */
				pide = ((unsigned long)res_ptr - (unsigned long)ioc->res_map);
				pide <<= 3;	/* convert to bit address */
				pide += bitshiftcnt;
				break;
			}
			mask <<= o;
			bitshiftcnt += o;
			if (0 == mask) {
				mask = RESMAP_MASK(bits_wanted);
				bitshiftcnt=0;
				res_ptr++;
			}
		}
		/* look in the same word on the next pass */
		ioc->res_bitshift = bitshiftcnt + bits_wanted;
	}

	/* wrapped ? */
	if (res_end <= res_ptr) {
		ioc->res_hint = (unsigned long *) ioc->res_map;
		ioc->res_bitshift = 0;
	} else {
		ioc->res_hint = res_ptr;
	}
	return (pide);
}


/**
 * sba_alloc_range - find free bits and mark them in IO PDIR resource bitmap
 * @ioc: IO MMU structure which owns the pdir we are interested in.
 * @size: number of bytes to create a mapping for
 *
 * Given a size, find consecutive unmarked and then mark those bits in the
 * resource bit map.
 */
static int
sba_alloc_range(struct ioc *ioc, size_t size)
{
	unsigned int pages_needed = size >> iovp_shift;
#ifdef PDIR_SEARCH_TIMING
	unsigned long itc_start = ia64_get_itc();
#endif
	unsigned long pide;

	ASSERT(pages_needed);
	ASSERT(pages_needed <= BITS_PER_LONG);
	ASSERT(0 == (size & ~iovp_mask));

	/*
	** "seek and ye shall find"...praying never hurts either...
	*/

	pide = sba_search_bitmap(ioc, pages_needed);
	if (pide >= (ioc->res_size << 3)) {
		pide = sba_search_bitmap(ioc, pages_needed);
		if (pide >= (ioc->res_size << 3))
			panic(__FILE__ ": I/O MMU @ %p is out of mapping resources\n",
			      ioc->ioc_hpa);
	}

#ifdef ASSERT_PDIR_SANITY
	/* verify the first enable bit is clear */
	if(0x00 != ((u8 *) ioc->pdir_base)[pide*PDIR_ENTRY_SIZE + 7]) {
		sba_dump_pdir_entry(ioc, "sba_search_bitmap() botched it?", pide);
	}
#endif

	DBG_RES("%s(%x) %d -> %lx hint %x/%x\n",
		__FUNCTION__, size, pages_needed, pide,
		(uint) ((unsigned long) ioc->res_hint - (unsigned long) ioc->res_map),
		ioc->res_bitshift );

#ifdef PDIR_SEARCH_TIMING
	ioc->avg_search[ioc->avg_idx++] = ia64_get_itc() - itc_start;
	ioc->avg_idx &= SBA_SEARCH_SAMPLE - 1;
#endif

	return (pide);
}


/**
 * sba_free_range - unmark bits in IO PDIR resource bitmap
 * @ioc: IO MMU structure which owns the pdir we are interested in.
 * @iova: IO virtual address which was previously allocated.
 * @size: number of bytes to create a mapping for
 *
 * clear bits in the ioc's resource map
 */
static SBA_INLINE void
sba_free_range(struct ioc *ioc, dma_addr_t iova, size_t size)
{
	unsigned long iovp = SBA_IOVP(ioc, iova);
	unsigned int pide = PDIR_INDEX(iovp);
	unsigned int ridx = pide >> 3;	/* convert bit to byte address */
	unsigned long *res_ptr = (unsigned long *) &((ioc)->res_map[ridx & ~RESMAP_IDX_MASK]);

	int bits_not_wanted = size >> iovp_shift;
	bits_not_wanted = 1UL << get_iovp_order(bits_not_wanted << PAGE_SHIFT);

	/* 3-bits "bit" address plus 2 (or 3) bits for "byte" == bit in word */
	unsigned long m = RESMAP_MASK(bits_not_wanted) << (pide & (BITS_PER_LONG - 1));

	DBG_RES("%s( ,%x,%x) %x/%lx %x %p %lx\n",
		__FUNCTION__, (uint) iova, size,
		bits_not_wanted, m, pide, res_ptr, *res_ptr);

	ASSERT(m != 0);
	ASSERT(bits_not_wanted);
	ASSERT((bits_not_wanted * iovp_size) <= DMA_CHUNK_SIZE);
	ASSERT(bits_not_wanted <= BITS_PER_LONG);
	ASSERT((*res_ptr & m) == m); /* verify same bits are set */
	*res_ptr &= ~m;
}


/**************************************************************
*
*   "Dynamic DMA Mapping" support (aka "Coherent I/O")
*
***************************************************************/

#define SBA_DMA_HINT(ioc, val) ((val) << (ioc)->hint_shift_pdir)


/**
 * sba_io_pdir_entry - fill in one IO PDIR entry
 * @pdir_ptr:  pointer to IO PDIR entry
 * @vba: Virtual CPU address of buffer to map
 *
 * SBA Mapping Routine
 *
 * Given a virtual address (vba, arg1) sba_io_pdir_entry()
 * loads the I/O PDIR entry pointed to by pdir_ptr (arg0).
 * Each IO Pdir entry consists of 8 bytes as shown below
 * (LSB == bit 0):
 *
 *  63                    40                                 11    7        0
 * +-+---------------------+----------------------------------+----+--------+
 * |V|        U            |            PPN[39:12]            | U  |   FF   |
 * +-+---------------------+----------------------------------+----+--------+
 *
 *  V  == Valid Bit
 *  U  == Unused
 * PPN == Physical Page Number
 *
 * The physical address fields are filled with the results of virt_to_phys()
 * on the vba.
 */

#if 1
#define sba_io_pdir_entry(pdir_ptr, vba) *pdir_ptr = ((vba & ~0xE000000000000FFFULL)	\
						      | 0x8000000000000000ULL)
#else
void SBA_INLINE
sba_io_pdir_entry(u64 *pdir_ptr, unsigned long vba)
{
	*pdir_ptr = ((vba & ~0xE000000000000FFFULL) | 0x80000000000000FFULL);
}
#endif

#ifdef ENABLE_MARK_CLEAN
/**
 * Since DMA is i-cache coherent, any (complete) pages that were written via
 * DMA can be marked as "clean" so that update_mmu_cache() doesn't have to
 * flush them when they get mapped into an executable vm-area.
 */
static void
mark_clean (void *addr, size_t size)
{
	unsigned long pg_addr, end;

	pg_addr = PAGE_ALIGN((unsigned long) addr);
	end = (unsigned long) addr + size;
	while (pg_addr + PAGE_SIZE <= end) {
		struct page *page = virt_to_page((void *)pg_addr);
		set_bit(PG_arch_1, &page->flags);
		pg_addr += PAGE_SIZE;
	}
}
#endif

/**
 * sba_mark_invalid - invalidate one or more IO PDIR entries
 * @ioc: IO MMU structure which owns the pdir we are interested in.
 * @iova:  IO Virtual Address mapped earlier
 * @byte_cnt:  number of bytes this mapping covers.
 *
 * Marking the IO PDIR entry(ies) as Invalid and invalidate
 * corresponding IO TLB entry. The PCOM (Purge Command Register)
 * is to purge stale entries in the IO TLB when unmapping entries.
 *
 * The PCOM register supports purging of multiple pages, with a minium
 * of 1 page and a maximum of 2GB. Hardware requires the address be
 * aligned to the size of the range being purged. The size of the range
 * must be a power of 2. The "Cool perf optimization" in the
 * allocation routine helps keep that true.
 */
static SBA_INLINE void
sba_mark_invalid(struct ioc *ioc, dma_addr_t iova, size_t byte_cnt)
{
	u32 iovp = (u32) SBA_IOVP(ioc,iova);

	int off = PDIR_INDEX(iovp);

	/* Must be non-zero and rounded up */
	ASSERT(byte_cnt > 0);
	ASSERT(0 == (byte_cnt & ~iovp_mask));

#ifdef ASSERT_PDIR_SANITY
	/* Assert first pdir entry is set */
	if (!(ioc->pdir_base[off] >> 60)) {
		sba_dump_pdir_entry(ioc,"sba_mark_invalid()", PDIR_INDEX(iovp));
	}
#endif

	if (byte_cnt <= iovp_size)
	{
		ASSERT(off < ioc->pdir_size);

		iovp |= iovp_shift;     /* set "size" field for PCOM */

#ifndef FULL_VALID_PDIR
		/*
		** clear I/O PDIR entry "valid" bit
		** Do NOT clear the rest - save it for debugging.
		** We should only clear bits that have previously
		** been enabled.
		*/
		ioc->pdir_base[off] &= ~(0x80000000000000FFULL);
#else
		/*
  		** If we want to maintain the PDIR as valid, put in
		** the spill page so devices prefetching won't
		** cause a hard fail.
		*/
		ioc->pdir_base[off] = (0x80000000000000FFULL | prefetch_spill_page);
#endif
	} else {
		u32 t = get_iovp_order(byte_cnt) + iovp_shift;

		iovp |= t;
		ASSERT(t <= 31);   /* 2GB! Max value of "size" field */

		do {
			/* verify this pdir entry is enabled */
			ASSERT(ioc->pdir_base[off]  >> 63);
#ifndef FULL_VALID_PDIR
			/* clear I/O Pdir entry "valid" bit first */
			ioc->pdir_base[off] &= ~(0x80000000000000FFULL);
#else
			ioc->pdir_base[off] = (0x80000000000000FFULL | prefetch_spill_page);
#endif
			off++;
			byte_cnt -= iovp_size;
		} while (byte_cnt > 0);
	}

	WRITE_REG(iovp | ioc->ibase, ioc->ioc_hpa+IOC_PCOM);
}

/**
 * sba_map_single - map one buffer and return IOVA for DMA
 * @dev: instance of PCI owned by the driver that's asking.
 * @addr:  driver buffer to map.
 * @size:  number of bytes to map in driver buffer.
 * @dir:  R/W or both.
 *
 * See Documentation/DMA-mapping.txt
 */
dma_addr_t
sba_map_single(struct pci_dev *dev, void *addr, size_t size, int dir)
{
	struct ioc *ioc;
	unsigned long flags;
	dma_addr_t iovp;
	dma_addr_t offset;
	u64 *pdir_start;
	int pide;
#ifdef ALLOW_IOV_BYPASS
	unsigned long pci_addr = virt_to_phys(addr);
#endif

	ioc = GET_IOC(dev);
	ASSERT(ioc);

#ifdef ALLOW_IOV_BYPASS
	/*
 	** Check if the PCI device can DMA to ptr... if so, just return ptr
 	*/
	if ((pci_addr & ~dev->dma_mask) == 0) {
		/*
 		** Device is bit capable of DMA'ing to the buffer...
		** just return the PCI address of ptr
 		*/
		DBG_BYPASS("sba_map_single() bypass mask/addr: 0x%lx/0x%lx\n",
		           dev->dma_mask, pci_addr);
		return pci_addr;
	}
#endif

	ASSERT(size > 0);
	ASSERT(size <= DMA_CHUNK_SIZE);

	/* save offset bits */
	offset = ((dma_addr_t) (long) addr) & ~iovp_mask;

	/* round up to nearest iovp_size */
	size = (size + offset + ~iovp_mask) & iovp_mask;

	spin_lock_irqsave(&ioc->res_lock, flags);
#ifdef ASSERT_PDIR_SANITY
	if (sba_check_pdir(ioc,"Check before sba_map_single()"))
		panic("Sanity check failed");
#endif

	pide = sba_alloc_range(ioc, size);
	iovp = (dma_addr_t) pide << iovp_shift;

	DBG_RUN("%s() 0x%p -> 0x%lx\n",
		__FUNCTION__, addr, (long) iovp | offset);

	pdir_start = &(ioc->pdir_base[pide]);

	while (size > 0) {
		ASSERT(((u8 *)pdir_start)[7] == 0); /* verify availability */
		sba_io_pdir_entry(pdir_start, (unsigned long) addr);

		DBG_RUN("     pdir 0x%p %lx\n", pdir_start, *pdir_start);

		addr += iovp_size;
		size -= iovp_size;
		pdir_start++;
	}
	/* force pdir update */
	wmb();

	/* form complete address */
#ifdef ASSERT_PDIR_SANITY
	sba_check_pdir(ioc,"Check after sba_map_single()");
#endif
	spin_unlock_irqrestore(&ioc->res_lock, flags);
	return SBA_IOVA(ioc, iovp, offset, DEFAULT_DMA_HINT_REG);
}

/**
 * sba_unmap_single - unmap one IOVA and free resources
 * @dev: instance of PCI owned by the driver that's asking.
 * @iova:  IOVA of driver buffer previously mapped.
 * @size:  number of bytes mapped in driver buffer.
 * @dir:  R/W or both.
 *
 * See Documentation/DMA-mapping.txt
 */
void sba_unmap_single(struct pci_dev *dev, dma_addr_t iova, size_t size, int dir)
{
	struct ioc *ioc;
#if DELAYED_RESOURCE_CNT > 0
	struct sba_dma_pair *d;
#endif
	unsigned long flags;
	dma_addr_t offset;

	ioc = GET_IOC(dev);
	ASSERT(ioc);

#ifdef ALLOW_IOV_BYPASS
	if ((iova & ioc->imask) != ioc->ibase) {
		/*
		** Address does not fall w/in IOVA, must be bypassing
		*/
		DBG_BYPASS("sba_unmap_single() bypass addr: 0x%lx\n", iova);

#ifdef ENABLE_MARK_CLEAN
		if (dir == PCI_DMA_FROMDEVICE) {
			mark_clean(phys_to_virt(iova), size);
		}
#endif
		return;
	}
#endif
	offset = iova & ~iovp_mask;

	DBG_RUN("%s() iovp 0x%lx/%x\n",
		__FUNCTION__, (long) iova, size);

	iova ^= offset;        /* clear offset bits */
	size += offset;
	size = ROUNDUP(size, iovp_size);

	spin_lock_irqsave(&ioc->res_lock, flags);

#if DELAYED_RESOURCE_CNT > 0
	d = &(ioc->saved[ioc->saved_cnt]);
	d->iova = iova;
	d->size = size;
	if (++(ioc->saved_cnt) >= DELAYED_RESOURCE_CNT) {
		int cnt = ioc->saved_cnt;
		while (cnt--) {
			sba_mark_invalid(ioc, d->iova, d->size);
			sba_free_range(ioc, d->iova, d->size);
			d--;
		}
		ioc->saved_cnt = 0;
		READ_REG(ioc->ioc_hpa+IOC_PCOM);	/* flush purges */
	}
#else /* DELAYED_RESOURCE_CNT == 0 */
	sba_mark_invalid(ioc, iova, size);
	sba_free_range(ioc, iova, size);
	READ_REG(ioc->ioc_hpa+IOC_PCOM);	/* flush purges */
#endif /* DELAYED_RESOURCE_CNT == 0 */
#ifdef ENABLE_MARK_CLEAN
	if (dir == PCI_DMA_FROMDEVICE) {
		u32 iovp = (u32) SBA_IOVP(ioc,iova);
		int off = PDIR_INDEX(iovp);
		void *addr;

		if (size <= iovp_size) {
			addr = phys_to_virt(ioc->pdir_base[off] &
					    ~0xE000000000000FFFULL);
			mark_clean(addr, size);
		} else {
			size_t byte_cnt = size;

			do {
				addr = phys_to_virt(ioc->pdir_base[off] &
				                    ~0xE000000000000FFFULL);
				mark_clean(addr, min(byte_cnt, iovp_size));
				off++;
				byte_cnt -= iovp_size;

			   } while (byte_cnt > 0);
		}
	}
#endif
	spin_unlock_irqrestore(&ioc->res_lock, flags);

	/* XXX REVISIT for 2.5 Linux - need syncdma for zero-copy support.
	** For Astro based systems this isn't a big deal WRT performance.
	** As long as 2.4 kernels copyin/copyout data from/to userspace,
	** we don't need the syncdma. The issue here is I/O MMU cachelines
	** are *not* coherent in all cases.  May be hwrev dependent.
	** Need to investigate more.
	asm volatile("syncdma");
	*/
}


/**
 * sba_alloc_coherent - allocate/map shared mem for DMA
 * @dev: instance of PCI owned by the driver that's asking.
 * @size:  number of bytes mapped in driver buffer.
 * @dma_handle:  IOVA of new buffer.
 *
 * See Documentation/DMA-mapping.txt
 */
void *
sba_alloc_coherent (struct pci_dev *dev, size_t size, dma_addr_t *dma_handle)
{
	struct ioc *ioc;
	void *addr;

	if (!dev)
		return NULL;	/* only support PCI */

	addr = (void *) __get_free_pages(GFP_ATOMIC, get_order(size));
	if (!addr)
		return NULL;

	/*
	 * REVISIT: if sba_map_single starts needing more than dma_mask from the
	 * device, this needs to be updated.
	 */
	ioc = GET_IOC(dev);
	ASSERT(ioc);
	*dma_handle = sba_map_single(ioc->sac_only_dev, addr, size, 0);

	memset(addr, 0, size);
	return addr;
}


/**
 * sba_free_coherent - free/unmap shared mem for DMA
 * @dev: instance of PCI owned by the driver that's asking.
 * @size:  number of bytes mapped in driver buffer.
 * @vaddr:  virtual address IOVA of "consistent" buffer.
 * @dma_handler:  IO virtual address of "consistent" buffer.
 *
 * See Documentation/DMA-mapping.txt
 */
void sba_free_coherent (struct pci_dev *dev, size_t size, void *vaddr, dma_addr_t dma_handle)
{
	sba_unmap_single(dev, dma_handle, size, 0);
	free_pages((unsigned long) vaddr, get_order(size));
}


/*
** Since 0 is a valid pdir_base index value, can't use that
** to determine if a value is valid or not. Use a flag to indicate
** the SG list entry contains a valid pdir index.
*/
#define PIDE_FLAG 0x1UL

#ifdef DEBUG_LARGE_SG_ENTRIES
int dump_run_sg = 0;
#endif


/**
 * sba_fill_pdir - write allocated SG entries into IO PDIR
 * @ioc: IO MMU structure which owns the pdir we are interested in.
 * @startsg:  list of IOVA/size pairs
 * @nents: number of entries in startsg list
 *
 * Take preprocessed SG list and write corresponding entries
 * in the IO PDIR.
 */

static SBA_INLINE int
sba_fill_pdir(
	struct ioc *ioc,
	struct scatterlist *startsg,
	int nents)
{
	struct scatterlist *dma_sg = startsg;	/* pointer to current DMA */
	int n_mappings = 0;
	u64 *pdirp = 0;
	unsigned long dma_offset = 0;

	dma_sg--;
	while (nents-- > 0) {
		int     cnt = startsg->dma_length;
		startsg->dma_length = 0;

#ifdef DEBUG_LARGE_SG_ENTRIES
		if (dump_run_sg)
			printk(" %2d : %08lx/%05x %p\n",
				nents, startsg->dma_address, cnt,
				sba_sg_address(startsg));
#else
		DBG_RUN_SG(" %d : %08lx/%05x %p\n",
				nents, startsg->dma_address, cnt,
				sba_sg_address(startsg));
#endif
		/*
		** Look for the start of a new DMA stream
		*/
		if (startsg->dma_address & PIDE_FLAG) {
			u32 pide = startsg->dma_address & ~PIDE_FLAG;
			dma_offset = (unsigned long) pide & ~iovp_mask;
			startsg->dma_address = 0;
			dma_sg++;
			dma_sg->dma_address = pide | ioc->ibase;
			pdirp = &(ioc->pdir_base[pide >> iovp_shift]);
			n_mappings++;
		}

		/*
		** Look for a VCONTIG chunk
		*/
		if (cnt) {
			unsigned long vaddr = (unsigned long) sba_sg_address(startsg);
			ASSERT(pdirp);

			/* Since multiple Vcontig blocks could make up
			** one DMA stream, *add* cnt to dma_len.
			*/
			dma_sg->dma_length += cnt;
			cnt += dma_offset;
			dma_offset=0;	/* only want offset on first chunk */
			cnt = ROUNDUP(cnt, iovp_size);
			do {
				sba_io_pdir_entry(pdirp, vaddr);
				vaddr += iovp_size;
				cnt -= iovp_size;
				pdirp++;
			} while (cnt > 0);
		}
		startsg++;
	}
	/* force pdir update */
	wmb();

#ifdef DEBUG_LARGE_SG_ENTRIES
	dump_run_sg = 0;
#endif
	return(n_mappings);
}


/*
** Two address ranges are DMA contiguous *iff* "end of prev" and
** "start of next" are both on an IOV page boundary.
**
** (shift left is a quick trick to mask off upper bits)
*/
#define DMA_CONTIG(__X, __Y) \
	(((((unsigned long) __X) | ((unsigned long) __Y)) << (BITS_PER_LONG - iovp_shift)) == 0UL)


/**
 * sba_coalesce_chunks - preprocess the SG list
 * @ioc: IO MMU structure which owns the pdir we are interested in.
 * @startsg:  list of IOVA/size pairs
 * @nents: number of entries in startsg list
 *
 * First pass is to walk the SG list and determine where the breaks are
 * in the DMA stream. Allocates PDIR entries but does not fill them.
 * Returns the number of DMA chunks.
 *
 * Doing the fill separate from the coalescing/allocation keeps the
 * code simpler. Future enhancement could make one pass through
 * the sglist do both.
 */
static SBA_INLINE int
sba_coalesce_chunks( struct ioc *ioc,
	struct scatterlist *startsg,
	int nents)
{
	struct scatterlist *vcontig_sg;    /* VCONTIG chunk head */
	unsigned long vcontig_len;         /* len of VCONTIG chunk */
	unsigned long vcontig_end;
	struct scatterlist *dma_sg;        /* next DMA stream head */
	unsigned long dma_offset, dma_len; /* start/len of DMA stream */
	int n_mappings = 0;

	while (nents > 0) {
		unsigned long vaddr = (unsigned long) sba_sg_address(startsg);

		/*
		** Prepare for first/next DMA stream
		*/
		dma_sg = vcontig_sg = startsg;
		dma_len = vcontig_len = vcontig_end = startsg->length;
		vcontig_end +=  vaddr;
		dma_offset = vaddr & ~iovp_mask;

		/* PARANOID: clear entries */
		startsg->dma_address = startsg->dma_length = 0;

		/*
		** This loop terminates one iteration "early" since
		** it's always looking one "ahead".
		*/
		while (--nents > 0) {
			unsigned long vaddr;	/* tmp */

			startsg++;

			/* PARANOID */
			startsg->dma_address = startsg->dma_length = 0;

			/* catch brokenness in SCSI layer */
			ASSERT(startsg->length <= DMA_CHUNK_SIZE);

			/*
			** First make sure current dma stream won't
			** exceed DMA_CHUNK_SIZE if we coalesce the
			** next entry.
			*/
			if (((dma_len + dma_offset + startsg->length + ~iovp_mask) & iovp_mask)
			    > DMA_CHUNK_SIZE)
				break;

			/*
			** Then look for virtually contiguous blocks.
			**
			** append the next transaction?
			*/
			vaddr = (unsigned long) sba_sg_address(startsg);
			if  (vcontig_end == vaddr)
			{
				vcontig_len += startsg->length;
				vcontig_end += startsg->length;
				dma_len     += startsg->length;
				continue;
			}

#ifdef DEBUG_LARGE_SG_ENTRIES
			dump_run_sg = (vcontig_len > iovp_size);
#endif

			/*
			** Not virtually contigous.
			** Terminate prev chunk.
			** Start a new chunk.
			**
			** Once we start a new VCONTIG chunk, dma_offset
			** can't change. And we need the offset from the first
			** chunk - not the last one. Ergo Successive chunks
			** must start on page boundaries and dove tail
			** with it's predecessor.
			*/
			vcontig_sg->dma_length = vcontig_len;

			vcontig_sg = startsg;
			vcontig_len = startsg->length;

			/*
			** 3) do the entries end/start on page boundaries?
			**    Don't update vcontig_end until we've checked.
			*/
			if (DMA_CONTIG(vcontig_end, vaddr))
			{
				vcontig_end = vcontig_len + vaddr;
				dma_len += vcontig_len;
				continue;
			} else {
				break;
			}
		}

		/*
		** End of DMA Stream
		** Terminate last VCONTIG block.
		** Allocate space for DMA stream.
		*/
		vcontig_sg->dma_length = vcontig_len;
		dma_len = (dma_len + dma_offset + ~iovp_mask) & iovp_mask;
		ASSERT(dma_len <= DMA_CHUNK_SIZE);
		dma_sg->dma_address = (dma_addr_t) (PIDE_FLAG
			| (sba_alloc_range(ioc, dma_len) << iovp_shift)
			| dma_offset);
		n_mappings++;
	}

	return n_mappings;
}


/**
 * sba_map_sg - map Scatter/Gather list
 * @dev: instance of PCI owned by the driver that's asking.
 * @sglist:  array of buffer/length pairs
 * @nents:  number of entries in list
 * @dir:  R/W or both.
 *
 * See Documentation/DMA-mapping.txt
 */
int sba_map_sg(struct pci_dev *dev, struct scatterlist *sglist, int nents, int dir)
{
	struct ioc *ioc;
	int coalesced, filled = 0;
	unsigned long flags;
#ifdef ALLOW_IOV_BYPASS
	struct scatterlist *sg;
#endif

	DBG_RUN_SG("%s() START %d entries\n", __FUNCTION__, nents);
	ioc = GET_IOC(dev);
	ASSERT(ioc);

#ifdef ALLOW_IOV_BYPASS
	if (dev->dma_mask >= ioc->dma_mask) {
		for (sg = sglist ; filled < nents ; filled++, sg++){
			sg->dma_length = sg->length;
			sg->dma_address = virt_to_phys(sba_sg_address(sg));
		}
		return filled;
	}
#endif
	/* Fast path single entry scatterlists. */
	if (nents == 1) {
		sglist->dma_length = sglist->length;
		sglist->dma_address = sba_map_single(dev, sba_sg_address(sglist), sglist->length,
		                                     dir);
		return 1;
	}

	spin_lock_irqsave(&ioc->res_lock, flags);

#ifdef ASSERT_PDIR_SANITY
	if (sba_check_pdir(ioc,"Check before sba_map_sg()"))
	{
		sba_dump_sg(ioc, sglist, nents);
		panic("Check before sba_map_sg()");
	}
#endif

	/*
	** First coalesce the chunks and allocate I/O pdir space
	**
	** If this is one DMA stream, we can properly map using the
	** correct virtual address associated with each DMA page.
	** w/o this association, we wouldn't have coherent DMA!
	** Access to the virtual address is what forces a two pass algorithm.
	*/
	coalesced = sba_coalesce_chunks(ioc, sglist, nents);

	/*
	** Program the I/O Pdir
	**
	** map the virtual addresses to the I/O Pdir
	** o dma_address will contain the pdir index
	** o dma_len will contain the number of bytes to map
	** o address contains the virtual address.
	*/
	filled = sba_fill_pdir(ioc, sglist, nents);

#ifdef ASSERT_PDIR_SANITY
	if (sba_check_pdir(ioc,"Check after sba_map_sg()"))
	{
		sba_dump_sg(ioc, sglist, nents);
		panic("Check after sba_map_sg()\n");
	}
#endif

	spin_unlock_irqrestore(&ioc->res_lock, flags);

	ASSERT(coalesced == filled);
	DBG_RUN_SG("%s() DONE %d mappings\n", __FUNCTION__, filled);

	return filled;
}


/**
 * sba_unmap_sg - unmap Scatter/Gather list
 * @dev: instance of PCI owned by the driver that's asking.
 * @sglist:  array of buffer/length pairs
 * @nents:  number of entries in list
 * @dir:  R/W or both.
 *
 * See Documentation/DMA-mapping.txt
 */
void sba_unmap_sg (struct pci_dev *dev, struct scatterlist *sglist, int nents, int dir)
{
	struct ioc *ioc;
#ifdef ASSERT_PDIR_SANITY
	unsigned long flags;
#endif

	DBG_RUN_SG("%s() START %d entries,  %p,%x\n",
		__FUNCTION__, nents, sba_sg_address(sglist), sglist->length);

	ioc = GET_IOC(dev);
	ASSERT(ioc);

#ifdef ASSERT_PDIR_SANITY
	spin_lock_irqsave(&ioc->res_lock, flags);
	sba_check_pdir(ioc,"Check before sba_unmap_sg()");
	spin_unlock_irqrestore(&ioc->res_lock, flags);
#endif

	while (nents && sglist->dma_length) {

		sba_unmap_single(dev, sglist->dma_address, sglist->dma_length, dir);
		sglist++;
		nents--;
	}

	DBG_RUN_SG("%s() DONE (nents %d)\n", __FUNCTION__,  nents);

#ifdef ASSERT_PDIR_SANITY
	spin_lock_irqsave(&ioc->res_lock, flags);
	sba_check_pdir(ioc,"Check after sba_unmap_sg()");
	spin_unlock_irqrestore(&ioc->res_lock, flags);
#endif

}

/**************************************************************
*
*   Initialization and claim
*
***************************************************************/

static void __init
ioc_iova_init(struct ioc *ioc)
{
	int tcnfg;
	int agp_found = 0;
	struct pci_dev *device = NULL;
#ifdef FULL_VALID_PDIR
	unsigned long index;
#endif

	/*
	** Firmware programs the base and size of a "safe IOVA space"
	** (one that doesn't overlap memory or LMMIO space) in the
	** IBASE and IMASK registers.
	*/
	ioc->ibase = READ_REG(ioc->ioc_hpa + IOC_IBASE) & ~0x1UL;
	ioc->imask = READ_REG(ioc->ioc_hpa + IOC_IMASK) | 0xFFFFFFFF00000000UL;

	ioc->iov_size = ~ioc->imask + 1;

	DBG_INIT("%s() hpa %p IOV base 0x%lx mask 0x%lx (%dMB)\n",
		__FUNCTION__, ioc->ioc_hpa, ioc->ibase, ioc->imask,
		ioc->iov_size >> 20);

	switch (iovp_size) {
		case  4*1024: tcnfg = 0; break;
		case  8*1024: tcnfg = 1; break;
		case 16*1024: tcnfg = 2; break;
		case 64*1024: tcnfg = 3; break;
		default:
			panic(PFX "Unsupported IOTLB page size %ldK",
				iovp_size >> 10);
			break;
	}
	WRITE_REG(tcnfg, ioc->ioc_hpa + IOC_TCNFG);

	ioc->pdir_size = (ioc->iov_size / iovp_size) * PDIR_ENTRY_SIZE;
	ioc->pdir_base = (void *) __get_free_pages(GFP_KERNEL,
						   get_order(ioc->pdir_size));
	if (!ioc->pdir_base)
		panic(PFX "Couldn't allocate I/O Page Table\n");

	memset(ioc->pdir_base, 0, ioc->pdir_size);

	DBG_INIT("%s() IOV page size %ldK pdir %p size %x\n", __FUNCTION__,
		iovp_size >> 10, ioc->pdir_base, ioc->pdir_size);

	ASSERT(ALIGN((unsigned long) ioc->pdir_base, 4*1024) == (unsigned long) ioc->pdir_base);
	WRITE_REG(virt_to_phys(ioc->pdir_base), ioc->ioc_hpa + IOC_PDIR_BASE);

	/*
	** If an AGP device is present, only use half of the IOV space
	** for PCI DMA.  Unfortunately we can't know ahead of time
	** whether GART support will actually be used, for now we
	** can just key on an AGP device found in the system.
	** We program the next pdir index after we stop w/ a key for
	** the GART code to handshake on.
	*/
	while ((device = pci_find_device(PCI_ANY_ID, PCI_ANY_ID, device)) != NULL)
		agp_found |= pci_find_capability(device, PCI_CAP_ID_AGP);

	if (agp_found && reserve_sba_gart) {
		printk(KERN_INFO PFX "reserving %dMb of IOVA space at 0x%lx for agpgart\n",
			ioc->iov_size/2 >> 20, ioc->ibase + ioc->iov_size/2);
		ioc->pdir_size /= 2;
		((u64 *)ioc->pdir_base)[PDIR_INDEX(ioc->iov_size/2)] = ZX1_SBA_IOMMU_COOKIE;
	}
#ifdef FULL_VALID_PDIR
	/*
  	** Check to see if the spill page has been allocated, we don't need more than
	** one across multiple SBAs.
	*/
	if (!prefetch_spill_page) {
		char *spill_poison = "SBAIOMMU POISON";
		int poison_size = 16;
		void *poison_addr, *addr;

		addr = (void *)__get_free_pages(GFP_KERNEL, get_order(iovp_size));
		if (!addr)
			panic(PFX "Couldn't allocate PDIR spill page\n");

		poison_addr = addr;
		for ( ; (u64) poison_addr < addr + iovp_size; poison_addr += poison_size)
			memcpy(poison_addr, spill_poison, poison_size);

		prefetch_spill_page = virt_to_phys(addr);

		DBG_INIT("%s() prefetch spill addr: 0x%lx\n", __FUNCTION__, prefetch_spill_page);
	}
	/*
  	** Set all the PDIR entries valid w/ the spill page as the target
	*/
	for (index = 0 ; index < (ioc->pdir_size / PDIR_ENTRY_SIZE) ; index++)
		((u64 *)ioc->pdir_base)[index] = (0x80000000000000FF | prefetch_spill_page);
#endif

	/* Clear I/O TLB of any possible entries */
	WRITE_REG(ioc->ibase | (get_iovp_order(ioc->iov_size) + iovp_shift), ioc->ioc_hpa + IOC_PCOM);
	READ_REG(ioc->ioc_hpa + IOC_PCOM);

	/* Enable IOVA translation */
	WRITE_REG(ioc->ibase | 1, ioc->ioc_hpa + IOC_IBASE);
	READ_REG(ioc->ioc_hpa + IOC_IBASE);
}

static void __init
ioc_resource_init(struct ioc *ioc)
{
	spin_lock_init(&ioc->res_lock);

	/* resource map size dictated by pdir_size */
	ioc->res_size = ioc->pdir_size / PDIR_ENTRY_SIZE; /* entries */
	ioc->res_size >>= 3;  /* convert bit count to byte count */
	DBG_INIT("%s() res_size 0x%x\n", __FUNCTION__, ioc->res_size);

	ioc->res_map = (char *) __get_free_pages(GFP_KERNEL,
						 get_order(ioc->res_size));
	if (!ioc->res_map)
		panic(PFX "Couldn't allocate resource map\n");

	memset(ioc->res_map, 0, ioc->res_size);
	/* next available IOVP - circular search */
	ioc->res_hint = (unsigned long *) ioc->res_map;

#ifdef ASSERT_PDIR_SANITY
	/* Mark first bit busy - ie no IOVA 0 */
	ioc->res_map[0] = 0x1;
	ioc->pdir_base[0] = 0x8000000000000000ULL | ZX1_SBA_IOMMU_COOKIE;
#endif
#ifdef FULL_VALID_PDIR
	/* Mark the last resource used so we don't prefetch beyond IOVA space */
	ioc->res_map[ioc->res_size - 1] |= 0x80UL; /* res_map is chars */
	ioc->pdir_base[(ioc->pdir_size / PDIR_ENTRY_SIZE) - 1] = (0x80000000000000FF
							      | prefetch_spill_page);
#endif

	DBG_INIT("%s() res_map %x %p\n", __FUNCTION__,
		 ioc->res_size, (void *) ioc->res_map);
}

static void __init
ioc_sac_init(struct ioc *ioc)
{
	struct pci_dev *sac = NULL;
	struct pci_controller *controller = NULL;

	/*
	 * pci_alloc_coherent() must return a DMA address which is
	 * SAC (single address cycle) addressable, so allocate a
	 * pseudo-device to enforce that.
	 */
	sac = kmalloc(sizeof(*sac), GFP_KERNEL);
	if (!sac)
		panic(PFX "Couldn't allocate struct pci_dev");
	memset(sac, 0, sizeof(*sac));

	controller = kmalloc(sizeof(*controller), GFP_KERNEL);
	if (!controller)
		panic(PFX "Couldn't allocate struct pci_controller");
	memset(controller, 0, sizeof(*controller));

	controller->iommu = ioc;
	sac->sysdata = controller;
	sac->dma_mask = 0xFFFFFFFFUL;
	ioc->sac_only_dev = sac;
}

static void __init
ioc_zx1_init(struct ioc *ioc)
{
	if (ioc->rev < 0x20)
		panic(PFX "IOC 2.0 or later required for IOMMU support\n");

	ioc->dma_mask = 0xFFFFFFFFFFUL;

	if (!iovp_shift) {
		/* 64k is max iommu page size */
		iovp_shift = min(PAGE_SHIFT, 16);
		iovp_size = (1 << iovp_shift);
		iovp_mask = ~(iovp_size - 1);
	}
}

static void __init
ioc_sx1000_init(struct ioc *ioc)
{
	if (!iovp_shift) {
		iovp_shift = 12;	/* 4K for now */
		iovp_size = (1 << iovp_shift);
		iovp_mask = ~(iovp_size - 1);
	}
}

typedef void (initfunc)(struct ioc *);

struct ioc_iommu {
	u32 func_id;
	char *name;
	initfunc *init;
};

static struct ioc_iommu ioc_iommu_info[] __initdata = {
	{ ZX1_IOC_ID, "zx1", ioc_zx1_init },
	{ REO_IOC_ID, "REO", ioc_sx1000_init },
	{ SX1000_IOC_ID, "sx1000", ioc_sx1000_init },
};

static struct ioc * __init
ioc_init(u64 hpa, void *handle)
{
	struct ioc *ioc;
	struct ioc_iommu *info;

	ioc = kmalloc(sizeof(*ioc), GFP_KERNEL);
	if (!ioc)
		return NULL;

	memset(ioc, 0, sizeof(*ioc));

	ioc->next = ioc_list;
	ioc_list = ioc;

	ioc->handle = handle;
	ioc->ioc_hpa = ioremap(hpa, 0x1000);

	ioc->func_id = READ_REG(ioc->ioc_hpa + IOC_FUNC_ID);
	ioc->rev = READ_REG(ioc->ioc_hpa + IOC_FCLASS) & 0xFFUL;
	ioc->dma_mask = 0xFFFFFFFFFFFFFFFFUL;	/* conservative */

	if (iovp_shift) {
		iovp_size = (1 << iovp_shift);
		iovp_mask = ~(iovp_size - 1);
	}

	for (info = ioc_iommu_info; info < ioc_iommu_info + ARRAY_SIZE(ioc_iommu_info); info++) {
		if (ioc->func_id == info->func_id) {
			ioc->name = info->name;
			if (info->init)
				(info->init)(ioc);
		}
	}
	DBG_INIT("%s: PAGE_SIZE %ldK, iovp_size %ldK\n", __FUNCTION__,
		PAGE_SIZE >> 10, iovp_size >> 10);

	if (!ioc->name) {
		ioc->name = kmalloc(24, GFP_KERNEL);
		if (ioc->name)
			sprintf((char *) ioc->name, "Unknown (%04x:%04x)",
				ioc->func_id & 0xFFFF, (ioc->func_id >> 16) & 0xFFFF);
		else
			ioc->name = "Unknown";
	}

	ioc_iova_init(ioc);
	ioc_resource_init(ioc);
	ioc_sac_init(ioc);

	printk(KERN_INFO PFX
		"%s %d.%d HPA 0x%lx IOVA space %dMb at 0x%lx\n",
		ioc->name, (ioc->rev >> 4) & 0xF, ioc->rev & 0xF,
		hpa, ioc->iov_size >> 20, ioc->ibase);

	return ioc;
}



/**************************************************************************
**
**   SBA initialization code (HW and SW)
**
**   o identify SBA chip itself
**   o FIXME: initialize DMA hints for reasonable defaults
**
**************************************************************************/

#ifdef CONFIG_PROC_FS
static void *
ioc_start(struct seq_file *s, loff_t *pos)
{
	struct ioc *ioc;
	loff_t n = *pos;

	for (ioc = ioc_list; ioc; ioc = ioc->next)
		if (!n--)
			return ioc;

	return NULL;
}

static void *
ioc_next(struct seq_file *s, void *v, loff_t *pos)
{
	struct ioc *ioc = v;

	++*pos;
	return ioc->next;
}

static void
ioc_stop(struct seq_file *s, void *v)
{
}

static int
ioc_show(struct seq_file *s, void *v)
{
	struct ioc *ioc = v;
	unsigned long *res_ptr = (unsigned long *)ioc->res_map;
	int i, used = 0;

	seq_printf(s, "Hewlett Packard %s IOC rev %d.%d\n",
		ioc->name, ((ioc->rev >> 4) & 0xF), (ioc->rev & 0xF));
	seq_printf(s, "IOVA size       : %d MB\n", ioc->iov_size/(1024*1024));
	seq_printf(s, "IOVA page size  : %ld kb\n", iovp_size/1024);

	for (i = 0; i < (ioc->res_size / sizeof(unsigned long)); ++i, ++res_ptr)
		used += hweight64(*res_ptr);

	seq_printf(s, "PDIR size       : %d entries\n", ioc->res_size << 3);
	seq_printf(s, "PDIR used       : %d entries\n", used);

#ifdef PDIR_SEARCH_TIMING
	{
		unsigned long i = 0, avg = 0, min, max;
		min = max = ioc->avg_search[0];
		for (i = 0; i < SBA_SEARCH_SAMPLE; i++) {
			avg += ioc->avg_search[i];
			if (ioc->avg_search[i] > max) max = ioc->avg_search[i];
			if (ioc->avg_search[i] < min) min = ioc->avg_search[i];
		}
		avg /= SBA_SEARCH_SAMPLE;
		seq_printf(s, "Bitmap search   : %ld/%ld/%ld (min/avg/max CPU Cycles)\n",
		           min, avg, max);
	}
#endif
#ifndef ALLOW_IOV_BYPASS
	 seq_printf(s, "IOVA bypass disabled\n");
#endif
	return 0;
}

static const struct seq_operations ioc_seq_ops = {
	.start = ioc_start,
	.next  = ioc_next,
	.stop  = ioc_stop,
	.show  = ioc_show
};

static int
ioc_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &ioc_seq_ops);
}

static const struct file_operations ioc_fops = {
	.open    = ioc_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = seq_release
};

static void __init
ioc_proc_init(void)
{
	struct proc_dir_entry *dir, *entry;

	dir = proc_mkdir("bus/mckinley", 0);
	if (!dir)
		return;

	entry = create_proc_entry(ioc_list->name, 0, dir);
	if (entry)
		entry->proc_fops = &ioc_fops;
}
#endif

static void
sba_connect_bus(struct pci_bus *bus)
{
	acpi_handle handle, parent;
	acpi_status status;
	struct ioc *ioc;

	if (!PCI_CONTROLLER(bus))
		panic(PFX "no sysdata on bus %d!\n", bus->number);

	if (PCI_CONTROLLER(bus)->iommu)
		return;

	handle = PCI_CONTROLLER(bus)->acpi_handle;
	if (!handle)
		return;

	/*
	 * The IOC scope encloses PCI root bridges in the ACPI
	 * namespace, so work our way out until we find an IOC we
	 * claimed previously.
	 */
	do {
		for (ioc = ioc_list; ioc; ioc = ioc->next)
			if (ioc->handle == handle) {
				PCI_CONTROLLER(bus)->iommu = ioc;
				return;
			}

		status = acpi_get_parent(handle, &parent);
		handle = parent;
	} while (ACPI_SUCCESS(status));

	printk(KERN_WARNING "No IOC for PCI Bus %04x:%02x in ACPI\n", PCI_SEGMENT(bus), bus->number);
}

extern acpi_status acpi_hp_csr_space (acpi_handle obj, u64 *csr_base, u64 *csr_length);

static int __init
acpi_sba_ioc_add(struct acpi_device *device)
{
	struct ioc *ioc;
	acpi_status status;
	u64 hpa, length;
	struct acpi_buffer buffer;
	struct acpi_device_info *dev_info;

	status = acpi_hp_csr_space(device->handle, &hpa, &length);
	if (ACPI_FAILURE(status))
		return 1;

	buffer.length = ACPI_ALLOCATE_LOCAL_BUFFER;
	status = acpi_get_object_info(device->handle, &buffer);
	if (ACPI_FAILURE(status))
		return 1;
	dev_info = buffer.pointer;

	/*
	 * For HWP0001, only SBA appears in ACPI namespace.  It encloses the PCI
	 * root bridges, and its CSR space includes the IOC function.
	 */
	if (strncmp("HWP0001", dev_info->hardware_id.value, 7) == 0)
		hpa += ZX1_IOC_OFFSET;
	ACPI_MEM_FREE(dev_info);

	ioc = ioc_init(hpa, device->handle);
	if (!ioc)
		return 1;

	return 0;
}

static struct acpi_driver acpi_sba_ioc_driver = {
	.name		= "IOC IOMMU Driver",
	.ids		= "HWP0001,HWP0004",
	.ops		= {
		.add	= acpi_sba_ioc_add,
	},
};

void __init
sba_init(void)
{
	acpi_bus_register_driver(&acpi_sba_ioc_driver);
	if (!ioc_list)
		return;

#ifdef CONFIG_PCI
	{
		struct pci_bus *b = NULL;
		pci_for_each_bus(b)
			sba_connect_bus(b);
	}
#endif

#ifdef CONFIG_PROC_FS
	ioc_proc_init();
#endif
}

static int __init
nosbagart(char *str)
{
	reserve_sba_gart = 0;
	return 1;
}

int
sba_dma_supported (struct pci_dev *dev, u64 mask)
{
	/* make sure it's at least 32bit capable */
	return ((mask & 0xFFFFFFFFUL) == 0xFFFFFFFFUL);
}

__setup("nosbagart", nosbagart);

static int __init
sba_page_override(char *str)
{
	unsigned long page_size;

	page_size = memparse(str, &str);
	switch (page_size) {
		case 4096:
		case 8192:
		case 16384:
		case 65536:
			iovp_shift = ffs(page_size) - 1;
			break;
		default:
			printk("%s: unknown/unsupported iommu page size %ld\n",
			       __FUNCTION__, page_size);
	}

	return 1;
}

__setup("sbapagesize=",sba_page_override);

EXPORT_SYMBOL(sba_init);
EXPORT_SYMBOL(sba_map_single);
EXPORT_SYMBOL(sba_unmap_single);
EXPORT_SYMBOL(sba_map_sg);
EXPORT_SYMBOL(sba_unmap_sg);
EXPORT_SYMBOL(sba_dma_supported);
EXPORT_SYMBOL(sba_alloc_coherent);
EXPORT_SYMBOL(sba_free_coherent);
