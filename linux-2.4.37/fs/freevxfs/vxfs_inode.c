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

#ident "$Id: vxfs_inode.c,v 1.42 2002/01/02 23:51:36 hch Exp hch $"

/*
 * Veritas filesystem driver - inode routines.
 */
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/kernel.h>
#include <linux/slab.h>

#include "vxfs.h"
#include "vxfs_inode.h"
#include "vxfs_extern.h"


extern const struct address_space_operations vxfs_aops;
extern const struct address_space_operations vxfs_immed_aops;

extern const struct inode_operations vxfs_immed_symlink_iops;

static const struct file_operations vxfs_file_operations = {
	.open =			generic_file_open,
	.llseek =		generic_file_llseek,
	.read =			generic_file_read,
	.mmap =			generic_file_mmap,
};


kmem_cache_t		*vxfs_inode_cachep;


#ifdef DIAGNOSTIC
/*
 * Dump inode contents (partially).
 */
void
vxfs_dumpi(struct vxfs_inode_info *vip, ino_t ino)
{
	printk(KERN_DEBUG "\n\n");
	if (ino)
		printk(KERN_DEBUG "dumping vxfs inode %ld\n", ino);
	else
		printk(KERN_DEBUG "dumping unknown vxfs inode\n");

	printk(KERN_DEBUG "---------------------------\n");
	printk(KERN_DEBUG "mode is %x\n", vip->vii_mode);
	printk(KERN_DEBUG "nlink:%u, uid:%u, gid:%u\n",
			vip->vii_nlink, vip->vii_uid, vip->vii_gid);
	printk(KERN_DEBUG "size:%Lx, blocks:%u\n",
			vip->vii_size, vip->vii_blocks);
	printk(KERN_DEBUG "orgtype:%u\n", vip->vii_orgtype);
}
#endif


/**
 * vxfs_blkiget - find inode based on extent #
 * @sbp:	superblock of the filesystem we search in
 * @extent:	number of the extent to search
 * @ino:	inode number to search
 *
 * Description:
 *  vxfs_blkiget searches inode @ino in the filesystem described by
 *  @sbp in the extent @extent.
 *  Returns the matching VxFS inode on success, else a NULL pointer.
 *
 * NOTE:
 *  While __vxfs_iget uses the pagecache vxfs_blkiget uses the
 *  buffercache.  This function should not be used outside the
 *  read_super() method, otherwise the data may be incoherent.
 */
struct vxfs_inode_info *
vxfs_blkiget(struct super_block *sbp, u_long extent, ino_t ino)
{
	struct buffer_head		*bp;
	u_long				block, offset;

	block = extent + ((ino * VXFS_ISIZE) / sbp->s_blocksize);
	offset = ((ino % (sbp->s_blocksize / VXFS_ISIZE)) * VXFS_ISIZE);
	bp = sb_bread(sbp, block);

	if (buffer_mapped(bp)) {
		struct vxfs_inode_info	*vip;
		struct vxfs_dinode	*dip;

		if (!(vip = kmem_cache_alloc(vxfs_inode_cachep, SLAB_KERNEL)))
			goto fail;
		dip = (struct vxfs_dinode *)(bp->b_data + offset);
		memcpy(vip, dip, sizeof(*vip));
#ifdef DIAGNOSTIC
		vxfs_dumpi(vip, ino);
#endif
		brelse(bp);
		return (vip);
	}

fail:
	printk(KERN_WARNING "vxfs: unable to read block %ld\n", block);
	brelse(bp);
	return NULL;
}

/**
 * __vxfs_iget - generic find inode facility
 * @sbp:		VFS superblock
 * @ino:		inode number
 * @ilistp:		inode list
 *
 * Description:
 *  Search the for inode number @ino in the filesystem
 *  described by @sbp.  Use the specified inode table (@ilistp).
 *  Returns the matching VxFS inode on success, else a NULL pointer.
 */
static struct vxfs_inode_info *
__vxfs_iget(ino_t ino, struct inode *ilistp)
{
	struct page			*pp;
	u_long				offset;

	offset = (ino % (PAGE_SIZE / VXFS_ISIZE)) * VXFS_ISIZE;
	pp = vxfs_get_page(ilistp->i_mapping, ino * VXFS_ISIZE / PAGE_SIZE);

	if (!IS_ERR(pp)) {
		struct vxfs_inode_info	*vip;
		struct vxfs_dinode	*dip;
		caddr_t			kaddr = (char *)page_address(pp);

		if (!(vip = kmem_cache_alloc(vxfs_inode_cachep, SLAB_KERNEL)))
			goto fail;
		dip = (struct vxfs_dinode *)(kaddr + offset);
		memcpy(vip, dip, sizeof(*vip));
#ifdef DIAGNOSTIC
		vxfs_dumpi(vip, ino);
#endif
		vxfs_put_page(pp);
		return (vip);
	}

	printk(KERN_WARNING "vxfs: error on page %p\n", pp);
	return NULL;

fail:
	printk(KERN_WARNING "vxfs: unable to read inode %ld\n", ino);
	vxfs_put_page(pp);
	return NULL;
}

/**
 * vxfs_stiget - find inode using the structural inode list
 * @sbp:	VFS superblock
 * @ino:	inode #
 *
 * Description:
 *  Find inode @ino in the filesystem described by @sbp using
 *  the structural inode list.
 *  Returns the matching VxFS inode on success, else a NULL pointer.
 */
