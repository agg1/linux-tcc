/*
 * shell.c
 *
 * "Shell" client implementation.
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

#include <linux/shfs_fs.h>
#include <linux/shfs_fs_sb.h>
#include "shfs_debug.h"
#include "proc.h"

/* directory list fields (ls -lan) */
#define DIR_COLS   9
#define DIR_PERM   0
#define DIR_NLINK  1
#define DIR_UID    2
#define DIR_GID    3
#define DIR_SIZE   4
#define DIR_MONTH  5
#define DIR_DAY    6
#define DIR_YEAR   7
#define DIR_NAME   8

/* aaa'aaa -> aaa'\''aaa */
static int
replace_quote(char *name)
{
	char *s;
	char *r = name;
	int q = 0;
	
	while (*r) {
		if (*r == '\'')
			q++;
		r++;
	}
	s = r+(3*q);
	if ((s-name+1) > SHFS_PATH_MAX)
		return -1;
	
	*(s--) = '\0';
	r--;
	while (q) {
		if (*r == '\'') {
			s -= 3;
			s[0] = '\'';
			s[1] = '\\';
			s[2] = '\'';
			s[3] = '\'';
			q--;
		} else {
			*s = *r;
		}
		s--;
		r--;
	}
	return 0;
}

static int
check_path(char *path)
{
	if (replace_quote(path) < 0)
		return 0;
	return 1;
}

/* returns NULL if not enough space */
static char *
get_ugid(struct shfs_sb_info *info, char *str, int max)
{
	char buffer[1024];
	char *s = str;
	
	if (max < 2)
		return NULL;
	strcpy(s, " "); s++;
	if (info->preserve_own) {
		int i;
		snprintf(buffer, sizeof(buffer), "%d '", current->uid);
		if (s - str + strlen(buffer)+1 > max)
			return NULL;
		strcpy(s, buffer); s += strlen(buffer);
		for (i = 0; i < current->ngroups; i++) {
			snprintf(buffer, sizeof(buffer), "%d ", current->groups[i]);
			if (s - str + strlen(buffer)+2 > max)
				return NULL;
			strcpy(s, buffer), s += strlen(buffer);
		}
		strcpy(s-1, "' "); s++;
	}
	return s;
}

static int 
do_command(struct shfs_sb_info *info, char *cmd, char *args, ...)
{
	va_list ap;
	int result;
	char *s;

	if (!sock_lock(info))
		return -EINTR;
	
	s = info->sockbuf;
	strcpy(s, cmd); s += strlen(cmd);
	s = get_ugid(info, s, SOCKBUF_SIZE - strlen(cmd));
	if (!s) {
		result = -ENAMETOOLONG;
		goto error;
	}
	va_start(ap, args);
	result = vsnprintf(s, SOCKBUF_SIZE - (s - info->sockbuf), args, ap);
	va_end(ap);
	if (result < 0 || strlen(info->sockbuf) + 2 > SOCKBUF_SIZE) {
		result = -ENAMETOOLONG;
		goto error;
	}
	s += result;
	strcpy(s, "\n");

	DEBUG("#%s", info->sockbuf);
	result = sock_write(info, (void *)info->sockbuf, strlen(info->sockbuf));
	if (result < 0)
		goto error;
	sock_readln(info, info->sockbuf, SOCKBUF_SIZE);
	switch (reply(info->sockbuf)) {
	case REP_COMPLETE:
		result = 0;
		break;
	case REP_EPERM:
		result = -EPERM;
		break;
	case REP_ENOENT:
		result = -ENOENT;
		break;
	case REP_NOTEMPTY:
		result = 1;
		break;
	default:
		result = -EIO;
		break;
	}

error:
	sock_unlock(info);
	return result;
}

static unsigned int
get_this_year(void)
{
	unsigned long s_per_year = 365*24*3600;
	unsigned long delta_s = 24*3600;

	unsigned int year = CURRENT_TIME / (s_per_year + delta_s/4);
	return 1970 + year; 
}

