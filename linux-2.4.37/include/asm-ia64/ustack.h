#ifndef _ASM_IA64_USTACK_H
#define _ASM_IA64_USTACK_H

/*
 * Constants for the user stack size
 */

#include <asm/page.h>

/* The absolute hard limit for stack size is 1/2 of the mappable space in the region */
#define MAX_USER_STACK_SIZE	(RGN_MAP_LIMIT/2)
/* Make a default stack size of 2GB */
#define DEFAULT_USER_STACK_SIZE	(1UL << 31)
#define __STACK_TOP		(0x6000000000000000UL + RGN_MAP_LIMIT)

#endif /* _ASM_IA64_USTACK_H */
