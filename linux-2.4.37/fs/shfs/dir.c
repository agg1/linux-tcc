/*
 * dcache.c
 *
 * Directory & dentry operations, partialy inspired by smbfs/ncpfs.
 */

#ifdef MODVERSIONS
#include <linux/modversions.h>
#endif

#include <linux/kernel.h>
#include <linux/module.h>
#include <asm/uaccess.h>
#include <linux/smp_lock.h>
#include <linux/stat.h>

#include <linux/shfs_fs.h>
#include <linux/shfs_fs_i.h>
#include "shfs_debug.h"
#include "proc.h"

static int
shfs_dir_open(struct inode *inode, struct file *filp)
{
	struct dentry *dentry = filp->f_dentry;
	int result = 0;

	DEBUG("%s\n", dentry->d_name.name);
	/* always open root dir (ioctl()) */
	if (IS_ROOT(dentry))
		return 0;
	result = shfs_revalidate_inode(dentry);
	if (result < 0)
		VERBOSE("failed (%d)\n", result);
	return result;
}

/*
 * Read a directory, using filldir to fill the dirent memory.
 * info->fops.readdir does the actual reading from the shfs server.
 *
 * The cache code is almost directly taken from shfs/ncpfs
 */
static int 
shfs_readdir(struct file *filp, void *dirent, filldir_t filldir)
{
	struct dentry *dentry = filp->f_dentry;
	struct shfs_sb_info *info = info_from_dentry(dentry);
	struct inode *dir = dentry->d_inode;
	char name[SHFS_PATH_MAX];
	union shfs_dir_cache *cache = NULL;
	struct shfs_cache_control ctl;
	struct page *page = NULL;
	int result;

	ctl.page = NULL;
	ctl.cache = NULL;

	DEBUG("%s (%d)\n", dentry->d_name.name, (unsigned int)filp->f_pos);

	result = 0;
	switch ((unsigned int) filp->f_pos) {
	case 0:
		DEBUG("f_pos=0\n");
		if (filldir(dirent, ".", 1, 0, dir->i_ino, DT_DIR) < 0)
			goto out;
		filp->f_pos = 1;
		/* fallthrough */
	case 1:
		DEBUG("f_pos=1\n");
		if (filldir(dirent, "..", 2, 1, dentry->d_parent->d_inode->i_ino, DT_DIR) < 0)
			goto out;
		filp->f_pos = 2;
	}

	/*
	 * Make sure our inode is up-to-date.
	 */
	result = shfs_revalidate_inode(dentry);
	if (result)
		goto out;


	page = grab_cache_page(&dir->i_data, 0);
	if (!page)
		goto read_really;

	ctl.cache = cache = kmap(page);
	ctl.head = cache->head;

	if (!Page_Uptodate(page) || !ctl.head.eof) {
		DEBUG("%s, page uptodate=%d, eof=%d\n", dentry->d_name.name, Page_Uptodate(page), ctl.head.eof);
		goto init_cache;
	}

	if (filp->f_pos == 2) {
		if (jiffies - ctl.head.time >= SHFS_MAX_AGE(info))
			goto init_cache;
	}

	if (filp->f_pos > ctl.head.end)
		goto finished;

	ctl.fpos = filp->f_pos + (SHFS_DIRCACHE_START - 2);
	ctl.ofs  = ctl.fpos / SHFS_DIRCACHE_SIZE;
	ctl.idx  = ctl.fpos % SHFS_DIRCACHE_SIZE;

	for (;;) {
		if (ctl.ofs != 0) {
			ctl.page = find_lock_page(&dir->i_data, ctl.ofs);
			if (!ctl.page)
				goto invalid_cache;
			ctl.cache = kmap(ctl.page);
			if (!Page_Uptodate(ctl.page))
				goto invalid_cache;
		}
		while (ctl.idx < SHFS_DIRCACHE_SIZE) {
			struct dentry *dent;
			int res;

			dent = shfs_dget_fpos(ctl.cache->dentry[ctl.idx],
					     dentry, filp->f_pos);
			if (!dent)
				goto invalid_cache;
			res = filldir(dirent, dent->d_name.name,
				      dent->d_name.len, filp->f_pos,
				      dent->d_inode->i_ino, DT_UNKNOWN);
			dput(dent);
			if (res)
				goto finished;
			filp->f_pos += 1;
			ctl.idx += 1;
			if (filp->f_pos > ctl.head.end)
				goto finished;
		}
		if (ctl.page) {
			kunmap(ctl.page);
			SetPageUptodate(ctl.page);
			UnlockPage(ctl.page);
			page_cache_release(ctl.page);
			ctl.page = NULL;
		}
		ctl.idx  = 0;
		ctl.ofs += 1;
	}
invalid_cache:
	if (ctl.page) {
		kunmap(ctl.page);
		UnlockPage(ctl.page);
		page_cache_release(ctl.page);
		ctl.page = NULL;
	}
	ctl.cache = cache;
init_cache:
	shfs_invalidate_dircache_entries(dentry);
	ctl.head.time = jiffies;
	ctl.head.eof = 0;
	ctl.fpos = 2;
	ctl.ofs = 0;
	ctl.idx = SHFS_DIRCACHE_START;
	ctl.filled = 0;
	ctl.valid  = 1;
read_really:
	result = -ENAMETOOLONG;
	if (get_name(dentry, name) < 0)
		goto out;
	result = info->fops.readdir(info, name, filp, dirent, filldir, &ctl);
	if (ctl.idx == -1)
		goto invalid_cache;	/* retry */
	ctl.head.end = ctl.fpos - 1;
	ctl.head.eof = ctl.valid;
finished:
	if (page) {
		cache->head = ctl.head;
		kunmap(page);
		SetPageUptodate(page);
		UnlockPage(page);
		page_cache_release(page);
	}
	if (ctl.page) {
		kunmap(ctl.page);
		SetPageUptodate(ctl.page);
		UnlockPage(ctl.page);
		page_cache_release(ctl.page);
	}
out:
	return result;
}

