/*
 *  arch/s390/kernel/debug.c
 *   S/390 debug facility
 *
 *    Copyright (C) 1999, 2000 IBM Deutschland Entwicklung GmbH,
 *                             IBM Corporation
 *    Author(s): Michael Holzheu (holzheu@de.ibm.com),
 *               Holger Smolinski (Holger.Smolinski@de.ibm.com)
 *
 *    Bugreports to: <Linux390@de.ibm.com>
 */

#include <linux/config.h>
#include <linux/stddef.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/ctype.h>
#include <linux/version.h>
#include <asm/uaccess.h>
#include <asm/semaphore.h>

#include <linux/module.h>

#include <asm/debug.h>

#define MIN(a,b) (((a)<(b))?(a):(b))
#define DEBUG_PROLOG_ENTRY -1

/* typedefs */

typedef struct file_private_info {
	loff_t offset;			/* offset of last read in file */
	int    act_area;                /* number of last formated area */
	int    act_entry;               /* last formated entry (offset */
                                        /* relative to beginning of last */
                                        /* formated area) */ 
	size_t act_entry_offset;        /* up to this offset we copied */
					/* in last read the last formated */
					/* entry to userland */
	char   temp_buf[2048];		/* buffer for output */
	debug_info_t *debug_info_org;   /* original debug information */
	debug_info_t *debug_info_snap;	/* snapshot of debug information */
	struct debug_view *view;	/* used view of debug info */
} file_private_info_t;

typedef struct
{
	char *string;
	/* 
	 * This assumes that all args are converted into longs 
	 * on L/390 this is the case for all types of parameter 
	 * except of floats, and long long (32 bit) 
         *
         */
	long args[0];
} debug_sprintf_entry;


extern void tod_to_timeval(uint64_t todval, struct timeval *xtime);

/* internal function prototyes */

static int debug_init(void);
static ssize_t debug_output(struct file *file, char *user_buf,
			    size_t user_len, loff_t * offset);
static ssize_t debug_input(struct file *file, const char *user_buf,
			   size_t user_len, loff_t * offset);
static int debug_open(struct inode *inode, struct file *file);
static int debug_close(struct inode *inode, struct file *file);
static struct proc_dir_entry 
*debug_create_proc_dir_entry(struct proc_dir_entry *root,
			     const char *name, mode_t mode,
			     struct inode_operations *iops,
			     struct file_operations *fops);
static void debug_delete_proc_dir_entry(struct proc_dir_entry *root,
					struct proc_dir_entry *entry);
static debug_info_t*  debug_info_create(char *name, int page_order, int nr_areas, int buf_size);
static void debug_info_get(debug_info_t *);
static void debug_info_put(debug_info_t *);
static int debug_prolog_level_fn(debug_info_t * id,
				 struct debug_view *view, char *out_buf);
static int debug_input_level_fn(debug_info_t * id, struct debug_view *view,
				struct file *file, const char *user_buf,
				size_t user_buf_size, loff_t * offset);
static int debug_input_flush_fn(debug_info_t * id, struct debug_view *view,
                                struct file *file, const char *user_buf,
                                size_t user_buf_size, loff_t * offset);
static int debug_hex_ascii_format_fn(debug_info_t * id, struct debug_view *view,
                                char *out_buf, const char *in_buf);
static int debug_raw_format_fn(debug_info_t * id,
				 struct debug_view *view, char *out_buf,
				 const char *in_buf);
static int debug_raw_header_fn(debug_info_t * id, struct debug_view *view,
                         int area, debug_entry_t * entry, char *out_buf);

static int debug_sprintf_format_fn(debug_info_t * id, struct debug_view *view,
				   char *out_buf, debug_sprintf_entry *curr_event);

/* globals */

struct debug_view debug_raw_view = {
	"raw",
	NULL,
	&debug_raw_header_fn,
	&debug_raw_format_fn,
	NULL,
	NULL
};

struct debug_view debug_hex_ascii_view = {
	"hex_ascii",
	NULL,
	&debug_dflt_header_fn,
	&debug_hex_ascii_format_fn,
	NULL,
	NULL
};

struct debug_view debug_level_view = {
	"level",
	&debug_prolog_level_fn,
	NULL,
	NULL,
	&debug_input_level_fn,
	NULL
};

struct debug_view debug_flush_view = {
        "flush",
        NULL,
        NULL,
        NULL,
        &debug_input_flush_fn,
        NULL
};

struct debug_view debug_sprintf_view = {
	"sprintf",
	NULL,
	&debug_dflt_header_fn,
	(debug_format_proc_t*)&debug_sprintf_format_fn,
	NULL,
	NULL
};


unsigned int debug_feature_version = __DEBUG_FEATURE_VERSION;

/* static globals */

