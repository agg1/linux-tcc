/*
 *  linux/fs/sysv/dir.c
 *
 *  minix/dir.c
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  coh/dir.c
 *  Copyright (C) 1993  Pascal Haible, Bruno Haible
 *
 *  sysv/dir.c
 *  Copyright (C) 1993  Bruno Haible
 *
 *  SystemV/Coherent directory handling functions
 */

#include <linux/fs.h>
#include <linux/sysv_fs.h>
#include <linux/pagemap.h>

static int sysv_readdir(struct file *, void *, filldir_t);

const struct file_operations sysv_dir_operations = {
	read:		generic_read_dir,
	readdir:	sysv_readdir,
	fsync:		sysv_sync_file,
};

static inline void dir_put_page(struct page *page)
{
	kunmap(page);
	page_cache_release(page);
}

static inline unsigned long dir_pages(struct inode *inode)
{
	return (inode->i_size+PAGE_CACHE_SIZE-1)>>PAGE_CACHE_SHIFT;
}

static int dir_commit_chunk(struct page *page, unsigned from, unsigned to)
{
	struct inode *dir = (struct inode *)page->mapping->host;
	int err = 0;

	dir->i_version = ++event;
	page->mapping->a_ops->commit_write(NULL, page, from, to);
	if (IS_SYNC(dir)) {
		int err2;
		err = writeout_one_page(page);
		err2 = waitfor_one_page(page);
		if (err == 0)
			err = err2;
	}
	return err;
}

static struct page * dir_get_page(struct inode *dir, unsigned long n)
{
	struct address_space *mapping = dir->i_mapping;
	struct page *page = read_cache_page(mapping, n,
				(filler_t*)mapping->a_ops->readpage, NULL);
	if (!IS_ERR(page)) {
		wait_on_page(page);
		kmap(page);
		if (!Page_Uptodate(page))
			goto fail;
	}
	return page;

fail:
	dir_put_page(page);
	return ERR_PTR(-EIO);
}

static int sysv_readdir(struct file * filp, void * dirent, filldir_t filldir)
{
	unsigned long pos = filp->f_pos;
	struct inode *inode = filp->f_dentry->d_inode;
	struct super_block *sb = inode->i_sb;
	unsigned offset = pos & ~PAGE_CACHE_MASK;
	unsigned long n = pos >> PAGE_CACHE_SHIFT;
	unsigned long npages = dir_pages(inode);

	pos = (pos + SYSV_DIRSIZE-1) & ~(SYSV_DIRSIZE-1);
	if (pos >= inode->i_size)
		goto done;

	for ( ; n < npages; n++, offset = 0) {
		char *kaddr, *limit;
		struct sysv_dir_entry *de;
		struct page *page = dir_get_page(inode, n);

		if (IS_ERR(page))
			continue;
		kaddr = (char *)page_address(page);
		de = (struct sysv_dir_entry *)(kaddr+offset);
		limit = kaddr + PAGE_CACHE_SIZE - SYSV_DIRSIZE;
		for ( ;(char*)de <= limit; de++) {
			char *name = de->name;
			int over;

			if (!de->inode)
				continue;

			offset = (char *)de - kaddr;

			over = filldir(dirent, name, strnlen(name,SYSV_NAMELEN),
					(n<<PAGE_CACHE_SHIFT) | offset,
					fs16_to_cpu(sb, de->inode), DT_UNKNOWN);
			if (over) {
				dir_put_page(page);
				goto done;
			}
		}
		dir_put_page(page);
	}

done:
	filp->f_pos = (n << PAGE_CACHE_SHIFT) | offset;
	filp->f_version = inode->i_version;
	UPDATE_ATIME(inode);
	return 0;
}

/* compare strings: name[0..len-1] (not zero-terminated) and
 * buffer[0..] (filled with zeroes up to buffer[0..maxlen-1])
 */
static inline int namecompare(int len, int maxlen,
	const char * name, const char * buffer)
{
	if (len < maxlen && buffer[len])
		return 0;
	return !memcmp(name, buffer, len);
}

