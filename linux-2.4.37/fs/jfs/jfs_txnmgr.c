/*
 *   Copyright (C) International Business Machines Corp., 2000-2004
 *   Portions Copyright (C) Christoph Hellwig, 2001-2002
 *
 *   This program is free software;  you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or 
 *   (at your option) any later version.
 * 
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY;  without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See
 *   the GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program;  if not, write to the Free Software 
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/*
 *      jfs_txnmgr.c: transaction manager
 *
 * notes:
 * transaction starts with txBegin() and ends with txCommit()
 * or txAbort().
 *
 * tlock is acquired at the time of update;
 * (obviate scan at commit time for xtree and dtree)
 * tlock and mp points to each other;
 * (no hashlist for mp -> tlock).
 *
 * special cases:
 * tlock on in-memory inode:
 * in-place tlock in the in-memory inode itself;
 * converted to page lock by iWrite() at commit time.
 *
 * tlock during write()/mmap() under anonymous transaction (tid = 0):
 * transferred (?) to transaction at commit time.
 *
 * use the page itself to update allocation maps
 * (obviate intermediate replication of allocation/deallocation data)
 * hold on to mp+lock thru update of maps
 */


#include <linux/fs.h>
#include <linux/vmalloc.h>
#include <linux/smp_lock.h>
#include <linux/completion.h>
#include "jfs_incore.h"
#include "jfs_filsys.h"
#include "jfs_metapage.h"
#include "jfs_dinode.h"
#include "jfs_imap.h"
#include "jfs_dmap.h"
#include "jfs_superblock.h"
#include "jfs_debug.h"

/*
 *      transaction management structures
 */
static struct {
	/* tblock */
	int freetid;		/* index of a free tid structure */
	wait_queue_head_t freewait;	/* eventlist of free tblock */

	/* tlock */
	int freelock;		/* index first free lock word */
	wait_queue_head_t freelockwait;	/* eventlist of free tlock */
	wait_queue_head_t lowlockwait;	/* eventlist of ample tlocks */
	int tlocksInUse;	/* Number of tlocks in use */
	int TlocksLow;		/* Indicates low number of available tlocks */
	spinlock_t LazyLock;	/* synchronize sync_queue & unlock_queue */
/*	struct tblock *sync_queue; * Transactions waiting for data sync */
	struct tblock *unlock_queue;	/* Txns waiting to be released */
	struct tblock *unlock_tail;	/* Tail of unlock_queue */
	struct list_head anon_list;	/* inodes having anonymous txns */
	struct list_head anon_list2;	/* inodes having anonymous txns
					   that couldn't be sync'ed */
} TxAnchor;

#ifdef CONFIG_JFS_STATISTICS
struct {
	uint txBegin;
	uint txBegin_barrier;
	uint txBegin_lockslow;
	uint txBegin_freetid;
	uint txBeginAnon;
	uint txBeginAnon_barrier;
	uint txBeginAnon_lockslow;
	uint txLockAlloc;
	uint txLockAlloc_freelock;
} TxStat;
#endif

static int nTxBlock = 512;	/* number of transaction blocks */
struct tblock *TxBlock;	        /* transaction block table */

static int nTxLock = 4096;	/* number of transaction locks */
static int TxLockLWM = 4096*.4;	/* Low water mark for number of txLocks used */
static int TxLockHWM = 4096*.8;	/* High water mark for number of txLocks used */
struct tlock *TxLock;           /* transaction lock table */


/*
 *      transaction management lock
 */
static spinlock_t jfsTxnLock = SPIN_LOCK_UNLOCKED;

#define TXN_LOCK()              spin_lock(&jfsTxnLock)
#define TXN_UNLOCK()            spin_unlock(&jfsTxnLock)

#define LAZY_LOCK_INIT()	spin_lock_init(&TxAnchor.LazyLock);
#define LAZY_LOCK(flags)	spin_lock_irqsave(&TxAnchor.LazyLock, flags)
#define LAZY_UNLOCK(flags) spin_unlock_irqrestore(&TxAnchor.LazyLock, flags)

DECLARE_WAIT_QUEUE_HEAD(jfs_sync_thread_wait);
DECLARE_WAIT_QUEUE_HEAD(jfs_commit_thread_wait);

/*
 * Retry logic exist outside these macros to protect from spurrious wakeups.
 */
static inline void TXN_SLEEP_DROP_LOCK(wait_queue_head_t * event)
{
	DECLARE_WAITQUEUE(wait, current);

	add_wait_queue(event, &wait);
	set_current_state(TASK_UNINTERRUPTIBLE);
	TXN_UNLOCK();
	schedule();
	current->state = TASK_RUNNING;
	remove_wait_queue(event, &wait);
}

#define TXN_SLEEP(event)\
{\
	TXN_SLEEP_DROP_LOCK(event);\
	TXN_LOCK();\
}

#define TXN_WAKEUP(event) wake_up_all(event)


/*
 *      statistics
 */
struct {
	tid_t maxtid;		/* 4: biggest tid ever used */
	lid_t maxlid;		/* 4: biggest lid ever used */
	int ntid;		/* 4: # of transactions performed */
	int nlid;		/* 4: # of tlocks acquired */
	int waitlock;		/* 4: # of tlock wait */
} stattx;


/*
 * external references
 */
extern int lmGroupCommit(struct jfs_log *, struct tblock *);
extern void lmSync(struct jfs_log *);
extern int jfs_commit_inode(struct inode *, int);
extern int jfs_stop_threads;

struct task_struct *jfsCommitTask;
extern struct completion jfsIOwait;

/*
 * forward references
 */
static int diLog(struct jfs_log * log, struct tblock * tblk, struct lrd * lrd,
		struct tlock * tlck, struct commit * cd);
static int dataLog(struct jfs_log * log, struct tblock * tblk, struct lrd * lrd,
		struct tlock * tlck);
static void dtLog(struct jfs_log * log, struct tblock * tblk, struct lrd * lrd,
		struct tlock * tlck);
static void mapLog(struct jfs_log * log, struct tblock * tblk, struct lrd * lrd,
		struct tlock * tlck);
static void txAllocPMap(struct inode *ip, struct maplock * maplock,
		struct tblock * tblk);
static void txForce(struct tblock * tblk);
static int txLog(struct jfs_log * log, struct tblock * tblk,
		struct commit * cd);
static void txUpdateMap(struct tblock * tblk);
static void txRelease(struct tblock * tblk);
static void xtLog(struct jfs_log * log, struct tblock * tblk, struct lrd * lrd,
	   struct tlock * tlck);
static void LogSyncRelease(struct metapage * mp);

/*
 *              transaction block/lock management
 *              ---------------------------------
 */

/*
 * Get a transaction lock from the free list.  If the number in use is
 * greater than the high water mark, wake up the sync daemon.  This should
 * free some anonymous transaction locks.  (TXN_LOCK must be held.)
 */
static lid_t txLockAlloc(void)
{
	lid_t lid;

	INCREMENT(TxStat.txLockAlloc);
	if (!TxAnchor.freelock) {
		INCREMENT(TxStat.txLockAlloc_freelock);
	}

	while (!(lid = TxAnchor.freelock))
		TXN_SLEEP(&TxAnchor.freelockwait);
	TxAnchor.freelock = TxLock[lid].next;
	HIGHWATERMARK(stattx.maxlid, lid);
	if ((++TxAnchor.tlocksInUse > TxLockHWM) && (TxAnchor.TlocksLow == 0)) {
		jfs_info("txLockAlloc TlocksLow");
		TxAnchor.TlocksLow = 1;
		wake_up(&jfs_sync_thread_wait);
	}

	return lid;
}

static void txLockFree(lid_t lid)
{
	TxLock[lid].next = TxAnchor.freelock;
	TxAnchor.freelock = lid;
	TxAnchor.tlocksInUse--;
	if (TxAnchor.TlocksLow && (TxAnchor.tlocksInUse < TxLockLWM)) {
		jfs_info("txLockFree TlocksLow no more");
		TxAnchor.TlocksLow = 0;
		TXN_WAKEUP(&TxAnchor.lowlockwait);
	}
	TXN_WAKEUP(&TxAnchor.freelockwait);
}

/*
 * NAME:        txInit()
 *
 * FUNCTION:    initialize transaction management structures
 *
 * RETURN:
 *
 * serialization: single thread at jfs_init()
 */
int txInit(void)
{
	int k, size;

	/*
	 * initialize transaction block (tblock) table
	 *
	 * transaction id (tid) = tblock index
	 * tid = 0 is reserved.
	 */
	size = sizeof(struct tblock) * nTxBlock;
	TxBlock = (struct tblock *) vmalloc(size);
	if (TxBlock == NULL)
		return -ENOMEM;

	for (k = 1; k < nTxBlock - 1; k++) {
		TxBlock[k].next = k + 1;
		init_waitqueue_head(&TxBlock[k].gcwait);
		init_waitqueue_head(&TxBlock[k].waitor);
	}
	TxBlock[k].next = 0;
	init_waitqueue_head(&TxBlock[k].gcwait);
	init_waitqueue_head(&TxBlock[k].waitor);

	TxAnchor.freetid = 1;
	init_waitqueue_head(&TxAnchor.freewait);

	stattx.maxtid = 1;	/* statistics */

	/*
	 * initialize transaction lock (tlock) table
	 *
	 * transaction lock id = tlock index
	 * tlock id = 0 is reserved.
	 */
	size = sizeof(struct tlock) * nTxLock;
	TxLock = (struct tlock *) vmalloc(size);
	if (TxLock == NULL) {
		vfree(TxBlock);
		return -ENOMEM;
	}

	/* initialize tlock table */
	for (k = 1; k < nTxLock - 1; k++)
		TxLock[k].next = k + 1;
	TxLock[k].next = 0;
	init_waitqueue_head(&TxAnchor.freelockwait);
	init_waitqueue_head(&TxAnchor.lowlockwait);

	TxAnchor.freelock = 1;
	TxAnchor.tlocksInUse = 0;
	INIT_LIST_HEAD(&TxAnchor.anon_list);
	INIT_LIST_HEAD(&TxAnchor.anon_list2);

	stattx.maxlid = 1;	/* statistics */

	return 0;
}

/*
 * NAME:        txExit()
 *
 * FUNCTION:    clean up when module is unloaded
 */
void txExit(void)
{
	vfree(TxLock);
	TxLock = 0;
	vfree(TxBlock);
	TxBlock = 0;
}


/*
 * NAME:        txBegin()
 *
 * FUNCTION:    start a transaction.
 *
 * PARAMETER:   sb	- superblock
 *              flag	- force for nested tx;
 *
 * RETURN:	tid	- transaction id
 *
 * note: flag force allows to start tx for nested tx
 * to prevent deadlock on logsync barrier;
 */
tid_t txBegin(struct super_block *sb, int flag)
{
	tid_t t;
	struct tblock *tblk;
	struct jfs_log *log;

	jfs_info("txBegin: flag = 0x%x", flag);
	log = JFS_SBI(sb)->log;

	TXN_LOCK();

	INCREMENT(TxStat.txBegin);

      retry:
	if (!(flag & COMMIT_FORCE)) {
		/*
		 * synchronize with logsync barrier
		 */
		if (test_bit(log_SYNCBARRIER, &log->flag) ||
		    test_bit(log_QUIESCE, &log->flag)) {
			INCREMENT(TxStat.txBegin_barrier);
			TXN_SLEEP(&log->syncwait);
			goto retry;
		}
	}
	if (flag == 0) {
		/*
		 * Don't begin transaction if we're getting starved for tlocks
		 * unless COMMIT_FORCE or COMMIT_INODE (which may ultimately
		 * free tlocks)
		 */
		if (TxAnchor.TlocksLow) {
			INCREMENT(TxStat.txBegin_lockslow);
			TXN_SLEEP(&TxAnchor.lowlockwait);
			goto retry;
		}
	}

	/*
	 * allocate transaction id/block
	 */
	if ((t = TxAnchor.freetid) == 0) {
		jfs_info("txBegin: waiting for free tid");
		INCREMENT(TxStat.txBegin_freetid);
		TXN_SLEEP(&TxAnchor.freewait);
		goto retry;
	}

	tblk = tid_to_tblock(t);

	if ((tblk->next == 0) && !(flag & COMMIT_FORCE)) {
		/* Don't let a non-forced transaction take the last tblk */
		jfs_info("txBegin: waiting for free tid");
		INCREMENT(TxStat.txBegin_freetid);
		TXN_SLEEP(&TxAnchor.freewait);
		goto retry;
	}

	TxAnchor.freetid = tblk->next;

	/*
	 * initialize transaction
	 */

	/*
	 * We can't zero the whole thing or we screw up another thread being
	 * awakened after sleeping on tblk->waitor
	 *
	 * memset(tblk, 0, sizeof(struct tblock));
	 */
	tblk->next = tblk->last = tblk->xflag = tblk->flag = tblk->lsn = 0;

	tblk->sb = sb;
	++log->logtid;
	tblk->logtid = log->logtid;

	++log->active;

	HIGHWATERMARK(stattx.maxtid, t);	/* statistics */
	INCREMENT(stattx.ntid);	/* statistics */

	TXN_UNLOCK();

	jfs_info("txBegin: returning tid = %d", t);

	return t;
}


