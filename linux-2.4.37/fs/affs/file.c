/*
 *  linux/fs/affs/file.c
 *
 *  (c) 1996  Hans-Joachim Widmaier - Rewritten
 *
 *  (C) 1993  Ray Burr - Modified for Amiga FFS filesystem.
 *
 *  (C) 1992  Eric Youngdale Modified for ISO 9660 filesystem.
 *
 *  (C) 1991  Linus Torvalds - minix filesystem
 *
 *  affs regular file handling primitives
 */

#include <asm/div64.h>
#include <asm/uaccess.h>
#include <asm/system.h>
#include <linux/sched.h>
#include <linux/affs_fs.h>
#include <linux/fcntl.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/locks.h>
#include <linux/smp_lock.h>
#include <linux/dirent.h>
#include <linux/fs.h>
#include <linux/amigaffs.h>
#include <linux/mm.h>
#include <linux/pagemap.h>

#if PAGE_SIZE < 4096
#error PAGE_SIZE must be at least 4096
#endif

static int affs_grow_extcache(struct inode *inode, u32 lc_idx);
static struct buffer_head *affs_alloc_extblock(struct inode *inode, struct buffer_head *bh, u32 ext);
static inline struct buffer_head *affs_get_extblock(struct inode *inode, u32 ext);
static struct buffer_head *affs_get_extblock_slow(struct inode *inode, u32 ext);
static int affs_get_block(struct inode *inode, long block, struct buffer_head *bh_result, int create);

static ssize_t affs_file_write(struct file *filp, const char *buf, size_t count, loff_t *ppos);
static int affs_file_open(struct inode *inode, struct file *filp);
static int affs_file_release(struct inode *inode, struct file *filp);

const struct file_operations affs_file_operations = {
	llseek:		generic_file_llseek,
	read:		generic_file_read,
	write:		affs_file_write,
	mmap:		generic_file_mmap,
	open:		affs_file_open,
	release:	affs_file_release,
	fsync:		file_fsync,
};

const struct inode_operations affs_file_inode_operations = {
	truncate:	affs_truncate,
	setattr:	affs_notify_change,
};

static int
affs_file_open(struct inode *inode, struct file *filp)
{
	if (atomic_read(&filp->f_count) != 1)
		return 0;
	pr_debug("AFFS: open(%d)\n", AFFS_INODE->i_opencnt);
	AFFS_INODE->i_opencnt++;
	return 0;
}

static int
affs_file_release(struct inode *inode, struct file *filp)
{
	if (atomic_read(&filp->f_count) != 0)
		return 0;
	pr_debug("AFFS: release(%d)\n", AFFS_INODE->i_opencnt);
	AFFS_INODE->i_opencnt--;
	if (!AFFS_INODE->i_opencnt)
		affs_free_prealloc(inode);

	return 0;
}

static int
affs_grow_extcache(struct inode *inode, u32 lc_idx)
{
	struct super_block	*sb = inode->i_sb;
	struct buffer_head	*bh;
	u32 lc_max;
	int i, j, key;

	if (!AFFS_INODE->i_lc) {
		char *ptr = (char *)get_zeroed_page(GFP_NOFS);
		if (!ptr)
			return -ENOMEM;
		AFFS_INODE->i_lc = (u32 *)ptr;
		AFFS_INODE->i_ac = (struct affs_ext_key *)(ptr + AFFS_CACHE_SIZE / 2);
	}

	lc_max = AFFS_LC_SIZE << AFFS_INODE->i_lc_shift;

	if (AFFS_INODE->i_extcnt > lc_max) {
		u32 lc_shift, lc_mask, tmp, off;

		/* need to recalculate linear cache, start from old size */
		lc_shift = AFFS_INODE->i_lc_shift;
		tmp = (AFFS_INODE->i_extcnt / AFFS_LC_SIZE) >> lc_shift;
		for (; tmp; tmp >>= 1)
			lc_shift++;
		lc_mask = (1 << lc_shift) - 1;

		/* fix idx and old size to new shift */
		lc_idx >>= (lc_shift - AFFS_INODE->i_lc_shift);
		AFFS_INODE->i_lc_size >>= (lc_shift - AFFS_INODE->i_lc_shift);

		/* first shrink old cache to make more space */
		off = 1 << (lc_shift - AFFS_INODE->i_lc_shift);
		for (i = 1, j = off; j < AFFS_LC_SIZE; i++, j += off)
			AFFS_INODE->i_ac[i] = AFFS_INODE->i_ac[j];

		AFFS_INODE->i_lc_shift = lc_shift;
		AFFS_INODE->i_lc_mask = lc_mask;
	}

	/* fill cache to the needed index */
	i = AFFS_INODE->i_lc_size;
	AFFS_INODE->i_lc_size = lc_idx + 1;
	for (; i <= lc_idx; i++) {
		if (!i) {
			AFFS_INODE->i_lc[0] = inode->i_ino;
			continue;
		}
		key = AFFS_INODE->i_lc[i - 1];
		j = AFFS_INODE->i_lc_mask + 1;
		// unlock cache
		for (; j > 0; j--) {
			bh = affs_bread(sb, key);
			if (!bh)
				goto err;
			key = be32_to_cpu(AFFS_TAIL(sb, bh)->extension);
			affs_brelse(bh);
		}
		// lock cache
		AFFS_INODE->i_lc[i] = key;
	}

	return 0;

err:
	// lock cache
	return -EIO;
}

