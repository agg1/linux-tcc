/*
 * IEEE 1394 for Linux
 *
 * Raw interface to the bus
 *
 * Copyright (C) 1999, 2000 Andreas E. Bombe
 *               2001, 2002 Manfred Weihs <weihs@ict.tuwien.ac.at>
 *                     2002 Christian Toegel <christian.toegel@gmx.at>
 *
 * This code is licensed under the GPL.  See the file COPYING in the root
 * directory of the kernel sources for details.
 *
 *
 * Contributions:
 *
 * Manfred Weihs <weihs@ict.tuwien.ac.at>
 *        configuration ROM manipulation
 *        address range mapping
 *        adaptation for new (transparent) loopback mechanism
 *        sending of arbitrary async packets
 * Christian Toegel <christian.toegel@gmx.at>
 *        address range mapping
 *        lock64 request
 *        transmit physical packet
 *        busreset notification control (switch on/off)
 *        busreset with selection of type (short/long)
 *        request_reply
 */

#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/version.h>
#include <linux/smp_lock.h>
#include <linux/vmalloc.h>
#include <asm/uaccess.h>
#include <asm/atomic.h>
#include <linux/devfs_fs_kernel.h>

#include "ieee1394.h"
#include "ieee1394_types.h"
#include "ieee1394_core.h"
#include "nodemgr.h"
#include "hosts.h"
#include "highlevel.h"
#include "iso.h"
#include "ieee1394_transactions.h"
#include "raw1394.h"
#include "raw1394-private.h"

#if BITS_PER_LONG == 64
#define int2ptr(x) ((void *)x)
#define ptr2int(x) ((u64)x)
#else
#define int2ptr(x) ((void *)(u32)x)
#define ptr2int(x) ((u64)(u32)x)
#endif

#ifdef CONFIG_IEEE1394_VERBOSEDEBUG
#define RAW1394_DEBUG
#endif

