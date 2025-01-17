#ifndef _SHFS_DEBUG_H
#define _SHFS_DEBUG_H

#define ENABLE_DEBUG

#define SHFS_VERBOSE 1
#define SHFS_ALLOC 2
#define SHFS_DEBUG 3

extern int debug_level;

#ifdef ENABLE_DEBUG

#define DEBUG(x...) do { if (debug_level >= SHFS_DEBUG) { printk(KERN_DEBUG "%s: ", __FUNCTION__); printk(x); } } while (0)

#define VERBOSE(x...) do { if (debug_level >= SHFS_VERBOSE) { printk(KERN_NOTICE "%s: ", __FUNCTION__); printk(x); } } while (0)

#include <linux/slab.h>
extern unsigned long alloc;

static inline void *
__kmem_malloc_debug(char *s, kmem_cache_t *cache, int flags)
{
	if (debug_level >= SHFS_ALLOC) {
		void *x = kmem_cache_alloc(cache, flags);
		alloc += (unsigned long) x;
		VERBOSE("alloc (%s): %p\n", s, x);
		return x;
	} else {
		return kmem_cache_alloc(cache, flags);
	}
}

static inline void
__kmem_free_debug(char *s, kmem_cache_t *cache, void *p)
{
	if (debug_level >= SHFS_ALLOC) {
		VERBOSE("free (%s): %p\n", s, p);
		alloc -= (unsigned long) p;
	}
	kmem_cache_free(cache, p);
}

#define KMEM_ALLOC(s, cache, flags) __kmem_malloc_debug(s, cache, flags);
#define KMEM_FREE(s, cache, p) __kmem_free_debug(s, cache, p);

#else

#define DEBUG(x...) ;
#define VERBOSE(x...) ;

#define KMEM_ALLOC(s, cache, flags) kmem_cache_alloc(cache, flags)
#define KMEM_FREE(s, cache, p) kmem_cache_free(cache, p)

#endif

#endif	/* _SHFS_DEBUG_H */