/*
 *	sysv_find_entry()
 *
 * finds an entry in the specified directory with the wanted name. It
 * returns the cache buffer in which the entry was found, and the entry
 * itself (as a parameter - res_dir). It does NOT read the inode of the
 * entry - you'll have to do that yourself if you want to.
 */
struct sysv_dir_entry *sysv_find_entry(struct dentry *dentry, struct page **res_page)
{
	const char * name = dentry->d_name.name;
	int namelen = dentry->d_name.len;
	struct inode * dir = dentry->d_parent->d_inode;
	unsigned long start, n;
	unsigned long npages = dir_pages(dir);
	struct page *page = NULL;
	struct sysv_dir_entry *de;

	*res_page = NULL;

	start = dir->u.sysv_i.i_dir_start_lookup;
	if (start >= npages)
		start = 0;
	n = start;

	do {
		char *kaddr;
		page = dir_get_page(dir, n);
		if (!IS_ERR(page)) {
			kaddr = (char*)page_address(page);
			de = (struct sysv_dir_entry *) kaddr;
			kaddr += PAGE_CACHE_SIZE - SYSV_DIRSIZE;
			for ( ; (char *) de <= kaddr ; de++) {
				if (!de->inode)
					continue;
				if (namecompare(namelen, SYSV_NAMELEN,
							name, de->name))
					goto found;
			}
		}
		dir_put_page(page);

		if (++n >= npages)
			n = 0;
	} while (n != start);

	return NULL;

found:
	dir->u.sysv_i.i_dir_start_lookup = n;
	*res_page = page;
	return de;
}

int sysv_add_link(struct dentry *dentry, struct inode *inode)
{
	struct inode *dir = dentry->d_parent->d_inode;
	const char * name = dentry->d_name.name;
	int namelen = dentry->d_name.len;
	struct page *page = NULL;
	struct sysv_dir_entry * de;
	unsigned long npages = dir_pages(dir);
	unsigned long n;
	char *kaddr;
	unsigned from, to;
	int err;

	/* We take care of directory expansion in the same loop */
	for (n = 0; n <= npages; n++) {
		page = dir_get_page(dir, n);
		err = PTR_ERR(page);
		if (IS_ERR(page))
			goto out;
		kaddr = (char*)page_address(page);
		de = (struct sysv_dir_entry *)kaddr;
		kaddr += PAGE_CACHE_SIZE - SYSV_DIRSIZE;
		while ((char *)de <= kaddr) {
			if (!de->inode)
				goto got_it;
			err = -EEXIST;
			if (namecompare(namelen, SYSV_NAMELEN, name, de->name)) 
				goto out_page;
			de++;
		}
		dir_put_page(page);
	}
	BUG();
	return -EINVAL;

got_it:
	from = (char*)de - (char*)page_address(page);
	to = from + SYSV_DIRSIZE;
	lock_page(page);
	err = page->mapping->a_ops->prepare_write(NULL, page, from, to);
	if (err)
		goto out_unlock;
	memcpy (de->name, name, namelen);
	memset (de->name + namelen, 0, SYSV_DIRSIZE - namelen - 2);
	de->inode = cpu_to_fs16(inode->i_sb, inode->i_ino);
	err = dir_commit_chunk(page, from, to);
	dir->i_mtime = dir->i_ctime = CURRENT_TIME;
	mark_inode_dirty(dir);
out_unlock:
	UnlockPage(page);
out_page:
	dir_put_page(page);
out:
	return err;
}

int sysv_delete_entry(struct sysv_dir_entry *de, struct page *page)
{
	struct address_space *mapping = page->mapping;
	struct inode *inode = (struct inode*)mapping->host;
	char *kaddr = (char*)page_address(page);
	unsigned from = (char*)de - kaddr;
	unsigned to = from + SYSV_DIRSIZE;
	int err;

	lock_page(page);
	err = mapping->a_ops->prepare_write(NULL, page, from, to);
	if (err)
		BUG();
	de->inode = 0;
	err = dir_commit_chunk(page, from, to);
	UnlockPage(page);
	dir_put_page(page);
	inode->i_ctime = inode->i_mtime = CURRENT_TIME;
	mark_inode_dirty(inode);
	return err;
}