static unsigned int
get_this_month(void) 
{
	int month = -1;
	long secs = CURRENT_TIME % (365*24*3600+24*3600/4);
	static long days_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	if (get_this_year() % 4) {
		days_month[1] = 28;
	} else {
		days_month[1] = 29;
	}
	while (month < 11 && secs >= 0) {
		month++;
		secs-=24*3600*days_month[month];
	}
	return (unsigned int)month;
}

static int
get_month(char *mon)
{
	static char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	unsigned int i;

	for (i = 0; i<12; i++)
		if (strcmp(mon, months[i]) == 0) {
			DEBUG("%s: %u\n", mon, i);
			return i;
		}

	return -1;
}

static int
parse_dir(char *s, char **col)
{
	int c = 0;
	int is_space = 1;
	int device = 0;

	while (*s) {
		if (c == DIR_COLS)
			break;
		
		if (*s == ' ') {
			if (!is_space) {
				if ((c-1 == DIR_SIZE) && device) {
					while (*s && (*s == ' '))
						s++;
					while (*s && (*s != ' '))
						s++;
				}
				*s = '\0';
				is_space = 1;
			}
		} else {
			if (is_space) {
				/* (b)lock/(c)haracter device hack */
				col[c++] = s;
				is_space = 0;
				if ((c-1 == DIR_PERM) && ((*s == 'b')||(*s == 'c'))) {
					device = 1;
				}

			}
		}
		s++;
	}
	return c;
}

static int
do_ls(struct shfs_sb_info *info, char *file, struct shfs_fattr *entry,
      struct file *filp, void *dirent, filldir_t filldir, struct shfs_cache_control *ctl)
{
	char *col[DIR_COLS];
	struct shfs_fattr fattr;
	struct qstr name;
	unsigned int year, mon, day, hour, min;
	unsigned int this_year = get_this_year();
	unsigned int this_month = get_this_month();
	int device, month;
	umode_t mode;
	char *b, *s, *command = entry ? "s_stat" : "s_lsdir";
	int result;
	
	if (!check_path(file))
		return -ENAMETOOLONG;
	if (!sock_lock(info))
		return -EINTR;

	s = info->sockbuf;
	strcpy(s, command); s += strlen(command);
	s = get_ugid(info, s, SOCKBUF_SIZE - strlen(command));
	if (!s) {
		result = -ENAMETOOLONG;
		goto out;
	}
	if (s - info->sockbuf + strlen(file) + 4 > SOCKBUF_SIZE) {
		result = -ENAMETOOLONG;
		goto out;
	}
	strcpy(s, "'"); s++;
	strcpy(s, file); s += strlen(file);
	strcpy(s, "'"); s++;
	strcpy(s, "\n");

	DEBUG(">%s\n", info->sockbuf);
	result = sock_write(info, (void *)info->sockbuf, strlen(info->sockbuf));
	if (result < 0)
		goto out;

