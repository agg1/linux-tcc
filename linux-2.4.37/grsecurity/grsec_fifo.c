#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/grinternal.h>

int
gr_handle_fifo(const struct dentry *dentry, const struct vfsmount *mnt,
	       const struct dentry *dir, const int flag, const int acc_mode)
{
#ifdef CONFIG_GRKERNSEC_FIFO
	if (grsec_enable_fifo && S_ISFIFO(dentry->d_inode->i_mode) &&
	    !(flag & O_EXCL) && (dir->d_inode->i_mode & S_ISVTX) &&
	    (dentry->d_inode->i_uid != dir->d_inode->i_uid) &&
	    (current->fsuid != dentry->d_inode->i_uid)) {
		if (!permission(dentry->d_inode, acc_mode))
			gr_log_fs_int2(GR_DONT_AUDIT, GR_FIFO_MSG, dentry, mnt, dentry->d_inode->i_uid, dentry->d_inode->i_gid);
		return -EACCES;
	}
#endif
	return 0;
}
