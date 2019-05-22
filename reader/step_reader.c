#define UNIT_SIZE 4096
#define PHY_SECT_SIZE 512
#define START_NO (1 * 1024 * 1024)
#define STRITE_READ_COUNT 2
#define STRIDE_SKIP_COUNT 3

#define TYPE_SEQ 1
#define TYPE_BACK_SEQ 2
#define TYPE_STRIDE 3
#define TYPE_BACK_STRIDE 4

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

off_t current_off = START_NO;
int step_count = 0;
int mode_type = 0;
int fd;
char buf[UNIT_SIZE];

void update_step() {
	int stride_tmp = (step_count + 1) % (STRITE_READ_COUNT);

	switch (mode_type) {
	case TYPE_SEQ:
		current_off += UNIT_SIZE;
		break;
	case TYPE_BACK_SEQ:
		current_off -= UNIT_SIZE;
		break;
	case TYPE_STRIDE:
		if (stride_tmp == 0)
			stride_tmp = STRIDE_SKIP_COUNT + 1;
		else
			stride_tmp = 1;
		current_off += stride_tmp * UNIT_SIZE;
		break;
	case TYPE_BACK_STRIDE:
		if (stride_tmp == 0)
			stride_tmp = -(STRIDE_SKIP_COUNT + 1 + STRITE_READ_COUNT - 1);
		else
			stride_tmp = 1;
		current_off += stride_tmp * UNIT_SIZE;
		break;
	}

	step_count += 1;
}

int step_read() {
	if (lseek(fd, current_off, SEEK_SET) < 0)
		return -1;
	if (read(fd, buf, UNIT_SIZE) < 0)
		return -2;
	return 0;
}

int get_current_block_no() {
	return current_off / PHY_SECT_SIZE;
}

int main(int argc, char *argv[]) {
	char *dev;
	int ret = 0;
	char cmd[1024];
	int step_ret = 0;
	int current_block_no;

	if (argc < 2) {
		printf("step_read <dev>\n");
		return -1;
	}

	dev = argv[1];
	fd = open(dev, O_RDONLY);
	
	if (fd < 0) {
		printf("can't open %s\n", dev);
		return -1;
	}

	printf("Select read mode:\n");
	printf("\t%d) Sequential Forward\n", TYPE_SEQ);
	printf("\t%d) Sequential Backward\n", TYPE_BACK_SEQ);
	printf("\t%d) Stride Forward\n", TYPE_STRIDE);
	printf("\t%d) Stride Backward\n", TYPE_BACK_STRIDE);
	printf(">>> ");

	scanf("%d", &mode_type);

	switch (mode_type) {
	case TYPE_SEQ:
		printf("\n<<Sequential Forward>>\n\n");
		break;
	case TYPE_BACK_SEQ:
		printf("\n<<Sequential Backward>>\n\n");
		break;
	case TYPE_STRIDE:
		printf("\n<<Stride Forward>>\n\n");
		break;
	case TYPE_BACK_STRIDE:
		printf("\n<<Stride Backward>>\n\n");
		break;
	default:
		printf("\n<<Unknown. Exit>>\n\n");
		ret = -1;
		goto close_fd;
		break;
	}

	while (1) {
		current_block_no = get_current_block_no();
		printf("\nNext read: %d(%d)\n", current_block_no, (UNIT_SIZE / PHY_SECT_SIZE));
		printf("Input anything to continue. (\"exit\" to exit) ");
		scanf("%s", cmd);
		if (strcmp(cmd, "exit") == 0)
			goto close_fd;

		step_ret = step_read();
		printf("\t");
		switch (step_ret) {
		case 0:
			printf("Read %d(%d)\n", current_block_no, (UNIT_SIZE / PHY_SECT_SIZE));
			update_step();
			break;
		case -1:
			printf("lseek() failed\n");
			goto close_fd;
			break;
		case -2:
			printf("read() failed\n");
			goto close_fd;
			break;
		}
	}


close_fd:
	close(fd);
	return ret;
}
