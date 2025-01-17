#ifdef MODVERSIONS
#include <linux/modversions.h>
#endif

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <asm/uaccess.h>
#include <asm/fcntl.h>
#include <linux/smp_lock.h>
#include <linux/stat.h>

#include <linux/shfs_fs.h>
#include <linux/shfs_fs_sb.h>
#include <linux/shfs_fs_i.h>
#include "shfs_debug.h"
#include "proc.h"

static int
shfs_file_readpage(struct file *f, struct page *p)
{
	struct dentry *dentry = f->f_dentry;
	struct shfs_sb_info *info = info_from_dentry(dentry);
	char *buffer;
	unsigned long offset, count;
	int result;
	
	get_page(p);

	buffer = kmap(p);
	offset = p->index << PAGE_CACHE_SHIFT;
	count = PAGE_SIZE;

	lock_kernel();
	do {
		DEBUG("\n");
		if (info->fcache_size) {
			result = fcache_file_read(f, offset, count, buffer);
		} else {
			char name[SHFS_PATH_MAX];
			if (get_name(f->f_dentry, name) < 0) {
				unlock_kernel();
				result = -ENAMETOOLONG;
				goto io_error;
			}
			result = info->fops.read(info, name, offset, count, buffer, 0);
		}
		if (result < 0) {
			VERBOSE("!%d\n", result);
			unlock_kernel();
			goto io_error;
		}
		count -= result;
		offset += result;
		buffer += result;
		dentry->d_inode->i_atime = CURRENT_TIME;
		ROUND_TO_MINS(dentry->d_inode->i_atime);
		if (!result)
			break;
	} while (count);

	unlock_kernel();
	memset(buffer, 0, count);
	flush_dcache_page(p);
	SetPageUptodate(p);
	result = 0;
io_error:
	kunmap(p);
	UnlockPage(p);	
	put_page(p);
	return result;
}

static int
shfs_file_writepage(struct page *p)
{
	return -EFAULT;
}

static int
shfs_file_preparewrite(struct file *f, struct page *p, unsigned offset, unsigned to)
{
	DEBUG("[%p, %u, %u]\n", p, offset, to);
	return 0;
}

static int
shfs_file_commitwrite(struct file *f, struct page *p, unsigned offset, unsigned to)
{
	struct dentry *dentry = f->f_dentry;
	struct shfs_sb_info *info = info_from_dentry(dentry);
	struct inode *inode = p->mapping->host;
	struct shfs_inode_info *i = (struct shfs_inode_info *)inode->u.generic_ip;
	char *buffer = kmap(p) + offset;
	int written = 0, result;
	unsigned count = to - offset;
	time_t time;
	
	DEBUG("[%p, %u, %u]\n", p, offset, to);
	offset += p->index << PAGE_CACHE_SHIFT;
	lock_kernel();
	if (info->readonly) {
		result = -EROFS;
		goto error;
	}

	do {
		if (info->fcache_size) {
			result = fcache_file_write(f, offset, count, buffer);
		} else {
			char name[SHFS_PATH_MAX];
			if (get_name(dentry, name) < 0) {
				result = -ENAMETOOLONG;
				goto error;
			}
			result = info->fops.write(info, name, offset, count, buffer, dentry->d_inode->i_ino);
		}
		if (result < 0) {
			VERBOSE("!%d\n", result);
			goto error;
		}
		count -= result;
		offset += result;
		buffer += result;
		written += result;
		if (!result)
			break;

	} while (count);

	memset(buffer, 0, count);
	result = 0;
error:
	unlock_kernel();
	kunmap(p);

	DEBUG("[%u, %lld]\n", offset, inode->i_size);
	time = CURRENT_TIME;
	ROUND_TO_MINS(time);
	if (time != inode->i_mtime) {
		inode->i_atime = inode->i_mtime = time;
		if (i)
			i->oldmtime = 0;        /* force inode reload */
	}
	if (offset > inode->i_size) {
		inode->i_size = offset;
		if (i)
			i->oldmtime = 0;        /* force inode reload */
	}
	return result;
}

static int
shfs_file_permission(struct inode *inode, int mask)
{
	int mode = inode->i_mode;

	mode >>= 6;
	if ((mode & 7 & mask) != mask)
		return -EACCES;
	return 0;
}

static int
shfs_file_open(struct inode *inode, struct file *f)
{
	struct dentry *dentry = f->f_dentry;
	struct shfs_sb_info *info = info_from_dentry(dentry);
	int mode = f->f_flags & O_ACCMODE;
	char name[SHFS_PATH_MAX];
	int result = 0;

	DEBUG("%s\n", dentry->d_name.name);
	if ((mode == O_WRONLY || mode == O_RDWR) && info->readonly)
		return -EROFS;
	if (!get_name(dentry, name))
		return -ENAMETOOLONG;

	result = info->fops.open(info, name, mode);

	switch (result) {
	case 1:			/* install special read handler for this file */
		if (dentry->d_inode)
			f->f_op = &shfs_slow_operations;
		result = 0;
		/* fallthrough */
	case 0:
		if (info->fcache_size)
			fcache_file_open(f);
		break;
	default:		/* error */
		DEBUG("!%d\n", result);
		break;
	}
	if (result >= 0)
		shfs_renew_times(dentry);

	return result;
}

