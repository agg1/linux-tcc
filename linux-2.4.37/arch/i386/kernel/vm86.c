/*
 *  linux/kernel/vm86.c
 *
 *  Copyright (C) 1994  Linus Torvalds
 *
 *  29 dec 2001 - Fixed oopses caused by unchecked access to the vm86
 *                stack - Manfred Spraul <manfreds@colorfullife.com>
 *
 *  22 mar 2002 - Manfred detected the stackfaults, but didn't handle
 *                them correctly. Now the emulation will be in a
 *                consistent state after stackfaults - Kasper Dupont
 *                <kasperd@daimi.au.dk>
 *
 *  22 mar 2002 - Added missing clear_IF in set_vflags_* Kasper Dupont
 *                <kasperd@daimi.au.dk>
 *
 *  ?? ??? 2002 - Fixed premature returns from handle_vm86_fault
 *                caused by Kasper Dupont's changes - Stas Sergeev
 *
 *   4 apr 2002 - Fixed CHECK_IF_IN_TRAP broken by Stas' changes.
 *                Kasper Dupont <kasperd@daimi.au.dk>
 *
 *   9 apr 2002 - Changed syntax of macros in handle_vm86_fault.
 *                Kasper Dupont <kasperd@daimi.au.dk>
 *
 *   9 apr 2002 - Changed stack access macros to jump to a label
 *                instead of returning to userspace. This simplifies
 *                do_int, and is needed by handle_vm6_fault. Kasper
 *                Dupont <kasperd@daimi.au.dk>
 *   
 */

#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/signal.h>
#include <linux/string.h>
#include <linux/ptrace.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/smp_lock.h>

#include <asm/uaccess.h>
#include <asm/pgalloc.h>
#include <asm/io.h>
#include <asm/irq.h>

/*
 * Known problems:
 *
 * Interrupt handling is not guaranteed:
 * - a real x86 will disable all interrupts for one instruction
 *   after a "mov ss,xx" to make stack handling atomic even without
 *   the 'lss' instruction. We can't guarantee this in v86 mode,
 *   as the next instruction might result in a page fault or similar.
 * - a real x86 will have interrupts disabled for one instruction
 *   past the 'sti' that enables them. We don't bother with all the
 *   details yet.
 *
 * Let's hope these problems do not actually matter for anything.
 */


#define KVM86	((struct kernel_vm86_struct *)regs)
#define VMPI 	KVM86->vm86plus


/*
 * 8- and 16-bit register defines..
 */
#define AL(regs)	(((unsigned char *)&((regs)->eax))[0])
#define AH(regs)	(((unsigned char *)&((regs)->eax))[1])
#define IP(regs)	(*(unsigned short *)&((regs)->eip))
#define SP(regs)	(*(unsigned short *)&((regs)->esp))

/*
 * virtual flags (16 and 32-bit versions)
 */
#define VFLAGS	(*(unsigned short *)&(current->thread.v86flags))
#define VEFLAGS	(current->thread.v86flags)

#define set_flags(X,new,mask) \
((X) = ((X) & ~(mask)) | ((new) & (mask)))

#define SAFE_MASK	(0xDD5)
#define RETURN_MASK	(0xDFF)

#define VM86_REGS_PART2 orig_eax
#define VM86_REGS_SIZE1 \
        ( (unsigned)( & (((struct kernel_vm86_regs *)0)->VM86_REGS_PART2) ) )
#define VM86_REGS_SIZE2 (sizeof(struct kernel_vm86_regs) - VM86_REGS_SIZE1)

struct pt_regs * FASTCALL(save_v86_state(struct kernel_vm86_regs * regs));
struct pt_regs * fastcall save_v86_state(struct kernel_vm86_regs * regs)
{
	struct tss_struct *tss;
	struct pt_regs *ret;
	unsigned long tmp;

