#include <linux/utsname.h>
#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/string.h>
#include <linux/fcntl.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <linux/poll.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>

//#include <asm/processor.h>
#include <asm/uaccess.h>
//#include <asm/irq.h>
//#include <asm/io.h>

//
#define SCRANDOM_NOMODULE 1
#include "../misc/scrandom/scrandom.c"

/*
 * Forward procedure declarations
 */
//#ifdef CONFIG_SYSCTL
//static void sysctl_init_random(struct entropy_store *random_state);
//#endif


/*
 * Changes to the entropy data is put into a queue rather than being added to
 * the entropy counts directly.  This is presumably to avoid doing heavy
 * hashing calculations during an interrupt in add_timer_randomness().
 * Instead, the entropy is only added to the pool once per timer tick.
 */
void batch_entropy_store(u32 a, u32 b, int num)
{ }

#ifndef CONFIG_ARCH_S390
void add_keyboard_randomness(unsigned char scancode)
{
}

void add_mouse_randomness(__u32 mouse_data)
{
}

void add_interrupt_randomness(int irq)
{
}
#endif

void add_blkdev_randomness(int major)
{
}

/*
 * This function is the exported kernel interface.  It returns some
 * number of good random numbers, suitable for seeding TCP sequence
 * numbers, etc.
 */
void get_random_bytes(void *buf, int nbytes)
{
	scrandom_get_random_bytes((char *)buf, nbytes);
}

/*********************************************************************
 *
 * Functions to interface with Linux
 *
 *********************************************************************/


void __init rand_initialize(void)
{
//	scr = kmalloc(sizeof(struct scrandom), GFP_KERNEL);
//	if (!scrandom_state)
//		return -ENOMEM;
//
//	scr->scrambler = kmalloc(SCRANDOM_BUFSIZE, GFP_KERNEL);
//	if (!scr->scrambler) {
//		kfree(scrandom_state);
//		return -ENOMEM;
//	}
//
//	sema_init(&scr->sem, 1); /* Init semaphore as a mutex */
//
//	//
//	scrandom_init(scr);

	printk(KERN_INFO "random: OK.\n");
}

#ifndef CONFIG_ARCH_S390
void rand_initialize_irq(int irq)
{
}
#endif

void rand_initialize_blkdev(int major, int mode)
{
}

/******************************************************************
 *
 * Hash function definition
 *
 *******************************************************************/

/*
 * This chunk of code defines a function
 * void HASH_TRANSFORM(__u32 digest[HASH_BUFFER_SIZE + HASH_EXTRA_SIZE],
 * 		__u32 const data[16])
 * 
 * The function hashes the input data to produce a digest in the first
 * HASH_BUFFER_SIZE words of the digest[] array, and uses HASH_EXTRA_SIZE
 * more words for internal purposes.  (This buffer is exported so the
 * caller can wipe it once rather than this code doing it each call,
 * and tacking it onto the end of the digest[] array is the quick and
 * dirty way of doing it.)
 *
 * It so happens that MD5 and SHA share most of the initial vector
 * used to initialize the digest[] array before the first call:
 * 1) 0x67452301
 * 2) 0xefcdab89
 * 3) 0x98badcfe
 * 4) 0x10325476
 * 5) 0xc3d2e1f0 (SHA only)
 * 
 * For /dev/random purposes, the length of the data being hashed is
 * fixed in length, so appending a bit count in the usual way is not
 * cryptographically necessary.
 */

#define HASH_BUFFER_SIZE 4
#define HASH_EXTRA_SIZE 0
#define HASH_TRANSFORM MD5Transform
	
/*
 * MD5 transform algorithm, taken from code written by Colin Plumb,
 * and put into the public domain
 */

/* The four core functions - F1 is optimized somewhat */

/* #define F1(x, y, z) (x & y | ~x & z) */
#define F1(x, y, z) (z ^ (x & (y ^ z)))
#define F2(x, y, z) F1(z, x, y)
#define F3(x, y, z) (x ^ y ^ z)
#define F4(x, y, z) (y ^ (x | ~z))

/* This is the central step in the MD5 algorithm. */
#define MD5STEP(f, w, x, y, z, data, s) \
	( w += f(x, y, z) + data,  w = w<<s | w>>(32-s),  w += x )

/*
 * The core of the MD5 algorithm, this alters an existing MD5 hash to
 * reflect the addition of 16 longwords of new data.  MD5Update blocks
 * the data and converts bytes into longwords for this routine.
 */
