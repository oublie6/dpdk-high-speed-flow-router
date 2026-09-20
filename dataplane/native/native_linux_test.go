package native

import (
	"errors"
	"os"
	"os/exec"
	"runtime"
	"syscall"
	"testing"
)

func TestRuntimeCleanupRejectsLiveResources(t *testing.T) {
	cases := []struct {
		name     string
		resource int
	}{
		{name: "worker", resource: testLiveWorker},
		{name: "port", resource: testLivePort},
		{name: "mempool", resource: testLiveMempool},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			err := testCleanupGuard(tc.resource)
			if !errors.Is(err, syscall.EBUSY) {
				t.Fatalf("cleanup guard returned %v, want EBUSY", err)
			}
		})
	}
}

// partial return 测试必须在真实 EAL/mempool 上运行，也必须与其他 EAL case
// 隔离到新进程，因为同一进程不允许第二次初始化 EAL。
func TestPartialTXOwnership(t *testing.T) {
	cpu := os.Getenv("FLOW_ROUTER_TEST_CPU")
	if cpu == "" {
		t.Skip("set FLOW_ROUTER_TEST_CPU to run the real EAL ownership test")
	}
	if os.Getenv("FLOW_ROUTER_NATIVE_TEST_CHILD") == "1" {
		runtime.LockOSThread()
		args := []string{"--lcores=0@" + cpu, "--no-huge", "--no-pci",
			"--no-shconf", "--no-telemetry", "-m", "64"}
		if err := Init(args); err != nil {
			t.Fatal(err)
		}
		if err := testPartialTXOwnership(); err != nil {
			t.Fatal(err)
		}
		if err := Cleanup(); err != nil {
			t.Fatal(err)
		}
		return
	}
	cmd := exec.Command(os.Args[0], "-test.run=^TestPartialTXOwnership$", "-test.v")
	cmd.Env = append(os.Environ(), "FLOW_ROUTER_NATIVE_TEST_CHILD=1")
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("%v\n%s", err, out)
	}
	t.Logf("%s", out)
}
