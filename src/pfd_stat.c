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
#include "pfd_stat.h"
#include "prefetchd_log.h"

struct pfd_seq_stat {
	sector_t start;
	long count;
};

inline void
reset_pfd_seq_stat(struct pfd_seq_stat *target) {
	target->count = 0;
}

inline bool
is_bio_fit_seq_stat(
		struct cache_c *dmc,
		struct pfd_seq_stat *seq_stat,
		struct bio *bio) {
	return seq_stat->start +
		(sector_t)seq_stat->count *
		(sector_t)dmc->block_size
		== bio->bi_iter.bi_sector ? true : false;
}

struct pfd_stat {
	struct dm_target *tgt;
	int pid;

	long stride;
	long stride_count;
	struct pfd_seq_stat seq_stats[2];
	struct pfd_seq_stat *curr_seq_stat;
	struct pfd_seq_stat *prev_seq_stat;
};

inline void
reset_pfd_stat(struct pfd_stat *target) {
	target->tgt = NULL;
	target->pid = -1;
	target->stride = 0;
	target->stride_count = 0;
	reset_pfd_seq_stat(&target->seq_stats[0]);
	reset_pfd_seq_stat(&target->seq_stats[1]);
	target->curr_seq_stat = &(target->seq_stats[0]);
	target->prev_seq_stat = &(target->seq_stats[1]);
}

static void
swap_pfd_stat_curr_prev(struct pfd_stat *target) {
	struct pfd_seq_stat *tmp;

	tmp = target->prev_seq_stat;
	target->prev_seq_stat = target->curr_seq_stat;
	target->curr_seq_stat = tmp;
}

struct pfd_stat_elm {
	struct pfd_stat stat;
	struct pfd_stat_elm *next;
	struct pfd_stat_elm *prev;
};

inline void
reset_pfd_stat_elm(struct pfd_stat_elm *target) {
	reset_pfd_stat(&target->stat);
	target->next = NULL;
	target->prev = NULL;
}

struct pfd_stat_queue {
	struct pfd_stat_elm stat_elms[PFD_STAT_COUNT];
	struct pfd_stat_elm *head;
	struct pfd_stat_elm *tail;
};

static void
reset_pfd_stat_queue(struct pfd_stat_queue *target) {
	int i;
	struct pfd_stat_elm *elm;

	for (i = 0; i < PFD_STAT_COUNT; i++) {
		elm = &target->stat_elms[i];
		reset_pfd_stat_elm(elm);
		if (i != 0)
			elm->prev = &target->stat_elms[i - 1];
		if (i != PFD_STAT_COUNT - 1)
			elm->next = &target->stat_elms[i + 1];
	}

	target->head = &target->stat_elms[0];
	target->tail = &target->stat_elms[PFD_STAT_COUNT - 1];
}

static void
pfd_stat_elm_to_head(
		struct pfd_stat_queue *q,
		struct pfd_stat_elm *elm) {
	if (q->head == elm) return;
	if (elm->next)
		elm->next->prev = elm->prev;
	if (elm->prev)
		elm->prev->next = elm->next;
	if (q->tail == elm)
		q->tail = elm->prev;
	q->head->prev = elm;
	elm->next = q->head;
	elm->prev = NULL;
	q->head = elm;
}

static struct pfd_stat_elm *
pfd_stat_queue_search(
		struct pfd_stat_queue *q,
		struct cache_c *dmc,
		int pid) {
	struct pfd_stat_elm *elm = q->head;
	struct pfd_stat *pfd_stat;

	while (elm != NULL) {
		pfd_stat = &elm->stat;

		if (pfd_stat->tgt == dmc->tgt && pfd_stat->pid == pid)
			return elm;
		else if (pfd_stat->tgt == NULL)
			return NULL;

		elm = elm->next;
	}

	return NULL;
}

DEFINE_SPINLOCK(global_lock);
struct pfd_stat_queue main_queue;

void pfd_stat_init() {
	spin_lock(&global_lock);
	reset_pfd_stat_queue(&main_queue);
	spin_unlock(&global_lock);
	MPPRINTK("\033[0;32;32mpfd_stat initialized");
}

