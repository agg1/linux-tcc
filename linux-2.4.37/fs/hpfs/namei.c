/*
 *  linux/fs/hpfs/namei.c
 *
 *  Mikulas Patocka (mikulas@artax.karlin.mff.cuni.cz), 1998-1999
 *
 *  adding & removing files & directories
 */

#include <linux/string.h>
#include "hpfs_fn.h"

int hpfs_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	const char *name = dentry->d_name.name;
	unsigned len = dentry->d_name.len;
	struct quad_buffer_head qbh0;
	struct buffer_head *bh;
	struct hpfs_dirent *de;
	struct fnode *fnode;
	struct dnode *dnode;
	struct inode *result;
	fnode_secno fno;
	dnode_secno dno;
	int r;
	struct hpfs_dirent dee;
	int err;
	if ((err = hpfs_chk_name((char *)name, &len))) return err==-ENOENT ? -EINVAL : err;
	if (!(fnode = hpfs_alloc_fnode(dir->i_sb, dir->i_hpfs_dno, &fno, &bh))) goto bail;
	if (!(dnode = hpfs_alloc_dnode(dir->i_sb, fno, &dno, &qbh0, 1))) goto bail1;
	memset(&dee, 0, sizeof dee);
	dee.directory = 1;
	if (!(mode & 0222)) dee.read_only = 1;
	/*dee.archive = 0;*/
	dee.hidden = name[0] == '.';
	dee.fnode = fno;
	dee.creation_date = dee.write_date = dee.read_date = gmt_to_local(dir->i_sb, CURRENT_TIME);
	hpfs_lock_inode(dir);
	r = hpfs_add_dirent(dir, (char *)name, len, &dee, 0);
	if (r == 1) goto bail2;
	if (r == -1) {
		brelse(bh);
		hpfs_brelse4(&qbh0);
		hpfs_free_sectors(dir->i_sb, fno, 1);
		hpfs_free_dnode(dir->i_sb, dno);
		hpfs_unlock_inode(dir);
		return -EEXIST;
	}
	fnode->len = len;
	memcpy(fnode->name, name, len > 15 ? 15 : len);
	fnode->up = dir->i_ino;
	fnode->dirflag = 1;
	fnode->btree.n_free_nodes = 7;
	fnode->btree.n_used_nodes = 1;
	fnode->btree.first_free = 0x14;
	fnode->u.external[0].disk_secno = dno;
	fnode->u.external[0].file_secno = -1;
	dnode->root_dnode = 1;
	dnode->up = fno;
	de = hpfs_add_de(dir->i_sb, dnode, "\001\001", 2, 0);
	de->creation_date = de->write_date = de->read_date = gmt_to_local(dir->i_sb, CURRENT_TIME);
	if (!(mode & 0222)) de->read_only = 1;
	de->first = de->directory = 1;
	/*de->hidden = de->system = 0;*/
	de->fnode = fno;
	mark_buffer_dirty(bh);
	brelse(bh);
	hpfs_mark_4buffers_dirty(&qbh0);
	hpfs_brelse4(&qbh0);
	dir->i_nlink++;
	hpfs_lock_iget(dir->i_sb, 1);
	if ((result = iget(dir->i_sb, fno))) {
		result->i_hpfs_parent_dir = dir->i_ino;
		result->i_ctime = result->i_mtime = result->i_atime = local_to_gmt(dir->i_sb, dee.creation_date);
		result->i_hpfs_ea_size = 0;
		if (dee.read_only) result->i_mode &= ~0222;
		if (result->i_uid != current->fsuid ||
		    result->i_gid != current->fsgid ||
		    result->i_mode != (mode | S_IFDIR)) {
			result->i_uid = current->fsuid;
			result->i_gid = current->fsgid;
			result->i_mode = mode | S_IFDIR;
			hpfs_write_inode_nolock(result);
		}
		d_instantiate(dentry, result);
	}
	hpfs_unlock_iget(dir->i_sb);
	hpfs_unlock_inode(dir);
	return 0;
	bail2:
	hpfs_brelse4(&qbh0);
	hpfs_free_dnode(dir->i_sb, dno);
	hpfs_unlock_inode(dir);
	bail1:
	brelse(bh);
	hpfs_free_sectors(dir->i_sb, fno, 1);
	bail:
	return -ENOSPC;
}

