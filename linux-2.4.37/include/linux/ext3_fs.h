/*
 *  linux/include/linux/ext3_fs.h
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/include/linux/minix_fs.h
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#ifndef _LINUX_EXT3_FS_H
#define _LINUX_EXT3_FS_H

#include <linux/types.h>

/*
 * The second extended filesystem constants/structures
 */

/*
 * Define EXT3FS_DEBUG to produce debug messages
 */
#undef EXT3FS_DEBUG

/*
 * Define EXT3_PREALLOCATE to preallocate data blocks for expanding files
 */
#undef  EXT3_PREALLOCATE /* @@@ Fix this! */
#define EXT3_DEFAULT_PREALLOC_BLOCKS	8

/*
 * The second extended file system version
 */
#define EXT3FS_DATE		"19 August 2002"
#define EXT3FS_VERSION		"2.4-0.9.19"

/*
 * Debug code
 */
#ifdef EXT3FS_DEBUG
#define ext3_debug(f, a...)						\
	do {								\
		printk (KERN_DEBUG "EXT3-fs DEBUG (%s, %d): %s:",	\
			__FILE__, __LINE__, __FUNCTION__);		\
		printk (KERN_DEBUG f, ## a);				\
	} while (0)
#else
#define ext3_debug(f, a...)	do {} while (0)
#endif

/*
 * Special inodes numbers
 */
#define	EXT3_BAD_INO		 1	/* Bad blocks inode */
#define EXT3_ROOT_INO		 2	/* Root inode */
#define EXT3_ACL_IDX_INO	 3	/* ACL inode */
#define EXT3_ACL_DATA_INO	 4	/* ACL inode */
#define EXT3_BOOT_LOADER_INO	 5	/* Boot loader inode */
#define EXT3_UNDEL_DIR_INO	 6	/* Undelete directory inode */
#define EXT3_RESIZE_INO		 7	/* Reserved group descriptors inode */
#define EXT3_JOURNAL_INO	 8	/* Journal inode */

/* First non-reserved inode for old ext3 filesystems */
#define EXT3_GOOD_OLD_FIRST_INO	11

/*
 * The second extended file system magic number
 */
#define EXT3_SUPER_MAGIC	0xEF53

/*
 * Maximal count of links to a file
 */
#define EXT3_LINK_MAX		32000

/*
 * Macro-instructions used to manage several block sizes
 */
#define EXT3_MIN_BLOCK_SIZE		1024
#define	EXT3_MAX_BLOCK_SIZE		4096
#define EXT3_MIN_BLOCK_LOG_SIZE		  10
#ifdef __KERNEL__
# define EXT3_BLOCK_SIZE(s)		((s)->s_blocksize)
#else
# define EXT3_BLOCK_SIZE(s)		(EXT3_MIN_BLOCK_SIZE << (s)->s_log_block_size)
#endif
#define EXT3_ACLE_PER_BLOCK(s)		(EXT3_BLOCK_SIZE(s) / sizeof (struct ext3_acl_entry))
#define	EXT3_ADDR_PER_BLOCK(s)		(EXT3_BLOCK_SIZE(s) / sizeof (__u32))
#ifdef __KERNEL__
# define EXT3_BLOCK_SIZE_BITS(s)	((s)->s_blocksize_bits)
#else
# define EXT3_BLOCK_SIZE_BITS(s)	((s)->s_log_block_size + 10)
#endif
#ifdef __KERNEL__
#define	EXT3_ADDR_PER_BLOCK_BITS(s)	((s)->u.ext3_sb.s_addr_per_block_bits)
#define EXT3_INODE_SIZE(s)		((s)->u.ext3_sb.s_inode_size)
#define EXT3_FIRST_INO(s)		((s)->u.ext3_sb.s_first_ino)
#else
#define EXT3_INODE_SIZE(s)	(((s)->s_rev_level == EXT3_GOOD_OLD_REV) ? \
				 EXT3_GOOD_OLD_INODE_SIZE : \
				 (s)->s_inode_size)
#define EXT3_FIRST_INO(s)	(((s)->s_rev_level == EXT3_GOOD_OLD_REV) ? \
				 EXT3_GOOD_OLD_FIRST_INO : \
				 (s)->s_first_ino)
