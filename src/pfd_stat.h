#define PFD_STAT_COUNT 64

#include <linux/types.h>

#ifndef PFD_STAT
#define PFD_STAT
struct pfd_public_stat {
	sector_t curr_sect;
	sector_t curr_start_sect;
	long curr_len;
	long stride;
	long stride_count;
};
#endif

void pfd_stat_init(void);
void pfd_stat_update(
		struct cache_c *dmc,
		struct bio *bio,
		struct pfd_public_stat *result);
