/*
 * proc.c
 *
 * Miscellaneous routines, socket read/write.
 */

#ifdef MODVERSIONS
#include <linux/modversions.h>
#endif

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/ctype.h>
#include <linux/string.h>
#include <asm/uaccess.h>
#include <asm/fcntl.h>
#include <linux/file.h>
#include <linux/smp_lock.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/uio.h>
#include <net/sock.h>
#include <linux/sched.h>

#include <linux/shfs_fs.h>
#include <linux/shfs_fs_sb.h>
#include "shfs_debug.h"
#include "proc.h"

/* our life would be too simple without RH */
#ifdef INIT_SIGHAND
#define	SIGLOCK(flags) do { spin_lock_irqsave(&current->sighand->siglock, (flags)); } while (0)
#define	SIGRECALC do { recalc_sigpending(); } while (0)
#define SIGUNLOCK(flags) do { spin_unlock_irqrestore(&current->sighand->siglock, (flags)); } while (0)
#else
#define SIGLOCK(flags) do { spin_lock_irqsave(&current->sigmask_lock, (flags)); } while (0)
#define SIGRECALC do { recalc_sigpending(current); } while (0)
#define SIGUNLOCK(flags) do { spin_unlock_irqrestore(&current->sigmask_lock, (flags)); } while (0)
#endif

static int
parse_mode(char *mode)
{
	return ((mode[0]-'0')<<6) + ((mode[1]-'0')<<3) + (mode[2]-'0');
}

int
parse_options(struct shfs_sb_info *info, char *opts)
{
	char *p, *q;
	int i, j;

	if (!opts) 
		return -1;

	while ((p = strsep(&opts, ","))) {
		if (strncmp(p, "mnt=", 4) == 0) {
			if (strlen(p+4) + 1 > SHFS_PATH_MAX)
				goto ugly_opts;
			strcpy(info->mount_point, p+4);
		} else if (strncmp(p, "ro", 2) == 0) {
			info->readonly = 1;
		} else if (strncmp(p, "rw", 2) == 0) {
			info->readonly = 0;
		} else if (strncmp(p, "suid", 4) == 0) {
			info->fmask |= S_ISUID|S_ISGID;
		} else if (strncmp(p, "nosuid", 6) == 0) {
			info->fmask &= ~(S_ISUID|S_ISGID);
		} else if (strncmp(p, "dev", 3) == 0) {
			info->fmask |= S_IFCHR|S_IFBLK;
		} else if (strncmp(p, "nodev", 5) == 0) {
			info->fmask &= ~(S_IFCHR|S_IFBLK);
		} else if (strncmp(p, "exec", 4) == 0) {
			info->fmask |= S_IXUSR|S_IXGRP|S_IXOTH;
		} else if (strncmp(p, "noexec", 6) == 0) {
			info->fmask &= ~(S_IXUSR|S_IXGRP|S_IXOTH);
		} else if (strncmp(p, "preserve", 8) == 0) {
			info->preserve_own = 1;
		} else if (strncmp(p, "cachesize=", 10) == 0) {
			if (strlen(p+10) > 5)
				goto ugly_opts;
			q = p+10;
			i = simple_strtoul(q, &q, 10);
			i--;
			for (j = 0; i; j++)
				i >>= 1;
			info->fcache_size = (1 << j) * PAGE_SIZE;
		} else if (strncmp(p, "cachemax=", 9) == 0) {
			if (strlen(p+9) > 5)
				goto ugly_opts;
			q = p+9;
			i = simple_strtoul(q, &q, 10);
			info->fcache_free = i;
		} else if (strncmp(p, "debug=", 6) == 0) {
			if (strlen(p+6) > 5)
				goto ugly_opts;
			q = p+6;
			i = simple_strtoul(q, &q, 10);
			debug_level = i;
		} else if (strncmp(p, "ttl=", 4) == 0) {
			if (strlen(p+4) > 10)
				goto ugly_opts;
			q = p+4;
			i = simple_strtoul(q, &q, 10);
			info->ttl = i * 1000;
		} else if (strncmp(p, "uid=", 4) == 0) {
			if (strlen(p+4) > 10)
				goto ugly_opts;
			q = p+4;
			i = simple_strtoul(q, &q, 10);
			info->uid = i; 
		} else if (strncmp(p, "gid=", 4) == 0) {
			if (strlen(p+4) > 10)
				goto ugly_opts;
			q = p+4;
			i = simple_strtoul(q, &q, 10);
			info->gid = i; 
		} else if (strncmp(p, "rmode=", 6) == 0) {
			if (strlen(p+6) > 3)
				goto ugly_opts;
			info->root_mode = S_IFDIR | (parse_mode(p+6) & (S_IRWXU | S_IRWXG | S_IRWXO));
		} else if (strncmp(p, "fd=", 3) == 0) {
			if (strlen(p+3) > 5)
				goto ugly_opts;
			q = p+3;
			i = simple_strtoul(q, &q, 10);
			info->sock = fget(i);
		} else if (strncmp(p, "version=", 8) == 0) {
			if (strlen(p+8) > 5)
				goto ugly_opts;
			q = p+8;
			i = simple_strtoul(q, &q, 10);
			info->version = i;
		}
	}
	return 0;
	
ugly_opts:
	VERBOSE("Invalid option: %s\n", p);
	return -1;
}

