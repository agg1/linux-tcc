/*
 * fixmap.h: compile-time virtual memory allocation
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 1998 Ingo Molnar
 *
 * Support of BIGMEM added by Gerhard Wichert, Siemens AG, July 1999
 */

#ifndef _ASM_FIXMAP_H
#define _ASM_FIXMAP_H

#include <linux/config.h>
#include <linux/kernel.h>
#include <asm/acpi.h>
#include <asm/apicdef.h>
#include <asm/page.h>
#ifdef CONFIG_HIGHMEM
#include <linux/threads.h>
#include <asm/kmap_types.h>
#endif

/*
 * Here we define all the compile-time 'special' virtual
 * addresses. The point is to have a constant address at
 * compile time, but to set the physical address only
 * in the boot process. We allocate these special  addresses
 * from the end of virtual memory (0xfffff000) backwards.
 * Also this lets us do fail-safe vmalloc(), we
 * can guarantee that these special addresses and
 * vmalloc()-ed addresses never overlap.
 *
 * these 'compile-time allocated' memory buffers are
 * fixed-size 4k pages. (or larger if used with an increment
 * highger than 1) use fixmap_set(idx,phys) to associate
 * physical memory with fixmap indices.
 *
 * TLB entries of such buffers will not be flushed across
 * task switches.
 */

/*
 * on UP currently we will have no trace of the fixmap mechanizm,
 * no page table allocations, etc. This might change in the
 * future, say framebuffers for the console driver(s) could be
 * fix-mapped?
 */
enum fixed_addresses {
	FIX_HOLE,
	FIX_VSYSCALL,
#ifdef CONFIG_X86_LOCAL_APIC
	FIX_APIC_BASE,	/* local (CPU) APIC) -- required for SMP or not */
#endif
#ifdef CONFIG_X86_IO_APIC
	FIX_IO_APIC_BASE_0,
	FIX_IO_APIC_BASE_END = FIX_IO_APIC_BASE_0 + MAX_IO_APICS-1,
#endif
#ifdef CONFIG_X86_VISWS_APIC
	FIX_CO_CPU,	/* Cobalt timer */
	FIX_CO_APIC,	/* Cobalt APIC Redirection Table */
	FIX_LI_PCIA,	/* Lithium PCI Bridge A */
	FIX_LI_PCIB,	/* Lithium PCI Bridge B */
#endif
#ifndef CONFIG_X86_F00F_WORKS_OK
	FIX_F00F,
#endif
#ifdef CONFIG_X86_SUMMIT
	FIX_CYCLONE_TIMER, /*cyclone timer register*/
#endif 
#ifdef CONFIG_HIGHMEM
	FIX_KMAP_BEGIN,	/* reserved pte's for temporary kernel mappings */
	FIX_KMAP_END = FIX_KMAP_BEGIN+(KM_TYPE_NR*NR_CPUS)-1,
#endif
#ifdef CONFIG_ACPI_BOOT
	FIX_ACPI_BEGIN,
	FIX_ACPI_END = FIX_ACPI_BEGIN + FIX_ACPI_PAGES - 1,
#endif
	__end_of_permanent_fixed_addresses,
	/* temporary boot-time mappings, used before ioremap() is functional */
#define NR_FIX_BTMAPS	16
	FIX_BTMAP_END = __end_of_permanent_fixed_addresses,
	FIX_BTMAP_BEGIN = FIX_BTMAP_END + NR_FIX_BTMAPS - 1,
	__end_of_fixed_addresses
};

extern void __set_fixmap (enum fixed_addresses idx,
					unsigned long phys, pgprot_t flags);

#define set_fixmap(idx, phys) \
		__set_fixmap(idx, phys, PAGE_KERNEL)
/*
 * Some hardware wants to get fixmapped without caching.
 */
#define set_fixmap_nocache(idx, phys) \
		__set_fixmap(idx, phys, PAGE_KERNEL_NOCACHE)
/*
 * used by vmalloc.c.
 *
 * Leave one empty page between vmalloc'ed areas and
 * the start of the fixmap.
 */
#define FIXADDR_TOP	(0xfffff000UL)
#define __FIXADDR_SIZE	(__end_of_permanent_fixed_addresses << PAGE_SHIFT)
#define FIXADDR_START	(FIXADDR_TOP - __FIXADDR_SIZE)

#define __fix_to_virt(x)	(FIXADDR_TOP - ((x) << PAGE_SHIFT))


#if __GNUC__ > 4 || __GNUC__ == 4 && __GNUC_MINOR__ >= 2
static void __this_fixmap_does_not_exist(void) {}
#else
extern void __this_fixmap_does_not_exist(void);
#endif

/*
 * 'index to address' translation. If anyone tries to use the idx
 * directly without tranlation, we catch the bug with a NULL-deference
 * kernel oops. Illegal ranges of incoming indices are caught too.
 */
static inline unsigned long fix_to_virt(const unsigned int idx)
{
	/*
	 * this branch gets completely eliminated after inlining,
	 * except when someone tries to use fixaddr indices in an
	 * illegal way. (such as mixing up address types or using
	 * out-of-range indices).
	 *
	 * If it doesn't get removed, the linker will complain
	 * loudly with a reasonably clear error message..
	 */
	if (idx >= __end_of_fixed_addresses)
		__this_fixmap_does_not_exist();

        return __fix_to_virt(idx);
}

#endif
