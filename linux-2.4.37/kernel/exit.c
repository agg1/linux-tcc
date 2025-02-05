/*
 *  linux/kernel/exit.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linux/config.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/smp_lock.h>
#include <linux/module.h>
#include <linux/completion.h>
#include <linux/personality.h>
#include <linux/tty.h>
#include <linux/namespace.h>
#include <linux/acct.h>
#include <linux/file.h>
#include <linux/binfmts.h>
#include <linux/ptrace.h>
#include <linux/mount.h>

#include <linux/grsecurity.h>

#include <asm/uaccess.h>
#include <asm/pgtable.h>
#include <asm/mmu_context.h>

extern void sem_exit (void);
extern struct task_struct *child_reaper;

int getrusage(struct task_struct *, int, struct rusage *);

static void __unhash_process(struct task_struct *p)
{
	nr_threads--;
	detach_pid(p, PIDTYPE_PID);
	detach_pid(p, PIDTYPE_TGID);
	if (thread_group_leader(p)) {
		detach_pid(p, PIDTYPE_PGID);
		detach_pid(p, PIDTYPE_SID);
	}

	REMOVE_LINKS(p);
	p->pid = 0;
}

void release_task(struct task_struct * p)
{
	task_t *leader;
 
	BUG_ON(p->state < TASK_ZOMBIE);
 
	if (p != current)
		wait_task_inactive(p);

	atomic_dec(&p->user->processes);
	free_uid(p->user);
	write_lock_irq(&tasklist_lock);
	if (unlikely(p->ptrace))
		__ptrace_unlink(p);
	BUG_ON(!list_empty(&p->ptrace_list) || !list_empty(&p->ptrace_children));
	__exit_signal(p);
	__exit_sighand(p);
	__unhash_process(p);

	/*
	 * If we are the last non-leader member of the thread
	 * group, and the leader is zombie, then notify the
	 * group leader's parent process. (if it wants notification.)
	 */
	leader = p->group_leader;
	if (leader != p && thread_group_empty(leader) &&
		    leader->state == TASK_ZOMBIE && leader->exit_signal != -1)
		do_notify_parent(leader, leader->exit_signal);

	p->parent->times.tms_cutime += p->times.tms_utime + p->times.tms_cutime;
	p->parent->times.tms_cstime += p->times.tms_stime + p->times.tms_cstime;
	p->parent->group_leader->group_times.tms_cutime += p->times.tms_utime + p->times.tms_cutime;
	p->parent->group_leader->group_times.tms_cstime += p->times.tms_stime + p->times.tms_cstime;

	p->parent->cmin_flt += p->min_flt + p->cmin_flt;
	p->parent->cmaj_flt += p->maj_flt + p->cmaj_flt;
	p->parent->cnswap += p->nswap + p->cnswap;
	sched_exit(p);
	write_unlock_irq(&tasklist_lock);

	release_thread(p);
	put_task_struct(p);
}

/* we are using it only for SMP init */

void unhash_process(struct task_struct *p)
{
	write_lock_irq(&tasklist_lock);
	__unhash_process(p);
	write_unlock_irq(&tasklist_lock);
}

/*
 * This checks not only the pgrp, but falls back on the pid if no
 * satisfactory pgrp is found. I dunno - gdb doesn't work correctly
 * without this...
 */
int session_of_pgrp(int pgrp)
{
	struct task_struct *p;
	struct list_head *l;
	struct pid *pid;
	int sid = -1;

	read_lock(&tasklist_lock);
	for_each_task_pid(pgrp, PIDTYPE_PGID, p, l, pid)
		if (p->session > 0) {
			sid = p->session;
			goto out;
		}
	p = find_task_by_pid(pgrp);
	if (p)
		sid = p->session;
out:
	read_unlock(&tasklist_lock);
	
	return sid;
}

/*
 * Determine if a process group is "orphaned", according to the POSIX
 * definition in 2.2.2.52.  Orphaned process groups are not to be affected
 * by terminal-generated stop signals.  Newly orphaned process groups are
 * to receive a SIGHUP and a SIGCONT.
 *
 * "I ask you, have you ever known what it is to be an orphan?"
 */
