/*
 *  c 2001 PPC 64 Team, IBM Corp
 *
 *      This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 *
 * /proc/ppc64/rtas/firmware_flash interface
 *
 * This file implements a firmware_flash interface to pump a firmware
 * image into the kernel.  At reboot time rtas_restart() will see the
 * firmware image and flash it as it reboots (see rtas.c).
 */

#include <linux/module.h>

#include <linux/config.h>
#include <linux/proc_fs.h>
#include <linux/init.h>
#include <asm/uaccess.h>
#include <asm/rtas.h>

#define MODULE_VERSION "1.0"
#define MODULE_NAME "rtas_flash"

#define FIRMWARE_FLASH_NAME "firmware_flash"
#define FIRMWARE_UPDATE_NAME "firmware_update"
#define MANAGE_FLASH_NAME "manage_flash"
#define VALIDATE_FLASH_NAME "validate_flash"

/* General RTAS Status Codes */
#define RTAS_RC_SUCCESS  0
#define RTAS_RC_HW_ERR	-1
#define RTAS_RC_BUSY	-2

/* Flash image status values */
#define FLASH_AUTH           -9002 /* RTAS Not Service Authority Partition */
#define FLASH_NO_OP          -1099 /* No operation initiated by user */
#define FLASH_IMG_SHORT	     -1005 /* Flash image shorter than expected */
#define FLASH_IMG_BAD_LEN    -1004 /* Bad length value in flash list block */
#define FLASH_IMG_NULL_DATA  -1003 /* Bad data value in flash list block */
#define FLASH_IMG_READY      0     /* Firmware img ready for flash on reboot */

/* Manage image status values */
#define MANAGE_AUTH          -9002 /* RTAS Not Service Authority Partition */
#define MANAGE_ACTIVE_ERR    -9001 /* RTAS Cannot Overwrite Active Img */
#define MANAGE_NO_OP         -1099 /* No operation initiated by user */
#define MANAGE_PARAM_ERR     -3    /* RTAS Parameter Error */
#define MANAGE_HW_ERR        -1    /* RTAS Hardware Error */

/* Validate image status values */
#define VALIDATE_AUTH          -9002 /* RTAS Not Service Authority Partition */
#define VALIDATE_NO_OP         -1099 /* No operation initiated by the user */
#define VALIDATE_INCOMPLETE    -1002 /* User copied < VALIDATE_BUF_SIZE */
#define VALIDATE_READY	       -1001 /* Firmware image ready for validation */
#define VALIDATE_PARAM_ERR     -3    /* RTAS Parameter Error */
#define VALIDATE_HW_ERR        -1    /* RTAS Hardware Error */
#define VALIDATE_TMP_UPDATE    0     /* Validate Return Status */
#define VALIDATE_FLASH_AUTH    1     /* Validate Return Status */
#define VALIDATE_INVALID_IMG   2     /* Validate Return Status */
#define VALIDATE_CUR_UNKNOWN   3     /* Validate Return Status */
#define VALIDATE_TMP_COMMIT_DL 4     /* Validate Return Status */
#define VALIDATE_TMP_COMMIT    5     /* Validate Return Status */
#define VALIDATE_TMP_UPDATE_DL 6     /* Validate Return Status */

/* ibm,manage-flash-image operation tokens */
#define RTAS_REJECT_TMP_IMG   0
#define RTAS_COMMIT_TMP_IMG   1

/* Array sizes */
#define VALIDATE_BUF_SIZE 4096
#define RTAS_MSG_MAXLEN   64

/* Local copy of the flash block list.
 * We only allow one open of the flash proc file and create this
 * list as we go.  This list will be put in the kernel's
 * rtas_firmware_flash_list global var once it is fully read.
 *
 * For convenience as we build the list we use virtual addrs,
 * we do not fill in the version number, and the length field
 * is treated as the number of entries currently in the block
 * (i.e. not a byte count).  This is all fixed on release.
 */

/* Status int must be first member of struct */
struct rtas_update_flash_t
{
	int status;			/* Flash update status */
	struct flash_block_list *flist; /* Local copy of flash block list */
};

/* Status int must be first member of struct */
struct rtas_manage_flash_t
{
	int status;			/* Returned status */
	unsigned int op;		/* Reject or commit image */
};

/* Status int must be first member of struct */
struct rtas_validate_flash_t
{
	int status;		 	/* Returned status */
	char buf[VALIDATE_BUF_SIZE]; 	/* Candidate image buffer */
	unsigned int buf_size;		/* Size of image buf */
	unsigned int update_results;	/* Update results token */
};