static void MD5Transform(__u32 buf[HASH_BUFFER_SIZE], __u32 const in[16])
{
	__u32 a, b, c, d;

	a = buf[0];
	b = buf[1];
	c = buf[2];
	d = buf[3];

	MD5STEP(F1, a, b, c, d, in[ 0]+0xd76aa478,  7);
	MD5STEP(F1, d, a, b, c, in[ 1]+0xe8c7b756, 12);
	MD5STEP(F1, c, d, a, b, in[ 2]+0x242070db, 17);
	MD5STEP(F1, b, c, d, a, in[ 3]+0xc1bdceee, 22);
	MD5STEP(F1, a, b, c, d, in[ 4]+0xf57c0faf,  7);
	MD5STEP(F1, d, a, b, c, in[ 5]+0x4787c62a, 12);
	MD5STEP(F1, c, d, a, b, in[ 6]+0xa8304613, 17);
	MD5STEP(F1, b, c, d, a, in[ 7]+0xfd469501, 22);
	MD5STEP(F1, a, b, c, d, in[ 8]+0x698098d8,  7);
	MD5STEP(F1, d, a, b, c, in[ 9]+0x8b44f7af, 12);
	MD5STEP(F1, c, d, a, b, in[10]+0xffff5bb1, 17);
	MD5STEP(F1, b, c, d, a, in[11]+0x895cd7be, 22);
	MD5STEP(F1, a, b, c, d, in[12]+0x6b901122,  7);
	MD5STEP(F1, d, a, b, c, in[13]+0xfd987193, 12);
	MD5STEP(F1, c, d, a, b, in[14]+0xa679438e, 17);
	MD5STEP(F1, b, c, d, a, in[15]+0x49b40821, 22);

	MD5STEP(F2, a, b, c, d, in[ 1]+0xf61e2562,  5);
	MD5STEP(F2, d, a, b, c, in[ 6]+0xc040b340,  9);
	MD5STEP(F2, c, d, a, b, in[11]+0x265e5a51, 14);
	MD5STEP(F2, b, c, d, a, in[ 0]+0xe9b6c7aa, 20);
	MD5STEP(F2, a, b, c, d, in[ 5]+0xd62f105d,  5);
	MD5STEP(F2, d, a, b, c, in[10]+0x02441453,  9);
	MD5STEP(F2, c, d, a, b, in[15]+0xd8a1e681, 14);
	MD5STEP(F2, b, c, d, a, in[ 4]+0xe7d3fbc8, 20);
	MD5STEP(F2, a, b, c, d, in[ 9]+0x21e1cde6,  5);
	MD5STEP(F2, d, a, b, c, in[14]+0xc33707d6,  9);
	MD5STEP(F2, c, d, a, b, in[ 3]+0xf4d50d87, 14);
	MD5STEP(F2, b, c, d, a, in[ 8]+0x455a14ed, 20);
	MD5STEP(F2, a, b, c, d, in[13]+0xa9e3e905,  5);
	MD5STEP(F2, d, a, b, c, in[ 2]+0xfcefa3f8,  9);
	MD5STEP(F2, c, d, a, b, in[ 7]+0x676f02d9, 14);
	MD5STEP(F2, b, c, d, a, in[12]+0x8d2a4c8a, 20);

	MD5STEP(F3, a, b, c, d, in[ 5]+0xfffa3942,  4);
	MD5STEP(F3, d, a, b, c, in[ 8]+0x8771f681, 11);
	MD5STEP(F3, c, d, a, b, in[11]+0x6d9d6122, 16);
	MD5STEP(F3, b, c, d, a, in[14]+0xfde5380c, 23);
	MD5STEP(F3, a, b, c, d, in[ 1]+0xa4beea44,  4);
	MD5STEP(F3, d, a, b, c, in[ 4]+0x4bdecfa9, 11);
	MD5STEP(F3, c, d, a, b, in[ 7]+0xf6bb4b60, 16);
	MD5STEP(F3, b, c, d, a, in[10]+0xbebfbc70, 23);
	MD5STEP(F3, a, b, c, d, in[13]+0x289b7ec6,  4);
	MD5STEP(F3, d, a, b, c, in[ 0]+0xeaa127fa, 11);
	MD5STEP(F3, c, d, a, b, in[ 3]+0xd4ef3085, 16);
	MD5STEP(F3, b, c, d, a, in[ 6]+0x04881d05, 23);
	MD5STEP(F3, a, b, c, d, in[ 9]+0xd9d4d039,  4);
	MD5STEP(F3, d, a, b, c, in[12]+0xe6db99e5, 11);
	MD5STEP(F3, c, d, a, b, in[15]+0x1fa27cf8, 16);
	MD5STEP(F3, b, c, d, a, in[ 2]+0xc4ac5665, 23);

	MD5STEP(F4, a, b, c, d, in[ 0]+0xf4292244,  6);
	MD5STEP(F4, d, a, b, c, in[ 7]+0x432aff97, 10);
	MD5STEP(F4, c, d, a, b, in[14]+0xab9423a7, 15);
	MD5STEP(F4, b, c, d, a, in[ 5]+0xfc93a039, 21);
	MD5STEP(F4, a, b, c, d, in[12]+0x655b59c3,  6);
	MD5STEP(F4, d, a, b, c, in[ 3]+0x8f0ccc92, 10);
	MD5STEP(F4, c, d, a, b, in[10]+0xffeff47d, 15);
	MD5STEP(F4, b, c, d, a, in[ 1]+0x85845dd1, 21);
	MD5STEP(F4, a, b, c, d, in[ 8]+0x6fa87e4f,  6);
	MD5STEP(F4, d, a, b, c, in[15]+0xfe2ce6e0, 10);
	MD5STEP(F4, c, d, a, b, in[ 6]+0xa3014314, 15);
	MD5STEP(F4, b, c, d, a, in[13]+0x4e0811a1, 21);
	MD5STEP(F4, a, b, c, d, in[ 4]+0xf7537e82,  6);
	MD5STEP(F4, d, a, b, c, in[11]+0xbd3af235, 10);
	MD5STEP(F4, c, d, a, b, in[ 2]+0x2ad7d2bb, 15);
	MD5STEP(F4, b, c, d, a, in[ 9]+0xeb86d391, 21);

	buf[0] += a;
	buf[1] += b;
	buf[2] += c;
	buf[3] += d;
}

