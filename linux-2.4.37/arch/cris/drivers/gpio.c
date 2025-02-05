/* $Id: gpio.c,v 1.25 2003/06/24 07:24:31 johana Exp $
 *
 * Etrax general port I/O device
 *
 * Copyright (c) 1999, 2000, 2001, 2002 Axis Communications AB
 *
 * Authors:    Bjorn Wesen      (initial version)
 *             Ola Knutsson     (LED handling)
 *             Johan Adolfsson  (read/set directions, write, port G)
 *
 * $Log: gpio.c,v $
 * Revision 1.25  2003/06/24 07:24:31  johana
 * Added gpio_pa_irq_enabled_mask to keep track of what interrupts we have
 * enabled so we anly clear those that we have enabled and let bits/pins
 * used by other drivers alone.
 * Update gpio_some_alarms in IO_CLRALARM ioctl().
 *
 * Revision 1.24  2003/06/19 08:58:25  johana
 * Turn off interrupts while fiddling with genconfig_shadow.
 * Put some printk within D() macro.
 *
 * Revision 1.23  2003/04/01 14:12:06  starvik
 * Added loglevel for lots of printks
 *
 * Revision 1.22  2003/01/09 18:03:47  starvik
 * init_ioremap is now called by kernel before device drivers are initialized
 *
 * Revision 1.21  2002/12/02 08:11:41  starvik
 * Merge of Linux 2.4.20
 *
 * Revision 1.20  2002/10/16 21:16:24  johana
 * Added support for PA high level interrupt.
 * That gives 2ms response time with iodtest for high levels and 2-12 ms
 * response time on low levels if the check is not made in
 * process.c:cpu_idle() as well.
 *
 * Revision 1.19  2002/10/14 18:27:33  johana
 * Implemented alarm handling so select() now works.
 * Latency is around 6-9 ms with a etrax_gpio_wake_up_check() in
 * cpu_idle().
 * Otherwise I get 15-18 ms (same as doing the poll in userspace -
 * but less overhead).
 * TODO? Perhaps we should add the check in IMMEDIATE_BH (or whatever it
 * is in 2.4) as well?
 * TODO? Perhaps call request_irq()/free_irq() only when needed?
 * Increased version to 2.5
 *
 * Revision 1.18  2002/10/11 15:02:00  johana
 * Mask inverted 8 bit value in setget_input().
 *
 * Revision 1.17  2002/06/17 15:53:01  johana
 * Added IO_READ_INBITS, IO_READ_OUTBITS, IO_SETGET_INPUT and IO_SETGET_OUTPUT
 * that take a pointer as argument and thus can handle 32 bit ports (G)
 * correctly.
 * These should be used instead of IO_READBITS, IO_SETINPUT and IO_SETOUTPUT.
 * (especially if Port G bit 31 is used)
 *
 * Revision 1.16  2002/06/17 09:59:51  johana
 * Returning 32 bit values in the ioctl return value doesn't work if bit
 * 31 is set (could happen for port G), so mask it of with 0x7FFFFFFF.
 * A new set of ioctl's will be added.
 *
 * Revision 1.15  2002/05/06 13:19:13  johana
 * IO_SETINPUT returns mask with bit set = inputs for PA and PB as well.
 *
 * Revision 1.14  2002/04/12 12:01:53  johana
 * Use global r_port_g_data_shadow.
 * Moved gpio_init_port_g() closer to gpio_init() and marked it __init.
 *
 * Revision 1.13  2002/04/10 12:03:55  johana
 * Added support for port G /dev/gpiog (minor 3).
 * Changed indentation on switch cases.
 * Fixed other spaces to tabs.
 *
 * Revision 1.12  2001/11/12 19:42:15  pkj
 * * Corrected return values from gpio_leds_ioctl().
 * * Fixed compiler warnings.
 *
 * Revision 1.11  2001/10/30 14:39:12  johana
 * Added D() around gpio_write printk.
 *
 * Revision 1.10  2001/10/25 10:24:42  johana
 * Added IO_CFG_WRITE_MODE ioctl and write method that can do fast
 * bittoggling in the kernel. (This speeds up programming an FPGA with 450kB
 * from ~60 seconds to 4 seconds).
 * Added save_flags/cli/restore_flags in ioctl.
 *
 * Revision 1.9  2001/05/04 14:16:07  matsfg
 * Corrected spelling error
 *
 * Revision 1.8  2001/04/27 13:55:26  matsfg
 * Moved initioremap.
 * Turns off all LEDS on init.
 * Added support for shutdown and powerbutton.
 *
 * Revision 1.7  2001/04/04 13:30:08  matsfg
 * Added bitset and bitclear for leds. Calls init_ioremap to set up memmapping
 *
 * Revision 1.6  2001/03/26 16:03:06  bjornw
 * Needs linux/config.h
 *
 * Revision 1.5  2001/03/26 14:22:03  bjornw
 * Namechange of some config options
 *
 * Revision 1.4  2001/02/27 13:52:48  bjornw
 * malloc.h -> slab.h
 *
 * Revision 1.3  2001/01/24 15:06:48  bjornw
 * gpio_wq correct type
 *
 * Revision 1.2  2001/01/18 16:07:30  bjornw
 * 2.4 port
 *
 * Revision 1.1  2001/01/18 15:55:16  bjornw
 * Verbatim copy of etraxgpio.c from elinux 2.0 added
 *
 *
 */

