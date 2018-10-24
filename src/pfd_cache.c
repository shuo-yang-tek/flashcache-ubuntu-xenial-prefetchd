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

inline int
dbn_to_cache_index(
		struct pfd_cache *cache,
		sector_t dbn) {
	return (int)
		((dbn >> cache->dmc->block_shift) % PFD_CACHE_BLOCK_COUNT);
}

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
	int i;
	struct pfd_cache *cache;

	for (i = 0; i < PFD_CACHE_COUNT_PER_SET; i++) {
		cache = main_cache_set.caches[i];
		if (cache != NULL) {
			vfree((void *)(cache->data));
			vfree((void *)cache);
		}
	}
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

bool pfd_cache_handle_bio(
		struct cache_c *dmc,
		struct bio *bio) {

	long flags;
	struct pfd_cache *cache;
	struct pfd_cache_meta *meta;
	sector_t dbn = bio->bi_iter.bi_sector;
	int index;
	void *data_src;
	void *data_dest;
	struct bio_vec bvec;
	struct bvec_iter iter;


	spin_lock_irqsave(&(main_cache_set.lock), flags);
	cache = find_cache_in_cache_set(dmc, &main_cache_set);
	spin_unlock_irqrestore(&(main_cache_set.lock), flags);

	if (cache == NULL) {
		MPPRINTK("\033[0;32;31mCan't find pfd_cache.");
		return false;
	}

	index = dbn_to_cache_index(cache, dbn);
	meta = &(cache->metas[index]);

	spin_lock_irqsave(&(cache->lock), flags);

	if (meta->status == empty || meta->dbn != dbn)
		goto cache_miss_unlock;
	atomic_inc(&(meta->hold_count));

	spin_unlock_irqrestore(&(cache->lock), flags);
	if (meta->status == prepare) {
		down_interruptible(&(meta->prepare_lock));
		up(&(meta->prepare_lock));
	}
	if (meta->status != valid) {
		atomic_dec(&(meta->hold_count));
		goto cache_miss;
	}

	data_src = cache->data + 
		((unsigned long)index << (SECTOR_SHIFT + dmc->block_shift));
	bio_for_each_segment(bvec, bio, iter) {
		data_dest = kmap(bvec.bv_page) + bvec.bv_offset;
		memcpy(data_dest, data_src, bvec.bv_len);
		kunmap(bvec.bv_page);
		data_src += bvec.bv_len;
	}

	bio_endio(bio);
	atomic_dec(&(meta->hold_count));

	DPPRINTK("\033[1;33mcache hit: %lu", dbn);
	return true;

cache_miss_unlock:
	spin_unlock_irqrestore(&(cache->lock), flags);

cache_miss:
	DPPRINTK("\033[0;32;34mcache miss: %lu", dbn);
	return false;
}

static long
get_dbn_of_step(
		struct cache_c *dmc,
		struct pfd_stat_info *info,
		long step) {

	long result = (long)(info->last_sect);
	long max_step = 
		info->stride_count * info->seq_total_count +
		info->seq_count;
	long tmp;

	if (max_step < PFD_CACHE_THRESHOLD_STEP)
		return -1;

	max_step = max_step > PFD_CACHE_MAX_STEP ?
		PFD_CACHE_MAX_STEP : max_step;

	if (step > max_step)
		return -1;

	if (info->seq_total_count == 0 || 
			step + info->seq_count <= info->seq_total_count)
		result += step << dmc->block_shift;
	else {
		tmp = info->seq_count + step;
		result -= info->seq_count << dmc->block_shift;
		result += (tmp / info->seq_total_count) *
			info->stride_distance_sect;
		result += (tmp % info->seq_total_count) << dmc->block_shift;
	}

	if (result >= (long)(dmc->disk_dev->bdev->bd_part->nr_sects))
		return -1;

	return result;
}

static void
alloc_prefetch(
		struct pfd_cache_meta *meta,
		sector_t dbn,
		int lookup_index) {
}

static bool
do_ssd_request(
		struct pfd_cache_meta *meta,
		sector_t dbn) {

	struct bio tmp_bio;
	int lookup_res;
	int lookup_index;
	struct cache_c *dmc = meta->cache->dmc;
	struct cacheblock *cacheblk;

	tmp_bio.bi_iter.bi_sector = dbn;
	tmp_bio.bi_iter.bi_size = 
		(unsigned int)dmc->block_size << SECTOR_SHIFT;

	ex_flashcache_setlocks_multiget(dmc, &tmp_bio);
	lookup_res = ex_flashcache_lookup(
			dmc,
			&tmp_bio,
			&lookup_index);
	if (lookup_res > 0) {
		cacheblk = &dmc->cache[lookup_index];
		if ((cacheblk->cache_state & VALID) && 
				(cacheblk->dbn == dbn)) {
			if (!(cacheblk->cache_state & BLOCK_IO_INPROG) && (cacheblk->nr_queued == 0)) {
				/*cacheblk->cache_state |= CACHEREADINPROG;*/
				ex_flashcache_setlocks_multidrop(dmc, &tmp_bio);
				alloc_prefetch(meta, dbn, lookup_index);
				return true;
			}
		}
	}
	ex_flashcache_setlocks_multidrop(dmc, &tmp_bio);
	return false;
}

void pfd_cache_prefetch(
		struct cache_c *dmc,
		struct pfd_stat_info *info) {

	long flags;
	struct pfd_cache *cache;
	long dbn;
	long step = 1;
	int meta_idx;
	struct pfd_cache_meta *meta;
	int stop_reason = 0;

	spin_lock_irqsave(&(main_cache_set.lock), flags);
	cache = find_cache_in_cache_set(dmc, &main_cache_set);
	spin_unlock_irqrestore(&(main_cache_set.lock), flags);

	if (cache == NULL) {
		MPPRINTK("\033[0;32;31mCan't find pfd_cache.");
		return;
	}

	spin_lock_irqsave(&(cache->lock), flags);

	dbn = get_dbn_of_step(dmc, info, step);
	while (dbn >= 0) {
		meta_idx = dbn_to_cache_index(cache, dbn);
		meta = &(cache->metas[meta_idx]);

		if (meta->status != empty && meta->dbn == (sector_t)dbn) {
			// mem already
			stop_reason = 1;
			break;
		}
		if (meta->status == prepare ||
				atomic_read(&(meta->hold_count)) > 0) {
			// no room
			stop_reason = 2;
			break;
		}

		// check ssd
		if (do_ssd_request(meta, dbn)) {
			stop_reason = 3;
			break;
		}

		// do hdd
		alloc_prefetch(meta, dbn, -1);
		step += 1;
		dbn = get_dbn_of_step(dmc, info, step);
	}

	spin_unlock_irqrestore(&(cache->lock), flags);
	DPPRINTK("prefetch count: %ld", step - 1);
	DPPRINTK("stop reason: %d\n", stop_reason);
}