static int clear_garbage(struct shfs_sb_info *);

/* sock_lock held */
int
sock_write(struct shfs_sb_info *info, const void *buffer, int count)
{
	struct file *f = info->sock;
	mm_segment_t fs;
	int c, result = 0;
	unsigned long flags, sigpipe;
	sigset_t old_set;

	if (!f)
		return -EIO;
	if (info->garbage) {
		result = clear_garbage(info);
		if (result < 0)
			return result;
	}
	
	c = count;

	fs = get_fs();
	set_fs(get_ds());

	SIGLOCK(flags);
	sigpipe = sigismember(&current->pending.signal, SIGPIPE);
	old_set = current->blocked;
	siginitsetinv(&current->blocked, sigmask(SIGKILL)|sigmask(SIGSTOP));
	SIGRECALC;
	SIGUNLOCK(flags);

	do {
		result = f->f_op->write(f, buffer, c, &f->f_pos);
		if (result < 0) {
			DEBUG("error: %d\n", result);
			if (result == -EAGAIN)
				continue;
			fput(f);
			info->sock = NULL;
			break;
		}
		buffer += result;
		c -= result;
	} while (c > 0);

	SIGLOCK(flags);
	if (result == -EPIPE && !sigpipe) {
		sigdelset(&current->pending.signal, SIGPIPE);
		result = -EIO;
	}
	current->blocked = old_set;
	SIGRECALC;
	SIGUNLOCK(flags);

	set_fs(fs);

	DEBUG(">%d\n", result);
	if (result < 0)
		set_garbage(info, 1, c);
	else
		result = count;
	return result;
}

#define BUFFER info->readlnbuf
#define LEN    info->readlnbuf_len

/* sock_lock held */
int
sock_read(struct shfs_sb_info *info, void *buffer, int count)
{
	struct file *f = info->sock;
	mm_segment_t fs;
	int c, result = 0;
	unsigned long flags, sigpipe;
	sigset_t old_set;

	if (!f)
		return -EIO;
	if (info->garbage) {
		result = clear_garbage(info);
		if (result < 0)
			return result;
	}
	c = count;
	if (LEN > 0) {
		if (count > LEN)
			c = LEN;
		memcpy(buffer, BUFFER, c);
		buffer += c;
		LEN -= c;
		if (LEN > 0)
			memmove(BUFFER, BUFFER+c, LEN);
		c = count - c;
	}

	/* don't block reading 0 bytes */
	if (c == 0)
		return count;

	SIGLOCK(flags);
	sigpipe = sigismember(&current->pending.signal, SIGPIPE);
	old_set = current->blocked;
	siginitsetinv(&current->blocked, sigmask(SIGKILL)|sigmask(SIGSTOP));
	SIGRECALC;
	SIGUNLOCK(flags);
	
	fs = get_fs();
	set_fs(get_ds());

	do {
		result = f->f_op->read(f, buffer, c, &f->f_pos);
 		if (!result) {
 			/*  peer has closed socket */
 			result = -EIO;
 		}
		if (result < 0) {
			DEBUG("error: %d\n", result);
			if (result == -EAGAIN)
				continue;
			fput(f);
			info->sock = NULL;
			break;
		}
		buffer += result;
		c -= result;
	} while (c > 0);

	SIGLOCK(flags);
	if (result == -EPIPE && !sigpipe) {
		sigdelset(&current->pending.signal, SIGPIPE);
		result = -EIO;
	}
	current->blocked = old_set;
	SIGRECALC;
	SIGUNLOCK(flags);

	set_fs(fs);
	
	DEBUG("<%d\n", result);
	if (result < 0)
		set_garbage(info, 0, c);
	else
		result = count;
	return result;
}
 