#include <linux/config.h>

#include <linux/module.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/ioport.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/string.h>
#include <linux/poll.h>
#include <linux/init.h>

#include <asm/etraxgpio.h>
#include <asm/svinto.h>
#include <asm/io.h>
#include <asm/system.h>
#include <asm/irq.h>

#define GPIO_MAJOR 120  /* experimental MAJOR number */

#define D(x)

#if 0
static int dp_cnt;
#define DP(x) do { dp_cnt++; if (dp_cnt % 1000 == 0) x; }while(0)
#else
#define DP(x)
#endif
	
static char gpio_name[] = "etrax gpio";

#if 0
static wait_queue_head_t *gpio_wq;
#endif

static int gpio_ioctl(struct inode *inode, struct file *file,
		      unsigned int cmd, unsigned long arg);
static ssize_t gpio_write(struct file * file, const char * buf, size_t count,
                          loff_t *off);
static int gpio_open(struct inode *inode, struct file *filp);
static int gpio_release(struct inode *inode, struct file *filp);
static unsigned int gpio_poll(struct file *filp, struct poll_table_struct *wait);

/* private data per open() of this driver */

struct gpio_private {
	struct gpio_private *next;
	/* These fields are for PA and PB only */
	volatile unsigned char *port, *shadow;
	volatile unsigned char *dir, *dir_shadow;
	unsigned char changeable_dir;
	unsigned char changeable_bits;
	unsigned char clk_mask;
	unsigned char data_mask;
	unsigned char write_msb;
	unsigned char pad1, pad2, pad3;
	/* These fields are generic */
	unsigned long highalarm, lowalarm;
	wait_queue_head_t alarm_wq;
	int minor;
};

/* linked list of alarms to check for */

static struct gpio_private *alarmlist = 0;

static int gpio_some_alarms = 0; /* Set if someone uses alarm */
static unsigned long gpio_pa_irq_enabled_mask = 0;

/* Port A and B use 8 bit access, but Port G is 32 bit */
#define NUM_PORTS (GPIO_MINOR_B+1)

static volatile unsigned char *ports[NUM_PORTS] = { 
	R_PORT_PA_DATA, 
	R_PORT_PB_DATA,
};
static volatile unsigned char *shads[NUM_PORTS] = {
	&port_pa_data_shadow, 
	&port_pb_data_shadow
};

/* What direction bits that are user changeable 1=changeable*/
#ifndef CONFIG_ETRAX_PA_CHANGEABLE_DIR
#define CONFIG_ETRAX_PA_CHANGEABLE_DIR 0x00
#endif
#ifndef CONFIG_ETRAX_PB_CHANGEABLE_DIR
#define CONFIG_ETRAX_PB_CHANGEABLE_DIR 0x00
#endif

#ifndef CONFIG_ETRAX_PA_CHANGEABLE_BITS
#define CONFIG_ETRAX_PA_CHANGEABLE_BITS 0xFF
#endif
#ifndef CONFIG_ETRAX_PB_CHANGEABLE_BITS
#define CONFIG_ETRAX_PB_CHANGEABLE_BITS 0xFF
#endif


static unsigned char changeable_dir[NUM_PORTS] = { 
	CONFIG_ETRAX_PA_CHANGEABLE_DIR,
	CONFIG_ETRAX_PB_CHANGEABLE_DIR 
};
static unsigned char changeable_bits[NUM_PORTS] = { 
	CONFIG_ETRAX_PA_CHANGEABLE_BITS,
	CONFIG_ETRAX_PB_CHANGEABLE_BITS 
};