static struct buffer_head *
affs_alloc_extblock(struct inode *inode, struct buffer_head *bh, u32 ext)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *new_bh;
	u32 blocknr, tmp;

	blocknr = affs_alloc_block(inode, bh->b_blocknr);
	if (!blocknr)
		return ERR_PTR(-ENOSPC);

	new_bh = affs_getzeroblk(sb, blocknr);
	if (!new_bh) {
		affs_free_block(sb, blocknr);
		return ERR_PTR(-EIO);
	}

	AFFS_HEAD(new_bh)->ptype = cpu_to_be32(T_LIST);
	AFFS_HEAD(new_bh)->key = cpu_to_be32(blocknr);
	AFFS_TAIL(sb, new_bh)->stype = cpu_to_be32(ST_FILE);
	AFFS_TAIL(sb, new_bh)->parent = cpu_to_be32(inode->i_ino);
	affs_fix_checksum(sb, new_bh);

	mark_buffer_dirty_inode(new_bh, inode);

	tmp = be32_to_cpu(AFFS_TAIL(sb, bh)->extension);
	if (tmp)
		affs_warning(sb, "alloc_ext", "previous extension set (%x)", tmp);
	AFFS_TAIL(sb, bh)->extension = cpu_to_be32(blocknr);
	affs_adjust_checksum(bh, blocknr - tmp);
	mark_buffer_dirty_inode(bh, inode);

	AFFS_INODE->i_extcnt++;
	mark_inode_dirty(inode);

	return new_bh;
}

static inline struct buffer_head *
affs_get_extblock(struct inode *inode, u32 ext)
{
	/* inline the simplest case: same extended block as last time */
	struct buffer_head *bh = AFFS_INODE->i_ext_bh;
	if (ext == AFFS_INODE->i_ext_last)
		atomic_inc(&bh->b_count);
	else
		/* we have to do more (not inlined) */
		bh = affs_get_extblock_slow(inode, ext);

	return bh;
}

static struct buffer_head *
affs_get_extblock_slow(struct inode *inode, u32 ext)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh;
	u32 ext_key;
	u32 lc_idx, lc_off, ac_idx;
	u32 tmp, idx;

	if (ext == AFFS_INODE->i_ext_last + 1) {
		/* read the next extended block from the current one */
		bh = AFFS_INODE->i_ext_bh;
		ext_key = be32_to_cpu(AFFS_TAIL(sb, bh)->extension);
		if (ext < AFFS_INODE->i_extcnt)
			goto read_ext;
		if (ext > AFFS_INODE->i_extcnt)
			BUG();
		bh = affs_alloc_extblock(inode, bh, ext);
		if (IS_ERR(bh))
			return bh;
		goto store_ext;
	}

	if (ext == 0) {
		/* we seek back to the file header block */
		ext_key = inode->i_ino;
		goto read_ext;
	}

	if (ext >= AFFS_INODE->i_extcnt) {
		struct buffer_head *prev_bh;

		/* allocate a new extended block */
		if (ext > AFFS_INODE->i_extcnt)
			BUG();

		/* get previous extended block */
		prev_bh = affs_get_extblock(inode, ext - 1);
		if (IS_ERR(prev_bh))
			return prev_bh;
		bh = affs_alloc_extblock(inode, prev_bh, ext);
		affs_brelse(prev_bh);
		if (IS_ERR(bh))
			return bh;
		goto store_ext;
	}