#endif

/*
 * Macro-instructions used to manage fragments
 */
#define EXT3_MIN_FRAG_SIZE		1024
#define	EXT3_MAX_FRAG_SIZE		4096
#define EXT3_MIN_FRAG_LOG_SIZE		  10
#ifdef __KERNEL__
# define EXT3_FRAG_SIZE(s)		((s)->u.ext3_sb.s_frag_size)
# define EXT3_FRAGS_PER_BLOCK(s)	((s)->u.ext3_sb.s_frags_per_block)
#else
# define EXT3_FRAG_SIZE(s)		(EXT3_MIN_FRAG_SIZE << (s)->s_log_frag_size)
# define EXT3_FRAGS_PER_BLOCK(s)	(EXT3_BLOCK_SIZE(s) / EXT3_FRAG_SIZE(s))
#endif

/*
 * ACL structures
 */
struct ext3_acl_header	/* Header of Access Control Lists */
{
	__u32	aclh_size;
	__u32	aclh_file_count;
	__u32	aclh_acle_count;
	__u32	aclh_first_acle;
};

struct ext3_acl_entry	/* Access Control List Entry */
{
	__u32	acle_size;
	__u16	acle_perms;	/* Access permissions */
	__u16	acle_type;	/* Type of entry */
	__u16	acle_tag;	/* User or group identity */
	__u16	acle_pad1;
	__u32	acle_next;	/* Pointer on next entry for the */
					/* same inode or on next free entry */
};

/*
 * Structure of a blocks group descriptor
 */
struct ext3_group_desc
{
	__u32	bg_block_bitmap;		/* Blocks bitmap block */
	__u32	bg_inode_bitmap;		/* Inodes bitmap block */
	__u32	bg_inode_table;		/* Inodes table block */
	__u16	bg_free_blocks_count;	/* Free blocks count */
	__u16	bg_free_inodes_count;	/* Free inodes count */
	__u16	bg_used_dirs_count;	/* Directories count */
	__u16	bg_pad;
	__u32	bg_reserved[3];
};

/*
 * Macro-instructions used to manage group descriptors
 */
#ifdef __KERNEL__
# define EXT3_BLOCKS_PER_GROUP(s)	((s)->u.ext3_sb.s_blocks_per_group)
# define EXT3_DESC_PER_BLOCK(s)		((s)->u.ext3_sb.s_desc_per_block)
# define EXT3_INODES_PER_GROUP(s)	((s)->u.ext3_sb.s_inodes_per_group)
# define EXT3_DESC_PER_BLOCK_BITS(s)	((s)->u.ext3_sb.s_desc_per_block_bits)
#else
# define EXT3_BLOCKS_PER_GROUP(s)	((s)->s_blocks_per_group)
# define EXT3_DESC_PER_BLOCK(s)		(EXT3_BLOCK_SIZE(s) / sizeof (struct ext3_group_desc))
# define EXT3_INODES_PER_GROUP(s)	((s)->s_inodes_per_group)
#endif

/*
 * Constants relative to the data blocks
 */
#define	EXT3_NDIR_BLOCKS		12
#define	EXT3_IND_BLOCK			EXT3_NDIR_BLOCKS
#define	EXT3_DIND_BLOCK			(EXT3_IND_BLOCK + 1)
#define	EXT3_TIND_BLOCK			(EXT3_DIND_BLOCK + 1)
#define	EXT3_N_BLOCKS			(EXT3_TIND_BLOCK + 1)

/*
 * Inode flags
 */
