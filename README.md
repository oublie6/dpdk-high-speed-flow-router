# DPDK High-Speed Flow Router

A high-performance **L2-L4 userspace flow router** built with **Go control plane + thin cgo + C/DPDK dataplane**.

This project is intentionally not a generic DPDK sample and not an application-layer proxy. Its goal is to build a small but complete, measurable packet dataplane that can be explained from NIC queues all the way to packet parsing, lookup, rewrite, forwarding, overload behavior, and benchmark results.

## Current Status

**Goal 001 implemented: Go -> cgo -> project C API -> DPDK EAL init/info/cleanup.**

The Linux CLI performs a one-shot runtime probe with explicit EAL arguments.
The thin wrapper owns C argument memory and pins the lifecycle to one OS thread.
RX/TX, mempools, parsing, tables, rewrite, workers and all performance measurements
remain unimplemented. See [architecture and ownership](docs/architecture.md) and
[Goal 001 acceptance evidence](docs/goals/001-bootstrap-go-cgo-dpdk.md).

## Build and run the EAL probe

Prerequisites: Linux, Go 1.13 or later with cgo, GCC (or Clang via `CC=clang`),
make, pkg-config and system DPDK development files. The verified versions are
recorded in Goal 001. On Debian/Ubuntu the dependency packages are:

```sh
sudo apt-get install golang-go gcc libc6-dev make pkg-config libdpdk-dev
./scripts/check_env.sh
make build
# System DPDK's forced include needs this narrow allowlist with older cgo:
export CGO_CFLAGS_ALLOW='-include|rte_config.h'
# Direct build (force recompilation of C sources outside the Go package):
go build -a -o bin/flow-router ./cmd/flow-router
```

DPDK headers and libraries come from `pkg-config libdpdk`; for a non-system
installation, set `PKG_CONFIG_PATH`. No machine-specific include/library paths
are embedded in the build. The environment check is read-only and reports
missing prerequisites, HugePages, CPU affinity and available NUMA information.

Choose a CPU from the process's allowed affinity list. This example selects the
first allowed CPU and maps it to DPDK logical lcore zero:

```sh
EAL_CPU=$(awk '/Cpus_allowed_list/ {split($2, a, /[-,]/); print a[1]}' /proc/self/status)
./bin/flow-router -- --lcores="0@${EAL_CPU}" --no-huge --no-pci --no-shconf -m 64
```

This allocates 64 MiB of EAL memory without HugePages and disables PCI probing
and shared configuration. It needs no NIC binding or network changes. Success
prints DPDK version, initialization status, main lcore/count and cleanup status,
then exits zero. Invalid EAL arguments exit nonzero with operation/error context.
All arguments after `--` are passed to EAL; choose them deliberately. A fresh
process is required for each probe, including after failures.

```sh
export CGO_CFLAGS_ALLOW='-include|rte_config.h'
go test ./...
go vet ./...
bash -n scripts/check_env.sh
bash -n scripts/start_codex_tmux.sh
# Opt-in real EAL tests (success, invalid arguments, repeated-init rejection):
FLOW_ROUTER_TEST_CPU="$EAL_CPU" go test -count=1 -v ./control/dataplane
```

The ordinary test run checks NUL rejection and skips the EAL integration test
unless `FLOW_ROUTER_TEST_CPU` is set. No EAL behavior is mocked. The existing
`start_codex_tmux.sh` is a development helper that may install/configure tmux;
it is separate from the read-only environment checker.

## Why this project

The project is designed around the engineering problems that matter in high-performance network dataplanes:

```text
RSS / multi-queue
        ↓
RXQ single ownership
        ↓
fixed lcore
        ↓
burst processing
        ↓
packet parsing
        ↓
route / flow lookup
        ↓
action / rewrite
        ↓
bounded TX
        ↓
benchmark
```

The objective is not simply to prove that DPDK can receive and transmit packets. The objective is to answer questions such as:

- Why is one RX queue usually owned by one lcore?
- How should mbuf ownership move through RX and TX?
- When does Run-To-Completion beat a cross-core pipeline?
- How should flow affinity interact with RSS?
- How should TX short returns be handled without leaks or infinite retry?
- What state should be per-lcore?
- What changes when the hardware is NUMA?
- Which optimizations actually improve end-to-end throughput?

## Target architecture

```text
                       Go Control Plane
                config / rules / lifecycle
                          / stats
                             |
                     coarse cgo API
                             |
                             v
+----------------------------------------------------------------+
|                       C / DPDK Dataplane                        |
|                                                                |
|  NIC / virtual PMD                                             |
|        |                                                       |
|       RSS                                                      |
|        |                                                       |
|       RXQ                                                      |
|        |                                                       |
|   fixed lcore                                                  |
|        |                                                       |
|   rte_eth_rx_burst()                                           |
|        |                                                       |
|   Ethernet -> IPv4 -> TCP/UDP parser                           |
|        |                                                       |
|   route / flow / policy lookup                                 |
|        |                                                       |
|   DROP / FORWARD / REWRITE                                     |
|        |                                                       |
|   bounded TX policy                                            |
|        |                                                       |
|   rte_eth_tx_burst()                                           |
|        |                                                       |
|       TXQ -> NIC                                               |
+----------------------------------------------------------------+
```

