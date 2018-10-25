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

struct cb_context {
	struct pfd_cache *cache;
	int index;
	int count;
	struct cb_context *next;
};

struct cb_context_stack {
	struct cb_context pool[PFD_CACHE_BLOCK_COUNT];
	int count;
	struct cb_context *head;
	spinlock_t lock;
	spinlock_t lock_interrupt;
};

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
	struct cb_context_stack context_stack;
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

static void
init_cb_context_stack(struct pfd_cache *cache) {
	int i;
	struct cb_context *cc;
	struct cb_context_stack *stack = &(cache->context_stack);

	for (i = 0; i < PFD_CACHE_BLOCK_COUNT; i++) {
		cc = &(stack->pool[i]);
		cc->next = i == PFD_CACHE_BLOCK_COUNT - 1 ?
			NULL : &(stack->pool[i + 1]);
		cc->cache = cache;
	}

	stack->count = PFD_CACHE_BLOCK_COUNT;
	stack->head = &(stack->pool[0]);
	spin_lock_init(&(stack->lock));
	spin_lock_init(&(stack->lock_interrupt));
}

static struct cb_context *
pop_cb_context_stack(struct cb_context_stack *stack, bool interrupt) {
	long flags;
	struct cb_context *result;
	spinlock_t *lock = interrupt ?
		&(stack->lock_interrupt) :
		&(stack->lock);

	spin_lock_irqsave(lock, flags);
	if (stack->count == 0) {
		spin_unlock_irqrestore(lock, flags);
		return NULL;
	}

	result = stack->head;
	stack->head = result->next;
	stack->count -= 1;
	result->next = NULL;

	spin_unlock_irqrestore(lock, flags);
	return result;
}

static void
push_cb_context_stack(
		struct cb_context_stack *stack,
		struct cb_context *context,
		bool interrupt) {

	spinlock_t *lock = interrupt ?
		&(stack->lock_interrupt) :
		&(stack->lock);
	long flags;
	spin_lock_irqsave(lock, flags);
	context->next = stack->head;
	stack->head = context;
	stack->count += 1;
	spin_unlock_irqrestore(lock, flags);
}

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

	init_cb_context_stack(cache);

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
	long walk_sects;

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
		tmp = info->seq_count + step - 1;
		walk_sects = (tmp / info->seq_total_count) *
			info->stride_distance_sect;
		walk_sects += 
			((info->seq_count - 1 + step) % info->seq_total_count) << dmc->block_shift;
		if (walk_sects >> (dmc->block_shift) >= PFD_CACHE_BLOCK_COUNT)
			return -1;
		else
			result += walk_sects - ((info->seq_count - 1) << dmc->block_shift);
	}

	if (result >= (long)(dmc->disk_dev->bdev->bd_part->nr_sects))
		return -1;

	return result;
}

/*static void io_callback(unsigned long error, void *context) {*/
	/*struct pfd_cache_meta *meta =*/
		/*(struct pfd_cache_meta *) context;*/
	/*struct cache_set *cache_set;*/
	/*struct cacheblock *cacheblk;*/
	/*struct pfd_cache *cache = meta->cache;*/
	/*struct cache_c *dmc = cache->dmc;*/
	/*long flags1, flags2;*/

	/*spin_lock_irqsave(&(meta->lock_interrupt), flags1);*/
	/*meta->status = error != 0 ? empty : valid;*/
	/*up(&(meta->prepare_lock));*/

	/*if (meta->ssd_index >= 0) {*/
		/*cacheblk = &(dmc->cache[meta->ssd_index]);*/
		/*cache_set = &(dmc->cache_sets[meta->ssd_index / dmc->assoc]);*/
		/*spin_lock_irqsave(&cache_set->set_spin_lock, flags2);*/
		/*cacheblk->cache_state &= ~BLOCK_IO_INPROG;*/
		/*spin_unlock_irqrestore(&cache_set->set_spin_lock, flags2);*/
	/*}*/

	/*spin_unlock_irqrestore(&(meta->lock_interrupt), flags1);*/

	/*DPPRINTK("%sio_callback. (%lu)",*/
			/*error ? "\033[0;32;31m" : "",*/
			/*meta->dbn);*/
/*}*/

