/*
 * Copyright (c) 2000-2004 Silicon Graphics, Inc.  All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * Further, this software is distributed without any warranty that it is
 * free of the rightful claim of any third person regarding infringement
 * or the like.  Any license provided herein, whether implied or
 * otherwise, applies only to this software file.  Patent licenses, if
 * any, provided herein do not apply to combinations of this program with
 * other software, or any other product whatsoever.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write the Free Software Foundation, Inc., 59
 * Temple Place - Suite 330, Boston MA 02111-1307, USA.
 *
 * Contact information: Silicon Graphics, Inc., 1600 Amphitheatre Pkwy,
 * Mountain View, CA  94043, or:
 *
 * http://www.sgi.com
 *
 * For further information regarding this notice, see:
 *
 * http://oss.sgi.com/projects/GenInfo/SGIGPLNoticeExplan/
 */

#include "xfs.h"
#include "xfs_inum.h"
#include "xfs_log.h"
#include "xfs_sb.h"
#include "xfs_dir.h"
#include "xfs_dir2.h"
#include "xfs_trans.h"
#include "xfs_dmapi.h"
#include "xfs_mount.h"
#include "xfs_bmap_btree.h"
#include "xfs_alloc_btree.h"
#include "xfs_ialloc_btree.h"
#include "xfs_alloc.h"
#include "xfs_btree.h"
#include "xfs_attr_sf.h"
#include "xfs_dir_sf.h"
#include "xfs_dir2_sf.h"
#include "xfs_dinode.h"
#include "xfs_inode.h"
#include "xfs_error.h"
#include "xfs_rw.h"
#include <linux/iobuf.h>

#include <linux/dcache.h>
#include <linux/smp_lock.h>
#include <linux/mman.h> /* for PROT_WRITE */

static const struct vm_operations_struct linvfs_file_vm_ops;

STATIC inline ssize_t
__linvfs_read(
	struct file	*file,
	char		*buf,
	int		ioflags,
	size_t		size,
	loff_t		*offset)
{
	struct inode	*inode = file->f_dentry->d_inode;
	vnode_t		*vp = LINVFS_GET_VP(inode);
	ssize_t		rval;

	if (unlikely(file->f_flags & O_DIRECT)) {
		ioflags |= IO_ISDIRECT;
		down_read(&inode->i_alloc_sem);
		VOP_READ(vp, file, buf, size, offset, ioflags, NULL, rval);
		up_read(&inode->i_alloc_sem);
	} else {
		VOP_READ(vp, file, buf, size, offset, ioflags, NULL, rval);
	}

	return rval;
}

STATIC ssize_t
linvfs_read(
	struct file	*file,
	char		*buf,
	size_t		size,
	loff_t		*offset)
{
	return __linvfs_read(file, buf, 0, size, offset);
}

STATIC ssize_t
linvfs_read_invis(
	struct file	*file,
	char		*buf,
	size_t		size,
	loff_t		*offset)
{
	return __linvfs_read(file, buf, IO_INVIS, size, offset);
}


STATIC inline ssize_t
__linvfs_write(
	struct file	*file,
	const char	*buf,
	int		ioflags,
	size_t		count,
	loff_t		*ppos)
{
	struct inode	*inode = file->f_dentry->d_inode;
	vnode_t		*vp = LINVFS_GET_VP(inode);
	loff_t		pos;
	ssize_t		rval;	/* Use negative errors in this f'n */

	if ((ssize_t) count < 0)
		return -EINVAL;

	if (!access_ok(VERIFY_READ, buf, count))
		return -EFAULT;

	pos = *ppos;
	if (pos < 0)
		return -EINVAL;

	rval = file->f_error;
	if (rval) {
		file->f_error = 0;
		return rval;
	}

	/* We allow multiple direct writers in, there is no
	 * potential call to vmtruncate in that path.
	 */
	if (unlikely(file->f_flags & O_DIRECT)) {
		ioflags |= IO_ISDIRECT;
		down_read(&inode->i_alloc_sem);
		VOP_WRITE(vp, file, buf, count, &pos, ioflags, NULL, rval);
		*ppos = pos;
		up_read(&inode->i_alloc_sem);
	} else {
		down(&inode->i_sem);
		VOP_WRITE(vp, file, buf, count, &pos, ioflags, NULL, rval);
		*ppos = pos;
		up(&inode->i_sem);
	}

	return rval;
}

