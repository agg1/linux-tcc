/*
 *  linux/fs/minix/file.c
 *
 *  Copyright (C) 1991, 1992 Linus Torvalds
 *
 *  minix regular file handling primitives
 */

#include <linux/fs.h>
#include <linux/minix_fs.h>

/*
 * We have mostly NULLs here: the current defaults are OK for
 * the minix filesystem.
 */
int minix_sync_file(struct file *, struct dentry *, int);

const struct file_operations minix_file_operations = {
	llseek:		generic_file_llseek,
	read:		generic_file_read,
	write:		generic_file_write,
	mmap:		generic_file_mmap,
	fsync:		minix_sync_file,
};

const struct inode_operations minix_file_inode_operations = {
	truncate:	minix_truncate,
};

int minix_sync_file(struct file * file, struct dentry *dentry, int datasync)
{
	struct inode *inode = dentry->d_inode;
	int err;

	err = fsync_inode_buffers(inode);
	err |= fsync_inode_data_buffers(inode);
	if (!(inode->i_state & I_DIRTY))
		return err;
	if (datasync && !(inode->i_state & I_DIRTY_DATASYNC))
		return err;
	
	err |= minix_sync_inode(inode);
	return err ? -EIO : 0;
}
