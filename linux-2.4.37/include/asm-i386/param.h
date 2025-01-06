#ifndef _ASMi386_PARAM_H
#define _ASMi386_PARAM_H

// https://www.kernel.org/pub/linux/kernel/people/rml/variable-HZ/v2.4/vhz-j64-2.4.23.patch
#ifndef HZ
#define HZ 500
#endif

#define EXEC_PAGESIZE	4096

#ifndef NGROUPS
#define NGROUPS		32
#endif

#ifndef NOGROUP
#define NOGROUP		(-1)
#endif

#define MAXHOSTNAMELEN	64	/* max length of hostname */

#ifdef __KERNEL__
# define CLOCKS_PER_SEC	500	/* frequency at which times() counts */
#endif

#endif
