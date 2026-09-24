package dataplane

import (
	"errors"
	"fmt"
	"reflect"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/oublie6/dpdk-high-speed-flow-router/dataplane/native"
)

func TestParseRulesValidSnapshot(t *testing.T) {
	input := `{
		"flows": [{
			"src_ipv4": "192.0.2.1", "dst_ipv4": "198.51.100.2",
			"src_port": 12345, "dst_port": 23456, "protocol": "udp",
			"action": {"type": "rewrite", "dst_ipv4": "203.0.113.9", "dst_port": 34567}
		}],
		"routes": [{"prefix": "10.1.2.99/24", "action": {"type": "forward"}}]
	}`
	snapshot, err := ParseRules(strings.NewReader(input))
	if err != nil {
		t.Fatal(err)
	}
	if len(snapshot.Flows) != 1 || len(snapshot.Routes) != 1 {
		t.Fatalf("unexpected rule counts: %+v", snapshot)
	}
	flow := snapshot.Flows[0]
	if flow.SrcIPv4 != 0xc0000201 || flow.DstIPv4 != 0xc6336402 ||
		flow.L4Proto != 17 || flow.SrcPort != 12345 || flow.DstPort != 23456 {
		t.Fatalf("flow was not normalized: %+v", flow)
	}
	if flow.Action.Type != native.ActionRewrite ||
		flow.Action.RewriteMask != native.RewriteDstIPv4|native.RewriteDstPort ||
		flow.Action.DstIPv4 != 0xcb007109 || flow.Action.DstPort != 34567 {
		t.Fatalf("rewrite was not normalized: %+v", flow.Action)
	}
	if snapshot.Routes[0].Prefix != 0x0a010200 || snapshot.Routes[0].Depth != 24 {
		t.Fatalf("route was not masked to its network: %+v", snapshot.Routes[0])
	}
}

func newRuleTestRuntime(initial RuleSnapshot) *Runtime {
	ready := make(chan struct{})
	close(ready)
	generation := uint64(1)
	return &Runtime{
		ready: ready, currentRules: cloneRules(initial), generation: generation,
		rulesOpen: true,
		publishRules: func(native.RuleSnapshot) (uint64, error) {
			generation++
			return generation, nil
		},
	}
}

func testFlow(port uint16) FlowRule {
	return FlowRule{SrcIPv4: 0xc0000201, DstIPv4: 0xc6336402,
		SrcPort: 12345, DstPort: port, L4Proto: 17,
		Action: RuleAction{Type: native.ActionDrop}}
}

func testRoute(prefix uint32, depth uint8) RouteRule {
	return RouteRule{Prefix: prefix, Depth: depth,
		Action: RuleAction{Type: native.ActionForward}}
}

func TestRuntimeReplaceRulesAndOwnership(t *testing.T) {
	initial := RuleSnapshot{Flows: []FlowRule{testFlow(1000)}}
	runtime := newRuleTestRuntime(initial)
	replacement := RuleSnapshot{Flows: []FlowRule{testFlow(2000)},
		Routes: []RouteRule{testRoute(0x0a000000, 8)}}
	if err := runtime.ReplaceRules(replacement); err != nil {
		t.Fatal(err)
	}
	if runtime.RulesGeneration() != 2 ||
		!reflect.DeepEqual(runtime.currentRules, replacement) {
		t.Fatalf("valid replace was not committed: %+v", runtime.currentRules)
	}
	// Runtime 必须拥有深拷贝；caller 后续改 slice 不能改变当前 generation。
	replacement.Flows[0].DstPort = 9999
	replacement.Routes = nil
	if runtime.currentRules.Flows[0].DstPort != 2000 ||
		len(runtime.currentRules.Routes) != 1 {
		t.Fatal("caller mutation changed Runtime-owned snapshot")
	}

	before := cloneRules(runtime.currentRules)
	invalid := RuleSnapshot{Flows: []FlowRule{{L4Proto: 1,
		Action: RuleAction{Type: native.ActionDrop}}}}
	if err := runtime.ReplaceRules(invalid); err == nil {
		t.Fatal("invalid replace was accepted")
	}
	if !reflect.DeepEqual(runtime.currentRules, before) ||
		runtime.RulesGeneration() != 2 {
		t.Fatal("invalid replace changed current snapshot or generation")
	}

	runtime.publishRules = func(native.RuleSnapshot) (uint64, error) {
		return 0, errors.New("injected native publish failure")
	}
	if err := runtime.ReplaceRules(initial); err == nil {
		t.Fatal("native publish failure was not returned")
	}
	if !reflect.DeepEqual(runtime.currentRules, before) ||
		runtime.RulesGeneration() != 2 {
		t.Fatal("native publish failure changed Go snapshot")
	}
}

func TestRuntimeFlowAndRouteCRUD(t *testing.T) {
	flow := testFlow(2000)
	route := testRoute(0x0a010200, 24)
	runtime := newRuleTestRuntime(RuleSnapshot{})
	if err := runtime.AddFlow(flow); err != nil {
		t.Fatal(err)
	}
	if err := runtime.AddFlow(flow); err == nil {
		t.Fatal("duplicate flow Add was accepted")
	}
	if err := runtime.DeleteFlow(flowRuleKey(flow)); err != nil {
		t.Fatal(err)
	}
	if err := runtime.DeleteFlow(flowRuleKey(flow)); err == nil {
		t.Fatal("missing flow Delete was accepted")
	}
	if err := runtime.AddRoute(route); err != nil {
		t.Fatal(err)
	}
	if err := runtime.AddRoute(route); err == nil {
		t.Fatal("duplicate route Add was accepted")
	}
	if err := runtime.DeleteRoute(routeRuleKey(route)); err != nil {
		t.Fatal(err)
	}
	if err := runtime.DeleteRoute(routeRuleKey(route)); err == nil {
		t.Fatal("missing route Delete was accepted")
	}
}