again:
	/* check if there is an extended cache and whether it's large enough */
	lc_idx = ext >> AFFS_INODE->i_lc_shift;
	lc_off = ext & AFFS_INODE->i_lc_mask;

	if (lc_idx >= AFFS_INODE->i_lc_size) {
		int err;

		err = affs_grow_extcache(inode, lc_idx);
		if (err)
			return ERR_PTR(err);
		goto again;
	}

	/* every n'th key we find in the linear cache */
	if (!lc_off) {
		ext_key = AFFS_INODE->i_lc[lc_idx];
		goto read_ext;
	}

	/* maybe it's still in the associative cache */
	ac_idx = (ext - lc_idx - 1) & AFFS_AC_MASK;
	if (AFFS_INODE->i_ac[ac_idx].ext == ext) {
		ext_key = AFFS_INODE->i_ac[ac_idx].key;
		goto read_ext;
	}

	/* try to find one of the previous extended blocks */
	tmp = ext;
	idx = ac_idx;
	while (--tmp, --lc_off > 0) {
		idx = (idx - 1) & AFFS_AC_MASK;
		if (AFFS_INODE->i_ac[idx].ext == tmp) {
			ext_key = AFFS_INODE->i_ac[idx].key;
			goto find_ext;
		}
	}

	/* fall back to the linear cache */
	ext_key = AFFS_INODE->i_lc[lc_idx];
find_ext:
	/* read all extended blocks until we find the one we need */
	//unlock cache
	do {
		bh = affs_bread(sb, ext_key);
		if (!bh)
			goto err_bread;
		ext_key = be32_to_cpu(AFFS_TAIL(sb, bh)->extension);
		affs_brelse(bh);
		tmp++;
	} while (tmp < ext);
	//lock cache

	/* store it in the associative cache */
	// recalculate ac_idx?
	AFFS_INODE->i_ac[ac_idx].ext = ext;
	AFFS_INODE->i_ac[ac_idx].key = ext_key;

read_ext:
	/* finally read the right extended block */
	//unlock cache
	bh = affs_bread(sb, ext_key);
	if (!bh)
		goto err_bread;
	//lock cache

store_ext:
	/* release old cached extended block and store the new one */
	affs_brelse(AFFS_INODE->i_ext_bh);
	AFFS_INODE->i_ext_last = ext;
	AFFS_INODE->i_ext_bh = bh;
	atomic_inc(&bh->b_count);

	return bh;

err_bread:
	affs_brelse(bh);
	return ERR_PTR(-EIO);
}

