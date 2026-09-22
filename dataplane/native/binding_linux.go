// Package native 是唯一的 cgo package；不负责 goroutine 或 OS thread 调度。
package native

/*
#cgo pkg-config: libdpdk
#cgo CFLAGS: -std=c11
#include <stdlib.h>
#include "dp_api.h"
#include "dp_binding.h"
#include "dp_test.h"
*/
import "C"

import (
	"fmt"
	"syscall"
	"unsafe"
)

// Info/Stats 都是 Go-owned 快照，没有借用 C pointer。
type Info struct {
	Initialized                    bool
	MainLcore, LcoreCount          uint
	Version                        string
	RXPort, TXPort, RXDesc, TXDesc uint16
	NBMbuf, CacheSize              uint
	SocketID                       int
}
type Stats struct {
	RX                         uint64
	ParseOK, ParseUnsupported  uint64
	ParseMalformed             uint64
	TXAccepted, TXUnsent, Drop uint64
	PortsClosed, PoolInUse     uint
	PoolFreed                  bool
}

// EAL 可能重排 argv，因此保存原始分配地址直到 cleanup；没有 Go pointer 逃逸。
var arguments struct {
	strings []*C.char
	array   **C.char
}

func status(operation string, rc C.int) error {
	if rc == 0 {
		return nil
	}
	return fmt.Errorf("%s: %w (status %d)", operation, syscall.Errno(-rc), int(rc))
}

func Init(args []string) error {
	arguments.strings = make([]*C.char, len(args)+1)
	arguments.strings[0] = C.CString("flow-router")
	for i, arg := range args {
		arguments.strings[i+1] = C.CString(arg)
	}
	// 多分配一个 NULL 结尾元素；calloc 由 C helper 完成初始化。
	arguments.array = C.dp_argv_alloc(C.size_t(len(args) + 2))
	if arguments.array == nil {
		freeArguments()
		return fmt.Errorf("allocate EAL argv: out of memory")
	}
	for i, p := range arguments.strings {
		C.dp_argv_set(arguments.array, C.size_t(i), p)
	}
	// init 失败是终止状态：部分 EAL state 可能仍引用 argv，保留到进程退出。
	return status("EAL init", C.dp_runtime_init(C.int(len(args)+1), arguments.array))
}
func freeArguments() {
	for _, p := range arguments.strings {
		C.free(unsafe.Pointer(p))
	}
	C.dp_argv_free_array(arguments.array)
	arguments.strings, arguments.array = nil, nil
}
func GetInfo() (Info, error) {
	var info C.struct_dp_runtime_info
	if err := status("EAL info", C.dp_runtime_get_info(&info)); err != nil {
		return Info{}, err
	}
	return Info{Initialized: info.initialized != 0, MainLcore: uint(info.main_lcore),
		LcoreCount: uint(info.lcore_count), Version: C.GoString(info.version),
		RXPort: uint16(info.rx_port), TXPort: uint16(info.tx_port),
		RXDesc: uint16(info.rx_desc), TXDesc: uint16(info.tx_desc),
		NBMbuf: uint(info.nb_mbuf), CacheSize: uint(info.cache_size), SocketID: int(info.socket_id)}, nil
}
func Setup(rxDevice, txDevice string) error {
	rx, tx := C.CString(rxDevice), C.CString(txDevice)
	defer C.free(unsafe.Pointer(rx))
	defer C.free(unsafe.Pointer(tx))
	// setup 只同步查找设备名，不保留这两个临时字符串。
	return status("dataplane setup", C.dp_dataplane_setup(rx, tx))
}
func Run() error      { return status("dataplane run", C.dp_dataplane_run()) }
func RequestStop()    { C.dp_dataplane_request_stop() }
func Teardown() error { return status("dataplane teardown", C.dp_dataplane_teardown()) }
func GetStats() (Stats, error) {
	var s C.struct_dp_stats
	if err := status("dataplane stats", C.dp_dataplane_get_stats(&s)); err != nil {
		return Stats{}, err
	}
	return Stats{RX: uint64(s.rx), ParseOK: uint64(s.parse_ok),
		ParseUnsupported: uint64(s.parse_unsupported), ParseMalformed: uint64(s.parse_malformed),
		TXAccepted: uint64(s.tx_accepted), TXUnsent: uint64(s.tx_unsent),
		Drop: uint64(s.drop), PortsClosed: uint(s.ports_closed), PoolInUse: uint(s.pool_in_use), PoolFreed: s.pool_freed != 0}, nil
}
func Cleanup() error {
	err := status("EAL cleanup", C.dp_runtime_cleanup())
	if err == nil {
		freeArguments()
	}
	return err
}