struct vxfs_inode_info *
vxfs_stiget(struct super_block *sbp, ino_t ino)
{
        return __vxfs_iget(ino, VXFS_SBI(sbp)->vsi_stilist);
}

/**
 * vxfs_transmod - mode for a VxFS inode
 * @vip:	VxFS inode
 *
 * Description:
 *  vxfs_transmod returns a Linux mode_t for a given
 *  VxFS inode structure.
 */
static __inline__ mode_t
vxfs_transmod(struct vxfs_inode_info *vip)
{
	mode_t			ret = vip->vii_mode & ~VXFS_TYPE_MASK;

	if (VXFS_ISFIFO(vip))
		ret |= S_IFIFO;
	if (VXFS_ISCHR(vip))
		ret |= S_IFCHR;
	if (VXFS_ISDIR(vip))
		ret |= S_IFDIR;
	if (VXFS_ISBLK(vip))
		ret |= S_IFBLK;
	if (VXFS_ISLNK(vip))
		ret |= S_IFLNK;
	if (VXFS_ISREG(vip))
		ret |= S_IFREG;
	if (VXFS_ISSOC(vip))
		ret |= S_IFSOCK;

	return (ret);
}

/**
 * vxfs_iinit- helper to fill inode fields
 * @ip:		VFS inode
 * @vip:	VxFS inode
 *
 * Description:
 *  vxfs_instino is a helper function to fill in all relevant
 *  fields in @ip from @vip.
 */
static void
vxfs_iinit(struct inode *ip, struct vxfs_inode_info *vip)
{

	ip->i_mode = vxfs_transmod(vip);
	ip->i_uid = (uid_t)vip->vii_uid;
	ip->i_gid = (gid_t)vip->vii_gid;

	ip->i_nlink = vip->vii_nlink;
	ip->i_size = vip->vii_size;

	ip->i_atime = vip->vii_atime;
	ip->i_ctime = vip->vii_ctime;
	ip->i_mtime = vip->vii_mtime;

	ip->i_blksize = PAGE_SIZE;
	ip->i_blocks = vip->vii_blocks;
	ip->i_generation = vip->vii_gen;

	ip->u.generic_ip = (void *)vip;
	
}

/**
 * vxfs_get_fake_inode - get fake inode structure
 * @sbp:		filesystem superblock
 * @vip:		fspriv inode
 *
 * Description:
 *  vxfs_fake_inode gets a fake inode (not in the inode hash) for a
 *  superblock, vxfs_inode pair.
 *  Returns the filled VFS inode.
 */
struct inode *
vxfs_get_fake_inode(struct super_block *sbp, struct vxfs_inode_info *vip)
{
	struct inode			*ip = NULL;

	if ((ip = new_inode(sbp))) {
		vxfs_iinit(ip, vip);
		ip->i_mapping->a_ops = &vxfs_aops;
	}
	return (ip);
}

/**
 * vxfs_put_fake_inode - free faked inode
 * *ip:			VFS inode
 *
 * Description:
 *  vxfs_put_fake_inode frees all data asssociated with @ip.
 */
void
vxfs_put_fake_inode(struct inode *ip)
{
	iput(ip);
}

/**
 * vxfs_read_inode - fill in inode information
 * @ip:		inode pointer to fill
 *
 * Description:
 *  vxfs_read_inode reads the disk inode for @ip and fills
 *  in all relevant fields in @ip.
 */
void
vxfs_read_inode(struct inode *ip)
{
	struct super_block		*sbp = ip->i_sb;
	struct vxfs_inode_info		*vip;
	const struct address_space_operations	*aops;
	ino_t				ino = ip->i_ino;

	if (!(vip = __vxfs_iget(ino, VXFS_SBI(sbp)->vsi_ilist)))
		return;

	vxfs_iinit(ip, vip);

	if (VXFS_ISIMMED(vip))
		aops = &vxfs_immed_aops;
	else
		aops = &vxfs_aops;

	if (S_ISREG(ip->i_mode)) {
		ip->i_fop = &vxfs_file_operations;
		ip->i_mapping->a_ops = aops;
	} else if (S_ISDIR(ip->i_mode)) {
		ip->i_op = &vxfs_dir_inode_ops;
		ip->i_fop = &vxfs_dir_operations;
		ip->i_mapping->a_ops = aops;
	} else if (S_ISLNK(ip->i_mode)) {
		if (!VXFS_ISIMMED(vip)) {
			ip->i_op = &page_symlink_inode_operations;
			ip->i_mapping->a_ops = &vxfs_aops;
		} else
			ip->i_op = &vxfs_immed_symlink_iops;
	} else
		init_special_inode(ip, ip->i_mode, vip->vii_rdev);

	return;
}

/**
 * vxfs_put_inode - remove inode from main memory
 * @ip:		inode to discard.
 *
 * Description:
 *  vxfs_put_inode() is called on each iput.  If we are the last
 *  link in memory, free the fspriv inode area.
 */
void
vxfs_put_inode(struct inode *ip)
{
	if (atomic_read(&ip->i_count) == 1)
		kmem_cache_free(vxfs_inode_cachep, ip->u.generic_ip);
}