/*static void*/
/*alloc_prefetch(*/
		/*struct pfd_cache_meta *meta,*/
		/*sector_t dbn,*/
		/*int lookup_index) {*/

	/*struct cache_c *dmc = meta->cache->dmc;*/
	/*struct dm_io_request req;*/
	/*struct dm_io_region region;*/
	/*int dm_io_ret;*/
	/*bool from_ssd = lookup_index >= 0 ? true : false;*/

	/*meta->dbn = dbn;*/
	/*meta->status = prepare;*/
	/*sema_init(&(meta->prepare_lock), 0);*/
	/*atomic_set(&(meta->hold_count), 0);*/
	/*meta->ssd_index = lookup_index;*/

	/*req.bi_op = READ;*/
	/*req.bi_op_flags = 0;*/
	/*req.notify.fn = (io_notify_fn)io_callback;*/
	/*req.notify.context = (void *)(meta);*/
	/*req.client = from_ssd ? ssd_client : hdd_client;*/
	/*req.mem.type = DM_IO_VMA;*/
	/*req.mem.offset = 0;*/
	/*req.mem.ptr.vma = meta->cache->data +*/
		/*((unsigned long)dbn_to_cache_index(meta->cache, dbn) <<*/
		 /*(dmc->block_shift + SECTOR_SHIFT));*/

	/*region.bdev = from_ssd ?*/
		/*dmc->cache_dev->bdev : dmc->disk_dev->bdev;*/
	/*region.sector = from_ssd ?*/
		/*INDEX_TO_CACHE_ADDR(dmc, lookup_index) :*/
		/*dbn;*/
	/*region.count = dmc->block_size;*/

	/*dm_io_ret = dm_io(&req, 1, &region, NULL);*/
	/*if (dm_io_ret) {*/
		/*meta->status = empty;*/
		/*up(&(meta->prepare_lock));*/
	/*}*/
	/*DPPRINTK("prefetch (%lu) on %s: %s.",*/
			/*dbn,*/
			/*!from_ssd ? "HDD" : "SSD",*/
			/*dm_io_ret ? "Failed" : "Sent");*/
/*}*/

static void
io_callback2(unsigned long error, void *context) {
	struct cb_context *cb_context =
		(struct cb_context *) context;
	struct pfd_cache *cache = cb_context->cache;
	struct cache_c *dmc = cache->dmc;
	struct pfd_cache_meta *meta;
	struct cache_set *cache_set;
	struct cacheblock *cacheblk;
	long flags1, flags2;
	int i;
	enum pfd_cache_meta_status status =
		error ? empty : valid;

	for (i = 0; i < cb_context->count; i++) {
		meta = &(cache->metas[(i + cb_context->index) % PFD_CACHE_BLOCK_COUNT]);
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
	}

	push_cb_context_stack(&(cache->context_stack), cb_context, true);

	DPPRINTK("%sio_callback. (%lu+%lu)",
			error ? "\033[0;32;31m" : "",
			meta->dbn,
			(unsigned long)cb_context->count << dmc->block_shift);
}

static int
dispatch_read_request(
		struct pfd_cache *cache,
		sector_t dbn,
		int count,
		int lookup_index) {

	struct cache_c *dmc = cache->dmc;
	int meta_index = dbn_to_cache_index(cache, dbn);
	int req_count = count + meta_index > PFD_CACHE_BLOCK_COUNT ?
		2 : 1;
	struct pfd_cache_meta *meta;
	int i;
	struct dm_io_request req;
	struct dm_io_region region;
	int dm_io_ret;
	bool ssd = lookup_index < 0 ? false : true;
	struct cb_context *cb_context;

	return 1;

	req.bi_op = READ;
	req.bi_op_flags = 0;
	req.notify.fn = (io_notify_fn)io_callback2;
	req.client = ssd ? ssd_client : hdd_client;
	req.mem.type = DM_IO_VMA;
	req.mem.offset = 0;
	region.bdev = ssd ?
		dmc->cache_dev->bdev : dmc->disk_dev->bdev;

	if (ssd) {
		region.bdev = dmc->cache_dev->bdev;
		region.sector = INDEX_TO_CACHE_ADDR(dmc, lookup_index);
	} else {
		region.bdev = dmc->disk_dev->bdev;
		region.sector = dbn;
	}

	for (i = 0; i < req_count; i++) {
		cb_context = pop_cb_context_stack(&(cache->context_stack), false);
		cb_context->index = meta_index;
		cb_context->count = PFD_CACHE_BLOCK_COUNT - meta_index;
		req.mem.ptr.vma = cache->data;
		region.sector = dbn;
		if (i != 0) {
			cb_context->count = count - cb_context->count;
			req.mem.ptr.vma +=
				(unsigned long) meta_index << (dmc->block_shift + SECTOR_SHIFT);
			cb_context->index = 0;
		}
		req.notify.context = (void *) cb_context;
		region.count = (sector_t)cb_context->count << dmc->block_shift;

		dm_io_ret = dm_io(&req, 1, &region, NULL);
		if (dm_io_ret)
			return i + 1;

		if (!ssd)
			region.sector += (sector_t)cb_context->count << dmc->block_shift;
	}

	return 0;
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

	return false;

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

				meta->ssd_index = lookup_index;
				dispatch_read_request(
						meta->cache,
						dbn,
						1,
						lookup_index);

				return true;
			}
		}
	}
	ex_flashcache_setlocks_multidrop(dmc, &tmp_bio);
	return false;
}

