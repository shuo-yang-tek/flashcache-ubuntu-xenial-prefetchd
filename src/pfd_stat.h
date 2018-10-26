//#define PFD_STAT_SEQ_FOR_ONLY
#define PFD_STAT_COUNT 64

#include <linux/types.h>

#ifndef PFD_STAT
#define PFD_STAT
struct pfd_stat_info {
	sector_t last_sect;
	long seq_count;
	long seq_total_count;
	long stride_distance_sect;
	long stride_count;
};
#endif

void pfd_stat_init(void);
void pfd_stat_update(
		struct cache_c *dmc,
		struct bio *bio,
		struct pfd_stat_info *result);