#define	EXT3_SECRM_FL			0x00000001 /* Secure deletion */
#define	EXT3_UNRM_FL			0x00000002 /* Undelete */
#define	EXT3_COMPR_FL			0x00000004 /* Compress file */
#define EXT3_SYNC_FL			0x00000008 /* Synchronous updates */
#define EXT3_IMMUTABLE_FL		0x00000010 /* Immutable file */
#define EXT3_APPEND_FL			0x00000020 /* writes to file may only append */
#define EXT3_NODUMP_FL			0x00000040 /* do not dump file */
#define EXT3_NOATIME_FL			0x00000080 /* do not update atime */
/* Reserved for compression usage... */
#define EXT3_DIRTY_FL			0x00000100
#define EXT3_COMPRBLK_FL		0x00000200 /* One or more compressed clusters */
#define EXT3_NOCOMPR_FL			0x00000400 /* Don't compress */
#define EXT3_ECOMPR_FL			0x00000800 /* Compression error */
/* End compression flags --- maybe not all used */
#define EXT3_INDEX_FL			0x00001000 /* hash-indexed directory */
#define EXT3_IMAGIC_FL			0x00002000 /* AFS directory */
#define EXT3_JOURNAL_DATA_FL		0x00004000 /* file data should be journaled */
#define EXT3_RESERVED_FL		0x80000000 /* reserved for ext3 lib */

#define EXT3_FL_USER_VISIBLE		0x00005FFF /* User visible flags */
#define EXT3_FL_USER_MODIFIABLE		0x000000FF /* User modifiable flags */

/*
 * Inode dynamic state flags
 */
#define EXT3_STATE_JDATA		0x00000001 /* journaled data exists */
#define EXT3_STATE_NEW			0x00000002 /* inode is newly created */

/*
 * ioctl commands
 */
#define	EXT3_IOC_GETFLAGS		_IOR('f', 1, long)
#define	EXT3_IOC_SETFLAGS		_IOW('f', 2, long)
#define	EXT3_IOC_GETVERSION		_IOR('f', 3, long)
#define	EXT3_IOC_SETVERSION		_IOW('f', 4, long)
#define	EXT3_IOC_GETVERSION_OLD		_IOR('v', 1, long)
#define	EXT3_IOC_SETVERSION_OLD		_IOW('v', 2, long)
#ifdef CONFIG_JBD_DEBUG
#define EXT3_IOC_WAIT_FOR_READONLY	_IOR('f', 99, long)
#endif

/*
 * Structure of an inode on the disk
 */
struct ext3_inode {
	__u16	i_mode;		/* File mode */
	__u16	i_uid;		/* Low 16 bits of Owner Uid */
	__u32	i_size;		/* Size in bytes */
	__u32	i_atime;	/* Access time */
	__u32	i_ctime;	/* Creation time */
	__u32	i_mtime;	/* Modification time */
	__u32	i_dtime;	/* Deletion Time */
	__u16	i_gid;		/* Low 16 bits of Group Id */
	__u16	i_links_count;	/* Links count */
	__u32	i_blocks;	/* Blocks count */
	__u32	i_flags;	/* File flags */
	union {
		struct {
			__u32  l_i_reserved1;
		} linux1;
		struct {
			__u32  h_i_translator;
		} hurd1;
		struct {
			__u32  m_i_reserved1;
		} masix1;
	} osd1;				/* OS dependent 1 */
	__u32	i_block[EXT3_N_BLOCKS];/* Pointers to blocks */
	__u32	i_generation;	/* File version (for NFS) */
	__u32	i_file_acl;	/* File ACL */
	__u32	i_dir_acl;	/* Directory ACL */
	__u32	i_faddr;	/* Fragment address */
	union {
		struct {
			__u8	l_i_frag;	/* Fragment number */
			__u8	l_i_fsize;	/* Fragment size */
			__u16	i_pad1;
			__u16	l_i_uid_high;	/* these 2 fields    */
			__u16	l_i_gid_high;	/* were reserved2[0] */
			__u32	l_i_reserved2;
		} linux2;
		struct {
			__u8	h_i_frag;	/* Fragment number */
			__u8	h_i_fsize;	/* Fragment size */
			__u16	h_i_mode_high;
			__u16	h_i_uid_high;
			__u16	h_i_gid_high;
			__u32	h_i_author;
		} hurd2;
		struct {
			__u8	m_i_frag;	/* Fragment number */
			__u8	m_i_fsize;	/* Fragment size */
			__u16	m_pad1;
			__u32	m_i_reserved2[2];
		} masix2;
	} osd2;				/* OS dependent 2 */
};

#define i_size_high	i_dir_acl

