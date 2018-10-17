#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/semaphore.h>
#include <linux/bio.h>
#include <stdbool.h>

#include "./prefetchd_log.h"
#include "./prefetchd_mem_cache.h"

DEFINE_SPINLOCK(mem_cache_global_lock);

enum mem_cache_status {
	not_used = 1,
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
		mem_cache_pool[i].status = not_used;
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

	data_src = mem_cache->data + (void *)(bio_start - cache_start);
	bio_for_each_segment(bvec, bio, iter) {
		data_dest = kmap(bvec.bv_page) + bvec.bv_offset;
		memcpy(data_dest, data_src, bvec.bv_len);
		data_src += bvec.bv_len;
	}
	bio_endio(bio);

	atomic_dec(&(mem_cache->hold_count));
	return true;
}