func TestRuntimeConcurrentWritersAreSerialized(t *testing.T) {
	runtime := newRuleTestRuntime(RuleSnapshot{})
	var inFlight int32
	var concurrent int32
	var generation uint64 = 1
	runtime.publishRules = func(native.RuleSnapshot) (uint64, error) {
		if atomic.AddInt32(&inFlight, 1) != 1 {
			atomic.StoreInt32(&concurrent, 1)
		}
		time.Sleep(5 * time.Millisecond)
		atomic.AddInt32(&inFlight, -1)
		return atomic.AddUint64(&generation, 1), nil
	}
	var group sync.WaitGroup
	errorsFound := make(chan error, 2)
	for _, port := range []uint16{2001, 2002} {
		group.Add(1)
		go func(flow FlowRule) {
			defer group.Done()
			errorsFound <- runtime.AddFlow(flow)
		}(testFlow(port))
	}
	group.Wait()
	close(errorsFound)
	for err := range errorsFound {
		if err != nil {
			t.Fatal(err)
		}
	}
	if atomic.LoadInt32(&concurrent) != 0 || len(runtime.currentRules.Flows) != 2 ||
		runtime.RulesGeneration() != 3 {
		t.Fatalf("writers were not serialized: generation=%d flows=%d",
			runtime.RulesGeneration(), len(runtime.currentRules.Flows))
	}
}

func TestParseRulesRejectsInvalidInput(t *testing.T) {
	flow := `"src_ipv4":"192.0.2.1","dst_ipv4":"198.51.100.2","src_port":1,"dst_port":2,"protocol":"udp"`
	cases := []struct {
		name  string
		input string
	}{
		{name: "invalid JSON", input: `{"flows": [`},
		{name: "null document", input: `null`},
		{name: "unknown field", input: `{"unknown": 1}`},
		{name: "trailing value", input: `{} {}`},
		{name: "non IPv4 source", input: `{"flows":[{` + flow + `,"src_ipv4":"2001:db8::1","action":{"type":"drop"}}]}`},
		{name: "invalid CIDR", input: `{"routes":[{"prefix":"10.0.0.0/33","action":{"type":"forward"}}]}`},
		{name: "IPv6 CIDR", input: `{"routes":[{"prefix":"2001:db8::/32","action":{"type":"forward"}}]}`},
		{name: "unsupported default route", input: `{"routes":[{"prefix":"0.0.0.0/0","action":{"type":"forward"}}]}`},
		{name: "invalid protocol", input: `{"flows":[{` + flow + `,"protocol":"icmp","action":{"type":"drop"}}]}`},
		{name: "invalid action", input: `{"flows":[{` + flow + `,"action":{"type":"mirror"}}]}`},
		{name: "empty rewrite", input: `{"flows":[{` + flow + `,"action":{"type":"rewrite"}}]}`},
		{name: "rewrite on forward", input: `{"routes":[{"prefix":"10.0.0.0/8","action":{"type":"forward","dst_port":9}}]}`},
		{name: "duplicate flow", input: `{"flows":[{` + flow + `,"action":{"type":"drop"}},{` + flow + `,"action":{"type":"forward"}}]}`},
		{name: "duplicate normalized route", input: `{"routes":[{"prefix":"10.1.2.1/24","action":{"type":"drop"}},{"prefix":"10.1.2.99/24","action":{"type":"forward"}}]}`},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if _, err := ParseRules(strings.NewReader(tc.input)); err == nil {
				t.Fatal("invalid rules were accepted")
			}
		})
	}
}

func TestRuleCapacity(t *testing.T) {
	document := rulesDocument{Flows: make([]flowDocument, native.MaxFlowRules+1)}
	if _, err := normalizeRules(document); err == nil {
		t.Fatal("flow capacity overflow was accepted")
	}
	document = rulesDocument{Routes: make([]routeDocument, native.MaxRouteRules+1)}
	if _, err := normalizeRules(document); err == nil {
		t.Fatal("route capacity overflow was accepted")
	}
}

func TestValidateProgrammaticSnapshot(t *testing.T) {
	base := FlowRule{SrcIPv4: 1, DstIPv4: 2, SrcPort: 3, DstPort: 4,
		L4Proto: 17, Action: RuleAction{Type: native.ActionDrop}}
	cases := []RuleSnapshot{
		{Flows: []FlowRule{base, base}},
		{Flows: []FlowRule{{L4Proto: 1, Action: RuleAction{Type: native.ActionDrop}}}},
		{Routes: []RouteRule{{Prefix: 0x0a010201, Depth: 24, Action: RuleAction{Type: native.ActionForward}}}},
		{Routes: []RouteRule{{Depth: 33, Action: RuleAction{Type: native.ActionForward}}}},
	}
	for i, snapshot := range cases {
		if err := validateRules(snapshot); err == nil {
			t.Fatalf("invalid programmatic snapshot %d was accepted: %s", i, fmt.Sprint(snapshot))
		}
	}
}
