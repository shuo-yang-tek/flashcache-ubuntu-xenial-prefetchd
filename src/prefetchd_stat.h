#include <stdlib.h>
#include <linux/bio.h>

#ifndef PREFETCHD_STAT
#define PREFETCHD_STAT

#define PREFETCHD_STAT_COUNT 64

enum prefetchd_stat_status {
	not_used = 1,
	initialized,
	sequential_forward,
	sequential_backward,
	stride_forward,
	stride_backward
};

struct prefetchd_stat_info {
	enum prefetchd_stat_status status;
	u_int8_t credibility;
	u_int64_t last_sector_num; // 512 bytes
	unsigned int last_size; // bytes
};

#endif

void prefetchd_stats_init();
void prefetchd_update_stat(int pid, struct bio *bio, struct prefetchd_stat_info *info);