	while ((result = sock_readln(info, info->sockbuf, SOCKBUF_SIZE)) > 0) {
		switch (reply(info->sockbuf)) {
		case REP_COMPLETE:
			result = 0;
			goto out;
		case REP_EPERM:
			result = -EPERM;
			goto out;
		case REP_ENOENT:
			result = -ENOENT;
			goto out;
		case REP_ERROR:
			result = -EIO;
			goto out;
		}

		result = parse_dir(info->sockbuf, col);
		if (result != DIR_COLS)
			continue;		/* skip `total xx' line */
		
		memset(&fattr, 0, sizeof(fattr));
		s = col[DIR_NAME];
		if (!strcmp(s, ".") || !strcmp(s, ".."))
			continue;
		name.name = s;
		/* name.len is assigned later */

		s = col[DIR_PERM];
		mode = 0; device = 0;
		switch (s[0]) {
		case 'b':
			device = 1;
			if ((info->fmask & S_IFMT) & S_IFBLK)
				mode = S_IFBLK;
			else
				mode = S_IFREG;
			break;
		case 'c':
			device = 1;
			if ((info->fmask & S_IFMT) & S_IFCHR)
				mode = S_IFCHR;
			else
				mode = S_IFREG;
		break;
		case 's':
		case 'S':			/* IRIX64 socket */
			mode = S_IFSOCK;
			break;
		case 'd':
			mode = S_IFDIR;
			break;
		case 'l':
			mode = S_IFLNK;
			break;
		case '-':
			mode = S_IFREG;
			break;
		case 'p':
			mode = S_IFIFO;
			break;
		}
		if (s[1] == 'r') mode |= S_IRUSR;
		if (s[2] == 'w') mode |= S_IWUSR;
		if (s[3] == 'x') mode |= S_IXUSR;
		if (s[3] == 's') mode |= S_IXUSR | S_ISUID;
		if (s[3] == 'S') mode |= S_ISUID;
		if (s[4] == 'r') mode |= S_IRGRP;
		if (s[5] == 'w') mode |= S_IWGRP;
		if (s[6] == 'x') mode |= S_IXGRP;
		if (s[6] == 's') mode |= S_IXGRP | S_ISGID;
		if (s[6] == 'S') mode |= S_ISGID;
		if (s[7] == 'r') mode |= S_IROTH;
		if (s[8] == 'w') mode |= S_IWOTH;
		if (s[9] == 'x') mode |= S_IXOTH;
		if (s[9] == 't') mode |= S_ISVTX | S_IXOTH;
		if (s[9] == 'T') mode |= S_ISVTX;
		fattr.f_mode = S_ISREG(mode) ? mode & info->fmask : mode;

		fattr.f_uid = simple_strtoul(col[DIR_UID], NULL, 10);
		fattr.f_gid = simple_strtoul(col[DIR_GID], NULL, 10);
		
		if (!device) {
			fattr.f_size = simple_strtoull(col[DIR_SIZE], NULL, 10);
		} else {
			unsigned short major, minor;
			fattr.f_size = 0;
			major = (unsigned short) simple_strtoul(col[DIR_SIZE], &s, 10);
			while (*s && (!isdigit(*s)))
				s++;
			minor = (unsigned short) simple_strtoul(s, NULL, 10);
			fattr.f_rdev = MKDEV(major, minor);
		}
		fattr.f_nlink = simple_strtoul(col[DIR_NLINK], NULL, 10);
		fattr.f_blksize = 4096;
		fattr.f_blocks = (fattr.f_size + 511) >> 9;

		month = get_month(col[DIR_MONTH]);
		/* some systems have month/day swapped (MacOS X) */
		if (month < 0) {
			day = simple_strtoul(col[DIR_MONTH], NULL, 10);
			mon = get_month(col[DIR_DAY]);
		} else {
			mon = (unsigned) month;
			day = simple_strtoul(col[DIR_DAY], NULL, 10);
		}
		
		s = col[DIR_YEAR];
		if (!strchr(s, ':')) {
			year = simple_strtoul(s, NULL, 10);
			hour = 12;
			min = 0;
		} else {
			year = this_year;
			if (mon > this_month) 
				year--;
			b = strchr(s, ':');
			*b = 0;
			hour = simple_strtoul(s, NULL, 10);
			min = simple_strtoul(++b, NULL, 10);
		}
		fattr.f_atime = fattr.f_mtime = fattr.f_ctime = mktime(year, mon + 1, day, hour, min, 0);

		if (S_ISLNK(mode) && ((s = strstr(name.name, " -> "))))
			*s = '\0';
		name.len = strlen(name.name);
		DEBUG("Name: %s, mode: %o, size: %llu, nlink: %d, month: %d, day: %d, year: %d, hour: %d, min: %d (time: %lu)\n", name.name, fattr.f_mode, fattr.f_size, fattr.f_nlink, mon, day, year, hour, min, fattr.f_atime);

		if (entry) {
			*entry = fattr;
		} else {
			result = shfs_fill_cache(filp, dirent, filldir, ctl, &name, &fattr);
			if (!result)
				break;
		}
	}
out:
	sock_unlock(info);
	return result;
}

static int
shell_readdir(struct shfs_sb_info *info, char *dir,
	      struct file *filp, void *dirent, filldir_t filldir, struct shfs_cache_control *ctl)
{
	return do_ls(info, dir, NULL, filp, dirent, filldir, ctl);
}

static int
shell_stat(struct shfs_sb_info *info, char *file, struct shfs_fattr *fattr)
{
	return do_ls(info, file, fattr, NULL, NULL, NULL, NULL);
}

