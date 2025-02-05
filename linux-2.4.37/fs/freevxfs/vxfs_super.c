/*
 * Copyright (c) 2000-2001 Christoph Hellwig.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification.
 * 2. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL").
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ident "$Id: vxfs_super.c,v 1.29 2002/01/02 22:02:12 hch Exp hch $"

/*
 * Veritas filesystem driver - superblock related routines.
 */
#include <linux/init.h>
#include <linux/module.h>

#include <linux/blkdev.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/stat.h>

#include "vxfs.h"
#include "vxfs_extern.h"
#include "vxfs_dir.h"
#include "vxfs_inode.h"


MODULE_AUTHOR("Christoph Hellwig");
MODULE_DESCRIPTION("Veritas Filesystem (VxFS) driver");
MODULE_LICENSE("Dual BSD/GPL");


static void		vxfs_put_super(struct super_block *);
static int		vxfs_statfs(struct super_block *, struct statfs *);

static const struct super_operations vxfs_super_ops = {
	.read_inode =		vxfs_read_inode,
	.put_inode =		vxfs_put_inode,
	.put_super =		vxfs_put_super,
	.statfs =		vxfs_statfs,
};

/**
 * vxfs_put_super - free superblock resources
 * @sbp:	VFS superblock.
 *
 * Description:
 *   vxfs_put_super frees all resources allocated for @sbp
 *   after the last instance of the filesystem is unmounted.
 */

static void
vxfs_put_super(struct super_block *sbp)
{
	struct vxfs_sb_info	*infp = VXFS_SBI(sbp);

	vxfs_put_fake_inode(infp->vsi_fship);
	vxfs_put_fake_inode(infp->vsi_ilist);
	vxfs_put_fake_inode(infp->vsi_stilist);

	brelse(infp->vsi_bp);
	kfree(infp);
}

/**
 * vxfs_statfs - get filesystem information
 * @sbp:	VFS superblock
 * @bufp:	output buffer
 *
 * Description:
 *   vxfs_statfs fills the statfs buffer @bufp with information
 *   about the filesystem described by @sbp.
 *
 * Returns:
 *   Zero.
 *
 * Locking:
 *   We are under bkl and @sbp->s_lock.
 *
 * Notes:
 *   This is everything but complete...
 */
static int
vxfs_statfs(struct super_block *sbp, struct statfs *bufp)
{
	struct vxfs_sb_info		*infp = VXFS_SBI(sbp);

	bufp->f_type = VXFS_SUPER_MAGIC;
	bufp->f_bsize = sbp->s_blocksize;
	bufp->f_blocks = infp->vsi_raw->vs_dsize;
	bufp->f_bfree = infp->vsi_raw->vs_free;
	bufp->f_bavail = 0;
	bufp->f_files = 0;
	bufp->f_ffree = infp->vsi_raw->vs_ifree;
	bufp->f_namelen = VXFS_NAMELEN;

	return 0;
}

/**
 * vxfs_read_super - read superblock into memory and initalize filesystem
 * @sbp:		VFS superblock (to fill)
 * @dp:			fs private mount data
 * @silent:		do not complain loudly when sth is wrong
 *
 * Description:
 *   We are called on the first mount of a filesystem to read the
 *   superblock into memory and do some basic setup.
 *
 * Returns:
 *   The superblock on success, else %NULL.
 *
 * Locking:
 *   We are under the bkl and @sbp->s_lock.
 */
static struct super_block *
vxfs_read_super(struct super_block *sbp, void *dp, int silent)
{
	struct vxfs_sb_info	*infp;
	struct vxfs_sb		*rsbp;
	struct buffer_head	*bp = NULL;
	u_long			bsize;

	infp = kmalloc(sizeof(*infp), GFP_KERNEL);
	if (!infp) {
		printk(KERN_WARNING "vxfs: unable to allocate incore superblock\n");
		return NULL;
	}
	memset(infp, 0, sizeof(*infp));

	bsize = sb_min_blocksize(sbp, BLOCK_SIZE);
	if (!bsize) {
		printk(KERN_WARNING "vxfs: unable to set blocksize\n");
		goto out;
	}

	bp = sb_bread(sbp, 1);
	if (!bp || !buffer_mapped(bp)) {
		if (!silent) {
			printk(KERN_WARNING
				"vxfs: unable to read disk superblock\n");
		}
		goto out;
	}

	rsbp = (struct vxfs_sb *)bp->b_data;
	if (rsbp->vs_magic != VXFS_SUPER_MAGIC) {
		if (!silent)
			printk(KERN_NOTICE "vxfs: WRONG superblock magic\n");
		goto out;
	}

	if ((rsbp->vs_version < 2 || rsbp->vs_version > 4) && !silent) {
		printk(KERN_NOTICE "vxfs: unsupported VxFS version (%d)\n",
		       rsbp->vs_version);
		goto out;
	}

#ifdef DIAGNOSTIC
	printk(KERN_DEBUG "vxfs: supported VxFS version (%d)\n", rsbp->vs_version);
	printk(KERN_DEBUG "vxfs: blocksize: %d\n", rsbp->vs_bsize);
#endif

	sbp->s_magic = rsbp->vs_magic;
	sbp->u.generic_sbp = (void *)infp;

	infp->vsi_raw = rsbp;
	infp->vsi_bp = bp;
	infp->vsi_oltext = rsbp->vs_oltext[0];
	infp->vsi_oltsize = rsbp->vs_oltsize;

	if (!sb_set_blocksize(sbp, rsbp->vs_bsize)) {
		printk(KERN_WARNING "vxfs: unable to set final block size\n");
		goto out;
	}

	if (vxfs_read_olt(sbp, bsize)) {
		printk(KERN_WARNING "vxfs: unable to read olt\n");
		goto out;
	}

	if (vxfs_read_fshead(sbp)) {
		printk(KERN_WARNING "vxfs: unable to read fshead\n");
		goto out;
	}

	sbp->s_op = &vxfs_super_ops;
	sbp->s_root = d_alloc_root(iget(sbp, VXFS_ROOT_INO));
	if (!sbp->s_root) {
		printk(KERN_WARNING "vxfs: unable to get root dentry.\n");
		goto out_free_ilist;
	}

	return (sbp);
	
out_free_ilist:
	vxfs_put_fake_inode(infp->vsi_fship);
	vxfs_put_fake_inode(infp->vsi_ilist);
	vxfs_put_fake_inode(infp->vsi_stilist);
out:
	brelse(bp);
	kfree(infp);
	return NULL;
}

/*
 * The usual module blurb.
 */
static DECLARE_FSTYPE_DEV(vxfs_fs_type, "vxfs", vxfs_read_super);

static int __init
vxfs_init(void)
{
	vxfs_inode_cachep = kmem_cache_create("vxfs_inode",
			sizeof(struct vxfs_inode_info), 0, 0, NULL, NULL);
	if (vxfs_inode_cachep)
		return (register_filesystem(&vxfs_fs_type));
	return -ENOMEM;
}

static void __exit
vxfs_cleanup(void)
{
	unregister_filesystem(&vxfs_fs_type);
	kmem_cache_destroy(vxfs_inode_cachep);
}

module_init(vxfs_init);
module_exit(vxfs_cleanup);
