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
	Workers                        []WorkerInfo
}
type WorkerInfo struct {
	WorkerID, LcoreID    uint
	RXQueueID, TXQueueID uint16
}
type PacketStats struct {
	RX, ParseOK, ParseUnsupported, ParseMalformed uint64
	FlowHit, RouteHit, LookupMiss                 uint64
	ActionDrop, ActionForward, ActionRewrite      uint64
	TXAccepted, TXUnsent, Drop                    uint64
	RXBursts, RXEmptyPolls, TXBursts              uint64
}
type WorkerStats struct {
	Info    WorkerInfo
	Packets PacketStats
}
type Stats struct {
	RX                         uint64
	ParseOK, ParseUnsupported  uint64
	ParseMalformed             uint64
	FlowHit, RouteHit          uint64
	LookupMiss                 uint64
	ActionDrop, ActionForward  uint64
	ActionRewrite              uint64
	TXAccepted, TXUnsent, Drop uint64
	RXBursts, RXEmptyPolls     uint64
	TXBursts                   uint64
	Workers                    []WorkerStats
	RulesGeneration            uint64
	RulesPublishSuccess        uint64
	RulesPublishFailed         uint64
	RulesReclaimed             uint64
	PortsClosed, PoolInUse     uint
	PoolFreed                  bool
	FlowTableFreed             bool
	RouteTableFreed            bool
	ActionStoreFreed           bool
	SnapshotFreed              bool
	QSBRFreed                  bool
}

const (
	MaxFlowRules   = int(C.DP_MAX_FLOW_RULES)
	MaxRouteRules  = int(C.DP_MAX_ROUTE_RULES)
	ActionDrop     = uint8(C.DP_ACTION_DROP)
	ActionForward  = uint8(C.DP_ACTION_FORWARD)
	ActionRewrite  = uint8(C.DP_ACTION_REWRITE)
	RewriteSrcIPv4 = uint8(C.DP_REWRITE_SRC_IPV4)
	RewriteDstIPv4 = uint8(C.DP_REWRITE_DST_IPV4)
	RewriteSrcPort = uint8(C.DP_REWRITE_SRC_PORT)
	RewriteDstPort = uint8(C.DP_REWRITE_DST_PORT)
)

type Action struct {
	Type             uint8
	RewriteMask      uint8
	SrcIPv4, DstIPv4 uint32
	SrcPort, DstPort uint16
}

type FlowRule struct {
	SrcIPv4, DstIPv4 uint32
	SrcPort, DstPort uint16
	L4Proto          uint8
	Action           Action
}

type RouteRule struct {
	Prefix uint32
	Depth  uint8
	Action Action
}

type RuleSnapshot struct {
	Flows  []FlowRule
	Routes []RouteRule
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
	result := Info{Initialized: info.initialized != 0, MainLcore: uint(info.main_lcore),
		LcoreCount: uint(info.lcore_count), Version: C.GoString(info.version),
		RXPort: uint16(info.rx_port), TXPort: uint16(info.tx_port),
		RXDesc: uint16(info.rx_desc), TXDesc: uint16(info.tx_desc),
		NBMbuf: uint(info.nb_mbuf), CacheSize: uint(info.cache_size), SocketID: int(info.socket_id)}
	for i := 0; i < int(info.worker_count); i++ {
		worker := info.workers[i]
		result.Workers = append(result.Workers, WorkerInfo{
			WorkerID: uint(worker.worker_id), LcoreID: uint(worker.lcore_id),
			RXQueueID: uint16(worker.rx_queue_id), TXQueueID: uint16(worker.tx_queue_id),
		})
	}
	return result, nil
}

type ruleArrays struct {
	flows  *C.struct_dp_flow_rule
	routes *C.struct_dp_route_rule
}