	if (!current->thread.vm86_info) {
		printk("no vm86_info: BAD\n");
		do_exit(SIGSEGV);
	}
	set_flags(regs->eflags, VEFLAGS, VIF_MASK | current->thread.v86mask);
	tmp = copy_to_user(&current->thread.vm86_info->regs,regs, VM86_REGS_SIZE1);
	tmp += copy_to_user(&current->thread.vm86_info->regs.VM86_REGS_PART2,
		&regs->VM86_REGS_PART2, VM86_REGS_SIZE2);
	tmp += put_user(current->thread.screen_bitmap,&current->thread.vm86_info->screen_bitmap);
	if (tmp) {
		printk("vm86: could not access userspace vm86_info\n");
		do_exit(SIGSEGV);
	}
	tss = init_tss + smp_processor_id();
	tss->esp0 = current->thread.esp0 = current->thread.saved_esp0;
	current->thread.saved_esp0 = 0;
	loadsegment(fs, current->thread.saved_fs);
	loadsegment(gs, current->thread.saved_gs);
	ret = KVM86->regs32;
	return ret;
}

static void mark_screen_rdonly(struct task_struct * tsk)
{
	pgd_t *pgd;
	pmd_t *pmd;
	pte_t *pte;
	int i;

	spin_lock(&tsk->mm->page_table_lock);
	pgd = pgd_offset(tsk->mm, 0xA0000);
	if (pgd_none(*pgd))
		goto out;
	if (pgd_bad(*pgd)) {
		pgd_ERROR(*pgd);
		pgd_clear(pgd);
		goto out;
	}
	pmd = pmd_offset(pgd, 0xA0000);
	if (pmd_none(*pmd))
		goto out;
	if (pmd_bad(*pmd)) {
		pmd_ERROR(*pmd);
		pmd_clear(pmd);
		goto out;
	}
	pte = pte_offset(pmd, 0xA0000);
	for (i = 0; i < 32; i++) {
		if (pte_present(*pte))
			set_pte(pte, pte_wrprotect(*pte));
		pte++;
	}
out:
	spin_unlock(&tsk->mm->page_table_lock);
	flush_tlb();
}



static int do_vm86_irq_handling(int subfunction, int irqnumber);
static void do_sys_vm86(struct kernel_vm86_struct *info, struct task_struct *tsk);

asmlinkage int sys_vm86old(struct vm86_struct * v86)
{
	struct kernel_vm86_struct info; /* declare this _on top_,
					 * this avoids wasting of stack space.
					 * This remains on the stack until we
					 * return to 32 bit user space.
					 */
	struct task_struct *tsk;
	int tmp, ret = -EPERM;

	tsk = current;
	if (tsk->thread.saved_esp0)
		goto out;
	tmp  = copy_from_user(&info, v86, VM86_REGS_SIZE1);
	tmp += copy_from_user(&info.regs.VM86_REGS_PART2, &v86->regs.VM86_REGS_PART2,
		(long)&info.vm86plus - (long)&info.regs.VM86_REGS_PART2);
	ret = -EFAULT;
	if (tmp)
		goto out;
	memset(&info.vm86plus, 0, (int)&info.regs32 - (int)&info.vm86plus);
	info.regs32 = (struct pt_regs *) &v86;
	tsk->thread.vm86_info = v86;
	do_sys_vm86(&info, tsk);
	ret = 0;	/* we never return here */
out:
	return ret;
}


