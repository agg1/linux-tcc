/*
 *  linux/fs/hfsplus/dir.c
 *
 * Copyright (C) 2001
 * Brad Boyer (flar@allandria.com)
 * (C) 2003 Ardis Technologies <roman@ardistech.com>
 *
 * Handling of directories
 */

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0)
#include <linux/buffer_head.h>
#endif

#include "hfsplus_fs.h"
#include "hfsplus_raw.h"

/* Find the entry inside dir named dentry->d_name */
static struct dentry *hfsplus_lookup(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = NULL;
	struct hfsplus_find_data fd;
	struct super_block *sb;
	hfsplus_cat_entry entry;
	int err;
	u32 cnid, linkid = 0;
	u16 type;

	sb = dir->i_sb;
	dentry->d_fsdata = NULL;
	hfsplus_find_init(HFSPLUS_SB(sb).cat_tree, &fd);
	hfsplus_fill_cat_key(fd.search_key, dir->i_ino, &dentry->d_name);
again:
	err = hfsplus_btree_find_entry(&fd, &entry, sizeof(entry));
	if (err) {
		if (err == -ENOENT) {
			hfsplus_find_exit(&fd);
			/* No such entry */
			inode = NULL;
			goto out;
		}
		goto fail;
	}
	type = be16_to_cpu(entry.type);
	if (type == HFSPLUS_FOLDER) {
		if (fd.entrylength < sizeof(hfsplus_cat_folder)) {
			err = -EIO;
			goto fail;
		}
		cnid = be32_to_cpu(entry.folder.id);
	} else if (type == HFSPLUS_FILE) {
		if (fd.entrylength < sizeof(hfsplus_cat_file)) {
			err = -EIO;
			goto fail;
		}
		cnid = be32_to_cpu(entry.file.id);
		if (entry.file.user_info.fdType == cpu_to_be32(HFSP_HARDLINK_TYPE) &&
		    entry.file.user_info.fdCreator == cpu_to_be32(HFSP_HFSPLUS_CREATOR)) {
			struct qstr str;
			char name[32];

			if (dentry->d_fsdata) {
				err = -ENOENT;
				inode = NULL;
				goto out;
			}
			dentry->d_fsdata = (void *)(unsigned long)cnid;
			linkid = be32_to_cpu(entry.file.permissions.dev);
			str.len = sprintf(name, "iNode%d", linkid);
			str.name = name;
			hfsplus_fill_cat_key(fd.search_key, HFSPLUS_SB(sb).hidden_dir->i_ino, &str);
			goto again;
		} else if (!dentry->d_fsdata)
			dentry->d_fsdata = (void *)(unsigned long)cnid;
	} else {
		printk("HFS+-fs: Illegal catalog entry type in lookup\n");
		err = -EIO;
		goto fail;
	}
	hfsplus_find_exit(&fd);
	inode = iget(dir->i_sb, cnid);
	if (!inode)
		return ERR_PTR(-EACCES);
	if (S_ISREG(inode->i_mode))
		HFSPLUS_I(inode).dev = linkid;
out:
	d_add(dentry, inode);
	return NULL;
fail:
	hfsplus_find_exit(&fd);
	return ERR_PTR(err);
}