static int will_become_orphaned_pgrp(int pgrp, task_t *ignored_task)
{
	struct task_struct *p;
	struct list_head *l;
	struct pid *pid;
	int ret = 1;

	for_each_task_pid(pgrp, PIDTYPE_PGID, p, l, pid) {
		if (p == ignored_task
				|| p->state >= TASK_ZOMBIE 
				|| p->real_parent->pid == 1)
			continue;
		if (p->real_parent->pgrp != pgrp
			    && p->real_parent->session == p->session) {
			ret = 0;
			break;
		}
	}
	return ret;	/* (sighing) "Often!" */
}

int is_orphaned_pgrp(int pgrp)
{
	int retval;

	read_lock(&tasklist_lock);
	retval = will_become_orphaned_pgrp(pgrp, NULL);
	read_unlock(&tasklist_lock);

	return retval;
}

static inline int has_stopped_jobs(int pgrp)
{
	int retval = 0;
	struct task_struct *p;
	struct list_head *l;
	struct pid *pid;

	for_each_task_pid(pgrp, PIDTYPE_PGID, p, l, pid) {
		if (p->state != TASK_STOPPED)
			continue;

		/* If p is stopped by a debugger on a signal that won't
		   stop it, then don't count p as stopped.  This isn't
		   perfect but it's a good approximation.  */
		if (unlikely (p->ptrace)
		    && p->exit_code != SIGSTOP
		    && p->exit_code != SIGTSTP
		    && p->exit_code != SIGTTOU
		    && p->exit_code != SIGTTIN)
			continue;

		retval = 1;
		break;
	}
	return retval;
}

/**
 * reparent_to_init() - Reparent the calling kernel thread to the init task.
 *
 * If a kernel thread is launched as a result of a system call, or if
 * it ever exits, it should generally reparent itself to init so that
 * it is correctly cleaned up on exit.
 *
 * The various task state such as scheduling policy and priority may have
 * been inherited from a user process, so we reset them to sane values here.
 *
 * NOTE that reparent_to_init() gives the caller full capabilities.
 */
void reparent_to_init(void)
{
	write_lock_irq(&tasklist_lock);

	ptrace_unlink(current);
	/* Reparent to init */
	REMOVE_LINKS(current);
	current->parent = child_reaper;
	current->real_parent = child_reaper;
	SET_LINKS(current);

	/* Set the exit signal to SIGCHLD so we signal init on exit */
	current->exit_signal = SIGCHLD;

	if ((current->policy == SCHED_NORMAL) && (task_nice(current) < 0))
		set_user_nice(current, 0);
	/* cpus_allowed? */
	/* rt_priority? */
	/* signals? */
	memcpy(current->rlim, init_task.rlim, sizeof(*(current->rlim)));
	switch_uid(INIT_USER);

	write_unlock_irq(&tasklist_lock);
}

/*
 *	Put all the gunge required to become a kernel thread without
 *	attached user resources in one place where it belongs.
 */

void daemonize(void)
{
	struct fs_struct *fs;


	/*
	 * If we were started as result of loading a module, close all of the
	 * user space pages.  We don't need them, and if we didn't close them
	 * they would be locked into memory.
	 */
	exit_mm(current);

	set_special_pids(1, 1);
	current->tty = NULL;

	/* Become as one with the init task */

	exit_fs(current);	/* current->fs->count--; */
	fs = init_task.fs;
	current->fs = fs;
	atomic_inc(&fs->count);
 	exit_files(current);
	current->files = init_task.files;
	atomic_inc(&current->files->count);

	reparent_to_init();
}

void __set_special_pids(pid_t session, pid_t pgrp)
{
	struct task_struct *curr = current;

	if (curr->session != session) {
		detach_pid(curr, PIDTYPE_SID);
		curr->session = session;
		attach_pid(curr, PIDTYPE_SID, session);
	}
	if (curr->pgrp != pgrp) {
		detach_pid(curr, PIDTYPE_PGID);
		curr->pgrp = pgrp;
		attach_pid(curr, PIDTYPE_PGID, pgrp);
	}
}

