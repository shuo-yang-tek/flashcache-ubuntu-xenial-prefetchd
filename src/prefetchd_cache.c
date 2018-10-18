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
#include "./prefetchd_log.h"
#include "./prefetchd_stat.h"
#include "./prefetchd_cache.h"

#define cache_meta_map_foreach(map, meta, i) \
	for ((i) = 0, (meta) = &cache_metas[(map).index]; \
			 (i) < (map).count; \
			 ++(i), \
			 (meta) = &cache_metas[((i) + (map).index) % PREFETCHD_CACHE_PAGE_COUNT])

#define size_to_page_count(size) ((size) >> PAGE_SHIFT)

#define sector_num_to_cache_index(sector_num) \
	(((sector_num) >> (PAGE_SHIFT - 9)) % PREFETCHD_CACHE_PAGE_COUNT)

#define get_cache_meta_map(sector_num, size, res) \
	(res)->index = sector_num_to_cache_index((sector_num)); \
	(res)->count = size_to_page_count((size));

#define is_request_fit_cache(sector_num, size) \
	((((size) >> PAGE_SHIFT) <= PREFETCHD_CACHE_MAX_PAGE_COUNT_PER_CACHE) && \
	!(((sector_num) % (PAGE_SIZE >> 9)) || \
	(size) % PAGE_SIZE))

#define is_bio_fit_cache(bio) \
	is_request_fit_cache((bio)->bi_iter.bi_sector, (bio)->bi_iter.bi_size)

#define is_meta_removable(meta) \
	(!((meta)->status == prepare || \
		((meta)->status == active && \
		 atomic_read(&((meta)->hold_count)) > 0)))

#define is_meta_match(meta, sector_num) \
	((meta)->status != empty && (meta)->sector_num == (sector_num))

DEFINE_SPINLOCK(cache_global_lock);

enum cache_status {
	empty = 1,
	prepare,
	active
};

struct cache_meta {
	u64 sector_num;

	enum cache_status status;
	struct semaphore prepare_lock;
	atomic_t hold_count;

	// for droping dmc lock
	struct cache_c *dmc;
	struct bio tmp_bio;
	int index;
	bool from_ssd;
};

struct cache_meta_map {
	int index;
	int count;
};

struct cache_meta_map_stack_elm {
	struct cache_meta_map map;
	struct cache_meta_map_stack_elm *next;
};

struct cache_meta_map_stack {
	struct cache_meta_map_stack_elm pool[PREFETCHD_CACHE_PAGE_COUNT];
	struct cache_meta_map_stack_elm *head;
	int count;
};

static void *cache_content;
static struct cache_meta *cache_metas;
static struct dm_io_client *hdd_client;
static struct dm_io_client *ssd_client;
static struct cache_meta_map_stack *map_stack;

static void init_map_stack(void) {
	int i;

	for (i = 0; i < PREFETCHD_CACHE_PAGE_COUNT; i++) {
		map_stack->pool[i].next =
			i == PREFETCHD_CACHE_PAGE_COUNT - 1 ?
			NULL :
			&(map_stack->pool[i + 1]);
	}

	map_stack->head = &(map_stack->pool[0]);
	map_stack->count = PREFETCHD_CACHE_PAGE_COUNT;
}

static struct cache_meta_map_stack_elm *pop_map_stack(void) {
	struct cache_meta_map_stack_elm *ret;

	if (map_stack->count <= 0) return NULL;
	ret = map_stack->head;
	map_stack->head = map_stack->head->next;
	ret->next = NULL;
	map_stack->count -= 1;
	return ret;
}

static void push_map_stack(struct cache_meta_map_stack_elm *elm) {
	elm->next = map_stack->head;
	map_stack->head = elm;
	map_stack->count += 1;
}

