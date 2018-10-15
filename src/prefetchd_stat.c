#include <linux/types.h>
#include <stdbool.h>
#include <linux/bio.h>
#include <linux/genhd.h>

#include "./prefetchd_stat.h"

#define bi_sector	bi_iter.bi_sector
#define bi_size		bi_iter.bi_size
#define bi_idx		bi_iter.bi_idx

struct req_info {
	u64 sector_num; // 512 bytes
	unsigned int size; // bytes
};

/*
static inline void initialize_stat(struct prefetchd_stat *target);
static struct prefetchd_stat *dequeue(void);
static void bring_to_head(struct prefetchd_stat *target);
static struct prefetchd_stat *enqueue(int pid, struct bio *bio);
static enum prefetchd_stat_status detect_status(struct prefetchd_stat *stat);
static void process_stat(struct prefetchd_stat *stat);
static inline bool stat_eq(int pid, struct bio *bio, struct prefetchd_stat *stat);
static inline void req_cpy(struct req_info *dest, struct req_info *src);
static inline void update_req(struct prefetchd_stat *stat, struct bio *bio);
static inline struct prefetchd_stat *get_stat_exist(int pid, struct bio *bio);
*/

struct prefetchd_stat {
	enum prefetchd_stat_status status;

	int pid;
	int major;
	u8 minor;

	u64 verified_count;
	struct req_info prev_req;
	struct req_info curr_req;

	struct prefetchd_stat *prev;
	struct prefetchd_stat *next;
};

static struct prefetchd_stat stat_pool[PREFETCHD_STAT_COUNT];
static struct prefetchd_stat *stats_head;
static struct prefetchd_stat *stats_tail;
static int stats_count;

static inline void initialize_stat(struct prefetchd_stat *target) {
	target->status = not_used;
	target->verified_count = 0;
	target->prev = NULL;
	target->next = NULL;
}

void prefetchd_stats_init() {
	int i;

	for (i = 0; i < PREFETCHD_STAT_COUNT; i++)
		initialize_stat(&stat_pool[i]);

	stats_head = NULL;
	stats_tail = NULL;
	stats_count = 0;
}

static struct prefetchd_stat *dequeue(void) {
	struct prefetchd_stat *target = stats_tail;

	if (stats_count == 0) return NULL;

	stats_tail = target->prev;
	if (stats_tail != NULL)
		stats_tail->next = NULL;

	initialize_stat(target);

	if (--stats_count == 0)
		stats_head = NULL;

	return target;
}

static void bring_to_head(struct prefetchd_stat *target) {
	if (stats_count < 2)
		return;

	if (target == stats_tail)
		stats_tail = target->prev;

	if (target->prev != NULL)
		target->prev->next = target->next;
	if (target->next != NULL)
		target->next->prev = target->prev;

	target->next = stats_head;
	target->prev = NULL;

	if (stats_head != NULL)
		stats_head->prev = target;

	stats_head = target;
}

static struct prefetchd_stat *enqueue(int pid, struct bio *bio) {
	struct prefetchd_stat *res;
	int i;

	if (stats_count == PREFETCHD_STAT_COUNT)
		res = dequeue();
	else {
		for (i = 0; i < PREFETCHD_STAT_COUNT; i++) {
			res = &stat_pool[i];
			if (res->status == not_used)
				break;
		}
	}

	res->status = initialized;
	res->pid = pid;
	res->major = bio->bi_disk->major;
	res->minor = bio->bi_partno;

	res->next = stats_head;
	res->prev = NULL;

	if (stats_head != NULL)
		stats_head->prev = res;

	stats_head = res;

	if (++stats_count == 1)
		stats_tail = res;

	return res;
}

static enum prefetchd_stat_status detect_status(struct prefetchd_stat *stat) {
	struct req_info *prev = &(stat->prev_req);
	struct req_info *curr = &(stat->curr_req);
	u64 prev_size = (u64)prev->size;
	u64 curr_size = (u64)curr->size;
	prev_size = (511 & prev_size) == 0 ? prev_size >> 9 : (prev_size >> 9) + 1;
	curr_size = (511 & curr_size) == 0 ? curr_size >> 9 : (curr_size >> 9) + 1;

	if (prev->sector_num < curr->sector_num) {
		if (curr->sector_num - prev->sector_num <= prev_size)
			return sequential_forward;
		else if (prev_size == curr_size)
			return stride_forward;
		else
			return initialized;
	} else if (prev->sector_num > curr->sector_num) {
		if (prev->sector_num - curr->sector_num <= curr_size)
			return sequential_backward;
		else if (prev_size == curr_size)
			return stride_backward;
		else
			return initialized;
	} else {
		return initialized;
	}
}

static void process_stat(struct prefetchd_stat *stat) {
	switch (stat->verified_count) {
	case 0:
		stat->status = initialized;
		break;
	case 1:
		stat->status = detect_status(stat);
		break;
	default:
		if (detect_status(stat) != stat->status) {
			stat->status = initialized;
		} else {
		}
	}

	if (stat->status == initialized)
		stat->verified_count = 1;
	else if (stat->verified_count < 0xFFFFFFFFFFFFFFFF)
		stat->verified_count += 1;
}

static inline bool stat_eq(int pid, struct bio *bio, struct prefetchd_stat *stat) {
	if (pid == stat->pid && bio->bi_disk->major == stat->major && bio->bi_partno == stat->minor) return true;
	return false;
}

static inline void req_cpy(struct req_info *dest, struct req_info *src) {
	dest->sector_num = src->sector_num;
	dest->size = src->size;
}

static inline void update_req(struct prefetchd_stat *stat, struct bio *bio) {
	req_cpy(&(stat->prev_req), &(stat->curr_req));
	stat->curr_req.sector_num = bio->bi_sector;
	stat->curr_req.size = bio->bi_size;
}

static inline struct prefetchd_stat *get_stat_exist(int pid, struct bio *bio) {
	struct prefetchd_stat *stat = stats_head;
	while (stat != NULL) {
		if (stat_eq(pid, bio, stat)) break;
		stat = stat->next;
	}
	return stat;
}

void prefetchd_update_stat(int pid, struct bio *bio, struct prefetchd_stat_info *info) {
	struct prefetchd_stat *stat;
	u64 verified_count;

	// lock stat
	
	stat = get_stat_exist(pid, bio);

	if (stat == NULL)
		stat = enqueue(pid, bio);
	else
		bring_to_head(stat);

	update_req(stat, bio);
	process_stat(stat);

	info->status = stat->status;
	if (info->status > 2) {
		verified_count = stat->verified_count - 1;
		info->credibility = verified_count > 0xFF ? 255 : (u8)(verified_count & 0xFF);
		info->last_sector_num = stat->curr_req.sector_num;
		info->last_size = stat->curr_req.size;
	}

	// unlock stat
}