static spinlock_t flash_file_open_lock = SPIN_LOCK_UNLOCKED;
static struct proc_dir_entry *firmware_flash_pde = NULL;
static struct proc_dir_entry *firmware_update_pde = NULL;
static struct proc_dir_entry *validate_pde = NULL;
static struct proc_dir_entry *manage_pde = NULL;

/* Do simple sanity checks on the flash image. */
static int flash_list_valid(struct flash_block_list *flist)
{
	struct flash_block_list *f;
	int i;
	unsigned long block_size, image_size;

	/* Paranoid self test here.  We also collect the image size. */
	image_size = 0;
	for (f = flist; f; f = f->next) {
		for (i = 0; i < f->num_blocks; i++) {
			if (f->blocks[i].data == NULL) {
				return FLASH_IMG_NULL_DATA;
			}
			block_size = f->blocks[i].length;
			if (block_size <= 0 || block_size > PAGE_SIZE) {
				return FLASH_IMG_BAD_LEN;
			}
			image_size += block_size;
		}
	}

	if (image_size < 2)
		return FLASH_NO_OP;

	printk(KERN_INFO "FLASH: flash image with %ld bytes stored for hardware flash on reboot\n", image_size);

	return FLASH_IMG_READY;
}

static void free_flash_list(struct flash_block_list *f)
{
	struct flash_block_list *next;
	int i;

	while (f) {
		for (i = 0; i < f->num_blocks; i++)
			free_page((unsigned long)(f->blocks[i].data));
		next = f->next;
		free_page((unsigned long)f);
		f = next;
	}
}

static int rtas_flash_release(struct inode *inode, struct file *file)
{
	struct proc_dir_entry *dp = file->f_dentry->d_inode->u.generic_ip;
	struct rtas_update_flash_t *uf;

	uf = (struct rtas_update_flash_t *) dp->data;
	if (uf->flist) {
		/* File was opened in write mode for a new flash attempt */
		/* Clear saved list */
		if (rtas_firmware_flash_list.next) {
			free_flash_list(rtas_firmware_flash_list.next);
			rtas_firmware_flash_list.next = NULL;
		}

		if (uf->status != FLASH_AUTH)
			uf->status = flash_list_valid(uf->flist);

		if (uf->status == FLASH_IMG_READY)
			rtas_firmware_flash_list.next = uf->flist;
		else
			free_flash_list(uf->flist);

		uf->flist = NULL;
	}

	atomic_dec(&dp->count);
	return 0;
}

static void get_flash_status_msg(int status, char *buf)
{
	char *msg;

	switch (status) {
	case FLASH_AUTH:
		msg = "error: this partition does not have service authority\n";
		break;
	case FLASH_NO_OP:
		msg = "info: no firmware image for flash\n";
		break;
	case FLASH_IMG_SHORT:
		msg = "error: flash image short\n";
		break;
	case FLASH_IMG_BAD_LEN:
		msg = "error: internal error bad length\n";
		break;
	case FLASH_IMG_NULL_DATA:
		msg = "error: internal error null data\n";
		break;
	case FLASH_IMG_READY:
		msg = "ready: firmware image ready for flash on reboot\n";
		break;
	default:
		sprintf(buf, "error: unexpected status value %d\n", status);
		return;
	}

	strcpy(buf, msg);
}

/* Reading the proc file will show status (not the firmware contents) */
static ssize_t rtas_flash_read(struct file *file, char *buf,
			       size_t count, loff_t *ppos)
{
	struct proc_dir_entry *dp = file->f_dentry->d_inode->u.generic_ip;
	struct rtas_update_flash_t *uf;
	char msg[RTAS_MSG_MAXLEN];
	int error;
	int msglen;

	uf = (struct rtas_update_flash_t *) dp->data;

	if (!strcmp(dp->name, FIRMWARE_FLASH_NAME)) {
		get_flash_status_msg(uf->status, msg);
	} else {	   /* FIRMWARE_UPDATE_NAME */
		sprintf(msg, "%d\n", uf->status);
	}
	msglen = strlen(msg);
	if (msglen > count)
		msglen = count;

	if (ppos && *ppos != 0)
		return 0;	/* be cheap */

	error = verify_area(VERIFY_WRITE, buf, msglen);
	if (error)
		return -EINVAL;

	if (copy_to_user(buf, msg, msglen))
		return -EFAULT;

	if (ppos)
		*ppos = msglen;
	return msglen;
}

