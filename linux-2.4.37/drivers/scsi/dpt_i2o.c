/***************************************************************************
                          dpti.c  -  description
                             -------------------
    begin                : Thu Sep 7 2000
    copyright            : (C) 2000 by Adaptec
    email                : aacraid@adaptec.com

    			   July 30, 2001 First version being submitted
			   for inclusion in the kernel.  V2.4

    See README.dpti for history, notes, license info, and credits
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

//#define DEBUG 1
//#define UARTDELAY 1

// On the real kernel ADDR32 should always be zero for 2.4. GFP_HIGH allocates
// high pages. Keep the macro around because of the broken unmerged ia64 tree

#define ADDR32 (0)

#include <linux/version.h>
#include <linux/module.h>

MODULE_AUTHOR("Deanna Bonds, with _lots_ of help from Mark Salyzyn");
MODULE_DESCRIPTION("Adaptec I2O RAID Driver");

////////////////////////////////////////////////////////////////

#include <linux/ioctl.h>	/* For SCSI-Passthrough */
#include <asm/uaccess.h>

#include <linux/stat.h>
#include <linux/slab.h>		/* for kmalloc() */
#include <linux/config.h>	/* for CONFIG_PCI */
#include <linux/pci.h>		/* for PCI support */
#include <linux/proc_fs.h>
#include <linux/blk.h>
#include <linux/delay.h>	/* for udelay */
#include <linux/tqueue.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>	/* for printk */
#include <linux/sched.h>
#include <linux/reboot.h>
#include <linux/smp_lock.h>

#include <linux/timer.h>
#include <linux/string.h>
#include <linux/ioport.h>
#include <linux/stat.h>

#include <asm/processor.h>	/* for boot_cpu_data */
#include <asm/pgtable.h>
#include <asm/io.h>		/* for virt_to_bus, etc. */

#include "scsi.h"
#include "hosts.h"
#include "sd.h"

#include "dpt/dptsig.h"
#include "dpti.h"

/*============================================================================
 * Create a binary signature - this is read by dptsig
 * Needed for our management apps
 *============================================================================
 */
static dpt_sig_S DPTI_sig = {
	{'d', 'P', 't', 'S', 'i', 'G'}, SIG_VERSION,
#ifdef __i386__
	PROC_INTEL, PROC_386 | PROC_486 | PROC_PENTIUM | PROC_SEXIUM,
#elif defined(__ia64__)
	PROC_INTEL, PROC_IA64,
#elif defined(__sparc__)
	PROC_ULTRASPARC,
#elif defined(__alpha__)
	PROC_ALPHA ,
#else
	(-1),(-1)
#endif
	 FT_HBADRVR, 0, OEM_DPT, OS_LINUX, CAP_OVERLAP, DEV_ALL,
	ADF_ALL_SC5, 0, 0, DPT_VERSION, DPT_REVISION, DPT_SUBREVISION,
	DPT_MONTH, DPT_DAY, DPT_YEAR, "Adaptec Linux I2O RAID Driver"
};




/*============================================================================
 * Globals
 *============================================================================
 */

DECLARE_MUTEX(adpt_configuration_lock);

static struct i2o_sys_tbl *sys_tbl = NULL;
static int sys_tbl_ind = 0;
static int sys_tbl_len = 0;

static adpt_hba* hbas[DPTI_MAX_HBA];
static adpt_hba* hba_chain = NULL;
static int hba_count = 0;

static const struct file_operations adpt_fops = {
	ioctl: adpt_ioctl,
	open: adpt_open,
	release: adpt_close
};

#ifdef REBOOT_NOTIFIER
static struct notifier_block adpt_reboot_notifier =
{
	 adpt_reboot_event,
	 NULL,
	 0
};
#endif

/* Structures and definitions for synchronous message posting.
 * See adpt_i2o_post_wait() for description
 * */
struct adpt_i2o_post_wait_data
{
	int status;
	u32 id;
	adpt_wait_queue_head_t *wq;
	struct adpt_i2o_post_wait_data *next;
};

static struct adpt_i2o_post_wait_data *adpt_post_wait_queue = NULL;
static u32 adpt_post_wait_id = 0;
static spinlock_t adpt_post_wait_lock = SPIN_LOCK_UNLOCKED;


/*============================================================================
 * 				Functions
 *============================================================================
 */

static u8 adpt_read_blink_led(adpt_hba* host)
{
	if(host->FwDebugBLEDflag_P != 0) {
		if( readb(host->FwDebugBLEDflag_P) == 0xbc ){
			return readb(host->FwDebugBLEDvalue_P);
		}
	}
	return 0;
}

/*============================================================================
 * Scsi host template interface functions
 *============================================================================
 */

static struct pci_device_id dptids[] = {
	{ PCI_DPT_VENDOR_ID, PCI_DPT_DEVICE_ID, PCI_ANY_ID, PCI_ANY_ID,},
	{ PCI_DPT_VENDOR_ID, PCI_DPT_RAPTOR_DEVICE_ID, PCI_ANY_ID, PCI_ANY_ID,},
	{ 0, }
};
MODULE_DEVICE_TABLE(pci,dptids);

static int adpt_detect(Scsi_Host_Template* sht)
{
	struct pci_dev *pDev = NULL;
	adpt_hba* pHba;

	adpt_init();
	sht->use_new_eh_code = 1;

	PINFO("Detecting Adaptec I2O RAID controllers...\n");

        /* search for all Adatpec I2O RAID cards */
	while ((pDev = pci_find_device( PCI_DPT_VENDOR_ID, PCI_ANY_ID, pDev))) {
		if(pDev->device == PCI_DPT_DEVICE_ID ||
		   pDev->device == PCI_DPT_RAPTOR_DEVICE_ID){
			if(adpt_install_hba(sht, pDev) ){
				PERROR("Could not Init an I2O RAID device\n");
				PERROR("Will not try to detect others.\n");
				return hba_count-1;
			}
		}
	}

	/* In INIT state, Activate IOPs */
	for (pHba = hba_chain; pHba; pHba = pHba->next) {
		// Activate does get status , init outbound, and get hrt
		if (adpt_i2o_activate_hba(pHba) < 0) {
			adpt_i2o_delete_hba(pHba);
		}
	}


	/* Active IOPs in HOLD state */

rebuild_sys_tab:
	if (hba_chain == NULL) 
		return 0;

	/*
	 * If build_sys_table fails, we kill everything and bail
	 * as we can't init the IOPs w/o a system table
	 */	
	if (adpt_i2o_build_sys_table() < 0) {
		adpt_i2o_sys_shutdown();
		return 0;
	}

	PDEBUG("HBA's in HOLD state\n");

	/* If IOP don't get online, we need to rebuild the System table */
	for (pHba = hba_chain; pHba; pHba = pHba->next) {
		if (adpt_i2o_online_hba(pHba) < 0) {
			adpt_i2o_delete_hba(pHba);	
			goto rebuild_sys_tab;
		}
	}

	/* Active IOPs now in OPERATIONAL state */
	PDEBUG("HBA's in OPERATIONAL state\n");

	printk(KERN_INFO"dpti: If you have a lot of devices this could take a few minutes.\n");
	for (pHba = hba_chain; pHba; pHba = pHba->next) {
		printk(KERN_INFO"%s: Reading the hardware resource table.\n", pHba->name);
		if (adpt_i2o_lct_get(pHba) < 0){
			adpt_i2o_delete_hba(pHba);
			continue;
		}

		if (adpt_i2o_parse_lct(pHba) < 0){
			adpt_i2o_delete_hba(pHba);
			continue;
		}
		adpt_inquiry(pHba);
	}

	for (pHba = hba_chain; pHba; pHba = pHba->next) {
		if( adpt_scsi_register(pHba,sht) < 0){
			adpt_i2o_delete_hba(pHba);
			continue;
		}
		pHba->initialized = TRUE;
		pHba->state &= ~DPTI_STATE_RESET;
	}

	// Register our control device node
	// nodes will need to be created in /dev to access this
	// the nodes can not be created from within the driver
	if (hba_count && register_chrdev(DPTI_I2O_MAJOR, DPT_DRIVER, &adpt_fops)) {
		adpt_i2o_sys_shutdown();
		return 0;
	}
	return hba_count;
}


/*
 * scsi_unregister will be called AFTER we return. 
 */
static int adpt_release(struct Scsi_Host *host)
{
	adpt_hba* pHba = (adpt_hba*) host->hostdata[0];
//	adpt_i2o_quiesce_hba(pHba);
	adpt_i2o_delete_hba(pHba);
	return 0;
}


static void adpt_inquiry(adpt_hba* pHba)
{
	u32 msg[14]; 
	u32 *mptr;
	u32 *lenptr;
	int direction;
	int scsidir;
	u32 len;
	u32 reqlen;
	u8* buf;
	u8  scb[16];
	s32 rcode;

	memset(msg, 0, sizeof(msg));
	buf = (u8*)kmalloc(80,GFP_KERNEL|ADDR32);
	if(!buf){
		printk(KERN_ERR"%s: Could not allocate buffer\n",pHba->name);
		return;
	}
	memset((void*)buf, 0, 36);
	
	len = 36;
	direction = 0x00000000;	
	scsidir  =0x40000000;	// DATA IN  (iop<--dev)

	reqlen = 14;		// SINGLE SGE
	/* Stick the headers on */
	msg[0] = reqlen<<16 | SGL_OFFSET_12;
	msg[1] = (0xff<<24|HOST_TID<<12|ADAPTER_TID);
	msg[2] = 0;
	msg[3]  = 0;
	// Adaptec/DPT Private stuff 
	msg[4] = I2O_CMD_SCSI_EXEC|DPT_ORGANIZATION_ID<<16;
	msg[5] = ADAPTER_TID | 1<<16 /* Interpret*/;
	/* Direction, disconnect ok | sense data | simple queue , CDBLen */
	// I2O_SCB_FLAG_ENABLE_DISCONNECT | 
	// I2O_SCB_FLAG_SIMPLE_QUEUE_TAG | 
	// I2O_SCB_FLAG_SENSE_DATA_IN_MESSAGE;
	msg[6] = scsidir|0x20a00000| 6 /* cmd len*/;

	mptr=msg+7;

	memset(scb, 0, sizeof(scb));
	// Write SCSI command into the message - always 16 byte block 
	scb[0] = INQUIRY;
	scb[1] = 0;
	scb[2] = 0;
	scb[3] = 0;
	scb[4] = 36;
	scb[5] = 0;
	// Don't care about the rest of scb

	memcpy(mptr, scb, sizeof(scb));
	mptr+=4;
	lenptr=mptr++;		/* Remember me - fill in when we know */

	/* Now fill in the SGList and command */
	*lenptr = len;
	*mptr++ = 0xD0000000|direction|len;
	*mptr++ = virt_to_bus(buf);

	// Send it on it's way
	rcode = adpt_i2o_post_wait(pHba, msg, reqlen<<2, 120);
	if (rcode != 0) {
		sprintf(pHba->detail, "Adaptec I2O RAID");
		printk(KERN_INFO "%s: Inquiry Error (%d)\n",pHba->name,rcode);
	} else {
		memset(pHba->detail, 0, sizeof(pHba->detail));
		memcpy(&(pHba->detail), "Vendor: Adaptec ", 16);
		memcpy(&(pHba->detail[16]), " Model: ", 8);
		memcpy(&(pHba->detail[24]), (u8*) &buf[16], 16);
		memcpy(&(pHba->detail[40]), " FW: ", 4);
		memcpy(&(pHba->detail[44]), (u8*) &buf[32], 4);
		pHba->detail[48] = '\0';	/* precautionary */
	}
	kfree(buf);
	adpt_i2o_status_get(pHba);
	return ;
}


static void adpt_select_queue_depths(struct Scsi_Host *host, Scsi_Device * devicelist)
{
	Scsi_Device *device;	/* scsi layer per device information */
	adpt_hba* pHba;

	pHba = (adpt_hba *) host->hostdata[0];

	for (device = devicelist; device != NULL; device = device->next) {
		if (device->host != host) {
			continue;
		}
		if (host->can_queue) {
			device->queue_depth =  host->can_queue - 1;
		} else {
			device->queue_depth = 1;
		}
	}
}

static int adpt_queue(Scsi_Cmnd * cmd, void (*done) (Scsi_Cmnd *))
{
	adpt_hba* pHba = NULL;
	struct adpt_device* pDev = NULL;	/* dpt per device information */
	ulong timeout = jiffies + (TMOUT_SCSI*HZ);

	cmd->scsi_done = done;
	/*
	 * SCSI REQUEST_SENSE commands will be executed automatically by the 
	 * Host Adapter for any errors, so they should not be executed 
	 * explicitly unless the Sense Data is zero indicating that no error 
	 * occurred.
	 */

	if ((cmd->cmnd[0] == REQUEST_SENSE) && (cmd->sense_buffer[0] != 0)) {
		cmd->result = (DID_OK << 16);
		cmd->scsi_done(cmd);
		return 0;
	}

	pHba = (adpt_hba*)cmd->host->hostdata[0];
	if (!pHba) {
		return FAILED;
	}

	rmb();
	/*
	 * TODO: I need to block here if I am processing ioctl cmds
	 * but if the outstanding cmds all finish before the ioctl,
	 * the scsi-core will not know to start sending cmds to me again.
	 * I need to a way to restart the scsi-cores queues or should I block
	 * calling scsi_done on the outstanding cmds instead
	 * for now we don't set the IOCTL state
	 */
	if(((pHba->state) & DPTI_STATE_IOCTL) || ((pHba->state) & DPTI_STATE_RESET)) {
		pHba->host->last_reset = jiffies;
		pHba->host->resetting = 1;
		return 1;
	}

	if(cmd->eh_state != SCSI_STATE_QUEUED){
		// If we are not doing error recovery
		mod_timer(&cmd->eh_timeout, timeout);
	}

	// TODO if the cmd->device if offline then I may need to issue a bus rescan
	// followed by a get_lct to see if the device is there anymore
	if((pDev = (struct adpt_device*) (cmd->device->hostdata)) == NULL) {
		/*
		 * First command request for this device.  Set up a pointer
		 * to the device structure.  This should be a TEST_UNIT_READY
		 * command from scan_scsis_single.
		 */
		if ((pDev = adpt_find_device(pHba, (u32)cmd->channel, (u32)cmd->target, (u32)cmd-> lun)) == NULL) {
			// TODO: if any luns are at this bus, scsi id then fake a TEST_UNIT_READY and INQUIRY response 
			// with type 7F (for all luns less than the max for this bus,id) so the lun scan will continue.
			cmd->result = (DID_NO_CONNECT << 16);
			cmd->scsi_done(cmd);
			return 0;
		}
		cmd->device->hostdata = pDev;
	}
	pDev->pScsi_dev = cmd->device;

	/*
	 * If we are being called from when the device is being reset, 
	 * delay processing of the command until later.
	 */
	if (pDev->state & DPTI_DEV_RESET ) {
		return FAILED;
	}
	return adpt_scsi_to_i2o(pHba, cmd, pDev);
}

