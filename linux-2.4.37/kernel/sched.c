/*
 *  kernel/sched.c
 *
 *  Kernel scheduler and related syscalls
 *
 *  Copyright (C) 1991-2002  Linus Torvalds
 *
 *  1996-12-23  Modified by Dave Grothe to fix bugs in semaphores and
 *		make semaphores SMP safe
 *  1998-11-19	Implemented schedule_timeout() and related stuff
 *		by Andrea Arcangeli
 *  1998-12-28  Implemented better SMP scheduling by Ingo Molnar
 *  2002-01-04	New ultra-scalable O(1) scheduler by Ingo Molnar:
 *		hybrid priority-list and round-robin design with
 *		an array-switch method of distributing timeslices
 *		and per-CPU runqueues.  Cleanups and useful suggestions
 *		by Davide Libenzi, preemptible kernel bits by Robert Love.
 */

#include <linux/mm.h>
#include <linux/nmi.h>
#include <linux/init.h>
#include <asm/uaccess.h>
#include <linux/highmem.h>
#include <linux/smp_lock.h>
#include <asm/mmu_context.h>
#include <linux/interrupt.h>
#include <linux/completion.h>
#include <linux/kernel_stat.h>
#include <linux/notifier.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/timer.h>

/*
 * Convert user-nice values [ -20 ... 0 ... 19 ]
 * to static priority [ MAX_RT_PRIO..MAX_PRIO-1 ],
 * and back.
 */
#define NICE_TO_PRIO(nice)	(MAX_RT_PRIO + (nice) + 20)
#define PRIO_TO_NICE(prio)	((prio) - MAX_RT_PRIO - 20)
#define TASK_NICE(p)		PRIO_TO_NICE((p)->static_prio)

/*
 * 'User priority' is the nice value converted to something we
 * can work with better when scaling various scheduler parameters,
 * it's a [ 0 ... 39 ] range.
 */
#define USER_PRIO(p)		((p)-MAX_RT_PRIO)
#define TASK_USER_PRIO(p)	USER_PRIO((p)->static_prio)
#define MAX_USER_PRIO		(USER_PRIO(MAX_PRIO))

/*
 * These are the 'tuning knobs' of the scheduler:
 *
 * Minimum timeslice is 10 msecs, default timeslice is 100 msecs,
 * maximum timeslice is 200 msecs. Timeslices get refilled after
 * they expire.
 */
#define MIN_TIMESLICE		( 10 * HZ / 1000)
#define MAX_TIMESLICE		(200 * HZ / 1000)
#define CHILD_PENALTY		50
#define PARENT_PENALTY		100
#define EXIT_WEIGHT		3
#define PRIO_BONUS_RATIO	25
#define INTERACTIVE_DELTA	2
#define MAX_SLEEP_AVG		(10*HZ)
#define STARVATION_LIMIT	(30*HZ)
#define SYNC_WAKEUPS		1
#define SMART_WAKE_CHILD	1

/*
 * If a task is 'interactive' then we reinsert it in the active
 * array after it has expired its current timeslice. (it will not
 * continue to run immediately, it will still roundrobin with
 * other interactive tasks.)
 *
 * This part scales the interactivity limit depending on niceness.
 *
 * We scale it linearly, offset by the INTERACTIVE_DELTA delta.
 * Here are a few examples of different nice levels:
 *
 *  TASK_INTERACTIVE(-20): [1,1,1,1,1,1,1,1,1,0,0]
 *  TASK_INTERACTIVE(-10): [1,1,1,1,1,1,1,0,0,0,0]
 *  TASK_INTERACTIVE(  0): [1,1,1,1,0,0,0,0,0,0,0]
 *  TASK_INTERACTIVE( 10): [1,1,0,0,0,0,0,0,0,0,0]
 *  TASK_INTERACTIVE( 19): [0,0,0,0,0,0,0,0,0,0,0]
 *
 * (the X axis represents the possible -5 ... 0 ... +5 dynamic
 *  priority range a task can explore, a value of '1' means the
 *  task is rated interactive.)
 *
 * Ie. nice +19 tasks can never get 'interactive' enough to be
 * reinserted into the active array. And only heavily CPU-hog nice -20
 * tasks will be expired. Default nice 0 tasks are somewhere between,
 * it takes some effort for them to get interactive, but it's not
 * too hard.
 */

#define SCALE(v1,v1_max,v2_max) \
	(v1) * (v2_max) / (v1_max)

#define DELTA(p) \
	(SCALE(TASK_NICE(p), 40, MAX_USER_PRIO*PRIO_BONUS_RATIO/100) + \
		INTERACTIVE_DELTA)

#define TASK_INTERACTIVE(p) \
	((p)->prio <= (p)->static_prio - DELTA(p))

/*
 * BASE_TIMESLICE scales user-nice values [ -20 ... 19 ]
 * to time slice values.
 *
 * The higher a thread's priority, the bigger timeslices
 * it gets during one round of execution. But even the lowest
 * priority thread gets MIN_TIMESLICE worth of execution time.
 *
 * task_timeslice() is the interface that is used by the scheduler.
 */
#define BASE_TIMESLICE(p) (MIN_TIMESLICE + \
	((MAX_TIMESLICE - MIN_TIMESLICE) * (MAX_PRIO-1-(p)->static_prio)/(MAX_USER_PRIO - 1)))

static inline unsigned int task_timeslice(task_t *p)
{
	return BASE_TIMESLICE(p);
}

/*
 * These are the runqueue data structures:
 */
 
#define BITMAP_SIZE ((((MAX_PRIO+1+7)/8)+sizeof(long)-1)/sizeof(long))

typedef struct runqueue runqueue_t;

struct prio_array {
	int nr_active;
	unsigned long bitmap[BITMAP_SIZE];
	struct list_head queue[MAX_PRIO];
};

/*
 * This is the main, per-CPU runqueue data structure.
 *
 * Locking rule: those places that want to lock multiple runqueues
 * (such as the load balancing or the thread migration code), lock
 * acquire operations must be ordered by ascending &runqueue.
 */
struct runqueue {
	spinlock_t lock;
	unsigned long nr_running, nr_switches, expired_timestamp,
			nr_uninterruptible;
	task_t *curr, *idle;
	struct mm_struct *prev_mm;
	prio_array_t *active, *expired, arrays[2];
	int prev_nr_running[NR_CPUS];

	task_t *migration_thread;
	struct list_head migration_queue;

	atomic_t nr_iowait;
} ____cacheline_aligned;

static struct runqueue runqueues[NR_CPUS] __cacheline_aligned;

#define cpu_rq(cpu)		(runqueues + (cpu))
#define this_rq()		cpu_rq(smp_processor_id())
#define task_rq(p)		cpu_rq(task_cpu(p))
#define cpu_curr(cpu)		(cpu_rq(cpu)->curr)
#define rt_task(p)		((p)->prio < MAX_RT_PRIO)

/*
 * Default context-switch locking:
 */
#ifndef prepare_arch_switch
# define prepare_arch_switch(rq, next)	do { } while(0)
# define finish_arch_switch(rq, next)	spin_unlock_irq(&(rq)->lock)
# define task_running(rq, p)		((rq)->curr == (p))
#endif

/*
 * task_rq_lock - lock the runqueue a given task resides on and disable
 * interrupts.  Note the ordering: we can safely lookup the task_rq without
 * explicitly disabling preemption.
 */
static inline runqueue_t *task_rq_lock(task_t *p, unsigned long *flags)
{
	struct runqueue *rq;
repeat_lock_task:
	local_irq_save(*flags);
	rq = task_rq(p);
	spin_lock(&rq->lock);
	if (unlikely(rq != task_rq(p))) {
		spin_unlock_irqrestore(&rq->lock, *flags);
		goto repeat_lock_task;
	}
	return rq;
}

static inline void task_rq_unlock(runqueue_t *rq, unsigned long *flags)
{
	spin_unlock_irqrestore(&rq->lock, *flags);
}

/*
 * rq_lock - lock a given runqueue and disable interrupts.
 */
static inline runqueue_t *this_rq_lock(void)
{
	runqueue_t *rq;
	local_irq_disable();
	rq = this_rq();
	spin_lock(&rq->lock);
	return rq;
}