asmlinkage int sys_vm86(unsigned long subfunction, struct vm86plus_struct * v86)
{
	struct kernel_vm86_struct info; /* declare this _on top_,
					 * this avoids wasting of stack space.
					 * This remains on the stack until we
					 * return to 32 bit user space.
					 */
	struct task_struct *tsk;
	int tmp, ret;

	tsk = current;
	switch (subfunction) {
		case VM86_REQUEST_IRQ:
		case VM86_FREE_IRQ:
		case VM86_GET_IRQ_BITS:
		case VM86_GET_AND_RESET_IRQ:
			ret = do_vm86_irq_handling(subfunction,(int)v86);
			goto out;
		case VM86_PLUS_INSTALL_CHECK:
			/* NOTE: on old vm86 stuff this will return the error
			   from verify_area(), because the subfunction is
			   interpreted as (invalid) address to vm86_struct.
			   So the installation check works.
			 */
			ret = 0;
			goto out;
	}

	/* we come here only for functions VM86_ENTER, VM86_ENTER_NO_BYPASS */
	ret = -EPERM;
	if (tsk->thread.saved_esp0)
		goto out;
	tmp  = copy_from_user(&info, v86, VM86_REGS_SIZE1);
	tmp += copy_from_user(&info.regs.VM86_REGS_PART2, &v86->regs.VM86_REGS_PART2,
		(long)&info.regs32 - (long)&info.regs.VM86_REGS_PART2);
	ret = -EFAULT;
	if (tmp)
		goto out;
	info.regs32 = (struct pt_regs *) &subfunction;
	info.vm86plus.is_vm86pus = 1;
	tsk->thread.vm86_info = (struct vm86_struct *)v86;
	do_sys_vm86(&info, tsk);
	ret = 0;	/* we never return here */
out:
	return ret;
}


static void do_sys_vm86(struct kernel_vm86_struct *info, struct task_struct *tsk)
{
	struct tss_struct *tss;
/*
 * make sure the vm86() system call doesn't try to do anything silly
 */
	info->regs.__null_ds = 0;
	info->regs.__null_es = 0;

/* we are clearing fs,gs later just before "jmp ret_from_sys_call",
 * because starting with Linux 2.1.x they aren't no longer saved/restored
 */

/*
 * The eflags register is also special: we cannot trust that the user
 * has set it up safely, so this makes sure interrupt etc flags are
 * inherited from protected mode.
 */
 	VEFLAGS = info->regs.eflags;
	info->regs.eflags &= SAFE_MASK;
	info->regs.eflags |= info->regs32->eflags & ~SAFE_MASK;
	info->regs.eflags |= VM_MASK;

	switch (info->cpu_type) {
		case CPU_286:
			tsk->thread.v86mask = 0;
			break;
		case CPU_386:
			tsk->thread.v86mask = NT_MASK | IOPL_MASK;
			break;
		case CPU_486:
			tsk->thread.v86mask = AC_MASK | NT_MASK | IOPL_MASK;
			break;
		default:
			tsk->thread.v86mask = ID_MASK | AC_MASK | NT_MASK | IOPL_MASK;
			break;
	}

/*
 * Save old state, set default return value (%eax) to 0
 */
	info->regs32->eax = 0;
	tsk->thread.saved_esp0 = tsk->thread.esp0;
//	asm volatile("movl %%fs,%0":"=m" (tsk->thread.saved_fs));
//	asm volatile("movl %%fs,%0":"=m" (tsk->thread.saved_fs));
	asm volatile("movw %%gs,%0":"=m" (tsk->thread.saved_gs));
	asm volatile("movw %%gs,%0":"=m" (tsk->thread.saved_gs));

	tss = init_tss + smp_processor_id();
	tss->esp0 = tsk->thread.esp0 = (unsigned long) &info->VM86_TSS_ESP0;

	tsk->thread.screen_bitmap = info->screen_bitmap;
	if (info->flags & VM86_SCREEN_BITMAP)
		mark_screen_rdonly(tsk);
	__asm__ __volatile__(
		"xorl %%eax,%%eax; movl %%eax,%%fs; movl %%eax,%%gs\n\t"
		"movl %0,%%esp\n\t"
		"jmp ret_from_sys_call"
		: /* no outputs */
		:"r" (&info->regs), "b" (tsk) : "ax");
	/* we never return here */
}

static inline void return_to_32bit(struct kernel_vm86_regs * regs16, int retval)
{
	struct pt_regs * regs32;

	regs32 = save_v86_state(regs16);
	regs32->eax = retval;
	__asm__ __volatile__("movl %0,%%esp\n\t"
		"jmp ret_from_sys_call"
		: : "r" (regs32), "b" (current));
}

