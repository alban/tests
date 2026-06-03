# pidfd_get_task() behavior with PIDFD_THREAD

This test checks how `pidfd_get_task()` behaves when used with different pidfd
types and thread types. Both `process_vm_readv(PROCESS_VM_PIDFD)` and
`process_madvise()` use `pidfd_get_task()` internally.

## Context

[Sashiko](https://sashiko.dev/#/patchset/20260602100917.3641359-1-alban.crequy@gmail.com)
raised a question about whether `pidfd_get_task()` restricts the syscall to
thread-group leaders when using `PROCESS_VM_PIDFD` with a `PIDFD_THREAD` pidfd
for a non-leader thread.

## Build and Run

```bash
gcc -o test_pidfd_thread test_pidfd_thread.c -pthread -Wall
gcc -o test_exited_leader test_exited_leader.c -pthread -Wall
# Run on a kernel with PROCESS_VM_PIDFD support:
sudo vimto --kernel ghcr.io/alban/ci-kernels:7.1-rc6-patch-alban-f6d1fd027fc1 exec -- ./test_pidfd_thread
sudo vimto --kernel ghcr.io/alban/ci-kernels:7.1-rc6-patch-alban-f6d1fd027fc1 exec -- ./test_exited_leader
```

## Test 1: test_pidfd_thread — all threads alive

Results (kernel v7.1-rc6 with pvm_flags patch):

```
Main PID/TID: 55/55
Secondary thread TID: 56
shared_buf at: 0x7f13369a9000

=== Test 1: pidfd_open(main_pid=55, 0) ===
  process_vm_readv(PROCESS_VM_PIDFD): regular pidfd -> main pid
    OK: read 64 bytes, data="HELLO FROM TARGET"
  process_madvise(MADV_SEQUENTIAL): regular pidfd -> main pid
    OK: returned 4096

=== Test 2: pidfd_open(main_tid=55, PIDFD_THREAD) ===
  process_vm_readv(PROCESS_VM_PIDFD): PIDFD_THREAD pidfd -> main thread (is also leader)
    OK: read 64 bytes, data="HELLO FROM TARGET"
  process_madvise(MADV_SEQUENTIAL): PIDFD_THREAD pidfd -> main thread (is also leader)
    OK: returned 4096

=== Test 3: pidfd_open(secondary_tid=56, PIDFD_THREAD) ===
  process_vm_readv(PROCESS_VM_PIDFD): PIDFD_THREAD pidfd -> secondary thread (non-leader)
    FAIL: -1 errno=3 (No such process)
  process_madvise(MADV_SEQUENTIAL): PIDFD_THREAD pidfd -> secondary thread (non-leader)
    FAIL: -1 errno=3 (No such process)

=== Test 4: pidfd_open(secondary_tid=56, 0) — expect EINVAL ===
  pidfd_open failed as expected: errno=2 (No such file or directory)

Done.
```

### Summary

| Test | pidfd type | Target | process_vm_readv | process_madvise |
|------|-----------|--------|-----------------|-----------------|
| 1 | Regular (`pidfd_open(pid, 0)`) | Main PID | ✅ OK | ✅ OK |
| 2 | `PIDFD_THREAD` | Main thread (=leader) | ✅ OK | ✅ OK |
| 3 | `PIDFD_THREAD` | Non-leader thread | ❌ ESRCH | ❌ ESRCH |
| 4 | Regular | Non-leader TID | pidfd_open fails | — |

## Test 2: test_exited_leader — leader thread exited, secondary alive

This tests a process where `pthread_exit()` was called in the main thread,
leaving only a secondary thread alive (zombie leader scenario).

Results (kernel v7.1-rc6 with pvm_flags patch):

```
Child main PID: 56 (leader, now exited)
Child secondary TID: 57 (still alive)
Child shared_buf at: 0x7f88fde93000

=== Test 1: pidfd_open(child_pid=56, 0) — zombie leader ===
  process_vm_readv: regular pidfd -> zombie leader
    errno=3 (No such process)
  process_madvise: regular pidfd -> zombie leader
    errno=3 (No such process)

=== Test 2: pidfd_open(child_pid=56, PIDFD_THREAD) — zombie leader ===
  process_vm_readv: PIDFD_THREAD pidfd -> zombie leader
    errno=3 (No such process)
  process_madvise: PIDFD_THREAD pidfd -> zombie leader
    errno=3 (No such process)

=== Test 3: pidfd_open(secondary_tid=57, PIDFD_THREAD) — alive non-leader ===
  process_vm_readv: PIDFD_THREAD pidfd -> alive non-leader
    errno=3 (No such process)
  process_madvise: PIDFD_THREAD pidfd -> alive non-leader
    errno=3 (No such process)

=== Test 4: process_vm_readv(pid=56, flags=0) — zombie leader, no pidfd ===
  process_vm_readv: plain pid -> zombie leader
    errno=3 (No such process)

=== Test 5: process_vm_readv(tid=57, flags=0) — alive non-leader, no pidfd ===
  process_vm_readv: plain tid -> alive non-leader
    OK: read 64 bytes, data="HELLO FROM CHILD"

Done.
```

### Summary

| Test | Method | Target | process_vm_readv | process_madvise |
|------|--------|--------|-----------------|-----------------|
| 1 | Regular pidfd | Zombie leader | ❌ ESRCH | ❌ ESRCH |
| 2 | PIDFD_THREAD pidfd | Zombie leader | ❌ ESRCH | ❌ ESRCH |
| 3 | PIDFD_THREAD pidfd | Alive non-leader | ❌ ESRCH | ❌ ESRCH |
| 4 | Plain PID (flags=0) | Zombie leader PID | ❌ ESRCH | — |
| 5 | Plain TID (flags=0) | Alive non-leader TID | ✅ OK | — |

## Analysis

**`pidfd_get_task()` always uses `PIDTYPE_TGID`** when resolving real pidfds
(kernel/pid.c line 640). This means:

1. For a non-leader thread's pid, `pid->tasks[PIDTYPE_TGID]` is empty → returns
   NULL → `-ESRCH`.
2. When the thread-group leader has exited (zombie), `get_pid_task(pid,
   PIDTYPE_TGID)` also returns NULL → `-ESRCH`.

The only path that works for a zombie-leader process is the plain TID path
(`find_get_task_by_vpid()` which finds any task), but `process_madvise` has no
such fallback since it always takes a pidfd.

**This behavior is identical for `process_vm_readv(PROCESS_VM_PIDFD)` and
`process_madvise()`** — it is a shared `pidfd_get_task()` limitation, not
specific to the PROCESS_VM_PIDFD patch.

In practice, this is acceptable because:
1. All threads in a thread group share the same `mm_struct`
2. Profilers use a regular pidfd for the thread-group leader (which is almost
   always alive)
3. The zombie-leader edge case is rare (requires `pthread_exit()` in main)
4. If this needs fixing, it should be done in `pidfd_get_task()` itself