// newRuleArrays 只负责把 Go-owned snapshot 复制到调用期间存活的 C array。
// native build 返回后不再借用这些数组，因此 caller 可以立即 release。
func newRuleArrays(snapshot RuleSnapshot) (ruleArrays, error) {
	if len(snapshot.Flows) > MaxFlowRules || len(snapshot.Routes) > MaxRouteRules {
		return ruleArrays{}, fmt.Errorf("rule snapshot exceeds native capacity")
	}
	arrays := ruleArrays{}
	if len(snapshot.Flows) != 0 {
		arrays.flows = C.dp_flow_rules_alloc(C.size_t(len(snapshot.Flows)))
		if arrays.flows == nil {
			return ruleArrays{}, fmt.Errorf("allocate flow rule array: out of memory")
		}
		for i, rule := range snapshot.Flows {
			action := rule.Action
			C.dp_flow_rule_set(arrays.flows, C.size_t(i), C.uint32_t(rule.SrcIPv4),
				C.uint32_t(rule.DstIPv4), C.uint16_t(rule.SrcPort),
				C.uint16_t(rule.DstPort), C.uint8_t(rule.L4Proto),
				C.uint8_t(action.Type), C.uint8_t(action.RewriteMask),
				C.uint32_t(action.SrcIPv4), C.uint32_t(action.DstIPv4),
				C.uint16_t(action.SrcPort), C.uint16_t(action.DstPort))
		}
	}

	if len(snapshot.Routes) != 0 {
		arrays.routes = C.dp_route_rules_alloc(C.size_t(len(snapshot.Routes)))
		if arrays.routes == nil {
			C.dp_rule_array_free(unsafe.Pointer(arrays.flows))
			return ruleArrays{}, fmt.Errorf("allocate route rule array: out of memory")
		}
		for i, rule := range snapshot.Routes {
			action := rule.Action
			C.dp_route_rule_set(arrays.routes, C.size_t(i), C.uint32_t(rule.Prefix),
				C.uint8_t(rule.Depth), C.uint8_t(action.Type),
				C.uint8_t(action.RewriteMask), C.uint32_t(action.SrcIPv4),
				C.uint32_t(action.DstIPv4), C.uint16_t(action.SrcPort),
				C.uint16_t(action.DstPort))
		}
	}
	return arrays, nil
}

func (arrays ruleArrays) release() {
	C.dp_rule_array_free(unsafe.Pointer(arrays.flows))
	C.dp_rule_array_free(unsafe.Pointer(arrays.routes))
}

// ConfigureRules 发布启动 generation 1；返回后 C 不借用临时 rule array。
func ConfigureRules(snapshot RuleSnapshot) error {
	arrays, err := newRuleArrays(snapshot)
	if err != nil {
		return err
	}
	defer arrays.release()
	return status("configure initial rules", C.dp_configure_rules(arrays.flows,
		C.uint32_t(len(snapshot.Flows)), arrays.routes,
		C.uint32_t(len(snapshot.Routes))))
}

// PublishRules 是除 RequestStop 外唯一允许跨线程进入的 native lifecycle API。
func PublishRules(snapshot RuleSnapshot) (uint64, error) {
	arrays, err := newRuleArrays(snapshot)
	if err != nil {
		return 0, err
	}
	defer arrays.release()
	var generation C.uint64_t
	err = status("publish rules", C.dp_publish_rules(arrays.flows,
		C.uint32_t(len(snapshot.Flows)), arrays.routes,
		C.uint32_t(len(snapshot.Routes)), &generation))
	return uint64(generation), err
}