static inline void set_IF(struct kernel_vm86_regs * regs)
{
	VEFLAGS |= VIF_MASK;
	if (VEFLAGS & VIP_MASK)
		return_to_32bit(regs, VM86_STI);
}

static inline void clear_IF(struct kernel_vm86_regs * regs)
{
	VEFLAGS &= ~VIF_MASK;
}

static inline void clear_TF(struct kernel_vm86_regs * regs)
{
	regs->eflags &= ~TF_MASK;
}

static inline void clear_AC(struct kernel_vm86_regs * regs)
{
	regs->eflags &= ~AC_MASK;
}

/* It is correct to call set_IF(regs) from the set_vflags_*
 * functions. However someone forgot to call clear_IF(regs)
 * in the opposite case.
 * After the command sequence CLI PUSHF STI POPF you should
 * end up with interrups disabled, but you ended up with
 * interrupts enabled.
 *  ( I was testing my own changes, but the only bug I
 *    could find was in a function I had not changed. )
 * [KD]
 */

static inline void set_vflags_long(unsigned long eflags, struct kernel_vm86_regs * regs)
{
	set_flags(VEFLAGS, eflags, current->thread.v86mask);
	set_flags(regs->eflags, eflags, SAFE_MASK);
	if (eflags & IF_MASK)
		set_IF(regs);
	else
		clear_IF(regs);
}

static inline void set_vflags_short(unsigned short flags, struct kernel_vm86_regs * regs)
{
	set_flags(VFLAGS, flags, current->thread.v86mask);
	set_flags(regs->eflags, flags, SAFE_MASK);
	if (flags & IF_MASK)
		set_IF(regs);
	else
		clear_IF(regs);
}

static inline unsigned long get_vflags(struct kernel_vm86_regs * regs)
{
	unsigned long flags = regs->eflags & RETURN_MASK;

	if (VEFLAGS & VIF_MASK)
		flags |= IF_MASK;
	else
		flags &= ~IF_MASK;
	flags |= IOPL_MASK;
	return flags | (VEFLAGS & current->thread.v86mask);
}

static inline int is_revectored(int nr, struct revectored_struct * bitmap)
{
	__asm__ __volatile__("btl %2,%1\n\tsbbl %0,%0"
		:"=r" (nr)
		:"m" (*bitmap),"r" (nr));
	return nr;
}

#define val_byte(val, n) (((__u8 *)&val)[n])

#define pushb(base, ptr, val, err_label) \
	do { \
		__u8 __val = val; \
		ptr--; \
		if (put_user(__val, base + ptr) < 0) \
			goto err_label; \
	} while(0)

#define pushw(base, ptr, val, err_label) \
	do { \
		__u16 __val = val; \
		ptr--; \
		if (put_user(val_byte(__val, 1), base + ptr) < 0) \
			goto err_label; \
		ptr--; \
		if (put_user(val_byte(__val, 0), base + ptr) < 0) \
			goto err_label; \
	} while(0)

#define pushl(base, ptr, val, err_label) \
	do { \
		__u32 __val = val; \
		ptr--; \
		if (put_user(val_byte(__val, 3), base + ptr) < 0) \
			goto err_label; \
		ptr--; \
		if (put_user(val_byte(__val, 2), base + ptr) < 0) \
			goto err_label; \
		ptr--; \
		if (put_user(val_byte(__val, 1), base + ptr) < 0) \
			goto err_label; \
		ptr--; \
		if (put_user(val_byte(__val, 0), base + ptr) < 0) \
			goto err_label; \
	} while(0)

#define popb(base, ptr, err_label) \
	({ \
	 	__u8 __res; \
		if (get_user(__res, base + ptr) < 0) \
			goto err_label; \
		ptr++; \
		__res; \
	})

#define popw(base, ptr, err_label) \
	({ \
	 	__u16 __res; \
		if (get_user(val_byte(__res, 0), base + ptr) < 0) \
			goto err_label; \
		ptr++; \
		if (get_user(val_byte(__res, 1), base + ptr) < 0) \
			goto err_label; \
		ptr++; \
		__res; \
	})