void pfd_stat_update(
		struct cache_c *dmc,
		struct bio *bio,
		struct pfd_stat_info *result) {

	struct pfd_stat_elm *elm;
	struct pfd_stat *pfd_stat;
	int pid = current->pid;
	struct pfd_seq_stat *curr;
	struct pfd_seq_stat *prev;
	long new_stride_abs;

	spin_lock(&global_lock);

	elm = pfd_stat_queue_search(&main_queue, dmc, pid);
	if (elm != NULL) {
		pfd_stat = &elm->stat;
	} else {
		elm = main_queue.tail;
		pfd_stat = &elm->stat;
		reset_pfd_stat(pfd_stat);
		pfd_stat->pid = pid;
		pfd_stat->tgt = dmc->tgt;
	}
	pfd_stat_elm_to_head(&main_queue, elm);

	curr = pfd_stat->curr_seq_stat;
	prev = pfd_stat->prev_seq_stat;

	if (curr->count == 0) {
		curr->count += 1;
		curr->start = bio->bi_iter.bi_sector;
		goto end;
	}

	if (is_bio_fit_seq_stat(dmc, curr, bio)) {
		curr->count += 1;
		if (prev->count == 0 || curr->count <= prev->count)
			goto end;
		else {
			swap_pfd_stat_curr_prev(pfd_stat);
			curr = pfd_stat->curr_seq_stat;
			prev = pfd_stat->prev_seq_stat;
			reset_pfd_seq_stat(prev);
			pfd_stat->stride = 0;
			pfd_stat->stride_count = 0;
			goto end;
		}
	} else {
		new_stride_abs = (long)bio->bi_iter.bi_sector - (long)curr->start;
		new_stride_abs = new_stride_abs < 0 ? -new_stride_abs : new_stride_abs;
		if (prev->count == 0) {
			if ((new_stride_abs >> dmc->block_shift) < curr->count) {
				curr->start = bio->bi_iter.bi_sector;
				curr->count = 1;
				pfd_stat->stride = 0;
				pfd_stat->stride_count = 0;
				goto end;
			}
			swap_pfd_stat_curr_prev(pfd_stat);
			curr = pfd_stat->curr_seq_stat;
			prev = pfd_stat->prev_seq_stat;
			curr->start = bio->bi_iter.bi_sector;
			curr->count = 1;
			pfd_stat->stride =
				(long)bio->bi_iter.bi_sector -
				(long)prev->start;
			pfd_stat->stride_count = 1;
			goto end;
		} else if (
				prev->count == curr->count &&
				(long)bio->bi_iter.bi_sector -
				(long)curr->start == pfd_stat->stride) {
			swap_pfd_stat_curr_prev(pfd_stat);
			curr = pfd_stat->curr_seq_stat;
			prev = pfd_stat->prev_seq_stat;
			curr->start = bio->bi_iter.bi_sector;
			curr->count = 1;
			pfd_stat->stride_count += 1;
			goto end;
		} else {
			swap_pfd_stat_curr_prev(pfd_stat);
			curr = pfd_stat->curr_seq_stat;
			prev = pfd_stat->prev_seq_stat;
			reset_pfd_seq_stat(prev);
			curr->start = bio->bi_iter.bi_sector;
			curr->count = 1;
			pfd_stat->stride = 0;
			pfd_stat->stride_count = 0;
			goto end;
		}
	}

end:
	result->last_sect = bio->bi_iter.bi_sector;
	result->seq_count = curr->count;
	result->seq_total_count = prev->count;
	result->stride_distance_sect = pfd_stat->stride;
	result->stride_count = pfd_stat->stride_count;

	spin_unlock(&global_lock);

	DPPRINTK("pfd_stat updated");
	DPPRINTK("\tpid: %d",
			pid);
	DPPRINTK("\treq: %lu",
			result->last_sect);
	DPPRINTK("\tstride: %ld",
			result->stride_distance_sect);
	DPPRINTK("\tseq: %ld / %ld",
			result->seq_count,
			result->seq_total_count);
	DPPRINTK("\tstride_count: %ld",
			result->stride_count);
}
