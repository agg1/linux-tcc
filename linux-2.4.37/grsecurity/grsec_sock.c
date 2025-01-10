#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/file.h>
#include <linux/net.h>
#include <net/sock.h>
#include <linux/grsecurity.h>
#include <linux/grinternal.h>
#include <linux/gracl.h>

#ifdef CONFIG_GRKERNSEC
#define gr_conn_table_size 32749
struct conn_table_entry {
	struct conn_table_entry *next;
	struct task_struct *task;
};

struct conn_table_entry *gr_conn_table[gr_conn_table_size];
spinlock_t gr_conn_table_lock = SPIN_LOCK_UNLOCKED;

extern const char * gr_socktype_to_name(unsigned char type);
extern const char * gr_proto_to_name(unsigned char proto);

static __inline__ int 
conn_hash(__u32 saddr, __u32 daddr, __u16 sport, __u16 dport, unsigned int size)
{
	return ((daddr + saddr + (sport << 8) + (dport << 16)) % size);
}

static __inline__ int
conn_match(const struct task_struct *task, __u32 saddr, __u32 daddr, 
	   __u16 sport, __u16 dport)
{
	if (unlikely(task->gr_saddr == saddr && task->gr_daddr == daddr &&
		     task->gr_sport == sport && task->gr_dport == dport))
		return 1;
	else
		return 0;
}

static void gr_add_to_task_ip_table_nolock(struct task_struct *task, struct conn_table_entry *newent)
{
	struct conn_table_entry **match;
	unsigned int index;

	index = conn_hash(task->gr_saddr, task->gr_daddr,
			  task->gr_sport, task->gr_dport, 
			  gr_conn_table_size);


	newent->task = task;

	match = &gr_conn_table[index];
	newent->next = *match;
	*match = newent;

	return;
}

void gr_del_task_from_ip_table_nolock(struct task_struct *task)
{
	struct conn_table_entry *match, *last = NULL;
	unsigned int index;

	index = conn_hash(task->gr_saddr, task->gr_daddr, 
			  task->gr_sport, task->gr_dport, 
			  gr_conn_table_size);

	match = gr_conn_table[index];
	while (match && !conn_match(match->task, 
		task->gr_saddr, task->gr_daddr, task->gr_sport, 
		task->gr_dport)) {
		last = match;
		match = match->next;
	}

	if (match) {
		if (last)
			last->next = match->next;
		else
			gr_conn_table[index] = NULL;
		kfree(match);
	}

	return;
}

struct task_struct * gr_lookup_task_ip_table(__u32 saddr, __u32 daddr,
					     __u16 sport, __u16 dport)
{
	struct conn_table_entry *match;
	unsigned int index;

	index = conn_hash(saddr, daddr, sport, dport, gr_conn_table_size);

	match = gr_conn_table[index];
	while (match && !conn_match(match->task, saddr, daddr, sport, dport))
		match = match->next;

	if (match)
		return match->task;
	else
		return NULL;
}

#endif

void gr_update_task_in_ip_table(struct task_struct *task, const struct sock *sk)
{
#ifdef CONFIG_GRKERNSEC
	struct conn_table_entry *newent;

	newent = kmalloc(sizeof(struct conn_table_entry), GFP_ATOMIC);
	if (newent == NULL)
		return;
	/* no bh lock needed since we are called with bh disabled */
	spin_lock(&gr_conn_table_lock);
	gr_del_task_from_ip_table_nolock(task);
	task->gr_saddr = sk->rcv_saddr;
	task->gr_daddr = sk->daddr;
	task->gr_sport = sk->sport;
	task->gr_dport = sk->dport;
	gr_add_to_task_ip_table_nolock(task, newent);
	spin_unlock(&gr_conn_table_lock);
#endif
	return;
}

void gr_del_task_from_ip_table(struct task_struct *task)
{
#ifdef CONFIG_GRKERNSEC
	spin_lock_bh(&gr_conn_table_lock);
	gr_del_task_from_ip_table_nolock(task);
	spin_unlock_bh(&gr_conn_table_lock);
#endif
	return;
}

void
gr_attach_curr_ip(const struct sock *sk)
{
#ifdef CONFIG_GRKERNSEC
	struct task_struct *p;

	if (unlikely(sk->protocol != IPPROTO_TCP))
		return;

	spin_lock_bh(&gr_conn_table_lock);
	p = gr_lookup_task_ip_table(sk->daddr, sk->rcv_saddr,
				    sk->dport, sk->sport);
	if (unlikely(p != NULL)) {
		current->curr_ip = p->curr_ip;
		current->used_accept = 1;
		gr_del_task_from_ip_table_nolock(p);
		spin_unlock_bh(&gr_conn_table_lock);
		return;
	}
	spin_unlock_bh(&gr_conn_table_lock);

	current->curr_ip = sk->daddr;
	current->used_accept = 1;
#endif
	return;
}

int
gr_handle_sock_all(const int family, const int type, const int protocol)
{
#ifdef CONFIG_GRKERNSEC_SOCKET_ALL
	if (grsec_enable_socket_all && in_group_p(grsec_socket_all_gid) &&
	    (family != AF_UNIX) && (family != AF_LOCAL) && (type < SOCK_MAX)) {
		gr_log_int_str2(GR_DONT_AUDIT, GR_SOCK2_MSG, family, gr_socktype_to_name(type), gr_proto_to_name(protocol));
		return -EACCES;
	}
#endif
	return 0;
}

int
gr_handle_sock_server(const struct sockaddr *sck)
{
#ifdef CONFIG_GRKERNSEC_SOCKET_SERVER
	if (grsec_enable_socket_server &&
	    in_group_p(grsec_socket_server_gid) &&
	    sck && (sck->sa_family != AF_UNIX) &&
	    (sck->sa_family != AF_LOCAL)) {
		gr_log_noargs(GR_DONT_AUDIT, GR_BIND_MSG);
		return -EACCES;
	}
#endif
	return 0;
}

int
gr_handle_sock_server_other(const struct sock *sck)
{
#ifdef CONFIG_GRKERNSEC_SOCKET_SERVER
	if (grsec_enable_socket_server &&
	    in_group_p(grsec_socket_server_gid) &&
	    sck && (sck->family != AF_UNIX) &&
	    (sck->family != AF_LOCAL)) {
		gr_log_noargs(GR_DONT_AUDIT, GR_BIND_MSG);
		return -EACCES;
	}
#endif
	return 0;
}

int
gr_handle_sock_client(const struct sockaddr *sck)
{
#ifdef CONFIG_GRKERNSEC_SOCKET_CLIENT
	if (grsec_enable_socket_client && in_group_p(grsec_socket_client_gid) &&
	    sck && (sck->sa_family != AF_UNIX) &&
	    (sck->sa_family != AF_LOCAL)) {
		gr_log_noargs(GR_DONT_AUDIT, GR_CONNECT_MSG);
		return -EACCES;
	}
#endif
	return 0;
}

__u32
gr_cap_rtnetlink(void)
{
#ifdef CONFIG_GRKERNSEC
	if (!gr_acl_is_enabled())
		return current->cap_effective;
	else if (cap_raised(current->cap_effective, CAP_NET_ADMIN) &&
		 gr_task_is_capable(current, CAP_NET_ADMIN))
		return current->cap_effective;
	else {
		printk("Returning 0 for rtnetlink!\n");
		return 0;
	}
#else
	return current->cap_effective;
#endif
}
