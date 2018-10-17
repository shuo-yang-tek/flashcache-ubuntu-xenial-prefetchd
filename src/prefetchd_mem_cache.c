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
#include "./prefetchd_mem_cache.h"

extern void flashcache_setlocks_multiget(struct cache_c *dmc, struct bio *bio);
extern void flashcache_setlocks_multidrop(struct cache_c *dmc, struct bio *bio);
extern int flashcache_lookup(struct cache_c *dmc, struct bio *bio, int *index);

DEFINE_SPINLOCK(mem_cache_global_lock);

enum mem_cache_status {
	empty = 1,
	prepare,
	active,
};

struct mem_cache {
	u64 sector_num;
	unsigned int size;
	void *data;

	enum mem_cache_status status;
	struct semaphore lock;
	atomic_t hold_count;
	atomic_t used_count;

	// for droping lock
	struct bio bio;
	struct cache_c *dmc;
};

struct mem_cache_list_elm {
	struct mem_cache *mem_cache;
	struct mem_cache_list_elm *prev;
	struct mem_cache_list_elm *next;
};

struct mem_cache_list {
	struct mem_cache_list_elm pool[MEM_CACHE_COUNT];
	struct mem_cache_list_elm *head;
	struct mem_cache_list_elm *tail;
	int count;
};

static void init_mem_cache_list(struct mem_cache_list *list) {
	int i;
	struct mem_cache_list_elm *elm;

	list->count = 0;
	list->head = NULL;
	list->tail = NULL;

	for (i = 0; i < MEM_CACHE_COUNT; i++) {
		elm = &(list->pool[i]);
		elm->mem_cache = NULL;
		elm->prev = NULL;
		elm->next = NULL;
	}
}

static struct mem_cache_list_elm *mem_cache_list_get_free(struct mem_cache_list *list) {
	int i;
	struct mem_cache_list_elm *elm;

	if (list->count >= MEM_CACHE_COUNT) return NULL;

	for (i = 0; i < MEM_CACHE_COUNT; i++) {
		elm = &(list->pool[i]);
		if (elm->mem_cache == NULL) return elm;
	}

	return NULL;
}

static bool mem_cache_list_insert_head(struct mem_cache_list *list, struct mem_cache *item) {
	struct mem_cache_list_elm *elm = mem_cache_list_get_free(list);

	if (elm == NULL) return false;
	elm->mem_cache = item;

	elm->prev = NULL;
	elm->next = list->head;

	if (elm->next != NULL)
		elm->next->prev = elm;
	if (++(list->count) == 1)
		list->tail = elm;

	list->head = elm;
	return true;
}

static bool mem_cache_list_insert_tail(struct mem_cache_list *list, struct mem_cache *item) {
	struct mem_cache_list_elm *elm = mem_cache_list_get_free(list);

	if (elm == NULL) return false;
	elm->mem_cache = item;

	elm->prev = list->tail;
	elm->next = NULL;

	if (elm->prev != NULL)
		elm->prev->next = elm;
	if (++(list->count) == 1)
		list->head = elm;

	list->tail = elm;
	return true;
}

static struct mem_cache *mem_cache_list_remove(struct mem_cache_list *list, struct mem_cache_list_elm *elm) {
	struct mem_cache *ret;

	if (list->count <= 0) return NULL;

	if (elm->prev != NULL)
		elm->prev->next = elm->next;
	if (elm->next != NULL)
		elm->next->prev = elm->prev;
	
	if (elm == list->head)
		list->head = elm->next;
	if (elm == list->tail)
		list->tail = elm->prev;

	ret = elm->mem_cache;

	elm->prev = NULL;
	elm->next = NULL;
	elm->mem_cache = NULL;

	list->count -= 1;

	return ret;
}

static struct mem_cache mem_cache_pool[MEM_CACHE_COUNT];
static struct mem_cache_list mem_cache_used_list;
static struct mem_cache_list mem_cache_free_list;

void prefetchd_mem_cache_init() {
	int i;

	init_mem_cache_list(&mem_cache_used_list);
	init_mem_cache_list(&mem_cache_free_list);

	for (i = 0; i < MEM_CACHE_COUNT; i++) {
		mem_cache_pool[i].status = empty;
		mem_cache_list_insert_tail(&mem_cache_free_list, &(mem_cache_pool[i]));
	}

	DPPRINTK("mem_cache initialized.");
}