/*
 * shouldn't be called too often since we instantiate dentry
 * in fill_cache()
 */
static struct dentry*
shfs_lookup(struct inode *dir, struct dentry *dentry)
{
	struct shfs_sb_info *info = info_from_dentry(dentry);
	char name[SHFS_PATH_MAX];
	struct shfs_fattr fattr;
	struct inode *inode;
	int result;
	
	DEBUG("%s\n", dentry->d_name.name);
	if (get_name(dentry, name) < 0)
		return ERR_PTR(-ENAMETOOLONG);

	result = info->fops.stat(info, name, &fattr);
	if (result < 0) {
		DEBUG("!%d\n", result);
		dentry->d_op = &shfs_dentry_operations;
		d_add(dentry, NULL);
		shfs_renew_times(dentry);
		if (result == -EINTR)
			return ERR_PTR(result);
		return NULL; 
	}

	fattr.f_ino = iunique(dentry->d_sb, 2);
	inode = shfs_iget(dir->i_sb, &fattr);
	if (inode) {
		dentry->d_op = &shfs_dentry_operations;
		d_add(dentry, inode);
		shfs_renew_times(dentry);
	}
	
	return NULL; 
}

static int
shfs_instantiate(struct dentry *dentry)
{
	struct shfs_sb_info *info = info_from_dentry(dentry);
	struct shfs_fattr fattr;
	struct inode *inode;
	char name[SHFS_PATH_MAX];
	int result;
	
	DEBUG("%s\n", dentry->d_name.name);
	if (get_name(dentry, name) < 0)
		return -ENAMETOOLONG;

	result = info->fops.stat(info, name, &fattr);
	if (result < 0) {
		VERBOSE("!%d\n", result);
		goto out;
	}

	shfs_renew_times(dentry);
	fattr.f_ino = iunique(dentry->d_sb, 2);
	inode = shfs_iget(dentry->d_sb, &fattr);
	result = -EACCES;
	if (!inode)
		goto out;
	dentry->d_op = &shfs_dentry_operations;
	d_instantiate(dentry, inode);
	result = 0;
out:
	return result;	
}

static int
shfs_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	struct shfs_sb_info *info = info_from_dentry(dentry);
	char name[SHFS_PATH_MAX];
	int result;
	
	if (info->readonly)
		return -EROFS;
	if (get_name(dentry, name) < 0)
		return -ENAMETOOLONG;

	result = info->fops.mkdir(info, name);
	if (result < 0)
		return result;

	shfs_invalid_dir_cache(dir);
	return shfs_instantiate(dentry);
}

static int
shfs_create(struct inode* dir, struct dentry *dentry, int mode)
{
	struct shfs_sb_info *info = info_from_dentry(dentry);
	char name[SHFS_PATH_MAX];
	int result, forced_write = 0;
	
	if (info->readonly)
		return -EROFS;
	if (get_name(dentry, name) < 0)
		return -ENAMETOOLONG;

	if (!(mode & S_IWUSR)) {
		forced_write = 1;
		mode |= S_IWUSR;
	}
	result = info->fops.create(info, name, mode);
	if (result < 0)
		return result;
	
	shfs_invalid_dir_cache(dir);
	result = shfs_instantiate(dentry);
	if (forced_write && dentry->d_inode && dentry->d_inode->u.generic_ip)
		((struct shfs_inode_info *)dentry->d_inode->u.generic_ip)->unset_write_on_close = 1;
	return result;
}

static int
shfs_rmdir(struct inode* dir, struct dentry *dentry)
{
	struct shfs_sb_info *info = info_from_dentry(dentry);
	char name[SHFS_PATH_MAX];
	int result;

	if (info->readonly)
		return -EROFS;
	if (!d_unhashed(dentry))
		return -EBUSY;
	if (get_name(dentry, name) < 0)
		return -ENAMETOOLONG;

	result = info->fops.rmdir(info, name);
	if (!result)
		shfs_renew_times(dentry);
	shfs_invalid_dir_cache(dir);
	return result;
}