/*
 * NAME:        txBeginAnon()
 *
 * FUNCTION:    start an anonymous transaction.
 *		Blocks if logsync or available tlocks are low to prevent
 *		anonymous tlocks from depleting supply.
 *
 * PARAMETER:   sb	- superblock
 *
 * RETURN:	none
 */
void txBeginAnon(struct super_block *sb)
{
	struct jfs_log *log;

	log = JFS_SBI(sb)->log;

	TXN_LOCK();
	INCREMENT(TxStat.txBeginAnon);

      retry:
	/*
	 * synchronize with logsync barrier
	 */
	if (test_bit(log_SYNCBARRIER, &log->flag) ||
	    test_bit(log_QUIESCE, &log->flag)) {
		INCREMENT(TxStat.txBeginAnon_barrier);
		TXN_SLEEP(&log->syncwait);
		goto retry;
	}

	/*
	 * Don't begin transaction if we're getting starved for tlocks
	 */
	if (TxAnchor.TlocksLow) {
		INCREMENT(TxStat.txBeginAnon_lockslow);
		TXN_SLEEP(&TxAnchor.lowlockwait);
		goto retry;
	}
	TXN_UNLOCK();
}


/*
 *      txEnd()
 *
 * function: free specified transaction block.
 *
 *      logsync barrier processing:
 *
 * serialization:
 */
void txEnd(tid_t tid)
{
	struct tblock *tblk = tid_to_tblock(tid);
	struct jfs_log *log;

	jfs_info("txEnd: tid = %d", tid);
	TXN_LOCK();

	/*
	 * wakeup transactions waiting on the page locked
	 * by the current transaction
	 */
	TXN_WAKEUP(&tblk->waitor);

	log = JFS_SBI(tblk->sb)->log;

	/*
	 * Lazy commit thread can't free this guy until we mark it UNLOCKED,
	 * otherwise, we would be left with a transaction that may have been
	 * reused.
	 *
	 * Lazy commit thread will turn off tblkGC_LAZY before calling this
	 * routine.
	 */
	if (tblk->flag & tblkGC_LAZY) {
		jfs_info("txEnd called w/lazy tid: %d, tblk = 0x%p", tid, tblk);
		TXN_UNLOCK();

		spin_lock_irq(&log->gclock);	// LOGGC_LOCK
		tblk->flag |= tblkGC_UNLOCKED;
		spin_unlock_irq(&log->gclock);	// LOGGC_UNLOCK
		return;
	}

	jfs_info("txEnd: tid: %d, tblk = 0x%p", tid, tblk);

	assert(tblk->next == 0);

	/*
	 * insert tblock back on freelist
	 */
	tblk->next = TxAnchor.freetid;
	TxAnchor.freetid = tid;

	/*
	 * mark the tblock not active
	 */
	if (--log->active == 0) {
		clear_bit(log_FLUSH, &log->flag);

		/*
		 * synchronize with logsync barrier
		 */
		if (test_bit(log_SYNCBARRIER, &log->flag)) {
			/* forward log syncpt */
			/* lmSync(log); */

			jfs_info("log barrier off: 0x%x", log->lsn);

			/* enable new transactions start */
			clear_bit(log_SYNCBARRIER, &log->flag);

			/* wakeup all waitors for logsync barrier */
			TXN_WAKEUP(&log->syncwait);
		}
	}

	/*
	 * wakeup all waitors for a free tblock
	 */
	TXN_WAKEUP(&TxAnchor.freewait);

	TXN_UNLOCK();
}


/*
 *      txLock()
 *
 * function: acquire a transaction lock on the specified <mp>
 *
 * parameter:
 *
 * return:      transaction lock id
 *
 * serialization:
 */
struct tlock *txLock(tid_t tid, struct inode *ip, struct metapage * mp,
		     int type)
{
	struct jfs_inode_info *jfs_ip = JFS_IP(ip);
	int dir_xtree = 0;
	lid_t lid;
	tid_t xtid;
	struct tlock *tlck;
	struct xtlock *xtlck;
	struct linelock *linelock;
	xtpage_t *p;
	struct tblock *tblk;

	assert(!test_cflag(COMMIT_Nolink, ip));

	TXN_LOCK();

	if (S_ISDIR(ip->i_mode) && (type & tlckXTREE) &&
	    !(mp->xflag & COMMIT_PAGE)) {
		/*
		 * Directory inode is special.  It can have both an xtree tlock
		 * and a dtree tlock associated with it.
		 */
		dir_xtree = 1;
		lid = jfs_ip->xtlid;
	} else
		lid = mp->lid;

	/* is page not locked by a transaction ? */
	if (lid == 0)
		goto allocateLock;

	jfs_info("txLock: tid:%d ip:0x%p mp:0x%p lid:%d", tid, ip, mp, lid);

	/* is page locked by the requester transaction ? */
	tlck = lid_to_tlock(lid);
	if ((xtid = tlck->tid) == tid)
		goto grantLock;

	/*
	 * is page locked by anonymous transaction/lock ?
	 *
	 * (page update without transaction (i.e., file write) is
	 * locked under anonymous transaction tid = 0:
	 * anonymous tlocks maintained on anonymous tlock list of
	 * the inode of the page and available to all anonymous
	 * transactions until txCommit() time at which point
	 * they are transferred to the transaction tlock list of
	 * the commiting transaction of the inode)
	 */
	if (xtid == 0) {
		tlck->tid = tid;
		tblk = tid_to_tblock(tid);
		/*
		 * The order of the tlocks in the transaction is important
		 * (during truncate, child xtree pages must be freed before
		 * parent's tlocks change the working map).
		 * Take tlock off anonymous list and add to tail of
		 * transaction list
		 *
		 * Note:  We really need to get rid of the tid & lid and
		 * use list_head's.  This code is getting UGLY!
		 */
		if (jfs_ip->atlhead == lid) {
			if (jfs_ip->atltail == lid) {
				/* only anonymous txn.
				 * Remove from anon_list
				 */
				list_del_init(&jfs_ip->anon_inode_list);
			}
			jfs_ip->atlhead = tlck->next;
		} else {
			lid_t last;
			for (last = jfs_ip->atlhead;
			     lid_to_tlock(last)->next != lid;
			     last = lid_to_tlock(last)->next) {
				assert(last);
			}
			lid_to_tlock(last)->next = tlck->next;
			if (jfs_ip->atltail == lid)
				jfs_ip->atltail = last;
		}

		/* insert the tlock at tail of transaction tlock list */

		if (tblk->next)
			lid_to_tlock(tblk->last)->next = lid;
		else
			tblk->next = lid;
		tlck->next = 0;
		tblk->last = lid;

		goto grantLock;
	}

	goto waitLock;

	/*
	 * allocate a tlock
	 */
      allocateLock:
	lid = txLockAlloc();
	tlck = lid_to_tlock(lid);

	/*
	 * initialize tlock
	 */
	tlck->tid = tid;

	/* mark tlock for meta-data page */
	if (mp->xflag & COMMIT_PAGE) {

		tlck->flag = tlckPAGELOCK;

		/* mark the page dirty and nohomeok */
		mark_metapage_dirty(mp);
		atomic_inc(&mp->nohomeok);

		jfs_info("locking mp = 0x%p, nohomeok = %d tid = %d tlck = 0x%p",
			 mp, atomic_read(&mp->nohomeok), tid, tlck);

		/* if anonymous transaction, and buffer is on the group
		 * commit synclist, mark inode to show this.  This will
		 * prevent the buffer from being marked nohomeok for too
		 * long a time.
		 */
		if ((tid == 0) && mp->lsn)
			set_cflag(COMMIT_Synclist, ip);
	}
	/* mark tlock for in-memory inode */
	else
		tlck->flag = tlckINODELOCK;

	tlck->type = 0;

	/* bind the tlock and the page */
	tlck->ip = ip;
	tlck->mp = mp;
	if (dir_xtree)
		jfs_ip->xtlid = lid;
	else
		mp->lid = lid;

	/*
	 * enqueue transaction lock to transaction/inode
	 */
	/* insert the tlock at tail of transaction tlock list */
	if (tid) {
		tblk = tid_to_tblock(tid);
		if (tblk->next)
			lid_to_tlock(tblk->last)->next = lid;
		else
			tblk->next = lid;
		tlck->next = 0;
		tblk->last = lid;
	}
	/* anonymous transaction:
	 * insert the tlock at head of inode anonymous tlock list
	 */
	else {
		tlck->next = jfs_ip->atlhead;
		jfs_ip->atlhead = lid;
		if (tlck->next == 0) {
			/* This inode's first anonymous transaction */
			jfs_ip->atltail = lid;
			list_add_tail(&jfs_ip->anon_inode_list,
				      &TxAnchor.anon_list);
		}
	}

	/* initialize type dependent area for linelock */
	linelock = (struct linelock *) & tlck->lock;
	linelock->next = 0;
	linelock->flag = tlckLINELOCK;
	linelock->maxcnt = TLOCKSHORT;
	linelock->index = 0;

	switch (type & tlckTYPE) {
	case tlckDTREE:
		linelock->l2linesize = L2DTSLOTSIZE;
		break;

	case tlckXTREE:
		linelock->l2linesize = L2XTSLOTSIZE;

		xtlck = (struct xtlock *) linelock;
		xtlck->header.offset = 0;
		xtlck->header.length = 2;

		if (type & tlckNEW) {
			xtlck->lwm.offset = XTENTRYSTART;
		} else {
			if (mp->xflag & COMMIT_PAGE)
				p = (xtpage_t *) mp->data;
			else
				p = &jfs_ip->i_xtroot;
			xtlck->lwm.offset =
			    le16_to_cpu(p->header.nextindex);
		}
		xtlck->lwm.length = 0;	/* ! */
		xtlck->twm.offset = 0;
		xtlck->hwm.offset = 0;

		xtlck->index = 2;
		break;

	case tlckINODE:
		linelock->l2linesize = L2INODESLOTSIZE;
		break;

	case tlckDATA:
		linelock->l2linesize = L2DATASLOTSIZE;
		break;

	default:
		jfs_err("UFO tlock:0x%p", tlck);
	}

	/*
	 * update tlock vector
	 */
      grantLock:
	tlck->type |= type;

	TXN_UNLOCK();

	return tlck;

	/*
	 * page is being locked by another transaction:
	 */
      waitLock:
	/* Only locks on ipimap or ipaimap should reach here */
	/* assert(jfs_ip->fileset == AGGREGATE_I); */
	if (jfs_ip->fileset != AGGREGATE_I) {
		jfs_err("txLock: trying to lock locked page!");
		dump_mem("ip", ip, sizeof(struct inode));
		dump_mem("mp", mp, sizeof(struct metapage));
		dump_mem("Locker's tblk", tid_to_tblock(tid),
			 sizeof(struct tblock));
		dump_mem("Tlock", tlck, sizeof(struct tlock));
		BUG();
	}
	INCREMENT(stattx.waitlock);	/* statistics */
	release_metapage(mp);

	jfs_info("txLock: in waitLock, tid = %d, xtid = %d, lid = %d",
		 tid, xtid, lid);
	TXN_SLEEP_DROP_LOCK(&tid_to_tblock(xtid)->waitor);
	jfs_info("txLock: awakened     tid = %d, lid = %d", tid, lid);

	return NULL;
}