static inline void rq_unlock(runqueue_t *rq)
{
	spin_unlock_irq(&rq->lock);
}

/*
 * Adding/removing a task to/from a priority array:
 */
static inline void dequeue_task(struct task_struct *p, prio_array_t *array)
{
	array->nr_active--;
	list_del(&p->run_list);
	if (list_empty(array->queue + p->prio))
		__clear_bit(p->prio, array->bitmap);
}

static inline void enqueue_task(struct task_struct *p, prio_array_t *array)
{
	list_add_tail(&p->run_list, array->queue + p->prio);
	__set_bit(p->prio, array->bitmap);
	array->nr_active++;
	p->array = array;
}

/*
 * effective_prio - return the priority that is based on the static
 * priority but is modified by bonuses/penalties.
 *
 * We scale the actual sleep average [0 .... MAX_SLEEP_AVG]
 * into the -5 ... 0 ... +5 bonus/penalty range.
 *
 * We use 25% of the full 0...39 priority range so that:
 *
 * 1) nice +19 interactive tasks do not preempt nice 0 CPU hogs.
 * 2) nice -20 CPU hogs do not get preempted by nice 0 tasks.
 *
 * Both properties are important to certain workloads.
 */
static inline int effective_prio(task_t *p)
{
	int bonus, prio;

	bonus = MAX_USER_PRIO*PRIO_BONUS_RATIO*p->sleep_avg/MAX_SLEEP_AVG/100 -
			MAX_USER_PRIO*PRIO_BONUS_RATIO/100/2;

	prio = p->static_prio - bonus;
	if (prio < MAX_RT_PRIO)
		prio = MAX_RT_PRIO;
	if (prio > MAX_PRIO-1)
		prio = MAX_PRIO-1;
	return prio;
}

/*
 * activate_task - move a task to the runqueue.

 * Also update all the scheduling statistics stuff. (sleep average
 * calculation, priority modifiers, etc.)
 */
static inline void __activate_task(task_t *p, runqueue_t *rq)
{
	enqueue_task(p, rq->active);
	rq->nr_running++;
}

static inline void activate_task(task_t *p, runqueue_t *rq)
{
	unsigned long sleep_time = jiffies - p->last_run;
	if (!rt_task(p) && sleep_time)
	{
		/*
		 * This code gives a bonus to interactive tasks. We update
		 * an 'average sleep time' value here, based on
		 * ->last_run. The more time a task spends sleeping,
		 * the higher the average gets - and the higher the priority
		 * boost gets as well.
		 */
		p->sleep_avg += sleep_time;
		if (p->sleep_avg > MAX_SLEEP_AVG)
			p->sleep_avg = MAX_SLEEP_AVG;
		p->prio = effective_prio(p);
	}
	__activate_task(p, rq);
}

/*
 * deactivate_task - remove a task from the runqueue.
 */
static inline void deactivate_task(struct task_struct *p, runqueue_t *rq)
{
	rq->nr_running--;
	if (p->state == TASK_UNINTERRUPTIBLE)
		rq->nr_uninterruptible++;
	dequeue_task(p, p->array);
	p->array = NULL;
}

/*
 * resched_task - mark a task 'to be rescheduled now'.
 *
 * On UP this means the setting of the need_resched flag, on SMP it
 * might also involve a cross-CPU call to trigger the scheduler on
 * the target CPU.
 */
static inline void resched_task(task_t *p)
{
#ifdef CONFIG_SMP
	int need_resched;
	need_resched = p->need_resched;
	wmb();
	set_tsk_need_resched(p);
	if (!need_resched && (task_cpu(p) != smp_processor_id()))
		smp_send_reschedule(task_cpu(p));
#else
	set_tsk_need_resched(p);
#endif
}

#ifdef CONFIG_SMP
/*
 * wait_task_inactive - wait for a thread to unschedule.
 *
 * The caller must ensure that the task *will* unschedule sometime soon,
 * else this function might spin for a *long* time.
 */
void wait_task_inactive(task_t * p)
{
	unsigned long flags;
	runqueue_t *rq;

repeat:
	rq = task_rq(p);
	if (unlikely(task_running(rq, p))) {
		cpu_relax();
 		/*
		 * enable/disable preemption just to make this
		 * a preemption point - we are busy-waiting
		 * anyway.
 		 */
		goto repeat;
	}
	rq = task_rq_lock(p, &flags);
	if (unlikely(task_running(rq, p))) {
		task_rq_unlock(rq, &flags);
		goto repeat;
	}
	task_rq_unlock(rq, &flags);
}

/*
 * kick_if_running - kick the remote CPU if the task is running currently.
 *
 * This code is used by the signal code to signal tasks
 * which are in user-mode, as quickly as possible.
 *
 * (Note that we do this lockless - if the task does anything
 * while the message is in flight then it will notice the
 * sigpending condition anyway.)
 */
void kick_if_running(task_t * p)
{
	if ((task_running(task_rq(p), p)) && (task_cpu(p) != smp_processor_id()))
		resched_task(p);
}
#endif

/*
 * try_to_wake_up - wake up a thread
 * @p: the to-be-woken-up thread
 * @state: the mask of task states that can be woken
 * @sync: do a synchronous wakeup?
 *
 * Put it on the run-queue if it's not already there. The "current"
 * thread is always on the run-queue (except when the actual
 * re-schedule is in progress), and as such you're allowed to do
 * the simpler "current->state = TASK_RUNNING" to mark yourself
 * runnable without the overhead of this.
 *
 * returns failure only if the task is already active.
 */
static int try_to_wake_up(task_t * p, unsigned int state, int sync)
{
	unsigned long flags;
	int success = 0;
	long old_state;
	runqueue_t *rq;

	sync &= SYNC_WAKEUPS;
repeat_lock_task:
	rq = task_rq_lock(p, &flags);
	old_state = p->state;
	if (old_state & state) {
		if (!p->array) {
			/*
			 * Fast-migrate the task if it's not running or runnable
			 * currently. Do not violate hard affinity.
			 */
			if (unlikely(sync && !task_running(rq, p) &&
				(task_cpu(p) != smp_processor_id()) &&
				(p->cpus_allowed & (1UL << smp_processor_id())))) {

				set_task_cpu(p, smp_processor_id());
				task_rq_unlock(rq, &flags);
				goto repeat_lock_task;
			}
			if (old_state == TASK_UNINTERRUPTIBLE)
				rq->nr_uninterruptible--;
			if (sync)
				__activate_task(p, rq);
			else {
				activate_task(p, rq);
				if (p->prio < rq->curr->prio)
					resched_task(rq->curr);
			}
			success = 1;
		}
		if (p->state >= TASK_ZOMBIE)
			BUG();
		p->state = TASK_RUNNING;
	}
	task_rq_unlock(rq, &flags);

	return success;
}

int fastcall wake_up_process(task_t * p)
{
	return try_to_wake_up(p, TASK_STOPPED | TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE, 0);
}

int fastcall wake_up_state(task_t *p, unsigned int state)
{
	return try_to_wake_up(p, state, 0);
}

/*
 * wake_up_forked_process - wake up a freshly forked process.
 *
 * This function will do some initial scheduler statistics housekeeping
 * that must be done for every newly created process.
 */
void fastcall wake_up_forked_process(task_t * p)
{
	runqueue_t *rq = this_rq_lock();

	p->state = TASK_RUNNING;
	if (!rt_task(p)) {
		/*
		 * We decrease the sleep average of forking parents
		 * and children as well, to keep max-interactive tasks
		 * from forking tasks that are max-interactive.
		 */
		current->sleep_avg = current->sleep_avg * PARENT_PENALTY / 100;
		p->sleep_avg = p->sleep_avg * CHILD_PENALTY / 100;
		p->prio = effective_prio(p);
	}
	set_task_cpu(p, smp_processor_id());

	if (SMART_WAKE_CHILD) {
		if (unlikely(!current->array))
			__activate_task(p, rq);
		else {
			p->prio = current->prio;
			list_add_tail(&p->run_list, &current->run_list);
			p->array = current->array;
			p->array->nr_active++;
			rq->nr_running++;
		}
	} else
		activate_task(p, rq);
	rq_unlock(rq);
}

