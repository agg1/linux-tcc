/*
 * copyright (C) 1999/2000 by Henning Zabel <henning@uni-paderborn.de>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */


/*
 *	USB-Kernel Driver for the Mustek MDC800 Digital Camera
 *	(c) 1999/2000 Henning Zabel <henning@uni-paderborn.de>
 *
 *
 * The driver brings the USB functions of the MDC800 to Linux.
 * To use the Camera you must support the USB Protocoll of the camera
 * to the Kernel Node.
 * The Driver uses a misc device Node. Create it with :
 * mknod /dev/mustek c 180 32
 *
 * The driver supports only one camera.
 * 
 * Fix: mdc800 used sleep_on and slept with io_lock held.
 * Converted sleep_on to waitqueues with schedule_timeout and made io_lock
 * a semaphore from a spinlock.
 * by Oliver Neukum <520047054719-0001@t-online.de>
 * (02/12/2001)
 * 
 * Identify version on module load.
 * (08/04/2001) gb
 *
 * version 0.7.5
 * Fixed potential SMP races with Spinlocks.
 * Thanks to Oliver Neukum <oliver.neukum@lrz.uni-muenchen.de> who 
 * noticed the race conditions.
 * (30/10/2000)
 *
 * Fixed: Setting urb->dev before submitting urb.
 * by Greg KH <greg@kroah.com>
 * (13/10/2000)
 *
 * version 0.7.3
 * bugfix : The mdc800->state field gets set to READY after the
 * the diconnect function sets it to NOT_CONNECTED. This makes the
 * driver running like the camera is connected and causes some
 * hang ups.
 *
 * version 0.7.1
 * MOD_INC and MOD_DEC are changed in usb_probe to prevent load/unload
 * problems when compiled as Module.
 * (04/04/2000)
 *
 * The mdc800 driver gets assigned the USB Minor 32-47. The Registration
 * was updated to use these values.
 * (26/03/2000)
 *
 * The Init und Exit Module Function are updated.
 * (01/03/2000)
 *
 * version 0.7.0
 * Rewrite of the driver : The driver now uses URB's. The old stuff
 * has been removed.
 *
 * version 0.6.0
 * Rewrite of this driver: The Emulation of the rs232 protocoll
 * has been removed from the driver. A special executeCommand function
 * for this driver is included to gphoto.
 * The driver supports two kind of communication to bulk endpoints.
 * Either with the dev->bus->ops->bulk... or with callback function.
 * (09/11/1999)
 *
 * version 0.5.0:
 * first Version that gets a version number. Most of the needed
 * functions work.
 * (20/10/1999)
 */

#include <linux/version.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <linux/spinlock.h>
#include <linux/errno.h>
#include <linux/random.h>
#include <linux/poll.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/smp_lock.h>

#include <linux/usb.h>

/*
 * Version Information
 */
#define DRIVER_VERSION "v0.7.5 (30/10/2000)"
#define DRIVER_AUTHOR "Henning Zabel <henning@uni-paderborn.de>"
#define DRIVER_DESC "USB Driver for Mustek MDC800 Digital Camera"

/* Vendor and Product Information */
#define MDC800_VENDOR_ID 	0x055f
#define MDC800_PRODUCT_ID	0xa800

/* Timeouts (msec) */
#define TO_DOWNLOAD_GET_READY		1500
#define TO_DOWNLOAD_GET_BUSY		1500
#define TO_WRITE_GET_READY		1000
#define TO_DEFAULT_COMMAND		5000
#define TO_READ_FROM_IRQ 		TO_DEFAULT_COMMAND
#define TO_GET_READY			TO_DEFAULT_COMMAND

/* Minor Number of the device (create with mknod /dev/mustek c 180 32) */
#define MDC800_DEVICE_MINOR_BASE 32


/**************************************************************************
	Data and structs
***************************************************************************/


typedef enum {
	NOT_CONNECTED, READY, WORKING, DOWNLOAD
} mdc800_state;


/* Data for the driver */
struct mdc800_data
{
	struct usb_device *	dev;			// Device Data
	mdc800_state 		state;

