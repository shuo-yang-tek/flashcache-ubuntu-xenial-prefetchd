#include <linux/version.h>
#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/semaphore.h>
#include <linux/bio.h>
#include <stdbool.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,26)
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,21)
#include <linux/device-mapper.h>
#include <linux/bio.h>
#endif
#include "dm.h"
#include "dm-io.h"
#include "dm-bio-list.h"
#include "kcopyd.h"
#else
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,27)
#include "dm.h"
#endif
#include <linux/device-mapper.h>
#include <linux/bio.h>
#include <linux/dm-kcopyd.h>
#endif

#include "flashcache.h"
#include "prefetchd_log.h"
#include "pfd_stat.h"
#include "pfd_cache.h"

enum pfd_cache_meta_status {
	empty = 1,
	prepare,
	valid,
};

struct pfd_cache_set;
struct pfd_cache;
struct pfd_cache_meta;

struct pfd_cache_meta {
	struct pfd_cache *cache;

	sector_t dbn;
	enum pfd_cache_meta_status status;
	struct semaphore prepare_lock;
	atomic_t hold_count;

	int ssd_index;
};

struct pfd_cache {
	struct pfd_cache_set *cache_set;
	struct cache_c *dmc;
	struct pfd_cache_meta metas[PFD_CACHE_BLOCK_COUNT];
	void *data;
	spinlock_t lock;
};

enum pfd_cache_set_alloc_status {
	alloc_empty = 1,
	alloc_prepare,
	alloc_active,
};

struct pfd_cache_set {
	int count;
	enum pfd_cache_set_alloc_status status_arr[PFD_CACHE_COUNT_PER_SET];
	struct pfd_cache *caches[PFD_CACHE_COUNT_PER_SET];
	spinlock_t lock;
};

struct pfd_cache_set main_cache_set;

static struct pfd_cache *
find_cache_in_cache_set(
		struct cache_c *dmc,
		struct pfd_cache_set *cache_set) {
	
	struct pfd_cache *found_cache = NULL;
	int i;

	for (i = 0; i < PFD_CACHE_COUNT_PER_SET; i++) {
		if (cache_set->status_arr[i] != alloc_active)
			continue;
		found_cache = cache_set->caches[i];
		if (found_cache->dmc == dmc)
			return found_cache;
	}

	return NULL;
}

static struct pfd_cache *
init_pfd_cache(
		struct cache_c *dmc,
		struct pfd_cache_set *cache_set,
		int idx) {

	struct pfd_cache *cache;
	struct pfd_cache_meta *meta;
	int i;

	cache = (struct pfd_cache *)vmalloc(sizeof(struct pfd_cache));
	if (cache == NULL)
		return NULL;
	cache->data = vmalloc(
			PFD_CACHE_BLOCK_COUNT << (SECTOR_SHIFT + dmc->block_shift));
	if (cache->data == NULL)
		goto free_cache;

	cache_set->caches[idx] = cache;
	cache->cache_set = cache_set;
	cache->dmc = dmc;
	spin_lock_init(&(cache->lock));
	for (i = 0; i < PFD_CACHE_BLOCK_COUNT; i++) {
		meta = &(cache->metas[i]);
		meta->cache = cache;
		meta->status = empty;
	}

	return cache;

free_cache:
	vfree((void *)cache);
	return NULL;
}

void pfd_cache_init() {
	int i;
	main_cache_set.count = 0;
	spin_lock_init(&(main_cache_set.lock));
	for (i = 0; i < PFD_CACHE_COUNT_PER_SET; i++) {
		main_cache_set.status_arr[i] = alloc_empty;
		main_cache_set.caches[i] = NULL;
	}
}

void pfd_cache_exit() {
	long flags;
	int i;
	struct pfd_cache *cache;

	spin_lock_irqsave(&(main_cache_set.lock), flags);
	for (i = 0; i < PFD_CACHE_COUNT_PER_SET; i++) {
		cache = main_cache_set.caches[i];
		if (cache != NULL) {
			vfree((void *)(cache->data));
			vfree((void *)cache);
		}
	}
	spin_unlock_irqrestore(&(main_cache_set.lock), flags);
}

void pfd_cache_add(struct cache_c *dmc) {
	struct pfd_cache *cache = NULL;
	int i;
	long flags;

	spin_lock_irqsave(&(main_cache_set.lock), flags);

	cache = find_cache_in_cache_set(dmc, &main_cache_set);
	if (cache != NULL) {
		spin_unlock_irqrestore(&(main_cache_set.lock), flags);
		MPPRINTK("pfd_cache already exist.");
		return;
	}

	if (main_cache_set.count == PFD_CACHE_COUNT_PER_SET) {
		spin_unlock_irqrestore(&(main_cache_set.lock), flags);
		MPPRINTK("\033[0;32;31mNo room to add pfd_cache.");
		return;
	}

	for(i = 0; i < PFD_CACHE_COUNT_PER_SET; i++) {
		if (main_cache_set.status_arr[i] == alloc_empty) {
			main_cache_set.status_arr[i] = alloc_prepare;
			main_cache_set.count += 1;
			break;
		}
	}
	spin_unlock_irqrestore(&(main_cache_set.lock), flags);

	if (i < PFD_CACHE_COUNT_PER_SET) {
		cache = init_pfd_cache(dmc, &main_cache_set, i);
		spin_lock_irqsave(&(main_cache_set.lock), flags);
		if (cache == NULL) {
			main_cache_set.status_arr[i] = alloc_empty;
			main_cache_set.count -= 1;
		} else {
			main_cache_set.status_arr[i] = alloc_active;
		}
		spin_unlock_irqrestore(&(main_cache_set.lock), flags);
	}

	if (cache == NULL) {
		MPPRINTK("\033[0;32;31mCan't alloc new pfd_cache.");
	} else {
		MPPRINTK("\033[0;32;32mNew pfd_cache created.");
	}
}
