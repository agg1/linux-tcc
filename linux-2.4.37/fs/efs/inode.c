/*
 * inode.c
 *
 * Copyright (c) 1999 Al Smith
 *
 * Portions derived from work (c) 1995,1996 Christian Vogelgsang,
 *              and from work (c) 1998 Mike Shaver.
 */

#include <linux/efs_fs.h>
#include <linux/efs_fs_sb.h>
#include <linux/module.h>


extern int efs_get_block(struct inode *, long, struct buffer_head *, int);
static int efs_readpage(struct file *file, struct page *page)
{
	return block_read_full_page(page,efs_get_block);
}
static int _efs_bmap(struct address_space *mapping, long block)
{
	return generic_block_bmap(mapping,block,efs_get_block);
}
const struct address_space_operations efs_aops = {
	readpage: efs_readpage,
	sync_page: block_sync_page,
	bmap: _efs_bmap
};

static inline void extent_copy(efs_extent *src, efs_extent *dst) {
	/*
	 * this is slightly evil. it doesn't just copy
	 * efs_extent from src to dst, it also mangles
	 * the bits so that dst ends up in cpu byte-order.
	 */

	dst->cooked.ex_magic  =  (unsigned int) src->raw[0];
	dst->cooked.ex_bn     = ((unsigned int) src->raw[1] << 16) |
				((unsigned int) src->raw[2] <<  8) |
				((unsigned int) src->raw[3] <<  0);
	dst->cooked.ex_length =  (unsigned int) src->raw[4];
	dst->cooked.ex_offset = ((unsigned int) src->raw[5] << 16) |
				((unsigned int) src->raw[6] <<  8) |
				((unsigned int) src->raw[7] <<  0);
	return;
}

void efs_read_inode(struct inode *inode) {
	int i, inode_index;
	dev_t device;
	struct buffer_head *bh;
	struct efs_sb_info    *sb = SUPER_INFO(inode->i_sb);
	struct efs_inode_info *in = INODE_INFO(inode);
	efs_block_t block, offset;
	struct efs_dinode *efs_inode;
  
	/*
	** EFS layout:
	**
	** |   cylinder group    |   cylinder group    |   cylinder group ..etc
	** |inodes|data          |inodes|data          |inodes|data       ..etc
	**
	** work out the inode block index, (considering initially that the
	** inodes are stored as consecutive blocks). then work out the block
	** number of that inode given the above layout, and finally the
	** offset of the inode within that block.
	*/

	inode_index = inode->i_ino /
		(EFS_BLOCKSIZE / sizeof(struct efs_dinode));

	block = sb->fs_start + sb->first_block + 
		(sb->group_size * (inode_index / sb->inode_blocks)) +
		(inode_index % sb->inode_blocks);

	offset = (inode->i_ino %
			(EFS_BLOCKSIZE / sizeof(struct efs_dinode))) *
		sizeof(struct efs_dinode);

	bh = sb_bread(inode->i_sb, block);
	if (!bh) {
		printk(KERN_WARNING "EFS: bread() failed at block %d\n", block);
		goto read_inode_error;
	}

	efs_inode = (struct efs_dinode *) (bh->b_data + offset);
    
	inode->i_mode  = be16_to_cpu(efs_inode->di_mode);
	inode->i_nlink = be16_to_cpu(efs_inode->di_nlink);
	inode->i_uid   = (uid_t)be16_to_cpu(efs_inode->di_uid);
	inode->i_gid   = (gid_t)be16_to_cpu(efs_inode->di_gid);
	inode->i_size  = be32_to_cpu(efs_inode->di_size);
	inode->i_atime = be32_to_cpu(efs_inode->di_atime);
	inode->i_mtime = be32_to_cpu(efs_inode->di_mtime);
	inode->i_ctime = be32_to_cpu(efs_inode->di_ctime);

	/* this is the number of blocks in the file */
	if (inode->i_size == 0) {
		inode->i_blocks = 0;
	} else {
		inode->i_blocks = ((inode->i_size - 1) >> EFS_BLOCKSIZE_BITS) + 1;
	}

	/*
	 * BUG: irix dev_t is 32-bits. linux dev_t is only 16-bits.
	 *
	 * apparently linux will change to 32-bit dev_t sometime during
	 * linux 2.3.
	 *
	 * as is, this code maps devices that can't be represented in
	 * 16-bits (ie major > 255 or minor > 255) to major = minor = 255.
	 *
	 * during 2.3 when 32-bit dev_t become available, we should test
	 * to see whether odev contains 65535. if this is the case then we
	 * should then do device = be32_to_cpu(efs_inode->di_u.di_dev.ndev).
	 */
    	device = be16_to_cpu(efs_inode->di_u.di_dev.odev);

	/* get the number of extents for this object */
	in->numextents = be16_to_cpu(efs_inode->di_numextents);
	in->lastextent = 0;

	/* copy the extents contained within the inode to memory */
	for(i = 0; i < EFS_DIRECTEXTENTS; i++) {
		extent_copy(&(efs_inode->di_u.di_extents[i]), &(in->extents[i]));
		if (i < in->numextents && in->extents[i].cooked.ex_magic != 0) {
			printk(KERN_WARNING "EFS: extent %d has bad magic number in inode %lu\n", i, inode->i_ino);
			brelse(bh);
			goto read_inode_error;
		}
	}

	brelse(bh);
   
#ifdef DEBUG
	printk(KERN_DEBUG "EFS: read_inode(): inode %lu, extents %d, mode %o\n",
		inode->i_ino, in->numextents, inode->i_mode);
#endif

	switch (inode->i_mode & S_IFMT) {
		case S_IFDIR: 
			inode->i_op = &efs_dir_inode_operations; 
			inode->i_fop = &efs_dir_operations; 
			break;
		case S_IFREG:
			inode->i_fop = &generic_ro_fops;
			inode->i_data.a_ops = &efs_aops;
			break;
		case S_IFLNK:
			inode->i_op = &page_symlink_inode_operations;
			inode->i_data.a_ops = &efs_symlink_aops;
			break;
		case S_IFCHR:
		case S_IFBLK:
		case S_IFIFO:
			init_special_inode(inode, inode->i_mode, device);
			break;
		default:
			printk(KERN_WARNING "EFS: unsupported inode mode %o\n", inode->i_mode);
			goto read_inode_error;
			break;
	}

	return;
        
read_inode_error:
	printk(KERN_WARNING "EFS: failed to read inode %lu\n", inode->i_ino);
	make_bad_inode(inode);

	return;
}

