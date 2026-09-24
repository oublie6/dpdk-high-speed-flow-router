package dataplane

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"os"

	"github.com/oublie6/dpdk-high-speed-flow-router/dataplane/native"
)

// RuleSnapshot 是启动前一次性发布的数据。所有地址都已经规范化为 host byte order。
type RuleSnapshot struct {
	Flows  []FlowRule
	Routes []RouteRule
}

type FlowRule struct {
	SrcIPv4, DstIPv4 uint32
	SrcPort, DstPort uint16
	L4Proto          uint8
	Action           RuleAction
}

// FlowKey 明确区分 exact-match key 与携带 action 的完整 FlowRule。
type FlowKey struct {
	SrcIPv4, DstIPv4 uint32
	SrcPort, DstPort uint16
	L4Proto          uint8
}

type RouteRule struct {
	Prefix uint32
	Depth  uint8
	Action RuleAction
}

type RouteKey struct {
	Prefix uint32
	Depth  uint8
}

type RuleAction struct {
	Type             uint8
	RewriteMask      uint8
	SrcIPv4, DstIPv4 uint32
	SrcPort, DstPort uint16
}

type rulesDocument struct {
	Flows  []flowDocument  `json:"flows"`
	Routes []routeDocument `json:"routes"`
}

type flowDocument struct {
	SrcIPv4  string         `json:"src_ipv4"`
	DstIPv4  string         `json:"dst_ipv4"`
	SrcPort  uint16         `json:"src_port"`
	DstPort  uint16         `json:"dst_port"`
	Protocol string         `json:"protocol"`
	Action   actionDocument `json:"action"`
}

type routeDocument struct {
	Prefix string         `json:"prefix"`
	Action actionDocument `json:"action"`
}

type actionDocument struct {
	Type    string  `json:"type"`
	SrcIPv4 *string `json:"src_ipv4"`
	DstIPv4 *string `json:"dst_ipv4"`
	SrcPort *uint16 `json:"src_port"`
	DstPort *uint16 `json:"dst_port"`
}

func ipv4Number(text string) (uint32, error) {
	address := net.ParseIP(text)
	ipv4 := address.To4()
	if ipv4 == nil {
		return 0, fmt.Errorf("%q is not an IPv4 address", text)
	}
	return binary.BigEndian.Uint32(ipv4), nil
}

func parseAction(document actionDocument) (RuleAction, error) {
	action := RuleAction{}
	switch document.Type {
	case "drop":
		action.Type = native.ActionDrop
	case "forward":
		action.Type = native.ActionForward
	case "rewrite":
		action.Type = native.ActionRewrite
	default:
		return RuleAction{}, fmt.Errorf("unsupported action type %q", document.Type)
	}

	if action.Type != native.ActionRewrite {
		if document.SrcIPv4 != nil || document.DstIPv4 != nil ||
			document.SrcPort != nil || document.DstPort != nil {
			return RuleAction{}, fmt.Errorf("action %q cannot contain rewrite fields", document.Type)
		}
		return action, nil
	}

	var err error
	if document.SrcIPv4 != nil {
		action.SrcIPv4, err = ipv4Number(*document.SrcIPv4)
		if err != nil {
			return RuleAction{}, fmt.Errorf("rewrite src_ipv4: %w", err)
		}
		action.RewriteMask |= native.RewriteSrcIPv4
	}
	if document.DstIPv4 != nil {
		action.DstIPv4, err = ipv4Number(*document.DstIPv4)
		if err != nil {
			return RuleAction{}, fmt.Errorf("rewrite dst_ipv4: %w", err)
		}
		action.RewriteMask |= native.RewriteDstIPv4
	}
	if document.SrcPort != nil {
		action.SrcPort = *document.SrcPort
		action.RewriteMask |= native.RewriteSrcPort
	}
	if document.DstPort != nil {
		action.DstPort = *document.DstPort
		action.RewriteMask |= native.RewriteDstPort
	}
	if action.RewriteMask == 0 {
		return RuleAction{}, fmt.Errorf("rewrite action must contain at least one field")
	}
	return action, nil
}

func parseProtocol(protocol string) (uint8, error) {
	switch protocol {
	case "tcp":
		return 6, nil
	case "udp":
		return 17, nil
	default:
		return 0, fmt.Errorf("unsupported protocol %q", protocol)
	}
}

