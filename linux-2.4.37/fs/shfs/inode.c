/*
 * inode.c
 * 
 * Inode/superblock operations, partialy inspired by smbfs/ncpfs.
 */

#ifdef MODVERSIONS
#include <linux/modversions.h>
#endif

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <asm/uaccess.h>
#include <linux/file.h>
#include <linux/smp_lock.h>

#include <linux/shfs_fs.h>
#include <linux/shfs_fs_sb.h>
#include <linux/shfs_fs_i.h>
#include "shfs_debug.h"
#include "proc.h"

MODULE_AUTHOR("Zemljanka core team");
MODULE_DESCRIPTION("SHell File System");

int debug_level;
#ifdef ENABLE_DEBUG
	unsigned long alloc;
#endif

kmem_cache_t	*inode_cache = NULL;

void 
shfs_set_inode_attr(struct inode *inode, struct shfs_fattr *fattr)
{
	struct shfs_sb_info *info = info_from_inode(inode);
	struct shfs_inode_info *i = inode->u.generic_ip;
	time_t last_time = inode->i_mtime;
	loff_t last_size = inode->i_size;

	inode->i_mode 	= fattr->f_mode;
	inode->i_nlink	= fattr->f_nlink;
	if (info->preserve_own) {
		inode->i_uid = fattr->f_uid;
		inode->i_gid = fattr->f_gid;
	} else {
		inode->i_uid = info->uid;
		inode->i_gid = info->gid;
	}
	inode->i_rdev	= fattr->f_rdev;
	inode->i_ctime	= fattr->f_ctime;
	inode->i_atime	= fattr->f_atime;
	inode->i_mtime	= fattr->f_mtime;
	inode->i_blksize= fattr->f_blksize;
	inode->i_blocks	= fattr->f_blocks;
	inode->i_size	= fattr->f_size;

	i->oldmtime = jiffies;

	if (inode->i_mtime != last_time || inode->i_size != last_size) {
		DEBUG("inode changed (%ld/%ld, %lu/%lu)\n", inode->i_mtime, last_time, (unsigned long)inode->i_size, (unsigned long)last_size);
		invalidate_inode_pages(inode);
		fcache_file_clear(inode);
	}
}

struct inode*
shfs_iget(struct super_block *sb, struct shfs_fattr *fattr)
{
	struct inode *inode;
	struct shfs_inode_info *i;

	inode = new_inode(sb);
	if (!inode)
		return NULL;
	inode->i_ino = fattr->f_ino;
	i = inode->u.generic_ip = (struct shfs_inode_info *)KMEM_ALLOC("inode", inode_cache, GFP_KERNEL);
	if (!i)
		return NULL;
	i->cache = NULL;
	i->unset_write_on_close = 0;
	shfs_set_inode_attr(inode, fattr);

	DEBUG("ino: %lu\n", inode->i_ino);
	if (S_ISDIR(inode->i_mode)) {
		DEBUG("dir\n");
		inode->i_op = &shfs_dir_inode_operations;
		inode->i_fop = &shfs_dir_operations;
	} else if (S_ISLNK(inode->i_mode)) { 
		DEBUG("link\n");
		inode->i_op = &shfs_symlink_inode_operations;
	} else if (S_ISREG(inode->i_mode) || S_ISBLK(inode->i_mode) || S_ISCHR(inode->i_mode)) {
		DEBUG("file/block/char\n");
		inode->i_op = &shfs_file_inode_operations;
		inode->i_fop = &shfs_file_operations;
		inode->i_data.a_ops = &shfs_file_aops;
	}
	
	insert_inode_hash(inode);
	return inode;
}

static void 
shfs_delete_inode(struct inode *inode)
{
	struct shfs_inode_info *i;

	DEBUG("ino: %lu\n", inode->i_ino);
	i = (struct shfs_inode_info *)inode->u.generic_ip;
	if (!i) {
		VERBOSE("invalid inode\n");
		goto out;
	}
	if (i->cache) {
		VERBOSE("file cache not free!\n");
		/* TODO: free it now? */
	}
	KMEM_FREE("inode", inode_cache, i);
out:
	clear_inode(inode);
}