static int
shfs_file_flush(struct file *f)
{
	struct dentry *dentry = f->f_dentry;
	struct shfs_sb_info *info = info_from_dentry(dentry);
	int result = 0;

	DEBUG("%s\n", dentry->d_name.name);
	if (info->fcache_size) {
		result = fcache_file_sync(f);
		if (result < 0)
			shfs_invalid_dir_cache(dentry->d_parent->d_inode);
		if (!result) {
			/* last reference */
			lock_kernel();
			filemap_fdatasync(dentry->d_inode->i_mapping);
			filemap_fdatawait(dentry->d_inode->i_mapping);
			unlock_kernel();
		}
	}
	return result < 0 ? result : 0;
}

static int
shfs_file_release(struct inode *inode, struct file *f)
{
	struct dentry *dentry = f->f_dentry;
	struct shfs_sb_info *info = info_from_dentry(dentry);
	int result;

	DEBUG("%s\n", dentry->d_name.name);
	if (info->fcache_size) {
		result = fcache_file_close(f);
		if (result < 0)
			shfs_invalid_dir_cache(dentry->d_parent->d_inode);
		if (!result) {
			/* last reference */
			lock_kernel();
			filemap_fdatasync(dentry->d_inode->i_mapping);
			filemap_fdatawait(dentry->d_inode->i_mapping);
			unlock_kernel();
		}
	}
	/* if file was forced to be writeable, change attrs back on close */
	if (dentry->d_inode && dentry->d_inode->u.generic_ip) {
		if  (((struct shfs_inode_info *)dentry->d_inode->u.generic_ip)->unset_write_on_close) {
			char name[SHFS_PATH_MAX];

			if (get_name(dentry, name) < 0)
				goto out;
			result = info->fops.chmod(info, name, dentry->d_inode->i_mode & ~S_IWUSR);
			if (result < 0)
				goto out;
			inode->i_mode &= ~S_IWUSR;
		}
	}
out:
	return 0;	/* ignored */
}

static int
shfs_file_sync(struct file *f, struct dentry *dentry, int datasync)
{
	return 0;
}

static ssize_t 
shfs_slow_read(struct file *f, char *buf, size_t count, loff_t *ppos)
{
	struct dentry *dentry = f->f_dentry;
	struct shfs_sb_info *info = info_from_dentry(dentry);
	char name[SHFS_PATH_MAX];
	unsigned long page;
	int result;
	
	DEBUG("%s\n", dentry->d_name.name);

	lock_kernel();
	page = __get_free_page(GFP_KERNEL);
	if (!page) {
		result = -ENOMEM;
		goto out;
	}
	if (get_name(dentry, name) < 0) {
		result = -ENAMETOOLONG;
		goto error;
	}
	count = count > PAGE_SIZE ? PAGE_SIZE : count;
	result = info->fops.read(info, name, *ppos, count, (char *)page, dentry->d_inode->i_ino);
	if (result < 0) {
		VERBOSE("!%d\n", result);
		goto error;
	}
	if (result != 0) {
		copy_to_user(buf, (char *)page, result);
		*ppos += result;
	}
error:
	free_page(page);
out:
	unlock_kernel();
	return result;
}

static ssize_t 
shfs_slow_write(struct file *f, const char *buf, size_t count, loff_t *offset)
{
	int written = 0;
	int result;
	
	DEBUG("\n");
	written = generic_file_write(f, buf, count, offset);
	if (written > 0) {
		result = shfs_file_flush(f);
		written = result < 0 ? result: written;
	}
	
	return written;
}

struct file_operations shfs_file_operations = {
	llseek:		generic_file_llseek,
	read:		generic_file_read,
	write:		generic_file_write,
	ioctl:		shfs_ioctl,
	mmap:		generic_file_mmap,
	open:		shfs_file_open,
	flush:		shfs_file_flush,
	release:	shfs_file_release,
	fsync:		shfs_file_sync,
};

struct file_operations shfs_slow_operations = {
	llseek:		generic_file_llseek,
	read:		shfs_slow_read,
	write:		shfs_slow_write,
	ioctl:		shfs_ioctl,
	mmap:		generic_file_mmap,
	open:		shfs_file_open,
	flush:		shfs_file_flush,
	release:	shfs_file_release,
	fsync:		shfs_file_sync,
};

struct inode_operations shfs_file_inode_operations = {
	permission:	shfs_file_permission,
	revalidate:	shfs_revalidate_inode,
	setattr:	shfs_notify_change,
};

struct address_space_operations shfs_file_aops = {
	readpage:	shfs_file_readpage,
	writepage:	shfs_file_writepage,
	sync_page:	block_sync_page,
	prepare_write:	shfs_file_preparewrite,
	commit_write:	shfs_file_commitwrite
};

