/*
 *	National Semiconductor PC87307/PC97307 (ala SC1200) WDT driver
 *	(c) Copyright 2002 Zwane Mwaikambo <zwane@commfireservices.com>,
 *			All Rights Reserved.
 *	Based on wdt.c and wdt977.c by Alan Cox and Woody Suwalski respectively.
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 *
 *	The author(s) of this software shall not be held liable for damages
 *	of any nature resulting due to the use of this software. This
 *	software is provided AS-IS with no warranties.
 *
 *	Changelog:
 *	20020220 Zwane Mwaikambo	Code based on datasheet, no hardware.
 *	20020221 Zwane Mwaikambo	Cleanups as suggested by Jeff Garzik and Alan Cox.
 *	20020222 Zwane Mwaikambo	Added probing.
 *	20020225 Zwane Mwaikambo	Added ISAPNP support.
 *	20020412 Rob Radez		Broke out start/stop functions
 *		 <rob@osinvestor.com>	Return proper status instead of temperature warning
 *					Add WDIOC_GETBOOTSTATUS and WDIOC_SETOPTIONS ioctls
 *					Fix CONFIG_WATCHDOG_NOWAYOUT
 *	20020530 Joel Becker		Add Matt Domsch's nowayout
 *					module option
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/watchdog.h>
#include <linux/ioport.h>
#include <linux/spinlock.h>
#include <linux/ioport.h>
#include <asm/semaphore.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <linux/notifier.h>
#include <linux/reboot.h>
#include <linux/init.h>
#include <linux/isapnp.h>

#define SC1200_MODULE_VER	"build 20020303"
#define SC1200_MODULE_NAME	"sc1200wdt"
#define PFX			SC1200_MODULE_NAME ": "

#define	MAX_TIMEOUT	255	/* 255 minutes */
#define PMIR		(io)	/* Power Management Index Register */
#define PMDR		(io+1)	/* Power Management Data Register */

/* Data Register indexes */
#define FER1		0x00	/* Function enable register 1 */
#define FER2		0x01	/* Function enable register 2 */
#define PMC1		0x02	/* Power Management Ctrl 1 */
#define PMC2		0x03	/* Power Management Ctrl 2 */
#define PMC3		0x04	/* Power Management Ctrl 3 */
#define WDTO		0x05	/* Watchdog timeout register */
#define	WDCF		0x06	/* Watchdog config register */
#define WDST		0x07	/* Watchdog status register */

/* WDCF bitfields - which devices assert WDO */
#define KBC_IRQ		0x01	/* Keyboard Controller */
#define MSE_IRQ		0x02	/* Mouse */
#define UART1_IRQ	0x03	/* Serial0 */
#define UART2_IRQ	0x04	/* Serial1 */
/* 5 -7 are reserved */

static char banner[] __initdata = KERN_INFO PFX SC1200_MODULE_VER;
static int timeout = 1;
static int io = -1;
static int io_len = 2;		/* for non plug and play */
struct semaphore open_sem;
static char expect_close;
spinlock_t sc1200wdt_lock;	/* io port access serialisation */

#if defined CONFIG_ISAPNP || defined CONFIG_ISAPNP_MODULE
static int isapnp = 1;
static struct pci_dev *wdt_dev;

MODULE_PARM(isapnp, "i");
MODULE_PARM_DESC(isapnp, "When set to 0 driver ISA PnP support will be disabled");
#endif

MODULE_PARM(io, "i");
MODULE_PARM_DESC(io, "io port");
MODULE_PARM(timeout, "i");
MODULE_PARM_DESC(timeout, "range is 0-255 minutes, default is 1");

#ifdef CONFIG_WATCHDOG_NOWAYOUT
static int nowayout = 1;
#else
static int nowayout = 0;
#endif

MODULE_PARM(nowayout,"i");
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started (default=CONFIG_WATCHDOG_NOWAYOUT)");


/* Read from Data Register */
static inline void sc1200wdt_read_data(unsigned char index, unsigned char *data)
{
	spin_lock(&sc1200wdt_lock);
	outb_p(index, PMIR);
	*data = inb(PMDR);
	spin_unlock(&sc1200wdt_lock);
}


/* Write to Data Register */
static inline void sc1200wdt_write_data(unsigned char index, unsigned char data)
{
	spin_lock(&sc1200wdt_lock);
	outb_p(index, PMIR);
	outb(data, PMDR);
	spin_unlock(&sc1200wdt_lock);
}


static void sc1200wdt_start(void)
{
	unsigned char reg;

	sc1200wdt_read_data(WDCF, &reg);
	/* assert WDO when any of the following interrupts are triggered too */
	reg |= (KBC_IRQ | MSE_IRQ | UART1_IRQ | UART2_IRQ);
	sc1200wdt_write_data(WDCF, reg);
	/* set the timeout and get the ball rolling */
	sc1200wdt_write_data(WDTO, timeout);
}


