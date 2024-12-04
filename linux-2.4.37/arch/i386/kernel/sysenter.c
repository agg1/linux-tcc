/*
 * linux/arch/i386/kernel/sysenter.c
 *
 * (C) Copyright 2002 Linus Torvalds
 *
 * This file contains the needed initializations to support AT_SYSINFO
 */

#include <linux/init.h>
#include <linux/smp.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include <asm/cpufeature.h>
#include <asm/msr.h>
#include <asm/pgtable.h>

extern int allowsysinfo;

static int __init sysenter_setup(void)
{
	static const char int80[] = {
		0xcd, 0x80,		/* int $0x80 */
		0xc3			/* ret */
	};
	unsigned long page;
	
	if (!allowsysinfo)
		return 0;

	page = get_zeroed_page(GFP_ATOMIC);

	__set_fixmap(FIX_VSYSCALL, __pa(page), PAGE_READONLY);
	memcpy((void *) page, int80, sizeof(int80));
	return 0;
}

__initcall(sysenter_setup);