/*
 * NAME:        txRelease()
 *
 * FUNCTION:    Release buffers associated with transaction locks, but don't
 *		mark homeok yet.  The allows other transactions to modify
 *		buffers, but won't let them go to disk until commit record
 *		actually gets written.
 *
 * PARAMETER:
 *              tblk    -
 *
 * RETURN:      Errors from subroutines.
 */
static void txRelease(struct tblock * tblk)
{
	struct metapage *mp;
	lid_t lid;
	struct tlock *tlck;

	TXN_LOCK();

	for (lid = tblk->next; lid; lid = tlck->next) {
		tlck = lid_to_tlock(lid);
		if ((mp = tlck->mp) != NULL &&
		    (tlck->type & tlckBTROOT) == 0) {
			assert(mp->xflag & COMMIT_PAGE);
			mp->lid = 0;
		}
	}

	/*
	 * wakeup transactions waiting on a page locked
	 * by the current transaction
	 */
	TXN_WAKEUP(&tblk->waitor);

	TXN_UNLOCK();
}


/*
 * NAME:        txUnlock()
 *
 * FUNCTION:    Initiates pageout of pages modified by tid in journalled
 *              objects and frees their lockwords.
 */
static void txUnlock(struct tblock * tblk)
{
	struct tlock *tlck;
	struct linelock *linelock;
	lid_t lid, next, llid, k;
	struct metapage *mp;
	struct jfs_log *log;
	int difft, diffp;

	jfs_info("txUnlock: tblk = 0x%p", tblk);
	log = JFS_SBI(tblk->sb)->log;

	/*
	 * mark page under tlock homeok (its log has been written):
	 */
	for (lid = tblk->next; lid; lid = next) {
		tlck = lid_to_tlock(lid);
		next = tlck->next;

		jfs_info("unlocking lid = %d, tlck = 0x%p", lid, tlck);

		/* unbind page from tlock */
		if ((mp = tlck->mp) != NULL &&
		    (tlck->type & tlckBTROOT) == 0) {
			assert(mp->xflag & COMMIT_PAGE);

			/* hold buffer
			 *
			 * It's possible that someone else has the metapage.
			 * The only things were changing are nohomeok, which
			 * is handled atomically, and clsn which is protected
			 * by the LOGSYNC_LOCK.
			 */
			hold_metapage(mp, 1);

			assert(atomic_read(&mp->nohomeok) > 0);
			atomic_dec(&mp->nohomeok);

			/* inherit younger/larger clsn */
			LOGSYNC_LOCK(log);
			if (mp->clsn) {
				logdiff(difft, tblk->clsn, log);
				logdiff(diffp, mp->clsn, log);
				if (difft > diffp)
					mp->clsn = tblk->clsn;
			} else
				mp->clsn = tblk->clsn;
			LOGSYNC_UNLOCK(log);

			assert(!(tlck->flag & tlckFREEPAGE));

			if (tlck->flag & tlckWRITEPAGE) {
				write_metapage(mp);
			} else {
				/* release page which has been forced */
				release_metapage(mp);
			}
		}

		/* insert tlock, and linelock(s) of the tlock if any,
		 * at head of freelist
		 */
		TXN_LOCK();

		llid = ((struct linelock *) & tlck->lock)->next;
		while (llid) {
			linelock = (struct linelock *) lid_to_tlock(llid);
			k = linelock->next;
			txLockFree(llid);
			llid = k;
		}
		txLockFree(lid);

		TXN_UNLOCK();
	}
	tblk->next = tblk->last = 0;

	/*
	 * remove tblock from logsynclist
	 * (allocation map pages inherited lsn of tblk and
	 * has been inserted in logsync list at txUpdateMap())
	 */
	if (tblk->lsn) {
		LOGSYNC_LOCK(log);
		log->count--;
		list_del(&tblk->synclist);
		LOGSYNC_UNLOCK(log);
	}
}


/*
 *      txMaplock()
 *
 * function: allocate a transaction lock for freed page/entry;
 *      for freed page, maplock is used as xtlock/dtlock type;
 */
struct tlock *txMaplock(tid_t tid, struct inode *ip, int type)
{
	struct jfs_inode_info *jfs_ip = JFS_IP(ip);
	lid_t lid;
	struct tblock *tblk;
	struct tlock *tlck;
	struct maplock *maplock;

	TXN_LOCK();

	/*
	 * allocate a tlock
	 */
	lid = txLockAlloc();
	tlck = lid_to_tlock(lid);

	/*
	 * initialize tlock
	 */
	tlck->tid = tid;

	/* bind the tlock and the object */
	tlck->flag = tlckINODELOCK;
	tlck->ip = ip;
	tlck->mp = NULL;

	tlck->type = type;

	/*
	 * enqueue transaction lock to transaction/inode
	 */
	/* insert the tlock at tail of transaction tlock list */
	if (tid) {
		tblk = tid_to_tblock(tid);
		if (tblk->next)
			lid_to_tlock(tblk->last)->next = lid;
		else
			tblk->next = lid;
		tlck->next = 0;
		tblk->last = lid;
	}
	/* anonymous transaction:
	 * insert the tlock at head of inode anonymous tlock list
	 */
	else {
		tlck->next = jfs_ip->atlhead;
		jfs_ip->atlhead = lid;
		if (tlck->next == 0) {
			/* This inode's first anonymous transaction */
			jfs_ip->atltail = lid;
			list_add_tail(&jfs_ip->anon_inode_list,
				      &TxAnchor.anon_list);
		}
	}

	TXN_UNLOCK();

	/* initialize type dependent area for maplock */
	maplock = (struct maplock *) & tlck->lock;
	maplock->next = 0;
	maplock->maxcnt = 0;
	maplock->index = 0;

	return tlck;
}


/*
 *      txLinelock()
 *
 * function: allocate a transaction lock for log vector list
 */
struct linelock *txLinelock(struct linelock * tlock)
{
	lid_t lid;
	struct tlock *tlck;
	struct linelock *linelock;

	TXN_LOCK();

	/* allocate a TxLock structure */
	lid = txLockAlloc();
	tlck = lid_to_tlock(lid);

	TXN_UNLOCK();

	/* initialize linelock */
	linelock = (struct linelock *) tlck;
	linelock->next = 0;
	linelock->flag = tlckLINELOCK;
	linelock->maxcnt = TLOCKLONG;
	linelock->index = 0;

	/* append linelock after tlock */
	linelock->next = tlock->next;
	tlock->next = lid;

	return linelock;
}



/*
 *              transaction commit management
 *              -----------------------------
 */

/*
 * NAME:        txCommit()
 *
 * FUNCTION:    commit the changes to the objects specified in
 *              clist.  For journalled segments only the
 *              changes of the caller are committed, ie by tid.
 *              for non-journalled segments the data are flushed to
 *              disk and then the change to the disk inode and indirect
 *              blocks committed (so blocks newly allocated to the
 *              segment will be made a part of the segment atomically).
 *
 *              all of the segments specified in clist must be in
 *              one file system. no more than 6 segments are needed
 *              to handle all unix svcs.
 *
 *              if the i_nlink field (i.e. disk inode link count)
 *              is zero, and the type of inode is a regular file or
 *              directory, or symbolic link , the inode is truncated
 *              to zero length. the truncation is committed but the
 *              VM resources are unaffected until it is closed (see
 *              iput and iclose).
 *
 * PARAMETER:
 *
 * RETURN:
 *
 * serialization:
 *              on entry the inode lock on each segment is assumed
 *              to be held.
 *
 * i/o error:
 */
int txCommit(tid_t tid,		/* transaction identifier */
	     int nip,		/* number of inodes to commit */
	     struct inode **iplist,	/* list of inode to commit */
	     int flag)
{
	int rc = 0;
	struct commit cd;
	struct jfs_log *log;
	struct tblock *tblk;
	struct lrd *lrd;
	int lsn;
	struct inode *ip;
	struct jfs_inode_info *jfs_ip;
	int k, n;
	ino_t top;
	struct super_block *sb;

	jfs_info("txCommit, tid = %d, flag = %d", tid, flag);
	/* is read-only file system ? */
	if (isReadOnly(iplist[0])) {
		rc = -EROFS;
		goto TheEnd;
	}

	sb = cd.sb = iplist[0]->i_sb;
	cd.tid = tid;

	if (tid == 0)
		tid = txBegin(sb, 0);
	tblk = tid_to_tblock(tid);

	/*
	 * initialize commit structure
	 */
	log = JFS_SBI(sb)->log;
	cd.log = log;

	/* initialize log record descriptor in commit */
	lrd = &cd.lrd;
	lrd->logtid = cpu_to_le32(tblk->logtid);
	lrd->backchain = 0;

	tblk->xflag |= flag;

	if ((flag & (COMMIT_FORCE | COMMIT_SYNC)) == 0)
		tblk->xflag |= COMMIT_LAZY;
	/*
	 *      prepare non-journaled objects for commit
	 *
	 * flush data pages of non-journaled file
	 * to prevent the file getting non-initialized disk blocks
	 * in case of crash.
	 * (new blocks - )
	 */
	cd.iplist = iplist;
	cd.nip = nip;

	/*
	 *      acquire transaction lock on (on-disk) inodes
	 *
	 * update on-disk inode from in-memory inode
	 * acquiring transaction locks for AFTER records
	 * on the on-disk inode of file object
	 *
	 * sort the inodes array by inode number in descending order
	 * to prevent deadlock when acquiring transaction lock
	 * of on-disk inodes on multiple on-disk inode pages by
	 * multiple concurrent transactions
	 */
	for (k = 0; k < cd.nip; k++) {
		top = (cd.iplist[k])->i_ino;
		for (n = k + 1; n < cd.nip; n++) {
			ip = cd.iplist[n];
			if (ip->i_ino > top) {
				top = ip->i_ino;
				cd.iplist[n] = cd.iplist[k];
				cd.iplist[k] = ip;
			}
		}

		ip = cd.iplist[k];
		jfs_ip = JFS_IP(ip);

		if (test_and_clear_cflag(COMMIT_Syncdata, ip) &&
		    ((tblk->flag & COMMIT_DELETE) == 0))
			fsync_inode_data_buffers(ip);

		/*
		 * Mark inode as not dirty.  It will still be on the dirty
		 * inode list, but we'll know not to commit it again unless
		 * it gets marked dirty again
		 */
		clear_cflag(COMMIT_Dirty, ip);

		/* inherit anonymous tlock(s) of inode */
		if (jfs_ip->atlhead) {
			lid_to_tlock(jfs_ip->atltail)->next = tblk->next;
			tblk->next = jfs_ip->atlhead;
			if (!tblk->last)
				tblk->last = jfs_ip->atltail;
			jfs_ip->atlhead = jfs_ip->atltail = 0;
			TXN_LOCK();
			list_del_init(&jfs_ip->anon_inode_list);
			TXN_UNLOCK();
		}

		/*
		 * acquire transaction lock on on-disk inode page
		 * (become first tlock of the tblk's tlock list)
		 */
		if (((rc = diWrite(tid, ip))))
			goto out;
	}

	/*
	 *      write log records from transaction locks
	 *
	 * txUpdateMap() resets XAD_NEW in XAD.
	 */
	if ((rc = txLog(log, tblk, &cd)))
		goto TheEnd;

	/*
	 * Ensure that inode isn't reused before
	 * lazy commit thread finishes processing
	 */
	if (tblk->xflag & (COMMIT_CREATE | COMMIT_DELETE)) {
		atomic_inc(&tblk->ip->i_count);
		/*
		 * Avoid a rare deadlock
		 *
		 * If the inode is locked, we may be blocked in
		 * jfs_commit_inode.  If so, we don't want the
		 * lazy_commit thread doing the last iput() on the inode
		 * since that may block on the locked inode.  Instead,
		 * commit the transaction synchronously, so the last iput
		 * will be done by the calling thread (or later)
		 */
		if (tblk->ip->i_state & I_LOCK)
			tblk->xflag &= ~COMMIT_LAZY;
	}

	ASSERT((!(tblk->xflag & COMMIT_DELETE)) ||
	       ((tblk->ip->i_nlink == 0) &&
		!test_cflag(COMMIT_Nolink, tblk->ip)));

	/*
	 *      write COMMIT log record
	 */
	lrd->type = cpu_to_le16(LOG_COMMIT);
	lrd->length = 0;
	lsn = lmLog(log, tblk, lrd, NULL);

	lmGroupCommit(log, tblk);

	/*
	 *      - transaction is now committed -
	 */

	/*
	 * force pages in careful update
	 * (imap addressing structure update)
	 */
	if (flag & COMMIT_FORCE)
		txForce(tblk);

	/*
	 *      update allocation map.
	 *
	 * update inode allocation map and inode:
	 * free pager lock on memory object of inode if any.
	 * update  block allocation map.
	 *
	 * txUpdateMap() resets XAD_NEW in XAD.
	 */
	if (tblk->xflag & COMMIT_FORCE)
		txUpdateMap(tblk);

	/*
	 *      free transaction locks and pageout/free pages
	 */
	txRelease(tblk);

	if ((tblk->flag & tblkGC_LAZY) == 0)
		txUnlock(tblk);


	/*
	 *      reset in-memory object state
	 */
	for (k = 0; k < cd.nip; k++) {
		ip = cd.iplist[k];
		jfs_ip = JFS_IP(ip);

		/*
		 * reset in-memory inode state
		 */
		jfs_ip->bxflag = 0;
		jfs_ip->blid = 0;
	}

      out:
	if (rc != 0)
		txAbort(tid, 1);

      TheEnd:
	jfs_info("txCommit: tid = %d, returning %d", tid, rc);
	return rc;
}