/*
 * Potentially available exiting-child timeslices are
 * retrieved here - this way the parent does not get
 * penalized for creating too many threads.
 *
 * (this cannot be used to 'generate' timeslices
 * artificially, because any timeslice recovered here
 * was given away by the parent in the first place.)
 */
void fastcall sched_exit(task_t * p)
{
	unsigned long flags;

	local_irq_save(flags);
	if (p->first_time_slice) {
		p->parent->time_slice += p->time_slice;
		if (unlikely(p->parent->time_slice > MAX_TIMESLICE))
			p->parent->time_slice = MAX_TIMESLICE;
	}
	local_irq_restore(flags);
	/*
	 * If the child was a (relative-) CPU hog then decrease
	 * the sleep_avg of the parent as well.
	 */
	if (p->sleep_avg < p->parent->sleep_avg)
		p->parent->sleep_avg = (p->parent->sleep_avg * EXIT_WEIGHT +
			p->sleep_avg) / (EXIT_WEIGHT + 1);
}

/**
 * schedule_tail - first thing a freshly forked thread must call.
 * @prev: the thread we just switched away from.
 */
asmlinkage void schedule_tail(task_t *prev)
{
	finish_arch_switch(this_rq(), prev);
	if (current->set_child_tid)
		put_user(current->pid, current->set_child_tid);
}

/*
 * context_switch - switch to the new MM and the new
 * thread's register state.
 */
static inline task_t * context_switch(runqueue_t *rq, task_t *prev, task_t *next)
{
	struct mm_struct *mm = next->mm;
	struct mm_struct *oldmm = prev->active_mm;

	if (unlikely(!mm)) {
		next->active_mm = oldmm;
		atomic_inc(&oldmm->mm_count);
		enter_lazy_tlb(oldmm, next, smp_processor_id());
	} else
		switch_mm(oldmm, mm, next, smp_processor_id());

	if (unlikely(!prev->mm)) {
		prev->active_mm = NULL;
		rq->prev_mm = oldmm;
	}

	/* Here we just switch the register state and the stack. */
	switch_to(prev, next, prev);

	return prev;
}

/*
 * nr_running, nr_uninterruptible and nr_context_switches:
 *
 * externally visible scheduler statistics: current number of runnable
 * threads, current number of uninterruptible-sleeping threads, total
 * number of context switches performed since bootup.
 */
unsigned long nr_running(void)
{
	unsigned long i, sum = 0;

	for (i = 0; i < NR_CPUS; i++)
		sum += cpu_rq(i)->nr_running;

	return sum;
}

unsigned long nr_uninterruptible(void)
{
	unsigned long i, sum = 0;

	for (i = 0; i < NR_CPUS; i++) {
		if (!cpu_online(i))
			continue;
		sum += cpu_rq(i)->nr_uninterruptible;
	}
	return sum;
}

unsigned long nr_context_switches(void)
{
	unsigned long i, sum = 0;

	for (i = 0; i < NR_CPUS; i++) {
		if (!cpu_online(i))
			continue;
		sum += cpu_rq(i)->nr_switches;
	}
	return sum;
}

unsigned long nr_iowait(void)
{
	unsigned long i, sum = 0;

	for (i = 0; i < NR_CPUS; ++i) {
		if (!cpu_online(i))
			continue;
		sum += atomic_read(&cpu_rq(i)->nr_iowait);
	}
	return sum;
}

/*
 * double_rq_lock - safely lock two runqueues
 *
 * Note this does not disable interrupts like task_rq_lock,
 * you need to do so manually before calling.
 */
static inline void double_rq_lock(runqueue_t *rq1, runqueue_t *rq2)
{
	if (rq1 == rq2)
		spin_lock(&rq1->lock);
	else {
		if (rq1 < rq2) {
			spin_lock(&rq1->lock);
			spin_lock(&rq2->lock);
		} else {
			spin_lock(&rq2->lock);
			spin_lock(&rq1->lock);
		}
	}
}

/*
 * double_rq_unlock - safely unlock two runqueues
 *
 * Note this does not restore interrupts like task_rq_unlock,
 * you need to do so manually after calling.
 */
static inline void double_rq_unlock(runqueue_t *rq1, runqueue_t *rq2)
{
	spin_unlock(&rq1->lock);
	if (rq1 != rq2)
		spin_unlock(&rq2->lock);
}

#if CONFIG_SMP

/*
 * double_lock_balance - lock the busiest runqueue
 *
 * this_rq is locked already. Recalculate nr_running if we have to
 * drop the runqueue lock.
 */
static inline unsigned int double_lock_balance(runqueue_t *this_rq,
	runqueue_t *busiest, int this_cpu, int idle, unsigned int nr_running)
{
	if (unlikely(!spin_trylock(&busiest->lock))) {
		if (busiest < this_rq) {
			spin_unlock(&this_rq->lock);
			spin_lock(&busiest->lock);
			spin_lock(&this_rq->lock);
			/* Need to recalculate nr_running */
			if (idle || (this_rq->nr_running > this_rq->prev_nr_running[this_cpu]))
				nr_running = this_rq->nr_running;
			else
				nr_running = this_rq->prev_nr_running[this_cpu];
		} else
			spin_lock(&busiest->lock);
 	}
	return nr_running;
}

/*
 * find_busiest_queue - find the busiest runqueue.
 */
static inline runqueue_t *find_busiest_queue(runqueue_t *this_rq, int this_cpu, int idle, int *imbalance)
{
	int nr_running, load, max_load, i;
	runqueue_t *busiest, *rq_src;

	/*
	 * We search all runqueues to find the most busy one.
	 * We do this lockless to reduce cache-bouncing overhead,
	 * we re-check the 'best' source CPU later on again, with
	 * the lock held.
	 *
	 * We fend off statistical fluctuations in runqueue lengths by
	 * saving the runqueue length during the previous load-balancing
	 * operation and using the smaller one the current and saved lengths.
	 * If a runqueue is long enough for a longer amount of time then
	 * we recognize it and pull tasks from it.
	 *
	 * The 'current runqueue length' is a statistical maximum variable,
	 * for that one we take the longer one - to avoid fluctuations in
	 * the other direction. So for a load-balance to happen it needs
	 * stable long runqueue on the target CPU and stable short runqueue
	 * on the local runqueue.
	 *
	 * We make an exception if this CPU is about to become idle - in
	 * that case we are less picky about moving a task across CPUs and
	 * take what can be taken.
	 */
	if (idle || (this_rq->nr_running > this_rq->prev_nr_running[this_cpu]))
		nr_running = this_rq->nr_running;
	else
		nr_running = this_rq->prev_nr_running[this_cpu];

	busiest = NULL;
	max_load = 1;
	for (i = 0; i < NR_CPUS; i++) {
		if (!cpu_online(i))
			continue;

		rq_src = cpu_rq(i);
		if (idle || (rq_src->nr_running < this_rq->prev_nr_running[i]))
			load = rq_src->nr_running;
		else
			load = this_rq->prev_nr_running[i];
		this_rq->prev_nr_running[i] = rq_src->nr_running;

		if ((load > max_load) && (rq_src != this_rq)) {
			busiest = rq_src;
			max_load = load;
 		}
	}

	if (likely(!busiest))
		goto out;

	//*imbalance = (max_load - nr_running) / 2;
	*imbalance = max_load - nr_running;

	/* It needs an at least ~25% imbalance to trigger balancing. */
	//if (!idle && (*imbalance < (max_load + 3)/4)) {
	if (!idle && ((*imbalance)*4 < max_load)) {
		busiest = NULL;
		goto out;
 	}

	nr_running = double_lock_balance(this_rq, busiest, this_cpu, idle, nr_running);
	/*
	 * Make sure nothing changed since we checked the
	 * runqueue length.
	 */
	//if (busiest->nr_running <= nr_running + 1) {
	if (busiest->nr_running <= nr_running) {
		spin_unlock(&busiest->lock);
		busiest = NULL;
	}
	/*
	 * We only want to steal a number of tasks equal to 1/2 the imbalance,
	 * otherwise we'll just shift the imbalance to the new queue:
	 */
	*imbalance /= 2;
out:
	return busiest;
}

