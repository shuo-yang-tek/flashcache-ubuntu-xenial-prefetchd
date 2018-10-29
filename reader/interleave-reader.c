#define CHILD_COUNT 4
#define READ_SIZE 4096
#define READ_COUNT 10000

#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <semaphore.h>

sem_t *sem;

int child_job(int id, char *dev) {
	int fd;
	int i;
	char buf[READ_SIZE];

	fd = open(dev, O_RDONLY);
	if (fd < 0) {
		printf("can't open %s\n", dev);
		return -1;
	}

	if (lseek(fd, (off_t)id * READ_COUNT * READ_SIZE, SEEK_SET) < 0) {
		printf("seek failed.\n");
		close(fd);
		return -1;
	}

	for (i = 0; i < READ_COUNT; i++) {
		sem_wait(sem);
		if (read(fd, buf, READ_SIZE) < 0) {
			printf("read failed.\n");
			close(fd);
			sem_post(sem);
			return -1;
		}
		sem_post(sem);
		usleep(1000);
	}

	close(fd);
	return 0;
}

int main(int argc, char *argv[]) {
	pid_t pid;
	int i;
	int n = CHILD_COUNT;
	struct timespec start;
	struct timespec end;
	double spent;
	char *dev;

	if (argc < 2) {
		printf("interleave-reader <dev>\n");
		return -1;
	}

	dev = argv[1];

	sem_unlink("sema001");
	sem = sem_open("sema001", O_CREAT, 0666, 1);

	clock_gettime(CLOCK_REALTIME, &start);

	for (i = 0; i < n; i++) {
		pid = fork();
		if (pid < 0) {
			printf("fork failed.\n");
			return -1;
		} else if (pid == 0) {
			return child_job(i, dev);
		}
	}

	while (n > 0) {
		wait(NULL);
		n -= 1;
	}

	clock_gettime(CLOCK_REALTIME, &end);

	sem_close(sem);
	sem_unlink("sema001");

	spent = (double)end.tv_sec - (double)start.tv_sec;
	spent += ((double)end.tv_nsec - (double)start.tv_nsec) / 1.0e9;

	printf("spent: %lf\n", spent);

	return 0;
}
