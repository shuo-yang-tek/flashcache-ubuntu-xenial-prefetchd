#define SIZE_PER_READ 4096
#define STRIDE_LEN 4096
#define COUNT 100000

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

int main(int argc, char *argv[]) {
	int type = 0;
	char *dev, *str;
	int fd;
	char buf[SIZE_PER_READ];
	int i;
	int seek_type = SEEK_SET;
	off_t seek_offset = 0;
	struct timespec start;
	struct timespec end;
	double spent;

	if (argc < 2) {
		printf("reader <path> [type]\n");
		return -1;
	}

	dev = argv[1];

	fd = open(dev, O_RDONLY);
	if (fd < 0) {
		printf("can't open %s\n", dev);
		return -1;
	}
	
	if (argc > 2) {
		str = argv[2];
		if (strcmp(str, "seq-back") == 0) {
			type = 1;
			seek_type = SEEK_END;
			printf("seq-back\n");
		} else if (strcmp(str, "str-for") == 0) {
			type = 2;
			printf("str-for\n");
		} else if (strcmp(str, "str-back") == 0) {
			type = 3;
			seek_type = SEEK_END;
			printf("str-back\n");
		}
	}

	if (type == 0) {
		printf("seq-for\n");
	}

	clock_gettime(CLOCK_REALTIME, &start);

	for (i = 0; i < COUNT; i++) {
		if (lseek(fd, seek_offset, seek_type) < 0) {
			printf("seek fail\n");
			break;
		}
		if (read(fd, buf, SIZE_PER_READ) < 0) {
			printf("read fail\n");
			break;
		}
		seek_offset += SIZE_PER_READ;
		if (type >= 2) seek_offset += STRIDE_LEN;
	}

	clock_gettime(CLOCK_REALTIME, &end);

	spent = (double)(end.tv_sec - start.tv_sec) + ((double)end.tv_nsec - (double)start.tv_nsec) / 1.0e9;
	printf("%lfs\n", spent);

	close(fd);
	return 0;
}
