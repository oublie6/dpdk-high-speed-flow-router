package main

import (
	"fmt"
	"os"

	"github.com/oublie6/dpdk-high-speed-flow-router/control/dataplane"
)

func main() {
	if len(os.Args) < 2 || os.Args[1] != "--" {
		fmt.Fprintln(os.Stderr, "Usage: flow-router -- <EAL arguments>")
		os.Exit(2)
	}
	info, err := dataplane.Probe(os.Args[2:])
	if err != nil {
		fmt.Fprintln(os.Stderr, "flow-router:", err)
		os.Exit(1)
	}
	fmt.Printf("DPDK version: %s\nEAL init succeeded: initialized=%t main_lcore=%d lcore_count=%d\nEAL cleanup succeeded\n",
		info.Version, info.Initialized, info.MainLcore, info.LcoreCount)
}