/* sock_lock held */
int 
sock_readln(struct shfs_sb_info *info, char *buffer, int count)
{
	struct file *f = info->sock;
	mm_segment_t fs;
	int c, l = 0, result;
	char *nl;
	unsigned long flags, sigpipe;
	sigset_t old_set;

	if (!f)
		return -EIO;
	if (info->garbage) {
		result = clear_garbage(info);
		if (result < 0)
			return result;
	}
	while (1) {
		nl = memchr(BUFFER, '\n', LEN);
		if (nl) {
			*nl = '\0';
			strncpy(buffer, BUFFER, count-1);
			buffer[count-1] = '\0';
			c = LEN-(nl-BUFFER+1);
			if (c > 0)
				memmove(BUFFER, nl+1, c);
			LEN = c;

			DEBUG("<%s\n", buffer);
			return strlen(buffer);
		}
		DEBUG("miss(%d)\n", LEN);
		c = READLNBUF_SIZE - LEN;
		if (c == 0) {
			BUFFER[READLNBUF_SIZE-1] = '\n';
			continue;
		}

		SIGLOCK(flags);
		sigpipe = sigismember(&current->pending.signal, SIGPIPE);
		old_set = current->blocked;
		siginitsetinv(&current->blocked, sigmask(SIGKILL)|sigmask(SIGSTOP));
		SIGRECALC;
		SIGUNLOCK(flags);

		fs = get_fs();
		set_fs(get_ds());

		result = f->f_op->read(f, BUFFER+LEN, c, &f->f_pos);

		SIGLOCK(flags);
		if (result == -EPIPE && !sigpipe) {
			sigdelset(&current->pending.signal, SIGPIPE);
			result = -EIO;
		}
		current->blocked = old_set;
		SIGRECALC;
		SIGUNLOCK(flags);

		set_fs(fs);

		DEBUG("<%d\n", result);
		if (!result) {
			/* peer has closed socket */
			result = -EIO;
		}
		if (result < 0) {
			DEBUG("error: %d\n", result);
			if (result == -EAGAIN)
				continue;
			fput(f);
			info->sock = NULL;
			set_garbage(info, 0, c);
			return result;
		}
		LEN += result;

		/* do not lock while reading 0 bytes */
		if (l++ > READLNBUF_SIZE && LEN == 0)
			return -EINVAL;
	}
}

int
reply(char *s)
{
	if (strncmp(s, "### ", 4))
		return 0;
	return simple_strtoul(s+4, NULL, 10);
}

static int
clear_garbage(struct shfs_sb_info *info)
{
	static unsigned long seq = 12345;
	char buffer[256];
	int i, c, state, garbage;
	int result;

	garbage = info->garbage_write;
	if (garbage)
		memset(buffer, ' ', sizeof(buffer));
	DEBUG(">%d\n", garbage);
	while (garbage > 0) {
		c = garbage < sizeof(buffer) ? garbage : sizeof(buffer);
		info->garbage = 0;
		result = sock_write(info, buffer, c);
		if (result < 0) {
			info->garbage_write = garbage;
			goto error;
		}
		garbage -= result;
	}
	info->garbage_write = 0;
	garbage = info->garbage_read;
	DEBUG("<%d\n", garbage);
	while (garbage > 0) {
		c = garbage < sizeof(buffer) ? garbage : sizeof(buffer);
		info->garbage = 0;
		result = sock_read(info, buffer, c);
		if (result < 0) {
			info->garbage_read = garbage;
			goto error;
		}
		garbage -= result;
	}
	info->garbage_read = 0;
		
	info->garbage = 0;
	sprintf(buffer, "\n\ns_ping %lu", seq);
	result = sock_write(info, (void *)buffer, strlen(buffer));
	if (result < 0)
		goto error;
	DEBUG("reading..\n");
	state = 0;
	for (i = 0; i < 100000; i++) {
		info->garbage = 0;
		result = sock_readln(info, buffer, sizeof(buffer));
		if (result < 0)
			goto error;
		if (((state == 0) || (state == 1)) && reply(buffer) == REP_PRELIM) {
			state = 1;
		} else if (state == 1 && simple_strtoul(buffer, NULL, 10) == (seq-1)) {
			state = 2;
		} else if (state == 2 && reply(buffer) == REP_NOP) {
			DEBUG("cleared\n");
			return 0;
		} else {
			state = 0;
		}
	}
error:
	info->garbage = 1;
	DEBUG("failed\n");
	return result;
}

