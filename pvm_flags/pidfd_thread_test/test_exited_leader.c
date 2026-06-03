/*
 * Test process_vm_readv and process_madvise on a process where the main
 * thread (thread-group leader) has exited but a secondary thread is still
 * alive (zombie leader scenario).
 *
 * The test forks a child that:
 *   1. Creates a secondary thread
 *   2. The main thread calls pthread_exit() (exits without terminating the process)
 *   3. The secondary thread stays alive and signals readiness via a pipe
 *
 * The parent then tests process_vm_readv and process_madvise with:
 *   - Regular pidfd for the main PID (thread-group leader, now a zombie)
 *   - PIDFD_THREAD pidfd for the main PID
 *   - PIDFD_THREAD pidfd for the secondary thread TID
 *   - Plain PID (non-pidfd path) with main PID
 *   - Plain TID (non-pidfd path) with secondary TID
 *
 * Build: gcc -o test_exited_leader test_exited_leader.c -pthread -Wall
 * Run:   sudo vimto --kernel ghcr.io/alban/ci-kernels:7.1-rc6-patch-alban-f6d1fd027fc1 exec -- ./test_exited_leader
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
#include <sys/wait.h>
#include <signal.h>

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

static int pidfd_open(pid_t pid, unsigned int flags)
{
	return syscall(__NR_pidfd_open, pid, flags);
}

static ssize_t do_process_vm_readv(int id, void *local_buf, size_t len,
				   void *remote_addr, unsigned long flags)
{
	struct iovec local_iov = { .iov_base = local_buf, .iov_len = len };
	struct iovec remote_iov = { .iov_base = remote_addr, .iov_len = len };

	return syscall(SYS_process_vm_readv, id, &local_iov, 1,
		       &remote_iov, 1, flags);
}

static long do_process_madvise(int pidfd, void *addr, size_t len, int advice)
{
	struct iovec iov = { .iov_base = addr, .iov_len = len };
	return syscall(__NR_process_madvise, pidfd, &iov, 1, advice, 0);
}

struct child_info {
	pid_t main_pid;
	pid_t secondary_tid;
	void *shared_addr;  /* mmap'd page in child, page-aligned */
};

static int ready_pipe[2];  /* child signals readiness */
static int done_pipe[2];   /* parent signals done */

static void *secondary_thread(void *arg)
{
	pid_t tid = syscall(SYS_gettid);
	/* Write our TID to parent */
	write(ready_pipe[1], &tid, sizeof(tid));

	/* Wait for parent to signal we can exit */
	char c;
	read(done_pipe[0], &c, 1);
	return NULL;
}

static void run_child(void)
{
	pthread_t thr;

	/* mmap a page that we'll share for reading */
	void *buf = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	strcpy(buf, "HELLO FROM CHILD");

	/* Write our shared buffer address to parent */
	write(ready_pipe[1], &buf, sizeof(buf));

	/* Create secondary thread */
	pthread_create(&thr, NULL, secondary_thread, NULL);

	/* Exit the main thread, leaving the secondary alive.
	 * The process stays alive because a thread is still running. */
	pthread_exit(NULL);
}

static void test_vm_readv(const char *desc, int id, void *remote_addr,
			  unsigned long flags)
{
	char buf[64] = {0};
	ssize_t ret = do_process_vm_readv(id, buf, sizeof(buf), remote_addr, flags);
	printf("  process_vm_readv: %s\n", desc);
	if (ret >= 0)
		printf("    OK: read %zd bytes, data=\"%.17s\"\n", ret, buf);
	else
		printf("    errno=%d (%s)\n", errno, strerror(errno));
}

static void test_madvise(const char *desc, int pidfd, void *remote_addr)
{
	long ret = do_process_madvise(pidfd, remote_addr, 4096, MADV_SEQUENTIAL);
	printf("  process_madvise: %s\n", desc);
	if (ret >= 0)
		printf("    OK: returned %ld\n", ret);
	else
		printf("    errno=%d (%s)\n", errno, strerror(errno));
}

