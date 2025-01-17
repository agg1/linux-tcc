#ifndef _SHFS_FS_H
#define _SHFS_FS_H

#include <linux/shfs.h>

#define SHFS_IOC_NEWCONN	_IOW('s', 2, int)

#ifdef __KERNEL__

#include <linux/ioctl.h>
#include <linux/pagemap.h>

#define SHFS_MAX_AGE(info)	(((info)->ttl * HZ) / 1000)
#define SOCKBUF_SIZE		(SHFS_PATH_MAX * 10)
#define READLNBUF_SIZE		(SHFS_PATH_MAX * 10)

#define SHFS_FCACHE_MAX		10	/* max number of files cached */
#define SHFS_FCACHE_PAGES	32	/* should be 2^x */

struct shfs_sb_info;

struct shfs_cache_head {
//	time_t		mtime;	/* unused */
	unsigned long	time;	/* cache age */
	unsigned long	end;	/* last valid fpos in cache */
	int		eof;
};

#define SHFS_DIRCACHE_SIZE	((int)(PAGE_CACHE_SIZE/sizeof(struct dentry *)))
union shfs_dir_cache {
	struct shfs_cache_head   head;
	struct dentry           *dentry[SHFS_DIRCACHE_SIZE];
};

#define SHFS_FIRSTCACHE_SIZE	((int)((SHFS_DIRCACHE_SIZE * \
	sizeof(struct dentry *) - sizeof(struct shfs_cache_head)) / \
	sizeof(struct dentry *)))

#define SHFS_DIRCACHE_START      (SHFS_DIRCACHE_SIZE - SHFS_FIRSTCACHE_SIZE)

struct shfs_cache_control {
	struct  shfs_cache_head		head;
	struct  page			*page;
	union   shfs_dir_cache		*cache;
	unsigned long			fpos, ofs;
	int				filled, valid, idx;
};

/* use instead of CURRENT_TIME since precision is minutes, not seconds */
#define ROUND_TO_MINS(x) do { (x) = ((x) / 60) * 60; } while (0)

/* shfs/dir.c */
extern struct dentry_operations shfs_dentry_operations;
extern struct file_operations shfs_dir_operations;
extern struct inode_operations shfs_dir_inode_operations;
extern void shfs_new_dentry(struct dentry *dentry);
extern void shfs_age_dentry(struct shfs_sb_info *info, struct dentry *dentry);
extern void shfs_renew_times(struct dentry * dentry);

/* shfs/file.c */
extern struct file_operations shfs_file_operations;
extern struct file_operations shfs_slow_operations;
extern struct inode_operations shfs_file_inode_operations;
extern struct address_space_operations shfs_file_aops;

/* shfs/symlink.c */
extern struct inode_operations shfs_symlink_inode_operations;

/* shfs/dcache.c */
void shfs_invalid_dir_cache(struct inode * dir);
void shfs_invalidate_dircache_entries(struct dentry *parent);
struct dentry *shfs_dget_fpos(struct dentry*, struct dentry*, unsigned long);
int shfs_fill_cache(struct file*, void*, filldir_t, struct shfs_cache_control*, struct qstr*, struct shfs_fattr*);

/* shfs/fcache.c */
#include <linux/slab.h>
extern kmem_cache_t *file_cache;
extern kmem_cache_t *dir_head_cache;
extern kmem_cache_t *dir_entry_cache;
extern kmem_cache_t *dir_name_cache;
void fcache_init(void);
void fcache_finish(void);
int fcache_file_open(struct file*);
int fcache_file_sync(struct file*);
int fcache_file_close(struct file*);
int fcache_file_clear(struct inode*);
int fcache_file_read(struct file*, unsigned, unsigned, char*);
int fcache_file_write(struct file*, unsigned, unsigned, char*);

/* shfs/ioctl.c */
int shfs_ioctl(struct inode *inode, struct file *f, unsigned int cmd, unsigned long arg);

/* shfs/proc.c */
int parse_options(struct shfs_sb_info *info, char *opts);
int sock_write(struct shfs_sb_info *info, const void *buf, int count);
int sock_read(struct shfs_sb_info *info, void *buffer, int count);
int sock_readln(struct shfs_sb_info *info, char *buffer, int count);
int reply(char *s);
void set_garbage(struct shfs_sb_info *info, int write, int count);
int get_name(struct dentry *d, char *name);
int shfs_notify_change(struct dentry *dentry, struct iattr *attr);
int shfs_statfs(struct super_block *sb, struct statfs *attr);
	
/* shfs/inode.c */
void shfs_set_inode_attr(struct inode *inode, struct shfs_fattr *fattr);
struct inode *shfs_iget(struct super_block*, struct shfs_fattr*);
int shfs_revalidate_inode(struct dentry*);

/* shfs/shell.c */
extern struct shfs_fileops shell_fops;

#endif  /* __KERNEL__ */

#endif	/* _SHFS_FS_H */
