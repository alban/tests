package main

import (
	"os"
	"runtime"
	"sync"
	"testing"
	"time"
	"unsafe"

	"golang.org/x/sys/unix"
)

const (
	PVM_FLAGS_PIDFD  = 1 << iota // 1
	PVM_FLAGS_NOWAIT             // 2
)

const (
	// userfaultfd is part of Linux UAPI
	// Defined in /usr/include/linux/userfaultfd.h
	UFFD_API                     = 0xAA
	UFFD_EVENT_PAGEFAULT         = 0x12
	UFFDIO_API                   = 0xc018aa3f
	UFFDIO_REGISTER_MODE_MISSING = 0x01
	UFFDIO_REGISTER              = 0xc020aa00
	UFFDIO_COPY                  = 0xc028aa03
)

func userfaultfd(flags int) (int, error) {
	r1, _, errno := unix.Syscall(unix.SYS_USERFAULTFD, uintptr(flags), 0, 0)
	if errno != 0 {
		return -1, errno
	}
	return int(r1), nil
}

func setupUserfaultfd(t *testing.T) int {
	uffd, err := userfaultfd(unix.O_CLOEXEC | unix.O_NONBLOCK)
	if err != nil {
		t.Fatalf("userfaultfd: %v", err)
	}

	api := struct {
		api      uint64
		features uint64
		ioctls   uint64
	}{
		api: UFFD_API,
	}

	_, _, errno := unix.Syscall(unix.SYS_IOCTL,
		uintptr(uffd),
		uintptr(UFFDIO_API),
		uintptr(unsafe.Pointer(&api)))
	if errno != 0 {
		unix.Close(uffd)
		t.Fatalf("UFFDIO_API: %v", errno)
	}

	return uffd
}

func allocateAndRegisterMemory(t *testing.T, uffd int, size int) ([]byte, error) {
	mem, err := unix.Mmap(-1, 0, size,
		unix.PROT_READ|unix.PROT_WRITE,
		unix.MAP_PRIVATE|unix.MAP_ANONYMOUS)
	if err != nil {
		t.Fatalf("mmap: %v", err)
	}
	t.Logf("Allocated memory with mmap at %p", &mem[0])

	reg := struct {
		start uint64
		len   uint64
		mode  uint64
	}{
		start: uint64(uintptr(unsafe.Pointer(&mem[0]))),
		len:   uint64(size),
		mode:  UFFDIO_REGISTER_MODE_MISSING,
	}

	_, _, errno := unix.Syscall(unix.SYS_IOCTL,
		uintptr(uffd),
		uintptr(UFFDIO_REGISTER),
		uintptr(unsafe.Pointer(&reg)))
	if errno != 0 {
		t.Fatalf("UFFDIO_REGISTER: %v", errno)
	}

	return mem, nil
}

func handleUserfaultfd(t *testing.T, uffd int, content []byte, delay time.Duration, max int) *sync.WaitGroup {
	var wg sync.WaitGroup
	wg.Add(1)

	go func() {
		defer wg.Done()
		timeout := time.After(4 * time.Second)
		count := 0

		for {
			select {
			case <-timeout:
				t.Logf("userfaultfd handler timed out after 4 seconds")
				return
			default:
				// Buffer for reading userfaultfd events
				buf := make([]byte, 32) // sizeof(struct uffd_msg)

				// Read userfaultfd events
				n, err := unix.Read(uffd, buf)
				if err != nil {
					if err == unix.EAGAIN {
						continue // No events available
					}
					t.Errorf("read userfaultfd: %v", err)
					return
				}

				if n < 32 {
					t.Errorf("short read on userfaultfd: %d bytes", n)
					return
				}

				count++

				// Parse the userfaultfd message
				msg := (*struct {
					event     uint8
					reserved1 uint8
					reserved2 uint16
					reserved3 uint32
					arg       struct {
						pagefault struct {
							flags   uint64
							address uint64
							feat    struct {
								ptid uint32
							}
						}
					}
				})(unsafe.Pointer(&buf[0]))

				if msg.event == UFFD_EVENT_PAGEFAULT {
					addr := msg.arg.pagefault.address
					t.Logf("Page fault at address 0x%x", addr)
					// Resolve the page fault by copying data
					pageSize := os.Getpagesize()
					pageAddr := addr &^ uint64(pageSize-1) // Align to page boundary
					t.Logf("Handling page fault for page at 0x%x", pageAddr)

					// Allocate source page with mmap (not Go-managed memory)
					sourcePage, err := unix.Mmap(-1, 0, pageSize,
						unix.PROT_READ|unix.PROT_WRITE,
						unix.MAP_PRIVATE|unix.MAP_ANONYMOUS)
					if err != nil {
						t.Errorf("mmap source page: %v", err)
						return
					}
					defer unix.Munmap(sourcePage)

					copy(sourcePage, content)

					copy := struct {
						dst  uint64
						src  uint64
						len  uint64
						mode uint64
					}{
						dst:  pageAddr,
						src:  uint64(uintptr(unsafe.Pointer(&sourcePage[0]))),
						len:  uint64(pageSize),
						mode: 0,
					}

					if delay > 0 {
						time.Sleep(delay)
					}
					_, _, errno := unix.Syscall(unix.SYS_IOCTL,
						uintptr(uffd),
						uintptr(UFFDIO_COPY),
						uintptr(unsafe.Pointer(&copy)))
					if errno != 0 {
						t.Errorf("UFFDIO_COPY failed: %v\n", errno)
						return
					}
				}

				if max > 0 && count >= max {
					return
				}
			}
		}
	}()

	return &wg
}