static int adpt_bios_param(Disk* disk, kdev_t dev, int geom[])
{
	int heads=-1;
	int sectors=-1;
	int cylinders=-1;

	// *** First lets set the default geometry ****
	
	// If the capacity is less than ox2000
	if (disk->capacity < 0x2000 ) {	// floppy
		heads = 18;
		sectors = 2;
	} 
	// else if between 0x2000 and 0x20000
	else if (disk->capacity < 0x20000) {
		heads = 64;
		sectors = 32;
	}
	// else if between 0x20000 and 0x40000
	else if (disk->capacity < 0x40000) {
		heads = 65;
		sectors = 63;
	}
	// else if between 0x4000 and 0x80000
	else if (disk->capacity < 0x80000) {
		heads = 128;
		sectors = 63;
	}
	// else if greater than 0x80000
	else {
		heads = 255;
		sectors = 63;
	}
	cylinders = disk->capacity / (heads * sectors);

	// Special case if CDROM
	if(disk->device->type == 5) {  // CDROM
		heads = 252;
		sectors = 63;
		cylinders = 1111;
	}

	geom[0] = heads;
	geom[1] = sectors;
	geom[2] = cylinders;
	
	PDEBUG("adpt_bios_param: exit\n");
	return 0;
}


static const char *adpt_info(struct Scsi_Host *host)
{
	adpt_hba* pHba;

	pHba = (adpt_hba *) host->hostdata[0];
	return (char *) (pHba->detail);
}

static int adpt_proc_info(char *buffer, char **start, off_t offset,
		  int length, int hostno, int inout)
{
	struct adpt_device* d;
	int id;
	int chan;
	int len = 0;
	int begin = 0;
	int pos = 0;
	adpt_hba* pHba;
	struct Scsi_Host *host;
	int unit;

	*start = buffer;
	if (inout == TRUE) {
		/*
		 * The user has done a write and wants us to take the
		 * data in the buffer and do something with it.
		 * proc_scsiwrite calls us with inout = 1
		 *
		 * Read data from buffer (writing to us) - NOT SUPPORTED
		 */
		return -EINVAL;
	}

	/*
	 * inout = 0 means the user has done a read and wants information
	 * returned, so we write information about the cards into the buffer
	 * proc_scsiread() calls us with inout = 0
	 */

	// Find HBA (host bus adapter) we are looking for
	down(&adpt_configuration_lock);
	for (pHba = hba_chain; pHba; pHba = pHba->next) {
		if (pHba->host->host_no == hostno) {
			break;	/* found adapter */
		}
	}
	up(&adpt_configuration_lock);
	if (pHba == NULL) {
		return 0;
	}
	host = pHba->host;

	len  = sprintf(buffer    , "Adaptec I2O RAID Driver Version: %s\n\n", DPT_I2O_VERSION);
	len += sprintf(buffer+len, "%s\n", pHba->detail);
	len += sprintf(buffer+len, "SCSI Host=scsi%d  Control Node=/dev/%s  irq=%d\n", 
			pHba->host->host_no, pHba->name, host->irq);
	len += sprintf(buffer+len, "\tpost fifo size  = %d\n\treply fifo size = %d\n\tsg table size   = %d\n\n",
			host->can_queue, (int) pHba->reply_fifo_size , host->sg_tablesize);

	pos = begin + len;

	/* CHECKPOINT */
	if(pos > offset + length) {
		goto stop_output;
	}
	if(pos <= offset) {
		/*
		 * If we haven't even written to where we last left
		 * off (the last time we were called), reset the 
		 * beginning pointer.
		 */
		len = 0;
		begin = pos;
	}
	len +=  sprintf(buffer+len, "Devices:\n");
	for(chan = 0; chan < MAX_CHANNEL; chan++) {
		for(id = 0; id < MAX_ID; id++) {
			d = pHba->channel[chan].device[id];
			while(d){
				len += sprintf(buffer+len,"\t%-24.24s", d->pScsi_dev->vendor);
				len += sprintf(buffer+len," Rev: %-8.8s\n", d->pScsi_dev->rev);
				pos = begin + len;


				/* CHECKPOINT */
				if(pos > offset + length) {
					goto stop_output;
				}
				if(pos <= offset) {
					len = 0;
					begin = pos;
				}

				unit = d->pI2o_dev->lct_data.tid;
				len += sprintf(buffer+len, "\tTID=%d, (Channel=%d, Target=%d, Lun=%d)  (%s)\n\n",
					       unit, (int)d->scsi_channel, (int)d->scsi_id, (int)d->scsi_lun,
					       d->pScsi_dev->online? "online":"offline"); 
				pos = begin + len;

				/* CHECKPOINT */
				if(pos > offset + length) {
					goto stop_output;
				}
				if(pos <= offset) {
					len = 0;
					begin = pos;
				}

				d = d->next_lun;
			}
		}
	}

	/*
	 * begin is where we last checked our position with regards to offset
	 * begin is always less than offset.  len is relative to begin.  It
	 * is the number of bytes written past begin
	 *
	 */
stop_output:
	/* stop the output and calculate the correct length */
	*(buffer + len) = '\0';

	*start = buffer + (offset - begin);	/* Start of wanted data */
	len -= (offset - begin);
	if(len > length) {
		len = length;
	} else if(len < 0){
		len = 0;
		**start = '\0';
	}
	return len;
}


/*===========================================================================
 * Error Handling routines
 *===========================================================================
 */

static int adpt_abort(Scsi_Cmnd * cmd)
{
	adpt_hba* pHba = NULL;	/* host bus adapter structure */
	struct adpt_device* dptdevice;	/* dpt per device information */
	u32 msg[5];
	int rcode;

	if(cmd->serial_number == 0){
		return FAILED;
	}
	pHba = (adpt_hba*) cmd->host->hostdata[0];
	printk(KERN_INFO"%s: Trying to Abort cmd=%ld\n",pHba->name, cmd->serial_number);
	if ((dptdevice = (void*) (cmd->device->hostdata)) == NULL) {
		printk(KERN_ERR "%s: Unable to abort: No device in cmnd\n",pHba->name);
		return FAILED;
	}

	memset(msg, 0, sizeof(msg));
	msg[0] = FIVE_WORD_MSG_SIZE|SGL_OFFSET_0;
	msg[1] = I2O_CMD_SCSI_ABORT<<24|HOST_TID<<12|dptdevice->tid;
	msg[2] = 0;
	msg[3]= 0; 
	msg[4] = (u32)cmd;
	if( (rcode = adpt_i2o_post_wait(pHba, msg, sizeof(msg), FOREVER)) != 0){
		if(rcode == -EOPNOTSUPP ){
			printk(KERN_INFO"%s: Abort cmd not supported\n",pHba->name);
			return FAILED;
		}
		printk(KERN_INFO"%s: Abort cmd=%ld failed.\n",pHba->name, cmd->serial_number);
		return FAILED;
	} 
	printk(KERN_INFO"%s: Abort cmd=%ld complete.\n",pHba->name, cmd->serial_number);
	return SUCCESS;
}


#define I2O_DEVICE_RESET 0x27
// This is the same for BLK and SCSI devices
// NOTE this is wrong in the i2o.h definitions
// This is not currently supported by our adapter but we issue it anyway
static int adpt_device_reset(Scsi_Cmnd* cmd)
{
	adpt_hba* pHba;
	u32 msg[4];
	u32 rcode;
	int old_state;
	struct adpt_device* d = (void*) cmd->device->hostdata;

	pHba = (void*) cmd->host->hostdata[0];
	printk(KERN_INFO"%s: Trying to reset device\n",pHba->name);
	if (!d) {
		printk(KERN_INFO"%s: Reset Device: Device Not found\n",pHba->name);
		return FAILED;
	}
	memset(msg, 0, sizeof(msg));
	msg[0] = FOUR_WORD_MSG_SIZE|SGL_OFFSET_0;
	msg[1] = (I2O_DEVICE_RESET<<24|HOST_TID<<12|d->tid);
	msg[2] = 0;
	msg[3] = 0;

	old_state = d->state;
	d->state |= DPTI_DEV_RESET;
	if( (rcode = adpt_i2o_post_wait(pHba, (void*)msg,sizeof(msg), FOREVER)) ){
		d->state = old_state;
		if(rcode == -EOPNOTSUPP ){
			printk(KERN_INFO"%s: Device reset not supported\n",pHba->name);
			return FAILED;
		}
		printk(KERN_INFO"%s: Device reset failed\n",pHba->name);
		return FAILED;
	} else {
		d->state = old_state;
		printk(KERN_INFO"%s: Device reset successful\n",pHba->name);
		return SUCCESS;
	}
}


#define I2O_HBA_BUS_RESET 0x87
// This version of bus reset is called by the eh_error handler
static int adpt_bus_reset(Scsi_Cmnd* cmd)
{
	adpt_hba* pHba;
	u32 msg[4];

	pHba = (adpt_hba*)cmd->host->hostdata[0];
	memset(msg, 0, sizeof(msg));
	printk(KERN_WARNING"%s: Bus reset: SCSI Bus %d: tid: %d\n",pHba->name, cmd->channel,pHba->channel[cmd->channel].tid );
	msg[0] = FOUR_WORD_MSG_SIZE|SGL_OFFSET_0;
	msg[1] = (I2O_HBA_BUS_RESET<<24|HOST_TID<<12|pHba->channel[cmd->channel].tid);
	msg[2] = 0;
	msg[3] = 0;
	if(adpt_i2o_post_wait(pHba, (void*)msg,sizeof(msg), FOREVER) ){
		printk(KERN_WARNING"%s: Bus reset failed.\n",pHba->name);
		return FAILED;
	} else {
		printk(KERN_WARNING"%s: Bus reset success.\n",pHba->name);
		return SUCCESS;
	}
}

// This version of reset is called by the eh_error_handler
static int adpt_reset(Scsi_Cmnd* cmd)
{
	adpt_hba* pHba;
	int rcode;
	pHba = (adpt_hba*)cmd->host->hostdata[0];
	printk(KERN_WARNING"%s: Hba Reset: scsi id %d: tid: %d\n",pHba->name,cmd->channel,pHba->channel[cmd->channel].tid );
	rcode =  adpt_hba_reset(pHba);
	if(rcode == 0){
		printk(KERN_WARNING"%s: HBA reset complete\n",pHba->name);
		return SUCCESS;
	} else {
		printk(KERN_WARNING"%s: HBA reset failed (%x)\n",pHba->name, rcode);
		return FAILED;
	}
}

// This version of reset is called by the ioctls and indirectly from eh_error_handler via adpt_reset
static int adpt_hba_reset(adpt_hba* pHba)
{
	int rcode;

	pHba->state |= DPTI_STATE_RESET;

	// Activate does get status , init outbound, and get hrt
	if ((rcode=adpt_i2o_activate_hba(pHba)) < 0) {
		printk(KERN_ERR "%s: Could not activate\n", pHba->name);
		adpt_i2o_delete_hba(pHba);
		return rcode;
	}

	if ((rcode=adpt_i2o_build_sys_table()) < 0) {
		adpt_i2o_delete_hba(pHba);
		return rcode;
	}
	PDEBUG("%s: in HOLD state\n",pHba->name);

	if ((rcode=adpt_i2o_online_hba(pHba)) < 0) {
		adpt_i2o_delete_hba(pHba);	
		return rcode;
	}
	PDEBUG("%s: in OPERATIONAL state\n",pHba->name);

	if ((rcode=adpt_i2o_lct_get(pHba)) < 0){
		adpt_i2o_delete_hba(pHba);
		return rcode;
	}

	if ((rcode=adpt_i2o_reparse_lct(pHba)) < 0){
		adpt_i2o_delete_hba(pHba);
		return rcode;
	}
	pHba->state &= ~DPTI_STATE_RESET;

	adpt_fail_posted_scbs(pHba);
	return 0;	/* return success */
}

/*===========================================================================
 * 
 *===========================================================================
 */


static void adpt_i2o_sys_shutdown(void)
{
	adpt_hba *pHba, *pNext;
	struct adpt_i2o_post_wait_data *p1, *old;

	 printk(KERN_INFO"Shutting down Adaptec I2O controllers.\n");
	 printk(KERN_INFO"   This could take a few minutes if there are many devices attached\n");
	/* Delete all IOPs from the controller chain */
	/* They should have already been released by the
	 * scsi-core
	 */
	for (pHba = hba_chain; pHba; pHba = pNext) {
		pNext = pHba->next;
		adpt_i2o_delete_hba(pHba);
	}

	/* Remove any timedout entries from the wait queue.  */
//	spin_lock_irqsave(&adpt_post_wait_lock, flags);
	/* Nothing should be outstanding at this point so just
	 * free them 
	 */
	for(p1 = adpt_post_wait_queue; p1;) {
		old = p1;
		p1 = p1->next;
		kfree(old);
	}
//	spin_unlock_irqrestore(&adpt_post_wait_lock, flags);
	adpt_post_wait_queue = 0;

	 printk(KERN_INFO "Adaptec I2O controllers down.\n");
}

/*
 * reboot/shutdown notification.
 *
 * - Quiesce each IOP in the system
 *
 */

#ifdef REBOOT_NOTIFIER
static int adpt_reboot_event(struct notifier_block *n, ulong code, void *p)
{

	 if(code != SYS_RESTART && code != SYS_HALT && code != SYS_POWER_OFF)
		  return NOTIFY_DONE;

	 adpt_i2o_sys_shutdown();

	 return NOTIFY_DONE;
}
#endif


