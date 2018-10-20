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
			 (meta) = &(cache_metas[((i) + (map).index) % PREFETCHD_CACHE_PAGE_COUNT]))

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
DEFINE_SPINLOCK(cache_global_lock_for_interrupt);

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

struct callback_context {
	struct cache_meta_map map;
	struct callback_context *next;
};

struct callback_context_stack {
	struct callback_context pool[PREFETCHD_CACHE_PAGE_COUNT];
	struct callback_context *head;
	int count;
};

static void *cache_content;
static struct cache_meta *cache_metas;
static struct dm_io_client *hdd_client;
static struct dm_io_client *ssd_client;
static struct callback_context_stack *callback_contexts;

static void init_callback_contexts(void) {
	int i;

	for (i = 0; i < PREFETCHD_CACHE_PAGE_COUNT; i++) {
		callback_contexts->pool[i].next =
			i == PREFETCHD_CACHE_PAGE_COUNT - 1 ?
			NULL :
			&(callback_contexts->pool[i + 1]);
	}

	callback_contexts->head = &(callback_contexts->pool[0]);
	callback_contexts->count = PREFETCHD_CACHE_PAGE_COUNT;
}

static struct callback_context *pop_callback_contexts(void) {
	struct callback_context *ret;

	if (callback_contexts->count <= 0) return NULL;
	ret = callback_contexts->head;
	callback_contexts->head = callback_contexts->head->next;
	ret->next = NULL;
	callback_contexts->count -= 1;
	return ret;
}

static void push_callback_contexts(struct callback_context *elm) {
	elm->next = callback_contexts->head;
	callback_contexts->head = elm;
	callback_contexts->count += 1;
}

bool prefetchd_cache_init() {
	int i;

	cache_content = vmalloc(PREFETCHD_CACHE_PAGE_COUNT << PAGE_SHIFT);
	if (cache_content == NULL)
		goto fail_log;

	hdd_client = dm_io_client_create();
	if (IS_ERR(hdd_client))
		goto free_content;

	ssd_client = dm_io_client_create();
	if (IS_ERR(ssd_client))
		goto free_hdd_client;

	cache_metas = vmalloc(sizeof(struct cache_meta) * PREFETCHD_CACHE_PAGE_COUNT);
	if (cache_metas == NULL)
		goto free_ssd_client;

	callback_contexts = vmalloc(sizeof(struct callback_context_stack));
	if (callback_contexts == NULL)
		goto free_metas;

	for (i = 0; i < PREFETCHD_CACHE_PAGE_COUNT; i++) {
		cache_metas[i].status = empty;
	}

	init_callback_contexts();

	DPPRINTK("prefetchd_cache initialized.");
	return true;

free_metas:
	vfree(cache_metas);

free_ssd_client:
	dm_io_client_destroy(ssd_client);

free_hdd_client:
	dm_io_client_destroy(hdd_client);

free_content:
	vfree(cache_content);

fail_log:
	DPPRINTK("prefetchd_cache initialize failed.");
	return false;
}

