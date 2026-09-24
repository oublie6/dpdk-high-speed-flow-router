package native

import (
	"errors"
	"fmt"
	"io/ioutil"
	"os"
	"os/exec"
	"reflect"
	"runtime"
	"strconv"
	"strings"
	"syscall"
	"testing"
)

func TestPacketParserValidMetadata(t *testing.T) {
	cases := []struct {
		name    string
		fixture int
		want    testPacketMeta
	}{
		{name: "valid TCP", fixture: fixtureValidTCP, want: testPacketMeta{
			EtherType: 0x0800, SrcIPv4: 0xc0000201, DstIPv4: 0xc6336402,
			L4Proto: 6, SrcPort: 12345, DstPort: 443,
			L2Len: 14, L3Len: 20, L4Len: 20, L4Offset: 34,
		}},
		{name: "valid UDP", fixture: fixtureValidUDP, want: testPacketMeta{
			EtherType: 0x0800, SrcIPv4: 0xc0000201, DstIPv4: 0xc6336402,
			L4Proto: 17, SrcPort: 12345, DstPort: 53,
			L2Len: 14, L3Len: 20, L4Len: 8, L4Offset: 34,
		}},
		{name: "IPv4 IHL greater than five", fixture: fixtureIPv4Options, want: testPacketMeta{
			EtherType: 0x0800, SrcIPv4: 0xc0000201, DstIPv4: 0xc6336402,
			L4Proto: 6, SrcPort: 12345, DstPort: 443,
			L2Len: 14, L3Len: 24, L4Len: 20, L4Offset: 38,
		}},
		{name: "TCP data offset greater than five", fixture: fixtureTCPOptions, want: testPacketMeta{
			EtherType: 0x0800, SrcIPv4: 0xc0000201, DstIPv4: 0xc6336402,
			L4Proto: 6, SrcPort: 12345, DstPort: 443,
			L2Len: 14, L3Len: 20, L4Len: 24, L4Offset: 34,
		}},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			result, got := testParseFixture(tc.fixture)
			if result != parseOK {
				t.Fatalf("parse result = %d, want DP_PARSE_OK", result)
			}
			if !reflect.DeepEqual(got, tc.want) {
				t.Fatalf("metadata = %+v, want %+v", got, tc.want)
			}
		})
	}
}

func TestPacketParserRejectedFixtures(t *testing.T) {
	cases := []struct {
		name       string
		fixture    int
		wantResult int
	}{
		{name: "non IPv4", fixture: fixtureNonIPv4, wantResult: parseUnsupported},
		{name: "unsupported L4", fixture: fixtureUnsupportedL4, wantResult: parseUnsupported},
		{name: "IPv4 fragment", fixture: fixtureIPv4Fragment, wantResult: parseUnsupported},
		{name: "Ethernet truncated", fixture: fixtureEthernetTruncated, wantResult: parseMalformed},
		{name: "IPv4 truncated", fixture: fixtureIPv4Truncated, wantResult: parseMalformed},
		{name: "invalid IPv4 version", fixture: fixtureInvalidIPv4Version, wantResult: parseMalformed},
		{name: "invalid IHL", fixture: fixtureInvalidIHL, wantResult: parseMalformed},
		{name: "invalid IPv4 total length", fixture: fixtureInvalidTotalLength, wantResult: parseMalformed},
		{name: "IPv4 total length beyond frame", fixture: fixtureTotalBeyondFrame, wantResult: parseMalformed},
		{name: "TCP truncated", fixture: fixtureTCPTruncated, wantResult: parseMalformed},
		{name: "invalid TCP data offset", fixture: fixtureInvalidTCPOffset, wantResult: parseMalformed},
		{name: "TCP header beyond IPv4 payload", fixture: fixtureTCPBeyondPayload, wantResult: parseMalformed},
		{name: "UDP truncated", fixture: fixtureUDPTruncated, wantResult: parseMalformed},
		{name: "UDP length below header", fixture: fixtureInvalidUDPLength, wantResult: parseMalformed},
		{name: "UDP length beyond IPv4 payload", fixture: fixtureUDPBeyondPayload, wantResult: parseMalformed},
		{name: "multi segment mbuf", fixture: fixtureMultiSegment, wantResult: parseUnsupported},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			result, _ := testParseFixture(tc.fixture)
			if result != tc.wantResult {
				t.Fatalf("parse result = %d, want %d", result, tc.wantResult)
			}
		})
	}
}

