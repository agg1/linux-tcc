#ifndef _LINUX_PTRACE_H
#define _LINUX_PTRACE_H
/* ptrace.h */
/* structs and defines to help the user use the ptrace system call. */

#include <linux/compiler.h>

/* has the defines to get at the registers. */

#define PTRACE_TRACEME		   0
#define PTRACE_PEEKTEXT		   1
#define PTRACE_PEEKDATA		   2
#define PTRACE_PEEKUSR		   3
#define PTRACE_POKETEXT		   4
#define PTRACE_POKEDATA		   5
#define PTRACE_POKEUSR		   6
#define PTRACE_CONT		   7
#define PTRACE_KILL		   8
#define PTRACE_SINGLESTEP	   9

#define PTRACE_ATTACH		0x10
#define PTRACE_DETACH		0x11

#define PTRACE_SYSCALL		  24

/* 0x4200-0x4300 are reserved for architecture-independent additions.  */
#define PTRACE_SETOPTIONS	0x4200
#define PTRACE_GETEVENTMSG	0x4201
#define PTRACE_GETSIGINFO	0x4202
#define PTRACE_SETSIGINFO	0x4203

/* options set using PTRACE_SETOPTIONS */
#define PTRACE_O_TRACESYSGOOD	0x00000001
#define PTRACE_O_TRACEFORK	0x00000002
#define PTRACE_O_TRACEVFORK	0x00000004
#define PTRACE_O_TRACECLONE	0x00000008
#define PTRACE_O_TRACEEXEC	0x00000010
#define PTRACE_O_TRACEVFORKDONE	0x00000020
#define PTRACE_O_TRACEEXIT	0x00000040

/* Wait extended result codes for the above trace options.  */
#define PTRACE_EVENT_FORK	1
#define PTRACE_EVENT_VFORK	2
#define PTRACE_EVENT_CLONE	3
#define PTRACE_EVENT_EXEC	4
#define PTRACE_EVENT_VFORK_DONE	5
#define PTRACE_EVENT_EXIT	6

#include <asm/ptrace.h>
#include <linux/sched.h>

extern int ptrace_readdata(struct task_struct *tsk, unsigned long src, char *dst, int len);
extern int ptrace_writedata(struct task_struct *tsk, char * src, unsigned long dst, int len);
extern int ptrace_attach(struct task_struct *tsk);
extern int ptrace_detach(struct task_struct *, unsigned int);
extern void ptrace_disable(struct task_struct *);
extern int ptrace_check_attach(struct task_struct *task, int kill);
extern int ptrace_request(struct task_struct *child, long request, long addr, long data);
extern void ptrace_notify(int exit_code);
extern void __ptrace_link(struct task_struct *child,
				struct task_struct *new_parent);
extern void __ptrace_unlink(struct task_struct *child);

static inline void ptrace_link(struct task_struct *child,
				struct task_struct *new_parent)
{
	if (unlikely(child->ptrace))
		__ptrace_link(child, new_parent);
}
static inline void ptrace_unlink(struct task_struct *child)
{
	if (unlikely(child->ptrace))
		__ptrace_unlink(child);
}

#ifdef __KERNEL__

#ifndef force_successful_syscall_return
/*
 * System call handlers that, upon successful completion, need to return a
 * negative value should call force_successful_syscall_return() right before
 * returning.  On architectures where the syscall convention provides for a
 * separate error flag (e.g., alpha, ia64, ppc{,64}, sparc{,64}, possibly
 * others), this macro can be used to ensure that the error flag will not get
 * set.  On architectures which do not support a separate error flag, the macro
 * is a no-op and the spurious error condition needs to be filtered out by some
 * other means (e.g., in user-level, by passing an extra argument to the
 * syscall handler, or something along those lines).
 */
#define force_successful_syscall_return() do { } while (0)
#endif
#endif

#endif