/*
 * pull_task - move a task from a remote runqueue to the local runqueue.
 * Both runqueues must be locked.
 */
static inline void pull_task(runqueue_t *src_rq, prio_array_t *src_array, task_t *p, runqueue_t *this_rq, int this_cpu)
{
	dequeue_task(p, src_array);
	src_rq->nr_running--;
	set_task_cpu(p, this_cpu);
	this_rq->nr_running++;
	enqueue_task(p, this_rq->active);
	/*
	 * Note that idle threads have a prio of MAX_PRIO, for this test
	 * to be always true for them.
	 */
	if (p->prio < this_rq->curr->prio)
		set_need_resched();
	else {
		if (p->prio == this_rq->curr->prio &&
				p->time_slice > this_rq->curr->time_slice)
			set_need_resched();
 	}
}

/*
 * Current runqueue is empty, or rebalance tick: if there is an
 * inbalance (current runqueue is too short) then pull from
 * busiest runqueue(s).
 *
 * We call this with the current runqueue locked,
 * irqs disabled.
 */
static void load_balance(runqueue_t *this_rq, int idle)
{
	int imbalance, idx, this_cpu = smp_processor_id();
	runqueue_t *busiest;
	prio_array_t *array;
	struct list_head *head, *curr;
	task_t *tmp;

	busiest = find_busiest_queue(this_rq, this_cpu, idle, &imbalance);
	if (!busiest)
		goto out;

	/*
	 * We first consider expired tasks. Those will likely not be
	 * executed in the near future, and they are most likely to
	 * be cache-cold, thus switching CPUs has the least effect
	 * on them.
	 */
	if (busiest->expired->nr_active)
		array = busiest->expired;
	else
		array = busiest->active;

new_array:
	/* Start searching at priority 0: */
	idx = 0;
skip_bitmap:
	if (!idx)
		idx = sched_find_first_bit(array->bitmap);
	else
		idx = find_next_bit(array->bitmap, MAX_PRIO, idx);
	if (idx >= MAX_PRIO) {
		if (array == busiest->expired) {
			array = busiest->active;
			goto new_array;
		}
		goto out_unlock;
 	}

	head = array->queue + idx;
	curr = head->prev;
skip_queue:
	tmp = list_entry(curr, task_t, run_list);

	/*
	 * We do not migrate tasks that are:
	 * 1) running (obviously), or
	 * 2) cannot be migrated to this CPU due to cpus_allowed, or
	 * 3) are cache-hot on their current CPU.
	 */

#define CAN_MIGRATE_TASK(p,rq,this_cpu)					\
	((jiffies - (p)->last_run > cache_decay_ticks) &&	\
		!task_running(rq, p) &&					\
			((p)->cpus_allowed & (1UL << (this_cpu))))

	curr = curr->prev;

	if (!CAN_MIGRATE_TASK(tmp, busiest, this_cpu)) {
		if (curr != head)
			goto skip_queue;
		idx++;
		goto skip_bitmap;
	}
	pull_task(busiest, array, tmp, this_rq, this_cpu);
	if (!idle && --imbalance) {
		if (curr != head)
			goto skip_queue;
		idx++;
		goto skip_bitmap;
	}
out_unlock:
	spin_unlock(&busiest->lock);
out:
	;
}
 
/*
 * One of the idle_cpu_tick() and busy_cpu_tick() functions will
 * get called every timer tick, on every CPU. Our balancing action
 * frequency and balancing agressivity depends on whether the CPU is
 * idle or not.
 *
 * busy-rebalance every 250 msecs. idle-rebalance every 1 msec. (or on
 * systems with HZ=100, every 10 msecs.)
 */
#define BUSY_REBALANCE_TICK (HZ/4 ?: 1)
#define IDLE_REBALANCE_TICK (HZ/1000 ?: 1)

static inline void idle_tick(runqueue_t *rq)
{
	if (jiffies % IDLE_REBALANCE_TICK)
		return;
	spin_lock(&rq->lock);
	load_balance(rq, 1);
	spin_unlock(&rq->lock);
}

#endif

/*
 * We place interactive tasks back into the active array, if possible.
 *
 * To guarantee that this does not starve expired tasks we ignore the
 * interactivity of a task if the first expired task had to wait more
 * than a 'reasonable' amount of time. This deadline timeout is
 * load-dependent, as the frequency of array switched decreases with
 * increasing number of running tasks:
 */
#define EXPIRED_STARVING(rq) \
		(STARVATION_LIMIT && ((rq)->expired_timestamp && \
		(jiffies - (rq)->expired_timestamp >= \
			STARVATION_LIMIT * ((rq)->nr_running) + 1)))

/*
 * This function gets called by the timer code, with HZ frequency.
 * We call it with interrupts disabled.
 *
 * It also gets called by the fork code, when changing the parent's
 * timeslices.
 */
void scheduler_tick(int user_ticks, int sys_ticks)
{
	int cpu = smp_processor_id();
	runqueue_t *rq = this_rq();
	task_t *p = current;

	if (p == rq->idle) {
		if (local_bh_count(cpu) || local_irq_count(cpu) > 1)
			kstat.per_cpu_system[cpu] += sys_ticks;
#if CONFIG_SMP
		idle_tick(rq);
#endif
		return;
	}
	if (TASK_NICE(p) > 0)
		kstat.per_cpu_nice[cpu] += user_ticks;
	else
		kstat.per_cpu_user[cpu] += user_ticks;
	kstat.per_cpu_system[cpu] += sys_ticks;

	/* Task might have expired already, but not scheduled off yet */
	if (p->array != rq->active) {
		set_tsk_need_resched(p);
		return;
	}
	spin_lock(&rq->lock);
	if (unlikely(rt_task(p))) {
		/*
		 * RR tasks need a special form of timeslice management.
		 * FIFO tasks have no timeslices.
		 */
		if ((p->policy == SCHED_RR) && !--p->time_slice) {
			p->time_slice = task_timeslice(p);
			p->first_time_slice = 0;
			set_tsk_need_resched(p);

			/* put it at the end of the queue: */
			dequeue_task(p, rq->active);
			enqueue_task(p, rq->active);
		}
		goto out;
	}

	/*
	 * The task was running during this tick - update the
	 * time slice counter and the sleep average. Note: we
	 * do not update a thread's priority until it either
	 * goes to sleep or uses up its timeslice. This makes
	 * it possible for interactive tasks to use up their
	 * timeslices at their highest priority levels.
	 */
	if (p->sleep_avg)
		p->sleep_avg--;
	if (!--p->time_slice) {
		dequeue_task(p, rq->active);
		set_tsk_need_resched(p);
		p->prio = effective_prio(p);
		p->time_slice = task_timeslice(p);
		p->first_time_slice = 0;

		if (!TASK_INTERACTIVE(p) || EXPIRED_STARVING(rq)) {
			if (!rq->expired_timestamp)
				rq->expired_timestamp = jiffies;
			enqueue_task(p, rq->expired);
		} else
			enqueue_task(p, rq->active);
	}
out:
#if CONFIG_SMP
	if (!(jiffies % BUSY_REBALANCE_TICK))
		load_balance(rq, 0);
#endif
	spin_unlock(&rq->lock);
}

void scheduling_functions_start_here(void) { }

/*
 * schedule() is the main scheduler function.
 */