	unsigned int		endpoint [4];

	struct urb *		irq_urb;
	wait_queue_head_t	irq_wait;
	int                     irq_woken;
	char*			irq_urb_buffer;

	int			camera_busy;          // is camera busy ?
	int 			camera_request_ready; // Status to synchronize with irq
	char 			camera_response [8];  // last Bytes send after busy

	struct urb *   		write_urb;
	char*			write_urb_buffer;
	wait_queue_head_t	write_wait;
	int                     written;


	struct urb *		download_urb;
	char*			download_urb_buffer;
	wait_queue_head_t	download_wait;
	int                     downloaded;
	int			download_left;		// Bytes left to download ?


	/* Device Data */
	char			out [64];	// Answer Buffer
	int 			out_ptr;	// Index to the first not readen byte
	int			out_count;	// Bytes in the buffer

	int			open;		// Camera device open ?
	struct semaphore	io_lock;	// IO -lock

	char 			in [8];		// Command Input Buffer
	int  			in_count;

	int			pic_index;	// Cache for the Imagesize (-1 for nothing cached )
	int			pic_len;
};


/* Specification of the Endpoints */
static struct usb_endpoint_descriptor mdc800_ed [4] =
{
	{ 0,0, 0x01, 0x02,  8, 0,0,0 },
	{ 0,0, 0x82, 0x03,  8, 0,0,0 },
	{ 0,0, 0x03, 0x02, 64, 0,0,0 },
	{ 0,0, 0x84, 0x02, 64, 0,0,0 }
};


/* The Variable used by the driver */
static struct mdc800_data* mdc800=0;


/***************************************************************************
	The USB Part of the driver
****************************************************************************/

static int mdc800_endpoint_equals (struct usb_endpoint_descriptor *a,struct usb_endpoint_descriptor *b)
{
	return (
		   ( a->bEndpointAddress == b->bEndpointAddress )
		&& ( a->bmAttributes     == b->bmAttributes     )
		&& ( a->wMaxPacketSize   == b->wMaxPacketSize   )
	);
}


/*
 * Checks wether the camera responds busy
 */
static int mdc800_isBusy (char* ch)
{
	int i=0;
	while (i<8)
	{
		if (ch [i] != (char)0x99)
			return 0;
		i++;
	}
	return 1;
}


/*
 * Checks wether the Camera is ready
 */
static int mdc800_isReady (char *ch)
{
	int i=0;
	while (i<8)
	{
		if (ch [i] != (char)0xbb)
			return 0;
		i++;
	}
	return 1;
}



/*
 * USB IRQ Handler for InputLine
 */
static void mdc800_usb_irq (struct urb *urb)
{
	int data_received=0, wake_up;
	unsigned char* b=urb->transfer_buffer;
	struct mdc800_data* mdc800=urb->context;

	if (urb->status >= 0)
	{

		//dbg ("%i %i %i %i %i %i %i %i \n",b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7]);

		if (mdc800_isBusy (b))
		{
			if (!mdc800->camera_busy)
			{
				mdc800->camera_busy=1;
				dbg ("gets busy");
			}
		}
		else
		{
			if (mdc800->camera_busy && mdc800_isReady (b))
			{
				mdc800->camera_busy=0;
				dbg ("gets ready");
			}
		}
		if (!(mdc800_isBusy (b) || mdc800_isReady (b)))
		{
			/* Store Data in camera_answer field */
			dbg ("%i %i %i %i %i %i %i %i ",b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7]);

			memcpy (mdc800->camera_response,b,8);
			data_received=1;
		}
	}
	wake_up= ( mdc800->camera_request_ready > 0 )
		&&
		(
			((mdc800->camera_request_ready == 1) && (!mdc800->camera_busy))
		||
			((mdc800->camera_request_ready == 2) && data_received)
		||
			((mdc800->camera_request_ready == 3) && (mdc800->camera_busy))
		||
			(urb->status < 0)
		);

	if (wake_up)
	{
		mdc800->camera_request_ready=0;
		mdc800->irq_woken=1;
		wake_up_interruptible (&mdc800->irq_wait);
	}
}