// testPartialTXOwnership 是 package 内测试入口，不向控制面暴露逐包 API。
func testPartialTXOwnership() error {
	return status("partial TX ownership test", C.dp_test_tx_partial_ownership())
}

const (
	testLiveWorker  = 1
	testLivePort    = 2
	testLiveMempool = 3
)

// testCleanupGuard 直接返回 cleanup 的 errno，供 package 测试断言防御边界。
func testCleanupGuard(resource int) error {
	return status("EAL cleanup guard test", C.dp_test_runtime_cleanup_guard(C.int(resource)))
}

type testPacketMeta struct {
	EtherType        uint16
	SrcIPv4, DstIPv4 uint32
	L4Proto          uint8
	SrcPort, DstPort uint16
	L2Len, L3Len     uint16
	L4Len, L4Offset  uint16
}

const (
	parseOK          = int(C.DP_PARSE_OK)
	parseUnsupported = int(C.DP_PARSE_UNSUPPORTED)
	parseMalformed   = int(C.DP_PARSE_MALFORMED)

	fixtureValidTCP           = int(C.DP_TEST_PARSE_VALID_TCP)
	fixtureValidUDP           = int(C.DP_TEST_PARSE_VALID_UDP)
	fixtureIPv4Options        = int(C.DP_TEST_PARSE_IPV4_OPTIONS)
	fixtureTCPOptions         = int(C.DP_TEST_PARSE_TCP_OPTIONS)
	fixtureNonIPv4            = int(C.DP_TEST_PARSE_NON_IPV4)
	fixtureUnsupportedL4      = int(C.DP_TEST_PARSE_UNSUPPORTED_L4)
	fixtureIPv4Fragment       = int(C.DP_TEST_PARSE_IPV4_FRAGMENT)
	fixtureEthernetTruncated  = int(C.DP_TEST_PARSE_ETHERNET_TRUNCATED)
	fixtureIPv4Truncated      = int(C.DP_TEST_PARSE_IPV4_TRUNCATED)
	fixtureInvalidIPv4Version = int(C.DP_TEST_PARSE_INVALID_IPV4_VERSION)
	fixtureInvalidIHL         = int(C.DP_TEST_PARSE_INVALID_IHL)
	fixtureInvalidTotalLength = int(C.DP_TEST_PARSE_INVALID_TOTAL_LENGTH)
	fixtureTotalBeyondFrame   = int(C.DP_TEST_PARSE_TOTAL_LENGTH_BEYOND_FRAME)
	fixtureTCPTruncated       = int(C.DP_TEST_PARSE_TCP_TRUNCATED)
	fixtureInvalidTCPOffset   = int(C.DP_TEST_PARSE_INVALID_TCP_OFFSET)
	fixtureTCPBeyondPayload   = int(C.DP_TEST_PARSE_TCP_HEADER_BEYOND_PAYLOAD)
	fixtureUDPTruncated       = int(C.DP_TEST_PARSE_UDP_TRUNCATED)
	fixtureInvalidUDPLength   = int(C.DP_TEST_PARSE_INVALID_UDP_LENGTH)
	fixtureUDPBeyondPayload   = int(C.DP_TEST_PARSE_UDP_BEYOND_PAYLOAD)
	fixtureMultiSegment       = int(C.DP_TEST_PARSE_MULTI_SEGMENT)
)

func testParseFixture(fixture int) (int, testPacketMeta) {
	var meta C.struct_dp_packet_meta
	result := int(C.dp_test_parse_fixture(C.int(fixture), &meta))
	return result, testPacketMeta{
		EtherType: uint16(meta.ether_type), SrcIPv4: uint32(meta.src_ipv4),
		DstIPv4: uint32(meta.dst_ipv4), L4Proto: uint8(meta.l4_proto),
		SrcPort: uint16(meta.src_port), DstPort: uint16(meta.dst_port),
		L2Len: uint16(meta.l2_len), L3Len: uint16(meta.l3_len),
		L4Len: uint16(meta.l4_len), L4Offset: uint16(meta.l4_offset),
	}
}