#ifdef RAW1394_DEBUG
#define DBGMSG(fmt, args...) \
printk(KERN_INFO "raw1394:" fmt "\n" , ## args)
#else
#define DBGMSG(fmt, args...)
#endif

static devfs_handle_t devfs_handle;

static LIST_HEAD(host_info_list);
static int host_count;
static spinlock_t host_info_lock = SPIN_LOCK_UNLOCKED;
static atomic_t internal_generation = ATOMIC_INIT(0);

static atomic_t iso_buffer_size;
static const int iso_buffer_max = 4 * 1024 * 1024; /* 4 MB */

static struct hpsb_highlevel raw1394_highlevel;

static int arm_read (struct hpsb_host *host, int nodeid, quadlet_t *buffer,
		     u64 addr, size_t length, u16 flags);
static int arm_write (struct hpsb_host *host, int nodeid, int destid,
		      quadlet_t *data, u64 addr, size_t length, u16 flags);
static int arm_lock (struct hpsb_host *host, int nodeid, quadlet_t *store,
             u64 addr, quadlet_t data, quadlet_t arg, int ext_tcode, u16 flags);
static int arm_lock64 (struct hpsb_host *host, int nodeid, octlet_t *store,
               u64 addr, octlet_t data, octlet_t arg, int ext_tcode, u16 flags);
static struct hpsb_address_ops arm_ops = {
	.read	= arm_read,
	.write	= arm_write,
	.lock	= arm_lock,
	.lock64	= arm_lock64,
};

static void queue_complete_cb(struct pending_request *req);

static struct pending_request *__alloc_pending_request(int flags)
{
        struct pending_request *req;

        req = (struct pending_request *)kmalloc(sizeof(struct pending_request),
                                                flags);
        if (req != NULL) {
                memset(req, 0, sizeof(struct pending_request));
                INIT_LIST_HEAD(&req->list);
        }

        return req;
}

static inline struct pending_request *alloc_pending_request(void)
{
        return __alloc_pending_request(SLAB_KERNEL);
}

static void free_pending_request(struct pending_request *req)
{
        if (req->ibs) {
                if (atomic_dec_and_test(&req->ibs->refcount)) {
                        atomic_sub(req->ibs->data_size, &iso_buffer_size);
                        kfree(req->ibs);
                }
        } else if (req->free_data) {
                kfree(req->data);
        }
        free_hpsb_packet(req->packet);
        kfree(req);
}

/* fi->reqlists_lock must be taken */
static void __queue_complete_req(struct pending_request *req)
{
	struct file_info *fi = req->file_info;
	list_del(&req->list);
        list_add_tail(&req->list, &fi->req_complete);

	up(&fi->complete_sem);
        wake_up_interruptible(&fi->poll_wait_complete);
}

static void queue_complete_req(struct pending_request *req)
{
        unsigned long flags;
        struct file_info *fi = req->file_info;

        spin_lock_irqsave(&fi->reqlists_lock, flags);
	__queue_complete_req(req);
        spin_unlock_irqrestore(&fi->reqlists_lock, flags);
}

static void queue_complete_cb(struct pending_request *req)
{
        struct hpsb_packet *packet = req->packet;
        int rcode = (packet->header[1] >> 12) & 0xf;

        switch (packet->ack_code) {
        case ACKX_NONE:
        case ACKX_SEND_ERROR:
                req->req.error = RAW1394_ERROR_SEND_ERROR;
                break;
        case ACKX_ABORTED:
                req->req.error = RAW1394_ERROR_ABORTED;
                break;
        case ACKX_TIMEOUT:
                req->req.error = RAW1394_ERROR_TIMEOUT;
                break;
        default:
                req->req.error = (packet->ack_code << 16) | rcode;
                break;
        }

        if (!((packet->ack_code == ACK_PENDING) && (rcode == RCODE_COMPLETE))) {
                req->req.length = 0;
        }

        if ((req->req.type == RAW1394_REQ_ASYNC_READ) ||
	    (req->req.type == RAW1394_REQ_ASYNC_WRITE) ||
	    (req->req.type == RAW1394_REQ_ASYNC_STREAM) ||
	    (req->req.type == RAW1394_REQ_LOCK) ||
	    (req->req.type == RAW1394_REQ_LOCK64))
                hpsb_free_tlabel(packet);

        queue_complete_req(req);
}


static void add_host(struct hpsb_host *host)
{
        struct host_info *hi;
        unsigned long flags;

        hi = (struct host_info *)kmalloc(sizeof(struct host_info), GFP_KERNEL);

        if (hi != NULL) {
                INIT_LIST_HEAD(&hi->list);
                hi->host = host;
                INIT_LIST_HEAD(&hi->file_info_list);

                spin_lock_irqsave(&host_info_lock, flags);
                list_add_tail(&hi->list, &host_info_list);
                host_count++;
                spin_unlock_irqrestore(&host_info_lock, flags);
        }

        atomic_inc(&internal_generation);
}


static struct host_info *find_host_info(struct hpsb_host *host)
{
        struct list_head *lh;
        struct host_info *hi;

        list_for_each(lh, &host_info_list) {
                hi = list_entry(lh, struct host_info, list);
                if (hi->host == host) {
                        return hi;
                }
        }

        return NULL;
}

static void remove_host(struct hpsb_host *host)
{
        struct host_info *hi;
        unsigned long flags;

        spin_lock_irqsave(&host_info_lock, flags);
        hi = find_host_info(host);

        if (hi != NULL) {
                list_del(&hi->list);
                host_count--;
                /* 
                   FIXME: address ranges should be removed 
                   and fileinfo states should be initialized
                   (including setting generation to 
                   internal-generation ...)
                */
        }
        spin_unlock_irqrestore(&host_info_lock, flags);

        if (hi == NULL) {
                printk(KERN_ERR "raw1394: attempt to remove unknown host "
                       "0x%p\n", host);
                return;
        }

        kfree(hi);

        atomic_inc(&internal_generation);
}

static void host_reset(struct hpsb_host *host)
{
        unsigned long flags;
        struct list_head *lh;
        struct host_info *hi;
        struct file_info *fi;
        struct pending_request *req;

        spin_lock_irqsave(&host_info_lock, flags);
        hi = find_host_info(host);

        if (hi != NULL) {
                list_for_each(lh, &hi->file_info_list) {
                        fi = list_entry(lh, struct file_info, list);
                        if (fi->notification == RAW1394_NOTIFY_ON) {
                                req = __alloc_pending_request(SLAB_ATOMIC);

                                if (req != NULL) {
                                        req->file_info = fi;
                                        req->req.type = RAW1394_REQ_BUS_RESET;
                                        req->req.generation = get_hpsb_generation(host);
                                        req->req.misc = (host->node_id << 16)
                                                | host->node_count;
                                        if (fi->protocol_version > 3) {
                                                req->req.misc |= (NODEID_TO_NODE(host->irm_id)
                                                                  << 8);
                                        }

                                        queue_complete_req(req);
                                }
                        }
                }
        }
        spin_unlock_irqrestore(&host_info_lock, flags);
}

static void iso_receive(struct hpsb_host *host, int channel, quadlet_t *data,
                        size_t length)
{
        unsigned long flags;
        struct list_head *lh;
        struct host_info *hi;
        struct file_info *fi;
        struct pending_request *req;
        struct iso_block_store *ibs = NULL;
        LIST_HEAD(reqs);

        if ((atomic_read(&iso_buffer_size) + length) > iso_buffer_max) {
                HPSB_INFO("dropped iso packet");
                return;
        }

        spin_lock_irqsave(&host_info_lock, flags);
        hi = find_host_info(host);

        if (hi != NULL) {
		list_for_each(lh, &hi->file_info_list) {
                        fi = list_entry(lh, struct file_info, list);

                        if (!(fi->listen_channels & (1ULL << channel))) {
                                continue;
                        }

                        req = __alloc_pending_request(SLAB_ATOMIC);
                        if (!req) break;

                        if (!ibs) {
                                ibs = kmalloc(sizeof(struct iso_block_store)
                                              + length, SLAB_ATOMIC);
                                if (!ibs) {
                                        kfree(req);
                                        break;
                                }

                                atomic_add(length, &iso_buffer_size);
                                atomic_set(&ibs->refcount, 0);
                                ibs->data_size = length;
                                memcpy(ibs->data, data, length);
                        }

                        atomic_inc(&ibs->refcount);

                        req->file_info = fi;
                        req->ibs = ibs;
                        req->data = ibs->data;
                        req->req.type = RAW1394_REQ_ISO_RECEIVE;
                        req->req.generation = get_hpsb_generation(host);
                        req->req.misc = 0;
                        req->req.recvb = ptr2int(fi->iso_buffer);
                        req->req.length = min(length, fi->iso_buffer_length);
                        
                        list_add_tail(&req->list, &reqs);
                }
        }
        spin_unlock_irqrestore(&host_info_lock, flags);

        lh = reqs.next;
        while (lh != &reqs) {
                req = list_entry(lh, struct pending_request, list);
                lh = lh->next;

                queue_complete_req(req);
        }
}

static void fcp_request(struct hpsb_host *host, int nodeid, int direction,
			int cts, u8 *data, size_t length)
{
        unsigned long flags;
        struct list_head *lh;
        struct host_info *hi;
        struct file_info *fi;
        struct pending_request *req;
        struct iso_block_store *ibs = NULL;
        LIST_HEAD(reqs);

        if ((atomic_read(&iso_buffer_size) + length) > iso_buffer_max) {
                HPSB_INFO("dropped fcp request");
                return;
        }

        spin_lock_irqsave(&host_info_lock, flags);
        hi = find_host_info(host);

        if (hi != NULL) {
		list_for_each(lh, &hi->file_info_list) {
                        fi = list_entry(lh, struct file_info, list);

                        if (!fi->fcp_buffer) {
                                continue;
                        }

                        req = __alloc_pending_request(SLAB_ATOMIC);
                        if (!req) break;

                        if (!ibs) {
                                ibs = kmalloc(sizeof(struct iso_block_store)
                                              + length, SLAB_ATOMIC);
                                if (!ibs) {
                                        kfree(req);
                                        break;
                                }

                                atomic_add(length, &iso_buffer_size);
                                atomic_set(&ibs->refcount, 0);
                                ibs->data_size = length;
                                memcpy(ibs->data, data, length);
                        }

                        atomic_inc(&ibs->refcount);

                        req->file_info = fi;
                        req->ibs = ibs;
                        req->data = ibs->data;
                        req->req.type = RAW1394_REQ_FCP_REQUEST;
                        req->req.generation = get_hpsb_generation(host);
                        req->req.misc = nodeid | (direction << 16);
                        req->req.recvb = ptr2int(fi->fcp_buffer);
                        req->req.length = length;
                        
                        list_add_tail(&req->list, &reqs);
                }
        }
        spin_unlock_irqrestore(&host_info_lock, flags);

        lh = reqs.next;
        while (lh != &reqs) {
                req = list_entry(lh, struct pending_request, list);
                lh = lh->next;

                queue_complete_req(req);
        }
}


static ssize_t raw1394_read(struct file *file, char *buffer, size_t count,
                    loff_t *offset_is_ignored)
{
        struct file_info *fi = (struct file_info *)file->private_data;
        struct list_head *lh;
        struct pending_request *req;

        if (count != sizeof(struct raw1394_request)) {
                return -EINVAL;
        }

        if (!access_ok(VERIFY_WRITE, buffer, count)) {
                return -EFAULT;
        }

        if (file->f_flags & O_NONBLOCK) {
                if (down_trylock(&fi->complete_sem)) {
                        return -EAGAIN;
                }
        } else {
                if (down_interruptible(&fi->complete_sem)) {
                        return -ERESTARTSYS;
                }
        }

        spin_lock_irq(&fi->reqlists_lock);
        lh = fi->req_complete.next;
        list_del(lh);
        spin_unlock_irq(&fi->reqlists_lock);

        req = list_entry(lh, struct pending_request, list);

        if (req->req.length) {
                if (copy_to_user(int2ptr(req->req.recvb), req->data,
                                 req->req.length)) {
                        req->req.error = RAW1394_ERROR_MEMFAULT;
                }
        }
        __copy_to_user(buffer, &req->req, sizeof(req->req));

        free_pending_request(req);
        return sizeof(struct raw1394_request);
}


static int state_opened(struct file_info *fi, struct pending_request *req)
{
        if (req->req.type == RAW1394_REQ_INITIALIZE) {
                switch (req->req.misc) {
                case RAW1394_KERNELAPI_VERSION:
                case 3:
                        fi->state = initialized;
                        fi->protocol_version = req->req.misc;
                        req->req.error = RAW1394_ERROR_NONE;
                        req->req.generation = atomic_read(&internal_generation);
                        break;

                default:
                        req->req.error = RAW1394_ERROR_COMPAT;
                        req->req.misc = RAW1394_KERNELAPI_VERSION;
                }
        } else {
                req->req.error = RAW1394_ERROR_STATE_ORDER;
        }

        req->req.length = 0;
        queue_complete_req(req);
        return sizeof(struct raw1394_request);
}

static int state_initialized(struct file_info *fi, struct pending_request *req)
{
        struct list_head *lh;
        struct host_info *hi;
        struct raw1394_khost_list *khl;

        if (req->req.generation != atomic_read(&internal_generation)) {
                req->req.error = RAW1394_ERROR_GENERATION;
                req->req.generation = atomic_read(&internal_generation);
                req->req.length = 0;
                queue_complete_req(req);
                return sizeof(struct raw1394_request);
        }

        switch (req->req.type) {
        case RAW1394_REQ_LIST_CARDS:
                spin_lock_irq(&host_info_lock);
                khl = kmalloc(sizeof(struct raw1394_khost_list) * host_count,
                              SLAB_ATOMIC);

                if (khl != NULL) {
                        req->req.misc = host_count;
                        req->data = (quadlet_t *)khl;
                        
                        list_for_each(lh, &host_info_list) {
                                hi = list_entry(lh, struct host_info, list);

                                khl->nodes = hi->host->node_count;
                                strcpy(khl->name, hi->host->driver->name);

                                khl++;
                        }
                }
                spin_unlock_irq(&host_info_lock);

                if (khl != NULL) {
                        req->req.error = RAW1394_ERROR_NONE;
                        req->req.length = min(req->req.length,
                                              (u32)(sizeof(struct raw1394_khost_list)
                                              * req->req.misc));
                        req->free_data = 1;
                } else {
                        return -ENOMEM;
                }
                break;

        case RAW1394_REQ_SET_CARD:
                lh = NULL;

                spin_lock_irq(&host_info_lock);
                if (req->req.misc < host_count) {
                        lh = host_info_list.next;
                        while (req->req.misc--) {
                                lh = lh->next;
                        }
                        hi = list_entry(lh, struct host_info, list);
                        hpsb_ref_host(hi->host); // XXX Need to handle failure case
                        list_add_tail(&fi->list, &hi->file_info_list);
                        fi->host = hi->host;
                        fi->state = connected;
                }
                spin_unlock_irq(&host_info_lock);

                if (lh != NULL) {
                        req->req.error = RAW1394_ERROR_NONE;
                        req->req.generation = get_hpsb_generation(fi->host);
                        req->req.misc = (fi->host->node_id << 16) 
                                | fi->host->node_count;
                        if (fi->protocol_version > 3) {
                                req->req.misc |= NODEID_TO_NODE(fi->host->irm_id) << 8;
                        }
                } else {
                        req->req.error = RAW1394_ERROR_INVALID_ARG;
                }

                req->req.length = 0;
                break;

        default:
                req->req.error = RAW1394_ERROR_STATE_ORDER;
                req->req.length = 0;
                break;
        }

        queue_complete_req(req);
        return sizeof(struct raw1394_request);
}

static void handle_iso_listen(struct file_info *fi, struct pending_request *req)
{
        int channel = req->req.misc;

        spin_lock_irq(&host_info_lock);
        if ((channel > 63) || (channel < -64)) {
                req->req.error = RAW1394_ERROR_INVALID_ARG;
        } else if (channel >= 0) {
                /* allocate channel req.misc */
                if (fi->listen_channels & (1ULL << channel)) {
                        req->req.error = RAW1394_ERROR_ALREADY;
                } else {
                        if (hpsb_listen_channel(&raw1394_highlevel, fi->host, channel)) {
				req->req.error = RAW1394_ERROR_ALREADY;
			} else {
				fi->listen_channels |= 1ULL << channel;
				fi->iso_buffer = int2ptr(req->req.recvb);
				fi->iso_buffer_length = req->req.length;
			}
                }
        } else {
                /* deallocate channel (one's complement neg) req.misc */
                channel = ~channel;

                if (fi->listen_channels & (1ULL << channel)) {
                        hpsb_unlisten_channel(&raw1394_highlevel, fi->host, channel);
                        fi->listen_channels &= ~(1ULL << channel);
                } else {
                        req->req.error = RAW1394_ERROR_INVALID_ARG;
                }
        }

        req->req.length = 0;
        queue_complete_req(req);
        spin_unlock_irq(&host_info_lock);
}

static void handle_fcp_listen(struct file_info *fi, struct pending_request *req)
{
        if (req->req.misc) {
                if (fi->fcp_buffer) {
                        req->req.error = RAW1394_ERROR_ALREADY;
                } else {
                        fi->fcp_buffer = (u8 *)int2ptr(req->req.recvb);
                }
        } else {
                if (!fi->fcp_buffer) {
                        req->req.error = RAW1394_ERROR_ALREADY;
                } else {
                        fi->fcp_buffer = NULL;
                }
        }

        req->req.length = 0;
        queue_complete_req(req);
}


static int handle_async_request(struct file_info *fi,
                                struct pending_request *req, int node)
{
        struct hpsb_packet *packet = NULL;
        u64 addr = req->req.address & 0xffffffffffffULL;

        switch (req->req.type) {
        case RAW1394_REQ_ASYNC_READ:
		DBGMSG("read_request called");
		packet = hpsb_make_readpacket(fi->host, node, addr, req->req.length);

		if (!packet)
			return -ENOMEM;

		if (req->req.length == 4)
			req->data = &packet->header[3];
		else
			req->data = packet->data;
  
                break;

	case RAW1394_REQ_ASYNC_WRITE:
		DBGMSG("write_request called");

		packet = hpsb_make_writepacket(fi->host, node, addr, NULL,
					       req->req.length);
		if (!packet)
			return -ENOMEM;

		if (req->req.length == 4) {
			if (copy_from_user(&packet->header[3], int2ptr(req->req.sendb),
					req->req.length))
				req->req.error = RAW1394_ERROR_MEMFAULT;
		} else {
			if (copy_from_user(packet->data, int2ptr(req->req.sendb),
					req->req.length))
				req->req.error = RAW1394_ERROR_MEMFAULT;
		}
			
		req->req.length = 0;
	    break;

	case RAW1394_REQ_ASYNC_STREAM:
		DBGMSG("stream_request called");

		packet = hpsb_make_streampacket(fi->host, NULL, req->req.length, node & 0x3f/*channel*/,
                                        (req->req.misc >> 16) & 0x3, req->req.misc & 0xf);
		if (!packet)
			return -ENOMEM;

		if (copy_from_user(packet->data, int2ptr(req->req.sendb),
		                   req->req.length))
			req->req.error = RAW1394_ERROR_MEMFAULT;
			
		req->req.length = 0;
		break;

        case RAW1394_REQ_LOCK:
                DBGMSG("lock_request called");
                if ((req->req.misc == EXTCODE_FETCH_ADD)
                    || (req->req.misc == EXTCODE_LITTLE_ADD)) {
                        if (req->req.length != 4) {
                                req->req.error = RAW1394_ERROR_INVALID_ARG;
                                break;
                        }
                } else {
                        if (req->req.length != 8) {
                                req->req.error = RAW1394_ERROR_INVALID_ARG;
                                break;
                        }
                }

                packet = hpsb_make_lockpacket(fi->host, node, addr,
                                              req->req.misc, NULL, 0);
                if (!packet) return -ENOMEM;

                if (copy_from_user(packet->data, int2ptr(req->req.sendb),
                                   req->req.length)) {
                        req->req.error = RAW1394_ERROR_MEMFAULT;
                        break;
                }

                req->data = packet->data;
                req->req.length = 4;
                break;

        case RAW1394_REQ_LOCK64:
                DBGMSG("lock64_request called");
                if ((req->req.misc == EXTCODE_FETCH_ADD)
                    || (req->req.misc == EXTCODE_LITTLE_ADD)) {
                        if (req->req.length != 8) {
                                req->req.error = RAW1394_ERROR_INVALID_ARG;
                                break;
                        }
                } else {
                        if (req->req.length != 16) {
                                req->req.error = RAW1394_ERROR_INVALID_ARG;
                                break;
                        }
                }
                packet = hpsb_make_lock64packet(fi->host, node, addr,
                                                req->req.misc, NULL, 0);
                if (!packet) return -ENOMEM;

                if (copy_from_user(packet->data, int2ptr(req->req.sendb),
                                   req->req.length)) {
                        req->req.error = RAW1394_ERROR_MEMFAULT;
                        break;
                }

                req->data = packet->data;
                req->req.length = 8;
                break;

        default:
                req->req.error = RAW1394_ERROR_STATE_ORDER;
        }

        req->packet = packet;

        if (req->req.error) {
                req->req.length = 0;
                queue_complete_req(req);
                return sizeof(struct raw1394_request);
        }

	hpsb_set_packet_complete_task(packet, (void(*)(void*))queue_complete_cb, req);

        spin_lock_irq(&fi->reqlists_lock);
        list_add_tail(&req->list, &fi->req_pending);
        spin_unlock_irq(&fi->reqlists_lock);

	packet->generation = req->req.generation;

        if (!hpsb_send_packet(packet)) {
                req->req.error = RAW1394_ERROR_SEND_ERROR;
                req->req.length = 0;
                hpsb_free_tlabel(packet);
                queue_complete_req(req);
        }
        return sizeof(struct raw1394_request);
}

static int handle_iso_send(struct file_info *fi, struct pending_request *req,
                           int channel)
{
        struct hpsb_packet *packet;

	packet = hpsb_make_isopacket(fi->host, req->req.length, channel & 0x3f,
				     (req->req.misc >> 16) & 0x3, req->req.misc & 0xf);
	if (!packet)
		return -ENOMEM;

        packet->speed_code = req->req.address & 0x3;

	req->packet = packet;

        if (copy_from_user(packet->data, int2ptr(req->req.sendb),
                           req->req.length)) {
                req->req.error = RAW1394_ERROR_MEMFAULT;
                req->req.length = 0;
                queue_complete_req(req);
                return sizeof(struct raw1394_request);
        }

        req->req.length = 0;
	hpsb_set_packet_complete_task(packet, (void (*)(void*))queue_complete_req, req);

        spin_lock_irq(&fi->reqlists_lock);
        list_add_tail(&req->list, &fi->req_pending);
        spin_unlock_irq(&fi->reqlists_lock);

	/* Update the generation of the packet just before sending. */
	packet->generation = req->req.generation;

        if (!hpsb_send_packet(packet)) {
                req->req.error = RAW1394_ERROR_SEND_ERROR;
                queue_complete_req(req);
        }

        return sizeof(struct raw1394_request);
}

static int handle_async_send(struct file_info *fi, struct pending_request *req)
{
        struct hpsb_packet *packet;
        int header_length = req->req.misc & 0xffff;
        int expect_response = req->req.misc >> 16;

        if ((header_length > req->req.length) ||
            (header_length  < 12))
        {
                req->req.error = RAW1394_ERROR_INVALID_ARG;
                req->req.length = 0;
                queue_complete_req(req);
                return sizeof(struct raw1394_request);
        } 

        packet = alloc_hpsb_packet(req->req.length-header_length);
        req->packet = packet;
        if (!packet) return -ENOMEM;

        if (copy_from_user(packet->header, int2ptr(req->req.sendb),
                           header_length)) {
                req->req.error = RAW1394_ERROR_MEMFAULT;
                req->req.length = 0;
                queue_complete_req(req);
                return sizeof(struct raw1394_request);
        }

        if (copy_from_user(packet->data, ((u8*) int2ptr(req->req.sendb)) + header_length,
                           packet->data_size)) {
                req->req.error = RAW1394_ERROR_MEMFAULT;
                req->req.length = 0;
                queue_complete_req(req);
                return sizeof(struct raw1394_request);
        }

        packet->type = hpsb_async;
        packet->node_id = packet->header[0] >> 16;
        packet->tcode = (packet->header[0] >> 4) & 0xf;
        packet->tlabel = (packet->header[0] >> 10) &0x3f;
        packet->host = fi->host;
        packet->expect_response = expect_response;
        packet->header_size=header_length;
        packet->data_size=req->req.length-header_length;

        req->req.length = 0;
        hpsb_set_packet_complete_task(packet, (void(*)(void*))queue_complete_cb, req);

        spin_lock_irq(&fi->reqlists_lock);
        list_add_tail(&req->list, &fi->req_pending);
        spin_unlock_irq(&fi->reqlists_lock);

        /* Update the generation of the packet just before sending. */
        packet->generation = req->req.generation;

        if (!hpsb_send_packet(packet)) {
                req->req.error = RAW1394_ERROR_SEND_ERROR;
                queue_complete_req(req);
        }

        return sizeof(struct raw1394_request);
}

static int arm_read (struct hpsb_host *host, int nodeid, quadlet_t *buffer,
		     u64 addr, size_t length, u16 flags)
{
        struct pending_request *req;
        struct list_head *lh;
        struct host_info *hi;
        struct file_info *fi = NULL;
        struct list_head *entry;
        struct arm_addr  *arm_addr = NULL;
        struct arm_request  *arm_req = NULL;
        struct arm_response *arm_resp = NULL;
        int found=0, size=0, rcode=-1;
        struct arm_request_response *arm_req_resp = NULL;

        DBGMSG("arm_read  called by node: %X"
              "addr: %4.4x %8.8x length: %Zu", nodeid,
              (u16) ((addr >>32) & 0xFFFF), (u32) (addr & 0xFFFFFFFF),
              length);
        spin_lock(&host_info_lock);
        hi = find_host_info(host); /* search address-entry */
        if (hi != NULL) {
                list_for_each(lh, &hi->file_info_list) {
                        fi = list_entry(lh, struct file_info, list);
                        entry = fi->addr_list.next;
                        while (entry != &(fi->addr_list)) {
                                arm_addr = list_entry(entry, struct arm_addr, addr_list);
                                if (((arm_addr->start) <= (addr)) && 
                                        ((arm_addr->end) >= (addr+length))) {
                                        found = 1;
                                        break;
                                }
                                entry = entry->next;
                        }
                        if (found) {
                                break;
                        }
                }
        }
        rcode = -1;
        if (!found) {
                printk(KERN_ERR "raw1394: arm_read FAILED addr_entry not found"
                " -> rcode_address_error\n");
                spin_unlock(&host_info_lock);
                return (RCODE_ADDRESS_ERROR);
        } else {
                DBGMSG("arm_read addr_entry FOUND");
        }
        if (arm_addr->rec_length < length) {
                DBGMSG("arm_read blocklength too big -> rcode_data_error");
                rcode = RCODE_DATA_ERROR; /* hardware error, data is unavailable */
        }
        if (rcode == -1) {
                if (arm_addr->access_rights & ARM_READ) {
                        if (!(arm_addr->client_transactions & ARM_READ)) {
                                memcpy(buffer,(arm_addr->addr_space_buffer)+(addr-(arm_addr->start)), 
                                       length);
                                DBGMSG("arm_read -> (rcode_complete)");
                                rcode = RCODE_COMPLETE;
                        }
                } else {
                        rcode = RCODE_TYPE_ERROR; /* function not allowed */
                        DBGMSG("arm_read -> rcode_type_error (access denied)");
                }
        }
        if (arm_addr->notification_options & ARM_READ) {
                DBGMSG("arm_read -> entering notification-section");
                req = __alloc_pending_request(SLAB_ATOMIC);
                if (!req) {
                        DBGMSG("arm_read -> rcode_conflict_error");
                        spin_unlock(&host_info_lock);
                        return(RCODE_CONFLICT_ERROR); /* A resource conflict was detected. 
                                                        The request may be retried */
                }
                if (rcode == RCODE_COMPLETE) {
                        size =  sizeof(struct arm_request)+sizeof(struct arm_response) +
                                length * sizeof(byte_t) +
                                sizeof (struct arm_request_response);
                } else {
                        size =  sizeof(struct arm_request)+sizeof(struct arm_response) +
                                sizeof (struct arm_request_response);
                }
                req->data = kmalloc(size, SLAB_ATOMIC);
                if (!(req->data)) {
                        free_pending_request(req);
                        DBGMSG("arm_read -> rcode_conflict_error");
                        spin_unlock(&host_info_lock);
                        return(RCODE_CONFLICT_ERROR); /* A resource conflict was detected. 
                                                        The request may be retried */
                }
                req->free_data=1;
                req->file_info = fi;
                req->req.type = RAW1394_REQ_ARM;
                req->req.generation = get_hpsb_generation(host);
                req->req.misc = ( ((length << 16) & (0xFFFF0000)) | (ARM_READ & 0xFF));
                req->req.tag  = arm_addr->arm_tag;
                req->req.recvb = arm_addr->recvb;
                req->req.length = size;
                arm_req_resp = (struct arm_request_response *) (req->data);
                arm_req  = (struct arm_request *) ((byte_t *)(req->data) + 
                        (sizeof (struct arm_request_response)));
                arm_resp = (struct arm_response *) ((byte_t *)(arm_req) + 
                        (sizeof(struct arm_request)));
                arm_req->buffer  = NULL;
                arm_resp->buffer = NULL;
                if (rcode == RCODE_COMPLETE) {
                        arm_resp->buffer = ((byte_t *)(arm_resp) + 
                                (sizeof(struct arm_response)));
                        memcpy (arm_resp->buffer,
                                (arm_addr->addr_space_buffer)+(addr-(arm_addr->start)), 
                                length);
                        arm_resp->buffer = int2ptr((arm_addr->recvb) + 
                                sizeof (struct arm_request_response) +
                                sizeof (struct arm_request) +
                                sizeof (struct arm_response));
                }
                arm_resp->buffer_length = (rcode == RCODE_COMPLETE) ? length : 0;
                arm_resp->response_code = rcode;
                arm_req->buffer_length = 0;
                arm_req->generation = req->req.generation;
                arm_req->extended_transaction_code = 0;
                arm_req->destination_offset = addr;
                arm_req->source_nodeid = nodeid;
                arm_req->destination_nodeid = host->node_id;
                arm_req->tlabel = (flags >> 10) & 0x3f;
                arm_req->tcode = (flags >> 4) & 0x0f;
                arm_req_resp->request  = int2ptr((arm_addr->recvb) + 
                        sizeof (struct arm_request_response));
                arm_req_resp->response = int2ptr((arm_addr->recvb) + 
                        sizeof (struct arm_request_response) +
                        sizeof (struct arm_request));
                queue_complete_req(req);
        }
        spin_unlock(&host_info_lock);
        return(rcode);
}

static int arm_write (struct hpsb_host *host, int nodeid, int destid,
		      quadlet_t *data, u64 addr, size_t length, u16 flags)
{
        struct pending_request *req;
        struct list_head *lh;
        struct host_info *hi;
        struct file_info *fi = NULL;
        struct list_head *entry;
        struct arm_addr  *arm_addr = NULL;
        struct arm_request  *arm_req = NULL;
        struct arm_response *arm_resp = NULL;        
        int found=0, size=0, rcode=-1, length_conflict=0;
        struct arm_request_response *arm_req_resp = NULL;

        DBGMSG("arm_write called by node: %X"
              "addr: %4.4x %8.8x length: %Zu", nodeid,
              (u16) ((addr >>32) & 0xFFFF), (u32) (addr & 0xFFFFFFFF),
              length);
        spin_lock(&host_info_lock);
        hi = find_host_info(host); /* search address-entry */
        if (hi != NULL) {
                list_for_each(lh, &hi->file_info_list) {
                        fi = list_entry(lh, struct file_info, list);
                        entry = fi->addr_list.next;
                        while (entry != &(fi->addr_list)) {
                                arm_addr = list_entry(entry, struct arm_addr, addr_list);
                                if (((arm_addr->start) <= (addr)) && 
                                        ((arm_addr->end) >= (addr+length))) {
                                        found = 1;
                                        break;
                                }
                                entry = entry->next;
                        }
                        if (found) {
                                break;
                        }
                }
        }
        rcode = -1;
        if (!found) {
                printk(KERN_ERR "raw1394: arm_write FAILED addr_entry not found"
                " -> rcode_address_error\n");
                spin_unlock(&host_info_lock);
                return (RCODE_ADDRESS_ERROR);
        } else {
                DBGMSG("arm_write addr_entry FOUND");
        }
        if (arm_addr->rec_length < length) {
                DBGMSG("arm_write blocklength too big -> rcode_data_error");
                length_conflict = 1;
                rcode = RCODE_DATA_ERROR; /* hardware error, data is unavailable */
        }
        if (rcode == -1) {
                if (arm_addr->access_rights & ARM_WRITE) {
                        if (!(arm_addr->client_transactions & ARM_WRITE)) {
                                memcpy((arm_addr->addr_space_buffer)+(addr-(arm_addr->start)),
                                        data, length);
                                DBGMSG("arm_write -> (rcode_complete)");
                                rcode = RCODE_COMPLETE;
                        }
                } else {
                        rcode = RCODE_TYPE_ERROR; /* function not allowed */
                        DBGMSG("arm_write -> rcode_type_error (access denied)");
                }
        }
        if (arm_addr->notification_options & ARM_WRITE) {
                DBGMSG("arm_write -> entering notification-section");
                req = __alloc_pending_request(SLAB_ATOMIC);
                if (!req) {
                        DBGMSG("arm_write -> rcode_conflict_error");
                        spin_unlock(&host_info_lock);
                        return(RCODE_CONFLICT_ERROR); /* A resource conflict was detected. 
                                                        The request my be retried */
                }
                size =  sizeof(struct arm_request)+sizeof(struct arm_response) +
                        (length) * sizeof(byte_t) +
                        sizeof (struct arm_request_response);
                req->data = kmalloc(size, SLAB_ATOMIC);
                if (!(req->data)) {
                        free_pending_request(req);
                        DBGMSG("arm_write -> rcode_conflict_error");
                        spin_unlock(&host_info_lock);
                        return(RCODE_CONFLICT_ERROR); /* A resource conflict was detected. 
                                                        The request may be retried */
                }
                req->free_data=1;
                req->file_info = fi;
                req->req.type = RAW1394_REQ_ARM;
                req->req.generation = get_hpsb_generation(host);
                req->req.misc = ( ((length << 16) & (0xFFFF0000)) | (ARM_WRITE & 0xFF));
                req->req.tag  = arm_addr->arm_tag;
                req->req.recvb = arm_addr->recvb;
                req->req.length = size;
                arm_req_resp = (struct arm_request_response *) (req->data);
                arm_req  = (struct arm_request *) ((byte_t *)(req->data) + 
                        (sizeof (struct arm_request_response)));
                arm_resp = (struct arm_response *) ((byte_t *)(arm_req) + 
                        (sizeof(struct arm_request)));
                arm_req->buffer = ((byte_t *)(arm_resp) + 
                        (sizeof(struct arm_response)));
                arm_resp->buffer = NULL;
                memcpy (arm_req->buffer, data, length);
                arm_req->buffer = int2ptr((arm_addr->recvb) + 
                        sizeof (struct arm_request_response) +
                        sizeof (struct arm_request) +
                        sizeof (struct arm_response));
                arm_req->buffer_length = length;
                arm_req->generation = req->req.generation;
                arm_req->extended_transaction_code = 0;
                arm_req->destination_offset = addr;
                arm_req->source_nodeid = nodeid;
                arm_req->destination_nodeid = destid;
                arm_req->tlabel = (flags >> 10) & 0x3f;
                arm_req->tcode = (flags >> 4) & 0x0f;
                arm_resp->buffer_length = 0;
                arm_resp->response_code = rcode;
                arm_req_resp->request  = int2ptr((arm_addr->recvb) + 
                        sizeof (struct arm_request_response));
                arm_req_resp->response = int2ptr((arm_addr->recvb) + 
                        sizeof (struct arm_request_response) +
                        sizeof (struct arm_request));
                queue_complete_req(req);
        }
        spin_unlock(&host_info_lock);
        return(rcode);
}

static int arm_lock (struct hpsb_host *host, int nodeid, quadlet_t *store,
             u64 addr, quadlet_t data, quadlet_t arg, int ext_tcode, u16 flags)
{
        struct pending_request *req;
        struct list_head *lh;
        struct host_info *hi;
        struct file_info *fi = NULL;
        struct list_head *entry;
        struct arm_addr  *arm_addr = NULL;
        struct arm_request  *arm_req = NULL;
        struct arm_response *arm_resp = NULL;        
        int found=0, size=0, rcode=-1;
        quadlet_t old, new;
        struct arm_request_response *arm_req_resp = NULL;

        if (((ext_tcode & 0xFF) == EXTCODE_FETCH_ADD) ||
                ((ext_tcode & 0xFF) == EXTCODE_LITTLE_ADD)) {
                DBGMSG("arm_lock  called by node: %X "
                      "addr: %4.4x %8.8x extcode: %2.2X data: %8.8X", 
                      nodeid, (u16) ((addr >>32) & 0xFFFF), (u32) (addr & 0xFFFFFFFF),
                      ext_tcode & 0xFF , be32_to_cpu(data));
        } else {
                DBGMSG("arm_lock  called by node: %X "
                      "addr: %4.4x %8.8x extcode: %2.2X data: %8.8X arg: %8.8X", 
                      nodeid, (u16) ((addr >>32) & 0xFFFF), (u32) (addr & 0xFFFFFFFF),
                      ext_tcode & 0xFF , be32_to_cpu(data), be32_to_cpu(arg));
        }
        spin_lock(&host_info_lock);
        hi = find_host_info(host); /* search address-entry */
        if (hi != NULL) {
                list_for_each(lh, &hi->file_info_list) {
                        fi = list_entry(lh, struct file_info, list);
                        entry = fi->addr_list.next;
                        while (entry != &(fi->addr_list)) {
                                arm_addr = list_entry(entry, struct arm_addr, addr_list);
                                if (((arm_addr->start) <= (addr)) && 
                                        ((arm_addr->end) >= (addr+sizeof(*store)))) {
                                        found = 1;
                                        break;
                                }
                                entry = entry->next;
                        }
                        if (found) {
                                break;
                        }
                }
        }
        rcode = -1;
        if (!found) {
                printk(KERN_ERR "raw1394: arm_lock FAILED addr_entry not found"
                " -> rcode_address_error\n");
                spin_unlock(&host_info_lock);
                return (RCODE_ADDRESS_ERROR);
        } else {
                DBGMSG("arm_lock addr_entry FOUND");
        }
        if (rcode == -1) {
                if (arm_addr->access_rights & ARM_LOCK) {
                        if (!(arm_addr->client_transactions & ARM_LOCK)) {
                                memcpy(&old,(arm_addr->addr_space_buffer)+(addr-(arm_addr->start)),
                                        sizeof(old));
                                switch (ext_tcode) {
                                        case (EXTCODE_MASK_SWAP):
                                                new = data | (old & ~arg);
                                                break;
                                        case (EXTCODE_COMPARE_SWAP):
                                                if (old == arg) {
                                                        new = data;
                                                } else {
                                                        new = old;
                                                }
                                                break;
                                        case (EXTCODE_FETCH_ADD):
                                                new = cpu_to_be32(be32_to_cpu(data) + be32_to_cpu(old));
                                                break;
                                        case (EXTCODE_LITTLE_ADD):
                                                new = cpu_to_le32(le32_to_cpu(data) + le32_to_cpu(old));
                                                break;
                                        case (EXTCODE_BOUNDED_ADD):
                                                if (old != arg) {
                                                        new = cpu_to_be32(be32_to_cpu(data) + 
                                                                be32_to_cpu(old));
                                                } else {
                                                        new = old;
                                                }
                                                break;
                                        case (EXTCODE_WRAP_ADD):
                                                if (old != arg) {
                                                        new = cpu_to_be32(be32_to_cpu(data) + 
                                                                be32_to_cpu(old));
                                                } else {
                                                        new = data;
                                                }
                                                break;
                                        default:
                                                rcode = RCODE_TYPE_ERROR; /* function not allowed */
                                                printk(KERN_ERR "raw1394: arm_lock FAILED "
                                                "ext_tcode not allowed -> rcode_type_error\n");
                                                break;
                                } /*switch*/
                                if (rcode == -1) {
                                        DBGMSG("arm_lock -> (rcode_complete)");
                                        rcode = RCODE_COMPLETE;
                                        memcpy (store, &old, sizeof(*store));
                                        memcpy ((arm_addr->addr_space_buffer)+
                                                (addr-(arm_addr->start)), 
                                                &new, sizeof(*store));
                                }
                        }
                } else {
                        rcode = RCODE_TYPE_ERROR; /* function not allowed */
                        DBGMSG("arm_lock -> rcode_type_error (access denied)");
                }
        }
        if (arm_addr->notification_options & ARM_LOCK) {
                DBGMSG("arm_lock -> entering notification-section");
                req = __alloc_pending_request(SLAB_ATOMIC);
                if (!req) {
                        DBGMSG("arm_lock -> rcode_conflict_error");
                        spin_unlock(&host_info_lock);
                        return(RCODE_CONFLICT_ERROR); /* A resource conflict was detected. 
                                                        The request may be retried */
                }
                size =  sizeof(struct arm_request)+sizeof(struct arm_response) +
                        3 * sizeof(*store) + 
                        sizeof (struct arm_request_response);  /* maximum */
                req->data = kmalloc(size, SLAB_ATOMIC);
                if (!(req->data)) {
                        free_pending_request(req);
                        DBGMSG("arm_lock -> rcode_conflict_error");
                        spin_unlock(&host_info_lock);
                        return(RCODE_CONFLICT_ERROR); /* A resource conflict was detected. 
                                                        The request may be retried */
                }
                req->free_data=1;
                arm_req_resp = (struct arm_request_response *) (req->data);
                arm_req  = (struct arm_request *) ((byte_t *)(req->data) + 
                        (sizeof (struct arm_request_response)));
                arm_resp = (struct arm_response *) ((byte_t *)(arm_req) + 
                        (sizeof(struct arm_request)));
                arm_req->buffer = ((byte_t *)(arm_resp) + 
                        (sizeof(struct arm_response)));
                arm_resp->buffer = ((byte_t *)(arm_req->buffer) + 
                        (2* sizeof(*store)));
                if ((ext_tcode == EXTCODE_FETCH_ADD) || 
                        (ext_tcode == EXTCODE_LITTLE_ADD)) {
                        arm_req->buffer_length = sizeof(*store);
                        memcpy (arm_req->buffer, &data, sizeof(*store));

                } else {
                        arm_req->buffer_length = 2 * sizeof(*store);
                        memcpy (arm_req->buffer, &arg,  sizeof(*store));
                        memcpy (((arm_req->buffer) + sizeof(*store)), 
                                &data, sizeof(*store));
                }
                if (rcode == RCODE_COMPLETE) {
                        arm_resp->buffer_length = sizeof(*store);
                        memcpy (arm_resp->buffer, &old, sizeof(*store));
                } else {
                        arm_resp->buffer = NULL;
                        arm_resp->buffer_length = 0;
                }
                req->file_info = fi;
                req->req.type = RAW1394_REQ_ARM;
                req->req.generation = get_hpsb_generation(host);
                req->req.misc = ( (((sizeof(*store)) << 16) & (0xFFFF0000)) | 
                        (ARM_LOCK & 0xFF));
                req->req.tag  = arm_addr->arm_tag;
                req->req.recvb = arm_addr->recvb;
                req->req.length = size;
                arm_req->generation = req->req.generation;
                arm_req->extended_transaction_code = ext_tcode;
                arm_req->destination_offset = addr;
                arm_req->source_nodeid = nodeid;
                arm_req->destination_nodeid = host->node_id;
                arm_req->tlabel = (flags >> 10) & 0x3f;
                arm_req->tcode = (flags >> 4) & 0x0f;
                arm_resp->response_code = rcode;
                arm_req_resp->request  = int2ptr((arm_addr->recvb) + 
                        sizeof (struct arm_request_response));
                arm_req_resp->response = int2ptr((arm_addr->recvb) + 
                        sizeof (struct arm_request_response) +
                        sizeof (struct arm_request));
                arm_req->buffer = int2ptr((arm_addr->recvb) + 
                        sizeof (struct arm_request_response) +
                        sizeof (struct arm_request) +
                        sizeof (struct arm_response));
                arm_resp->buffer = int2ptr((arm_addr->recvb) + 
                        sizeof (struct arm_request_response) +
                        sizeof (struct arm_request) +
                        sizeof (struct arm_response) +
                        2* sizeof (*store));
                queue_complete_req(req);
        }
        spin_unlock(&host_info_lock);
        return(rcode);
}

static int arm_lock64 (struct hpsb_host *host, int nodeid, octlet_t *store,
               u64 addr, octlet_t data, octlet_t arg, int ext_tcode, u16 flags)
{
        struct pending_request *req;
        struct list_head *lh;
        struct host_info *hi;
        struct file_info *fi = NULL;
        struct list_head *entry;
        struct arm_addr  *arm_addr = NULL;
        struct arm_request  *arm_req = NULL;
        struct arm_response *arm_resp = NULL;
        int found=0, size=0, rcode=-1;
        octlet_t old, new;
        struct arm_request_response *arm_req_resp = NULL;

        if (((ext_tcode & 0xFF) == EXTCODE_FETCH_ADD) ||
                ((ext_tcode & 0xFF) == EXTCODE_LITTLE_ADD)) {
                DBGMSG("arm_lock64 called by node: %X "
                      "addr: %4.4x %8.8x extcode: %2.2X data: %8.8X %8.8X ",
                      nodeid, (u16) ((addr >>32) & 0xFFFF),
                      (u32) (addr & 0xFFFFFFFF), 
                      ext_tcode & 0xFF , 
                      (u32) ((be64_to_cpu(data) >> 32) & 0xFFFFFFFF), 
                      (u32) (be64_to_cpu(data) & 0xFFFFFFFF));
        } else {
                DBGMSG("arm_lock64 called by node: %X "
                      "addr: %4.4x %8.8x extcode: %2.2X data: %8.8X %8.8X arg: "
                      "%8.8X %8.8X ",
                      nodeid, (u16) ((addr >>32) & 0xFFFF),
                      (u32) (addr & 0xFFFFFFFF), 
                      ext_tcode & 0xFF , 
                      (u32) ((be64_to_cpu(data) >> 32) & 0xFFFFFFFF), 
                      (u32) (be64_to_cpu(data) & 0xFFFFFFFF),
                      (u32) ((be64_to_cpu(arg)  >> 32) & 0xFFFFFFFF), 
                      (u32) (be64_to_cpu(arg)  & 0xFFFFFFFF));
        }
        spin_lock(&host_info_lock);
        hi = find_host_info(host); /* search addressentry in file_info's for host */
        if (hi != NULL) {
                list_for_each(lh, &hi->file_info_list) {
                        fi = list_entry(lh, struct file_info, list);
                        entry = fi->addr_list.next;
                        while (entry != &(fi->addr_list)) {
                                arm_addr = list_entry(entry, struct arm_addr, addr_list);
                                if (((arm_addr->start) <= (addr)) && 
                                        ((arm_addr->end) >= (addr+sizeof(*store)))) {
                                        found = 1;
                                        break;
                                }
                                entry = entry->next;
                        }
                        if (found) {
                                break;
                        }
                }
        }
        rcode = -1;
        if (!found) {
                printk(KERN_ERR "raw1394: arm_lock64 FAILED addr_entry not found"
                " -> rcode_address_error\n");
                spin_unlock(&host_info_lock);
                return (RCODE_ADDRESS_ERROR);
        } else {
                DBGMSG("arm_lock64 addr_entry FOUND");
        }
        if (rcode == -1) {
                if (arm_addr->access_rights & ARM_LOCK) {
                        if (!(arm_addr->client_transactions & ARM_LOCK)) {
                                memcpy(&old,(arm_addr->addr_space_buffer)+(addr-(arm_addr->start)),
                                        sizeof(old));
                                switch (ext_tcode) {
                                        case (EXTCODE_MASK_SWAP):
                                                new = data | (old & ~arg);
                                                break;
                                        case (EXTCODE_COMPARE_SWAP):
                                                if (old == arg) {
                                                        new = data;
                                                } else {
                                                        new = old;
                                                }
                                                break;
                                        case (EXTCODE_FETCH_ADD):
                                                new = cpu_to_be64(be64_to_cpu(data) + be64_to_cpu(old));
                                                break;
                                        case (EXTCODE_LITTLE_ADD):
                                                new = cpu_to_le64(le64_to_cpu(data) + le64_to_cpu(old));
                                                break;
                                        case (EXTCODE_BOUNDED_ADD):
                                                if (old != arg) {
                                                        new = cpu_to_be64(be64_to_cpu(data) + 
                                                                be64_to_cpu(old));
                                                } else {
                                                        new = old;
                                                }
                                                break;
                                        case (EXTCODE_WRAP_ADD):
                                                if (old != arg) {
                                                        new = cpu_to_be64(be64_to_cpu(data) + 
                                                                be64_to_cpu(old));
                                                } else {
                                                        new = data;
                                                }
                                                break;
                                        default:
                                                printk(KERN_ERR "raw1394: arm_lock64 FAILED "
                                                "ext_tcode not allowed -> rcode_type_error\n");
                                                rcode = RCODE_TYPE_ERROR; /* function not allowed */
                                                break;
                                } /*switch*/
                                if (rcode == -1) {
                                        DBGMSG("arm_lock64 -> (rcode_complete)");
                                        rcode = RCODE_COMPLETE;
                                        memcpy (store, &old, sizeof(*store));
                                        memcpy ((arm_addr->addr_space_buffer)+
                                                (addr-(arm_addr->start)), 
                                                &new, sizeof(*store));
                                } 
                        }
                } else {
                        rcode = RCODE_TYPE_ERROR; /* function not allowed */
                        DBGMSG("arm_lock64 -> rcode_type_error (access denied)");
                }
        }
        if (arm_addr->notification_options & ARM_LOCK) {
                DBGMSG("arm_lock64 -> entering notification-section");
                req = __alloc_pending_request(SLAB_ATOMIC);
                if (!req) {
                        spin_unlock(&host_info_lock);
                        DBGMSG("arm_lock64 -> rcode_conflict_error");
                        return(RCODE_CONFLICT_ERROR); /* A resource conflict was detected. 
                                                        The request may be retried */
                }
                size =  sizeof(struct arm_request)+sizeof(struct arm_response) +
                        3 * sizeof(*store) +
                        sizeof (struct arm_request_response); /* maximum */
                req->data = kmalloc(size, SLAB_ATOMIC);
                if (!(req->data)) {
                        free_pending_request(req);
                        spin_unlock(&host_info_lock);
                        DBGMSG("arm_lock64 -> rcode_conflict_error");
                        return(RCODE_CONFLICT_ERROR); /* A resource conflict was detected. 
                                                        The request may be retried */
                }
                req->free_data=1;
                arm_req_resp = (struct arm_request_response *) (req->data);
                arm_req  = (struct arm_request *) ((byte_t *)(req->data) + 
                        (sizeof (struct arm_request_response)));
                arm_resp = (struct arm_response *) ((byte_t *)(arm_req) + 
                        (sizeof(struct arm_request)));
                arm_req->buffer = ((byte_t *)(arm_resp) + 
                        (sizeof(struct arm_response)));
                arm_resp->buffer = ((byte_t *)(arm_req->buffer) + 
                        (2* sizeof(*store)));
                if ((ext_tcode == EXTCODE_FETCH_ADD) || 
                        (ext_tcode == EXTCODE_LITTLE_ADD)) {
                        arm_req->buffer_length = sizeof(*store);
                        memcpy (arm_req->buffer, &data, sizeof(*store));

                } else {
                        arm_req->buffer_length = 2 * sizeof(*store);
                        memcpy (arm_req->buffer, &arg,  sizeof(*store));
                        memcpy (((arm_req->buffer) + sizeof(*store)), 
                                &data, sizeof(*store));
                }
                if (rcode == RCODE_COMPLETE) {
                        arm_resp->buffer_length = sizeof(*store);
                        memcpy (arm_resp->buffer, &old, sizeof(*store));
                } else {
                        arm_resp->buffer = NULL;
                        arm_resp->buffer_length = 0;
                }
                req->file_info = fi;
                req->req.type = RAW1394_REQ_ARM;
                req->req.generation = get_hpsb_generation(host);
                req->req.misc = ( (((sizeof(*store)) << 16) & (0xFFFF0000)) | 
                        (ARM_LOCK & 0xFF));
                req->req.tag  = arm_addr->arm_tag;
                req->req.recvb = arm_addr->recvb;
                req->req.length = size;
                arm_req->generation = req->req.generation;
                arm_req->extended_transaction_code = ext_tcode;
                arm_req->destination_offset = addr;
                arm_req->source_nodeid = nodeid;
                arm_req->destination_nodeid = host->node_id;
                arm_req->tlabel = (flags >> 10) & 0x3f;
                arm_req->tcode = (flags >> 4) & 0x0f;
                arm_resp->response_code = rcode;
                arm_req_resp->request  = int2ptr((arm_addr->recvb) + 
                        sizeof (struct arm_request_response));
                arm_req_resp->response = int2ptr((arm_addr->recvb) + 
                        sizeof (struct arm_request_response) +
                        sizeof (struct arm_request));
                arm_req->buffer = int2ptr((arm_addr->recvb) + 
                        sizeof (struct arm_request_response) +
                        sizeof (struct arm_request) +
                        sizeof (struct arm_response));
                arm_resp->buffer = int2ptr((arm_addr->recvb) + 
                        sizeof (struct arm_request_response) +
                        sizeof (struct arm_request) +
                        sizeof (struct arm_response) +
                        2* sizeof (*store));
                queue_complete_req(req);
        }
        spin_unlock(&host_info_lock);
        return(rcode);
}

static int arm_register(struct file_info *fi, struct pending_request *req)
{
        int retval;
        struct arm_addr *addr;
        struct list_head *lh, *lh_1, *lh_2;
        struct host_info *hi;
        struct file_info *fi_hlp = NULL;
        struct list_head *entry;
        struct arm_addr  *arm_addr = NULL;
        int same_host, another_host;
        unsigned long flags;

        DBGMSG("arm_register called "
              "addr(Offset): %8.8x %8.8x length: %u "
              "rights: %2.2X notify: %2.2X "
              "max_blk_len: %4.4X",
              (u32) ((req->req.address >>32) & 0xFFFF),
              (u32) (req->req.address & 0xFFFFFFFF),
              req->req.length, ((req->req.misc >> 8) & 0xFF),
              (req->req.misc & 0xFF),((req->req.misc >> 16) & 0xFFFF));
        /* check addressrange */
        if ((((req->req.address) & ~(0xFFFFFFFFFFFFULL)) != 0) ||
                (((req->req.address + req->req.length) & ~(0xFFFFFFFFFFFFULL)) != 0)) {
                req->req.length = 0;
                return (-EINVAL);
        }
        /* addr-list-entry for fileinfo */
        addr = (struct arm_addr *)kmalloc(sizeof(struct arm_addr), SLAB_KERNEL); 
        if (!addr) {
                req->req.length = 0;
                return (-ENOMEM);
        } 
        /* allocation of addr_space_buffer */
        addr->addr_space_buffer = (u8 *)vmalloc(req->req.length);
        if (!(addr->addr_space_buffer)) {
                kfree(addr);
                req->req.length = 0;
                return (-ENOMEM);
        }
        /* initialization of addr_space_buffer */
        if ((req->req.sendb)== (unsigned long)NULL) {
                /* init: set 0 */
                memset(addr->addr_space_buffer, 0,req->req.length);
        } else {
                /* init: user -> kernel */
                if (copy_from_user(addr->addr_space_buffer,int2ptr(req->req.sendb),
                        req->req.length)) {
                        vfree(addr->addr_space_buffer);
                        kfree(addr);
                        return (-EFAULT);
                }
        }
        INIT_LIST_HEAD(&addr->addr_list);
        addr->arm_tag   = req->req.tag;
        addr->start     = req->req.address;
        addr->end       = req->req.address + req->req.length;
        addr->access_rights = (u8) (req->req.misc & 0x0F);
        addr->notification_options = (u8) ((req->req.misc >> 4) & 0x0F);
        addr->client_transactions = (u8) ((req->req.misc >> 8) & 0x0F);
        addr->access_rights |= addr->client_transactions;
        addr->notification_options |= addr->client_transactions;
        addr->recvb     = req->req.recvb;
        addr->rec_length = (u16) ((req->req.misc >> 16) & 0xFFFF);
        spin_lock_irqsave(&host_info_lock, flags);
        hi = find_host_info(fi->host);
        same_host = 0;
        another_host = 0;
        /* same host with address-entry containing same addressrange ? */
        list_for_each(lh, &hi->file_info_list) {
                fi_hlp = list_entry(lh, struct file_info, list);
                entry = fi_hlp->addr_list.next;
                while (entry != &(fi_hlp->addr_list)) {
                        arm_addr = list_entry(entry, struct arm_addr, addr_list);
                        if ( (arm_addr->start == addr->start) && 
                                (arm_addr->end == addr->end)) {
                                DBGMSG("same host ownes same "
                                        "addressrange -> EALREADY");
                                same_host = 1;
                                break;
                        }
                        entry = entry->next;
                }
                if (same_host) {
                        break;
                }
        }
        if (same_host) {
                /* addressrange occupied by same host */
                vfree(addr->addr_space_buffer);
                kfree(addr);
                spin_unlock_irqrestore(&host_info_lock, flags);
                return (-EALREADY);
        }
        /* another host with valid address-entry containing same addressrange */
        list_for_each(lh_1, &host_info_list) {
                hi = list_entry(lh_1, struct host_info, list);
                if (hi->host != fi->host) {
                        list_for_each(lh_2, &hi->file_info_list) {
                                fi_hlp = list_entry(lh_2, struct file_info, list);
                                entry = fi_hlp->addr_list.next;
                                while (entry != &(fi_hlp->addr_list)) {
                                        arm_addr = list_entry(entry, struct arm_addr, addr_list);
                                        if ( (arm_addr->start == addr->start) && 
                                                (arm_addr->end == addr->end)) {
                                                DBGMSG("another host ownes same "
                                                "addressrange");
                                                another_host = 1;
                                                break;
                                        }
                                        entry = entry->next;
                                }
                                if (another_host) {
                                        break;
                                }
                        }
                }
        }
        if (another_host) {
                DBGMSG("another hosts entry is valid -> SUCCESS");
                if (copy_to_user(int2ptr(req->req.recvb),
                        int2ptr(&addr->start),sizeof(u64))) {
                        printk(KERN_ERR "raw1394: arm_register failed "
                              " address-range-entry is invalid -> EFAULT !!!\n");
                        vfree(addr->addr_space_buffer);
                        kfree(addr);
                        spin_unlock_irqrestore(&host_info_lock, flags);
                        return (-EFAULT);
                }
                free_pending_request(req); /* immediate success or fail */
                /* INSERT ENTRY */
                list_add_tail(&addr->addr_list, &fi->addr_list);
                spin_unlock_irqrestore(&host_info_lock, flags);
                return sizeof(struct raw1394_request);
        }
        retval = hpsb_register_addrspace(&raw1394_highlevel, &arm_ops, req->req.address,
                req->req.address + req->req.length);
        if (retval) {
               /* INSERT ENTRY */
               list_add_tail(&addr->addr_list, &fi->addr_list);
        } else {
                DBGMSG("arm_register failed errno: %d \n",retval);
                vfree(addr->addr_space_buffer);
                kfree(addr);
                spin_unlock_irqrestore(&host_info_lock, flags);
                return (-EALREADY); 
        }
        spin_unlock_irqrestore(&host_info_lock, flags);
        free_pending_request(req); /* immediate success or fail */
        return sizeof(struct raw1394_request);
}

static int arm_unregister(struct file_info *fi, struct pending_request *req)
{
        int found  = 0;
        int retval = 0;
        struct list_head *entry;
        struct arm_addr  *addr = NULL;
        struct list_head *lh_1, *lh_2;
        struct host_info *hi;
        struct file_info *fi_hlp = NULL;
        struct arm_addr  *arm_addr = NULL;
        int another_host;
        unsigned long flags;

        DBGMSG("arm_Unregister called addr(Offset): "
              "%8.8x %8.8x",
              (u32) ((req->req.address >>32) & 0xFFFF),
              (u32) (req->req.address & 0xFFFFFFFF));
        spin_lock_irqsave(&host_info_lock, flags);
        /* get addr */
        entry = fi->addr_list.next;
        while (entry != &(fi->addr_list)) {
                addr = list_entry(entry, struct arm_addr, addr_list);
                if (addr->start == req->req.address) {
                        found = 1;
                        break;
                }
                entry = entry->next;
        }
        if (!found) {
                DBGMSG("arm_Unregister addr not found");
                spin_unlock_irqrestore(&host_info_lock, flags);
                return (-EINVAL);
        }
        DBGMSG("arm_Unregister addr found");
        another_host = 0;
        /* another host with valid address-entry containing 
           same addressrange */
        list_for_each(lh_1, &host_info_list) {
                hi = list_entry(lh_1, struct host_info, list);
                if (hi->host != fi->host) {
                        list_for_each(lh_2, &hi->file_info_list) {
                                fi_hlp = list_entry(lh_2, struct file_info, list);
                                entry = fi_hlp->addr_list.next;
                                while (entry != &(fi_hlp->addr_list)) {
                                        arm_addr = list_entry(entry, 
                                                struct arm_addr, addr_list);
                                        if (arm_addr->start == 
                                                addr->start) {
                                                DBGMSG("another host ownes "
                                                "same addressrange");
                                                another_host = 1;
                                                break;
                                        }
                                        entry = entry->next;
                                }
                                if (another_host) {
                                        break;
                                }
                        }
                }
        }
        if (another_host) {
                DBGMSG("delete entry from list -> success");
                list_del(&addr->addr_list);
                vfree(addr->addr_space_buffer);
                kfree(addr);
                free_pending_request(req); /* immediate success or fail */
                spin_unlock_irqrestore(&host_info_lock, flags);
                return sizeof(struct raw1394_request);
        } 
        retval = hpsb_unregister_addrspace(&raw1394_highlevel, addr->start);
        if (!retval) {
                printk(KERN_ERR "raw1394: arm_Unregister failed -> EINVAL\n");
                spin_unlock_irqrestore(&host_info_lock, flags);
                return (-EINVAL);
        }
        DBGMSG("delete entry from list -> success");
        list_del(&addr->addr_list);
        spin_unlock_irqrestore(&host_info_lock, flags);
        vfree(addr->addr_space_buffer);
        kfree(addr);
        free_pending_request(req); /* immediate success or fail */
        return sizeof(struct raw1394_request);
}

static int reset_notification(struct file_info *fi, struct pending_request *req)
{
        DBGMSG("reset_notification called - switch %s ",
                (req->req.misc == RAW1394_NOTIFY_OFF)?"OFF":"ON");
        if ((req->req.misc == RAW1394_NOTIFY_OFF) ||
                (req->req.misc == RAW1394_NOTIFY_ON)) {
                fi->notification=(u8)req->req.misc;
                free_pending_request(req); /* we have to free the request, because we queue no response, and therefore nobody will free it */
                return sizeof(struct raw1394_request);
        } 
        /* error EINVAL (22) invalid argument */
        return (-EINVAL);
}

static int write_phypacket(struct file_info *fi, struct pending_request *req)
{
        struct hpsb_packet *packet = NULL;
        int retval=0;
        quadlet_t data;

        data = be32_to_cpu((u32)req->req.sendb);
        DBGMSG("write_phypacket called - quadlet 0x%8.8x ",data);
        packet = hpsb_make_phypacket (fi->host, data);
        if (!packet) return -ENOMEM;
        req->req.length=0;
        req->packet=packet;
        hpsb_set_packet_complete_task(packet, (void(*)(void*))queue_complete_cb, req);
        spin_lock_irq(&fi->reqlists_lock);
        list_add_tail(&req->list, &fi->req_pending);
        spin_unlock_irq(&fi->reqlists_lock);
        packet->generation = req->req.generation;
        retval = hpsb_send_packet(packet);
        DBGMSG("write_phypacket send_packet called => retval: %d ",
                retval);
        if (! retval) {
                req->req.error = RAW1394_ERROR_SEND_ERROR;
                req->req.length = 0;
                queue_complete_req(req);
        }
        return sizeof(struct raw1394_request);
}

static int get_config_rom(struct file_info *fi, struct pending_request *req)
{
        size_t return_size;
        unsigned char rom_version;
        int ret=sizeof(struct raw1394_request);
        quadlet_t *data = kmalloc(req->req.length, SLAB_KERNEL);
        int status;
        if (!data) return -ENOMEM;
        status = hpsb_get_config_rom(fi->host, data, 
                req->req.length, &return_size, &rom_version);
        if (copy_to_user(int2ptr(req->req.recvb), data, 
                req->req.length))
                ret = -EFAULT;
        if (copy_to_user(int2ptr(req->req.tag), &return_size, 
                sizeof(return_size)))
                ret = -EFAULT;
        if (copy_to_user(int2ptr(req->req.address), &rom_version, 
                sizeof(rom_version)))
                ret = -EFAULT;
        if (copy_to_user(int2ptr(req->req.sendb), &status, 
                sizeof(status)))
                ret = -EFAULT;
        kfree(data);
        if (ret >= 0) {
                free_pending_request(req); /* we have to free the request, because we queue no response, and therefore nobody will free it */
        }
        return ret;
}

static int update_config_rom(struct file_info *fi, struct pending_request *req)
{
        int ret=sizeof(struct raw1394_request);
        quadlet_t *data = kmalloc(req->req.length, SLAB_KERNEL);
        if (!data) return -ENOMEM;
        if (copy_from_user(data,int2ptr(req->req.sendb), 
                req->req.length)) {
                ret= -EFAULT;
        } else {
                int status = hpsb_update_config_rom(fi->host, 
                        data, req->req.length, 
                        (unsigned char) req->req.misc);
                if (copy_to_user(int2ptr(req->req.recvb), 
                        &status, sizeof(status)))
                        ret = -ENOMEM;
        }
        kfree(data);
        if (ret >= 0) {
                free_pending_request(req); /* we have to free the request, because we queue no response, and therefore nobody will free it */
        }
        return ret;
}

static int state_connected(struct file_info *fi, struct pending_request *req)
{
        int node = req->req.address >> 48;

        req->req.error = RAW1394_ERROR_NONE;

        switch (req->req.type) {

        case RAW1394_REQ_ECHO:
                queue_complete_req(req);
                return sizeof(struct raw1394_request);

        case RAW1394_REQ_ISO_SEND:
                return handle_iso_send(fi, req, node);

        case RAW1394_REQ_ARM_REGISTER:
                return arm_register(fi, req);

        case RAW1394_REQ_ARM_UNREGISTER:
                return arm_unregister(fi, req);

        case RAW1394_REQ_RESET_NOTIFY:
                return reset_notification(fi, req);

        case RAW1394_REQ_ISO_LISTEN:
                handle_iso_listen(fi, req);
                return sizeof(struct raw1394_request);

        case RAW1394_REQ_FCP_LISTEN:
                handle_fcp_listen(fi, req);
                return sizeof(struct raw1394_request);

        case RAW1394_REQ_RESET_BUS:
                if (req->req.misc == RAW1394_LONG_RESET) {
                        DBGMSG("busreset called (type: LONG)");
                        hpsb_reset_bus(fi->host, LONG_RESET);
                        free_pending_request(req); /* we have to free the request, because we queue no response, and therefore nobody will free it */
                        return sizeof(struct raw1394_request);
                }
                if (req->req.misc == RAW1394_SHORT_RESET) {
                        DBGMSG("busreset called (type: SHORT)");
                        hpsb_reset_bus(fi->host, SHORT_RESET);
                        free_pending_request(req); /* we have to free the request, because we queue no response, and therefore nobody will free it */
                        return sizeof(struct raw1394_request);
                }
                /* error EINVAL (22) invalid argument */
                return (-EINVAL);
        case RAW1394_REQ_GET_ROM:
                return get_config_rom(fi, req);

        case RAW1394_REQ_UPDATE_ROM:
                return update_config_rom(fi, req);
        }

        if (req->req.generation != get_hpsb_generation(fi->host)) {
                req->req.error = RAW1394_ERROR_GENERATION;
                req->req.generation = get_hpsb_generation(fi->host);
                req->req.length = 0;
                queue_complete_req(req);
                return sizeof(struct raw1394_request);
        }

        switch (req->req.type) {
        case RAW1394_REQ_PHYPACKET:
                return write_phypacket(fi, req);
        case RAW1394_REQ_ASYNC_SEND:
                return handle_async_send(fi, req);
        }

        if (req->req.length == 0) {
                req->req.error = RAW1394_ERROR_INVALID_ARG;
                queue_complete_req(req);
                return sizeof(struct raw1394_request);
        }

        return handle_async_request(fi, req, node);
}


static ssize_t raw1394_write(struct file *file, const char *buffer, size_t count,
                     loff_t *offset_is_ignored)
{
        struct file_info *fi = (struct file_info *)file->private_data;
        struct pending_request *req;
        ssize_t retval = 0;

        if (count != sizeof(struct raw1394_request)) {
                return -EINVAL;
        }

        req = alloc_pending_request();
        if (req == NULL) {
                return -ENOMEM;
        }
        req->file_info = fi;

        if (copy_from_user(&req->req, buffer, sizeof(struct raw1394_request))) {
                free_pending_request(req);
                return -EFAULT;
        }

        switch (fi->state) {
        case opened:
                retval = state_opened(fi, req);
                break;

        case initialized:
                retval = state_initialized(fi, req);
                break;

        case connected:
                retval = state_connected(fi, req);
                break;
        }

        if (retval < 0) {
                free_pending_request(req);
        }

        return retval;
}

/* rawiso operations */

/* check if any RAW1394_REQ_RAWISO_ACTIVITY event is already in the
 * completion queue (reqlists_lock must be taken) */
static inline int __rawiso_event_in_queue(struct file_info *fi)
{
	struct list_head *lh;
	struct pending_request *req;

	list_for_each(lh, &fi->req_complete) {
		req = list_entry(lh, struct pending_request, list);
		if (req->req.type == RAW1394_REQ_RAWISO_ACTIVITY) {
			return 1;
		}
	}

	return 0;
}

/* put a RAWISO_ACTIVITY event in the queue, if one isn't there already */
static void queue_rawiso_event(struct file_info *fi)
{
	unsigned long flags;

	spin_lock_irqsave(&fi->reqlists_lock, flags);

	/* only one ISO activity event may be in the queue */
	if (!__rawiso_event_in_queue(fi)) {
		struct pending_request *req = __alloc_pending_request(SLAB_ATOMIC);

		if (req) {
			req->file_info = fi;
			req->req.type = RAW1394_REQ_RAWISO_ACTIVITY;
			req->req.generation = get_hpsb_generation(fi->host);
			__queue_complete_req(req);
		} else {
			/* on allocation failure, signal an overflow */
			if (fi->iso_handle) {
				atomic_inc(&fi->iso_handle->overflows);
			}
		}
	}
	spin_unlock_irqrestore(&fi->reqlists_lock, flags);
}

static void rawiso_activity_cb(struct hpsb_iso *iso)
{
	unsigned long flags;
        struct list_head *lh;
        struct host_info *hi;

        spin_lock_irqsave(&host_info_lock, flags);
        hi = find_host_info(iso->host);

	if (hi != NULL) {
		list_for_each(lh, &hi->file_info_list) {
			struct file_info *fi = list_entry(lh, struct file_info, list);
			if (fi->iso_handle == iso)
				queue_rawiso_event(fi);
		}
	}

	spin_unlock_irqrestore(&host_info_lock, flags);
}

/* helper function - gather all the kernel iso status bits for returning to user-space */
static void raw1394_iso_fill_status(struct hpsb_iso *iso, struct raw1394_iso_status *stat)
{
	stat->config.data_buf_size = iso->buf_size;
	stat->config.buf_packets = iso->buf_packets;
	stat->config.channel = iso->channel;
	stat->config.speed = iso->speed;
	stat->config.irq_interval = iso->irq_interval;
	stat->n_packets = hpsb_iso_n_ready(iso);
	stat->overflows = atomic_read(&iso->overflows);
	stat->xmit_cycle = iso->xmit_cycle;
}

static int raw1394_iso_xmit_init(struct file_info *fi, void *uaddr)
{
	struct raw1394_iso_status stat;

	if (!fi->host)
		return -EINVAL;

	if (copy_from_user(&stat, uaddr, sizeof(stat)))
		return -EFAULT;

	fi->iso_handle = hpsb_iso_xmit_init(fi->host,
					    stat.config.data_buf_size,
					    stat.config.buf_packets,
					    stat.config.channel,
					    stat.config.speed,
					    stat.config.irq_interval,
					    rawiso_activity_cb);
	if (!fi->iso_handle)
		return -ENOMEM;

	fi->iso_state = RAW1394_ISO_XMIT;

	raw1394_iso_fill_status(fi->iso_handle, &stat);
	if (copy_to_user(uaddr, &stat, sizeof(stat)))
		return -EFAULT;

	/* queue an event to get things started */
	rawiso_activity_cb(fi->iso_handle);

	return 0;
}

static int raw1394_iso_recv_init(struct file_info *fi, void *uaddr)
{
	struct raw1394_iso_status stat;

	if (!fi->host)
		return -EINVAL;

	if (copy_from_user(&stat, uaddr, sizeof(stat)))
		return -EFAULT;

	fi->iso_handle = hpsb_iso_recv_init(fi->host,
					    stat.config.data_buf_size,
					    stat.config.buf_packets,
					    stat.config.channel,
					    stat.config.irq_interval,
					    rawiso_activity_cb);
	if (!fi->iso_handle)
		return -ENOMEM;

	fi->iso_state = RAW1394_ISO_RECV;

	raw1394_iso_fill_status(fi->iso_handle, &stat);
	if (copy_to_user(uaddr, &stat, sizeof(stat)))
		return -EFAULT;
	return 0;
}

static int raw1394_iso_get_status(struct file_info *fi, void *uaddr)
{
	struct raw1394_iso_status stat;
	struct hpsb_iso *iso = fi->iso_handle;

	raw1394_iso_fill_status(fi->iso_handle, &stat);
	if (copy_to_user(uaddr, &stat, sizeof(stat)))
		return -EFAULT;

	/* reset overflow counter */
	atomic_set(&iso->overflows, 0);

	return 0;
}

/* copy N packet_infos out of the ringbuffer into user-supplied array */
static int raw1394_iso_recv_packets(struct file_info *fi, void *uaddr)
{
	struct raw1394_iso_packets upackets;
	unsigned int packet = fi->iso_handle->first_packet;
	int i;

	if (copy_from_user(&upackets, uaddr, sizeof(upackets)))
		return -EFAULT;

	if (upackets.n_packets > hpsb_iso_n_ready(fi->iso_handle))
		return -EINVAL;

	/* ensure user-supplied buffer is accessible and big enough */
	if (verify_area(VERIFY_WRITE, upackets.infos,
		       upackets.n_packets * sizeof(struct raw1394_iso_packet_info)))
		return -EFAULT;

	/* copy the packet_infos out */
	for (i = 0; i < upackets.n_packets; i++) {
		if (__copy_to_user(&upackets.infos[i],
				  &fi->iso_handle->infos[packet],
				  sizeof(struct raw1394_iso_packet_info)))
			return -EFAULT;
		
		packet = (packet + 1) % fi->iso_handle->buf_packets;
	}

	return 0;
}

/* copy N packet_infos from user to ringbuffer, and queue them for transmission */
static int raw1394_iso_send_packets(struct file_info *fi, void *uaddr)
{
	struct raw1394_iso_packets upackets;
	int i, rv;

	if (copy_from_user(&upackets, uaddr, sizeof(upackets)))
		return -EFAULT;

	if (upackets.n_packets > hpsb_iso_n_ready(fi->iso_handle))
		return -EINVAL;

	/* ensure user-supplied buffer is accessible and big enough */
	if (verify_area(VERIFY_READ, upackets.infos,
		       upackets.n_packets * sizeof(struct raw1394_iso_packet_info)))
		return -EFAULT;

	/* copy the infos structs in and queue the packets */
	for (i = 0; i < upackets.n_packets; i++) {
		struct raw1394_iso_packet_info info;

		if (__copy_from_user(&info, &upackets.infos[i],
				    sizeof(struct raw1394_iso_packet_info)))
			return -EFAULT;

		rv = hpsb_iso_xmit_queue_packet(fi->iso_handle, info.offset,
						info.len, info.tag, info.sy);
		if (rv)
			return rv;
	}

	return 0;
}

static void raw1394_iso_shutdown(struct file_info *fi)
{
	if (fi->iso_handle)
		hpsb_iso_shutdown(fi->iso_handle);

	fi->iso_handle = NULL;
	fi->iso_state = RAW1394_ISO_INACTIVE;
}

/* mmap the rawiso xmit/recv buffer */
static int raw1394_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct file_info *fi = file->private_data;

	if (fi->iso_state == RAW1394_ISO_INACTIVE)
		return -EINVAL;

	return dma_region_mmap(&fi->iso_handle->data_buf, file, vma);
}