#if defined(__KERNEL__) || defined(__linux__)
#define i_reserved1	osd1.linux1.l_i_reserved1
#define i_frag		osd2.linux2.l_i_frag
#define i_fsize		osd2.linux2.l_i_fsize
#define i_uid_low	i_uid
#define i_gid_low	i_gid
#define i_uid_high	osd2.linux2.l_i_uid_high
#define i_gid_high	osd2.linux2.l_i_gid_high
#define i_reserved2	osd2.linux2.l_i_reserved2

#elif defined(__GNU__)

#define i_translator	osd1.hurd1.h_i_translator
#define i_frag		osd2.hurd2.h_i_frag;
#define i_fsize		osd2.hurd2.h_i_fsize;
#define i_uid_high	osd2.hurd2.h_i_uid_high
#define i_gid_high	osd2.hurd2.h_i_gid_high
#define i_author	osd2.hurd2.h_i_author

#elif defined(__masix__)

#define i_reserved1	osd1.masix1.m_i_reserved1
#define i_frag		osd2.masix2.m_i_frag
#define i_fsize		osd2.masix2.m_i_fsize
#define i_reserved2	osd2.masix2.m_i_reserved2

#endif /* defined(__KERNEL__) || defined(__linux__) */

/*
 * File system states
 */
#define	EXT3_VALID_FS			0x0001	/* Unmounted cleanly */
#define	EXT3_ERROR_FS			0x0002	/* Errors detected */
#define	EXT3_ORPHAN_FS			0x0004	/* Orphans being recovered */

/*
 * Mount flags
 */
#define EXT3_MOUNT_CHECK		0x0001	/* Do mount-time checks */
#define EXT3_MOUNT_GRPID		0x0004	/* Create files with directory's group */
#define EXT3_MOUNT_DEBUG		0x0008	/* Some debugging messages */
#define EXT3_MOUNT_ERRORS_CONT		0x0010	/* Continue on errors */
#define EXT3_MOUNT_ERRORS_RO		0x0020	/* Remount fs ro on errors */
#define EXT3_MOUNT_ERRORS_PANIC		0x0040	/* Panic on errors */
#define EXT3_MOUNT_MINIX_DF		0x0080	/* Mimics the Minix statfs */
#define EXT3_MOUNT_NOLOAD		0x0100	/* Don't use existing journal*/
#define EXT3_MOUNT_ABORT		0x0200	/* Fatal error detected */
#define EXT3_MOUNT_DATA_FLAGS		0x0C00	/* Mode for data writes: */
  #define EXT3_MOUNT_JOURNAL_DATA	0x0400	/* Write data to journal */
  #define EXT3_MOUNT_ORDERED_DATA	0x0800	/* Flush data before commit */
  #define EXT3_MOUNT_WRITEBACK_DATA	0x0C00	/* No data ordering */
#define EXT3_MOUNT_UPDATE_JOURNAL	0x1000	/* Update the journal format */
#define EXT3_MOUNT_NO_UID32		0x2000  /* Disable 32-bit UIDs */

/* Compatibility, for having both ext2_fs.h and ext3_fs.h included at once */
#ifndef _LINUX_EXT2_FS_H
#define clear_opt(o, opt)		o &= ~EXT3_MOUNT_##opt
#define set_opt(o, opt)			o |= EXT3_MOUNT_##opt
#define test_opt(sb, opt)		((sb)->u.ext3_sb.s_mount_opt & \
					 EXT3_MOUNT_##opt)
#else
#define EXT2_MOUNT_NOLOAD		EXT3_MOUNT_NOLOAD
#define EXT2_MOUNT_ABORT		EXT3_MOUNT_ABORT
#define EXT2_MOUNT_DATA_FLAGS		EXT3_MOUNT_DATA_FLAGS
#endif

#define ext3_set_bit			ext2_set_bit
#define ext3_clear_bit			ext2_clear_bit
#define ext3_test_bit			ext2_test_bit
#define ext3_find_first_zero_bit	ext2_find_first_zero_bit
#define ext3_find_next_zero_bit		ext2_find_next_zero_bit

/*
 * Maximal mount counts between two filesystem checks
 */
#define EXT3_DFL_MAX_MNT_COUNT		20	/* Allow 20 mounts */
#define EXT3_DFL_CHECKINTERVAL		0	/* Don't use interval check */

/*
 * Behaviour when detecting errors
 */
