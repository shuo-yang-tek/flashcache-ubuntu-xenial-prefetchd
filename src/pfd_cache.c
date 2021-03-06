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

	struct semaphore prepare_lock;
	spinlock_t lock;
	spinlock_t lock_interrupt;

	sector_t dbn;
	enum pfd_cache_meta_status status;
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

enum cache_set_init_status {
	set_empty = 1,
	set_prepare,
	set_valid,
};

struct pfd_cache_set {
	int count;
	enum cache_set_init_status status_arr[PFD_CACHE_COUNT_PER_SET];
	struct cache_c *dmc_arr[PFD_CACHE_COUNT_PER_SET];
	struct pfd_cache *caches[PFD_CACHE_COUNT_PER_SET];
	spinlock_t lock;
};

static struct pfd_cache_set main_cache_set;
static struct dm_io_client *hdd_client;
static struct dm_io_client *ssd_client;

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

	int i;

	for (i = 0; i < PFD_CACHE_COUNT_PER_SET; i++) {
		if (cache_set->status_arr[i] == set_valid && cache_set->dmc_arr[i] == dmc)
			return cache_set->caches[i];
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
	for (i = 0; i < PFD_CACHE_BLOCK_COUNT; i++) {
		meta = &(cache->metas[i]);
		meta->cache = cache;
		meta->status = empty;
		atomic_set(&(meta->hold_count), 0);
		spin_lock_init(&(meta->lock));
		spin_lock_init(&(meta->lock_interrupt));
	}
	spin_lock_init(&(cache->lock));

	return cache;

free_cache:
	vfree((void *)cache);
	return NULL;
}

int pfd_cache_init() {
	int i;

	hdd_client = dm_io_client_create();
	if (IS_ERR(hdd_client))
		return -1;

	ssd_client = dm_io_client_create();
	if (IS_ERR(ssd_client))
		goto free_hdd_client;

	main_cache_set.count = 0;
	spin_lock_init(&(main_cache_set.lock));
	for (i = 0; i < PFD_CACHE_COUNT_PER_SET; i++) {
		main_cache_set.status_arr[i] = set_empty;
		main_cache_set.dmc_arr[i] = NULL;
		main_cache_set.caches[i] = NULL;
	}

	return 0;

free_hdd_client:
	dm_io_client_destroy(hdd_client);

	return -1;
}

