/*
 *  Fast Userspace Mutexes (which I call "Futexes!").
 *  (C) Rusty Russell, IBM 2002
 *
 *  Thanks to Ben LaHaise for yelling "hashed waitqueues" loudly
 *  enough at me, Linus for the original (flawed) idea, Matthew
 *  Kirkwood for proof-of-concept implementation.
 *
 *  "The futexes are also cursed."
 *  "But they come in a choice of three flavours!"
 *
 *  Generalized futexes for every mapping type, Ingo Molnar, 2002
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include <linux/slab.h>
#include <linux/poll.h>
#include <linux/file.h>
#include <linux/hash.h>
#include <linux/init.h>
#include <linux/futex.h>
#include <linux/mount.h>
#include <linux/vcache.h>
#include <linux/module.h>

#define FUTEX_HASHBITS 8

/*
 * We use this hashed waitqueue instead of a normal wait_queue_t, so
 * we can wake only the relevent ones (hashed queues may be shared):
 */
struct futex_q {
	struct list_head list;
	wait_queue_head_t waiters;

	/* Page struct and offset within it. */
	struct page *page;
	int offset;

	/* the virtual => physical COW-safe cache */
	vcache_t vcache;
};

/* The key for the hash is the address + index + offset within page */
static struct list_head futex_queues[1<<FUTEX_HASHBITS];
static spinlock_t futex_lock = SPIN_LOCK_UNLOCKED;

/*
 * These are all locks that are necessery to look up a physical
 * mapping safely, and modify/search the futex hash, atomically:
 */
static inline void lock_futex_mm(void)
{
	spin_lock(&current->mm->page_table_lock);
	spin_lock(&vcache_lock);
	spin_lock(&futex_lock);
}

static inline void unlock_futex_mm(void)
{
	spin_unlock(&futex_lock);
	spin_unlock(&vcache_lock);
	spin_unlock(&current->mm->page_table_lock);
}

/*
 * The physical page is shared, so we can hash on its address:
 */
static inline struct list_head *hash_futex(struct page *page, int offset)
{
	return &futex_queues[hash_long((unsigned long)page + offset,
							FUTEX_HASHBITS)];
}

/* Waiter either waiting in FUTEX_WAIT or poll(), or expecting signal */
static inline void tell_waiter(struct futex_q *q)
{
	wake_up_all(&q->waiters);
}

/*
 * Get kernel address of the user page and pin it.
 *
 * Must be called with (and returns with) all futex-MM locks held.
 */
static struct page *__pin_page(unsigned long addr)
{
	struct mm_struct *mm = current->mm;
	struct page *page, *tmp;
	int err;

	/*
	 * Do a quick atomic lookup first - this is the fastpath.
	 */
	page = follow_page(mm, addr, 0);
	if (likely(page != NULL)) {	
		if (!PageReserved(page))
			get_page(page);
		return page;
	}

	/*
	 * No luck - need to fault in the page:
	 */
repeat_lookup:

	unlock_futex_mm();

	down_read(&mm->mmap_sem);
	err = get_user_pages(current, mm, addr, 1, 0, 0, &page, NULL);
	up_read(&mm->mmap_sem);

	lock_futex_mm();

	if (err < 0)
		return NULL;
	/*
	 * Since the faulting happened with locks released, we have to
	 * check for races:
	 */
	tmp = follow_page(mm, addr, 0);
	if (tmp != page) {
		put_page(page);
		goto repeat_lookup;
	}

	return page;
}

static inline void unpin_page(struct page *page)
{
	put_page(page);
}

/*
 * Wake up all waiters hashed on the physical page that is mapped
 * to this virtual address:
 */
static int futex_wake(unsigned long uaddr, int offset, int num)
{
	struct list_head *i, *next, *head;
	struct page *page;
	int ret = 0;

	lock_futex_mm();

	page = __pin_page(uaddr - offset);
	if (!page) {
		unlock_futex_mm();
		return -EFAULT;
	}

	head = hash_futex(page, offset);

	list_for_each_safe(i, next, head) {
		struct futex_q *this = list_entry(i, struct futex_q, list);

		if (this->page == page && this->offset == offset) {
			list_del_init(i);
			__detach_vcache(&this->vcache);
			tell_waiter(this);
			ret++;
			if (ret >= num)
				break;
		}
	}

	unlock_futex_mm();
	unpin_page(page);

	return ret;
}

/*
 * This gets called by the COW code, we have to rehash any
 * futexes that were pending on the old physical page, and
 * rehash it to the new physical page. The pagetable_lock
 * and vcache_lock is already held:
 */