#define EXT3_ERRORS_CONTINUE		1	/* Continue execution */
#define EXT3_ERRORS_RO			2	/* Remount fs read-only */
#define EXT3_ERRORS_PANIC		3	/* Panic */
#define EXT3_ERRORS_DEFAULT		EXT3_ERRORS_CONTINUE

/*
 * Structure of the super block
 */
struct ext3_super_block {
/*00*/	__u32	s_inodes_count;		/* Inodes count */
	__u32	s_blocks_count;		/* Blocks count */
	__u32	s_r_blocks_count;	/* Reserved blocks count */
	__u32	s_free_blocks_count;	/* Free blocks count */
/*10*/	__u32	s_free_inodes_count;	/* Free inodes count */
	__u32	s_first_data_block;	/* First Data Block */
	__u32	s_log_block_size;	/* Block size */
	__s32	s_log_frag_size;	/* Fragment size */
/*20*/	__u32	s_blocks_per_group;	/* # Blocks per group */
	__u32	s_frags_per_group;	/* # Fragments per group */
	__u32	s_inodes_per_group;	/* # Inodes per group */
	__u32	s_mtime;		/* Mount time */
/*30*/	__u32	s_wtime;		/* Write time */
	__u16	s_mnt_count;		/* Mount count */
	__s16	s_max_mnt_count;	/* Maximal mount count */
	__u16	s_magic;		/* Magic signature */
	__u16	s_state;		/* File system state */
	__u16	s_errors;		/* Behaviour when detecting errors */
	__u16	s_minor_rev_level;	/* minor revision level */
/*40*/	__u32	s_lastcheck;		/* time of last check */
	__u32	s_checkinterval;	/* max. time between checks */
	__u32	s_creator_os;		/* OS */
	__u32	s_rev_level;		/* Revision level */
/*50*/	__u16	s_def_resuid;		/* Default uid for reserved blocks */
	__u16	s_def_resgid;		/* Default gid for reserved blocks */
	/*
	 * These fields are for EXT3_DYNAMIC_REV superblocks only.
	 *
	 * Note: the difference between the compatible feature set and
	 * the incompatible feature set is that if there is a bit set
	 * in the incompatible feature set that the kernel doesn't
	 * know about, it should refuse to mount the filesystem.
	 *
	 * e2fsck's requirements are more strict; if it doesn't know
	 * about a feature in either the compatible or incompatible
	 * feature set, it must abort and not try to meddle with
	 * things it doesn't understand...
	 */
	__u32	s_first_ino;		/* First non-reserved inode */
	__u16   s_inode_size;		/* size of inode structure */
	__u16	s_block_group_nr;	/* block group # of this superblock */
	__u32	s_feature_compat;	/* compatible feature set */
/*60*/	__u32	s_feature_incompat;	/* incompatible feature set */
	__u32	s_feature_ro_compat;	/* readonly-compatible feature set */
/*68*/	__u8	s_uuid[16];		/* 128-bit uuid for volume */
/*78*/	char	s_volume_name[16];	/* volume name */
/*88*/	char	s_last_mounted[64];	/* directory where last mounted */
/*C8*/	__u32	s_algorithm_usage_bitmap; /* For compression */
	/*
	 * Performance hints.  Directory preallocation should only
	 * happen if the EXT3_FEATURE_COMPAT_DIR_PREALLOC flag is on.
	 */
	__u8	s_prealloc_blocks;	/* Nr of blocks to try to preallocate*/
	__u8	s_prealloc_dir_blocks;	/* Nr to preallocate for dirs */
	__u16	s_padding1;
	/*
	 * Journaling support valid if EXT3_FEATURE_COMPAT_HAS_JOURNAL set.
	 */
/*D0*/	__u8	s_journal_uuid[16];	/* uuid of journal superblock */
/*E0*/	__u32	s_journal_inum;		/* inode number of journal file */
	__u32	s_journal_dev;		/* device number of journal file */
	__u32	s_last_orphan;		/* start of list of inodes to delete */
	__u32	s_hash_seed[4];		/* HTREE hash seed */
	__u8	s_def_hash_version;	/* Default hash version to use */
	__u8	s_reserved_char_pad;
	__u16	s_reserved_word_pad;
	__u32	s_default_mount_opts;
	__u32	s_first_meta_bg; 	/* First metablock block group */
	__u32	s_reserved[190];	/* Padding to the end of the block */
};