int sysv_make_empty(struct inode *inode, struct inode *dir)
{
	struct address_space *mapping = inode->i_mapping;
	struct page *page = grab_cache_page(mapping, 0);
	struct sysv_dir_entry * de;
	char *base;
	int err;

	if (!page)
		return -ENOMEM;
	err = mapping->a_ops->prepare_write(NULL, page, 0, 2 * SYSV_DIRSIZE);
	if (err)
		goto fail;

	base = (char*)page_address(page);
	memset(base, 0, PAGE_CACHE_SIZE);

	de = (struct sysv_dir_entry *) base;
	de->inode = cpu_to_fs16(inode->i_sb, inode->i_ino);
	strcpy(de->name,".");
	de++;
	de->inode = cpu_to_fs16(inode->i_sb, dir->i_ino);
	strcpy(de->name,"..");

	err = dir_commit_chunk(page, 0, 2 * SYSV_DIRSIZE);
fail:
	UnlockPage(page);
	page_cache_release(page);
	return err;
}

/*
 * routine to check that the specified directory is empty (for rmdir)
 */
int sysv_empty_dir(struct inode * inode)
{
	struct super_block *sb = inode->i_sb;
	struct page *page = NULL;
	unsigned long i, npages = dir_pages(inode);

	for (i = 0; i < npages; i++) {
		char *kaddr;
		struct sysv_dir_entry * de;
		page = dir_get_page(inode, i);

		if (IS_ERR(page))
			continue;

		kaddr = (char *)page_address(page);
		de = (struct sysv_dir_entry *)kaddr;
		kaddr += PAGE_CACHE_SIZE-SYSV_DIRSIZE;

		for ( ;(char *)de <= kaddr; de++) {
			if (!de->inode)
				continue;
			/* check for . and .. */
			if (de->name[0] != '.')
				goto not_empty;
			if (!de->name[1]) {
				if (de->inode == cpu_to_fs16(sb, inode->i_ino))
					continue;
				goto not_empty;
			}
			if (de->name[1] != '.' || de->name[2])
				goto not_empty;
		}
		dir_put_page(page);
	}
	return 1;

not_empty:
	dir_put_page(page);
	return 0;
}

/* Releases the page */
void sysv_set_link(struct sysv_dir_entry *de, struct page *page,
	struct inode *inode)
{
	struct inode *dir = (struct inode*)page->mapping->host;
	unsigned from = (char *)de-(char*)page_address(page);
	unsigned to = from + SYSV_DIRSIZE;
	int err;

	lock_page(page);
	err = page->mapping->a_ops->prepare_write(NULL, page, from, to);
	if (err)
		BUG();
	de->inode = cpu_to_fs16(inode->i_sb, inode->i_ino);
	err = dir_commit_chunk(page, from, to);
	UnlockPage(page);
	dir_put_page(page);
	dir->i_mtime = dir->i_ctime = CURRENT_TIME;
	mark_inode_dirty(dir);
}

struct sysv_dir_entry * sysv_dotdot (struct inode *dir, struct page **p)
{
	struct page *page = dir_get_page(dir, 0);
	struct sysv_dir_entry *de = NULL;

	if (!IS_ERR(page)) {
		de = (struct sysv_dir_entry*) page_address(page) + 1;
		*p = page;
	}
	return de;
}

ino_t sysv_inode_by_name(struct dentry *dentry)
{
	struct page *page;
	struct sysv_dir_entry *de = sysv_find_entry (dentry, &page);
	ino_t res = 0;
	
	if (de) {
		res = fs16_to_cpu(dentry->d_sb, de->inode);
		dir_put_page(page);
	}
	return res;
}