static int adpt_install_hba(Scsi_Host_Template* sht, struct pci_dev* pDev) 
{

	adpt_hba* pHba = NULL;
	adpt_hba* p = NULL;
	ulong base_addr0_phys = 0;
	ulong base_addr1_phys = 0;
	u32 hba_map0_area_size = 0;
	u32 hba_map1_area_size = 0;
	ulong base_addr_virt = 0;
	ulong msg_addr_virt = 0;

	int raptorFlag = FALSE;
	int i;

	if(pci_enable_device(pDev)) {
		return -EINVAL;
	}
	pci_set_master(pDev);

	base_addr0_phys = pci_resource_start(pDev,0);
	hba_map0_area_size = pci_resource_len(pDev,0);

	// Check if standard PCI card or single BAR Raptor
	if(pDev->device == PCI_DPT_DEVICE_ID){
		if(pDev->subsystem_device >=0xc032 && pDev->subsystem_device <= 0xc03b){
			// Raptor card with this device id needs 4M
			hba_map0_area_size = 0x400000;
		} else { // Not Raptor - it is a PCI card
			if(hba_map0_area_size > 0x100000 ){ 
				hba_map0_area_size = 0x100000;
			}
		}
	} else {// Raptor split BAR config
		// Use BAR1 in this configuration
		base_addr1_phys = pci_resource_start(pDev,1);
		hba_map1_area_size = pci_resource_len(pDev,1);
		raptorFlag = TRUE;
	}


	base_addr_virt = (ulong)ioremap(base_addr0_phys,hba_map0_area_size);
	if(base_addr_virt == 0) {
		PERROR("dpti: adpt_config_hba: io remap failed\n");
		return -EINVAL;
	}

        if(raptorFlag == TRUE) {
		msg_addr_virt = (ulong)ioremap(base_addr1_phys, hba_map1_area_size );
		if(msg_addr_virt == 0) {
			PERROR("dpti: adpt_config_hba: io remap failed on BAR1\n");
			iounmap((void*)base_addr_virt);
			return -EINVAL;
		}
	} else {
		msg_addr_virt = base_addr_virt;
	}
	
	// Allocate and zero the data structure
	pHba = kmalloc(sizeof(adpt_hba), GFP_KERNEL);
	if( pHba == NULL) {
		if(msg_addr_virt != base_addr_virt){
			iounmap((void*)msg_addr_virt);
		}
		iounmap((void*)base_addr_virt);
		return -ENOMEM;
	}
	memset(pHba, 0, sizeof(adpt_hba));

	down(&adpt_configuration_lock);
	for(i=0;i<DPTI_MAX_HBA;i++) {
		if(hbas[i]==NULL) {
			hbas[i]=pHba;
			break;
		}
	}

	if(hba_chain != NULL){
		for(p = hba_chain; p->next; p = p->next);
		p->next = pHba;
	} else {
		hba_chain = pHba;
	}
	pHba->next = NULL;
	pHba->unit = hba_count;
	sprintf(pHba->name, "dpti%d", i);
	hba_count++;
	
	up(&adpt_configuration_lock);

	pHba->pDev = pDev;
	pHba->base_addr_phys = base_addr0_phys;

	// Set up the Virtual Base Address of the I2O Device
	pHba->base_addr_virt = base_addr_virt;
	pHba->msg_addr_virt = msg_addr_virt;  
	pHba->irq_mask = (ulong)(base_addr_virt+0x30);
	pHba->post_port = (ulong)(base_addr_virt+0x40);
	pHba->reply_port = (ulong)(base_addr_virt+0x44);

	pHba->hrt = NULL;
	pHba->lct = NULL;
	pHba->lct_size = 0;
	pHba->status_block = NULL;
	pHba->post_count = 0;
	pHba->state = DPTI_STATE_RESET;
	pHba->pDev = pDev;
	pHba->devices = NULL;

	// Initializing the spinlocks
	spin_lock_init(&pHba->state_lock);

	if(raptorFlag == 0){
		printk(KERN_INFO"Adaptec I2O RAID controller %d at %lx size=%x irq=%d\n", 
			hba_count-1, base_addr_virt, hba_map0_area_size, pDev->irq);
	} else {
		printk(KERN_INFO"Adaptec I2O RAID controller %d irq=%d\n",hba_count-1, pDev->irq);
		printk(KERN_INFO"     BAR0 %lx - size= %x\n",base_addr_virt,hba_map0_area_size);
		printk(KERN_INFO"     BAR1 %lx - size= %x\n",msg_addr_virt,hba_map1_area_size);
	}

	if (request_irq (pDev->irq, adpt_isr, SA_SHIRQ, pHba->name, pHba)) {
		printk(KERN_ERR"%s: Couldn't register IRQ %d\n", pHba->name, pDev->irq);
		adpt_i2o_delete_hba(pHba);
		return -EINVAL;
	}

	return 0;
}


static void adpt_i2o_delete_hba(adpt_hba* pHba)
{
	adpt_hba* p1;
	adpt_hba* p2;
	struct i2o_device* d;
	struct i2o_device* next;
	int i;
	int j;
	struct adpt_device* pDev;
	struct adpt_device* pNext;


	down(&adpt_configuration_lock);
	// scsi_unregister calls our adpt_release which
	// does a quiese
	if(pHba->host){
		free_irq(pHba->host->irq, pHba);
	}
	for(i=0;i<DPTI_MAX_HBA;i++) {
		if(hbas[i]==pHba) {
			hbas[i] = NULL;
		}
	}
	p2 = NULL;
	for( p1 = hba_chain; p1; p2 = p1,p1=p1->next){
		if(p1 == pHba) {
			if(p2) {
				p2->next = p1->next;
			} else {
				hba_chain = p1->next;
			}
			break;
		}
	}

	hba_count--;
	up(&adpt_configuration_lock);

	iounmap((void*)pHba->base_addr_virt);
	if(pHba->msg_addr_virt != pHba->base_addr_virt){
		iounmap((void*)pHba->msg_addr_virt);
	}
	if(pHba->hrt) {
		kfree(pHba->hrt);
	}
	if(pHba->lct){
		kfree(pHba->lct);
	}
	if(pHba->status_block) {
		kfree(pHba->status_block);
	}
	if(pHba->reply_pool){
		kfree(pHba->reply_pool);
	}

	for(d = pHba->devices; d ; d = next){
		next = d->next;
		kfree(d);
	}
	for(i = 0 ; i < pHba->top_scsi_channel ; i++){
		for(j = 0; j < MAX_ID; j++){
			if(pHba->channel[i].device[j] != NULL){
				for(pDev = pHba->channel[i].device[j]; pDev; pDev = pNext){
					pNext = pDev->next_lun;
					kfree(pDev);
				}
			}
		}
	}
	kfree(pHba);

	if(hba_count <= 0){
		unregister_chrdev(DPTI_I2O_MAJOR, DPT_DRIVER);   
	}
}


static int adpt_init(void)
{
	int i;

	printk(KERN_INFO"Loading Adaptec I2O RAID: Version " DPT_I2O_VERSION "\n");
	for (i = 0; i < DPTI_MAX_HBA; i++) {
		hbas[i] = NULL;
	}
#ifdef REBOOT_NOTIFIER
	register_reboot_notifier(&adpt_reboot_notifier);
#endif

	return 0;
}


static struct adpt_device* adpt_find_device(adpt_hba* pHba, u32 chan, u32 id, u32 lun)
{
	struct adpt_device* d;

	if(chan < 0 || chan >= MAX_CHANNEL)
		return NULL;
	
	if( pHba->channel[chan].device == NULL){
		printk(KERN_DEBUG"Adaptec I2O RAID: Trying to find device before they are allocated\n");
		return NULL;
	}

	d = pHba->channel[chan].device[id];
	if(!d || d->tid == 0) {
		return NULL;
	}

	/* If it is the only lun at that address then this should match*/
	if(d->scsi_lun == lun){
		return d;
	}

	/* else we need to look through all the luns */
	for(d=d->next_lun ; d ; d = d->next_lun){
		if(d->scsi_lun == lun){
			return d;
		}
	}
	return NULL;
}


static int adpt_i2o_post_wait(adpt_hba* pHba, u32* msg, int len, int timeout)
{
	// I used my own version of the WAIT_QUEUE_HEAD
	// to handle some version differences
	// When embedded in the kernel this could go back to the vanilla one
	ADPT_DECLARE_WAIT_QUEUE_HEAD(adpt_wq_i2o_post);
	int status = 0;
	ulong flags = 0;
	struct adpt_i2o_post_wait_data *p1, *p2;
	struct adpt_i2o_post_wait_data *wait_data =
		kmalloc(sizeof(struct adpt_i2o_post_wait_data),GFP_KERNEL);
	adpt_wait_queue_t wait;

	if(!wait_data){
		return -ENOMEM;
	}
	/*
	 * The spin locking is needed to keep anyone from playing
	 * with the queue pointers and id while we do the same
	 */
	spin_lock_irqsave(&adpt_post_wait_lock, flags);
       // TODO we need a MORE unique way of getting ids
       // to support async LCT get
	wait_data->next = adpt_post_wait_queue;
	adpt_post_wait_queue = wait_data;
	adpt_post_wait_id++;
	adpt_post_wait_id = (adpt_post_wait_id & 0x7fff);
	wait_data->id =  adpt_post_wait_id;
	spin_unlock_irqrestore(&adpt_post_wait_lock, flags);

	wait_data->wq = &adpt_wq_i2o_post;
	wait_data->status = -ETIMEDOUT;

	// this code is taken from kernel/sched.c:interruptible_sleep_on_timeout
	wait.task = current;
	init_waitqueue_entry(&wait, current);
	wq_write_lock_irqsave(&adpt_wq_i2o_post.lock,flags);
	__add_wait_queue(&adpt_wq_i2o_post, &wait);
	wq_write_unlock(&adpt_wq_i2o_post.lock);

	msg[2] |= 0x80000000 | ((u32)wait_data->id);
	timeout *= HZ;
	if((status = adpt_i2o_post_this(pHba, msg, len)) == 0){
		if(!timeout){
			set_current_state(TASK_INTERRUPTIBLE);
			spin_unlock_irq(&io_request_lock);
			schedule();
			spin_lock_irq(&io_request_lock);
		} else {
			set_current_state(TASK_INTERRUPTIBLE);
			spin_unlock_irq(&io_request_lock);
			schedule_timeout(timeout*HZ);
			spin_lock_irq(&io_request_lock);
		}
	}
	wq_write_lock_irq(&adpt_wq_i2o_post.lock);
	__remove_wait_queue(&adpt_wq_i2o_post, &wait);
	wq_write_unlock_irqrestore(&adpt_wq_i2o_post.lock,flags);

	if(status == -ETIMEDOUT){
		printk(KERN_INFO"dpti%d: POST WAIT TIMEOUT\n",pHba->unit);
		// We will have to free the wait_data memory during shutdown
		return status;
	}

	/* Remove the entry from the queue.  */
	p2 = NULL;
	spin_lock_irqsave(&adpt_post_wait_lock, flags);
	for(p1 = adpt_post_wait_queue; p1; p2 = p1, p1 = p1->next) {
		if(p1 == wait_data) {
			if(p1->status == I2O_DETAIL_STATUS_UNSUPPORTED_FUNCTION ) {
				status = -EOPNOTSUPP;
			}
			if(p2) {
				p2->next = p1->next;
			} else {
				adpt_post_wait_queue = p1->next;
			}
			break;
		}
	}
	spin_unlock_irqrestore(&adpt_post_wait_lock, flags);

	kfree(wait_data);

	return status;
}


static s32 adpt_i2o_post_this(adpt_hba* pHba, u32* data, int len)
{

	u32 m = EMPTY_QUEUE;
	u32 *msg;
	ulong timeout = jiffies + 30*HZ;
	do {
		rmb();
		m = readl(pHba->post_port);
		if (m != EMPTY_QUEUE) {
			break;
		}
		if(time_after(jiffies,timeout)){
			printk(KERN_WARNING"dpti%d: Timeout waiting for message frame!\n", pHba->unit);
			return -ETIMEDOUT;
		}
	} while(m == EMPTY_QUEUE);
		
	msg = (u32*) (pHba->msg_addr_virt + m);
	memcpy_toio(msg, data, len);
	wmb();

	//post message
	writel(m, pHba->post_port);
	wmb();

	return 0;
}


static void adpt_i2o_post_wait_complete(u32 context, int status)
{
	struct adpt_i2o_post_wait_data *p1 = NULL;
	/*
	 * We need to search through the adpt_post_wait
	 * queue to see if the given message is still
	 * outstanding.  If not, it means that the IOP
	 * took longer to respond to the message than we
	 * had allowed and timer has already expired.
	 * Not much we can do about that except log
	 * it for debug purposes, increase timeout, and recompile
	 *
	 * Lock needed to keep anyone from moving queue pointers
	 * around while we're looking through them.
	 */

	context &= 0x7fff;

	spin_lock(&adpt_post_wait_lock);
	for(p1 = adpt_post_wait_queue; p1; p1 = p1->next) {
		if(p1->id == context) {
			p1->status = status;
			spin_unlock(&adpt_post_wait_lock);
			wake_up_interruptible(p1->wq);
			return;
		}
	}
	spin_unlock(&adpt_post_wait_lock);
        // If this happens we loose commands that probably really completed
	printk(KERN_DEBUG"dpti: Could Not find task %d in wait queue\n",context);
	printk(KERN_DEBUG"      Tasks in wait queue:\n");
	for(p1 = adpt_post_wait_queue; p1; p1 = p1->next) {
		printk(KERN_DEBUG"           %d\n",p1->id);
	}
	return;
}

static s32 adpt_i2o_reset_hba(adpt_hba* pHba)			
{
	u32 msg[8];
	u8* status;
	u32 m = EMPTY_QUEUE ;
	ulong timeout = jiffies + (TMOUT_IOPRESET*HZ);

	if(pHba->initialized  == FALSE) {	// First time reset should be quick
		timeout = jiffies + (25*HZ);
	} else {
		adpt_i2o_quiesce_hba(pHba);
	}

	do {
		rmb();
		m = readl(pHba->post_port);
		if (m != EMPTY_QUEUE) {
			break;
		}
		if(time_after(jiffies,timeout)){
			printk(KERN_WARNING"Timeout waiting for message!\n");
			return -ETIMEDOUT;
		}
	} while (m == EMPTY_QUEUE);

	status = (u8*)kmalloc(4, GFP_KERNEL|ADDR32);
	if(status == NULL) {
		adpt_send_nop(pHba, m);
		printk(KERN_ERR"IOP reset failed - no free memory.\n");
		return -ENOMEM;
	}
	memset(status,0,4);

	msg[0]=EIGHT_WORD_MSG_SIZE|SGL_OFFSET_0;
	msg[1]=I2O_CMD_ADAPTER_RESET<<24|HOST_TID<<12|ADAPTER_TID;
	msg[2]=0;
	msg[3]=0;
	msg[4]=0;
	msg[5]=0;
	msg[6]=virt_to_bus(status);
	msg[7]=0;     

	memcpy_toio(pHba->msg_addr_virt+m, msg, sizeof(msg));
	wmb();
	writel(m, pHba->post_port);
	wmb();

	while(*status == 0){
		if(time_after(jiffies,timeout)){
			printk(KERN_WARNING"%s: IOP Reset Timeout\n",pHba->name);
			/* We loose 4 bytes of "status" here, but we cannot
			   free these because controller may awake and corrupt
			   those bytes at any time */
			return -ETIMEDOUT;
		}
		rmb();
	}

	if(*status == 0x01 /*I2O_EXEC_IOP_RESET_IN_PROGRESS*/) {
		PDEBUG("%s: Reset in progress...\n", pHba->name);
		// Here we wait for message frame to become available
		// indicated that reset has finished
		do {
			rmb();
			m = readl(pHba->post_port);
			if (m != EMPTY_QUEUE) {
				break;
			}
			if(time_after(jiffies,timeout)){
				printk(KERN_ERR "%s:Timeout waiting for IOP Reset.\n",pHba->name);
			/* We loose 4 bytes of "status" here, but we cannot
			   free these because controller may awake and corrupt
			   those bytes at any time */
				return -ETIMEDOUT;
			}
		} while (m == EMPTY_QUEUE);
		// Flush the offset
		adpt_send_nop(pHba, m);
	}
	adpt_i2o_status_get(pHba);
	if(*status == 0x02 ||
			pHba->status_block->iop_state != ADAPTER_STATE_RESET) {
		printk(KERN_WARNING"%s: Reset reject, trying to clear\n",
				pHba->name);
	} else {
		PDEBUG("%s: Reset completed.\n", pHba->name);
	}

	kfree(status);
#ifdef UARTDELAY
	// This delay is to allow someone attached to the card through the debug UART to 
	// set up the dump levels that they want before the rest of the initialization sequence
	adpt_delay(20000);
#endif
	return 0;
}