#ifdef __KERNEL__
#define EXT3_SB(sb)	(&((sb)->u.ext3_sb))
#define EXT3_I(inode)	(&((inode)->u.ext3_i))
#else
/* Assume that user mode programs are passing in an ext3fs superblock, not
 * a kernel struct super_block.  This will allow us to call the feature-test
 * macros from user land. */
#define EXT3_SB(sb)	(sb)
#endif

#define NEXT_ORPHAN(inode) (inode)->u.ext3_i.i_dtime

/*
 * Codes for operating systems
 */
#define EXT3_OS_LINUX		0
#define EXT3_OS_HURD		1
#define EXT3_OS_MASIX		2
#define EXT3_OS_FREEBSD		3
#define EXT3_OS_LITES		4

/*
 * Revision levels
 */
#define EXT3_GOOD_OLD_REV	0	/* The good old (original) format */
#define EXT3_DYNAMIC_REV	1	/* V2 format w/ dynamic inode sizes */

#define EXT3_CURRENT_REV	EXT3_GOOD_OLD_REV
#define EXT3_MAX_SUPP_REV	EXT3_DYNAMIC_REV

#define EXT3_GOOD_OLD_INODE_SIZE 128

/*
 * Feature set definitions
 */

#define EXT3_HAS_COMPAT_FEATURE(sb,mask)			\
	( EXT3_SB(sb)->s_es->s_feature_compat & cpu_to_le32(mask) )
#define EXT3_HAS_RO_COMPAT_FEATURE(sb,mask)			\
	( EXT3_SB(sb)->s_es->s_feature_ro_compat & cpu_to_le32(mask) )
#define EXT3_HAS_INCOMPAT_FEATURE(sb,mask)			\
	( EXT3_SB(sb)->s_es->s_feature_incompat & cpu_to_le32(mask) )
#define EXT3_SET_COMPAT_FEATURE(sb,mask)			\
	EXT3_SB(sb)->s_es->s_feature_compat |= cpu_to_le32(mask)
#define EXT3_SET_RO_COMPAT_FEATURE(sb,mask)			\
	EXT3_SB(sb)->s_es->s_feature_ro_compat |= cpu_to_le32(mask)
#define EXT3_SET_INCOMPAT_FEATURE(sb,mask)			\
	EXT3_SB(sb)->s_es->s_feature_incompat |= cpu_to_le32(mask)
#define EXT3_CLEAR_COMPAT_FEATURE(sb,mask)			\
	EXT3_SB(sb)->s_es->s_feature_compat &= ~cpu_to_le32(mask)
#define EXT3_CLEAR_RO_COMPAT_FEATURE(sb,mask)			\
	EXT3_SB(sb)->s_es->s_feature_ro_compat &= ~cpu_to_le32(mask)
#define EXT3_CLEAR_INCOMPAT_FEATURE(sb,mask)			\
	EXT3_SB(sb)->s_es->s_feature_incompat &= ~cpu_to_le32(mask)

#define EXT3_FEATURE_COMPAT_DIR_PREALLOC	0x0001
#define EXT3_FEATURE_COMPAT_IMAGIC_INODES	0x0002
#define EXT3_FEATURE_COMPAT_HAS_JOURNAL		0x0004
#define EXT3_FEATURE_COMPAT_EXT_ATTR		0x0008
#define EXT3_FEATURE_COMPAT_RESIZE_INODE	0x0010
#define EXT3_FEATURE_COMPAT_DIR_INDEX		0x0020

#define EXT3_FEATURE_RO_COMPAT_SPARSE_SUPER	0x0001
#define EXT3_FEATURE_RO_COMPAT_LARGE_FILE	0x0002
#define EXT3_FEATURE_RO_COMPAT_BTREE_DIR	0x0004

#define EXT3_FEATURE_INCOMPAT_COMPRESSION	0x0001
#define EXT3_FEATURE_INCOMPAT_FILETYPE		0x0002
#define EXT3_FEATURE_INCOMPAT_RECOVER		0x0004 /* Needs recovery */
#define EXT3_FEATURE_INCOMPAT_JOURNAL_DEV	0x0008 /* Journal device */
#define EXT3_FEATURE_INCOMPAT_META_BG		0x0010