/* returns 1 if file is "non-empty" but has zero size */
static int
shell_open(struct shfs_sb_info *info, char *file, int mode)
{
	char bufmode[3] = "";

	if (!check_path(file))
		return -ENAMETOOLONG;

	if (mode == O_RDONLY)
		strcpy(bufmode, "R");
	else if (mode == O_WRONLY)
		strcpy(bufmode, "W");
	else
		strcpy(bufmode, "RW");

	DEBUG("Open: %s (%s)\n", file, bufmode);
	return do_command(info, "s_open", "'%s' %s", file, bufmode);
}

/* data should be aligned (offset % count == 0), ino == 0 => normal read, != 0 => slow read */
static int
shell_read(struct shfs_sb_info *info, char *file, unsigned offset,
	   unsigned count, char *buffer, unsigned long ino)
{
	unsigned bs = 1, offset2 = offset, count2 = count;
	int result;
	char *s;

	DEBUG("<%s[%u, %u]\n", file, (unsigned)offset, (unsigned)count);

	if (!check_path(file))
		return -ENAMETOOLONG;
	/* read speedup if possible */
	if (count && !(offset % count)) {
		bs = count;
		offset2 = offset / count;
		count2 = 1;
	}

	if (!sock_lock(info))
		return -EINTR;
	
	s = info->sockbuf;
	if (ino) {
		strcpy(s, "s_sread"); s += strlen("s_sread");
	} else {
		strcpy(s, "s_read"); s += strlen("s_read");
	}
	s = get_ugid(info, s, SOCKBUF_SIZE - strlen(info->sockbuf));
	if (!s) {
		result = -ENAMETOOLONG;
		goto error;
	}
	if (ino) {
		result = snprintf(s, SOCKBUF_SIZE - (s - info->sockbuf), 
			"'%s' %u %u %u %u %u %lu\n", file, offset, count, bs, offset2, count2, ino);
	} else {
		result = snprintf(s, SOCKBUF_SIZE - (s - info->sockbuf), 
			"'%s' %u %u %u %u %u\n", file, offset, count, bs, offset2, count2);
	}
	if (result < 0) {
		result = -ENAMETOOLONG;
		goto error;
	}

	DEBUG("<%s", info->sockbuf);
	result = sock_write(info, (void *)info->sockbuf, strlen(info->sockbuf));
	if (result < 0)
		goto error;

	result = sock_readln(info, info->sockbuf, SOCKBUF_SIZE);
	if (result < 0) {
		set_garbage(info, 0, count);
		goto error;
	}
	switch (reply(info->sockbuf)) {
	case REP_PRELIM:
		break;
	case REP_EPERM:
		result = -EPERM;
		goto error;
	case REP_ENOENT:
		result = -ENOENT;
		goto error;
	default:
		set_garbage(info, 0, count);
		result = -EIO;
		goto error;
	}
	if (ino) {
		result = sock_readln(info, info->sockbuf, SOCKBUF_SIZE);
		if (result < 0)
			goto error;
		count = simple_strtoul(info->sockbuf, NULL, 10);
	}

	result = sock_read(info, buffer, count);
	if (result < 0)
		goto error;
	result = sock_readln(info, info->sockbuf, SOCKBUF_SIZE);
	if (result < 0)
		goto error;
	switch (reply(info->sockbuf)) {
	case REP_COMPLETE:
		break;
	case REP_EPERM:
		result = -EPERM;
		goto error;
	case REP_ENOENT:
		result = -ENOENT;
		goto error;
	default:
		result = -EIO;
		goto error;
	}

	result = count;
error:
	sock_unlock(info);
	DEBUG("<%d\n", result);
	return result;
}