static int
affs_get_block(struct inode *inode, long block, struct buffer_head *bh_result, int create)
{
	struct super_block	*sb = inode->i_sb;
	struct buffer_head	*ext_bh;
	u32			 ext;

	pr_debug("AFFS: get_block(%u, %ld)\n", (u32)inode->i_ino, block);

	if (block < 0)
		goto err_small;

	if (block >= AFFS_INODE->i_blkcnt) {
		if (block > AFFS_INODE->i_blkcnt || !create)
			goto err_big;
	} else
		create = 0;

	//lock cache
	affs_lock_ext(inode);

	ext = block / AFFS_SB->s_hashsize;
	block -= ext * AFFS_SB->s_hashsize;
	ext_bh = affs_get_extblock(inode, ext);
	if (IS_ERR(ext_bh))
		goto err_ext;
	bh_result->b_blocknr = be32_to_cpu(AFFS_BLOCK(sb, ext_bh, block));
	bh_result->b_dev = inode->i_dev;
	bh_result->b_state |= (1UL << BH_Mapped);

	if (create) {
		u32 blocknr = affs_alloc_block(inode, ext_bh->b_blocknr);
		if (!blocknr)
			goto err_alloc;
		bh_result->b_state |= (1UL << BH_New);
		AFFS_INODE->mmu_private += AFFS_SB->s_data_blksize;
		AFFS_INODE->i_blkcnt++;

		/* store new block */
		if (bh_result->b_blocknr)
			affs_warning(sb, "get_block", "block already set (%x)", bh_result->b_blocknr);
		AFFS_BLOCK(sb, ext_bh, block) = cpu_to_be32(blocknr);
		AFFS_HEAD(ext_bh)->block_count = cpu_to_be32(block + 1);
		affs_adjust_checksum(ext_bh, blocknr - bh_result->b_blocknr + 1);
		bh_result->b_blocknr = blocknr;

		if (!block) {
			/* insert first block into header block */
			u32 tmp = be32_to_cpu(AFFS_HEAD(ext_bh)->first_data);
			if (tmp)
				affs_warning(sb, "get_block", "first block already set (%d)", tmp);
			AFFS_HEAD(ext_bh)->first_data = cpu_to_be32(blocknr);
			affs_adjust_checksum(ext_bh, blocknr - tmp);
		}
	}

	affs_brelse(ext_bh);
	//unlock cache
	affs_unlock_ext(inode);
	return 0;

err_small:
	affs_error(inode->i_sb,"get_block","Block < 0");
	return -EIO;
err_big:
	affs_error(inode->i_sb,"get_block","strange block request %d", block);
	return -EIO;
err_ext:
	// unlock cache
	affs_unlock_ext(inode);
	return PTR_ERR(ext_bh);
err_alloc:
	brelse(ext_bh);
	bh_result->b_state &= ~(1UL << BH_Mapped);
	// unlock cache
	affs_unlock_ext(inode);
	return -ENOSPC;
}

static int affs_writepage(struct page *page)
{
	return block_write_full_page(page, affs_get_block);
}
static int affs_readpage(struct file *file, struct page *page)
{
	return block_read_full_page(page, affs_get_block);
}
static int affs_prepare_write(struct file *file, struct page *page, unsigned from, unsigned to)
{
	return cont_prepare_write(page, from, to, affs_get_block,
		&page->mapping->host->u.affs_i.mmu_private);
}
static int _affs_bmap(struct address_space *mapping, long block)
{
	return generic_block_bmap(mapping,block,affs_get_block);
}
const struct address_space_operations affs_aops = {
	readpage: affs_readpage,
	writepage: affs_writepage,
	sync_page: block_sync_page,
	prepare_write: affs_prepare_write,
	commit_write: generic_commit_write,
	bmap: _affs_bmap
};

static inline struct buffer_head *
affs_bread_ino(struct inode *inode, int block, int create)
{
	struct buffer_head *bh, tmp_bh;
	int err;

	tmp_bh.b_state = 0;
	err = affs_get_block(inode, block, &tmp_bh, create);
	if (!err) {
		bh = affs_bread(inode->i_sb, tmp_bh.b_blocknr);
		if (bh) {
			bh->b_state |= tmp_bh.b_state;
			return bh;
		}
		err = -EIO;
	}
	return ERR_PTR(err);
}

static inline struct buffer_head *
affs_getzeroblk_ino(struct inode *inode, int block)
{
	struct buffer_head *bh, tmp_bh;
	int err;

	tmp_bh.b_state = 0;
	err = affs_get_block(inode, block, &tmp_bh, 1);
	if (!err) {
		bh = affs_getzeroblk(inode->i_sb, tmp_bh.b_blocknr);
		if (bh) {
			bh->b_state |= tmp_bh.b_state;
			return bh;
		}
		err = -EIO;
	}
	return ERR_PTR(err);
}

static inline struct buffer_head *
affs_getemptyblk_ino(struct inode *inode, int block)
{
	struct buffer_head *bh, tmp_bh;
	int err;

	tmp_bh.b_state = 0;
	err = affs_get_block(inode, block, &tmp_bh, 1);
	if (!err) {
		bh = affs_getemptyblk(inode->i_sb, tmp_bh.b_blocknr);
		if (bh) {
			bh->b_state |= tmp_bh.b_state;
			return bh;
		}
		err = -EIO;
	}
	return ERR_PTR(err);
}