static volatile unsigned char *dir[NUM_PORTS] = { 
	R_PORT_PA_DIR, 
	R_PORT_PB_DIR 
};

static volatile unsigned char *dir_shadow[NUM_PORTS] = {
	&port_pa_dir_shadow, 
	&port_pb_dir_shadow 
};

/* Port G is 32 bit, handle it special, some bits are both inputs 
   and outputs at the same time, only some of the bits can change direction
   and some of them in groups of 8 bit. */
static unsigned long changeable_dir_g;
static unsigned long dir_g_in_bits;
static unsigned long dir_g_out_bits;
static unsigned long dir_g_shadow; /* 1=output */

#define USE_PORTS(priv) ((priv)->minor <= GPIO_MINOR_B)



static unsigned int 
gpio_poll(struct file *file,
	  poll_table *wait)
{
	unsigned int mask = 0;
	struct gpio_private *priv = (struct gpio_private *)file->private_data;
	unsigned long data;
	poll_wait(file, &priv->alarm_wq, wait);
	if (priv->minor == GPIO_MINOR_A) {
		unsigned long flags;
		unsigned long tmp;
		data = *R_PORT_PA_DATA;
		/* PA has support for high level interrupt -
		 * lets activate for those low and with highalarm set
		 */
		tmp = ~data & priv->highalarm & 0xFF;
		tmp = (tmp << R_IRQ_MASK1_SET__pa0__BITNR);
		save_flags(flags); cli();
		gpio_pa_irq_enabled_mask |= tmp;
		*R_IRQ_MASK1_SET = tmp;
		restore_flags(flags);
		
	} else if (priv->minor == GPIO_MINOR_B)
		data = *R_PORT_PB_DATA;
	else if (priv->minor == GPIO_MINOR_G)
		data = *R_PORT_G_DATA;
	else
		return 0;
	
	if ((data & priv->highalarm) ||
	    (~data & priv->lowalarm)) {
		mask = POLLIN|POLLRDNORM;
	}
	
	DP(printk("gpio_poll ready: mask 0x%08X\n", mask));
	return mask;
}

void etrax_gpio_wake_up_check(void)
{
	struct gpio_private *priv = alarmlist;
	unsigned long data = 0;
	while (priv) {
		if (USE_PORTS(priv)) {
			data = *priv->port;
		} else if (priv->minor == GPIO_MINOR_G) {
			data = *R_PORT_G_DATA;
		}
		if ((data & priv->highalarm) ||
		    (~data & priv->lowalarm)) {
			DP(printk("etrax_gpio_wake_up_check %i\n",priv->minor));
			wake_up_interruptible(&priv->alarm_wq);
		}
		priv = priv->next;
	}
}

static void 
gpio_poll_timer_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	if (gpio_some_alarms) {
		etrax_gpio_wake_up_check();
	}
}

static void 
gpio_pa_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	unsigned long tmp;
	/* Find what PA interrupts are active */
	tmp = (*R_IRQ_READ1);

	/* Find those that we have enabled */
	tmp &= gpio_pa_irq_enabled_mask;

	/* Clear them.. */
	*R_IRQ_MASK1_CLR = tmp;
	gpio_pa_irq_enabled_mask &= ~tmp;
	
	if (gpio_some_alarms) {
		etrax_gpio_wake_up_check();
	}
}


static ssize_t gpio_write(struct file * file, const char * buf, size_t count,
                          loff_t *off)
{
	struct gpio_private *priv = (struct gpio_private *)file->private_data;
	unsigned char data, clk_mask, data_mask, write_msb;
	unsigned long flags;
	ssize_t retval = count;
	if (priv->minor !=GPIO_MINOR_A && priv->minor != GPIO_MINOR_B) {
		return -EFAULT;
	}
    
	if (verify_area(VERIFY_READ, buf, count)) {
		return -EFAULT;
	}
	clk_mask = priv->clk_mask;
	data_mask = priv->data_mask;
	/* It must have been configured using the IO_CFG_WRITE_MODE */
	/* Perhaps a better error code? */
	if (clk_mask == 0 || data_mask == 0) {
		return -EPERM;
	}
	write_msb = priv->write_msb;
	D(printk("gpio_write: %lu to data 0x%02X clk 0x%02X msb: %i\n",count, data_mask, clk_mask, write_msb));
	while (count--) {
		int i;
		data = *buf++;
		if (priv->write_msb) {
			for (i = 7; i >= 0;i--) {
				save_flags(flags); cli();
				*priv->port = *priv->shadow &= ~clk_mask;
				if (data & 1<<i)
					*priv->port = *priv->shadow |= data_mask;
				else
					*priv->port = *priv->shadow &= ~data_mask;
			/* For FPGA: min 5.0ns (DCC) before CCLK high */
				*priv->port = *priv->shadow |= clk_mask;
				restore_flags(flags);
			}
		} else {
			for (i = 0; i <= 7;i++) {
				save_flags(flags); cli();
				*priv->port = *priv->shadow &= ~clk_mask;
				if (data & 1<<i)
					*priv->port = *priv->shadow |= data_mask;
				else
					*priv->port = *priv->shadow &= ~data_mask;
			/* For FPGA: min 5.0ns (DCC) before CCLK high */
				*priv->port = *priv->shadow |= clk_mask;
				restore_flags(flags);
			}
		}
	}
	return retval;
}