static int
shell_write(struct shfs_sb_info *info, char *file, unsigned offset,
	    unsigned count, char *buffer, unsigned long ino)
{
	unsigned offset2 = offset, bs = 1;
	int result;
	char *s;
	
	if (!check_path(file))
		return -ENAMETOOLONG;
#if 0
	if (!count)
		return 0;
#endif	
	DEBUG(">%s[%u, %u]\n", file, (unsigned)offset, (unsigned)count);
	if (offset % info->fcache_size == 0) {
		offset2 = offset / info->fcache_size;
		bs = info->fcache_size;
	}
		
	if (!sock_lock(info))
		return -EINTR;

	s = info->sockbuf;
	strcpy(s, "s_write"); s += strlen("s_write");
	s = get_ugid(info, s, SOCKBUF_SIZE - strlen(info->sockbuf));
	if (!s) {
		result = -ENAMETOOLONG;
		goto error;
	}

	result = snprintf(s, SOCKBUF_SIZE - (s - info->sockbuf), 
		"'%s' %u %u %u %u %lu\n", file, offset, count, bs, offset2, ino);
	if (result < 0) {
		result = -ENAMETOOLONG;
		goto error;
	}

	DEBUG(">%s", info->sockbuf);
	result = sock_write(info, (void *)info->sockbuf, strlen(info->sockbuf));
	if (result < 0)
		goto error;
	result = sock_readln(info, info->sockbuf, SOCKBUF_SIZE);
	if (result < 0) {
		set_garbage(info, 1, count);
		goto error;
	}
	switch (reply(info->sockbuf)) {
	case REP_PRELIM:
		break;
	case REP_EPERM:
		result = -EPERM;
		goto error;
	case REP_ENOENT:
		result = -ENOENT;
		goto error;
	default:
		result = -EIO;
		goto error;
	}

	result = sock_write(info, buffer, count);
	if (result < 0)
		goto error;
	result = sock_readln(info, info->sockbuf, SOCKBUF_SIZE);
	if (result < 0)
		goto error;
	switch (reply(info->sockbuf)) {
	case REP_COMPLETE:
		break;
	case REP_EPERM:
		result = -EPERM;
		goto error;
	case REP_ENOENT:
		result = -ENOENT;
		goto error;
	case REP_ENOSPC:
		result = -ENOSPC;
		set_garbage(info, 1, count);
		goto error;
	default:
		result = -EIO;
		goto error;
	}

	result = count;
error:
	sock_unlock(info);
	DEBUG(">%d\n", result);
	return result;
}

static int
shell_mkdir(struct shfs_sb_info *info, char *dir)
{
	if (!check_path(dir))
		return -ENAMETOOLONG;

	DEBUG("Mkdir %s\n", dir);
	return do_command(info, "s_mkdir", "'%s'", dir);
}

static int
shell_rmdir(struct shfs_sb_info *info, char *dir)
{
	if (!check_path(dir))
		return -ENAMETOOLONG;

	DEBUG("Rmdir %s\n", dir);
	return do_command(info, "s_rmdir", "'%s'", dir);
}

static int
shell_rename(struct shfs_sb_info *info, char *old, char *new)
{
	if (!check_path(old) || !check_path(new))
		return -ENAMETOOLONG;

	DEBUG("Rename %s -> %s\n", old, new);
	return do_command(info, "s_mv", "'%s' '%s'", old, new);
}

static int
shell_unlink(struct shfs_sb_info *info, char *file)
{
	if (!check_path(file))
		return -ENAMETOOLONG;

	DEBUG("Remove %s\n", file);
	return do_command(info, "s_rm", "'%s'", file);
}

static int
shell_create(struct shfs_sb_info *info, char *file, int mode)
{
	if (!check_path(file))
		return -ENAMETOOLONG;

	DEBUG("Create %s (%o)\n", file, mode);
	return do_command(info, "s_creat", "'%s' %o", file, mode & S_IALLUGO);
}

static int
shell_link(struct shfs_sb_info *info, char *old, char *new)
{
	if (!check_path(old) || !check_path(new))
		return -ENAMETOOLONG;

	DEBUG("Link %s -> %s\n", old, new);
	return do_command(info, "s_ln", "'%s' '%s'", old, new);
}

static int
shell_symlink(struct shfs_sb_info *info, char *old, char *new)
{
	if (!check_path(old) || !check_path(new))
		return -ENAMETOOLONG;

	DEBUG("Symlink %s -> %s\n", old, new);
	return do_command(info, "s_sln", "'%s' '%s'", old, new);
}