asmlinkage void schedule(void)
{
	task_t *prev, *next;
	runqueue_t *rq;
	prio_array_t *array;
	struct list_head *queue;
	int idx;

	BUG_ON(in_interrupt());

need_resched:
	prev = current;
	rq = this_rq();

	release_kernel_lock(prev, smp_processor_id());
	/*
	 * Ok, we are leaving the CPU now, lets update the 'last run'
	 * timestamp:
	 */
	prev->last_run = jiffies;
	spin_lock_irq(&rq->lock);

	switch (prev->state) {
	case TASK_INTERRUPTIBLE:
		if (unlikely(signal_pending(prev))) {
			prev->state = TASK_RUNNING;
			break;
		}
	default:
		deactivate_task(prev, rq);
	case TASK_RUNNING:
		;
	}
#if CONFIG_SMP
pick_next_task:
#endif
	if (unlikely(!rq->nr_running)) {
#if CONFIG_SMP
		load_balance(rq, 1);
		if (rq->nr_running)
			goto pick_next_task;
#endif
		next = rq->idle;
		rq->expired_timestamp = 0;
		goto switch_tasks;
	}

	array = rq->active;
	if (unlikely(!array->nr_active)) {
		/*
		 * Switch the active and expired arrays.
		 */
		rq->active = rq->expired;
		rq->expired = array;
		array = rq->active;
		rq->expired_timestamp = 0;
	}

	idx = sched_find_first_bit(array->bitmap);
	queue = array->queue + idx;
	next = list_entry(queue->next, task_t, run_list);

switch_tasks:
	prefetch(next);
	clear_tsk_need_resched(prev);

	if (likely(prev != next)) {
		struct mm_struct *prev_mm;
		rq->nr_switches++;
		rq->curr = next;
	
		prepare_arch_switch(rq, next);
		prev = context_switch(rq, prev, next);
		barrier();
		rq = this_rq();
		prev_mm = rq->prev_mm;
		rq->prev_mm = NULL;
		finish_arch_switch(rq, prev);
		if (prev_mm)
			mmdrop(prev_mm);
	} else
		spin_unlock_irq(&rq->lock);

	reacquire_kernel_lock(current);
	if (need_resched())
		goto need_resched;
}

/*
 * The core wakeup function.  Non-exclusive wakeups (nr_exclusive == 0) just
 * wake everything up.  If it's an exclusive wakeup (nr_exclusive == small +ve
 * number) then we wake all the non-exclusive tasks and one exclusive task.
 *
 * There are circumstances in which we can try to wake a task which has already
 * started to run but is not in state TASK_RUNNING.  try_to_wake_up() returns
 * zero in this (rare) case, and we handle it by continuing to scan the queue.
 */
static inline void __wake_up_common(wait_queue_head_t *q, unsigned int mode, int nr_exclusive, int sync)
{
	struct list_head *tmp;

	unsigned int state;
	wait_queue_t *curr;
	task_t *p;

	list_for_each(tmp, &q->task_list) {
		curr = list_entry(tmp, wait_queue_t, task_list);
		p = curr->task;
		state = p->state;
		if ((state & mode) && try_to_wake_up(p, mode, sync) &&
			((curr->flags & WQ_FLAG_EXCLUSIVE) && !--nr_exclusive))
				break;
	}
}

/**
 * __wake_up - wake up threads blocked on a waitqueue.
 * @q: the waitqueue
 * @mode: which threads
 * @nr_exclusive: how many wake-one or wake-many threads to wake up
 */
void fastcall __wake_up(wait_queue_head_t *q, unsigned int mode, int nr_exclusive)
{
	unsigned long flags;

	if (unlikely(!q))
		return;

	spin_lock_irqsave(&q->lock, flags);
	__wake_up_common(q, mode, nr_exclusive, 0);
	spin_unlock_irqrestore(&q->lock, flags);
}

/*
 * Same as __wake_up but called with the spinlock in wait_queue_head_t held.
 */
void __wake_up_locked(wait_queue_head_t *q, unsigned int mode)
{
	__wake_up_common(q, mode, 1, 0);
}

#if CONFIG_SMP

/**
 * __wake_up - sync- wake up threads blocked on a waitqueue.
 * @q: the waitqueue
 * @mode: which threads
 * @nr_exclusive: how many wake-one or wake-many threads to wake up
 *
 * The sync wakeup differs that the waker knows that it will schedule
 * away soon, so while the target thread will be woken up, it will not
 * be migrated to another CPU - ie. the two threads are 'synchronized'
 * with each other. This can prevent needless bouncing between CPUs.
 */
void fastcall __wake_up_sync(wait_queue_head_t *q, unsigned int mode, int nr_exclusive)
{
	unsigned long flags;

	if (unlikely(!q))
		return;

	spin_lock_irqsave(&q->lock, flags);
	if (likely(nr_exclusive))
		__wake_up_common(q, mode, nr_exclusive, 1);
	else
		__wake_up_common(q, mode, nr_exclusive, 0);
	spin_unlock_irqrestore(&q->lock, flags);
}

#endif

void fastcall complete(struct completion *x)
{
	unsigned long flags;

	spin_lock_irqsave(&x->wait.lock, flags);
	x->done++;
	__wake_up_common(&x->wait, TASK_UNINTERRUPTIBLE | TASK_INTERRUPTIBLE, 1, 0);
	spin_unlock_irqrestore(&x->wait.lock, flags);
}

void fastcall complete_all(struct completion *x)
{
	unsigned long flags;

	spin_lock_irqsave(&x->wait.lock, flags);
	x->done += UINT_MAX/2;
	__wake_up_common(&x->wait, TASK_UNINTERRUPTIBLE | TASK_INTERRUPTIBLE, 0, 0);
	spin_unlock_irqrestore(&x->wait.lock, flags);
}

void fastcall wait_for_completion(struct completion *x)
{
	spin_lock_irq(&x->wait.lock);
	if (!x->done) {
		DECLARE_WAITQUEUE(wait, current);

		wait.flags |= WQ_FLAG_EXCLUSIVE;
		__add_wait_queue_tail(&x->wait, &wait);
		do {
			__set_current_state(TASK_UNINTERRUPTIBLE);
			spin_unlock_irq(&x->wait.lock);
			schedule();
			spin_lock_irq(&x->wait.lock);
		} while (!x->done);
		__remove_wait_queue(&x->wait, &wait);
	}
	x->done--;
	spin_unlock_irq(&x->wait.lock);
}

#define	SLEEP_ON_VAR				\
	unsigned long flags;			\
	wait_queue_t wait;			\
	init_waitqueue_entry(&wait, current);

#define SLEEP_ON_HEAD					\
	spin_lock_irqsave(&q->lock,flags);		\
	__add_wait_queue(q, &wait);			\
	spin_unlock(&q->lock);

#define	SLEEP_ON_TAIL						\
	spin_lock_irq(&q->lock);				\
	__remove_wait_queue(q, &wait);				\
	spin_unlock_irqrestore(&q->lock, flags);

void fastcall interruptible_sleep_on(wait_queue_head_t *q)
{
	SLEEP_ON_VAR

	current->state = TASK_INTERRUPTIBLE;

	SLEEP_ON_HEAD
	schedule();
	SLEEP_ON_TAIL
}

long fastcall interruptible_sleep_on_timeout(wait_queue_head_t *q, long timeout)
{
	SLEEP_ON_VAR

	current->state = TASK_INTERRUPTIBLE;

	SLEEP_ON_HEAD
	timeout = schedule_timeout(timeout);
	SLEEP_ON_TAIL

	return timeout;
}

void fastcall sleep_on(wait_queue_head_t *q)
{
	SLEEP_ON_VAR
	
	current->state = TASK_UNINTERRUPTIBLE;

	SLEEP_ON_HEAD
	schedule();
	SLEEP_ON_TAIL
}

long fastcall sleep_on_timeout(wait_queue_head_t *q, long timeout)
{
	SLEEP_ON_VAR
	
	current->state = TASK_UNINTERRUPTIBLE;

	SLEEP_ON_HEAD
	timeout = schedule_timeout(timeout);
	SLEEP_ON_TAIL

	return timeout;
}

void scheduling_functions_end_here(void) { }

void set_user_nice(task_t *p, long nice)
{
	unsigned long flags;
	prio_array_t *array;
	runqueue_t *rq;

	if (TASK_NICE(p) == nice || nice < -20 || nice > 19)
		return;
	/*
	 * We have to be careful, if called from sys_setpriority(),
	 * the task might be in the middle of scheduling on another CPU.
	 */
	rq = task_rq_lock(p, &flags);
	if (rt_task(p)) {
		p->static_prio = NICE_TO_PRIO(nice);
		goto out_unlock;
	}
	array = p->array;
	if (array)
		dequeue_task(p, array);
	p->static_prio = NICE_TO_PRIO(nice);
	p->prio = NICE_TO_PRIO(nice);
	if (array) {
		enqueue_task(p, array);
		/*
		 * If the task is running and lowered its priority,
		 * or increased its priority then reschedule its CPU:
		 */
		if ((NICE_TO_PRIO(nice) < p->static_prio) ||
							task_running(rq, p))
			resched_task(rq->curr);
	}
out_unlock:
	task_rq_unlock(rq, &flags);
}

