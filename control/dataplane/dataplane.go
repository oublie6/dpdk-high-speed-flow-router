// Package dataplane owns the coarse Go/C runtime boundary.
package dataplane

// Info is a Go-owned snapshot; it contains no C pointers.
type Info struct {
	Initialized bool
	MainLcore   uint
	LcoreCount  uint
	Version     string
}