/*
 * Waits a while until the irq responds that camera is ready
 *
 *  mode : 0: Wait for camera gets ready
 *         1: Wait for receiving data
 *         2: Wait for camera gets busy
 *
 * msec: Time to wait
 */
static int mdc800_usb_waitForIRQ (int mode, int msec)
{
        DECLARE_WAITQUEUE(wait, current);

	mdc800->camera_request_ready=1+mode;

	add_wait_queue(&mdc800->irq_wait, &wait);
	set_current_state(TASK_INTERRUPTIBLE);
	if (!mdc800->irq_woken)
	{
	        schedule_timeout (msec*HZ/1000);
	}
        remove_wait_queue(&mdc800->irq_wait, &wait);
	set_current_state(TASK_RUNNING);
	mdc800->irq_woken = 0;

	if (mdc800->camera_request_ready>0)
	{
		mdc800->camera_request_ready=0;
		err ("timeout waiting for camera.");
		return -1;
	}
	
	if (mdc800->state == NOT_CONNECTED)
	{
		warn ("Camera gets disconnected during waiting for irq.");
		mdc800->camera_request_ready=0;
		return -2;
	}
	
	return 0;
}


/*
 * The write_urb callback function
 */
static void mdc800_usb_write_notify (struct urb *urb)
{
	struct mdc800_data* mdc800=urb->context;

	if (urb->status != 0)
	{
		err ("writing command fails (status=%i)", urb->status);
	}
	else
	{	
		mdc800->state=READY;
	}
	mdc800->written = 1;
	wake_up_interruptible (&mdc800->write_wait);
}


/*
 * The download_urb callback function
 */
static void mdc800_usb_download_notify (struct urb *urb)
{
	struct mdc800_data* mdc800=urb->context;

	if (urb->status == 0)
	{
		/* Fill output buffer with these data */
		memcpy (mdc800->out,  urb->transfer_buffer, 64);
		mdc800->out_count=64;
		mdc800->out_ptr=0;
		mdc800->download_left-=64;
		if (mdc800->download_left == 0)
		{
			mdc800->state=READY;
		}
	}
	else
	{
		err ("request bytes fails (status:%i)", urb->status);
	}
	mdc800->downloaded = 1;
	wake_up_interruptible (&mdc800->download_wait);
}


/***************************************************************************
	Probing for the Camera
 ***************************************************************************/

static struct usb_driver mdc800_usb_driver;

/*
 * Callback to search the Mustek MDC800 on the USB Bus
 */