static int adpt_i2o_parse_lct(adpt_hba* pHba)
{
	int i;
	int max;
	int tid;
	struct i2o_device *d;
	i2o_lct *lct = pHba->lct;
	u8 bus_no = 0;
	s16 scsi_id;
	s16 scsi_lun;
	u32 buf[10]; // larger than 7, or 8 ...
	struct adpt_device* pDev; 
	
	if (lct == NULL) {
		printk(KERN_ERR "%s: LCT is empty???\n",pHba->name);
		return -1;
	}
	
	max = lct->table_size;	
	max -= 3;
	max /= 9;

	for(i=0;i<max;i++) {
		if( lct->lct_entry[i].user_tid != 0xfff){
			/*
			 * If we have hidden devices, we need to inform the upper layers about
			 * the possible maximum id reference to handle device access when
			 * an array is disassembled. This code has no other purpose but to
			 * allow us future access to devices that are currently hidden
			 * behind arrays, hotspares or have not been configured (JBOD mode).
			 */
			if( lct->lct_entry[i].class_id != I2O_CLASS_RANDOM_BLOCK_STORAGE &&
			    lct->lct_entry[i].class_id != I2O_CLASS_SCSI_PERIPHERAL &&
			    lct->lct_entry[i].class_id != I2O_CLASS_FIBRE_CHANNEL_PERIPHERAL ){
			    	continue;
			}
			tid = lct->lct_entry[i].tid;
			// I2O_DPT_DEVICE_INFO_GROUP_NO;
			if(adpt_i2o_query_scalar(pHba, tid, 0x8000, -1, buf, 32)<0) {
				continue;
			}
			bus_no = buf[0]>>16;
			scsi_id = buf[1];
			scsi_lun = (buf[2]>>8 )&0xff;
			if(bus_no >= MAX_CHANNEL) {	// Something wrong skip it
				printk(KERN_WARNING"%s: Channel number %d out of range \n", pHba->name, bus_no);
				continue;
			}
			if(scsi_id > MAX_ID){
				printk(KERN_WARNING"%s: SCSI ID %d out of range \n", pHba->name, bus_no);
				continue;
			}
			if(bus_no > pHba->top_scsi_channel){
				pHba->top_scsi_channel = bus_no;
			}
			if(scsi_id > pHba->top_scsi_id){
				pHba->top_scsi_id = scsi_id;
			}
			if(scsi_lun > pHba->top_scsi_lun){
				pHba->top_scsi_lun = scsi_lun;
			}
			continue;
		}
		d = (struct i2o_device *)kmalloc(sizeof(struct i2o_device), GFP_KERNEL);
		if(d==NULL)
		{
			printk(KERN_CRIT"%s: Out of memory for I2O device data.\n",pHba->name);
			return -ENOMEM;
		}
		
		d->controller = (void*)pHba;
		d->next = NULL;

		memcpy(&d->lct_data, &lct->lct_entry[i], sizeof(i2o_lct_entry));

		d->flags = 0;
		tid = d->lct_data.tid;
		adpt_i2o_report_hba_unit(pHba, d);
		adpt_i2o_install_device(pHba, d);
	}
	bus_no = 0;
	for(d = pHba->devices; d ; d = d->next) {
		if(d->lct_data.class_id  == I2O_CLASS_BUS_ADAPTER_PORT ||
		   d->lct_data.class_id  == I2O_CLASS_FIBRE_CHANNEL_PORT){
			tid = d->lct_data.tid;
			// TODO get the bus_no from hrt-but for now they are in order
			//bus_no = 
			if(bus_no > pHba->top_scsi_channel){
				pHba->top_scsi_channel = bus_no;
			}
			pHba->channel[bus_no].type = d->lct_data.class_id;
			pHba->channel[bus_no].tid = tid;
			if(adpt_i2o_query_scalar(pHba, tid, 0x0200, -1, buf, 28)>=0)
			{
				pHba->channel[bus_no].scsi_id = buf[1];
				PDEBUG("Bus %d - SCSI ID %d.\n", bus_no, buf[1]);
			}
			// TODO remove - this is just until we get from hrt
			bus_no++;
			if(bus_no >= MAX_CHANNEL) {	// Something wrong skip it
				printk(KERN_WARNING"%s: Channel number %d out of range - LCT\n", pHba->name, bus_no);
				break;
			}
		}
	}

	// Setup adpt_device table
	for(d = pHba->devices; d ; d = d->next) {
		if(d->lct_data.class_id  == I2O_CLASS_RANDOM_BLOCK_STORAGE ||
		   d->lct_data.class_id  == I2O_CLASS_SCSI_PERIPHERAL ||
		   d->lct_data.class_id  == I2O_CLASS_FIBRE_CHANNEL_PERIPHERAL ){

			tid = d->lct_data.tid;
			scsi_id = -1;
			// I2O_DPT_DEVICE_INFO_GROUP_NO;
			if(adpt_i2o_query_scalar(pHba, tid, 0x8000, -1, buf, 32)>=0) {
				bus_no = buf[0]>>16;
				scsi_id = buf[1];
				scsi_lun = (buf[2]>>8 )&0xff;
				if(bus_no >= MAX_CHANNEL) {	// Something wrong skip it
					continue;
				}
				if(scsi_id > MAX_ID){
					continue;
				}
				if( pHba->channel[bus_no].device[scsi_id] == NULL){
					pDev =  kmalloc(sizeof(struct adpt_device),GFP_KERNEL);
					if(pDev == NULL) {
						return -ENOMEM;
					}
					pHba->channel[bus_no].device[scsi_id] = pDev;
					memset(pDev,0,sizeof(struct adpt_device));
				} else {
					for( pDev = pHba->channel[bus_no].device[scsi_id];	
							pDev->next_lun; pDev = pDev->next_lun){
					}
					pDev->next_lun = kmalloc(sizeof(struct adpt_device),GFP_KERNEL);
					if(pDev->next_lun == NULL) {
						return -ENOMEM;
					}
					memset(pDev->next_lun,0,sizeof(struct adpt_device));
					pDev = pDev->next_lun;
				}
				pDev->tid = tid;
				pDev->scsi_channel = bus_no;
				pDev->scsi_id = scsi_id;
				pDev->scsi_lun = scsi_lun;
				pDev->pI2o_dev = d;
				d->owner = pDev;
				pDev->type = (buf[0])&0xff;
				pDev->flags = (buf[0]>>8)&0xff;
				if(scsi_id > pHba->top_scsi_id){
					pHba->top_scsi_id = scsi_id;
				}
				if(scsi_lun > pHba->top_scsi_lun){
					pHba->top_scsi_lun = scsi_lun;
				}
			}
			if(scsi_id == -1){
				printk(KERN_WARNING"Could not find SCSI ID for %s\n",
						d->lct_data.identity_tag);
			}
		}
	}
	return 0;
}


/*
 *	Each I2O controller has a chain of devices on it - these match
 *	the useful parts of the LCT of the board.
 */
 
static int adpt_i2o_install_device(adpt_hba* pHba, struct i2o_device *d)
{
	down(&adpt_configuration_lock);
	d->controller=pHba;
	d->owner=NULL;
	d->next=pHba->devices;
	d->prev=NULL;
	if (pHba->devices != NULL){
		pHba->devices->prev=d;
	}
	pHba->devices=d;
	*d->dev_name = 0;

	up(&adpt_configuration_lock);
	return 0;
}

static int adpt_open(struct inode *inode, struct file *file)
{
	int minor;
	adpt_hba* pHba;

	//TODO check for root access
	//
	minor = MINOR(inode->i_rdev);
	if (minor >= hba_count) {
		return -ENXIO;
	}
	down(&adpt_configuration_lock);
	for (pHba = hba_chain; pHba; pHba = pHba->next) {
		if (pHba->unit == minor) {
			break;	/* found adapter */
		}
	}
	if (pHba == NULL) {
		up(&adpt_configuration_lock);
		return -ENXIO;
	}

//	if(pHba->in_use){
	//	up(&adpt_configuration_lock);
//		return -EBUSY;
//	}

	pHba->in_use = 1;
	up(&adpt_configuration_lock);

	return 0;
}

static int adpt_close(struct inode *inode, struct file *file)
{
	int minor;
	adpt_hba* pHba;

	minor = MINOR(inode->i_rdev);
	if (minor >= hba_count) {
		return -ENXIO;
	}
	down(&adpt_configuration_lock);
	for (pHba = hba_chain; pHba; pHba = pHba->next) {
		if (pHba->unit == minor) {
			break;	/* found adapter */
		}
	}
	up(&adpt_configuration_lock);
	if (pHba == NULL) {
		return -ENXIO;
	}

	pHba->in_use = 0;

	return 0;
}


static int adpt_i2o_passthru(adpt_hba* pHba, u32* arg)
{
	u32 msg[MAX_MESSAGE_SIZE];
	u32* reply = NULL;
	u32 size = 0;
	u32 reply_size = 0;
	u32* user_msg = (u32*)arg;
	u32* user_reply = NULL;
	ulong sg_list[pHba->sg_tablesize];
	u32 sg_offset = 0;
	u32 sg_count = 0;
	int sg_index = 0;
	u32 i = 0;
	u32 rcode = 0;
	ulong p = 0;
	ulong flags = 0;

	memset(&msg, 0, MAX_MESSAGE_SIZE*4);
	// get user msg size in u32s 
	if(get_user(size, &user_msg[0])){
		return -EFAULT;
	}
	size = size>>16;

	user_reply = &user_msg[size];
	if(size > MAX_MESSAGE_SIZE){
		return -EFAULT;
	}
	size *= 4; // Convert to bytes

	/* Copy in the user's I2O command */
	if(copy_from_user((void*)msg, (void*)user_msg, size)) {
		return -EFAULT;
	}
	get_user(reply_size, &user_reply[0]);
	reply_size = reply_size>>16;
	if(reply_size > REPLY_FRAME_SIZE){
		reply_size = REPLY_FRAME_SIZE;
	}
	reply_size *= 4;
	reply = kmalloc(REPLY_FRAME_SIZE*4, GFP_KERNEL);
	if(reply == NULL) {
		printk(KERN_WARNING"%s: Could not allocate reply buffer\n",pHba->name);
		return -ENOMEM;
	}
	memset(reply,0,REPLY_FRAME_SIZE*4);
	sg_offset = (msg[0]>>4)&0xf;
	msg[2] = 0x40000000; // IOCTL context
	msg[3] = (u32)reply;
	memset(sg_list,0, sizeof(sg_list[0])*pHba->sg_tablesize);
	if(sg_offset) {
		// TODO 64bit fix
		struct sg_simple_element *sg =  (struct sg_simple_element*) (msg+sg_offset);
		sg_count = (size - sg_offset*4) / sizeof(struct sg_simple_element);
		if (sg_count > pHba->sg_tablesize){
			printk(KERN_DEBUG"%s:IOCTL SG List too large (%u)\n", pHba->name,sg_count);
			kfree (reply);
			return -EINVAL;
		}

		for(i = 0; i < sg_count; i++) {
			int sg_size;

			if (!(sg[i].flag_count & 0x10000000 /*I2O_SGL_FLAGS_SIMPLE_ADDRESS_ELEMENT*/)) {
				printk(KERN_DEBUG"%s:Bad SG element %d - not simple (%x)\n",pHba->name,i,  sg[i].flag_count);
				rcode = -EINVAL;
				goto cleanup;
			}
			sg_size = sg[i].flag_count & 0xffffff;      
			/* Allocate memory for the transfer */
			p = (ulong)kmalloc(sg_size, GFP_KERNEL|ADDR32);
			if(p == 0) {
				printk(KERN_DEBUG"%s: Could not allocate SG buffer - size = %d buffer number %d of %d\n",
						pHba->name,sg_size,i,sg_count);
				rcode = -ENOMEM;
				goto cleanup;
			}
			sg_list[sg_index++] = p; // sglist indexed with input frame, not our internal frame.
			/* Copy in the user's SG buffer if necessary */
			if(sg[i].flag_count & 0x04000000 /*I2O_SGL_FLAGS_DIR*/) {
				// TODO 64bit fix
				if (copy_from_user((void*)p,(void*)sg[i].addr_bus, sg_size)) {
					printk(KERN_DEBUG"%s: Could not copy SG buf %d FROM user\n",pHba->name,i);
					rcode = -EFAULT;
					goto cleanup;
				}
			}
			//TODO 64bit fix
			sg[i].addr_bus = (u32)virt_to_bus((void*)p);
		}
	}

	do {
		spin_lock_irqsave(&io_request_lock, flags);
		// This state stops any new commands from enterring the
		// controller while processing the ioctl
//		pHba->state |= DPTI_STATE_IOCTL;
//		We can't set this now - The scsi subsystem sets host_blocked and
//		the queue empties and stops.  We need a way to restart the queue
		rcode = adpt_i2o_post_wait(pHba, msg, size, FOREVER);
//		pHba->state &= ~DPTI_STATE_IOCTL;
		spin_unlock_irqrestore(&io_request_lock, flags);
	} while(rcode == -ETIMEDOUT);  

	if(rcode){
		goto cleanup;
	}

	if(sg_offset) {
	/* Copy back the Scatter Gather buffers back to user space */
		u32 j;
		// TODO 64bit fix
		struct sg_simple_element* sg;
		int sg_size;

		// re-acquire the original message to handle correctly the sg copy operation
		memset(&msg, 0, MAX_MESSAGE_SIZE*4); 
		// get user msg size in u32s 
		if(get_user(size, &user_msg[0])){
			rcode = -EFAULT; 
			goto cleanup; 
		}
		size = size>>16;
		size *= 4;
		/* Copy in the user's I2O command */
		if (copy_from_user ((void*)msg, (void*)user_msg, size)) {
			rcode = -EFAULT;
			goto cleanup;
		}
		sg_count = (size - sg_offset*4) / sizeof(struct sg_simple_element);

		// TODO 64bit fix
		sg 	 = (struct sg_simple_element*)(msg + sg_offset);
		for (j = 0; j < sg_count; j++) {
			/* Copy out the SG list to user's buffer if necessary */
			if(! (sg[j].flag_count & 0x4000000 /*I2O_SGL_FLAGS_DIR*/)) {
				sg_size = sg[j].flag_count & 0xffffff; 
				// TODO 64bit fix
				if (copy_to_user((void*)sg[j].addr_bus,(void*)sg_list[j], sg_size)) {
					printk(KERN_WARNING"%s: Could not copy %lx TO user %x\n",pHba->name, sg_list[j], sg[j].addr_bus);
					rcode = -EFAULT;
					goto cleanup;
				}
			}
		}
	} 

	/* Copy back the reply to user space */
	if (reply_size) {
		// we wrote our own values for context - now restore the user supplied ones
		if(copy_from_user(reply+2, user_msg+2, sizeof(u32)*2)) {
			printk(KERN_WARNING"%s: Could not copy message context FROM user\n",pHba->name);
			rcode = -EFAULT;
		}
		if(copy_to_user(user_reply, reply, reply_size)) {
			printk(KERN_WARNING"%s: Could not copy reply TO user\n",pHba->name);
			rcode = -EFAULT;
		}
	}


cleanup:
	kfree (reply);
	while(sg_index) {
		if(sg_list[--sg_index]) {
			kfree((void*)(sg_list[sg_index]));
		}
	}
	return rcode;
}


