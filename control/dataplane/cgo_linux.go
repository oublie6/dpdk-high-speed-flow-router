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

// 项目 C API 统一返回负 errno，而不是把 DPDK 内部错误直接暴露给 Go。
// 这里补充操作名称，并通过 wrapped errno 保留 errors.Is 的判断能力。
func status(operation string, rc C.int) error {
	if rc == 0 {
		return nil
	}
	return fmt.Errorf("%s: %w (status %d)", operation, syscall.Errno(-rc), int(rc))
}

// Probe 同步完成一次 EAL 初始化、运行时信息复制和 cleanup。
// 当前约束为一个进程只允许尝试初始化一次。
// args 只包含 EAL 参数，不包含 argv[0]；C 不会长期持有任何 Go pointer。
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

	// EAL 会记录 thread-local lcore state，并可能修改调用线程 affinity。
	// 因此 init/info/cleanup 全部固定在同一个 OS thread 上执行。
	// 这个 goroutine 完成后直接退出，不把带有 EAL affinity 的 thread
	// 重新交给普通 Go scheduler 使用。
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

	// EAL 允许调整 argv 的 pointer 顺序，所以单独保存每个原始 C string 地址，
	// cleanup 时按原始地址逐一释放，避免依赖被 EAL 修改后的 argv。
	// C 数组额外分配一个 zeroed slot 作为 argv[argc] == NULL。
	// 传给 C 的数组中只保存 C pointer；Go slice 只用于本地 bookkeeping。
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
		// EAL init 失败后可能仍残留 process-global state。
		// 当前把失败视为终止状态，不 retry，也不主动释放这批很小的 C 内存；
		// 让进程退出统一回收，避免误释放潜在仍被 EAL 引用的地址。
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
			// cleanup 没有完整成功时同样保留 argv 到进程退出。
			return
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