static void* mdc800_usb_probe (struct usb_device *dev ,unsigned int ifnum,
			       const struct usb_device_id *id)
{
	int i,j;
	struct usb_interface_descriptor	*intf_desc;
	int irq_interval=0;

	dbg ("(mdc800_usb_probe) called.");


	if (mdc800->dev != 0)
	{
		warn ("only one Mustek MDC800 is supported.");
		return 0;
	}

	if (dev->descriptor.bNumConfigurations != 1)
	{
		err ("probe fails -> wrong Number of Configuration");
		return 0;
	}
	intf_desc=&dev->actconfig->interface[ifnum].altsetting[0];

	if (
			( intf_desc->bInterfaceClass != 0xff )
		||	( intf_desc->bInterfaceSubClass != 0 )
		|| ( intf_desc->bInterfaceProtocol != 0 )
		|| ( intf_desc->bNumEndpoints != 4)
	)
	{
		err ("probe fails -> wrong Interface");
		return 0;
	}

	/* Check the Endpoints */
	for (i=0; i<4; i++)
	{
		mdc800->endpoint[i]=-1;
		for (j=0; j<4; j++)
		{
			if (mdc800_endpoint_equals (&intf_desc->endpoint [j],&mdc800_ed [i]))
			{
				mdc800->endpoint[i]=intf_desc->endpoint [j].bEndpointAddress ;
				if (i==1)
				{
					irq_interval=intf_desc->endpoint [j].bInterval;
				}

				continue;
			}
		}
		if (mdc800->endpoint[i] == -1)
		{
			err ("probe fails -> Wrong Endpoints.");
			return 0;
		}
	}


	usb_driver_claim_interface (&mdc800_usb_driver, &dev->actconfig->interface[ifnum], mdc800);
	if (usb_set_interface (dev, ifnum, 0) < 0)
	{
		err ("MDC800 Configuration fails.");
		return 0;
	}

	info ("Found Mustek MDC800 on USB.");

	down (&mdc800->io_lock);

	mdc800->dev=dev;
	mdc800->open=0;

	/* Setup URB Structs */
	FILL_INT_URB (
		mdc800->irq_urb,
		mdc800->dev,
		usb_rcvintpipe (mdc800->dev,mdc800->endpoint [1]),
		mdc800->irq_urb_buffer,
		8,
		mdc800_usb_irq,
		mdc800,
		irq_interval
	);

	FILL_BULK_URB (
		mdc800->write_urb,
		mdc800->dev,
		usb_sndbulkpipe (mdc800->dev, mdc800->endpoint[0]),
		mdc800->write_urb_buffer,
		8,
		mdc800_usb_write_notify,
		mdc800
	);

	FILL_BULK_URB (
		mdc800->download_urb,
		mdc800->dev,
		usb_rcvbulkpipe (mdc800->dev, mdc800->endpoint [3]),
		mdc800->download_urb_buffer,
		64,
		mdc800_usb_download_notify,
		mdc800
	);

	mdc800->state=READY;

	up (&mdc800->io_lock);
	
	return mdc800;
}


/*
 * Disconnect USB device (maybe the MDC800)
 */
static void mdc800_usb_disconnect (struct usb_device *dev,void* ptr)
{
	struct mdc800_data* mdc800=(struct mdc800_data*) ptr;

	dbg ("(mdc800_usb_disconnect) called");

	if (mdc800->state == NOT_CONNECTED)
		return;

	down (&mdc800->io_lock);
	mdc800->state=NOT_CONNECTED;

	usb_unlink_urb (mdc800->irq_urb);
	usb_unlink_urb (mdc800->write_urb);
	usb_unlink_urb (mdc800->download_urb);

	usb_driver_release_interface (&mdc800_usb_driver, &dev->actconfig->interface[1]);

	mdc800->dev=0;
	up (&mdc800->io_lock);
	info ("Mustek MDC800 disconnected from USB.");
}


/***************************************************************************
	The Misc device Part (file_operations)
****************************************************************************/

/*
 * This Function calc the Answersize for a command.
 */
static int mdc800_getAnswerSize (char command)
{
	switch ((unsigned char) command)
	{
		case 0x2a:
		case 0x49:
		case 0x51:
		case 0x0d:
		case 0x20:
		case 0x07:
		case 0x01:
		case 0x25:
		case 0x00:
			return 8;

		case 0x05:
		case 0x3e:
			return mdc800->pic_len;

		case 0x09:
			return 4096;

		default:
			return 0;
	}
}


/*
 * Init the device: (1) alloc mem (2) Increase MOD Count ..
 */
static int mdc800_device_open (struct inode* inode, struct file *file)
{
	int retval=0;
	int errn=0;

	down (&mdc800->io_lock);
	
	if (mdc800->state == NOT_CONNECTED)
	{
		errn=-EBUSY;
		goto error_out;
	}
	if (mdc800->open)
	{
		errn=-EBUSY;
		goto error_out;
	}

	mdc800->in_count=0;
	mdc800->out_count=0;
	mdc800->out_ptr=0;
	mdc800->pic_index=0;
	mdc800->pic_len=-1;
	mdc800->download_left=0;

	mdc800->camera_busy=0;
	mdc800->camera_request_ready=0;

	retval=0;
	mdc800->irq_urb->dev = mdc800->dev;
	if (usb_submit_urb (mdc800->irq_urb))
	{
		err ("request USB irq fails (submit_retval=%i urb_status=%i).",retval, mdc800->irq_urb->status);
		errn = -EIO;
		goto error_out;
	}

	mdc800->open=1;
	dbg ("Mustek MDC800 device opened.");

error_out:
	up (&mdc800->io_lock);
	return errn;
}