#define popl(base, ptr, err_label) \
	({ \
	 	__u32 __res; \
		if (get_user(val_byte(__res, 0), base + ptr) < 0) \
			goto err_label; \
		ptr++; \
		if (get_user(val_byte(__res, 1), base + ptr) < 0) \
			goto err_label; \
		ptr++; \
		if (get_user(val_byte(__res, 2), base + ptr) < 0) \
			goto err_label; \
		ptr++; \
		if (get_user(val_byte(__res, 3), base + ptr) < 0) \
			goto err_label; \
		ptr++; \
		__res; \
	})

/* There are so many possible reasons for this function to return
 * VM86_INTx, so adding another doesn't bother me. We can expect
 * userspace programs to be able to handle it. (Getting a problem
 * in userspace is always better than an Oops anyway.) [KD]
 */
static void do_int(struct kernel_vm86_regs *regs, int i,
    unsigned char * ssp, unsigned short sp)
{
	unsigned long *intr_ptr, segoffs;

	if (regs->cs == BIOSSEG)
		goto cannot_handle;
	if (is_revectored(i, &KVM86->int_revectored))
		goto cannot_handle;
	if (i==0x21 && is_revectored(AH(regs),&KVM86->int21_revectored))
		goto cannot_handle;
	intr_ptr = (unsigned long *) (i << 2);
	if (get_user(segoffs, intr_ptr))
		goto cannot_handle;
	if ((segoffs >> 16) == BIOSSEG)
		goto cannot_handle;
	pushw(ssp, sp, get_vflags(regs), cannot_handle);
	pushw(ssp, sp, regs->cs, cannot_handle);
	pushw(ssp, sp, IP(regs), cannot_handle);
	regs->cs = segoffs >> 16;
	SP(regs) -= 6;
	IP(regs) = segoffs & 0xffff;
	clear_TF(regs);
	clear_IF(regs);
	clear_AC(regs);
	return;

cannot_handle:
	return_to_32bit(regs, VM86_INTx + (i << 8));
}

int handle_vm86_trap(struct kernel_vm86_regs * regs, long error_code, int trapno)
{
	if (VMPI.is_vm86pus) {
		if ( (trapno==3) || (trapno==1) )
			return_to_32bit(regs, VM86_TRAP + (trapno << 8));
		do_int(regs, trapno, (unsigned char *) (regs->ss << 4), SP(regs));
		return 0;
	}
	if (trapno !=1)
		return 1; /* we let this handle by the calling routine */
	if (current->ptrace & PT_PTRACED) {
		unsigned long flags;
		spin_lock_irqsave(&current->sighand->siglock, flags);
		sigdelset(&current->blocked, SIGTRAP);
		recalc_sigpending();
		spin_unlock_irqrestore(&current->sighand->siglock, flags);
	}
	send_sig(SIGTRAP, current, 1);
	current->thread.trap_no = trapno;
	current->thread.error_code = error_code;
	return 0;
}