void set_special_pids(pid_t session, pid_t pgrp)
{
	write_lock_irq(&tasklist_lock);
	__set_special_pids(session, pgrp);
	write_unlock_irq(&tasklist_lock);
}

static inline void close_files(struct files_struct * files)
{
	int i, j;

	j = 0;
	for (;;) {
		unsigned long set;
		i = j * __NFDBITS;
		if (i >= files->max_fdset || i >= files->max_fds)
			break;
		set = files->open_fds->fds_bits[j++];
		while (set) {
			if (set & 1) {
				struct file * file = xchg(&files->fd[i], NULL);
				if (file)
					filp_close(file, files);
			}
			i++;
			set >>= 1;
		}
	}
}

void put_files_struct(struct files_struct *files)
{
	if (atomic_dec_and_test(&files->count)) {
		close_files(files);
		/*
		 * Free the fd and fdset arrays if we expanded them.
		 */
		if (files->fd != &files->fd_array[0])
			free_fd_array(files->fd, files->max_fds);
		if (files->max_fdset > __FD_SETSIZE) {
			free_fdset(files->open_fds, files->max_fdset);
			free_fdset(files->close_on_exec, files->max_fdset);
		}
		kmem_cache_free(files_cachep, files);
	}
}

static inline void __exit_files(struct task_struct *tsk)
{
	struct files_struct * files = tsk->files;

	if (files) {
		task_lock(tsk);
		tsk->files = NULL;
		task_unlock(tsk);
		put_files_struct(files);
	}
}

void exit_files(struct task_struct *tsk)
{
	__exit_files(tsk);
}

static inline void __put_fs_struct(struct fs_struct *fs)
{
	/* No need to hold fs->lock if we are killing it */
	if (atomic_dec_and_test(&fs->count)) {
		dput(fs->root);
		mntput(fs->rootmnt);
		dput(fs->pwd);
		mntput(fs->pwdmnt);
		if (fs->altroot) {
			dput(fs->altroot);
			mntput(fs->altrootmnt);
		}
		kmem_cache_free(fs_cachep, fs);
	}
}

void put_fs_struct(struct fs_struct *fs)
{
	__put_fs_struct(fs);
}

static inline void __exit_fs(struct task_struct *tsk)
{
	struct fs_struct * fs = tsk->fs;

	if (fs) {
		task_lock(tsk);
		tsk->fs = NULL;
		task_unlock(tsk);
		__put_fs_struct(fs);
	}
}

void exit_fs(struct task_struct *tsk)
{
	__exit_fs(tsk);
}

/*
 * We can use these to temporarily drop into
 * "lazy TLB" mode and back.
 */
struct mm_struct * start_lazy_tlb(void)
{
	struct mm_struct *mm = current->mm;
	current->mm = NULL;
	/* active_mm is still 'mm' */
	atomic_inc(&mm->mm_count);
	enter_lazy_tlb(mm, current, smp_processor_id());
	return mm;
}

void end_lazy_tlb(struct mm_struct *mm)
{
	struct mm_struct *active_mm = current->active_mm;

	current->mm = mm;
	if (mm != active_mm) {
		current->active_mm = mm;
		activate_mm(active_mm, mm);
	}
	mmdrop(active_mm);
}

/*
 * Turn us into a lazy TLB process if we
 * aren't already..
 */
static inline void __exit_mm(struct task_struct * tsk)
{
	struct mm_struct *mm = tsk->mm;

	mm_release();
	if (!mm)
		return;
	/*
	 * Serialize with any possible pending coredump:
	 */
	if (mm->core_waiters) {
		down_write(&mm->mmap_sem);
		if (!--mm->core_waiters)
			complete(mm->core_startup_done);
		up_write(&mm->mmap_sem);

		wait_for_completion(&mm->core_done);
	}
	atomic_inc(&mm->mm_count);
	if (mm != tsk->active_mm) BUG();
	/* more a memory barrier than a real lock */
	task_lock(tsk);
	tsk->mm = NULL;
	enter_lazy_tlb(mm, current, smp_processor_id());
	task_unlock(tsk);
	mmput(mm);
}