/*
 * Close the Camera and release Memory
 */
static int mdc800_device_release (struct inode* inode, struct file *file)
{
	int retval=0;
	dbg ("Mustek MDC800 device closed.");

	down (&mdc800->io_lock);
	if (mdc800->open && (mdc800->state != NOT_CONNECTED))
	{
		usb_unlink_urb (mdc800->irq_urb);
		usb_unlink_urb (mdc800->write_urb);
		usb_unlink_urb (mdc800->download_urb);
		mdc800->open=0;
	}
	else
	{
		retval=-EIO;
	}

        up(&mdc800->io_lock);
	return retval;
}


/*
 * The Device read callback Function
 */
static ssize_t mdc800_device_read (struct file *file, char *buf, size_t len, loff_t *pos)
{
	size_t left=len, sts=len; /* single transfer size */
	char* ptr=buf;
	DECLARE_WAITQUEUE(wait, current);

	down (&mdc800->io_lock);
	if (mdc800->state == NOT_CONNECTED)
	{
		up (&mdc800->io_lock);
		return -EBUSY;
	}
	if (mdc800->state == WORKING)
	{
		warn ("Illegal State \"working\" reached during read ?!");
		up (&mdc800->io_lock);
		return -EBUSY;
	}
	if (!mdc800->open)
	{
		up (&mdc800->io_lock);
		return -EBUSY;
	}

	while (left)
	{
		if (signal_pending (current)) 
		{
			up (&mdc800->io_lock);
			return -EINTR;
		}

		sts=left > (mdc800->out_count-mdc800->out_ptr)?mdc800->out_count-mdc800->out_ptr:left;

		if (sts <= 0)
		{
			/* Too less Data in buffer */
			if (mdc800->state == DOWNLOAD)
			{
				mdc800->out_count=0;
				mdc800->out_ptr=0;

				/* Download -> Request new bytes */
				mdc800->download_urb->dev = mdc800->dev;
				if (usb_submit_urb (mdc800->download_urb))
				{
					err ("Can't submit download urb (status=%i)",mdc800->download_urb->status);
					up (&mdc800->io_lock);
					return len-left;
				}
				add_wait_queue(&mdc800->download_wait, &wait);
				set_current_state(TASK_INTERRUPTIBLE);
				if (!mdc800->downloaded)
				{
				        schedule_timeout (TO_DOWNLOAD_GET_READY*HZ/1000);
				}
                                set_current_state(TASK_RUNNING);
				remove_wait_queue(&mdc800->download_wait, &wait);
				mdc800->downloaded = 0;
				if (mdc800->download_urb->status != 0)
				{
					err ("request download-bytes fails (status=%i)",mdc800->download_urb->status);
					up (&mdc800->io_lock);
					return len-left;
				}
			}
			else
			{
				/* No more bytes -> that's an error*/
				up (&mdc800->io_lock);
				return -EIO;
			}
		}
		else
		{
			/* memcpy Bytes */
			if (copy_to_user(ptr, &mdc800->out [mdc800->out_ptr], sts)) {
				up (&mdc800->io_lock);
				return -EFAULT;
			}
			ptr+=sts;
			left-=sts;
			mdc800->out_ptr+=sts;
		}
	}

	up (&mdc800->io_lock);
	return len-left;
}


/*
 * The Device write callback Function
 * If a 8Byte Command is received, it will be send to the camera.
 * After this the driver initiates the request for the answer or
 * just waits until the camera becomes ready.
 */