int main(void)
{
	pid_t child_pid;
	pid_t secondary_tid;
	void *child_shared_addr;
	int pidfd;

	pipe(ready_pipe);
	pipe(done_pipe);

	child_pid = fork();
	if (child_pid == 0) {
		close(ready_pipe[0]);
		close(done_pipe[1]);
		run_child();
		_exit(0); /* not reached */
	}

	close(ready_pipe[1]);
	close(done_pipe[0]);

	/* Read shared address from child */
	read(ready_pipe[0], &child_shared_addr, sizeof(child_shared_addr));
	/* Read secondary thread TID */
	read(ready_pipe[0], &secondary_tid, sizeof(secondary_tid));

	/* Give the main thread time to fully exit */
	usleep(100000);

	printf("Child main PID: %d (leader, now exited)\n", child_pid);
	printf("Child secondary TID: %d (still alive)\n", secondary_tid);
	printf("Child shared_buf at: %p\n\n", child_shared_addr);

	/* === Test 1: Regular pidfd for main PID (zombie leader) === */
	printf("=== Test 1: pidfd_open(child_pid=%d, 0) — zombie leader ===\n", child_pid);
	pidfd = pidfd_open(child_pid, 0);
	if (pidfd < 0) {
		printf("  pidfd_open failed: errno=%d (%s)\n", errno, strerror(errno));
	} else {
		test_vm_readv("regular pidfd -> zombie leader", pidfd,
			      child_shared_addr, PROCESS_VM_PIDFD);
		test_madvise("regular pidfd -> zombie leader", pidfd,
			     child_shared_addr);
		close(pidfd);
	}
	printf("\n");

	/* === Test 2: PIDFD_THREAD pidfd for main PID (zombie leader) === */
	printf("=== Test 2: pidfd_open(child_pid=%d, PIDFD_THREAD) — zombie leader ===\n", child_pid);
	pidfd = pidfd_open(child_pid, PIDFD_THREAD);
	if (pidfd < 0) {
		printf("  pidfd_open failed: errno=%d (%s)\n", errno, strerror(errno));
	} else {
		test_vm_readv("PIDFD_THREAD pidfd -> zombie leader", pidfd,
			      child_shared_addr, PROCESS_VM_PIDFD);
		test_madvise("PIDFD_THREAD pidfd -> zombie leader", pidfd,
			     child_shared_addr);
		close(pidfd);
	}
	printf("\n");

	/* === Test 3: PIDFD_THREAD pidfd for secondary TID (alive) === */
	printf("=== Test 3: pidfd_open(secondary_tid=%d, PIDFD_THREAD) — alive non-leader ===\n", secondary_tid);
	pidfd = pidfd_open(secondary_tid, PIDFD_THREAD);
	if (pidfd < 0) {
		printf("  pidfd_open failed: errno=%d (%s)\n", errno, strerror(errno));
	} else {
		test_vm_readv("PIDFD_THREAD pidfd -> alive non-leader", pidfd,
			      child_shared_addr, PROCESS_VM_PIDFD);
		test_madvise("PIDFD_THREAD pidfd -> alive non-leader", pidfd,
			     child_shared_addr);
		close(pidfd);
	}
	printf("\n");

	/* === Test 4: Plain PID (no pidfd, flags=0) with main PID === */
	printf("=== Test 4: process_vm_readv(pid=%d, flags=0) — zombie leader, no pidfd ===\n", child_pid);
	test_vm_readv("plain pid -> zombie leader", child_pid,
		      child_shared_addr, 0);
	printf("\n");

	/* === Test 5: Plain TID (no pidfd, flags=0) with secondary TID === */
	printf("=== Test 5: process_vm_readv(tid=%d, flags=0) — alive non-leader, no pidfd ===\n", secondary_tid);
	test_vm_readv("plain tid -> alive non-leader", secondary_tid,
		      child_shared_addr, 0);
	printf("\n");

	/* Let the child's secondary thread exit */
	char c = 'x';
	write(done_pipe[1], &c, 1);
	waitpid(child_pid, NULL, 0);

	printf("Done.\n");
	return 0;
}
