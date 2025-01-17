/*
 * fcache.c
 *
 * File cache: read/write operations are grouped together in chunks
 * and done together.
 */

#ifdef MODVERSIONS
#include <linux/modversions.h>
#endif

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/vmalloc.h>

#include <linux/shfs_fs.h>
#include <linux/shfs_fs_sb.h>
#include <linux/shfs_fs_i.h>
#include "shfs_debug.h"

#define SHFS_FCACHE_READ  	0
#define SHFS_FCACHE_WRITE 	1

struct shfs_file {
	int		type:2;		/* why does gcc complain for 1-bit? */
	int		new:1;
	off_t		offset;
	unsigned long	count;
	char          	*data;
};

kmem_cache_t	*file_cache = NULL;

void
fcache_init(void)
{
	file_cache = kmem_cache_create("shfs_file", sizeof(struct shfs_file), 0, 0, NULL, NULL);
	DEBUG("file_cache: %p\n", file_cache);
}

void
fcache_finish(void)
{
	kmem_cache_destroy(file_cache);
}

static struct shfs_file *
alloc_fcache(struct shfs_sb_info *info)
{
	struct shfs_file *cache;

	spin_lock(&info->fcache_lock);
	if (info->fcache_free <= 0) {
		spin_unlock(&info->fcache_lock);
		return NULL;
	}
	info->fcache_free--;
	spin_unlock(&info->fcache_lock);
				
	cache = (struct shfs_file *)KMEM_ALLOC("fcache", file_cache, GFP_KERNEL);
	if (!cache)
		return NULL;
	cache->data = vmalloc(info->fcache_size);
	if (!cache->data) {
		KMEM_FREE("fcache", file_cache, cache);
		return NULL;
	}
	cache->type = SHFS_FCACHE_READ;
	cache->new = 1;
	cache->offset = 0;
	cache->count = 0;

	return cache;
}

static void
free_fcache(struct shfs_sb_info *info, struct shfs_file *cache)
{
	DEBUG("release\n");
	vfree(cache->data);
	KMEM_FREE("fcache", file_cache, cache);

	spin_lock(&info->fcache_lock);
	info->fcache_free++;
	spin_unlock(&info->fcache_lock);
}	

int 
fcache_file_open(struct file *f)
{
	struct inode *inode;
	struct shfs_inode_info *p;

	if (!f->f_dentry || !(inode = f->f_dentry->d_inode)) {
		VERBOSE("invalid\n");
		return -EINVAL;
	}
	DEBUG("ino: %lu\n", inode->i_ino);
	if (S_ISDIR(inode->i_mode)) {
		VERBOSE("dir in file cache?\n");
		return -EINVAL;
	}
	p = (struct shfs_inode_info *)inode->u.generic_ip;
	if (!p) {
		VERBOSE("inode without info\n");
		return -EINVAL;
	}
	if (!p->cache)
		p->cache = alloc_fcache(info_from_dentry(f->f_dentry));
	return 0;
}

int 
fcache_file_sync(struct file *f)
{
	struct inode *inode;
	struct shfs_inode_info *p;
	struct shfs_file *cache;
	int result;

	if (!f->f_dentry || !(inode = f->f_dentry->d_inode)) {
		VERBOSE("invalid\n");
		return -EINVAL;
	}
	DEBUG("ino: %lu\n", inode->i_ino);
	if (S_ISDIR(inode->i_mode)) {
		VERBOSE("dir in file cache?\n");
		return -EINVAL;
	}
	p = (struct shfs_inode_info *)inode->u.generic_ip;
	if (!p) {
		VERBOSE("inode without info\n");
		return -EINVAL;
	}
	if (!p->cache)
		return 0;

	cache = p->cache;
	result = 0;
	if (cache->count && cache->type == SHFS_FCACHE_WRITE) {
		char name[SHFS_PATH_MAX];
		struct shfs_sb_info *info = info_from_dentry(f->f_dentry);

		DEBUG("sync\n");
		if (get_name(f->f_dentry, name) < 0)
			return -ENAMETOOLONG;
		result = info->fops.write(info, name, cache->offset, cache->count, cache->data, inode->i_ino);
		cache->type = SHFS_FCACHE_READ;
		cache->count = 0;
	}
	return result < 0 ? result : 0;
}

