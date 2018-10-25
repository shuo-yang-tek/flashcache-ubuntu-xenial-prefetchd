#define PFD_CACHE_COUNT_PER_SET 4
#define PFD_CACHE_BLOCK_COUNT 16384
#define PFD_CACHE_MAX_STEP 128
#define PFD_CACHE_THRESHOLD_STEP 4

#include <stdbool.h>

int pfd_cache_init(void);
void pfd_cache_exit(void);
void pfd_cache_add(struct cache_c *dmc);
bool pfd_cache_handle_bio(
		struct cache_c *dmc,
		struct bio *bio);
void pfd_cache_prefetch(
		struct cache_c *dmc,
		struct pfd_stat_info *info);
int pfd_cache_reset(void);
