package dataplane

import (
	"errors"
	"os"
	"os/exec"
	"strings"
	"testing"

	"github.com/oublie6/dpdk-high-speed-flow-router/dataplane/native"
)

type failingTeardownAPI struct {
	cleanupCalls int
}

func (*failingTeardownAPI) Teardown() error { return errors.New("injected teardown failure") }
func (*failingTeardownAPI) GetStats() (Stats, error) {
	return Stats{PortsClosed: 1, PoolInUse: 1}, nil
}
func (api *failingTeardownAPI) Cleanup() error {
	api.cleanupCalls++
	return nil
}

func TestTeardownFailureSkipsCleanup(t *testing.T) {
	api := &failingTeardownAPI{}
	r := &Runtime{}
	err := r.finishLifecycle(api, nil)
	if err == nil || !strings.Contains(err.Error(), "injected teardown failure") {
		t.Fatalf("expected injected teardown failure, got %v", err)
	}
	if api.cleanupCalls != 0 {
		t.Fatalf("Cleanup called %d times after Teardown failure", api.cleanupCalls)
	}
	if r.stats.PortsClosed != 1 || r.stats.PoolInUse != 1 {
		t.Fatalf("stats were not retained after Teardown failure: %+v", r.stats)
	}
}

// 每个 EAL case 都需要独立进程：cleanup 后不能在同一进程再次初始化。
// 真实 EAL 集成测试需要显式设置 FLOW_ROUTER_TEST_CPU，避免普通 unit test
// 无意占用 EAL 内存或修改当前线程 affinity。
// 这里把允许使用的 physical CPU 映射到 DPDK logical lcore 0。
func TestEALLifecycle(t *testing.T) {
	cpu := os.Getenv("FLOW_ROUTER_TEST_CPU")
	if cpu == "" {
		t.Skip("set FLOW_ROUTER_TEST_CPU to an allowed CPU for real EAL integration tests")
	}
	if mode := os.Getenv("FLOW_ROUTER_TEST_CHILD"); mode != "" {
		args := []string{"--lcores=0@" + cpu, "--no-huge", "--no-pci", "--no-shconf", "-m", "64"}
		if mode == "invalid" {
			args = append(args, "--flow-router-invalid-eal-option")
		}
		info, err := Probe(args)
		// DPDK 25.11.3 对未知参数会在 rte_eal_init 内直接退出进程，
		// invalid case 正常不会执行到这里；外层进程断言退出码和诊断文本。
		if err != nil {
			t.Fatal(err)
		}
		if !info.Initialized || info.MainLcore != 0 || info.LcoreCount != 1 || info.Version != "DPDK 25.11.3" {
			t.Fatalf("unexpected runtime info: %+v", info)
		}
		if _, err := Probe(args); err == nil {
			t.Fatal("second init attempt was accepted")
		}
		return
	}

	for _, mode := range []string{"success", "invalid"} {
		t.Run(mode, func(t *testing.T) {
			cmd := exec.Command(os.Args[0], "-test.run=^TestEALLifecycle$", "-test.v")
			cmd.Env = append(os.Environ(), "FLOW_ROUTER_TEST_CHILD="+mode)
			out, err := cmd.CombinedOutput()
			if mode == "invalid" {
				if err == nil || !strings.Contains(string(out), "unknown argument --flow-router-invalid-eal-option") {
					t.Fatalf("expected DPDK argparse failure, got %v\n%s", err, out)
				}
				return
			}
			if err != nil {
				t.Fatalf("%v\n%s", err, out)
			}
			t.Logf("%s", out)
		})
	}
}

func TestRejectNUL(t *testing.T) {
	if _, err := Probe([]string{"--no-huge\x00ignored"}); err == nil {
		t.Fatal("embedded NUL would silently truncate an EAL argument")
	}
}

func TestWorkerCountValidation(t *testing.T) {
	for _, workers := range []uint{5, 100} {
		err := validate(Config{EALArgs: []string{"--no-pci"}, Workers: workers})
		if err == nil || !strings.Contains(err.Error(), "workers must be between 1 and 4") {
			t.Fatalf("workers=%d validation error = %v", workers, err)
		}
	}
}

// Setup 失败发生在静态规则已经发布之后，必须仍由 Teardown 释放 hash/action，
// 然后才允许 EAL cleanup。独立子进程隔离一次性 EAL lifecycle。
func TestSetupFailureFreesStaticRules(t *testing.T) {
	cpu := os.Getenv("FLOW_ROUTER_TEST_CPU")
	if cpu == "" {
		t.Skip("set FLOW_ROUTER_TEST_CPU to run the setup failure cleanup test")
	}
	if os.Getenv("FLOW_ROUTER_SETUP_FAILURE_CHILD") == "1" {
		rules := RuleSnapshot{Flows: []FlowRule{{
			SrcIPv4: 0xc0000201, DstIPv4: 0xc6336402,
			SrcPort: 12345, DstPort: 23456, L4Proto: 17,
			Action: RuleAction{Type: native.ActionDrop},
		}}}
		args := []string{"--lcores=0@" + cpu, "--no-huge", "--no-pci",
			"--no-shconf", "--no-telemetry", "-m", "64"}
		runtime, err := Start(Config{EALArgs: args, RXDevice: "missing_rx",
			TXDevice: "missing_tx", Rules: rules})
		if err != nil {
			t.Fatal(err)
		}
		if _, err := runtime.Ready(); err == nil {
			t.Fatal("setup failure was not reported by Ready")
		}
		stats, err := runtime.Wait()
		if err == nil {
			t.Fatal("setup failure was not retained by Wait")
		}
		if !stats.FlowTableFreed || !stats.ActionStoreFreed ||
			!stats.SnapshotFreed || !stats.QSBRFreed {
			t.Fatalf("static resources were not freed after setup failure: %+v", stats)
		}
		return
	}
	cmd := exec.Command(os.Args[0], "-test.run=^TestSetupFailureFreesStaticRules$", "-test.v")
	cmd.Env = append(os.Environ(), "FLOW_ROUTER_SETUP_FAILURE_CHILD=1")
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("%v\n%s", err, out)
	}
	t.Logf("%s", out)
}