void pfd_cache_exit() {
	int i;
	struct pfd_cache *cache;

	dm_io_client_destroy(hdd_client);
	dm_io_client_destroy(ssd_client);
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

	for (i = 0; i < PFD_CACHE_COUNT_PER_SET; i++) {
		if (main_cache_set.status_arr[i] != set_empty &&
				main_cache_set.dmc_arr[i] == dmc) {
			spin_unlock_irqrestore(&(main_cache_set.lock), flags);
			MPPRINTK("pfd_cache already exist.");
			return;
		}
	}

	if (main_cache_set.count == PFD_CACHE_COUNT_PER_SET) {
		spin_unlock_irqrestore(&(main_cache_set.lock), flags);
		MPPRINTK("\033[0;32;31mNo room to add pfd_cache.");
		return;
	}

	for(i = 0; i < PFD_CACHE_COUNT_PER_SET; i++) {
		if (main_cache_set.status_arr[i] == set_empty) {
			main_cache_set.status_arr[i] = set_prepare;
			main_cache_set.dmc_arr[i] = dmc;
			main_cache_set.count += 1;
			break;
		}
	}
	spin_unlock_irqrestore(&(main_cache_set.lock), flags);

	if (i < PFD_CACHE_COUNT_PER_SET) {
		cache = init_pfd_cache(dmc, &main_cache_set, i);
		spin_lock_irqsave(&(main_cache_set.lock), flags);
		if (cache == NULL) {
			main_cache_set.status_arr[i] = set_empty;
			main_cache_set.dmc_arr[i] = NULL;
			main_cache_set.count -= 1;
		} else {
			main_cache_set.status_arr[i] = set_valid;
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

	spin_lock_irqsave(&(meta->lock), flags);

	if (meta->status == empty || meta->dbn != dbn)
		goto cache_miss_unlock;

	atomic_inc(&(meta->hold_count));
	spin_unlock_irqrestore(&(meta->lock), flags);

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
	spin_unlock_irqrestore(&(meta->lock), flags);

cache_miss:
	DPPRINTK("\033[0;32;34mcache miss: %lu", dbn);
	return false;
}

static void
io_callback(unsigned long error, void *context) {
	struct pfd_cache_meta *meta =
		(struct pfd_cache_meta *)context;
	struct pfd_cache *cache = meta->cache;
	struct cache_c *dmc = cache->dmc;
	struct cache_set *cache_set;
	struct cacheblock *cacheblk;
	long flags1, flags2;
	enum pfd_cache_meta_status status = error == 0 ?
		valid : empty;

	spin_lock_irqsave(&(meta->lock_interrupt), flags1);
	meta->status = status;
	up(&(meta->prepare_lock));

	if (meta->ssd_index >= 0) {
		cacheblk = &(dmc->cache[meta->ssd_index]);
		cache_set = &(dmc->cache_sets[meta->ssd_index / dmc->assoc]);
		spin_lock_irqsave(&cache_set->set_spin_lock, flags2);
		cacheblk->cache_state &= ~BLOCK_IO_INPROG;
		spin_unlock_irqrestore(&cache_set->set_spin_lock, flags2);
	}

	spin_unlock_irqrestore(&(meta->lock_interrupt), flags1);

	DPPRINTK("%sio_callback. (%lu)",
			error ? "\033[0;32;31m" : "",
			meta->dbn);
}

static void
dispatch_io_request(struct pfd_cache_meta *meta) {
	struct pfd_cache *cache = meta->cache;
	struct cache_c *dmc = cache->dmc;
	struct dm_io_request req;
	struct dm_io_region region;
	int dm_io_ret;
	bool from_ssd = meta->ssd_index < 0 ? false : true;
	int meta_idx = dbn_to_cache_index(cache, meta->dbn);
	long flags;

	req.bi_op = READ;
	req.bi_op_flags = 0;
	req.notify.fn = (io_notify_fn)io_callback;
	req.notify.context = (void *)meta;
	req.client = from_ssd ?
		ssd_client : hdd_client;
	req.mem.type = DM_IO_VMA;
	req.mem.offset = 0;
	req.mem.ptr.vma =cache->data +
		((unsigned long)meta_idx << (dmc->block_shift + SECTOR_SHIFT));

	region.bdev = from_ssd ?
		dmc->cache_dev->bdev :
		dmc->disk_dev->bdev;
	region.sector = from_ssd ?
		INDEX_TO_CACHE_ADDR(dmc, meta->ssd_index) :
		meta->dbn;
	region.count = dmc->block_size;

	dm_io_ret = dm_io(&req, 1, &region, NULL);
	if (dm_io_ret != 0) {
		spin_lock_irqsave(&(meta->lock), flags);
		meta->status = empty;
		up(&(meta->prepare_lock));
		spin_unlock_irqrestore(&(meta->lock), flags);
	}

	DPPRINTK("%sdispatch io: %lu on %s",
			dm_io_ret ? "\033[0;32;31m" : "",
			meta->dbn,
			from_ssd ? "SSD" : "HDD");
}

static int
get_ssd_cache_index(
		struct pfd_cache_meta *meta,
		sector_t dbn) {

	struct bio tmp_bio;
	int lookup_res;
	int lookup_index;
	struct cache_c *dmc = meta->cache->dmc;
	struct cacheblock *cacheblk;
	int ret;

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
				cacheblk->cache_state |= CACHEREADINPROG;
				ex_flashcache_setlocks_multidrop(dmc, &tmp_bio);

				return lookup_index;
			}
		}
	}
	ex_flashcache_setlocks_multidrop(dmc, &tmp_bio);
	return -1;
}

static bool
update_seq_status(
		struct cache_c *dmc,
		sector_t dbn,
		long *start,
		int *count) {

	dbn = INDEX_TO_CACHE_ADDR(dmc, dbn);

	if (*start < 0) {
		*start = (long)dbn;
		*count = 1;
		return false;
	}

	if ((*start) - (long)dbn == (long)dmc->block_size) {
		*start = (long)dbn;
		*count += 1;
	} else if ((long)dbn - (*start) == (*count) << dmc->block_shift)
		*count += 1;
	else {
		*start = (long)dbn;
		*count = 1;
		return true;
	}

	return false;
}