void exit_mm(struct task_struct *tsk)
{
	__exit_mm(tsk);
}

static inline void choose_new_parent(task_t *p, task_t *reaper, task_t *child_reaper)
{
	/*
	 * Make sure we're not reparenting to ourselves and that
	 * the parent is not a zombie.
	 */
	if (p == reaper || reaper->state >= TASK_ZOMBIE)
		p->real_parent = child_reaper;
	else
		p->real_parent = reaper;
	if (p->parent == p->real_parent)
		BUG();
}

static inline void reparent_thread(task_t *p, task_t *father, int traced)
{
	/* We dont want people slaying init.  */
	if (p->exit_signal != -1)
		p->exit_signal = SIGCHLD;
	p->self_exec_id++;

	if (p->pdeath_signal)
		/* We already hold the tasklist_lock here.  */
		group_send_sig_info(p->pdeath_signal, (void *) 0, p);

	/* Move the child from its dying parent to the new one.  */
	if (unlikely(traced)) {
		/* Preserve ptrace links if someone else is tracing this child.  */
		list_del_init(&p->ptrace_list);
		if (p->parent != p->real_parent)
			list_add(&p->ptrace_list, &p->real_parent->ptrace_children);
	} else {
		/* If this child is being traced, then we're the one tracing it
		 * anyway, so let go of it.
		 */
		p->ptrace = 0;
		list_del_init(&p->sibling);
		p->parent = p->real_parent;
		list_add_tail(&p->sibling, &p->parent->children);

		/* If we'd notified the old parent about this child's death,
		 * also notify the new parent.
		 */
		if (p->state == TASK_ZOMBIE && p->exit_signal != -1)
			do_notify_parent(p, p->exit_signal);
	}

	/*
	 * process group orphan check
	 * Case ii: Our child is in a different pgrp
	 * than we are, and it was the only connection
	 * outside, so the child pgrp is now orphaned.
	 */
	if ((p->pgrp != father->pgrp) &&
	    (p->session == father->session)) {
		int pgrp = p->pgrp;

		if (will_become_orphaned_pgrp(pgrp, NULL) && has_stopped_jobs(pgrp)) {
			__kill_pg_info(SIGHUP, (void *)1, pgrp);
			__kill_pg_info(SIGCONT, (void *)1, pgrp);
		}
	}
}

/*
 * When we die, we re-parent all our children.
 * Try to give them to another thread in our thread
 * group, and if no such member exists, give it to
 * the global child reaper process (ie "init")
 */
static inline void forget_original_parent(struct task_struct * father)
{
	struct task_struct *p, *reaper = father;
	struct list_head *_p, *_n;

	reaper = father->group_leader;
	if (reaper == father)
		reaper = child_reaper;

	/*
	 * There are only two places where our children can be:
	 *
	 * - in our child list
	 * - in our ptraced child list
	 *
	 * Search them and reparent children.
	 */
	list_for_each_safe(_p, _n, &father->children) {
		p = list_entry(_p,struct task_struct,sibling);
		if (father == p->real_parent) {
			choose_new_parent(p, reaper, child_reaper);
			reparent_thread(p, father, 0);
		} else {
			ptrace_unlink (p);
			if (p->state == TASK_ZOMBIE && p->exit_signal != -1)
				do_notify_parent(p, p->exit_signal);
		}
	}
	list_for_each_safe(_p, _n, &father->ptrace_children) {
		p = list_entry(_p,struct task_struct,ptrace_list);
		choose_new_parent(p, reaper, child_reaper);
		reparent_thread(p, father, 1);
	}
}

/*
 * Send signals to all our closest relatives so that they know
 * to properly mourn us..
 */
