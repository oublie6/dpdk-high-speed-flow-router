package dataplane

import (
	"os"
	"os/exec"
	"strings"
	"testing"
)

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
		if mode == "invalid" {
			if err == nil || !strings.Contains(err.Error(), "EAL init") {
				t.Fatalf("expected contextual init error, got %v", err)
			}
		} else {
			if err != nil {
				t.Fatal(err)
			}
			if !info.Initialized || info.MainLcore != 0 || info.LcoreCount != 1 || info.Version == "" {
				t.Fatalf("unexpected runtime info: %+v", info)
			}
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