bool prefetchd_cache_init() {
	int i;

	cache_content = vmalloc(PREFETCHD_CACHE_PAGE_COUNT << PAGE_SHIFT);
	if (cache_content == NULL)
		goto fail_log;

	cache_metas = 
		(struct cache_meta *)
		vmalloc(sizeof(struct cache_meta) * PREFETCHD_CACHE_PAGE_COUNT);
	if (cache_metas == NULL)
		goto free_content;

	hdd_client = dm_io_client_create();
	if (IS_ERR(hdd_client))
		goto free_metas;

	ssd_client = dm_io_client_create();
	if (IS_ERR(ssd_client))
		goto free_hdd_client;

	map_stack = (struct cache_meta_map_stack *)
		vmalloc(sizeof(struct cache_meta_map_stack));
	if (map_stack == NULL)
		goto free_ssd_client;

	for (i = 0; i < PREFETCHD_CACHE_PAGE_COUNT; i++) {
		cache_metas[i].status = empty;
	}

	init_map_stack();

	DPPRINTK("prefetchd_cache initialized.");
	return true;

free_ssd_client:
	dm_io_client_destroy(ssd_client);

free_hdd_client:
	dm_io_client_destroy(hdd_client);

free_metas:
	vfree((void *)cache_metas);

free_content:
	vfree(cache_content);

fail_log:
	DPPRINTK("prefetchd_cache initialize failed.");
	return false;
}

void prefetchd_cache_exit() {
	vfree(cache_content);
	vfree((void *)cache_metas);
	dm_io_client_destroy(hdd_client);
	dm_io_client_destroy(ssd_client);
	vfree((void *)map_stack);
}

bool prefetchd_cache_handle_bio(struct bio *bio) {
	struct cache_meta_map map;
	struct cache_meta *meta;
	long flags;
	int i;
	u64 sector_num;
	void *data_src;
	void *data_dest;
	struct bio_vec bvec;
	struct bvec_iter iter;

	if (!is_bio_fit_cache(bio)) return false;

	get_cache_meta_map(
		bio->bi_iter.bi_sector,
		bio->bi_iter.bi_size,
		&map);

	spin_lock_irqsave(&cache_global_lock, flags);

	cache_meta_map_foreach(map, meta, i) {
		if (meta->status == empty)
			goto cache_miss;
		sector_num = bio->bi_iter.bi_sector + (i << (PAGE_SHIFT - 9));
		if (sector_num != meta->sector_num)
			goto cache_miss;
	}

	cache_meta_map_foreach(map, meta, i) {
		atomic_inc(&(meta->hold_count));
	}

	spin_unlock_irqrestore(&cache_global_lock, flags);

	cache_meta_map_foreach(map, meta, i) {
		if (meta->status == prepare) {
			down_interruptible(&(meta->prepare_lock));
			up(&(meta->prepare_lock));
		}
	}

	data_src = bio->bi_iter.bi_sector << 9;
	bio_for_each_segment(bvec, bio, iter) {
		data_dest = kmap(bvec.bv_page) + bvec.bv_offset;
		memcpy(data_dest, data_src, bvec.bv_len);
		data_src += bvec.bv_len;
	}

	bio_endio(bio);

	cache_meta_map_foreach(map, meta, i) {
		atomic_dec(&(meta->hold_count));
	}

	DPPRINTK("cache hit: %llu+%u",
		bio->bi_iter.bi_sector,
		bio->bi_iter.bi_size >> 9);

	return true;

cache_miss:
	spin_unlock_irqrestore(&cache_global_lock, flags);
	DPPRINTK("cache miss: %llu+%u",
		bio->bi_iter.bi_sector,
		bio->bi_iter.bi_size >> 9);

	return false;
}

static int 
get_prefetch_cache_count(
		struct cache_c *dmc,
		struct prefetchd_stat_info *info) {
	u64 ret;
	u64 last_page_count = (u64)info->last_size >> PAGE_SHIFT;

	switch (info->status) {
	case sequential_forward:
		ret = info->last_sector_num + (info->last_size >> 9);
		ret = dmc->tgt->len - ret;
		ret /= (u64)(info->last_size >> 9);
		break;
	case sequential_backward:
		ret = info->last_sector_num - dmc->tgt->begin;
		ret /= (u64)(info->last_size >> 9);
		break;
	case stride_forward:
		ret = info->last_sector_num + info->stride_count;
		if (ret >= dmc->tgt->len) {
			ret = 0;
			break;
		}
		ret = dmc->tgt->len - ret;
		ret = ret / info->stride_count;
		break;
	case stride_backward:
		ret = info->last_sector_num - dmc->tgt->begin;
		ret = ret / info->stride_count;
		break;
	}

	ret = ret > (u64)(info->credibility) ?
		(u64)(info->credibility) : ret;

	ret = last_page_count * ret >
		PREFETCHD_CACHE_MAX_PAGE_COUNT_PER_CACHE ?
		PREFETCHD_CACHE_MAX_PAGE_COUNT_PER_CACHE / last_page_count :
		ret;

	return (int)ret;
}