static void exit_notify(struct task_struct *tsk)
{
	struct task_struct *t;

	if (signal_pending(tsk) && !tsk->signal->group_exit
	    && !thread_group_empty(tsk)) {
		/*
		 * This occurs when there was a race between our exit
		 * syscall and a group signal choosing us as the one to
		 * wake up.  It could be that we are the only thread
		 * alerted to check for pending signals, but another thread
		 * should be woken now to take the signal since we will not.
		 * Now we'll wake all the threads in the group just to make
		 * sure someone gets all the pending signals.
		 */
		read_lock(&tasklist_lock);
		spin_lock_irq(&tsk->sighand->siglock);
		for (t = next_thread(tsk); t != tsk; t = next_thread(t))
			if (!signal_pending(t) && !(t->flags & PF_EXITING)) {
				recalc_sigpending_tsk(t);
				if (signal_pending(t))
					signal_wake_up(t, 0);
			}
		spin_unlock_irq(&tsk->sighand->siglock);
		read_unlock(&tasklist_lock);
	}

	write_lock_irq(&tasklist_lock);

	/*
	 * This does two things:
	 *
  	 * A.  Make init inherit all the child processes
	 * B.  Check to see if any process groups have become orphaned
	 *	as a result of our exiting, and if they have any stopped
	 *	jobs, send them a SIGHUP and then a SIGCONT.  (POSIX 3.2.2.2)
	 */

	forget_original_parent(tsk);
	BUG_ON(!list_empty(&tsk->children));

	/*
	 * Check to see if any process groups have become orphaned
	 * as a result of our exiting, and if they have any stopped
	 * jobs, send them a SIGHUP and then a SIGCONT.  (POSIX 3.2.2.2)
	 *
	 * Case i: Our father is in a different pgrp than we are
	 * and we were the only connection outside, so our pgrp
	 * is about to become orphaned.
	 */
	 
	t = tsk->real_parent;
	
	if ((t->pgrp != tsk->pgrp) &&
	    (t->session == tsk->session) &&
	    will_become_orphaned_pgrp(tsk->pgrp, tsk) &&
	    has_stopped_jobs(tsk->pgrp)) {
		__kill_pg_info(SIGHUP, (void *)1, tsk->pgrp);
		__kill_pg_info(SIGCONT, (void *)1, tsk->pgrp);
	}

	/* Let father know we died 
	 *
	 * Thread signals are configurable, but you aren't going to use
	 * that to send signals to arbitary processes. 
	 * That stops right now.
	 *
	 * If the parent exec id doesn't match the exec id we saved
	 * when we started then we know the parent has changed security
	 * domain.
	 *
	 * If our self_exec id doesn't match our parent_exec_id then
	 * we have changed execution domain as these two values started
	 * the same after a fork.
	 */
	
	if (tsk->exit_signal != SIGCHLD && tsk->exit_signal != -1 &&
	    ( tsk->parent_exec_id != t->self_exec_id  ||
	      tsk->self_exec_id != tsk->parent_exec_id)
 	    && !capable(CAP_KILL))
		tsk->exit_signal = SIGCHLD;

	/*
	 * If something other than our normal parent is ptracing us, then
	 * send it a SIGCHLD instead of honoring exit_signal.  exit_signal
	 * only has special meaning to our real parent.
	 */
	if (tsk->exit_signal != -1) {
		if (tsk->parent == tsk->real_parent)
			do_notify_parent(tsk, tsk->exit_signal);
		else
			do_notify_parent(tsk, SIGCHLD);
	}

	tsk->state = TASK_ZOMBIE;
	/*
	 * No need to unlock IRQs, we'll schedule() immediately
	 * anyway.
	 */
	write_unlock(&tasklist_lock);
}

