/*
 *            Telephony registration for Linux
 *
 *              (c) Copyright 1999 Red Hat Software Inc.
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              as published by the Free Software Foundation; either version
 *              2 of the License, or (at your option) any later version.
 *
 * Author:      Alan Cox, <alan@redhat.com>
 *
 * Fixes:       Mar 01 2000 Thomas Sparr, <thomas.l.sparr@telia.com>
 *              phone_register_device now works with unit!=PHONE_UNIT_ANY
 */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/phonedev.h>
#include <linux/init.h>
#include <asm/uaccess.h>
#include <asm/system.h>

#include <linux/kmod.h>
#include <linux/sem.h>


#define PHONE_NUM_DEVICES	256

/*
 *    Active devices 
 */

static struct phone_device *phone_device[PHONE_NUM_DEVICES];
static DECLARE_MUTEX(phone_lock);

/*
 *    Open a phone device.
 */

static int phone_open(struct inode *inode, struct file *file)
{
	unsigned int minor = MINOR(inode->i_rdev);
	int err = 0;
	struct phone_device *p;
	const struct file_operations *old_fops, *new_fops = NULL;

	if (minor >= PHONE_NUM_DEVICES)
		return -ENODEV;

	down(&phone_lock);
	p = phone_device[minor];
	if (p)
		new_fops = fops_get(p->f_op);
	if (!new_fops) {
		char modname[32];

		up(&phone_lock);
		sprintf(modname, "char-major-%d-%d", PHONE_MAJOR, minor);
		request_module(modname);
		down(&phone_lock);
		p = phone_device[minor];
		if (p == NULL || (new_fops = fops_get(p->f_op)) == NULL)
		{
			err=-ENODEV;
			goto end;
		}
	}
	old_fops = file->f_op;
	file->f_op = new_fops;
	if (p->open)
		err = p->open(p, file);	/* Tell the device it is open */
	if (err) {
		fops_put(file->f_op);
		file->f_op = fops_get(old_fops);
	}
	fops_put(old_fops);
end:
	up(&phone_lock);
	return err;
}

/*
 *    Telephony For Linux device drivers request registration here.
 */

int phone_register_device(struct phone_device *p, int unit)
{
	int base;
	int end;
	int i;

	base = 0;
	end = PHONE_NUM_DEVICES - 1;

	if (unit != PHONE_UNIT_ANY) {
		base = unit;
		end = unit + 1;  /* enter the loop at least one time */
	}
	
	down(&phone_lock);
	for (i = base; i < end; i++) {
		if (phone_device[i] == NULL) {
			phone_device[i] = p;
			p->minor = i;
			MOD_INC_USE_COUNT;
			up(&phone_lock);
			return 0;
		}
	}
	up(&phone_lock);
	return -ENFILE;
}

/*
 *    Unregister an unused Telephony for linux device
 */

void phone_unregister_device(struct phone_device *pfd)
{
	down(&phone_lock);
	if (phone_device[pfd->minor] != pfd)
		panic("phone: bad unregister");
	phone_device[pfd->minor] = NULL;
	up(&phone_lock);
	MOD_DEC_USE_COUNT;
}


static const struct file_operations phone_fops = {
	owner:		THIS_MODULE,
	open:		phone_open,
};

/*
 *	Board init functions
 */
 

/*
 *    Initialise Telephony for linux
 */

static int __init telephony_init(void)
{
	printk(KERN_INFO "Linux telephony interface: v1.00\n");
	if (register_chrdev(PHONE_MAJOR, "telephony", &phone_fops)) {
		printk("phonedev: unable to get major %d\n", PHONE_MAJOR);
		return -EIO;
	}

	return 0;
}

static void __exit telephony_exit(void)
{
	unregister_chrdev(PHONE_MAJOR, "telephony");
}

module_init(telephony_init);
module_exit(telephony_exit);

MODULE_LICENSE("GPL");

EXPORT_SYMBOL(phone_register_device);
EXPORT_SYMBOL(phone_unregister_device);