static int
gpio_open(struct inode *inode, struct file *filp)
{
	struct gpio_private *priv;
	int p = MINOR(inode->i_rdev);

	if (p > GPIO_MINOR_LAST)
		return -EINVAL;

	priv = (struct gpio_private *)kmalloc(sizeof(struct gpio_private), 
					      GFP_KERNEL);

	if (!priv)
		return -ENOMEM;

	priv->minor = p;

	/* initialize the io/alarm struct and link it into our alarmlist */

	priv->next = alarmlist;
	alarmlist = priv;
	if (USE_PORTS(priv)) { /* A and B */
		priv->port = ports[p];
		priv->shadow = shads[p];
		priv->dir = dir[p];
		priv->dir_shadow = dir_shadow[p];
		priv->changeable_dir = changeable_dir[p];
		priv->changeable_bits = changeable_bits[p];
	} else {
		priv->port = NULL;
		priv->shadow = NULL;
		priv->dir = NULL;
		priv->dir_shadow = NULL;
		priv->changeable_dir = 0;
		priv->changeable_bits = 0;
	}

	priv->highalarm = 0;
	priv->lowalarm = 0;
	priv->clk_mask = 0;
	priv->data_mask = 0;
	init_waitqueue_head(&priv->alarm_wq);

	filp->private_data = (void *)priv;

	return 0;
}

static int
gpio_release(struct inode *inode, struct file *filp)
{
	struct gpio_private *p = alarmlist;
	struct gpio_private *todel = (struct gpio_private *)filp->private_data;
	
	/* unlink from alarmlist and free the private structure */

	if (p == todel) {
		alarmlist = todel->next;
	} else {
		while (p->next != todel)
			p = p->next;
		p->next = todel->next;
	}

	kfree(todel);
	/* Check if there are still any alarms set */
	p = alarmlist;
	while (p) {
		if (p->highalarm | p->lowalarm) {
			gpio_some_alarms = 1;
			return 0;
		}
		p = p->next;
	}
	gpio_some_alarms = 0;
	
	return 0;
}

/* Main device API. ioctl's to read/set/clear bits, as well as to 
 * set alarms to wait for using a subsequent select().
 */

unsigned long inline setget_input(struct gpio_private *priv, unsigned long arg)
{
	/* Set direction 0=unchanged 1=input, 
	 * return mask with 1=input 
	 */
	unsigned long flags;
	if (USE_PORTS(priv)) {
		save_flags(flags); cli();
		*priv->dir = *priv->dir_shadow &= 
		~((unsigned char)arg & priv->changeable_dir);
		restore_flags(flags);
		return ~(*priv->dir_shadow) & 0xFF; /* Only 8 bits */
	} else if (priv->minor == GPIO_MINOR_G) {
		/* We must fiddle with R_GEN_CONFIG to change dir */
		save_flags(flags); cli();
		if (((arg & dir_g_in_bits) != arg) && 
		    (arg & changeable_dir_g)) {
			arg &= changeable_dir_g;
			/* Clear bits in genconfig to set to input */
			if (arg & (1<<0)) {
				genconfig_shadow &= ~IO_MASK(R_GEN_CONFIG,g0dir);
				dir_g_in_bits |= (1<<0);
				dir_g_out_bits &= ~(1<<0);
			}
			if ((arg & 0x0000FF00) == 0x0000FF00) {
				genconfig_shadow &= ~IO_MASK(R_GEN_CONFIG,g8_15dir);
				dir_g_in_bits |= 0x0000FF00;
				dir_g_out_bits &= ~0x0000FF00;
			}
			if ((arg & 0x00FF0000) == 0x00FF0000) {
				genconfig_shadow &= ~IO_MASK(R_GEN_CONFIG,g16_23dir);
				dir_g_in_bits |= 0x00FF0000;
				dir_g_out_bits &= ~0x00FF0000;
			}
			if (arg & (1<<24)) {
				genconfig_shadow &= ~IO_MASK(R_GEN_CONFIG,g24dir);
				dir_g_in_bits |= (1<<24);
				dir_g_out_bits &= ~(1<<24);
			}
			D(printk(KERN_INFO "gpio: SETINPUT on port G set "
				 "genconfig to 0x%08lX "
				 "in_bits: 0x%08lX "
				 "out_bits: 0x%08lX\n", 
				 (unsigned long)genconfig_shadow, 
				 dir_g_in_bits, dir_g_out_bits));
			*R_GEN_CONFIG = genconfig_shadow;
			/* Must be a >120 ns delay before writing this again */
				
		}
		restore_flags(flags);
		return dir_g_in_bits;
	}
	return 0;
} /* setget_input */

