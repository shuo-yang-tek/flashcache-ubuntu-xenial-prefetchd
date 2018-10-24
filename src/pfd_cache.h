#define PFD_CACHE_COUNT_PER_SET 4
#define PFD_CACHE_BLOCK_COUNT 16384

void pfd_cache_init(void);
void pfd_cache_exit(void);
void pfd_cache_add(struct cache_c *dmc);