static int
shell_readlink(struct shfs_sb_info *info, char *name, char *real_name)
{
	char *s;
	int result = 0;

	if (!check_path(name) || !check_path(real_name))
		return -ENAMETOOLONG;

	if (!sock_lock(info))
		return -EINTR;

	s = info->sockbuf;
	strcpy(s, "s_readlink"); s += strlen("s_readlink");
	s = get_ugid(info, s, SOCKBUF_SIZE - strlen(info->sockbuf));
	if (!s) {
		result = -ENAMETOOLONG;
		goto error;
	}
	if (s - info->sockbuf + strlen(name) + 4 > SOCKBUF_SIZE) {
		result = -ENAMETOOLONG;
		goto error;
	}
	strcpy(s, "'"); s++;
	strcpy(s, name); s += strlen(name);
	strcpy(s, "'"); s++;
	strcpy(s, "\n");

	DEBUG("Readlink %s\n", info->sockbuf);
	result = sock_write(info, (void *)info->sockbuf, strlen(info->sockbuf));
	if (result < 0)
		goto error;
	result = sock_readln(info, info->sockbuf, SOCKBUF_SIZE);
	if (result < 0)
		goto error;

	switch (reply(info->sockbuf)) {
	case REP_COMPLETE:
		result = -EIO;
		goto error;
	case REP_EPERM:
		result = -EPERM;
		goto error;
	}

	strncpy(real_name, info->sockbuf, SHFS_PATH_MAX-1);
	real_name[SHFS_PATH_MAX-1] = '\0';

	result = sock_readln(info, info->sockbuf, SOCKBUF_SIZE);
	if (result < 0)
		goto error;
	switch (reply(info->sockbuf)) {
	case REP_COMPLETE:
		result = 0;
		break;
	case REP_EPERM:
		result = -EPERM;
		goto error;
	default:
		info->garbage = 1;
		result = -EIO;
		goto error;
	}
error:
	sock_unlock(info);
	return result;
}

static int
shell_chmod(struct shfs_sb_info *info, char *file, umode_t mode)
{
	if (!check_path(file))
		return -ENAMETOOLONG;

	DEBUG("Chmod %o %s\n", mode, file);
	return do_command(info, "s_chmod", "'%s' %o", file, mode);
}

static int
shell_chown(struct shfs_sb_info *info, char *file, uid_t user)
{
	if (!check_path(file))
		return -ENAMETOOLONG;

	DEBUG("Chown %u %s\n", user, file);
	return do_command(info, "s_chown", "'%s' %u", file, user);
}

static int
shell_chgrp(struct shfs_sb_info *info, char *file, gid_t group)
{
	if (!check_path(file))
		return -ENAMETOOLONG;

	DEBUG("Chgrp %u %s\n", group, file);
	return do_command(info, "s_chgrp", "'%s' %u", file, group);
}

static int
shell_trunc(struct shfs_sb_info *info, char *file, loff_t size)
{
	unsigned seek = 1;

	if (!check_path(file))
		return -ENAMETOOLONG;

	DEBUG("Truncate %s %u\n", file, (unsigned)size);
	/* dd doesn't like bs=0 */
	if (size == 0) {
		seek = 0;
		size = 1;
	}
	return do_command(info, "s_trunc", "'%s' %u %u", file, (unsigned) size, seek);
}

/* this code is borrowed from dietlibc */

static int
__isleap(int year) {
  /* every fourth year is a leap year except for century years that are
   * not divisible by 400. */
/*  return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)); */
  return (!(year%4) && ((year%100) || !(year%400)));
}

/* days per month -- nonleap! */
const short __spm[12] =
  { 0,
    (31),
    (31+28),
    (31+28+31),
    (31+28+31+30),
    (31+28+31+30+31),
    (31+28+31+30+31+30),
    (31+28+31+30+31+30+31),
    (31+28+31+30+31+30+31+31),
    (31+28+31+30+31+30+31+31+30),
    (31+28+31+30+31+30+31+31+30+31),
    (31+28+31+30+31+30+31+31+30+31+30),
  };

/* seconds per day */
#define SPD 24*60*60