static void futex_vcache_callback(vcache_t *vcache, struct page *new_page)
{
	struct futex_q *q = container_of(vcache, struct futex_q, vcache);
	struct list_head *head = hash_futex(new_page, q->offset);

	spin_lock(&futex_lock);

	if (!list_empty(&q->list)) {
		q->page = new_page;
		list_del(&q->list);
		list_add_tail(&q->list, head);
	}

	spin_unlock(&futex_lock);
}

static inline void __queue_me(struct futex_q *q, struct page *page,
				unsigned long uaddr, int offset)
{
	struct list_head *head = hash_futex(page, offset);

	q->offset = offset;
	q->page = page;

	list_add_tail(&q->list, head);
	/*
	 * We register a futex callback to this virtual address,
	 * to make sure a COW properly rehashes the futex-queue.
	 */
	__attach_vcache(&q->vcache, uaddr, current->mm, futex_vcache_callback);
}

/* Return 1 if we were still queued (ie. 0 means we were woken) */
static inline int unqueue_me(struct futex_q *q)
{
	int ret = 0;

	spin_lock(&vcache_lock);
	spin_lock(&futex_lock);
	if (!list_empty(&q->list)) {
		list_del(&q->list);
		__detach_vcache(&q->vcache);
		ret = 1;
	}
	spin_unlock(&futex_lock);
	spin_unlock(&vcache_lock);
	return ret;
}

static int futex_wait(unsigned long uaddr,
		      int offset,
		      int val,
		      unsigned long time)
{
	DECLARE_WAITQUEUE(wait, current);
	int ret = 0, curval;
	struct page *page;
	struct futex_q q;

	init_waitqueue_head(&q.waiters);

	lock_futex_mm();

	page = __pin_page(uaddr - offset);
	if (!page) {
		unlock_futex_mm();
		return -EFAULT;
	}
	__queue_me(&q, page, uaddr, offset);

	unlock_futex_mm();

	/* Page is pinned, but may no longer be in this address space. */
	if (get_user(curval, (int *)uaddr) != 0) {
		ret = -EFAULT;
		goto out;
	}
	if (curval != val) {
		ret = -EWOULDBLOCK;
		goto out;
	}
	/*
	 * The get_user() above might fault and schedule so we
	 * cannot just set TASK_INTERRUPTIBLE state when queueing
	 * ourselves into the futex hash. This code thus has to
	 * rely on the FUTEX_WAKE code doing a wakeup after removing
	 * the waiter from the list.
	 */
	add_wait_queue(&q.waiters, &wait);
	set_current_state(TASK_INTERRUPTIBLE);
	if (!list_empty(&q.list))
		time = schedule_timeout(time);
	set_current_state(TASK_RUNNING);
	/*
	 * NOTE: we dont remove ourselves from the waitqueue because
	 * we are the only user of it.
	 */
	if (time == 0) {
		ret = -ETIMEDOUT;
		goto out;
	}
	if (signal_pending(current))
		ret = -EINTR;
out:
	/* Were we woken up anyway? */
	if (!unqueue_me(&q))
		ret = 0;
	unpin_page(page);

	return ret;
}

static inline int futex_wait_utime(unsigned long uaddr,
		      int offset,
		      int val,
		      struct timespec* utime)
{
	unsigned long time = MAX_SCHEDULE_TIMEOUT;

	if (utime) {
		struct timespec t;
		if (copy_from_user(&t, utime, sizeof(t)) != 0)
			return -EFAULT;
		time = timespec_to_jiffies(&t) + 1;
	}

	return futex_wait(uaddr, offset, val, time);
}

asmlinkage int sys_futex(unsigned long uaddr, int op, int val, struct timespec *utime)
{
	unsigned long pos_in_page;
	int ret;

	pos_in_page = uaddr % PAGE_SIZE;

	/* Must be "naturally" aligned */
	if (pos_in_page % sizeof(int))
		return -EINVAL;

	switch (op) {
	case FUTEX_WAIT:
		ret = futex_wait_utime(uaddr, pos_in_page, val, utime);
		break;
	case FUTEX_WAKE:
		ret = futex_wake(uaddr, pos_in_page, val);
		break;
	/*
	 * We disable FUTEX_FD support due to risks: it is the least tested
	 * aspect of futexes (they were broken for many kernel versions and
	 * no-one noticed) and always were one of the biggest source of bugs.
	 */
#if 0
	case FUTEX_FD:
		/* non-zero val means F_SETOWN(getpid()) & F_SETSIG(val) */
		ret = futex_fd(uaddr, pos_in_page, val);
		break;
#endif
	default:
		ret = -EINVAL;
	}
	return ret;
}

static int __init init(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(futex_queues); i++)
		INIT_LIST_HEAD(&futex_queues[i]);
	return 0;
}
__initcall(init);