func TestProcessVmReadv(t *testing.T) {
	var pinner runtime.Pinner

	// Set up userfaultfd
	uffd := setupUserfaultfd(t)
	defer unix.Close(uffd)

	// Regular buffer without userfaultfd
	data := []byte{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08}
	pinner.Pin(&data[0])
	defer pinner.Unpin()
	dataPtr := unsafe.Pointer(&data[0])

	// Buffer with userfaultfd
	mem, err := allocateAndRegisterMemory(t, uffd, os.Getpagesize())
	if err != nil {
		t.Fatal(err)
	}
	defer unix.Munmap(mem)
	dataWithUserfaultfdPtr := unsafe.Pointer(&mem[0])

	tests := []struct {
		name                     string
		flags                    uint
		expectError              bool
		expectedErr              error
		usePidFd                 bool
		useUffd                  bool
		expectedUffdHandlerCalls int
		useNullPtr               bool
	}{
		{
			name:        "read from normal buffer",
			flags:       0,
			expectError: false,
		},
		{
			name:        "invalid flag",
			flags:       255,
			expectError: true,
			expectedErr: unix.EINVAL,
		},
		{
			name:        "invalid address",
			flags:       0,
			expectError: true,
			expectedErr: unix.EFAULT,
			useNullPtr:  true,
		},
		{
			name:        "invalid address and invalid flag",
			flags:       255,
			expectError: true,
			expectedErr: unix.EINVAL,
			useNullPtr:  true,
		},
		{
			name:        "invalid address and all flag",
			flags:       PVM_FLAGS_NOWAIT | PVM_FLAGS_PIDFD,
			expectError: true,
			expectedErr: unix.EFAULT,
			useNullPtr:  true,
			usePidFd:    true,
		},
		{
			name:                     "read from userfaultfd buffer",
			flags:                    0,
			expectError:              false,
			useUffd:                  true,
			expectedUffdHandlerCalls: 1,
		},
		{
			name:                     "read from userfaultfd buffer (again)",
			flags:                    0,
			expectError:              false,
			useUffd:                  true,
			expectedUffdHandlerCalls: 1,
		},
		{
			name:        "read with NOWAIT from normal buffer",
			flags:       PVM_FLAGS_NOWAIT,
			expectError: false,
		},
		{
			name:        "read with NOWAIT from userfaultfd buffer",
			flags:       PVM_FLAGS_NOWAIT,
			expectError: true,
			expectedErr: unix.EFAULT,
			useUffd:     true,
		},
		{
			name:        "read from normal buffer with pidfd",
			flags:       PVM_FLAGS_PIDFD,
			expectError: false,
			usePidFd:    true,
		},
		{
			name:        "read with NOWAIT and PIDFD from normal buffer",
			flags:       PVM_FLAGS_NOWAIT | PVM_FLAGS_PIDFD,
			expectError: false,
			usePidFd:    true,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			// Create buffer to read into
			readBuffer := make([]byte, len(data))

			// Set up local and remote iovec structures
			localIov := unix.Iovec{
				Base: &readBuffer[0],
				Len:  uint64(len(readBuffer)),
			}

			remoteIov := unix.RemoteIovec{
				Base: uintptr(dataPtr),
				Len:  int(len(data)),
			}

			if tt.useNullPtr {
				remoteIov.Base = uintptr(0)
			} else if tt.useUffd {
				remoteIov.Base = uintptr(dataWithUserfaultfdPtr)
				unix.Madvise(mem, unix.MADV_DONTNEED)
				wg := handleUserfaultfd(t, uffd, data, time.Second, tt.expectedUffdHandlerCalls)
				defer wg.Wait() // Wait for the goroutine to complete
			}

			var pidOrFd int
			if tt.usePidFd {
				// Use pidfd_open to get a file descriptor for the current process
				pidFd, err := unix.PidfdOpen(os.Getpid(), 0)
				if err != nil {
					t.Skipf("pidfd_open not supported: %v", err)
				}
				defer unix.Close(pidFd)
				pidOrFd = pidFd
			} else {
				pidOrFd = os.Getpid()
			}

			// Call process_vm_readv using the unix package
			t.Logf("Calling process_vm_readv(%d, %x, %x, %d)", pidOrFd, localIov.Base, remoteIov.Base, tt.flags)
			n, err := unix.ProcessVMReadv(
				pidOrFd,
				[]unix.Iovec{localIov},
				[]unix.RemoteIovec{remoteIov},
				tt.flags,
			)

			if tt.expectError {
				if err == nil {
					t.Fatalf("expected error %v, but got nil", tt.expectedErr)
				}
				if err != tt.expectedErr {
					t.Fatalf("expected error %v, but got %v", tt.expectedErr, err)
				}
				t.Logf("Got expected error: %v", err)
				return
			}

			if err != nil {
				t.Fatalf("process_vm_readv failed: %v", err)
			}

			if n != len(data) {
				t.Fatalf("expected to read %d bytes, but read %d", len(data), n)
			}

			// Verify the data matches
			for i := 0; i < len(data); i++ {
				if readBuffer[i] != data[i] {
					t.Fatalf("data mismatch at index %d: expected 0x%02x, got 0x%02x", i, data[i], readBuffer[i])
				}
			}

			t.Logf("Successfully read %d bytes via process_vm_readv: %v", n, readBuffer)
		})
	}
}
