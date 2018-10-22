#include <linux/module.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include "prefetchd_log.h"
#include "prefetchd_reset.h"

static int
proc_show_fn(struct seq_file *m, void *v) {
	return 0;
}

static int
proc_open_fn(struct inode *inode, struct file *file) {
	return single_open(file, proc_show_fn, NULL);
}

static ssize_t
proc_write_fn(struct file *file, const char __user *buffer, size_t count, loff_t *f_pos) {
	MPPRINTK("reset received.");
	return count;
}

static struct file_operations fops = {
	.owner = THIS_MODULE,
	.open = proc_open_fn,
	.release = single_release,
	.read = seq_read,
	.llseek = seq_lseek,
	.write = proc_write_fn,
};

int prefetchd_reset_init() {
	struct proc_dir_entry *file;

	file = proc_create(
			PREFETCHD_RESET_FILENAME,
			0200,
			NULL,
			&fops);

	if (!file) return -1;

	MPPRINTK("%s created.",
			PREFETCHD_RESET_FILENAME);
	return 0;
}

void prefetchd_reset_exit() {
	remove_proc_entry(PREFETCHD_RESET_FILENAME, NULL);
}