void
set_garbage(struct shfs_sb_info *info, int write, int count)
{
	info->garbage = 1;
	if (write)
		info->garbage_write = count;
	else
		info->garbage_read = count;
}

int
get_name(struct dentry *d, char *name)
{
	int len = 0;
	struct dentry *p;

	for (p = d; !IS_ROOT(p); p = p->d_parent)
		len += p->d_name.len + 1;
		
	if (len + 1 > SHFS_PATH_MAX) 
		return 0;
	if (len == 0) {
		name[0] = '/';
		name[1] = '\0';
		return 1;
	}
	
	name[len] = '\0';
	for (p = d; !IS_ROOT(p); p = p->d_parent) {
		len -= p->d_name.len;
		strncpy(&(name[len]), p->d_name.name, p->d_name.len);
		len--;
		name[len] = '/';
	}

	return 1;
}

int
shfs_notify_change(struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = dentry->d_inode;
	struct shfs_sb_info *info = info_from_inode(inode);
	char file[SHFS_PATH_MAX];
	int result;

	DEBUG("\n");
	if (get_name(dentry, file) < 0)
		return -ENAMETOOLONG;
	result = inode_change_ok(inode, attr);
	if (result < 0)
		return result;
	if (info->readonly)
		return -EROFS;

	if (attr->ia_valid & ATTR_MODE) {
		result = info->fops.chmod(info, file, attr->ia_mode);
		if (result < 0)
			goto error;
		inode->i_mode = attr->ia_mode;
	}
	if (attr->ia_valid & ATTR_UID) {
		result = info->fops.chown(info, file, attr->ia_uid);
		if (result < 0)
			goto error;
		inode->i_uid = info->preserve_own ? info->uid : attr->ia_uid;
	}
	if (attr->ia_valid & ATTR_GID) {
		result = info->fops.chgrp(info, file, attr->ia_gid);
		if (result < 0)
			goto error;
		inode->i_gid = info->preserve_own ? info->gid : attr->ia_gid;
	}
	if (attr->ia_valid & ATTR_SIZE) {
                filemap_fdatasync(inode->i_mapping);
                filemap_fdatawait(inode->i_mapping);
		result = info->fops.trunc(info, file, attr->ia_size);
		if (result < 0)
			goto error;
		result = vmtruncate(inode, attr->ia_size);
		if (result != 0)
			goto error;
		inode->i_size = attr->ia_size;
		mark_inode_dirty(inode);
	}
	/* optimisation: call settime once when setting both atime and mtime */
	if (attr->ia_valid & ATTR_ATIME && attr->ia_valid & ATTR_MTIME
	    && attr->ia_atime == attr->ia_mtime) {
		result = info->fops.settime(info, file, 1, 1, &attr->ia_atime);
		if (result < 0)
			goto error;
		inode->i_atime = attr->ia_atime;
		inode->i_mtime = attr->ia_mtime;
	} else {
		if (attr->ia_valid & ATTR_ATIME) {
			result = info->fops.settime(info, file, 1, 0, &attr->ia_atime);
			if (result < 0)
				goto error;
			inode->i_atime = attr->ia_atime;
		}
		if (attr->ia_valid & ATTR_MTIME) {
			result = info->fops.settime(info, file, 0, 1, &attr->ia_mtime);
			if (result < 0)
				goto error;
			inode->i_mtime = attr->ia_mtime;
		}
	}
error:
	return result;
}

int
shfs_statfs(struct super_block *sb, struct statfs *attr)
{
	struct shfs_sb_info *info = info_from_sb(sb);

	DEBUG("\n");
	return info->fops.statfs(info, attr);
}