void pfd_cache_prefetch(
		struct cache_c *dmc,
		struct pfd_stat_info *info) {

	long flags;
	struct pfd_cache *cache;
	struct pfd_cache_meta *meta;
	int meta_idx;
	sector_t dbn_arr[PFD_CACHE_MAX_STEP];
	int dbn_arr_count = pfd_stat_get_prefetch_dbns(
			dmc, info, dbn_arr);
	int i, i_end, i_step;
	sector_t dbn;
	long ssd_seq_status_start = -1;
	int ssd_seq_status_count;
	int ssd_count = 0;
	int ssd_max = dbn_arr_count >> PFD_CACHE_MAX_SSD_SHIFT;
	int ssd_index;

	if (dbn_arr_count == 0)
		return;
	else if (dbn_arr_count > 0) {
		i = 0;
		i_end = dbn_arr_count;
		i_step = 1;
	} else {
		i = (-dbn_arr_count) - 1;
		i_end = -1;
		i_step = -1;
	}

	spin_lock_irqsave(&(main_cache_set.lock), flags);
	cache = find_cache_in_cache_set(dmc, &main_cache_set);
	spin_unlock_irqrestore(&(main_cache_set.lock), flags);

	if (cache == NULL) {
		MPPRINTK("\033[0;32;31mCan't find pfd_cache.");
		return;
	}

	for (; i != i_end; i += i_step) {
		dbn = dbn_arr[i];
		meta_idx = dbn_to_cache_index(cache, dbn);
		meta = &(cache->metas[meta_idx]);

		spin_lock_irqsave(&(meta->lock), flags);

		if (meta->status != empty && meta->dbn == dbn) {
			// exist
			spin_unlock_irqrestore(&(meta->lock), flags);
			continue;
		}

		if (meta->status == prepare ||
				atomic_read(&(meta->hold_count)) > 0) {
			// busy
			spin_unlock_irqrestore(&(meta->lock), flags);
			continue;
		}

		// setup meta
		meta->dbn = dbn;
		meta->status = prepare;
		sema_init(&(meta->prepare_lock), 0);

		spin_unlock_irqrestore(&(meta->lock), flags);

		if (ssd_count < ssd_max) {
			// ssd
			ssd_index = get_ssd_cache_index(meta, dbn);
			if (ssd_index >= 0) {
				meta->ssd_index = ssd_index;
				dispatch_io_request(meta);
				if (update_seq_status(
							dmc, dbn,
							&ssd_seq_status_start,
							&ssd_seq_status_count)) {
					ssd_count += 1;
				}
				continue;
			}
		}

		meta->ssd_index = -1;
		dispatch_io_request(meta);
	}
}

int pfd_cache_reset() {
	long flags1, flags2;
	int i, j;
	struct pfd_cache *cache;
	struct pfd_cache_meta *meta;

	MPPRINTK("\033[1;33mpfd_cache reseting...");

	spin_lock_irqsave(&(main_cache_set.lock), flags1);

	for (i = 0; i < PFD_CACHE_COUNT_PER_SET; i++) {
		if (main_cache_set.status_arr[i] == set_prepare) {
			goto fail;
		}
		cache = main_cache_set.caches[i];
		if (cache != NULL) {
			for (j = 0; j < PFD_CACHE_BLOCK_COUNT; j++) {
				meta = &(cache->metas[j]);
				spin_lock_irqsave(&(meta->lock), flags2);
				if (meta->status == prepare || atomic_read(&(meta->hold_count)) > 0) {
					spin_unlock_irqrestore(&(meta->lock), flags2);
					goto fail;
				}
				meta->status = empty;
				spin_unlock_irqrestore(&(meta->lock), flags2);
			}
		}
	}

	spin_unlock_irqrestore(&(main_cache_set.lock), flags1);
	MPPRINTK("\033[0;32;32mpfd_cache reseted.");
	return 0;

fail:
	spin_unlock_irqrestore(&(main_cache_set.lock), flags1);

	MPPRINTK("\033[0;32;31mcan't reset pfd_cache");
	return -1;
}
