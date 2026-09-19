// Package dataplane 封装 Go 控制面与 C/DPDK runtime 之间的粗粒度边界。
package dataplane

// Info 是完全由 Go 持有的运行时快照，不包含 C pointer。
type Info struct {
	Initialized bool
	MainLcore   uint
	LcoreCount  uint
	Version     string
}