void prefetchd_cache_exit() {
	vfree(cache_content);
	dm_io_client_destroy(hdd_client);
	dm_io_client_destroy(ssd_client);
	vfree(cache_metas);
	vfree(callback_contexts);
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
	u64 src_offset;
	u64 src_size = PREFETCHD_CACHE_PAGE_COUNT << PAGE_SHIFT;
	u64 cpy_end;
	u64 tmp1, tmp2;
	bool need_revert = false;

	if (!is_bio_fit_cache(bio)) return false;

	get_cache_meta_map(
		bio->bi_iter.bi_sector,
		bio->bi_iter.bi_size,
		&map);

	spin_lock_irqsave(&cache_global_lock, flags);

	cache_meta_map_foreach(map, meta, i) {
		if (meta->status == empty)
			goto cache_miss;
		sector_num = bio->bi_iter.bi_sector + ((u64)i << (PAGE_SHIFT - 9));
		if (sector_num != meta->sector_num)
			goto cache_miss;
	}

	cache_meta_map_foreach(map, meta, i) {
		atomic_inc(&(meta->hold_count));
	}

	spin_unlock_irqrestore(&cache_global_lock, flags);

	cache_meta_map_foreach(map, meta, i) {
		if (meta->status == prepare) {
			down(&(meta->prepare_lock));
			up(&(meta->prepare_lock));
			if (meta->status != active) {
				need_revert = true;
				break;
			}
		}
	}

	if (need_revert) {
		cache_meta_map_foreach(map, meta, i) {
			atomic_dec(&(meta->hold_count));
		}
		goto cache_miss_no_unlock;
	}

	src_offset = ((u64)(map.index) << PAGE_SHIFT);
	data_src = cache_content + src_offset;
	bio_for_each_segment(bvec, bio, iter) {
		data_dest = kmap(bvec.bv_page) + bvec.bv_offset;
		cpy_end = src_offset + (u64)(bvec.bv_len);
		if (cpy_end <= src_size) {
			memcpy(data_dest, data_src, bvec.bv_len);
		} else {
			tmp1 = cpy_end - src_size;
			tmp2 = (u64)(bvec.bv_len) - tmp1;
			if (tmp2 > 0);
				memcpy(data_dest, data_src, tmp2);
			if (tmp1 > 0)
				memcpy(data_dest + tmp2, cache_content, tmp1);
		}
		kunmap(bvec.bv_page);
		src_offset += bvec.bv_len;
		if (src_offset >= src_size)
			src_offset = src_offset % src_size;
		data_src = cache_content + src_offset;
	}

	bio_endio(bio);

	cache_meta_map_foreach(map, meta, i) {
		atomic_dec(&(meta->hold_count));
	}

	DPPRINTK("\033[1;33mcache hit: %llu+%u",
		bio->bi_iter.bi_sector,
		bio->bi_iter.bi_size >> 9);

	return true;

cache_miss:
	spin_unlock_irqrestore(&cache_global_lock, flags);
cache_miss_no_unlock:
	DPPRINTK("\033[0;32;34mcache miss: %llu+%u",
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
	u64 last_sector_count = (u64)info->last_size >> 9;
	u64 disk_sect_count = dmc->disk_dev->bdev->bd_part->nr_sects;
	u64 cache_sect_count = PREFETCHD_CACHE_PAGE_COUNT << (PAGE_SHIFT - 9);
	u64 tmp;

	switch (info->status) {
	case sequential_forward:
		ret = info->last_sector_num + last_sector_count;
		ret = ret >= disk_sect_count ? 0 : (disk_sect_count - ret) / last_sector_count;
		break;
	case sequential_backward:
		ret = info->last_sector_num / last_sector_count;
		break;
	case stride_forward:
		ret = info->last_sector_num + last_sector_count;
		ret = ret >= disk_sect_count ? 0 : (disk_sect_count - ret) / info->stride_count;
		break;
	case stride_backward:
		ret = info->last_sector_num / info->stride_count;
		break;
	}

	switch (info->status) {
	case stride_forward:
	case stride_backward:
		tmp = cache_sect_count / info->stride_count;
		ret = ret > tmp ? tmp : ret;
		break;
	}

	ret = ret > (s64)(info->credibility) ?
		(s64)(info->credibility) : ret;

	ret = (s64)last_page_count * ret >
		PREFETCHD_CACHE_MAX_PAGE_COUNT_PER_CACHE ?
		(s64)(PREFETCHD_CACHE_MAX_PAGE_COUNT_PER_CACHE / last_page_count) :
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
			info->stride_count * ((u64)idx + 1);
		break;
	case stride_backward:
		*sector_num =  info->last_sector_num -
			info->stride_count * ((u64)idx + 1);
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
		*sector_num = info->last_sector_num + (u64)(info->last_size >> 9);
		break;
	case sequential_backward:
		*sector_num = info->last_sector_num - (u64)((*size) >> 9);
		break;
	default:
		*size = 0;
	}
}

static void io_callback(unsigned long error, void *context) {
	struct callback_context *elm;
	struct cache_meta_map *map;
	struct cache_meta *meta;
	int i;
	enum cache_status status;

	elm = (struct callback_context *)context;
	map = &(elm->map);

	status = error ? empty : active;

	spin_lock(&cache_global_lock_for_interrupt);

	cache_meta_map_foreach(*map, meta, i) {
		meta->status = status;
		up(&(meta->prepare_lock));
	}

	push_callback_contexts(elm);
	spin_unlock(&cache_global_lock_for_interrupt);

	DPPRINTK("%sio_callback. (%llu+%u)",
			error ? "\033[0;32;31m" : "",
			cache_metas[map->index].sector_num,
			map->count << (PAGE_SHIFT - 9));
}