static debug_info_t *debug_area_first = NULL;
static debug_info_t *debug_area_last = NULL;
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,3,98))
static struct semaphore debug_lock = MUTEX;
#else
DECLARE_MUTEX(debug_lock);
#endif

static int initialized = 0;

static const struct file_operations debug_file_ops = {
	read:    debug_output,
	write:   debug_input,	
	open:    debug_open,
	release: debug_close,
};

static const struct inode_operations debug_inode_ops = {
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,3,98))
	default_file_ops: &debug_file_ops,	/* file ops */
#endif
};


static struct proc_dir_entry *debug_proc_root_entry;

/* functions */

/*
 * debug_info_alloc
 * - alloc new debug-info
 */

static debug_info_t*  debug_info_alloc(char *name, int page_order,
                                        int nr_areas, int buf_size)
{
	debug_info_t* rc;
	int i;

	/* alloc everything */

	rc = (debug_info_t*) kmalloc(sizeof(debug_info_t), GFP_ATOMIC);
	if(!rc)
		goto fail_malloc_rc;
	rc->active_entry = (int*)kmalloc(nr_areas * sizeof(int), GFP_ATOMIC);
	if(!rc->active_entry)
		goto fail_malloc_active_entry;
	memset(rc->active_entry, 0, nr_areas * sizeof(int));
	rc->areas = (debug_entry_t **) kmalloc(nr_areas *
						sizeof(debug_entry_t *),
						GFP_ATOMIC);
	if (!rc->areas)
		goto fail_malloc_areas;
	for (i = 0; i < nr_areas; i++) {
		rc->areas[i] = (debug_entry_t *) __get_free_pages(GFP_ATOMIC,
								page_order);
		if (!rc->areas[i]) {
			for (i--; i >= 0; i--) {
				free_pages((unsigned long) rc->areas[i],
						page_order);
			}
			goto fail_malloc_areas2;
		} else {
			memset(rc->areas[i], 0, PAGE_SIZE << page_order);
		}
	}

	/* initialize members */

	spin_lock_init(&rc->lock);
	rc->page_order  = page_order;
	rc->nr_areas    = nr_areas;
	rc->active_area = 0;
	rc->level       = DEBUG_DEFAULT_LEVEL;
	rc->buf_size    = buf_size;
	rc->entry_size  = sizeof(debug_entry_t) + buf_size;
	strncpy(rc->name, name, MIN(strlen(name), (DEBUG_MAX_PROCF_LEN - 1)));
	rc->name[MIN(strlen(name), (DEBUG_MAX_PROCF_LEN - 1))] = 0;
	memset(rc->views, 0, DEBUG_MAX_VIEWS * sizeof(struct debug_view *));
#ifdef CONFIG_PROC_FS
	memset(rc->proc_entries, 0 ,DEBUG_MAX_VIEWS *
		sizeof(struct proc_dir_entry*));
#endif /* CONFIG_PROC_FS */
	atomic_set(&(rc->ref_count), 0);

	return rc;

fail_malloc_areas2:
	kfree(rc->areas);
fail_malloc_areas:
	kfree(rc->active_entry);
fail_malloc_active_entry:
	kfree(rc);
fail_malloc_rc:
	return NULL;
}

/*
 * debug_info_free
 * - free memory debug-info
 */

static void debug_info_free(debug_info_t* db_info){
	int i;
	for (i = 0; i < db_info->nr_areas; i++) {
		free_pages((unsigned long) db_info->areas[i],
		db_info->page_order);
	}
	kfree(db_info->areas);
	kfree(db_info->active_entry);
	kfree(db_info);
}

/*
 * debug_info_create
 * - create new debug-info
 */

static debug_info_t*  debug_info_create(char *name, int page_order, 
                                        int nr_areas, int buf_size)
{
	debug_info_t* rc;

        rc = debug_info_alloc(name, page_order, nr_areas, buf_size);
        if(!rc) 
		goto out;


	/* create proc rood directory */

        rc->proc_root_entry =
            debug_create_proc_dir_entry(debug_proc_root_entry, rc->name,
                                        S_IFDIR | S_IRUGO | S_IXUGO |
                                        S_IWUSR | S_IWGRP, NULL, NULL);

	/* append new element to linked list */

        if(debug_area_first == NULL){
                /* first element in list */
                debug_area_first = rc;
                rc->prev = NULL;
        }
        else{
                /* append element to end of list */
                debug_area_last->next = rc;
                rc->prev = debug_area_last;
        }
        debug_area_last = rc;
        rc->next = NULL;

	debug_info_get(rc);
out:
	return rc;
}

/*
 * debug_info_copy
 * - copy debug-info
 */

