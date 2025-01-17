/*
 * ioctl.c
 *
 * ioctl() call for persistent connection support. 
 */

#ifdef MODVERSIONS
#include <linux/modversions.h>
#endif

#include <linux/kernel.h>
#include <linux/module.h>
#include <asm/uaccess.h>
#include <asm/fcntl.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/errno.h>
#include <linux/ioctl.h>

#include <linux/shfs_fs.h>
#include <linux/shfs_fs_sb.h>
#include "shfs_debug.h"

int
shfs_ioctl(struct inode *inode, struct file *f,
	   unsigned int cmd, unsigned long arg)
{
	struct shfs_sb_info *info = info_from_inode(inode);
	int result = -EINVAL;

	switch (cmd) {
	case SHFS_IOC_NEWCONN:
		VERBOSE("Reconnect (%ld)\n", arg);
		if (info->sock)
			fput(info->sock);
		info->sock = fget(arg);
		info->garbage = 0;
		info->garbage_read = 0;
		info->garbage_write = 0;
		VERBOSE(">%p\n", info->sock);
		result = 0;
		break;
	default:
		break;
	}

	return result;
}