static int
shfs_rename(struct inode *old_dir, struct dentry *old_dentry, struct inode *new_dir, struct dentry *new_dentry)
{
	struct shfs_sb_info *info = info_from_dentry(old_dentry);
	char old[SHFS_PATH_MAX], new[SHFS_PATH_MAX];
	int result;
	
	if (info->readonly)
		return -EROFS;

	if (get_name(old_dentry, old) < 0)
		return -ENAMETOOLONG;
	if (get_name(new_dentry, new) < 0)
		return -ENAMETOOLONG;
	if (new_dentry->d_inode)
		d_delete(new_dentry);
	
	result = info->fops.rename(info, old, new);
	if (!result) {
		shfs_renew_times(old_dentry);
		shfs_renew_times(new_dentry);
	}
	shfs_invalid_dir_cache(old_dir);
	shfs_invalid_dir_cache(new_dir);
	return result;
}

static int
shfs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct shfs_sb_info *info = info_from_dentry(dentry);
	char name[SHFS_PATH_MAX];
	int result;

	if (info->readonly)
		return -EROFS;
	if (get_name(dentry, name) < 0)
		return -ENAMETOOLONG;

	result = info->fops.unlink(info, name);
	if (!result)
		shfs_renew_times(dentry);
	shfs_invalid_dir_cache(dir);
	return result;
}

static int
shfs_link(struct dentry *old_dentry, struct inode *dir, struct dentry *new_dentry)
{
	struct shfs_sb_info *info = info_from_dentry(old_dentry);
	char old[SHFS_PATH_MAX], new[SHFS_PATH_MAX];
	int result;

	if (info->readonly)
		return -EROFS;

	if (get_name(old_dentry, old) < 0)
		return -ENAMETOOLONG;
	if (get_name(new_dentry, new) < 0)
		return -ENAMETOOLONG;
	
	result = info->fops.link(info, old, new);
	if (result < 0)
		return result;
	shfs_invalid_dir_cache(dir);
	return shfs_instantiate(new_dentry);
}

static int
shfs_symlink(struct inode *dir, struct dentry *dentry, const char *oldname)
{
	struct shfs_sb_info *info = info_from_dentry(dentry);
	char old[SHFS_PATH_MAX], new[SHFS_PATH_MAX];
	int result;

	if (info->readonly)
		return -EROFS;

	if (get_name(dentry, new) < 0)
		return -ENAMETOOLONG;
	if (strlen(oldname) + 1 > SHFS_PATH_MAX)
		return -ENAMETOOLONG;
	strcpy(old, oldname);
	
	result = info->fops.symlink(info, old, new);
	if (result < 0)
		return result;

	shfs_invalid_dir_cache(dir);
	return shfs_instantiate(dentry);
}

/*
 * Initialize a new dentry
 */
void
shfs_new_dentry(struct dentry *dentry)
{
	dentry->d_op = &shfs_dentry_operations;
	dentry->d_time = jiffies;
}

void
shfs_age_dentry(struct shfs_sb_info *info, struct dentry *dentry)
{
	dentry->d_time = jiffies - SHFS_MAX_AGE(info);
}

/*
 * Whenever a lookup succeeds, we know the parent directories
 * are all valid, so we want to update the dentry timestamps.
 * N.B. Move this to dcache?
 */
void
shfs_renew_times(struct dentry * dentry)
{
	for (;;) {
		dentry->d_time = jiffies;
		if (IS_ROOT(dentry))
			break;
		dentry = dentry->d_parent;
	}
}

static int
shfs_d_revalidate(struct dentry *dentry, int flags)
{
	struct shfs_sb_info *info = info_from_dentry(dentry);
	struct inode *inode = dentry->d_inode;
	unsigned long age;
	int result;

	DEBUG("%s\n", dentry->d_name.name);
	age = jiffies - dentry->d_time;
	
	result = (age <= SHFS_MAX_AGE(info));
	DEBUG("valid: %d\n", result);
	if (!inode)
		return result;	/* negative dentry */
	lock_kernel();
	if (is_bad_inode(inode))
		result = 0;
	else if (!result)
		result = (shfs_revalidate_inode(dentry) == 0);
	unlock_kernel();
	return result;
}

static int
shfs_d_delete(struct dentry *dentry)
{
	if (dentry->d_inode) {
		if (is_bad_inode(dentry->d_inode)) {
			DEBUG("bad inode, unhashing\n");
			return 1;
		}
	} else {
		/* ??? */
	}
	return 0;
}

struct dentry_operations shfs_dentry_operations = {
	d_revalidate:	shfs_d_revalidate,
	d_delete:	shfs_d_delete,
};

struct file_operations shfs_dir_operations = {
	read:		generic_read_dir,
	readdir:	shfs_readdir,
	ioctl:		shfs_ioctl,
	open:		shfs_dir_open,
};

struct inode_operations shfs_dir_inode_operations = {
	create:		shfs_create,
	lookup:		shfs_lookup,
	link:		shfs_link,
	unlink:		shfs_unlink,
	symlink:	shfs_symlink,
	mkdir:		shfs_mkdir,
	rmdir:		shfs_rmdir,
	rename:		shfs_rename,
	setattr:	shfs_notify_change,
	revalidate:	shfs_revalidate_inode,
};

