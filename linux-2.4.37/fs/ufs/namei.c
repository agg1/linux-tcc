/*
 * linux/fs/ufs/namei.c
 *
 * Copyright (C) 1998
 * Daniel Pirkl <daniel.pirkl@email.cz>
 * Charles University, Faculty of Mathematics and Physics
 *
 *  from
 *
 *  linux/fs/ext2/namei.c
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/namei.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  Big-endian to little-endian byte-swapping/bitmaps by
 *        David S. Miller (davem@caip.rutgers.edu), 1995
 */

#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/ufs_fs.h>

#undef UFS_NAMEI_DEBUG

#ifdef UFS_NAMEI_DEBUG
#define UFSD(x) printk("(%s, %d), %s: ", __FILE__, __LINE__, __FUNCTION__); printk x;
#else
#define UFSD(x)
#endif

static inline void ufs_inc_count(struct inode *inode)
{
	inode->i_nlink++;
	mark_inode_dirty(inode);
}

static inline void ufs_dec_count(struct inode *inode)
{
	inode->i_nlink--;
	mark_inode_dirty(inode);
}

static inline int ufs_add_nondir(struct dentry *dentry, struct inode *inode)
{
	int err = ufs_add_link(dentry, inode);
	if (!err) {
		d_instantiate(dentry, inode);
		return 0;
	}
	ufs_dec_count(inode);
	iput(inode);
	return err;
}

static struct dentry *ufs_lookup(struct inode * dir, struct dentry *dentry)
{
	struct inode * inode = NULL;
	ino_t ino;
	
	if (dentry->d_name.len > UFS_MAXNAMLEN)
		return ERR_PTR(-ENAMETOOLONG);

	ino = ufs_inode_by_name(dir, dentry);
	if (ino) {
		inode = iget(dir->i_sb, ino);
		if (!inode) 
			return ERR_PTR(-EACCES);
	}
	d_add(dentry, inode);
	return NULL;
}

/*
 * By the time this is called, we already have created
 * the directory cache entry for the new file, but it
 * is so far negative - it has no inode.
 *
 * If the create succeeds, we fill in the inode information
 * with d_instantiate(). 
 */
static int ufs_create (struct inode * dir, struct dentry * dentry, int mode)
{
	struct inode * inode = ufs_new_inode(dir, mode);
	int err = PTR_ERR(inode);
	if (!IS_ERR(inode)) {
		inode->i_op = &ufs_file_inode_operations;
		inode->i_fop = &ufs_file_operations;
		inode->i_mapping->a_ops = &ufs_aops;
		mark_inode_dirty(inode);
		err = ufs_add_nondir(dentry, inode);
	}
	return err;
}

static int ufs_mknod (struct inode * dir, struct dentry *dentry, int mode, int rdev)
{
	struct inode * inode = ufs_new_inode(dir, mode);
	int err = PTR_ERR(inode);
	if (!IS_ERR(inode)) {
		init_special_inode(inode, mode, rdev);
		mark_inode_dirty(inode);
		err = ufs_add_nondir(dentry, inode);
	}
	return err;
}

static int ufs_symlink (struct inode * dir, struct dentry * dentry,
	const char * symname)
{
	struct super_block * sb = dir->i_sb;
	int err = -ENAMETOOLONG;
	unsigned l = strlen(symname)+1;
	struct inode * inode;

	if (l > sb->s_blocksize)
		goto out;

	inode = ufs_new_inode(dir, S_IFLNK | S_IRWXUGO);
	err = PTR_ERR(inode);
	if (IS_ERR(inode))
		goto out;

	if (l > sb->u.ufs_sb.s_uspi->s_maxsymlinklen) {
		/* slow symlink */
		inode->i_op = &page_symlink_inode_operations;
		inode->i_mapping->a_ops = &ufs_aops;
		err = block_symlink(inode, symname, l);
		if (err)
			goto out_fail;
	} else {
		/* fast symlink */
		inode->i_op = &ufs_fast_symlink_inode_operations;
		memcpy((char*)&inode->u.ufs_i.i_u1.i_data,symname,l);
		inode->i_size = l-1;
	}
	mark_inode_dirty(inode);

	err = ufs_add_nondir(dentry, inode);
out:
	return err;

out_fail:
	ufs_dec_count(inode);
	iput(inode);
	goto out;
}

static int ufs_link (struct dentry * old_dentry, struct inode * dir,
	struct dentry *dentry)
{
	struct inode *inode = old_dentry->d_inode;

	if (S_ISDIR(inode->i_mode))
		return -EPERM;

	if (inode->i_nlink >= UFS_LINK_MAX)
		return -EMLINK;