static debug_info_t* debug_info_copy(debug_info_t* in)
{
        int i;
        debug_info_t* rc;
        rc = debug_info_alloc(in->name, in->page_order, 
                                in->nr_areas, in->buf_size);
        if(!rc)
                goto out;

        for(i = 0; i < in->nr_areas; i++){
                memcpy(rc->areas[i],in->areas[i], PAGE_SIZE << in->page_order);
        }
out:
        return rc;
}

/*
 * debug_info_get
 * - increments reference count for debug-info
 */

static void debug_info_get(debug_info_t * db_info)
{
	if (db_info)
		atomic_inc(&db_info->ref_count);
}

/*
 * debug_info_put:
 * - decreases reference count for debug-info and frees it if necessary
 */

static void debug_info_put(debug_info_t *db_info)
{
	int i;

	if (!db_info)
		return;
	if (atomic_dec_and_test(&db_info->ref_count)) {
#ifdef DEBUG
		printk(KERN_INFO "debug: freeing debug area %p (%s)\n",
		       db_info, db_info->name);
#endif
		for (i = 0; i < DEBUG_MAX_VIEWS; i++) {
			if (db_info->views[i] != NULL)
				debug_delete_proc_dir_entry
				    (db_info->proc_root_entry,
				     db_info->proc_entries[i]);
		}
		debug_delete_proc_dir_entry(debug_proc_root_entry,
					    db_info->proc_root_entry);
		if(db_info == debug_area_first)
			debug_area_first = db_info->next;
		if(db_info == debug_area_last)
			debug_area_last = db_info->prev;
		if(db_info->prev) db_info->prev->next = db_info->next;
		if(db_info->next) db_info->next->prev = db_info->prev;
		debug_info_free(db_info);
	}
}

/*
 * debug_format_entry:
 * - format one debug entry and return size of formated data
 */

static int debug_format_entry(file_private_info_t *p_info)
{
	debug_info_t *id_org    = p_info->debug_info_org;
	debug_info_t *id_snap   = p_info->debug_info_snap;
	struct debug_view *view = p_info->view;
	debug_entry_t *act_entry;
	size_t len = 0;
	if(p_info->act_entry == DEBUG_PROLOG_ENTRY){
		/* print prolog */
        	if (view->prolog_proc)
                	len += view->prolog_proc(id_org, view,p_info->temp_buf);
		goto out;
	}

	act_entry = (debug_entry_t *) ((char*)id_snap->areas[p_info->act_area] +
					p_info->act_entry);
                        
	if (act_entry->id.stck == 0LL)
			goto out;  /* empty entry */
	if (view->header_proc)
		len += view->header_proc(id_org, view, p_info->act_area, 
					act_entry, p_info->temp_buf + len);
	if (view->format_proc)
		len += view->format_proc(id_org, view, p_info->temp_buf + len,
						DEBUG_DATA(act_entry));
      out:
        return len;
}

/*
 * debug_next_entry:
 * - goto next entry in p_info
 */

extern inline int debug_next_entry(file_private_info_t *p_info)
{
	debug_info_t *id = p_info->debug_info_snap;
	if(p_info->act_entry == DEBUG_PROLOG_ENTRY){
		p_info->act_entry = 0;
		goto out;
	}
	if ((p_info->act_entry += id->entry_size)
		> ((PAGE_SIZE << (id->page_order)) 
		- id->entry_size)){

		/* next area */
		p_info->act_entry = 0;
        	p_info->act_area++;
        	if(p_info->act_area >= id->nr_areas)
			return 1;
	}
out:
	return 0;	
}

/*
 * debug_output:
 * - called for user read()
 * - copies formated debug entries to the user buffer
 */

static ssize_t debug_output(struct file *file,	/* file descriptor */
			    char *user_buf,	/* user buffer */
			    size_t  len,	/* length of buffer */
			    loff_t *offset	/* offset in the file */ )
{
	size_t count = 0;
	size_t entry_offset, size = 0;
	int rc;
	file_private_info_t *p_info;

	p_info = ((file_private_info_t *) file->private_data);
	if (*offset != p_info->offset) 
		return -EPIPE;
	if(p_info->act_area >= p_info->debug_info_snap->nr_areas)
		return 0;

	entry_offset = p_info->act_entry_offset;

	while(count < len){
		size = debug_format_entry(p_info);
		size = MIN((len - count), (size - entry_offset));

		if(size){
			if (copy_to_user(user_buf + count, 
					p_info->temp_buf + entry_offset, size))
				return -EFAULT;
		}
		count += size;
		entry_offset = 0;
		if(count != len)
			if(debug_next_entry(p_info)) 
				goto out;
	}
out:
	p_info->offset           += count;
	p_info->act_entry_offset = size;	
	*offset = p_info->offset;
	return count;
}