func TestRuntimeCleanupRejectsLiveResources(t *testing.T) {
	cases := []struct {
		name     string
		resource int
	}{
		{name: "worker", resource: testLiveWorker},
		{name: "port", resource: testLivePort},
		{name: "mempool", resource: testLiveMempool},
		{name: "active rule snapshot", resource: testLiveActiveRules},
		{name: "QSBR", resource: testLiveQSBR},
		{name: "writer", resource: testLiveWriter},
		{name: "reader", resource: testLiveReader},
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

func TestWorkerStatsAggregationAndLayout(t *testing.T) {
	if err := testWorkerStatsLayout(); err != nil {
		t.Fatal(err)
	}
}

func TestWorkerSetupValidation(t *testing.T) {
	if err := testQueueCapacity(1, 4, 2); !errors.Is(err, syscall.ENOSPC) {
		t.Fatalf("RX queue capacity error = %v, want ENOSPC", err)
	}
	if err := testQueueCapacity(4, 1, 2); !errors.Is(err, syscall.ENOSPC) {
		t.Fatalf("TX queue capacity error = %v, want ENOSPC", err)
	}
	if err := testQueueCapacity(4, 4, 4); err != nil {
		t.Fatalf("valid queue capacity rejected: %v", err)
	}
	if err := testWorkerSocket(0, 1); !errors.Is(err, syscall.EXDEV) {
		t.Fatalf("cross-NUMA worker error = %v, want EXDEV", err)
	}
	if err := testWorkerSocket(0, 0); err != nil {
		t.Fatalf("same-NUMA worker rejected: %v", err)
	}
}

func allowedCPUs(t *testing.T) []int {
	t.Helper()
	data, err := ioutil.ReadFile("/proc/self/status")
	if err != nil {
		t.Fatal(err)
	}
	for _, line := range strings.Split(string(data), "\n") {
		if !strings.HasPrefix(line, "Cpus_allowed_list:") {
			continue
		}
		var cpus []int
		for _, part := range strings.Split(strings.TrimSpace(strings.TrimPrefix(line, "Cpus_allowed_list:")), ",") {
			bounds := strings.Split(part, "-")
			first, parseErr := strconv.Atoi(bounds[0])
			if parseErr != nil {
				t.Fatal(parseErr)
			}
			last := first
			if len(bounds) == 2 {
				last, parseErr = strconv.Atoi(bounds[1])
				if parseErr != nil {
					t.Fatal(parseErr)
				}
			}
			for cpu := first; cpu <= last; cpu++ {
				cpus = append(cpus, cpu)
			}
		}
		return cpus
	}
	t.Fatal("Cpus_allowed_list not found")
	return nil
}

// 在真实 2-lcore EAL/TAP 上覆盖 lcore mismatch、partial queue setup、remote
// launch failure 和 worker error。Run 必须 stop/join 已启动 lcore，随后才能
// 安全释放 readers、snapshot、QSBR、ports、pool 与 EAL。
func TestMultiWorkerFailureCleanup(t *testing.T) {
	if os.Getenv("FLOW_ROUTER_TEST_CPU") == "" {
		t.Skip("set FLOW_ROUTER_TEST_CPU to run real multi-worker failure tests")
	}
	if mode := os.Getenv("FLOW_ROUTER_MULTI_FAILURE_CHILD"); mode != "" {
		cpus := allowedCPUs(t)
		if len(cpus) < 2 {
			t.Skip("at least two allowed CPUs are required")
		}
		runtime.LockOSThread()
		rxIface := fmt.Sprintf("nrx%d", os.Getpid())
		txIface := fmt.Sprintf("ntx%d", os.Getpid())
		args := []string{fmt.Sprintf("--lcores=0@%d,1@%d", cpus[0], cpus[1]),
			"--no-huge", "--no-pci", "--no-shconf", "--no-telemetry", "-m", "256",
			"--vdev=net_tap_rx,iface=" + rxIface,
			"--vdev=net_tap_tx,iface=" + txIface}
		if err := Init(args); err != nil {
			t.Fatal(err)
		}
		if err := ConfigureRules(RuleSnapshot{}); err != nil {
			t.Fatal(err)
		}
		if mode == "mismatch" {
			if err := Setup("net_tap_rx", "net_tap_tx", 1); !errors.Is(err, syscall.EINVAL) {
				t.Fatalf("lcore/worker mismatch error = %v, want EINVAL", err)
			}
			if err := Teardown(); err != nil {
				t.Fatal(err)
			}
			if err := Cleanup(); err != nil {
				t.Fatal(err)
			}
			for _, name := range []string{rxIface, txIface} {
				if _, err := os.Stat("/sys/class/net/" + name); !os.IsNotExist(err) {
					t.Fatalf("TAP remains after partial queue cleanup: %s", name)
				}
			}
			return
		}
		if mode == "queue" {
			testInjectQueueSetupFailure(1)
			if err := Setup("net_tap_rx", "net_tap_tx", 2); !errors.Is(err, syscall.EIO) {
				t.Fatalf("partial queue setup error = %v, want EIO", err)
			}
			if err := Teardown(); err != nil {
				t.Fatal(err)
			}
			stats, err := GetStats()
			if err != nil {
				t.Fatal(err)
			}
			if stats.PortsClosed != 2 || stats.PoolInUse != 0 || !stats.PoolFreed ||
				!stats.SnapshotFreed || !stats.QSBRFreed {
				t.Fatalf("partial queue cleanup incomplete: %+v", stats)
			}
			if err := Cleanup(); err != nil {
				t.Fatal(err)
			}
			for _, name := range []string{rxIface, txIface} {
				if _, err := os.Stat("/sys/class/net/" + name); !os.IsNotExist(err) {
					t.Fatalf("TAP remains after partial queue cleanup: %s", name)
				}
			}
			return
		}
		if err := Setup("net_tap_rx", "net_tap_tx", 2); err != nil {
			t.Fatal(err)
		}
		info, err := GetInfo()
		if err != nil || len(info.Workers) != 2 ||
			info.Workers[0].WorkerID != 0 || info.Workers[0].RXQueueID != 0 ||
			info.Workers[0].TXQueueID != 0 || info.Workers[1].WorkerID != 1 ||
			info.Workers[1].RXQueueID != 1 || info.Workers[1].TXQueueID != 1 {
			t.Fatalf("invalid single-owner mapping: info=%+v err=%v", info, err)
		}
		if mode == "launch" {
			testInjectRemoteLaunchFailure(1)
		} else {
			testInjectWorkerFailure(1)
		}
		if err := Run(); !errors.Is(err, syscall.EIO) {
			t.Fatalf("Run error = %v, want EIO", err)
		}
		if err := Teardown(); err != nil {
			t.Fatal(err)
		}
		stats, err := GetStats()
		if err != nil {
			t.Fatal(err)
		}
		if stats.PortsClosed != 2 || stats.PoolInUse != 0 || !stats.PoolFreed ||
			!stats.SnapshotFreed || !stats.QSBRFreed {
			t.Fatalf("incomplete failure cleanup: %+v", stats)
		}
		if err := Cleanup(); err != nil {
			t.Fatal(err)
		}
		for _, name := range []string{rxIface, txIface} {
			if _, err := os.Stat("/sys/class/net/" + name); !os.IsNotExist(err) {
				t.Fatalf("TAP remains after cleanup: %s", name)
			}
		}
		return
	}
	for _, mode := range []string{"mismatch", "queue", "launch", "worker"} {
		t.Run(mode, func(t *testing.T) {
			cmd := exec.Command(os.Args[0], "-test.run=^TestMultiWorkerFailureCleanup$", "-test.v")
			cmd.Env = append(os.Environ(), "FLOW_ROUTER_MULTI_FAILURE_CHILD="+mode)
			out, err := cmd.CombinedOutput()
			if err != nil {
				t.Fatalf("%v\n%s", err, out)
			}
			t.Logf("%s", out)
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

// rte_hash、rte_lpm 与 mbuf pool 都使用真实 EAL allocator。该 case 单独占用
// 进程，并给 LPM tbl24 留出足够的 no-huge 内存。
func TestStaticLookupAndActions(t *testing.T) {
	cpu := os.Getenv("FLOW_ROUTER_TEST_CPU")
	if cpu == "" {
		t.Skip("set FLOW_ROUTER_TEST_CPU to run the real EAL lookup/action test")
	}
	if os.Getenv("FLOW_ROUTER_LOOKUP_TEST_CHILD") == "1" {
		runtime.LockOSThread()
		args := []string{"--lcores=0@" + cpu, "--no-huge", "--no-pci",
			"--no-shconf", "--no-telemetry", "-m", "256"}
		if err := Init(args); err != nil {
			t.Fatal(err)
		}
		if err := testStaticLookupActions(); err != nil {
			t.Fatal(err)
		}
		if err := Cleanup(); err != nil {
			t.Fatal(err)
		}
		return
	}
	cmd := exec.Command(os.Args[0], "-test.run=^TestStaticLookupAndActions$", "-test.v")
	cmd.Env = append(os.Environ(), "FLOW_ROUTER_LOOKUP_TEST_CHILD=1")
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("%v\n%s", err, out)
	}
	t.Logf("%s", out)
	t.Log("flow exact hit PASS; route longest-prefix PASS; flow precedence PASS; " +
		"lookup miss PASS; DROP ownership PASS; FORWARD unchanged PASS; " +
		"UDP REWRITE + checksum PASS; TCP REWRITE + checksum PASS; " +
		"lookup/action resources freed PASS")
}

// 该测试使用真实 rte_hash、rte_lpm allocator 与 DPDK QSBR，并让 publish
// 在普通 pthread 上执行，覆盖非 EAL control goroutine 的调用条件。
func TestDynamicRulesQSBR(t *testing.T) {
	cpu := os.Getenv("FLOW_ROUTER_TEST_CPU")
	if cpu == "" {
		t.Skip("set FLOW_ROUTER_TEST_CPU to run the real QSBR test")
	}
	if os.Getenv("FLOW_ROUTER_QSBR_TEST_CHILD") == "1" {
		runtime.LockOSThread()
		args := []string{"--lcores=0@" + cpu, "--no-huge", "--no-pci",
			"--no-shconf", "--no-telemetry", "-m", "256"}
		if err := Init(args); err != nil {
			t.Fatal(err)
		}
		if err := testDynamicRulesQSBR(); err != nil {
			t.Fatal(err)
		}
		if err := Cleanup(); err != nil {
			t.Fatal(err)
		}
		return
	}
	cmd := exec.Command(os.Args[0], "-test.run=^TestDynamicRulesQSBR$", "-test.v")
	cmd.Env = append(os.Environ(), "FLOW_ROUTER_QSBR_TEST_CHILD=1")
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("%v\n%s", err, out)
	}
	t.Logf("%s", out)
	t.Log("2-reader lifecycle PASS; reader0-only retain PASS; reader1 reclaim PASS; " +
		"publish visibility PASS; two idle readers publish PASS; build rollback PASS; " +
		"50 publishes leak-free PASS; " +
		"snapshot/QSBR teardown PASS")
}
