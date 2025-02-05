#ifndef GR_ACL_H
#define GR_ACL_H

#include <linux/grdefs.h>
#include <linux/resource.h>

#include <asm/resource.h>

/* Major status information */

#define GR_VERSION  "grsecurity 2.1.14"
#define GRSECURITY_VERSION 0x2114

enum {
	GR_SHUTDOWN = 0,
	GR_ENABLE = 1,
	GR_SPROLE = 2,
	GR_RELOAD = 3,
	GR_SEGVMOD = 4,
	GR_STATUS = 5,
	GR_UNSPROLE = 6,
	GR_PASSSET = 7,
	GR_SPROLEPAM = 8,
};

/* Password setup definitions
 * kernel/grhash.c */
enum {
	GR_PW_LEN = 128,
	GR_SALT_LEN = 16,
	GR_SHA_LEN = 32,
};

enum {
	GR_SPROLE_LEN = 64,
};

#define GR_NLIMITS 32

/* Begin Data Structures */

struct sprole_pw {
	unsigned char *rolename;
	unsigned char salt[GR_SALT_LEN];
	unsigned char sum[GR_SHA_LEN];	/* 256-bit SHA hash of the password */
};

struct name_entry {
	__u32 key;
	ino_t inode;
	__u32 device;
	char *name;
	__u16 len;
	__u8 deleted;
	struct name_entry *prev;
	struct name_entry *next;
};

struct inodev_entry {
	struct name_entry *nentry;
	struct inodev_entry *prev;
	struct inodev_entry *next;
};

struct acl_role_db {
	struct acl_role_label **r_hash;
	__u32 r_size;
};

struct name_db {
	struct name_entry **n_hash;
	__u32 n_size;
};

struct inodev_db {
	struct inodev_entry **i_hash;
	__u32 i_size;
};

struct crash_uid {
	uid_t uid;
	unsigned long expires;
};

struct gr_hash_struct {
	void **table;
	void **nametable;
	void *first;
	__u32 table_size;
	__u32 used_size;
	int type;
};

/* Userspace Grsecurity ACL data structures */
struct acl_subject_label {
	char *filename;
	ino_t inode;
	__u32 device;
	__u32 mode;
	__u32 cap_mask;
	__u32 cap_mask_unused;
	__u32 cap_lower;
	__u32 cap_lower_unused;

	struct rlimit res[GR_NLIMITS];
	__u32 resmask;

	__u8 user_trans_type;
	__u8 group_trans_type;
	uid_t *user_transitions;
	gid_t *group_transitions;
	__u16 user_trans_num;
	__u16 group_trans_num;

	__u32 ip_proto[8];
	__u32 ip_type;
	struct acl_ip_label **ips;
	__u32 ip_num;
	__u32 inaddr_any_override;

	__u32 crashes;
	unsigned long expires;

	struct acl_subject_label *parent_subject;
	struct gr_hash_struct *hash;
	struct acl_subject_label *prev;
	struct acl_subject_label *next;

	struct acl_object_label **obj_hash;
	__u32 obj_hash_size;
	__u16 pax_flags;
};

struct role_allowed_ip {
	__u32 addr;
	__u32 netmask;

	struct role_allowed_ip *prev;
	struct role_allowed_ip *next;
};

struct role_transition {
	char *rolename;

	struct role_transition *prev;
	struct role_transition *next;
};

struct acl_role_label {
	char *rolename;
	uid_t uidgid;
	__u16 roletype;

	__u16 auth_attempts;
	unsigned long expires;

	struct acl_subject_label *root_label;
	struct gr_hash_struct *hash;

	struct acl_role_label *prev;
	struct acl_role_label *next;

	struct role_transition *transitions;
	struct role_allowed_ip *allowed_ips;
	uid_t *domain_children;
	__u16 domain_child_num;

	struct acl_subject_label **subj_hash;
	__u32 subj_hash_size;
};