/*
 * debug_input:
 * - called for user write()
 * - calls input function of view
 */

static ssize_t debug_input(struct file *file,
			   const char *user_buf, size_t length,
			   loff_t *offset)
{
	int rc = 0;
	file_private_info_t *p_info;

	down(&debug_lock);
	p_info = ((file_private_info_t *) file->private_data);
	if (p_info->view->input_proc)
		rc = p_info->view->input_proc(p_info->debug_info_org,
					      p_info->view, file, user_buf,
					      length, offset);
	else
		rc = -EPERM;
	up(&debug_lock);
	return rc;		/* number of input characters */
}

/*
 * debug_open:
 * - called for user open()
 * - copies formated output to private_data area of the file
 *   handle
 */

static int debug_open(struct inode *inode, struct file *file)
{
	int i = 0, rc = 0;
	file_private_info_t *p_info;
	debug_info_t *debug_info, *debug_info_snapshot;

#ifdef DEBUG
	printk("debug_open\n");
#endif
	MOD_INC_USE_COUNT;
	down(&debug_lock);

	/* find debug log and view */

	debug_info = debug_area_first;
	while(debug_info != NULL){
		for (i = 0; i < DEBUG_MAX_VIEWS; i++) {
			if (debug_info->views[i] == NULL)
				continue;
			else if (debug_info->proc_entries[i]->low_ino ==
				 file->f_dentry->d_inode->i_ino) {
				goto found;	/* found view ! */
			}
		}
		debug_info = debug_info->next;
	}
	/* no entry found */
	rc = -EINVAL;
	goto out;

      found:

	/* make snapshot of current debug areas to get it consistent */

	debug_info_snapshot = debug_info_copy(debug_info);

	if(!debug_info_snapshot){
#ifdef DEBUG
		printk(KERN_ERR "debug_open: debug_info_copy failed (out of mem)\n");
#endif
		rc = -ENOMEM;
		goto out;
	}

	if ((file->private_data =
	     kmalloc(sizeof(file_private_info_t), GFP_ATOMIC)) == 0) {
#ifdef DEBUG
		printk(KERN_ERR "debug_open: kmalloc failed\n");
#endif
		debug_info_free(debug_info_snapshot);	
		rc = -ENOMEM;
		goto out;
	}
	p_info = (file_private_info_t *) file->private_data;
	p_info->offset = 0;
	p_info->debug_info_snap = debug_info_snapshot;
	p_info->debug_info_org  = debug_info;
	p_info->view = debug_info->views[i];
	p_info->act_area = 0;
	p_info->act_entry = DEBUG_PROLOG_ENTRY;
	p_info->act_entry_offset = 0;

	debug_info_get(debug_info);

      out:
	up(&debug_lock);
	if (rc != 0)
		MOD_DEC_USE_COUNT;
	return rc;
}

/*
 * debug_close:
 * - called for user close()
 * - deletes  private_data area of the file handle
 */

static int debug_close(struct inode *inode, struct file *file)
{
	file_private_info_t *p_info;
#ifdef DEBUG
	printk("debug_close\n");
#endif
	p_info = (file_private_info_t *) file->private_data;
	debug_info_free(p_info->debug_info_snap);
	debug_info_put(p_info->debug_info_org);
	kfree(file->private_data);
	MOD_DEC_USE_COUNT;
	return 0;		/* success */
}

/*
 * debug_create_proc_dir_entry:
 * - initializes proc-dir-entry and registers it
 */

static struct proc_dir_entry *debug_create_proc_dir_entry
    (struct proc_dir_entry *root, const char *name, mode_t mode,
     struct inode_operations *iops, struct file_operations *fops) 
{
	struct proc_dir_entry *rc = NULL;

#ifdef CONFIG_PROC_FS
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,3,98))
	const char *fn = name;
	int len;
	len = strlen(fn);

	rc = (struct proc_dir_entry *) kmalloc(sizeof(struct proc_dir_entry)
					       + len + 1, GFP_ATOMIC);
	if (!rc)
		goto out;

	memset(rc, 0, sizeof(struct proc_dir_entry));
	memcpy(((char *) rc) + sizeof(*rc), fn, len + 1);
	rc->name = ((char *) rc) + sizeof(*rc);
	rc->namelen = len;
	rc->low_ino = 0, rc->mode = mode;
	rc->nlink = 1;
	rc->uid = 0;
	rc->gid = 0;
	rc->size = 0;
	rc->get_info = NULL;
	rc->ops = iops;

	proc_register(root, rc);
#else
	rc = create_proc_entry(name, mode, root);
	if (!rc)
		goto out;
	if (fops)
		rc->proc_fops = fops;
#endif

      out:
#endif /* CONFIG_PROC_FS */
	return rc;
}


/*
 * delete_proc_dir_entry:
 */