static ssize_t mdc800_device_write (struct file *file, const char *buf, size_t len, loff_t *pos)
{
	size_t i=0;
	DECLARE_WAITQUEUE(wait, current);

	down (&mdc800->io_lock);
	if (mdc800->state != READY)
	{
		up (&mdc800->io_lock);
		return -EBUSY;
	}
	if (!mdc800->open )
	{
		up (&mdc800->io_lock);
		return -EBUSY;
	}

	while (i<len)
	{
		unsigned char c;
		if (signal_pending (current)) 
		{
			up (&mdc800->io_lock);
			return -EINTR;
		}
		
		if(get_user(c, buf+i))
		{
			up(&mdc800->io_lock);
			return -EFAULT;
		}

		/* check for command start */
		if (c == 0x55)
		{
			mdc800->in_count=0;
			mdc800->out_count=0;
			mdc800->out_ptr=0;
			mdc800->download_left=0;
		}

		/* save command byte */
		if (mdc800->in_count < 8)
		{
			mdc800->in[mdc800->in_count] = c;
			mdc800->in_count++;
		}
		else
		{
			up (&mdc800->io_lock);
			return -EIO;
		}

		/* Command Buffer full ? -> send it to camera */
		if (mdc800->in_count == 8)
		{
			int answersize;

			if (mdc800_usb_waitForIRQ (0,TO_GET_READY))
			{
				err ("Camera didn't get ready.\n");
				up (&mdc800->io_lock);
				return -EIO;
			}

			answersize=mdc800_getAnswerSize (mdc800->in[1]);

			mdc800->state=WORKING;
			memcpy (mdc800->write_urb->transfer_buffer, mdc800->in,8);
			mdc800->write_urb->dev = mdc800->dev;
			if (usb_submit_urb (mdc800->write_urb))
			{
				err ("submitting write urb fails (status=%i)", mdc800->write_urb->status);
				up (&mdc800->io_lock);
				return -EIO;
			}
			add_wait_queue(&mdc800->write_wait, &wait);
			set_current_state(TASK_INTERRUPTIBLE);
			if (!mdc800->written)
			{
			        schedule_timeout (TO_WRITE_GET_READY*HZ/1000);
			}
                        set_current_state(TASK_RUNNING);
			remove_wait_queue(&mdc800->write_wait, &wait);
			mdc800->written = 0;
			if (mdc800->state == WORKING)
			{
				usb_unlink_urb (mdc800->write_urb);
				up (&mdc800->io_lock);
				return -EIO;
			}

			switch ((unsigned char) mdc800->in[1])
			{
				case 0x05: /* Download Image */
				case 0x3e: /* Take shot in Fine Mode (WCam Mode) */
					if (mdc800->pic_len < 0)
					{
						err ("call 0x07 before 0x05,0x3e");
						mdc800->state=READY;
						up (&mdc800->io_lock);
						return -EIO;
					}
					mdc800->pic_len=-1;

				case 0x09: /* Download Thumbnail */
					mdc800->download_left=answersize+64;
					mdc800->state=DOWNLOAD;
					mdc800_usb_waitForIRQ (0,TO_DOWNLOAD_GET_BUSY);
					break;


				default:
					if (answersize)
					{

						if (mdc800_usb_waitForIRQ (1,TO_READ_FROM_IRQ))
						{
							err ("requesting answer from irq fails");
							up (&mdc800->io_lock);
							return -EIO;
						}

						/* Write dummy data, (this is ugly but part of the USB Protocol */
						/* if you use endpoint 1 as bulk and not as irq) */
						memcpy (mdc800->out, mdc800->camera_response,8);

						/* This is the interpreted answer */
						memcpy (&mdc800->out[8], mdc800->camera_response,8);

						mdc800->out_ptr=0;
						mdc800->out_count=16;

						/* Cache the Imagesize, if command was getImageSize */
						if (mdc800->in [1] == (char) 0x07)
						{
							mdc800->pic_len=(int) 65536*(unsigned char) mdc800->camera_response[0]+256*(unsigned char) mdc800->camera_response[1]+(unsigned char) mdc800->camera_response[2];

							dbg ("cached imagesize = %i",mdc800->pic_len);
						}

					}
					else
					{
						if (mdc800_usb_waitForIRQ (0,TO_DEFAULT_COMMAND))
						{
							err ("Command Timeout.");
							up (&mdc800->io_lock);
							return -EIO;
						}
					}
					mdc800->state=READY;
					break;
			}
		}
		i++;
	}
	up (&mdc800->io_lock);
	return i;
}