/*
 * NAME:        txLog()
 *
 * FUNCTION:    Writes AFTER log records for all lines modified
 *              by tid for segments specified by inodes in comdata.
 *              Code assumes only WRITELOCKS are recorded in lockwords.
 *
 * PARAMETERS:
 *
 * RETURN :
 */
static int txLog(struct jfs_log * log, struct tblock * tblk, struct commit * cd)
{
	int rc = 0;
	struct inode *ip;
	lid_t lid;
	struct tlock *tlck;
	struct lrd *lrd = &cd->lrd;

	/*
	 * write log record(s) for each tlock of transaction,
	 */
	for (lid = tblk->next; lid; lid = tlck->next) {
		tlck = lid_to_tlock(lid);

		tlck->flag |= tlckLOG;

		/* initialize lrd common */
		ip = tlck->ip;
		lrd->aggregate = cpu_to_le32(kdev_t_to_nr(ip->i_dev));
		lrd->log.redopage.fileset = cpu_to_le32(JFS_IP(ip)->fileset);
		lrd->log.redopage.inode = cpu_to_le32(ip->i_ino);

		/* write log record of page from the tlock */
		switch (tlck->type & tlckTYPE) {
		case tlckXTREE:
			xtLog(log, tblk, lrd, tlck);
			break;

		case tlckDTREE:
			dtLog(log, tblk, lrd, tlck);
			break;

		case tlckINODE:
			diLog(log, tblk, lrd, tlck, cd);
			break;

		case tlckMAP:
			mapLog(log, tblk, lrd, tlck);
			break;

		case tlckDATA:
			dataLog(log, tblk, lrd, tlck);
			break;

		default:
			jfs_err("UFO tlock:0x%p", tlck);
		}
	}

	return rc;
}


/*
 *      diLog()
 *
 * function:    log inode tlock and format maplock to update bmap;
 */
static int diLog(struct jfs_log * log, struct tblock * tblk, struct lrd * lrd,
	  struct tlock * tlck, struct commit * cd)
{
	int rc = 0;
	struct metapage *mp;
	pxd_t *pxd;
	struct pxd_lock *pxdlock;

	mp = tlck->mp;

	/* initialize as REDOPAGE record format */
	lrd->log.redopage.type = cpu_to_le16(LOG_INODE);
	lrd->log.redopage.l2linesize = cpu_to_le16(L2INODESLOTSIZE);

	pxd = &lrd->log.redopage.pxd;

	/*
	 *      inode after image
	 */
	if (tlck->type & tlckENTRY) {
		/* log after-image for logredo(): */
		lrd->type = cpu_to_le16(LOG_REDOPAGE);
//              *pxd = mp->cm_pxd;
		PXDaddress(pxd, mp->index);
		PXDlength(pxd,
			  mp->logical_size >> tblk->sb->s_blocksize_bits);
		lrd->backchain = cpu_to_le32(lmLog(log, tblk, lrd, tlck));

		/* mark page as homeward bound */
		tlck->flag |= tlckWRITEPAGE;
	} else if (tlck->type & tlckFREE) {
		/*
		 *      free inode extent
		 *
		 * (pages of the freed inode extent have been invalidated and
		 * a maplock for free of the extent has been formatted at
		 * txLock() time);
		 *
		 * the tlock had been acquired on the inode allocation map page
		 * (iag) that specifies the freed extent, even though the map
		 * page is not itself logged, to prevent pageout of the map
		 * page before the log;
		 */

		/* log LOG_NOREDOINOEXT of the freed inode extent for
		 * logredo() to start NoRedoPage filters, and to update
		 * imap and bmap for free of the extent;
		 */
		lrd->type = cpu_to_le16(LOG_NOREDOINOEXT);
		/*
		 * For the LOG_NOREDOINOEXT record, we need
		 * to pass the IAG number and inode extent
		 * index (within that IAG) from which the
		 * the extent being released.  These have been
		 * passed to us in the iplist[1] and iplist[2].
		 */
		lrd->log.noredoinoext.iagnum =
		    cpu_to_le32((u32) (size_t) cd->iplist[1]);
		lrd->log.noredoinoext.inoext_idx =
		    cpu_to_le32((u32) (size_t) cd->iplist[2]);

		pxdlock = (struct pxd_lock *) & tlck->lock;
		*pxd = pxdlock->pxd;
		lrd->backchain = cpu_to_le32(lmLog(log, tblk, lrd, NULL));

		/* update bmap */
		tlck->flag |= tlckUPDATEMAP;

		/* mark page as homeward bound */
		tlck->flag |= tlckWRITEPAGE;
	} else
		jfs_err("diLog: UFO type tlck:0x%p", tlck);
#ifdef  _JFS_WIP
	/*
	 *      alloc/free external EA extent
	 *
	 * a maplock for txUpdateMap() to update bPWMAP for alloc/free
	 * of the extent has been formatted at txLock() time;
	 */
	else {
		assert(tlck->type & tlckEA);

		/* log LOG_UPDATEMAP for logredo() to update bmap for
		 * alloc of new (and free of old) external EA extent;
		 */
		lrd->type = cpu_to_le16(LOG_UPDATEMAP);
		pxdlock = (struct pxd_lock *) & tlck->lock;
		nlock = pxdlock->index;
		for (i = 0; i < nlock; i++, pxdlock++) {
			if (pxdlock->flag & mlckALLOCPXD)
				lrd->log.updatemap.type =
				    cpu_to_le16(LOG_ALLOCPXD);
			else
				lrd->log.updatemap.type =
				    cpu_to_le16(LOG_FREEPXD);
			lrd->log.updatemap.nxd = cpu_to_le16(1);
			lrd->log.updatemap.pxd = pxdlock->pxd;
			lrd->backchain =
			    cpu_to_le32(lmLog(log, tblk, lrd, NULL));
		}

		/* update bmap */
		tlck->flag |= tlckUPDATEMAP;
	}
#endif				/* _JFS_WIP */

	return rc;
}


/*
 *      dataLog()
 *
 * function:    log data tlock
 */
static int dataLog(struct jfs_log * log, struct tblock * tblk, struct lrd * lrd,
	    struct tlock * tlck)
{
	struct metapage *mp;
	pxd_t *pxd;

	mp = tlck->mp;

	/* initialize as REDOPAGE record format */
	lrd->log.redopage.type = cpu_to_le16(LOG_DATA);
	lrd->log.redopage.l2linesize = cpu_to_le16(L2DATASLOTSIZE);

	pxd = &lrd->log.redopage.pxd;

	/* log after-image for logredo(): */
	lrd->type = cpu_to_le16(LOG_REDOPAGE);

	if (JFS_IP(tlck->ip)->next_index < MAX_INLINE_DIRTABLE_ENTRY) {
		/*
		 * The table has been truncated, we've must have deleted
		 * the last entry, so don't bother logging this
		 */
		mp->lid = 0;
		hold_metapage(mp, 0);
		atomic_dec(&mp->nohomeok);
		discard_metapage(mp);
		tlck->mp = 0;
		return 0;
	}

	PXDaddress(pxd, mp->index);
	PXDlength(pxd, mp->logical_size >> tblk->sb->s_blocksize_bits);

	lrd->backchain = cpu_to_le32(lmLog(log, tblk, lrd, tlck));

	/* mark page as homeward bound */
	tlck->flag |= tlckWRITEPAGE;

	return 0;
}


/*
 *      dtLog()
 *
 * function:    log dtree tlock and format maplock to update bmap;
 */
static void dtLog(struct jfs_log * log, struct tblock * tblk, struct lrd * lrd,
	   struct tlock * tlck)
{
	struct metapage *mp;
	struct pxd_lock *pxdlock;
	pxd_t *pxd;

	mp = tlck->mp;

	/* initialize as REDOPAGE/NOREDOPAGE record format */
	lrd->log.redopage.type = cpu_to_le16(LOG_DTREE);
	lrd->log.redopage.l2linesize = cpu_to_le16(L2DTSLOTSIZE);

	pxd = &lrd->log.redopage.pxd;

	if (tlck->type & tlckBTROOT)
		lrd->log.redopage.type |= cpu_to_le16(LOG_BTROOT);

	/*
	 *      page extension via relocation: entry insertion;
	 *      page extension in-place: entry insertion;
	 *      new right page from page split, reinitialized in-line
	 *      root from root page split: entry insertion;
	 */
	if (tlck->type & (tlckNEW | tlckEXTEND)) {
		/* log after-image of the new page for logredo():
		 * mark log (LOG_NEW) for logredo() to initialize
		 * freelist and update bmap for alloc of the new page;
		 */
		lrd->type = cpu_to_le16(LOG_REDOPAGE);
		if (tlck->type & tlckEXTEND)
			lrd->log.redopage.type |= cpu_to_le16(LOG_EXTEND);
		else
			lrd->log.redopage.type |= cpu_to_le16(LOG_NEW);
//              *pxd = mp->cm_pxd;
		PXDaddress(pxd, mp->index);
		PXDlength(pxd,
			  mp->logical_size >> tblk->sb->s_blocksize_bits);
		lrd->backchain = cpu_to_le32(lmLog(log, tblk, lrd, tlck));

		/* format a maplock for txUpdateMap() to update bPMAP for
		 * alloc of the new page;
		 */
		if (tlck->type & tlckBTROOT)
			return;
		tlck->flag |= tlckUPDATEMAP;
		pxdlock = (struct pxd_lock *) & tlck->lock;
		pxdlock->flag = mlckALLOCPXD;
		pxdlock->pxd = *pxd;

		pxdlock->index = 1;

		/* mark page as homeward bound */
		tlck->flag |= tlckWRITEPAGE;
		return;
	}

	/*
	 *      entry insertion/deletion,
	 *      sibling page link update (old right page before split);
	 */
	if (tlck->type & (tlckENTRY | tlckRELINK)) {
		/* log after-image for logredo(): */
		lrd->type = cpu_to_le16(LOG_REDOPAGE);
		PXDaddress(pxd, mp->index);
		PXDlength(pxd,
			  mp->logical_size >> tblk->sb->s_blocksize_bits);
		lrd->backchain = cpu_to_le32(lmLog(log, tblk, lrd, tlck));

		/* mark page as homeward bound */
		tlck->flag |= tlckWRITEPAGE;
		return;
	}