int hpfs_create(struct inode *dir, struct dentry *dentry, int mode)
{
	const char *name = dentry->d_name.name;
	unsigned len = dentry->d_name.len;
	struct inode *result = NULL;
	struct buffer_head *bh;
	struct fnode *fnode;
	fnode_secno fno;
	int r;
	struct hpfs_dirent dee;
	int err;
	if ((err = hpfs_chk_name((char *)name, &len))) return err==-ENOENT ? -EINVAL : err;
	if (!(fnode = hpfs_alloc_fnode(dir->i_sb, dir->i_hpfs_dno, &fno, &bh))) goto bail;
	memset(&dee, 0, sizeof dee);
	if (!(mode & 0222)) dee.read_only = 1;
	dee.archive = 1;
	dee.hidden = name[0] == '.';
	dee.fnode = fno;
	dee.creation_date = dee.write_date = dee.read_date = gmt_to_local(dir->i_sb, CURRENT_TIME);
	hpfs_lock_inode(dir);
	r = hpfs_add_dirent(dir, (char *)name, len, &dee, 0);
	if (r == 1) goto bail1;
	if (r == -1) {
		brelse(bh);
		hpfs_free_sectors(dir->i_sb, fno, 1);
		hpfs_unlock_inode(dir);
		return -EEXIST;
	}
	fnode->len = len;
	memcpy(fnode->name, name, len > 15 ? 15 : len);
	fnode->up = dir->i_ino;
	mark_buffer_dirty(bh);
	brelse(bh);
	hpfs_lock_iget(dir->i_sb, 2);
	if ((result = iget(dir->i_sb, fno))) {
		hpfs_decide_conv(result, (char *)name, len);
		result->i_hpfs_parent_dir = dir->i_ino;
		result->i_ctime = result->i_mtime = result->i_atime = local_to_gmt(dir->i_sb, dee.creation_date);
		result->i_hpfs_ea_size = 0;
		if (dee.read_only) result->i_mode &= ~0222;
		if (result->i_blocks == -1) result->i_blocks = 1;
		if (result->i_size == -1) {
			result->i_size = 0;
			result->i_data.a_ops = &hpfs_aops;
			result->u.hpfs_i.mmu_private = 0;
		}
		if (result->i_uid != current->fsuid ||
		    result->i_gid != current->fsgid ||
		    result->i_mode != (mode | S_IFREG)) {
			result->i_uid = current->fsuid;
			result->i_gid = current->fsgid;
			result->i_mode = mode | S_IFREG;
			hpfs_write_inode_nolock(result);
		}
		d_instantiate(dentry, result);
	}
	hpfs_unlock_iget(dir->i_sb);
	hpfs_unlock_inode(dir);
	return 0;
	bail1:
	brelse(bh);
	hpfs_free_sectors(dir->i_sb, fno, 1);
	hpfs_unlock_inode(dir);
	bail:
	return -ENOSPC;
}

int hpfs_mknod(struct inode *dir, struct dentry *dentry, int mode, int rdev)
{
	const char *name = dentry->d_name.name;
	unsigned len = dentry->d_name.len;
	struct buffer_head *bh;
	struct fnode *fnode;
	fnode_secno fno;
	int r;
	struct hpfs_dirent dee;
	struct inode *result = NULL;
	int err;
	if ((err = hpfs_chk_name((char *)name, &len))) return err==-ENOENT ? -EINVAL : err;
	if (dir->i_sb->s_hpfs_eas < 2) return -EPERM;
	if (!(fnode = hpfs_alloc_fnode(dir->i_sb, dir->i_hpfs_dno, &fno, &bh))) goto bail;
	memset(&dee, 0, sizeof dee);
	if (!(mode & 0222)) dee.read_only = 1;
	dee.archive = 1;
	dee.hidden = name[0] == '.';
	dee.fnode = fno;
	dee.creation_date = dee.write_date = dee.read_date = gmt_to_local(dir->i_sb, CURRENT_TIME);
	hpfs_lock_inode(dir);
	r = hpfs_add_dirent(dir, (char *)name, len, &dee, 0);
	if (r == 1) goto bail1;
	if (r == -1) {
		brelse(bh);
		hpfs_free_sectors(dir->i_sb, fno, 1);
		hpfs_unlock_inode(dir);
		return -EEXIST;
	}
	fnode->len = len;
	memcpy(fnode->name, name, len > 15 ? 15 : len);
	fnode->up = dir->i_ino;
	mark_buffer_dirty(bh);
	hpfs_lock_iget(dir->i_sb, 2);
	if ((result = iget(dir->i_sb, fno))) {
		result->i_hpfs_parent_dir = dir->i_ino;
		result->i_ctime = result->i_mtime = result->i_atime = local_to_gmt(dir->i_sb, dee.creation_date);
		result->i_hpfs_ea_size = 0;
		/*if (result->i_blocks == -1) result->i_blocks = 1;
		if (result->i_size == -1) result->i_size = 0;*/
		result->i_uid = current->fsuid;
		result->i_gid = current->fsgid;
		result->i_nlink = 1;
		result->i_size = 0;
		result->i_blocks = 1;
		init_special_inode(result, mode, rdev);
		hpfs_write_inode_nolock(result);
		d_instantiate(dentry, result);
	}
	hpfs_unlock_iget(dir->i_sb);
	hpfs_unlock_inode(dir);
	brelse(bh);
	return 0;
	bail1:
	brelse(bh);
	hpfs_free_sectors(dir->i_sb, fno, 1);
	hpfs_unlock_inode(dir);
	bail:
	return -ENOSPC;
}