struct user_acl_role_db {
	struct acl_role_label **r_table;
	__u32 num_pointers;		/* Number of allocations to track */
	__u32 num_roles;		/* Number of roles */
	__u32 num_domain_children;	/* Number of domain children */
	__u32 num_subjects;		/* Number of subjects */
	__u32 num_objects;		/* Number of objects */
};

struct acl_object_label {
	char *filename;
	ino_t inode;
	__u32 device;
	__u32 mode;

	struct acl_subject_label *nested;
	struct acl_object_label *globbed;

	/* next two structures not used */

	struct acl_object_label *prev;
	struct acl_object_label *next;
};

struct acl_ip_label {
	char *iface;
	__u32 addr;
	__u32 netmask;
	__u16 low, high;
	__u8 mode;
	__u32 type;
	__u32 proto[8];

	/* next two structures not used */

	struct acl_ip_label *prev;
	struct acl_ip_label *next;
};

struct gr_arg {
	struct user_acl_role_db role_db;
	unsigned char pw[GR_PW_LEN];
	unsigned char salt[GR_SALT_LEN];
	unsigned char sum[GR_SHA_LEN];
	unsigned char sp_role[GR_SPROLE_LEN];
	struct sprole_pw *sprole_pws;
	__u32 segv_device;
	ino_t segv_inode;
	uid_t segv_uid;
	__u16 num_sprole_pws;
	__u16 mode;
};

struct gr_arg_wrapper {
	struct gr_arg *arg;
	__u32 version;
	__u32 size;
};

struct subject_map {
	struct acl_subject_label *user;
	struct acl_subject_label *kernel;
	struct subject_map *prev;
	struct subject_map *next;
};

struct acl_subj_map_db {
	struct subject_map **s_hash;
	__u32 s_size;
};

/* End Data Structures Section */

/* Hash functions generated by empirical testing by Brad Spengler
   Makes good use of the low bits of the inode.  Generally 0-1 times
   in loop for successful match.  0-3 for unsuccessful match.
   Shift/add algorithm with modulus of table size and an XOR*/

static __inline__ unsigned int
rhash(const uid_t uid, const __u16 type, const unsigned int sz)
{
	return (((uid << type) + (uid ^ type)) % sz);
}

static __inline__ unsigned int
shash(const struct acl_subject_label *userp, const unsigned int sz)
{
	return ((const unsigned long)userp % sz);
}

static __inline__ unsigned int
fhash(const ino_t ino, const __u32 dev, const unsigned int sz)
{
	return (((ino + dev) ^ ((ino << 13) + (ino << 23) + (dev << 9))) % sz);
}

static __inline__ unsigned int
nhash(const char *name, const __u16 len, const unsigned int sz)
{
	return full_name_hash(name, len) % sz;
}

#define FOR_EACH_ROLE_START(role,iter) \
	role = NULL; \
	iter = 0; \
	while (iter < acl_role_set.r_size) { \
		if (role == NULL) \
			role = acl_role_set.r_hash[iter]; \
		if (role == NULL) { \
			iter++; \
			continue; \
		}

#define FOR_EACH_ROLE_END(role,iter) \
		role = role->next; \
		if (role == NULL) \
			iter++; \
	}

#define FOR_EACH_SUBJECT_START(role,subj,iter) \
	subj = NULL; \
	iter = 0; \
	while (iter < role->subj_hash_size) { \
		if (subj == NULL) \
			subj = role->subj_hash[iter]; \
		if (subj == NULL) { \
			iter++; \
			continue; \
		}

#define FOR_EACH_SUBJECT_END(subj,iter) \
		subj = subj->next; \
		if (subj == NULL) \
			iter++; \
	}


#define FOR_EACH_NESTED_SUBJECT_START(role,subj) \
	subj = role->hash->first; \
	while (subj != NULL) {

#define FOR_EACH_NESTED_SUBJECT_END(subj) \
		subj = subj->next; \
	}

#endif