#undef F1
#undef F2
#undef F3
#undef F4
#undef MD5STEP

//static ssize_t
//random_read(struct file * file, char * buf, size_t nbytes, loff_t *ppos)
//{
//}
//
//static ssize_t
//urandom_read(struct file * file, char * buf,
//		      size_t nbytes, loff_t *ppos)
//{
//}

static unsigned int
random_poll(struct file *file, poll_table * wait)
{
	return 0;
}

static ssize_t
random_write(struct file * file, const char * buffer,
	     size_t count, loff_t *ppos)
{
	return count;
}

static int
random_ioctl(struct inode * inode, struct file * file,
	     unsigned int cmd, unsigned long arg)
{
	return 0;
}

const struct file_operations random_fops = {
	read:		scrandom_read,
	open:		scrandom_open,
	write:		random_write,
	poll:		random_poll,
	ioctl:		random_ioctl,
};

const struct file_operations urandom_fops = {
	read:		scrandom_read,
	open:		scrandom_open,
	write:		random_write,
	ioctl:		random_ioctl,
};

/***************************************************************
 * Random UUID interface
 * 
 * Used here for a Boot ID, but can be useful for other kernel 
 * drivers.
 ***************************************************************/

/*
 * Generate random UUID
 */
void generate_random_uuid(unsigned char uuid_out[16])
{
	get_random_bytes(uuid_out, 16);
	/* Set UUID version to 4 --- truely random generation */
	uuid_out[6] = (uuid_out[6] & 0x0F) | 0x40;
	/* Set the UUID variant to DCE */
	uuid_out[8] = (uuid_out[8] & 0x3F) | 0x80;
}

/********************************************************************
 *
 * Sysctl interface
 *
 ********************************************************************/

#ifdef CONFIG_SYSCTL

#include <linux/sysctl.h>

static int sysctl_poolsize = 8192;
static int entropy_avail = 8192;
static int min_read_thresh, max_read_thresh;
static int min_write_thresh, max_write_thresh;
static int random_read_wakeup_thresh = 8;
static int random_write_wakeup_thresh = 128;
static char sysctl_bootid[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};

/*
 * This function handles a request from the user to change the pool size 
 * of the primary entropy store.
 */
static int change_poolsize(int poolsize)
{
	return 0;
}