static void debug_delete_proc_dir_entry
    (struct proc_dir_entry *root, struct proc_dir_entry *proc_entry) 
{

#ifdef CONFIG_PROC_FS
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,3,98))
	proc_unregister(root, proc_entry->low_ino);
	kfree(proc_entry);
#else
	remove_proc_entry(proc_entry->name, root);
#endif
#endif /* CONFIG_PROC_FS */
}

/*
 * debug_register:
 * - creates and initializes debug area for the caller
 * - returns handle for debug area
 */

debug_info_t *debug_register
    (char *name, int page_order, int nr_areas, int buf_size) 
{
	debug_info_t *rc = NULL;

	MOD_INC_USE_COUNT;
	if (!initialized)
		debug_init();
	down(&debug_lock);

        /* create new debug_info */

	rc = debug_info_create(name, page_order, nr_areas, buf_size);
	if(!rc) 
		goto out;
	debug_register_view(rc, &debug_level_view);
        debug_register_view(rc, &debug_flush_view);
#ifdef DEBUG
	printk(KERN_INFO
	       "debug: reserved %d areas of %d pages for debugging %s\n",
	       nr_areas, 1 << page_order, rc->name);
#endif
      out:
        if (rc == NULL){
		printk(KERN_ERR "debug: debug_register failed for %s\n",name);
		MOD_DEC_USE_COUNT;
        }
	up(&debug_lock);
	return rc;
}

/*
 * debug_unregister:
 * - give back debug area
 */

void debug_unregister(debug_info_t * id)
{
	if (!id)
		goto out;
	down(&debug_lock);
#ifdef DEBUG
	printk(KERN_INFO "debug: unregistering %s\n", id->name);
#endif
	debug_info_put(id);
	up(&debug_lock);

	MOD_DEC_USE_COUNT;
      out:
	return;
}

/*
 * debug_set_level:
 * - set actual debug level
 */

void debug_set_level(debug_info_t* id, int new_level)
{
	unsigned long flags;
	if(!id)
		return;	
	spin_lock_irqsave(&id->lock,flags);
        if(new_level == DEBUG_OFF_LEVEL){
                id->level = DEBUG_OFF_LEVEL;
                printk(KERN_INFO "debug: %s: switched off\n",id->name);
        } else if ((new_level > DEBUG_MAX_LEVEL) || (new_level < 0)) {
                printk(KERN_INFO
                        "debug: %s: level %i is out of range (%i - %i)\n",
                        id->name, new_level, 0, DEBUG_MAX_LEVEL);
        } else {
                id->level = new_level;
#ifdef DEBUG
                printk(KERN_INFO 
			"debug: %s: new level %i\n",id->name,id->level);
#endif
        }
	spin_unlock_irqrestore(&id->lock,flags);
}


/*
 * proceed_active_entry:
 * - set active entry to next in the ring buffer
 */

extern inline void proceed_active_entry(debug_info_t * id)
{
	if ((id->active_entry[id->active_area] += id->entry_size)
	    > ((PAGE_SIZE << (id->page_order)) - id->entry_size))
		id->active_entry[id->active_area] = 0;
}

/*
 * proceed_active_area:
 * - set active area to next in the ring buffer
 */

extern inline void proceed_active_area(debug_info_t * id)
{
	id->active_area++;
	id->active_area = id->active_area % id->nr_areas;
}

/*
 * get_active_entry:
 */

extern inline debug_entry_t *get_active_entry(debug_info_t * id)
{
	return (debug_entry_t *) ((char *) id->areas[id->active_area] +
				  id->active_entry[id->active_area]);
}

/*
 * debug_common:
 * - set timestamp, caller address, cpu number etc.
 */

extern inline debug_entry_t *debug_common(debug_info_t * id, int level, 
                                    const void *buf, int len, int exception)
{
	unsigned long flags;
	debug_entry_t *active;

	spin_lock_irqsave(&id->lock, flags);
	active = get_active_entry(id);
	STCK(active->id.stck);
	active->id.fields.cpuid = smp_processor_id();
	active->caller = __builtin_return_address(0);
	active->id.fields.exception = exception;
	active->id.fields.level     = level;
	memset(DEBUG_DATA(active), 0, id->buf_size);
	memcpy(DEBUG_DATA(active), buf, MIN(len, id->buf_size));
	proceed_active_entry(id);
	if(exception)
		proceed_active_area(id);
	spin_unlock_irqrestore(&id->lock, flags);

	return active;
}

/*
 * debug_event_common:
 * - write debug entry with given size
 */

debug_entry_t *debug_event_common(debug_info_t * id, int level, const void *buf,
			          int len)
{
	return debug_common(id, level, buf, len, 0);
}