unsigned long inline setget_output(struct gpio_private *priv, unsigned long arg)
{
	unsigned long flags;
	if (USE_PORTS(priv)) {
		save_flags(flags); cli();
		*priv->dir = *priv->dir_shadow |= 
		  ((unsigned char)arg & priv->changeable_dir);
		restore_flags(flags);
		return *priv->dir_shadow;
	} else if (priv->minor == GPIO_MINOR_G) {
		/* We must fiddle with R_GEN_CONFIG to change dir */
		save_flags(flags); cli();
		if (((arg & dir_g_out_bits) != arg) &&
		    (arg & changeable_dir_g)) {
			/* Set bits in genconfig to set to output */
			if (arg & (1<<0)) {
				genconfig_shadow |= IO_MASK(R_GEN_CONFIG,g0dir);
				dir_g_out_bits |= (1<<0);
				dir_g_in_bits &= ~(1<<0);
			}
			if ((arg & 0x0000FF00) == 0x0000FF00) {
				genconfig_shadow |= IO_MASK(R_GEN_CONFIG,g8_15dir);
				dir_g_out_bits |= 0x0000FF00;
				dir_g_in_bits &= ~0x0000FF00;
			}
			if ((arg & 0x00FF0000) == 0x00FF0000) {
				genconfig_shadow |= IO_MASK(R_GEN_CONFIG,g16_23dir);
				dir_g_out_bits |= 0x00FF0000;
				dir_g_in_bits &= ~0x00FF0000;
			}
			if (arg & (1<<24)) {
				genconfig_shadow |= IO_MASK(R_GEN_CONFIG,g24dir);
				dir_g_out_bits |= (1<<24);
				dir_g_in_bits &= ~(1<<24);
			}
			D(printk(KERN_INFO "gpio: SETOUTPUT on port G set "
				 "genconfig to 0x%08lX "
				 "in_bits: 0x%08lX "
				 "out_bits: 0x%08lX\n", 
				 (unsigned long)genconfig_shadow, 
				 dir_g_in_bits, dir_g_out_bits));
			*R_GEN_CONFIG = genconfig_shadow;
			/* Must be a >120 ns delay before writing this again */
		}
		restore_flags(flags);
		return dir_g_out_bits & 0x7FFFFFFF;
	}
	return 0;
} /* setget_output */

static int
gpio_leds_ioctl(unsigned int cmd, unsigned long arg);

