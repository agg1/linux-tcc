#ifndef _SHFS_H
#define _SHFS_H

#define PROTO_VERSION 2

/* response code */
#define REP_PRELIM	100
#define REP_COMPLETE	200
#define REP_NOP 	201
#define REP_NOTEMPTY	202		/* file with zero size but not empty */
#define REP_CONTINUE	300
#define REP_TRANSIENT	400
#define REP_ERROR	500
#define REP_EPERM	501
#define REP_ENOSPC	502
#define REP_ENOENT	503

#define SHFS_SUPER_MAGIC 0xD0D0

#ifdef __KERNEL__

#define SHFS_DEFAULT_TTL 20000
#define SHFS_PATH_MAX 512

#include <linux/types.h>

struct shfs_fattr {
	unsigned long 	f_ino;
	umode_t		f_mode;
	nlink_t		f_nlink;
	uid_t		f_uid;
	gid_t		f_gid;
	kdev_t		f_rdev;
	loff_t		f_size;
	time_t		f_atime;
	time_t		f_mtime;
	time_t		f_ctime;
	unsigned long 	f_blksize;
	unsigned long	f_blocks;
};

#endif  /* __KERNEL__ */

#endif	/* _SHFS_H */