bool prefetchd_mem_cache_handle_bio(struct bio *bio) {
	long flags;
	struct mem_cache_list_elm *elm;
	struct mem_cache *mem_cache;
	u64 bio_start = bio->bi_iter.bi_sector << 9;
	u64 bio_end = bio_start + bio->bi_iter.bi_size;
	u64 cache_start, cache_end;
	bool need_sleep;
	struct bio_vec bvec;
	struct bvec_iter iter;
	void *data_src;
	void *data_dest;

	if (bio->bi_iter.bi_size > SIZE_PER_MEM_CACHE) return false;
	spin_lock_irqsave(&mem_cache_global_lock, flags);

	elm = mem_cache_used_list.head;
	while (elm != NULL) {
		mem_cache = elm->mem_cache;

		cache_start = mem_cache->sector_num << 9;
		cache_end = cache_start + mem_cache->size;

		if (cache_start <= bio_start && cache_end >= bio_end)
			break;

		elm = elm->next;
	}

	if (elm == NULL) {
		spin_unlock_irqrestore(&mem_cache_global_lock, flags);
		return false;
	}

	if (mem_cache->status == prepare)
		need_sleep = true;

	atomic_inc(&(mem_cache->hold_count));
	atomic_inc(&(mem_cache->used_count));

	spin_unlock_irqrestore(&mem_cache_global_lock, flags);

	if (need_sleep) {
		down_interruptible(&(mem_cache->lock));
		up(&(mem_cache->lock));
	}

	data_src = mem_cache->data + (bio_start - cache_start);
	bio_for_each_segment(bvec, bio, iter) {
		data_dest = kmap(bvec.bv_page) + bvec.bv_offset;
		memcpy(data_dest, data_src, bvec.bv_len);
		data_src += bvec.bv_len;
	}
	bio_endio(bio);

	atomic_dec(&(mem_cache->hold_count));
	DPPRINTK("MEM_CACHE Hit.");
	return true;
}

static int get_mem_cache_count(struct cache_c *dmc, struct prefetchd_stat_info *stat_info) {
	u64 disk_start = (dmc->tgt->begin) << 9;
	u64 disk_len = (dmc->tgt->len) << 9;
	u64 a, b, result, result2;

	switch (stat_info->status) {
	case sequential_forward:
		a = disk_len - ((stat_info->last_sector_num << 9) + ((u64)(stat_info->last_size)));
		b = (u64)(stat_info->last_size);
		break;
	case sequential_backward:
		a = (stat_info->last_sector_num << 9) - disk_start;
		b = (u64)(stat_info->last_size);
		break;
	case stride_forward:
		a = disk_len - ((stat_info->last_sector_num + stat_info->stride_count) << 9);
		b = stat_info->stride_count;
		break;
	case stride_backward:
		a = (stat_info->last_sector_num << 9) - disk_start;
		b = stat_info->stride_count;
		break;
	default:
		return -1;
	}

	result = a / b;

	switch (stat_info->status) {
	case sequential_forward:
	case sequential_backward:
		result2 = SIZE_PER_MEM_CACHE / (u64)stat_info->last_size;
		result = result > result2 ? result2 : result;
		break;
	case stride_forward:
	case stride_backward:
		result = result > MAX_MEM_CACHE_COUNT_PER_PREFETCH ?
			MAX_MEM_CACHE_COUNT_PER_PREFETCH : result;
		break;
	}

	return (int) result;
}

static void request_mem_cache(struct mem_cache *tar) {
}