extern const struct address_space_operations hpfs_symlink_aops;

int hpfs_symlink(struct inode *dir, struct dentry *dentry, const char *symlink)
{
	const char *name = dentry->d_name.name;
	unsigned len = dentry->d_name.len;
	struct buffer_head *bh;
	struct fnode *fnode;
	fnode_secno fno;
	int r;
	struct hpfs_dirent dee;
	struct inode *result;
	int err;
	if ((err = hpfs_chk_name((char *)name, &len))) return err==-ENOENT ? -EINVAL : err;
	if (dir->i_sb->s_hpfs_eas < 2) return -EPERM;
	if (!(fnode = hpfs_alloc_fnode(dir->i_sb, dir->i_hpfs_dno, &fno, &bh))) goto bail;
	memset(&dee, 0, sizeof dee);
	dee.archive = 1;
	dee.hidden = name[0] == '.';
	dee.fnode = fno;
	dee.creation_date = dee.write_date = dee.read_date = gmt_to_local(dir->i_sb, CURRENT_TIME);
	hpfs_lock_inode(dir);
	r = hpfs_add_dirent(dir, (char *)name, len, &dee, 0);
	if (r == 1) goto bail1;
	if (r == -1) {
		brelse(bh);
		hpfs_free_sectors(dir->i_sb, fno, 1);
		hpfs_unlock_inode(dir);
		return -EEXIST;
	}
	fnode->len = len;
	memcpy(fnode->name, name, len > 15 ? 15 : len);
	fnode->up = dir->i_ino;
	mark_buffer_dirty(bh);
	brelse(bh);
	hpfs_lock_iget(dir->i_sb, 2);
	if ((result = iget(dir->i_sb, fno))) {
		result->i_hpfs_parent_dir = dir->i_ino;
		result->i_ctime = result->i_mtime = result->i_atime = local_to_gmt(dir->i_sb, dee.creation_date);
		result->i_hpfs_ea_size = 0;
		/*if (result->i_blocks == -1) result->i_blocks = 1;
		if (result->i_size == -1) result->i_size = 0;*/
		result->i_mode = S_IFLNK | 0777;
		result->i_uid = current->fsuid;
		result->i_gid = current->fsgid;
		result->i_blocks = 1;
		result->i_size = strlen(symlink);
		result->i_op = &page_symlink_inode_operations;
		result->i_data.a_ops = &hpfs_symlink_aops;
		if ((fnode = hpfs_map_fnode(dir->i_sb, fno, &bh))) {
			hpfs_set_ea(result, fnode, "SYMLINK", (char *)symlink, strlen(symlink));
			mark_buffer_dirty(bh);
			brelse(bh);
		}
		hpfs_write_inode_nolock(result);
		d_instantiate(dentry, result);
	}
	hpfs_unlock_iget(dir->i_sb);
	hpfs_unlock_inode(dir);
	return 0;
	bail1:
	brelse(bh);
	hpfs_free_sectors(dir->i_sb, fno, 1);
	hpfs_unlock_inode(dir);
	bail:
	return -ENOSPC;
}