static inline efs_block_t
efs_extent_check(efs_extent *ptr, efs_block_t block, struct efs_sb_info *sb) {
	efs_block_t start;
	efs_block_t length;
	efs_block_t offset;

	/*
	 * given an extent and a logical block within a file,
	 * can this block be found within this extent ?
	 */
	start  = ptr->cooked.ex_bn;
	length = ptr->cooked.ex_length;
	offset = ptr->cooked.ex_offset;

	if ((block >= offset) && (block < offset+length)) {
		return(sb->fs_start + start + block - offset);
	} else {
		return 0;
	}
}

efs_block_t efs_map_block(struct inode *inode, efs_block_t block) {
	struct efs_sb_info    *sb = SUPER_INFO(inode->i_sb);
	struct efs_inode_info *in = INODE_INFO(inode);
	struct buffer_head    *bh = NULL;

	int cur, last, first = 1;
	int ibase, ioffset, dirext, direxts, indext, indexts;
	efs_block_t iblock, result = 0, lastblock = 0;
	efs_extent ext, *exts;

	last = in->lastextent;

	if (in->numextents <= EFS_DIRECTEXTENTS) {
		/* first check the last extent we returned */
		if ((result = efs_extent_check(&in->extents[last], block, sb)))
			return result;
    
		/* if we only have one extent then nothing can be found */
		if (in->numextents == 1) {
			printk(KERN_ERR "EFS: map_block() failed to map (1 extent)\n");
			return 0;
		}

		direxts = in->numextents;

		/*
		 * check the stored extents in the inode
		 * start with next extent and check forwards
		 */
		for(dirext = 1; dirext < direxts; dirext++) {
			cur = (last + dirext) % in->numextents;
			if ((result = efs_extent_check(&in->extents[cur], block, sb))) {
				in->lastextent = cur;
				return result;
			}
		}

		printk(KERN_ERR "EFS: map_block() failed to map block %u (dir)\n", block);
		return 0;
	}

#ifdef DEBUG
	printk(KERN_DEBUG "EFS: map_block(): indirect search for logical block %u\n", block);
#endif
	direxts = in->extents[0].cooked.ex_offset;
	indexts = in->numextents;

	for(indext = 0; indext < indexts; indext++) {
		cur = (last + indext) % indexts;

		/*
		 * work out which direct extent contains `cur'.
		 *
		 * also compute ibase: i.e. the number of the first
		 * indirect extent contained within direct extent `cur'.
		 *
		 */
		ibase = 0;
		for(dirext = 0; cur < ibase && dirext < direxts; dirext++) {
			ibase += in->extents[dirext].cooked.ex_length *
				(EFS_BLOCKSIZE / sizeof(efs_extent));
		}

		if (dirext == direxts) {
			/* should never happen */
			printk(KERN_ERR "EFS: couldn't find direct extent for indirect extent %d (block %u)\n", cur, block);
			if (bh) brelse(bh);
			return 0;
		}
		
		/* work out block number and offset of this indirect extent */
		iblock = sb->fs_start + in->extents[dirext].cooked.ex_bn +
			(cur - ibase) /
			(EFS_BLOCKSIZE / sizeof(efs_extent));
		ioffset = (cur - ibase) %
			(EFS_BLOCKSIZE / sizeof(efs_extent));

		if (first || lastblock != iblock) {
			if (bh) brelse(bh);

			bh = sb_bread(inode->i_sb, iblock);
			if (!bh) {
				printk(KERN_ERR "EFS: bread() failed at block %d\n", iblock);
				return 0;
			}
#ifdef DEBUG
			printk(KERN_DEBUG "EFS: map_block(): read indirect extent block %d\n", iblock);
#endif
			first = 0;
			lastblock = iblock;
		}

		exts = (efs_extent *) bh->b_data;

		extent_copy(&(exts[ioffset]), &ext);

		if (ext.cooked.ex_magic != 0) {
			printk(KERN_ERR "EFS: extent %d has bad magic number in block %d\n", cur, iblock);
			if (bh) brelse(bh);
			return 0;
		}

		if ((result = efs_extent_check(&ext, block, sb))) {
			if (bh) brelse(bh);
			in->lastextent = cur;
			return result;
		}
	}
	if (bh) brelse(bh);
	printk(KERN_ERR "EFS: map_block() failed to map block %u (indir)\n", block);
	return 0;
}  

MODULE_LICENSE("GPL");