/* We could be much more efficient here.  But to keep this function
 * simple we allocate a page to the block list no matter how small the
 * count is.  If the system is low on memory it will be just as well
 * that we fail....
 */
static ssize_t rtas_flash_write(struct file *file, const char *buffer,
				size_t count, loff_t *off)
{
	struct proc_dir_entry *dp = file->f_dentry->d_inode->u.generic_ip;
	struct rtas_update_flash_t *uf;
	char *p;
	int next_free;
	struct flash_block_list *fl;

	uf = (struct rtas_update_flash_t *) dp->data;

	if (uf->status == FLASH_AUTH || count == 0)
		return count;	/* discard data */

	/* In the case that the image is not ready for flashing, the memory
	 * allocated for the block list will be freed upon the release of the
	 * proc file
	 */
	if (uf->flist == NULL) {
		uf->flist = (struct flash_block_list *) get_free_page(GFP_KERNEL);
		if (!uf->flist)
			return -ENOMEM;
	}

	fl = uf->flist;
	while (fl->next)
		fl = fl->next; /* seek to last block_list for append */
	next_free = fl->num_blocks;
	if (next_free == FLASH_BLOCKS_PER_NODE) {
		/* Need to allocate another block_list */
		fl->next = (struct flash_block_list *)get_free_page(GFP_KERNEL);
		if (!fl->next)
			return -ENOMEM;
		fl = fl->next;
		next_free = 0;
	}

	if (count > PAGE_SIZE)
		count = PAGE_SIZE;
	p = (char *)get_free_page(GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	if(copy_from_user(p, buffer, count)) {
		free_page((unsigned long)p);
		return -EFAULT;
	}
	fl->blocks[next_free].data = p;
	fl->blocks[next_free].length = count;
	fl->num_blocks++;

	return count;
}

static int rtas_excl_open(struct inode *inode, struct file *file)
{
	struct proc_dir_entry *dp = file->f_dentry->d_inode->u.generic_ip;

	/* Enforce exclusive open with use count of PDE */
	spin_lock(&flash_file_open_lock);
	if (atomic_read(&dp->count) > 1) {
		spin_unlock(&flash_file_open_lock);
		return -EBUSY;
	}

	atomic_inc(&dp->count);
	spin_unlock(&flash_file_open_lock);

	return 0;
}

static int rtas_excl_release(struct inode *inode, struct file *file)
{
	struct proc_dir_entry *dp = file->f_dentry->d_inode->u.generic_ip;

	atomic_dec(&dp->count);

	return 0;
}

static void manage_flash(struct rtas_manage_flash_t *args_buf)
{
	unsigned int wait_time;
	s32 rc;

	while (1) {
		rc = (s32) rtas_call(rtas_token("ibm,manage-flash-image"), 1,
				1, NULL, (long) args_buf->op);
		if (rc == RTAS_RC_BUSY)
			udelay(1);
		else if (rtas_is_extended_busy(rc)) {
			wait_time = rtas_extended_busy_delay_time(rc);
			udelay(wait_time * 1000);
		} else
			break;
	}

	args_buf->status = rc;
}

static ssize_t manage_flash_read(struct file *file, char *buf,
			       size_t count, loff_t *ppos)
{
	struct proc_dir_entry *dp = file->f_dentry->d_inode->u.generic_ip;
	struct rtas_manage_flash_t *args_buf;
	char msg[RTAS_MSG_MAXLEN];
	int msglen;
	int error;

	args_buf = (struct rtas_manage_flash_t *) dp->data;
	if (args_buf == NULL)
		return 0;

	msglen = sprintf(msg, "%d\n", args_buf->status);
	if (msglen > count)
		msglen = count;

	if (ppos && *ppos != 0)
		return 0;	/* be cheap */

	error = verify_area(VERIFY_WRITE, buf, msglen);
	if (error)
		return -EINVAL;

	if (copy_to_user(buf, msg, msglen))
		return -EFAULT;

	if (ppos)
		*ppos = msglen;
	return msglen;
}

static ssize_t manage_flash_write(struct file *file, const char *buf,
				size_t count, loff_t *off)
{
	struct proc_dir_entry *dp = file->f_dentry->d_inode->u.generic_ip;
	struct rtas_manage_flash_t *args_buf;
	const char reject_str[] = "0";
	const char commit_str[] = "1";
	char stkbuf[10];
	int op;

	args_buf = (struct rtas_manage_flash_t *) dp->data;
	if ((args_buf->status == MANAGE_AUTH) || (count == 0))
		return count;

	op = -1;
	if (buf) {
		if (count > 9) count = 9;
		if (copy_from_user (stkbuf, buf, count)) {
			return -EFAULT;
		}
		if (strncmp(stkbuf, reject_str, strlen(reject_str)) == 0)
			op = RTAS_REJECT_TMP_IMG;
		else if (strncmp(stkbuf, commit_str, strlen(commit_str)) == 0)
			op = RTAS_COMMIT_TMP_IMG;
	}

	if (op == -1)   /* buf is empty, or contains invalid string */
		return -EINVAL;

	args_buf->op = op;
	manage_flash(args_buf);

	return count;
}

static void validate_flash(struct rtas_validate_flash_t *args_buf)
{
	int token = rtas_token("ibm,validate-flash-image");
	unsigned int wait_time;
	long update_results;
	s32 rc;

	rc = 0;
	while(1) {
		spin_lock(&rtas_data_buf_lock);
		memcpy(rtas_data_buf, args_buf->buf, VALIDATE_BUF_SIZE);
		rc = (s32) rtas_call(token, 2, 2, &update_results,
				     __pa(rtas_data_buf), args_buf->buf_size);
		memcpy(args_buf->buf, rtas_data_buf, VALIDATE_BUF_SIZE);
		spin_unlock(&rtas_data_buf_lock);

		if (rc == RTAS_RC_BUSY)
			udelay(1);
		else if (rtas_is_extended_busy(rc)) {
			wait_time = rtas_extended_busy_delay_time(rc);
			udelay(wait_time * 1000);
		} else
			break;
	}

	args_buf->status = rc;
	args_buf->update_results = (u32) update_results;
}

static int get_validate_flash_msg(struct rtas_validate_flash_t *args_buf,
				   char *msg)
{
	int n;

	if (args_buf->status >= VALIDATE_TMP_UPDATE) {
		n = sprintf(msg, "%d\n", args_buf->update_results);
		if ((args_buf->update_results >= VALIDATE_CUR_UNKNOWN) ||
		    (args_buf->update_results == VALIDATE_TMP_UPDATE))
			n += sprintf(msg + n, "%s\n", args_buf->buf);
	} else {
		n = sprintf(msg, "%d\n", args_buf->status);
	}
	return n;
}

static ssize_t validate_flash_read(struct file *file, char *buf,
			       size_t count, loff_t *ppos)
{
	struct proc_dir_entry *dp = file->f_dentry->d_inode->u.generic_ip;
	struct rtas_validate_flash_t *args_buf;
	char msg[RTAS_MSG_MAXLEN];
	int msglen;
	int error;

	args_buf = (struct rtas_validate_flash_t *) dp->data;

	if (ppos && *ppos != 0)
		return 0;	/* be cheap */

	msglen = get_validate_flash_msg(args_buf, msg);
	if (msglen > count)
		msglen = count;

	error = verify_area(VERIFY_WRITE, buf, msglen);
	if (error)
		return -EINVAL;

	if (copy_to_user(buf, msg, msglen))
		return -EFAULT;

	if (ppos)
		*ppos = msglen;
	return msglen;
}

static ssize_t validate_flash_write(struct file *file, const char *buf,
				size_t count, loff_t *off)
{
	struct proc_dir_entry *dp = file->f_dentry->d_inode->u.generic_ip;
	struct rtas_validate_flash_t *args_buf;
	int rc;

	args_buf = (struct rtas_validate_flash_t *) dp->data;

	if (dp->data == NULL) {
		dp->data = kmalloc(sizeof(struct rtas_validate_flash_t),
				GFP_KERNEL);
		if (dp->data == NULL)
			return -ENOMEM;
	}

	/* We are only interested in the first 4K of the
	 * candidate image */
	if ((*off >= VALIDATE_BUF_SIZE) ||
		(args_buf->status == VALIDATE_AUTH)) {
		*off += count;
		return count;
	}

	if (*off + count >= VALIDATE_BUF_SIZE)  {
		count = VALIDATE_BUF_SIZE - *off;
		args_buf->status = VALIDATE_READY;
	} else {
		args_buf->status = VALIDATE_INCOMPLETE;
	}

	if (verify_area(VERIFY_READ, buf, count)) {
		rc = -EFAULT;
		goto done;
	}
	if (copy_from_user(args_buf->buf + *off, buf, count)) {
		rc = -EFAULT;
		goto done;
	}

	*off += count;
	rc = count;
done:
	if (rc < 0) {
		kfree(dp->data);
		dp->data = NULL;
	}
	return rc;
}

static int validate_flash_release(struct inode *inode, struct file *file)
{
	struct proc_dir_entry *dp = file->f_dentry->d_inode->u.generic_ip;
	struct rtas_validate_flash_t *args_buf;

	args_buf = (struct rtas_validate_flash_t *) dp->data;

	if (args_buf->status == VALIDATE_READY) {
		args_buf->buf_size = VALIDATE_BUF_SIZE;
		validate_flash(args_buf);
	}

	atomic_dec(&dp->count);

	return 0;
}

static inline void remove_flash_pde(struct proc_dir_entry *dp)
{
	if (dp) {
		if (dp->data != NULL)
			kfree(dp->data);
		remove_proc_entry(dp->name, rtas_proc_dir);
	}
}

static inline int initialize_flash_pde_data(const char *rtas_call_name,
					    size_t buf_size,
					    struct proc_dir_entry *dp)
{
	int *status;
	int token;

	dp->data = kmalloc(buf_size, GFP_KERNEL);
	if (dp->data == NULL) {
		remove_flash_pde(dp);
		return -ENOMEM;
	}

	memset(dp->data, 0, buf_size);

	/* This code assumes that the status int is the first member of the
	 * struct
	 */
	status = (int *) dp->data;
	token = rtas_token(rtas_call_name);
	if (token == RTAS_UNKNOWN_SERVICE)
		*status = FLASH_AUTH;
	else
		*status = FLASH_NO_OP;

	return 0;
}

static inline struct proc_dir_entry * create_flash_pde(const char *filename,
					struct file_operations *fops)
{
	struct proc_dir_entry *ent = NULL;

	ent = create_proc_entry(filename, S_IRUSR | S_IWUSR, rtas_proc_dir);
	if (ent != NULL) {
		ent->nlink = 1;
		ent->proc_fops = fops;
		ent->owner = THIS_MODULE;
	}

	return ent;
}

static const struct file_operations rtas_flash_operations = {
	read:		rtas_flash_read,
	write:		rtas_flash_write,
	open:		rtas_excl_open,
	release:	rtas_flash_release,
};

static const struct file_operations manage_flash_operations = {
	read:		manage_flash_read,
	write:		manage_flash_write,
	open:		rtas_excl_open,
	release:	rtas_excl_release,
};

static const struct file_operations validate_flash_operations = {
	read:		validate_flash_read,
	write:		validate_flash_write,
	open:		rtas_excl_open,
	release:	validate_flash_release,
};

int __init rtas_flash_init(void)
{
	int rc;

	if (!rtas_proc_dir) {
		printk(KERN_WARNING "rtas proc dir does not already exist");
		return -ENOENT;
	}

	firmware_flash_pde = create_flash_pde(FIRMWARE_FLASH_NAME,
					      &rtas_flash_operations);
	rc = initialize_flash_pde_data("ibm,update-flash-64-and-reboot",
				       sizeof(struct rtas_update_flash_t),
				       firmware_flash_pde);
	if (rc != 0)
		return rc;

	firmware_update_pde = create_flash_pde(FIRMWARE_UPDATE_NAME,
					       &rtas_flash_operations);
	rc = initialize_flash_pde_data("ibm,update-flash-64-and-reboot",
				       sizeof(struct rtas_update_flash_t),
				       firmware_update_pde);
	if (rc != 0)
		return rc;

	validate_pde = create_flash_pde(VALIDATE_FLASH_NAME,
					&validate_flash_operations);
	rc = initialize_flash_pde_data("ibm,validate-flash-image",
				       sizeof(struct rtas_validate_flash_t),
				       validate_pde);
	if (rc != 0)
		return rc;

	manage_pde = create_flash_pde(MANAGE_FLASH_NAME,
				      &manage_flash_operations);
	rc = initialize_flash_pde_data("ibm,manage-flash-image",
				       sizeof(struct rtas_manage_flash_t),
				       manage_pde);
	if (rc != 0)
		return rc;

	return 0;
}

void __exit rtas_flash_cleanup(void)
{
	if (!rtas_proc_dir)
		return;
	remove_flash_pde(firmware_flash_pde);
	remove_flash_pde(firmware_update_pde);
	remove_flash_pde(validate_pde);
	remove_flash_pde(manage_pde);
}

module_init(rtas_flash_init);
module_exit(rtas_flash_cleanup);
MODULE_LICENSE("GPL");