/*
 * debug_exception_common:
 * - write debug entry with given size and switch to next debug area
 */

debug_entry_t *debug_exception_common(debug_info_t * id, int level, 
                                      const void *buf, int len)
{
	return debug_common(id, level, buf, len, 1);
}

/*
 * counts arguments in format string for sprintf view
 */

extern inline int debug_count_numargs(char *string)
{
	int numargs=0;

	while(*string) {
		if(*string++=='%')
			numargs++;
	}
	return(numargs);
}

/*
 * debug_sprintf_event:
 */

debug_entry_t *debug_sprintf_event(debug_info_t* id,
                                   int level,char *string,...)
{
	va_list   ap;
	int numargs,alloc_size,idx;
	debug_sprintf_entry *curr_event;
	debug_entry_t *retval = NULL;

	if((!id) || (level > id->level))
		return NULL;
	else {
		numargs=debug_count_numargs(string);
		alloc_size=offsetof(debug_sprintf_entry,args[numargs]);
		curr_event=alloca(alloc_size);

		if(curr_event){
			va_start(ap,string);
			curr_event->string=string;
			for(idx=0;idx<numargs;idx++)
				curr_event->args[idx]=va_arg(ap,long);
			retval=debug_common(id,level, curr_event,alloc_size,0);
			va_end(ap);
		}
		return retval;
	}
}

/*
 * debug_sprintf_exception:
 */

debug_entry_t *debug_sprintf_exception(debug_info_t* id,
                                       int level,char *string,...)
{
	va_list   ap;
	int numargs,alloc_size,idx;
	debug_sprintf_entry *curr_event;
	debug_entry_t *retval = NULL;

	if((!id) || (level > id->level))
		return NULL;
	else {
		numargs=debug_count_numargs(string);
		alloc_size=offsetof(debug_sprintf_entry,args[numargs]);
		curr_event=alloca(alloc_size);

		if(curr_event){
			va_start(ap,string);
			curr_event->string=string;
			for(idx=0;idx<numargs;idx++)
				curr_event->args[idx]=va_arg(ap,long);
			retval=debug_common(id,level, curr_event,alloc_size,1);
			va_end(ap);
		}
		return retval;
	}
}

/*
 * debug_init:
 * - is called exactly once to initialize the debug feature
 */

int debug_init(void)
{
	int rc = 0;

	down(&debug_lock);
	if (!initialized) {
#ifdef CONFIG_PROC_FS
		debug_proc_root_entry =
		    debug_create_proc_dir_entry(&proc_root, DEBUG_DIR_ROOT,
						S_IFDIR | S_IRUGO | S_IXUGO
						| S_IWUSR | S_IWGRP, NULL,
						NULL);
#endif /* CONFIG_PROC_FS */
		printk(KERN_INFO "debug: Initialization complete\n");
		initialized = 1;
	}
	up(&debug_lock);

	return rc;
}

/*
 * debug_register_view:
 */

int debug_register_view(debug_info_t * id, struct debug_view *view)
{
	int rc = 0;
	int i;
	unsigned long flags;
	mode_t mode = S_IFREG;
	struct proc_dir_entry *pde;


	if (!id)
		goto out;
	pde = debug_create_proc_dir_entry(id->proc_root_entry,
					  view->name, mode,
					  &debug_inode_ops,
					  &debug_file_ops);
	if(!pde){
		printk(KERN_WARNING "debug: create_proc_entry() failed! Cannot register view %s/%s\n", id->name,view->name);
		rc = -1;
		goto out;
	}

	spin_lock_irqsave(&id->lock, flags);
	for (i = 0; i < DEBUG_MAX_VIEWS; i++) {
		if (id->views[i] == NULL)
			break;
	}
	if (i == DEBUG_MAX_VIEWS) {
		printk(KERN_WARNING "debug: cannot register view %s/%s\n",
			id->name,view->name);
		printk(KERN_WARNING 
			"debug: maximum number of views reached (%i)!\n", i);
		debug_delete_proc_dir_entry(id->proc_root_entry, pde);
		
		rc = -1;
	}
	else {
		id->views[i] = view;
		if (view->prolog_proc || view->format_proc || view->header_proc)
			mode |= S_IRUSR;
		if (view->input_proc)
			mode |= S_IWUSR;
		id->proc_entries[i] = pde;
		rc = 0;
	}
	spin_unlock_irqrestore(&id->lock, flags);
      out:
	return rc;
}

/*
 * debug_unregister_view:
 */