The hot packet path stays in C/DPDK. Go does not process packets one by one.

## v0.1 scope

The first version is deliberately constrained.

### Protocols

- Ethernet
- IPv4
- TCP
- UDP

### Core features

- L2/L3/L4 parsing;
- route and/or exact flow lookup;
- packet drop;
- output-port forwarding;
- basic IPv4/TCP/UDP field rewrite;
- RX/TX burst processing;
- fixed RXQ -> lcore ownership;
- multi-queue architecture;
- RSS-aware flow affinity;
- Run-To-Completion fast path;
- per-lcore statistics;
- bounded TX retry/drop policy;
- reproducible functional and performance tests.

### Control plane

The Go side will own:

- configuration;
- route/rule lifecycle;
- process lifecycle;
- stats access;
- later, immutable rule/config publication.

The cgo layer must remain thin and coarse-grained.

## Non-goals for v0.1

The following are intentionally deferred:

- IPv6;
- NAT;
- conntrack;
- TCP connection termination;
- TCP stream reassembly;
- user-space TCP stack;
- Kafka/DDS/HTTP/SFTP or other L7 parsing;
- DPI;
- large wildcard ACL engine;
- crypto;
- generic plugin architecture;
- VPP-like graph engine;
- mandatory cross-core rte_ring pipeline;
- distributed control plane.

The first release should be small enough to finish, benchmark, explain, and defend in an interview.

## Design principles

### Single-owner dataplane

The default model is:

```text
RSS -> RXQ -> fixed lcore -> RTC -> TXQ
```

Hardware RSS performs the first level of flow sharding. The dataplane should preserve ownership and locality instead of immediately redistributing packets in software.

### Run-To-Completion first

A packet should normally be parsed, looked up, modified, and transmitted by the same lcore that receives it.

Cross-core `rte_ring` pipelines are added only when measurements demonstrate a heavy/slow stage that benefits from separation.

### NUMA-aware when hardware permits

A real-NIC deployment should try to align:

```text
NIC
+ RX/TX queues
+ worker lcores
+ mempool
+ hot flow/route state
```

within the same NUMA node.

The initial cloud/software lab cannot fully prove real-NIC NUMA performance, so software-lab results will be labeled honestly.

### Explicit ownership

Every mbuf must have an unambiguous owner.

In particular:

```text
RX burst returns mbuf
        ↓
application owns it
        ↓
TX accepts mbuf
        ↓
PMD/TX owns accepted packets

TX does not accept mbuf
        ↓
application still owns it
        ↓
bounded retry / drop / free
```

### Evidence-driven optimization

The project will prefer measurements over folklore.

No optimization is considered successful merely because it is theoretically faster.

## Planned repository structure

```text
.
├── AGENTS.md
├── README.md
├── docs/
│   ├── architecture.md
│   ├── dataplane.md
│   ├── control-plane.md
│   └── benchmark.md
├── cmd/
│   └── flow-router/
├── control/
├── dataplane/
│   ├── core/
│   ├── parser/
│   ├── lookup/
│   └── action/
├── include/
├── configs/
├── scripts/
├── benchmarks/
├── results/
└── tests/
```

Directories will be created when they are needed rather than as empty scaffolding.

## Planned implementation path

```text
Phase 0  Scope and architecture
        ↓
Phase 1  Go/C build skeleton + EAL
        ↓
Phase 2  mempool + port + RX/TX queues
        ↓
Phase 3  single-queue RTC forwarding
        ↓
Phase 4  Ethernet / IPv4 / TCP / UDP parser
        ↓
Phase 5  route / flow lookup
        ↓
Phase 6  drop / forward / rewrite
        ↓
Phase 7  stats + bounded TX handling
        ↓
Phase 8  multi-queue + RSS + fixed lcore
        ↓
Phase 9  benchmark and profiling
        ↓
Phase 10 evidence-driven optimization
```

Later releases may add conntrack, NAT, richer ACLs, slow paths, or other stateful features, but only after the baseline dataplane is stable and measurable.

## Benchmark goals

The project will track at least:

- packets per second;
- throughput in Gbps;
- RX/TX/drop counters;
- CPU utilization;
- cycles per packet when practical;
- packet size;
- flow count;
- queue count;
- burst size;
- workload duration.

Benchmark reports must also identify whether the test used a software PMD/TAP/PCAP path or a real DPDK-capable NIC.

## Related work

This project is deliberately **DPDK-first and from-scratch**.

A separate future project will study **VPP / GoVPP** and compare this project's design with an industrial userspace dataplane framework:

```text
P4 / P4Runtime experience
        ↓
this DPDK Flow Router
        ↓
VPP / GoVPP
        ↓
Cloud Native / Cloud Network Dataplane
```

The long-term goal is to understand both:

- how a high-performance dataplane is built from first principles;
- how the same problems are solved in a mature framework such as VPP.

## Current next step

Goal 001 is ready for ChatGPT review of the commit, ownership and acceptance
evidence. Do not start Goal 002 until that review and the next design discussion
settle:

1. what exactly a `route`, `flow`, and `policy` mean in v0.1;
2. which packet fields can be rewritten;
3. the first software test topology;
4. port / queue / lcore mapping;
5. the configuration model;
6. the Go/C API;
7. the first benchmark baseline.

See [AGENTS.md](AGENTS.md) for project development rules.