#define EXT3_FEATURE_COMPAT_SUPP	0
#define EXT3_FEATURE_INCOMPAT_SUPP	(EXT3_FEATURE_INCOMPAT_FILETYPE| \
					 EXT3_FEATURE_INCOMPAT_RECOVER| \
					 EXT3_FEATURE_INCOMPAT_META_BG)
#define EXT3_FEATURE_RO_COMPAT_SUPP	(EXT3_FEATURE_RO_COMPAT_SPARSE_SUPER| \
					 EXT3_FEATURE_RO_COMPAT_LARGE_FILE| \
					 EXT3_FEATURE_RO_COMPAT_BTREE_DIR)

/*
 * Default mount options
 */
#define EXT3_DEFM_DEBUG		0x0001
#define EXT3_DEFM_BSDGROUPS	0x0002
#define EXT3_DEFM_XATTR_USER	0x0004
#define EXT3_DEFM_ACL		0x0008
#define EXT3_DEFM_UID16		0x0010
#define EXT3_DEFM_JMODE		0x0060
#define EXT3_DEFM_JMODE_DATA	0x0020
#define EXT3_DEFM_JMODE_ORDERED	0x0040
#define EXT3_DEFM_JMODE_WBACK	0x0060

#define        EXT3_DEF_RESUID         0
#define        EXT3_DEF_RESGID         0

/*
 * Structure of a directory entry
 */
#define EXT3_NAME_LEN 255

struct ext3_dir_entry {
	__u32	inode;			/* Inode number */
	__u16	rec_len;		/* Directory entry length */
	__u16	name_len;		/* Name length */
	char	name[EXT3_NAME_LEN];	/* File name */
};

/*
 * The new version of the directory entry.  Since EXT3 structures are
 * stored in intel byte order, and the name_len field could never be
 * bigger than 255 chars, it's safe to reclaim the extra byte for the
 * file_type field.
 */
struct ext3_dir_entry_2 {
	__u32	inode;			/* Inode number */
	__u16	rec_len;		/* Directory entry length */
	__u8	name_len;		/* Name length */
	__u8	file_type;
	char	name[EXT3_NAME_LEN];	/* File name */
};

/*
 * Ext3 directory file types.  Only the low 3 bits are used.  The
 * other bits are reserved for now.
 */
#define EXT3_FT_UNKNOWN		0
#define EXT3_FT_REG_FILE	1
#define EXT3_FT_DIR		2
#define EXT3_FT_CHRDEV		3
#define EXT3_FT_BLKDEV		4
#define EXT3_FT_FIFO		5
#define EXT3_FT_SOCK		6
#define EXT3_FT_SYMLINK		7

#define EXT3_FT_MAX		8

/*
 * EXT3_DIR_PAD defines the directory entries boundaries
 *
 * NOTE: It must be a multiple of 4
 */
#define EXT3_DIR_PAD			4
#define EXT3_DIR_ROUND			(EXT3_DIR_PAD - 1)
#define EXT3_DIR_REC_LEN(name_len)	(((name_len) + 8 + EXT3_DIR_ROUND) & \
					 ~EXT3_DIR_ROUND)

#ifdef __KERNEL__
/*
 * Describe an inode's exact location on disk and in memory
 */
struct ext3_iloc
{
	struct buffer_head *bh;
	struct ext3_inode *raw_inode;
	unsigned long block_group;
};

/*
 * Function prototypes
 */

/*
 * Ok, these declarations are also in <linux/kernel.h> but none of the
 * ext3 source programs needs to include it so they are duplicated here.
 */
# define NORET_TYPE    /**/
# define ATTRIB_NORET  __attribute__((noreturn))
# define NORET_AND     noreturn,

/* balloc.c */
extern int ext3_bg_has_super(struct super_block *sb, int group);
extern unsigned long ext3_bg_num_gdb(struct super_block *sb, int group);
extern int ext3_new_block (handle_t *, struct inode *, unsigned long,
					    __u32 *, __u32 *, int *);