static int hfsplus_readdir(struct file *filp, void *dirent, filldir_t filldir)
{
	struct inode *inode = filp->f_dentry->d_inode;
	struct super_block *sb = inode->i_sb;
	int len, err;
	char strbuf[HFSPLUS_MAX_STRLEN + 1];
	hfsplus_cat_entry entry;
	struct hfsplus_find_data fd;
	struct hfsplus_readdir_data *rd;
	u16 type;

	if (filp->f_pos >= inode->i_size)
		return 0;

	hfsplus_find_init(HFSPLUS_SB(sb).cat_tree, &fd);
	hfsplus_fill_cat_key(fd.search_key, inode->i_ino, NULL);
	err = hfsplus_btree_find(&fd);
	if (err)
		goto out;

	switch ((u32)filp->f_pos) {
	case 0:
		/* This is completely artificial... */
		if (filldir(dirent, ".", 1, 0, inode->i_ino, DT_DIR))
			goto out;
		filp->f_pos++;
		/* fall through */
	case 1:
		hfsplus_bnode_readbytes(fd.bnode, &entry, fd.entryoffset, fd.entrylength);
		if (be16_to_cpu(entry.type) != HFSPLUS_FOLDER_THREAD) {
			printk("HFS+-fs: bad catalog folder thread\n");
			err = -EIO;
			goto out;
		}
		if (fd.entrylength < HFSPLUS_MIN_THREAD_SZ) {
			printk("HFS+-fs: truncated catalog thread\n");
			err = -EIO;
			goto out;
		}
		if (filldir(dirent, "..", 2, 1,
			    be32_to_cpu(entry.thread.parentID), DT_DIR))
			goto out;
		filp->f_pos++;
		/* fall through */
	default:
		if (filp->f_pos >= inode->i_size)
			goto out;
		err = hfsplus_btree_move(&fd, filp->f_pos - 1);
		if (err)
			goto out;
	}

	for (;;) {
		if (be32_to_cpu(fd.key->cat.parent) != inode->i_ino) {
			printk("HFS+-fs: walked past end of dir\n");
			err = -EIO;
			goto out;
		}
		hfsplus_bnode_readbytes(fd.bnode, &entry, fd.entryoffset, fd.entrylength);
		type = be16_to_cpu(entry.type);
		len = HFSPLUS_MAX_STRLEN;
		err = hfsplus_uni2asc(&fd.key->cat.name, strbuf, &len);
		if (err)
			goto out;
		if (type == HFSPLUS_FOLDER) {
			if (fd.entrylength < sizeof(hfsplus_cat_folder)) {
				printk("HFS+-fs: small dir entry\n");
				err = -EIO;
				goto out;
			}
			if (HFSPLUS_SB(sb).hidden_dir &&
			    HFSPLUS_SB(sb).hidden_dir->i_ino == be32_to_cpu(entry.folder.id))
				goto next;
			if (filldir(dirent, strbuf, len, filp->f_pos,
				    be32_to_cpu(entry.folder.id), DT_DIR))
				break;
		} else if (type == HFSPLUS_FILE) {
			if (fd.entrylength < sizeof(hfsplus_cat_file)) {
				printk("HFS+-fs: small file entry\n");
				err = -EIO;
				goto out;
			}
			if (filldir(dirent, strbuf, len, filp->f_pos,
				    be32_to_cpu(entry.file.id), DT_REG))
				break;
		} else {
			printk("HFS+-fs: bad catalog entry type\n");
			err = -EIO;
			goto out;
		}
	next:
		filp->f_pos++;
		if (filp->f_pos >= inode->i_size)
			goto out;
		err = hfsplus_btree_move(&fd, 1);
		if (err)
			goto out;
	}
	rd = filp->private_data;
	if (!filp->private_data) {
		rd = kmalloc(sizeof(struct hfsplus_readdir_data), GFP_KERNEL);
		if (!rd) {
			err = -ENOMEM;
			goto out;
		}
		filp->private_data = rd;
		rd->file = filp;
		list_add(&rd->list, &HFSPLUS_I(inode).open_dir_list);
	}
	memcpy(&rd->key, fd.key, sizeof(hfsplus_cat_key));
out:
	hfsplus_find_exit(&fd);
	return err;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0)
static loff_t hfsplus_seek_dir(struct file *file, loff_t offset, int origin)
{
	loff_t res;

	down(&file->f_dentry->d_inode->i_sem);
	res = default_llseek(file, offset, origin);
	up(&file->f_dentry->d_inode->i_sem);

	return res;
}
#endif

static int hfsplus_dir_release(struct inode *inode, struct file *file)
{
	struct hfsplus_readdir_data *rd = file->private_data;
	if (rd) {
		list_del(&rd->list);
		kfree(rd);
	}
	return 0;
}