/*
 * This routine returns information about the system.  This does not effect
 * any logic and if the info is wrong - it doesn't matter.
 */

/* Get all the info we can not get from kernel services */
static int adpt_system_info(void *buffer)
{
	sysInfo_S si;

	memset(&si, 0, sizeof(si));

	si.osType = OS_LINUX;
	si.osMajorVersion = (u8) (LINUX_VERSION_CODE >> 16);
	si.osMinorVersion = (u8) (LINUX_VERSION_CODE >> 8 & 0x0ff);
	si.osRevision =     (u8) (LINUX_VERSION_CODE & 0x0ff);
	si.busType = SI_PCI_BUS;
	si.processorFamily = DPTI_sig.dsProcessorFamily;

#if defined __i386__ 
	adpt_i386_info(&si);
#elif defined (__ia64__)
	adpt_ia64_info(&si);
#elif defined(__sparc__)
	adpt_sparc_info(&si);
#elif defined (__alpha__)
	adpt_alpha_info(&si);
#else
	si.processorType = 0xff ;
#endif
	if(copy_to_user(buffer, &si, sizeof(si))){
		printk(KERN_WARNING"dpti: Could not copy buffer TO user\n");
		return -EFAULT;
	}

	return 0;
}

#if defined __ia64__ 
static void adpt_ia64_info(sysInfo_S* si)
{
	// This is all the info we need for now
	// We will add more info as our new
	// managmenent utility requires it
	si->processorType = PROC_IA64;
}
#endif


#if defined __sparc__ 
static void adpt_sparc_info(sysInfo_S* si)
{
	// This is all the info we need for now
	// We will add more info as our new
	// managmenent utility requires it
	si->processorType = PROC_ULTRASPARC;
}
#endif

#if defined __alpha__ 
static void adpt_alpha_info(sysInfo_S* si)
{
	// This is all the info we need for now
	// We will add more info as our new
	// managmenent utility requires it
	si->processorType = PROC_ALPHA;
}
#endif

#if defined __i386__

static void adpt_i386_info(sysInfo_S* si)
{
	// This is all the info we need for now
	// We will add more info as our new
	// managmenent utility requires it
	switch (boot_cpu_data.x86) {
	case CPU_386:
		si->processorType = PROC_386;
		break;
	case CPU_486:
		si->processorType = PROC_486;
		break;
	case CPU_586:
		si->processorType = PROC_PENTIUM;
		break;
	default:  // Just in case 
		si->processorType = PROC_PENTIUM;
		break;
	}
}

#endif


static int adpt_ioctl(struct inode *inode, struct file *file, uint cmd,
	      ulong arg)
{
	int minor;
	int error = 0;
	adpt_hba* pHba;
	ulong flags;

	minor = MINOR(inode->i_rdev);
	if (minor >= DPTI_MAX_HBA){
		return -ENXIO;
	}
	down(&adpt_configuration_lock);
	for (pHba = hba_chain; pHba; pHba = pHba->next) {
		if (pHba->unit == minor) {
			break;	/* found adapter */
		}
	}
	up(&adpt_configuration_lock);
	if(pHba == NULL){
		return -ENXIO;
	}

	while((volatile u32) pHba->state & DPTI_STATE_RESET ) {
		set_task_state(current,TASK_UNINTERRUPTIBLE);
		schedule_timeout(2);

	}

	switch (cmd) {
	// TODO: handle 3 cases
	case DPT_SIGNATURE:
		if (copy_to_user((char*)arg, &DPTI_sig, sizeof(DPTI_sig))) {
			return -EFAULT;
		}
		break;
	case I2OUSRCMD:
		return	adpt_i2o_passthru(pHba,(u32*)arg);
		break;

	case DPT_CTRLINFO:{
		drvrHBAinfo_S HbaInfo;

#define FLG_OSD_PCI_VALID 0x0001
#define FLG_OSD_DMA	  0x0002
#define FLG_OSD_I2O	  0x0004
		memset(&HbaInfo, 0, sizeof(HbaInfo));
		HbaInfo.drvrHBAnum = pHba->unit;
		HbaInfo.baseAddr = (ulong) pHba->base_addr_phys;
		HbaInfo.blinkState = adpt_read_blink_led(pHba);
		HbaInfo.pciBusNum =  pHba->pDev->bus->number;
		HbaInfo.pciDeviceNum=PCI_SLOT(pHba->pDev->devfn); 
		HbaInfo.Interrupt = pHba->pDev->irq; 
		HbaInfo.hbaFlags = FLG_OSD_PCI_VALID | FLG_OSD_DMA | FLG_OSD_I2O;
		if(copy_to_user((void *) arg, &HbaInfo, sizeof(HbaInfo))){
			printk(KERN_WARNING"%s: Could not copy HbaInfo TO user\n",pHba->name);
			return -EFAULT;
		}
		break;
		}
	case DPT_SYSINFO:
		return adpt_system_info((void*)arg);
		break;
	case DPT_BLINKLED:{
		u32 value;
		value = (u32)adpt_read_blink_led(pHba);
		if (copy_to_user((char*)arg, &value, sizeof(value))) {
			return -EFAULT;
		}
		break;
		}
	case I2ORESETCMD:
		spin_lock_irqsave(&io_request_lock, flags);
		adpt_hba_reset(pHba);
		spin_unlock_irqrestore(&io_request_lock, flags);
		break;
	case I2ORESCANCMD:
		adpt_rescan(pHba);
		break;
	case DPT_TARGET_BUSY & 0xFFFF:
	case DPT_TARGET_BUSY:
	{
		TARGET_BUSY_T busy;
		struct adpt_device* d;

		if (copy_from_user((void*)&busy, (void*)arg, sizeof(TARGET_BUSY_T))) {
			return -EFAULT;
		}

		d = adpt_find_device(pHba, busy.channel, busy.id, busy.lun);
		if(d == NULL){
			return -ENODEV;
		}
		busy.isBusy = ((d->pScsi_dev) && (0 != d->pScsi_dev->access_count)) ? 1 : 0;
		if (copy_to_user ((char*)arg, &busy, sizeof(busy))) {
			return -EFAULT;
		}
		break;
	}
	default:
		return -EINVAL;
	}

	return error;
}


static void adpt_isr(int irq, void *dev_id, struct pt_regs *regs)
{
	Scsi_Cmnd* cmd;
	adpt_hba* pHba=NULL;
	u32 m;
	ulong reply;
	u32 status=0;
	u32 context;
	ulong flags = 0;

	pHba = dev_id;
	if (pHba == NULL ){
		printk(KERN_WARNING"adpt_isr: NULL dev_id\n");
		return;
	}
	spin_lock_irqsave(&io_request_lock, flags);
	while( readl(pHba->irq_mask) & I2O_INTERRUPT_PENDING_B) {
		m = readl(pHba->reply_port);
		if(m == EMPTY_QUEUE){
			// Try twice then give up
			rmb();
			m = readl(pHba->reply_port);
			if(m == EMPTY_QUEUE){ 
				// This really should not happen
				printk(KERN_ERR"dpti: Could not get reply frame\n");
				spin_unlock_irqrestore(&io_request_lock,flags);
				return;
			}
		}
		reply = (ulong)bus_to_virt(m);

		if (readl(reply) & MSG_FAIL) {
			u32 old_m = readl(reply+28); 
			ulong msg;
			u32 old_context;
			PDEBUG("%s: Failed message\n",pHba->name);
			if(old_m >= 0x100000){
				printk(KERN_ERR"%s: Bad preserved MFA (%x)- dropping frame\n",pHba->name,old_m);
				writel(m,pHba->reply_port);
				continue;
			}
			// Transaction context is 0 in failed reply frame
			msg = (ulong)(pHba->msg_addr_virt + old_m);
			old_context = readl(msg+12);
			writel(old_context, reply+12);
			adpt_send_nop(pHba, old_m);
		} 
		context = readl(reply+8);
		if(context & 0x40000000){ // IOCTL
			ulong p = (ulong)(readl(reply+12));
			if( p != 0) {
				memcpy((void*)p, (void*)reply, REPLY_FRAME_SIZE * 4);
			}
			// All IOCTLs will also be post wait
		}
		if(context & 0x80000000){ // Post wait message
			status = readl(reply+16);
			if(status  >> 24){
				status &=  0xffff; /* Get detail status */
			} else {
				status = I2O_POST_WAIT_OK;
			}
			if(!(context & 0x40000000)) {
				cmd = (Scsi_Cmnd*) readl(reply+12); 
				if(cmd != NULL) {
					printk(KERN_WARNING"%s: Apparent SCSI cmd in Post Wait Context - cmd=%p context=%x\n", pHba->name, cmd, context);
				}
			}
			adpt_i2o_post_wait_complete(context, status);
		} else { // SCSI message
			cmd = (Scsi_Cmnd*) readl(reply+12); 
			if(cmd != NULL){
				if(cmd->serial_number != 0) { // If not timedout
					adpt_i2o_to_scsi(reply, cmd);
				}
			}
		}
		writel(m, pHba->reply_port);
		wmb();
		rmb();
	}
	spin_unlock_irqrestore(&io_request_lock, flags);
	return;

}

static s32 adpt_scsi_to_i2o(adpt_hba* pHba, Scsi_Cmnd* cmd, struct adpt_device* d)
{
	int i;
	u32 msg[MAX_MESSAGE_SIZE];
	u32* mptr;
	u32 *lenptr;
	int direction;
	int scsidir;
	u32 len;
	u32 reqlen;
	s32 rcode;

	memset(msg, 0 , sizeof(msg));
	len = cmd->request_bufflen;
	direction = 0x00000000;	
	
	scsidir = 0x00000000;			// DATA NO XFER
	if(len) {
		/*
		 * Set SCBFlags to indicate if data is being transferred
		 * in or out, or no data transfer
		 * Note:  Do not have to verify index is less than 0 since
		 * cmd->cmnd[0] is an unsigned char
		 */
		switch(cmd->sc_data_direction){
		case SCSI_DATA_READ:
			scsidir  =0x40000000;	// DATA IN  (iop<--dev)
			break;
		case SCSI_DATA_WRITE:
			direction=0x04000000;	// SGL OUT
			scsidir  =0x80000000;	// DATA OUT (iop-->dev)
			break;
		case SCSI_DATA_NONE:
			break;
		case SCSI_DATA_UNKNOWN:
			scsidir  =0x40000000;	// DATA IN  (iop<--dev)
			// Assume In - and continue;
			break;
		default:
			printk(KERN_WARNING"%s: scsi opcode 0x%x not supported.\n",
			     pHba->name, cmd->cmnd[0]);
			cmd->result = (DID_OK <<16) | (INITIATOR_ERROR << 8);
			cmd->scsi_done(cmd);
			return 	0;
		}
	}
	// msg[0] is set later
	// I2O_CMD_SCSI_EXEC
	msg[1] = ((0xff<<24)|(HOST_TID<<12)|d->tid);
	msg[2] = 0;
	msg[3] = (u32)cmd;	/* We want the SCSI control block back */
	// Our cards use the transaction context as the tag for queueing
	// Adaptec/DPT Private stuff 
	msg[4] = I2O_CMD_SCSI_EXEC|(DPT_ORGANIZATION_ID<<16);
	msg[5] = d->tid;
	/* Direction, disconnect ok | sense data | simple queue , CDBLen */
	// I2O_SCB_FLAG_ENABLE_DISCONNECT | 
	// I2O_SCB_FLAG_SIMPLE_QUEUE_TAG | 
	// I2O_SCB_FLAG_SENSE_DATA_IN_MESSAGE;
	msg[6] = scsidir|0x20a00000|cmd->cmd_len;

	mptr=msg+7;

	// Write SCSI command into the message - always 16 byte block 
	memset(mptr, 0,  16);
	memcpy(mptr, cmd->cmnd, cmd->cmd_len);
	mptr+=4;
	lenptr=mptr++;		/* Remember me - fill in when we know */
	reqlen = 14;		// SINGLE SGE
	/* Now fill in the SGList and command */
	if(cmd->use_sg) {
		struct scatterlist *sg = (struct scatterlist *)cmd->request_buffer;
		len = 0;
		for(i = 0 ; i < cmd->use_sg; i++) {
			*mptr++ = direction|0x10000000|sg->length;
			len+=sg->length;
			*mptr++ = virt_to_bus(sg->address);
			sg++;
		}
		/* Make this an end of list */
		mptr[-2] = direction|0xD0000000|(sg-1)->length;
		reqlen = mptr - msg;
		*lenptr = len;
		
		if(cmd->underflow && len != cmd->underflow){
			printk(KERN_WARNING"Cmd len %08X Cmd underflow %08X\n",
				len, cmd->underflow);
		}
	} else {
		*lenptr = len = cmd->request_bufflen;
		if(len == 0) {
			reqlen = 12;
		} else {
			*mptr++ = 0xD0000000|direction|cmd->request_bufflen;
			*mptr++ = virt_to_bus(cmd->request_buffer);
		}
	}
	
	/* Stick the headers on */
	msg[0] = reqlen<<16 | ((reqlen > 12) ? SGL_OFFSET_12 : SGL_OFFSET_0);
	
	// Send it on it's way
	rcode = adpt_i2o_post_this(pHba, msg, reqlen<<2);
	if (rcode == 0) {
		return 0;
	}
	return rcode;
}


