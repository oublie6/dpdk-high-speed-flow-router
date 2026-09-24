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
	Workers            uint
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
	rulesMu       sync.Mutex
	currentRules  RuleSnapshot
	generation    uint64
	rulesOpen     bool
	publishRules  func(native.RuleSnapshot) (uint64, error)
}

func validate(cfg Config) error {
	if cfg.Workers < 1 || cfg.Workers > 4 {
		return fmt.Errorf("workers must be between 1 and 4")
	}
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
	if cfg.Workers == 0 {
		cfg.Workers = 1
	}
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
	r := &Runtime{ready: make(chan struct{}), done: make(chan struct{}),
		currentRules: cloneRules(cfg.Rules), publishRules: native.PublishRules}
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
		if err == nil && r.info.LcoreCount != cfg.Workers {
			err = fmt.Errorf("EAL enabled lcore count %d does not match workers %d",
				r.info.LcoreCount, cfg.Workers)
		}
	}
	if err == nil && !cfg.Probe {
		err = native.ConfigureRules(toNativeRules(cfg.Rules))
	}
	if err == nil && !cfg.Probe {
		err = native.Setup(cfg.RXDevice, cfg.TXDevice, cfg.Workers)
		if err == nil {
			r.info, err = native.GetInfo()
		}
	}
	if err == nil && !cfg.Probe {
		r.rulesMu.Lock()
		r.generation = 1
		r.rulesOpen = true
		r.rulesMu.Unlock()
	}
	r.startErr = err
	close(r.ready) // channel close 发布初始化结果，Ready 之后读取不与 owner 竞争。
	if err == nil && !cfg.Probe {
		err = native.Run()
	}
	if initialized {
		// 与动态 writer 使用同一把 Go mutex：Run 返回后先关闭发布入口，再在
		// owner thread 清理，避免 Teardown 与尚未结束的 PublishRules 并发。
		r.rulesMu.Lock()
		r.rulesOpen = false
		err = r.finishLifecycle(nativeCleanupAPI{}, err)
		r.rulesMu.Unlock()
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
	r, err := Start(Config{EALArgs: args, Workers: 1, Probe: true})
	if err != nil {
		return Info{}, err
	}
	info, _ := r.Ready()
	_, err = r.Wait()
	return info, err
}