static int
gpio_ioctl(struct inode *inode, struct file *file,
	   unsigned int cmd, unsigned long arg)
{
	unsigned long flags;
	unsigned long val;
	struct gpio_private *priv = (struct gpio_private *)file->private_data;
	if (_IOC_TYPE(cmd) != ETRAXGPIO_IOCTYPE) {
		return -EINVAL;
	}

	switch (_IOC_NR(cmd)) {
	case IO_READBITS: /* Use IO_READ_INBITS and IO_READ_OUTBITS instead */
		// read the port
		if (USE_PORTS(priv)) {
			return *priv->port;
		} else if (priv->minor == GPIO_MINOR_G) {
			return (*R_PORT_G_DATA) & 0x7FFFFFFF;
		}
		break;
	case IO_SETBITS:
		save_flags(flags); cli();
		// set changeable bits with a 1 in arg
		if (USE_PORTS(priv)) {
			*priv->port = *priv->shadow |= 
			  ((unsigned char)arg & priv->changeable_bits);
		} else if (priv->minor == GPIO_MINOR_G) {
			*R_PORT_G_DATA = port_g_data_shadow |= (arg & dir_g_out_bits);
		}
		restore_flags(flags);
		break;
	case IO_CLRBITS:
		save_flags(flags); cli();
		// clear changeable bits with a 1 in arg
		if (USE_PORTS(priv)) {
			*priv->port = *priv->shadow &= 
			 ~((unsigned char)arg & priv->changeable_bits);
		} else if (priv->minor == GPIO_MINOR_G) {
			*R_PORT_G_DATA = port_g_data_shadow &= ~((unsigned long)arg & dir_g_out_bits);
		}
		restore_flags(flags);
		break;
	case IO_HIGHALARM:
		// set alarm when bits with 1 in arg go high
		priv->highalarm |= arg;
		gpio_some_alarms = 1;
		break;
	case IO_LOWALARM:
		// set alarm when bits with 1 in arg go low
		priv->lowalarm |= arg;
		gpio_some_alarms = 1;
		break;
	case IO_CLRALARM:
		// clear alarm for bits with 1 in arg
		priv->highalarm &= ~arg;
		priv->lowalarm  &= ~arg;
		{
			/* Must update gpio_some_alarms */
			struct gpio_private *p = alarmlist;
			int some_alarms;
			some_alarms = 0;
			while (p) {
				if (p->highalarm | p->lowalarm) {
					some_alarms = 1;
					break;
				}
				p = p->next;
			}
			gpio_some_alarms = some_alarms;
		}		
		break;
	case IO_READDIR: /* Use IO_SETGET_INPUT/OUTPUT instead! */
		/* Read direction 0=input 1=output */
		if (USE_PORTS(priv)) {
			return *priv->dir_shadow;
		} else if (priv->minor == GPIO_MINOR_G) {
			/* Note: Some bits are both in and out,
			 * Those that are dual is set here as well.
			 */
			return (dir_g_shadow | dir_g_out_bits) & 0x7FFFFFFF;
		}
	case IO_SETINPUT: /* Use IO_SETGET_INPUT instead! */
		/* Set direction 0=unchanged 1=input, 
		 * return mask with 1=input 
		 */
		return setget_input(priv, arg) & 0x7FFFFFFF;
		break;
	case IO_SETOUTPUT: /* Use IO_SETGET_OUTPUT instead! */
		/* Set direction 0=unchanged 1=output, 
		 * return mask with 1=output 
		 */
		return setget_output(priv, arg) & 0x7FFFFFFF;

	case IO_SHUTDOWN:
		SOFT_SHUTDOWN();
		break;
	case IO_GET_PWR_BT:
#if defined (CONFIG_ETRAX_SOFT_SHUTDOWN)
		return (*R_PORT_G_DATA & ( 1 << CONFIG_ETRAX_POWERBUTTON_BIT));
#else
		return 0;
#endif
		break;
	case IO_CFG_WRITE_MODE:
		priv->clk_mask = arg & 0xFF;
		priv->data_mask = (arg >> 8) & 0xFF;
		priv->write_msb = (arg >> 16) & 0x01;
		/* Check if we're allowed to change the bits and
		 * the direction is correct
		 */
		if (!((priv->clk_mask & priv->changeable_bits) &&
		      (priv->data_mask & priv->changeable_bits) &&
		      (priv->clk_mask & *priv->dir_shadow) &&
		      (priv->data_mask & *priv->dir_shadow)))
		{
			priv->clk_mask = 0;
			priv->data_mask = 0;
			return -EPERM;
		}
		break;
	case IO_READ_INBITS: 
		/* *arg is result of reading the input pins */
		if (USE_PORTS(priv)) {
			val = *priv->port;
		} else if (priv->minor == GPIO_MINOR_G) {
			val = *R_PORT_G_DATA;
		}
		if (copy_to_user((unsigned long*)arg, &val, sizeof(val)))
			return -EFAULT;
		return 0;
		break;
	case IO_READ_OUTBITS:
		 /* *arg is result of reading the output shadow */
		if (USE_PORTS(priv)) {
			val = *priv->shadow;
		} else if (priv->minor == GPIO_MINOR_G) {
			val = port_g_data_shadow;
		}
		if (copy_to_user((unsigned long*)arg, &val, sizeof(val)))
			return -EFAULT;
		break;
	case IO_SETGET_INPUT: 
		/* bits set in *arg is set to input,
		 * *arg updated with current input pins.
		 */
		if (copy_from_user(&val, (unsigned long*)arg, sizeof(val)))
			return -EFAULT;
		val = setget_input(priv, val);
		if (copy_to_user((unsigned long*)arg, &val, sizeof(val)))
			return -EFAULT;
		break;
	case IO_SETGET_OUTPUT:
		/* bits set in *arg is set to output,
		 * *arg updated with current output pins.
		 */
		if (copy_from_user(&val, (unsigned long*)arg, sizeof(val)))
			return -EFAULT;
		val = setget_output(priv, val);
		if (copy_to_user((unsigned long*)arg, &val, sizeof(val)))
			return -EFAULT;
		break;
	default:
		if (priv->minor == GPIO_MINOR_LEDS)
			return gpio_leds_ioctl(cmd, arg);
		else
			return -EINVAL;
	} /* switch */
	
	return 0;
}