	/*
	 *      page deletion: page has been invalidated
	 *      page relocation: source extent
	 *
	 *      a maplock for free of the page has been formatted
	 *      at txLock() time);
	 */
	if (tlck->type & (tlckFREE | tlckRELOCATE)) {
		/* log LOG_NOREDOPAGE of the deleted page for logredo()
		 * to start NoRedoPage filter and to update bmap for free
		 * of the deletd page
		 */
		lrd->type = cpu_to_le16(LOG_NOREDOPAGE);
		pxdlock = (struct pxd_lock *) & tlck->lock;
		*pxd = pxdlock->pxd;
		lrd->backchain = cpu_to_le32(lmLog(log, tblk, lrd, NULL));

		/* a maplock for txUpdateMap() for free of the page
		 * has been formatted at txLock() time;
		 */
		tlck->flag |= tlckUPDATEMAP;
	}
	return;
}


/*
 *      xtLog()
 *
 * function:    log xtree tlock and format maplock to update bmap;
 */
static void xtLog(struct jfs_log * log, struct tblock * tblk, struct lrd * lrd,
	   struct tlock * tlck)
{
	struct inode *ip;
	struct metapage *mp;
	xtpage_t *p;
	struct xtlock *xtlck;
	struct maplock *maplock;
	struct xdlistlock *xadlock;
	struct pxd_lock *pxdlock;
	pxd_t *pxd;
	int next, lwm, hwm;

	ip = tlck->ip;
	mp = tlck->mp;

	/* initialize as REDOPAGE/NOREDOPAGE record format */
	lrd->log.redopage.type = cpu_to_le16(LOG_XTREE);
	lrd->log.redopage.l2linesize = cpu_to_le16(L2XTSLOTSIZE);

	pxd = &lrd->log.redopage.pxd;

	if (tlck->type & tlckBTROOT) {
		lrd->log.redopage.type |= cpu_to_le16(LOG_BTROOT);
		p = &JFS_IP(ip)->i_xtroot;
		if (S_ISDIR(ip->i_mode))
			lrd->log.redopage.type |=
			    cpu_to_le16(LOG_DIR_XTREE);
	} else
		p = (xtpage_t *) mp->data;
	next = le16_to_cpu(p->header.nextindex);

	xtlck = (struct xtlock *) & tlck->lock;

	maplock = (struct maplock *) & tlck->lock;
	xadlock = (struct xdlistlock *) maplock;

	/*
	 *      entry insertion/extension;
	 *      sibling page link update (old right page before split);
	 */
	if (tlck->type & (tlckNEW | tlckGROW | tlckRELINK)) {
		/* log after-image for logredo():
		 * logredo() will update bmap for alloc of new/extended
		 * extents (XAD_NEW|XAD_EXTEND) of XAD[lwm:next) from
		 * after-image of XADlist;
		 * logredo() resets (XAD_NEW|XAD_EXTEND) flag when
		 * applying the after-image to the meta-data page.
		 */
		lrd->type = cpu_to_le16(LOG_REDOPAGE);
//              *pxd = mp->cm_pxd;
		PXDaddress(pxd, mp->index);
		PXDlength(pxd,
			  mp->logical_size >> tblk->sb->s_blocksize_bits);
		lrd->backchain = cpu_to_le32(lmLog(log, tblk, lrd, tlck));

		/* format a maplock for txUpdateMap() to update bPMAP
		 * for alloc of new/extended extents of XAD[lwm:next)
		 * from the page itself;
		 * txUpdateMap() resets (XAD_NEW|XAD_EXTEND) flag.
		 */
		lwm = xtlck->lwm.offset;
		if (lwm == 0)
			lwm = XTPAGEMAXSLOT;

		if (lwm == next)
			goto out;
		if (lwm > next) {
			jfs_err("xtLog: lwm > next\n");
			goto out;
		}
		tlck->flag |= tlckUPDATEMAP;
		xadlock->flag = mlckALLOCXADLIST;
		xadlock->count = next - lwm;
		if ((xadlock->count <= 2) && (tblk->xflag & COMMIT_LAZY)) {
			int i;
			/*
			 * Lazy commit may allow xtree to be modified before
			 * txUpdateMap runs.  Copy xad into linelock to
			 * preserve correct data.
			 */
			xadlock->xdlist = &xtlck->pxdlock;
			memcpy(xadlock->xdlist, &p->xad[lwm],
			       sizeof(xad_t) * xadlock->count);

			for (i = 0; i < xadlock->count; i++)
				p->xad[lwm + i].flag &=
				    ~(XAD_NEW | XAD_EXTENDED);
		} else {
			/*
			 * xdlist will point to into inode's xtree, ensure
			 * that transaction is not committed lazily.
			 */
			xadlock->xdlist = &p->xad[lwm];
			tblk->xflag &= ~COMMIT_LAZY;
		}
		jfs_info("xtLog: alloc ip:0x%p mp:0x%p tlck:0x%p lwm:%d "
			 "count:%d", tlck->ip, mp, tlck, lwm, xadlock->count);

		maplock->index = 1;

	      out:
		/* mark page as homeward bound */
		tlck->flag |= tlckWRITEPAGE;

		return;
	}

	/*
	 *      page deletion: file deletion/truncation (ref. xtTruncate())
	 *
	 * (page will be invalidated after log is written and bmap
	 * is updated from the page);
	 */
	if (tlck->type & tlckFREE) {
		/* LOG_NOREDOPAGE log for NoRedoPage filter:
		 * if page free from file delete, NoRedoFile filter from
		 * inode image of zero link count will subsume NoRedoPage
		 * filters for each page;
		 * if page free from file truncattion, write NoRedoPage
		 * filter;
		 *
		 * upadte of block allocation map for the page itself:
		 * if page free from deletion and truncation, LOG_UPDATEMAP
		 * log for the page itself is generated from processing
		 * its parent page xad entries;
		 */
		/* if page free from file truncation, log LOG_NOREDOPAGE
		 * of the deleted page for logredo() to start NoRedoPage
		 * filter for the page;
		 */
		if (tblk->xflag & COMMIT_TRUNCATE) {
			/* write NOREDOPAGE for the page */
			lrd->type = cpu_to_le16(LOG_NOREDOPAGE);
			PXDaddress(pxd, mp->index);
			PXDlength(pxd,
				  mp->logical_size >> tblk->sb->
				  s_blocksize_bits);
			lrd->backchain =
			    cpu_to_le32(lmLog(log, tblk, lrd, NULL));

			if (tlck->type & tlckBTROOT) {
				/* Empty xtree must be logged */
				lrd->type = cpu_to_le16(LOG_REDOPAGE);
				lrd->backchain =
				    cpu_to_le32(lmLog(log, tblk, lrd, tlck));
			}
		}

		/* init LOG_UPDATEMAP of the freed extents
		 * XAD[XTENTRYSTART:hwm) from the deleted page itself
		 * for logredo() to update bmap;
		 */
		lrd->type = cpu_to_le16(LOG_UPDATEMAP);
		lrd->log.updatemap.type = cpu_to_le16(LOG_FREEXADLIST);
		xtlck = (struct xtlock *) & tlck->lock;
		hwm = xtlck->hwm.offset;
		lrd->log.updatemap.nxd =
		    cpu_to_le16(hwm - XTENTRYSTART + 1);
		/* reformat linelock for lmLog() */
		xtlck->header.offset = XTENTRYSTART;
		xtlck->header.length = hwm - XTENTRYSTART + 1;
		xtlck->index = 1;
		lrd->backchain = cpu_to_le32(lmLog(log, tblk, lrd, tlck));

		/* format a maplock for txUpdateMap() to update bmap
		 * to free extents of XAD[XTENTRYSTART:hwm) from the
		 * deleted page itself;
		 */
		tlck->flag |= tlckUPDATEMAP;
		xadlock->flag = mlckFREEXADLIST;
		xadlock->count = hwm - XTENTRYSTART + 1;
		if ((xadlock->count <= 2) && (tblk->xflag & COMMIT_LAZY)) {
			/*
			 * Lazy commit may allow xtree to be modified before
			 * txUpdateMap runs.  Copy xad into linelock to
			 * preserve correct data.
			 */
			xadlock->xdlist = &xtlck->pxdlock;
			memcpy(xadlock->xdlist, &p->xad[XTENTRYSTART],
			       sizeof(xad_t) * xadlock->count);
		} else {
			/*
			 * xdlist will point to into inode's xtree, ensure
			 * that transaction is not committed lazily.
			 */
			xadlock->xdlist = &p->xad[XTENTRYSTART];
			tblk->xflag &= ~COMMIT_LAZY;
		}
		jfs_info("xtLog: free ip:0x%p mp:0x%p count:%d lwm:2",
			 tlck->ip, mp, xadlock->count);

		maplock->index = 1;

		/* mark page as invalid */
		if (((tblk->xflag & COMMIT_PWMAP) || S_ISDIR(ip->i_mode))
		    && !(tlck->type & tlckBTROOT))
			tlck->flag |= tlckFREEPAGE;
		/*
		   else (tblk->xflag & COMMIT_PMAP)
		   ? release the page;
		 */
		return;
	}

	/*
	 *      page/entry truncation: file truncation (ref. xtTruncate())
	 *
	 *     |----------+------+------+---------------|
	 *                |      |      |
	 *                |      |     hwm - hwm before truncation
	 *                |     next - truncation point
	 *               lwm - lwm before truncation
	 * header ?
	 */
	if (tlck->type & tlckTRUNCATE) {
		pxd_t tpxd;	/* truncated extent of xad */
		int twm;

		/*
		 * For truncation the entire linelock may be used, so it would
		 * be difficult to store xad list in linelock itself.
		 * Therefore, we'll just force transaction to be committed
		 * synchronously, so that xtree pages won't be changed before
		 * txUpdateMap runs.
		 */
		tblk->xflag &= ~COMMIT_LAZY;
		lwm = xtlck->lwm.offset;
		if (lwm == 0)
			lwm = XTPAGEMAXSLOT;
		hwm = xtlck->hwm.offset;
		twm = xtlck->twm.offset;

		/*
		 *      write log records
		 */
		/* log after-image for logredo():
		 *
		 * logredo() will update bmap for alloc of new/extended
		 * extents (XAD_NEW|XAD_EXTEND) of XAD[lwm:next) from
		 * after-image of XADlist;
		 * logredo() resets (XAD_NEW|XAD_EXTEND) flag when
		 * applying the after-image to the meta-data page.
		 */
		lrd->type = cpu_to_le16(LOG_REDOPAGE);
		PXDaddress(pxd, mp->index);
		PXDlength(pxd, mp->logical_size >> tblk->sb->s_blocksize_bits);
		lrd->backchain = cpu_to_le32(lmLog(log, tblk, lrd, tlck));

		/*
		 * truncate entry XAD[twm == next - 1]:
		 */
		if (twm == next - 1) {
			/* init LOG_UPDATEMAP for logredo() to update bmap for
			 * free of truncated delta extent of the truncated
			 * entry XAD[next - 1]:
			 * (xtlck->pxdlock = truncated delta extent);
			 */
			pxdlock = (struct pxd_lock *) & xtlck->pxdlock;
			/* assert(pxdlock->type & tlckTRUNCATE); */
			lrd->type = cpu_to_le16(LOG_UPDATEMAP);
			lrd->log.updatemap.type = cpu_to_le16(LOG_FREEPXD);
			lrd->log.updatemap.nxd = cpu_to_le16(1);
			lrd->log.updatemap.pxd = pxdlock->pxd;
			tpxd = pxdlock->pxd;	/* save to format maplock */
			lrd->backchain =
			    cpu_to_le32(lmLog(log, tblk, lrd, NULL));
		}

		/*
		 * free entries XAD[next:hwm]:
		 */
		if (hwm >= next) {
			/* init LOG_UPDATEMAP of the freed extents
			 * XAD[next:hwm] from the deleted page itself
			 * for logredo() to update bmap;
			 */
			lrd->type = cpu_to_le16(LOG_UPDATEMAP);
			lrd->log.updatemap.type =
			    cpu_to_le16(LOG_FREEXADLIST);
			xtlck = (struct xtlock *) & tlck->lock;
			hwm = xtlck->hwm.offset;
			lrd->log.updatemap.nxd =
			    cpu_to_le16(hwm - next + 1);
			/* reformat linelock for lmLog() */
			xtlck->header.offset = next;
			xtlck->header.length = hwm - next + 1;
			xtlck->index = 1;
			lrd->backchain =
			    cpu_to_le32(lmLog(log, tblk, lrd, tlck));
		}

		/*
		 *      format maplock(s) for txUpdateMap() to update bmap
		 */
		maplock->index = 0;

		/*
		 * allocate entries XAD[lwm:next):
		 */
		if (lwm < next) {
			/* format a maplock for txUpdateMap() to update bPMAP
			 * for alloc of new/extended extents of XAD[lwm:next)
			 * from the page itself;
			 * txUpdateMap() resets (XAD_NEW|XAD_EXTEND) flag.
			 */
			tlck->flag |= tlckUPDATEMAP;
			xadlock->flag = mlckALLOCXADLIST;
			xadlock->count = next - lwm;
			xadlock->xdlist = &p->xad[lwm];

			jfs_info("xtLog: alloc ip:0x%p mp:0x%p count:%d "
				 "lwm:%d next:%d",
				 tlck->ip, mp, xadlock->count, lwm, next);
			maplock->index++;
			xadlock++;
		}

		/*
		 * truncate entry XAD[twm == next - 1]:
		 */
		if (twm == next - 1) {
			struct pxd_lock *pxdlock;

			/* format a maplock for txUpdateMap() to update bmap
			 * to free truncated delta extent of the truncated
			 * entry XAD[next - 1];
			 * (xtlck->pxdlock = truncated delta extent);
			 */
			tlck->flag |= tlckUPDATEMAP;
			pxdlock = (struct pxd_lock *) xadlock;
			pxdlock->flag = mlckFREEPXD;
			pxdlock->count = 1;
			pxdlock->pxd = tpxd;

			jfs_info("xtLog: truncate ip:0x%p mp:0x%p count:%d "
				 "hwm:%d", ip, mp, pxdlock->count, hwm);
			maplock->index++;
			xadlock++;
		}

		/*
		 * free entries XAD[next:hwm]:
		 */
		if (hwm >= next) {
			/* format a maplock for txUpdateMap() to update bmap
			 * to free extents of XAD[next:hwm] from thedeleted
			 * page itself;
			 */
			tlck->flag |= tlckUPDATEMAP;
			xadlock->flag = mlckFREEXADLIST;
			xadlock->count = hwm - next + 1;
			xadlock->xdlist = &p->xad[next];

			jfs_info("xtLog: free ip:0x%p mp:0x%p count:%d "
				 "next:%d hwm:%d",
				 tlck->ip, mp, xadlock->count, next, hwm);
			maplock->index++;
		}

		/* mark page as homeward bound */
		tlck->flag |= tlckWRITEPAGE;
	}
	return;
}