void handle_vm86_fault(struct kernel_vm86_regs * regs, long error_code)
{
	unsigned char *csp, *ssp, opcode;
	unsigned short ip, sp;
	int data32, pref_done;

#define CHECK_IF_IN_TRAP \
	if (VMPI.vm86dbg_active && VMPI.vm86dbg_TFpendig) \
		newflags |= TF_MASK
#define VM86_FAULT_RETURN do { \
	if (VMPI.force_return_for_pic  && (VEFLAGS & (IF_MASK | VIF_MASK))) \
		return_to_32bit(regs, VM86_PICRETURN); \
	return; } while (0)
	                                   
	csp = (unsigned char *) (regs->cs << 4);
	ssp = (unsigned char *) (regs->ss << 4);
	sp = SP(regs);
	ip = IP(regs);

	data32 = 0;
	pref_done = 0;
	do {
		switch (opcode = popb(csp, ip, simulate_sigsegv)) {
			case 0x66:      /* 32-bit data */     data32=1; break;
			case 0x67:      /* 32-bit address */  break;
			case 0x2e:      /* CS */              break;
			case 0x3e:      /* DS */              break;
			case 0x26:      /* ES */              break;
			case 0x36:      /* SS */              break;
			case 0x65:      /* GS */              break;
			case 0x64:      /* FS */              break;
			case 0xf2:      /* repnz */	      break;
			case 0xf3:      /* rep */             break;
			default: pref_done = 1;
		}
	} while (!pref_done);

	switch (opcode) {

	/* pushf */
	case 0x9c:
		if (data32) {
			pushl(ssp, sp, get_vflags(regs), simulate_sigsegv);
			SP(regs) -= 4;
		} else {
			pushw(ssp, sp, get_vflags(regs), simulate_sigsegv);
			SP(regs) -= 2;
		}
		IP(regs) = ip;
		VM86_FAULT_RETURN;

	/* popf */
	case 0x9d:
		{
		unsigned long newflags;
		if (data32) {
			newflags=popl(ssp, sp, simulate_sigsegv);
			SP(regs) += 4;
		} else {
			newflags = popw(ssp, sp, simulate_sigsegv);
			SP(regs) += 2;
		}
		IP(regs) = ip;
		CHECK_IF_IN_TRAP;
		if (data32) {
			set_vflags_long(newflags, regs);
		} else {
			set_vflags_short(newflags, regs);
		}
		VM86_FAULT_RETURN;
		}

	/* int xx */
	case 0xcd: {
	        int intno=popb(csp, ip, simulate_sigsegv);
		IP(regs) = ip;
		if (VMPI.vm86dbg_active) {
			if ( (1 << (intno &7)) & VMPI.vm86dbg_intxxtab[intno >> 3] )
				return_to_32bit(regs, VM86_INTx + (intno << 8));
		}
		do_int(regs, intno, ssp, sp);
		return;
	}

	/* iret */
	case 0xcf:
		{
		unsigned long newip;
		unsigned long newcs;
		unsigned long newflags;
		if (data32) {
			newip=popl(ssp, sp, simulate_sigsegv);
			newcs=popl(ssp, sp, simulate_sigsegv);
			newflags=popl(ssp, sp, simulate_sigsegv);
			SP(regs) += 12;
		} else {
			newip = popw(ssp, sp, simulate_sigsegv);
			newcs = popw(ssp, sp, simulate_sigsegv);
			newflags = popw(ssp, sp, simulate_sigsegv);
			SP(regs) += 6;
		}
		IP(regs) = newip;
		regs->cs = newcs;
		CHECK_IF_IN_TRAP;
		if (data32) {
			set_vflags_long(newflags, regs);
		} else {
			set_vflags_short(newflags, regs);
		}
		VM86_FAULT_RETURN;
		}

	/* cli */
	case 0xfa:
		IP(regs) = ip;
		clear_IF(regs);
		VM86_FAULT_RETURN;

	/* sti */
	/*
	 * Damn. This is incorrect: the 'sti' instruction should actually
	 * enable interrupts after the /next/ instruction. Not good.
	 *
	 * Probably needs some horsing around with the TF flag. Aiee..
	 */
	case 0xfb:
		IP(regs) = ip;
		set_IF(regs);
		VM86_FAULT_RETURN;

	default:
		return_to_32bit(regs, VM86_UNKNOWN);
	}

	return;

simulate_sigsegv:
	/* FIXME: After a long discussion with Stas we finally
	 *        agreed, that this is wrong. Here we should
	 *        really send a SIGSEGV to the user program.
	 *        But how do we create the correct context? We
	 *        are inside a general protection fault handler
	 *        and has just returned from a page fault handler.
	 *        The correct context for the signal handler
	 *        should be a mixture of the two, but how do we
	 *        get the information? [KD]
	 */
	return_to_32bit(regs, VM86_UNKNOWN);
}

