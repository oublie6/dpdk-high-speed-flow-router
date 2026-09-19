# Repository Instructions

## 1. Repository purpose

This repository implements a **DPDK-based high-speed L2-L4 flow router**.

The project exists for two purposes:

1. build a real, measurable userspace dataplane rather than another DPDK API demo;
2. serve as an engineering project for **Cloud Native / Container Networking / Network Infra** roles.

The implementation should prove that the author can reason about and build:

```text
NIC / virtual PMD
-> RSS / RX queue
-> fixed lcore
-> burst RX
-> L2/L3/L4 parse
-> route / flow / policy lookup
-> action / rewrite
-> bounded TX
-> TX queue
```

The companion learning repository `oublie6/high-performance-network-learning` remains the source of truth for learning notes, theory, experiments, corrections, and interview-gap tracking. This repository owns product code, design docs, benchmarks, and implementation decisions for the Flow Router itself.

---

## 2. Core architecture

The intended architecture is:

```text
                Go Control Plane
        config / rules / lifecycle / stats
                       |
              coarse-grained cgo API
                       |
                       v
+--------------------------------------------------+
|                C / DPDK Dataplane                |
|                                                  |
| RXQ -> fixed lcore -> parse -> lookup -> action  |
|                         -> rewrite -> TXQ         |
+--------------------------------------------------+
```

The hot path must remain in C/DPDK.

**Do not cross the Go/C boundary per packet.**

Go is responsible for control-plane concerns. C/DPDK is responsible for packet processing.

---

## 3. v0.1 scope

The first usable version intentionally stays small.

### Supported packet scope

- Ethernet
- IPv4
- TCP
- UDP

### Supported dataplane behavior

- packet parsing;
- route and/or exact flow lookup;
- `DROP`;
- `FORWARD`;
- basic L3/L4 `REWRITE`;
- RX/TX burst processing;
- RSS / multi-queue aware execution;
- fixed RXQ -> lcore ownership;
- Run-To-Completion datapath;
- per-lcore statistics;
- explicit TX partial-return handling;
- bounded retry / drop policy;
- reproducible benchmark.

### Control plane

The Go control plane should eventually provide:

- config loading;
- rule / route management;
- lifecycle management;
- stats querying;
- coarse-grained configuration publication to the dataplane.

The first milestone may use static configuration before dynamic rule updates are introduced.

---

## 4. Explicit non-goals for v0.1

Do not add these merely because they are interesting:

- IPv6;
- NAT;
- conntrack;
- TCP termination;
- TCP stream reassembly;
- user-space TCP stack;
- HTTP/Kafka/DDS/SFTP/application-layer parsing;
- complex wildcard ACL engine;
- DPI;
- crypto;
- multi-stage cross-core pipeline;
- generic plugin framework;
- VPP-like graph framework;
- distributed control plane;
- production-grade HA.

These may be considered only after the v0.1 datapath is running and measured.

---

## 5. Dataplane design principles

### 5.1 Single ownership first

Prefer:

```text
RSS
-> RXQ
-> fixed lcore
-> RTC processing
-> TXQ
```

A queue should have a single owner in the fast path unless there is a measured reason to do otherwise.

Avoid shared mutable state in the packet path.

### 5.2 RTC before pipeline

Do not introduce `rte_ring` between stages by default.

A pipeline is justified only when measurements show a real reason, such as:

- a heavy stage that can scale horizontally;
- slow or highly variable work;
- blocking/non-fast-path work;
- an isolation requirement.

### 5.3 NUMA awareness

Where hardware allows it, keep:

```text
NIC / queue / lcore / mempool / hot state
```

on the same NUMA node.

The software/virtual-PMD lab may not reproduce real NIC NUMA behavior. Document that limitation rather than pretending it does.

### 5.4 Ownership must be explicit

For every mbuf transition, be able to answer:

> Who owns this mbuf now, and who is responsible for freeing it?

TX short returns must never leak unsent mbufs or retry forever.

### 5.5 Optimize the common path

Keep the hot path short:

- cheap checks first;
- early drop;
- compact hot structures;
- per-lcore mutable state;
- read-mostly shared configuration;
- no logging in the packet hot path except deliberately sampled/debug builds.

### 5.6 Measure before adding complexity