static void
flush_dispatch_req_pool(
		struct pfd_cache *cache,
		long start,
		int count) {

	struct pfd_cache_meta *meta;
	struct cache_c *dmc = cache->dmc;
	int i, tmp;
	long flags;
	int ret;
	int start_index;

	if (start < 0) return;
	start_index = dbn_to_cache_index(cache, (sector_t)start);

	/*ret = dispatch_read_request(*/
			/*cache,*/
			/*(sector_t)start,*/
			/*count,*/
			/*-1);*/
	ret = 1;

	DPPRINTK("---- %ld+%ld",
			start, (long)count << dmc->block_shift);

	if (ret == 1) {
		for (i = 0; i < count; i++) {
			meta = &(cache->metas[(i + start_index) % PFD_CACHE_BLOCK_COUNT]);
			spin_lock_irqsave(&(meta->lock), flags);
			meta->status = empty;
			up(&(meta->prepare_lock));
			spin_unlock_irqrestore(&(meta->lock), flags);
		}
	} else if (ret == 2) {
		tmp = start_index + count - PFD_CACHE_BLOCK_COUNT;
		for (i = 0; i < tmp; i++) {
			meta = &(cache->metas[i]);
			spin_lock_irqsave(&(meta->lock), flags);
			meta->status = empty;
			up(&(meta->prepare_lock));
			spin_unlock_irqrestore(&(meta->lock), flags);
		}
	}
}

static void
update_dispatch_req_pool(
		struct pfd_cache_meta *meta,
		long dbn,
		long *start,
		int *count) {

	struct pfd_cache *cache = meta->cache;
	struct cache_c *dmc = cache->dmc;
	long flags;

	if (dbn < 0) {
		spin_lock_irqsave(&(meta->lock), flags);
		meta->status = empty;
		up(&(meta->prepare_lock));
		spin_unlock_irqrestore(&(meta->lock), flags);
		return;
	}
	if (*start < 0) {
		*start = dbn;
		*count = 1;
		return;
	}

	if (dbn + (long)dmc->block_size == *start) {
		*start -= (long)dmc->block_size;
		*count += 1;
	}
	else if (*start + ((long)count << dmc->block_shift) == dbn)
		*count += 1;
	else {
		flush_dispatch_req_pool(cache, *start, *count);
		*start = dbn;
		*count = 1;
	}
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
	long seq_pool_start = -1;
	int seq_pool_count;

	spin_lock_irqsave(&(main_cache_set.lock), flags);
	cache = find_cache_in_cache_set(dmc, &main_cache_set);
	spin_unlock_irqrestore(&(main_cache_set.lock), flags);

	if (cache == NULL) {
		MPPRINTK("\033[0;32;31mCan't find pfd_cache.");
		return;
	}

	dbn = get_dbn_of_step(dmc, info, step);
	while (dbn >= 0) {
		meta_idx = dbn_to_cache_index(cache, dbn);
		meta = &(cache->metas[meta_idx]);

		spin_lock_irqsave(&(meta->lock), flags);

		if (meta->status != empty && meta->dbn == (sector_t)dbn) {
			// mem already
			stop_reason = 1;
			spin_unlock_irqrestore(&(meta->lock), flags);
			break;
		}
		if (meta->status == prepare ||
				atomic_read(&(meta->hold_count)) > 0) {
			// no room
			stop_reason = 2;
			spin_unlock_irqrestore(&(meta->lock), flags);
			break;
		}

		// setup meta
		meta->dbn = dbn;
		meta->status = prepare;
		sema_init(&(meta->prepare_lock), 0);
		atomic_set(&(meta->hold_count), 0);
		meta->ssd_index = -1;

		// check ssd
		if (do_ssd_request(meta, dbn)) {
			stop_reason = 3;
			spin_unlock_irqrestore(&(meta->lock), flags);
			break;
		}

		// do hdd
		spin_unlock_irqrestore(&(meta->lock), flags);
		step += 1;
		dbn = get_dbn_of_step(dmc, info, step);
		update_dispatch_req_pool(meta, dbn, &seq_pool_start, &seq_pool_count);
	}
	flush_dispatch_req_pool(cache, seq_pool_start, seq_pool_count);

	DPPRINTK("stop reason: %d\n", stop_reason);
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