/* ioctl is only used for rawiso operations */
static int raw1394_ioctl(struct inode *inode, struct file *file, unsigned int cmd, unsigned long arg)
{
	struct file_info *fi = file->private_data;

	switch(fi->iso_state) {
	case RAW1394_ISO_INACTIVE:
		switch(cmd) {
		case RAW1394_IOC_ISO_XMIT_INIT:
			return raw1394_iso_xmit_init(fi, (void*) arg);
		case RAW1394_IOC_ISO_RECV_INIT:
			return raw1394_iso_recv_init(fi, (void*) arg);
		default:
			break;
		}
		break;
	case RAW1394_ISO_RECV:
		switch(cmd) {
		case RAW1394_IOC_ISO_RECV_START: {
			/* copy args from user-space */
			int args[3];
			if (copy_from_user(&args[0], (void*) arg, sizeof(args)))
				return -EFAULT;
			return hpsb_iso_recv_start(fi->iso_handle, args[0], args[1], args[2]);
		}
		case RAW1394_IOC_ISO_XMIT_RECV_STOP:
			hpsb_iso_stop(fi->iso_handle);
			return 0;
		case RAW1394_IOC_ISO_RECV_LISTEN_CHANNEL:
			return hpsb_iso_recv_listen_channel(fi->iso_handle, arg);
		case RAW1394_IOC_ISO_RECV_UNLISTEN_CHANNEL:
			return hpsb_iso_recv_unlisten_channel(fi->iso_handle, arg);
		case RAW1394_IOC_ISO_RECV_SET_CHANNEL_MASK: {
			/* copy the u64 from user-space */
			u64 mask;
			if (copy_from_user(&mask, (void*) arg, sizeof(mask)))
				return -EFAULT;
			return hpsb_iso_recv_set_channel_mask(fi->iso_handle, mask);
		}
		case RAW1394_IOC_ISO_GET_STATUS:
			return raw1394_iso_get_status(fi, (void*) arg);
		case RAW1394_IOC_ISO_RECV_PACKETS:
			return raw1394_iso_recv_packets(fi, (void*) arg);
		case RAW1394_IOC_ISO_RECV_RELEASE_PACKETS:
			return hpsb_iso_recv_release_packets(fi->iso_handle, arg);
		case RAW1394_IOC_ISO_RECV_FLUSH:
			return hpsb_iso_recv_flush(fi->iso_handle);
		case RAW1394_IOC_ISO_SHUTDOWN:
			raw1394_iso_shutdown(fi);
			return 0;
		case RAW1394_IOC_ISO_QUEUE_ACTIVITY:
			queue_rawiso_event(fi);
			return 0;
		}
		break;
	case RAW1394_ISO_XMIT:
		switch(cmd) {
		case RAW1394_IOC_ISO_XMIT_START: {
			/* copy two ints from user-space */
			int args[2];
			if (copy_from_user(&args[0], (void*) arg, sizeof(args)))
				return -EFAULT;
			return hpsb_iso_xmit_start(fi->iso_handle, args[0], args[1]);
		}
		case RAW1394_IOC_ISO_XMIT_SYNC:
			return hpsb_iso_xmit_sync(fi->iso_handle);
		case RAW1394_IOC_ISO_XMIT_RECV_STOP:
			hpsb_iso_stop(fi->iso_handle);
			return 0;
		case RAW1394_IOC_ISO_GET_STATUS:
			return raw1394_iso_get_status(fi, (void*) arg);
		case RAW1394_IOC_ISO_XMIT_PACKETS:
			return raw1394_iso_send_packets(fi, (void*) arg);
		case RAW1394_IOC_ISO_SHUTDOWN:
			raw1394_iso_shutdown(fi);
			return 0;
		case RAW1394_IOC_ISO_QUEUE_ACTIVITY:
			queue_rawiso_event(fi);
			return 0;
		}
		break;
	default:
		break;
	}

	return -EINVAL;
}

