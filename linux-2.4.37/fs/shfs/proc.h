#ifndef _PROC_H
#define _PROC_H

#include <linux/shfs_fs_sb.h>

static inline int
sock_lock(struct shfs_sb_info *info)
{
	int result;
	DEBUG("?\n");
	result = down_interruptible(&(info->sock_sem));
	DEBUG("!\n");
	return (result != -EINTR);
}

static inline void
sock_unlock(struct shfs_sb_info *info)
{
	up(&(info->sock_sem));
	DEBUG("\n");
}

#endif	/* _PROC_H */