static ssize_t
affs_file_write(struct file *file, const char *buf, size_t count, loff_t *ppos)
{
	ssize_t retval;

	retval = generic_file_write (file, buf, count, ppos);
	if (retval >0) {
		struct inode *inode = file->f_dentry->d_inode;
		inode->i_ctime = inode->i_mtime = CURRENT_TIME;
		mark_inode_dirty(inode);
	}
	return retval;
}

static int
affs_do_readpage_ofs(struct file *file, struct page *page, unsigned from, unsigned to)
{
	struct inode *inode = page->mapping->host;
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh;
	char *data;
	u32 bidx, boff, bsize;
	u32 tmp;

	pr_debug("AFFS: read_page(%u, %ld, %d, %d)\n", (u32)inode->i_ino, page->index, from, to);
	if (from > to || to > PAGE_CACHE_SIZE)
		BUG();
	data = page_address(page);
	bsize = AFFS_SB->s_data_blksize;
	tmp = (page->index << PAGE_CACHE_SHIFT) + from;
	bidx = tmp / bsize;
	boff = tmp % bsize;

	while (from < to) {
		bh = affs_bread_ino(inode, bidx, 0);
		if (IS_ERR(bh))
			return PTR_ERR(bh);
		tmp = min(bsize - boff, to - from);
		if (from + tmp > to || tmp > bsize)
			BUG();
		memcpy(data + from, AFFS_DATA(bh) + boff, tmp);
		affs_brelse(bh);
		bidx++;
		from += tmp;
		boff = 0;
	}
	return 0;
}

static int
affs_extent_file_ofs(struct inode *inode, u32 newsize)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh, *prev_bh;
	u32 bidx, boff;
	u32 size, bsize;
	u32 tmp;

	pr_debug("AFFS: extent_file(%u, %d)\n", (u32)inode->i_ino, newsize);
	bsize = AFFS_SB->s_data_blksize;
	bh = NULL;
	size = AFFS_INODE->mmu_private;
	bidx = size / bsize;
	boff = size % bsize;
	if (boff) {
		bh = affs_bread_ino(inode, bidx, 0);
		if (IS_ERR(bh))
			return PTR_ERR(bh);
		tmp = min(bsize - boff, newsize - size);
		if (boff + tmp > bsize || tmp > bsize)
			BUG();
		memset(AFFS_DATA(bh) + boff, 0, tmp);
		AFFS_DATA_HEAD(bh)->size = cpu_to_be32(be32_to_cpu(AFFS_DATA_HEAD(bh)->size) + tmp);
		affs_fix_checksum(sb, bh);
		mark_buffer_dirty_inode(bh, inode);
		size += tmp;
		bidx++;
	} else if (bidx) {
		bh = affs_bread_ino(inode, bidx - 1, 0);
		if (IS_ERR(bh))
			return PTR_ERR(bh);
	}

	while (size < newsize) {
		prev_bh = bh;
		bh = affs_getzeroblk_ino(inode, bidx);
		if (IS_ERR(bh))
			goto out;
		tmp = min(bsize, newsize - size);
		if (tmp > bsize)
			BUG();
		AFFS_DATA_HEAD(bh)->ptype = cpu_to_be32(T_DATA);
		AFFS_DATA_HEAD(bh)->key = cpu_to_be32(inode->i_ino);
		AFFS_DATA_HEAD(bh)->sequence = cpu_to_be32(bidx);
		AFFS_DATA_HEAD(bh)->size = cpu_to_be32(tmp);
		affs_fix_checksum(sb, bh);
		bh->b_state &= ~(1UL << BH_New);
		mark_buffer_dirty_inode(bh, inode);
		if (prev_bh) {
			u32 tmp = be32_to_cpu(AFFS_DATA_HEAD(prev_bh)->next);
			if (tmp)
				affs_warning(sb, "extent_file_ofs", "next block already set for %d (%d)", bidx, tmp);
			AFFS_DATA_HEAD(prev_bh)->next = cpu_to_be32(bh->b_blocknr);
			affs_adjust_checksum(prev_bh, bh->b_blocknr - tmp);
			mark_buffer_dirty_inode(prev_bh, inode);
			affs_brelse(prev_bh);
		}
		size += bsize;
		bidx++;
	}
	affs_brelse(bh);
	inode->i_size = AFFS_INODE->mmu_private = newsize;
	return 0;

out:
	inode->i_size = AFFS_INODE->mmu_private = size;
	return PTR_ERR(bh);
}