static unsigned int raw1394_poll(struct file *file, poll_table *pt)
{
        struct file_info *fi = file->private_data;
        unsigned int mask = POLLOUT | POLLWRNORM;

        poll_wait(file, &fi->poll_wait_complete, pt);

        spin_lock_irq(&fi->reqlists_lock);
        if (!list_empty(&fi->req_complete)) {
                mask |= POLLIN | POLLRDNORM;
        }
        spin_unlock_irq(&fi->reqlists_lock);

        return mask;
}

static int raw1394_open(struct inode *inode, struct file *file)
{
        struct file_info *fi;

        if (ieee1394_file_to_instance(file) > 0) {
                return -ENXIO;
        }

        fi = kmalloc(sizeof(struct file_info), SLAB_KERNEL);
        if (fi == NULL)
                return -ENOMEM;
        
        memset(fi, 0, sizeof(struct file_info));
        fi->notification = (u8) RAW1394_NOTIFY_ON; /* busreset notification */

        INIT_LIST_HEAD(&fi->list);
        fi->state = opened;
        INIT_LIST_HEAD(&fi->req_pending);
        INIT_LIST_HEAD(&fi->req_complete);
        sema_init(&fi->complete_sem, 0);
        spin_lock_init(&fi->reqlists_lock);
        init_waitqueue_head(&fi->poll_wait_complete);
        INIT_LIST_HEAD(&fi->addr_list);

        file->private_data = fi;

        return 0;
}