static void alloc_prefetch(
		struct cache_c *dmc,
		struct bio *tmp_bio,
		int *index,
		u64 sector_num,
		struct cache_meta_map *map
		) {
	int i, j;
	struct cache_meta *meta;
	struct dm_io_request req[2];
	struct dm_io_region region[2];
	int dm_io_ret;
	struct callback_context *map_elm[2];
	long flags;
	int req_count = 
		map->index + map->count > PREFETCHD_CACHE_PAGE_COUNT ?
		2 : 1;
	bool from_ssd = index == NULL ? false : true;

	/*if (from_ssd) {*/
		/*ex_flashcache_setlocks_multidrop(dmc, tmp_bio);*/
		/*return;*/
	/*}*/

	for (i = 0; i < req_count; i++) {
		map_elm[i] = pop_callback_contexts();
		if (map_elm[i] == NULL) {
			DPPRINTK("callback_contexts leak.");
			for (j = 0; j < i; j++)
				push_callback_contexts(map_elm[j]);
			return;
		}
	}

	map_elm[0]->map = *map;
	if (req_count > 1) {
		map_elm[0]->map.count = map->index + map->count - PREFETCHD_CACHE_PAGE_COUNT;
		map_elm[1]->map.index = 0;
		map_elm[1]->map.count = map->count - map_elm[0]->map.count;
	}

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

	for (i = 0; i < req_count; i++) {
		req[i].bi_op = READ;
		req[i].bi_op_flags = 0;
		req[i].notify.fn = (io_notify_fn)io_callback;
		req[i].notify.context = (void *)map_elm[i];
		req[i].client = index == NULL ? hdd_client : ssd_client;
		req[i].mem.type = DM_IO_VMA;
		req[i].mem.offset = 0;
		req[i].mem.ptr.vma = (void *)cache_content +
			((u64)map_elm[i]->map.index) << PAGE_SHIFT;
	}

	if (!from_ssd) {
		// HDD case
		for (i = 0; i < req_count; i++) {
			region[i].bdev = dmc->disk_dev->bdev;
			region[i].sector = map_elm[i]->map.index << (PAGE_SHIFT - 9);
			region[i].count = map_elm[i]->map.count << (PAGE_SHIFT - 9);
		}
	} else {
		// SSD case
	}

	for (i = 0; i < req_count; i++) {
		dm_io_ret = dm_io(&req[i], 1, &region[i], NULL);
		if (dm_io_ret) {
			cache_meta_map_foreach(map_elm[i]->map, meta, j) {
				meta->status = empty;
			}
		}
		DPPRINTK("\033[0;32;31mdm_io return: %d", dm_io_ret);
	}

	DPPRINTK("prefetch (%llu+%d) on %s: %s.",
			sector_num,
			(map->count) << (PAGE_SHIFT - 9),
			!from_ssd ? "HDD" : "SSD",
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
	/*tmp_bio.bi_iter.bi_sector = sector_num;*/
	/*tmp_bio.bi_iter.bi_size = size;*/
	/*ex_flashcache_setlocks_multiget(dmc, &tmp_bio);*/
	/*lookup_res = ex_flashcache_lookup(dmc, &tmp_bio, &lookup_index);*/
	/*if (lookup_res > 0) {*/
		/*cacheblk = &dmc->cache[lookup_index];*/
		/*if ((cacheblk->cache_state & VALID) && */
				/*(cacheblk->dbn == tmp_bio.bi_iter.bi_sector)) {*/
			/*alloc_prefetch(*/
					/*dmc,*/
					/*&tmp_bio,*/
					/*&lookup_index,*/
					/*sector_num,*/
					/*&map);*/
			/*spin_unlock_irqrestore(&cache_global_lock, flags); // unlock*/
			/*return;*/
		/*}*/
	/*}*/

	/*// prefetch on hdd*/
	/*ex_flashcache_setlocks_multidrop(dmc, &tmp_bio);*/

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
