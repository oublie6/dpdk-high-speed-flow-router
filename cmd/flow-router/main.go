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
	workers := flag.Uint("workers", 1, "RTC worker 数量（1-4，必须等于 EAL lcore 数）")
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
		TXDevice: *tx, Workers: *workers, Probe: *probe, Rules: rules})
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
			fmt.Printf("rx device: %s -> port %d queues=%d desc=%d\ntx device: %s -> port %d queues=%d desc=%d\n",
				*rx, info.RXPort, len(info.Workers), info.RXDesc,
				*tx, info.TXPort, len(info.Workers), info.TXDesc)
			for _, worker := range info.Workers {
				fmt.Printf("worker mapping: id=%d lcore=%d RXQ%d -> worker%d -> TXQ%d\n",
					worker.WorkerID, worker.LcoreID, worker.RXQueueID,
					worker.WorkerID, worker.TXQueueID)
			}
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
		fmt.Printf("bursts: rx=%d empty=%d tx=%d\n",
			stats.RXBursts, stats.RXEmptyPolls, stats.TXBursts)
		for _, worker := range stats.Workers {
			p := worker.Packets
			fmt.Printf("worker: id=%d lcore=%d rxq=%d txq=%d rx=%d flow_hit=%d "+
				"route_hit=%d drop=%d tx=%d tx_unsent=%d rx_bursts=%d empty=%d tx_bursts=%d\n",
				worker.Info.WorkerID, worker.Info.LcoreID, worker.Info.RXQueueID,
				worker.Info.TXQueueID, p.RX, p.FlowHit, p.RouteHit, p.Drop,
				p.TXAccepted, p.TXUnsent, p.RXBursts, p.RXEmptyPolls, p.TXBursts)
		}
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
