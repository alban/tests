#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

int main() {
	int i = 1;
	while (i++) {
		char pathname[1024];
		struct stat buf;
		struct timespec tp1, tp2;

		snprintf(pathname, sizeof(pathname), "/foo_%d_", i);

		clock_gettime(CLOCK_MONOTONIC, &tp1);
		lstat(pathname, &buf);
		clock_gettime(CLOCK_MONOTONIC, &tp2);

		long long unsigned int ts1 = tp1.tv_sec * 1000 * 1000 * 1000 + tp1.tv_nsec;
		long long unsigned int ts2 = tp2.tv_sec * 1000 * 1000 * 1000 + tp2.tv_nsec;
		printf("%llu %llu %s\n", ts1, ts2, pathname);
	}

	return 0;
}
