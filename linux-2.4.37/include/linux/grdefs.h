#ifndef GRDEFS_H
#define GRDEFS_H

/* Begin grsecurity status declarations */

enum {
	GR_READY = 0x01,
	GR_STATUS_INIT = 0x00	// disabled state
};

/* Begin  ACL declarations */

/* Role flags */

enum {
	GR_ROLE_USER = 0x0001,
	GR_ROLE_GROUP = 0x0002,
	GR_ROLE_DEFAULT = 0x0004,
	GR_ROLE_SPECIAL = 0x0008,
	GR_ROLE_AUTH = 0x0010,
	GR_ROLE_NOPW = 0x0020,
	GR_ROLE_GOD = 0x0040,
	GR_ROLE_LEARN = 0x0080,
	GR_ROLE_TPE = 0x0100,
	GR_ROLE_DOMAIN = 0x0200,
	GR_ROLE_PAM = 0x0400
};

/* ACL Subject and Object mode flags */
enum {
	GR_DELETED = 0x80000000
};

/* ACL Object-only mode flags */
enum {
	GR_READ 	= 0x00000001,
	GR_APPEND 	= 0x00000002,
	GR_WRITE 	= 0x00000004,
	GR_EXEC 	= 0x00000008,
	GR_FIND 	= 0x00000010,
	GR_INHERIT 	= 0x00000020,
	GR_SETID 	= 0x00000040,
	GR_CREATE 	= 0x00000080,
	GR_DELETE 	= 0x00000100,
	GR_LINK		= 0x00000200,
	GR_AUDIT_READ 	= 0x00000400,
	GR_AUDIT_APPEND = 0x00000800,
	GR_AUDIT_WRITE 	= 0x00001000,
	GR_AUDIT_EXEC 	= 0x00002000,
	GR_AUDIT_FIND 	= 0x00004000,
	GR_AUDIT_INHERIT= 0x00008000,
	GR_AUDIT_SETID 	= 0x00010000,
	GR_AUDIT_CREATE = 0x00020000,
	GR_AUDIT_DELETE = 0x00040000,
	GR_AUDIT_LINK	= 0x00080000,
	GR_PTRACERD 	= 0x00100000,
	GR_NOPTRACE	= 0x00200000,
	GR_SUPPRESS 	= 0x00400000,
	GR_NOLEARN 	= 0x00800000
};

#define GR_AUDITS (GR_AUDIT_READ | GR_AUDIT_WRITE | GR_AUDIT_APPEND | GR_AUDIT_EXEC | \
		   GR_AUDIT_FIND | GR_AUDIT_INHERIT | GR_AUDIT_SETID | \
		   GR_AUDIT_CREATE | GR_AUDIT_DELETE | GR_AUDIT_LINK)

/* ACL subject-only mode flags */
enum {
	GR_KILL 	= 0x00000001,
	GR_VIEW 	= 0x00000002,
	GR_PROTECTED 	= 0x00000004,
	GR_LEARN 	= 0x00000008,
	GR_OVERRIDE 	= 0x00000010,
	/* just a placeholder, this mode is only used in userspace */
	GR_DUMMY 	= 0x00000020,
	GR_PROTSHM 	= 0x00000040,
	GR_KILLPROC 	= 0x00000080,
	GR_KILLIPPROC	= 0x00000100,
	/* just a placeholder, this mode is only used in userspace */
	GR_NOTROJAN 	= 0x00000200,
	GR_PROTPROCFD 	= 0x00000400,
	GR_PROCACCT 	= 0x00000800,
	GR_RELAXPTRACE	= 0x00001000,
	GR_NESTED	= 0x00002000,
	GR_INHERITLEARN = 0x00004000,
	GR_PROCFIND	= 0x00008000,
	GR_POVERRIDE	= 0x00010000,
	GR_KERNELAUTH	= 0x00020000,
};

/* PaX flags */
enum {
	GR_PAX_ENABLE_SEGMEXEC  =  0x0001,
	GR_PAX_ENABLE_PAGEEXEC  =  0x0002,
	GR_PAX_ENABLE_MPROTECT  =  0x0004,
	GR_PAX_ENABLE_RANDMMAP  =  0x0008,
	GR_PAX_ENABLE_EMUTRAMP  =  0x0010,
	GR_PAX_DISABLE_SEGMEXEC =  0x0100,
	GR_PAX_DISABLE_PAGEEXEC  = 0x0200,
	GR_PAX_DISABLE_MPROTECT  = 0x0400,
	GR_PAX_DISABLE_RANDMMAP  = 0x0800,
	GR_PAX_DISABLE_EMUTRAMP  = 0x1000
};

enum {
	GR_ID_USER      = 0x01,
	GR_ID_GROUP     = 0x02,
};

enum {
	GR_ID_ALLOW     = 0x01,
	GR_ID_DENY      = 0x02,
};

#define GR_CRASH_RES	31
#define GR_UIDTABLE_MAX 500

/* begin resource learning section */
enum {
	GR_RLIM_CPU_BUMP = 60,
	GR_RLIM_FSIZE_BUMP = 50000,
	GR_RLIM_DATA_BUMP = 10000,
	GR_RLIM_STACK_BUMP = 1000,
	GR_RLIM_CORE_BUMP = 10000,
	GR_RLIM_RSS_BUMP = 500000,
	GR_RLIM_NPROC_BUMP = 1,
	GR_RLIM_NOFILE_BUMP = 5,
	GR_RLIM_MEMLOCK_BUMP = 50000,
	GR_RLIM_AS_BUMP = 500000,
	GR_RLIM_LOCKS_BUMP = 2
};

#endif
