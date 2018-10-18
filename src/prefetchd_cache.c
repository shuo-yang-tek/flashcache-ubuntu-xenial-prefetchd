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
	for ((i) = 0, (meta) = cache_metas[(map).index]; \
			 (i) < (map).count; \
			 ++(i), \
			 (meta) = cache_metas[((i) + (map).index) % PREFETCHD_CACHE_PAGE_COUNT])

extern void flashcache_setlocks_multiget(struct cache_c *dmc, struct bio *bio);
extern void flashcache_setlocks_multidrop(struct cache_c *dmc, struct bio *bio);
extern int flashcache_lookup(struct cache_c *dmc, struct bio *bio, int *index);

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
};

struct cache_meta_map {
	int index;
	int count;
};

inline static unsigned int size_to_page_count(unsigned int size) {
	return (PAGE_SIZE - (size % PAGE_SIZE) + size) >> PAGE_SHIFT;
}

inline static void
get_cache_meta_map(u64 sector_num, unsigned int size, struct cache_meta_map *res) {
	long len = (long)size - (long)(PAGE_SIZE - ((sector_num << 9) % PAGE_SIZE));
	res->index = sector_num >> (PAGE_SHIFT - 9);
	if (len <= 0)
		res->count = 1;
	else
		res->count = size_to_page_count(len);
}

static void *cache_content;
static struct cache_meta *cache_metas;

bool prefetchd_cache_init() {
	int i;

	cache_content = vmalloc(PREFETCHD_CACHE_PAGE_COUNT << PAGE_SHIFT);
	if (cache_content == NULL)
		goto fail_log;

	cache_metas = 
		(struct cache_meta *)
		vmalloc(sizeof(cache_meta) * PREFETCHD_CACHE_PAGE_COUNT);
	if (cache_metas == NULL)
		goto free_metas;

	for (i = 0; i < PREFETCHD_CACHE_PAGE_COUNT; i++) {
		cache_metas[i]->status = empty;
	}

	return true;

free_content:
	vfree(cache_content);

fail_log:
	DPPRINTK("prefetchd_cache initialize failed.");
	return false;
}

void prefetchd_cache_exit() {
	vfree(cache_content);
	vfree((void *)cache_metas);
}

bool prefetchd_cache_handle_bio(struct bio *bio) {
	struct cache_meta_map map;
	struct cache_meta *meta;
	long flags;
	int i;
	u64 sector_num;
	s64 sector_diff;
	void *data_src;
	void *data_dest;
	struct bio_vec bvec;
	struct bvec_iter iter;

	get_cache_meta_map(
		bio->bi_iter.bi_sector,
		bio->bi_iter.bi_size,
		&map);

	spin_lock_irqsave(&cache_global_lock, flags);

	cache_meta_map_foreach(map, meta, i) {
		if (meta->status == empty)
			goto cache_miss;
		sector_num = bio->bi_iter.bi_sector + (i << (PAGE_SHIFT - 9));
		sector_diff = (s64)sector_num - (s64)(meta->sector_num);
		if (sector_diff < 0 || sector_diff > (PAGE_SIZE >> 9))
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