#ifndef __alpha__

/*
 * sys_nice - change the priority of the current process.
 * @increment: priority increment
 *
 * sys_setpriority is a more generic, but much slower function that
 * does similar things.
 */

asmlinkage long sys_nice(int increment)
{
	long nice;

	/*
	 *	Setpriority might change our priority at the same moment.
	 *	We don't have to worry. Conceptually one call occurs first
	 *	and we have a single winner.
	 */
	if (increment < 0) {
		if (!capable(CAP_SYS_NICE))
			return -EPERM;
		if (increment < -40)
			increment = -40;
	}
	if (increment > 40)
		increment = 40;

	nice = PRIO_TO_NICE(current->static_prio) + increment;
	if (nice < -20)
		nice = -20;
	if (nice > 19)
		nice = 19;
	set_user_nice(current, nice);
	return 0;
}

#endif

/**
 * task_prio - return the priority value of a given task.
 * @p: the task in question.
 *
 * This is the priority value as seen by users in /proc.
 * RT tasks are offset by -200. Normal tasks are centered
 * around 0, value goes from -16 to +15.
 */
int task_prio(task_t *p)
{
	return p->prio - MAX_USER_RT_PRIO;
}

/**
 * task_nice - return the nice value of a given task.
 * @p: the task in question.
 */
int task_nice(task_t *p)
{
	return TASK_NICE(p);
}

/**
 * task_curr - is this task currently executing on a CPU?
 * @p: the task in question.
 */
int task_curr(task_t *p)
{
	return cpu_curr(task_cpu(p)) == p;
}

/**
 * idle_cpu - is a given cpu idle currently?
 * @cpu: the processor in question.
 */
int idle_cpu(int cpu)
{
	return cpu_curr(cpu) == cpu_rq(cpu)->idle;
}

/**
 * find_process_by_pid - find a process with a matching PID value.
 * @pid: the pid in question.
 */
static inline task_t *find_process_by_pid(pid_t pid)
{
	return pid ? find_task_by_pid(pid) : current;
}

/*
 * setscheduler - change the scheduling policy and/or RT priority of a thread.
 */
static int setscheduler(pid_t pid, int policy, struct sched_param *param)
{
	struct sched_param lp;
	int retval = -EINVAL;
	prio_array_t *array;
	unsigned long flags;
	runqueue_t *rq;
	task_t *p;

	if (!param || pid < 0)
		goto out_nounlock;

	retval = -EFAULT;
	if (copy_from_user(&lp, param, sizeof(struct sched_param)))
		goto out_nounlock;

	/*
	 * We play safe to avoid deadlocks.
	 */
	read_lock_irq(&tasklist_lock);

	p = find_process_by_pid(pid);

	retval = -ESRCH;
	if (!p)
		goto out_unlock_tasklist;

	/*
	 * To be able to change p->policy safely, the apropriate
	 * runqueue lock must be held.
	 */
	rq = task_rq_lock(p, &flags);

	if (policy < 0)
		policy = p->policy;
	else {
		retval = -EINVAL;
		if (policy != SCHED_FIFO && policy != SCHED_RR &&
				policy != SCHED_NORMAL)
			goto out_unlock;
	}

	/*
	 * Valid priorities for SCHED_FIFO and SCHED_RR are
	 * 1..MAX_USER_RT_PRIO-1, valid priority for SCHED_NORMAL is 0.
	 */
	retval = -EINVAL;
	if (lp.sched_priority < 0 || lp.sched_priority > MAX_USER_RT_PRIO-1)
		goto out_unlock;
	if ((policy == SCHED_NORMAL) != (lp.sched_priority == 0))
		goto out_unlock;

	retval = -EPERM;
	if ((policy == SCHED_FIFO || policy == SCHED_RR) &&
	    !capable(CAP_SYS_NICE))
		goto out_unlock;
	if ((current->euid != p->euid) && (current->euid != p->uid) &&
	    !capable(CAP_SYS_NICE))
		goto out_unlock;

	array = p->array;
	if (array)
		deactivate_task(p, task_rq(p));
	retval = 0;
	p->policy = policy;
	p->rt_priority = lp.sched_priority;
	if (policy != SCHED_NORMAL)
		p->prio = MAX_USER_RT_PRIO-1 - p->rt_priority;
	else
		p->prio = p->static_prio;
	if (array)
		__activate_task(p, task_rq(p));

out_unlock:
	task_rq_unlock(rq, &flags);
out_unlock_tasklist:
	read_unlock_irq(&tasklist_lock);

out_nounlock:
	return retval;
}

/**
 * sys_sched_setscheduler - set/change the scheduler policy and RT priority
 * @pid: the pid in question.
 * @policy: new policy
 * @param: structure containing the new RT priority.
 */
asmlinkage long sys_sched_setscheduler(pid_t pid, int policy,
				      struct sched_param *param)
{
	return setscheduler(pid, policy, param);
}

/**
 * sys_sched_setparam - set/change the RT priority of a thread
 * @pid: the pid in question.
 * @param: structure containing the new RT priority.
 */
asmlinkage long sys_sched_setparam(pid_t pid, struct sched_param *param)
{
	return setscheduler(pid, -1, param);
}

/**
 * sys_sched_getscheduler - get the policy (scheduling class) of a thread
 * @pid: the pid in question.
 */
asmlinkage long sys_sched_getscheduler(pid_t pid)
{
	int retval = -EINVAL;
	task_t *p;

	if (pid < 0)
		goto out_nounlock;

	retval = -ESRCH;
	read_lock(&tasklist_lock);
	p = find_process_by_pid(pid);
	if (p)
		retval = p->policy;
	read_unlock(&tasklist_lock);

out_nounlock:
	return retval;
}

/**
 * sys_sched_getscheduler - get the RT priority of a thread
 * @pid: the pid in question.
 * @param: structure containing the RT priority.
 */
asmlinkage long sys_sched_getparam(pid_t pid, struct sched_param *param)
{
	struct sched_param lp;
	int retval = -EINVAL;
	task_t *p;

	if (!param || pid < 0)
		goto out_nounlock;

	read_lock(&tasklist_lock);
	p = find_process_by_pid(pid);
	retval = -ESRCH;
	if (!p)
		goto out_unlock;
	lp.sched_priority = p->rt_priority;
	read_unlock(&tasklist_lock);

	/*
	 * This one might sleep, we cannot do it with a spinlock held ...
	 */
	retval = copy_to_user(param, &lp, sizeof(*param)) ? -EFAULT : 0;

out_nounlock:
	return retval;

out_unlock:
	read_unlock(&tasklist_lock);
	return retval;
}

/**
 * sys_sched_setaffinity - set the cpu affinity of a process
 * @pid: pid of the process
 * @len: length in bytes of the bitmask pointed to by user_mask_ptr
 * @user_mask_ptr: user-space pointer to the new cpu mask
 */
asmlinkage int sys_sched_setaffinity(pid_t pid, unsigned int len,
				      unsigned long *user_mask_ptr)
{
	unsigned long new_mask;
	int retval;
	task_t *p;

	if (len < sizeof(new_mask))
		return -EINVAL;

	if (copy_from_user(&new_mask, user_mask_ptr, sizeof(new_mask)))
		return -EFAULT;

	new_mask &= cpu_online_map;
	if (!new_mask)
		return -EINVAL;

	read_lock(&tasklist_lock);

	p = find_process_by_pid(pid);
	if (!p) {
		read_unlock(&tasklist_lock);
		return -ESRCH;
	}

	/*
	 * It is not safe to call set_cpus_allowed with the
	 * tasklist_lock held.  We will bump the task_struct's
	 * usage count and then drop tasklist_lock.
	 */
	get_task_struct(p);
	read_unlock(&tasklist_lock);

	retval = -EPERM;
	if ((current->euid != p->euid) && (current->euid != p->uid) &&
			!capable(CAP_SYS_NICE))
		goto out_unlock;

	retval = 0;
	set_cpus_allowed(p, new_mask);

out_unlock:
	put_task_struct(p);
	return retval;
}