asmlinkage NORET_TYPE void do_exit(long code)
{
	struct task_struct *tsk = current;

	if (unlikely(in_interrupt()))
		panic("Aiee, killing interrupt handler!");
	if (unlikely(!tsk->pid))
		panic("Attempted to kill the idle task!");
	if (unlikely(tsk->pid == 1))
		panic("Attempted to kill init!");

	/*
	 * If do_exit is called because this processes oopsed, it's possible
	 * that get_fs() was left as KERNEL_DS, so reset it to USER_DS before
	 * continuing. Amongst other possible reasons, this is to prevent
	 * mm_release()->clear_child_tid() from writing to a user-controlled
	 * kernel address.
	 */
	set_fs(USER_DS);

	tsk->flags |= PF_EXITING;
	del_timer_sync(&tsk->real_timer);

	if (unlikely(current->ptrace & PT_TRACE_EXIT))
		ptrace_notify((PTRACE_EVENT_EXIT << 8) | SIGTRAP);

	acct_process(code);

	gr_del_task_from_ip_table(tsk);

	__exit_mm(tsk);

	sem_exit();
	gr_shm_exit();
	__exit_files(tsk);
	__exit_fs(tsk);
	exit_namespace(tsk);
	exit_thread();

	if (tsk->leader)
		disassociate_ctty(1);

	put_exec_domain(tsk->exec_domain);
	if (tsk->binfmt && tsk->binfmt->module)
		__MOD_DEC_USE_COUNT(tsk->binfmt->module);

	tsk->exit_code = code;
	exit_notify(tsk);

	if (tsk->exit_signal == -1)
		release_task(tsk);

	schedule();
	BUG();
	/* Avoid "noreturn function does return".  */
	for (;;) ;
}

NORET_TYPE void complete_and_exit(struct completion *comp, long code)
{
	if (comp)
		complete(comp);
	
	do_exit(code);
}

asmlinkage void sys_exit(int error_code)
{
	do_exit((error_code&0xff)<<8);
}

void check_tasklist_locked(void)
{
#if CONFIG_SMP
	if (!rwlock_is_locked(&tasklist_lock))
		BUG();
#endif
}

task_t * fastcall next_thread(task_t *p)
{
	struct pid_link *link = p->pids + PIDTYPE_TGID;
	struct list_head *tmp, *head = &link->pidptr->task_list;

#if CONFIG_SMP
	if (!p->sighand)
		BUG();
	check_tasklist_locked();
#endif
	tmp = link->pid_chain.next;
	if (tmp == head)
		tmp = head->next;

	return pid_task(tmp, PIDTYPE_TGID);
}

/*
 * Take down every thread in the group.  This is called by fatal signals
 * as well as by sys_exit_group (below).
 */
NORET_TYPE void
do_group_exit(int exit_code)
{
	BUG_ON(exit_code & 0x80); /* core dumps don't get here */

	if (current->signal->group_exit)
		exit_code = current->signal->group_exit_code;
	else if (!thread_group_empty(current)) {
		struct signal_struct *const sig = current->signal;
		struct sighand_struct *const sighand = current->sighand;
		read_lock(&tasklist_lock);
		spin_lock_irq(&sighand->siglock);
		if (sig->group_exit)
			/* Another thread got here before we took the lock.  */
			exit_code = sig->group_exit_code;
		else {
			sig->group_exit = 1;
			sig->group_exit_code = exit_code;
			zap_other_threads(current);
		}
		spin_unlock_irq(&sighand->siglock);
		read_unlock(&tasklist_lock);
	}

	do_exit(exit_code);
	/* NOTREACHED */
}

/*
 * this kills every thread in the thread group. Note that any externally
 * wait4()-ing process will get the correct exit code - even if this
 * thread is not the thread group leader.
 */
asmlinkage void sys_exit_group(int error_code)
{
	do_group_exit((error_code & 0xff) << 8);
}

static int eligible_child(pid_t pid, int options, task_t *p)
{
	if (pid > 0) {
		if (p->pid != pid)
			return 0;
	} else if (!pid) {
		if (p->pgrp != current->pgrp)
			return 0;
	} else if (pid != -1) {
		if (p->pgrp != -pid)
			return 0;
	}

	/*
	 * Do not consider detached threads that are
	 * not ptraced:
	 */
	if (p->exit_signal == -1 && !p->ptrace)
		return 0;

	/* Wait for all children (clone and not) if __WALL is set;
	 * otherwise, wait for clone children *only* if __WCLONE is
	 * set; otherwise, wait for non-clone children *only*.  (Note:
	 * A "clone" child here is one that reports to its parent
	 * using a signal other than SIGCHLD.) */
	if (((p->exit_signal != SIGCHLD) ^ ((options & __WCLONE) != 0))
	    && !(options & __WALL))
		return 0;
	/*
	 * Do not consider thread group leaders that are
	 * in a non-empty thread group:
	 */
	if (current->tgid != p->tgid && delay_group_leader(p))
		return 2;

	return 1;
}

