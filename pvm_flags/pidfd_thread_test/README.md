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
# Run on a kernel with PROCESS_VM_PIDFD support:
sudo vimto --kernel ghcr.io/alban/ci-kernels:7.1-rc6-patch-alban-f6d1fd027fc1 exec -- ./test_pidfd_thread
```

## Results (kernel v7.1-rc6 with pvm_flags patch)

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

## Analysis

| Test | pidfd type | Target | process_vm_readv | process_madvise |
|------|-----------|--------|-----------------|-----------------|
| 1 | Regular (`pidfd_open(pid, 0)`) | Main PID | ✅ OK | ✅ OK |
| 2 | `PIDFD_THREAD` | Main thread (=leader) | ✅ OK | ✅ OK |
| 3 | `PIDFD_THREAD` | Non-leader thread | ❌ ESRCH | ❌ ESRCH |
| 4 | Regular | Non-leader TID | pidfd_open fails | — |

**Conclusion**: `pidfd_get_task()` always uses `PIDTYPE_TGID` when resolving
real pidfds (kernel/pid.c line 640). For a non-leader thread's pid, the
`pid->tasks[PIDTYPE_TGID]` list is empty, so it returns NULL → `-ESRCH`.

This behavior is **identical** for `process_vm_readv(PROCESS_VM_PIDFD)` and
`process_madvise()` — it is a shared `pidfd_get_task()` limitation, not
specific to the PROCESS_VM_PIDFD patch.

In practice, this is not a problem for profilers because:
1. All threads in a thread group share the same `mm_struct`
2. Profilers would use a regular pidfd for the thread-group leader
3. If this needs fixing, it should be done in `pidfd_get_task()` itself