int debug_unregister_view(debug_info_t * id, struct debug_view *view)
{
	int rc = 0;
	int i;
	unsigned long flags;

	if (!id)
		goto out;
	spin_lock_irqsave(&id->lock, flags);
	for (i = 0; i < DEBUG_MAX_VIEWS; i++) {
		if (id->views[i] == view)
			break;
	}
	if (i == DEBUG_MAX_VIEWS)
		rc = -1;
	else {
		debug_delete_proc_dir_entry(id->proc_root_entry,
					    id->proc_entries[i]);		
		id->views[i] = NULL;
		rc = 0;
	}
	spin_unlock_irqrestore(&id->lock, flags);
      out:
	return rc;
}

/*
 * functions for debug-views
 ***********************************
*/

/*
 * prints out actual debug level
 */

static int debug_prolog_level_fn(debug_info_t * id,
				 struct debug_view *view, char *out_buf)
{
	int rc = 0;

	if(id->level == -1) rc = sprintf(out_buf,"-\n");
	else rc = sprintf(out_buf, "%i\n", id->level);
	return rc;
}

/*
 * reads new debug level
 */

static int debug_input_level_fn(debug_info_t * id, struct debug_view *view,
				struct file *file, const char *user_buf,
				size_t in_buf_size, loff_t * offset)
{
	char input_buf[1];
	int rc = in_buf_size;

	if (*offset != 0)
		goto out;
	if (copy_from_user(input_buf, user_buf, 1)){
		rc = -EFAULT;
		goto out;
	}
	if (isdigit(input_buf[0])) {
		int new_level = ((int) input_buf[0] - (int) '0');
		debug_set_level(id, new_level);
	} else if(input_buf[0] == '-') {
		debug_set_level(id, DEBUG_OFF_LEVEL);
	} else {
		printk(KERN_INFO "debug: level `%c` is not valid\n",
		       input_buf[0]);
	}
      out:
	*offset = in_buf_size;
	return rc;		/* number of input characters */
}


/*
 * flushes debug areas
 */
 
void debug_flush(debug_info_t* id, int area)
{
        unsigned long flags;
        int i;

        if(!id)
                return;
        spin_lock_irqsave(&id->lock,flags);
        if(area == DEBUG_FLUSH_ALL){
                id->active_area = 0;
                memset(id->active_entry, 0, id->nr_areas * sizeof(int));
                for (i = 0; i < id->nr_areas; i++) 
                        memset(id->areas[i], 0, PAGE_SIZE << id->page_order);
                printk(KERN_INFO "debug: %s: all areas flushed\n",id->name);
        } else if(area >= 0 && area < id->nr_areas) {
                id->active_entry[area] = 0;
                memset(id->areas[area], 0, PAGE_SIZE << id->page_order);
                printk(KERN_INFO
                        "debug: %s: area %i has been flushed\n",
                        id->name, area);
        } else {
                printk(KERN_INFO
                        "debug: %s: area %i cannot be flushed (range: %i - %i)\n",
                        id->name, area, 0, id->nr_areas-1);
        }
        spin_unlock_irqrestore(&id->lock,flags);
}

/*
 * view function: flushes debug areas 
 */
 
static int debug_input_flush_fn(debug_info_t * id, struct debug_view *view,
                                struct file *file, const char *user_buf,
                                size_t in_buf_size, loff_t * offset)
{
        char input_buf[1];
        int rc = in_buf_size;
 
        if (*offset != 0)
                goto out;
        if (copy_from_user(input_buf, user_buf, 1)){
                rc = -EFAULT;
                goto out;
        }
        if(input_buf[0] == '-') { 
                debug_flush(id, DEBUG_FLUSH_ALL);
                goto out;
        }
        if (isdigit(input_buf[0])) {
                int area = ((int) input_buf[0] - (int) '0');
                debug_flush(id, area);
                goto out;
        }

        printk(KERN_INFO "debug: area `%c` is not valid\n", input_buf[0]);

      out:
        *offset = in_buf_size;
        return rc;              /* number of input characters */
}

/*
 * prints debug header in raw format
 */

int debug_raw_header_fn(debug_info_t * id, struct debug_view *view,
                         int area, debug_entry_t * entry, char *out_buf)
{
        int rc;

	rc = sizeof(debug_entry_t);
	memcpy(out_buf,entry,sizeof(debug_entry_t));
        return rc;
}

/*
 * prints debug data in raw format
 */

static int debug_raw_format_fn(debug_info_t * id, struct debug_view *view,
			       char *out_buf, const char *in_buf)
{
	int rc;

	rc = id->buf_size;
	memcpy(out_buf, in_buf, id->buf_size);
	return rc;
}

/*
 * prints debug data in hex/ascii format
 */

