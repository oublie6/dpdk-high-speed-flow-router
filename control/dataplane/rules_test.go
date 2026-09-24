package dataplane

import (
	"fmt"
	"strings"
	"testing"

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