func normalizeRules(document rulesDocument) (RuleSnapshot, error) {
	if len(document.Flows) > native.MaxFlowRules {
		return RuleSnapshot{}, fmt.Errorf("flow rule count %d exceeds capacity %d", len(document.Flows), native.MaxFlowRules)
	}
	if len(document.Routes) > native.MaxRouteRules {
		return RuleSnapshot{}, fmt.Errorf("route rule count %d exceeds capacity %d", len(document.Routes), native.MaxRouteRules)
	}

	snapshot := RuleSnapshot{
		Flows:  make([]FlowRule, 0, len(document.Flows)),
		Routes: make([]RouteRule, 0, len(document.Routes)),
	}
	flowKeys := make(map[[5]uint64]struct{}, len(document.Flows))
	for index, item := range document.Flows {
		src, err := ipv4Number(item.SrcIPv4)
		if err != nil {
			return RuleSnapshot{}, fmt.Errorf("flow %d src_ipv4: %w", index, err)
		}
		dst, err := ipv4Number(item.DstIPv4)
		if err != nil {
			return RuleSnapshot{}, fmt.Errorf("flow %d dst_ipv4: %w", index, err)
		}
		protocol, err := parseProtocol(item.Protocol)
		if err != nil {
			return RuleSnapshot{}, fmt.Errorf("flow %d: %w", index, err)
		}
		action, err := parseAction(item.Action)
		if err != nil {
			return RuleSnapshot{}, fmt.Errorf("flow %d: %w", index, err)
		}
		key := [5]uint64{uint64(src), uint64(dst), uint64(item.SrcPort), uint64(item.DstPort), uint64(protocol)}
		if _, exists := flowKeys[key]; exists {
			return RuleSnapshot{}, fmt.Errorf("duplicate flow at index %d", index)
		}
		flowKeys[key] = struct{}{}
		snapshot.Flows = append(snapshot.Flows, FlowRule{
			SrcIPv4: src, DstIPv4: dst, SrcPort: item.SrcPort,
			DstPort: item.DstPort, L4Proto: protocol, Action: action,
		})
	}

	routeKeys := make(map[[2]uint64]struct{}, len(document.Routes))
	for index, item := range document.Routes {
		address, network, err := net.ParseCIDR(item.Prefix)
		if err != nil || address.To4() == nil {
			return RuleSnapshot{}, fmt.Errorf("route %d prefix %q is not a valid IPv4 CIDR", index, item.Prefix)
		}
		depth, bits := network.Mask.Size()
		if bits != 32 || depth == 0 {
			return RuleSnapshot{}, fmt.Errorf("route %d prefix %q is not a valid IPv4 CIDR", index, item.Prefix)
		}
		prefixNumber := binary.BigEndian.Uint32(network.IP.To4())
		action, err := parseAction(item.Action)
		if err != nil {
			return RuleSnapshot{}, fmt.Errorf("route %d: %w", index, err)
		}
		key := [2]uint64{uint64(prefixNumber), uint64(depth)}
		if _, exists := routeKeys[key]; exists {
			return RuleSnapshot{}, fmt.Errorf("duplicate route at index %d", index)
		}
		routeKeys[key] = struct{}{}
		snapshot.Routes = append(snapshot.Routes, RouteRule{
			Prefix: prefixNumber, Depth: uint8(depth), Action: action,
		})
	}
	return snapshot, nil
}

// ParseRules 严格拒绝未知字段和尾随 JSON，避免拼写错误静默变成空规则。
func ParseRules(reader io.Reader) (RuleSnapshot, error) {
	decoder := json.NewDecoder(reader)
	decoder.DisallowUnknownFields()
	var document *rulesDocument
	if err := decoder.Decode(&document); err != nil {
		return RuleSnapshot{}, fmt.Errorf("decode rules JSON: %w", err)
	}
	if document == nil {
		return RuleSnapshot{}, fmt.Errorf("rules JSON must be an object")
	}
	var trailing interface{}
	if err := decoder.Decode(&trailing); err != io.EOF {
		if err == nil {
			return RuleSnapshot{}, fmt.Errorf("rules JSON contains a second value")
		}
		return RuleSnapshot{}, fmt.Errorf("decode trailing rules JSON: %w", err)
	}
	return normalizeRules(*document)
}

