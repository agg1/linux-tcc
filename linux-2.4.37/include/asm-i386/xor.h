/*
 * include/asm-i386/xor.h
 *
 * Optimized RAID-5 checksumming functions for MMX and SSE.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * You should have received a copy of the GNU General Public License
 * (for example /usr/src/linux/COPYING); if not, write to the Free
 * Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
 * High-speed RAID5 checksumming functions utilizing MMX instructions.
 * Copyright (C) 1998 Ingo Molnar.
 */

#define FPU_SAVE							\
  do {									\
	if (!(current->flags & PF_USEDFPU))				\
		__asm__ __volatile__ (" clts;\n");			\
	__asm__ __volatile__ ("fsave %0; fwait": "=m"(fpu_save[0]));	\
  } while (0)

#define FPU_RESTORE							\
  do {									\
	__asm__ __volatile__ ("frstor %0": : "m"(fpu_save[0]));		\
	if (!(current->flags & PF_USEDFPU))				\
		stts();							\
  } while (0)

#undef FPU_SAVE
#undef FPU_RESTORE

/* Also try the generic routines.  */
#include <asm-generic/xor.h>

#undef XOR_TRY_TEMPLATES
#define XOR_TRY_TEMPLATES				\
	do {						\
		xor_speed(&xor_block_8regs);		\
		xor_speed(&xor_block_32regs);		\
	} while (0)

/* We force the use of the SSE xor block because it can write around L2.
   We may also be able to load into the L1 only depending on how the cpu
   deals with a load to a line that is being prefetched.  */
#define XOR_SELECT_TEMPLATE(FASTEST) \
	(FASTEST)