/*
 *      mapLog()
 *
 * function:    log from maplock of freed data extents;
 */
void mapLog(struct jfs_log * log, struct tblock * tblk, struct lrd * lrd,
	    struct tlock * tlck)
{
	struct pxd_lock *pxdlock;
	int i, nlock;
	pxd_t *pxd;

	/*
	 *      page relocation: free the source page extent
	 *
	 * a maplock for txUpdateMap() for free of the page
	 * has been formatted at txLock() time saving the src
	 * relocated page address;
	 */
	if (tlck->type & tlckRELOCATE) {
		/* log LOG_NOREDOPAGE of the old relocated page
		 * for logredo() to start NoRedoPage filter;
		 */
		lrd->type = cpu_to_le16(LOG_NOREDOPAGE);
		pxdlock = (struct pxd_lock *) & tlck->lock;
		pxd = &lrd->log.redopage.pxd;
		*pxd = pxdlock->pxd;
		lrd->backchain = cpu_to_le32(lmLog(log, tblk, lrd, NULL));

		/* (N.B. currently, logredo() does NOT update bmap
		 * for free of the page itself for (LOG_XTREE|LOG_NOREDOPAGE);
		 * if page free from relocation, LOG_UPDATEMAP log is
		 * specifically generated now for logredo()
		 * to update bmap for free of src relocated page;
		 * (new flag LOG_RELOCATE may be introduced which will
		 * inform logredo() to start NORedoPage filter and also
		 * update block allocation map at the same time, thus
		 * avoiding an extra log write);
		 */
		lrd->type = cpu_to_le16(LOG_UPDATEMAP);
		lrd->log.updatemap.type = cpu_to_le16(LOG_FREEPXD);
		lrd->log.updatemap.nxd = cpu_to_le16(1);
		lrd->log.updatemap.pxd = pxdlock->pxd;
		lrd->backchain = cpu_to_le32(lmLog(log, tblk, lrd, NULL));

		/* a maplock for txUpdateMap() for free of the page
		 * has been formatted at txLock() time;
		 */
		tlck->flag |= tlckUPDATEMAP;
		return;
	}
	/*

	 * Otherwise it's not a relocate request
	 *
	 */
	else {
		/* log LOG_UPDATEMAP for logredo() to update bmap for
		 * free of truncated/relocated delta extent of the data;
		 * e.g.: external EA extent, relocated/truncated extent
		 * from xtTailgate();
		 */
		lrd->type = cpu_to_le16(LOG_UPDATEMAP);
		pxdlock = (struct pxd_lock *) & tlck->lock;
		nlock = pxdlock->index;
		for (i = 0; i < nlock; i++, pxdlock++) {
			if (pxdlock->flag & mlckALLOCPXD)
				lrd->log.updatemap.type =
				    cpu_to_le16(LOG_ALLOCPXD);
			else
				lrd->log.updatemap.type =
				    cpu_to_le16(LOG_FREEPXD);
			lrd->log.updatemap.nxd = cpu_to_le16(1);
			lrd->log.updatemap.pxd = pxdlock->pxd;
			lrd->backchain =
			    cpu_to_le32(lmLog(log, tblk, lrd, NULL));
			jfs_info("mapLog: xaddr:0x%lx xlen:0x%x",
				 (ulong) addressPXD(&pxdlock->pxd),
				 lengthPXD(&pxdlock->pxd));
		}

		/* update bmap */
		tlck->flag |= tlckUPDATEMAP;
	}
}


/*
 *      txEA()
 *
 * function:    acquire maplock for EA/ACL extents or
 *              set COMMIT_INLINE flag;
 */
void txEA(tid_t tid, struct inode *ip, dxd_t * oldea, dxd_t * newea)
{
	struct tlock *tlck = NULL;
	struct pxd_lock *maplock = NULL, *pxdlock = NULL;

	/*
	 * format maplock for alloc of new EA extent
	 */
	if (newea) {
		/* Since the newea could be a completely zeroed entry we need to
		 * check for the two flags which indicate we should actually
		 * commit new EA data
		 */
		if (newea->flag & DXD_EXTENT) {
			tlck = txMaplock(tid, ip, tlckMAP);
			maplock = (struct pxd_lock *) & tlck->lock;
			pxdlock = (struct pxd_lock *) maplock;
			pxdlock->flag = mlckALLOCPXD;
			PXDaddress(&pxdlock->pxd, addressDXD(newea));
			PXDlength(&pxdlock->pxd, lengthDXD(newea));
			pxdlock++;
			maplock->index = 1;
		} else if (newea->flag & DXD_INLINE) {
			tlck = NULL;

			set_cflag(COMMIT_Inlineea, ip);
		}
	}

	/*
	 * format maplock for free of old EA extent
	 */
	if (!test_cflag(COMMIT_Nolink, ip) && oldea->flag & DXD_EXTENT) {
		if (tlck == NULL) {
			tlck = txMaplock(tid, ip, tlckMAP);
			maplock = (struct pxd_lock *) & tlck->lock;
			pxdlock = (struct pxd_lock *) maplock;
			maplock->index = 0;
		}
		pxdlock->flag = mlckFREEPXD;
		PXDaddress(&pxdlock->pxd, addressDXD(oldea));
		PXDlength(&pxdlock->pxd, lengthDXD(oldea));
		maplock->index++;
	}
}


/*
 *      txForce()
 *
 * function: synchronously write pages locked by transaction
 *              after txLog() but before txUpdateMap();
 */
void txForce(struct tblock * tblk)
{
	struct tlock *tlck;
	lid_t lid, next;
	struct metapage *mp;

	/*
	 * reverse the order of transaction tlocks in
	 * careful update order of address index pages
	 * (right to left, bottom up)
	 */
	tlck = lid_to_tlock(tblk->next);
	lid = tlck->next;
	tlck->next = 0;
	while (lid) {
		tlck = lid_to_tlock(lid);
		next = tlck->next;
		tlck->next = tblk->next;
		tblk->next = lid;
		lid = next;
	}

	/*
	 * synchronously write the page, and
	 * hold the page for txUpdateMap();
	 */
	for (lid = tblk->next; lid; lid = next) {
		tlck = lid_to_tlock(lid);
		next = tlck->next;

		if ((mp = tlck->mp) != NULL &&
		    (tlck->type & tlckBTROOT) == 0) {
			assert(mp->xflag & COMMIT_PAGE);

			if (tlck->flag & tlckWRITEPAGE) {
				tlck->flag &= ~tlckWRITEPAGE;

				/* do not release page to freelist */

				/*
				 * The "right" thing to do here is to
				 * synchronously write the metadata.
				 * With the current implementation this
				 * is hard since write_metapage requires
				 * us to kunmap & remap the page.  If we
				 * have tlocks pointing into the metadata
				 * pages, we don't want to do this.  I think
				 * we can get by with synchronously writing
				 * the pages when they are released.
				 */
				assert(atomic_read(&mp->nohomeok));
				set_bit(META_dirty, &mp->flag);
				set_bit(META_sync, &mp->flag);
			}
		}
	}
}


/*
 *      txUpdateMap()
 *
 * function:    update persistent allocation map (and working map
 *              if appropriate);
 *
 * parameter:
 */
static void txUpdateMap(struct tblock * tblk)
{
	struct inode *ip;
	struct inode *ipimap;
	lid_t lid;
	struct tlock *tlck;
	struct maplock *maplock;
	struct pxd_lock pxdlock;
	int maptype;
	int k, nlock;
	struct metapage *mp = 0;

	ipimap = JFS_SBI(tblk->sb)->ipimap;

	maptype = (tblk->xflag & COMMIT_PMAP) ? COMMIT_PMAP : COMMIT_PWMAP;


	/*
	 *      update block allocation map
	 *
	 * update allocation state in pmap (and wmap) and
	 * update lsn of the pmap page;
	 */
	/*
	 * scan each tlock/page of transaction for block allocation/free:
	 *
	 * for each tlock/page of transaction, update map.
	 *  ? are there tlock for pmap and pwmap at the same time ?
	 */
	for (lid = tblk->next; lid; lid = tlck->next) {
		tlck = lid_to_tlock(lid);

		if ((tlck->flag & tlckUPDATEMAP) == 0)
			continue;

		if (tlck->flag & tlckFREEPAGE) {
			/*
			 * Another thread may attempt to reuse freed space
			 * immediately, so we want to get rid of the metapage
			 * before anyone else has a chance to get it.
			 * Lock metapage, update maps, then invalidate
			 * the metapage.
			 */
			mp = tlck->mp;
			ASSERT(mp->xflag & COMMIT_PAGE);
			hold_metapage(mp, 0);
		}

		/*
		 * extent list:
		 * . in-line PXD list:
		 * . out-of-line XAD list:
		 */
		maplock = (struct maplock *) & tlck->lock;
		nlock = maplock->index;

		for (k = 0; k < nlock; k++, maplock++) {
			/*
			 * allocate blocks in persistent map:
			 *
			 * blocks have been allocated from wmap at alloc time;
			 */
			if (maplock->flag & mlckALLOC) {
				txAllocPMap(ipimap, maplock, tblk);
			}
			/*
			 * free blocks in persistent and working map:
			 * blocks will be freed in pmap and then in wmap;
			 *
			 * ? tblock specifies the PMAP/PWMAP based upon
			 * transaction
			 *
			 * free blocks in persistent map:
			 * blocks will be freed from wmap at last reference
			 * release of the object for regular files;
			 *
			 * Alway free blocks from both persistent & working
			 * maps for directories
			 */
			else {	/* (maplock->flag & mlckFREE) */

				if (S_ISDIR(tlck->ip->i_mode))
					txFreeMap(ipimap, maplock,
						  tblk, COMMIT_PWMAP);
				else
					txFreeMap(ipimap, maplock,
						  tblk, maptype);
			}
		}
		if (tlck->flag & tlckFREEPAGE) {
			if (!(tblk->flag & tblkGC_LAZY)) {
				/* This is equivalent to txRelease */
				ASSERT(mp->lid == lid);
				tlck->mp->lid = 0;
			}
			assert(atomic_read(&mp->nohomeok) == 1);
			atomic_dec(&mp->nohomeok);
			discard_metapage(mp);
			tlck->mp = 0;
		}
	}
	/*
	 *      update inode allocation map
	 *
	 * update allocation state in pmap and
	 * update lsn of the pmap page;
	 * update in-memory inode flag/state
	 *
	 * unlock mapper/write lock
	 */
	if (tblk->xflag & COMMIT_CREATE) {
		ip = tblk->ip;

		ASSERT(test_cflag(COMMIT_New, ip));
		clear_cflag(COMMIT_New, ip);

		diUpdatePMap(ipimap, ip->i_ino, FALSE, tblk);
		ipimap->i_state |= I_DIRTY;
		/* update persistent block allocation map
		 * for the allocation of inode extent;
		 */
		pxdlock.flag = mlckALLOCPXD;
		pxdlock.pxd = JFS_IP(ip)->ixpxd;
		pxdlock.index = 1;
		txAllocPMap(ip, (struct maplock *) & pxdlock, tblk);
		iput(ip);
	} else if (tblk->xflag & COMMIT_DELETE) {
		ip = tblk->ip;
		diUpdatePMap(ipimap, ip->i_ino, TRUE, tblk);
		ipimap->i_state |= I_DIRTY;
		iput(ip);
	}
}


