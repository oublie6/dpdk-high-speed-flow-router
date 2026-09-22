package native

import (
	"errors"
	"os"
	"os/exec"
	"reflect"
	"runtime"
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