static int
affs_readpage_ofs(struct file *file, struct page *page)
{
	struct inode *inode = page->mapping->host;
	u32 to;
	int err;

	pr_debug("AFFS: read_page(%u, %ld)\n", (u32)inode->i_ino, page->index);
	to = PAGE_CACHE_SIZE;
	if (((page->index + 1) << PAGE_CACHE_SHIFT) > inode->i_size) {
		to = inode->i_size & ~PAGE_CACHE_MASK;
		memset(page_address(page) + to, 0, PAGE_CACHE_SIZE - to);
	}

	err = affs_do_readpage_ofs(file, page, 0, to);
	if (!err)
		SetPageUptodate(page);
	UnlockPage(page);
	return err;
}

static int affs_prepare_write_ofs(struct file *file, struct page *page, unsigned from, unsigned to)
{
	struct inode *inode = page->mapping->host;
	u32 size, offset;
	u32 tmp;
	int err = 0;

	pr_debug("AFFS: prepare_write(%u, %ld, %d, %d)\n", (u32)inode->i_ino, page->index, from, to);
	offset = page->index << PAGE_CACHE_SHIFT;
	if (offset + from > AFFS_INODE->mmu_private) {
		err = affs_extent_file_ofs(inode, offset + from);
		if (err)
			return err;
	}
	size = inode->i_size;

	if (Page_Uptodate(page))
		return 0;

	if (from) {
		err = affs_do_readpage_ofs(file, page, 0, from);
		if (err)
			return err;
	}
	if (to < PAGE_CACHE_SIZE) {
		memset(page_address(page) + to, 0, PAGE_CACHE_SIZE - to);
		if (size > offset + to) {
			if (size < offset + PAGE_CACHE_SIZE)
				tmp = size & ~PAGE_CACHE_MASK;
			else
				tmp = PAGE_CACHE_SIZE;
			err = affs_do_readpage_ofs(file, page, to, tmp);
		}
	}
	return err;
}

