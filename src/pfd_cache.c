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

DEFINE_SPINLOCK(global_lock);
DEFINE_SPINLOCK(callback_lock);

static void do_prefetch_endless_seq(
		struct cache_c *dmc,
		struct pfd_public_stat *info) {
}

static void do_prefetch_seq_back(
		struct cache_c *dmc,
		struct pfd_public_stat *info) {
}

static void do_prefetch_stride(
		struct cache_c *dmc,
		struct pfd_public_stat *info) {
}

void pfd_cache_prefetch(
		struct cache_c *dmc,
		struct pfd_public_stat *info) {

	long flags;
	long stride_abs = info->stride < 0 ?
		-(info->stride) : info->stride;
	long stride_abs_blocks = stride_abs >> dmc->block_shift;

	spin_lock_irqsave(&global_lock, flags);

	if (info->curr_len == 0)
		do_prefetch_endless_seq(dmc, info);
	else if (info->curr_len == stride_abs_blocks)
		do_prefetch_seq_back(dmc, info);
	else if (info->curr_len < stride_abs_blocks)
		do_prefetch_stride(dmc, info);

	spin_unlock_irqrestore(&global_lock, flags);
}
