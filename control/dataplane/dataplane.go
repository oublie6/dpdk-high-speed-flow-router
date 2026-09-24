// Package dataplane 是纯 Go lifecycle manager，packet hot path 全部留在 native。
package dataplane

import (
	"fmt"
	"runtime"
	"strings"
	"sync"

	"github.com/oublie6/dpdk-high-speed-flow-router/dataplane/native"
)

type Info = native.Info
type Stats = native.Stats

type Config struct {
	EALArgs            []string
	RXDevice, TXDevice string
	Probe              bool
	Rules              RuleSnapshot
}

// EAL 是 process-global、不可重入的资源；即使 init 失败，也不允许第二次尝试。
var lifecycle struct {
	sync.Mutex
	attempted bool
}

type Runtime struct {
	ready, done   chan struct{}
	info          Info
	stats         Stats
	startErr, err error
}

func validate(cfg Config) error {
	noPCI := false
	for _, arg := range cfg.EALArgs {
		if strings.IndexByte(arg, 0) >= 0 {
			return fmt.Errorf("EAL argument contains a NUL byte")
		}
		if arg == "--no-pci" {
			noPCI = true
		}
	}
	if !noPCI {
		return fmt.Errorf("software-only runtime requires --no-pci")
	}
	if !cfg.Probe && (cfg.RXDevice == "" || cfg.TXDevice == "" || cfg.RXDevice == cfg.TXDevice) {
		return fmt.Errorf("RX/TX device names must be distinct and nonempty")
	}
	if strings.IndexByte(cfg.RXDevice, 0) >= 0 || strings.IndexByte(cfg.TXDevice, 0) >= 0 {
		return fmt.Errorf("device name contains a NUL byte")
	}
	if err := validateRules(cfg.Rules); err != nil {
		return fmt.Errorf("static rules: %w", err)
	}
	return nil
}

// Start 立即返回 handle；Ready 等待初始化结果，Stop 可在初始化期间请求，Wait 等待清理。
func Start(cfg Config) (*Runtime, error) {
	if err := validate(cfg); err != nil {
		return nil, err
	}
	lifecycle.Lock()
	defer lifecycle.Unlock()
	if lifecycle.attempted {
		return nil, fmt.Errorf("EAL lifecycle: only one initialization attempt per process is supported")
	}
	lifecycle.attempted = true
	// caller 返回后可以修改原始 slice；manager 拥有自己的参数快照。
	cfg.EALArgs = append([]string(nil), cfg.EALArgs...)
	cfg.Rules = cloneRules(cfg.Rules)
	r := &Runtime{ready: make(chan struct{}), done: make(chan struct{})}
	go r.owner(cfg)
	return r, nil
}

func combine(first, next error) error {
	if first == nil {
		return next
	}
	if next == nil {
		return first
	}
	return fmt.Errorf("%v; %w", first, next)
}

// cleanupAPI 只描述 worker 返回后的生命周期操作，便于用可控 fake 验证失败路径。
// 生产路径仍直接调用 native package，不改变 Go/C 的粗粒度边界。
type cleanupAPI interface {
	Teardown() error
	GetStats() (Stats, error)
	Cleanup() error
}

type nativeCleanupAPI struct{}

func (nativeCleanupAPI) Teardown() error          { return native.Teardown() }
func (nativeCleanupAPI) GetStats() (Stats, error) { return native.GetStats() }
func (nativeCleanupAPI) Cleanup() error           { return native.Cleanup() }

func (r *Runtime) finishLifecycle(api cleanupAPI, runErr error) error {
	// Teardown 失败意味着 port 或 mempool 可能仍被 DPDK 持有。此时不能进入
	// rte_eal_cleanup；保留资源到进程退出，由操作系统完成最终回收。
	teardownErr := api.Teardown()

	var statsErr error
	r.stats, statsErr = api.GetStats()
	err := combine(runErr, teardownErr)
	err = combine(err, statsErr)
	if teardownErr != nil {
		return err
	}

	cleanupErr := api.Cleanup()
	return combine(err, cleanupErr)
}

func (r *Runtime) owner(cfg Config) {
	// 不 Unlock：goroutine 退出时 Go 回收此 OS thread，避免 EAL affinity 污染调度器。
	runtime.LockOSThread()
	err := native.Init(cfg.EALArgs)
	initialized := err == nil
	if err == nil {
		r.info, err = native.GetInfo()
		if err == nil && r.info.LcoreCount != 1 {
			err = fmt.Errorf("exactly one EAL lcore is required")
		}
	}
	if err == nil && !cfg.Probe {
		err = native.ConfigureRules(toNativeRules(cfg.Rules))
	}
	if err == nil && !cfg.Probe {
		err = native.Setup(cfg.RXDevice, cfg.TXDevice)
		if err == nil {
			r.info, err = native.GetInfo()
		}
	}
	r.startErr = err
	close(r.ready) // channel close 发布初始化结果，Ready 之后读取不与 owner 竞争。
	if err == nil && !cfg.Probe {
		err = native.Run()
	}
	if initialized {
		// Run 返回就是 worker 已停止；此后仍在同一个 owner thread 按依赖顺序清理。
		err = r.finishLifecycle(nativeCleanupAPI{}, err)
	}
	r.err = err
	close(r.done)
}

func (r *Runtime) Ready() (Info, error)  { <-r.ready; return r.info, r.startErr }
func (r *Runtime) Stop()                 { native.RequestStop() }
func (r *Runtime) Done() <-chan struct{} { return r.done }
func (r *Runtime) Wait() (Stats, error)  { <-r.done; return r.stats, r.err }

// Probe 保留 Goal 001 的同步 EAL 回归入口，使用同一个 owner/lifecycle 实现。
func Probe(args []string) (Info, error) {
	r, err := Start(Config{EALArgs: args, Probe: true})
	if err != nil {
		return Info{}, err
	}
	info, _ := r.Ready()
	_, err = r.Wait()
	return info, err
}