func LoadRulesFile(path string) (RuleSnapshot, error) {
	file, err := os.Open(path)
	if err != nil {
		return RuleSnapshot{}, fmt.Errorf("open rules file: %w", err)
	}
	defer file.Close()
	snapshot, err := ParseRules(file)
	if err != nil {
		return RuleSnapshot{}, fmt.Errorf("load rules file %q: %w", path, err)
	}
	return snapshot, nil
}

func cloneRules(snapshot RuleSnapshot) RuleSnapshot {
	return RuleSnapshot{
		Flows:  append([]FlowRule(nil), snapshot.Flows...),
		Routes: append([]RouteRule(nil), snapshot.Routes...),
	}
}

func validateRuleAction(action RuleAction) error {
	validMask := native.RewriteSrcIPv4 | native.RewriteDstIPv4 |
		native.RewriteSrcPort | native.RewriteDstPort
	if action.Type == native.ActionDrop || action.Type == native.ActionForward {
		if action.RewriteMask != 0 {
			return fmt.Errorf("non-rewrite action contains a rewrite mask")
		}
		return nil
	}
	if action.Type != native.ActionRewrite {
		return fmt.Errorf("unknown action type %d", action.Type)
	}
	if action.RewriteMask == 0 || action.RewriteMask&^validMask != 0 {
		return fmt.Errorf("invalid rewrite mask %#x", action.RewriteMask)
	}
	return nil
}

func validateRules(snapshot RuleSnapshot) error {
	if len(snapshot.Flows) > native.MaxFlowRules || len(snapshot.Routes) > native.MaxRouteRules {
		return fmt.Errorf("rule snapshot exceeds native capacity")
	}
	flowKeys := make(map[[5]uint64]struct{}, len(snapshot.Flows))
	for index, rule := range snapshot.Flows {
		if rule.L4Proto != 6 && rule.L4Proto != 17 {
			return fmt.Errorf("flow %d has unsupported protocol %d", index, rule.L4Proto)
		}
		if err := validateRuleAction(rule.Action); err != nil {
			return fmt.Errorf("flow %d: %w", index, err)
		}
		key := [5]uint64{uint64(rule.SrcIPv4), uint64(rule.DstIPv4), uint64(rule.SrcPort), uint64(rule.DstPort), uint64(rule.L4Proto)}
		if _, exists := flowKeys[key]; exists {
			return fmt.Errorf("duplicate flow at index %d", index)
		}
		flowKeys[key] = struct{}{}
	}
	routeKeys := make(map[[2]uint64]struct{}, len(snapshot.Routes))
	for index, rule := range snapshot.Routes {
		if rule.Depth == 0 || rule.Depth > 32 {
			return fmt.Errorf("route %d has invalid prefix depth %d", index, rule.Depth)
		}
		mask := uint32(0)
		if rule.Depth != 0 {
			mask = ^uint32(0) << (32 - rule.Depth)
		}
		if rule.Prefix&mask != rule.Prefix {
			return fmt.Errorf("route %d prefix is not normalized", index)
		}
		if err := validateRuleAction(rule.Action); err != nil {
			return fmt.Errorf("route %d: %w", index, err)
		}
		key := [2]uint64{uint64(rule.Prefix), uint64(rule.Depth)}
		if _, exists := routeKeys[key]; exists {
			return fmt.Errorf("duplicate route at index %d", index)
		}
		routeKeys[key] = struct{}{}
	}
	return nil
}

func toNativeRules(snapshot RuleSnapshot) native.RuleSnapshot {
	result := native.RuleSnapshot{
		Flows:  make([]native.FlowRule, len(snapshot.Flows)),
		Routes: make([]native.RouteRule, len(snapshot.Routes)),
	}
	for i, rule := range snapshot.Flows {
		result.Flows[i] = native.FlowRule{
			SrcIPv4: rule.SrcIPv4, DstIPv4: rule.DstIPv4,
			SrcPort: rule.SrcPort, DstPort: rule.DstPort, L4Proto: rule.L4Proto,
			Action: native.Action(rule.Action),
		}
	}
	for i, rule := range snapshot.Routes {
		result.Routes[i] = native.RouteRule{
			Prefix: rule.Prefix, Depth: rule.Depth, Action: native.Action(rule.Action),
		}
	}
	return result
}