static int debug_hex_ascii_format_fn(debug_info_t * id, struct debug_view *view,
		    		  char *out_buf, const char *in_buf)
{
	int i, rc = 0;

	for (i = 0; i < id->buf_size; i++) {
                rc += sprintf(out_buf + rc, "%02x ",
                              ((unsigned char *) in_buf)[i]);
        }
	rc += sprintf(out_buf + rc, "| ");
	for (i = 0; i < id->buf_size; i++) {
		unsigned char c = in_buf[i];
		if (!isprint(c))
			rc += sprintf(out_buf + rc, ".");
		else
			rc += sprintf(out_buf + rc, "%c", c);
	}
	rc += sprintf(out_buf + rc, "\n");
	return rc;
}

/*
 * prints header for debug entry
 */

int debug_dflt_header_fn(debug_info_t * id, struct debug_view *view,
			 int area, debug_entry_t * entry, char *out_buf)
{
	struct timeval time_val;
	unsigned long long time;
	char *except_str;
	unsigned long caller;
	int rc = 0;
	unsigned int level;

	level = entry->id.fields.level;
	time = entry->id.stck;
	/* adjust todclock to 1970 */
	time -= 0x8126d60e46000000LL - (0x3c26700LL * 1000000 * 4096);
	tod_to_timeval(time, &time_val);

	if (entry->id.fields.exception)
		except_str = "*";
	else
		except_str = "-";
	caller = (unsigned long) entry->caller;
#if defined(CONFIG_ARCH_S390X)
	rc += sprintf(out_buf, "%02i %011lu:%06lu %1u %1s %02i %016lx  ",
		      area, time_val.tv_sec, time_val.tv_usec, level,
		      except_str, entry->id.fields.cpuid, caller);
#else
	caller &= 0x7fffffff;
	rc += sprintf(out_buf, "%02i %011lu:%06lu %1u %1s %02i %08lx  ",
		      area, time_val.tv_sec, time_val.tv_usec, level,
		      except_str, entry->id.fields.cpuid, caller);
#endif
	return rc;
}

/*
 * prints debug data sprintf-formated:
 * debug_sprinf_event/exception calls must be used together with this view
 */

#define DEBUG_SPRINTF_MAX_ARGS 10

int debug_sprintf_format_fn(debug_info_t * id, struct debug_view *view,
                            char *out_buf, debug_sprintf_entry *curr_event)
{
	int num_longs, num_used_args = 0,i, rc = 0;
	int index[DEBUG_SPRINTF_MAX_ARGS];

	/* count of longs fit into one entry */
	num_longs = id->buf_size /  sizeof(long); 

	if(num_longs < 1)
		goto out; /* bufsize of entry too small */
	if(num_longs == 1) {
		/* no args, we use only the string */
		strcpy(out_buf, curr_event->string);
		rc = strlen(curr_event->string);
		goto out;
	}

	/* number of arguments used for sprintf (without the format string) */
	num_used_args   = MIN(DEBUG_SPRINTF_MAX_ARGS, (num_longs - 1));

	memset(index,0, DEBUG_SPRINTF_MAX_ARGS * sizeof(int));

	for(i = 0; i < num_used_args; i++)
		index[i] = i;

	rc =  sprintf(out_buf, curr_event->string, curr_event->args[index[0]],
		curr_event->args[index[1]], curr_event->args[index[2]],
		curr_event->args[index[3]], curr_event->args[index[4]],
		curr_event->args[index[5]], curr_event->args[index[6]],
		curr_event->args[index[7]], curr_event->args[index[8]],
		curr_event->args[index[9]]);

out:

	return rc;
}

/*
 * init_module:
 */

#ifdef MODULE
int init_module(void)
{
	int rc = 0;
#ifdef DEBUG
	printk("debug_module_init: \n");
#endif
	rc = debug_init();
	if (rc) 
		printk(KERN_INFO "debug: an error occurred with debug_init\n");
	return rc;
}

/*
 * cleanup_module:
 */

void cleanup_module(void)
{
#ifdef DEBUG
	printk("debug_cleanup_module: \n");
#endif
#ifdef CONFIG_PROC_FS
	debug_delete_proc_dir_entry(&proc_root, debug_proc_root_entry);
#endif /* CONFIG_PROC_FS */
	return;
}

#endif			/* MODULE */

EXPORT_SYMBOL(debug_register);
EXPORT_SYMBOL(debug_unregister); 
EXPORT_SYMBOL(debug_set_level);
EXPORT_SYMBOL(debug_register_view);
EXPORT_SYMBOL(debug_unregister_view);
EXPORT_SYMBOL(debug_event_common);
EXPORT_SYMBOL(debug_exception_common);
EXPORT_SYMBOL(debug_hex_ascii_view);
EXPORT_SYMBOL(debug_raw_view);
EXPORT_SYMBOL(debug_dflt_header_fn);
EXPORT_SYMBOL(debug_sprintf_view);
EXPORT_SYMBOL(debug_sprintf_exception);
EXPORT_SYMBOL(debug_sprintf_event);