static int affs_commit_write_ofs(struct file *file, struct page *page, unsigned from, unsigned to)
{
	struct inode *inode = page->mapping->host;
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh, *prev_bh;
	char *data;
	u32 bidx, boff, bsize;
	u32 tmp;
	int written;

	pr_debug("AFFS: commit_write(%u, %ld, %d, %d)\n", (u32)inode->i_ino, page->index, from, to);
	bsize = AFFS_SB->s_data_blksize;
	data = page_address(page);

	bh = NULL;
	written = 0;
	tmp = (page->index << PAGE_CACHE_SHIFT) + from;
	bidx = tmp / bsize;
	boff = tmp % bsize;
	if (boff) {
		bh = affs_bread_ino(inode, bidx, 0);
		if (IS_ERR(bh))
			return PTR_ERR(bh);
		tmp = min(bsize - boff, to - from);
		if (boff + tmp > bsize || tmp > bsize)
			BUG();
		memcpy(AFFS_DATA(bh) + boff, data + from, tmp);
		AFFS_DATA_HEAD(bh)->size = cpu_to_be32(be32_to_cpu(AFFS_DATA_HEAD(bh)->size) + tmp);
		affs_fix_checksum(sb, bh);
		mark_buffer_dirty_inode(bh, inode);
		written += tmp;
		from += tmp;
		bidx++;
	} else if (bidx) {
		bh = affs_bread_ino(inode, bidx - 1, 0);
		if (IS_ERR(bh))
			return PTR_ERR(bh);
	}
	while (from + bsize <= to) {
		prev_bh = bh;
		bh = affs_getemptyblk_ino(inode, bidx);
		if (IS_ERR(bh))
			goto out;
		memcpy(AFFS_DATA(bh), data + from, bsize);
		if (bh->b_state & (1UL << BH_New)) {
			AFFS_DATA_HEAD(bh)->ptype = cpu_to_be32(T_DATA);
			AFFS_DATA_HEAD(bh)->key = cpu_to_be32(inode->i_ino);
			AFFS_DATA_HEAD(bh)->sequence = cpu_to_be32(bidx);
			AFFS_DATA_HEAD(bh)->size = cpu_to_be32(bsize);
			AFFS_DATA_HEAD(bh)->next = 0;
			bh->b_state &= ~(1UL << BH_New);
			if (prev_bh) {
				u32 tmp = be32_to_cpu(AFFS_DATA_HEAD(prev_bh)->next);
				if (tmp)
					affs_warning(sb, "commit_write_ofs", "next block already set for %d (%d)", bidx, tmp);
				AFFS_DATA_HEAD(prev_bh)->next = cpu_to_be32(bh->b_blocknr);
				affs_adjust_checksum(prev_bh, bh->b_blocknr - tmp);
				mark_buffer_dirty_inode(prev_bh, inode);
			}
		}
		affs_brelse(prev_bh);
		affs_fix_checksum(sb, bh);
		mark_buffer_dirty_inode(bh, inode);
		written += bsize;
		from += bsize;
		bidx++;
	}
	if (from < to) {
		prev_bh = bh;
		bh = affs_bread_ino(inode, bidx, 1);
		if (IS_ERR(bh))
			goto out;
		tmp = min(bsize, to - from);
		if (tmp > bsize)
			BUG();
		memcpy(AFFS_DATA(bh), data + from, tmp);
		if (bh->b_state & (1UL << BH_New)) {
			AFFS_DATA_HEAD(bh)->ptype = cpu_to_be32(T_DATA);
			AFFS_DATA_HEAD(bh)->key = cpu_to_be32(inode->i_ino);
			AFFS_DATA_HEAD(bh)->sequence = cpu_to_be32(bidx);
			AFFS_DATA_HEAD(bh)->size = cpu_to_be32(tmp);
			AFFS_DATA_HEAD(bh)->next = 0;
			bh->b_state &= ~(1UL << BH_New);
			if (prev_bh) {
				u32 tmp = be32_to_cpu(AFFS_DATA_HEAD(prev_bh)->next);
				if (tmp)
					affs_warning(sb, "commit_write_ofs", "next block already set for %d (%d)", bidx, tmp);
				AFFS_DATA_HEAD(prev_bh)->next = cpu_to_be32(bh->b_blocknr);
				affs_adjust_checksum(prev_bh, bh->b_blocknr - tmp);
				mark_buffer_dirty_inode(prev_bh, inode);
			}
		} else if (be32_to_cpu(AFFS_DATA_HEAD(bh)->size) < tmp)
			AFFS_DATA_HEAD(bh)->size = cpu_to_be32(tmp);
		affs_brelse(prev_bh);
		affs_fix_checksum(sb, bh);
		mark_buffer_dirty_inode(bh, inode);
		written += tmp;
		from += tmp;
		bidx++;
	}
	SetPageUptodate(page);

done:
	affs_brelse(bh);
	tmp = (page->index << PAGE_CACHE_SHIFT) + from;
	if (tmp > inode->i_size)
		inode->i_size = AFFS_INODE->mmu_private = tmp;

	return written;

out:
	bh = prev_bh;
	if (!written)
		written = PTR_ERR(bh);
	goto done;
}

const struct address_space_operations affs_aops_ofs = {
	readpage: affs_readpage_ofs,
	//writepage: affs_writepage_ofs,
	//sync_page: affs_sync_page_ofs,
	prepare_write: affs_prepare_write_ofs,
	commit_write: affs_commit_write_ofs
};

/* Free any preallocated blocks. */

void
affs_free_prealloc(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;

	pr_debug("AFFS: free_prealloc(ino=%lu)\n", inode->i_ino);

	while (inode->u.affs_i.i_pa_cnt) {
		inode->u.affs_i.i_pa_cnt--;
		affs_free_block(sb, ++inode->u.affs_i.i_lastalloc);
	}
}

/* Truncate (or enlarge) a file to the requested size. */

