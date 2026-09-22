package main

import (
	"flag"
	"fmt"
	"os"
	"os/signal"
	"syscall"

	"github.com/oublie6/dpdk-high-speed-flow-router/control/dataplane"
)

func run() error {
	probe := flag.Bool("probe", false, "只执行 EAL init/info/cleanup")
	rx := flag.String("rx-device", "net_tap_rx", "RX TAP vdev name")
	tx := flag.String("tx-device", "net_tap_tx", "TX TAP vdev name")
	flag.Parse()
	signals := make(chan os.Signal, 1)
	signal.Notify(signals, os.Interrupt, syscall.SIGTERM)
	defer signal.Stop(signals)
	r, err := dataplane.Start(dataplane.Config{EALArgs: flag.Args(), RXDevice: *rx, TXDevice: *tx, Probe: *probe})
	if err != nil {
		return err
	}
	go func() {
		select {
		case <-signals:
			r.Stop()
		case <-r.Done():
		}
	}()
	info, startErr := r.Ready()
	if startErr == nil {
		fmt.Printf("DPDK version: %s\nEAL init succeeded: main_lcore=%d lcore_count=%d\n", info.Version, info.MainLcore, info.LcoreCount)
		if !*probe {
			fmt.Printf("rx device: %s -> port %d RXQ0 desc=%d\ntx device: %s -> port %d TXQ0 desc=%d\n", *rx, info.RXPort, info.RXDesc, *tx, info.TXPort, info.TXDesc)
			fmt.Printf("mempool: nb_mbuf=%d cache_size=%d socket_id=%d burst_size=32\ndataplane ready\n", info.NBMbuf, info.CacheSize, info.SocketID)
		}
	}
	stats, err := r.Wait()
	if err != nil {
		return err
	}
	if !*probe {
		fmt.Printf("stats: rx=%d parse_ok=%d parse_unsupported=%d parse_malformed=%d "+
			"tx_accepted=%d tx_unsent=%d drop=%d\n", stats.RX, stats.ParseOK,
			stats.ParseUnsupported, stats.ParseMalformed, stats.TXAccepted,
			stats.TXUnsent, stats.Drop)
		fmt.Printf("teardown: ports_closed=%d pool_in_use=%d pool_freed=%t\n", stats.PortsClosed, stats.PoolInUse, stats.PoolFreed)
	}
	fmt.Println("EAL cleanup succeeded")
	return nil
}
func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "flow-router:", err)
		os.Exit(1)
	}
}