static bool mem_cache_alloc(
	struct cache_c *dmc,
	struct prefetchd_stat_info *stat_info,
	struct bio *tmp_bio,
	int *index,
	int count
) {
	/*
	 * duplicate check NOT implement
	 */

	struct mem_cache *mem_caches[MAX_MEM_CACHE_COUNT_PER_PREFETCH];
	struct mem_cache *tar;
	long flags;
	int i;
	int need_count;
	u64 sector_num;
	unsigned int size;
	struct bio bio_content;

	if (tmp_bio == NULL) {
		bio_content.bi_iter.bi_size = 0;
	} else {
		bio_content = *tmp_bio;
	}

	switch (stat_info->status) {
	case sequential_forward:
	case sequential_backward:
		need_count = 1;
		break;
	case stride_forward:
	case stride_backward:
		need_count = count;
		break;
	default:
		return false;
	}

	spin_lock_irqsave(&mem_cache_global_lock, flags);

	if (mem_cache_free_list.count < need_count) {
		spin_unlock_irqrestore(&mem_cache_global_lock, flags);
		DPPRINTK("cache slot not enough.");
		return false;
	}

	for (i = 0; i < need_count; i++) {
		mem_caches[i] = mem_cache_list_remove(
			&mem_cache_free_list,
			mem_cache_free_list.head);
		mem_cache_list_insert_tail(
			&mem_cache_used_list,
			mem_caches[i]);
		mem_caches[i]->status = prepare;
		sema_init(&(mem_caches[i]->lock), 0);
		atomic_set(&(mem_caches[i]->hold_count), 0);
		atomic_set(&(mem_caches[i]->used_count), 0);
	}

	spin_unlock_irqrestore(&mem_cache_global_lock, flags);

	switch (stat_info->status) {
	case sequential_forward:
	case sequential_backward:
		size = stat_info->last_size * (unsigned int)count;
		break;
	case stride_forward:
	case stride_backward:
		size = stat_info->last_size;
		break;
	}

	for (i = 0; i < need_count; i++) {
		switch (stat_info->status) {
		case sequential_forward:
			sector_num = stat_info->last_sector_num + ((u64)stat_info->last_size >> 9);
			break;
		case sequential_backward:
			sector_num = stat_info->last_sector_num - ((u64)size >> 9);
			break;
		case stride_forward:
			sector_num = stat_info->last_sector_num + 
				stat_info->stride_count * (u64)(i + 1);
			break;
		case stride_backward:
			sector_num = stat_info->last_sector_num -
				stat_info->stride_count * (u64)(i + 1);
			break;
		}

		tar = mem_caches[i];
		tar->sector_num = sector_num;
		tar->size = size;
		tar->dmc = dmc;
		tar->bio = bio_content;
		tar->data = vmalloc((unsigned long)size);
	}

	switch (stat_info->status) {
	case sequential_forward:
	case stride_forward:
		for (i = 0; i < need_count; i++) {
			request_mem_cache(mem_caches[i]);
		}
		break;
	default:
		for (i = need_count - 1; i >= 0; i--) {
			request_mem_cache(mem_caches[i]);
		}
	}

	return true;
}

bool prefetchd_mem_cache_create(struct cache_c *dmc, struct prefetchd_stat_info *stat_info) {
	struct bio tmp_bio;
	int lookup_index, lookup_res;
	int max_mem_cache_count;
	struct cacheblock *cacheblk;

	// basic check
	if (stat_info->status <= 2)
		return false;
	if (stat_info->last_size > SIZE_PER_MEM_CACHE)
		return false;

	max_mem_cache_count = get_mem_cache_count(dmc, stat_info);
	if (max_mem_cache_count <= 0) return false;

	// ssd check
	switch (stat_info->status) {
	case sequential_forward:
		tmp_bio.bi_iter.bi_sector = 
			stat_info->last_sector_num +
			(u64)(stat_info->last_size >> 9);
		break;
	case sequential_backward:
		tmp_bio.bi_iter.bi_sector = 
			stat_info->last_sector_num - 
			(u64)(stat_info->last_size >> 9);
		break;
	case stride_forward:
		tmp_bio.bi_iter.bi_sector =
			stat_info->last_sector_num +
			stat_info->stride_count;
		break;
	case stride_backward:
		tmp_bio.bi_iter.bi_sector =
			stat_info->last_sector_num -
			stat_info->stride_count;
		break;
	default:
		return false;
	}
	tmp_bio.bi_iter.bi_size = stat_info->last_size;
	flashcache_setlocks_multiget(dmc, &tmp_bio);
	lookup_res = flashcache_lookup(dmc, &tmp_bio, &lookup_index);
	if (lookup_res > 0) {
		cacheblk = &dmc->cache[lookup_index];
		if ((cacheblk->cache_state & VALID) && 
		    (cacheblk->dbn == tmp_bio.bi_iter.bi_sector)) {
			return mem_cache_alloc(dmc, stat_info, &tmp_bio, &lookup_index, 1);
		}
	}
	flashcache_setlocks_multidrop(dmc, &tmp_bio);

	max_mem_cache_count = 
		max_mem_cache_count > (int)(stat_info->credibility) ?
		(int)(stat_info->credibility) :
		max_mem_cache_count;

	return mem_cache_alloc(dmc, stat_info, NULL, NULL, max_mem_cache_count);
}