/*
 *      txAllocPMap()
 *
 * function: allocate from persistent map;
 *
 * parameter:
 *      ipbmap  -
 *      malock -
 *              xad list:
 *              pxd:
 *
 *      maptype -
 *              allocate from persistent map;
 *              free from persistent map;
 *              (e.g., tmp file - free from working map at releae
 *               of last reference);
 *              free from persistent and working map;
 *
 *      lsn     - log sequence number;
 */
static void txAllocPMap(struct inode *ip, struct maplock * maplock,
			struct tblock * tblk)
{
	struct inode *ipbmap = JFS_SBI(ip->i_sb)->ipbmap;
	struct xdlistlock *xadlistlock;
	xad_t *xad;
	s64 xaddr;
	int xlen;
	struct pxd_lock *pxdlock;
	struct xdlistlock *pxdlistlock;
	pxd_t *pxd;
	int n;

	/*
	 * allocate from persistent map;
	 */
	if (maplock->flag & mlckALLOCXADLIST) {
		xadlistlock = (struct xdlistlock *) maplock;
		xad = xadlistlock->xdlist;
		for (n = 0; n < xadlistlock->count; n++, xad++) {
			if (xad->flag & (XAD_NEW | XAD_EXTENDED)) {
				xaddr = addressXAD(xad);
				xlen = lengthXAD(xad);
				dbUpdatePMap(ipbmap, FALSE, xaddr,
					     (s64) xlen, tblk);
				xad->flag &= ~(XAD_NEW | XAD_EXTENDED);
				jfs_info("allocPMap: xaddr:0x%lx xlen:%d",
					 (ulong) xaddr, xlen);
			}
		}
	} else if (maplock->flag & mlckALLOCPXD) {
		pxdlock = (struct pxd_lock *) maplock;
		xaddr = addressPXD(&pxdlock->pxd);
		xlen = lengthPXD(&pxdlock->pxd);
		dbUpdatePMap(ipbmap, FALSE, xaddr, (s64) xlen, tblk);
		jfs_info("allocPMap: xaddr:0x%lx xlen:%d", (ulong) xaddr, xlen);
	} else {		/* (maplock->flag & mlckALLOCPXDLIST) */

		pxdlistlock = (struct xdlistlock *) maplock;
		pxd = pxdlistlock->xdlist;
		for (n = 0; n < pxdlistlock->count; n++, pxd++) {
			xaddr = addressPXD(pxd);
			xlen = lengthPXD(pxd);
			dbUpdatePMap(ipbmap, FALSE, xaddr, (s64) xlen,
				     tblk);
			jfs_info("allocPMap: xaddr:0x%lx xlen:%d",
				 (ulong) xaddr, xlen);
		}
	}
}


/*
 *      txFreeMap()
 *
 * function:    free from persistent and/or working map;
 *
 * todo: optimization
 */
void txFreeMap(struct inode *ip,
	       struct maplock * maplock, struct tblock * tblk, int maptype)
{
	struct inode *ipbmap = JFS_SBI(ip->i_sb)->ipbmap;
	struct xdlistlock *xadlistlock;
	xad_t *xad;
	s64 xaddr;
	int xlen;
	struct pxd_lock *pxdlock;
	struct xdlistlock *pxdlistlock;
	pxd_t *pxd;
	int n;

	jfs_info("txFreeMap: tblk:0x%p maplock:0x%p maptype:0x%x",
		 tblk, maplock, maptype);

	/*
	 * free from persistent map;
	 */
	if (maptype == COMMIT_PMAP || maptype == COMMIT_PWMAP) {
		if (maplock->flag & mlckFREEXADLIST) {
			xadlistlock = (struct xdlistlock *) maplock;
			xad = xadlistlock->xdlist;
			for (n = 0; n < xadlistlock->count; n++, xad++) {
				if (!(xad->flag & XAD_NEW)) {
					xaddr = addressXAD(xad);
					xlen = lengthXAD(xad);
					dbUpdatePMap(ipbmap, TRUE, xaddr,
						     (s64) xlen, tblk);
					jfs_info("freePMap: xaddr:0x%lx "
						 "xlen:%d",
						 (ulong) xaddr, xlen);
				}
			}
		} else if (maplock->flag & mlckFREEPXD) {
			pxdlock = (struct pxd_lock *) maplock;
			xaddr = addressPXD(&pxdlock->pxd);
			xlen = lengthPXD(&pxdlock->pxd);
			dbUpdatePMap(ipbmap, TRUE, xaddr, (s64) xlen,
				     tblk);
			jfs_info("freePMap: xaddr:0x%lx xlen:%d",
				 (ulong) xaddr, xlen);
		} else {	/* (maplock->flag & mlckALLOCPXDLIST) */

			pxdlistlock = (struct xdlistlock *) maplock;
			pxd = pxdlistlock->xdlist;
			for (n = 0; n < pxdlistlock->count; n++, pxd++) {
				xaddr = addressPXD(pxd);
				xlen = lengthPXD(pxd);
				dbUpdatePMap(ipbmap, TRUE, xaddr,
					     (s64) xlen, tblk);
				jfs_info("freePMap: xaddr:0x%lx xlen:%d",
					 (ulong) xaddr, xlen);
			}
		}
	}

	/*
	 * free from working map;
	 */
	if (maptype == COMMIT_PWMAP || maptype == COMMIT_WMAP) {
		if (maplock->flag & mlckFREEXADLIST) {
			xadlistlock = (struct xdlistlock *) maplock;
			xad = xadlistlock->xdlist;
			for (n = 0; n < xadlistlock->count; n++, xad++) {
				xaddr = addressXAD(xad);
				xlen = lengthXAD(xad);
				dbFree(ip, xaddr, (s64) xlen);
				xad->flag = 0;
				jfs_info("freeWMap: xaddr:0x%lx xlen:%d",
					 (ulong) xaddr, xlen);
			}
		} else if (maplock->flag & mlckFREEPXD) {
			pxdlock = (struct pxd_lock *) maplock;
			xaddr = addressPXD(&pxdlock->pxd);
			xlen = lengthPXD(&pxdlock->pxd);
			dbFree(ip, xaddr, (s64) xlen);
			jfs_info("freeWMap: xaddr:0x%lx xlen:%d",
				 (ulong) xaddr, xlen);
		} else {	/* (maplock->flag & mlckFREEPXDLIST) */

			pxdlistlock = (struct xdlistlock *) maplock;
			pxd = pxdlistlock->xdlist;
			for (n = 0; n < pxdlistlock->count; n++, pxd++) {
				xaddr = addressPXD(pxd);
				xlen = lengthPXD(pxd);
				dbFree(ip, xaddr, (s64) xlen);
				jfs_info("freeWMap: xaddr:0x%lx xlen:%d",
					 (ulong) xaddr, xlen);
			}
		}
	}
}


/*
 *      txFreelock()
 *
 * function:    remove tlock from inode anonymous locklist
 */
void txFreelock(struct inode *ip)
{
	struct jfs_inode_info *jfs_ip = JFS_IP(ip);
	struct tlock *xtlck, *tlck;
	lid_t xlid = 0, lid;

	if (!jfs_ip->atlhead)
		return;

	xtlck = (struct tlock *) &jfs_ip->atlhead;

	while ((lid = xtlck->next)) {
		tlck = lid_to_tlock(lid);
		if (tlck->flag & tlckFREELOCK) {
			xtlck->next = tlck->next;
			txLockFree(lid);
		} else {
			xtlck = tlck;
			xlid = lid;
		}
	}

	if (jfs_ip->atlhead)
		jfs_ip->atltail = xlid;
	else {
		jfs_ip->atltail = 0;
		/*
		 * If inode was on anon_list, remove it
		 */
		TXN_LOCK();
		list_del_init(&jfs_ip->anon_inode_list);
		TXN_UNLOCK();
	}
}


/*
 *      txAbort()
 *
 * function: abort tx before commit;
 *
 * frees line-locks and segment locks for all
 * segments in comdata structure.
 * Optionally sets state of file-system to FM_DIRTY in super-block.
 * log age of page-frames in memory for which caller has
 * are reset to 0 (to avoid logwarap).
 */
void txAbort(tid_t tid, int dirty)
{
	lid_t lid, next;
	struct metapage *mp;
	struct tblock *tblk = tid_to_tblock(tid);
	struct tlock *tlck;

	jfs_warn("txAbort: tid:%d dirty:0x%x", tid, dirty);

	/*
	 * free tlocks of the transaction
	 */
	for (lid = tblk->next; lid; lid = next) {
		tlck = lid_to_tlock(lid);
		next = tlck->next;
		mp = tlck->mp;
		JFS_IP(tlck->ip)->xtlid = 0;

		if (mp) {
			mp->lid = 0;

			/*
			 * reset lsn of page to avoid logwarap:
			 *
			 * (page may have been previously committed by another
			 * transaction(s) but has not been paged, i.e.,
			 * it may be on logsync list even though it has not
			 * been logged for the current tx.)
			 */
			if (mp->xflag & COMMIT_PAGE && mp->lsn)
				LogSyncRelease(mp);
		}
		/* insert tlock at head of freelist */
		TXN_LOCK();
		txLockFree(lid);
		TXN_UNLOCK();
	}

	/* caller will free the transaction block */

	tblk->next = tblk->last = 0;

	/*
	 * mark filesystem dirty
	 */
	if (dirty)
		jfs_error(tblk->sb, "txAbort");

	return;
}

/*
 *      txLazyCommit(void)
 *
 *	All transactions except those changing ipimap (COMMIT_FORCE) are
 *	processed by this routine.  This insures that the inode and block
 *	allocation maps are updated in order.  For synchronous transactions,
 *	let the user thread finish processing after txUpdateMap() is called.
 */
static void txLazyCommit(struct tblock * tblk)
{
	struct jfs_log *log;

	while (((tblk->flag & tblkGC_READY) == 0) &&
	       ((tblk->flag & tblkGC_UNLOCKED) == 0)) {
		/* We must have gotten ahead of the user thread
		 */
		jfs_info("txLazyCommit: tblk 0x%p not unlocked", tblk);
		yield();
	}

	jfs_info("txLazyCommit: processing tblk 0x%p", tblk);

	txUpdateMap(tblk);

	log = (struct jfs_log *) JFS_SBI(tblk->sb)->log;

	spin_lock_irq(&log->gclock);	// LOGGC_LOCK

	tblk->flag |= tblkGC_COMMITTED;

	if (tblk->flag & tblkGC_READY)
		log->gcrtc--;

	wake_up_all(&tblk->gcwait);	// LOGGC_WAKEUP

	/*
	 * Can't release log->gclock until we've tested tblk->flag
	 */
	if (tblk->flag & tblkGC_LAZY) {
		spin_unlock_irq(&log->gclock);	// LOGGC_UNLOCK
		txUnlock(tblk);
		tblk->flag &= ~tblkGC_LAZY;
		txEnd(tblk - TxBlock);	/* Convert back to tid */
	} else
		spin_unlock_irq(&log->gclock);	// LOGGC_UNLOCK

	jfs_info("txLazyCommit: done: tblk = 0x%p", tblk);
}

