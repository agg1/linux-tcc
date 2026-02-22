/* Copyright (c) 2019,2026 Michael Ackermann, aggi
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public Version 2 License as published by the Free
 * Software Foundation;
 *
 * PRNG utilizing non-primitive recurse LFSR matrix
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

#include <asm/uaccess.h>

#define SCRANDOM_IV 0x1AA9BB7Au
#define SCRANDOM_MODUL 7
#define SCRANDOM_DIST1 2
#define SCRANDOM_DIST2 5
#define SCRANDOM_DIST3 9
//#define SCRANDOM_LFSRSIZE sizeof(void *)
#define SCRANDOM_LFSRSIZE 4
// larger buf increases throughput with the tradeoff random seed initialization is more expensive
// a total 512byte/4096bit sized LFSR scrambler matrix is sufficient for all intents and purposes
#define SCRANDOM_BUFNUM 128
#define SCRANDOM_BUFSIZE SCRANDOM_BUFNUM*SCRANDOM_LFSRSIZE

// entropy source
static u32 global_seed = SCRANDOM_IV;

struct scrandom {
	u32 scrambler[SCRANDOM_BUFNUM];	// SCRANDOM_BUFNUM amount of LFSRSIZE/u32
	u32 s1, s2, s3; 		// generator polynome
	u32 index;			// random index
};

static void scrandom_shift(struct scrandom *scr) {
	u32 index = 0;
	u32 *scrambler = &(scr->scrambler[0]);
	*scrambler ^= global_seed; *scrambler ^= (u32)(scr->scrambler[0]);
	// update generator polynome
	scr->s1=((*scrambler)%SCRANDOM_MODUL)+SCRANDOM_DIST1;
	scr->s2=((*scrambler)%SCRANDOM_MODUL)+SCRANDOM_DIST2;
	scr->s3=((*scrambler)%SCRANDOM_MODUL)+SCRANDOM_DIST3;

	// iterate the entire LFSR scrambler matrix
	while ( index < SCRANDOM_BUFNUM ) {
 		scrambler = &(scr->scrambler[index]);
		// scramble
		*scrambler^=((*scrambler)>>scr->s1);
		*scrambler^=((*scrambler)<<scr->s2);
		*scrambler^=((*scrambler)>>scr->s3);
		// interleave entropy into next buffer
		scr->scrambler[((index + 1)%SCRANDOM_BUFNUM)] ^= (u32)scrambler;
		scr->scrambler[((index + 1)%SCRANDOM_BUFNUM)] += *scrambler;
		//
		index++;
	}

	// next random index
	scr->index = (scr->scrambler[(index%SCRANDOM_BUFNUM)])%SCRANDOM_BUFNUM;
	// update global_seed to improve entropy whenever a scrandom instance was created
	global_seed ^= scr->scrambler[scr->index];
}

static ssize_t scrandom_read(struct file *filp, char *buf, size_t count, loff_t *f_pos) {
	struct scrandom scr;
	u32 done_bytes = 0;
	// get largest chunk possible first to sature data bus
	while ( (done_bytes+SCRANDOM_BUFSIZE) <= count ) {
		scrandom_shift(&scr);
		if(copy_to_user(buf, (u8 *)(scr.scrambler), SCRANDOM_BUFSIZE)) {
			count = -EFAULT;
			goto out;
		}
		buf+=SCRANDOM_BUFSIZE;
		done_bytes+=SCRANDOM_BUFSIZE;
	}
	while ( (done_bytes+SCRANDOM_LFSRSIZE) <= count ) {
		scrandom_shift(&scr);
		if(copy_to_user(buf, (u8 *)&(scr.scrambler[scr.index]), SCRANDOM_LFSRSIZE)) {
			count = -EFAULT;
			goto out;
		}
		buf+=SCRANDOM_LFSRSIZE;
		done_bytes+=SCRANDOM_LFSRSIZE;
	}
	if ( done_bytes < count ) {
		scrandom_shift(&scr);
		if(copy_to_user(buf, (u8 *)&(scr.scrambler[scr.index]), count - done_bytes)) {
			count = -EFAULT;
			goto out;
		}
	}
out:
	return count;
}

#ifndef SCRANDOM_NOMODULE
MODULE_DESCRIPTION("Ultra-High-Speed pseudo-random number generator");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("aggi");

static int scr_major = 233;
static int scr_minor = 28;

static struct file_operations scr_fops = {
	read:       scrandom_read,
};

static void scrandom_cleanup_module(void) {
	unregister_chrdev(scr_major, "scrandom");
}

int scrandom_init_module(void) {
	int result;

	result = register_chrdev(scr_major, "scrandom", &scr_fops);
	if (result < 0) {
		printk(KERN_WARNING "scrandom: can't get major %d\n", scr_major);
	
		return result;
	}

	if (scr_major == 0)
		scr_major = result; /* dynamic */

	devfs_register_chrdev(scr_major, "scrandom", &scr_fops);

	devfs_register(NULL, "scrandom", DEVFS_FL_NONE,
		scr_major, scr_minor,
		S_IRUGO | S_IWUSR | S_IFCHR,
		&scr_fops, NULL);

	return result;
}

module_init(scrandom_init_module);
module_exit(scrandom_cleanup_module);
#endif

