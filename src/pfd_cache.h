#define PFD_CACHE_COUNT_PER_SET 4
#define PFD_CACHE_BLOCK_COUNT 16384

void pfd_cache_init(void);
void pfd_cache_exit(void);
void pfd_cache_add(struct cache_c *dmc);
bool pfd_cache_handle_bio(
		struct cache_c *dmc,
		struct bio *bio);
void pfd_cache_prefetch(
		struct cache_c *dmc,
		struct pfd_stat_info *info);