/*
 *      jfs_lazycommit(void)
 *
 *	To be run as a kernel daemon.  If lbmIODone is called in an interrupt
 *	context, or where blocking is not wanted, this routine will process
 *	committed transactions from the unlock queue.
 */
int jfs_lazycommit(void *arg)
{
	int WorkDone;
	struct tblock *tblk;
	unsigned long flags;

	lock_kernel();

	daemonize();
	current->tty = NULL;
	strcpy(current->comm, "jfsCommit");

	unlock_kernel();

	jfsCommitTask = current;

	spin_lock_irq(&current->sighand->siglock);
	sigfillset(&current->blocked);
	recalc_sigpending();
	spin_unlock_irq(&current->sighand->siglock);

	LAZY_LOCK_INIT();
	TxAnchor.unlock_queue = TxAnchor.unlock_tail = 0;

	complete(&jfsIOwait);

	do {
		DECLARE_WAITQUEUE(wq, current);

		LAZY_LOCK(flags);
restart:
		WorkDone = 0;
		while ((tblk = TxAnchor.unlock_queue)) {
			/*
			 * We can't get ahead of user thread.  Spinning is
			 * simpler than blocking/waking.  We shouldn't spin
			 * very long, since user thread shouldn't be blocking
			 * between lmGroupCommit & txEnd.
			 */
			WorkDone = 1;

			/*
			 * Remove first transaction from queue
			 */
			TxAnchor.unlock_queue = tblk->cqnext;
			tblk->cqnext = 0;
			if (TxAnchor.unlock_tail == tblk)
				TxAnchor.unlock_tail = 0;

			LAZY_UNLOCK(flags);
			txLazyCommit(tblk);

			/*
			 * We can be running indefinately if other processors
			 * are adding transactions to this list
			 */
			cond_resched();
			LAZY_LOCK(flags);
		}

		if (WorkDone)
			goto restart;

		add_wait_queue(&jfs_commit_thread_wait, &wq);
		set_current_state(TASK_INTERRUPTIBLE);
		LAZY_UNLOCK(flags);
		schedule();
		current->state = TASK_RUNNING;
		remove_wait_queue(&jfs_commit_thread_wait, &wq);
	} while (!jfs_stop_threads);

	if (TxAnchor.unlock_queue)
		jfs_err("jfs_lazycommit being killed w/pending transactions!");
	else
		jfs_info("jfs_lazycommit being killed\n");
	complete_and_exit(&jfsIOwait, 0);
}

void txLazyUnlock(struct tblock * tblk)
{
	unsigned long flags;

	LAZY_LOCK(flags);

	if (TxAnchor.unlock_tail)
		TxAnchor.unlock_tail->cqnext = tblk;
	else
		TxAnchor.unlock_queue = tblk;
	TxAnchor.unlock_tail = tblk;
	tblk->cqnext = 0;
	LAZY_UNLOCK(flags);
	wake_up(&jfs_commit_thread_wait);
}

static void LogSyncRelease(struct metapage * mp)
{
	struct jfs_log *log = mp->log;

	assert(atomic_read(&mp->nohomeok));
	assert(log);
	atomic_dec(&mp->nohomeok);

	if (atomic_read(&mp->nohomeok))
		return;

	hold_metapage(mp, 0);

	LOGSYNC_LOCK(log);
	mp->log = NULL;
	mp->lsn = 0;
	mp->clsn = 0;
	log->count--;
	list_del_init(&mp->synclist);
	LOGSYNC_UNLOCK(log);

	release_metapage(mp);
}

/*
 *	txQuiesce
 *
 *	Block all new transactions and push anonymous transactions to
 *	completion
 *
 *	This does almost the same thing as jfs_sync below.  We don't
 *	worry about deadlocking when TlocksLow is set, since we would
 *	expect jfs_sync to get us out of that jam.
 */
void txQuiesce(struct super_block *sb)
{
	struct inode *ip;
	struct jfs_inode_info *jfs_ip;
	struct jfs_log *log = JFS_SBI(sb)->log;
	tid_t tid;

	set_bit(log_QUIESCE, &log->flag);

	TXN_LOCK();
restart:
	while (!list_empty(&TxAnchor.anon_list)) {
		jfs_ip = list_entry(TxAnchor.anon_list.next,
				    struct jfs_inode_info,
				    anon_inode_list);
		ip = jfs_ip->inode;

		/*
		 * inode will be removed from anonymous list
		 * when it is committed
		 */
		TXN_UNLOCK();
		tid = txBegin(ip->i_sb, COMMIT_INODE | COMMIT_FORCE);
		down(&jfs_ip->commit_sem);
		txCommit(tid, 1, &ip, 0);
		txEnd(tid);
		up(&jfs_ip->commit_sem);
		/*
		 * Just to be safe.  I don't know how
		 * long we can run without blocking
		 */
		cond_resched();
		TXN_LOCK();
	}

	/*
	 * If jfs_sync is running in parallel, there could be some inodes
	 * on anon_list2.  Let's check.
	 */
	if (!list_empty(&TxAnchor.anon_list2)) {
		list_splice(&TxAnchor.anon_list2, &TxAnchor.anon_list);
		INIT_LIST_HEAD(&TxAnchor.anon_list2);
		goto restart;
	}
	TXN_UNLOCK();

	/*
	 * We may need to kick off the group commit
	 */
	jfs_flush_journal(log, 0);
}

/*
 * txResume()
 *
 * Allows transactions to start again following txQuiesce
 */
void txResume(struct super_block *sb)
{
	struct jfs_log *log = JFS_SBI(sb)->log;

	clear_bit(log_QUIESCE, &log->flag);
	TXN_WAKEUP(&log->syncwait);
}

/*
 *      jfs_sync(void)
 *
 *	To be run as a kernel daemon.  This is awakened when tlocks run low.
 *	We write any inodes that have anonymous tlocks so they will become
 *	available.
 */
int jfs_sync(void *arg)
{
	struct inode *ip;
	struct jfs_inode_info *jfs_ip;
	int rc;
	tid_t tid;

	lock_kernel();

	daemonize();
	current->tty = NULL;
	strcpy(current->comm, "jfsSync");

	unlock_kernel();

	spin_lock_irq(&current->sighand->siglock);
	sigfillset(&current->blocked);
	recalc_sigpending();
	spin_unlock_irq(&current->sighand->siglock);

	complete(&jfsIOwait);

	do {
		DECLARE_WAITQUEUE(wq, current);
		/*
		 * write each inode on the anonymous inode list
		 */
		TXN_LOCK();
		while (TxAnchor.TlocksLow && !list_empty(&TxAnchor.anon_list)) {
			jfs_ip = list_entry(TxAnchor.anon_list.next,
					    struct jfs_inode_info,
					    anon_inode_list);
			ip = jfs_ip->inode;

			if (! igrab(ip)) {
				/*
				 * Inode is being freed
				 */
				list_del_init(&jfs_ip->anon_inode_list);
			} else if (! down_trylock(&jfs_ip->commit_sem)) {
				/*
				 * inode will be removed from anonymous list
				 * when it is committed
				 */
				TXN_UNLOCK();
				tid = txBegin(ip->i_sb, COMMIT_INODE);
				rc = txCommit(tid, 1, &ip, 0);
				txEnd(tid);
				up(&jfs_ip->commit_sem);

				iput(ip);
				/*
				 * Just to be safe.  I don't know how
				 * long we can run without blocking
				 */
				cond_resched();
				TXN_LOCK();
			} else {
				/* We can't get the commit semaphore.  It may
				 * be held by a thread waiting for tlock's
				 * so let's not block here.  Save it to
				 * put back on the anon_list.
				 */

				/* Take off anon_list */
				list_del(&jfs_ip->anon_inode_list);

				/* Put on anon_list2 */
				list_add(&jfs_ip->anon_inode_list,
					 &TxAnchor.anon_list2);

				TXN_UNLOCK();
				iput(ip);
				TXN_LOCK();
			}
		}
		/* Add anon_list2 back to anon_list */
		if (!list_empty(&TxAnchor.anon_list2)) {
			list_splice(&TxAnchor.anon_list2, &TxAnchor.anon_list);
			INIT_LIST_HEAD(&TxAnchor.anon_list2);
		}
		add_wait_queue(&jfs_sync_thread_wait, &wq);
		set_current_state(TASK_INTERRUPTIBLE);
		TXN_UNLOCK();
		schedule();
		current->state = TASK_RUNNING;
		remove_wait_queue(&jfs_sync_thread_wait, &wq);
	} while (!jfs_stop_threads);

	jfs_info("jfs_sync being killed");
	complete_and_exit(&jfsIOwait, 0);
}

#if defined(CONFIG_PROC_FS) && defined(CONFIG_JFS_DEBUG)
int jfs_txanchor_read(char *buffer, char **start, off_t offset, int length,
		      int *eof, void *data)
{
	int len = 0;
	off_t begin;
	char *freewait;
	char *freelockwait;
	char *lowlockwait;

	freewait =
	    waitqueue_active(&TxAnchor.freewait) ? "active" : "empty";
	freelockwait =
	    waitqueue_active(&TxAnchor.freelockwait) ? "active" : "empty";
	lowlockwait =
	    waitqueue_active(&TxAnchor.lowlockwait) ? "active" : "empty";

	len += sprintf(buffer,
		       "JFS TxAnchor\n"
		       "============\n"
		       "freetid = %d\n"
		       "freewait = %s\n"
		       "freelock = %d\n"
		       "freelockwait = %s\n"
		       "lowlockwait = %s\n"
		       "tlocksInUse = %d\n"
		       "TlocksLow = %d\n"
		       "unlock_queue = 0x%p\n"
		       "unlock_tail = 0x%p\n",
		       TxAnchor.freetid,
		       freewait,
		       TxAnchor.freelock,
		       freelockwait,
		       lowlockwait,
		       TxAnchor.tlocksInUse,
		       TxAnchor.TlocksLow,
		       TxAnchor.unlock_queue,
		       TxAnchor.unlock_tail);

	begin = offset;
	*start = buffer + begin;
	len -= begin;

	if (len > length)
		len = length;
	else
		*eof = 1;

	if (len < 0)
		len = 0;

	return len;
}
#endif

#if defined(CONFIG_PROC_FS) && defined(CONFIG_JFS_STATISTICS)
int jfs_txstats_read(char *buffer, char **start, off_t offset, int length,
		     int *eof, void *data)
{
	int len = 0;
	off_t begin;

	len += sprintf(buffer,
		       "JFS TxStats\n"
		       "===========\n"
		       "calls to txBegin = %d\n"
		       "txBegin blocked by sync barrier = %d\n"
		       "txBegin blocked by tlocks low = %d\n"
		       "txBegin blocked by no free tid = %d\n"
		       "calls to txBeginAnon = %d\n"
		       "txBeginAnon blocked by sync barrier = %d\n"
		       "txBeginAnon blocked by tlocks low = %d\n"
		       "calls to txLockAlloc = %d\n"
		       "tLockAlloc blocked by no free lock = %d\n",
		       TxStat.txBegin,
		       TxStat.txBegin_barrier,
		       TxStat.txBegin_lockslow,
		       TxStat.txBegin_freetid,
		       TxStat.txBeginAnon,
		       TxStat.txBeginAnon_barrier,
		       TxStat.txBeginAnon_lockslow,
		       TxStat.txLockAlloc,
		       TxStat.txLockAlloc_freelock);

	begin = offset;
	*start = buffer + begin;
	len -= begin;

	if (len > length)
		len = length;
	else
		*eof = 1;

	if (len < 0)
		len = 0;

	return len;
}
#endif