/*
 * Handle sys_wait4 work for one task in state TASK_ZOMBIE.  We hold
 * read_lock(&tasklist_lock) on entry.  If we return zero, we still hold
 * the lock and this task is uninteresting.  If we return nonzero, we have
 * released the lock and the system call should return.
 */
static int wait_task_zombie(task_t *p, unsigned int *stat_addr, struct rusage *ru)
{
	unsigned long state;
	int retval;

	/*
	 * Try to move the task's state to DEAD
	 * only one thread is allowed to do this:
	 */
	state = xchg(&p->state, TASK_DEAD);
	if (state != TASK_ZOMBIE) {
		BUG_ON(state != TASK_DEAD);
		return 0;
	}
	if (unlikely(p->exit_signal == -1))
		/*
		 * This can only happen in a race with a ptraced thread
		 * dying on another processor.
		 */
		return 0;

	/*
	 * Now we are sure this task is interesting, and no other
	 * thread can reap it because we set its state to TASK_DEAD.
	 */
	read_unlock(&tasklist_lock);

	retval = ru ? getrusage(p, RUSAGE_BOTH, ru) : 0;
	if (!retval && stat_addr) {
		if (p->signal->group_exit)
			retval = put_user(p->signal->group_exit_code, stat_addr);
		else
			retval = put_user(p->exit_code, stat_addr);
	}
	if (retval) {
		p->state = TASK_ZOMBIE;
		return retval;
	}
	retval = p->pid;
	if (p->real_parent != p->parent) {
		write_lock_irq(&tasklist_lock);
		/* Double-check with lock held.  */
		if (p->real_parent != p->parent) {
			__ptrace_unlink(p);
			do_notify_parent(p, p->exit_signal);
			p->state = TASK_ZOMBIE;
			p = NULL;
		}
		write_unlock_irq(&tasklist_lock);
	}
	if (p != NULL)
		release_task(p);
	BUG_ON(!retval);
	return retval;
}

/*
 * Handle sys_wait4 work for one task in state TASK_STOPPED.  We hold
 * read_lock(&tasklist_lock) on entry.  If we return zero, we still hold
 * the lock and this task is uninteresting.  If we return nonzero, we have
 * released the lock and the system call should return.
 */
static int wait_task_stopped(task_t *p, int delayed_group_leader,
			     unsigned int *stat_addr, struct rusage *ru)
{
	int retval, exit_code;

	if (!p->exit_code)
		return 0;
	if (delayed_group_leader && !(p->ptrace & PT_PTRACED) &&
	    p->signal && p->signal->group_stop_count > 0)
		/*
		 * A group stop is in progress and this is the group leader.
		 * We won't report until all threads have stopped.
		 */
		return 0;

	/*
	 * Now we are pretty sure this task is interesting.
	 * Make sure it doesn't get reaped out from under us while we
	 * give up the lock and then examine it below.  We don't want to
	 * keep holding onto the tasklist_lock while we call getrusage and
	 * possibly take page faults for user memory.
	 */
	get_task_struct(p);
	read_unlock(&tasklist_lock);
	write_lock_irq(&tasklist_lock);

	/*
	 * This uses xchg to be atomic with the thread resuming and setting
	 * it.  It must also be done with the write lock held to prevent a
	 * race with the TASK_ZOMBIE case.
	 */
	exit_code = xchg(&p->exit_code, 0);
	if (unlikely(p->state > TASK_STOPPED)) {
		/*
		 * The task resumed and then died.  Let the next iteration
		 * catch it in TASK_ZOMBIE.  Note that exit_code might
		 * already be zero here if it resumed and did _exit(0).
		 * The task itself is dead and won't touch exit_code again;
		 * other processors in this function are locked out.
		 */
		p->exit_code = exit_code;
		exit_code = 0;
	}
	if (unlikely(exit_code == 0)) {
		/*
		 * Another thread in this function got to it first, or it
		 * resumed, or it resumed and then died.
		 */
		write_unlock_irq(&tasklist_lock);
		put_task_struct(p);
		read_lock(&tasklist_lock);
		return 0;
	}

	/* move to end of parent's list to avoid starvation */
	remove_parent(p);
	add_parent(p, p->parent);

	write_unlock_irq(&tasklist_lock);

	retval = ru ? getrusage(p, RUSAGE_BOTH, ru) : 0;
	if (!retval && stat_addr)
		retval = put_user((exit_code << 8) | 0x7f, stat_addr);
	if (!retval)
		retval = p->pid;
	put_task_struct(p);

	BUG_ON(!retval);
	return retval;
}