func Setup(rxDevice, txDevice string, workerCount uint) error {
	rx, tx := C.CString(rxDevice), C.CString(txDevice)
	defer C.free(unsafe.Pointer(rx))
	defer C.free(unsafe.Pointer(tx))
	// setup 只同步查找设备名，不保留这两个临时字符串。
	return status("dataplane setup", C.dp_dataplane_setup(rx, tx, C.uint(workerCount)))
}
func Run() error      { return status("dataplane run", C.dp_dataplane_run()) }
func RequestStop()    { C.dp_dataplane_request_stop() }
func Teardown() error { return status("dataplane teardown", C.dp_dataplane_teardown()) }
func GetStats() (Stats, error) {
	var s C.struct_dp_stats
	if err := status("dataplane stats", C.dp_dataplane_get_stats(&s)); err != nil {
		return Stats{}, err
	}
	result := Stats{RX: uint64(s.rx), ParseOK: uint64(s.parse_ok),
		ParseUnsupported: uint64(s.parse_unsupported), ParseMalformed: uint64(s.parse_malformed),
		FlowHit: uint64(s.flow_hit), RouteHit: uint64(s.route_hit),
		LookupMiss: uint64(s.lookup_miss), ActionDrop: uint64(s.action_drop),
		ActionForward: uint64(s.action_forward), ActionRewrite: uint64(s.action_rewrite),
		TXAccepted: uint64(s.tx_accepted), TXUnsent: uint64(s.tx_unsent), Drop: uint64(s.drop),
		RXBursts: uint64(s.rx_bursts), RXEmptyPolls: uint64(s.rx_empty_polls),
		TXBursts: uint64(s.tx_bursts), RulesGeneration: uint64(s.rules_generation),
		RulesPublishSuccess: uint64(s.rules_publish_success),
		RulesPublishFailed:  uint64(s.rules_publish_failed),
		RulesReclaimed:      uint64(s.rules_reclaimed),
		PortsClosed:         uint(s.ports_closed), PoolInUse: uint(s.pool_in_use),
		PoolFreed: s.pool_freed != 0, FlowTableFreed: s.flow_table_freed != 0,
		RouteTableFreed: s.route_table_freed != 0, ActionStoreFreed: s.action_store_freed != 0,
		SnapshotFreed: s.snapshot_freed != 0, QSBRFreed: s.qsbr_freed != 0}
	for i := 0; i < int(s.worker_count); i++ {
		worker := s.workers[i]
		p := worker.packets
		result.Workers = append(result.Workers, WorkerStats{
			Info: WorkerInfo{WorkerID: uint(worker.info.worker_id), LcoreID: uint(worker.info.lcore_id),
				RXQueueID: uint16(worker.info.rx_queue_id), TXQueueID: uint16(worker.info.tx_queue_id)},
			Packets: PacketStats{RX: uint64(p.rx), ParseOK: uint64(p.parse_ok),
				ParseUnsupported: uint64(p.parse_unsupported), ParseMalformed: uint64(p.parse_malformed),
				FlowHit: uint64(p.flow_hit), RouteHit: uint64(p.route_hit), LookupMiss: uint64(p.lookup_miss),
				ActionDrop: uint64(p.action_drop), ActionForward: uint64(p.action_forward),
				ActionRewrite: uint64(p.action_rewrite), TXAccepted: uint64(p.tx_accepted),
				TXUnsent: uint64(p.tx_unsent), Drop: uint64(p.drop), RXBursts: uint64(p.rx_bursts),
				RXEmptyPolls: uint64(p.rx_empty_polls), TXBursts: uint64(p.tx_bursts)},
		})
	}
	return result, nil
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

func testStaticLookupActions() error {
	return status("static lookup/action test", C.dp_test_static_lookup_actions())
}

const (
	testLiveWorker      = 1
	testLivePort        = 2
	testLiveMempool     = 3
	testLiveActiveRules = 4
	testLiveQSBR        = 5
	testLiveWriter      = 6
	testLiveReader      = 7
)

// testCleanupGuard 直接返回 cleanup 的 errno，供 package 测试断言防御边界。
func testCleanupGuard(resource int) error {
	return status("EAL cleanup guard test", C.dp_test_runtime_cleanup_guard(C.int(resource)))
}

func testDynamicRulesQSBR() error {
	return status("dynamic rules/QSBR test", C.dp_test_dynamic_rules_qsbr())
}

func testWorkerStatsLayout() error {
	return status("worker stats/layout test", C.dp_test_worker_stats_layout())
}

func testInjectRemoteLaunchFailure(workerID int) {
	C.dp_test_inject_remote_launch_failure(C.int(workerID))
}

func testInjectWorkerFailure(workerID int) {
	C.dp_test_inject_worker_failure(C.int(workerID))
}

func testInjectQueueSetupFailure(queueID int) {
	C.dp_test_inject_queue_setup_failure(C.int(queueID))
}

func testQueueCapacity(maxRX, maxTX, workers uint) error {
	return status("queue capacity test", C.dp_test_queue_capacity(
		C.uint(maxRX), C.uint(maxTX), C.uint(workers)))
}

func testWorkerSocket(baseline, worker int) error {
	return status("worker socket test", C.dp_test_worker_socket(
		C.int(baseline), C.int(worker)))
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