No performance claim is accepted without a documented workload and measurement.

---

## 6. Go / C boundary rules

The preferred boundary is coarse-grained.

Good examples:

```text
StartDataplane(config)
StopDataplane()
PublishRules(snapshot)
ReadStats()
```

Bad examples:

```text
Go -> C -> process one packet -> Go
```

Avoid Go pointers escaping into long-lived C dataplane state.

Any shared structure crossing the boundary must have an explicit lifetime and ownership model.

---

## 7. Planned repository layout

Prefer this structure unless implementation evidence suggests a better one:

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

Do not create directories only to make the repository look large. Add them when they contain real code or documentation.

---

## 8. Development sequence

Use this sequence unless a real implementation issue requires reordering:

```text
goals / non-goals
-> architecture
-> build system
-> Go/C boundary
-> EAL init
-> port / virtual PMD init
-> mempool
-> RX/TX queue
-> single RXQ single lcore RTC loop
-> Ethernet/IPv4/TCP/UDP parser
-> action: drop/forward
-> rewrite
-> flow/route lookup
-> stats
-> bounded TX short-return handling
-> multi-queue
-> RSS / affinity
-> benchmark
-> optimization from evidence
```

Do not start by implementing every future feature.

---

## 9. Coding rules

### C / DPDK

- keep hot-path functions small;
- avoid hidden allocation in the packet path;
- avoid locks in the common path;
- distinguish packet data length from full packet length;
- validate multi-segment assumptions before directly dereferencing headers;
- use explicit endian conversions;
- make ownership transitions visible in code;
- keep error/slow paths out of the common instruction stream where practical.

### Go

- keep the control plane idiomatic and simple;
- do not use Go for per-packet processing in this project;
- prefer typed configuration and explicit validation;
- keep C bindings thin;
- wrap cgo behind a small package instead of leaking it throughout the control plane.

### General

- no secrets or host-specific credentials;
- no hard-coded management IPs;
- no generated giant logs/pcaps committed to Git;
- comments should explain non-obvious design decisions, not restate code.

---

## 10. Benchmark policy

Performance work is part of the project, not a final decoration.

Every benchmark should record:

- git commit;
- DPDK version;
- CPU / core count;
- NUMA topology when relevant;
- NIC/PMD type;
- queue count;
- burst size;
- packet size;
- flow count/distribution;
- duration;
- forwarding behavior;
- offered load;
- RX/TX/drop counters.

Preferred metrics include:

- Mpps;
- Gbps;
- packets dropped;
- cycles/packet when practical;
- CPU utilization;
- latency where the environment permits;
- burst/queue behavior where useful.

Software PMD/TAP/PCAP results must be labeled as software-lab evidence, not real-NIC line-rate evidence.

---

## 11. Testing policy

Separate functional correctness from performance testing.

Functional tests should cover, at minimum:

- Ethernet / IPv4 parsing;
- TCP / UDP parsing;
- malformed/truncated packet handling;
- match behavior;
- drop;
- forwarding decision;
- rewrite and checksum correctness;
- TX partial-return ownership logic where it can be simulated.

Prefer deterministic packet fixtures and small focused tests.

---

## 12. Documentation policy

Important design choices must be recorded in `docs/`.

When a design changes, document:

```text
old assumption
-> observed evidence/problem
-> new design
-> trade-off
```

Do not silently rewrite history when the correction itself is useful engineering evidence.

---

## 13. Session closing protocol

After a meaningful implementation session:

1. update the relevant design document;
2. record what is actually implemented;
3. record what is only planned;
4. record benchmark evidence if any;
5. update README current status;
6. record open questions / next step;
7. review for secrets, host-specific data, large generated files;
8. commit focused changes.

A future agent should be able to resume from Git without relying on chat history.

---

## 14. Current project state

Current state:

```text
repository created
-> project contract / README
-> architecture discussion
-> v0.1 design
-> implementation
```

No dataplane architecture should be treated as final until the initial design discussion is complete.

## 15. Immediate next step

Before writing the main implementation, agree on:

1. v0.1 packet-processing semantics;
2. route table vs flow table responsibilities;
3. exact rewrite actions;
4. virtual/software test topology;
5. port / queue / lcore model;
6. config format;
7. Go/C API surface;
8. benchmark baseline.