/* ---------------- vm86 special IRQ passing stuff ----------------- */

#define VM86_IRQNAME		"vm86irq"

static struct vm86_irqs {
	struct task_struct *tsk;
	int sig;
} vm86_irqs[16];
static int irqbits;

#define ALLOWED_SIGS ( 1 /* 0 = don't send a signal */ \
	| (1 << SIGUSR1) | (1 << SIGUSR2) | (1 << SIGIO)  | (1 << SIGURG) \
	| (1 << SIGUNUSED) )
	
static void irq_handler(int intno, void *dev_id, struct pt_regs * regs) {
	int irq_bit;
	unsigned long flags;
	
	save_flags(flags);
	cli();
	irq_bit = 1 << intno;
	if ((irqbits & irq_bit) || ! vm86_irqs[intno].tsk)
		goto out;
	irqbits |= irq_bit;
	if (vm86_irqs[intno].sig)
		send_sig(vm86_irqs[intno].sig, vm86_irqs[intno].tsk, 1);
	/* else user will poll for IRQs */
out:
	restore_flags(flags);
}

static inline void free_vm86_irq(int irqnumber)
{
	free_irq(irqnumber,0);
	vm86_irqs[irqnumber].tsk = 0;
	irqbits &= ~(1 << irqnumber);
}

static inline int task_valid(struct task_struct *tsk)
{
	struct task_struct *g, *p;
	int ret = 0;

	read_lock(&tasklist_lock);
	do_each_thread(g, p) 
		if ((p == tsk) && (p->signal)) {
			ret = 1;
			break;
		}
	while_each_thread(g, p);
	read_unlock(&tasklist_lock);
	return ret;
}

void release_x86_irqs(struct task_struct *task)
{
	int i;
	for (i=3; i<16; i++)
	    if (vm86_irqs[i].tsk == task)
		free_vm86_irq(i);
}

static inline void handle_irq_zombies(void)
{
	int i;
	for (i=3; i<16; i++) {
		if (vm86_irqs[i].tsk) {
			if (task_valid(vm86_irqs[i].tsk)) continue;
			free_vm86_irq(i);
		}
	}
}

static inline int get_and_reset_irq(int irqnumber)
{
	int bit;
	unsigned long flags;
	
	if ( (irqnumber<3) || (irqnumber>15) ) return 0;
	if (vm86_irqs[irqnumber].tsk != current) return 0;
	save_flags(flags);
	cli();
	bit = irqbits & (1 << irqnumber);
	irqbits &= ~bit;
	restore_flags(flags);
	return bit;
}


static int do_vm86_irq_handling(int subfunction, int irqnumber)
{
	int ret;
	switch (subfunction) {
		case VM86_GET_AND_RESET_IRQ: {
			return get_and_reset_irq(irqnumber);
		}
		case VM86_GET_IRQ_BITS: {
			return irqbits;
		}
		case VM86_REQUEST_IRQ: {
			int sig = irqnumber >> 8;
			int irq = irqnumber & 255;
			handle_irq_zombies();
			if (!capable(CAP_SYS_ADMIN)) return -EPERM;
			if (!((1 << sig) & ALLOWED_SIGS)) return -EPERM;
			if ( (irq<3) || (irq>15) ) return -EPERM;
			if (vm86_irqs[irq].tsk) return -EPERM;
			ret = request_irq(irq, &irq_handler, 0, VM86_IRQNAME, 0);
			if (ret) return ret;
			vm86_irqs[irq].sig = sig;
			vm86_irqs[irq].tsk = current;
			return irq;
		}
		case  VM86_FREE_IRQ: {
			handle_irq_zombies();
			if ( (irqnumber<3) || (irqnumber>15) ) return -EPERM;
			if (!vm86_irqs[irqnumber].tsk) return 0;
			if (vm86_irqs[irqnumber].tsk != current) return -EPERM;
			free_vm86_irq(irqnumber);
			return 0;
		}
	}
	return -EINVAL;
}