static int
gpio_leds_ioctl(unsigned int cmd, unsigned long arg)
{
	unsigned char green;
	unsigned char red;

	switch (_IOC_NR(cmd)) {
	case IO_LEDACTIVE_SET:
		green = ((unsigned char) arg) & 1;
		red   = (((unsigned char) arg) >> 1) & 1;
		LED_ACTIVE_SET_G(green);
		LED_ACTIVE_SET_R(red);
		break;

	case IO_LED_SETBIT:
		LED_BIT_SET(arg);
		break;

	case IO_LED_CLRBIT:
		LED_BIT_CLR(arg);
		break;

	default:
		return -EINVAL;
	} /* switch */

	return 0;
}

const struct file_operations gpio_fops = {
	owner:       THIS_MODULE,
	poll:        gpio_poll,
	ioctl:       gpio_ioctl,
	write:       gpio_write,
	open:        gpio_open,
	release:     gpio_release,
};


static void __init gpio_init_port_g(void)
{
#define GROUPA (0x0000FF3F)
#define GROUPB (1<<6 | 1<<7)
#define GROUPC (1<<30 | 1<<31)
#define GROUPD (0x3FFF0000)
#define GROUPD_LOW (0x00FF0000)
	unsigned long used_in_bits = 0;
	unsigned long used_out_bits = 0;
	if (genconfig_shadow & IO_STATE(R_GEN_CONFIG, scsi0, select)){
		used_in_bits  |= GROUPA | GROUPB | 0 | 0;
		used_out_bits |= GROUPA | GROUPB | 0 | 0;
	}
	if (genconfig_shadow & IO_STATE(R_GEN_CONFIG, ata, select)) {
		used_in_bits  |= GROUPA | GROUPB | GROUPC | (GROUPD & ~(1<<25|1<<26));
		used_out_bits |= GROUPA | GROUPB | GROUPC | GROUPD;
	}

	if (genconfig_shadow & IO_STATE(R_GEN_CONFIG, par0, select)) {
		used_in_bits  |= (GROUPA & ~(1<<0)) | 0 | 0 | 0;
		used_out_bits |= (GROUPA & ~(1<<0)) | 0 | 0 | 0;
	}
	if (genconfig_shadow & IO_STATE(R_GEN_CONFIG, ser2, select)) {
		used_in_bits  |= 0 | GROUPB | 0 | 0;
		used_out_bits |= 0 | GROUPB | 0 | 0;
	}
	/* mio same as shared RAM ? */
	if (genconfig_shadow & IO_STATE(R_GEN_CONFIG, mio, select)) {
		used_in_bits  |= (GROUPA & ~(1<<0)) | 0 |0 |GROUPD_LOW;
		used_out_bits |= (GROUPA & ~(1<<0|1<<1|1<<2)) | 0 |0 |GROUPD_LOW;
	}
	if (genconfig_shadow & IO_STATE(R_GEN_CONFIG, scsi1, select)) {
		used_in_bits  |= 0 | 0 | GROUPC | GROUPD;
		used_out_bits |= 0 | 0 | GROUPC | GROUPD;
	}
	if (genconfig_shadow & IO_STATE(R_GEN_CONFIG, scsi0w, select)) {
		used_in_bits  |= GROUPA | GROUPB | 0 | (GROUPD_LOW | 1<<24);
		used_out_bits |= GROUPA | GROUPB | 0 | (GROUPD_LOW | 1<<24 | 1<<25|1<<26);
	}

	if (genconfig_shadow & IO_STATE(R_GEN_CONFIG, par1, select)) {
		used_in_bits  |= 0 | 0 | 0 | (GROUPD & ~(1<<24));
		used_out_bits |= 0 | 0 | 0 | (GROUPD & ~(1<<24));
	}
	if (genconfig_shadow & IO_STATE(R_GEN_CONFIG, ser3, select)) {
		used_in_bits  |= 0 | 0 | GROUPC | 0;
		used_out_bits |= 0 | 0 | GROUPC | 0;
	}
	/* mio same as shared RAM-W? */
	if (genconfig_shadow & IO_STATE(R_GEN_CONFIG, mio_w, select)) {
		used_in_bits  |= (GROUPA & ~(1<<0)) | 0 | 0 |GROUPD_LOW;
		used_out_bits |= (GROUPA & ~(1<<0|1<<1|1<<2)) | 0 | 0 |GROUPD_LOW;
	}
	/* TODO: USB p2, parw, sync ser3? */

	/* Initialise the dir_g_shadow etc. depending on genconfig */
	/* 0=input 1=output */
	if (genconfig_shadow & IO_STATE(R_GEN_CONFIG, g0dir, out)) 
		dir_g_shadow |= (1 << 0);
	if (genconfig_shadow & IO_STATE(R_GEN_CONFIG, g8_15dir, out))
		dir_g_shadow |= 0x0000FF00;
	if (genconfig_shadow & IO_STATE(R_GEN_CONFIG, g16_23dir, out))
		dir_g_shadow |= 0x00FF0000;
	if (genconfig_shadow & IO_STATE(R_GEN_CONFIG, g24dir, out))
		dir_g_shadow |= (1 << 24);

	dir_g_in_bits = ~used_in_bits;
	dir_g_out_bits = ~used_out_bits;

	changeable_dir_g = 0x01FFFF01; /* all that can change dir */
	changeable_dir_g &= dir_g_out_bits;
	changeable_dir_g &= dir_g_in_bits;
	/* Correct the bits that can change direction */ 
	dir_g_out_bits &= ~changeable_dir_g;
	dir_g_out_bits |= dir_g_shadow;
	dir_g_in_bits &= ~changeable_dir_g;
	dir_g_in_bits |= (~dir_g_shadow & changeable_dir_g);


	printk(KERN_INFO
	       "GPIO port G: in_bits: 0x%08lX out_bits: 0x%08lX val: %08lX\n",
	       dir_g_in_bits, dir_g_out_bits, (unsigned long)*R_PORT_G_DATA);
	printk(KERN_INFO
	       "GPIO port G: dir: %08lX changeable: %08lX\n", 
	       dir_g_shadow, changeable_dir_g);
}