STATIC inline ssize_t
linvfs_write(
	struct file	*file,
	const char	*buf,
	size_t		count,
	loff_t		*ppos)
{
	return __linvfs_write(file, buf, 0, count, ppos);
}

STATIC inline ssize_t
linvfs_write_invis(
	struct file	*file,
	const char	*buf,
	size_t		count,
	loff_t		*ppos)
{
	return __linvfs_write(file, buf, IO_INVIS, count, ppos);
}

STATIC int
linvfs_open(
	struct inode	*inode,
	struct file	*filp)
{
	vnode_t		*vp = LINVFS_GET_VP(inode);
	int		error;

	if (!(filp->f_flags & O_LARGEFILE) && i_size_read(inode) > MAX_NON_LFS)
		return -EFBIG;

	ASSERT(vp);
	VOP_OPEN(vp, NULL, error);
	return -error;
}


STATIC int
linvfs_release(
	struct inode	*inode,
	struct file	*filp)
{
	vnode_t		*vp = LINVFS_GET_VP(inode);
	int		error = 0;

	if (vp)
		VOP_RELEASE(vp, error);
	return -error;
}


STATIC int
linvfs_fsync(
	struct file	*filp,
	struct dentry	*dentry,
	int		datasync)
{
	struct inode	*inode = dentry->d_inode;
	vnode_t		*vp = LINVFS_GET_VP(inode);
	int		error;
	int		flags = FSYNC_WAIT;

	error = fsync_inode_data_buffers(inode);
	if (error)
		return error;

	if (datasync)
		flags |= FSYNC_DATA;

	ASSERT(vp);
	VOP_FSYNC(vp, flags, NULL, (xfs_off_t)0, (xfs_off_t)-1, error);
	return -error;
}

/*
 * linvfs_readdir maps to VOP_READDIR().
 * We need to build a uio, cred, ...
 */

#define nextdp(dp)      ((struct xfs_dirent *)((char *)(dp) + (dp)->d_reclen))

STATIC int
linvfs_readdir(
	struct file	*filp,
	void		*dirent,
	filldir_t	filldir)
{
	int		error = 0;
	vnode_t		*vp;
	uio_t		uio;
	iovec_t		iov;
	int		eof = 0;
	caddr_t		read_buf;
	int		namelen, size = 0;
	size_t		rlen = PAGE_CACHE_SIZE;
	xfs_off_t	start_offset, curr_offset;
	xfs_dirent_t	*dbp = NULL;

	vp = LINVFS_GET_VP(filp->f_dentry->d_inode);
	ASSERT(vp);

	/* Try fairly hard to get memory */
	do {
		if ((read_buf = (caddr_t)kmalloc(rlen, GFP_KERNEL)))
			break;
		rlen >>= 1;
	} while (rlen >= 1024);

	if (read_buf == NULL)
		return -ENOMEM;

	uio.uio_iov = &iov;
	uio.uio_segflg = UIO_SYSSPACE;
	curr_offset = filp->f_pos;
	if (filp->f_pos != 0x7fffffff)
		uio.uio_offset = filp->f_pos;
	else
		uio.uio_offset = 0xffffffff;

	while (!eof) {
		uio.uio_resid = iov.iov_len = rlen;
		iov.iov_base = read_buf;
		uio.uio_iovcnt = 1;

		start_offset = uio.uio_offset;

		VOP_READDIR(vp, &uio, NULL, &eof, error);
		if ((uio.uio_offset == start_offset) || error) {
			size = 0;
			break;
		}

		size = rlen - uio.uio_resid;
		dbp = (xfs_dirent_t *)read_buf;
		while (size > 0) {
			namelen = strlen(dbp->d_name);

			if (filldir(dirent, dbp->d_name, namelen,
					(loff_t) curr_offset & 0x7fffffff,
					(ino_t) dbp->d_ino,
					DT_UNKNOWN)) {
				goto done;
			}
			size -= dbp->d_reclen;
			curr_offset = (loff_t)dbp->d_off /* & 0x7fffffff */;
			dbp = nextdp(dbp);
		}
	}
done:
	if (!error) {
		if (size == 0)
			filp->f_pos = uio.uio_offset & 0x7fffffff;
		else if (dbp)
			filp->f_pos = curr_offset;
	}

	kfree(read_buf);
	return -error;
}