static char *
strctime(const time_t *timep, char *s)
{
	time_t i;
	int sec, min, hour;
	int day, month, year;
	time_t work = *timep % (SPD);
	
	sec = work % 60; work /= 60;
	min = work % 60; hour = work / 60;
	work = *timep / (SPD);

	for (i = 1970; ; i++) {
		time_t k = __isleap(i) ? 366 : 365;
		if (work >= k)
			work -= k;
		else
			break;
	}
	year = i;

	day=1;
	if (__isleap(i) && (work > 58)) {
		if (work == 59)
			day = 2; /* 29.2. */
		work -= 1;
	}

	for (i = 11; i && (__spm[i] > work); i--)
		;
	month = i;
 	day += work - __spm[i];

	sprintf(s, "%.4d%.2d%.2d%.2d%.2d.%.2d", year, month+1, day, hour, min, sec);

	return s;
}

static int
shell_settime(struct shfs_sb_info *info, char *file, int atime, int mtime, time_t *time)
{
	char str[20];
	
	if (!check_path(file))
		return -ENAMETOOLONG;

	strctime(time, str);
	DEBUG("Settime %s (%s%s) %s\n", str, atime ? "a" : "", mtime ? "m" : "", file);
	return do_command(info, "s_settime", "'%s' %s%s %s", file, atime ? "a" : "", mtime ? "m" : "", str);
}

static int
shell_statfs(struct shfs_sb_info *info, struct statfs *attr)
{
	char *s, *p;
	int result = 0;

	attr->f_type = SHFS_SUPER_MAGIC;
	attr->f_bsize = 4096;
	attr->f_blocks = 0;
	attr->f_bfree = 0;
	attr->f_bavail = 0;
	attr->f_files = 1;
	attr->f_bavail = 1;
	attr->f_namelen = SHFS_PATH_MAX;

	if (!sock_lock(info))
		return -EINTR;

	s = info->sockbuf;
	strcpy(s, "s_statfs"); s += strlen("s_statfs");
	s = get_ugid(info, s, SOCKBUF_SIZE - strlen(info->sockbuf));
	if (!s) {
		result = -ENAMETOOLONG;
		goto error;
	}
	strcpy(s, "\n");

	DEBUG("Statfs %s\n", info->sockbuf);
	result = sock_write(info, (void *)info->sockbuf, strlen(info->sockbuf));
	if (result < 0)
		goto error;
	result = sock_readln(info, info->sockbuf, SOCKBUF_SIZE);
	if (result < 0)
		goto error;

	s = info->sockbuf;
	if ((p = strsep(&s, " ")))
		attr->f_blocks = simple_strtoull(p, NULL, 10);
	if ((p = strsep(&s, " ")))
		attr->f_bfree = attr->f_blocks - simple_strtoull(p, NULL, 10);
	if ((p = strsep(&s, " ")))
		attr->f_bavail = simple_strtoull(p, NULL, 10);

	result = sock_readln(info, info->sockbuf, SOCKBUF_SIZE);
	if (result < 0)
		goto error;
	switch (reply(info->sockbuf)) {
	case REP_COMPLETE:
		result = 0;
		break;
	case REP_EPERM:
		result = -EPERM;
		goto error;
	default:
		info->garbage = 1;
		result = -EIO;
		goto error;
	}
error:
	sock_unlock(info);
	return result;
}

static int
shell_finish(struct shfs_sb_info *info)
{
	DEBUG("Finish\n");
	return do_command(info, "s_finish", "");
}

struct shfs_fileops shell_fops = {
	readdir:	shell_readdir,
	stat:		shell_stat,
	open:		shell_open,
	read:		shell_read,
	write:		shell_write,
	mkdir:		shell_mkdir,
	rmdir:		shell_rmdir,
	rename:		shell_rename,
	unlink:		shell_unlink,
	create:		shell_create,
	link:		shell_link,
	symlink:	shell_symlink,
	readlink:	shell_readlink,
	chmod:		shell_chmod,
	chown:		shell_chown,
	chgrp:		shell_chgrp,
	trunc:		shell_trunc,
	settime:	shell_settime,
	statfs:		shell_statfs,
	finish:		shell_finish,
};