static s32 adpt_scsi_register(adpt_hba* pHba,Scsi_Host_Template * sht)
{
	struct Scsi_Host *host = NULL;

	host = scsi_register(sht, sizeof(adpt_hba*));
	if (host == NULL) {
		printk ("%s: scsi_register returned NULL\n",pHba->name);
		return -1;
	}
	host->hostdata[0] = (unsigned long)pHba;
	pHba->host = host;

	host->irq = pHba->pDev->irq;;
	/* no IO ports, so don't have to set host->io_port and 
	 * host->n_io_port
	 */
	host->io_port = 0;
	host->n_io_port = 0;
				/* see comments in hosts.h */
	host->max_id = 16;
	host->max_lun = 256;
	host->max_channel = pHba->top_scsi_channel + 1;
	host->cmd_per_lun = 256;
	host->unique_id = (uint) pHba;
	host->sg_tablesize = pHba->sg_tablesize;
	host->can_queue = pHba->post_fifo_size;
	host->select_queue_depths = adpt_select_queue_depths;

	return 0;
}


static s32 adpt_i2o_to_scsi(ulong reply, Scsi_Cmnd* cmd)
{
	adpt_hba* pHba;
	u32 hba_status;
	u32 dev_status;
	u32 reply_flags = readl(reply) & 0xff00; // Leave it shifted up 8 bits 
	// I know this would look cleaner if I just read bytes
	// but the model I have been using for all the rest of the
	// io is in 4 byte words - so I keep that model
	u16 detailed_status = readl(reply+16) &0xffff;
	dev_status = (detailed_status & 0xff);
	hba_status = detailed_status >> 8;

	// calculate resid for sg 
	cmd->resid = cmd->request_bufflen - readl(reply+5);

	pHba = (adpt_hba*) cmd->host->hostdata[0];

	cmd->sense_buffer[0] = '\0';  // initialize sense valid flag to false

	if(!(reply_flags & MSG_FAIL)) {
		switch(detailed_status & I2O_SCSI_DSC_MASK) {
		case I2O_SCSI_DSC_SUCCESS:
			cmd->result = (DID_OK << 16);
			// handle underflow
			if(readl(reply+5) < cmd->underflow ) {
				cmd->result = (DID_ERROR <<16);
				printk(KERN_WARNING"%s: SCSI CMD underflow\n",pHba->name);
			}
			break;
		case I2O_SCSI_DSC_REQUEST_ABORTED:
			cmd->result = (DID_ABORT << 16);
			break;
		case I2O_SCSI_DSC_PATH_INVALID:
		case I2O_SCSI_DSC_DEVICE_NOT_PRESENT:
		case I2O_SCSI_DSC_SELECTION_TIMEOUT:
		case I2O_SCSI_DSC_COMMAND_TIMEOUT:
		case I2O_SCSI_DSC_NO_ADAPTER:
		case I2O_SCSI_DSC_RESOURCE_UNAVAILABLE:
			printk(KERN_WARNING"%s: SCSI Timeout-Device (%d,%d,%d) hba status=0x%x, dev status=0x%x, cmd=0x%x\n",
				pHba->name, (u32)cmd->channel, (u32)cmd->target, (u32)cmd->lun, hba_status, dev_status, cmd->cmnd[0]);
			cmd->result = (DID_TIME_OUT << 16);
			break;
		case I2O_SCSI_DSC_ADAPTER_BUSY:
		case I2O_SCSI_DSC_BUS_BUSY:
			cmd->result = (DID_BUS_BUSY << 16);
			break;
		case I2O_SCSI_DSC_SCSI_BUS_RESET:
		case I2O_SCSI_DSC_BDR_MESSAGE_SENT:
			cmd->result = (DID_RESET << 16);
			break;
		case I2O_SCSI_DSC_PARITY_ERROR_FAILURE:
			printk(KERN_WARNING"%s: SCSI CMD parity error\n",pHba->name);
			cmd->result = (DID_PARITY << 16);
			break;
		case I2O_SCSI_DSC_UNABLE_TO_ABORT:
		case I2O_SCSI_DSC_COMPLETE_WITH_ERROR:
		case I2O_SCSI_DSC_UNABLE_TO_TERMINATE:
		case I2O_SCSI_DSC_MR_MESSAGE_RECEIVED:
		case I2O_SCSI_DSC_AUTOSENSE_FAILED:
		case I2O_SCSI_DSC_DATA_OVERRUN:
		case I2O_SCSI_DSC_UNEXPECTED_BUS_FREE:
		case I2O_SCSI_DSC_SEQUENCE_FAILURE:
		case I2O_SCSI_DSC_REQUEST_LENGTH_ERROR:
		case I2O_SCSI_DSC_PROVIDE_FAILURE:
		case I2O_SCSI_DSC_REQUEST_TERMINATED:
		case I2O_SCSI_DSC_IDE_MESSAGE_SENT:
		case I2O_SCSI_DSC_UNACKNOWLEDGED_EVENT:
		case I2O_SCSI_DSC_MESSAGE_RECEIVED:
		case I2O_SCSI_DSC_INVALID_CDB:
		case I2O_SCSI_DSC_LUN_INVALID:
		case I2O_SCSI_DSC_SCSI_TID_INVALID:
		case I2O_SCSI_DSC_FUNCTION_UNAVAILABLE:
		case I2O_SCSI_DSC_NO_NEXUS:
		case I2O_SCSI_DSC_CDB_RECEIVED:
		case I2O_SCSI_DSC_LUN_ALREADY_ENABLED:
		case I2O_SCSI_DSC_QUEUE_FROZEN:
		case I2O_SCSI_DSC_REQUEST_INVALID:
		default:
			printk(KERN_WARNING"%s: SCSI error %0x-Device(%d,%d,%d) hba_status=0x%x, dev_status=0x%x, cmd=0x%x\n",
				pHba->name, detailed_status & I2O_SCSI_DSC_MASK, (u32)cmd->channel, (u32)cmd->target, (u32)cmd-> lun,
			       hba_status, dev_status, cmd->cmnd[0]);
			cmd->result = (DID_ERROR << 16);
			break;
		}

		// copy over the request sense data if it was a check
		// condition status
		if(dev_status == 0x02 /*CHECK_CONDITION*/) {
			u32 len = sizeof(cmd->sense_buffer);
			len = (len > 40) ?  40 : len;
			// Copy over the sense data
			memcpy(cmd->sense_buffer, (void*)(reply+28) , len);
			if(cmd->sense_buffer[0] == 0x70 /* class 7 */ && 
			   cmd->sense_buffer[2] == DATA_PROTECT ){
				/* This is to handle an array failed */
				cmd->result = (DID_TIME_OUT << 16);
				printk(KERN_WARNING"%s: SCSI Data Protect-Device (%d,%d,%d) hba_status=0x%x, dev_status=0x%x, cmd=0x%x\n",
					pHba->name, (u32)cmd->channel, (u32)cmd->target, (u32)cmd->lun, 
					hba_status, dev_status, cmd->cmnd[0]);

			}
		}
	} else {
		/* In this condtion we could not talk to the tid
		 * the card rejected it.  We should signal a retry
		 * for a limitted number of retries.
		 */
		cmd->result = (DID_TIME_OUT << 16);
		printk(KERN_WARNING"%s: I2O MSG_FAIL - Device (%d,%d,%d) tid=%d, cmd=0x%x\n",
			pHba->name, (u32)cmd->channel, (u32)cmd->target, (u32)cmd-> lun,
			((struct adpt_device*)(cmd->device->hostdata))->tid, cmd->cmnd[0]);
	}

	cmd->result |= (dev_status);

	if(cmd->scsi_done != NULL){
		cmd->scsi_done(cmd);
	} 
	return cmd->result;
}


static s32 adpt_rescan(adpt_hba* pHba)
{
	s32 rcode;
	ulong flags;

	spin_lock_irqsave(&io_request_lock, flags);
	if ((rcode=adpt_i2o_lct_get(pHba)) < 0){
		spin_unlock_irqrestore(&io_request_lock, flags);
		return rcode;
	}

	if ((rcode=adpt_i2o_reparse_lct(pHba)) < 0){
		spin_unlock_irqrestore(&io_request_lock, flags);
		return rcode;
	}
	spin_unlock_irqrestore(&io_request_lock, flags);
	return 0;
}


static s32 adpt_i2o_reparse_lct(adpt_hba* pHba)
{
	int i;
	int max;
	int tid;
	struct i2o_device *d;
	i2o_lct *lct = pHba->lct;
	u8 bus_no = 0;
	s16 scsi_id;
	s16 scsi_lun;
	u32 buf[10]; // at least 8 u32's
	struct adpt_device* pDev = NULL;
	struct i2o_device* pI2o_dev = NULL;
	
	if (lct == NULL) {
		printk(KERN_ERR "%s: LCT is empty???\n",pHba->name);
		return -1;
	}
	
	max = lct->table_size;	
	max -= 3;
	max /= 9;

	// Mark each drive as unscanned
	for (d = pHba->devices; d; d = d->next) {
		pDev =(struct adpt_device*) d->owner;
		if(!pDev){
			continue;
		}
		pDev->state |= DPTI_DEV_UNSCANNED;
	}

	printk(KERN_INFO "%s: LCT has %d entries.\n", pHba->name,max);
	
	for(i=0;i<max;i++) {
		if( lct->lct_entry[i].user_tid != 0xfff){
			continue;
		}

		if( lct->lct_entry[i].class_id == I2O_CLASS_RANDOM_BLOCK_STORAGE ||
		    lct->lct_entry[i].class_id == I2O_CLASS_SCSI_PERIPHERAL ||
		    lct->lct_entry[i].class_id == I2O_CLASS_FIBRE_CHANNEL_PERIPHERAL ){
			tid = lct->lct_entry[i].tid;
			if(adpt_i2o_query_scalar(pHba, tid, 0x8000, -1, buf, 32)<0) {
				printk(KERN_ERR"%s: Could not query device\n",pHba->name);
				continue;
			}
			bus_no = buf[0]>>16;
			scsi_id = buf[1];
			scsi_lun = (buf[2]>>8 )&0xff;
			pDev = pHba->channel[bus_no].device[scsi_id];
			/* da lun */
			while(pDev) {
				if(pDev->scsi_lun == scsi_lun) {
					break;
				}
				pDev = pDev->next_lun;
			}
			if(!pDev ) { // Something new add it
				d = (struct i2o_device *)kmalloc(sizeof(struct i2o_device), GFP_KERNEL);
				if(d==NULL)
				{
					printk(KERN_CRIT "Out of memory for I2O device data.\n");
					return -ENOMEM;
				}
				
				d->controller = (void*)pHba;
				d->next = NULL;

				memcpy(&d->lct_data, &lct->lct_entry[i], sizeof(i2o_lct_entry));

				d->flags = 0;
				adpt_i2o_report_hba_unit(pHba, d);
				adpt_i2o_install_device(pHba, d);
	
				if(bus_no >= MAX_CHANNEL) {	// Something wrong skip it
					printk(KERN_WARNING"%s: Channel number %d out of range \n", pHba->name, bus_no);
					continue;
				}
				pDev = pHba->channel[bus_no].device[scsi_id];	
				if( pDev == NULL){
					pDev =  kmalloc(sizeof(struct adpt_device),GFP_KERNEL);
					if(pDev == NULL) {
						return -ENOMEM;
					}
					pHba->channel[bus_no].device[scsi_id] = pDev;
				} else {
					while (pDev->next_lun) {
						pDev = pDev->next_lun;
					}
					pDev = pDev->next_lun = kmalloc(sizeof(struct adpt_device),GFP_KERNEL);
					if(pDev == NULL) {
						return -ENOMEM;
					}
				}
				memset(pDev,0,sizeof(struct adpt_device));
				pDev->tid = d->lct_data.tid;
				pDev->scsi_channel = bus_no;
				pDev->scsi_id = scsi_id;
				pDev->scsi_lun = scsi_lun;
				pDev->pI2o_dev = d;
				d->owner = pDev;
				pDev->type = (buf[0])&0xff;
				pDev->flags = (buf[0]>>8)&0xff;
				// Too late, SCSI system has made up it's mind, but what the hey ...
				if(scsi_id > pHba->top_scsi_id){
					pHba->top_scsi_id = scsi_id;
				}
				if(scsi_lun > pHba->top_scsi_lun){
					pHba->top_scsi_lun = scsi_lun;
				}
				continue;
			} // end of new i2o device

			// We found an old device - check it
			while(pDev) {
				if(pDev->scsi_lun == scsi_lun) {
					if(pDev->pScsi_dev->online == FALSE) {
						printk(KERN_WARNING"%s: Setting device (%d,%d,%d) back online\n",
								pHba->name,bus_no,scsi_id,scsi_lun);
						if (pDev->pScsi_dev) {
							pDev->pScsi_dev->online = TRUE;
						}
					}
					d = pDev->pI2o_dev;
					if(d->lct_data.tid != tid) { // something changed
						pDev->tid = tid;
						memcpy(&d->lct_data, &lct->lct_entry[i], sizeof(i2o_lct_entry));
						if (pDev->pScsi_dev) {
							pDev->pScsi_dev->changed = TRUE;
							pDev->pScsi_dev->removable = TRUE;
						}
					}
					// Found it - mark it scanned
					pDev->state = DPTI_DEV_ONLINE;
					break;
				}
				pDev = pDev->next_lun;
			}
		}
	}
	for (pI2o_dev = pHba->devices; pI2o_dev; pI2o_dev = pI2o_dev->next) {
		pDev =(struct adpt_device*) pI2o_dev->owner;
		if(!pDev){
			continue;
		}
		// Drive offline drives that previously existed but could not be found
		// in the LCT table
		if (pDev->state & DPTI_DEV_UNSCANNED){
			pDev->state = DPTI_DEV_OFFLINE;
			printk(KERN_WARNING"%s: Device (%d,%d,%d) offline\n",pHba->name,pDev->scsi_channel,pDev->scsi_id,pDev->scsi_lun);
			if (pDev->pScsi_dev) {
				pDev->pScsi_dev->online = FALSE;
				if (pDev->pScsi_dev->access_count) {
					// A drive that was mounted is no longer there... bad!
					SCSI_LOG_ERROR_RECOVERY(1, printk ("%s:Rescan: Previously "
								 "mounted drive not found!\n",pHba->name));
					printk(KERN_WARNING"%s:Mounted drive taken offline\n",pHba->name);
				}
			}
		}
	}
	return 0;
}

