#define PREFETCHD_CACHE_PAGE_COUNT 16384
#define PREFETCHD_CACHE_MAX_PAGE_COUNT_PER_CACHE 128

#include <stdbool.h>
#include <linux/blk_types.h>

bool prefetchd_cache_init(void);
void prefetchd_cache_exit(void);
bool prefetchd_cache_handle_bio(struct bio *bio);
