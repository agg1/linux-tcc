#include <linux/kernel.h>
#include <linux/config.h>
#include <linux/sched.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/skbuff.h>
#include <linux/sysctl.h>

#ifdef CONFIG_PAX_HAVE_ACL_FLAGS
void
pax_set_initial_flags(struct linux_binprm *bprm)
{
	return;
}
#endif

#ifdef CONFIG_SYSCTL
__u32
gr_handle_sysctl(const struct ctl_table * table, const void *oldval, const void *newval)
{
	return 1;
}
#endif

int
gr_acl_is_enabled(void)
{
	return 0;
}

int
gr_handle_rawio(const struct inode *inode)
{
	return 0;
}

void
gr_acl_handle_psacct(struct task_struct *task, const long code)
{
	return;
}

int
gr_handle_ptrace(struct task_struct *task, const long request)
{
	return 0;
}

int
gr_handle_proc_ptrace(struct task_struct *task)
{
	return 0;
}

void
gr_learn_resource(const struct task_struct *task,
		  const int res, const unsigned long wanted, const int gt)
{
	return;
}

int
gr_set_acls(const int type)
{
	return 0;
}

int
gr_check_hidden_task(const struct task_struct *tsk)
{
	return 0;
}

int
gr_check_protected_task(const struct task_struct *task)
{
	return 0;
}

void
gr_copy_label(struct task_struct *tsk)
{
	return;
}

void
gr_set_pax_flags(struct task_struct *task)
{
	return;
}

int
gr_set_proc_label(const struct dentry *dentry, const struct vfsmount *mnt)
{
	return 0;
}

void
gr_handle_delete(const ino_t ino, const __u32 dev)
{
	return;
}

void
gr_handle_create(const struct dentry *dentry, const struct vfsmount *mnt)
{
	return;
}

void
gr_handle_crash(struct task_struct *task, const int sig)
{
	return;
}

int
gr_check_crash_exec(const struct file *filp)
{
	return 0;
}

int
gr_check_crash_uid(const uid_t uid)
{
	return 0;
}

int
gr_handle_rename(struct inode *old_dir, struct inode *new_dir,
		 struct dentry *old_dentry,
		 struct dentry *new_dentry,
		 struct vfsmount *mnt, const __u8 replace)
{
	return 0;
}

int
gr_search_socket(const int family, const int type, const int protocol)
{
	return 1;
}

int
gr_search_connectbind(const int mode, const struct socket *sock,
		      const struct sockaddr_in *addr)
{
	return 0;
}

int
gr_task_is_capable(struct task_struct *task, const int cap)
{
	return 1;
}

void
gr_handle_alertkill(struct task_struct *task)
{
	return;
}

__u32
gr_acl_handle_execve(const struct dentry * dentry, const struct vfsmount * mnt)
{
	return 1;
}

__u32
gr_acl_handle_hidden_file(const struct dentry * dentry,
			  const struct vfsmount * mnt)
{
	return 1;
}

__u32
gr_acl_handle_open(const struct dentry * dentry, const struct vfsmount * mnt,
		   const int fmode)
{
	return 1;
}

__u32
gr_acl_handle_rmdir(const struct dentry * dentry, const struct vfsmount * mnt)
{
	return 1;
}

__u32
gr_acl_handle_unlink(const struct dentry * dentry, const struct vfsmount * mnt)
{
	return 1;
}

int
gr_acl_handle_mmap(const struct file *file, const unsigned long prot,
		   unsigned int *vm_flags)
{
	return 1;
}

__u32
gr_acl_handle_truncate(const struct dentry * dentry,
		       const struct vfsmount * mnt)
{
	return 1;
}

__u32
gr_acl_handle_utime(const struct dentry * dentry, const struct vfsmount * mnt)
{
	return 1;
}

__u32
gr_acl_handle_access(const struct dentry * dentry,
		     const struct vfsmount * mnt, const int fmode)
{
	return 1;
}

__u32
gr_acl_handle_fchmod(const struct dentry * dentry, const struct vfsmount * mnt,
		     mode_t mode)
{
	return 1;
}

__u32
gr_acl_handle_chmod(const struct dentry * dentry, const struct vfsmount * mnt,
		    mode_t mode)
{
	return 1;
}

__u32
gr_acl_handle_chown(const struct dentry * dentry, const struct vfsmount * mnt)
{
	return 1;
}

#ifndef CONFIG_GRKERNSEC
void
grsecurity_init(void)
{
	return;
}
#endif

__u32
gr_acl_handle_mknod(const struct dentry * new_dentry,
		    const struct dentry * parent_dentry,
		    const struct vfsmount * parent_mnt,
		    const int mode)
{
	return 1;
}

__u32
gr_acl_handle_mkdir(const struct dentry * new_dentry,
		    const struct dentry * parent_dentry,
		    const struct vfsmount * parent_mnt)
{
	return 1;
}

__u32
gr_acl_handle_symlink(const struct dentry * new_dentry,
		      const struct dentry * parent_dentry,
		      const struct vfsmount * parent_mnt, const char *from)
{
	return 1;
}

__u32
gr_acl_handle_link(const struct dentry * new_dentry,
		   const struct dentry * parent_dentry,
		   const struct vfsmount * parent_mnt,
		   const struct dentry * old_dentry,
		   const struct vfsmount * old_mnt, const char *to)
{
	return 1;
}

int
gr_acl_handle_rename(const struct dentry *new_dentry,
		     const struct dentry *parent_dentry,
		     const struct vfsmount *parent_mnt,
		     const struct dentry *old_dentry,
		     const struct inode *old_parent_inode,
		     const struct vfsmount *old_mnt, const char *newname)
{
	return 1;
}

int
gr_acl_handle_filldir(const struct file *file, const char *name,
		      const int namelen, const ino_t ino)
{
	return 1;
}

int
gr_handle_shmat(const pid_t shm_cprid, const pid_t shm_lapid,
		const time_t shm_createtime, const uid_t cuid, const int shmid)
{
	return 1;
}

int
gr_search_bind(const struct socket *sock, const struct sockaddr_in *addr)
{
	return 0;
}

int
gr_search_accept(const struct socket *sock)
{
	return 0;
}

int
gr_search_listen(const struct socket *sock)
{
	return 0;
}

int
gr_search_connect(const struct socket *sock, const struct sockaddr_in *addr)
{
	return 0;
}

__u32
gr_acl_handle_unix(const struct dentry * dentry, const struct vfsmount * mnt)
{
	return 1;
}

__u32
gr_acl_handle_creat(const struct dentry * dentry,
		    const struct dentry * p_dentry,
		    const struct vfsmount * p_mnt, const int fmode,
		    const int imode)
{
	return 1;
}

void
gr_acl_handle_exit(void)
{
	return;
}

int
gr_acl_handle_mprotect(const struct file *file, const unsigned long prot)
{
	return 1;
}

void
gr_set_role_label(const uid_t uid, const gid_t gid)
{
	return;
}

int
gr_acl_handle_procpidmem(const struct task_struct *task)
{
	return 0;
}

int
gr_search_udp_recvmsg(const struct sock *sk, const struct sk_buff *skb)
{
	return 1;
}

int
gr_search_udp_sendmsg(const struct sock *sk, const struct sockaddr_in *addr)
{
	return 1;
}

void
gr_set_kernel_label(struct task_struct *task)
{
	return;
}

int
gr_check_user_change(int real, int effective, int fs)
{
	return 0;
}

int
gr_check_group_change(int real, int effective, int fs)
{
	return 0;
}
