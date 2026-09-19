package dataplane

/*
#cgo pkg-config: libdpdk
#cgo CFLAGS: -I${SRCDIR}/../../dataplane/include
#include <stdlib.h>
#include "dp_api.h"
*/
import "C"

import (
	"fmt"
	"runtime"
	"strings"
	"sync"
	"syscall"
	"unsafe"
)

var lifecycle struct {
	sync.Mutex
	attempted bool
}

// The public C API returns negative errno values, not DPDK internals. Keep
// the operation name and preserve errors.Is support through the wrapped errno.
func status(operation string, rc C.int) error {
	if rc == 0 {
		return nil
	}
	return fmt.Errorf("%s: %w (status %d)", operation, syscall.Errno(-rc), int(rc))
}

// Probe initializes EAL, copies runtime information and cleans up synchronously.
// EAL supports one initialization attempt per process. Pass only EAL arguments,
// without argv[0]. No Go pointer is retained by C.
func Probe(args []string) (info Info, err error) {
	lifecycle.Lock()
	defer lifecycle.Unlock()
	if lifecycle.attempted {
		return info, fmt.Errorf("EAL lifecycle: only one initialization attempt per process is supported")
	}
	for _, arg := range args {
		if strings.IndexByte(arg, 0) >= 0 {
			return info, fmt.Errorf("EAL argument contains a NUL byte")
		}
	}

	// EAL records thread-local lcore state and changes the calling thread's
	// affinity. Keep all calls on this thread; let Go retire it when this
	// dedicated goroutine exits instead of returning it to the scheduler.
	result := make(chan probeResult, 1)
	lifecycle.attempted = true
	go func() {
		runtime.LockOSThread()
		i, e := probe(args)
		result <- probeResult{i, e}
	}()
	r := <-result
	return r.info, r.err
}

type probeResult struct {
	info Info
	err  error
}

func probe(args []string) (info Info, err error) {
	argv := make([]*C.char, len(args)+1)
	argv[0] = C.CString("flow-router")
	for i, arg := range args {
		argv[i+1] = C.CString(arg)
	}
	// EAL may reorder argv. Keep the original allocations separately so every
	// string is freed exactly once, independent of the mutated pointer array.
	// Allocate one extra zeroed slot for argv[argc]. Only C pointers are
	// written into this C array; the Go slice is bookkeeping, never passed.
	memory := C.calloc(C.size_t(len(argv)+1), C.size_t(unsafe.Sizeof(uintptr(0))))
	if memory == nil {
		for _, p := range argv {
			C.free(unsafe.Pointer(p))
		}
		return info, fmt.Errorf("allocate EAL argv: out of memory")
	}
	for i, p := range argv {
		*(**C.char)(unsafe.Pointer(uintptr(memory) + uintptr(i)*unsafe.Sizeof(p))) = p
	}
	if err = status("EAL init", C.dp_runtime_init(C.int(len(argv)), (**C.char)(memory))); err != nil {
		// Failed EAL initialization is terminal: DPDK may retain argv pointers.
		// Keep these C allocations until process exit; do not retry EAL.
		return info, err
	}
	defer func() {
		cleanupErr := status("EAL cleanup", C.dp_runtime_cleanup())
		if cleanupErr != nil {
			if err != nil {
				err = fmt.Errorf("%v; %w", err, cleanupErr)
			} else {
				err = cleanupErr
			}
			return // Preserve argv if cleanup did not complete.
		}
		for _, p := range argv {
			C.free(unsafe.Pointer(p))
		}
		C.free(memory)
	}()
	var ci C.struct_dp_runtime_info
	if err = status("EAL runtime info", C.dp_runtime_get_info(&ci)); err != nil {
		return info, err
	}
	return Info{
		Initialized: ci.initialized != 0,
		MainLcore:   uint(ci.main_lcore),
		LcoreCount:  uint(ci.lcore_count),
		Version:     C.GoString(ci.version),
	}, nil
}