/***************************************************************************
	Init and Cleanup this driver (Structs and types)
****************************************************************************/

/* File Operations of this drivers */
static const struct file_operations mdc800_device_ops = {
	owner:		THIS_MODULE,
	read:		mdc800_device_read,
	write:		mdc800_device_write,
	open:		mdc800_device_open,
	release:	mdc800_device_release,
};



static struct usb_device_id mdc800_table [] = {
	{ USB_DEVICE(MDC800_VENDOR_ID, MDC800_PRODUCT_ID) },
	{ }						/* Terminating entry */
};

MODULE_DEVICE_TABLE (usb, mdc800_table);
/*
 * USB Driver Struct for this device
 */
static struct usb_driver mdc800_usb_driver =
{
	name:		"mdc800",
	probe:		mdc800_usb_probe,
	disconnect:	mdc800_usb_disconnect,
	fops:		&mdc800_device_ops,
	minor:		MDC800_DEVICE_MINOR_BASE,
	id_table:	mdc800_table
};



/************************************************************************
	Init and Cleanup this driver (Main Functions)
*************************************************************************/

#define try(A)           if ((A) == 0) goto cleanup_on_fail;
#define try_free_mem(A)  if (A != 0) { kfree (A); A=0; }
#define try_free_urb(A)  if (A != 0) { usb_free_urb (A); A=0; }

int __init usb_mdc800_init (void)
{
	/* Allocate Memory */
	try (mdc800=kmalloc (sizeof (struct mdc800_data), GFP_KERNEL));

	memset(mdc800, 0, sizeof(struct mdc800_data));
	mdc800->dev=0;
	mdc800->open=0;
	mdc800->state=NOT_CONNECTED;
	init_MUTEX (&mdc800->io_lock);

	init_waitqueue_head (&mdc800->irq_wait);
	init_waitqueue_head (&mdc800->write_wait);
	init_waitqueue_head (&mdc800->download_wait);

	mdc800->irq_woken = 0;
	mdc800->downloaded = 0;
	mdc800->written = 0;

	try (mdc800->irq_urb_buffer=kmalloc (8, GFP_KERNEL));
	try (mdc800->write_urb_buffer=kmalloc (8, GFP_KERNEL));
	try (mdc800->download_urb_buffer=kmalloc (64, GFP_KERNEL));

	try (mdc800->irq_urb=usb_alloc_urb (0));
	try (mdc800->download_urb=usb_alloc_urb (0));
	try (mdc800->write_urb=usb_alloc_urb (0));

	/* Register the driver */
	if (usb_register (&mdc800_usb_driver) < 0)
		goto cleanup_on_fail;

	info (DRIVER_VERSION ":" DRIVER_DESC);

	return 0;

	/* Clean driver up, when something fails */

cleanup_on_fail:

	if (mdc800 != 0)
	{
		err ("can't alloc memory!");

		try_free_mem (mdc800->download_urb_buffer);
		try_free_mem (mdc800->write_urb_buffer);
		try_free_mem (mdc800->irq_urb_buffer);

		try_free_urb (mdc800->write_urb);
		try_free_urb (mdc800->download_urb);
		try_free_urb (mdc800->irq_urb);

		kfree (mdc800);
	}
	mdc800=0;
	return -1;
}


void __exit usb_mdc800_cleanup (void)
{
	usb_deregister (&mdc800_usb_driver);

	usb_free_urb (mdc800->irq_urb);
	usb_free_urb (mdc800->download_urb);
	usb_free_urb (mdc800->write_urb);

	kfree (mdc800->irq_urb_buffer);
	kfree (mdc800->write_urb_buffer);
	kfree (mdc800->download_urb_buffer);

	kfree (mdc800);
	mdc800=0;
}

module_init (usb_mdc800_init);
module_exit (usb_mdc800_cleanup);

MODULE_AUTHOR( DRIVER_AUTHOR );
MODULE_DESCRIPTION( DRIVER_DESC );
MODULE_LICENSE("GPL");