int 
fcache_file_close(struct file *f)
{
	int result;

	result = fcache_file_sync(f);
	if (result == 0) {
		struct shfs_inode_info *p;

		p = (struct shfs_inode_info *)f->f_dentry->d_inode->u.generic_ip;
		if (!p) {
			VERBOSE("inode without info\n");
			return -EINVAL;
		}
		if (!p->cache)
			return 0;
		free_fcache(info_from_dentry(f->f_dentry), p->cache);
		p->cache = NULL;
	}
	return result < 0 ? result : 0;
}

int
fcache_file_clear(struct inode *inode)
{
	struct shfs_inode_info *p;
	struct shfs_file *cache;

	if (!inode || S_ISDIR(inode->i_mode)) {
		DEBUG("invalid\n");
		return -EINVAL;
	}
	DEBUG("ino: %lu\n", inode->i_ino);
	p = (struct shfs_inode_info *)inode->u.generic_ip;
	if (!p) {
		VERBOSE("inode without info\n");
		return -EINVAL;
	}
	if (!p->cache)
		return 0;

	cache = p->cache;
	if (cache->count) {
		if (cache->type == SHFS_FCACHE_WRITE) {
			VERBOSE("Aieeee, out of sync\n");
		} else {
			cache->count = 0;
		}
	}
	return 0;
}

int 
fcache_file_read(struct file *f, unsigned offset, unsigned count, char *buffer)
{
	char name[SHFS_PATH_MAX];
	struct shfs_sb_info *info;
	unsigned readahead, c, o, x, y, z, read = 0;
	struct inode *inode;
	struct shfs_inode_info *p;
	struct shfs_file *cache;
	int result = 0;

	DEBUG("[%u, %u]\n", offset, count);
	if (!f->f_dentry || !(inode = f->f_dentry->d_inode)) {
		VERBOSE("invalid\n");
		return -EINVAL;
	}
	if (get_name(f->f_dentry, name) < 0)
		return -ENAMETOOLONG;
	DEBUG("ino: %lu\n", inode->i_ino);
	if (S_ISDIR(inode->i_mode)) {
		VERBOSE("dir in file cache?\n");
		return -EINVAL;
	}
	p = (struct shfs_inode_info *)inode->u.generic_ip;
	if (!p) {
		VERBOSE("inode without info\n");
		return -EINVAL;
			return result;
	}
	info = info_from_dentry(f->f_dentry);
	if (!p->cache) {
		p->cache = alloc_fcache(info);
		if (!p->cache)
			return info->fops.read(info, name, offset, count, buffer, 0);
	}

	cache = p->cache;
	/* hit? */
	if (offset >= cache->offset && offset < (cache->offset + cache->count)) {
		o = offset - cache->offset;
		c = count > cache->count - o ? cache->count - o : count;
		memcpy(buffer, cache->data + o, c);
		buffer += c;
		offset += c;
		count -= c;
		read += c;
	}

	if (count && cache->type == SHFS_FCACHE_WRITE) {
		info->fops.write(info, name, cache->offset, cache->count, cache->data, inode->i_ino);
		cache->type = SHFS_FCACHE_READ;
		cache->offset = offset;
		cache->count = 0;
	}

	while (count) {
		o = offset & PAGE_MASK;
		x = offset - o;
		if (cache->offset + cache->count == offset)
			readahead = cache->count;
		else {
			cache->offset = o;
			cache->count = 0;
			readahead = 0;
		}
		if (readahead < (x + count))
			readahead = x + count;
		readahead = (readahead+PAGE_SIZE-1) & PAGE_MASK;
		if (readahead > info->fcache_size)
			readahead = info->fcache_size;
		if (o % readahead) {	 /* HD */
			y = o, z = readahead;
			while (y && z)
				if (y > z) y %= z; else z %= y;
			readahead = y > z ? y : z;
		}
		if (cache->count + readahead > info->fcache_size) {
			cache->offset = o;
			cache->count = 0;
		}
		DEBUG("[%u, %u]\n", o, readahead);
		result = info->fops.read(info, name, o, readahead, cache->data+cache->count, 0);
		if (result < 0) {
			cache->count = 0;
			return result;
		}

		c = result - x;
		if (c > count)
			c = count;
		memcpy(buffer, cache->data + cache->count + x, c);
		buffer += c;
		offset += c;
		count -= c;
		read += c;
		cache->count += result;
	}
	return read;
}

