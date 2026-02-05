/* Copyright (c) 2019,2026 Michael Ackermann, aggi
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public Version 2 License as published by the Free
 * Software Foundation;
 *
 * ultra fast PRNG utilizing non-primitive recurse LFSR matrix seeded with timer
 * see http://www.billauer.co.il frandom RC4 version
 *
*/
#include <linux/version.h>
#include <linux/module.h>
#include <linux/config.h>

#include <linux/miscdevice.h>

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/random.h>

// entropy source
#include <linux/time.h>
#include <linux/timex.h>

#include <asm/uaccess.h>

#define SCRANDOM_MAJOR 233
#define SCRANDOM_MINOR 28

#define SCRANDOM_IV 0x1AA9BB7Au
#define SCRANDOM_SALT 38
#define SCRANDOM_MODUL 3
#define SCRANDOM_DIST1 2
#define SCRANDOM_DIST2 5
#define SCRANDOM_DIST3 9
#define SCRANDOM_SALT1 ((SCRANDOM_SALT%SCRANDOM_MODUL)+SCRANDOM_DIST1)
#define SCRANDOM_SALT2 ((SCRANDOM_SALT%SCRANDOM_MODUL)+SCRANDOM_DIST2)
#define SCRANDOM_SALT3 ((SCRANDOM_SALT%SCRANDOM_MODUL)+SCRANDOM_DIST3)
//#define SCRANDOM_LFSRSIZE sizeof(void *)
#define SCRANDOM_LFSRSIZE 4
// larger buf increases throughput with the tradeoff random seed initialization is more expensive
// a total 64byte/512bit sized LFSR scrambler matrix is sufficient for all intents and purposes
#define SCRANDOM_BUFNUM 16
#define SCRANDOM_BUFSIZE SCRANDOM_BUFNUM*SCRANDOM_LFSRSIZE

struct scrandom {
	struct semaphore sem;

	u32 *scrambler;
	u32 index;
	u32 s1;
	u32 s2;
	u32 s3;
};

static struct scrandom *scrandom_state;

static u32 global_seed = SCRANDOM_IV;

static struct file_operations scr_fops;
static int scr_major = SCRANDOM_MAJOR;
static int scr_minor = SCRANDOM_MINOR;

static void scrandom_shift(struct scrandom *scr) {
	scr->index %= SCRANDOM_BUFNUM;
	u32 *scrambler = &(scr->scrambler[scr->index]);

	*scrambler^=((*scrambler)>>scr->s1);
	*scrambler^=((*scrambler)<<scr->s2);
	*scrambler^=((*scrambler)>>scr->s3);

	scr->s1=((*scrambler)%SCRANDOM_MODUL)+SCRANDOM_DIST1;
	scr->s2=((*scrambler)%SCRANDOM_MODUL)+SCRANDOM_DIST2;
	scr->s3=((*scrambler)%SCRANDOM_MODUL)+SCRANDOM_DIST3;
}

static void scrandom_init(struct scrandom *scr) {
	u32 *pos32;
	scr->index=0;
	scr->s1 = SCRANDOM_SALT1;
	scr->s2 = SCRANDOM_SALT2;
	scr->s3 = SCRANDOM_SALT3;

	u32 entropy_clock = 0;
	struct timeval entropy_time;

	while ( scr->index < SCRANDOM_BUFNUM ) {
		pos32 = &(scr->scrambler[(scr->index)%SCRANDOM_BUFNUM]);
		if ( scr->index == 0) {
			*pos32 = global_seed;
			entropy_clock = get_cycles();
			*pos32 ^= entropy_clock;
			do_gettimeofday(&entropy_time);
			*pos32 ^= (u32)entropy_time.tv_usec; //
		}
		scrandom_shift(scr);
		scr->index++;
	}
	global_seed = *pos32;
	scr->index = 0;
}

static int scrandom_open(struct inode *inode, struct file *filp) {
	struct scrandom *scr;

	//int num = MINOR(kdev_t_to_nr(inode->i_rdev));
	//if (num != scr_minor)
	//	return -ENODEV;

	scr = kmalloc(sizeof(struct scrandom), GFP_KERNEL);
	if (!scr)
		return -ENOMEM;

	scr->scrambler = kmalloc(SCRANDOM_BUFSIZE, GFP_KERNEL);
	if (!scr->scrambler) {
		kfree(scr);
		return -ENOMEM;
	};

	sema_init(&scr->sem, 1); /* Init semaphore as a mutex */

	scrandom_init(scr);
	filp->private_data = scr;

	return 0;
}

static int scrandom_release(struct inode *inode, struct file *filp) {
	struct scrandom *scr = filp->private_data;

	kfree(scr->scrambler);
	kfree(scr);

	return 0;
}