/* main driver initialization routine, called from mem.c */

static __init int
gpio_init(void)
{
	int res;
#if defined (CONFIG_ETRAX_CSP0_LEDS)
	int i;
#endif

	/* do the formalities */

	res = register_chrdev(GPIO_MAJOR, gpio_name, &gpio_fops);
	if (res < 0) {
		printk(KERN_ERR "gpio: couldn't get a major number.\n");
		return res;
	}

	/* Clear all leds */
#if defined (CONFIG_ETRAX_CSP0_LEDS) ||  defined (CONFIG_ETRAX_PA_LEDS) || defined (CONFIG_ETRAX_PB_LEDS)
	LED_NETWORK_SET(0);
	LED_ACTIVE_SET(0);
	LED_DISK_READ(0);
	LED_DISK_WRITE(0);

#if defined (CONFIG_ETRAX_CSP0_LEDS)
	for (i = 0; i < 32; i++) {
		LED_BIT_SET(i);
	}
#endif

#endif
	gpio_init_port_g();
	printk(KERN_INFO
	       "ETRAX 100LX GPIO driver v2.5, (c) 2001, 2002 Axis Communications AB\n");
	/* We call etrax_gpio_wake_up_check() from timer interrupt and
	 * from cpu_idle() in kernel/process.c
	 * The check in cpu_idle() reduces latency from ~15 ms to ~6 ms
	 * in some tests.
	 */  
	if (request_irq(TIMER0_IRQ_NBR, gpio_poll_timer_interrupt,
			SA_SHIRQ | SA_INTERRUPT,"gpio poll", NULL)) {
		printk(KERN_CRIT "err: timer0 irq for gpio\n");
	}
	if (request_irq(PA_IRQ_NBR, gpio_pa_interrupt,
			SA_SHIRQ | SA_INTERRUPT,"gpio PA", NULL)) {
		printk(KERN_CRIT "err: PA irq for gpio\n");
	}
	
	return res;
}

/* this makes sure that gpio_init is called during kernel boot */

module_init(gpio_init);