static void adpt_fail_posted_scbs(adpt_hba* pHba)
{
	Scsi_Cmnd* 	cmd = NULL;
	Scsi_Device* 	d = NULL;

	if( pHba->host->host_queue != NULL ) {
		d = pHba->host->host_queue;
		if(!d){
			return;
		}
		while( d->next != NULL ){
			for(cmd = d->device_queue; cmd ; cmd = cmd->next){
				if(cmd->serial_number == 0){
					continue;
				}
				cmd->result = (DID_OK << 16) | (QUEUE_FULL <<1);
				cmd->scsi_done(cmd);
			}
			d = d->next;
		}
	}
}


/*============================================================================
 *  Routines from i2o subsystem
 *============================================================================
 */



/*
 *	Bring an I2O controller into HOLD state. See the spec.
 */
static int adpt_i2o_activate_hba(adpt_hba* pHba)
{
	int rcode;

	if(pHba->initialized ) {
		if (adpt_i2o_status_get(pHba) < 0) {
			if((rcode = adpt_i2o_reset_hba(pHba)) != 0){
				printk(KERN_WARNING"%s: Could NOT reset.\n", pHba->name);
				return rcode;
			}
			if (adpt_i2o_status_get(pHba) < 0) {
				printk(KERN_INFO "HBA not responding.\n");
				return -1;
			}
		}

		if(pHba->status_block->iop_state == ADAPTER_STATE_FAULTED) {
			printk(KERN_CRIT "%s: hardware fault\n", pHba->name);
			return -1;
		}

		if (pHba->status_block->iop_state == ADAPTER_STATE_READY ||
		    pHba->status_block->iop_state == ADAPTER_STATE_OPERATIONAL ||
		    pHba->status_block->iop_state == ADAPTER_STATE_HOLD ||
		    pHba->status_block->iop_state == ADAPTER_STATE_FAILED) {
			adpt_i2o_reset_hba(pHba);			
			if (adpt_i2o_status_get(pHba) < 0 || pHba->status_block->iop_state != ADAPTER_STATE_RESET) {
				printk(KERN_ERR "%s: Failed to initialize.\n", pHba->name);
				return -1;
			}
		}
	} else {
		if((rcode = adpt_i2o_reset_hba(pHba)) != 0){
			printk(KERN_WARNING"%s: Could NOT reset.\n", pHba->name);
			return rcode;
		}

	}

	if (adpt_i2o_init_outbound_q(pHba) < 0) {
		return -1;
	}

	/* In HOLD state */
	
	if (adpt_i2o_hrt_get(pHba) < 0) {
		return -1;
	}

	return 0;
}

/*
 *	Bring a controller online into OPERATIONAL state. 
 */
 
static int adpt_i2o_online_hba(adpt_hba* pHba)
{
	if (adpt_i2o_systab_send(pHba) < 0) {
		adpt_i2o_delete_hba(pHba);
		return -1;
	}
	/* In READY state */

	if (adpt_i2o_enable_hba(pHba) < 0) {
		adpt_i2o_delete_hba(pHba);
		return -1;
	}

	/* In OPERATIONAL state  */
	return 0;
}

static s32 adpt_send_nop(adpt_hba*pHba,u32 m)
{
	u32 *msg;
	ulong timeout = jiffies + 5*HZ;

	while(m == EMPTY_QUEUE){
		rmb();
		m = readl(pHba->post_port);
		if(m != EMPTY_QUEUE){
			break;
		}
		if(time_after(jiffies,timeout)){
			printk(KERN_ERR "%s: Timeout waiting for message frame!\n",pHba->name);
			return 2;
		}
	}
	msg = (u32*)(pHba->msg_addr_virt + m);
	writel( THREE_WORD_MSG_SIZE | SGL_OFFSET_0,&msg[0]);
	writel( I2O_CMD_UTIL_NOP << 24 | HOST_TID << 12 | 0,&msg[1]);
	writel( 0,&msg[2]);
	wmb();

	writel(m, pHba->post_port);
	wmb();
	return 0;
}

static s32 adpt_i2o_init_outbound_q(adpt_hba* pHba)
{
	u8 *status;
	u32 *msg = NULL;
	int i;
	ulong timeout = jiffies + TMOUT_INITOUTBOUND*HZ;
	u32* ptr;
	u32 outbound_frame;  // This had to be a 32 bit address
	u32 m;

	do {
		rmb();
		m = readl(pHba->post_port);
		if (m != EMPTY_QUEUE) {
			break;
		}

		if(time_after(jiffies,timeout)){
			printk(KERN_WARNING"%s: Timeout waiting for message frame\n",pHba->name);
			return -ETIMEDOUT;
		}
	} while(m == EMPTY_QUEUE);

	msg=(u32 *)(pHba->msg_addr_virt+m);

	status = kmalloc(4,GFP_KERNEL|ADDR32);
	if (status==NULL) {
		adpt_send_nop(pHba, m);
		printk(KERN_WARNING"%s: IOP reset failed - no free memory.\n",
			pHba->name);
		return -ENOMEM;
	}
	memset(status, 0, 4);

	writel(EIGHT_WORD_MSG_SIZE| SGL_OFFSET_6, &msg[0]);
	writel(I2O_CMD_OUTBOUND_INIT<<24 | HOST_TID<<12 | ADAPTER_TID, &msg[1]);
	writel(0, &msg[2]);
	writel(0x0106, &msg[3]);	/* Transaction context */
	writel(4096, &msg[4]);		/* Host page frame size */
	writel((REPLY_FRAME_SIZE)<<16|0x80, &msg[5]);	/* Outbound msg frame size and Initcode */
	writel(0xD0000004, &msg[6]);		/* Simple SG LE, EOB */
	writel(virt_to_bus(status), &msg[7]);

	writel(m, pHba->post_port);
	wmb();

	// Wait for the reply status to come back
	do {
		if (*status) {
			if (*status != 0x01 /*I2O_EXEC_OUTBOUND_INIT_IN_PROGRESS*/) {
				break;
			}
		}
		rmb();
		if(time_after(jiffies,timeout)){
			printk(KERN_WARNING"%s: Timeout Initializing\n",pHba->name);
			kfree((void*)status);
			return -ETIMEDOUT;
		}
	} while (1);

	// If the command was successful, fill the fifo with our reply
	// message packets
	if(*status != 0x04 /*I2O_EXEC_OUTBOUND_INIT_COMPLETE*/) {
		kfree((void*)status);
		return -2;
	}
	kfree((void*)status);

	if(pHba->reply_pool != NULL){
		kfree(pHba->reply_pool);
	}

	pHba->reply_pool = (u32*)kmalloc(pHba->reply_fifo_size * REPLY_FRAME_SIZE * 4, GFP_KERNEL|ADDR32);
	if(!pHba->reply_pool){
		printk(KERN_ERR"%s: Could not allocate reply pool\n",pHba->name);
		return -1;
	}
	memset(pHba->reply_pool, 0 , pHba->reply_fifo_size * REPLY_FRAME_SIZE * 4);

	ptr = pHba->reply_pool;
	for(i = 0; i < pHba->reply_fifo_size; i++) {
		outbound_frame = (u32)virt_to_bus(ptr);
		writel(outbound_frame, pHba->reply_port);
		wmb();
		ptr +=  REPLY_FRAME_SIZE;
	}
	adpt_i2o_status_get(pHba);
	return 0;
}


/*
 * I2O System Table.  Contains information about
 * all the IOPs in the system.  Used to inform IOPs
 * about each other's existence.
 *
 * sys_tbl_ver is the CurrentChangeIndicator that is
 * used by IOPs to track changes.
 */



static s32 adpt_i2o_status_get(adpt_hba* pHba)
{
	ulong timeout;
	u32 m;
	u32 *msg;
	u8 *status_block=NULL;
	ulong status_block_bus;

	if(pHba->status_block == NULL) {
		pHba->status_block = (i2o_status_block*)
			kmalloc(sizeof(i2o_status_block),GFP_KERNEL|ADDR32);
		if(pHba->status_block == NULL) {
			printk(KERN_ERR
			"dpti%d: Get Status Block failed; Out of memory. \n", 
			pHba->unit);
			return -ENOMEM;
		}
	}
	memset(pHba->status_block, 0, sizeof(i2o_status_block));
	status_block = (u8*)(pHba->status_block);
	status_block_bus = virt_to_bus(pHba->status_block);
	timeout = jiffies+TMOUT_GETSTATUS*HZ;
	do {
		rmb();
		m = readl(pHba->post_port);
		if (m != EMPTY_QUEUE) {
			break;
		}
		if(time_after(jiffies,timeout)){
			printk(KERN_ERR "%s: Timeout waiting for message !\n",
					pHba->name);
			return -ETIMEDOUT;
		}
	} while(m==EMPTY_QUEUE);

	
	msg=(u32*)(pHba->msg_addr_virt+m);

	writel(NINE_WORD_MSG_SIZE|SGL_OFFSET_0, &msg[0]);
	writel(I2O_CMD_STATUS_GET<<24|HOST_TID<<12|ADAPTER_TID, &msg[1]);
	writel(1, &msg[2]);
	writel(0, &msg[3]);
	writel(0, &msg[4]);
	writel(0, &msg[5]);
	writel(((u32)status_block_bus)&0xffffffff, &msg[6]);
	writel(0, &msg[7]);
	writel(sizeof(i2o_status_block), &msg[8]); // 88 bytes

	//post message
	writel(m, pHba->post_port);
	wmb();

	while(status_block[87]!=0xff){
		if(time_after(jiffies,timeout)){
			printk(KERN_ERR"dpti%d: Get status timeout.\n",
				pHba->unit);
			return -ETIMEDOUT;
		}
		rmb();
	}

	// Set up our number of outbound and inbound messages
	pHba->post_fifo_size = pHba->status_block->max_inbound_frames;
	if (pHba->post_fifo_size > MAX_TO_IOP_MESSAGES) {
		pHba->post_fifo_size = MAX_TO_IOP_MESSAGES;
	}

	pHba->reply_fifo_size = pHba->status_block->max_outbound_frames;
	if (pHba->reply_fifo_size > MAX_FROM_IOP_MESSAGES) {
		pHba->reply_fifo_size = MAX_FROM_IOP_MESSAGES;
	}

	// Calculate the Scatter Gather list size
	pHba->sg_tablesize = (pHba->status_block->inbound_frame_size * 4 -40)/ sizeof(struct sg_simple_element);
	if (pHba->sg_tablesize > SG_LIST_ELEMENTS) {
		pHba->sg_tablesize = SG_LIST_ELEMENTS;
	}


#ifdef DEBUG
	printk("dpti%d: State = ",pHba->unit);
	switch(pHba->status_block->iop_state) {
		case 0x01:
			printk("INIT\n");
			break;
		case 0x02:
			printk("RESET\n");
			break;
		case 0x04:
			printk("HOLD\n");
			break;
		case 0x05:
			printk("READY\n");
			break;
		case 0x08:
			printk("OPERATIONAL\n");
			break;
		case 0x10:
			printk("FAILED\n");
			break;
		case 0x11:
			printk("FAULTED\n");
			break;
		default:
			printk("%x (unknown!!)\n",pHba->status_block->iop_state);
	}
#endif
	return 0;
}

/*
 * Get the IOP's Logical Configuration Table
 */
static int adpt_i2o_lct_get(adpt_hba* pHba)
{
	u32 msg[8];
	int ret;
	u32 buf[16];

	if ((pHba->lct_size == 0) || (pHba->lct == NULL)){
		pHba->lct_size = pHba->status_block->expected_lct_size;
	}
	do {
		if (pHba->lct == NULL) {
			pHba->lct = kmalloc(pHba->lct_size, GFP_KERNEL|ADDR32);
			if(pHba->lct == NULL) {
				printk(KERN_CRIT "%s: Lct Get failed. Out of memory.\n",
					pHba->name);
				return -ENOMEM;
			}
		}
		memset(pHba->lct, 0, pHba->lct_size);

		msg[0] = EIGHT_WORD_MSG_SIZE|SGL_OFFSET_6;
		msg[1] = I2O_CMD_LCT_NOTIFY<<24 | HOST_TID<<12 | ADAPTER_TID;
		msg[2] = 0;
		msg[3] = 0;
		msg[4] = 0xFFFFFFFF;	/* All devices */
		msg[5] = 0x00000000;	/* Report now */
		msg[6] = 0xD0000000|pHba->lct_size;
		msg[7] = virt_to_bus(pHba->lct);

		if ((ret=adpt_i2o_post_wait(pHba, msg, sizeof(msg), 360))) {
			printk(KERN_ERR "%s: LCT Get failed (status=%#10x.\n", 
				pHba->name, ret);	
			printk(KERN_ERR"Adaptec: Error Reading Hardware.\n");
			return ret;
		}

		if ((pHba->lct->table_size << 2) > pHba->lct_size) {
			pHba->lct_size = pHba->lct->table_size << 2;
			kfree(pHba->lct);
			pHba->lct = NULL;
		}
	} while (pHba->lct == NULL);

	PDEBUG("%s: Hardware resource table read.\n", pHba->name);


	// I2O_DPT_EXEC_IOP_BUFFERS_GROUP_NO;
	if(adpt_i2o_query_scalar(pHba, 0 , 0x8000, -1, buf, sizeof(buf))>=0) {
		pHba->FwDebugBufferSize = buf[1];
		pHba->FwDebugBuffer_P    = pHba->base_addr_virt + buf[0];
		pHba->FwDebugFlags_P     = pHba->FwDebugBuffer_P + FW_DEBUG_FLAGS_OFFSET;
		pHba->FwDebugBLEDvalue_P = pHba->FwDebugBuffer_P + FW_DEBUG_BLED_OFFSET;
		pHba->FwDebugBLEDflag_P  = pHba->FwDebugBLEDvalue_P + 1;
		pHba->FwDebugStrLength_P = pHba->FwDebugBuffer_P + FW_DEBUG_STR_LENGTH_OFFSET;
		pHba->FwDebugBuffer_P += buf[2]; 
		pHba->FwDebugFlags = 0;
	}

	return 0;
}