static ssize_t scrandom_read(struct file *filp, char *buf, size_t count, loff_t *f_pos) {
	struct scrandom *scr = filp->private_data;
	u32 done_bytes = 0;
	scr->index=0;
  
	if (down_interruptible(&scr->sem))
		return -ERESTARTSYS;

	while ( (done_bytes+SCRANDOM_BUFSIZE) <= count ) {
		while ( scr->index < SCRANDOM_BUFNUM ) {
			scrandom_shift(scr);
			scr->index++;
		}
		if(copy_to_user(buf, (u8 *)(scr->scrambler), SCRANDOM_BUFSIZE)) {
			count = -EFAULT;
			goto out;
		}
		buf+=SCRANDOM_BUFSIZE;
		done_bytes+=SCRANDOM_BUFSIZE;
	}
	while ( (done_bytes+SCRANDOM_LFSRSIZE) <= count ) {
		scrandom_shift(scr);
		if(copy_to_user(buf, (u8 *)&(scr->scrambler[scr->index]), SCRANDOM_LFSRSIZE)) {
			count = -EFAULT;
			goto out;
		}
		buf+=SCRANDOM_LFSRSIZE;
		done_bytes+=SCRANDOM_LFSRSIZE;
		scr->index++;
	}
	if ( done_bytes < count ) {
		scrandom_shift(scr);
		if(copy_to_user(buf, (u8 *)&(scr->scrambler[scr->index]), count - done_bytes)) {
			count = -EFAULT;
			goto out;
		}
		scr->index++;
	}

out:
	up(&scr->sem);
	return count;
}

static struct file_operations scr_fops = {
	read:       scrandom_read,
	open:       scrandom_open,
	release:    scrandom_release,
};

static void scrandom_cleanup_module(void) {
	kfree(scrandom_state->scrambler);
	kfree(scrandom_state);

	unregister_chrdev(scr_major, "scrandom");
}

#ifndef SCRANDOM_NOMODULE
/* prevent re-definition when scrandom.c is sourced into random.c */

MODULE_DESCRIPTION("Ultra-High-Speed pseudo-random number generator");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("aggi");

int scrandom_init_module(void) {
	int result;

	scrandom_state = kmalloc(sizeof(struct scrandom), GFP_KERNEL);
	if (!scrandom_state)
		return -ENOMEM;

	scrandom_state->scrambler = kmalloc(SCRANDOM_BUFSIZE, GFP_KERNEL);
	if (!scrandom_state->scrambler) {
		kfree(scrandom_state);
		return -ENOMEM;
	}

	sema_init(&scrandom_state->sem, 1); /* Init semaphore as a mutex */

#ifdef SET_MODULE_OWNER
	SET_MODULE_OWNER(&frandom_fops);
#endif
	/*
	 * Register your major, and accept a dynamic number. This is the
	 * first thing to do, in order to avoid releasing other module's
	 * fops in frandom_cleanup_module()
	 */
	result = register_chrdev(scr_major, "scrandom", &scr_fops);
	if (result < 0) {
		printk(KERN_WARNING "scrandom: can't get major %d\n", scr_major);

		kfree(scrandom_state->scrambler);
		kfree(scrandom_state);
	
		return result;
	}

	if (scr_major == 0)
		scr_major = result; /* dynamic */

	devfs_register_chrdev(scr_major, "scrandom", &scr_fops);

	devfs_register(NULL, "scrandom", DEVFS_FL_NONE,
		scr_major, scr_minor,
		S_IRUGO | S_IWUSR | S_IFCHR,
		&scr_fops, NULL);

	printk(KERN_INFO "scrandom: OK.\n"); return 0;

	return result;
}

module_init(scrandom_init_module);
module_exit(scrandom_cleanup_module);
#endif

static void scrandom_get_random_bytes(char *buf, int count)
{
	struct scrandom *scr;

	scr = kmalloc(sizeof(struct scrandom), GFP_KERNEL);
	if (!scr) {
		count = -ENOMEM;
		return;
	}

	scr->scrambler = kmalloc(SCRANDOM_BUFSIZE, GFP_KERNEL);
	if (!scr->scrambler) {
		count = -ENOMEM;
		kfree(scr);
		return;
	};

//	sema_init(&scr->sem, 1); /* Init semaphore as a mutex */

	scrandom_init(scr);

	u32 done_bytes = 0;
	scr->index = 0;
  
//	if (down_interruptible(&scr->sem))
//		return -ERESTARTSYS;

	while ( (done_bytes+SCRANDOM_BUFSIZE) <= count ) {
		while ( scr->index < SCRANDOM_BUFNUM ) {
			scrandom_shift(scr);
			scr->index++;
		}
		if(copy_to_user(buf, (u8 *)(scr->scrambler), SCRANDOM_BUFSIZE)) {
			count = -EFAULT;
			goto out;
		}
		buf+=SCRANDOM_BUFSIZE;
		done_bytes+=SCRANDOM_BUFSIZE;
	}
	while ( (done_bytes+SCRANDOM_LFSRSIZE) <= count ) {
		scrandom_shift(scr);
		if(copy_to_user(buf, (u8 *)&(scr->scrambler[scr->index]), SCRANDOM_LFSRSIZE)) {
			count = -EFAULT;
			goto out;
		}
		buf+=SCRANDOM_LFSRSIZE;
		done_bytes+=SCRANDOM_LFSRSIZE;
		scr->index++;
	}
	if ( done_bytes < count ) {
		scrandom_shift(scr);
		if(copy_to_user(buf, (u8 *)&(scr->scrambler[scr->index]), count - done_bytes)) {
			count = -EFAULT;
			goto out;
		}
		scr->index++;
	}

out:
	kfree(scr->scrambler);
	kfree(scr);
//	up(&scr->sem);
}

//EXPORT_SYMBOL(srandom_get_random_bytes);