inline static void get_stride_prefetch_step(
		struct prefetchd_stat_info *info,
		int idx,
		u64 *sector_num,
		unsigned int *size
		) {
	*size = info->last_size;
	switch (info->status) {
	case stride_forward:
		*sector_num = info->last_sector_num + 
			info->stride_count * (idx + 1);
		break;
	case stride_backward:
		*sector_num =  info->last_sector_num -
			info->stride_count * (idx + 1);
		break;
	default:
		*size = 0;
	}
}

inline static void get_seq_prefetch_step(
		struct prefetchd_stat_info *info,
		int idx,
		u64 *sector_num,
		unsigned int *size
		) {
	*size = (idx + 1) * info->last_size;
	switch (info->status) {
	case sequential_forward:
		*sector_num = info->last_sector_num + (u64)((*size) >> 9);
		break;
	case sequential_backward:
		*sector_num = info->last_sector_num - (u64)((*size) >> 9);
		break;
	default:
		*size = 0;
	}
}

static void io_callback(unsigned long error, void *context) {
	struct cache_meta_map_stack_elm *elm
		= (struct cache_meta_map_stack_elm *)context;
	struct cache_meta_map *map = &(elm->map);
	struct cache_meta *meta;
	int i;
	long flags;

	spin_lock_irqsave(&cache_global_lock, flags);

	cache_meta_map_foreach(*map, meta, i) {
		meta->status = active;
		up(&(meta->prepare_lock));
	}

	push_map_stack(elm);
	spin_unlock_irqrestore(&cache_global_lock, flags);

	DPPRINTK("io_callback: %ld", error);
}

static void alloc_prefetch(
		struct cache_c *dmc,
		struct bio *tmp_bio,
		int *index,
		u64 sector_num,
		struct cache_meta_map *map
		) {
	int i;
	struct cache_meta *meta;
	struct dm_io_request req;
	struct dm_io_region region;
	int dm_io_ret;
	struct cache_meta_map_stack_elm *map_elm;
	long flags;

	if (index != NULL) return;

	map_elm = pop_map_stack();
	if (map_elm == NULL) {
		DPPRINTK("map_stack leak.");
		return;
	}
	map_elm->map = *map;

	cache_meta_map_foreach(*map, meta, i) {
		meta->sector_num = sector_num + ((u64)i << (PAGE_SHIFT - 9));
		meta->status = prepare;
		sema_init(&(meta->prepare_lock), 0);
		atomic_set(&(meta->hold_count), 0);
		meta->dmc = dmc;
		if (tmp_bio != NULL) {
			meta->tmp_bio = *tmp_bio;
		}
		if (index != NULL) {
			meta->index = *index;
			meta->from_ssd = true;
		} else {
			meta->from_ssd = false;
		}
	}

	if (index == NULL) {
		// HDD case
		req.bi_op = READ;
		req.bi_op_flags = 0;
		req.notify.fn = (io_notify_fn)io_callback;
		req.notify.context = (void *)map_elm;
		req.client = hdd_client;
		req.mem.type = DM_IO_VMA;
		req.mem.offset = 0;
		req.mem.ptr.vma = (void *)cache_content + 
			((u64)(map->index) << PAGE_SHIFT);

		region.bdev = dmc->disk_dev->bdev;
		region.sector = sector_num;
		region.count = (u64)(map->count) << (PAGE_SHIFT - 9);
	} else {
		// SSD case
	}

	dm_io_ret = dm_io(&req, 1, &region, NULL);
	if (dm_io_ret) {
		cache_meta_map_foreach(*map, meta, i) {
			meta->status = empty;
		}
	}

	DPPRINTK("prefetch (%llu+%d) on %s: %s.",
			sector_num,
			(map->count) << (PAGE_SHIFT - 9),
			index == NULL ? "HDD" : "SSD",
			dm_io_ret ? "Failed" : "Sent");
}