static int adpt_i2o_build_sys_table(void)
{
	adpt_hba* pHba = NULL;
	int count = 0;

	sys_tbl_len = sizeof(struct i2o_sys_tbl) +	// Header + IOPs
				(hba_count) * sizeof(struct i2o_sys_tbl_entry);

	if(sys_tbl)
		kfree(sys_tbl);

	sys_tbl = kmalloc(sys_tbl_len, GFP_KERNEL|ADDR32);
	if(!sys_tbl) {
		printk(KERN_WARNING "SysTab Set failed. Out of memory.\n");	
		return -ENOMEM;
	}
	memset(sys_tbl, 0, sys_tbl_len);

	sys_tbl->num_entries = hba_count;
	sys_tbl->version = I2OVERSION;
	sys_tbl->change_ind = sys_tbl_ind++;

	for(pHba = hba_chain; pHba; pHba = pHba->next) {
		// Get updated Status Block so we have the latest information
		if (adpt_i2o_status_get(pHba)) {
			sys_tbl->num_entries--;
			continue; // try next one	
		}

		sys_tbl->iops[count].org_id = pHba->status_block->org_id;
		sys_tbl->iops[count].iop_id = pHba->unit + 2;
		sys_tbl->iops[count].seg_num = 0;
		sys_tbl->iops[count].i2o_version = pHba->status_block->i2o_version;
		sys_tbl->iops[count].iop_state = pHba->status_block->iop_state;
		sys_tbl->iops[count].msg_type = pHba->status_block->msg_type;
		sys_tbl->iops[count].frame_size = pHba->status_block->inbound_frame_size;
		sys_tbl->iops[count].last_changed = sys_tbl_ind - 1; // ??
		sys_tbl->iops[count].iop_capabilities = pHba->status_block->iop_capabilities;
		sys_tbl->iops[count].inbound_low = (u32)virt_to_bus((void*)pHba->post_port);
		sys_tbl->iops[count].inbound_high = (u32)((u64)virt_to_bus((void*)pHba->post_port)>>32);

		count++;
	}

#ifdef DEBUG
{
	u32 *table = (u32*)sys_tbl;
	printk(KERN_DEBUG"sys_tbl_len=%d in 32bit words\n",(sys_tbl_len >>2));
	for(count = 0; count < (sys_tbl_len >>2); count++) {
		printk(KERN_INFO "sys_tbl[%d] = %0#10x\n", 
			count, table[count]);
	}
}
#endif

	return 0;
}


/*
 *	 Dump the information block associated with a given unit (TID)
 */
 
static void adpt_i2o_report_hba_unit(adpt_hba* pHba, struct i2o_device *d)
{
	char buf[64];
	int unit = d->lct_data.tid;

	printk(KERN_INFO "TID %3.3d ", unit);

	if(adpt_i2o_query_scalar(pHba, unit, 0xF100, 3, buf, 16)>=0)
	{
		buf[16]=0;
		printk(" Vendor: %-12.12s", buf);
	}
	if(adpt_i2o_query_scalar(pHba, unit, 0xF100, 4, buf, 16)>=0)
	{
		buf[16]=0;
		printk(" Device: %-12.12s", buf);
	}
	if(adpt_i2o_query_scalar(pHba, unit, 0xF100, 6, buf, 8)>=0)
	{
		buf[8]=0;
		printk(" Rev: %-12.12s\n", buf);
	}
#ifdef DEBUG
	 printk(KERN_INFO "\tClass: %.21s\n", adpt_i2o_get_class_name(d->lct_data.class_id));
	 printk(KERN_INFO "\tSubclass: 0x%04X\n", d->lct_data.sub_class);
	 printk(KERN_INFO "\tFlags: ");

	 if(d->lct_data.device_flags&(1<<0))
		  printk("C");	     // ConfigDialog requested
	 if(d->lct_data.device_flags&(1<<1))
		  printk("U");	     // Multi-user capable
	 if(!(d->lct_data.device_flags&(1<<4)))
		  printk("P");	     // Peer service enabled!
	 if(!(d->lct_data.device_flags&(1<<5)))
		  printk("M");	     // Mgmt service enabled!
	 printk("\n");
#endif
}

#ifdef DEBUG
/*
 *	Do i2o class name lookup
 */
static const char *adpt_i2o_get_class_name(int class)
{
	int idx = 16;
	static char *i2o_class_name[] = {
		"Executive",
		"Device Driver Module",
		"Block Device",
		"Tape Device",
		"LAN Interface",
		"WAN Interface",
		"Fibre Channel Port",
		"Fibre Channel Device",
		"SCSI Device",
		"ATE Port",
		"ATE Device",
		"Floppy Controller",
		"Floppy Device",
		"Secondary Bus Port",
		"Peer Transport Agent",
		"Peer Transport",
		"Unknown"
	};
	
	switch(class&0xFFF) {
	case I2O_CLASS_EXECUTIVE:
		idx = 0; break;
	case I2O_CLASS_DDM:
		idx = 1; break;
	case I2O_CLASS_RANDOM_BLOCK_STORAGE:
		idx = 2; break;
	case I2O_CLASS_SEQUENTIAL_STORAGE:
		idx = 3; break;
	case I2O_CLASS_LAN:
		idx = 4; break;
	case I2O_CLASS_WAN:
		idx = 5; break;
	case I2O_CLASS_FIBRE_CHANNEL_PORT:
		idx = 6; break;
	case I2O_CLASS_FIBRE_CHANNEL_PERIPHERAL:
		idx = 7; break;
	case I2O_CLASS_SCSI_PERIPHERAL:
		idx = 8; break;
	case I2O_CLASS_ATE_PORT:
		idx = 9; break;
	case I2O_CLASS_ATE_PERIPHERAL:
		idx = 10; break;
	case I2O_CLASS_FLOPPY_CONTROLLER:
		idx = 11; break;
	case I2O_CLASS_FLOPPY_DEVICE:
		idx = 12; break;
	case I2O_CLASS_BUS_ADAPTER_PORT:
		idx = 13; break;
	case I2O_CLASS_PEER_TRANSPORT_AGENT:
		idx = 14; break;
	case I2O_CLASS_PEER_TRANSPORT:
		idx = 15; break;
	}
	return i2o_class_name[idx];
}
#endif


static s32 adpt_i2o_hrt_get(adpt_hba* pHba)
{
	u32 msg[6];
	int ret, size = sizeof(i2o_hrt);

	do {
		if (pHba->hrt == NULL) {
			pHba->hrt=kmalloc(size, GFP_KERNEL|ADDR32);
			if (pHba->hrt == NULL) {
				printk(KERN_CRIT "%s: Hrt Get failed; Out of memory.\n", pHba->name);
				return -ENOMEM;
			}
		}

		msg[0]= SIX_WORD_MSG_SIZE| SGL_OFFSET_4;
		msg[1]= I2O_CMD_HRT_GET<<24 | HOST_TID<<12 | ADAPTER_TID;
		msg[2]= 0;
		msg[3]= 0;
		msg[4]= (0xD0000000 | size);    /* Simple transaction */
		msg[5]= virt_to_bus(pHba->hrt);   /* Dump it here */

		if ((ret = adpt_i2o_post_wait(pHba, msg, sizeof(msg),20))) {
			printk(KERN_ERR "%s: Unable to get HRT (status=%#10x)\n", pHba->name, ret);
			return ret;
		}

		if (pHba->hrt->num_entries * pHba->hrt->entry_len << 2 > size) {
			size = pHba->hrt->num_entries * pHba->hrt->entry_len << 2;
			kfree(pHba->hrt);
			pHba->hrt = NULL;
		}
	} while(pHba->hrt == NULL);
	return 0;
}                                                                                                                                       

/*
 *	 Query one scalar group value or a whole scalar group.
 */		    	
static int adpt_i2o_query_scalar(adpt_hba* pHba, int tid, 
			int group, int field, void *buf, int buflen)
{
	u16 opblk[] = { 1, 0, I2O_PARAMS_FIELD_GET, group, 1, field };
	u8  resblk[8+buflen]; /* 8 bytes for header */
	int size;

	if (field == -1)  		/* whole group */
			opblk[4] = -1;

	size = adpt_i2o_issue_params(I2O_CMD_UTIL_PARAMS_GET, pHba, tid, 
		opblk, sizeof(opblk), resblk, sizeof(resblk));
			
	memcpy(buf, resblk+8, buflen);  /* cut off header */

	if (size < 0)
		return size;	

	return buflen;
}


/*	Issue UTIL_PARAMS_GET or UTIL_PARAMS_SET
 *
 *	This function can be used for all UtilParamsGet/Set operations.
 *	The OperationBlock is given in opblk-buffer, 
 *	and results are returned in resblk-buffer.
 *	Note that the minimum sized resblk is 8 bytes and contains
 *	ResultCount, ErrorInfoSize, BlockStatus and BlockSize.
 */
static int adpt_i2o_issue_params(int cmd, adpt_hba* pHba, int tid, 
		  void *opblk, int oplen, void *resblk, int reslen)
{
	u32 msg[9]; 
	u32 *res = (u32 *)resblk;
	int wait_status;

	msg[0] = NINE_WORD_MSG_SIZE | SGL_OFFSET_5;
	msg[1] = cmd << 24 | HOST_TID << 12 | tid; 
	msg[2] = 0;
	msg[3] = 0;
	msg[4] = 0;
	msg[5] = 0x54000000 | oplen;	/* OperationBlock */
	msg[6] = virt_to_bus(opblk);
	msg[7] = 0xD0000000 | reslen;	/* ResultBlock */
	msg[8] = virt_to_bus(resblk);

	if ((wait_status = adpt_i2o_post_wait(pHba, msg, sizeof(msg), 20))) {
   		return wait_status; 	/* -DetailedStatus */
	}

	if (res[1]&0x00FF0000) { 	/* BlockStatus != SUCCESS */
		printk(KERN_WARNING "%s: %s - Error:\n  ErrorInfoSize = 0x%02x, "
			"BlockStatus = 0x%02x, BlockSize = 0x%04x\n",
			pHba->name,
			(cmd == I2O_CMD_UTIL_PARAMS_SET) ? "PARAMS_SET"
							 : "PARAMS_GET",   
			res[1]>>24, (res[1]>>16)&0xFF, res[1]&0xFFFF);
		return -((res[1] >> 16) & 0xFF); /* -BlockStatus */
	}

	 return 4 + ((res[1] & 0x0000FFFF) << 2); /* bytes used in resblk */ 
}


static s32 adpt_i2o_quiesce_hba(adpt_hba* pHba)
{
	u32 msg[4];
	int ret;

	adpt_i2o_status_get(pHba);

	/* SysQuiesce discarded if IOP not in READY or OPERATIONAL state */

	if((pHba->status_block->iop_state != ADAPTER_STATE_READY) &&
   	   (pHba->status_block->iop_state != ADAPTER_STATE_OPERATIONAL)){
		return 0;
	}

	msg[0] = FOUR_WORD_MSG_SIZE|SGL_OFFSET_0;
	msg[1] = I2O_CMD_SYS_QUIESCE<<24|HOST_TID<<12|ADAPTER_TID;
	msg[2] = 0;
	msg[3] = 0;

	if((ret = adpt_i2o_post_wait(pHba, msg, sizeof(msg), 240))) {
		printk(KERN_INFO"dpti%d: Unable to quiesce (status=%#x).\n",
				pHba->unit, -ret);
	} else {
		printk(KERN_INFO"dpti%d: Quiesced.\n",pHba->unit);
	}

	adpt_i2o_status_get(pHba);
	return ret;
}


/* 
 * Enable IOP. Allows the IOP to resume external operations.
 */
static int adpt_i2o_enable_hba(adpt_hba* pHba)
{
	u32 msg[4];
	int ret;
	
	adpt_i2o_status_get(pHba);
	if(!pHba->status_block){
		return -ENOMEM;
	}
	/* Enable only allowed on READY state */
	if(pHba->status_block->iop_state == ADAPTER_STATE_OPERATIONAL)
		return 0;

	if(pHba->status_block->iop_state != ADAPTER_STATE_READY)
		return -EINVAL;

	msg[0]=FOUR_WORD_MSG_SIZE|SGL_OFFSET_0;
	msg[1]=I2O_CMD_SYS_ENABLE<<24|HOST_TID<<12|ADAPTER_TID;
	msg[2]= 0;
	msg[3]= 0;

	if ((ret = adpt_i2o_post_wait(pHba, msg, sizeof(msg), 240))) {
		printk(KERN_WARNING"%s: Could not enable (status=%#10x).\n", 
			pHba->name, ret);
	} else {
		PDEBUG("%s: Enabled.\n", pHba->name);
	}

	adpt_i2o_status_get(pHba);
	return ret;
}


static int adpt_i2o_systab_send(adpt_hba* pHba)
{
	 u32 msg[12];
	 int ret;

	msg[0] = I2O_MESSAGE_SIZE(12) | SGL_OFFSET_6;
	msg[1] = I2O_CMD_SYS_TAB_SET<<24 | HOST_TID<<12 | ADAPTER_TID;
	msg[2] = 0;
	msg[3] = 0;
	msg[4] = (0<<16) | ((pHba->unit+2) << 12); /* Host 0 IOP ID (unit + 2) */
	msg[5] = 0;				   /* Segment 0 */

	/* 
	 * Provide three SGL-elements:
	 * System table (SysTab), Private memory space declaration and 
	 * Private i/o space declaration  
	 */
	msg[6] = 0x54000000 | sys_tbl_len;
	msg[7] = virt_to_phys(sys_tbl);
	msg[8] = 0x54000000 | 0;
	msg[9] = 0;
	msg[10] = 0xD4000000 | 0;
	msg[11] = 0;

	if ((ret=adpt_i2o_post_wait(pHba, msg, sizeof(msg), 120))) {
		printk(KERN_INFO "%s: Unable to set SysTab (status=%#10x).\n", 
			pHba->name, ret);
	}
#ifdef DEBUG
	else {
		PINFO("%s: SysTab set.\n", pHba->name);
	}
#endif

	return ret;	
 }


/*============================================================================
 *
 *============================================================================
 */


#ifdef UARTDELAY 

static static void adpt_delay(int millisec)
{
	int i;
	for (i = 0; i < millisec; i++) {
		udelay(1000);	/* delay for one millisecond */
	}
}

#endif

static Scsi_Host_Template driver_template = DPT_I2O;
#include "scsi_module.c"
EXPORT_NO_SYMBOLS;
MODULE_LICENSE("GPL");
