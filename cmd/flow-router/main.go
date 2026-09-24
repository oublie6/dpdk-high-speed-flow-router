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
	rulesFile := flag.String("rules-file", "", "启动规则 JSON；SIGHUP 触发运行期重载")
	flag.Parse()
	var rules dataplane.RuleSnapshot
	if *rulesFile != "" {
		var err error
		rules, err = dataplane.LoadRulesFile(*rulesFile)
		if err != nil {
			return err
		}
	}
	signals := make(chan os.Signal, 1)
	signal.Notify(signals, os.Interrupt, syscall.SIGTERM, syscall.SIGHUP)
	defer signal.Stop(signals)
	r, err := dataplane.Start(dataplane.Config{EALArgs: flag.Args(), RXDevice: *rx,
		TXDevice: *tx, Probe: *probe, Rules: rules})
	if err != nil {
		return err
	}
	go func() {
		for {
			select {
			case received := <-signals:
				if received != syscall.SIGHUP {
					r.Stop()
					return
				}
				if *rulesFile == "" {
					fmt.Fprintln(os.Stderr, "rules reload failed: --rules-file is required")
					continue
				}
				updated, loadErr := dataplane.LoadRulesFile(*rulesFile)
				if loadErr != nil {
					fmt.Fprintln(os.Stderr, "rules reload failed:", loadErr)
					continue
				}
				if publishErr := r.ReplaceRules(updated); publishErr != nil {
					fmt.Fprintln(os.Stderr, "rules reload failed:", publishErr)
					continue
				}
				fmt.Printf("rules reload succeeded: generation=%d\n", r.RulesGeneration())
			case <-r.Done():
				return
			}
		}
	}()
	info, startErr := r.Ready()
	if startErr == nil {
		fmt.Printf("DPDK version: %s\nEAL init succeeded: main_lcore=%d lcore_count=%d\n", info.Version, info.MainLcore, info.LcoreCount)
		if !*probe {
			fmt.Printf("rx device: %s -> port %d RXQ0 desc=%d\ntx device: %s -> port %d TXQ0 desc=%d\n", *rx, info.RXPort, info.RXDesc, *tx, info.TXPort, info.TXDesc)
			fmt.Printf("mempool: nb_mbuf=%d cache_size=%d socket_id=%d burst_size=32\nrules generation: 1\ndataplane ready\n", info.NBMbuf, info.CacheSize, info.SocketID)
		}
	}
	stats, err := r.Wait()
	if err != nil {
		return err
	}
	if !*probe {
		fmt.Printf("stats: rx=%d parse_ok=%d parse_unsupported=%d parse_malformed=%d "+
			"flow_hit=%d route_hit=%d lookup_miss=%d action_drop=%d "+
			"action_forward=%d action_rewrite=%d tx_accepted=%d tx_unsent=%d drop=%d\n",
			stats.RX, stats.ParseOK, stats.ParseUnsupported, stats.ParseMalformed,
			stats.FlowHit, stats.RouteHit, stats.LookupMiss, stats.ActionDrop,
			stats.ActionForward, stats.ActionRewrite, stats.TXAccepted,
			stats.TXUnsent, stats.Drop)
		fmt.Printf("rules: generation=%d publish_success=%d publish_failed=%d reclaimed=%d\n",
			stats.RulesGeneration, stats.RulesPublishSuccess,
			stats.RulesPublishFailed, stats.RulesReclaimed)
		fmt.Printf("teardown: ports_closed=%d pool_in_use=%d pool_freed=%t "+
			"flow_table_freed=%t route_table_freed=%t action_store_freed=%t "+
			"snapshot_freed=%t qsbr_freed=%t\n",
			stats.PortsClosed, stats.PoolInUse, stats.PoolFreed,
			stats.FlowTableFreed, stats.RouteTableFreed, stats.ActionStoreFreed,
			stats.SnapshotFreed, stats.QSBRFreed)
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