	inode->i_ctime = CURRENT_TIME;
	ufs_inc_count(inode);
	atomic_inc(&inode->i_count);

	return ufs_add_nondir(dentry, inode);
}

static int ufs_mkdir(struct inode * dir, struct dentry * dentry, int mode)
{
	struct inode * inode;
	int err = -EMLINK;

	if (dir->i_nlink >= UFS_LINK_MAX)
		goto out;

	ufs_inc_count(dir);

	inode = ufs_new_inode(dir, S_IFDIR|mode);
	err = PTR_ERR(inode);
	if (IS_ERR(inode))
		goto out_dir;

	inode->i_op = &ufs_dir_inode_operations;
	inode->i_fop = &ufs_dir_operations;

	ufs_inc_count(inode);

	err = ufs_make_empty(inode, dir);
	if (err)
		goto out_fail;

	err = ufs_add_link(dentry, inode);
	if (err)
		goto out_fail;

	d_instantiate(dentry, inode);
out:
	return err;

out_fail:
	ufs_dec_count(inode);
	ufs_dec_count(inode);
	iput (inode);
out_dir:
	ufs_dec_count(dir);
	goto out;
}

static int ufs_unlink(struct inode * dir, struct dentry *dentry)
{
	struct inode * inode = dentry->d_inode;
	struct buffer_head * bh;
	struct ufs_dir_entry * de;
	int err = -ENOENT;

	de = ufs_find_entry (dentry, &bh);
	if (!de)
		goto out;

	err = ufs_delete_entry (dir, de, bh);
	if (err)
		goto out;

	inode->i_ctime = dir->i_ctime;
	ufs_dec_count(inode);
	err = 0;
out:
	return err;
}

static int ufs_rmdir (struct inode * dir, struct dentry *dentry)
{
	struct inode * inode = dentry->d_inode;
	int err= -ENOTEMPTY;

	if (ufs_empty_dir (inode)) {
		err = ufs_unlink(dir, dentry);
		if (!err) {
			inode->i_size = 0;
			ufs_dec_count(inode);
			ufs_dec_count(dir);
		}
	}
	return err;
}

static int ufs_rename (struct inode * old_dir, struct dentry * old_dentry,
	struct inode * new_dir,	struct dentry * new_dentry )
{
	struct inode *old_inode = old_dentry->d_inode;
	struct inode *new_inode = new_dentry->d_inode;
	struct buffer_head *dir_bh = NULL;
	struct ufs_dir_entry *dir_de = NULL;
	struct buffer_head *old_bh;
	struct ufs_dir_entry *old_de;
	int err = -ENOENT;

	old_de = ufs_find_entry (old_dentry, &old_bh);
	if (!old_de)
		goto out;

	if (S_ISDIR(old_inode->i_mode)) {
		err = -EIO;
		dir_de = ufs_dotdot(old_inode, &dir_bh);
		if (!dir_de)
			goto out_old;
	}

	if (new_inode) {
		struct buffer_head *new_bh;
		struct ufs_dir_entry *new_de;

		err = -ENOTEMPTY;
		if (dir_de && !ufs_empty_dir (new_inode))
			goto out_dir;
		err = -ENOENT;
		new_de = ufs_find_entry (new_dentry, &new_bh);
		if (!new_de)
			goto out_dir;
		ufs_inc_count(old_inode);
		ufs_set_link(new_dir, new_de, new_bh, old_inode);
		new_inode->i_ctime = CURRENT_TIME;
		if (dir_de)
			new_inode->i_nlink--;
		ufs_dec_count(new_inode);
	} else {
		if (dir_de) {
			err = -EMLINK;
			if (new_dir->i_nlink >= UFS_LINK_MAX)
				goto out_dir;
		}
		ufs_inc_count(old_inode);
		err = ufs_add_link(new_dentry, old_inode);
		if (err) {
			ufs_dec_count(old_inode);
			goto out_dir;
		}
		if (dir_de)
			ufs_inc_count(new_dir);
	}

	ufs_delete_entry (old_dir, old_de, old_bh);

	ufs_dec_count(old_inode);

	if (dir_de) {
		ufs_set_link(old_inode, dir_de, dir_bh, new_dir);
		ufs_dec_count(old_dir);
	}
	return 0;

out_dir:
	if (dir_de)
		brelse(dir_bh);
out_old:
	brelse (old_bh);
out:
	return err;
}

const struct inode_operations ufs_dir_inode_operations = {
	create:		ufs_create,
	lookup:		ufs_lookup,
	link:		ufs_link,
	unlink:		ufs_unlink,
	symlink:	ufs_symlink,
	mkdir:		ufs_mkdir,
	rmdir:		ufs_rmdir,
	mknod:		ufs_mknod,
	rename:		ufs_rename,
};