int hpfs_unlink(struct inode *dir, struct dentry *dentry)
{
	const char *name = dentry->d_name.name;
	unsigned len = dentry->d_name.len;
	struct quad_buffer_head qbh;
	struct hpfs_dirent *de;
	struct inode *inode = dentry->d_inode;
	dnode_secno dno;
	fnode_secno fno;
	int r;
	int rep = 0;
	hpfs_adjust_length((char *)name, &len);
	again:
	hpfs_lock_2inodes(dir, inode);
	if (!(de = map_dirent(dir, dir->i_hpfs_dno, (char *)name, len, &dno, &qbh))) {
		hpfs_unlock_2inodes(dir, inode);
		return -ENOENT;
	}
	if (de->first) {
		hpfs_brelse4(&qbh);
		hpfs_unlock_2inodes(dir, inode);
		return -EPERM;
	}
	if (de->directory) {
		hpfs_brelse4(&qbh);
		hpfs_unlock_2inodes(dir, inode);
		return -EISDIR;
	}
	fno = de->fnode;
	if ((r = hpfs_remove_dirent(dir, dno, de, &qbh, 1)) == 1) hpfs_error(dir->i_sb, "there was error when removing dirent");
	if (r != 2) {
		inode->i_nlink--;
		hpfs_unlock_2inodes(dir, inode);
	} else {	/* no space for deleting, try to truncate file */
		struct iattr newattrs;
		int err;
		hpfs_unlock_2inodes(dir, inode);
		if (rep)
			goto ret;
		d_drop(dentry);
		if (atomic_read(&dentry->d_count) > 1 ||
		    permission(inode, MAY_WRITE) ||
		    get_write_access(inode)) {
			d_rehash(dentry);
			goto ret;
		}
		/*printk("HPFS: truncating file before delete.\n");*/
		down(&inode->i_sem);
		newattrs.ia_size = 0;
		newattrs.ia_valid = ATTR_SIZE | ATTR_CTIME;
		err = notify_change(dentry, &newattrs);
		up(&inode->i_sem);
		put_write_access(inode);
		if (err)
			goto ret;
		rep = 1;
		goto again;
	}
	ret:
	return r == 2 ? -ENOSPC : r == 1 ? -EFSERROR : 0;
}

int hpfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	const char *name = dentry->d_name.name;
	unsigned len = dentry->d_name.len;
	struct quad_buffer_head qbh;
	struct hpfs_dirent *de;
	struct inode *inode = dentry->d_inode;
	dnode_secno dno;
	fnode_secno fno;
	int n_items = 0;
	int r;
	hpfs_adjust_length((char *)name, &len);
	hpfs_lock_2inodes(dir, inode);
	if (!(de = map_dirent(dir, dir->i_hpfs_dno, (char *)name, len, &dno, &qbh))) {
		hpfs_unlock_2inodes(dir, inode);
		return -ENOENT;
	}	
	if (de->first) {
		hpfs_brelse4(&qbh);
		hpfs_unlock_2inodes(dir, inode);
		return -EPERM;
	}
	if (!de->directory) {
		hpfs_brelse4(&qbh);
		hpfs_unlock_2inodes(dir, inode);
		return -ENOTDIR;
	}
	hpfs_count_dnodes(dir->i_sb, inode->i_hpfs_dno, NULL, NULL, &n_items);
	if (n_items) {
		hpfs_brelse4(&qbh);
		hpfs_unlock_2inodes(dir, inode);
		return -ENOTEMPTY;
	}
	fno = de->fnode;
	if ((r = hpfs_remove_dirent(dir, dno, de, &qbh, 1)) == 1)
		hpfs_error(dir->i_sb, "there was error when removing dirent");
	if (r != 2) {
		dir->i_nlink--;
		inode->i_nlink = 0;
		hpfs_unlock_2inodes(dir, inode);
	} else hpfs_unlock_2inodes(dir, inode);
	return r == 2 ? -ENOSPC : r == 1 ? -EFSERROR : 0;
}

int hpfs_symlink_readpage(struct file *file, struct page *page)
{
	char *link = kmap(page);
	struct inode *i = page->mapping->host;
	struct fnode *fnode;
	struct buffer_head *bh;
	int err;

	err = -EIO;
	lock_kernel();
	if (!(fnode = hpfs_map_fnode(i->i_sb, i->i_ino, &bh)))
		goto fail;
	err = hpfs_read_ea(i->i_sb, fnode, "SYMLINK", link, PAGE_SIZE);
	brelse(bh);
	if (err)
		goto fail;
	unlock_kernel();
	SetPageUptodate(page);
	kunmap(page);
	UnlockPage(page);
	return 0;

fail:
	unlock_kernel();
	SetPageError(page);
	kunmap(page);
	UnlockPage(page);
	return err;
}
	