func flowRuleKey(rule FlowRule) FlowKey {
	return FlowKey{SrcIPv4: rule.SrcIPv4, DstIPv4: rule.DstIPv4,
		SrcPort: rule.SrcPort, DstPort: rule.DstPort, L4Proto: rule.L4Proto}
}

func routeRuleKey(rule RouteRule) RouteKey {
	return RouteKey{Prefix: rule.Prefix, Depth: rule.Depth}
}

// publishSnapshotLocked 只在 rulesMu 持有期间调用。native 成功并完成旧代回收后，
// 才替换 Go-owned currentRules，因此失败不会造成两侧 generation 分歧。
func (r *Runtime) publishSnapshotLocked(snapshot RuleSnapshot) error {
	if !r.rulesOpen {
		return fmt.Errorf("runtime rule publication is not available")
	}
	generation, err := r.publishRules(toNativeRules(snapshot))
	if err != nil {
		return err
	}
	r.currentRules = cloneRules(snapshot)
	r.generation = generation
	return nil
}

func (r *Runtime) ReplaceRules(snapshot RuleSnapshot) error {
	candidate := cloneRules(snapshot)
	if err := validateRules(candidate); err != nil {
		return fmt.Errorf("replace rules: %w", err)
	}
	<-r.ready
	r.rulesMu.Lock()
	defer r.rulesMu.Unlock()
	return r.publishSnapshotLocked(candidate)
}

func (r *Runtime) AddFlow(rule FlowRule) error {
	<-r.ready
	r.rulesMu.Lock()
	defer r.rulesMu.Unlock()
	candidate := cloneRules(r.currentRules)
	wanted := flowRuleKey(rule)
	for _, existing := range candidate.Flows {
		if flowRuleKey(existing) == wanted {
			return fmt.Errorf("add flow: duplicate key")
		}
	}
	candidate.Flows = append(candidate.Flows, rule)
	if err := validateRules(candidate); err != nil {
		return fmt.Errorf("add flow: %w", err)
	}
	return r.publishSnapshotLocked(candidate)
}

func (r *Runtime) DeleteFlow(key FlowKey) error {
	<-r.ready
	r.rulesMu.Lock()
	defer r.rulesMu.Unlock()
	candidate := cloneRules(r.currentRules)
	index := -1
	for i, existing := range candidate.Flows {
		if flowRuleKey(existing) == key {
			index = i
			break
		}
	}
	if index < 0 {
		return fmt.Errorf("delete flow: key does not exist")
	}
	candidate.Flows = append(candidate.Flows[:index], candidate.Flows[index+1:]...)
	return r.publishSnapshotLocked(candidate)
}

func (r *Runtime) AddRoute(rule RouteRule) error {
	<-r.ready
	r.rulesMu.Lock()
	defer r.rulesMu.Unlock()
	candidate := cloneRules(r.currentRules)
	wanted := routeRuleKey(rule)
	for _, existing := range candidate.Routes {
		if routeRuleKey(existing) == wanted {
			return fmt.Errorf("add route: duplicate key")
		}
	}
	candidate.Routes = append(candidate.Routes, rule)
	if err := validateRules(candidate); err != nil {
		return fmt.Errorf("add route: %w", err)
	}
	return r.publishSnapshotLocked(candidate)
}

func (r *Runtime) DeleteRoute(key RouteKey) error {
	<-r.ready
	r.rulesMu.Lock()
	defer r.rulesMu.Unlock()
	candidate := cloneRules(r.currentRules)
	index := -1
	for i, existing := range candidate.Routes {
		if routeRuleKey(existing) == key {
			index = i
			break
		}
	}
	if index < 0 {
		return fmt.Errorf("delete route: key does not exist")
	}
	candidate.Routes = append(candidate.Routes[:index], candidate.Routes[index+1:]...)
	return r.publishSnapshotLocked(candidate)
}

func (r *Runtime) RulesGeneration() uint64 {
	r.rulesMu.Lock()
	defer r.rulesMu.Unlock()
	return r.generation
}