static int proc_do_poolsize(ctl_table *table, int write, struct file *filp,
			    void *buffer, size_t *lenp)
{
	return 0;
}

static int poolsize_strategy(ctl_table *table, int *name, int nlen,
			     void *oldval, size_t *oldlenp,
			     void *newval, size_t newlen, void **context)
{
	return 0;
}

static int proc_do_bootid(ctl_table *table, int write, struct file *filp,
			void *buffer, size_t *lenp)
{
	ctl_table	fake_table;
	unsigned char	buf[64];

	char uuid[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
	sprintf(buf, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
		"%02x%02x%02x%02x%02x%02x",
		uuid[0],  uuid[1],  uuid[2],  uuid[3],
		uuid[4],  uuid[5],  uuid[6],  uuid[7],
		uuid[8],  uuid[9],  uuid[10], uuid[11],
		uuid[12], uuid[13], uuid[14], uuid[15]);
	fake_table.data = buf;
	fake_table.maxlen = sizeof(buf);

	return proc_dostring(&fake_table, write, filp, buffer, lenp);
}

/*
 * These functions is used to return both the bootid UUID, and random
 * UUID.  The difference is in whether table->data is NULL; if it is,
 * then a new UUID is generated and returned to the user.
 * 
 * If the user accesses this via the proc interface, it will be returned
 * as an ASCII string in the standard UUID format.  If accesses via the 
 * sysctl system call, it is returned as 16 bytes of binary data.
 */
static int proc_do_uuid(ctl_table *table, int write, struct file *filp,
			void *buffer, size_t *lenp)
{
	ctl_table	fake_table;
	unsigned char	buf[64], tmp_uuid[16], *uuid;

	uuid = table->data;
	if (!uuid) {
		uuid = tmp_uuid;
		uuid[8] = 0;
	}
	if (uuid[8] == 0)
		generate_random_uuid(uuid);

	sprintf(buf, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
		"%02x%02x%02x%02x%02x%02x",
		uuid[0],  uuid[1],  uuid[2],  uuid[3],
		uuid[4],  uuid[5],  uuid[6],  uuid[7],
		uuid[8],  uuid[9],  uuid[10], uuid[11],
		uuid[12], uuid[13], uuid[14], uuid[15]);
	fake_table.data = buf;
	fake_table.maxlen = sizeof(buf);

	return proc_dostring(&fake_table, write, filp, buffer, lenp);
}

static int uuid_strategy(ctl_table *table, int *name, int nlen,
			 void *oldval, size_t *oldlenp,
			 void *newval, size_t newlen, void **context)
{
	unsigned char	tmp_uuid[16], *uuid;
	unsigned int	len;

	if (!oldval || !oldlenp)
		return 1;

	uuid = table->data;
	if (!uuid) {
		uuid = tmp_uuid;
		uuid[8] = 0;
	}
	if (uuid[8] == 0)
		generate_random_uuid(uuid);

	if (get_user(len, oldlenp))
		return -EFAULT;
	if (len) {
		if (len > 16)
			len = 16;
		if (copy_to_user(oldval, uuid, len) ||
		    put_user(len, oldlenp))
			return -EFAULT;
	}
	return 1;
}

ctl_table random_table[] = {
	{RANDOM_POOLSIZE, "poolsize",
	 &sysctl_poolsize, sizeof(int), 0644, NULL,
	 &proc_do_poolsize, &poolsize_strategy},
	{RANDOM_ENTROPY_COUNT, "entropy_avail",
	 NULL, sizeof(int), 0444, NULL,
	 &proc_dointvec},
	{RANDOM_READ_THRESH, "read_wakeup_threshold",
	 &random_read_wakeup_thresh, sizeof(int), 0644, NULL,
	 &proc_dointvec_minmax, &sysctl_intvec, 0,
	 &min_read_thresh, &max_read_thresh},
	{RANDOM_WRITE_THRESH, "write_wakeup_threshold",
	 &random_write_wakeup_thresh, sizeof(int), 0644, NULL,
	 &proc_dointvec_minmax, &sysctl_intvec, 0,
	 &min_write_thresh, &max_write_thresh},
	{RANDOM_BOOT_ID, "boot_id",
	 &sysctl_bootid, 16, 0444, NULL,
	 &proc_do_bootid, &uuid_strategy},
	{RANDOM_UUID, "uuid",
	 NULL, 16, 0444, NULL,
	 &proc_do_uuid, &uuid_strategy},
	{0}
};

static void sysctl_init_random(struct entropy_store *random_state)
{
//	min_read_thresh = 8;
//	min_write_thresh = 0;
//	max_read_thresh = max_write_thresh = 8192; //random_state->poolinfo.POOLBITS;
	random_table[1].data = &entropy_avail; //&random_state->entropy_count;
}
#endif 	/* CONFIG_SYSCTL */

/********************************************************************
 *
 * Random funtions for networking
 *
 ********************************************************************/

/*
 * TCP initial sequence number picking.  This uses the random number
 * generator to pick an initial secret value.  This value is hashed
 * along with the TCP endpoint information to provide a unique
 * starting point for each pair of TCP endpoints.  This defeats
 * attacks which rely on guessing the initial TCP sequence number.
 * This algorithm was suggested by Steve Bellovin.
 *
 * Using a very strong hash was taking an appreciable amount of the total
 * TCP connection establishment time, so this is a weaker hash,
 * compensated for by changing the secret periodically.
 */

/* F, G and H are basic MD4 functions: selection, majority, parity */
#define F(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define G(x, y, z) (((x) & (y)) + (((x) ^ (y)) & (z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))

/*
 * The generic round function.  The application is so specific that
 * we don't bother protecting all the arguments with parens, as is generally
 * good macro practice, in favor of extra legibility.
 * Rotation is separate from addition to prevent recomputation
 */
#define ROUND(f, a, b, c, d, x, s)	\
	(a += f(b, c, d) + x, a = (a << s) | (a >> (32-s)))
#define K1 0
#define K2 013240474631UL
#define K3 015666365641UL

/*
 * Basic cut-down MD4 transform.  Returns only 32 bits of result.
 */
static __u32 halfMD4Transform (__u32 const buf[4], __u32 const in[8])
{
	__u32	a = buf[0], b = buf[1], c = buf[2], d = buf[3];

	/* Round 1 */
	ROUND(F, a, b, c, d, in[0] + K1,  3);
	ROUND(F, d, a, b, c, in[1] + K1,  7);
	ROUND(F, c, d, a, b, in[2] + K1, 11);
	ROUND(F, b, c, d, a, in[3] + K1, 19);
	ROUND(F, a, b, c, d, in[4] + K1,  3);
	ROUND(F, d, a, b, c, in[5] + K1,  7);
	ROUND(F, c, d, a, b, in[6] + K1, 11);
	ROUND(F, b, c, d, a, in[7] + K1, 19);

	/* Round 2 */
	ROUND(G, a, b, c, d, in[1] + K2,  3);
	ROUND(G, d, a, b, c, in[3] + K2,  5);
	ROUND(G, c, d, a, b, in[5] + K2,  9);
	ROUND(G, b, c, d, a, in[7] + K2, 13);
	ROUND(G, a, b, c, d, in[0] + K2,  3);
	ROUND(G, d, a, b, c, in[2] + K2,  5);
	ROUND(G, c, d, a, b, in[4] + K2,  9);
	ROUND(G, b, c, d, a, in[6] + K2, 13);

	/* Round 3 */
	ROUND(H, a, b, c, d, in[3] + K3,  3);
	ROUND(H, d, a, b, c, in[7] + K3,  9);
	ROUND(H, c, d, a, b, in[2] + K3, 11);
	ROUND(H, b, c, d, a, in[6] + K3, 15);
	ROUND(H, a, b, c, d, in[1] + K3,  3);
	ROUND(H, d, a, b, c, in[5] + K3,  9);
	ROUND(H, c, d, a, b, in[0] + K3, 11);
	ROUND(H, b, c, d, a, in[4] + K3, 15);

	return buf[1] + b;	/* "most hashed" word */
	/* Alternative: return sum of all words? */
}

#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)

static __u32 twothirdsMD4Transform (__u32 const buf[4], __u32 const in[12])
{
	__u32	a = buf[0], b = buf[1], c = buf[2], d = buf[3];

	/* Round 1 */
	ROUND(F, a, b, c, d, in[ 0] + K1,  3);
	ROUND(F, d, a, b, c, in[ 1] + K1,  7);
	ROUND(F, c, d, a, b, in[ 2] + K1, 11);
	ROUND(F, b, c, d, a, in[ 3] + K1, 19);
	ROUND(F, a, b, c, d, in[ 4] + K1,  3);
	ROUND(F, d, a, b, c, in[ 5] + K1,  7);
	ROUND(F, c, d, a, b, in[ 6] + K1, 11);
	ROUND(F, b, c, d, a, in[ 7] + K1, 19);
	ROUND(F, a, b, c, d, in[ 8] + K1,  3);
	ROUND(F, d, a, b, c, in[ 9] + K1,  7);
	ROUND(F, c, d, a, b, in[10] + K1, 11);
	ROUND(F, b, c, d, a, in[11] + K1, 19);

	/* Round 2 */
	ROUND(G, a, b, c, d, in[ 1] + K2,  3);
	ROUND(G, d, a, b, c, in[ 3] + K2,  5);
	ROUND(G, c, d, a, b, in[ 5] + K2,  9);
	ROUND(G, b, c, d, a, in[ 7] + K2, 13);
	ROUND(G, a, b, c, d, in[ 9] + K2,  3);
	ROUND(G, d, a, b, c, in[11] + K2,  5);
	ROUND(G, c, d, a, b, in[ 0] + K2,  9);
	ROUND(G, b, c, d, a, in[ 2] + K2, 13);
	ROUND(G, a, b, c, d, in[ 4] + K2,  3);
	ROUND(G, d, a, b, c, in[ 6] + K2,  5);
	ROUND(G, c, d, a, b, in[ 8] + K2,  9);
	ROUND(G, b, c, d, a, in[10] + K2, 13);

	/* Round 3 */
	ROUND(H, a, b, c, d, in[ 3] + K3,  3);
	ROUND(H, d, a, b, c, in[ 7] + K3,  9);
	ROUND(H, c, d, a, b, in[11] + K3, 11);
	ROUND(H, b, c, d, a, in[ 2] + K3, 15);
	ROUND(H, a, b, c, d, in[ 6] + K3,  3);
	ROUND(H, d, a, b, c, in[10] + K3,  9);
	ROUND(H, c, d, a, b, in[ 1] + K3, 11);
	ROUND(H, b, c, d, a, in[ 5] + K3, 15);
	ROUND(H, a, b, c, d, in[ 9] + K3,  3);
	ROUND(H, d, a, b, c, in[ 0] + K3,  9);
	ROUND(H, c, d, a, b, in[ 4] + K3, 11);
	ROUND(H, b, c, d, a, in[ 8] + K3, 15);

	return buf[1] + b;	/* "most hashed" word */
	/* Alternative: return sum of all words? */
}
#endif

#undef ROUND
#undef F
#undef G
#undef H
#undef K1
#undef K2
#undef K3

/* This should not be decreased so low that ISNs wrap too fast. */
#define REKEY_INTERVAL	300
/*
 * Bit layout of the tcp sequence numbers (before adding current time):
 * bit 24-31: increased after every key exchange
 * bit 0-23: hash(source,dest)
 *
 * The implementation is similar to the algorithm described
 * in the Appendix of RFC 1185, except that
 * - it uses a 1 MHz clock instead of a 250 kHz clock
 * - it performs a rekey every 5 minutes, which is equivalent
 * 	to a (source,dest) tulple dependent forward jump of the
 * 	clock by 0..2^(HASH_BITS+1)
 *
 * Thus the average ISN wraparound time is 68 minutes instead of
 * 4.55 hours.
 *
 * SMP cleanup and lock avoidance with poor man's RCU.
 * 			Manfred Spraul <manfred@colorfullife.com>
 * 		
 */
#define COUNT_BITS	8
#define COUNT_MASK	( (1<<COUNT_BITS)-1)
#define HASH_BITS	24
#define HASH_MASK	( (1<<HASH_BITS)-1 )

static struct keydata {
	time_t rekey_time;
	__u32	count;		// already shifted to the final position
	__u32	secret[12];
} ____cacheline_aligned ip_keydata[2];

static spinlock_t ip_lock = SPIN_LOCK_UNLOCKED;
static unsigned int ip_cnt;

static struct keydata *__check_and_rekey(time_t time)
{
	struct keydata *keyptr;
	spin_lock_bh(&ip_lock);
	keyptr = &ip_keydata[ip_cnt&1];
	if (!keyptr->rekey_time || (time - keyptr->rekey_time) > REKEY_INTERVAL) {
		keyptr = &ip_keydata[1^(ip_cnt&1)];
		keyptr->rekey_time = time;
		get_random_bytes(keyptr->secret, sizeof(keyptr->secret));
		keyptr->count = (ip_cnt&COUNT_MASK)<<HASH_BITS;
		mb();
		ip_cnt++;
	}
	spin_unlock_bh(&ip_lock);
	return keyptr;
}

static inline struct keydata *check_and_rekey(time_t time)
{
	struct keydata *keyptr = &ip_keydata[ip_cnt&1];

	rmb();
	if (!keyptr->rekey_time || (time - keyptr->rekey_time) > REKEY_INTERVAL) {
		keyptr = __check_and_rekey(time);
	}

	return keyptr;
}

#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
__u32 secure_tcpv6_sequence_number(__u32 *saddr, __u32 *daddr,
				   __u16 sport, __u16 dport)
{
	struct timeval 	tv;
	__u32		seq;
	__u32		hash[12];
	struct keydata *keyptr;

	/* The procedure is the same as for IPv4, but addresses are longer.
	 * Thus we must use twothirdsMD4Transform.
	 */

	do_gettimeofday(&tv);	/* We need the usecs below... */
	keyptr = check_and_rekey(tv.tv_sec);

	memcpy(hash, saddr, 16);
	hash[4]=(sport << 16) + dport;
	memcpy(&hash[5],keyptr->secret,sizeof(__u32)*7);

	seq = twothirdsMD4Transform(daddr, hash) & HASH_MASK;
	seq += keyptr->count;
	seq += tv.tv_usec + tv.tv_sec*1000000;

	return seq;
}

__u32 secure_ipv6_id(__u32 *daddr)
{
	struct keydata *keyptr;

	keyptr = check_and_rekey(CURRENT_TIME);

	return halfMD4Transform(daddr, keyptr->secret);
}

#endif


__u32 secure_tcp_sequence_number(__u32 saddr, __u32 daddr,
				 __u16 sport, __u16 dport)
{
	struct timeval 	tv;
	__u32		seq;
	__u32	hash[4];
	struct keydata *keyptr;

	/*
	 * Pick a random secret every REKEY_INTERVAL seconds.
	 */
	do_gettimeofday(&tv);	/* We need the usecs below... */
	keyptr = check_and_rekey(tv.tv_sec);

	/*
	 *  Pick a unique starting offset for each TCP connection endpoints
	 *  (saddr, daddr, sport, dport).
	 *  Note that the words are placed into the starting vector, which is 
	 *  then mixed with a partial MD4 over random data.
	 */
	hash[0]=saddr;
	hash[1]=daddr;
	hash[2]=(sport << 16) + dport;
	hash[3]=keyptr->secret[11];

	seq = halfMD4Transform(hash, keyptr->secret) & HASH_MASK;
	seq += keyptr->count;
	/*
	 *	As close as possible to RFC 793, which
	 *	suggests using a 250 kHz clock.
	 *	Further reading shows this assumes 2 Mb/s networks.
	 *	For 10 Mb/s Ethernet, a 1 MHz clock is appropriate.
	 *	That's funny, Linux has one built in!  Use it!
	 *	(Networks are faster now - should this be increased?)
	 */
	seq += tv.tv_usec + tv.tv_sec*1000000;
#if 0
	printk("init_seq(%lx, %lx, %d, %d) = %d\n",
	       saddr, daddr, sport, dport, seq);
#endif
	return seq;
}

/*  The code below is shamelessly stolen from secure_tcp_sequence_number().
 *  All blames to Andrey V. Savochkin <saw@msu.ru>.
 */
__u32 secure_ip_id(__u32 daddr)
{
	struct keydata *keyptr;
	__u32 hash[4];

	keyptr = check_and_rekey(CURRENT_TIME);

	/*
	 *  Pick a unique starting offset for each IP destination.
	 *  The dest ip address is placed in the starting vector,
	 *  which is then hashed with random data.
	 */
	hash[0] = daddr;
	hash[1] = keyptr->secret[9];
	hash[2] = keyptr->secret[10];
	hash[3] = keyptr->secret[11];

	return halfMD4Transform(hash, keyptr->secret);
}

#ifdef CONFIG_SYN_COOKIES
/*
 * Secure SYN cookie computation. This is the algorithm worked out by
 * Dan Bernstein and Eric Schenk.
 *
 * For linux I implement the 1 minute counter by looking at the jiffies clock.
 * The count is passed in as a parameter, so this code doesn't much care.
 */

#define COOKIEBITS 24	/* Upper bits store count */
#define COOKIEMASK (((__u32)1 << COOKIEBITS) - 1)

static int	syncookie_init;
static __u32	syncookie_secret[2][16-3+HASH_BUFFER_SIZE];

__u32 secure_tcp_syn_cookie(__u32 saddr, __u32 daddr, __u16 sport,
		__u16 dport, __u32 sseq, __u32 count, __u32 data)
{
	__u32 	tmp[16 + HASH_BUFFER_SIZE + HASH_EXTRA_SIZE];
	__u32	seq;

	/*
	 * Pick two random secrets the first time we need a cookie.
	 */
	if (syncookie_init == 0) {
		get_random_bytes(syncookie_secret, sizeof(syncookie_secret));
		syncookie_init = 1;
	}

	/*
	 * Compute the secure sequence number.
	 * The output should be:
   	 *   HASH(sec1,saddr,sport,daddr,dport,sec1) + sseq + (count * 2^24)
	 *      + (HASH(sec2,saddr,sport,daddr,dport,count,sec2) % 2^24).
	 * Where sseq is their sequence number and count increases every
	 * minute by 1.
	 * As an extra hack, we add a small "data" value that encodes the
	 * MSS into the second hash value.
	 */

	memcpy(tmp+3, syncookie_secret[0], sizeof(syncookie_secret[0]));
	tmp[0]=saddr;
	tmp[1]=daddr;
	tmp[2]=(sport << 16) + dport;
	HASH_TRANSFORM(tmp+16, tmp);
	seq = tmp[17] + sseq + (count << COOKIEBITS);

	memcpy(tmp+3, syncookie_secret[1], sizeof(syncookie_secret[1]));
	tmp[0]=saddr;
	tmp[1]=daddr;
	tmp[2]=(sport << 16) + dport;
	tmp[3] = count;	/* minute counter */
	HASH_TRANSFORM(tmp+16, tmp);

	/* Add in the second hash and the data */
	return seq + ((tmp[17] + data) & COOKIEMASK);
}

/*
 * This retrieves the small "data" value from the syncookie.
 * If the syncookie is bad, the data returned will be out of
 * range.  This must be checked by the caller.
 *
 * The count value used to generate the cookie must be within
 * "maxdiff" if the current (passed-in) "count".  The return value
 * is (__u32)-1 if this test fails.
 */
__u32 check_tcp_syn_cookie(__u32 cookie, __u32 saddr, __u32 daddr, __u16 sport,
		__u16 dport, __u32 sseq, __u32 count, __u32 maxdiff)
{
	__u32 	tmp[16 + HASH_BUFFER_SIZE + HASH_EXTRA_SIZE];
	__u32	diff;

	if (syncookie_init == 0)
		return (__u32)-1;	/* Well, duh! */

	/* Strip away the layers from the cookie */
	memcpy(tmp+3, syncookie_secret[0], sizeof(syncookie_secret[0]));
	tmp[0]=saddr;
	tmp[1]=daddr;
	tmp[2]=(sport << 16) + dport;
	HASH_TRANSFORM(tmp+16, tmp);
	cookie -= tmp[17] + sseq;
	/* Cookie is now reduced to (count * 2^24) ^ (hash % 2^24) */

	diff = (count - (cookie >> COOKIEBITS)) & ((__u32)-1 >> COOKIEBITS);
	if (diff >= maxdiff)
		return (__u32)-1;

	memcpy(tmp+3, syncookie_secret[1], sizeof(syncookie_secret[1]));
	tmp[0] = saddr;
	tmp[1] = daddr;
	tmp[2] = (sport << 16) + dport;
	tmp[3] = count - diff;	/* minute counter */
	HASH_TRANSFORM(tmp+16, tmp);

	return (cookie - tmp[17]) & COOKIEMASK;	/* Leaving the data behind */
}
#endif



#ifndef CONFIG_ARCH_S390
EXPORT_SYMBOL(add_keyboard_randomness);
EXPORT_SYMBOL(add_mouse_randomness);
EXPORT_SYMBOL(add_interrupt_randomness);
#endif
EXPORT_SYMBOL(add_blkdev_randomness);
EXPORT_SYMBOL(batch_entropy_store);
EXPORT_SYMBOL(generate_random_uuid);

