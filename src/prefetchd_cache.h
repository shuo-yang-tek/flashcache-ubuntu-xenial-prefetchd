#define PREFETCHD_CACHE_PAGE_COUNT 16384
#define PREFETCHD_CACHE_MAX_PAGE_COUNT_PER_CACHE 128
#define PREFETCHD_MAX_SSD_STEP 3

#include <stdbool.h>
#include <linux/blk_types.h>

#include "prefetchd_stat.h"

bool prefetchd_cache_init(void);
void prefetchd_cache_exit(void);
bool prefetchd_cache_handle_bio(struct bio *bio);
void prefetchd_do_prefetch(
		struct cache_c *dmc,
		struct prefetchd_stat_info *info
		);
int prefetchd_cache_reset(void);