int hfsplus_create(struct inode *dir, struct dentry *dentry, int mode)
{
	struct inode *inode;
	int res;

	inode = hfsplus_new_inode(dir->i_sb, mode);
	if (!inode)
		return -ENOSPC;

	res = hfsplus_create_cat(inode->i_ino, dir, &dentry->d_name, inode);
	if (res) {
		inode->i_nlink = 0;
		iput(inode);
		return res;
	}
	dentry->d_fsdata = (void *)inode->i_ino;
	d_instantiate(dentry, inode);
	mark_inode_dirty(inode);
	return 0;
}

int hfsplus_link(struct dentry *src_dentry, struct inode *dst_dir, struct dentry *dst_dentry)
{
	struct super_block *sb = dst_dir->i_sb;
	struct inode *inode = src_dentry->d_inode;
	struct inode *src_dir = src_dentry->d_parent->d_inode;
	struct qstr str;
	char name[32];
	u32 cnid, id;
	int res;

	if (HFSPLUS_IS_RSRC(inode))
		return -EPERM;

	if (inode->i_ino == (u32)(unsigned long)src_dentry->d_fsdata) {
		for (;;) {
			get_random_bytes(&id, sizeof(cnid));
			id &= 0x3fffffff;
			str.name = name;
			str.len = sprintf(name, "iNode%d", id);
			res = hfsplus_rename_cat(inode->i_ino,
						 src_dir, &src_dentry->d_name,
						 HFSPLUS_SB(sb).hidden_dir, &str);
			if (!res)
				break;
			if (res != -EEXIST)
				return res;
		}
		HFSPLUS_I(inode).dev = id;
		cnid = HFSPLUS_SB(sb).next_cnid++;
		src_dentry->d_fsdata = (void *)(unsigned long)cnid;
		res = hfsplus_create_cat(cnid, src_dir, &src_dentry->d_name, inode);
		if (res)
			/* panic? */
			return res;
		HFSPLUS_SB(sb).file_count++;
	}
	cnid = HFSPLUS_SB(sb).next_cnid++;
	res = hfsplus_create_cat(cnid, dst_dir, &dst_dentry->d_name, inode);
	if (res)
		return res;

	inode->i_nlink++;
	dst_dentry->d_fsdata = (void *)(unsigned long)cnid;
	d_instantiate(dst_dentry, inode);
	atomic_inc(&inode->i_count);
	inode->i_ctime = CURRENT_TIME;
	mark_inode_dirty(inode);
	HFSPLUS_SB(sb).file_count++;
	sb->s_dirt = 1;

	return 0;
}

int hfsplus_unlink(struct inode *dir, struct dentry *dentry)
{
	struct super_block *sb = dir->i_sb;
	struct inode *inode = dentry->d_inode;
	struct qstr str;
	char name[32];
	u32 cnid;
	int res;

	if (HFSPLUS_IS_RSRC(inode))
		return -EPERM;

	cnid = (u32)(unsigned long)dentry->d_fsdata;
	if (inode->i_ino == cnid &&
	    atomic_read(&HFSPLUS_I(inode).opencnt)) {
		str.name = name;
		str.len = sprintf(name, "temp%lu", inode->i_ino);
		res = hfsplus_rename_cat(inode->i_ino,
					 dir, &dentry->d_name,
					 HFSPLUS_SB(sb).hidden_dir, &str);
		if (!res)
			inode->i_flags |= S_DEAD;
		return res;
	}
	res = hfsplus_delete_cat(cnid, dir, &dentry->d_name);
	if (res)
		return res;

	inode->i_nlink--;
	hfsplus_delete_inode(inode);
	if (inode->i_ino != cnid && !inode->i_nlink) {
		if (!atomic_read(&HFSPLUS_I(inode).opencnt)) {
			res = hfsplus_delete_cat(inode->i_ino, HFSPLUS_SB(sb).hidden_dir, NULL);
			if (!res)
				hfsplus_delete_inode(inode);
		} else
			inode->i_flags |= S_DEAD;
	}
	inode->i_ctime = CURRENT_TIME;
	mark_inode_dirty(inode);

	return res;
}