int 
fcache_file_write(struct file *f, unsigned offset, unsigned count, char *buffer)
{
	struct dentry *dentry = f->f_dentry;
	char name[SHFS_PATH_MAX];
	struct shfs_sb_info *info;
	unsigned o, c, wrote = 0;
	struct inode *inode;
	struct shfs_inode_info *p;
	struct shfs_file *cache;
	int result = 0;

	DEBUG("[%u, %u]\n", offset, count);
	if (!(inode = dentry->d_inode)) {
		VERBOSE("invalid\n");
		return -EINVAL;
	}
	if (get_name(dentry, name) < 0)
		return -ENAMETOOLONG;
	DEBUG("ino: %lu\n", inode->i_ino);
	if (S_ISDIR(inode->i_mode)) {
		VERBOSE("dir in file cache?\n");
		return -EINVAL;
	}
	p = (struct shfs_inode_info *)inode->u.generic_ip;
	if (!p) {
		VERBOSE("inode without info\n");
		return -EINVAL;
	}
	info = info_from_dentry(dentry);
	if (!p->cache) {
		p->cache = alloc_fcache(info);
		if (!p->cache)
			return info->fops.write(info, name, offset, count, buffer, inode->i_ino);
	}

	cache = p->cache;
	if (cache->type == SHFS_FCACHE_READ) {
		cache->offset = offset;
		cache->count = 0;
		cache->type = SHFS_FCACHE_WRITE;
	}

	DEBUG("1 [%lu, %lu]\n", cache->offset, cache->count);
	if (offset >= cache->offset && offset < (cache->offset + cache->count)) {
		o = offset - cache->offset;
		c = count > info->fcache_size - o ? info->fcache_size - o : count;
		memcpy(cache->data + o, buffer, c);
		if (o + c > cache->count)
			cache->count = o + c;
		buffer += c;
		offset += c;
		count -= c;
		wrote += c;
	}
	DEBUG("2 [%lu, %lu]\n", cache->offset, cache->count);
	if (count && offset == cache->offset+cache->count) {
		o = offset - cache->offset;
		c = count > info->fcache_size - cache->count ? info->fcache_size - cache->count : count;
		memcpy(cache->data + o, buffer, c);
		cache->count += c;
		buffer += c;
		offset += c;
		count -= c;
		wrote += c;
	}
	DEBUG("3 [%lu, %lu]\n", cache->offset, cache->count);
	while (count) {
		result = info->fops.write(info, name, cache->offset, cache->count, cache->data, inode->i_ino);
		if (result < 0)
			break;
		c = count > info->fcache_size ? info->fcache_size : count;
		memcpy(cache->data, buffer, c);
		cache->offset = offset;
		cache->count = c;
		buffer += c;
		offset += c;
		count -= c;
		wrote += c;
	}
	DEBUG("4 [%lu, %lu]\n", cache->offset, cache->count);
	if (cache->new && wrote) {
		result = info->fops.write(info, name, cache->offset, 1, cache->data, inode->i_ino);
		cache->new = 0;
	}

	return result >= 0 ? wrote : result;
}