static void sc1200wdt_stop(void)
{
	sc1200wdt_write_data(WDTO, 0);
}


/* This returns the status of the WDO signal, inactive high. */
static inline int sc1200wdt_status(void)
{
	unsigned char ret;

	sc1200wdt_read_data(WDST, &ret);
	/* If the bit is inactive, the watchdog is enabled, so return
	 * KEEPALIVEPING which is a bit of a kludge because there's nothing
	 * else for enabled/disabled status
	 */
	return (ret & 0x01) ? 0 : WDIOF_KEEPALIVEPING;	/* bits 1 - 7 are undefined */
}


static int sc1200wdt_open(struct inode *inode, struct file *file)
{
	/* allow one at a time */
	if (down_trylock(&open_sem))
		return -EBUSY;

	if (timeout > MAX_TIMEOUT)
		timeout = MAX_TIMEOUT;

	sc1200wdt_start();
	printk(KERN_INFO PFX "Watchdog enabled, timeout = %d min(s)", timeout);

	return 0;
}


static int sc1200wdt_ioctl(struct inode *inode, struct file *file, unsigned int cmd, unsigned long arg)
{
	int new_timeout;
	static struct watchdog_info ident = {
		options:		WDIOF_KEEPALIVEPING | WDIOF_SETTIMEOUT | WDIOF_MAGICCLOSE,
		firmware_version:	0,
		identity:		"PC87307/PC97307"
	};

	switch (cmd) {
		default:
			return -ENOTTY;	/* Keep Pavel Machek amused ;) */

		case WDIOC_GETSUPPORT:
			if (copy_to_user((struct watchdog_info *)arg, &ident, sizeof ident))
				return -EFAULT;
			return 0;

		case WDIOC_GETSTATUS:
			return put_user(sc1200wdt_status(), (int *)arg);

		case WDIOC_GETBOOTSTATUS:
			return put_user(0, (int *)arg);

		case WDIOC_KEEPALIVE:
			sc1200wdt_write_data(WDTO, timeout);
			return 0;

		case WDIOC_SETTIMEOUT:
			if (get_user(new_timeout, (int *)arg))
				return -EFAULT;

			/* the API states this is given in secs */
			new_timeout /= 60;
			if (new_timeout < 0 || new_timeout > MAX_TIMEOUT)
				return -EINVAL;

			timeout = new_timeout;
			sc1200wdt_write_data(WDTO, timeout);
			/* fall through and return the new timeout */

		case WDIOC_GETTIMEOUT:
			return put_user(timeout * 60, (int *)arg);

		case WDIOC_SETOPTIONS:
		{
			int options, retval = -EINVAL;

			if (get_user(options, (int *)arg))
				return -EFAULT;

			if (options & WDIOS_DISABLECARD) {
				sc1200wdt_stop();
				retval = 0;
			}

			if (options & WDIOS_ENABLECARD) {
				sc1200wdt_start();
				retval = 0;
			}

			return retval;
		}
	}
}


static int sc1200wdt_release(struct inode *inode, struct file *file)
{
	if (expect_close == 42) {
		sc1200wdt_stop();
		printk(KERN_INFO PFX "Watchdog disabled\n");
	} else {
		sc1200wdt_write_data(WDTO, timeout);
		printk(KERN_CRIT PFX "Unexpected close!, timeout = %d min(s)\n", timeout);
	}
	up(&open_sem);
	expect_close = 0;

	return 0;
}


static ssize_t sc1200wdt_write(struct file *file, const char *data, size_t len, loff_t *ppos)
{
	if (ppos != &file->f_pos)
		return -ESPIPE;
	
	if (len) {
		if (!nowayout) {
			size_t i;

			expect_close = 0;

			for (i = 0; i != len; i++)
			{
				char c;
				if(get_user(c, data+i))
					return -EFAULT;
				if (c == 'V')
					expect_close = 42;
			}
		}
		sc1200wdt_write_data(WDTO, timeout);
		return len;
	}

	return 0;
}


static int sc1200wdt_notify_sys(struct notifier_block *this, unsigned long code, void *unused)
{
	if (code == SYS_DOWN || code == SYS_HALT)
		sc1200wdt_stop();

	return NOTIFY_DONE;
}


static struct notifier_block sc1200wdt_notifier =
{
	notifier_call:	sc1200wdt_notify_sys
};

static const struct file_operations sc1200wdt_fops = {
	owner:		THIS_MODULE,
	write:		sc1200wdt_write,
	ioctl:		sc1200wdt_ioctl,
	open:		sc1200wdt_open,
	release:	sc1200wdt_release
};

static struct miscdevice sc1200wdt_miscdev =
{
	minor:		WATCHDOG_MINOR,
	name:		"watchdog",
	fops:		&sc1200wdt_fops,
};