static int raw1394_release(struct inode *inode, struct file *file)
{
        struct file_info *fi = file->private_data;
        struct list_head *lh;
        struct pending_request *req;
        int done = 0, i, fail = 0;
        int retval = 0;
        struct list_head *entry;
        struct arm_addr  *addr = NULL;
        struct list_head *lh_1, *lh_2;
        struct host_info *hi;
        struct file_info *fi_hlp = NULL;
        struct arm_addr  *arm_addr = NULL;
        int another_host;

	if (fi->iso_state != RAW1394_ISO_INACTIVE)
		raw1394_iso_shutdown(fi);

        for (i = 0; i < 64; i++) {
                if (fi->listen_channels & (1ULL << i)) {
                        hpsb_unlisten_channel(&raw1394_highlevel, fi->host, i);
                }
        }

        spin_lock_irq(&host_info_lock);
        fi->listen_channels = 0;
        spin_unlock_irq(&host_info_lock);

        fail = 0;
        /* set address-entries invalid */
        spin_lock_irq(&host_info_lock);

        while (!list_empty(&fi->addr_list)) {
                another_host = 0;
                lh = fi->addr_list.next;
                addr = list_entry(lh, struct arm_addr, addr_list);
                /* another host with valid address-entry containing 
                   same addressrange? */
                list_for_each(lh_1, &host_info_list) {
                        hi = list_entry(lh_1, struct host_info, list);
                        if (hi->host != fi->host) {
                                list_for_each(lh_2, &hi->file_info_list) {
                                        fi_hlp = list_entry(lh_2, struct file_info, list);
                                        entry = fi_hlp->addr_list.next;
                                        while (entry != &(fi_hlp->addr_list)) {
                                                arm_addr = list_entry(entry, 
                                                        struct arm_addr, addr_list);
                                                if (arm_addr->start == 
                                                        addr->start) {
                                                        DBGMSG("raw1394_release: "
                                                        "another host ownes "
                                                        "same addressrange");
                                                        another_host = 1;
                                                        break;
                                                }
                                                entry = entry->next;
                                        }
                                        if (another_host) {
                                                break;
                                        }
                                }
                        }
                }
                if (!another_host) {
                        DBGMSG("raw1394_release: call hpsb_arm_unregister");
                        retval = hpsb_unregister_addrspace(&raw1394_highlevel, addr->start);
                        if (!retval) {
                                ++fail;
                                printk(KERN_ERR "raw1394_release arm_Unregister failed\n");
                        }
                }
                DBGMSG("raw1394_release: delete addr_entry from list");
                list_del(&addr->addr_list);
                vfree(addr->addr_space_buffer);
                kfree(addr);
        } /* while */
        spin_unlock_irq(&host_info_lock);
        if (fail > 0) {
                printk(KERN_ERR "raw1394: during addr_list-release "
                        "error(s) occurred \n");
        }

        while (!done) {
                spin_lock_irq(&fi->reqlists_lock);

                while (!list_empty(&fi->req_complete)) {
                        lh = fi->req_complete.next;
                        list_del(lh);

                        req = list_entry(lh, struct pending_request, list);

                        free_pending_request(req);
                }

                if (list_empty(&fi->req_pending)) done = 1;

                spin_unlock_irq(&fi->reqlists_lock);

                if (!done) down_interruptible(&fi->complete_sem);
        }

        if (fi->state == connected) {
                spin_lock_irq(&host_info_lock);
                list_del(&fi->list);
                spin_unlock_irq(&host_info_lock);

                hpsb_unref_host(fi->host);
        }

        kfree(fi);

        return 0;
}