/**
 * sys_sched_getaffinity - get the cpu affinity of a process
 * @pid: pid of the process
 * @len: length in bytes of the bitmask pointed to by user_mask_ptr
 * @user_mask_ptr: user-space pointer to hold the current cpu mask
 */
asmlinkage int sys_sched_getaffinity(pid_t pid, unsigned int len,
				      unsigned long *user_mask_ptr)
{
	unsigned int real_len;
	unsigned long mask;
	int retval;
	task_t *p;

	real_len = sizeof(mask);
	if (len < real_len)
		return -EINVAL;

	read_lock(&tasklist_lock);

	retval = -ESRCH;
	p = find_process_by_pid(pid);
	if (!p)
		goto out_unlock;

	retval = 0;
	mask = p->cpus_allowed & cpu_online_map;

out_unlock:
	read_unlock(&tasklist_lock);
	if (retval)
		return retval;
	if (copy_to_user(user_mask_ptr, &mask, real_len))
		return -EFAULT;
	return real_len;
}

/**
 * sys_sched_yield - yield the current processor to other threads.
 *
 * this function yields the current CPU by moving the calling thread
 * to the expired array. If there are no other threads running on this
 * CPU then this function will return.
 */
asmlinkage long sys_sched_yield(void)
{
	runqueue_t *rq = this_rq_lock();
	prio_array_t *array = current->array;

	/*
	 * We implement yielding by moving the task into the expired
	 * queue.
	 *
	 * (special rule: RT tasks will just roundrobin in the active
	 *  array.)
	 */
	if (likely(!rt_task(current))) {
		dequeue_task(current, array);
		enqueue_task(current, rq->expired);
	} else {
		list_del(&current->run_list);
		list_add_tail(&current->run_list, array->queue + current->prio);
	}
	spin_unlock(&rq->lock);

	schedule();

	return 0;
}

void __cond_resched(void)
{
	set_current_state(TASK_RUNNING);
	schedule();
}

/**
 * yield - yield the current processor to other threads.
 *
 * this is a shortcut for kernel-space yielding - it marks the
 * thread runnable and calls sys_sched_yield().
 */
void yield(void)
{
	set_current_state(TASK_RUNNING);
	sys_sched_yield();
}

/*
 * This task is about to go to sleep on IO.  Increment rq->nr_iowait so
 * that process accounting knows that this is a task in IO wait state.
 *
 * But don't do that if it is a deliberate, throttling IO wait (this task
 * has set its backing_dev_info: the queue against which it should throttle)
 */
void io_schedule(void)
{
	struct runqueue *rq = this_rq();

	atomic_inc(&rq->nr_iowait);
	schedule();
	atomic_dec(&rq->nr_iowait);
}

long io_schedule_timeout(long timeout)
{
	struct runqueue *rq = this_rq();
	long ret;

	atomic_inc(&rq->nr_iowait);
	ret = schedule_timeout(timeout);
	atomic_dec(&rq->nr_iowait);
	return ret;
}

/**
 * sys_sched_get_priority_max - return maximum RT priority.
 * @policy: scheduling class.
 *
 * this syscall returns the maximum rt_priority that can be used
 * by a given scheduling class.
 */
asmlinkage long sys_sched_get_priority_max(int policy)
{
	int ret = -EINVAL;

	switch (policy) {
	case SCHED_FIFO:
	case SCHED_RR:
		ret = MAX_USER_RT_PRIO-1;
		break;
	case SCHED_NORMAL:
		ret = 0;
		break;
	}
	return ret;
}

/**
 * sys_sched_get_priority_min - return minimum RT priority.
 * @policy: scheduling class.
 *
 * this syscall returns the minimum rt_priority that can be used
 * by a given scheduling class.
 */
asmlinkage long sys_sched_get_priority_min(int policy)
{
	int ret = -EINVAL;

	switch (policy) {
	case SCHED_FIFO:
	case SCHED_RR:
		ret = 1;
		break;
	case SCHED_NORMAL:
		ret = 0;
	}
	return ret;
}

/**
 * sys_sched_rr_get_interval - return the default timeslice of a process.
 * @pid: pid of the process.
 * @interval: userspace pointer to the timeslice value.
 *
 * this syscall writes the default timeslice value of a given process
 * into the user-space timespec buffer. A value of '0' means infinity.
 */
asmlinkage long sys_sched_rr_get_interval(pid_t pid, struct timespec *interval)
{
	int retval = -EINVAL;
	struct timespec t;
	task_t *p;

	if (pid < 0)
		goto out_nounlock;

	retval = -ESRCH;
	read_lock(&tasklist_lock);
	p = find_process_by_pid(pid);
	if (p)
		jiffies_to_timespec(p->policy & SCHED_FIFO ?
				 0 : task_timeslice(p), &t);
	read_unlock(&tasklist_lock);
	if (p)
		retval = copy_to_user(interval, &t, sizeof(t)) ? -EFAULT : 0;
out_nounlock:
	return retval;
}

static inline struct task_struct *eldest_child(struct task_struct *p)
{
	if (list_empty(&p->children)) return NULL;
	return list_entry(p->children.next,struct task_struct,sibling);
}

static inline struct task_struct *older_sibling(struct task_struct *p)
{
	if (p->sibling.prev==&p->parent->children) return NULL;
	return list_entry(p->sibling.prev,struct task_struct,sibling);
}

static inline struct task_struct *younger_sibling(struct task_struct *p)
{
	if (p->sibling.next==&p->parent->children) return NULL;
	return list_entry(p->sibling.next,struct task_struct,sibling);
}

void show_task(task_t * p)
{
	unsigned long free = 0;
	task_t *relative;
	int state;
	static const char * stat_nam[] = { "R", "S", "D", "T", "Z", "X" };

	printk("%-13.13s ", p->comm);
	state = p->state ? __ffs(p->state) + 1 : 0;
	if (((unsigned) state) < sizeof(stat_nam)/sizeof(char *))
		printk(stat_nam[state]);
	else
		printk(" ");
#if (BITS_PER_LONG == 32)
	if (p == current)
		printk(" current  ");
	else
		printk(" %08lX ", thread_saved_pc(&p->thread));
#else
	if (p == current)
		printk("   current task   ");
	else
		printk(" %016lx ", thread_saved_pc(&p->thread));
#endif
	{
		unsigned long * n = (unsigned long *) (p+1);
		while (!*n)
			n++;
		free = (unsigned long) n - (unsigned long)(p+1);
	}
	printk("%5lu %5d %6d ", free, p->pid, p->parent->pid);
	if ((relative = eldest_child(p)))
		printk("%5d ", relative->pid);
	else
		printk("      ");
	if ((relative = younger_sibling(p)))
		printk("%7d", relative->pid);
	else
		printk("       ");
	if ((relative = older_sibling(p)))
		printk(" %5d", relative->pid);
	else
		printk("      ");
	if (!p->mm)
		printk(" (L-TLB)\n");
	else
		printk(" (NOTLB)\n");
//	print_signals(p);
	{
		extern void show_trace_task(task_t *tsk);
		show_trace_task(p);
	}
}

char * render_sigset_t(sigset_t *set, char *buffer)
{
	int i = _NSIG, x;
	do {
		i -= 4, x = 0;
		if (sigismember(set, i+1)) x |= 1;
		if (sigismember(set, i+2)) x |= 2;
		if (sigismember(set, i+3)) x |= 4;
		if (sigismember(set, i+4)) x |= 8;
		*buffer++ = (x < 10 ? '0' : 'a' - 10) + x;
	} while (i >= 4);
	*buffer = 0;
	return buffer;
}