static int __init sc1200wdt_probe(void)
{
	/* The probe works by reading the PMC3 register's default value of 0x0e
	 * there is one caveat, if the device disables the parallel port or any
	 * of the UARTs we won't be able to detect it.
	 * Nb. This could be done with accuracy by reading the SID registers, but
	 * we don't have access to those io regions.
	 */
	
	unsigned char reg;

	sc1200wdt_read_data(PMC3, &reg);
	reg &= 0x0f;				/* we don't want the UART busy bits */
	return (reg == 0x0e) ? 0 : -ENODEV;
}


#if defined CONFIG_ISAPNP || defined CONFIG_ISAPNP_MODULE

static int __init sc1200wdt_isapnp_probe(void)
{
	int ret;

	/* The WDT is logical device 8 on the main device */
	wdt_dev = isapnp_find_dev(NULL, ISAPNP_VENDOR('N','S','C'), ISAPNP_FUNCTION(0x08), NULL);
	if (!wdt_dev)
		return -ENODEV;
	
	if (wdt_dev->prepare(wdt_dev) < 0) {
		printk(KERN_ERR PFX "ISA PnP found device that could not be autoconfigured\n");
		return -EAGAIN;
	}

	if (!(pci_resource_flags(wdt_dev, 0) & IORESOURCE_IO)) {
		printk(KERN_ERR PFX "ISA PnP could not find io ports\n");
		return -ENODEV;
	}

	ret = wdt_dev->activate(wdt_dev);
	if (ret && (ret != -EBUSY))
		return -ENOMEM;

	/* io port resource overriding support? */
	io = pci_resource_start(wdt_dev, 0);
	io_len = pci_resource_len(wdt_dev, 0);

	printk(KERN_DEBUG PFX "ISA PnP found device at io port %#x/%d\n", io, io_len);
	return 0;
}

#endif /* CONFIG_ISAPNP */


static int __init sc1200wdt_init(void)
{
	int ret;

	printk(banner);

	spin_lock_init(&sc1200wdt_lock);
	sema_init(&open_sem, 1);

#if defined CONFIG_ISAPNP || defined CONFIG_ISAPNP_MODULE
	if (isapnp) {
		ret = sc1200wdt_isapnp_probe();
		if (ret)
			goto out_clean;
	}
#endif

	if (io == -1) {
		printk(KERN_ERR PFX "io parameter must be specified\n");
		ret = -EINVAL;
		goto out_pnp;
	}

	if (!request_region(io, io_len, SC1200_MODULE_NAME)) {
		printk(KERN_ERR PFX "Unable to register IO port %#x\n", io);
		ret = -EBUSY;
		goto out_pnp;
	}

	ret = sc1200wdt_probe();
	if (ret)
		goto out_io;

	ret = register_reboot_notifier(&sc1200wdt_notifier);
	if (ret) {
		printk(KERN_ERR PFX "Unable to register reboot notifier err = %d\n", ret);
		goto out_io;
	}

	ret = misc_register(&sc1200wdt_miscdev);
	if (ret) {
		printk(KERN_ERR PFX "Unable to register miscdev on minor %d\n", WATCHDOG_MINOR);
		goto out_rbt;
	}

	/* ret = 0 */

out_clean:
	return ret;

out_rbt:
	unregister_reboot_notifier(&sc1200wdt_notifier);

out_io:
	release_region(io, io_len);

out_pnp:
#if defined CONFIG_ISAPNP || defined CONFIG_ISAPNP_MODULE
	if (isapnp && wdt_dev)
		wdt_dev->deactivate(wdt_dev);
#endif
	goto out_clean;
}	


static void __exit sc1200wdt_exit(void)
{
	misc_deregister(&sc1200wdt_miscdev);
	unregister_reboot_notifier(&sc1200wdt_notifier);

#if defined CONFIG_ISAPNP || defined CONFIG_ISAPNP_MODULE
	if(isapnp && wdt_dev)
		wdt_dev->deactivate(wdt_dev);
#endif

	release_region(io, io_len);
}


#ifndef MODULE
static int __init sc1200wdt_setup(char *str)
{
	int ints[4];

	str = get_options (str, ARRAY_SIZE(ints), ints);

	if (ints[0] > 0) {
		io = ints[1];
		if (ints[0] > 1)
			timeout = ints[2];

#if defined CONFIG_ISAPNP || defined CONFIG_ISAPNP_MODULE
		if (ints[0] > 2)
			isapnp = ints[3];
#endif
	}

	return 1;
}

__setup("sc1200wdt=", sc1200wdt_setup);
#endif /* MODULE */


module_init(sc1200wdt_init);
module_exit(sc1200wdt_exit);

MODULE_AUTHOR("Zwane Mwaikambo <zwane@commfireservices.com>");
MODULE_DESCRIPTION("Driver for National Semiconductor PC87307/PC97307 watchdog component");
MODULE_LICENSE("GPL");
EXPORT_NO_SYMBOLS;

