#define MEM_CACHE_COUNT 512
#define SIZE_PER_MEM_CACHE 0x20000
#define MAX_MEM_CACHE_COUNT_PER_PREFETCH 16

#include <stdbool.h>

void prefetchd_mem_cache_init(void);
bool prefetchd_mem_cache_handle_bio(struct bio *bio);
