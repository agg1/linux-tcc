/*
 *  shfs_fs_i.h
 */

#ifndef _SHFS_FS_I_H
#define _SHFS_FS_I_H

#ifdef __KERNEL__
#include <linux/types.h>

struct shfs_file;

struct shfs_inode_info {
	unsigned long oldmtime;		/* last time refreshed */
	int unset_write_on_close;	/* created ro, opened for write */
	struct shfs_file *cache;	/* readahead cache */
};

#endif

#endif