void show_state(void)
{
	task_t *g, *p;

#if (BITS_PER_LONG == 32)
	printk("\n"
	       "                         free                        sibling\n");
	printk("  task             PC    stack   pid father child younger older\n");
#else
	printk("\n"
	       "                                 free                        sibling\n");
	printk("  task                 PC        stack   pid father child younger older\n");
#endif
	read_lock(&tasklist_lock);
	do_each_thread(g, p) {
		/*
		 * reset the NMI-timeout, listing all files on a slow
		 * console might take alot of time:
		 */
		touch_nmi_watchdog();
		show_task(p);
	} while_each_thread(g, p);

	read_unlock(&tasklist_lock);
}

void __init init_idle(task_t *idle, int cpu)
{
	runqueue_t *idle_rq = cpu_rq(cpu), *rq = cpu_rq(task_cpu(idle));
	unsigned long flags;

	local_irq_save(flags);
	double_rq_lock(idle_rq, rq);

	idle_rq->curr = idle_rq->idle = idle;
	deactivate_task(idle, rq);
	idle->array = NULL;
	idle->prio = MAX_PRIO;
	idle->state = TASK_RUNNING;
	set_task_cpu(idle, cpu);
	double_rq_unlock(idle_rq, rq);
	set_tsk_need_resched(idle);
	local_irq_restore(flags);
}

#if CONFIG_SMP
/*
 * This is how migration works:
 *
 * 1) we queue a migration_req_t structure in the source CPU's
 *    runqueue and wake up that CPU's migration thread.
 * 2) we down() the locked semaphore => thread blocks.
 * 3) migration thread wakes up (implicitly it forces the migrated
 *    thread off the CPU)
 * 4) it gets the migration request and checks whether the migrated
 *    task is still in the wrong runqueue.
 * 5) if it's in the wrong runqueue then the migration thread removes
 *    it and puts it into the right queue.
 * 6) migration thread up()s the semaphore.
 * 7) we wake up and the migration is done.
 */

typedef struct {
	struct list_head list;
	task_t *task;
	struct completion done;
} migration_req_t;

/*
 * Change a given task's CPU affinity. Migrate the thread to a
 * proper CPU and schedule it away if the CPU it's executing on
 * is removed from the allowed bitmask.
 *
 * NOTE: the caller must have a valid reference to the task, the
 * task must not exit() & deallocate itself prematurely.  The
 * call is not atomic; no spinlocks may be held.
 */
void set_cpus_allowed(task_t *p, unsigned long new_mask)
{
	unsigned long flags;
	migration_req_t req;
	runqueue_t *rq;

#if 0 /* FIXME: Grab cpu_lock, return error on this case. --RR */
	new_mask &= cpu_online_map;
	if (!new_mask)
		BUG();
#endif

	rq = task_rq_lock(p, &flags);
	p->cpus_allowed = new_mask;
	/*
	 * Can the task run on the task's current CPU? If not then
	 * migrate the thread off to a proper CPU.
	 */
	if (new_mask & (1UL << task_cpu(p))) {
		task_rq_unlock(rq, &flags);
		return;
	}
	/*
	 * If the task is not on a runqueue (and not running), then
	 * it is sufficient to simply update the task's cpu field.
	 */
	if (!p->array && !task_running(rq, p)) {
		set_task_cpu(p, __ffs(p->cpus_allowed));
		task_rq_unlock(rq, &flags);
		return;
	}
	init_completion(&req.done);
	req.task = p;
	list_add(&req.list, &rq->migration_queue);
	task_rq_unlock(rq, &flags);

	wake_up_process(rq->migration_thread);

	wait_for_completion(&req.done);
}

/*
 * migration_thread - this is a highprio system thread that performs
 * thread migration by 'pulling' threads into the target runqueue.
 */
static int migration_thread(void * data)
{
	struct sched_param param = { .sched_priority = MAX_RT_PRIO-1 };
	int cpu = (long) data;
	runqueue_t *rq;
	int ret;
 
	daemonize();
	sigfillset(&current->blocked);
	set_fs(KERNEL_DS);

	/*
	 * Either we are running on the right CPU, or there's a
	 * a migration thread on the target CPU, guaranteed.
	 */
	set_cpus_allowed(current, 1UL << cpu);

	ret = setscheduler(0, SCHED_FIFO, &param);

	rq = this_rq();
	rq->migration_thread = current;

	sprintf(current->comm, "migration/%d", smp_processor_id());

	for (;;) {
		runqueue_t *rq_src, *rq_dest;
		struct list_head *head;
		int cpu_src, cpu_dest;
		migration_req_t *req;
		unsigned long flags;
		task_t *p;

		spin_lock_irqsave(&rq->lock, flags);
		head = &rq->migration_queue;
		current->state = TASK_INTERRUPTIBLE;
		if (list_empty(head)) {
			spin_unlock_irqrestore(&rq->lock, flags);
			schedule();
			continue;
		}
		req = list_entry(head->next, migration_req_t, list);
		list_del_init(head->next);
		spin_unlock_irqrestore(&rq->lock, flags);

		p = req->task;
		cpu_dest = __ffs(p->cpus_allowed);
		rq_dest = cpu_rq(cpu_dest);
repeat:
		cpu_src = task_cpu(p);
		rq_src = cpu_rq(cpu_src);

		local_irq_save(flags);
		double_rq_lock(rq_src, rq_dest);
		if (task_cpu(p) != cpu_src) {
			double_rq_unlock(rq_src, rq_dest);
			local_irq_restore(flags);
			goto repeat;
		}
		if (rq_src == rq) {
			set_task_cpu(p, cpu_dest);
			if (p->array) {
				deactivate_task(p, rq_src);
				__activate_task(p, rq_dest);
				if (p->prio < rq_dest->curr->prio)
					resched_task(rq_dest->curr);
			}
		}
		double_rq_unlock(rq_src, rq_dest);
		local_irq_restore(flags);

		complete(&req->done);
	}
}

/*
 * migration_call - callback that gets triggered when a CPU is added.
 * Here we can start up the necessary migration thread for the new CPU.
 */
void migration_call(void *hcpu)
{
	printk("Starting migration thread for cpu %li\n", (long)hcpu);
	kernel_thread(migration_thread, hcpu, CLONE_KERNEL);
	while (!cpu_rq((long)hcpu)->migration_thread)
		yield();
}

__init int migration_init(void)
{
	int cpu;

	/* Start one for boot CPU. */
	migration_call((void *)(long)smp_processor_id());

	printk("smp_num_cpus: %d.\n", smp_num_cpus);
	for (cpu = 0; cpu < smp_num_cpus; cpu++)
		if (cpu != smp_processor_id())
			migration_call((void *)(long)cpu);
	return 0;
}

#endif


extern void init_timervecs(void);
extern void timer_bh(void);
extern void tqueue_bh(void);
extern void immediate_bh(void);

void __init sched_init(void)
{
	runqueue_t *rq;
	int i, j, k;

	for (i = 0; i < NR_CPUS; i++) {
		prio_array_t *array;

		rq = cpu_rq(i);
		rq->active = rq->arrays;
		rq->expired = rq->arrays + 1;
		spin_lock_init(&rq->lock);
		INIT_LIST_HEAD(&rq->migration_queue);
		atomic_set(&rq->nr_iowait, 0);

		for (j = 0; j < 2; j++) {
			array = rq->arrays + j;
			for (k = 0; k < MAX_PRIO; k++) {
				INIT_LIST_HEAD(array->queue + k);
				__clear_bit(k, array->bitmap);
			}
			// delimiter for bitsearch
			__set_bit(MAX_PRIO, array->bitmap);
		}
	}
	/*
	 * We have to do a little magic to get the first
	 * thread right in SMP mode.
	 */
	rq = this_rq();
	rq->curr = current;
	rq->idle = current;
	set_task_cpu(current, smp_processor_id());
	wake_up_forked_process(current);

	init_timervecs();
	init_bh(TIMER_BH, timer_bh);
	init_bh(TQUEUE_BH, tqueue_bh);
	init_bh(IMMEDIATE_BH, immediate_bh);

	/*
	 * The boot idle thread does lazy MMU switching as well:
	 */
	atomic_inc(&init_mm.mm_count);
	enter_lazy_tlb(&init_mm, current, smp_processor_id());
}