void prefetchd_do_prefetch(
		struct cache_c *dmc,
		struct prefetchd_stat_info *info
		) {
	struct cache_meta_map map;
	struct cache_meta *meta;
	struct bio tmp_bio;
	struct cacheblock *cacheblk;
	int prefetch_count;
	int i, j;
	int lookup_index;
	int lookup_res;
	long flags;
	u64 sector_num;
	unsigned int size;

	if (info->status <= 2) return;

	if (!is_request_fit_cache(
		info->last_sector_num,
		info->last_size)) return;

	prefetch_count = get_prefetch_cache_count(dmc, info);
	if (prefetch_count <= 0) return;

	// check mem cache
	switch (info->status) {
	case sequential_forward:
	case sequential_backward:
		get_seq_prefetch_step(info, 0, &sector_num, &size);
		break;
	case stride_forward:
	case stride_backward:
		get_stride_prefetch_step(info, 0, &sector_num, &size);
		break;
	}
	get_cache_meta_map(
			sector_num,
			size,
			&map);
	spin_lock_irqsave(&cache_global_lock, flags); // lock
	cache_meta_map_foreach(map, meta, i) {
		if (!is_meta_match(meta, sector_num))
			goto mem_miss;
	}
	// mem cache hit
	spin_unlock_irqrestore(&cache_global_lock, flags); // unlock
	DPPRINTK("prefetch already exist. (%llu+%u)",
			info->last_sector_num,
			info->last_size >> 9);
	return;

mem_miss:
	// check cache_metas available
	cache_meta_map_foreach(map, meta, i) {
		if (!is_meta_removable(meta)) {
			// can't prefetch
			spin_unlock_irqrestore(&cache_global_lock, flags); // unlock
			DPPRINTK("not enough room to prefetch. (%llu+%u)",
					info->last_sector_num,
					info->last_size >> 9);
			return;
		}
	}

	// check ssd content
	tmp_bio.bi_iter.bi_sector = sector_num;
	tmp_bio.bi_iter.bi_size = size;
	ex_flashcache_setlocks_multiget(dmc, &tmp_bio);
	lookup_res = ex_flashcache_lookup(dmc, &tmp_bio, &lookup_index);
	if (lookup_res > 0) {
		cacheblk = &dmc->cache[lookup_index];
		if ((cacheblk->cache_state & VALID) && 
		    (cacheblk->dbn == tmp_bio.bi_iter.bi_sector)) {
			alloc_prefetch(
					dmc,
					&tmp_bio,
					&lookup_index,
					sector_num,
					&map);
			spin_unlock_irqrestore(&cache_global_lock, flags); // unlock
			return;
		}
	}

	// prefetch on hdd
	ex_flashcache_setlocks_multidrop(dmc, &tmp_bio);

	switch (info->status) {
	case sequential_forward:
	case sequential_backward:
		// seq case
		get_seq_prefetch_step(
				info,
				prefetch_count - 1,
				&sector_num,
				&size);
		get_cache_meta_map(
				sector_num,
				size,
				&map);
		// check metas is available
		cache_meta_map_foreach(map, meta, i) {
			if (!is_meta_removable(meta)) {
				spin_unlock_irqrestore(&cache_global_lock, flags); // unlock
				DPPRINTK("not enough room to prefetch. (%llu+%u)",
						sector_num,
						size >> 9);
				return;
			}
		}
		alloc_prefetch(
				dmc,
				NULL,
				NULL,
				sector_num,
				&map);
		break;
	default:
		// stride case
		// check metas available
		for (j = 1 /* first have checked */; j < prefetch_count; j++) {
			get_stride_prefetch_step(
					info,
					j,
					&sector_num,
					&size);
			get_cache_meta_map(
					sector_num,
					size,
					&map);
			cache_meta_map_foreach(map, meta, i) {
				if (!is_meta_removable(meta)) {
					spin_unlock_irqrestore(&cache_global_lock, flags); // unlock
					DPPRINTK("not enough room to prefetch. (%llu+%u)",
							sector_num,
							size >> 9);
					return;
				}
			}
		}
		// make req
		if (info->status == stride_forward) {
			for (j = 0; j < prefetch_count; j++) {
				get_stride_prefetch_step(
						info,
						j,
						&sector_num,
						&size);
				get_cache_meta_map(
						sector_num,
						size,
						&map);
				cache_meta_map_foreach(map, meta, i) {
					alloc_prefetch(
							dmc,
							NULL,
							NULL,
							sector_num,
							&map);
				}
			}
		} else {
			for (j = prefetch_count - 1; j >= 0; j--) {
				get_stride_prefetch_step(
						info,
						j,
						&sector_num,
						&size);
				get_cache_meta_map(
						sector_num,
						size,
						&map);
				cache_meta_map_foreach(map, meta, i) {
					alloc_prefetch(
							dmc,
							NULL,
							NULL,
							sector_num,
							&map);
				}
			}
		}
	}
	spin_unlock_irqrestore(&cache_global_lock, flags); // unlock
}