int hpfs_rename(struct inode *old_dir, struct dentry *old_dentry,
		struct inode *new_dir, struct dentry *new_dentry)
{
	char *old_name = (char *)old_dentry->d_name.name;
	int old_len = old_dentry->d_name.len;
	char *new_name = (char *)new_dentry->d_name.name;
	int new_len = new_dentry->d_name.len;
	struct inode *i = old_dentry->d_inode;
	struct inode *new_inode = new_dentry->d_inode;
	struct quad_buffer_head qbh, qbh1;
	struct hpfs_dirent *dep, *nde;
	struct hpfs_dirent de;
	dnode_secno dno;
	int r;
	struct buffer_head *bh;
	struct fnode *fnode;
	int err;
	if ((err = hpfs_chk_name((char *)new_name, &new_len))) return err;
	err = 0;
	hpfs_adjust_length((char *)old_name, &old_len);

	hpfs_lock_3inodes(old_dir, new_dir, i);
	
	/* Erm? Moving over the empty non-busy directory is perfectly legal */
	if (new_inode && S_ISDIR(new_inode->i_mode)) {
		err = -EINVAL;
		goto end1;
	}

	if (!(dep = map_dirent(old_dir, old_dir->i_hpfs_dno, (char *)old_name, old_len, &dno, &qbh))) {
		hpfs_error(i->i_sb, "lookup succeeded but map dirent failed");
		err = -ENOENT;
		goto end1;
	}
	copy_de(&de, dep);
	de.hidden = new_name[0] == '.';

	if (new_inode) {
		int r;
		if ((r = hpfs_remove_dirent(old_dir, dno, dep, &qbh, 1)) != 2) {
			if ((nde = map_dirent(new_dir, new_dir->i_hpfs_dno, (char *)new_name, new_len, NULL, &qbh1))) {
				new_inode->i_nlink = 0;
				copy_de(nde, &de);
				memcpy(nde->name, new_name, new_len);
				hpfs_mark_4buffers_dirty(&qbh1);
				hpfs_brelse4(&qbh1);
				goto end;
			}
			hpfs_error(new_dir->i_sb, "hpfs_rename: could not find dirent");
			err = -EFSERROR;
			goto end1;
		}
		err = r == 2 ? -ENOSPC : r == 1 ? -EFSERROR : 0;
		goto end1;
	}

	if (new_dir == old_dir) hpfs_brelse4(&qbh);

	hpfs_lock_creation(i->i_sb);
	if ((r = hpfs_add_dirent(new_dir, new_name, new_len, &de, 1))) {
		hpfs_unlock_creation(i->i_sb);
		if (r == -1) hpfs_error(new_dir->i_sb, "hpfs_rename: dirent already exists!");
		err = r == 1 ? -ENOSPC : -EFSERROR;
		if (new_dir != old_dir) hpfs_brelse4(&qbh);
		goto end1;
	}
	
	if (new_dir == old_dir)
		if (!(dep = map_dirent(old_dir, old_dir->i_hpfs_dno, (char *)old_name, old_len, &dno, &qbh))) {
			hpfs_unlock_creation(i->i_sb);
			hpfs_error(i->i_sb, "lookup succeeded but map dirent failed at #2");
			err = -ENOENT;
			goto end1;
		}

	if ((r = hpfs_remove_dirent(old_dir, dno, dep, &qbh, 0))) {
		hpfs_unlock_creation(i->i_sb);
		hpfs_error(i->i_sb, "hpfs_rename: could not remove dirent");
		err = r == 2 ? -ENOSPC : -EFSERROR;
		goto end1;
	}
	hpfs_unlock_creation(i->i_sb);
	
	end:
	i->i_hpfs_parent_dir = new_dir->i_ino;
	if (S_ISDIR(i->i_mode)) {
		new_dir->i_nlink++;
		old_dir->i_nlink--;
	}
	if ((fnode = hpfs_map_fnode(i->i_sb, i->i_ino, &bh))) {
		fnode->up = new_dir->i_ino;
		fnode->len = new_len;
		memcpy(fnode->name, new_name, new_len>15?15:new_len);
		if (new_len < 15) memset(&fnode->name[new_len], 0, 15 - new_len);
		mark_buffer_dirty(bh);
		brelse(bh);
	}
	i->i_hpfs_conv = i->i_sb->s_hpfs_conv;
	hpfs_decide_conv(i, (char *)new_name, new_len);
	end1:
	hpfs_unlock_3inodes(old_dir, new_dir, i);
	return err;
}