int hfsplus_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	struct inode *inode;
	int res;

	inode = hfsplus_new_inode(dir->i_sb, S_IFDIR | mode);
	if (!inode)
		return -ENOSPC;

	res = hfsplus_create_cat(inode->i_ino, dir, &dentry->d_name, inode);
	if (res) {
		inode->i_nlink = 0;
		iput(inode);
		return res;
	}
	d_instantiate(dentry, inode);
	mark_inode_dirty(inode);
	return 0;
}

int hfsplus_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode;
	int res;

	inode = dentry->d_inode;
	if (inode->i_size != 2)
		return -ENOTEMPTY;
	res = hfsplus_delete_cat(inode->i_ino, dir, &dentry->d_name);
	if (res)
		return res;
	inode->i_nlink = 0;
	inode->i_ctime = CURRENT_TIME;
	hfsplus_delete_inode(inode);
	mark_inode_dirty(inode);
	return 0;
}

int hfsplus_symlink(struct inode *dir, struct dentry *dentry, const char *symname)
{
	struct super_block *sb;
	struct inode *inode;
	int res;

	sb = dir->i_sb;
	inode = hfsplus_new_inode(sb, S_IFLNK | S_IRWXUGO);
	if (!inode)
		return -ENOSPC;

	res = page_symlink(inode, symname, strlen(symname) + 1);
	if (res) {
		inode->i_nlink = 0;
		iput (inode);
		return res;
	}

	mark_inode_dirty(inode);
	res = hfsplus_create_cat(inode->i_ino, dir, &dentry->d_name, inode);

	if (!res) {
		d_instantiate(dentry, inode);
		mark_inode_dirty(inode);
	}

	return res;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0)
int hfsplus_mknod(struct inode *dir, struct dentry *dentry, int mode, dev_t rdev)
#else
int hfsplus_mknod(struct inode *dir, struct dentry *dentry, int mode, int rdev)
#endif
{
	struct super_block *sb;
	struct inode *inode;
	int res;

	sb = dir->i_sb;
	inode = hfsplus_new_inode(sb, mode);
	if (!inode)
		return -ENOSPC;

	res = hfsplus_create_cat(inode->i_ino, dir, &dentry->d_name, inode);
	if (res) {
		inode->i_nlink = 0;
		iput(inode);
		return res;
	}
	init_special_inode(inode, mode, rdev);
	d_instantiate(dentry, inode);
	mark_inode_dirty(inode);

	return 0;
}

int hfsplus_rename(struct inode *old_dir, struct dentry *old_dentry,
		   struct inode *new_dir, struct dentry *new_dentry)
{
	int res;

	/* Unlink destination if it already exists */
	if (new_dentry->d_inode) {
		res = hfsplus_unlink(new_dir, new_dentry);
		if (res)
			return res;
	}

	res = hfsplus_rename_cat((u32)(unsigned long)old_dentry->d_fsdata,
				 old_dir, &old_dentry->d_name,
				 new_dir, &new_dentry->d_name);
	if (!res)
		new_dentry->d_fsdata = old_dentry->d_fsdata;
	return res;
}

const struct inode_operations hfsplus_dir_inode_operations = {
	.lookup		= hfsplus_lookup,
	.create		= hfsplus_create,
	.link		= hfsplus_link,
	.unlink		= hfsplus_unlink,
	.mkdir		= hfsplus_mkdir,
	.rmdir		= hfsplus_rmdir,
	.symlink	= hfsplus_symlink,
	.mknod		= hfsplus_mknod,
	.rename		= hfsplus_rename,
};

const struct file_operations hfsplus_dir_operations = {
	.read		= generic_read_dir,
	.readdir	= hfsplus_readdir,
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0)
	.llseek		= hfsplus_seek_dir,
#else
	.llseek		= generic_file_llseek,
#endif
	.release	= hfsplus_dir_release,
};