STATIC int
linvfs_file_mmap(
	struct file	*filp,
	struct vm_area_struct *vma)
{
	struct inode	*ip = filp->f_dentry->d_inode;
	vnode_t		*vp = LINVFS_GET_VP(ip);
	vattr_t		va = { .va_mask = XFS_AT_UPDATIME };
	int		error;

	if (vp->v_vfsp->vfs_flag & VFS_DMI) {
		xfs_mount_t	*mp = XFS_VFSTOM(vp->v_vfsp);

		error = -XFS_SEND_MMAP(mp, vma, 0);
		if (error)
			return error;
	}

	vma->vm_ops = &linvfs_file_vm_ops;

	VOP_SETATTR(vp, &va, XFS_AT_UPDATIME, NULL, error);
	if (!error)
		vn_revalidate(vp);	/* update Linux inode flags */
	return 0;
}


STATIC int
linvfs_ioctl(
	struct inode	*inode,
	struct file	*filp,
	unsigned int	cmd,
	unsigned long	arg)
{
	int		error;
	vnode_t		*vp = LINVFS_GET_VP(inode);

	unlock_kernel();
	VOP_IOCTL(vp, inode, filp, 0, cmd, arg, error);
	VMODIFY(vp);
	lock_kernel();

	/* NOTE:  some of the ioctl's return positive #'s as a
	 *	  byte count indicating success, such as
	 *	  readlink_by_handle.  So we don't "sign flip"
	 *	  like most other routines.  This means true
	 *	  errors need to be returned as a negative value.
	 */
	return error;
}

STATIC int
linvfs_ioctl_invis(
	struct inode	*inode,
	struct file	*filp,
	unsigned int	cmd,
	unsigned long	arg)
{
	int		error;
	vnode_t		*vp = LINVFS_GET_VP(inode);

	unlock_kernel();
	VOP_IOCTL(vp, inode, filp, IO_INVIS, cmd, arg, error);
	VMODIFY(vp);
	lock_kernel();

	/* NOTE:  some of the ioctl's return positive #'s as a
	 *	  byte count indicating success, such as
	 *	  readlink_by_handle.  So we don't "sign flip"
	 *	  like most other routines.  This means true
	 *	  errors need to be returned as a negative value.
	 */
	return error;
}

#ifdef HAVE_VMOP_MPROTECT
STATIC int
linvfs_mprotect(
	struct vm_area_struct *vma,
	unsigned int	newflags)
{
	vnode_t		*vp = LINVFS_GET_VP(vma->vm_file->f_dentry->d_inode);
	int		error = 0;

	if (vp->v_vfsp->vfs_flag & VFS_DMI) {
		if ((vma->vm_flags & VM_MAYSHARE) &&
		    (newflags & PROT_WRITE) && !(vma->vm_flags & PROT_WRITE)) {
			xfs_mount_t	*mp = XFS_VFSTOM(vp->v_vfsp);

			error = XFS_SEND_MMAP(mp, vma, VM_WRITE);
		    }
	}
	return error;
}
#endif /* HAVE_VMOP_MPROTECT */


const struct file_operations linvfs_file_operations = {
	.llseek		= generic_file_llseek,
	.read		= linvfs_read,
	.write		= linvfs_write,
	.ioctl		= linvfs_ioctl,
	.mmap		= linvfs_file_mmap,
	.open		= linvfs_open,
	.release	= linvfs_release,
	.fsync		= linvfs_fsync,
};

const struct file_operations linvfs_invis_file_operations = {
	.llseek		= generic_file_llseek,
	.read		= linvfs_read_invis,
	.write		= linvfs_write_invis,
	.ioctl		= linvfs_ioctl_invis,
	.mmap		= linvfs_file_mmap,
	.open		= linvfs_open,
	.release	= linvfs_release,
	.fsync		= linvfs_fsync,
};


const struct file_operations linvfs_dir_operations = {
	.read		= generic_read_dir,
	.readdir	= linvfs_readdir,
	.ioctl		= linvfs_ioctl,
	.fsync		= linvfs_fsync,
};

static const struct vm_operations_struct linvfs_file_vm_ops = {
	.nopage		= filemap_nopage,
#ifdef HAVE_VMOP_MPROTECT
	.mprotect	= linvfs_mprotect,
#endif
};