/* borrowed from smbfs */
static int
shfs_refresh_inode(struct dentry *dentry)
{
	struct shfs_sb_info *info = info_from_dentry(dentry);
	struct inode *inode = dentry->d_inode;
	struct shfs_fattr fattr;
	char name[SHFS_PATH_MAX];
	int result;

	if (inode->i_ino == 2) {
		shfs_renew_times(dentry);
		return 0;
	}
	if (get_name(dentry, name) < 0)
		return -ENAMETOOLONG;

	result = info->fops.stat(info, name, &fattr);
	if (result < 0)
		goto out;

	shfs_renew_times(dentry);
	if (S_ISLNK(inode->i_mode))
		goto out;
	if ((inode->i_mode & S_IFMT) == (fattr.f_mode & S_IFMT)) {
		shfs_set_inode_attr(inode, &fattr);
	} else {
		/* big touble */
		fattr.f_mode = inode->i_mode; /* save mode */
		make_bad_inode(inode);
		inode->i_mode = fattr.f_mode; /* restore mode */
		/*
		 * No need to worry about unhashing the dentry: the
		 * lookup validation will see that the inode is bad.
		 * But we do want to invalidate the caches ...
		 */
		if (!S_ISDIR(inode->i_mode))
			invalidate_inode_pages(inode);
		else
			shfs_invalid_dir_cache(inode);
		result = -EIO;
	}
out:
	return result;
}

int
shfs_revalidate_inode(struct dentry *dentry)
{
	struct shfs_sb_info *info = info_from_dentry(dentry);
	struct inode *inode = dentry->d_inode;
	struct shfs_inode_info *i = (struct shfs_inode_info *)inode->u.generic_ip;
	int result;

        DEBUG("%s\n", dentry->d_name.name);
	result = 0;

	lock_kernel();
	if (is_bad_inode(inode))
		goto out;
	if (inode->i_sb->s_magic != SHFS_SUPER_MAGIC)
		goto out;
	if (time_before(jiffies, i->oldmtime + SHFS_MAX_AGE(info)))
		goto out;
		
	result = shfs_refresh_inode(dentry);
out:
	unlock_kernel();
	DEBUG("%d\n", result);
	return result;
}

static void
shfs_put_super(struct super_block *sb)
{
	struct shfs_sb_info *info = info_from_sb(sb);
	int result;

	result = info->fops.finish(info);
	if (info->sock)
		fput(info->sock);
	if (!sock_lock(info)) {
		VERBOSE("Cannot free caches!\n");
		return;
	}
	kfree(info->sockbuf);
	kfree(info->readlnbuf);
	kfree(info);
	DEBUG("Super block discarded!\n");
}

struct super_operations shfs_sops = {
	put_inode:	force_delete,
	delete_inode:	shfs_delete_inode,
	put_super:	shfs_put_super,
	statfs:		shfs_statfs,
};

static void
init_root_dirent(struct shfs_sb_info *server, struct shfs_fattr *fattr)
{
	memset(fattr, 0, sizeof(*fattr));
	fattr->f_nlink = 1;
	fattr->f_uid = server->uid;
	fattr->f_gid = server->gid;
	fattr->f_blksize = 512;

	fattr->f_ino = 2;
	fattr->f_mtime = CURRENT_TIME;
	fattr->f_mode = server->root_mode;
	fattr->f_size = 512;
	fattr->f_blocks = 0;
}