extern void ext3_free_blocks (handle_t *, struct inode *, unsigned long,
			      unsigned long);
extern unsigned long ext3_count_free_blocks (struct super_block *);
extern void ext3_check_blocks_bitmap (struct super_block *);
extern struct ext3_group_desc * ext3_get_group_desc(struct super_block * sb,
						    unsigned int block_group,
						    struct buffer_head ** bh);

/* dir.c */
extern int ext3_check_dir_entry(const char *, struct inode *,
				struct ext3_dir_entry_2 *, struct buffer_head *,
				unsigned long);
/* fsync.c */
extern int ext3_sync_file (struct file *, struct dentry *, int);

/* ialloc.c */
extern struct inode * ext3_new_inode (handle_t *, const struct inode *, int);
extern void ext3_free_inode (handle_t *, struct inode *);
extern struct inode * ext3_orphan_get (struct super_block *, unsigned long);
extern unsigned long ext3_count_free_inodes (struct super_block *);
extern void ext3_check_inodes_bitmap (struct super_block *);
extern unsigned long ext3_count_free (struct buffer_head *, unsigned);

/* inode.c */
extern struct buffer_head * ext3_getblk (handle_t *, struct inode *, long, int, int *);
extern struct buffer_head * ext3_bread (handle_t *, struct inode *, int, int, int *);

extern int  ext3_get_inode_loc (struct inode *, struct ext3_iloc *);
extern void ext3_read_inode (struct inode *);
extern void ext3_write_inode (struct inode *, int);
extern int  ext3_setattr (struct dentry *, struct iattr *);
extern void ext3_put_inode (struct inode *);
extern void ext3_delete_inode (struct inode *);
extern int  ext3_sync_inode (handle_t *, struct inode *);
extern void ext3_discard_prealloc (struct inode *);
extern void ext3_dirty_inode(struct inode *);
extern int ext3_change_inode_journal_flag(struct inode *, int);
extern void ext3_truncate (struct inode *);
extern void ext3_set_inode_flags(struct inode *);

/* ioctl.c */
extern int ext3_ioctl (struct inode *, struct file *, unsigned int,
		       unsigned long);

/* namei.c */
extern int ext3_orphan_add(handle_t *, struct inode *);
extern int ext3_orphan_del(handle_t *, struct inode *);

/* super.c */
extern void ext3_error (struct super_block *, const char *, const char *, ...)
	__attribute__ ((format (printf, 3, 4)));
extern void __ext3_std_error (struct super_block *, const char *, int);
extern void ext3_abort (struct super_block *, const char *, const char *, ...)
	__attribute__ ((format (printf, 3, 4)));
extern NORET_TYPE void ext3_panic (struct super_block *, const char *,
				   const char *, ...)
	__attribute__ ((NORET_AND format (printf, 3, 4)));
extern void ext3_warning (struct super_block *, const char *, const char *, ...)
	__attribute__ ((format (printf, 3, 4)));
extern void ext3_update_dynamic_rev (struct super_block *sb);
extern void ext3_put_super (struct super_block *);
extern void ext3_write_super (struct super_block *);
extern void ext3_write_super_lockfs (struct super_block *);
extern void ext3_unlockfs (struct super_block *);
extern int ext3_remount (struct super_block *, int *, char *);
extern struct super_block * ext3_read_super (struct super_block *,void *,int);
extern int ext3_statfs (struct super_block *, struct statfs *);

#define ext3_std_error(sb, errno)				\
do {								\
	if ((errno))						\
		__ext3_std_error((sb), __FUNCTION__, (errno));	\
} while (0)
extern const char *ext3_decode_error(struct super_block *sb, int errno, char nbuf[16]);

/*
 * Inodes and files operations
 */

/* dir.c */
extern const struct file_operations ext3_dir_operations;

/* file.c */
extern const struct inode_operations ext3_file_inode_operations;
extern const struct file_operations ext3_file_operations;

/* inode.c */
extern const struct address_space_operations ext3_aops;

/* namei.c */
extern const struct inode_operations ext3_dir_inode_operations;

/* symlink.c */
extern const struct inode_operations ext3_fast_symlink_inode_operations;


#endif	/* __KERNEL__ */

#endif	/* _LINUX_EXT3_FS_H */