asmlinkage long sys_wait4(pid_t pid,unsigned int * stat_addr, int options, struct rusage * ru)
{
	DECLARE_WAITQUEUE(wait, current);
	struct task_struct *tsk;
	int flag, retval, workaround = 0;

	if (options & ~(WNOHANG|WUNTRACED|__WNOTHREAD|__WCLONE|__WALL))
		return -EINVAL;

	if (current->sighand->action[SIGCHLD-1].sa.sa_handler == SIG_IGN) {
		static unsigned long last_timestamp;

		// rate-limit it to 1 per minute:
		if (jiffies - last_timestamp > 60*HZ) {
			last_timestamp = jiffies;
			printk(KERN_INFO "application bug: %s(%d) has SIGCHLD set to SIG_IGN but calls wait().\n", current->comm, current->pid);
			printk(KERN_INFO "(see the NOTES section of 'man 2 wait'). Workaround activated.\n");
		}
		current->sighand->action[SIGCHLD-1].sa.sa_handler = SIG_DFL;
		workaround = 1;
	}
	add_wait_queue(&current->wait_chldexit,&wait);
repeat:
	flag = 0;
	current->state = TASK_INTERRUPTIBLE;
	read_lock(&tasklist_lock);
	tsk = current;
	do {
		struct task_struct *p;
		struct list_head *_p;
		int ret;

		list_for_each(_p,&tsk->children) {
			p = list_entry(_p,struct task_struct,sibling);

			ret = eligible_child(pid, options, p);
			if (!ret)
				continue;
			flag = 1;

			switch (p->state) {
			case TASK_STOPPED:
				if (!(options & WUNTRACED) &&
				    !(p->ptrace & PT_PTRACED))
					continue;
				retval = wait_task_stopped(p, ret == 2,
							   stat_addr, ru);
				if (retval != 0) /* He released the lock.  */
					goto end_wait4;
				break;
			case TASK_ZOMBIE:
				/*
				 * Eligible but we cannot release it yet:
				 */
				if (ret == 2)
					continue;
				retval = wait_task_zombie(p, stat_addr, ru);
				if (retval != 0) /* He released the lock.  */
					goto end_wait4;
				break;
			}
		}
		if (!flag) {
			list_for_each (_p,&tsk->ptrace_children) {
				p = list_entry(_p,struct task_struct,ptrace_list);
				if (!eligible_child(pid, options, p))
					continue;
				flag = 1;
				break;
			}
		}
		if (options & __WNOTHREAD)
			break;
		tsk = next_thread(tsk);
		if (tsk->signal != current->signal)
			BUG();
	} while (tsk != current);
	read_unlock(&tasklist_lock);
	if (flag) {
		retval = 0;
		if (options & WNOHANG)
			goto end_wait4;
		retval = -ERESTARTSYS;
		if (signal_pending(current))
			goto end_wait4;
		schedule();
		goto repeat;
	}
	retval = -ECHILD;
end_wait4:
	current->state = TASK_RUNNING;
	remove_wait_queue(&current->wait_chldexit,&wait);
	if (workaround)
		current->sighand->action[SIGCHLD-1].sa.sa_handler = SIG_IGN;
	return retval;
}

#if !defined(__alpha__) && !defined(__ia64__) && !defined(__arm__)

/*
 * sys_waitpid() remains for compatibility. waitpid() should be
 * implemented by calling sys_wait4() from libc.a.
 */
asmlinkage long sys_waitpid(pid_t pid,unsigned int * stat_addr, int options)
{
	return sys_wait4(pid, stat_addr, options, NULL);
}

#endif