static struct super_block*
shfs_read_super(struct super_block *sb, void *opts, int silent)
{
	struct shfs_sb_info *info;
	struct shfs_fattr root;
	struct inode *root_inode;
	int result;
	
	info = (struct shfs_sb_info*)kmalloc(sizeof(struct shfs_sb_info), GFP_KERNEL);
	if (!info) {
		printk(KERN_NOTICE "Not enough kmem!\n");
		goto out;
	}

	memset(info, 0, sizeof(struct shfs_sb_info));

	sb->u.generic_sbp = info;
	sb->s_blocksize = 4096;
	sb->s_blocksize_bits = 12;
	sb->s_magic = SHFS_SUPER_MAGIC;
	sb->s_op = &shfs_sops;
	sb->s_flags = 0;
	
	/* fill-in default values */
	info->fops = shell_fops;
	info->version = 0;
	info->ttl = SHFS_DEFAULT_TTL;
	info->uid = current->uid;
	info->gid = current->gid;
	info->root_mode = (S_IRUSR | S_IWUSR | S_IXUSR | S_IFDIR);
	info->fmask = 00177777;
	info->mount_point[0] = 0;
	init_MUTEX(&info->sock_sem);
	info->sock = NULL;
	info->sockbuf = (char *)kmalloc(SOCKBUF_SIZE, GFP_KERNEL);
	if (!info->sockbuf) {
		printk(KERN_NOTICE "Not enough kmem!\n");
		goto out_no_mem;
	}
	info->readlnbuf_len = 0;
	info->readlnbuf = (char *)kmalloc(READLNBUF_SIZE, GFP_KERNEL);
	if (!info->readlnbuf) {
		printk(KERN_NOTICE "Not enough kmem!\n");
		kfree(info->sockbuf);
		goto out_no_mem;
	}
	spin_lock_init(&info->fcache_lock);
	info->fcache_free = SHFS_FCACHE_MAX;
	info->fcache_size = SHFS_FCACHE_PAGES * PAGE_SIZE;
	info->garbage_read = 0;
	info->garbage_write = 0;
	info->garbage = 0;
	info->readonly = 0;
	info->preserve_own = 0;
	info->stable_symlinks = 0;

	debug_level = 0;
	result = parse_options(info, (char *)opts);
	if (result < 0)
		goto out_no_opts;
	if (!info->sock) {
		VERBOSE("Socket not specified\n");
		goto out_no_opts;
	}
	if (info->version != PROTO_VERSION) {
		printk(KERN_NOTICE "shfs: version mismatch (module: %d, mount: %d)\n", PROTO_VERSION, info->version);
		goto out_no_opts;
	}

	init_root_dirent(info, &root);
	root_inode = shfs_iget(sb, &root);
	if (!root_inode) 
		goto out_no_root;
	sb->s_root = d_alloc_root(root_inode);
	if (!sb->s_root) 
		goto out_no_root;
	shfs_new_dentry(sb->s_root);

	DEBUG("ok\n");
	return sb;

out_no_root:
	iput(root_inode);
out_no_opts:
	kfree(info->sockbuf);
	kfree(info->readlnbuf);
out_no_mem:
	kfree(info);
out:
	DEBUG("failed\n");
	return NULL;
}

static DECLARE_FSTYPE (sh_fs_type, "shfs", shfs_read_super, 0);

static int __init 
init_shfs(void)
{
	printk(KERN_NOTICE "SHell File System, (c) 2002-2004 Miroslav Spousta\n");
	fcache_init();
	inode_cache = kmem_cache_create("shfs_inode", sizeof(struct shfs_inode_info), 0, 0, NULL, NULL);
	
	debug_level = 0;
#ifdef ENABLE_DEBUG
	alloc = 0;
#endif
	return register_filesystem(&sh_fs_type);
}

static void __exit 
exit_shfs(void)
{
	VERBOSE("Unregistering shfs.\n");
#ifdef ENABLE_DEBUG
	if (alloc)
		VERBOSE("Memory leak (%lu)!\n", alloc);
#endif
	unregister_filesystem(&sh_fs_type);
	kmem_cache_destroy(inode_cache);
	fcache_finish();
}

EXPORT_NO_SYMBOLS;

module_init(init_shfs);
module_exit(exit_shfs);

#include <linux/version.h>

#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,4,10))
	MODULE_LICENSE("GPL");
#endif