/*** HOTPLUG STUFF **********************************************************/
/*
 * Export information about protocols/devices supported by this driver.
 */
static struct ieee1394_device_id raw1394_id_table[] = {
	{
		.match_flags	= IEEE1394_MATCH_SPECIFIER_ID | IEEE1394_MATCH_VERSION,
		.specifier_id	= AVC_UNIT_SPEC_ID_ENTRY & 0xffffff,
		.version	= AVC_SW_VERSION_ENTRY & 0xffffff
	},
	{
		.match_flags	= IEEE1394_MATCH_SPECIFIER_ID | IEEE1394_MATCH_VERSION,
		.specifier_id	= CAMERA_UNIT_SPEC_ID_ENTRY & 0xffffff,
		.version	= CAMERA_SW_VERSION_ENTRY & 0xffffff
	},
	{ }
};

MODULE_DEVICE_TABLE(ieee1394, raw1394_id_table);

static struct hpsb_protocol_driver raw1394_driver = {
	.name =		"raw1394 Driver",
	.id_table = 	raw1394_id_table,
};


/******************************************************************************/


static struct hpsb_highlevel raw1394_highlevel = {
	.name =		RAW1394_DEVICE_NAME,
        .add_host =    add_host,
        .remove_host = remove_host,
        .host_reset =  host_reset,
        .iso_receive = iso_receive,
        .fcp_request = fcp_request,
};

