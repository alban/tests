/*
 * Test pidfd_get_task behavior with different pidfd types and thread types.
 * Tests process_vm_readv(PROCESS_VM_PIDFD) and process_madvise().
 *
 * Build: gcc -o /tmp/test_pidfd_thread /tmp/test_pidfd_thread.c -pthread
 * Run: sudo /home/alban/go/bin/vimto --kernel ghcr.io/alban/ci-kernels:7.1-rc6-patch-alban-f6d1fd027fc1 exec -- /tmp/test_pidfd_thread
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <linux/pidfd.h>

#ifndef PROCESS_VM_PIDFD
#define PROCESS_VM_PIDFD 0x01
#endif

#ifndef PIDFD_THREAD
#define PIDFD_THREAD 0200  /* O_EXCL */
#endif

#ifndef __NR_pidfd_open
#define __NR_pidfd_open 434
#endif

#ifndef __NR_process_madvise
#define __NR_process_madvise 440
#endif

/* Shared data that threads can read */
static volatile int thread_tid = 0;
static volatile int thread_ready = 0;
static char *shared_buf;

static void *thread_func(void *arg)
{
	thread_tid = syscall(SYS_gettid);
	__atomic_store_n(&thread_ready, 1, __ATOMIC_RELEASE);

	/* Spin until main is done */
	while (__atomic_load_n(&thread_ready, __ATOMIC_ACQUIRE) != 2)
		usleep(1000);
	return NULL;
}

static int pidfd_open(pid_t pid, unsigned int flags)
{
	return syscall(__NR_pidfd_open, pid, flags);
}

static ssize_t do_process_vm_readv(int pidfd, void *local_buf, size_t len,
				   void *remote_addr, unsigned long flags)
{
	struct iovec local_iov = { .iov_base = local_buf, .iov_len = len };
	struct iovec remote_iov = { .iov_base = remote_addr, .iov_len = len };

	return syscall(SYS_process_vm_readv, pidfd, &local_iov, 1,
		       &remote_iov, 1, flags);
}

static long do_process_madvise(int pidfd, void *addr, size_t len, int advice)
{
	struct iovec iov = { .iov_base = addr, .iov_len = len };
	return syscall(__NR_process_madvise, pidfd, &iov, 1, advice, 0);
}

static void test_one(const char *desc, int pidfd, void *remote_addr)
{
	char buf[64] = {0};
	ssize_t ret;
	long mret;

	/* Test process_vm_readv with PROCESS_VM_PIDFD */
	ret = do_process_vm_readv(pidfd, buf, sizeof(buf), remote_addr,
				  PROCESS_VM_PIDFD);
	printf("  process_vm_readv(PROCESS_VM_PIDFD): %s\n", desc);
	if (ret >= 0)
		printf("    OK: read %zd bytes, data=\"%.17s\"\n", ret, buf);
	else
		printf("    FAIL: %zd errno=%d (%s)\n", ret, errno, strerror(errno));

	/* Test process_madvise */
	mret = do_process_madvise(pidfd, remote_addr, 4096, MADV_SEQUENTIAL);
	printf("  process_madvise(MADV_SEQUENTIAL): %s\n", desc);
	if (mret >= 0)
		printf("    OK: returned %ld\n", mret);
	else
		printf("    FAIL: %ld errno=%d (%s)\n", mret, errno, strerror(errno));
}

int main(void)
{
	pid_t main_pid = getpid();
	pid_t main_tid = syscall(SYS_gettid);
	pthread_t thr;
	int pidfd;

	/* Use mmap so process_madvise works */
	shared_buf = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	strcpy(shared_buf, "HELLO FROM TARGET");

	printf("Main PID/TID: %d/%d\n", main_pid, main_tid);

	/* Create a secondary thread */
	pthread_create(&thr, NULL, thread_func, NULL);
	while (!__atomic_load_n(&thread_ready, __ATOMIC_ACQUIRE))
		usleep(100);
	printf("Secondary thread TID: %d\n", thread_tid);
	printf("shared_buf at: %p\n\n", shared_buf);

	/* Test 1: Regular pidfd for main pid (pidfd_open(pid, 0)) */
	printf("=== Test 1: pidfd_open(main_pid=%d, 0) ===\n", main_pid);
	pidfd = pidfd_open(main_pid, 0);
	if (pidfd < 0) {
		printf("  pidfd_open failed: %s\n", strerror(errno));
	} else {
		test_one("regular pidfd -> main pid", pidfd, shared_buf);
		close(pidfd);
	}
	printf("\n");

	/* Test 2: PIDFD_THREAD pidfd for main thread */
	printf("=== Test 2: pidfd_open(main_tid=%d, PIDFD_THREAD) ===\n", main_tid);
	pidfd = pidfd_open(main_tid, PIDFD_THREAD);
	if (pidfd < 0) {
		printf("  pidfd_open failed: %s\n", strerror(errno));
	} else {
		test_one("PIDFD_THREAD pidfd -> main thread (is also leader)", pidfd, shared_buf);
		close(pidfd);
	}
	printf("\n");

	/* Test 3: PIDFD_THREAD pidfd for secondary thread */
	printf("=== Test 3: pidfd_open(secondary_tid=%d, PIDFD_THREAD) ===\n", thread_tid);
	pidfd = pidfd_open(thread_tid, PIDFD_THREAD);
	if (pidfd < 0) {
		printf("  pidfd_open failed: %s\n", strerror(errno));
	} else {
		test_one("PIDFD_THREAD pidfd -> secondary thread (non-leader)", pidfd, shared_buf);
		close(pidfd);
	}
	printf("\n");

	/* Test 4: Regular pidfd_open on a non-leader TID (should fail at pidfd_open) */
	printf("=== Test 4: pidfd_open(secondary_tid=%d, 0) — expect EINVAL ===\n", thread_tid);
	pidfd = pidfd_open(thread_tid, 0);
	if (pidfd < 0) {
		printf("  pidfd_open failed as expected: errno=%d (%s)\n", errno, strerror(errno));
	} else {
		test_one("regular pidfd -> secondary tid (unexpected success)", pidfd, shared_buf);
		close(pidfd);
	}
	printf("\n");

	/* Let thread exit */
	__atomic_store_n(&thread_ready, 2, __ATOMIC_RELEASE);
	pthread_join(thr, NULL);

	printf("Done.\n");
	return 0;
}