void
affs_truncate(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	u32 ext, ext_key;
	u32 last_blk, blkcnt, blk;
	u32 size;
	struct buffer_head *ext_bh;
	int i;

	pr_debug("AFFS: truncate(inode=%d, oldsize=%u, newsize=%u)\n",
		 (u32)inode->i_ino, (u32)AFFS_INODE->mmu_private, (u32)inode->i_size);

	last_blk = 0;
	ext = 0;
	if (inode->i_size) {
		last_blk = ((u32)inode->i_size - 1) / AFFS_SB->s_data_blksize;
		ext = last_blk / AFFS_SB->s_hashsize;
	}

	if (inode->i_size > AFFS_INODE->mmu_private) {
		struct address_space *mapping = inode->i_mapping;
		struct page *page;
		u32 size = inode->i_size - 1;
		int res;

		page = grab_cache_page(mapping, size >> PAGE_CACHE_SHIFT);
		if (!page)
			return;
		size = (size & (PAGE_CACHE_SIZE - 1)) + 1;
		res = mapping->a_ops->prepare_write(NULL, page, size, size);
		if (!res)
			res = mapping->a_ops->commit_write(NULL, page, size, size);
		UnlockPage(page);
		page_cache_release(page);
		mark_inode_dirty(inode);
		return;
	} else if (inode->i_size == AFFS_INODE->mmu_private)
		return;

	// lock cache
	ext_bh = affs_get_extblock(inode, ext);
	if (IS_ERR(ext_bh)) {
		affs_warning(sb, "truncate", "unexpected read error for ext block %u (%d)",
			     ext, PTR_ERR(ext_bh));
		return;
	}
	if (AFFS_INODE->i_lc) {
		/* clear linear cache */
		i = (ext + 1) >> AFFS_INODE->i_lc_shift;
		if (AFFS_INODE->i_lc_size > i) {
			AFFS_INODE->i_lc_size = i;
			for (; i < AFFS_LC_SIZE; i++)
				AFFS_INODE->i_lc[i] = 0;
		}
		/* clear associative cache */
		for (i = 0; i < AFFS_AC_SIZE; i++)
			if (AFFS_INODE->i_ac[i].ext >= ext)
				AFFS_INODE->i_ac[i].ext = 0;
	}
	ext_key = be32_to_cpu(AFFS_TAIL(sb, ext_bh)->extension);

	blkcnt = AFFS_INODE->i_blkcnt;
	i = 0;
	blk = last_blk;
	if (inode->i_size) {
		i = last_blk % AFFS_SB->s_hashsize + 1;
		blk++;
	} else
		AFFS_HEAD(ext_bh)->first_data = 0;
	size = AFFS_SB->s_hashsize;
	if (size > blkcnt - blk + i)
		size = blkcnt - blk + i;
	for (; i < size; i++, blk++) {
		affs_free_block(sb, be32_to_cpu(AFFS_BLOCK(sb, ext_bh, i)));
		AFFS_BLOCK(sb, ext_bh, i) = 0;
	}
	AFFS_TAIL(sb, ext_bh)->extension = 0;
	affs_fix_checksum(sb, ext_bh);
	mark_buffer_dirty_inode(ext_bh, inode);
	affs_brelse(ext_bh);

	if (inode->i_size) {
		AFFS_INODE->i_blkcnt = last_blk + 1;
		AFFS_INODE->i_extcnt = ext + 1;
		if (AFFS_SB->s_flags & SF_OFS) {
			struct buffer_head *bh = affs_bread_ino(inode, last_blk, 0);
			u32 tmp;
			if (IS_ERR(bh)) {
				affs_warning(sb, "truncate", "unexpected read error for last block %u (%d)",
					     last_blk, PTR_ERR(bh));
				return;
			}
			tmp = be32_to_cpu(AFFS_DATA_HEAD(bh)->next);
			AFFS_DATA_HEAD(bh)->next = 0;
			affs_adjust_checksum(bh, -tmp);
			affs_brelse(bh);
		}
	} else {
		AFFS_INODE->i_blkcnt = 0;
		AFFS_INODE->i_extcnt = 1;
	}
	AFFS_INODE->mmu_private = inode->i_size;
	// unlock cache

	while (ext_key) {
		ext_bh = affs_bread(sb, ext_key);
		size = AFFS_SB->s_hashsize;
		if (size > blkcnt - blk)
			size = blkcnt - blk;
		for (i = 0; i < size; i++, blk++)
			affs_free_block(sb, be32_to_cpu(AFFS_BLOCK(sb, ext_bh, i)));
		affs_free_block(sb, ext_key);
		ext_key = be32_to_cpu(AFFS_TAIL(sb, ext_bh)->extension);
		affs_brelse(ext_bh);
	}
	affs_free_prealloc(inode);
}