static const struct file_operations file_ops = {
	.owner =	THIS_MODULE,
        .read =		raw1394_read, 
        .write =	raw1394_write,
	.mmap =         raw1394_mmap,
	.ioctl =        raw1394_ioctl,
        .poll =		raw1394_poll, 
        .open =		raw1394_open, 
        .release =	raw1394_release, 
};

static int __init init_raw1394(void)
{
	hpsb_register_highlevel(&raw1394_highlevel);

        devfs_handle = devfs_register(NULL,
                                      RAW1394_DEVICE_NAME, DEVFS_FL_NONE,
                                      IEEE1394_MAJOR,
                                      IEEE1394_MINOR_BLOCK_RAW1394 * 16,
                                      S_IFCHR | S_IRUSR | S_IWUSR, &file_ops,
                                      NULL);

        if (ieee1394_register_chardev(IEEE1394_MINOR_BLOCK_RAW1394,
                                      THIS_MODULE, &file_ops)) {
                HPSB_ERR("raw1394 failed to register minor device block");
                devfs_unregister(devfs_handle);
                hpsb_unregister_highlevel(&raw1394_highlevel);
                return -EBUSY;
        }

        printk(KERN_INFO "raw1394: /dev/%s device initialized\n", RAW1394_DEVICE_NAME);

	hpsb_register_protocol(&raw1394_driver);

        return 0;
}

static void __exit cleanup_raw1394(void)
{
	hpsb_unregister_protocol(&raw1394_driver);
        ieee1394_unregister_chardev(IEEE1394_MINOR_BLOCK_RAW1394);
        devfs_unregister(devfs_handle);
        hpsb_unregister_highlevel(&raw1394_highlevel);
}

module_init(init_raw1394);
module_exit(cleanup_raw1394);
MODULE_LICENSE("GPL");
