#define PREFETCHD_PREFIX "prefetchd: "

#ifdef PREFETCHD_DEBUG
#define DPPRINTK( s, arg... ) printk(PREFETCHD_PREFIX s "\n", ##arg)
#else
#define DPPRINTK( s, arg... )
#endif

#define MPPRINTK( s, arg... ) printk(PREFETCHD_PREFIX s "\n", ##arg)
